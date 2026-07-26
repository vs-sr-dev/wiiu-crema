// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan
//
// GX2 PoC 14 — a slice of a football match, and the last thing standing
// between this repository and the word it has been refusing to use.
//
// Every example so far has been asked for something it already knew how to
// give. This one is picked because it asks for three things nothing here has
// ever been asked for: a ball that obeys physics rather than a script, a net
// you can see through, and — the deep one — a clock that is not the frame
// rate. A football is the genre that will not let you fake any of them.
//
// Built in steps, and each step is a thing that can be looked at:
//
//   1. the bricks and the pitch      <- this file, right now
//   2. the fixed step and the ball
//   3. eight players and a rig
//   4. the goals and the see-through pass
//   5. the grass
//   6. the match: scenes, readout, sound
//
// --- why a stick figure is three meshes and not one --------------------------
//
// Nothing here is modelled. A player is a box, a sphere and a wedge at eleven
// different sizes, and the whole team is the same three baked meshes drawn
// with different matrices — which is the reason the instance block carries a
// full 3x4 transform this time instead of PoC 13's position-and-yaw. A limb
// swings about a joint that is itself swinging about another joint; four
// numbers cannot say that, and twelve can.

#include <gx2/draw.h>
#include <gx2/registers.h>
#include <gx2/sampler.h>
#include <gx2/shaders.h>
#include <gx2/state.h>
#include <gx2/texture.h>
#include <gx2r/draw.h>
#include <whb/gfx.h>
#include <whb/log.h>
#include <vpad/input.h>
#include <coreinit/time.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crema_app.h"
#include "crema_audio.h"
#include "crema_bank.h"
#include "crema_blend.h"
#include "crema_buffer.h"
#include "crema_frame.h"
#include "crema_hud.h"
#include "crema_input.h"
#include "crema_matrix.h"
#include "crema_mesh.h"
#include "crema_pak.h"
#include "crema_shader.h"
#include "crema_texture.h"

#define PI 3.14159265f

// --- the pitch ---------------------------------------------------------------
//
// Metres, and they mean metres: a player is 1.8 of them tall and the numbers
// below were chosen by looking at the picture, not by scaling something else.
// A real pitch is 105 x 68 and would put eight players in a field of grass
// with nothing to look at; this one is arcade-sized on purpose.

#define PITCH_X       24.0f      // half length — goals sit at +/- this
#define PITCH_Z       16.0f      // half width
#define LINE_W        0.14f
#define CIRCLE_R      5.0f
#define BOX_DEPTH     7.0f       // penalty area, measured in from the goal line
#define BOX_HALF_Z    9.5f
#define GRASS_TILE    4.0f       // metres per repeat of the mown texture

// --- the match ---------------------------------------------------------------
//
// Four phases in a switch, and NOT in crema_scene, which is a decision with a
// reason rather than an omission. That module's own header says it: PoC 12's
// four states share every resource, none of them is ever entered or left, and
// nothing is loaded or freed when one becomes another — which is fine, because
// a shoot-'em-up is one place. A football match is one place too. The pitch,
// the eight men and the ball are alive in all four of these, the score belongs
// to none of them, and a scene stack would be bookkeeping wearing the costume
// of a lifetime. The module is right about when to use it, including here.
typedef enum {
    // Before the whistle, the example proves its own central claim: the same
    // kick is fired twice, once with the console idle and once with 26 ms
    // burned out of every frame, and the two must land on the same five
    // decimals. It happens here rather than on a button because a measurement
    // that depends on somebody remembering to take it is a measurement nobody
    // takes twice.
    PHASE_SELFTEST = 0,
    PHASE_KICKOFF,       // everyone walking back, ball on the spot
    PHASE_PLAY,
    PHASE_GOAL,          // the ball is in the net and the world keeps running
    PHASE_FULLTIME,
} Phase;

#define MATCH_LEN     180.0f     // seconds of simulation, shown as 90 minutes
#define KICKOFF_STEPS 180        // 1.5 s
#define GOAL_STEPS    300        // 2.5 s

// The goal, which is three different things sharing a name: a frame the ball
// comes off, a net the ball stops in, and a line whose crossing is the only
// event in the whole example a player cares about.
#define GOAL_HALF_Z   3.4f
#define GOAL_H        2.30f
#define GOAL_DEPTH    1.90f
#define POST_W        0.13f
#define NET_CELL      0.13f      // metres per hole in the cord

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static float signf(float v) { return v < 0.0f ? -1.0f : 1.0f; }

// --- which way is forward ----------------------------------------------------
//
// Two functions rather than the arithmetic inline, and the reason is that the
// arithmetic was wrong in three places at once and agreed with itself.
//
// A figure is modelled facing -Z and turned by mat4_rotate_y, which takes
// (0,0,-1) to (-sin y, 0, -cos y). Note the minus on the X: the obvious guess,
// atan2(dx, -dz), is right for anybody running away from the camera and
// MIRRORED for anybody running along the pitch — so the feet pointed backwards
// only while the game was doing the thing a football game mostly does.
static void headingVec(float yaw, float *hx, float *hz)
{
    *hx = -sinf(yaw);
    *hz = -cosf(yaw);
}

static float yawFromDir(float dx, float dz)
{
    return atan2f(-dx, -dz);
}

// --- the clock ---------------------------------------------------------------
//
// The reason this example exists. Everything before it in this repository
// integrated against the frame: `pos += vel * clock.dt`, with dt whatever the
// last frame happened to take. That is fine for a ship that is told where to
// go, and it stops being fine the moment something bounces — a restitution
// applied at 60 Hz and the same one applied at 30 Hz do not describe the same
// ball, and no amount of care in the bounce code fixes it.
//
// So the simulation gets its own clock, 120 steps a second, and the frame gets
// what is between two of them. 120 rather than 60 for a reason worth stating:
// the console presents at 59.94 Hz, not 60, so an accumulator at 120 does not
// settle into a tidy two-steps-per-frame — it does two, two, two, three. The
// awkward step is not a defect to be tuned away, it is the accumulator earning
// its keep, and the interpolation is what makes it invisible.
//
// The runaway case — a frame so slow that catching up costs more than the
// frame did, forever — is already prevented upstream, and not on purpose:
// CremaClockTick clamps dt to 50 ms because coming back from the HOME menu
// hands you several seconds. Fifty milliseconds is six steps. The guard that
// exists for the HOME menu is exactly the guard a fixed step needs.
#define SIM_HZ        120.0f
#define SIM_DT        (1.0f / SIM_HZ)
#define SIM_MAX_STEPS 8          // dt is clamped to 50 ms upstream: never hit

// --- the ball ----------------------------------------------------------------
//
// Numbers, not a solver. Gravity is stronger than the world's because the
// pitch is smaller than the world's, and a ball that hangs is a ball that
// looks like it is falling through syrup.
#define GRAVITY       -16.0f
#define BALL_DRAG     0.006f     // quadratic: a = -k |v| v
#define BALL_BOUNCE   0.56f      // vertical restitution off the turf
#define BALL_GRAB     0.82f      // horizontal speed kept through a bounce
#define BALL_ROLL     1.7f       // rolling deceleration, m/s^2
#define BALL_MAGNUS   0.016f     // how hard spin bends a flight
#define BALL_SPINDAMP 1.1f

// --- shared shape ------------------------------------------------------------

#define MAX_BRICKS    64         // per batch; uInst[] below is four times this

#define GLOBAL_UBO_DECL \
    "layout(binding = 0) uniform Global {\n" \
    "    mat4 uViewProj;\n"       \
    "    vec4 uLightDir;\n"       \
    "    vec4 uCamPos;\n"         \
    "    vec4 uFog;\n"            /* x = starts, y = ends */ \
    "    vec4 uFogColor;\n"       \
    "    vec4 uGrass;\n"          /* xz = patch offset, w = seconds */ \
    "};\n"

typedef struct {
    Mat4  viewProj;
    float lightDir[4];
    float camPos[4];
    float fog[4];
    float fogColor[4];
    float grass[4];
} Global;

// One brick: a 3x4 transform stored as three rows, plus a colour whose fourth
// component is a flag rather than an alpha. Sixty-four bytes, and the sign
// trick the HUD invented is here too — a number doing double duty is a house
// idiom by now.
typedef struct {
    float row[3][4];
    float tint[4];     // rgb, w: 0 = plain brick, 1 = the ball's panels
} Brick;

typedef struct {
    Brick            items[MAX_BRICKS];
    uint32_t         count;
    CremaUniformRing ring;
    void            *ubo;
    const CremaMesh *mesh;
} BrickBatch;

// --- shaders -----------------------------------------------------------------
//
// The normal is the only interesting line in here. The instance transform is
// rotation times a non-uniform scale — a shin is 0.17 x 0.64 x 0.17 — and a
// normal pushed through that matrix comes out wrong, leaning towards the
// squashed axis. The usual fix is to send an inverse-transpose as well, which
// is three more vec4 per brick. But this matrix is known to be R*S with R
// orthonormal, and that means each column is a unit axis multiplied by its own
// scale: normalising the columns hands the rotation straight back. Three
// normalize() calls against 25% more instance bandwidth.

static const char *VS_BRICK =
    "#version 450\n"
    "layout(location = 0) in vec3 aPosition;\n"
    "layout(location = 1) in vec3 aNormal;\n"
    "layout(location = 2) in vec2 aUV;\n"
    GLOBAL_UBO_DECL
    "layout(binding = 1) uniform Instances {\n"
    "    vec4 uInst[256];\n"      // MAX_BRICKS * 4
    "};\n"
    "layout(location = 0) out vec3 vNormal;\n"
    "layout(location = 1) out vec3 vTint;\n"
    "layout(location = 2) out vec3 vObject;\n"
    "layout(location = 3) out vec3 vWorld;\n"
    "layout(location = 4) out float vFlag;\n"
    "void main()\n"
    "{\n"
    "    int base = gl_InstanceID * 4;\n"
    "    vec4 r0 = uInst[base];\n"
    "    vec4 r1 = uInst[base + 1];\n"
    "    vec4 r2 = uInst[base + 2];\n"
    "    vec4 tint = uInst[base + 3];\n"
    "    vec3 world = vec3(dot(r0.xyz, aPosition) + r0.w,\n"
    "                      dot(r1.xyz, aPosition) + r1.w,\n"
    "                      dot(r2.xyz, aPosition) + r2.w);\n"
    "    mat3 rot = mat3(normalize(vec3(r0.x, r1.x, r2.x)),\n"
    "                    normalize(vec3(r0.y, r1.y, r2.y)),\n"
    "                    normalize(vec3(r0.z, r1.z, r2.z)));\n"
    "    gl_Position = uViewProj * vec4(world, 1.0);\n"
    "    vNormal  = rot * aNormal;\n"
    "    vTint    = tint.rgb;\n"
    "    vObject  = aPosition;\n"
    "    vWorld   = world;\n"
    "    vFlag    = tint.w;\n"
    "}\n";

// The ball's panels, and the reason the sphere is the one brick with real
// texture coordinates and smooth normals.
//
// A football is a truncated icosahedron: twelve pentagons and twenty hexagons,
// white, with a black seam where any two of them meet. Painting the twelve
// dark patches instead is easier and wrong — it gives you a ball with spots
// rather than a ball with panels, and the seams are the part the eye reads as
// stitching.
//
// So the panels are found rather than drawn: take the thirty-two panel
// centres — the icosahedron's twelve vertices and its twenty face centres —
// and ask which one a point is nearest. That is a Voronoi diagram on the
// sphere, and its cells ARE the panels. The seam is simply where the nearest
// two are too close to call.
//
// Thirty-two sites cost sixteen dot products, not thirty-two: they come in
// antipodal pairs, so an abs() covers both ends of each axis.
//
// All of it against the OBJECT-space normal — object space because a pattern
// living in world space would sit still while the ball rolled underneath it,
// which is precisely the thing a spinning ball is supposed to show you.
static const char *PS_BRICK =
    "#version 450\n"
    "layout(location = 0) in vec3 vNormal;\n"
    "layout(location = 1) in vec3 vTint;\n"
    "layout(location = 2) in vec3 vObject;\n"
    "layout(location = 3) in vec3 vWorld;\n"
    "layout(location = 4) in float vFlag;\n"
    GLOBAL_UBO_DECL
    "layout(location = 0) out vec4 oColor;\n"
    // 6 axes: the pentagon centres (icosahedron vertices).
    // 10 axes: the hexagon centres (icosahedron face centres).
    "const vec3 PANEL[16] = vec3[16](\n"
    "    vec3( 0.0000,  0.5257,  0.8507), vec3( 0.0000,  0.5257, -0.8507),\n"
    "    vec3( 0.5257,  0.8507,  0.0000), vec3( 0.5257, -0.8507,  0.0000),\n"
    "    vec3( 0.8507,  0.0000,  0.5257), vec3(-0.8507,  0.0000,  0.5257),\n"
    "    vec3( 0.5774,  0.5774,  0.5774), vec3( 0.5774,  0.5774, -0.5774),\n"
    "    vec3( 0.5774, -0.5774,  0.5774), vec3(-0.5774,  0.5774,  0.5774),\n"
    "    vec3( 0.0000,  0.3568,  0.9342), vec3( 0.0000,  0.3568, -0.9342),\n"
    "    vec3( 0.3568,  0.9342,  0.0000), vec3( 0.3568, -0.9342,  0.0000),\n"
    "    vec3( 0.9342,  0.0000,  0.3568), vec3(-0.9342,  0.0000,  0.3568));\n"
    "void main()\n"
    "{\n"
    "    vec3 base = vTint;\n"
    "    if (vFlag > 0.5) {\n"
    "        vec3 d = normalize(vObject);\n"
    "        float near0 = -1.0, near1 = -1.0;\n"
    "        for (int i = 0; i < 16; i++) {\n"
    "            float t = abs(dot(d, PANEL[i]));\n"
    "            if (t > near0) { near1 = near0; near0 = t; }\n"
    "            else if (t > near1) { near1 = t; }\n"
    "        }\n"
    // The two thresholds are an angle in disguise: near the boundary the gap
    // between the nearest two sites grows as 0.64 * delta with delta in
    // radians, so 0.005..0.015 is a seam that fades in between half a degree
    // and one and a half. Panels are 37 degrees across — the first guess was
    // three times too wide and painted a black ball with white cracks.
    "        base = mix(vec3(0.05, 0.05, 0.06), base,\n"
    "                   smoothstep(0.005, 0.015, near0 - near1));\n"
    "    }\n"
    "    vec3 n = normalize(vNormal);\n"
    "    float diff = max(dot(n, -uLightDir.xyz), 0.0);\n"
    // Plastic, not cloth: a hard little highlight is most of what tells the eye
    // these are moulded bricks rather than flat-shaded polygons.
    "    vec3 view = normalize(uCamPos.xyz - vWorld);\n"
    // `half` is a reserved word in GLSL even where no half exists — CafeGLSL
    // takes the standard at its word and this cost a build to find out.
    "    vec3 hv = normalize(view - uLightDir.xyz);\n"
    "    float spec = pow(max(dot(n, hv), 0.0), 24.0) * 0.35;\n"
    // A touch of sky bounce from above keeps the shadowed side from going flat
    // black, which on a stick figure is the difference between a toy and a
    // silhouette.
    "    float sky = 0.5 + 0.5 * n.y;\n"
    "    vec3 lit = base * (0.30 + 0.22 * sky + 0.62 * diff) + vec3(spec);\n"
    "    float fog = smoothstep(uFog.x, uFog.y, length(vWorld - uCamPos.xyz));\n"
    "    oColor = vec4(mix(lit, uFogColor.rgb, fog), 1.0);\n"
    "}\n";

static const char *VS_GROUND =
    "#version 450\n"
    "layout(location = 0) in vec3 aPosition;\n"
    "layout(location = 1) in vec3 aNormal;\n"
    "layout(location = 2) in vec2 aUV;\n"
    GLOBAL_UBO_DECL
    "layout(location = 0) out vec2 vUV;\n"
    "layout(location = 1) out vec3 vWorld;\n"
    "void main()\n"
    "{\n"
    "    gl_Position = uViewProj * vec4(aPosition, 1.0);\n"
    "    vUV = aUV;\n"
    "    vWorld = aPosition;\n"
    "}\n";

static const char *PS_GROUND =
    "#version 450\n"
    "layout(location = 0) in vec2 vUV;\n"
    "layout(location = 1) in vec3 vWorld;\n"
    GLOBAL_UBO_DECL
    // uTurf, not uGrass: the Global block gained a uGrass and CafeGLSL
    // reported the clash as "redeclaration has incorrect type", which is
    // accurate and takes a moment to read as a name collision.
    "layout(binding = 0) uniform sampler2D uTurf;\n"
    "layout(location = 0) out vec4 oColor;\n"
    "void main()\n"
    "{\n"
    "    vec3 base = texture(uTurf, vUV).rgb;\n"
    "    float diff = max(dot(vec3(0.0, 1.0, 0.0), -uLightDir.xyz), 0.0);\n"
    "    vec3 lit = base * (0.42 + 0.68 * diff);\n"
    "    float fog = smoothstep(uFog.x, uFog.y, length(vWorld - uCamPos.xyz));\n"
    "    oColor = vec4(mix(lit, uFogColor.rgb, fog), 1.0);\n"
    "}\n";

// The first transparent thing in this example, and it is here at step two
// rather than with the nets because a ball in the air is unreadable without
// it: on a side-on camera, "high and near" and "low and far" draw the same
// pixels. The shadow is the only thing on screen carrying the ball's height.
//
// No texture and no shadow map — a disc is a smoothstep on the distance from
// the middle of a quad, which is PoC 11's billboard trick standing on the
// ground instead of facing the camera.
static const char *VS_SHADOW =
    "#version 450\n"
    "layout(location = 0) in vec3 aPosition;\n"
    "layout(location = 1) in vec3 aNormal;\n"
    "layout(location = 2) in vec2 aUV;\n"
    GLOBAL_UBO_DECL
    "layout(binding = 1) uniform Instances {\n"
    "    vec4 uInst[256];\n"
    "};\n"
    "layout(location = 0) out vec2 vUV;\n"
    "layout(location = 1) out float vAlpha;\n"
    "layout(location = 2) out vec3 vTint;\n"
    "void main()\n"
    "{\n"
    "    int base = gl_InstanceID * 4;\n"
    "    vec4 r0 = uInst[base];\n"
    "    vec4 r1 = uInst[base + 1];\n"
    "    vec4 r2 = uInst[base + 2];\n"
    "    vec3 world = vec3(dot(r0.xyz, aPosition) + r0.w,\n"
    "                      dot(r1.xyz, aPosition) + r1.w,\n"
    "                      dot(r2.xyz, aPosition) + r2.w);\n"
    "    gl_Position = uViewProj * vec4(world, 1.0);\n"
    "    vUV = aUV;\n"
    "    vAlpha = uInst[base + 3].w;\n"
    "    vTint = uInst[base + 3].rgb;\n"
    "}\n";

static const char *PS_SHADOW =
    "#version 450\n"
    "layout(location = 0) in vec2 vUV;\n"
    "layout(location = 1) in float vAlpha;\n"
    "layout(location = 2) in vec3 vTint;\n"
    "layout(location = 0) out vec4 oColor;\n"
    "void main()\n"
    "{\n"
    "    float d = length(vUV - vec2(0.5)) * 2.0;\n"
    // Negative alpha means a ring rather than a disc — the house trick of
    // letting a sign carry a bit, which crema_hud started and the shmup's
    // mirrored ship carried on. Here it is the difference between a shadow
    // and the marker under the player the pad is holding.
    "    float shape = vAlpha >= 0.0\n"
    "        ? 1.0 - smoothstep(0.72, 1.0, d)\n"
    "        : smoothstep(0.62, 0.74, d) * (1.0 - smoothstep(0.88, 1.0, d));\n"
    "    oColor = vec4(vTint, abs(vAlpha) * shape);\n"
    "}\n";

// --- the grass ---------------------------------------------------------------
//
// The only thing in this example that spends the console's budget on purpose.
// Twenty thousand blades, each one triangle, standing still.
//
// The first version had the patch follow the camera, snapped to the blade
// spacing — PoC 11's ground trick, which works because a tiling texture is the
// same at every offset. A jittered grid is not: shift it by one cell and every
// blade lands where its NEIGHBOUR was, and the neighbour is a different blade
// with a different height, lean and colour. The whole field re-rolled itself
// as the camera panned, which reads exactly like grass sliding along with you.
//
// So it does not follow anything. The pitch is sixty metres long and the whole
// visible field fits in one buffer, under the 65535-vertex ceiling a 16-bit
// index buffer imposes — which is the real constraint here and the reason the
// spacing is what it is.
//
// Three things happen in the vertex shader and none of them could happen
// anywhere else at this count:
//
//   the wind — a sine on a per-blade phase, applied only to the tip, so the
//   base stays planted and the blade bends instead of sliding;
//
//   the fade — height scaled to zero with distance, because a blade a
//   hundred metres away is a subpixel triangle and a field of subpixel
//   triangles is a field of shimmer. Shrinking them is free and popping is
//   what you get if you do not;
//
//   the edge — the patch is bigger than the ground it stands on, so blades
//   past the touchline shrink away rather than floating over the void.
//
// No alpha anywhere: a blade is a tapered triangle, which means it is opaque,
// needs no sorting and costs the transparent pass nothing. A grass texture on
// a quad would have been the obvious way and would have cost a blend, a sort
// and a mip chain to fight.
static const char *VS_GRASS =
    "#version 450\n"
    "layout(location = 0) in vec3 aPosition;\n"
    "layout(location = 1) in vec3 aNormal;\n"     // xyz: base tint
    "layout(location = 2) in vec2 aUV;\n"         // x = phase, y = 0 base 1 tip
    GLOBAL_UBO_DECL
    "layout(location = 0) out vec3 vColor;\n"
    "layout(location = 1) out vec3 vWorld;\n"
    "void main()\n"
    "{\n"
    "    vec3 p = aPosition;\n"
    "    float tip = aUV.y;\n"
    "    float d = length(p.xz - uCamPos.xz);\n"
    "    float k = 1.0 - smoothstep(20.0, 40.0, d);\n"
    // Off the edge of the world the ground quad ends and so must the grass.
    "    k *= 1.0 - smoothstep(30.0, 32.0, abs(p.x));\n"
    "    k *= 1.0 - smoothstep(20.0, 22.0, abs(p.z));\n"
    "    float sway = sin(uGrass.w * 2.1 + aUV.x) * 0.055\n"
    "               + sin(uGrass.w * 5.3 + aUV.x * 2.7) * 0.018;\n"
    "    p.y *= k;\n"
    "    p.x += sway * tip * k;\n"
    "    p.z += sway * 0.6 * tip * k;\n"
    "    gl_Position = uViewProj * vec4(p, 1.0);\n"
    "    vColor = aNormal * (0.52 + 0.58 * tip);\n"
    "    vWorld = p;\n"
    "}\n";

static const char *PS_GRASS =
    "#version 450\n"
    "layout(location = 0) in vec3 vColor;\n"
    "layout(location = 1) in vec3 vWorld;\n"
    GLOBAL_UBO_DECL
    "layout(location = 0) out vec4 oColor;\n"
    "void main()\n"
    "{\n"
    "    float fog = smoothstep(uFog.x, uFog.y, length(vWorld - uCamPos.xyz));\n"
    "    oColor = vec4(mix(vColor, uFogColor.rgb, fog), 1.0);\n"
    "}\n";

// The net. World-space geometry, so no instance block and no transform — the
// two goals do not move and there was never a reason to pretend they might.
//
// What makes this the first real transparency in the project is what it is
// drawn OVER: PoC 11's billboards were transparent against the sky, and a net
// is transparent against the players standing behind it. Depth test on, depth
// write off, culling off — a net has two sides and you see both.
static const char *VS_NET =
    "#version 450\n"
    "layout(location = 0) in vec3 aPosition;\n"
    "layout(location = 1) in vec3 aNormal;\n"
    "layout(location = 2) in vec2 aUV;\n"
    GLOBAL_UBO_DECL
    "layout(location = 0) out vec2 vUV;\n"
    "layout(location = 1) out vec3 vNormal;\n"
    "void main()\n"
    "{\n"
    "    gl_Position = uViewProj * vec4(aPosition, 1.0);\n"
    "    vUV = aUV;\n"
    "    vNormal = aNormal;\n"
    "}\n";

static const char *PS_NET =
    "#version 450\n"
    "layout(location = 0) in vec2 vUV;\n"
    "layout(location = 1) in vec3 vNormal;\n"
    GLOBAL_UBO_DECL
    "layout(binding = 0) uniform sampler2D uNet;\n"
    "layout(location = 0) out vec4 oColor;\n"
    "void main()\n"
    "{\n"
    "    vec4 cord = texture(uNet, vUV);\n"
    "    float lit = 0.55 + 0.45 * abs(dot(normalize(vNormal), -uLightDir.xyz));\n"
    "    oColor = vec4(cord.rgb * lit, cord.a);\n"
    "}\n";

// --- a stick figure ----------------------------------------------------------
//
// Eleven bricks and eight numbers. Everything below is measured from the sole
// of the foot upwards, which is why the figure stands on the ground instead of
// being placed at its own centre and hoping.

#define HIP_Y     0.78f
#define LEG_LEN   0.64f
#define LEG_W     0.17f
#define FOOT_H    0.14f
#define TORSO_Y   1.10f
#define TORSO_H   0.64f
#define SHOULDER  1.36f
#define SHOULDER_X 0.28f   // torso is 0.46 wide: the arm just overlaps it
#define ARM_FLARE 0.15f    // how far the arms are held from the body
#define ARM_LEN   0.52f
#define ARM_W     0.12f
#define HEAD_Y    1.62f
#define HEAD_R    0.20f
#define BALL_R    0.20f    // arcade-sized: a real one is 0.11 and reads as dirt

typedef struct {
    Vec3  pos;          // where the feet are
    float yaw;          // facing, radians, 0 = -Z
    float stride;       // walk cycle phase, radians
    float swing;        // how much of the walk shows: 0 standing, 1 sprinting
    float lean;         // forward tilt of the whole figure
    float tint[3];      // the shirt
} Figure;

// --- the world, and the one rule the fixed step imposes ----------------------
//
// Everything the simulation owns lives in ONE struct, and that is not tidiness.
// Interpolating means keeping the previous state as well as the current one,
// and the way that discipline rots is field by field: somebody adds a velocity
// to a player, forgets to add it to the copy, and a month later the game
// stutters in a way nobody can reproduce. With the world in a single struct
// the previous state is one assignment — `prev = cur` — and there is nothing
// to forget.
//
// The cost is honest and worth writing down: every step copies the whole
// world. Four hundred bytes at 120 Hz is 48 KB/s, which on this machine is
// nothing, and the day it is not nothing is the day the struct has grown too
// big to reason about anyway.

#define MAX_MEN 8
#define TEAM_SIZE 4

typedef struct { float w, x, y, z; } Quat;

// A player, split down the middle by a question the fixed step forces and
// nothing before it did: what is DRAWN and what is merely known. `look` is
// the first, and is the only part that gets interpolated — the velocity of a
// man is not a picture of anything, and lerping it would be work done to
// produce a number nobody looks at.
typedef struct {
    Figure  look;
    Vec3    vel;
    uint8_t team;         // 0 blue, 1 red
    uint8_t role;         // 0 keeper, 1..3 out on the pitch
    float   kickCool;     // stops one touch becoming eight
} Man;

typedef struct {
    Vec3     ballPos, ballVel;
    Vec3     ballSpin;      // rad/s about each world axis
    Quat     ballRot;
    Man      men[MAX_MEN];
    uint32_t manCount;
    int      controlled;    // index of the man the pad is driving, -1 for none
    int      lastTouch;
} World;

static Quat quat_identity(void)
{
    Quat q = { 1.0f, 0.0f, 0.0f, 0.0f };
    return q;
}

static Quat quat_normalize(Quat q)
{
    float n = sqrtf(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (n < 1e-8f)
        return quat_identity();
    Quat r = { q.w / n, q.x / n, q.y / n, q.z / n };
    return r;
}

// Angular velocity applied for dt: the small-angle integration, which at 120 Hz
// is exact enough that the renormalise below is the only thing keeping it
// honest. A ball rolling at speed spins about 0.8 radians per step, which is
// also the reason the ROTATION has to be interpolated and not just the
// position — half a step of that is 24 degrees of visible stutter.
static Quat quat_integrate(Quat q, Vec3 omega, float dt)
{
    float hx = omega.x * dt * 0.5f;
    float hy = omega.y * dt * 0.5f;
    float hz = omega.z * dt * 0.5f;
    Quat r;
    r.w = q.w - hx * q.x - hy * q.y - hz * q.z;
    r.x = q.x + hx * q.w + hy * q.z - hz * q.y;
    r.y = q.y - hx * q.z + hy * q.w + hz * q.x;
    r.z = q.z + hx * q.y - hy * q.x + hz * q.w;
    return quat_normalize(r);
}

// Normalised lerp rather than a proper slerp: over one 120 Hz step the two
// differ by less than a thousandth of a degree, and one of them is four lines.
static Quat quat_nlerp(Quat a, Quat b, float t)
{
    if (a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z < 0.0f) {
        b.w = -b.w; b.x = -b.x; b.y = -b.y; b.z = -b.z;
    }
    Quat r = { a.w + (b.w - a.w) * t, a.x + (b.x - a.x) * t,
               a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
    return quat_normalize(r);
}

static Mat4 quat_to_mat4(Quat q)
{
    Mat4 m = mat4_identity();
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    m.m[0][0] = 1.0f - 2.0f * (yy + zz);
    m.m[0][1] = 2.0f * (xy + wz);
    m.m[0][2] = 2.0f * (xz - wy);
    m.m[1][0] = 2.0f * (xy - wz);
    m.m[1][1] = 1.0f - 2.0f * (xx + zz);
    m.m[1][2] = 2.0f * (yz + wx);
    m.m[2][0] = 2.0f * (xz + wy);
    m.m[2][1] = 2.0f * (yz - wx);
    m.m[2][2] = 1.0f - 2.0f * (xx + yy);
    return m;
}

static Vec3 vec3_add(Vec3 a, Vec3 b) { Vec3 r = { a.x+b.x, a.y+b.y, a.z+b.z }; return r; }
static Vec3 vec3_scale(Vec3 a, float s) { Vec3 r = { a.x*s, a.y*s, a.z*s }; return r; }
static float vec3_len(Vec3 a) { return sqrtf(vec3_dot(a, a)); }

static Vec3 vec3_lerp(Vec3 a, Vec3 b, float t)
{
    Vec3 r = { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
               a.z + (b.z - a.z) * t };
    return r;
}

// An angle is not a number for this purpose. A player turning past the wrap
// point goes from 3.1 to -3.1, and a straight lerp spins him the long way
// round through every heading he does not have — one frame of a man revolving
// on the spot, once per turn, and it took until this was written down to
// realise the walk phase must therefore never be wrapped at all.
static float lerpAngle(float a, float b, float t)
{
    float d = b - a;
    while (d > PI)  d -= 2.0f * PI;
    while (d < -PI) d += 2.0f * PI;
    return a + d * t;
}

static void worldLerp(const World *a, const World *b, float t, World *out)
{
    *out = *b;
    out->ballPos  = vec3_lerp(a->ballPos, b->ballPos, t);
    out->ballRot  = quat_nlerp(a->ballRot, b->ballRot, t);
    for (uint32_t i = 0; i < b->manCount && i < a->manCount; i++) {
        const Figure *fa = &a->men[i].look;
        const Figure *fb = &b->men[i].look;
        Figure *fo = &out->men[i].look;
        fo->pos    = vec3_lerp(fa->pos, fb->pos, t);
        fo->yaw    = lerpAngle(fa->yaw, fb->yaw, t);
        fo->stride = fa->stride + (fb->stride - fa->stride) * t;
        fo->swing  = fa->swing + (fb->swing - fa->swing) * t;
        fo->lean   = fa->lean + (fb->lean - fa->lean) * t;
    }
}

// --- the ball ----------------------------------------------------------------
//
// The whole of the physics, and the point of the fixed step: none of this is
// safe against a variable dt. A restitution is applied at the instant of
// contact, and with a frame-length step the instant is wherever the frame
// happened to land — so the same kick bounces to a different height depending
// on how busy the GPU was. That is the bug the clock exists to make
// impossible, and it is the one the probe below actually measures.

static void ballStep(World *w, float dt)
{
    Vec3 *p = &w->ballPos, *v = &w->ballVel, *s = &w->ballSpin;

    if (p->y > BALL_R + 0.0001f || v->y > 0.0f) {
        // In flight: gravity, quadratic drag, and the Magnus force, which is
        // spin crossed into velocity and is the entire reason a struck ball
        // bends instead of going where it was pointed.
        Vec3 magnus = vec3_cross(*s, *v);
        float speed = vec3_len(*v);
        v->x += (magnus.x * BALL_MAGNUS - v->x * speed * BALL_DRAG) * dt;
        v->y += (GRAVITY + magnus.y * BALL_MAGNUS
                 - v->y * speed * BALL_DRAG) * dt;
        v->z += (magnus.z * BALL_MAGNUS - v->z * speed * BALL_DRAG) * dt;
    }

    *p = vec3_add(*p, vec3_scale(*v, dt));

    // `<=`, and the equals sign is the whole bug. A ball at rest sits at
    // exactly BALL_R with exactly zero vertical speed, so a strict `<` was
    // false for every step after the one it landed on: no rolling friction
    // ever again — the ball rolled forever — and, worse, the spin stayed
    // whatever the KICK had put there and slowly faded. The ball was turning
    // about the axis it had been struck around while travelling in a
    // different direction entirely, which is exactly what "the roll looks
    // backwards" looks like. Floating point does not forgive a resting
    // contact tested with a strict inequality.
    if (p->y <= BALL_R) {
        p->y = BALL_R;
        if (v->y < -1.2f) {
            // A bounce. The turf takes most of the vertical and some of the
            // horizontal, and it takes the spin with it — which is why a
            // ball with backspin sits up off the first bounce.
            v->y = -v->y * BALL_BOUNCE;
            v->x *= BALL_GRAB;
            v->z *= BALL_GRAB;
            *s = vec3_scale(*s, 0.55f);
        } else {
            v->y = 0.0f;
            // Rolling. Speed bleeds off at a constant rate rather than
            // proportionally: a ball rolling on grass stops, it does not
            // approach stopping forever.
            float speed = sqrtf(v->x * v->x + v->z * v->z);
            if (speed > 0.001f) {
                float keep = speed - BALL_ROLL * dt;
                if (keep < 0.0f) keep = 0.0f;
                v->x *= keep / speed;
                v->z *= keep / speed;
            }
            // A rolling ball's spin is not free: it is exactly what the
            // contact patch demands. up x v, over the radius.
            s->x = v->z / BALL_R;
            s->y *= expf(-BALL_SPINDAMP * dt);
            s->z = -v->x / BALL_R;
        }
    } else {
        *s = vec3_scale(*s, expf(-0.35f * dt));
    }

    // Last, not first: the picture then shows the spin the ball has now
    // rather than the one it had before it touched the ground.
    w->ballRot = quat_integrate(w->ballRot, *s, dt);
}

// --- the goals ---------------------------------------------------------------
//
// Three separate things wearing one name. The frame is solid and the ball
// comes off it; the net is soft and the ball stops in it; and somewhere
// between the two is a line whose crossing is the only event in this whole
// example that a player cares about.
//
// Returns +1 or -1 — the side of the pitch the ball went into — or 0.
static int ballGoals(World *w, bool *hitFrame)
{
    Vec3 *p = &w->ballPos, *v = &w->ballVel;
    int scored = 0;
    *hitFrame = false;

    for (int s = -1; s <= 1; s += 2) {
        float side = (float)s;
        float gx = PITCH_X * side;

        // The two posts, as circles seen from above. A post is the one piece
        // of a football pitch whose entire purpose is to be hit.
        if (p->y < GOAL_H + POST_W) {
            for (int k = 0; k < 2; k++) {
                float pz = k ? GOAL_HALF_Z : -GOAL_HALF_Z;
                float dx = p->x - gx, dz = p->z - pz;
                float d = sqrtf(dx * dx + dz * dz);
                float want = BALL_R + POST_W * 0.5f;
                if (d >= want || d < 1e-4f)
                    continue;
                dx /= d; dz /= d;
                p->x = gx + dx * want;
                p->z = pz + dz * want;
                float vn = v->x * dx + v->z * dz;
                v->x -= 1.6f * vn * dx;      // 0.6 restitution off aluminium
                v->z -= 1.6f * vn * dz;
                if (fabsf(vn) > 2.0f)
                    *hitFrame = true;
            }
        }

        // The crossbar.
        if (fabsf(p->z) < GOAL_HALF_Z && fabsf(p->x - gx) < BALL_R + POST_W &&
            fabsf(p->y - GOAL_H) < BALL_R + POST_W * 0.5f) {
            if (fabsf(v->y) > 2.0f)
                *hitFrame = true;
            p->y = p->y > GOAL_H ? GOAL_H + BALL_R + POST_W * 0.5f
                                 : GOAL_H - BALL_R - POST_W * 0.5f;
            v->y = -v->y * 0.55f;
        }

        // Inside the mouth. Everything from here is the net: it absorbs
        // rather than reflects, which is why a goal ends with the ball
        // sitting in the back of it instead of coming out again.
        bool inside = p->x * side > gx * side &&
                      fabsf(p->z) < GOAL_HALF_Z && p->y < GOAL_H;
        if (!inside)
            continue;

        scored = s;
        float depthLimit = (PITCH_X + GOAL_DEPTH) * side;
        if (p->x * side > depthLimit * side - BALL_R) {
            p->x = depthLimit - BALL_R * side;
            v->x *= -0.12f;
            v->z *= 0.5f; v->y *= 0.5f;
        }
        if (fabsf(p->z) > GOAL_HALF_Z - BALL_R) {
            p->z = signf(p->z) * (GOAL_HALF_Z - BALL_R);
            v->z *= -0.12f;
        }
        if (p->y > GOAL_H - BALL_R) {
            p->y = GOAL_H - BALL_R;
            v->y *= -0.12f;
        }
        w->ballSpin = vec3_scale(w->ballSpin, 0.7f);
    }
    return scored;
}

// Out of play. Called only when the ball is not in a goal, so anything that
// leaves the rectangle is a restart of some kind.
//
// The rules are a rough sketch — no throw-in animation, no corner, and
// whoever arrives first takes it. What matters is that the ball comes BACK:
// without this the match drifts off the side of the world and eight men stand
// looking at a dot near the boards, which is exactly what it did.
static bool ballOutOfPlay(World *w)
{
    Vec3 *p = &w->ballPos;
    bool out = false;

    if (fabsf(p->z) > PITCH_Z + BALL_R) {
        p->z = signf(p->z) * (PITCH_Z - 0.4f);      // a throw-in, roughly
        p->x = clampf(p->x, -PITCH_X + 1.0f, PITCH_X - 1.0f);
        out = true;
    } else if (fabsf(p->x) > PITCH_X + BALL_R) {
        p->x = signf(p->x) * (PITCH_X - 4.5f);      // a goal kick, roughly
        p->z = clampf(p->z, -5.0f, 5.0f);
        out = true;
    }

    if (out) {
        p->y = BALL_R;
        w->ballVel.x = w->ballVel.y = w->ballVel.z = 0.0f;
        w->ballSpin.x = w->ballSpin.y = w->ballSpin.z = 0.0f;
    }
    return out;
}

// --- eight men ---------------------------------------------------------------
//
// The football is deliberately thin. What this step is for is not tactics, it
// is the thing a football game has and none of the previous examples did:
// several agents with continuous state, all of them touching the same object,
// all of them stepping at the same fixed rate. The intelligence below would
// embarrass a 1992 arcade cabinet and that is fine — what matters is that
// eight of them and a ball produce a picture that reads as a game.

#define MAN_R       0.36f
#define RUN_SPEED   7.2f
#define RUN_ACCEL   30.0f
#define TURN_RATE   10.0f
#define REACH       0.68f     // how close a foot has to be to touch the ball
#define KICK_COOL   0.22f

// Where each role stands when the ball is in the middle. Blue attacks +X and
// red is this mirrored, so one table describes both teams.
static const float HOME[TEAM_SIZE][2] = {
    { -20.5f,  0.0f },   // keeper
    { -12.0f, -6.5f },   // back
    {  -5.0f,  6.0f },   // middle
    {   2.5f, -2.5f },   // front
};

static float distXZ(Vec3 a, Vec3 b)
{
    float dx = a.x - b.x, dz = a.z - b.z;
    return sqrtf(dx * dx + dz * dz);
}

// Which man of a team is nearest the ball, keeper excluded — he has a job.
static int nearestOutfield(const World *w, int team)
{
    int best = -1;
    float bestD = 1e9f;
    for (uint32_t i = 0; i < w->manCount; i++) {
        if (w->men[i].team != team || w->men[i].role == 0)
            continue;
        float d = distXZ(w->men[i].look.pos, w->ballPos);
        if (d < bestD) { bestD = d; best = (int)i; }
    }
    return best;
}

// The AI, all of it: go where the ball is if you are the nearest, otherwise
// stand where your role says, shifted by where play is. A keeper does neither
// and slides along his line instead.
static void manThink(World *w, int i, float *outDirX, float *outDirZ,
                     float *outSpeed)
{
    const Man *m = &w->men[i];
    float side = m->team == 0 ? 1.0f : -1.0f;   // +1 attacks +X
    Vec3 pos = m->look.pos;
    float tx, tz, speed = RUN_SPEED * 0.86f;
    // How far outside the lines this man is allowed to want to be. A player
    // holding a shape stays on the pitch; a player chasing the ball follows it
    // out, and clamping BOTH to the touchline is a deadlock: the ball settles
    // two metres into touch, the nearest man stops at the line, and the match
    // stands there looking at it. Which is exactly what happened.
    float roam = 0.5f;

    if (m->role == 0) {
        tx = HOME[0][0] * side;
        tz = clampf(w->ballPos.z * 0.55f, -GOAL_HALF_Z - 1.0f,
                    GOAL_HALF_Z + 1.0f);
        // Off his line only when the ball is genuinely his problem.
        if (w->ballPos.x * side < -PITCH_X + 8.0f) {
            tx = HOME[0][0] * side + (w->ballPos.x - HOME[0][0] * side) * 0.35f;
            tz = w->ballPos.z * 0.8f;
            speed = RUN_SPEED;
        }
    } else if (i == nearestOutfield(w, m->team)) {
        // As far out as the boards, because that is as far as the ball goes.
        roam = 6.5f;
        // Chase, but aim a little past the ball so the run finishes with the
        // ball in front of the boot rather than under it.
        float aheadX = w->ballPos.x + w->ballVel.x * 0.18f;
        float aheadZ = w->ballPos.z + w->ballVel.z * 0.18f;
        tx = aheadX; tz = aheadZ;
        speed = RUN_SPEED;
    } else {
        tx = HOME[m->role][0] * side + clampf(w->ballPos.x * 0.55f, -9.0f, 9.0f);
        tz = HOME[m->role][1] * side + clampf(w->ballPos.z * 0.45f, -6.0f, 6.0f);
    }

    tx = clampf(tx, -PITCH_X - roam, PITCH_X + roam);
    tz = clampf(tz, -PITCH_Z - roam, PITCH_Z + roam);

    float dx = tx - pos.x, dz = tz - pos.z;
    float d = sqrtf(dx * dx + dz * dz);
    if (d < 0.35f) { *outDirX = 0.0f; *outDirZ = 0.0f; *outSpeed = 0.0f; return; }
    *outDirX = dx / d;
    *outDirZ = dz / d;
    // Ease off over the last stride so nobody vibrates on their own mark.
    *outSpeed = speed * clampf(d / 1.6f, 0.25f, 1.0f);
}

static void manMove(Man *m, float dirX, float dirZ, float speed, float dt)
{
    float wantX = dirX * speed, wantZ = dirZ * speed;
    float ax = wantX - m->vel.x, az = wantZ - m->vel.z;
    float amag = sqrtf(ax * ax + az * az);
    float amax = RUN_ACCEL * dt;
    if (amag > amax && amag > 1e-5f) { ax *= amax / amag; az *= amax / amag; }
    m->vel.x += ax;
    m->vel.z += az;
    m->look.pos.x += m->vel.x * dt;
    m->look.pos.z += m->vel.z * dt;
    m->look.pos.x = clampf(m->look.pos.x, -PITCH_X - 7.0f, PITCH_X + 7.0f);
    m->look.pos.z = clampf(m->look.pos.z, -PITCH_Z - 5.0f, PITCH_Z + 5.0f);

    float sp = sqrtf(m->vel.x * m->vel.x + m->vel.z * m->vel.z);
    if (sp > 0.2f) {
        float want = yawFromDir(m->vel.x, m->vel.z);
        float d = want - m->look.yaw;
        while (d > PI)  d -= 2.0f * PI;
        while (d < -PI) d += 2.0f * PI;
        float step = TURN_RATE * dt;
        m->look.yaw += clampf(d, -step, step);
    }
    // The walk is driven by distance covered, not by time: a man slowing down
    // takes shorter steps instead of the same steps more slowly, and that one
    // line is most of the difference between running and skating.
    m->look.stride += sp * dt * 2.6f;
    m->look.swing = clampf(sp / RUN_SPEED, 0.0f, 1.0f);
    m->look.lean  = clampf(sp / RUN_SPEED, 0.0f, 1.0f) * 0.10f;
    if (m->kickCool > 0.0f) m->kickCool -= dt;
}

// Nobody stands inside anybody. Twenty-eight pairs, resolved by pushing both
// halfway out — cheap, and without it the whole team converges into one man.
static void manSeparate(World *w)
{
    for (uint32_t i = 0; i < w->manCount; i++) {
        for (uint32_t j = i + 1; j < w->manCount; j++) {
            Vec3 *a = &w->men[i].look.pos, *b = &w->men[j].look.pos;
            float dx = b->x - a->x, dz = b->z - a->z;
            float d = sqrtf(dx * dx + dz * dz);
            float want = MAN_R * 2.0f;
            if (d > want || d < 1e-4f)
                continue;
            float push = (want - d) * 0.5f;
            dx /= d; dz /= d;
            a->x -= dx * push; a->z -= dz * push;
            b->x += dx * push; b->z += dz * push;
        }
    }
}

// --- the batch ---------------------------------------------------------------

static void brickReset(BrickBatch *b)
{
    b->count = 0;
}

// Column-major Mat4 in, three rows out. The transpose lives here, once,
// instead of in the eleven places a limb is built.
static void brickPush(BrickBatch *b, Mat4 m, const float tint[3], float flag)
{
    if (b->count >= MAX_BRICKS)
        return;
    Brick *k = &b->items[b->count++];
    for (int row = 0; row < 3; row++) {
        k->row[row][0] = m.m[0][row];
        k->row[row][1] = m.m[1][row];
        k->row[row][2] = m.m[2][row];
        k->row[row][3] = m.m[3][row];
    }
    k->tint[0] = tint[0]; k->tint[1] = tint[1]; k->tint[2] = tint[2];
    k->tint[3] = flag;
}

static Mat4 mat4_mul3(Mat4 a, Mat4 b, Mat4 c)
{
    return mat4_mul(a, mat4_mul(b, c));
}

// A limb: a box hanging from a joint, swinging about it. The two translations
// either side of the rotation are the whole difference between a leg and a
// floating rectangle — the box mesh is centred on itself, so it has to be
// pushed half its length down the local Y before the joint can turn it.
static Mat4 limb(Mat4 body, float jx, float jy, float jz, float pitch,
                 float roll, float w, float len, float d)
{
    Mat4 m = mat4_mul(body, mat4_translate(jx, jy, jz));
    m = mat4_mul(m, mat4_rotate_z(roll));
    m = mat4_mul(m, mat4_rotate_x(pitch));
    return mat4_mul3(m, mat4_translate(0.0f, -len * 0.5f, 0.0f),
                     mat4_scale(w, len, d));
}

// Where a limb ends, in the same frame: the joint, turned, then walked down
// its own length. A hand and a foot are placed by asking the arm and the leg
// where they finished rather than by repeating the trigonometry.
static Mat4 limbEnd(Mat4 body, float jx, float jy, float jz, float pitch,
                    float roll, float len)
{
    Mat4 m = mat4_mul(body, mat4_translate(jx, jy, jz));
    m = mat4_mul(m, mat4_rotate_z(roll));
    m = mat4_mul(m, mat4_rotate_x(pitch));
    return mat4_mul(m, mat4_translate(0.0f, -len, 0.0f));
}

static void figureBuild(const Figure *f, BrickBatch *box, BrickBatch *sphere,
                        BrickBatch *wedge)
{
    static const float SKIN[3] = { 0.94f, 0.80f, 0.62f };
    float shorts[3] = { f->tint[0] * 0.45f, f->tint[1] * 0.45f,
                        f->tint[2] * 0.45f };

    Mat4 body = mat4_mul(mat4_translate(f->pos.x, f->pos.y, f->pos.z),
                         mat4_rotate_y(f->yaw));
    body = mat4_mul(body, mat4_rotate_x(f->lean));

    // Arms and legs swing in opposite phase, and the arms swing less: a walk
    // where they match reads as a toy soldier.
    float legA = sinf(f->stride) * 0.85f * f->swing;
    float legB = sinf(f->stride + PI) * 0.85f * f->swing;
    float armA = sinf(f->stride + PI) * 0.55f * f->swing;
    float armB = sinf(f->stride) * 0.55f * f->swing;

    brickPush(box, mat4_mul(body, mat4_mul(mat4_translate(0, TORSO_Y, 0),
                                           mat4_scale(0.46f, TORSO_H, 0.26f))),
              f->tint, 0.0f);

    brickPush(box, limb(body, -0.13f, HIP_Y, 0.0f, legA, 0.0f,
                        LEG_W, LEG_LEN, LEG_W), shorts, 0.0f);
    brickPush(box, limb(body, 0.13f, HIP_Y, 0.0f, legB, 0.0f,
                        LEG_W, LEG_LEN, LEG_W), shorts, 0.0f);

    // The roll holds the arms clear of the chest, and its sign is the whole
    // point: a positive rotation about Z carries a hanging limb towards +X, so
    // the LEFT arm — the one at -X — needs a negative one. Getting that
    // backwards does not look like a mistake, it looks like a man walking with
    // his arms inside his own ribs.
    brickPush(box, limb(body, -SHOULDER_X, SHOULDER, 0.0f, armA, -ARM_FLARE,
                        ARM_W, ARM_LEN, ARM_W), f->tint, 0.0f);
    brickPush(box, limb(body, SHOULDER_X, SHOULDER, 0.0f, armB, ARM_FLARE,
                        ARM_W, ARM_LEN, ARM_W), f->tint, 0.0f);

    brickPush(sphere, mat4_mul(body, mat4_mul(mat4_translate(0, HEAD_Y, 0),
                                              mat4_scale(HEAD_R * 2.0f,
                                                         HEAD_R * 2.0f,
                                                         HEAD_R * 2.0f))),
              SKIN, 0.0f);
    brickPush(sphere, mat4_mul(limbEnd(body, -SHOULDER_X, SHOULDER, 0.0f, armA,
                                       -ARM_FLARE, ARM_LEN),
                               mat4_scale(0.19f, 0.19f, 0.19f)), SKIN, 0.0f);
    brickPush(sphere, mat4_mul(limbEnd(body, SHOULDER_X, SHOULDER, 0.0f, armB,
                                       ARM_FLARE, ARM_LEN),
                               mat4_scale(0.19f, 0.19f, 0.19f)), SKIN, 0.0f);

    // The foot un-does its own leg's swing so the sole stays level with the
    // ground — a foot that rotates with the shin points at the sky halfway
    // through every stride.
    static const float BOOT[3] = { 0.10f, 0.10f, 0.12f };
    brickPush(wedge, mat4_mul3(limbEnd(body, -0.13f, HIP_Y, 0, legA, 0, LEG_LEN),
                               mat4_rotate_x(-legA),
                               mat4_mul(mat4_translate(0, -FOOT_H, 0),
                                        mat4_scale(0.20f, FOOT_H, 0.34f))),
              BOOT, 0.0f);
    brickPush(wedge, mat4_mul3(limbEnd(body, 0.13f, HIP_Y, 0, legB, 0, LEG_LEN),
                               mat4_rotate_x(-legB),
                               mat4_mul(mat4_translate(0, -FOOT_H, 0),
                                        mat4_scale(0.20f, FOOT_H, 0.34f))),
              BOOT, 0.0f);
}

// --- geometry built on the console -------------------------------------------
//
// The pitch markings are not baked, and that is deliberate: they are entirely
// determined by six numbers at the top of this file, so a baked file would be
// those numbers written down twice with nothing keeping them in agreement.
// A hundred-odd triangles built at startup is not a load time.

typedef struct {
    float v[8];    // x y z | nx ny nz | u v
} GVertex;

typedef struct {
    GVertex  verts[512];
    uint16_t idx[768];
    uint32_t vertexCount, indexCount;
} MeshBuild;

static void meshQuad(MeshBuild *mb, float x0, float z0, float x1, float z1,
                     float y)
{
    if (mb->vertexCount + 4 > 512 || mb->indexCount + 6 > 768)
        return;
    uint16_t base = (uint16_t)mb->vertexCount;
    const float corner[4][2] = { { x0, z0 }, { x1, z0 }, { x1, z1 }, { x0, z1 } };
    for (int i = 0; i < 4; i++) {
        GVertex *g = &mb->verts[mb->vertexCount++];
        g->v[0] = corner[i][0]; g->v[1] = y; g->v[2] = corner[i][1];
        g->v[3] = 0.0f; g->v[4] = 1.0f; g->v[5] = 0.0f;
        g->v[6] = corner[i][0] / GRASS_TILE; g->v[7] = corner[i][1] / GRASS_TILE;
    }
    // CCW seen from above (+Y), which is where anything looking at a pitch is.
    static const uint16_t order[6] = { 0, 3, 2, 0, 2, 1 };
    for (int i = 0; i < 6; i++)
        mb->idx[mb->indexCount++] = (uint16_t)(base + order[i]);
}

// A quad anywhere, in any orientation, with texture coordinates measured in
// METRES rather than in fractions of the face. That is the whole reason this
// exists next to meshQuad: a net has a mesh size, and a mesh size that
// stretched to fit each panel would give a goal whose cord was fat on the back
// and thin on the sides.
static void meshQuad3(MeshBuild *mb, Vec3 a, Vec3 b, Vec3 c, Vec3 d,
                      float cell)
{
    if (mb->vertexCount + 4 > 512 || mb->indexCount + 6 > 768)
        return;
    Vec3 e0 = vec3_sub(b, a), e1 = vec3_sub(d, a);
    Vec3 n = vec3_normalize(vec3_cross(e0, e1));
    float u = vec3_len(e0) / cell, v = vec3_len(e1) / cell;
    const Vec3 corner[4] = { a, b, c, d };
    const float uv[4][2] = { { 0, 0 }, { u, 0 }, { u, v }, { 0, v } };
    uint16_t base = (uint16_t)mb->vertexCount;
    for (int i = 0; i < 4; i++) {
        GVertex *g = &mb->verts[mb->vertexCount++];
        g->v[0] = corner[i].x; g->v[1] = corner[i].y; g->v[2] = corner[i].z;
        g->v[3] = n.x; g->v[4] = n.y; g->v[5] = n.z;
        g->v[6] = uv[i][0]; g->v[7] = uv[i][1];
    }
    static const uint16_t order[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; i++)
        mb->idx[mb->indexCount++] = (uint16_t)(base + order[i]);
}

// A line is a quad given a width, which lets one call draw a touchline and one
// draw a segment of the centre circle.
static void meshLine(MeshBuild *mb, float ax, float az, float bx, float bz,
                     float width, float y)
{
    float dx = bx - ax, dz = bz - az;
    float len = sqrtf(dx * dx + dz * dz);
    if (len < 1e-5f)
        return;
    float nx = -dz / len * width * 0.5f;
    float nz = dx / len * width * 0.5f;
    if (mb->vertexCount + 4 > 512 || mb->indexCount + 6 > 768)
        return;
    uint16_t base = (uint16_t)mb->vertexCount;
    const float corner[4][2] = {
        { ax + nx, az + nz }, { bx + nx, bz + nz },
        { bx - nx, bz - nz }, { ax - nx, az - nz },
    };
    for (int i = 0; i < 4; i++) {
        GVertex *g = &mb->verts[mb->vertexCount++];
        g->v[0] = corner[i][0]; g->v[1] = y; g->v[2] = corner[i][1];
        g->v[3] = 0.0f; g->v[4] = 1.0f; g->v[5] = 0.0f;
        g->v[6] = 0.0f; g->v[7] = 0.0f;
    }
    // Wound the opposite way round from meshQuad above, and that is not a typo:
    // the corners are laid out along the segment rather than around a
    // rectangle, so the same index order would face the ground. Culling makes
    // the mistake invisible instead of wrong — the lines simply were not there.
    static const uint16_t order[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; i++)
        mb->idx[mb->indexCount++] = (uint16_t)(base + order[i]);
}

static bool meshUpload(MeshBuild *mb, CremaMesh *out, const CremaMesh *layout)
{
    memset(out, 0, sizeof(*out));
    if (!CremaBufferCreateVertex(&out->vbo, sizeof(GVertex), mb->vertexCount,
                                 mb->verts))
        return false;
    if (!CremaBufferCreateIndexU16(&out->ibo, mb->indexCount, mb->idx)) {
        CremaBufferDestroy(&out->vbo);
        return false;
    }
    out->vertexCount = mb->vertexCount;
    out->indexCount  = mb->indexCount;
    out->stride      = sizeof(GVertex);
    out->attribCount = layout->attribCount;
    memcpy(out->attribs, layout->attribs, sizeof(out->attribs));
    return true;
}

static void buildGround(MeshBuild *mb)
{
    memset(mb, 0, sizeof(*mb));
    // Bigger than the pitch: the ball goes out, and so does the eye.
    meshQuad(mb, -PITCH_X - 8.0f, -PITCH_Z - 6.0f,
             PITCH_X + 8.0f, PITCH_Z + 6.0f, 0.0f);
}

// The one mesh with texture coordinates that mean something: a shadow is a
// disc drawn on a square, and the square has to know where its middle is.
static void buildUnitQuad(MeshBuild *mb)
{
    memset(mb, 0, sizeof(*mb));
    const float c[4][2] = { { -0.5f, -0.5f }, { 0.5f, -0.5f },
                            { 0.5f, 0.5f }, { -0.5f, 0.5f } };
    for (int i = 0; i < 4; i++) {
        GVertex *g = &mb->verts[mb->vertexCount++];
        g->v[0] = c[i][0]; g->v[1] = 0.0f; g->v[2] = c[i][1];
        g->v[3] = 0.0f; g->v[4] = 1.0f; g->v[5] = 0.0f;
        g->v[6] = c[i][0] + 0.5f; g->v[7] = c[i][1] + 0.5f;
    }
    static const uint16_t order[6] = { 0, 3, 2, 0, 2, 1 };
    for (int i = 0; i < 6; i++)
        mb->idx[mb->indexCount++] = order[i];
}

// Four panels a goal, back, two sides and a roof, and no floor — a net does
// not have one and the ball would sit on it.
static void buildNets(MeshBuild *mb)
{
    memset(mb, 0, sizeof(*mb));
    for (int s = -1; s <= 1; s += 2) {
        float side = (float)s;
        float gx = PITCH_X * side;
        float bx = gx + GOAL_DEPTH * side;
        float z0 = -GOAL_HALF_Z, z1 = GOAL_HALF_Z;
        float h = GOAL_H;

        Vec3 b00 = { bx, 0.0f, z0 }, b01 = { bx, 0.0f, z1 };
        Vec3 b10 = { bx, h, z0 },    b11 = { bx, h, z1 };
        Vec3 f00 = { gx, 0.0f, z0 }, f01 = { gx, 0.0f, z1 };
        Vec3 f10 = { gx, h, z0 },    f11 = { gx, h, z1 };

        meshQuad3(mb, b00, b01, b11, b10, NET_CELL);   // back
        meshQuad3(mb, f00, b00, b10, f10, NET_CELL);   // -Z side
        meshQuad3(mb, b01, f01, f11, b11, NET_CELL);   // +Z side
        meshQuad3(mb, f10, b10, b11, f11, NET_CELL);   // roof
    }
}

static void buildLines(MeshBuild *mb)
{
    memset(mb, 0, sizeof(*mb));
    const float y = 0.012f;      // above the grass, below anything that stands
    const float X = PITCH_X, Z = PITCH_Z;

    meshLine(mb, -X, -Z, X, -Z, LINE_W, y);      // touchlines
    meshLine(mb, -X, Z, X, Z, LINE_W, y);
    meshLine(mb, -X, -Z, -X, Z, LINE_W, y);      // goal lines
    meshLine(mb, X, -Z, X, Z, LINE_W, y);
    meshLine(mb, 0.0f, -Z, 0.0f, Z, LINE_W, y);  // halfway

    for (int i = 0; i < 32; i++) {
        float a0 = (float)i / 32.0f * 2.0f * PI;
        float a1 = (float)(i + 1) / 32.0f * 2.0f * PI;
        meshLine(mb, cosf(a0) * CIRCLE_R, sinf(a0) * CIRCLE_R,
                 cosf(a1) * CIRCLE_R, sinf(a1) * CIRCLE_R, LINE_W, y);
    }
    meshQuad(mb, -0.16f, -0.16f, 0.16f, 0.16f, y);   // centre spot

    for (int side = -1; side <= 1; side += 2) {
        float gx = (float)side * X;
        float bx = gx - (float)side * BOX_DEPTH;
        meshLine(mb, bx, -BOX_HALF_Z, bx, BOX_HALF_Z, LINE_W, y);
        meshLine(mb, gx, -BOX_HALF_Z, bx, -BOX_HALF_Z, LINE_W, y);
        meshLine(mb, gx, BOX_HALF_Z, bx, BOX_HALF_Z, LINE_W, y);
    }
}

// --- the blades --------------------------------------------------------------
//
// Too big for the little static builder above — this is fourteen thousand
// triangles and it is built once into a heap buffer that is handed to the GPU
// and freed. The layout is the same 32 bytes as everything else, with the
// fields repurposed: the normal slot carries the blade's colour and the UV
// slot carries its wind phase and whether the vertex is at the bottom or the
// top. A vertex format that already exists and is being used for something
// else is cheaper than a second one.

// 188 x 106 = 19928 blades = 59784 vertices. The ceiling is 65535 and it is a
// hard one: a 16-bit index cannot name a vertex past it.
#define GRASS_CELL   0.33f
#define GRASS_NX     188        // 62 m, the full width of the visible ground
#define GRASS_NZ     106        // 35 m, as far back as the fade reaches
#define GRASS_Z0     (-14.0f)

static uint32_t hash32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static float hashf(uint32_t seed)
{
    return (float)(hash32(seed) & 0xFFFFFF) / (float)0xFFFFFF;
}

static bool buildGrass(CremaMesh *out, const CremaMesh *layout,
                       uint32_t *outBlades)
{
    const uint32_t blades = GRASS_NX * GRASS_NZ;
    GVertex *verts = (GVertex *)malloc(sizeof(GVertex) * blades * 3);
    uint16_t *idx = (uint16_t *)malloc(sizeof(uint16_t) * blades * 3);
    if (!verts || !idx) {
        free(verts); free(idx);
        return false;
    }

    uint32_t v = 0;
    for (uint32_t iz = 0; iz < GRASS_NZ; iz++) {
        for (uint32_t ix = 0; ix < GRASS_NX; ix++) {
            uint32_t seed = iz * 8191u + ix;
            float jx = hashf(seed * 3u) - 0.5f;
            float jz = hashf(seed * 5u + 1u) - 0.5f;
            float bx = ((float)ix - GRASS_NX * 0.5f) * GRASS_CELL
                       + jx * GRASS_CELL;
            float bz = GRASS_Z0 + (float)iz * GRASS_CELL + jz * GRASS_CELL;

            float h = 0.13f + hashf(seed * 7u + 2u) * 0.13f;
            float wdt = 0.030f + hashf(seed * 11u + 3u) * 0.016f;
            float lean = (hashf(seed * 13u + 4u) - 0.5f) * 0.10f;
            float phase = hashf(seed * 17u + 5u) * 6.2831f;

            // The blades are not all the same green. A field of one colour
            // reads as a carpet however many triangles it has.
            float shade = 0.80f + hashf(seed * 19u + 6u) * 0.42f;
            float r = 0.195f * shade, g = 0.490f * shade, b = 0.150f * shade;

            // Base left, base right, tip. Wound so the front face is the one
            // pointing roughly at the touchline camera — and drawn with
            // culling off anyway, because half of them face away.
            GVertex *a = &verts[v + 0], *bb = &verts[v + 1], *c = &verts[v + 2];
            a->v[0] = bx - wdt; a->v[1] = 0.0f; a->v[2] = bz;
            bb->v[0] = bx + wdt; bb->v[1] = 0.0f; bb->v[2] = bz;
            c->v[0] = bx + lean; c->v[1] = h; c->v[2] = bz + lean * 0.5f;
            for (int k = 0; k < 3; k++) {
                GVertex *g3 = &verts[v + k];
                g3->v[3] = r; g3->v[4] = g; g3->v[5] = b;
                g3->v[6] = phase;
                g3->v[7] = (k == 2) ? 1.0f : 0.0f;
            }
            idx[v] = (uint16_t)v; idx[v + 1] = (uint16_t)(v + 1);
            idx[v + 2] = (uint16_t)(v + 2);
            v += 3;
        }
    }

    memset(out, 0, sizeof(*out));
    bool ok = CremaBufferCreateVertex(&out->vbo, sizeof(GVertex), v, verts) &&
              CremaBufferCreateIndexU16(&out->ibo, v, idx);
    free(verts); free(idx);
    if (!ok) {
        CremaMeshDestroy(out);
        return false;
    }
    out->vertexCount = v;
    out->indexCount  = v;
    out->stride      = sizeof(GVertex);
    out->attribCount = layout->attribCount;
    memcpy(out->attribs, layout->attribs, sizeof(out->attribs));
    *outBlades = blades;
    return true;
}

// --- the application ---------------------------------------------------------

typedef struct {
    CremaMesh    box, sphere, wedge;
    CremaMesh    ground, lines, quad, nets, blades;
    GX2Texture   grass, net;
    GX2Sampler   grassSampler, netSampler;
    uint32_t     bladeCount;

    CremaShader *shBrick, *shGround, *shShadow, *shNet, *shGrass;
    int32_t      brGlobalVs, brGlobalPs, brInst;
    int32_t      grGlobalVs, grGlobalPs;
    int32_t      sdGlobalVs, sdInst;
    int32_t      ntGlobalVs, ntGlobalPs;
    int32_t      blGlobalVs, blGlobalPs;
    uint32_t     grUnit, ntUnit;

    CremaUniformRing globalRing;
    void            *globalUbo;

    BrickBatch   boxes, spheres, wedges, marks, shadows;

    Global       global;
    float        camX;

    // The two states the whole example is built around, and the number
    // between them.
    World        prev, cur, view;
    float        accumulator;
    float        alpha;

    // What the clock actually did, kept because the argument for a fixed step
    // is not a feeling about it.
    uint32_t     stepsThisFrame;
    uint32_t     stepsTotal, framesTotal;
    uint32_t     stepHistogram[5];   // 0, 1, 2, 3, 4+ steps in a frame
    uint32_t     dropped;

    // The probe: a kick nobody's hands are on, measured in steps rather than
    // in seconds, so the answer is a number that must not change.
    CremaBank    bank;
    bool         hasBank;
    const CremaInstrument *sfxKick, *sfxTap, *sfxWhistle, *sfxPost, *sfxCheer;

    CremaHudRenderer hudRenderer;
    GX2Texture   font;
    GX2Sampler   fontSampler;
    HudList      tvHud, padHud;
    CremaUniformRing tvHudRing, padHudRing;
    void        *tvHudUbo, *padHudUbo;

    uint32_t     scoreBlue, scoreRed;
    int          phase;
    uint32_t     phaseSteps;    // counted in steps, like everything else here
    float        matchTime;     // seconds of SIMULATION, not of wall clock

    bool         probeArmed, probeRequest;
    uint32_t     probeStep;
    const char  *probeLabel;
    double       lastFps;
    bool         stress, forceStress;
} App;

static App app;

static void batchInit(BrickBatch *b, const CremaMesh *mesh)
{
    memset(b, 0, sizeof(*b));
    b->mesh = mesh;
    CremaUniformRingCreate(&b->ring, sizeof(Brick) * MAX_BRICKS,
                           CREMA_FRAMES_IN_FLIGHT);
}

static void batchStore(BrickBatch *b, uint32_t slot)
{
    if (b->count == 0) {
        b->ubo = NULL;
        return;
    }
    b->ubo = CremaUniformRingStore(&b->ring, slot, b->items,
                                   sizeof(Brick) * b->count);
}

static void batchDraw(const BrickBatch *b)
{
    if (!b->ubo || b->count == 0)
        return;
    GX2SetVertexUniformBlock(app.brInst, sizeof(Brick) * b->count, b->ubo);
    CremaMeshDraw(b->mesh, b->count);
}

// --- one step of the world ---------------------------------------------------
//
// Note what is NOT passed in: a delta. The step takes dt as an argument because
// writing SIM_DT eleven times is worse, but it is the same number every time
// and nothing in here would be correct if it were not.
// Everybody back where they started. The one place in the example that writes
// to the whole world at once, and it is fine to do it inside a step: the
// interpolation between the frame before and the frame after teleports every
// man in one frame, which is what a restart looks like anyway.
static void worldKickoff(World *w)
{
    for (uint32_t i = 0; i < w->manCount; i++) {
        Man *m = &w->men[i];
        float side = m->team == 0 ? 1.0f : -1.0f;
        m->look.pos.x = HOME[m->role][0] * side;
        m->look.pos.z = HOME[m->role][1] * side;
        m->look.yaw = yawFromDir(m->team == 0 ? 1.0f : -1.0f, 0.0f);
        m->look.swing = 0.0f;
        m->vel.x = m->vel.z = 0.0f;
        m->kickCool = 0.0f;
    }
    w->ballPos.x = 0.0f; w->ballPos.y = BALL_R; w->ballPos.z = 0.0f;
    w->ballVel.x = w->ballVel.y = w->ballVel.z = 0.0f;
    w->ballSpin.x = w->ballSpin.y = w->ballSpin.z = 0.0f;
    w->lastTouch = -1;
}

static void kickBall(App *a, int by, float dirX, float dirZ, float speed,
                     float rise, float sideSpin)
{
    World *w = &a->cur;
    w->ballVel.x = dirX * speed;
    w->ballVel.y = rise;
    w->ballVel.z = dirZ * speed;
    w->ballSpin.x = 0.0f;
    w->ballSpin.y = sideSpin;
    w->ballSpin.z = 0.0f;
    w->lastTouch = by;
    if (by >= 0)
        w->men[by].kickCool = KICK_COOL;

    // The sound is fired from inside the STEP, which is where the event is.
    // Firing it from the frame would mean a frame that ran three steps and
    // contained two kicks played one of them — and it would be whichever one
    // happened to be last.
    const CremaInstrument *snd = speed > 17.0f ? a->sfxKick : a->sfxTap;
    if (snd)
        CremaAudioPlay(&snd->sound, speed > 17.0f ? 0.85f : 0.55f,
                       0.94f + speed * 0.006f);
}

static void worldStep(App *a, const CremaInput *in, float dt)
{
    World *w = &a->cur;

    // --- the phase, counted in steps -------------------------------------
    //
    // Every duration in this example is a number of steps rather than a
    // number of seconds, and that is not pedantry: a countdown in seconds
    // integrated against a variable dt finishes at a different point in the
    // simulation depending on the frame rate, which puts the restart in a
    // different place. Steps cannot do that.
    if (a->phaseSteps > 0)
        a->phaseSteps--;

    if (a->phase == PHASE_FULLTIME) {
        // Frozen. The one phase where the world does not step at all — and
        // the men keep their last pose because nothing writes over it.
        if (CremaInputPressed(in, VPAD_BUTTON_A)) {
            a->scoreBlue = a->scoreRed = 0;
            a->matchTime = 0.0f;
            a->phase = PHASE_KICKOFF;
            a->phaseSteps = KICKOFF_STEPS;
            worldKickoff(w);
        }
        return;
    }

    if (a->phase == PHASE_KICKOFF && a->phaseSteps == 0) {
        a->phase = PHASE_PLAY;
        if (a->sfxWhistle)
            CremaAudioPlay(&a->sfxWhistle->sound, 0.7f, 1.0f);
    }

    if (a->phase == PHASE_GOAL && a->phaseSteps == 0) {
        worldKickoff(w);
        a->phase = PHASE_KICKOFF;
        a->phaseSteps = KICKOFF_STEPS;
    }

    if (a->phase == PHASE_PLAY) {
        a->matchTime += dt;
        if (a->matchTime >= MATCH_LEN) {
            a->matchTime = MATCH_LEN;
            a->phase = PHASE_FULLTIME;
            if (a->sfxWhistle) {
                CremaAudioPlay(&a->sfxWhistle->sound, 0.8f, 1.0f);
                CremaAudioPlay(&a->sfxWhistle->sound, 0.8f, 0.97f);
            }
            if (a->sfxCheer)
                CremaAudioPlay(&a->sfxCheer->sound, 0.8f, 0.95f);
            WHBLogPrintf("[poc14] FULL TIME - blue %u red %u",
                         a->scoreBlue, a->scoreRed);
        }
    }

    // The men move in every phase but this decides whether they may touch the
    // ball — which is the whole of "the game is running" in one bool.
    bool live = (a->phase == PHASE_PLAY);

    // Aim in camera space: pushing the stick away from you sends the ball away
    // from you, which on a side-on view means -Z, and nobody has ever wanted
    // it to mean anything else.
    float ax = in->leftX, az = -in->leftY;
    float aim = sqrtf(ax * ax + az * az);
    bool aiming = aim >= 0.15f;
    if (!aiming) { ax = 0.0f; az = -1.0f; }
    else         { ax /= aim; az /= aim; }

    // The pad drives whichever blue shirt is nearest the ball, and the switch
    // happens on its own. A football game that makes you choose is a football
    // game you spend playing the menu.
    w->controlled = nearestOutfield(w, 0);

    for (uint32_t i = 0; i < w->manCount; i++) {
        Man *m = &w->men[i];
        float dirX, dirZ, speed;
        if ((int)i == w->controlled) {
            dirX = aiming ? ax : 0.0f;
            dirZ = aiming ? az : 0.0f;
            speed = aiming ? RUN_SPEED * clampf(aim, 0.0f, 1.0f) : 0.0f;
        } else {
            manThink(w, (int)i, &dirX, &dirZ, &speed);
        }
        manMove(m, dirX, dirZ, speed, dt);
    }
    manSeparate(w);

    // --- who touches the ball -------------------------------------------
    //
    // In front of the boot, not at the middle of the man: the reach point is
    // pushed forward along the heading, which is why running past the ball
    // does not collect it and running at it does.
    for (uint32_t i = 0; i < w->manCount && !a->probeArmed && live; i++) {
        // `!probeArmed`, and it is not a detail. The probe is a measurement,
        // and a measurement with eight men running through it measures them
        // as well. Twice in a row the two runs happened to agree because
        // nobody reached the ball — which is luck, and luck reported as a
        // result is worse than no result. The ball is untouchable while the
        // probe is in the air.
        Man *m = &w->men[i];
        if (m->kickCool > 0.0f)
            continue;
        float hx, hz;
        headingVec(m->look.yaw, &hx, &hz);
        float fx = m->look.pos.x + hx * 0.22f;
        float fz = m->look.pos.z + hz * 0.22f;
        float dx = w->ballPos.x - fx, dz = w->ballPos.z - fz;
        float d = sqrtf(dx * dx + dz * dz);
        if (d > REACH || w->ballPos.y > 1.1f)
            continue;

        float side = m->team == 0 ? 1.0f : -1.0f;

        if ((int)i == w->controlled) {
            bool pass  = CremaInputPressed(in, VPAD_BUTTON_A);
            bool shoot = CremaInputPressed(in, VPAD_BUTTON_B);
            if (pass || shoot) {
                float kx = aiming ? ax : hx, kz = aiming ? az : hz;
                float spin = 0.0f;
                if (CremaInputHeld(in, VPAD_BUTTON_L)) spin -= 30.0f;
                if (CremaInputHeld(in, VPAD_BUTTON_R)) spin += 30.0f;
                kickBall(a, (int)i, kx, kz, shoot ? 21.0f : 14.0f,
                         shoot ? 4.2f : 1.2f, spin);
                continue;
            }
        } else {
            // The AI shoots when the goal is worth shooting at and passes it
            // forward otherwise. Two lines, and they are enough to make the
            // ball travel up the pitch instead of orbiting one player.
            float toGoal = (PITCH_X * side) - m->look.pos.x;
            if (fabsf(toGoal) < 13.0f && fabsf(m->look.pos.z) < 9.0f) {
                float gx = PITCH_X * side - m->look.pos.x;
                float gz = 0.0f - m->look.pos.z;
                float gd = sqrtf(gx * gx + gz * gz);
                kickBall(a, (int)i, gx / gd, gz / gd, 19.0f, 3.0f, 0.0f);
                continue;
            }
            // Under pressure, get rid of it up the pitch. No timer and no dice
            // roll: the trigger is an opponent close enough to take it, which
            // is also why the ball moves when two players meet instead of the
            // two of them shuffling over it.
            bool pressed = false;
            for (uint32_t k = 0; k < w->manCount && !pressed; k++)
                if (w->men[k].team != m->team &&
                    distXZ(w->men[k].look.pos, m->look.pos) < 1.7f)
                    pressed = true;
            if (pressed) {
                kickBall(a, (int)i, side * 0.85f + hx * 0.15f, hz * 0.5f,
                         15.0f, 2.2f, 0.0f);
                continue;
            }
        }

        // No kick: a touch. The ball is nudged to stay just ahead of the man
        // carrying it, which is what dribbling is when nobody is watching too
        // closely.
        float carry = 4.6f;
        w->ballVel.x = hx * carry;
        w->ballVel.z = hz * carry;
        if (w->ballVel.y < 0.0f) w->ballVel.y *= 0.4f;
        w->lastTouch = (int)i;
    }

    // The probe. A kick with fixed numbers, and a count of STEPS rather than
    // of seconds — the claim being tested is that a given number of steps
    // always produces the same ball, however many frames they were spread
    // across.
    if (CremaInputPressed(in, VPAD_BUTTON_Y))
        a->probeRequest = true;
    if (a->probeRequest) {
        a->probeRequest = false;
        w->ballPos.x = 0.0f; w->ballPos.y = BALL_R; w->ballPos.z = 0.0f;
        w->ballVel.x = 6.0f; w->ballVel.y = 7.5f; w->ballVel.z = -11.0f;
        w->ballSpin.x = 0.0f; w->ballSpin.y = 26.0f; w->ballSpin.z = 0.0f;
        w->ballRot = quat_identity();
        a->probeArmed = true;
        a->probeStep = 0;
    }

    ballStep(w, dt);

    bool hitFrame = false;
    int scored = ballGoals(w, &hitFrame);
    if (hitFrame && a->sfxPost)
        CremaAudioPlay(&a->sfxPost->sound, 0.75f, 1.0f);
    if (scored == 0 && live)
        ballOutOfPlay(w);
    if (scored != 0 && a->phase == PHASE_PLAY) {
        if (scored > 0) a->scoreBlue++; else a->scoreRed++;
        a->phase = PHASE_GOAL;
        a->phaseSteps = GOAL_STEPS;
        if (a->sfxWhistle) CremaAudioPlay(&a->sfxWhistle->sound, 0.55f, 1.0f);
        if (a->sfxCheer)   CremaAudioPlay(&a->sfxCheer->sound, 0.95f, 1.0f);
        WHBLogPrintf("[poc14] GOAL for %s - %u:%u at %.0f' (last touch: man %d)",
                     scored > 0 ? "BLUE" : "RED", a->scoreBlue, a->scoreRed,
                     a->matchTime / MATCH_LEN * 90.0f, w->lastTouch);
    }

    if (a->probeArmed) {
        a->probeStep++;
        if (a->probeStep == 240) {
            a->probeArmed = false;
            WHBLogPrintf("[poc14] PROBE %-6s at %.1f fps | after 240 steps: "
                         "pos %.5f %.5f %.5f | vel %.5f %.5f %.5f",
                         a->probeLabel ? a->probeLabel : "?", a->lastFps,
                         w->ballPos.x, w->ballPos.y, w->ballPos.z,
                         w->ballVel.x, w->ballVel.y, w->ballVel.z);
        }
    }
}

static void kickoffDraw(void *user)
{
    (void)user;

    CremaBlendSet(CREMA_BLEND_OPAQUE);
    CremaDepthSet(true, true);
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, TRUE);

    CremaShaderBind(app.shGround);
    GX2SetVertexUniformBlock(app.grGlobalVs, sizeof(Global), app.globalUbo);
    GX2SetPixelUniformBlock(app.grGlobalPs, sizeof(Global), app.globalUbo);
    GX2SetPixelTexture(&app.grass, app.grUnit);
    GX2SetPixelSampler(&app.grassSampler, app.grUnit);
    CremaMeshDraw(&app.ground, 1);

    // Blades before anything that stands in them: they are opaque, so the
    // order only matters for the depth buffer doing the occluding for free.
    // Culling off — half of fourteen thousand triangles face away from you.
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);
    CremaShaderBind(app.shGrass);
    GX2SetVertexUniformBlock(app.blGlobalVs, sizeof(Global), app.globalUbo);
    GX2SetPixelUniformBlock(app.blGlobalPs, sizeof(Global), app.globalUbo);
    CremaMeshDraw(&app.blades, 1);
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, TRUE);

    CremaShaderBind(app.shBrick);
    GX2SetVertexUniformBlock(app.brGlobalVs, sizeof(Global), app.globalUbo);
    GX2SetPixelUniformBlock(app.brGlobalPs, sizeof(Global), app.globalUbo);

    // The markings ride on the brick shader with an identity transform: they
    // are already in world space, so the instance block only has to say white.
    if (app.marks.ubo) {
        GX2SetVertexUniformBlock(app.brInst, sizeof(Brick) * app.marks.count,
                                 app.marks.ubo);
        CremaMeshDraw(&app.lines, app.marks.count);
    }

    batchDraw(&app.boxes);
    batchDraw(&app.spheres);
    batchDraw(&app.wedges);

    // --- the transparent pass -------------------------------------------
    //
    // Everything opaque is behind us. Depth test on, depth write off — the
    // rule crema_blend states, and this is the first example where it is
    // load-bearing rather than tidy: a net drawn with depth writes would
    // occlude the other net panel behind it and the goal would look solid.
    CremaBlendSet(CREMA_BLEND_ALPHA);
    CremaDepthSet(true, false);

    // Culling off for the net alone: you see the inside of the far panel
    // through the near one, and a single-sided net is a goal with a hole.
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);
    CremaShaderBind(app.shNet);
    GX2SetVertexUniformBlock(app.ntGlobalVs, sizeof(Global), app.globalUbo);
    GX2SetPixelUniformBlock(app.ntGlobalPs, sizeof(Global), app.globalUbo);
    GX2SetPixelTexture(&app.net, app.ntUnit);
    GX2SetPixelSampler(&app.netSampler, app.ntUnit);
    CremaMeshDraw(&app.nets, 1);
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, TRUE);

    if (app.shadows.ubo) {
        CremaShaderBind(app.shShadow);
        GX2SetVertexUniformBlock(app.sdGlobalVs, sizeof(Global), app.globalUbo);
        GX2SetVertexUniformBlock(app.sdInst, sizeof(Brick) * app.shadows.count,
                                 app.shadows.ubo);
        CremaMeshDraw(&app.quad, app.shadows.count);
    }

    CremaBlendSet(CREMA_BLEND_OPAQUE);
    CremaDepthSet(true, true);
}

// --- the readout -------------------------------------------------------------
//
// Two lists, both in flight in the same frame, which is the third time this
// codebase has hit the case crema_hud's interface was shaped around: the
// renderer owns the shader and the caller owns the memory, because a frame
// with two different readouts in it cannot share one buffer between them.
// PoC 11 found it, PoC 12 confirmed it, and here it arrives without anybody
// having to think about it — which is what a settled interface looks like.

static void buildTvHud(App *a)
{
    HudList *h = &a->tvHud;
    hudClear(h);

    // The score, hung off the middle of the screen so it does not move when
    // the numbers change width. Two shirt-coloured blocks and the goals
    // between them: no team names, because the colours already said it.
    const float cx = HUD_VIRTUAL_W * 0.5f;
    hudRect(h, cx - 132.0f, 22.0f, 264.0f, 54.0f, 0.05f, 0.07f, 0.10f, 0.62f);
    hudRect(h, cx - 128.0f, 26.0f, 44.0f, 46.0f, 0.16f, 0.34f, 0.88f, 0.95f);
    hudRect(h, cx + 84.0f, 26.0f, 44.0f, 46.0f, 0.88f, 0.20f, 0.18f, 0.95f);
    hudNumber(h, cx - 62.0f, 32.0f, 36.0f, a->scoreBlue, 1,
              0.96f, 0.97f, 1.0f, 1.0f);
    hudText(h, cx - 12.0f, 32.0f, 36.0f, "-", 0.70f, 0.74f, 0.80f, 1.0f);
    hudNumber(h, cx + 36.0f, 32.0f, 36.0f, a->scoreRed, 1,
              0.96f, 0.97f, 1.0f, 1.0f);

    // Ninety minutes squeezed into three, because a clock that reads 2:59 in
    // a football game reads as broken.
    uint32_t minute = (uint32_t)(a->matchTime / MATCH_LEN * 90.0f);
    float x = hudNumber(h, cx - 26.0f, 84.0f, 22.0f, minute, 2,
                        0.92f, 0.94f, 0.96f, 0.9f);
    hudText(h, x, 84.0f, 22.0f, "'", 0.92f, 0.94f, 0.96f, 0.9f);

    const char *banner = NULL;
    switch (a->phase) {
    case PHASE_SELFTEST: banner = "SELF TEST"; break;
    case PHASE_KICKOFF:  banner = "KICK OFF"; break;
    case PHASE_GOAL:     banner = "GOAL"; break;
    case PHASE_FULLTIME: banner = "FULL TIME"; break;
    default: break;
    }
    if (banner) {
        float size = a->phase == PHASE_GOAL ? 96.0f : 56.0f;
        float wide = (float)strlen(banner) * size * HUD_ADVANCE;
        hudText(h, cx - wide * 0.5f, HUD_VIRTUAL_H * 0.34f, size, banner,
                1.0f, 0.95f, 0.55f, 1.0f);
    }
    if (a->phase == PHASE_FULLTIME)
        hudText(h, cx - 150.0f, HUD_VIRTUAL_H * 0.34f + 80.0f, 26.0f,
                "PRESS A TO PLAY AGAIN", 0.90f, 0.92f, 0.95f, 0.9f);
    if (a->phase == PHASE_SELFTEST)
        hudText(h, cx - 268.0f, HUD_VIRTUAL_H * 0.34f + 74.0f, 22.0f,
                "SAME KICK AT 60 FPS AND UNDER LOAD - SEE THE LOG",
                0.86f, 0.90f, 0.94f, 0.85f);

    if (a->stress)
        hudText(h, 24.0f, HUD_VIRTUAL_H - 44.0f, 24.0f, "CPU LOAD",
                1.0f, 0.45f, 0.30f, 0.95f);
}

// The GamePad, and it is not the television again. A tactical plan of the
// pitch: nine blips and nothing else, glanceable, which is the Star Fox Zero
// lesson this project decided on in its second session and has kept.
static void buildPadHud(App *a)
{
    HudList *h = &a->padHud;
    hudClear(h);

    const float x0 = 140.0f, y0 = 96.0f;
    const float w = HUD_VIRTUAL_W - x0 * 2.0f;
    const float d = w * (PITCH_Z / PITCH_X);

    hudRect(h, x0 - 10.0f, y0 - 10.0f, w + 20.0f, d + 20.0f,
            0.08f, 0.26f, 0.10f, 1.0f);
    hudFrame(h, x0, y0, w, d, 3.0f, 0.85f, 0.88f, 0.92f, 0.9f);
    hudRect(h, x0 + w * 0.5f - 1.5f, y0, 3.0f, d, 0.85f, 0.88f, 0.92f, 0.9f);

    for (int s = -1; s <= 1; s += 2) {
        float gx = s < 0 ? x0 - 6.0f : x0 + w - 6.0f;
        hudRect(h, gx, y0 + d * 0.5f - 22.0f, 12.0f, 44.0f,
                0.95f, 0.95f, 0.98f, 0.9f);
    }

    for (uint32_t i = 0; i < a->view.manCount; i++) {
        const Figure *f = &a->view.men[i].look;
        float px = x0 + (f->pos.x + PITCH_X) / (PITCH_X * 2.0f) * w;
        float py = y0 + (f->pos.z + PITCH_Z) / (PITCH_Z * 2.0f) * d;
        bool mine = (int)i == a->view.controlled;
        float size = mine ? 20.0f : 14.0f;
        hudRect(h, px - size * 0.5f, py - size * 0.5f, size, size,
                f->tint[0], f->tint[1], f->tint[2], 1.0f);
        if (mine)
            hudFrame(h, px - 14.0f, py - 14.0f, 28.0f, 28.0f, 3.0f,
                     1.0f, 0.92f, 0.30f, 1.0f);
    }

    float bx = x0 + (a->view.ballPos.x + PITCH_X) / (PITCH_X * 2.0f) * w;
    float by = y0 + (a->view.ballPos.z + PITCH_Z) / (PITCH_Z * 2.0f) * d;
    hudRect(h, bx - 6.0f, by - 6.0f, 12.0f, 12.0f, 1.0f, 1.0f, 1.0f, 1.0f);

    hudText(h, x0, y0 + d + 26.0f, 22.0f, "L STICK RUNS   A PASS   B SHOOT",
            0.80f, 0.84f, 0.90f, 0.85f);
    hudText(h, x0, y0 + d + 56.0f, 22.0f, "L R BEND IT    Y PROBE   ZL LOAD",
            0.80f, 0.84f, 0.90f, 0.85f);
}

// The frame, which is solid and therefore made of the same bricks as
// everybody's arms and legs. Six a goal, and the only asset a goal needs.
static void pushGoalFrames(void)
{
    static const float WHITE[3] = { 0.95f, 0.96f, 0.97f };
    for (int s = -1; s <= 1; s += 2) {
        float side = (float)s;
        float gx = PITCH_X * side;
        float bx = gx + GOAL_DEPTH * side;
        for (int k = 0; k < 2; k++) {
            float pz = k ? GOAL_HALF_Z : -GOAL_HALF_Z;
            brickPush(&app.boxes,
                      mat4_mul(mat4_translate(gx, GOAL_H * 0.5f, pz),
                               mat4_scale(POST_W, GOAL_H, POST_W)),
                      WHITE, 0.0f);
            brickPush(&app.boxes,
                      mat4_mul(mat4_translate(bx, GOAL_H * 0.5f, pz),
                               mat4_scale(POST_W, GOAL_H, POST_W)),
                      WHITE, 0.0f);
        }
        brickPush(&app.boxes,
                  mat4_mul(mat4_translate(gx, GOAL_H, 0.0f),
                           mat4_scale(POST_W, POST_W,
                                      GOAL_HALF_Z * 2.0f + POST_W)),
                  WHITE, 0.0f);
        brickPush(&app.boxes,
                  mat4_mul(mat4_translate(bx, GOAL_H, 0.0f),
                           mat4_scale(POST_W, POST_W,
                                      GOAL_HALF_Z * 2.0f + POST_W)),
                  WHITE, 0.0f);
    }
}

// A disc on the grass, sized and faded by how far above it its owner is. The
// numbers are the whole of the trick: a shadow that does not grow reads as a
// sticker, and one that does not fade reads as a hole.
static void pushShadow(Vec3 at, float radius, float strength)
{
    float h = at.y;
    if (h < 0.0f) h = 0.0f;
    float grow = 1.0f + h * 0.22f;
    float fade = 1.0f - h / 9.0f;
    if (fade < 0.10f) fade = 0.10f;
    static const float DARK[3] = { 0.02f, 0.05f, 0.02f };
    Mat4 m = mat4_mul(mat4_translate(at.x, 0.02f, at.z),
                      mat4_scale(radius * 2.0f * grow, 1.0f,
                                 radius * 2.0f * grow));
    brickPush(&app.shadows, m, DARK, strength * fade);
}

// The ring under whoever the pad is holding. Negative alpha, see the shader.
static void pushMarker(Vec3 at, const float tint[3])
{
    Mat4 m = mat4_mul(mat4_translate(at.x, 0.03f, at.z),
                      mat4_scale(1.5f, 1.0f, 1.5f));
    brickPush(&app.shadows, m, tint, -0.85f);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (!CremaAppInit("poc14-kickoff"))
        return -1;
    // First, and it is a rule with a reason: until a title takes AX over, the
    // console keeps playing whatever it was handed on the way in, which is the
    // Wii U Menu's music.
    CremaAudioInit();
    if (!CremaShaderInitCompiler()) {
        CremaAppShutdown();
        return -1;
    }
    memset(&app, 0, sizeof(app));

    bool ok = false;
    CremaPak pak;
    if (CremaPakOpen(&pak, "/vol/content/assets.cpak")) {
        size_t bs = 0, ss = 0, ws = 0, gs = 0, ns = 0, fs = 0;
        const void *boxBlob    = CremaPakFind(&pak, "box.cmesh", &bs);
        const void *sphereBlob = CremaPakFind(&pak, "sphere.cmesh", &ss);
        const void *wedgeBlob  = CremaPakFind(&pak, "wedge.cmesh", &ws);
        const void *grassBlob  = CremaPakFind(&pak, "grass.ctex", &gs);
        const void *netBlob    = CremaPakFind(&pak, "net.ctex", &ns);
        const void *fontBlob   = CremaPakFind(&pak, "font.ctex", &fs);
        ok = boxBlob && sphereBlob && wedgeBlob && grassBlob && netBlob &&
             fontBlob &&
             CremaTextureLoadFromMemory(&app.font, fontBlob, fs, "font.ctex") &&
             CremaMeshLoadFromMemory(&app.box, boxBlob, bs, "box.cmesh") &&
             CremaMeshLoadFromMemory(&app.sphere, sphereBlob, ss, "sphere.cmesh") &&
             CremaMeshLoadFromMemory(&app.wedge, wedgeBlob, ws, "wedge.cmesh") &&
             CremaTextureLoadFromMemory(&app.grass, grassBlob, gs, "grass.ctex") &&
             CremaTextureLoadFromMemory(&app.net, netBlob, ns, "net.ctex");

        size_t as = 0;
        const void *bankBlob = CremaPakFind(&pak, "match.cbank", &as);
        if (ok && bankBlob)
            app.hasBank = CremaBankLoadFromMemory(&app.bank, bankBlob, as);
        CremaPakClose(&pak);
    }
    if (app.hasBank) {
        app.sfxKick    = CremaBankFind(&app.bank, "kick");
        app.sfxTap     = CremaBankFind(&app.bank, "tap");
        app.sfxWhistle = CremaBankFind(&app.bank, "whistle");
        app.sfxPost    = CremaBankFind(&app.bank, "post");
        app.sfxCheer   = CremaBankFind(&app.bank, "cheer");
        CremaAudioSetHeadroom(0.55f);
    }
    if (!ok) {
        WHBLogPrintf("[poc14] asset load failed - is the content dir bundled?");
        CremaShaderShutdownCompiler();
        CremaAppShutdown();
        return -1;
    }

    app.shBrick  = CremaShaderCompile(VS_BRICK, PS_BRICK,
                                      app.box.attribs, app.box.attribCount);
    app.shGround = CremaShaderCompile(VS_GROUND, PS_GROUND,
                                      app.box.attribs, app.box.attribCount);
    app.shShadow = CremaShaderCompile(VS_SHADOW, PS_SHADOW,
                                      app.box.attribs, app.box.attribCount);
    app.shNet    = CremaShaderCompile(VS_NET, PS_NET,
                                      app.box.attribs, app.box.attribCount);
    app.shGrass  = CremaShaderCompile(VS_GRASS, PS_GRASS,
                                      app.box.attribs, app.box.attribCount);
    if (!app.shBrick || !app.shGround || !app.shShadow || !app.shNet ||
        !app.shGrass) {
        WHBLogPrintf("[poc14] shader setup failed");
        CremaShaderShutdownCompiler();
        CremaAppShutdown();
        return -1;
    }

    app.brGlobalVs = CremaShaderVSBlockLocation(app.shBrick, "Global");
    app.brGlobalPs = CremaShaderPSBlockLocation(app.shBrick, "Global");
    app.brInst     = CremaShaderVSBlockLocation(app.shBrick, "Instances");
    if (app.brGlobalVs < 0) app.brGlobalVs = 0;
    if (app.brGlobalPs < 0) app.brGlobalPs = 0;
    if (app.brInst     < 0) app.brInst     = 1;
    app.grGlobalVs = CremaShaderVSBlockLocation(app.shGround, "Global");
    app.grGlobalPs = CremaShaderPSBlockLocation(app.shGround, "Global");
    if (app.grGlobalVs < 0) app.grGlobalVs = 0;
    if (app.grGlobalPs < 0) app.grGlobalPs = 0;
    if (app.shGround->ps->samplerVarCount > 0)
        app.grUnit = app.shGround->ps->samplerVars[0].location;
    app.sdGlobalVs = CremaShaderVSBlockLocation(app.shShadow, "Global");
    app.sdInst     = CremaShaderVSBlockLocation(app.shShadow, "Instances");
    if (app.sdGlobalVs < 0) app.sdGlobalVs = 0;
    if (app.sdInst     < 0) app.sdInst     = 1;
    app.ntGlobalVs = CremaShaderVSBlockLocation(app.shNet, "Global");
    app.ntGlobalPs = CremaShaderPSBlockLocation(app.shNet, "Global");
    if (app.ntGlobalVs < 0) app.ntGlobalVs = 0;
    if (app.ntGlobalPs < 0) app.ntGlobalPs = 0;
    if (app.shNet->ps->samplerVarCount > 0)
        app.ntUnit = app.shNet->ps->samplerVars[0].location;
    app.blGlobalVs = CremaShaderVSBlockLocation(app.shGrass, "Global");
    app.blGlobalPs = CremaShaderPSBlockLocation(app.shGrass, "Global");
    if (app.blGlobalVs < 0) app.blGlobalVs = 0;
    if (app.blGlobalPs < 0) app.blGlobalPs = 0;

    CremaSamplerInitTrilinear(&app.grassSampler, GX2_TEX_CLAMP_MODE_WRAP);
    CremaSamplerInitTrilinear(&app.netSampler, GX2_TEX_CLAMP_MODE_WRAP);
    CremaSamplerInitBilinear(&app.fontSampler, GX2_TEX_CLAMP_MODE_CLAMP);
    if (!CremaHudRendererCreate(&app.hudRenderer)) {
        WHBLogPrintf("[poc14] hud renderer failed");
        return -1;
    }

    static MeshBuild build;
    buildGround(&build);
    if (!meshUpload(&build, &app.ground, &app.box)) {
        WHBLogPrintf("[poc14] ground buffer failed");
        return -1;
    }
    buildLines(&build);
    if (!meshUpload(&build, &app.lines, &app.box)) {
        WHBLogPrintf("[poc14] line buffer failed");
        return -1;
    }
    WHBLogPrintf("[poc14] pitch markings: %u verts, %u tris, built on the "
                 "console from six numbers", build.vertexCount,
                 build.indexCount / 3);
    buildUnitQuad(&build);
    if (!meshUpload(&build, &app.quad, &app.box)) {
        WHBLogPrintf("[poc14] shadow quad failed");
        return -1;
    }
    buildNets(&build);
    if (!meshUpload(&build, &app.nets, &app.box)) {
        WHBLogPrintf("[poc14] net buffer failed");
        return -1;
    }
    uint64_t grassT0 = OSGetSystemTime();
    if (!buildGrass(&app.blades, &app.box, &app.bladeCount)) {
        WHBLogPrintf("[poc14] grass buffer failed");
        return -1;
    }
    WHBLogPrintf("[poc14] grass: %u blades = %u triangles, %u KB, built in "
                 "%u us", app.bladeCount, app.blades.indexCount / 3,
                 (unsigned)(app.blades.vertexCount * sizeof(GVertex) / 1024),
                 (uint32_t)OSTicksToMicroseconds(OSGetSystemTime() - grassT0));

    batchInit(&app.boxes,   &app.box);
    batchInit(&app.spheres, &app.sphere);
    batchInit(&app.wedges,  &app.wedge);
    batchInit(&app.marks,   &app.lines);
    batchInit(&app.shadows, &app.quad);
    CremaUniformRingCreate(&app.globalRing, sizeof(Global),
                           CREMA_FRAMES_IN_FLIGHT);
    CremaUniformRingCreate(&app.tvHudRing, CREMA_HUD_BLOCK_BYTES,
                           CREMA_FRAMES_IN_FLIGHT);
    CremaUniformRingCreate(&app.padHudRing, CREMA_HUD_BLOCK_BYTES,
                           CREMA_FRAMES_IN_FLIGHT);

    CremaFrame frame;
    CremaFrameInit(&frame, CREMA_PACING_FENCED, 1);
    CremaFrameStats stats;
    CremaClock clock;
    CremaClockInit(&clock);
    CremaInput input;
    CremaInputInit(&input);

    // Two teams from one table, mirrored. The keeper of each side keeps his
    // own colour but darker, which is the cheapest possible way of saying
    // "this one has a different job" without a second shirt.
    static const float BLUE[3] = { 0.16f, 0.34f, 0.88f };
    static const float RED[3]  = { 0.88f, 0.20f, 0.18f };
    app.cur.manCount = TEAM_SIZE * 2;
    for (uint32_t i = 0; i < app.cur.manCount; i++) {
        Man *m = &app.cur.men[i];
        memset(m, 0, sizeof(*m));
        m->team = (uint8_t)(i / TEAM_SIZE);
        m->role = (uint8_t)(i % TEAM_SIZE);
        float side = m->team == 0 ? 1.0f : -1.0f;
        m->look.pos.x = HOME[m->role][0] * side;
        m->look.pos.z = HOME[m->role][1] * side;
        m->look.yaw = yawFromDir(m->team == 0 ? 1.0f : -1.0f, 0.0f);
        const float *shirt = m->team == 0 ? BLUE : RED;
        float dim = m->role == 0 ? 0.45f : 1.0f;
        m->look.tint[0] = shirt[0] * dim;
        m->look.tint[1] = shirt[1] * dim;
        m->look.tint[2] = shirt[2] * dim;
        if (m->role == 0) {          // the keeper gets a green sleeve of his own
            m->look.tint[1] += 0.30f;
        }
    }
    app.cur.controlled = -1;
    app.cur.lastTouch = -1;
    app.cur.ballPos.y = BALL_R;
    app.cur.ballRot = quat_identity();
    app.prev = app.cur;
    app.phase = PHASE_SELFTEST;

    WHBLogPrintf("[poc14] a fixed step at %d Hz under a 59.94 Hz frame. "
                 "L-stick runs, A passes, B shoots, L/R bend it, "
                 "Y re-runs the probe, ZL loads the CPU.", (int)SIM_HZ);

    while (CremaAppRunning()) {
        CremaClockTick(&clock);
        CremaInputPoll(&input);

        // Deliberate load, so the claim can be tested rather than asserted:
        // hold ZL and the frame rate falls through the floor while the ball
        // keeps exactly the flight it had.
        // The self-test, and the reason it is not a button. A claim that only
        // holds when somebody remembers to hold ZL at the right moment is a
        // claim nobody checks twice. So the same kick is fired twice on a
        // timer — once with the console idle, once with 26 ms burned out of
        // every frame — and the two log lines either match to five decimals
        // or the fixed step is a lie.
        if (app.framesTotal == 180) {
            app.probeRequest = true;
            app.probeLabel = "quiet";
        } else if (app.framesTotal == 480) {
            app.forceStress = true;
        } else if (app.framesTotal == 500) {
            app.probeRequest = true;
            app.probeLabel = "loaded";
        } else if (app.framesTotal == 700) {
            app.forceStress = false;
        } else if (app.framesTotal == 740 && app.phase == PHASE_SELFTEST) {
            worldKickoff(&app.cur);
            app.prev = app.cur;
            app.phase = PHASE_KICKOFF;
            app.phaseSteps = KICKOFF_STEPS;
        }

        app.stress = app.forceStress || CremaInputHeld(&input, VPAD_BUTTON_ZL);
        if (app.stress) {
            uint64_t until = OSGetSystemTime() +
                             OSMicrosecondsToTicks(26000);
            while (OSGetSystemTime() < until) { }
        }

        // --- the fixed step ------------------------------------------------
        //
        // Everything above this line runs once per frame. Everything below it
        // runs a whole number of times per second, forever, whatever the
        // frame rate is doing.
        app.accumulator += clock.dt;
        app.stepsThisFrame = 0;

        // An input edge belongs to exactly one step. `pressed` is computed
        // once per FRAME, and a frame that runs three steps would otherwise
        // hand the same button-down to all three — the ball gets kicked three
        // times, once, at random, depending on how late the frame was. The
        // fix is one line and the bug is invisible until it isn't.
        CremaInput stepInput = input;

        while (app.accumulator >= SIM_DT && app.stepsThisFrame < SIM_MAX_STEPS) {
            app.prev = app.cur;
            worldStep(&app, &stepInput, SIM_DT);
            stepInput.pressed = 0;
            stepInput.released = 0;
            app.accumulator -= SIM_DT;
            app.stepsThisFrame++;
        }
        if (app.accumulator >= SIM_DT) {
            // Never reached while CremaClockTick clamps dt to 50 ms, and kept
            // so that the day something changes that, the log says so.
            app.dropped += (uint32_t)(app.accumulator / SIM_DT);
            app.accumulator = 0.0f;
        }

        CremaAudioUpdate();

        app.alpha = app.accumulator / SIM_DT;
        app.stepsTotal += app.stepsThisFrame;
        app.framesTotal++;
        app.stepHistogram[app.stepsThisFrame < 4 ? app.stepsThisFrame : 4]++;

        // --- what is drawn is between two states, and is neither of them ----
        worldLerp(&app.prev, &app.cur, app.alpha, &app.view);

        // The camera chases the ball rather than the stick, and it does it on
        // frame time, not sim time — a camera filter is a property of the
        // picture, and putting it in the step would make it stutter at
        // exactly the moments it exists to smooth over.
        float want = app.view.ballPos.x;
        if (want < -PITCH_X) want = -PITCH_X;
        if (want > PITCH_X)  want = PITCH_X;
        app.camX += (want - app.camX) * (1.0f - expf(-2.6f * clock.dt));

        // The television camera: side on, high, and looking slightly across
        // itself, which is what stops a side-on view from feeling like a
        // security monitor.
        Vec3 eye = { app.camX * 0.70f, 8.4f, PITCH_Z + 10.5f };
        Vec3 at  = { app.camX * 0.94f, 1.0f, 0.0f };
        Vec3 up  = { 0.0f, 1.0f, 0.0f };
        Mat4 view = mat4_look_at(eye, at, up);
        Mat4 proj = mat4_perspective(38.0f * PI / 180.0f, 16.0f / 9.0f,
                                     0.5f, 220.0f);

        app.global.viewProj = mat4_mul(proj, view);
        app.global.lightDir[0] = 0.36f;
        app.global.lightDir[1] = -0.86f;
        app.global.lightDir[2] = 0.36f;
        app.global.camPos[0] = eye.x;
        app.global.camPos[1] = eye.y;
        app.global.camPos[2] = eye.z;
        app.global.fog[0] = 70.0f;
        app.global.fog[1] = 190.0f;
        app.global.fogColor[0] = 0.70f;
        app.global.fogColor[1] = 0.78f;
        app.global.fogColor[2] = 0.86f;
        // Only the wind clock. The offset that used to live in xy is gone —
        // see the note above VS_GRASS on why a jittered field cannot slide.
        app.global.grass[0] = 0.0f;
        app.global.grass[1] = 0.0f;
        app.global.grass[2] = 0.0f;
        app.global.grass[3] = clock.elapsed;

        brickReset(&app.boxes);
        brickReset(&app.spheres);
        brickReset(&app.wedges);
        brickReset(&app.marks);
        brickReset(&app.shadows);

        for (uint32_t i = 0; i < app.view.manCount; i++) {
            figureBuild(&app.view.men[i].look, &app.boxes, &app.spheres,
                        &app.wedges);
            pushShadow(app.view.men[i].look.pos, 0.42f, 0.40f);
        }
        if (app.view.controlled >= 0) {
            static const float MARK[3] = { 1.0f, 0.92f, 0.30f };
            pushMarker(app.view.men[app.view.controlled].look.pos, MARK);
        }

        pushGoalFrames();

        static const float WHITE[3] = { 0.94f, 0.95f, 0.96f };
        brickPush(&app.marks, mat4_identity(), WHITE, 0.0f);

        static const float BALL_TINT[3] = { 0.96f, 0.96f, 0.96f };
        Mat4 ballM = mat4_mul(mat4_translate(app.view.ballPos.x,
                                             app.view.ballPos.y,
                                             app.view.ballPos.z),
                              mat4_mul(quat_to_mat4(app.view.ballRot),
                                       mat4_scale(BALL_R * 2.0f, BALL_R * 2.0f,
                                                  BALL_R * 2.0f)));
        brickPush(&app.spheres, ballM, BALL_TINT, 1.0f);
        // Wider than the ball on purpose. A shadow exactly the size of the
        // thing casting it is a shadow the thing stands on top of and hides —
        // which is what happened, and it looked like the pass was broken.
        pushShadow(app.view.ballPos, BALL_R * 1.55f, 0.50f);

        uint32_t slot = CremaFrameBegin(&frame);
        app.globalUbo = CremaUniformRingStore(&app.globalRing, slot,
                                              &app.global, sizeof(Global));
        batchStore(&app.boxes,   slot);
        batchStore(&app.spheres, slot);
        batchStore(&app.wedges,  slot);
        batchStore(&app.marks,   slot);
        batchStore(&app.shadows, slot);

        buildTvHud(&app);
        buildPadHud(&app);
        app.tvHudUbo = CremaUniformRingStore(&app.tvHudRing, slot,
                                             app.tvHud.items,
                                             CREMA_HUD_BLOCK_BYTES);
        app.padHudUbo = CremaUniformRingStore(&app.padHudRing, slot,
                                              app.padHud.items,
                                              CREMA_HUD_BLOCK_BYTES);

        // Not CremaFrameDrawBoth: the two screens do not show the same thing.
        // The television gets the match; the GamePad gets the plan of it and
        // no 3D pass at all, which is the only reason a second screen is
        // worth the frame it costs.
        static const float SKY[4] = { 0.62f, 0.74f, 0.88f, 1.0f };
        static const float TACTICAL[4] = { 0.05f, 0.09f, 0.07f, 1.0f };

        WHBGfxBeginRenderTV();
        WHBGfxClearColor(SKY[0], SKY[1], SKY[2], SKY[3]);
        kickoffDraw(&app);
        CremaHudDraw(&app.hudRenderer, app.tvHudUbo, app.tvHud.count,
                     &app.font, &app.fontSampler);
        WHBGfxFinishRenderTV();

        WHBGfxBeginRenderDRC();
        WHBGfxClearColor(TACTICAL[0], TACTICAL[1], TACTICAL[2], TACTICAL[3]);
        CremaHudDraw(&app.hudRenderer, app.padHudUbo, app.padHud.count,
                     &app.font, &app.fontSampler);
        WHBGfxFinishRenderDRC();

        CremaFrameEnd(&frame, &stats);

        if (stats.updated)
            app.lastFps = stats.fps;

        if (stats.updated) {
            WHBLogPrintf("[poc14] %.1f fps | sync %.2f ms | steps avg %.3f "
                         "(0:%u 1:%u 2:%u 3:%u 4+:%u) | alpha %.2f | "
                         "%u:%u | ball %.1f %.1f %.1f | man %d%s",
                         stats.fps, stats.drainMs,
                         app.framesTotal ? (double)app.stepsTotal /
                                           (double)app.framesTotal : 0.0,
                         app.stepHistogram[0], app.stepHistogram[1],
                         app.stepHistogram[2], app.stepHistogram[3],
                         app.stepHistogram[4], app.alpha,
                         app.scoreBlue, app.scoreRed,
                         app.cur.ballPos.x, app.cur.ballPos.y,
                         app.cur.ballPos.z, app.cur.controlled,
                         app.dropped ? " DROPPED STEPS" : "");
        }
    }

    CremaFrameSettle(&frame);
    CremaUniformRingDestroy(&app.globalRing);
    CremaUniformRingDestroy(&app.tvHudRing);
    CremaUniformRingDestroy(&app.padHudRing);
    CremaHudRendererDestroy(&app.hudRenderer);
    CremaTextureDestroy(&app.font);
    CremaUniformRingDestroy(&app.boxes.ring);
    CremaUniformRingDestroy(&app.spheres.ring);
    CremaUniformRingDestroy(&app.wedges.ring);
    CremaUniformRingDestroy(&app.marks.ring);
    CremaUniformRingDestroy(&app.shadows.ring);
    CremaMeshDestroy(&app.ground);
    CremaMeshDestroy(&app.lines);
    CremaMeshDestroy(&app.quad);
    CremaMeshDestroy(&app.nets);
    CremaMeshDestroy(&app.blades);
    CremaTextureDestroy(&app.net);
    CremaShaderFree(app.shNet);
    CremaShaderFree(app.shGrass);
    CremaMeshDestroy(&app.box);
    CremaMeshDestroy(&app.sphere);
    CremaMeshDestroy(&app.wedge);
    CremaTextureDestroy(&app.grass);
    CremaShaderFree(app.shBrick);
    CremaShaderFree(app.shGround);
    CremaShaderFree(app.shShadow);
    CremaShaderShutdownCompiler();
    if (app.hasBank)
        CremaBankClose(&app.bank);
    CremaAudioShutdown();
    CremaAppShutdown();
    return 0;
}
