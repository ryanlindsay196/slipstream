// raster.h - a from-scratch software rasterizer.
//
// Pipeline per mesh: transform -> trivial reject -> homogeneous clipping
// (Sutherland-Hodgman, only for triangles that need it) -> back-face cull ->
// integer edge-function rasterization with 4 bits of sub-pixel precision ->
// perspective-correct interpolation of UV and colour -> 1/w depth test -> fog.
#pragma once

#include "core/base.h"
#include "render/math3d.h"

namespace ss {

struct Arena;

struct Texture {
    const u32* pixels;  // 0x00RRGGBB
    int        sizeLog2;  // square, power-of-two
};

struct Vertex {
    Vec3  pos;
    float u, v;
    u32   color;  // baked lighting * albedo
};

enum MeshFlags : u8 {
    Mesh_DoubleSided = 1 << 0,
    Mesh_Darken      = 1 << 1,  // multiply destination by 1/2, no depth write (blob shadows)
    Mesh_NoFog       = 1 << 2,
};

struct Mesh {
    const Vertex* verts;
    const u16*    indices;
    int           vertexCount;
    int           indexCount;
    const Texture* texture;  // nullptr = vertex colour only
    u8            flags;
};

struct RenderTarget {
    u32*   color;
    float* depth;  // stores 1/w; 0 = infinitely far
    int    width;
    int    height;
};

struct RasterStats {
    u32 meshes;
    u32 trianglesIn;
    u32 trianglesClipped;
    u32 trianglesDrawn;
};

struct RenderContext {
    RenderTarget target;
    Mat4         viewProj;
    u32          fogColor;
    float        fogStart;
    float        fogEnd;
    Arena*       scratch;
    RasterStats  stats;
};

void ClearDepth(RenderTarget& rt);
void DrawMesh(RenderContext& rc, const Mesh& mesh, const Mat4& model, u32 tint = 0xFFFFFF);

inline u32 Rgb(int r, int g, int b) { return ((u32)r << 16) | ((u32)g << 8) | (u32)b; }
u32 MulColor(u32 c, float s);       // scale, clamped
u32 LerpColor(u32 a, u32 b, float t);

}  // namespace ss
