// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>
#include <zstd.h>

#include "common/cityhash.h"
#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/file_sys/nup_container.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/loader/loader.h"
#include "shader_recompiler/frontend/ampere/instruction.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "shader_recompiler/program_header.h"
#include "shader_recompiler/stage.h"
#include "video_core/native_lowering.h"
#include "video_core/shader_cache_v2/cache.h"
#include "video_core/shader_cache_v2/cache_file.h"
#include "video_core/shader_cache_v2/cache_format.h"
#include "video_core/shader_cache_v2/records.h"
#include "video_core/shader_warm/emission_stamp.h"
#include "video_core/shader_warm/shader_archives.h"
#include "video_core/shader_warm/shader_warm_builder.h"
#include "video_core/shader_warm/warm_records.h"

namespace VideoCommon::ShaderWarm {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view StageReading = "Reading game files";
constexpr std::string_view StageExtracting = "Extracting shaders";
constexpr std::string_view StageConverting = "Converting";
constexpr std::string_view StageWriting = "Writing";
/// Conversions in flight (each holds a copy of its program).
constexpr size_t MaxPendingJobs = 2048;
constexpr size_t ChunkSize = 16 * 1024 * 1024;
constexpr size_t MaxCarryOverTail = 2 * 1024 * 1024;
constexpr size_t MaxBufferSize = ChunkSize + MaxCarryOverTail;
constexpr size_t NativeHeaderSize = 128;

template <typename T>
T Le(std::span<const u8> data, size_t offset) {
    T value;
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}

struct ZstdScannerContext {
    ZSTD_DCtx* dctx{nullptr};
    std::vector<u8> decompressed_buf;

    ZstdScannerContext() {
        dctx = ZSTD_createDCtx();
        decompressed_buf.resize(ChunkSize);
    }
    ~ZstdScannerContext() {
        if (dctx) {
            ZSTD_freeDCtx(dctx);
        }
    }
};

void ScanGenericRomFSChunk(
    std::span<const u8> chunk,
    const std::function<bool(Shader::Stage, const Shader::ProgramHeader&, std::span<const u8>)>&
        try_enqueue_shader) {
    size_t offset = 0;
    while (offset + NativeHeaderSize + 32 <= chunk.size()) {
        const u32 common0 = Le<u32>(chunk, offset);
        const u32 sph_type = common0 & 0x1f;
        const u32 shader_type = (common0 >> 10) & 0xf;
        const u32 reserved1 = (common0 >> 21) & 0x3;
        const u32 reserved2 = (common0 >> 25) & 0x1;

        if (reserved1 != 0 || reserved2 != 0) {
            offset += 16;
            continue;
        }

        if (shader_type >= 1 && shader_type <= 4) {
            if (sph_type != 1) {
                offset += 16;
                continue;
            }
        } else if (shader_type == 5) {
            if (sph_type != 2) {
                offset += 16;
                continue;
            }
        } else {
            offset += 16;
            continue;
        }

        const u64 first_inst_low = Le<u64>(chunk, offset + NativeHeaderSize);
        const u64 first_inst_high = Le<u64>(chunk, offset + NativeHeaderSize + 8);
        if (first_inst_low == 0 && first_inst_high == 0) {
            offset += 16;
            continue;
        }

        const size_t max_bytes =
            std::min<size_t>(chunk.size() - (offset + NativeHeaderSize), MaxCarryOverTail);
        const size_t max_instructions = max_bytes / sizeof(Shader::Ampere::Instruction);
        if (max_instructions < 2) {
            offset += 16;
            continue;
        }

        Shader::Ampere::ProgramCaptureState capture;
        size_t num_instructions = 0;
        bool complete = false;
        for (size_t idx = 0; idx < max_instructions; ++idx) {
            Shader::Ampere::Instruction inst;
            std::memcpy(&inst,
                        chunk.data() + offset + NativeHeaderSize +
                            idx * sizeof(Shader::Ampere::Instruction),
                        sizeof(inst));
            const auto res = capture.Observe(inst, static_cast<u32>(idx));
            if (res == Shader::Ampere::ProgramCaptureState::Result::InvalidTarget) {
                break;
            }
            if (res == Shader::Ampere::ProgramCaptureState::Result::Complete) {
                num_instructions = idx + 1;
                complete = true;
                break;
            }
        }

        if (complete && num_instructions >= 2) {
            const size_t microcode_size = num_instructions * sizeof(Shader::Ampere::Instruction);
            const size_t total_code_size = NativeHeaderSize + microcode_size;

            Shader::Stage stage = Shader::Stage::VertexB;
            switch (shader_type) {
            case 1: stage = Shader::Stage::VertexB; break;
            case 2: stage = Shader::Stage::TessellationControl; break;
            case 3: stage = Shader::Stage::TessellationEval; break;
            case 4: stage = Shader::Stage::Geometry; break;
            case 5: stage = Shader::Stage::Fragment; break;
            default: break;
            }

            Shader::ProgramHeader sph;
            std::memcpy(&sph, chunk.data() + offset, sizeof(sph));
            const auto code_span = chunk.subspan(offset, total_code_size);

            if (try_enqueue_shader(stage, sph, code_span)) {
                offset += total_code_size;
                offset = (offset + 15) & ~size_t{15};
                continue;
            }
        }

        offset += 16;
    }
}

void ScanZstdFramesInChunk(
    std::span<const u8> raw_chunk, ZstdScannerContext& ctx,
    const std::function<bool(Shader::Stage, const Shader::ProgramHeader&, std::span<const u8>)>&
        try_enqueue_shader) {
    if (ctx.dctx == nullptr || raw_chunk.size() < 4) {
        return;
    }
    const u8* ptr = raw_chunk.data();
    const u8* const end = raw_chunk.data() + raw_chunk.size();
    while (ptr + 4 <= end) {
        const void* found = std::memchr(ptr, 0x28, end - ptr);
        if (!found) {
            break;
        }
        const u8* cand = static_cast<const u8*>(found);
        if (cand + 4 > end) {
            break;
        }
        if (cand[1] != 0xb5 || cand[2] != 0x2f || cand[3] != 0xfd) {
            ptr = cand + 1;
            continue;
        }

        const size_t avail = static_cast<size_t>(end - cand);
        const unsigned dict_id = ZSTD_getDictID_fromFrame(cand, avail);
        if (dict_id != 0) {
            const size_t comp_size = ZSTD_findFrameCompressedSize(cand, avail);
            if (!ZSTD_isError(comp_size) && comp_size > 0 && comp_size <= avail) {
                ptr = cand + comp_size;
            } else {
                ptr = cand + 4;
            }
            continue;
        }

        const unsigned long long content_size = ZSTD_getFrameContentSize(cand, avail);
        if (content_size == ZSTD_CONTENTSIZE_ERROR) {
            ptr = cand + 4;
            continue;
        }

        const size_t comp_size = ZSTD_findFrameCompressedSize(cand, avail);
        if (ZSTD_isError(comp_size) || comp_size == 0 || comp_size > avail) {
            break;
        }

        ZSTD_DCtx_reset(ctx.dctx, ZSTD_reset_session_only);
        if (content_size != ZSTD_CONTENTSIZE_UNKNOWN && content_size <= ChunkSize &&
            content_size > NativeHeaderSize) {
            const size_t res = ZSTD_decompressDCtx(
                ctx.dctx, ctx.decompressed_buf.data(), ctx.decompressed_buf.size(), cand, comp_size);
            if (!ZSTD_isError(res) && res > NativeHeaderSize) {
                ScanGenericRomFSChunk(
                    std::span<const u8>(ctx.decompressed_buf.data(), res), try_enqueue_shader);
            }
        } else if (content_size == ZSTD_CONTENTSIZE_UNKNOWN) {
            ZSTD_inBuffer in_buf{.src = cand, .size = comp_size, .pos = 0};
            ZSTD_outBuffer out_buf{.dst = ctx.decompressed_buf.data(),
                                   .size = ctx.decompressed_buf.size(),
                                   .pos = 0};
            size_t last_ret = 1;
            while (in_buf.pos < in_buf.size && out_buf.pos < out_buf.size && last_ret != 0) {
                last_ret = ZSTD_decompressStream(ctx.dctx, &out_buf, &in_buf);
                if (ZSTD_isError(last_ret)) {
                    break;
                }
            }
            if (!ZSTD_isError(last_ret) && out_buf.pos > NativeHeaderSize) {
                ScanGenericRomFSChunk(
                    std::span<const u8>(ctx.decompressed_buf.data(), out_buf.pos), try_enqueue_shader);
            }
        }

        ptr = cand + comp_size;
    }
}

fs::path TitleDir(u64 title_id) {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir) /
           fmt::format("{:016x}", title_id);
}

struct ArchiveFile {
    std::string path; ///< Inside the RomFS, '/' separated (the archive identity)
    FileSys::VirtualFile file;
};

/// Appends every file under `dir` to `out`, recursively, with its '/' separated path.
void ListFiles(const FileSys::VirtualDir& dir, const std::string& prefix,
               std::vector<ArchiveFile>& out) {
    for (const auto& file : dir->GetFiles()) {
        out.push_back({prefix + file->GetName(), file});
    }
    for (const auto& sub : dir->GetSubdirectories()) {
        ListFiles(sub, prefix + sub->GetName() + "/", out);
    }
}

/// The game's RomFS trees in the order the offline pipeline read them: a .nup gives its base
/// and newest update package (both are read, like the exported packages 0 and 1); other
/// formats give the loader's RomFS.
struct GameFiles {
    std::unique_ptr<FileSys::NupContainer> nup;
    std::unique_ptr<Loader::AppLoader> loader;
    std::vector<FileSys::VirtualDir> romfs;
};

/// Opens the game file and collects its RomFS trees into `game`. Returns the error text when the
/// file cannot be opened or has no RomFS.
std::optional<std::string> OpenGame(Core::System& system, const fs::path& game_path,
                                    GameFiles& game) {
    FileSys::VirtualFilesystem filesystem = system.GetFilesystem();
    if (filesystem == nullptr) {
        filesystem = std::make_shared<FileSys::RealVfsFilesystem>();
    }
    const auto file =
        filesystem->OpenFile(Common::FS::PathToUTF8String(game_path), FileSys::OpenMode::Read);
    if (file == nullptr) {
        return "Cannot open the game file";
    }
    if (FileSys::NupContainer::IsNupFile(file)) {
        // Small memory cache: every member is read once.
        game.nup = std::make_unique<FileSys::NupContainer>(file, 128 * 1024 * 1024);
        if (game.nup->GetStatus() != Loader::ResultStatus::Success) {
            return "The game file is not a valid package";
        }
        std::vector<size_t> packages;
        if (const auto base = game.nup->FindBasePackage()) {
            packages.push_back(*base);
        }
        if (const auto update = game.nup->FindBestUpdatePackage()) {
            packages.push_back(*update);
        }
        std::sort(packages.begin(), packages.end());
        for (const size_t package : packages) {
            if (auto dir = game.nup->GetRomFSDir(package)) {
                game.romfs.push_back(std::move(dir));
            }
        }
    } else {
        game.loader = Loader::GetLoader(system, file);
        if (game.loader == nullptr) {
            return "Unsupported game file format";
        }
        FileSys::VirtualFile romfs;
        if (game.loader->ReadRomFS(romfs) != Loader::ResultStatus::Success || romfs == nullptr) {
            return "Cannot read the game's RomFS (are the keys installed?)";
        }
        if (auto dir = FileSys::ExtractRomFS(romfs)) {
            game.romfs.push_back(std::move(dir));
        }
    }
    if (game.romfs.empty()) {
        return "The game has no RomFS";
    }
    return std::nullopt;
}

struct PairHash {
    u128 a{};
    u128 b{};
    bool operator==(const PairHash&) const = default;
};
struct PairHasher {
    size_t operator()(const PairHash& h) const noexcept {
        return static_cast<size_t>(h.a[0] ^ (h.b[0] * 0x9E3779B97F4A7C15ULL));
    }
};

u128 Hash128(std::span<const u8> bytes) {
    return Common::CityHash128(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

struct U128Hasher {
    size_t operator()(const u128& h) const noexcept {
        return static_cast<size_t>(h[0] ^ (h[1] * 0x9E3779B97F4A7C15ULL));
    }
};

enum class JobKind : u8 {
    Compute,
    Graphics,
};

struct Job {
    size_t seq{};
    JobKind kind{JobKind::Compute};
    std::vector<u8> program;
    ComputeControl control{};
    Shader::Stage stage{Shader::Stage::VertexB};
    Shader::ProgramHeader sph{};
    u128 code_hash{};
};

struct JobResult {
    bool ok{};
    JobKind kind{JobKind::Compute};
    std::vector<u8> bytes; ///< The full compute entry or encoded program record
    ComputeKey compute_key{};
    u64 program_key{};
    u128 code_hash{};
    u64 stage_hash{};
    LoweredCode lowered{};
};

class Converter {
public:
    /// Starts `threads` workers that convert pushed jobs until `stop` is requested.
    Converter(size_t threads, std::stop_token stop_) : stop{std::move(stop_)} {
        for (size_t i = 0; i < threads; ++i) {
            workers.emplace_back([this] { Work(); });
        }
    }

    /// Lets the workers finish the queued jobs, then joins them.
    ~Converter() {
        {
            std::scoped_lock lock{mutex};
            closing = true;
        }
        jobs_cv.notify_all();
        for (auto& worker : workers) {
            worker.join();
        }
    }

    void Push(Job job) {
        {
            std::scoped_lock lock{mutex};
            jobs.push_back(std::move(job));
            ++pushed;
        }
        jobs_cv.notify_one();
    }

    /// Next result in submission order; waits when `wait` is set, else empty when not ready.
    std::optional<JobResult> Next(bool wait) {
        std::unique_lock lock{mutex};
        for (;;) {
            const auto it = results.find(next);
            if (it != results.end()) {
                JobResult result = std::move(it->second);
                results.erase(it);
                ++next;
                return result;
            }
            if (!wait || next == pushed || stop.stop_requested()) {
                return std::nullopt;
            }
            results_cv.wait_for(lock, std::chrono::milliseconds{100});
        }
    }

    size_t Pending() {
        std::scoped_lock lock{mutex};
        return pushed - next;
    }

private:
    /// Worker loop: takes jobs in order, converts each one (skipped once stopping) and files the
    /// result under its sequence number. Returns once closing with no job left.
    void Work() {
        Common::SetCurrentThreadName("ShaderWarmBuild");
        Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
        for (;;) {
            Job job;
            {
                std::unique_lock lock{mutex};
                jobs_cv.wait(lock, [this] { return closing || !jobs.empty(); });
                if (jobs.empty()) {
                    return;
                }
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            JobResult result;
            if (!stop.stop_requested()) {
                result = Convert(job);
            }
            {
                std::scoped_lock lock{mutex};
                results.emplace(job.seq, std::move(result));
            }
            results_cv.notify_all();
        }
    }

    /// Lowers one program and makes its compute or graphics entry. The result is not ok when lowering or
    /// conversion failed.
    static JobResult Convert(const Job& job) {
        JobResult result;
        result.kind = job.kind;
        try {
            if (job.kind == JobKind::Compute) {
                LoweredCode lowered;
                if (const auto error = LowerProgram(job.program, lowered)) {
                    LOG_DEBUG(Render_Vulkan, "Shader precache: compute program not converted: {}", *error);
                    return result;
                }
                result.bytes = ComputeEntry(lowered, job.control, result.compute_key);
                result.ok = true;
            } else {
                using Shader::Ampere::Instruction;
                if (job.program.empty() || job.program.size() % sizeof(Instruction) ||
                    job.program.size() / sizeof(Instruction) > 65536) {
                    return result;
                }
                std::vector<Instruction> native(job.program.size() / sizeof(Instruction));
                std::memcpy(native.data(), job.program.data(), job.program.size());
                Shader::Ampere::ProgramCaptureState capture;
                for (u32 index = 0; index < native.size(); ++index) {
                    const auto res = capture.Observe(native[index], index);
                    if (res == Shader::Ampere::ProgramCaptureState::Result::InvalidTarget) {
                        return result;
                    }
                    if (res == Shader::Ampere::ProgramCaptureState::Result::Complete) {
                        native.resize(index + 1);
                        break;
                    }
                }
                if (!VideoCommon::NativeTrapOffsets(native).empty()) {
                    return result;
                }
                const bool geometry = (job.stage == Shader::Stage::Geometry);
                const auto tessellation_stage = job.stage == Shader::Stage::TessellationControl
                                                   ? Shader::Ampere::TessellationStage::Control
                                               : job.stage == Shader::Stage::TessellationEval
                                                   ? Shader::Ampere::TessellationStage::Evaluation
                                                   : Shader::Ampere::TessellationStage::None;
                const VideoCommon::NativeLoweringInputs inputs{
                    .geometry = geometry,
                    .tessellation_stage = tessellation_stage,
                    .native_code_address = 0,
                };
                auto lowered = VideoCommon::LowerNativeProgram(native, inputs);
                if (!lowered) {
                    return result;
                }
                const auto traps = VideoCommon::NativeTrapOffsets(native);
                auto code = VideoCommon::MakeLoweredNativeCode(native, job.sph, 128, traps, lowered);
                auto record = VideoCommon::ShaderCacheV2::MakeProgramRecord(
                    job.stage, 128, job.sph, native, inputs, *code);
                std::vector<u8> lowered_bytes(code->code.size() * sizeof(u64));
                std::memcpy(lowered_bytes.data(), code->code.data(), lowered_bytes.size());
                result.program_key = VideoCommon::ShaderCacheV2::ProgramKey(record);
                result.code_hash = job.code_hash;
                result.stage_hash = StageHash(lowered_bytes);
                result.lowered = LoweredCode{
                    .code = std::move(lowered_bytes),
                    .sph = job.sph,
                    .texture_buffer = code->texture_bound,
                };
                const std::vector<u8> payload = VideoCommon::ShaderCacheV2::EncodeProgram(record);
                VideoCommon::ShaderCacheV2::EncodeRecord(
                    result.bytes, VideoCommon::ShaderCacheV2::RecordKind::Program, result.program_key, payload);
                result.ok = true;
            }
        } catch (const std::exception& e) {
            LOG_DEBUG(Render_Vulkan, "Shader precache: conversion failed: {}", e.what());
        }
        return result;
    }

    std::stop_token stop;
    std::mutex mutex;
    std::condition_variable jobs_cv;
    std::condition_variable results_cv;
    std::deque<Job> jobs;
    std::map<size_t, JobResult> results;
    size_t pushed{};
    size_t next{};
    bool closing{};
    std::vector<std::thread> workers;
};

template <typename T>
void Write(std::ofstream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

void WriteHeader(std::ofstream& out) {
    out.write("yuzucach", 8);
    Write(out, WarmCacheVersion);
}

std::atomic<bool> warm_before_start{false};
std::atomic<bool> skip_warm{false};

/// Records `entries` for the finished precache file at `warm_file` (see WarmCountPath).
void WriteWarmCount(const fs::path& warm_file, u64 entries) {
    std::error_code ec;
    const auto size = fs::file_size(warm_file, ec);
    if (ec) {
        return;
    }
    std::ofstream out(WarmCountPath(warm_file), std::ios::trunc);
    out << entries << ' ' << size << ' ' << WarmBuilderVersion << '\n';
}

} // Anonymous namespace

fs::path ComputeWarmPath(u64 title_id) {
    return TitleDir(title_id) / "warm-compute.bin";
}

fs::path PairsWarmPath(u64 title_id) {
    return TitleDir(title_id) / "warm-pairs.bin";
}

fs::path WarmMarkerPath(const fs::path& warm_file) {
    fs::path marker = warm_file;
    marker.replace_extension(".done");
    return marker;
}

fs::path WarmCountPath(const fs::path& warm_file) {
    fs::path count = warm_file;
    count.replace_extension(".count");
    return count;
}

u64 ReadWarmCount(const fs::path& warm_file) {
    std::ifstream in(WarmCountPath(warm_file));
    u64 entries{};
    u64 size{};
    if (!(in >> entries >> size)) {
        return 0;
    }
    std::error_code ec;
    const auto actual = fs::file_size(warm_file, ec);
    return !ec && actual == size ? entries : 0;
}

u64 EmissionStamp() {
    return GeneratedEmissionStamp;
}

std::optional<WarmStamp> CurrentWarmStamp(const fs::path& warm_file, u64 entries, u32 driver,
                                          u32 device_id, u32 cache_version, u64 variant) {
    std::error_code ec;
    const auto size = fs::file_size(warm_file, ec);
    if (ec) {
        return std::nullopt;
    }
    // Hashed in chunks (the file is ~70 MB): tens of milliseconds at boot.
    std::ifstream in(warm_file, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::vector<char> chunk(8u << 20);
    u64 hash = size;
    u64 read_total = 0;
    while (in) {
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const auto got = static_cast<size_t>(in.gcount());
        if (got == 0) {
            break;
        }
        hash = Common::CityHash64WithSeed(chunk.data(), got, hash);
        read_total += got;
    }
    if (read_total != size) {
        return std::nullopt;
    }
    return WarmStamp{.size = size,
                     .content_hash = hash,
                     .entries = entries,
                     .driver = driver,
                     .device_id = device_id,
                     .cache_version = cache_version,
                     .variant = variant};
}

std::optional<WarmStamp> ReadWarmMarker(const fs::path& warm_file) {
    std::ifstream in(WarmMarkerPath(warm_file));
    WarmStamp stamp;
    // A marker of the older format (last-write time, no variant) fails here: the compute
    // programs not in the cache are built once more.
    std::string format;
    if (!(in >> format) || format != "v2" ||
        !(in >> stamp.size >> stamp.content_hash >> stamp.entries >> stamp.driver >>
          stamp.device_id >> stamp.cache_version >> stamp.variant)) {
        return std::nullopt;
    }
    return stamp;
}

bool WriteWarmMarker(const fs::path& warm_file, const WarmStamp& stamp) {
    std::ofstream out(WarmMarkerPath(warm_file), std::ios::trunc);
    out << "v2 " << stamp.size << ' ' << stamp.content_hash << ' ' << stamp.entries << ' '
        << stamp.driver << ' ' << stamp.device_id << ' ' << stamp.cache_version << ' '
        << stamp.variant << '\n';
    out.close();
    return static_cast<bool>(out);
}

void DropStaleWarmMarker(const fs::path& warm_file, const fs::path& new_file) {
    if (const auto marker = ReadWarmMarker(warm_file)) {
        const auto incoming = CurrentWarmStamp(new_file, 0, 0, 0, 0);
        if (incoming && incoming->size == marker->size &&
            incoming->content_hash == marker->content_hash) {
            return;
        }
    }
    std::error_code ignored;
    fs::remove(WarmMarkerPath(warm_file), ignored);
}

fs::path WarmProgressPath(const fs::path& warm_file) {
    fs::path progress = warm_file;
    progress.replace_extension(".progress");
    return progress;
}

std::unordered_set<u64> ReadWarmProgress(const fs::path& warm_file, const WarmStamp& current) {
    std::ifstream in(WarmProgressPath(warm_file));
    WarmStamp stamp;
    std::string format;
    if (!(in >> format) || format != "v1" ||
        !(in >> stamp.size >> stamp.content_hash >> stamp.entries >> stamp.driver >>
          stamp.device_id >> stamp.cache_version >> stamp.variant) ||
        !stamp.Matches(current)) {
        return {};
    }
    std::unordered_set<u64> built;
    u64 key{};
    while (in >> std::hex >> key) {
        built.insert(key);
    }
    return built;
}

bool WriteWarmProgress(const fs::path& warm_file, const WarmStamp& stamp,
                       const std::unordered_set<u64>& built) {
    std::ofstream out(WarmProgressPath(warm_file), std::ios::trunc);
    out << "v1 " << stamp.size << ' ' << stamp.content_hash << ' ' << stamp.entries << ' '
        << stamp.driver << ' ' << stamp.device_id << ' ' << stamp.cache_version << ' '
        << stamp.variant << '\n'
        << std::hex;
    for (const u64 key : built) {
        out << key << '\n';
    }
    out.close();
    return static_cast<bool>(out);
}

fs::path WarmRequestPath(const fs::path& warm_file) {
    fs::path request = warm_file;
    request.replace_extension(".requested");
    return request;
}

bool WarmRequested(const fs::path& warm_file) {
    std::error_code ec;
    return fs::exists(WarmRequestPath(warm_file), ec);
}

bool SetWarmRequested(const fs::path& warm_file, bool requested) {
    std::error_code ec;
    if (!requested) {
        fs::remove(WarmRequestPath(warm_file), ec);
        return !ec;
    }
    fs::create_directories(warm_file.parent_path(), ec);
    std::ofstream out(WarmRequestPath(warm_file), std::ios::trunc);
    return static_cast<bool>(out);
}

bool WarmPending(u64 title_id) {
    const auto file = ComputeWarmPath(title_id);
    const auto pairs = PairsWarmPath(title_id);
    std::error_code ec;
    const bool compute_done = fs::exists(file, ec) && fs::exists(WarmMarkerPath(file), ec);
    const bool pairs_done = fs::exists(pairs, ec) && fs::exists(WarmMarkerPath(pairs), ec);
    return !compute_done || !pairs_done;
}

void RequestWarmBeforeStart(bool before_start) {
    skip_warm.store(false, std::memory_order_relaxed);
    warm_before_start.store(before_start, std::memory_order_relaxed);
}

bool TakeWarmBeforeStart() {
    return warm_before_start.exchange(false, std::memory_order_relaxed);
}

void RequestSkipWarm() {
    skip_warm.store(true, std::memory_order_relaxed);
}

bool TakeSkipWarm() {
    return skip_warm.exchange(false, std::memory_order_relaxed);
}

bool DeleteWarmFiles(u64 title_id, std::string* error) {
    const auto warm = ComputeWarmPath(title_id);
    fs::path tmp = warm;
    tmp += ".tmp";
    const auto pairs = PairsWarmPath(title_id);
    fs::path pairs_tmp = pairs;
    pairs_tmp += ".tmp";
    bool ok = true;
    for (const auto& path :
         {warm, WarmMarkerPath(warm), WarmCountPath(warm), WarmProgressPath(warm),
          WarmRequestPath(warm), tmp,
          pairs, WarmMarkerPath(pairs), WarmCountPath(pairs), pairs_tmp,
          TitleDir(title_id) / "precache-graphics.bin"}) {
        std::error_code ec;
        fs::remove(path, ec);
        if (ec) {
            ok = false;
            if (error != nullptr) {
                *error = ec.message();
            }
        }
    }
    return ok;
}

std::optional<std::string> WarmFilesStale(const fs::path& warm_file) {
    std::error_code ec;
    if (!fs::exists(warm_file, ec)) {
        return "none built yet";
    }
    {
        std::ifstream in(warm_file, std::ios::binary);
        std::array<char, 8> magic{};
        u32 version{};
        in.read(magic.data(), magic.size())
            .read(reinterpret_cast<char*>(&version), sizeof(version));
        const auto& compatible = VideoCommon::CompatibleVulkanPipelineCacheVersions;
        if (!in || magic != VideoCommon::PipelineCacheMagic ||
            (version != WarmCacheVersion &&
             std::ranges::find(compatible, version) == compatible.end())) {
            return "built for another cache version";
        }
    }
    std::ifstream in(WarmCountPath(warm_file));
    u64 entries{};
    u64 size{};
    u32 builder{};
    if (!(in >> entries >> size >> builder) || builder != WarmBuilderVersion) {
        return "built by an older version";
    }
    if (fs::file_size(warm_file, ec) != size || ec) {
        return "changed since it was built";
    }
    return std::nullopt;
}

Result BuildWarmFiles(Core::System& system, const fs::path& game_path, u64 title_id,
                      const std::function<void(const Progress&)>& progress,
                      std::stop_token stop) {
    return BuildWarmFilesWith(system, game_path, title_id, BuildOptions{}, progress,
                              std::move(stop));
}

Result BuildWarmFilesWith(Core::System& system, const fs::path& game_path, u64 title_id,
                          const BuildOptions& options,
                          const std::function<void(const Progress&)>& progress,
                          std::stop_token stop) {
    Result result;
    const auto report = [&](std::string_view stage, size_t done, size_t total) {
        if (progress) {
            progress(Progress{std::string(stage), done, total});
        }
    };
    const auto begin = std::chrono::steady_clock::now();
    report(StageReading, 0, 0);

    const fs::path output_dir = options.output_dir.empty() ? TitleDir(title_id) : options.output_dir;
    std::error_code ec;
    fs::create_directories(output_dir, ec);
    const fs::path compute_path = output_dir / "warm-compute.bin";
    fs::path compute_tmp = compute_path;
    compute_tmp += ".tmp";
    std::ofstream compute_out;
    const auto fail = [&](std::string error) {
        compute_out.close();
        std::error_code ignored;
        fs::remove(compute_tmp, ignored);
        result.ok = false;
        result.error = std::move(error);
        LOG_WARNING(Render_Vulkan, "Shader precache build for {:016x} failed: {}", title_id,
                    result.error);
        return result;
    };

    GameFiles game;
    if (const auto error = OpenGame(system, game_path, game)) {
        return fail(*error);
    }

    // Archive list, in the offline pipeline's order: every shader archive of each package, then
    // each package's effects, sorted by path inside a package (see ClassifyGameFile).
    std::vector<ArchiveFile> archives;
    std::vector<ArchiveFile> effects;
    std::vector<ArchiveFile> generic_packages;
    ZstdDictionaries dictionaries;
    FileSys::VirtualFile dictionary_pack;
    for (const auto& romfs : game.romfs) {
        std::vector<ArchiveFile> files;
        ListFiles(romfs, "", files);
        std::sort(files.begin(), files.end(),
                  [](const ArchiveFile& a, const ArchiveFile& b) { return a.path < b.path; });
        std::vector<ArchiveFile> package_effects;
        for (auto& file : files) {
            switch (ClassifyGameFile(file.path)) {
            case GameFileKind::ShaderArchive:
                archives.push_back(file);
                break;
            case GameFileKind::Effect:
                package_effects.push_back(file);
                break;
            case GameFileKind::ZstdDictionaries:
                dictionary_pack = file.file; // the newest package wins
                break;
            case GameFileKind::GenericPackage:
                generic_packages.push_back(file);
                break;
            case GameFileKind::Other:
                break;
            }
        }
        effects.insert(effects.end(), package_effects.begin(), package_effects.end());
    }
    archives.insert(archives.end(), effects.begin(), effects.end());
    if (dictionary_pack) {
        try {
            dictionaries.Load(dictionary_pack->ReadAllBytes());
        } catch (const ArchiveError& e) {
            LOG_WARNING(Render_Vulkan, "Shader precache: zstd dictionary pack: {}", e.what());
        }
    }
    const size_t total_archives = archives.size() + generic_packages.size();
    result.archives = total_archives;
    LOG_INFO(Render_Vulkan,
             "Shader precache build: {} files ({} archives, {} generic packages), {} zstd dictionaries",
             total_archives, archives.size(), generic_packages.size(), dictionaries.Count());

    compute_out.open(compute_tmp, std::ios::binary | std::ios::trunc);
    if (!compute_out) {
        return fail("Cannot write to " + Common::FS::PathToUTF8String(output_dir));
    }
    WriteHeader(compute_out);

    std::unordered_set<PairHash, PairHasher> compute_identities;
    std::unordered_set<std::string> compute_keys;
    std::unordered_set<u128, U128Hasher> graphics_identities;
    std::unordered_set<u64> graphics_keys;
    std::vector<u8> graphics_records_bytes;

    struct PairIdentity {
        u128 vs_hash;
        u128 ps_hash;
        bool operator==(const PairIdentity& o) const noexcept {
            return vs_hash == o.vs_hash && ps_hash == o.ps_hash;
        }
    };
    struct PairIdentityHasher {
        size_t operator()(const PairIdentity& p) const noexcept {
            return U128Hasher{}(p.vs_hash) ^ (U128Hasher{}(p.ps_hash) * 0x9E3779B97F4A7C15ULL);
        }
    };
    std::unordered_set<PairIdentity, PairIdentityHasher> pair_identities;
    std::vector<PairIdentity> extracted_pairs;
    std::unordered_map<u128, u64, U128Hasher> code_hash_to_stage_hash;
    std::unordered_map<u128, LoweredCode, U128Hasher> code_hash_to_lowered;
    size_t archive_errors = 0;
    size_t member_errors = 0;
    size_t seq = 0;

    const size_t threads =
        options.threads != 0
            ? options.threads
            : std::clamp<size_t>(std::thread::hardware_concurrency() / 4, 2, 6);
    auto converter = std::make_unique<Converter>(threads, stop);

    // Consumes finished conversions in submission order (first program wins on equal keys).
    size_t consumed = 0;
    const auto consume = [&](JobResult&& r) {
        ++consumed;
        if (!r.ok) {
            ++result.skipped;
            return;
        }
        if (r.kind == JobKind::Compute) {
            const std::string key(reinterpret_cast<const char*>(r.compute_key.data()),
                                  r.compute_key.size());
            if (compute_keys.insert(key).second) {
                compute_out.write(reinterpret_cast<const char*>(r.bytes.data()),
                                  static_cast<std::streamsize>(r.bytes.size()));
                ++result.compute_programs;
            }
        } else {
            if (r.code_hash != u128{}) {
                code_hash_to_stage_hash[r.code_hash] = r.stage_hash;
                code_hash_to_lowered[r.code_hash] = std::move(r.lowered);
            }
            if (graphics_keys.insert(r.program_key).second) {
                graphics_records_bytes.insert(graphics_records_bytes.end(),
                                              r.bytes.begin(), r.bytes.end());
                ++result.graphics_programs;
            }
        }
    };
    const auto drain = [&] {
        while (auto r = converter->Next(false)) {
            consume(std::move(*r));
        }
    };

    const auto try_enqueue_shader = [&](Shader::Stage stage, const Shader::ProgramHeader& sph,
                                        std::span<const u8> code) -> bool {
        if (code.size() <= 128 || (code.size() - 128) % 16) {
            return false;
        }
        const u128 code_h = Hash128(code);
        if (!graphics_identities.insert(code_h).second) {
            return false;
        }
        Job job;
        job.kind = JobKind::Graphics;
        job.seq = seq++;
        job.stage = stage;
        job.code_hash = code_h;
        job.sph = sph;
        job.program.assign(code.begin() + 128, code.end());
        converter->Push(std::move(job));
        return true;
    };

    for (size_t archive_index = 0; archive_index < archives.size(); ++archive_index) {
        if (stop.stop_requested()) {
            converter.reset();
            return fail("Stopped");
        }
        report(StageExtracting, archive_index, total_archives);
        const auto& archive = archives[archive_index];
        const auto data = archive.file->ReadAllBytes();
        const auto name = archive.path.substr(archive.path.rfind('/') + 1);
        const auto on_record = [&](const ShaderRecord& record) {
            if (record.stage == ProgramStage::Compute) {
                if (!compute_identities.insert(PairHash{Hash128(record.code), Hash128(record.control)})
                         .second) {
                    return;
                }
                const auto values = ReadComputeControl(record.control);
                if (!values || !values->workgroup[0] || !values->workgroup[1] ||
                    !values->workgroup[2]) {
                    ++result.skipped;
                    return;
                }
                Job job;
                job.kind = JobKind::Compute;
                job.control = *values;
                job.seq = seq++;
                job.program.assign(record.code.begin(), record.code.end());
                converter->Push(std::move(job));
            } else {
                Shader::Stage stage = Shader::Stage::VertexB;
                switch (record.stage) {
                case ProgramStage::Vertex: stage = Shader::Stage::VertexB; break;
                case ProgramStage::TessControl: stage = Shader::Stage::TessellationControl; break;
                case ProgramStage::TessEval: stage = Shader::Stage::TessellationEval; break;
                case ProgramStage::Geometry: stage = Shader::Stage::Geometry; break;
                case ProgramStage::Fragment: stage = Shader::Stage::Fragment; break;
                case ProgramStage::Task: stage = Shader::Stage::Task; break;
                case ProgramStage::Mesh: stage = Shader::Stage::Mesh; break;
                default: return;
                }
                Shader::ProgramHeader sph{};
                std::memcpy(&sph, record.code.data(), sizeof(sph));
                try_enqueue_shader(stage, sph, record.code);
            }
            while (converter->Pending() > MaxPendingJobs && !stop.stop_requested()) {
                if (auto r = converter->Next(true)) {
                    consume(std::move(*r));
                }
            }
        };
        const auto on_pair = [&](const ShaderPairRecord& pair) {
            if (pair.vs.code.size() <= 128 || (pair.vs.code.size() - 128) % 16 ||
                pair.ps.code.size() <= 128 || (pair.ps.code.size() - 128) % 16) {
                return;
            }
            const u128 vs_h = Hash128(pair.vs.code);
            const u128 ps_h = Hash128(pair.ps.code);
            const PairIdentity pid{vs_h, ps_h};
            if (pair_identities.insert(pid).second) {
                extracted_pairs.push_back(pid);
            }
        };
        const auto on_member_error = [&](const std::string& error) {
            ++member_errors;
            LOG_DEBUG(Render_Vulkan, "Shader precache: {}: {}", archive.path, error);
        };
        try {
            WalkArchive(data, name, &dictionaries, on_record, on_pair, on_member_error);
        } catch (const ArchiveError& e) {
            ++archive_errors;
            LOG_WARNING(Render_Vulkan, "Shader precache: {}: {}", archive.path, e.what());
        }
        drain();
    }

    for (size_t pkg_index = 0; pkg_index < generic_packages.size(); ++pkg_index) {
        if (stop.stop_requested()) {
            converter.reset();
            return fail("Stopped");
        }
        report(StageExtracting, archives.size() + pkg_index, total_archives);
        const auto& pkg = generic_packages[pkg_index];
        const u64 file_size = pkg.file->GetSize();
        if (file_size < NativeHeaderSize + 32) {
            continue;
        }

        std::vector<u8> window(MaxBufferSize);
        size_t carry_over = 0;
        u64 file_offset = 0;
        ZstdScannerContext zstd_ctx;

        while (file_offset < file_size && !stop.stop_requested()) {
            const size_t to_read = static_cast<size_t>(
                std::min<u64>(ChunkSize, file_size - file_offset));
            const size_t bytes_read = pkg.file->Read(
                window.data() + carry_over, to_read, file_offset);
            if (bytes_read == 0) {
                break;
            }

            const size_t chunk_valid = carry_over + bytes_read;
            const std::span<const u8> active_chunk(window.data(), chunk_valid);

            ScanGenericRomFSChunk(active_chunk, try_enqueue_shader);
            ScanZstdFramesInChunk(active_chunk, zstd_ctx, try_enqueue_shader);

            file_offset += bytes_read;
            if (file_offset < file_size) {
                carry_over = std::min<size_t>(chunk_valid, MaxCarryOverTail);
                std::memmove(window.data(), window.data() + chunk_valid - carry_over, carry_over);
            } else {
                carry_over = 0;
            }

            while (converter->Pending() > MaxPendingJobs && !stop.stop_requested()) {
                if (auto r = converter->Next(true)) {
                    consume(std::move(*r));
                }
            }
        }
        drain();
    }
    report(StageExtracting, total_archives, total_archives);

    const size_t total_jobs = seq;
    while (consumed < total_jobs) {
        if (stop.stop_requested()) {
            converter.reset();
            return fail("Stopped");
        }
        report(StageConverting, consumed, total_jobs);
        if (auto r = converter->Next(true)) {
            consume(std::move(*r));
        }
    }
    report(StageConverting, total_jobs, total_jobs);
    converter.reset();

    report(StageWriting, 0, 1);
    compute_out.close();
    if (!compute_out) {
        return fail("Writing the precache file failed");
    }
    // A new file: the count of the old one no longer applies, and its marker only when the
    // contents changed (the marker also carries the driver and variant, checked at boot).
    DropStaleWarmMarker(compute_path, compute_tmp);
    std::error_code ignored;
    fs::remove(WarmCountPath(compute_path), ignored);
    fs::rename(compute_tmp, compute_path, ec);
    if (ec) {
        return fail("Cannot replace " + Common::FS::PathToUTF8String(compute_path));
    }
    WriteWarmCount(compute_path, result.compute_programs);

    const fs::path pairs_path = output_dir / "warm-pairs.bin";
    fs::path pairs_tmp = pairs_path;
    pairs_tmp += ".tmp";
    std::ofstream pairs_out(pairs_tmp, std::ios::binary | std::ios::trunc);
    size_t pairs_written = 0;
    if (pairs_out) {
        WriteHeader(pairs_out);
        for (const auto& p : extracted_pairs) {
            auto vs_it = code_hash_to_lowered.find(p.vs_hash);
            auto ps_it = code_hash_to_lowered.find(p.ps_hash);
            auto vs_stage = code_hash_to_stage_hash.find(p.vs_hash);
            auto ps_stage = code_hash_to_stage_hash.find(p.ps_hash);
            if (vs_it != code_hash_to_lowered.end() && ps_it != code_hash_to_lowered.end() &&
                vs_stage != code_hash_to_stage_hash.end() && ps_stage != code_hash_to_stage_hash.end()) {
                const auto entry = GraphicsPairEntry(vs_it->second, ps_it->second, vs_stage->second, ps_stage->second);
                pairs_out.write(reinterpret_cast<const char*>(entry.data()),
                                static_cast<std::streamsize>(entry.size()));
                ++pairs_written;
            }
        }
        pairs_out.close();
        if (pairs_written > 0) {
            DropStaleWarmMarker(pairs_path, pairs_tmp);
            std::error_code ignored_pairs;
            fs::remove(WarmCountPath(pairs_path), ignored_pairs);
            fs::rename(pairs_tmp, pairs_path, ec);
            WriteWarmCount(pairs_path, pairs_written);
            LOG_INFO(Render_Vulkan, "Shader precache: wrote {} graphics pairs to {}",
                     pairs_written, Common::FS::PathToUTF8String(pairs_path));
        } else {
            std::error_code ignored_pairs;
            fs::remove(pairs_tmp, ignored_pairs);
        }
    }

    if (!graphics_records_bytes.empty()) {
        const auto cache_v2_path = output_dir / VideoCommon::ShaderCacheV2::FileName;
        const auto write_result =
            VideoCommon::ShaderCacheV2::AppendToLog(cache_v2_path, graphics_records_bytes);
        if (write_result != VideoCommon::ShaderCacheV2::WriteResult::Written) {
            LOG_WARNING(Render_Vulkan,
                        "Shader precache: writing {} graphics records to {} failed ({})",
                        result.graphics_programs, Common::FS::PathToUTF8String(cache_v2_path),
                        static_cast<int>(write_result));
        }
    }

    report(StageWriting, 1, 1);
    result.ok = true;
    LOG_INFO(Render_Vulkan,
             "Shader precache build for {:016x}: {} compute entries, {} graphics programs, {} programs skipped, {} "
             "archive and {} member errors, {:.1f} s",
             title_id, result.compute_programs, result.graphics_programs, result.skipped,
             archive_errors, member_errors,
             std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count());
    return result;
}

} // namespace VideoCommon::ShaderWarm
