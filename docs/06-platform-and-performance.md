# Platform and GPU Compatibility

## 1. Supported targets

Windows x64 and Apple Silicon macOS arm64 are first-release requirements. Intel Mac, Linux, consoles, and Windows CPU-only/software-rendering modes are outside the release scope. Focus hardware evaluation on gaming laptops and Apple Silicon Macs sold during approximately October 2023–October 2026.

A purchase year does not guarantee a GPU capability, memory budget, sustained clock speed, or frame rate. The reference matrix below is a development target, not a published minimum specification. The final store requirements must come from measured packaged builds.

| Reference tier | Example hardware target | Rendering target | Frame-rate goal |
| --- | --- | --- | --- |
| Windows gaming laptop baseline | RTX 4050 Laptop 6 GB, 16 GB RAM, plugged in | 1080p output, TSR, Balanced/High hybrid | Stable 60 FPS |
| Windows preferred | RTX 4060 Laptop 8 GB or comparable AMD GPU, 16–32 GB RAM | 1080p High, optional 1440p upscaling | Stable 60 FPS |
| Mac base | M2/M3-class base chip, 16 GB unified memory | 900p–1080p output, Performance/Balanced | Stable 30 FPS |
| Mac preferred | M3 Pro-class or newer, 18 GB+ unified memory | 1080p Balanced/High hybrid | Target 60 FPS |
| Recent low-memory Mac stress case | Base Apple Silicon with 8 GB unified memory | Reduced internal resolution and texture residency | Feasibility gate; no untested promise |

Hardware generations alone are insufficient: laptop power limits and shared memory vary. Include a non-NVIDIA Windows GPU in validation. No CUDA requirement. A base fanless Mac cannot be assumed to sustain the same scene as a Pro/Max model. If the 8 GB case cannot meet the smoothness gate, publish the measured memory requirement explicitly instead of claiming every recent Mac is supported.

## 2. GPU use

Windows uses Unreal's DirectX RHI for rasterization, material shaders, lighting, supported compute shaders, and approved Niagara GPU simulation. Prefer D3D12/SM6 on target gaming laptops. Windows chooses from OS-visible adapters; request high-performance graphics preference through supported application metadata/settings and validate hybrid-GPU laptops. Show the selected GPU and active graphics API in Advanced Graphics. Do not claim the game can force a powered-off or driver-disabled adapter.

macOS uses Unreal's Metal RHI and the Apple Silicon GPU. No Windows-only shader, DLL, NVIDIA API, or compute kernel may be required for combat or visuals. Avoid third-party binary plugins without native arm64 support. Keep GPU particle paths and memory allocations validated under Metal.

CPU gameplay and GPU rendering are complementary. GPU acceleration does not mean migrating game logic to CUDA. Every spell has CPU-authoritative hit checks and a scalable visual presentation.

## 3. Feature policy

| Feature | Windows target | Apple Silicon Mac target |
| --- | --- | --- |
| Realistic PBR materials | Required | Required |
| Niagara mesh/ribbon/sprite core | Required | Required; effect-by-effect validation |
| GPU particle simulation | Enabled after capability/performance validation | Enabled only for validated Metal paths |
| Lumen software tracing / evaluated scalable GI | High-quality option | Evaluated quality option |
| Hardware ray tracing | Optional, capable GPUs only | Disabled for shipping baseline |
| Nanite / Virtual Shadow Maps | Per-content/per-device validation | M2+ evaluation; ordinary LOD/shadow fallback |
| Real-time 3D fluid simulation | Research only | Research only |
| TSR / dynamic resolution | Evaluated, exposed settings | Evaluated, exposed settings |

Epic's current Mac documentation lists software Lumen on M1+, Nanite/VSM support on M2+ as beta, and Mac hardware ray tracing on M2+ as experimental. The shipping baseline avoids those experimental dependencies. Development tooling currently lists macOS 14.5 and a version-specific Xcode requirement. These are engine/editor requirements, not automatically the game's final player requirements. [Official Mac development requirements](https://dev.epicgames.com/documentation/en-us/unreal-engine/macos-development-requirements-for-unreal-engine).

## 4. First launch and settings

Read GPU adapter, backend, memory characteristics, display mode, and supported shader features through engine/platform APIs. Select a conservative initial preset by capability class; unified memory must not be mistaken for dedicated VRAM. Run a short optional hardware benchmark before gameplay or use a tested default, then offer a 15-second preview with Revert if the user changes resolution/fullscreen.

Expose preset, texture quality, shadows, GI/reflections, effects detail, view distance, resolution scale, VSync, FPS limit, fullscreen/windowed mode, and optional ray tracing on validated Windows hardware. Advanced unavailable features display a short reason. Saved settings are applied early enough to prevent an expensive startup scene on low-memory Macs.

Avoid changing preset continuously during combat. Dynamic resolution may adapt within user-selected bounds; cosmetic effect density may decrease under measured load without changing gameplay warnings. On Mac, cap the base preset to 30 FPS by default; expose 60 where tested. An unsupported driver/device gets a clear localized compatibility message when feasible before loading a heavy scene.

## 5. Frame budgets

For the Windows baseline, a 60 FPS frame permits 16.67 ms. Initial targets: game thread <=5 ms, render thread <=5 ms, GPU <=14 ms, including VFX <=2 ms. CPU and GPU work overlap; do not add these numbers as if they were sequential.

For the base Mac, a 30 FPS frame permits 33.33 ms. Target GPU <=28 ms, game thread <=8 ms, and VFX <=3.5 ms. The preferred Mac targets the 60 FPS budget with quality reduced where necessary. Memory target: process resident memory <=6 GB on the 16 GB base Mac, and texture residency <=3.5 GB on the 6 GB Windows GPU, subject to actual captures. Editor residency is not the release budget.

After shader warmup, the benchmark target is p95 frame time <=18.5 ms and p99 <=25 ms for 60 FPS tiers; p95 <=35 ms and p99 <=45 ms for the 30 FPS tier. No recurring spikes above 50 ms during combat. Loading screens are excluded but must be separately measured. These thresholds define the initial 'runs smoothly' acceptance contract and may require content reductions.

## 6. Sustained-load validation

Benchmark an identical 15-minute route containing eight enemies, multiple player spells, a boss, rain/fog, and repeated lightning impacts. Test a cold first run and a warm run, then 30 minutes of sustained combat on a fanless Mac. Record graphics settings, output/internal resolution, power state, OS, driver, exact chip, RAM, frame-time distribution, temperature/throttling observations, CPU/GPU timing, texture memory, and build hash.

Use Unreal Insights, `stat unit`, `stat gpu`, GPU profiling, and Metal-specific capture tools as available. Investigate hitching through shader compilation, asset streaming, garbage collection, and transient allocation. Warm necessary PSOs, pre-load immediately needed hero assets, pool short-lived actors/effects, and test camera turns into previously unseen content. Do not use offline rendered videos as performance evidence.

Reference: [Epic's Windows requirements](https://dev.epicgames.com/documentation/en-us/unreal-engine/hardware-and-software-specifications-for-unreal-engine). Actual player compatibility is established by this project's tests, not inferred from editor recommendations.

## 7. Cross-platform parity

Shared content IDs, damage values, cooldowns, campaign flags, and save schema must match. Per-platform differences may include GI, shadows, effect density, upscaling, and output resolution. A Mac particle limitation must never change attack radius or give a hidden gameplay disadvantage.

Release both platform builds from the same gameplay tag. A blocking Mac bug blocks dual-platform 1.0 acceptance. If Store review finishes later than Mac verification, retain the tested matching artifacts and communicate release-channel status honestly.
