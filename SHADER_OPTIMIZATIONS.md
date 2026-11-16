# Shader Compilation Optimizations for 6-Core CPUs (Lichii-nx2)

This document provides a clear, technical, and beginner-accessible explanation of the high-performance pipeline optimizations implemented in the emulator.

---

## 🍳 The Kitchen Analogy (For Beginners)

Imagine a restaurant kitchen with **6 cooks** (representing the 6 CPU cores of a processor like the Intel Core i5-9400F):
* The video game requires **4 dedicated cooks** working non-stop just to maintain game logic at 60 FPS and stream audio without stuttering.
* That leaves **2 cooks available** on the counter.
* **What the emulator did previously:** Every time a new visual effect appeared on screen (a new power-up, particle, or explosion), the emulator spawned **8 additional helpers** simultaneously to compile the graphics recipes (the shaders).
* **The bottleneck:** 14 workers tried to squeeze into a space built for 6! They constantly bumped into each other. Dishes were delayed (the game screen froze for 0.5s–1.5s), and the waiter dropped trays (audio crackled and popped).

---

## 1. Why Were These Changes Made? (The Problem)

1. **The Reality of 6-Core CPUs:** A vast portion of the player base uses 6-core processors (such as the 6-thread i5-9400F, i5-8400, Ryzen 3500; and 12-thread chips like the Ryzen 5600 and i5-12400).
2. **Switch 2 CPU Emulation Overhead:** The Switch 2 emulates up to **6 guest CPU cores** (`NUM_CPU_CORES = 6`). When the host CPU allocates 8 shader workers, all CPU cores become completely saturated. This starves the emulator's JIT and audio threads, causing massive *Shader Stutter* and audio dropouts.
3. **Forced Wait on Switch 2 Draws:** By default, asynchronous shader compilation was disabled for native Switch 2 titles (`NativeAsyncDraws::Wait`), forcing the game to freeze whenever an unbuilt shader was requested.
4. **Lock Contention on Thread Wakeup:** When a worker finished the first compilation stage and woke the driver thread, it called `notify_one()` while still holding the condition variable mutex. The receiving thread woke up on another core, immediately collided with the locked mutex, and was forced back to sleep by the Windows scheduler.

---

## 2. What Was Done? (The Solutions)

1. **Smart Worker Thread Scaling:** Instead of spawning 8 worker threads indiscriminately, the emulator now dynamically calculates the ideal worker count based on the host CPU's hardware concurrency.
2. **Eliminated Windows Throttling (Normal Priority):** Replaced `ThreadPriority::Low` and `SetCurrentThreadToBackgroundWork()` with `Normal` priority and all-core affinity, preventing the Windows kernel from placing shader tasks into low-power background efficiency queues.
3. **Enabled Bounded Asynchronous Mode (`Bounded`) by Default:** Native Switch 2 draws no longer lock the main thread indefinitely. The emulator waits up to a strict microsecond budget; if the shader is heavy, it skips the draw for a single frame (visually imperceptible) and preserves rock-solid 60 FPS pacing.
4. **Clean Condition Variable Signalling (Notify Outside Mutex Lock):** Workers now release the `std::scoped_lock` before notifying waiting threads, eliminating unnecessary context switching and sleep cycles on Windows.

---

## 3. How It Was Implemented (Code Locations)

### A. Automatic Thread Allocation by CPU Topology
* **File:** `src/video_core/renderer_vulkan/vk_pipeline_cache.cpp`
* **Implementation:** Added `GetSmartPipelineWorkers(&device)`:
  * **CPUs with $\le 6$ hardware threads (e.g. i5-9400F):** Allocates exactly **2 CPU workers + 1 Driver worker**. This leaves 3 to 4 physical cores 100% free for game emulation, GPU submission, and audio.
  * **CPUs with 12 threads (e.g. Ryzen 5600, i5-12400):** Allocates **3 CPU + 2 Driver workers**.
  * **CPUs with $\ge 16$ threads (Ryzen 7/9, i7/i9):** Scales up to **6 CPU + 3 Driver workers**.
  * **NVIDIA Driver Optimization:** On NVIDIA GPUs (e.g. RTX 5050 / RTX 3060), driver workers are capped to 1 on $\le 8$ threads to avoid internal driver serialize locks.

### B. High-Performance Thread Scheduling
* **File:** `src/video_core/shader_jobs.cpp`
* **Implementation:** Replaced `ThreadPriority::Low` and background work flags with:
  ```cpp
  Common::SetCurrentThreadPriority(Common::ThreadPriority::Normal);
  Common::SetCurrentThreadToAllCores();
  ```

### C. Default Bounded Async Shaders for Switch 2
* **File:** `src/common/settings.h`
* **Implementation:** Changed default value of `native_async_draws` from `Wait` to:
  ```cpp
  NativeAsyncDraws::Bounded
  ```

### D. Lock-Free Condition Variable Notification
* **File:** `src/video_core/shader_jobs.cpp`
* **Implementation:** In `RunCpu` and `Promote`, moved `driver_condition.notify_one()` and `notify_all()` outside the `std::scoped_lock{mutex}` scope.

---

## 4. Real In-Game Benchmark Comparison (Build 228 vs. Optimized Build)

Tested on a brand-new track without shader cache (*Cold Cache*) in Mario Kart World (Switch 2) using an Intel Core i5-9400F and NVIDIA RTX 5050:

| Metric | Build 228 (Original Upstream) | Optimized Build (Custom 1.0) | Real-World Impact |
| :--- | :--- | :--- | :--- |
| **Max Frame Freeze** | **981 ms (~1.0s freeze)** | **75 ms** | **92.4% reduction in stutter** |
| **Severe Freezes (> 100 ms)** | **9 frames** (per 600 presents) | **0 frames (ZERO)** | Perfectly smooth driving |
| **Driver Shader Compile Time** | **600 ms – 1,081 ms** | **4.4 ms – 75 ms** | Compiles up to 15x faster |
| **Real-Time 60 Hz Consistency** | **Drops to 87% (slow motion)** | **99% – 100% locked** | 100% stable game speed |

For in-depth analysis and telemetry logs, refer to [BENCHMARK_REPORT_BUILD_228_VS_OPTIMIZED.md](BENCHMARK_REPORT_BUILD_228_VS_OPTIMIZED.md).
