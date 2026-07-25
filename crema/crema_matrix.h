// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan
//
// Minimal 4x4 float matrix / vec math for the GX2 PoCs.
// Matrices are column-major (GLSL convention: m[col][row]), so they can be
// uploaded straight into a `uniform mat4` block without transposing.

#pragma once
#include <math.h>
#include <stdbool.h>

typedef struct { float m[4][4]; } Mat4;   // m[col][row]
typedef struct { float x, y, z; } Vec3;

static inline Mat4 mat4_identity(void)
{
    Mat4 r = {{{0}}};
    r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.0f;
    return r;
}

static inline Mat4 mat4_mul(Mat4 a, Mat4 b)
{
    Mat4 r;
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++)
            r.m[c][row] = a.m[0][row] * b.m[c][0]
                        + a.m[1][row] * b.m[c][1]
                        + a.m[2][row] * b.m[c][2]
                        + a.m[3][row] * b.m[c][3];
    return r;
}

static inline Mat4 mat4_translate(float x, float y, float z)
{
    Mat4 r = mat4_identity();
    r.m[3][0] = x; r.m[3][1] = y; r.m[3][2] = z;
    return r;
}

static inline Mat4 mat4_rotate_x(float rad)
{
    Mat4 r = mat4_identity();
    float c = cosf(rad), s = sinf(rad);
    r.m[1][1] = c; r.m[2][1] = -s;
    r.m[1][2] = s; r.m[2][2] = c;
    return r;
}

static inline Mat4 mat4_rotate_y(float rad)
{
    Mat4 r = mat4_identity();
    float c = cosf(rad), s = sinf(rad);
    r.m[0][0] = c;  r.m[2][0] = s;
    r.m[0][2] = -s; r.m[2][2] = c;
    return r;
}

static inline Mat4 mat4_rotate_z(float rad)
{
    Mat4 r = mat4_identity();
    float c = cosf(rad), s = sinf(rad);
    r.m[0][0] = c; r.m[1][0] = -s;
    r.m[0][1] = s; r.m[1][1] = c;
    return r;
}

static inline Mat4 mat4_scale(float x, float y, float z)
{
    Mat4 r = mat4_identity();
    r.m[0][0] = x; r.m[1][1] = y; r.m[2][2] = z;
    return r;
}

// Right-handed perspective, depth mapped to [-1, 1] (GL convention — GX2's
// default depth range is [0,1] but the viewport transform handles it; we
// pair this with GX2SetViewport's default zNear/zFar).
static inline Mat4 mat4_perspective(float fovy_rad, float aspect, float znear, float zfar)
{
    Mat4 r = {{{0}}};
    float f = 1.0f / tanf(fovy_rad * 0.5f);
    r.m[0][0] = f / aspect;
    r.m[1][1] = f;
    r.m[2][2] = (zfar + znear) / (znear - zfar);
    r.m[2][3] = -1.0f;
    r.m[3][2] = (2.0f * zfar * znear) / (znear - zfar);
    return r;
}

static inline Vec3 vec3_sub(Vec3 a, Vec3 b) { Vec3 r = { a.x-b.x, a.y-b.y, a.z-b.z }; return r; }
static inline Vec3 vec3_cross(Vec3 a, Vec3 b)
{
    Vec3 r = { a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
    return r;
}
static inline float vec3_dot(Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline Vec3 vec3_normalize(Vec3 v)
{
    float len = sqrtf(vec3_dot(v, v));
    if (len < 1e-8f) { Vec3 z = {0,0,0}; return z; }
    Vec3 r = { v.x/len, v.y/len, v.z/len };
    return r;
}

// World point -> 2D screen position, the same transform the vertex shader does,
// done once on the CPU so a HUD marker can sit on top of a thing in the world.
// Returns false when the point is behind the camera: w <= 0 flips the divide
// and would paint the marker for something you cannot see, mirrored, in front
// of you. That check is the whole reason this is not two lines inline.
static inline bool mat4_project(Mat4 viewProj, Vec3 p, float screenW,
                                float screenH, float *outX, float *outY)
{
    float x = viewProj.m[0][0]*p.x + viewProj.m[1][0]*p.y + viewProj.m[2][0]*p.z + viewProj.m[3][0];
    float y = viewProj.m[0][1]*p.x + viewProj.m[1][1]*p.y + viewProj.m[2][1]*p.z + viewProj.m[3][1];
    float w = viewProj.m[0][3]*p.x + viewProj.m[1][3]*p.y + viewProj.m[2][3]*p.z + viewProj.m[3][3];
    if (w <= 0.0001f)
        return false;
    *outX = (x / w * 0.5f + 0.5f) * screenW;
    *outY = (0.5f - y / w * 0.5f) * screenH;   // screen y grows downwards
    return true;
}

static inline Mat4 mat4_look_at(Vec3 eye, Vec3 center, Vec3 up)
{
    Vec3 f = vec3_normalize(vec3_sub(center, eye));
    Vec3 s = vec3_normalize(vec3_cross(f, up));
    Vec3 u = vec3_cross(s, f);
    Mat4 r = mat4_identity();
    r.m[0][0] = s.x;  r.m[1][0] = s.y;  r.m[2][0] = s.z;
    r.m[0][1] = u.x;  r.m[1][1] = u.y;  r.m[2][1] = u.z;
    r.m[0][2] = -f.x; r.m[1][2] = -f.y; r.m[2][2] = -f.z;
    r.m[3][0] = -vec3_dot(s, eye);
    r.m[3][1] = -vec3_dot(u, eye);
    r.m[3][2] =  vec3_dot(f, eye);
    return r;
}
