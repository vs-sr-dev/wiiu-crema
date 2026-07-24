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
- **crema_buffer** — GX2R vertex/index buffers: create, upload, destroy, with
  the cache maintenance the lock/unlock pair implies
- **crema_texture** — RGBA8 textures, CPU box-filtered mip chains, per-level
  upload through a linear staging surface + `GX2CopySurface` (works for any
  tile mode), trilinear sampler setup
- **crema_frame** — frame pacing in three flavours (`GX2DrawDone` per frame,
  fenced pipelining at vsync, fenced uncapped for benchmarks), TV+GamePad
  presentation, and the double-buffered `CremaUniformRing` that makes writing
  next frame's uniforms safe while the GPU still reads the last one
- **crema_mesh** — baked `.cmesh` loading: no parsing, no byteswap, file bytes
  read straight into GPU buffers, and the file describes its own vertex layout
- **crema_matrix** — column-major mat4/vec3 math, GL conventions

The whole engine-grade frame is four calls — this is [poc9](examples/poc9-scene/main.cpp)
in full:

```c
CremaFrameInit(&frame, CREMA_PACING_FENCED, 1);   // vsync, 2 frames in flight
while (CremaAppRunning()) {
    uint32_t slot = CremaFrameBegin(&frame);      // waits on frame N-2 only
    view.globalUbo = CremaUniformRingStore(&globals, slot, &blk, sizeof(blk));
    CremaFrameDrawBoth(SKY, drawScene, &view);    // TV + GamePad
    CremaFrameEnd(&frame, &stats);                // swap, flush, fence
}
```

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
| 10 | `poc10-mesh` | the asset pipeline: baked mesh + texture loaded from the .wuhb, instanced squadron, per-pixel lit | Cemu-verified, HW pending |

To our knowledge these are the first published GX2 polygon/fill throughput
numbers measured from homebrew on real hardware.

## The asset pipeline (`tools/`)

Everything up to PoC 9 generated its geometry in code. PoC 10 loads it, and the
split is deliberate: **all the thinking happens offline, on the PC.**

```sh
python tools/gen_ship.py  examples/poc10-mesh/assets          # the example asset
python tools/crema_bake.py mesh    assets/ship.obj content/ship.cmesh
python tools/crema_bake.py texture assets/hull.png content/hull.ctex
```

The baker interleaves vertices into the exact layout the fetch shader wants,
builds the mip chain, and writes everything **big-endian** — which is the
console's native order. So `CremaMeshLoad` parses nothing and converts nothing:
it `fread`s the blobs straight into GX2R buffers. The file also carries its own
attribute table, which is handed to the shader compiler, so the program never
hard-codes a vertex layout:

```c
CremaMeshLoad(&ship, "/vol/content/ship.cmesh");
shader = CremaShaderCompile(VS, PS, ship.attribs, ship.attribCount);
```

The baker also *checks* the model, because back-face culling turns a winding
mistake into a see-through ship: it splits the mesh into connected components,
verifies each one is closed, and takes the signed volume — positive exactly
when a closed surface faces outwards, whatever its shape. (Comparing normals
against the mesh centre, the obvious version, lies about wings and any flat
part sitting off-centre. Ours caught seven inside-out solids on the first run.)

Assets ship inside the `.wuhb` via `wut_create_wuhb(... CONTENT <dir>)` and are
read from `/vol/content/`. Load timing is measured and logged, but the Cemu
number says nothing about the console — that one waits for hardware.

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
