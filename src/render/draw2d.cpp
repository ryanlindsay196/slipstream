#include "render/draw2d.h"

namespace ss {

namespace {

// 3x5 bitmap font, rows top to bottom.
const char* Glyph(char ch) {
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    switch (ch) {
        case '0': return "111101101101111";
        case '1': return "010110010010111";
        case '2': return "111001111100111";
        case '3': return "111001111001111";
        case '4': return "101101111001001";
        case '5': return "111100111001111";
        case '6': return "111100111101111";
        case '7': return "111001001010010";
        case '8': return "111101111101111";
        case '9': return "111101111001111";
        case 'A': return "010101111101101";
        case 'B': return "110101110101110";
        case 'C': return "011100100100011";
        case 'D': return "110101101101110";
        case 'E': return "111100110100111";
        case 'F': return "111100110100100";
        case 'G': return "011100101101011";
        case 'H': return "101101111101101";
        case 'I': return "111010010010111";
        case 'J': return "001001001101010";
        case 'K': return "101101110101101";
        case 'L': return "100100100100111";
        case 'M': return "101111111101101";
        case 'N': return "110101101101101";
        case 'O': return "010101101101010";
        case 'P': return "110101110100100";
        case 'Q': return "010101101110011";
        case 'R': return "110101110101101";
        case 'S': return "011100010001110";
        case 'T': return "111010010010010";
        case 'U': return "101101101101111";
        case 'V': return "101101101101010";
        case 'W': return "101101111111101";
        case 'X': return "101101010101101";
        case 'Y': return "101101010010010";
        case 'Z': return "111001010100111";
        case '.': return "000000000000010";
        case ',': return "000000000010100";
        case ':': return "000010000010000";
        case '-': return "000000111000000";
        case '+': return "000010111010000";
        case '/': return "001001010100100";
        case '%': return "101001010100101";
        case '=': return "000111000111000";
        case '(': return "010100100100010";
        case ')': return "010001001001010";
        case '<': return "001010100010001";
        case '>': return "100010001010100";
        case '!': return "010010010000010";
        case '?': return "111001011000010";
        case '_': return "000000000000111";
        case '\'': return "010010000000000";
        case '#': return "101111101111101";
        default:  return nullptr;
    }
}

inline void Put(Canvas& c, int x, int y, u32 color) {
    if ((unsigned)x < (unsigned)c.width && (unsigned)y < (unsigned)c.height) c.pixels[y * c.width + x] = color;
}

}  // namespace

void FillRect(Canvas& c, int x, int y, int w, int h, u32 color) {
    int x0 = Max(x, 0), y0 = Max(y, 0), x1 = Min(x + w, c.width), y1 = Min(y + h, c.height);
    for (int yy = y0; yy < y1; ++yy)
        for (int xx = x0; xx < x1; ++xx) c.pixels[yy * c.width + xx] = color;
}

void BlendRect(Canvas& c, int x, int y, int w, int h, u32 color, int alpha) {
    int x0 = Max(x, 0), y0 = Max(y, 0), x1 = Min(x + w, c.width), y1 = Min(y + h, c.height);
    u32 sr = (color >> 16) & 0xFF, sg = (color >> 8) & 0xFF, sb = color & 0xFF;
    for (int yy = y0; yy < y1; ++yy)
        for (int xx = x0; xx < x1; ++xx) {
            u32& d  = c.pixels[yy * c.width + xx];
            u32  dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
            dr += (u32)(((i32)sr - (i32)dr) * alpha >> 8);
            dg += (u32)(((i32)sg - (i32)dg) * alpha >> 8);
            db += (u32)(((i32)sb - (i32)db) * alpha >> 8);
            d = ((dr & 0xFF) << 16) | ((dg & 0xFF) << 8) | (db & 0xFF);
        }
}

void DrawLine(Canvas& c, int x0, int y0, int x1, int y1, u32 color) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = -(y1 > y0 ? y1 - y0 : y0 - y1), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        Put(c, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

int TextWidth(const char* text, int scale) {
    int n = (int)strlen(text);
    return n ? (n * 4 - 1) * scale : 0;
}

void DrawText(Canvas& c, int x, int y, const char* text, u32 color, int scale) {
    for (; *text; ++text, x += 4 * scale) {
        const char* g = Glyph(*text);
        if (!g) continue;
        for (int i = 0; i < 15; ++i)
            if (g[i] == '1') FillRect(c, x + (i % 3) * scale, y + (i / 3) * scale, scale, scale, color);
    }
}

void DrawTextShadow(Canvas& c, int x, int y, const char* text, u32 color, int scale) {
    DrawText(c, x + scale, y + scale, text, 0x000000, scale);
    DrawText(c, x, y, text, color, scale);
}

void DrawTextCentered(Canvas& c, int cx, int y, const char* text, u32 color, int scale) {
    DrawTextShadow(c, cx - TextWidth(text, scale) / 2, y, text, color, scale);
}

}  // namespace ss
