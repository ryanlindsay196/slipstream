// draw2d.h - HUD/menu primitives on the framebuffer: rects, lines, bitmap text.
#pragma once

#include "core/base.h"

namespace ss {

struct Canvas {
    u32* pixels;
    int  width;
    int  height;
};

void FillRect(Canvas& c, int x, int y, int w, int h, u32 color);
void BlendRect(Canvas& c, int x, int y, int w, int h, u32 color, int alpha256);  // alpha 0..256
void DrawLine(Canvas& c, int x0, int y0, int x1, int y1, u32 color);
void DrawText(Canvas& c, int x, int y, const char* text, u32 color, int scale = 1);
void DrawTextShadow(Canvas& c, int x, int y, const char* text, u32 color, int scale = 1);
int  TextWidth(const char* text, int scale = 1);
void DrawTextCentered(Canvas& c, int cx, int y, const char* text, u32 color, int scale = 1);

}  // namespace ss
