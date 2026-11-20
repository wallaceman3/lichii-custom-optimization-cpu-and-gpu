## Lichii Custom Optimization v1.13

### Highlights & Performance Optimizations

#### 1. CPU Stutter & Frame Pacing Engine
* **ZeroTrim working-set thrashing elimination:** The kernel zero-page trimming thread (`TrimZeroPages`) now tracks newly zero-filled allocations. After initial boot passes, if fewer than 64 MiB of fresh zeroed memory has been allocated, it enters an exponential backoff sleeping up to 300 s (with emergency fallback if system commit headroom drops below 2 GiB). This eliminates ~113 MiB of cyclic working-set evictions every 11 seconds, preventing ~29,000 periodic soft page faults and 1.0–1.6 s CPU hitches during gameplay.
* **Dual-VSync clock race elimination (30 FPS Mode):** Extends the `b.lo` to unconditional branch bypass on `vblank_wait_site` in the loader to fixed 30 FPS mode (`mkw_30fps`). This lets `PaceToVblank` (`swap_interval = 2`) act as the sole authoritative master clock, eliminating the 0.3 ms drift between CoreTiming and host steady-clock that caused 33.3 ms frames to drop into 66.6–73.3 ms hitches.
* **8 MiB Shader Stage Texture Pool:** Expands `MaxFlushedTexturePoolBytes` from 4 MiB to 8 MiB. Titles with large descriptor pools (such as Mario Kart World's 4.2 MB pool with 131,372 descriptors) now translate asynchronously across background shader workers instead of falling back to synchronous translation on `GPUBack`.
* **Lock-free OnCPURead fast path:** `texture_cache.mutex` acquisition during `FlushRegion` / `FlushRegionForRead` is now guarded by `page_filter.Any(addr, size)`, and `MaxReadBackPages` is raised from 512 to 2048. Eliminates mutex contention between guest CPU culling reads and GPU texture submission.
* **Macro 0x142 DMA coordinate page cache:** Caches the last clean 4 KiB DMA page in `MacroEngine::Execute`, eliminating hundreds of redundant `FlushPending` and `IsMemoryDirty` lock acquisitions during consecutive 24-byte coordinate batch reads.
* **Abandoned SamplesPassed query suppression:** Automatically suspends `StartCounter` / `EndCounter` when 4 consecutive 1024-slot batches of `SamplesPassed` queries are dropped without reading, immediately reactivating upon the guest's first `ResetCounter` / `WriteCounter`. Saves over 1 million redundant query commands per session.

#### 2. Dynamic Physical VRAM Target & Proportional Garbage Collector
* **Dynamic 85% Physical Dedicated VRAM Target:** Replaces the volatile DXGI budget-based cap with a clean soft target calculated from the GPU's total dedicated physical VRAM (`main_heap_size` from Vulkan device local memory). On an 8 GB GPU (e.g. RTX 5050), usable headroom expands from ~6.07 GB to ~6.95 GB (~7,120 MiB), while guaranteeing a fixed 15% safety margin for Windows DWM and preventing system commit exhaustion. Cards with 12 GB scale to ~10.4 GB, and 16 GB cards scale to ~13.9 GB.
* **Proportional 3-tier Garbage Collector:** Evaluates usage percentage against total physical VRAM, with emergency eviction triggered at $\ge 85\%$ and high priority at $\ge 75\%$. Internal spacing between critical, expected, and minimum thresholds now scales proportionally (`min(critical / 10, 768 MiB)` and `min(expected / 4, 1 GiB)`), eliminating texture thrashing and re-decode overhead across long races.

### Measured on

i5-9400F (6C/6T), RTX 5050 8 GB, 16 GB DDR4, Windows 11 — Mario Kart World 1.8.0, 1080p docked:
* Frame pacing: rock-solid 33.33 ms in 30 FPS mode with zero 100+ ms cyclic spikes
* ZeroTrim CPU overhead: 0 ms during active racing (backed off for 300 s)
* VRAM usage: perfectly stable within 85% physical ceiling, 0 MiB host spill, 0 crashes

### Credits

* **Upstream Author:** Zortos
* **Memory & VRAM Architecture:** Tasarren
