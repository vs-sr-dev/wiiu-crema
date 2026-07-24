// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Samuele Voltan
//
// GX2 PoC 2 — rotating wireframe cube. New pieces over PoC 1:
//   - mat4 MVP in a uniform block (byteswapped for the GPU, see CremaUniformStore)
//   - indexed drawing (GX2RDrawIndexed, U16 indices) with LINES primitives
//   - per-frame animation timed with OSGetSystemTime

#include <gx2/draw.h>
#include <gx2/registers.h>
#include <gx2/shaders.h>
#include <gx2r/buffer.h>
#include <gx2r/draw.h>
#include <whb/gfx.h>
#include <coreinit/time.h>
#include <string.h>

#include "crema_app.h"
#include "crema_shader.h"
#include "crema_matrix.h"

static const char *VS_SRC = R"(
#version 450
layout(location = 0) in vec3 aPosition;
layout(binding = 0) uniform Transform {
    mat4 uMVP;
};
void main()
{
    gl_Position = uMVP * vec4(aPosition, 1.0);
}
)";

static const char *PS_SRC = R"(
#version 450
layout(location = 0) out vec4 oColor;
void main()
{
    oColor = vec4(0.2, 1.0, 0.4, 1.0);
}
)";

static const float CUBE_VERTS[] = {
    -1, -1, -1,   1, -1, -1,   1,  1, -1,  -1,  1, -1,   // back
    -1, -1,  1,   1, -1,  1,   1,  1,  1,  -1,  1,  1,   // front
};

static const uint16_t CUBE_EDGES[] = {
    0,1, 1,2, 2,3, 3,0,   // back face
    4,5, 5,6, 6,7, 7,4,   // front face
    0,4, 1,5, 2,6, 3,7,   // connecting edges
};
#define EDGE_INDEX_COUNT (sizeof(CUBE_EDGES) / sizeof(CUBE_EDGES[0]))

static void drawScene(const CremaShader *shader, GX2RBuffer *vbo, GX2RBuffer *ibo,
                      int32_t blockLoc, const void *ubo, size_t uboSize)
{
    CremaShaderBind(shader);
    // Wireframe: no depth test, no culling, chunky lines.
    GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_LESS);
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);
    GX2SetLineWidth(3.0f);
    GX2SetVertexUniformBlock(blockLoc, uboSize, ubo);
    GX2RSetAttributeBuffer(vbo, 0, 3 * sizeof(float), 0);
    GX2RDrawIndexed(GX2_PRIMITIVE_MODE_LINES, ibo, GX2_INDEX_TYPE_U16,
                    EDGE_INDEX_COUNT, 0, 0, 1);
}

int main(int argc, char **argv)
{
    if (!CremaAppInit("poc2-wirecube"))
        return -1;
    if (!CremaShaderInitCompiler()) {
        CremaAppShutdown();
        return -1;
    }

    const CremaAttrib attribs[] = {
        { 0, 0, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32_32 },
    };
    CremaShader *shader = CremaShaderCompile(VS_SRC, PS_SRC, attribs, 1);
    if (!shader) {
        CremaShaderShutdownCompiler();
        CremaAppShutdown();
        return -1;
    }
    int32_t blockLoc = CremaShaderVSBlockLocation(shader, "Transform");
    if (blockLoc < 0)
        blockLoc = 0;

    GX2RBuffer vbo;
    memset(&vbo, 0, sizeof(vbo));
    vbo.flags = (GX2RResourceFlags)(GX2R_RESOURCE_BIND_VERTEX_BUFFER |
                                    GX2R_RESOURCE_USAGE_CPU_WRITE |
                                    GX2R_RESOURCE_USAGE_GPU_READ);
    vbo.elemSize  = 3 * sizeof(float);
    vbo.elemCount = 8;
    GX2RCreateBuffer(&vbo);
    void *p = GX2RLockBufferEx(&vbo, (GX2RResourceFlags)0);
    memcpy(p, CUBE_VERTS, sizeof(CUBE_VERTS));
    GX2RUnlockBufferEx(&vbo, (GX2RResourceFlags)0);

    GX2RBuffer ibo;
    memset(&ibo, 0, sizeof(ibo));
    ibo.flags = (GX2RResourceFlags)(GX2R_RESOURCE_BIND_INDEX_BUFFER |
                                    GX2R_RESOURCE_USAGE_CPU_WRITE |
                                    GX2R_RESOURCE_USAGE_GPU_READ);
    ibo.elemSize  = sizeof(uint16_t);
    ibo.elemCount = EDGE_INDEX_COUNT;
    GX2RCreateBuffer(&ibo);
    p = GX2RLockBufferEx(&ibo, (GX2RResourceFlags)0);
    memcpy(p, CUBE_EDGES, sizeof(CUBE_EDGES));
    GX2RUnlockBufferEx(&ibo, (GX2RResourceFlags)0);

    Mat4 *ubo = (Mat4 *)CremaUniformAlloc(sizeof(Mat4));

    Mat4 proj = mat4_perspective(60.0f * 3.14159265f / 180.0f,
                                 16.0f / 9.0f, 0.1f, 100.0f);
    Mat4 view = mat4_translate(0.0f, 0.0f, -5.0f);

    uint64_t t0 = OSGetSystemTime();
    while (CremaAppRunning()) {
        float t = (float)((double)OSTicksToMilliseconds(OSGetSystemTime() - t0) / 1000.0);
        Mat4 model = mat4_mul(mat4_rotate_y(t * 0.9f), mat4_rotate_x(t * 0.5f));
        Mat4 mvp = mat4_mul(proj, mat4_mul(view, model));
        // Safe to rewrite in place each frame: WHBGfxFinishRender does a full
        // GX2DrawDone, so the GPU is idle by the time we get here.
        CremaUniformStore(ubo, &mvp, sizeof(Mat4));

        WHBGfxBeginRender();

        WHBGfxBeginRenderTV();
        WHBGfxClearColor(0.02f, 0.02f, 0.08f, 1.0f);
        drawScene(shader, &vbo, &ibo, blockLoc, ubo, sizeof(Mat4));
        WHBGfxFinishRenderTV();

        WHBGfxBeginRenderDRC();
        WHBGfxClearColor(0.02f, 0.02f, 0.08f, 1.0f);
        drawScene(shader, &vbo, &ibo, blockLoc, ubo, sizeof(Mat4));
        WHBGfxFinishRenderDRC();

        CremaFinishRenderAndMark(NULL);
    }

    CremaUniformFreeBlock(ubo);
    GX2RDestroyBufferEx(&ibo, (GX2RResourceFlags)0);
    GX2RDestroyBufferEx(&vbo, (GX2RResourceFlags)0);
    CremaShaderFree(shader);
    CremaShaderShutdownCompiler();
    CremaAppShutdown();
    return 0;
}
