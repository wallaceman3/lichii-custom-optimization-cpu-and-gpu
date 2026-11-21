## Lichii Custom Optimization v1.14 (build-366 Integration)

### Highlights & Performance Optimizations

#### 1. Upstream build-366 Integration
* **Third GPU Thread Integration:** Integrates the upstream extra GPU follower thread dedicated to Switch 2 draws while adapting Smart Pipeline Worker limits to prevent thread thrashing and CPU core contention on 6-core/6-thread processors.
* **Precompile All Shaders Compatibility:** Full compatibility with the new up-front disk resource precompiler, utilizing dynamic Boot Turbo worker scaling during cache loading.
* **Switch 2 Editions & Updates Support:** Native support for .nup update bundles, loose update directories, and Switch 2 edition control metadata.
* **Game-Specific Fixes (Upstream):** Incorporates upstream stability and visual fixes for *The Legend of Zelda: Ocarina of Time* (cloth physics and fence jump animation under 60 FPS), *Xenoblade Chronicles DE* (docked/handheld transition stability), *The Duskbloods* (FSR 3.1 motion and IPv6), and *Mario Kart World* (save data storage).

#### 2. CPU & Memory Architecture
* **16-Way Associative UBO Stream Cache:** Eliminates recurrent uniform buffer staging reallocations and memcpy stalls during multi-actor rendering (such as 12-kart racing packs in Mario Kart World).
* **Nx2FastDraw Same-Value Write Filter:** Prevents redundant draw breaking when game macros reissue identical pipeline registers.
* **Shared Dynarmic JIT Executable Cache:** Shares a single 1 GiB executable JIT cache across all guest CPU cores on x86-64, eliminating redundant code generation and saving 800–900 MiB of system memory.
* **Guest CPU Spin-Loop Yielding:** Detects tight `CNTPCT` counter polling loops and yields the physical host CPU thread after 2,000 iterations to prevent guest worker thread starvation.

#### 3. Dynamic Physical VRAM Target & Proportional Garbage Collector
* **Dynamic 85% Physical Dedicated VRAM Target:** Allocates GPU device memory based on total dedicated physical VRAM (~6.95 GB on 8 GB GPUs, ~10.4 GB on 12 GB, ~13.9 GB on 16 GB), reserving a safe 15% margin for Windows DWM and host commit.
* **3-Tier Proportional GC:** Multi-stage eviction hierarchy (emergency at $\ge 85\%$, high priority at $\ge 75\%$, relaxed under 75%) that scales thresholds proportionally with hardware capacity, preventing texture thrashing across long gameplay sessions.
* **Separate Vulkan Dedicated Image and Buffer Pools:** Cleanly distinguishes image allocations from buffer pools through Direct VMA allocation, ensuring zero driver probe churn.

### Measured on

i5-9400F (6C/6T), RTX 5050 8 GB, 16 GB DDR4, Windows 11 — Mario Kart World 1.8.0, 1080p docked:
* Frame pacing: rock-solid 33.33 ms in 30 FPS mode and fluid high-FPS pacing with 0 ms ZeroTrim stall
* Multi-actor rendering: smooth frametimes even with full racing packs in viewport
* VRAM: perfectly bounded within physical budget, zero host RAM spill

### Credits

* **Upstream Author:** Zortos
* **Memory & VRAM Architecture:** Tasarren
