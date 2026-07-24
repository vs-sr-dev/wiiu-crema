# Crema ☕

**A clean-room GX2 rendering framework for Wii U homebrew** — it sits on top
of *Espresso* (the CPU) and *Latte* (the GPU), like any good crema should.

Crema is a small, honest layer: runtime GLSL shaders, cache-safe resource
handling, frame pacing, and the hard-earned lessons of getting GX2 code from
the Cemu emulator onto real silicon. It ships with nine progressive examples,
each one verified on real hardware, culminating in a fly-through 3D scene at
a rock-solid 59.94 fps with the CPU fully idle.

Every byte is home-grown (MIT). No SDK leaks, no foreign engine code.

## What the framework provides (`crema/`)

- **crema_app** — ProcUI lifecycle, UDP + Cemu logging, per-second frame
  stats (fps / frame ms / GPU-drain ms)
- **crema_shader** — runtime GLSL → Latte compilation via
  [CafeGLSL](https://github.com/Exzap/CafeGLSL), fetch-shader construction,
  VS/PS uniform-block reflection, big-endian-safe uniform upload
  (`CremaUniformStore` byteswaps + invalidates — the GPU reads LE)
- **crema_matrix** — column-major mat4/vec3 math, GL conventions

## The examples (`examples/`)

| # | Example | What it proves | Real-HW result |
|---|---------|----------------|----------------|
| 1 | `poc1-triangle` | WHBGfx init, runtime GLSL, GX2R buffers | 60 fps |
| 2 | `poc2-wirecube` | mat4 uniform block (byteswapped!), U16 index buffer, LINES | 60 fps |
| 3 | `poc3-flatcube` | depth test, back-face culling, flat shading | 60 fps |
| 4 | `poc4-gouraud` | procedural torus, per-vertex diffuse+specular | 60 fps |
| 5 | `poc5-stress` | geometry throughput, escalating workload, vsync off | **130 Mtris/s** @ 1.18M tris/frame |
| 6 | `poc6-engine` | naive draws vs display list vs instancing | **159 fps / 188 Mtris/s** (instanced) |
| 7 | `poc7-fillrate` | ROP fill, linear-vs-tiled textures, per-pixel ALU cost | **1.67 Gpix/s** flat fill |
| 8 | `poc8-fence` | GX2DrawDone vs fenced pipelining, double-buffered UBOs | **162.6 fps / 191.8 Mtris/s** |
| 9 | `poc9-scene` | the pieces assembled: fly-cam scene, mipmapped ground, fog, instancing, TV+DRC | 59.94 fps, 0.00 ms CPU sync |

To our knowledge these are the first published GX2 polygon/fill throughput
numbers measured from homebrew on real hardware.

## Lessons learned (Cemu vs real hardware)

All of these render fine in Cemu and fail on the console — found the hard way,
one per PoC. If you write GX2 code, this list is the part you want:

1. **`GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_BLOCK)`** is required on
   hardware for CafeGLSL shaders; Cemu renders without it.
2. **Uniform-block data must be byteswapped** (32-bit words) and
   `GX2Invalidate`d — the GPU fetches them little-endian, the CPU is BE.
3. **`GX2Invalidate` after *every* CPU write** to GPU-visible memory. A
   `memset` whose cache lines flush *after* a `GX2CopySurface` overwrites the
   GPU's output with stale zeros (random black pixels, hardware only —
   Cemu does not emulate the Espresso caches).
4. **Display lists must contain pure commands only** (uniform binds, draws).
   `GX2SetShaderMode` or GX2R calls inside a display list hang the GPU on
   replay. Even then, uniform-block-heavy DLs stalled the CP on our hardware —
   instancing beat them anyway.
5. **`WHBGfxBeginRender` waits for *every* flip**: with 2 frames in flight it
   pins you to 59.94 Hz even with `GX2SetSwapInterval(0)`. For uncapped
   pipelining, wait on `GX2GetSwapStatus` yourself (≤2 swaps outstanding).
6. **Mipmaps are not optional.** A minified no-mip ground texture thrashes the
   texture cache: 60 → 30 fps from just looking down. Box-filter a chain,
   upload per level via `GX2CopySurface`, sample trilinear.

Also: front faces are **CCW seen from outside** with culling on, and tiled
textures (`GX2_TILE_MODE_DEFAULT`, GPU-swizzled from a linear staging copy)
are the correct default — linear only survives while the texture fits in
cache.

## Building

Requirements: Docker (the official `devkitpro/devkitppc` image provides
devkitPPC + wut + wuhbtool). No native toolchain install needed.

```sh
./build.sh              # cmake + make + .wuhb packaging for all examples
./build.sh clean
./build.sh shell        # interactive shell inside the container
```

### Shaders at runtime: CafeGLSL

Crema compiles GLSL on the console through Exzap's CafeGLSL (not bundled —
fetch it with `external/get-cafeglsl.sh`), installed as:

- Cemu: `<Cemu data dir>/cafeLibs/glslcompiler.rpl`
- real hardware (Aroma): `sd:/wiiu/libs/glslcompiler.rpl`

For shipping builds you can bake the compiled shader binaries and skip the
compiler entirely (the Immaterial demo pattern).

### Running

- **Cemu** (2.1+): `Cemu.exe -g build/examples/.../pocN_*.wuhb`. Iterate here —
  seconds per cycle.
- **Real hardware** (Aroma): copy the `.wuhb` to `sd:/wiiu/apps/`, logs
  broadcast over UDP port 4405 (`tools/udplog.ps1` is a minimal listener).
  Certify here — see the lessons above for why you must.

## Measured hardware profile (Latte, 550 MHz R7xx-class)

- Geometry: **191.8 Mtris/s** sustained (Gouraud, instanced, fenced) ≈ 1.8
  cycles/triangle — near the 1 tri/clock setup limit. ~2.2M tris/frame @ 60 fps.
- Fill: **1.67 Gpix/s** opaque (≈30 fullscreen 720p layers per frame @ 60).
- Heavy per-pixel math (≈10 transcendentals): ~2.7 ms per fullscreen 720p pass.
- Engine recipe that gets you there: static vertex/uniform data, animation in
  the vertex shader, instancing, fenced pacing — CPU cost ≈ 0.1 ms/frame.

## Credits

- [wut](https://github.com/devkitPro/wut) & devkitPPC — devkitPro team
- [CafeGLSL](https://github.com/Exzap/CafeGLSL) — Exzap & Crementif
- GX2 knowledge base: [decaf-emu](https://github.com/decaf-emu/decaf-emu),
  [WiiUBrew](https://wiiubrew.org),
  [Immaterial](https://github.com/glastonbridge/immaterial-wiiu-demo) write-ups

## License

MIT — see [LICENSE](LICENSE).
