<img src="crema.png" alt="Crema" width="520">

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
- **crema_bank** — instruments: PCM plus the number that makes a sample an
  instrument, how many samples are one cycle. A note is then a playback rate
  and nothing else. The bank keeps its own cache-flushed copy, because unlike
  a texture an instrument is read by the DSP *while it plays*
- **crema_music** — the sequencer, ticking on AX's audio frame (3 ms) instead
  of on the game loop, and never allocating there: channels reserve their
  voices up front, and a note-on re-aims a voice the channel already owns
- **crema_pak** — a `.cpak` archive: one open, two reads, however many assets
  are inside, because on this console the cost of an asset is dominated by
  touching its file at all. No compression and no transformation — a directory
  followed by a concatenation. The mesh and texture loaders grew in-memory
  twins to go with it, and the from-a-path versions are now the special case
- **crema_matrix** — column-major mat4/vec3 math, GL conventions, and the
  world-to-screen projection a HUD marker needs (including the check that
  keeps a marker from appearing for something behind the camera)

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
| 10 | `poc10-mesh` | the asset pipeline: baked mesh + texture loaded from the .wuhb, instanced squadron, per-pixel lit — and the same two assets loaded twice, loose and packed, to measure the difference | 59.9 fps, 356 KB in 38.95 ms loose vs **32.33 ms from one .cpak** |
| 11 | `poc11-flight` | a game, not a demo: arcade flight model, chase camera, free wingmen, hostiles you can shoot, a HUD with the GamePad running its own tactical screen, an engine note that rides the throttle, and a chip tune sequenced on the audio thread | 60 fps, CPU idle |

To our knowledge these are the first published GX2 polygon/fill throughput
numbers measured from homebrew on real hardware.

## The asset pipeline (`tools/`)

Everything up to PoC 9 generated its geometry in code. PoC 10 loads it, and the
split is deliberate: **all the thinking happens offline, on the PC.**

```sh
python tools/gen_ship.py  examples/poc10-mesh/assets          # the example asset
python tools/crema_bake.py mesh    assets/ship.obj content/ship.cmesh
python tools/crema_bake.py texture assets/hull.png content/hull.ctex
python tools/gen_font.py  examples/poc11-flight/assets/font.png
python tools/crema_bake.py texture assets/font.png content/font.ctex --no-mips
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

### The HUD, and giving the GamePad something to do

Text is the one asset a 3D framework always ends up needing and never plans
for. Crema's answer is the same instancing trick the billboards use, moved to
2D: the CPU builds a list of quads, two `vec4` each, and the whole readout —
every letter, the throttle bar, the radar blips, the bracket around a locked
target — is **one draw call**. A negative height means "not a glyph, a solid
rectangle", which is the entire difference between text and geometry.

The font ([`tools/gen_font.py`](tools/gen_font.py)) is drawn as strokes and
rasterised at 4x before being scaled down, so it is smooth at any size and
still ours — no typeface imported. The first version was a hand-placed 5x7
bitmap, which was honest and looked it.

Two things worth stealing:

- **Sample glyph cells clamped to their texel centres, not to their edges.**
  Stop at the cell edge and the bilinear filter drags in the first row of the
  glyph in the next cell down — a phantom underscore under half the alphabet.
  Shrink the *coordinate* to the centres instead and the glyph's own first row
  arrives under-weighted, which reads as a top row shaved off. You need the
  quad to span the full cell and the *fetch* to be clamped inside it.
- **The crosshair does not belong at the centre of the screen.** The camera
  sits behind and above the ship, so the ray out of the nose projects lower
  than the middle. Project a point on the ray itself and the sight lands on
  the gun instead of on the lens.

PoC 11 also stops calling `CremaFrameDrawBoth`: the TV gets the world with a
HUD over it, and the GamePad gets a tactical map — radar rotated into the
ship's own heading, contacts, the locked target — **with no 3D pass at all**.
A second screen that repeats the first is a second screen you render twice.

### Sound: the console is a sampler, so the chip is made offline

AX is not a file player and there is no synthesiser anywhere in it. It is a
hardware sampler: PCM in memory, a pitch, a loop point. Which turns out to be
enough, because **a sampler with pitch and loop control _is_ a sound chip** — a
pulse wave is one cycle in a loop with the duty already baked in, a NES triangle
is its sixteen steps written out exactly, noise is an LFSR sequence generated
once. Each maps onto a voice with nothing left over, and unlike the chip it
imitates you are not limited to four of them.

So the waveforms are made on the PC, where a wrong number is a rerun instead of
a rebuild:

```sh
python tools/gen_waves.py examples/poc11-flight/assets/audio   # WAVs + manifest
python tools/crema_bake.py bank content/audio.cbank assets/audio/bank.json
python tools/gen_song.py  examples/poc11-flight/assets/audio/theme.csong
```

What comes out of the generator is ordinary 16-bit mono WAV, so the preview tool
is whatever plays WAVs on your machine. It also prints what an ear cannot: the
explosion was clipping 550 samples flat on its attack (now saturated by a `tanh`
— an explosion should sound squashed, but by a curve, not by a ceiling), and the
engine loop's wrap-around step is 359 against a largest in-loop step of 423, so
the loop closes without the tick you would otherwise hear five times a second
and blame on the DSP.

The `.cbank` adds the one number a WAV cannot carry and an instrument cannot do
without: how many samples make one cycle. From it, a note is only a playback
rate — `ratio = frequency * cycleSamples / rate` — and that is the entire theory
of playing music on this hardware.

A song (`.csong`) is **a piano roll, not a pattern grid**: a flat list of events
sorted in time. It names its instruments instead of indexing them, so a bank and
a song can be rebuilt separately.

### Importing from chiproll, detuning included

That shape was chosen so that importing from a piano-roll editor would be a
translation rather than a redesign, and
[chiproll](https://github.com/vs-sr-dev/chiproll) is the editor in question:

```sh
python tools/chiproll_import.py session.json content/song.csong
```

Its export carries, per step, both the note you meant and the register the chip
would actually be given — and, because those two do not agree, how many cents
apart they are. **That disagreement is worth importing.** A NES makes its pitch
by dividing a clock by an integer, so it cannot play in tune: E6 comes out 3.3
cents flat, every time, on every NES ever built. Take the notes and you have the
tune; take the notes *and the cents* and you have the machine. So `.csong`
events carry a signed cents byte, and the player adds it as a fraction of a
semitone — which the resampler was always able to do and nothing else in the
chain had to change.

In the imported test session, 23 of its 25 pitched notes are somewhere other
than where equal temperament would put them.

**How far out depends on how high you are**, which is the part worth knowing.
The divider is an integer, so its steps get coarser as the pitch rises: around
E6 two adjacent register values are ~20 cents apart, and landing 3.3 cents from
the right note is nearly luck. Down at A2 on the triangle they are ~3.4 cents
apart and the chip is almost in tune by construction. The session above shows
exactly that — its top notes are 3–4 cents out, its bottom ones 0.06 and 0.4.
Which also means you will not *hear* it on staccato single notes, since a few
cents is under most listeners' threshold; it shows up when two channels hold
the same note or the octave and start to beat.

Two rules of chiproll's format are worth writing down, because they are
deliberate choices rather than accidents, and because together they are what
makes the grid unambiguous:

- **an empty step is a rest, not a held note.** The DAW piano-roll convention,
  and the opposite of FamiTracker, where a blank cell sustains and rests have
  to be written.
- a run of the same note over contiguous steps is *usually* one held note —
  and this is the one thing the importer cannot know for certain.

chiproll distinguishes a note dragged across two cells from two single notes
side by side, but **its JSON export does not carry the distinction**: sixteen
separate G3s and one G3 held across sixteen cells serialise to sixteen
identical cells, differing only in their index. No reader can recover that, so
the importer looks for an explicit tie flag, uses it if it is ever there, and
otherwise reports every run it had to guess at — a silent guess about note
lengths is a silent guess about the music. `--runs=attacks` takes the other
reading.

(The same session exported to FamiTracker text loses it too, in the opposite
direction: it writes the note on all sixteen rows, and in a tracker a blank row
*sustains*, so that spelling means sixteen attacks. The difference is that the
tracker format has the vocabulary and is not using it, while the JSON has no
vocabulary for it at all.)

### The sequencer runs on AX's clock, not on yours

`AXRegisterAppFrameCallback` gives you a tick per audio frame — **3 ms, 333
times a second**, on AX's thread, in time with the DSP rather than with the
picture. That is the difference between music and music that stutters: a game
loop can only place a note to the nearest 16.7 ms, and at 132 BPM a sixteenth
note is 113 ms, so every note would land up to a seventh of its own length late
and land differently each time.

The price is a rule, and it is the oldest one in real-time audio: **the callback
never allocates and never blocks.** Each channel reserves its voice once, on the
game thread, before the song starts; from then on a note-on is a re-aiming of a
voice the channel already owns and a note-off is a stop. Nothing is acquired or
freed while the song plays, so the audio thread and the game thread never have
to agree about who owns what — there is nothing to race over, and no lock in the
one place a lock would be heard.

Measured on real hardware, four channels playing: **333.4 ticks/s and 14 µs in
the worst tick against a 3000 µs frame** — 0.47% of the audio thread, with the
game still at 59.9 fps and 0.00 ms of CPU sync. The voice count sits at 5 and
never climbs, which is the check that matters: five is four channels plus the
engine, and a sequencer that leaked voices would walk it up to the pool limit
and go silent.

Worth noting that the console beat the emulator here — Cemu reported 42 µs for
the same work. It recompiles PPC to x86, but it measures time through its own
overhead, while on hardware `OSGetSystemTime` reads the real timebase and the
code is native.

### Every note was a rectangle, and that was the missing sound

The tune played, and it sounded bare. The instinct is to reach for reverb; the
actual hole was earlier than that. Every note was a rectangle — full volume the
instant it started, full volume until the note-off, then nothing. No real sound
has ever done that, and a melody made of rectangles is a test tone with a tune
in it.

So the `.cbank` grew sixteen bytes per instrument (attack, decay, sustain,
release, and a vibrato that waits before it starts, because that is what a
player does), and the sequencer's tick — which was already running 333 times a
second doing nothing but starting and stopping notes — now shapes them too.
Which raises the problem worth writing down:

**333 Hz is not enough resolution for a volume.** A fade written once per audio
frame is a staircase of 3 ms steps, and a staircase in a volume is a buzz. But
an AX voice does not carry a volume: it carries a volume *and a per-sample
delta*, and Cemu's own mixer says exactly what happens to them
(`ax_mix.cpp`) —

```cpp
volumeScaler += volumeScalerDelta;      // once per sample
sampleData[i] *= volumeScaler;
...
internalShadowCopy->veVolume = veVolume + volumeDelta * sampleCount;
```

So the sequencer says "from here to there, over this frame" and the hardware
fills in the 144 samples between. The envelope thinks at 333 Hz and comes out at
48 kHz.

The second line of that quote is a trap, and it is why the code tracks whether a
voice is mid-ramp: **the delta stays in the voice.** Write one and stop writing,
and the DSP keeps applying it every frame afterwards, walking the volume off on
its own. A note that reaches its sustain has to be told once, explicitly, that
it has stopped moving — after which the channel writes nothing at all, which is
why holding a chord costs the same as holding silence.

Two more things fell out of it. An attack of 2 ms is *shorter than a tick*, so
rounding it to the nearest tick rounds it to zero and puts the click back; it
gets one full frame of ramp instead, and three milliseconds against two is not a
difference anyone can hear. And sustain zero turned out to be a real answer
rather than a degenerate one — it is a drum: the noise channel's voice now ends
itself when the decay runs out instead of looping static until the note-off.

Cost on Cemu: **1 to 13 µs in a typical tick** against a 3000 µs frame, with the
frame rate untouched at 60.1 and `sync 0.00 ms`. The worst-tick figure is not
worth quoting from an emulator — it ranged from 52 µs to 650 µs across runs of
the same binary, which is the JIT and the host scheduler, not the code.
Hardware said 14 µs for the sequencer before envelopes existed and will be the
one to say what they cost. **Not yet measured on hardware.**

### Our own code inside AX's signal path

There is one programmable point in this console's audio pipeline, and wut does
not describe it. `AXRegisterAuxCallback` puts a function of yours in the signal
path — three milliseconds of somebody's mix, once per audio frame, to do as you
like with — and since wut exposes `sndcore2` and nothing else, there is no
effect library to switch on. A reverb here is something you write.

The header declares:

```c
typedef void (*AXAuxCallback)(void *, void *);
```

which is **one argument short**. AX passes three: the channel data, the user
pointer, and a struct saying how many channels and how many samples — and
without that third one you cannot write the loop. The rest was undocumented in
the same way and was read out of Cemu's implementation, which is where a working
emulator had to know the answers:

| | |
|---|---|
| registration | `(device, deviceIndex, auxBus, callback, userData)` — wut's two `unk`s |
| buffer | **planar int32**, an array of `numChannels` pointers, each `numSamples` long |
| shape | 6 channels for the TV, 4 for the GamePad, 144 samples at 48 kHz |
| direction | **in place** — the buffer handed in is the one AX reads back |
| routing | `mix[channel].bus[1]` is the send to aux bus **0**; AX indexes its scratch as `(1 + auxBus)` |
| timing | it processes the **previous** frame, before that frame's app callback — 3 ms of latency |

All of it confirmed at runtime: **338 calls a second, 6 channels × 144 samples**,
the third argument arriving exactly where it was predicted to.

The one thing an emulator could not answer was the numeric scale. Cemu stores
aux samples shifted right by eight with a comment saying it is not sure why, so
the effect measures its own peak instead of assuming: **13090 on a mix of five
voices at a 0.45 send**, which settles it — the buffer is at int16 scale, ±32767,
not the ±128 that reading the shift alone would suggest.

An echo — 170 ms, feedback, in plain C in a file that includes no console header
— costs **6-8 µs per call against a 3000 µs frame**, about 0.2% of the audio
thread. That is the number that says a reverb is affordable. Two caveats worth
having in writing: **Cemu does not implement the GamePad's aux path at all**
(its mixer stores the TV aux buses and leaves the DRC ones a `// todo`), the one
place where the emulator is stricter than the console; and there is no cache
maintenance around these buffers here, on the theory that AX presents them
coherently — untested, and the kind of thing only hardware can settle.

### The mix had no headroom, and a second aux bus proved it

Then a report: firing while the lead comes in makes it clip. Every voice was
mixed at unity and nothing kept count — four music channels, an engine and a
laser are a sum of six numbers that were each perfectly reasonable alone.

Rather than guess how much to turn down, measure. **An aux bus receives exactly
the voices the main bus does, summed the same way**, so a second callback with
every send at 1.0 is a true peak meter of the mix: it compares, then writes
silence over its own buffer so it costs nothing on the way out. What it read,
with nobody even shooting:

```
[mix] peak 62859 (192% of full scale)
```

Not "close to clipping" — nearly twice over, continuously. `CremaAudioSetHeadroom`
scales every voice's main bus *and its sends together*, so turning it down costs
volume and never balance, and an effect stays in the same proportion to the dry
signal. At 0.35 the same scene reads **55-69%**, with the echo's return
accounting for another 28% on top of that.

The general lesson is not about this game. **A mixer that only adds has no idea
how loud it is**, and neither does anyone reading the code; the number was
available all along from hardware that was already summing it for us.

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

### Cashing that in: `.cpak`, one archive, two reads

The archive format follows from the numbers above and contains no cleverness:
no compression, no transformation, just a directory followed by a
concatenation. What it buys is that **opening it costs two reads no matter how
many assets are inside** — the header says how long the rest is, and the rest
arrives in one call.

PoC 10 loads the same two assets both ways in the same run, because a
measurement taken on another day on another card is not a comparison:

| | on real hardware |
|---|---|
| two loose files | **38.95 ms** |
| the same two in one `.cpak` | **32.33 ms** (−17%) |

The 6.6 ms saved is exactly the fixed cost of the second file: one `fopen`
(0.14), one first-read-on-a-stream (3.13), one more read call. Which is the
part worth understanding — **the win scales with the number of files, not with
their size**, and two assets is the least favourable case there is. Twenty
assets in twenty files would spend ~80 ms before reading anything useful; in
one archive they would spend ~4.

What is left is honest bandwidth: of the 32 ms, 21.8 is 355 KB arriving at
16.7 MB/s and 3.7 is staging into the GPU. All the per-file overhead is now
gone, so the only lever remaining is moving fewer bytes — which is compression,
a different piece of work with a different trade.

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
