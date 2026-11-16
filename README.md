<p align="center">
  <img src="dist/branding/lichii_icon_with_logo.png" alt="Lichii Custom" width="550">
</p>

# Lichii - Custom CPU & GPU Optimization for 6-Core Processors

> **High-performance build tailored for 6-core / 6-thread CPUs (e.g. Intel Core i5-9400F, i5-8400, AMD Ryzen 3500) and modern NVIDIA graphics cards.**

This fork eliminates shader stutter, audio crackling, and long compilation freezes in Nintendo Switch 2 emulation by recalibrating worker thread scheduling, removing Windows background throttling, and enabling bounded asynchronous pipeline rendering.

---

## 📊 Live Benchmark Comparison (Uncached Track / Cold Cache)

Tested on a brand-new track without prior cache in **Mario Kart World** (*Nintendo Switch 2 - Program ID 0400C3F00006E000*) with an **Intel Core i5-9400F** and **NVIDIA GeForce RTX 5050**:

| Metric | Build 228 (Original Upstream) | Optimized Build (Custom 1.0) | Real-World Impact |
| :--- | :--- | :--- | :--- |
| **Max Frame Freeze (Frametime Spike)** | **981 ms (~1.0s full freeze)** | **75 ms** | **92.4% reduction in stutter**. No freezes while racing. |
| **Severe Hitches (> 100 ms)** | **9 frames** (per 600 presents window) | **0 (ZERO frames > 100 ms)** | Complete elimination of game-breaking stutter. |
| **Driver Shader Compile Time** | **600 ms – 1,081 ms** per shader | **4.4 ms – 75 ms** per shader | **Shaders build up to 15x faster** on the GPU. |
| **Host GPU Fence Blocking Time** | **160,953 µs (160 ms)** | **4,010 µs (4.0 ms)** | Vulkan command queue executes without driver backlog. |
| **60 Hz Real-Time Consistency** | **Drops to 87% (slow motion)** | **99% – 100% locked** | 100% stable game speed and pristine audio. |

---

## 🍳 The Problem & The Kitchen Analogy (For Beginners)

Imagine a restaurant kitchen with **6 cooks** (representing the 6 CPU cores of a processor like the Intel Core i5-9400F):
* The video game requires **4 dedicated cooks** working non-stop just to maintain game logic at 60 FPS and stream audio without stuttering.
* That leaves **2 cooks available** on the counter.
* **What the original emulator did:** Every time a new visual effect appeared on screen (a new power-up, particle, or explosion), the emulator spawned **8 additional helpers** simultaneously to compile the graphics shaders.
* **The bottleneck:** 14 workers tried to squeeze into a space built for 6! They constantly bumped into each other. Dishes were delayed (the game screen froze for 0.5s–1.5s), and the waiter dropped trays (audio crackled and popped).

---

## ⚙️ Key Technical Optimizations

### 1. Smart Worker Thread Allocation (`vk_pipeline_cache.cpp`)
* Automatically detects host CPU concurrency.
* On $\le 6$-thread processors (e.g. i5-9400F), limits compilation to **2 CPU workers + 1 Driver worker**.
* Permanently preserves 3 to 4 physical cores exclusively for guest CPU logic, physics, and audio emulation.
* Limits NVIDIA driver workers to 1 on $\le 8$ threads to avoid internal driver serialization stalls.

### 2. Default Bounded Async Pipeline Draws (`settings.h`)
* Changed default `native_async_draws` from `Wait` to `Bounded` for native Switch 2 titles.
* When an unbuilt pipeline is encountered, the emulator skips the unbuilt draw for a single frame instead of freezing the render thread for 1 to 2 seconds.

### 3. Mutex Contention Resolution & High-Priority Scheduling (`shader_jobs.cpp`)
* Removed Windows `SetCurrentThreadToBackgroundWork()` and raised worker priority from `Low` to `Normal` with all-core affinity.
* Moved `driver_condition.notify_one()` and `notify_all()` outside the `std::scoped_lock` mutex, eliminating thread collision on wakeup.
* Slashed individual driver compile times from **1,081 ms down to 4.4 ms – 75 ms**.

---

## 📖 In-Depth Documentation

* 🇬🇧 **[SHADER_OPTIMIZATIONS.md](SHADER_OPTIMIZATIONS.md)**: Full technical and beginner-friendly documentation of all changes.
* 🇬🇧 **[BENCHMARK_REPORT_BUILD_228_VS_OPTIMIZED.md](BENCHMARK_REPORT_BUILD_228_VS_OPTIMIZED.md)**: Detailed telemetry analysis, graphs, and long-session memory diagnostics.
* 🇧🇷 **[OTIMIZACOES_SHADERS_PT.md](OTIMIZACOES_SHADERS_PT.md)**: Versão completa da documentação em Português.
* 🇧🇷 **[RELATORIO_COMPARATIVO_BUILD_228_VS_OTIMIZADA_PT.md](RELATORIO_COMPARATIVO_BUILD_228_VS_OTIMIZADA_PT.md)**: Relatório comparativo com métricas de telemetria em Português.
* 🏛️ **[README_UPSTREAM.md](README_UPSTREAM.md)**: Original Lichii upstream documentation and license disclosures.

---

## 📥 Downloads & Installation

Compiled binaries for Windows, Linux, and macOS are automatically packaged and published via GitHub Actions.
Check the **[Releases](https://github.com/wallaceman3/lichii-custom-optimization-cpu-and-gpu/releases)** or the **[Actions Tab](https://github.com/wallaceman3/lichii-custom-optimization-cpu-and-gpu/actions)** to download the latest builds.
