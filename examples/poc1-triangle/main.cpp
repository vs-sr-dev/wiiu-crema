// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan
//
// GX2 PoC 1 — a single interpolated-colour triangle on TV + GamePad.
// Pipeline: WHBGfx (scan/colour/depth buffers, context states) + CafeGLSL
// (runtime GLSL -> Latte) + GX2R vertex buffers (cache-safe lock/unlock).

#include <gx2/draw.h>
#include <gx2r/buffer.h>
#include <gx2r/draw.h>
#include <whb/gfx.h>
#include <string.h>

#include "crema_app.h"
#include "crema_shader.h"

static const char *VS_SRC = R"(
#version 450
layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec3 aColor;
layout(location = 0) out vec3 vColor;
void main()
{
    gl_Position = vec4(aPosition, 0.0, 1.0);
    vColor = aColor;
}
)";

static const char *PS_SRC = R"(
#version 450
layout(location = 0) in vec3 vColor;
layout(location = 0) out vec4 oColor;
void main()
{
    oColor = vec4(vColor, 1.0);
}
)";

// interleaved: x, y, r, g, b
static const float VERTICES[] = {
     0.0f,  0.7f,   1.0f, 0.0f, 0.0f,
     0.7f, -0.7f,   0.0f, 1.0f, 0.0f,
    -0.7f, -0.7f,   0.0f, 0.0f, 1.0f,
};
#define VERTEX_STRIDE (5 * sizeof(float))
#define VERTEX_COUNT  3

static void drawScene(const CremaShader *shader, GX2RBuffer *vbo)
{
    CremaShaderBind(shader);
    GX2RSetAttributeBuffer(vbo, 0, VERTEX_STRIDE, 0);
    GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, VERTEX_COUNT, 0, 1);
}

int main(int argc, char **argv)
{
    if (!CremaAppInit("poc1-triangle"))
        return -1;

    if (!CremaShaderInitCompiler()) {
        CremaAppShutdown();
        return -1;
    }

    const CremaAttrib attribs[] = {
        { 0, 0, 0,                 GX2_ATTRIB_FORMAT_FLOAT_32_32 },
        { 1, 0, 2 * sizeof(float), GX2_ATTRIB_FORMAT_FLOAT_32_32_32 },
    };
    CremaShader *shader = CremaShaderCompile(VS_SRC, PS_SRC, attribs, 2);
    if (!shader) {
        CremaShaderShutdownCompiler();
        CremaAppShutdown();
        return -1;
    }

    GX2RBuffer vbo;
    memset(&vbo, 0, sizeof(vbo));
    vbo.flags = (GX2RResourceFlags)(GX2R_RESOURCE_BIND_VERTEX_BUFFER |
                                    GX2R_RESOURCE_USAGE_CPU_READ |
                                    GX2R_RESOURCE_USAGE_CPU_WRITE |
                                    GX2R_RESOURCE_USAGE_GPU_READ);
    vbo.elemSize  = VERTEX_STRIDE;
    vbo.elemCount = VERTEX_COUNT;
    GX2RCreateBuffer(&vbo);
    void *p = GX2RLockBufferEx(&vbo, (GX2RResourceFlags)0);
    memcpy(p, VERTICES, sizeof(VERTICES));
    GX2RUnlockBufferEx(&vbo, (GX2RResourceFlags)0);

    while (CremaAppRunning()) {
        WHBGfxBeginRender();

        WHBGfxBeginRenderTV();
        WHBGfxClearColor(0.05f, 0.05f, 0.15f, 1.0f);
        drawScene(shader, &vbo);
        WHBGfxFinishRenderTV();

        WHBGfxBeginRenderDRC();
        WHBGfxClearColor(0.05f, 0.05f, 0.15f, 1.0f);
        drawScene(shader, &vbo);
        WHBGfxFinishRenderDRC();

        CremaFinishRenderAndMark(NULL);
    }

    GX2RDestroyBufferEx(&vbo, (GX2RResourceFlags)0);
    CremaShaderFree(shader);
    CremaShaderShutdownCompiler();
    CremaAppShutdown();
    return 0;
}
