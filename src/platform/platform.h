// platform.h - the entire OS surface the game may touch.
//
// To port Slipstream, implement this header. The renderer is pure software, so
// "graphics" here is just presenting a 32-bit framebuffer. Implementations:
//   platform_win32.cpp - GDI present, keyboard, XInput pads, Winsock UDP, QPC
//   platform_posix.cpp - headless (no window): timers, mmap, files, BSD sockets
#pragma once

#include "core/base.h"

namespace ss {
struct Arena;
}

namespace ss::plat {

// Platform-neutral key codes. Each backend maps its native codes onto these,
// so bindings files are portable between platforms.
enum Key : u16 {
    Key_None,
    Key_A, Key_B, Key_C, Key_D, Key_E, Key_F, Key_G, Key_H, Key_I, Key_J, Key_K, Key_L, Key_M,
    Key_N, Key_O, Key_P, Key_Q, Key_R, Key_S, Key_T, Key_U, Key_V, Key_W, Key_X, Key_Y, Key_Z,
    Key_0, Key_1, Key_2, Key_3, Key_4, Key_5, Key_6, Key_7, Key_8, Key_9,
    Key_Up, Key_Down, Key_Left, Key_Right,
    Key_Space, Key_Enter, Key_Escape, Key_Backspace, Key_Delete, Key_Tab,
    Key_LShift, Key_RShift, Key_LCtrl, Key_RCtrl, Key_LAlt, Key_RAlt,
    Key_Comma, Key_Period, Key_Slash, Key_Semicolon, Key_Minus, Key_Equals,
    Key_F1, Key_F2, Key_F3, Key_F4, Key_F5, Key_F6, Key_F7, Key_F8, Key_F9, Key_F10, Key_F11, Key_F12,
    Key_Count
};

enum PadButton : u16 {
    Pad_A, Pad_B, Pad_X, Pad_Y, Pad_LB, Pad_RB, Pad_Back, Pad_Start, Pad_LStick, Pad_RStick,
    Pad_DUp, Pad_DDown, Pad_DLeft, Pad_DRight,
    Pad_ButtonCount
};

enum PadAxis : u16 { Axis_LX, Axis_LY, Axis_RX, Axis_RY, Axis_LT, Axis_RT, Axis_Count };

constexpr int kMaxPads = 4;

struct GamepadState {
    bool connected;
    u32  buttons;            // bit per PadButton
    i16  axes[Axis_Count];   // sticks -32767..32767 (up = +), triggers 0..32767
};

struct InputFrame {
    bool         keys[Key_Count];
    GamepadState pads[kMaxPads];
    char         text[32];   // UTF-8/ASCII characters typed this frame (for text fields)
    int          textLen;
    bool         quitRequested;
};

struct WindowDesc {
    const char* title;
    int         width;  // framebuffer size
    int         height;
    int         scale;  // initial window scale
};

bool Init(const WindowDesc* window);  // nullptr => headless
void Shutdown();
void PumpEvents(InputFrame* out);
void Present(const u32* pixels, int width, int height);  // 0x00RRGGBB, top-down

u64  Now();        // monotonic ticks
u64  Frequency();  // ticks per second
u32  NowMs();
void SleepMs(u32 ms);

void* ReserveMemory(size_t size);
void  ReleaseMemory(void* p, size_t size);

bool LoadFile(const char* path, Arena& arena, u8** outData, size_t* outSize);
bool SaveFile(const char* path, const void* data, size_t size);

void Log(const char* fmt, ...);

// ---- UDP ----
struct NetAddress {
    u32 ip;    // host byte order, e.g. 127.0.0.1 = 0x7F000001
    u16 port;
};
inline bool operator==(NetAddress a, NetAddress b) { return a.ip == b.ip && a.port == b.port; }

using Socket = i64;
constexpr Socket kInvalidSocket = -1;

Socket UdpOpen(u16 port, bool loopbackOnly = false);  // port 0 = any; non-blocking
void   UdpClose(Socket s);
bool   UdpSend(Socket s, NetAddress to, const void* data, int size);
int    UdpRecv(Socket s, NetAddress* from, void* buffer, int capacity);  // -1 when nothing pending
bool   ParseAddress(const char* text, NetAddress* out);  // "a.b.c.d:port" or "localhost:port"

inline double TicksToMs(u64 ticks) { return (double)ticks * 1000.0 / (double)Frequency(); }

}  // namespace ss::plat
