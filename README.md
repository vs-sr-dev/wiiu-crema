# Crema ☕

**A clean-room GX2 rendering framework for Wii U homebrew** — it sits on top
of *Espresso* (the CPU) and *Latte* (the GPU), like any good crema should.

Crema is a small, honest layer: runtime GLSL shaders, cache-safe resource
handling, frame pacing, sound, and the hard-earned lessons of getting GX2 code
from the Cemu emulator onto real silicon. It ships with eleven progressive
examples, each one verified on real hardware, culminating in a flyable game —
59.9 fps, the CPU idle, and an engine note that follows the throttle.

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
- **crema_blend** — blend mode (opaque / alpha / additive) and depth as two
  calls, with test and write as separate switches: the transparent pass wants
  test on and write off, or overlapping billboards punch holes in each other
- **crema_effect** — timed billboard effects (flashes, tracers, explosions).
  What separates an effect from an entity is that an effect knows it is going
  to die: it carries its own lifetime and packs itself into an instance array
- **crema_input** — GamePad polling with rescaled dead zones and, the part that
  matters, edge-triggered buttons: `held` fires a gun sixty times a second
- **crema_entity** — a pool of world objects over caller-owned storage, so
  spawning never allocates mid-frame
- **crema_collide** — bounding spheres from the AABB the baker put in the mesh,
  sphere/sphere and ray/sphere. Spheres because an AABB stops bounding anything
  the moment the object rotates
- **crema_audio** — sound through AX, which is not a file player but a hardware
  sampler: a `CremaSound` is PCM in DSP-visible memory (cache-flushed, because
  the DSP reads memory the CPU still holds dirty — lesson 3 with a different
  chip), and a voice is that buffer played at a pitch. One-shots are fired and
  forgotten, reclaimed when they end; a held voice you own and retune every
  frame. Init it *first*: until a title takes AX over, the system keeps playing
  the transition audio it was handed — which is the Wii U Menu's music
- **crema_matrix** — column-major mat4/vec3 math, GL conventions

Nothing here was designed in advance. Every module was extracted from an
example that had already written it — twice, usually — which is why the layer
is small and why each piece has a real caller.

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
| 10 | `poc10-mesh` | the asset pipeline: baked mesh + texture loaded from the .wuhb, instanced squadron, per-pixel lit | 59.9 fps, 356 KB loaded in 36 ms |
| 11 | `poc11-flight` | a game, not a demo: arcade flight model, chase camera, free wingmen, hostiles you can shoot, and an engine note that rides the throttle | 60 fps, CPU idle |

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
read from `/vol/content/` — confirmed working on real hardware under Aroma, not
just in Cemu.

### The same waveform on both machines

PoC 11's sounds are generated in plain C in
[`sounds.h`](examples/poc11-flight/sounds.h) — a file with no console header in
it. That is not tidiness: it means `tools/render_sounds.c` can compile *the same
source* on a PC and write WAVs, so the offline preview is the sound, not a
model of it.

```sh
cc -O2 -o render_sounds tools/render_sounds.c -lm && ./render_sounds out/
```

It reports what an ear cannot: the explosion was clipping 550 samples flat on
its attack (fixed with a `tanh` saturation — an explosion should be squashed,
but by a curve, not a ceiling), and the engine loop's wrap-around step is 359
against a largest in-loop step of 423, so the loop closes without the tick you
would otherwise hear five times a second and blame on the DSP.

### What loading actually costs (measured on console)

Loading a 14.5 KB mesh and a 341 KB mipped texture from the SD card under
Aroma, instrumented until the accounting closed:

| operation | cost |
|---|---|
| `fopen` | ~0.15 ms |
| **first read on a stream** | **~3–4 ms, whatever its size** |
| each read after that | ~0.74 ms fixed |
| the bytes themselves | 15–18 MB/s |
| staging + `GX2CopySurface` for a 341 KB mip chain | ~3.7 ms |
| one `GX2DrawDone` for the whole chain | ~0.25 ms |

That first-read figure is not a typo: **64 bytes cost 3.09 ms** because it is
the read that makes stdio set the stream up, and every filesystem call here is
a round trip to IOSU. Reading 14 KB from the same warm stream then costs 2.28.

So the cost of an asset is dominated by *touching its file at all*, not by its
size — more than half the load time of our 14.5 KB mesh is stream setup. Twenty
assets in twenty files would burn ~80 ms before reading anything useful. One
archive, one open, few big reads.

Applying that took the same two assets from **49 ms to 36 ms (−28%)** without
changing a byte of content: one read for a whole mip chain instead of nine, and
no zeroing of surfaces that are about to be overwritten (the `memset` was
costing more than every GPU wait combined). Keep the `GX2Invalidate` when you
drop the `memset` — the allocation may hold dirty cache lines from its previous
owner, and lesson 3 above is what happens if they flush late.

The one thing that turned out not to matter: waiting for the GPU. We batched
nine per-level syncs into one expecting to save real time and saved nothing
measurable — those copies are tiny and the GPU is idle. The instrumentation was
the part that paid off, not the optimisation it shipped with.

## Lessons learned (Cemu vs real hardware)

All of these look fine in Cemu and are wrong on the console — found the hard
way, roughly one per PoC. If you write GX2 code, this list is the part you want:

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
7. **The Wii U Menu's music keeps playing until you take AX.** The system does
   not stop its own audio when it launches you — it hands it over as
   *transition audio* and waits for the title to end it. Every PoC here ran
   with the menu humming underneath until one of them called `AXInit`.
   Measured, not guessed: `__OSGetSavedAudioFlags()` reads **0x1 before
   `AXInit` and 0x0 after** on hardware (in Cemu it is 0x0 either side — the
   emulator never hands you anything, so this one is invisible there). Init
   audio *first*, before you load a single asset.

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
- Audio: AX reports **96 voices** over a 48 kHz mix, resampling each one from
  whatever rate you baked it at. A handful playing cost nothing we could
  measure — the frame stayed at 59.9 fps with 0.00 ms of CPU sync.
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
