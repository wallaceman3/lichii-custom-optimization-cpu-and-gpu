<!--
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
-->

# Lichii (Custom CPU & GPU Optimization Fork)

<p align="center">
  <img src="dist/branding/lichii_icon_with_logo.png" alt="Lichii" width="500">
</p>

An enhanced, high-performance fork of **[zortos293/Lichii](https://github.com/zortos293/Lichii)**, engineered to deliver perfectly stable frame pacing, eliminate shader compilation stutter on 6-core/6-thread CPUs, and significantly reduce system RAM and GPU VRAM consumption.

---

## 🚀 Key Optimizations in this Build

### 1. Smart Shader Worker Scaling (`GetSmartPipelineWorkers`)
- **Gameplay Protection:** On 6-core/6-thread CPUs (e.g., Core i5-9400F, i5-8400, Ryzen 3500), shader compilation threads are balanced to **2 CPU + 1 Driver** worker during gameplay, preventing thread thrashing and starving the guest CPU, audio, and Vulkan threads.
- Scalable dynamic profiles for 4-thread, 8-thread, 12-thread, and 16+ thread CPUs.

### 2. VIP Shader Queue & Batch Syscalls
- **4-Tier Priority Hierarchy:** `NeedNow` (blocking) -> `Vip` (essential gameplay) -> `Soon` (effects/particles) -> `Background` (precache).
- Shaders with depth-write enabled and compute shaders are automatically classified as **VIP**, ensuring ground, environment, and characters compile before transparent smoke or dust.
- **Syscall Batching (`BeginBatch` / `BatchScope`):** Coalesces rapid multi-shader dispatches into a single mutex acquisition and single `notify_all()` kernel syscall, eliminating context-switch storms.

### 3. Nx2FastDraw Same-Value Write Filter
- Prevents spurious FastDraw breaks when game macros rewrite the same viewport, blend state, or vertex buffer parameters with identical values (`if (regs.reg_array[method] == argument) return;`).
- Adds specialized routing for `IsStoreOnlyExecutable` registers, keeping FastDraw active across dense instances and foliage.

### 4. GPU ASTC & Unswizzle Compute Thresholds
- High-resolution textures and map streaming data ($\ge 4096$ bytes or $\ge 64\times 64$) are decoded and unswizzled in microseconds via GPU Compute Shaders (`astc_decoder.comp`).
- Small UI icons bypass GPU queue barriers and decode in nanoseconds on the CPU. Complete Vulkan memory barriers prevent texture flickering on first display.

### 5. Precision Hybrid Frame Pacing (`PresentManager` + Mailbox)
- **`VK_PRESENT_MODE_MAILBOX_KHR`:** Decouples the emulator from rigid FIFO VSync half-rate drops (no more 60 $\to$ 30 FPS drops on 0.1 ms misses).
- **Zero-Stall Mailbox & Auto Presentation:** In Mailbox VSync and Auto pacing modes, avoids redundant CPU spin-waits and artificial thread delays, delivering fresh frames immediately to the swapchain for maximum throughput on 6-core CPUs while keeping pacing telemetry accurately logged.
- **Hybrid `SleepUntil`:** For fixed target modes (30/60 FPS), uses kernel OS sleep with a trimmed 250 µs spin-wait to save CPU cycles.
- **Anti-Burst Protection:** Resets pacing cadence after loading screens or long hitches to prevent fast-forward frame bursts.

### 6. Asynchronous N-1 Occlusion Queries
- In `SamplesQueryBank::Sync`, occlusion queries (`ZPassPixelCount64`) return the previous frame's result when pending (`VK_NOT_READY`), eliminating 2–8 ms CPU stalls in the middle of a frame.
- Transform Feedback and physics queries remain strictly synchronous for 100% collision accuracy.

### 7. Guest CPU Busy-Polling Mitigation
- Automatically detects tight spin-loops in Dynarmic (`GetCNTPCT`) and yields the physical host CPU thread after 2,000 tight iterations.
- Detects `svcSleepThread(0)` and explicit yield SVCs to prevent idle guest threads from burning 100% of a physical CPU core.

### 8. VRAM Reduction, 3-Tier GC & Memory Architecture (Credits: Tasarren)
- **Direct VMA Memory Allocation:** Streamlines image creation directly through Vulkan Memory Allocator to eliminate probe/destruction overhead while preserving 4 MiB buffer blocks.
- **Dynamic VRAM Target (85% DXGI Budget):** Soft target caps device memory usage to 85% of available budget (~6.1 GB on 8 GB cards), reserving ample head-room for Windows DWM and preventing system commit exhaustion.
- **Discrete GPU Memory Threshold Protection:** Distinguishes integrated GPUs from discrete GPUs so non-texture allocations never inadvertently inflate discrete GPU retention floors up to the physical memory limit.
- **3-Tier Texture Garbage Collector (Anti-Thrashing):**
  - **< 75% VRAM:** Relaxed retention (90 frames, ~1.5–3s), preserving textures across corners and turns with zero re-decode thrashing.
  - **75%–85% VRAM:** Moderate eviction (60 frames, 32 iterations/frame) for steady pruning of stale assets.
  - **> 85% VRAM (Emergency):** High-throughput eviction (15 frames, 128 iterations/frame) to swiftly reclaim space before driver OOM limits.
- **Allocation & ShaderCacheWriter Memory Shield:** Protected with `try/catch` exception handlers in `JoinImages` and `CacheWriter` to ensure low-commit spikes or cache write delays never trigger process aborts.

### 9. Draw Submission Batching (`DefaultNativeBudget = 256`)
- Elevates native desktop submission batching from 128 to 256 draw calls, cutting Vulkan queue recording and submission frequency in half on 6-core CPUs while keeping discrete GPUs continuously supplied with work.
- Guarantees immediate flushing on swapchain present and key render target transitions.

### 10. Low-Latency Micro-Spin for GPU Readbacks (`WaitForSyncOperation`)
- Employs an ultra-short adaptive pause micro-spin loop prior to invoking the kernel condition variable wait during GPU-to-CPU read synchronization (`OnCPURead`).
- Eliminates Windows kernel context switches and thread descheduling when fast discrete GPUs complete transfers in microseconds, with reliable fallback to kernel waits if transfers take longer.

---

## 🎮 Recommended Settings Guide

Open **Emulation > Configure** (or `Ctrl+,`):

* **Graphics:**
  - **Resolution:** 1X (720p/1080p).
  - **VSync Mode:** `Mailbox` (configured by default for flat frametimes).
  - **ASTC Recompression:** `BC3` (or Uncompressed if VRAM $\ge 8$ GB).
  - **ASTC Decode Method:** `GPU`.
* **Advanced Graphics:**
  - **Frame Pacing Mode:** `Target Auto` or `Target 60`.
  - **Async Shader Compilation:** `Enabled`.
  - **Disk Shader Cache:** `Enabled`.

---

## 👥 Credits and Acknowledgements

This custom optimization fork builds upon the incredible work of the open-source community:

- **[Zortos (zortos293)](https://github.com/zortos293/Lichii):** Creator and main maintainer of Lichii.
- **[Tasarren](https://github.com/tasarren/Lichii):** Author of the VMA dedicated render target/storage pool optimizations and VRAM reduction techniques.
- **Upstream Projects:** Eglantine (Camille LaVey), Eden Emulator Project, yuzu, and Sudachi.

---

## License

Lichii is licensed under the **GNU General Public License v3.0 or later** (GPL-3.0-or-later). See [LICENSE.txt](LICENSE.txt) for details.
