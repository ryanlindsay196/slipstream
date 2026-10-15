#include "render/raster.h"
#include "core/memory.h"

namespace ss {

namespace {

constexpr float kNear = 0.1f;

struct ClipVert {
    float x, y, z, w;
    float u, v;
    float r, g, b;
};

ClipVert LerpVert(const ClipVert& a, const ClipVert& b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t,
            a.u + (b.u - a.u) * t, a.v + (b.v - a.v) * t, a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
            a.b + (b.b - a.b) * t};
}

// Signed distance to each clip plane; >= 0 is inside.
inline float PlaneDist(const ClipVert& v, int plane) {
    switch (plane) {
        case 0:  return v.z - kNear;
        case 1:  return v.w + v.x;
        case 2:  return v.w - v.x;
        case 3:  return v.w + v.y;
        default: return v.w - v.y;
    }
}

inline u32 Outcode(const ClipVert& v) {
    u32 code = 0;
    for (int p = 0; p < 5; ++p)
        if (PlaneDist(v, p) < 0) code |= 1u << p;
    return code;
}

struct ScreenVert {
    i32   x, y;  // 28.4 fixed point
    float invW;
    float uw, vw, rw, gw, bw;  // attributes premultiplied by 1/w
};

ScreenVert ToScreen(const ClipVert& c, int width, int height) {
    float      invW = 1.0f / c.w;
    float      sx   = (c.x * invW * 0.5f + 0.5f) * (float)width;
    float      sy   = (0.5f - c.y * invW * 0.5f) * (float)height;
    ScreenVert s;
    s.x    = (i32)std::lround(sx * 16.0f);
    s.y    = (i32)std::lround(sy * 16.0f);
    s.invW = invW;
    s.uw   = c.u * invW;
    s.vw   = c.v * invW;
    s.rw   = c.r * invW;
    s.gw   = c.g * invW;
    s.bw   = c.b * invW;
    return s;
}

inline i32 Edge(const ScreenVert& a, const ScreenVert& b, i32 px, i32 py) {
    return (i32)((i64)(b.x - a.x) * (py - a.y) - (i64)(b.y - a.y) * (px - a.x));
}

void RasterTriangle(RenderContext& rc, const Mesh& mesh, ScreenVert v0, ScreenVert v1, ScreenVert v2) {
    RenderTarget& rt   = rc.target;
    i64           area = (i64)(v1.x - v0.x) * (v2.y - v0.y) - (i64)(v1.y - v0.y) * (v2.x - v0.x);
    if (area == 0) return;
    if (area < 0) {
        if (!(mesh.flags & Mesh_DoubleSided)) return;  // back-facing
        ScreenVert t = v1;
        v1           = v2;
        v2           = t;
        area         = -area;
    }
    ++rc.stats.trianglesDrawn;

    // Pixel-centre bounding box, clamped to the target.
    int minX = Max(0, (Min(v0.x, Min(v1.x, v2.x)) - 8 + 15) >> 4);
    int maxX = Min(rt.width - 1, (Max(v0.x, Max(v1.x, v2.x)) - 8) >> 4);
    int minY = Max(0, (Min(v0.y, Min(v1.y, v2.y)) - 8 + 15) >> 4);
    int maxY = Min(rt.height - 1, (Max(v0.y, Max(v1.y, v2.y)) - 8) >> 4);
    if (minX > maxX || minY > maxY) return;

    // Edge-function steps per pixel (16 sub-pixel units).
    i32 a0 = -(v2.y - v1.y) * 16, b0 = (v2.x - v1.x) * 16;
    i32 a1 = -(v0.y - v2.y) * 16, b1 = (v0.x - v2.x) * 16;
    i32 a2 = -(v1.y - v0.y) * 16, b2 = (v1.x - v0.x) * 16;

    i32 px = minX * 16 + 8, py = minY * 16 + 8;
    i32 w0Row = Edge(v1, v2, px, py);
    i32 w1Row = Edge(v2, v0, px, py);
    i32 w2Row = Edge(v0, v1, px, py);

    // Barycentric weights l1, l2 (of v1, v2) are linear in screen space.
    float invArea = 1.0f / (float)area;
    float dl1dx = (float)a1 * invArea, dl1dy = (float)b1 * invArea;
    float dl2dx = (float)a2 * invArea, dl2dy = (float)b2 * invArea;
    float l1Row = (float)w1Row * invArea, l2Row = (float)w2Row * invArea;

    float dInvW1 = v1.invW - v0.invW, dInvW2 = v2.invW - v0.invW;
    float dU1 = v1.uw - v0.uw, dU2 = v2.uw - v0.uw;
    float dV1 = v1.vw - v0.vw, dV2 = v2.vw - v0.vw;
    float dR1 = v1.rw - v0.rw, dR2 = v2.rw - v0.rw;
    float dG1 = v1.gw - v0.gw, dG2 = v2.gw - v0.gw;
    float dB1 = v1.bw - v0.bw, dB2 = v2.bw - v0.bw;

    const Texture* tex    = mesh.texture;
    int            tsize  = tex ? 1 << tex->sizeLog2 : 0;
    int            tmask  = tsize - 1;
    float          tscale = (float)tsize;
    bool           darken = (mesh.flags & Mesh_Darken) != 0;
    bool           fog    = !(mesh.flags & Mesh_NoFog);
    float          fogScale = 1.0f / (rc.fogEnd - rc.fogStart);
    float          fogR = (float)((rc.fogColor >> 16) & 0xFF), fogG = (float)((rc.fogColor >> 8) & 0xFF),
          fogB = (float)(rc.fogColor & 0xFF);

    for (int y = minY; y <= maxY; ++y) {
        i32   w0 = w0Row, w1 = w1Row, w2 = w2Row;
        float l1 = l1Row, l2 = l2Row;
        u32*   colorRow = rt.color + y * rt.width;
        float* depthRow = rt.depth + y * rt.width;
        for (int x = minX; x <= maxX; ++x) {
            if ((w0 | w1 | w2) >= 0) {
                float invW = v0.invW + l1 * dInvW1 + l2 * dInvW2;
                if (invW > depthRow[x]) {
                    if (darken) {
                        colorRow[x] = (colorRow[x] >> 1) & 0x7F7F7F;
                    } else {
                        depthRow[x] = invW;
                        float w     = 1.0f / invW;
                        float r = (v0.rw + l1 * dR1 + l2 * dR2) * w;
                        float g = (v0.gw + l1 * dG1 + l2 * dG2) * w;
                        float b = (v0.bw + l1 * dB1 + l2 * dB2) * w;
                        if (tex) {
                            float u  = (v0.uw + l1 * dU1 + l2 * dU2) * w;
                            float vv = (v0.vw + l1 * dV1 + l2 * dV2) * w;
                            int   tu = (int)((u + 1024.0f) * tscale) & tmask;
                            int   tv = (int)((vv + 1024.0f) * tscale) & tmask;
                            u32   t  = tex->pixels[(tv << tex->sizeLog2) + tu];
                            r *= (float)((t >> 16) & 0xFF) * (1.0f / 255.0f);
                            g *= (float)((t >> 8) & 0xFF) * (1.0f / 255.0f);
                            b *= (float)(t & 0xFF) * (1.0f / 255.0f);
                        }
                        if (fog) {
                            float f = (w - rc.fogStart) * fogScale;
                            f       = f < 0 ? 0 : (f > 1 ? 1 : f);
                            r += (fogR - r) * f;
                            g += (fogG - g) * f;
                            b += (fogB - b) * f;
                        }
                        int ir = (int)r, ig = (int)g, ib = (int)b;
                        ir = ir > 255 ? 255 : ir;
                        ig = ig > 255 ? 255 : ig;
                        ib = ib > 255 ? 255 : ib;
                        colorRow[x] = ((u32)ir << 16) | ((u32)ig << 8) | (u32)ib;
                    }
                }
            }
            w0 += a0;
            w1 += a1;
            w2 += a2;
            l1 += dl1dx;
            l2 += dl2dx;
        }
        w0Row += b0;
        w1Row += b1;
        w2Row += b2;
        l1Row += dl1dy;
        l2Row += dl2dy;
    }
}

}  // namespace

u32 MulColor(u32 c, float s) {
    int r = (int)((float)((c >> 16) & 0xFF) * s), g = (int)((float)((c >> 8) & 0xFF) * s), b = (int)((float)(c & 0xFF) * s);
    return Rgb(Clamp(r, 0, 255), Clamp(g, 0, 255), Clamp(b, 0, 255));
}

u32 LerpColor(u32 a, u32 b, float t) {
    auto ch = [&](int shift) {
        float x = (float)((a >> shift) & 0xFF), y = (float)((b >> shift) & 0xFF);
        return (u32)Clamp((int)(x + (y - x) * t), 0, 255) << shift;
    };
    return ch(16) | ch(8) | ch(0);
}

void ClearDepth(RenderTarget& rt) { memset(rt.depth, 0, sizeof(float) * (size_t)rt.width * (size_t)rt.height); }

void DrawMesh(RenderContext& rc, const Mesh& mesh, const Mat4& model, u32 tint) {
    ScratchScope scope(*rc.scratch);
    ++rc.stats.meshes;

    Mat4      mvp = rc.viewProj * model;
    ClipVert* cv  = rc.scratch->Push<ClipVert>((size_t)mesh.vertexCount);
    u32*      oc  = rc.scratch->Push<u32>((size_t)mesh.vertexCount);
    float     tr = (float)((tint >> 16) & 0xFF) / 255.0f, tg = (float)((tint >> 8) & 0xFF) / 255.0f,
          tb = (float)(tint & 0xFF) / 255.0f;
    for (int i = 0; i < mesh.vertexCount; ++i) {
        const Vertex& v = mesh.verts[i];
        Vec4          p = Transform(mvp, v.pos);
        cv[i] = {p.x, p.y, p.z, p.w, v.u, v.v, (float)((v.color >> 16) & 0xFF) * tr,
                 (float)((v.color >> 8) & 0xFF) * tg, (float)(v.color & 0xFF) * tb};
        oc[i] = Outcode(cv[i]);
    }

    int w = rc.target.width, h = rc.target.height;
    for (int i = 0; i + 2 < mesh.indexCount; i += 3) {
        ++rc.stats.trianglesIn;
        u16 i0 = mesh.indices[i], i1 = mesh.indices[i + 1], i2 = mesh.indices[i + 2];
        if (oc[i0] & oc[i1] & oc[i2]) continue;  // all outside one plane
        if (!(oc[i0] | oc[i1] | oc[i2])) {
            RasterTriangle(rc, mesh, ToScreen(cv[i0], w, h), ToScreen(cv[i1], w, h), ToScreen(cv[i2], w, h));
            continue;
        }

        // Sutherland-Hodgman against only the planes this triangle crosses.
        ++rc.stats.trianglesClipped;
        ClipVert bufA[9], bufB[9];
        ClipVert* in  = bufA;
        ClipVert* out = bufB;
        int       n   = 3;
        in[0] = cv[i0];
        in[1] = cv[i1];
        in[2] = cv[i2];
        u32 planes = oc[i0] | oc[i1] | oc[i2];
        for (int p = 0; p < 5 && n >= 3; ++p) {
            if (!(planes & (1u << p))) continue;
            int m = 0;
            for (int k = 0; k < n; ++k) {
                const ClipVert& a  = in[k];
                const ClipVert& b  = in[(k + 1) % n];
                float           da = PlaneDist(a, p), db = PlaneDist(b, p);
                if (da >= 0) out[m++] = a;
                if ((da >= 0) != (db >= 0)) out[m++] = LerpVert(a, b, da / (da - db));
            }
            ClipVert* t = in;
            in          = out;
            out         = t;
            n           = m;
        }
        if (n < 3) continue;
        ScreenVert s0 = ToScreen(in[0], w, h);
        for (int k = 1; k + 1 < n; ++k) RasterTriangle(rc, mesh, s0, ToScreen(in[k], w, h), ToScreen(in[k + 1], w, h));
    }
}

}  // namespace ss
