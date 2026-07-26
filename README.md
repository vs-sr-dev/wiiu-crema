<img src="crema.png" alt="Crema" width="520">

# Crema ☕

**A clean-room framework for Wii U homebrew** — it sits on top of *Espresso*
(the CPU) and *Latte* (the GPU), like any good crema should.

Crema is a small, honest layer: runtime GLSL shaders, cache-safe resource
handling, frame pacing, an audio stack that treats AX as what it is, and the
hard-earned lessons of getting GX2 code from the Cemu emulator onto real
silicon. It ships with twelve progressive examples, each one verified on real
hardware — a flyable game at 59.9 fps with the CPU idle and an engine note that
follows the throttle, and a shoot-'em-up you can actually lose.

It started as a rendering framework and the word has quietly stopped being
true; it is not an engine yet either, but of the two things that were standing
between it and that word, one has gone: **it can save now** — the shoot-'em-up's
high score survives the power going off. What is still missing is a name for the
other one: nothing here knows what a scene is.

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
- **crema_hud** — the readout as a list of quads drawn in one instanced call: a
  letter, a bar and a radar blip are the same quad with different numbers, and
  one negative number is the entire difference between text and geometry. The
  list builder knows nothing about a Wii U; the renderer beside it owns the
  shader and the quad
- **crema_effect** — timed billboard effects (flashes, tracers, explosions).
  What separates an effect from an entity is that an effect knows it is going
  to die: it carries its own lifetime and packs itself into an instance array —
  and now draws itself, asking for three numbers about the camera rather than
  for the application's uniform layout
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
  the transition audio it was handed — which is the Wii U Menu's music. It also
  owns the two things a mix needs that nothing else was keeping track of: the
  three **aux sends** that put your own code in AX's signal path, and a
  **headroom** applied to every voice, because a mixer that only adds has no
  idea how loud it is
- **crema_bank** — instruments: PCM plus the number that makes a sample an
  instrument, how many samples are one cycle. A note is then a playback rate
  and nothing else. The bank keeps its own cache-flushed copy, because unlike
  a texture an instrument is read by the DSP *while it plays*. Samples may be
  **ADPCM**, which the Wii U decodes in hardware for the same price as PCM —
  four bits a sample, so the compression is free at playback and paid for
  entirely by the baker
- **crema_save** — persistence: a named blob on the SD card beside the `.wuhb`,
  with a magic, a version the *caller* owns, and a checksum, replaced by writing
  elsewhere and renaming so that losing power costs the new save and not the old
  one. Not `nn_save`, and that is a decision with a reason — see below
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
| 12 | `poc12-shmup` | a game you can lose: title screen, pause, three lives, score, waves, a record that survives the power going off — and written from an empty file to find out which of PoC 11's parts a *different* game actually needs | 59.9 fps, 0.00 ms sync, save 1.1 ms |

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
./tools/build_fx.sh && tools/fx_render --impulse --out clicks.wav   # audition an effect
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

Cost, measured on hardware: **1-2 µs in a typical tick and 8-16 µs in the worst**
against a 3000 µs frame, at 59.9 fps with `sync 0.00 ms`. The sequencer's worst
tick was 14 µs before any of this existed, so **envelopes and vibrato for four
channels cost about two microseconds**. (Cemu reported 1-13 µs typical and a
worst tick that wandered between 52 and 650 µs across runs of the same binary —
the JIT and the host scheduler, not the code. Worst-case timings are not worth
quoting from an emulator.)

They also cost less volume than they save. With envelopes off the mix meter read
72% of full scale and with them on 63%: a rectangle holds full volume for the
whole note, an envelope falls to its sustain, so the same music through the same
speakers leaves more headroom for everything else.

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
voices at a 0.45 send**, and the same order of magnitude again on the console,
which settles it — the buffer is at int16 scale, ±32767, not the ±128 that
reading the shift alone would suggest.

An echo — 170 ms, feedback, in plain C in a file that includes no console header
— costs **12-13 µs per call against a 3000 µs frame on hardware**, about 0.4% of
the audio thread. That is the number that says a reverb is affordable.

Two caveats worth having in writing. **Cemu does not implement the GamePad's aux
path at all** (its mixer stores the TV aux buses and leaves the DRC ones a
`// todo`), the one place where the emulator is stricter than the console. And
there is **no cache maintenance around these buffers**, on the theory that AX
presents them coherently — which hardware confirmed: 6 channels of 144 samples
arriving 333.6 times a second with sane, stable peaks, so Nintendo's own effect
library was not doing it either.

### The emulator lied about this one, and in the other direction

The sequencer was cheaper on hardware than in Cemu — 14 µs against 42 — and it
was tempting to generalise. The echo went the other way: **23 µs on console
against 6-8 in Cemu**, three times worse on the real machine. So the effect was
taken apart against the clock, on hardware, one hypothesis at a time.

| | cost per call | what was removed |
|---|---|---|
| as written | 23 µs | |
| without the `%` | **−5 µs** | two integer divisions per sample |
| in fixed point | **−5.5 µs** | the int↔float conversions |
| what is left | **12.5 µs** | memory |

Two of those are properties of this CPU rather than of this effect:

**A modulo is a division.** `write = (cursor + i) % length` reads like index
arithmetic and compiles to `divwu`, which on Espresso is about twenty cycles and
does not pipeline. An index that only ever advances by one wraps at most once,
so a comparison says the same thing in a cycle. Two divisions per sample were
more arithmetic than the entire effect.

**There is no instruction that turns an integer into a float.** A compiler
writes `(float)sample` as a store and a load through memory, and the same again
coming back, so a DSP loop over integer samples pays a memory round trip per
conversion — three of them per sample here. In 12-bit fixed point the same gain
is a multiply and a shift.

What remains is genuinely memory. 54 cycles a sample for about fourteen
instructions is not arithmetic; it is a 76 KB delay line against a 32 KB L1,
with a read head and a write head 8160 samples apart. The estimate that got
there was wrong in detail — a conversion cost 8 cycles, not the 20 predicted —
which is the argument for measuring rather than reasoning: both hypotheses were
right, one behind the other, and only the clock could say in what proportion.

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
signal. At 0.35 the same scene reads **55-69%** in Cemu and **30-72%** on the
console, with the echo's return accounting for another 25% on top of that.

The general lesson is not about this game. **A mixer that only adds has no idea
how loud it is**, and neither does anyone reading the code; the number was
available all along from hardware that was already summing it for us.

### The PC end of the effect, and what it found on the first run

`echo.h` was written to include no console header, so that the same arithmetic
could be auditioned on a PC where a wrong number is a rerun and then run
unchanged in an audio callback where a wrong number is a reboot. For a day it
had nothing to be auditioned *with*. `tools/fx_render.c` is that thing: it reads
a WAV, hands the effect a **planar int32 buffer, 144 samples at a time**, and
writes the result back out. Not a simulation — the same file, the same shape, the
same block size AX uses. Byte order is the one thing deliberately not copied:
the samples are native on both machines, and a tool that byteswapped would be
testing a bug the console cannot have.

It paid for itself immediately. Rendered with a click train, in the placement the
console actually uses:

| | the click | 1st repeat | 2nd | 3rd |
|---|---|---|---|---|
| on an aux bus | **26000** | 2700 | 945 | 331 |
| as an insert | 20000 | 9000 | 3149 | 1102 |

The click was 20000 going in. On the aux bus it came out at 26000 — and the extra
30% is exactly the send level, because **`echoProcess` returned its input along
with the wet signal, and on an aux bus the input is `send * dry`**. AX adds an
aux return to a main mix that is already carrying the dry at full level, so the
effect was buying a second copy of the dry mix and calling it reverb: three
times less echo than the same settings give as an insert, for 30% more level.
That is a quarter of the headroom this project had spent a whole section
measuring, and it had been going into nothing since the day the effect was
written. Neither the console nor the emulator had said a word about it, because
both were doing precisely what they were asked.

The fix is one AND per sample — `in & dryMask`, where the mask is 0 for a send
and −1 for an insert, and an AND is a cycle where a multiply would be five. The
echo is unchanged to the decibel and the mix peak fell from 130% of the dry to
100%. On hardware the effect still costs **13 µs of a 3000 µs frame**, exactly
what it cost before, and the aux meter now reads `peak in 9755, out 4764` where
the output had previously been arithmetically guaranteed to exceed the input.

Then the console corrected the conclusion, which is the part worth keeping. The
recovered margin looked like room for more volume, so PoC 11's headroom went from
0.35 to 0.40 — and Cemu agreed, worst second 79% of full scale. Two minutes on
real hardware said otherwise:

```
[14:08:15.586] [flight] hit at 10 m - score 6
[14:08:16.112] [mix] peak 32508 (99% of full scale) | headroom 0.40
```

An explosion ten metres away while the music was busy, once, with the echo's
4764 stacked on top of that — so it clipped. Cemu never produced the coincidence,
which is not an emulator lie; it is what a peak meter over a short session is
worth. Back at 0.35 the same instant scales to 28444 dry plus 4169 of echo:
99.5%, on the line and not over it. **The fix bought margin, not volume** — at
0.35 *before* it, that instant would have summed to roughly 124%. The headroom
that had been chosen by measurement a day earlier was already right; what changed
is that it is now right with room to spare instead of by luck.

The general point:

> **An effect has to know where in the mixer it is standing.** A send returns wet
> only, because the dry is already there; an insert has been given the mix and
> must give it back. There is no sensible default, which is why it is an argument
> to `echoInit` and not a setter with one.

### A sampler was already a wavetable synthesiser; nobody had asked it

A single cycle in a loop is one timbre forever. Several cycles in a row, looped
as a whole, is a **wavetable**: the voice walks through them and starts again, and
the timbre moves without anything on the console doing anything.

The interesting part is what it cost. `.cbank` did not change, `crema_bank` did
not change, the sequencer did not change, AX did not change. Forty lines in
`tools/gen_waves.py`, and that is the whole feature — because `cycleSamples` says
how long **one** cycle is, which is all the pitch arithmetic ever wanted, while
the loop is the whole buffer:

```
ratio = frequency * cycleSamples / rate      # 48 cycles or one, same note
```

Two instruments came out of it: `pwm`, whose duty sweeps 0.50 → 0.09 → 0.50
across 48 cycles (the sweep lands on multiples of 1/32, so it is a staircase of
about a dozen steps — which is also what it sounded like on the machines where
the duty was two bits in a register), and `sync`, a saw running at up to 2.7× the
rate it is restarted at. The lead of the theme is on `pwm` now and the last bar's
arpeggio on `sync`.

What it is *not*, and it matters when writing a part: the table is walked once
per cycle of the note, so **the modulation is pitch-locked** — 9.2 Hz at A4, 18.3
an octave up. A chip's PWM runs at its own rate and does not care what note is
playing. This is a different instrument, brighter and thinner up top, and it is
why the bass is not on it. The LFO version would mean moving a playing voice's
sample pointer from the audio thread, which is a different feature with a click
in it.

The parameter sweeps out and back rather than round, so the last cycle is a
neighbour of the first and the table joins onto itself.

And then the console hummed, which is the actual lesson in this section.

The bass arpeggio was clean on the bars that used a plain oscillator and buzzed on
the one bar that used a table. The cause is a single sentence: **a pulse of duty
*d* has an average of *d* − (1 − *d*) times its amplitude.** A 9% pulse sits at
−81% of full scale and a 50% one sits at zero, so a table that sweeps the duty
sweeps its own DC offset with it — and a DC offset that moves is not an offset,
it is a signal. Measured after the fact, `pwm` swept from −16120 to 0 across its
48 cycles: an 81%-amplitude waveform at the table's rate, 2 to 9 Hz on the notes
that song plays. `sync` had the same disease at 19%, from truncating a saw at a
non-integer number of periods.

A real chip does not have this problem, and the reason is not in its digital
section: **its output is AC-coupled.** A capacitor removes the DC before it
reaches a speaker, which is why nobody writing for a 2A03 ever thinks about it.
These samples go straight into a digital mixer, so the capacitor has to be in the
generator. Taking each cycle's own mean out is the idealised version of one — a
high-pass whose cutoff is exactly the cycle rate, which leaves the table exactly
periodic where a real one-pole filter would droop.

The rescale afterwards matters as much as the subtraction, and getting there is
worth the two lines: a 9% pulse with its mean removed is a spike +2A(1−d) tall,
which overflows int16 before it sounds like anything. Normalising the table back
to the peak it started with keeps it in range and keeps it comparable to every
other instrument in the bank. What falls out is the *correct* behaviour rather
than a compromise — RMS per cycle goes 6381 at the narrow end to 10946 at 50%,
so narrow duty is thin and quiet and wide duty is full and loud, which is what
pulse-width modulation sounds like on hardware that has a capacitor in it.

The generator now prints the DC swing next to the seam, because a number that has
been wrong once should not be able to go wrong again quietly:

```
pwm   1536 samples  peak 19840  table of 48 cycles, sweep at f/48, DC swing 1 (0.0% of peak)
```

And with the hum gone, a second fault underneath it became audible — reported,
again, more precisely than any instrument would have managed: *"as if the
wavetable moved in mono-legato compared to the clean attacks of the other bars,
muddying the sound, like playing fast notes on a mono-legato soundbank."*

It was not legato. **A voice is retriggered from sample zero on every note-on, so
cycle 0 of a table is the attack** — and the sweep had been written starting at
the end where the character is absent: a plain saw for `sync`, the thinnest and
quietest duty for `pwm`. A sixteenth at 132 BPM lasts 113 ms, and at D3 that
covers seventeen of forty-eight cycles, so every note was hearing only the dullest
quarter of the table and never arriving where the sound was. Sixteen notes that
all open soft and swell slightly have no transients between them, and a run of
them reads as one continuous morph.

Reversing the modulator so the strong end comes first fixed it: short notes get
the whole character immediately, long ones get the journey, and the table still
joins onto itself because a palindrome reversed is still a palindrome. Which is a
design rule rather than a bug fix, and it generalises past this project:

> **Put the sound at cycle zero.** Everything after it is the note evolving;
> cycle zero is the note arriving. A table whose interesting end is in the middle
> is a table only long notes can play.

One number is worth stating because it looks like a defect and is not: a pulse
that swings ±A has an RMS of A whatever its duty, which is the most a waveform
can have at that peak. A DC-free table with a varying duty cannot match it — the
narrow cycles become tall spikes, and normalising the table's peak brings
everything else down with them. Measured, cycle 0 against `pulse25`: `pwm` is
−5.2 dB and `sync` −6.3 dB. That is arithmetic, not a mistake, and the place to
compensate is the score, where balance belongs.

### ADPCM, and the four bits the DSP decodes for free

The Wii U decodes Nintendo DSP-ADPCM **in hardware**: an AX voice set to
`AX_VOICE_FORMAT_ADPCM` costs the DSP exactly what an LPCM16 one does. So the
compression is free at playback and paid for entirely offline, which makes it the
rare optimisation with no runtime side to get wrong — except for the three things
that are not in any header. All of this was read out of Cemu's decoder
(`snd_core/ax_mix.cpp`, `AX_readADPCMSamples`):

- **A frame is 8 bytes and holds 14 samples.** One header byte, then seven bytes
  of two nibbles. So the ratio is 3.5:1 and not 4:1 — fourteen samples cost eight
  bytes, not seven.
- The header is `predictor << 4 | scale`; the scale is a shift, and the predictor
  picks one of **eight** coefficient pairs. The decoder masks that nibble with 7,
  so a ninth would silently be the first.
- The coefficients are **per voice, not per frame** — sixteen int16 in Q11 handed
  to AX once. An instrument gets eight second-order predictors chosen for its own
  material, and each frame says which one fits.
- **Offsets are counted in nibbles, and the header nibbles count.** Sample *s*
  lives at nibble `(s / 14) * 16 + (s % 14) + 2`. Getting this wrong is the
  easiest way to make a voice play static while everything else looks right.

`tools/adpcm.py` is the encoder, and the step that decides the quality is the one
easiest to skip: fit a second-order predictor to every frame by least squares,
then **cluster the hundreds of candidates down to eight with k-means** (seeded
from the data's own quantiles, so a build is reproducible). A fixed coefficient
table would be simpler and sounds worse, because a pulse wave and an explosion do
not want the same predictors and the format was built to let each instrument have
its own. Then every frame tries all eight at the smallest scale that does not
clamp, **closed loop** — the history fed to the predictor is what the decoder will
actually reconstruct, so error cannot accumulate unseen. Finally the result is
decoded again with the console's own arithmetic and the SNR printed, because a
compressor that cannot tell you what it cost is a compressor you have to trust.

What the numbers said, and they are the reason this is opt-in per instrument
rather than a switch for the bank:

| instrument | samples | SNR | verdict |
|---|---|---|---|
| `engine` | 6400 | **79.8 dB** | five sine harmonics; a second-order predictor guesses it almost exactly |
| `boom` | 28800 | 29.2 dB | filtered noise and a thump. It is an explosion |
| `noise` | 4096 | 26.6 dB | noise compressed as noise |
| `laser` | 7040 | 25.6 dB | the brightest sound in the game, and the one whose edges soften most — left as PCM, it is only 14 KB |
| `pwm` | 1536 | 21.1 dB | a wavetable of 32-sample cycles |
| `pulse50` | 32 | 2.67:1 | 2.3 frames long: the header byte stops being a rounding error |

So `boom` and `engine` are compressed and nothing else is. They were 70 KB of the
bank's 96 and are now 20, and the console — not the emulator — says:

```
[bank] 11 instruments at 32000 Hz, 47 KB resident (96 KB as PCM16), 2 compressed
```

and the explosions still sound like explosions, which is the whole test: get the
nibble arithmetic wrong and a voice plays static rather than anything subtler.

The loop needed the most care, for a reason worth stating plainly: **an ADPCM
sample is not decodable on its own.** It is a correction to a prediction made from
the two samples before it, so entering a stream part-way means telling the
hardware what those two samples were and which frame header is in force. AX has
fields for exactly that, and the baker fills them by decoding up to the loop
point rather than by guessing — and it refuses a loop that does not begin on a
frame boundary, because a frame carries its own scale and there is nowhere else
to enter it.

One divergence to be aware of, found in the source rather than by ear: **Cemu
does not restore `loopYn1`/`loopYn2` when a voice loops.** Its
`handleAdpcmDecodeLoop` resets the scale and the offset and leaves the history
where the tail left it, while the hardware has fields for that history and uses
them. Our looping ADPCM instrument loops to sample 0, where the encoder's own
history was silence, so the two machines differ only in the first two samples
after each wrap. It is small here. It would not be small for a loop into the
middle of a sample, and it is not the kind of thing an emulator tells you about.

### What a second game is for

Nothing in `crema/` was designed. Every module was extracted from an example
that had already written it, usually twice — which sounds like discipline and
was, for eleven PoCs, mostly luck: they were all variations on "draw a thing",
so "a second example needs this" was never a hard question. PoC 12 is a
shoot-'em-up, and it was written **from an empty file** for exactly that reason.
A second example cloned from the first is not a witness. Reuse `crema/` freely,
reuse the baked assets freely, and copy nothing else — then the things you find
yourself reaching back for are the answer.

It answered within an hour. The HUD list builder was wanted *unchanged*, so it
became `crema_hud.h`. Then two more things turned out to be duplicated, and both
had the same shape:

> **`crema/` held the data and the examples held the drawing.** The effect pool
> was a module and the shader that puts it on screen was copied. The HUD builder
> was a module and the shader that puts it on screen was copied.

So `crema_hud` and `crema_effect` grew a renderer, the first things in Crema to
own GPU state. Two decisions made that possible, and both were forced by the two
examples disagreeing:

**The renderer owns the shader; the game owns the memory.** PoC 11 draws its HUD
twice per frame — once for the television, once for the GamePad's tactical
screen — with two different lists, both in flight at once. A uniform buffer
inside the renderer would have to be overwritten between the two draws, and the
first would read the second one's list. Whoever knows how many lists a frame has
must own the storage for them, and that is never the framework.

**A module asks for the numbers it needs, not for "the Global block".** PoC 11's
global uniforms have ten fields; PoC 12's have three. A billboard renderer that
read the application's Global block would be a module that dictates the uniform
layout of every game built on it. `CremaEffectDraw` declares its own three —
view-projection, camera right, camera up — and the caller fills them. The same
shader now serves a camera that never moves and a camera that never stops, and
the two never have to agree about anything else.

The cost of the extraction, measured: PoC 11 lost 150 lines, PoC 12 lost 118,
`crema/` gained 142 that both share, and on hardware nothing changed at all —
59.9 fps, 0.00 ms sync, the echo still 12-13 µs. What is left in each example is
the one shader that game actually invented.

### Saving, and where a homebrew title is allowed to put a file

The shoot-'em-up had a "BEST" line on its HUD that was a lie the moment you
pressed HOME: it said *best* and meant *best since you turned it on*. Fixing that
turned out to be less about writing a file than about deciding where one is
allowed to go, and of the three candidates only one survives contact with both
the emulator and the console.

**`/vol/save` via `nn_save`** is the real Wii U answer and is wrong here twice
over. A `.wuhb` launched under Aroma has no save directory of its own — that path
belongs to whichever title is hosting it, so a high score would be written into
somebody else's save data. And Cemu does not export `SAVEInitCommonSaveDir` at all
(`nn_save.cpp` registers `SAVEInit` and `SAVEInitSaveDir` and stops), so the two
targets would disagree about a call that cannot be tested in the place it fails.
**`/vol/content`** is read-only; it is inside the bundle. Which leaves
**`/vol/external01`** — the SD card, where the `.wuhb` already lives. The file
lands next to the thing that wrote it, a PC can read it, and both targets reach it
the same way.

One difference between them is invisible until it bites. Under Aroma the card is
already mounted process-wide; Cemu's source says it mounts the card only from
`FSMount` and `FSBindMount`, which reads like *the emulator will not have it until
we ask*. So the module looks before it mounts — and then found the path already
there on **both** targets, so the mount never runs. Worth forcing once anyway,
because an untried fallback is not a fallback: `FSGetMountSource` and `FSMount`
both return OK against an already-mounted card and change nothing. The probe stays
in front of it regardless. Under Aroma another module owns that mount and
refcounts it, and mounting a volume somebody else is holding is not a thing to do
speculatively.

The file itself carries a magic, a version and an FNV-1a checksum, and is written
to `record.dat.new` and renamed. Three deliberate choices:

- **The version belongs to the caller, not to the module.** A game that changes
  the shape of its save says so, and a file from the old shape then reads as
  *absent* rather than as garbage that happens to fit.
- **The checksum is not about bit rot**, it is what a half-written file from an
  earlier crash looks like. Reading it would put a plausible wrong number on
  screen instead of no number at all.
- **The rename is the only moment the real name changes meaning.** Until it, the
  file under the real name is still the previous save, entire — so losing power
  during a write costs the new score and nothing else.

Costs, measured in Cemu: **1114 µs to write** eight bytes with the header, **402 µs
to read** them. Slow enough to be worth doing once per game over rather than once
per point scored, and nowhere near slow enough to need a thread. That is the sort
of thing worth measuring precisely because both answers were plausible.

Two fields rather than one, on purpose. A high score alone is indistinguishable
from a single word written to a file; the thing worth proving is that a *struct*
goes out and comes back with its parts still in the right order. So there is a
play counter too, and it is on the title screen — a number that is not zero on a
fresh boot is a number that came from a file.

```
[save] ready at /vol/external01/wiiu/apps/gx2poc
[poc12] no record yet in /vol/external01/wiiu/apps/gx2poc (193 us)
[poc12] record saved: best 14100, 1 games, 1114 us
...
[poc12] record loaded from /vol/external01/wiiu/apps/gx2poc: best 14100 after 5 games, 402 us
```

Verified on the console the only way that counts, and it is worth naming the
difference: not by returning to the HOME menu, which leaves the process and the
filesystem cache alive, but by **switching the Wii U off at the wall and turning
it back on**. The score and the play counter were both there. Anything short of a
real power cycle can be passed by a save that never reached the card.

Then the card came out of the console and into a PC, which is the part a save
format is for:

```
$ xxd record.dat                        # written by the Wii U, read on a PC
00000000: 4353 4156 0000 0001 0000 0008 47f1 e6de  CSAV........G...
00000010: 0000 15e0 0000 0002                      ........
```

`CSAV`, version 1, eight bytes of payload — and `0x15e0` is 5600 with a play
count of 2. The checksum was recomputed from scratch by a second FNV-1a
implementation on the other machine and came out `0x47f1e6de`, the same word the
console wrote. Which is the whole point: **the bytes came back identical, and
something other than the program that wrote them can say so.**

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
8. **The emulator does not only forgive — it also lies the other way.** Every
   lesson above is "works in Cemu, breaks on hardware", and it is tempting to
   conclude the console is simply the stricter machine. It is not. The
   sequencer, which is pure arithmetic, cost 14 µs on hardware against 42 in
   Cemu; the echo, which walks a 76 KB delay line, cost 23 µs on hardware
   against 6-8 in Cemu. Cemu runs on a CPU with enormous caches, so it hides
   memory costs, and it measures through its own JIT, so it inflates
   arithmetic. **Compute-bound code looks worse in the emulator than it is;
   memory-bound code looks better.** Profile the second kind on the console or
   do not profile it.
9. **A homebrew title has no save directory of its own.** A `.wuhb` launched
   under Aroma borrows a host title's, so `nn_save` would write your high score
   into somebody else's save data — and Cemu does not export
   `SAVEInitCommonSaveDir` at all, so the emulator cannot even show you the
   problem. Write to the SD card (`/vol/external01`), next to the `.wuhb`.
10. **Both machines have the SD card already mounted, and neither says so.**
   Cemu's source reaches its own mount only from `FSMount`/`FSBindMount`, which
   reads like "the emulator will not have it until you ask" — and then
   `/vol/external01` was there on both targets and the fallback never ran. Probe
   before mounting: under Aroma another module owns that mount and refcounts it.
11. **An effect on an aux bus must return the wet signal only.** AX adds an aux
   return to a main mix that is already carrying the dry, so an effect that
   passes its input through puts a second copy of the dry into the mix at the
   send level. Ours did, for a day, and it cost a quarter of the mix's headroom
   and three quarters of the echo's audible output. No emulator will mention
   this, because nothing is going wrong: it is a question about where the code
   is standing, and only a PC render answered it.
12. **A DC offset that moves is a signal.** Chip oscillators have large constant
   DC offsets — a 9% pulse sits at −81% of full scale — and on real hardware
   nobody notices, because the output is AC-coupled and a capacitor removes it. A
   *wavetable* that sweeps the duty sweeps that offset too, and there is no
   capacitor in a digital mixer: it comes out as a full-amplitude waveform at the
   table's own rate. It was audible on the console as a hum on the one bar that
   used a table, and invisible in every number the build was printing. Take each
   cycle's own mean out, then renormalise.
13. **Cycle zero of a wavetable is the attack.** A voice is retriggered from
   sample zero on every note-on, so whatever is in the first cycle is what the
   ear hears as the note arriving — and a sixteenth at 132 BPM only covers a
   quarter of a 48-cycle table. A sweep that started at the end without the
   character in it gave a whole bar of notes no transients, and it sounded like a
   mono-legato patch rather than like a wrong waveform, which is why it took a
   listener to name it.
14. **Cemu does not restore the ADPCM loop history.** Its
   `handleAdpcmDecodeLoop` resets the frame scale and the offset and leaves
   `yn1`/`yn2` where the tail left them, while the hardware has fields for
   exactly that state and uses them. A loop back to sample 0 (where the
   encoder's own history was silence) barely differs; a loop into the middle of
   a sample would.

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
- Audio thread: the frame is **3000 µs** (144 samples at 48 kHz, 333 times a
  second). A four-channel sequencer with envelopes and vibrato costs **1-2 µs**
  typical and 8-16 µs worst; a 170 ms stereo echo in the aux callback costs
  **12-13 µs**. Together under 1% — the DSP work on this console is not the
  thing that will run out.
- Engine recipe that gets you there: static vertex/uniform data, animation in
  the vertex shader, instancing, fenced pacing — CPU cost ≈ 0.1 ms/frame.

## Credits

- [wut](https://github.com/devkitPro/wut) & devkitPPC — devkitPro team
- [CafeGLSL](https://github.com/Exzap/CafeGLSL) — Exzap & Crementif
- GX2 knowledge base: [decaf-emu](https://github.com/decaf-emu/decaf-emu),
  [WiiUBrew](https://wiiubrew.org),
  [Immaterial](https://github.com/glastonbridge/immaterial-wiiu-demo) write-ups
- [Cemu](https://github.com/cemu-project/Cemu) — not only for running the code
  before the console did, but as documentation: the AX aux-callback ABI and the
  meaning of a voice's volume delta were read out of its mixer, because a
  working emulator had to know answers wut does not write down
- [chiproll](https://github.com/vs-sr-dev/chiproll) — the piano roll the chip
  tunes are written in, and the source of the `.csong` format's convictions

## License

MIT — see [LICENSE](LICENSE).
