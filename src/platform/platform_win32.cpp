#if defined(_WIN32)

#include "platform/platform.h"
#include "core/memory.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mstcpip.h>
#include <timeapi.h>
#include <xinput.h>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#include <cstdarg>
#include <cstdio>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ws2_32.lib")

namespace ss::plat {

namespace {
HWND          g_hwnd = nullptr;
bool          g_quit = false;
bool          g_keys[Key_Count];
char          g_text[32];
int           g_textLen = 0;
LARGE_INTEGER g_freq;

// XInput is loaded dynamically so the exe runs on machines without it.
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetStateFn g_xinputGetState = nullptr;
int              g_padRetryCountdown[kMaxPads];
GamepadState     g_pads[kMaxPads];

Key MapVirtualKeyToKey(WPARAM vk, LPARAM lp) {
    bool extended = (lp & (1 << 24)) != 0;
    if (vk >= 'A' && vk <= 'Z') return (Key)(Key_A + (vk - 'A'));
    if (vk >= '0' && vk <= '9') return (Key)(Key_0 + (vk - '0'));
    if (vk >= VK_F1 && vk <= VK_F12) return (Key)(Key_F1 + (vk - VK_F1));
    switch (vk) {
        case VK_UP:         return Key_Up;
        case VK_DOWN:       return Key_Down;
        case VK_LEFT:       return Key_Left;
        case VK_RIGHT:      return Key_Right;
        case VK_SPACE:      return Key_Space;
        case VK_RETURN:     return Key_Enter;
        case VK_ESCAPE:     return Key_Escape;
        case VK_BACK:       return Key_Backspace;
        case VK_DELETE:     return Key_Delete;
        case VK_TAB:        return Key_Tab;
        case VK_SHIFT: {
            UINT sc = (UINT)((lp >> 16) & 0xFF);
            return MapVirtualKeyA(sc, MAPVK_VSC_TO_VK_EX) == VK_RSHIFT ? Key_RShift : Key_LShift;
        }
        case VK_CONTROL:    return extended ? Key_RCtrl : Key_LCtrl;
        case VK_MENU:       return extended ? Key_RAlt : Key_LAlt;
        case VK_OEM_COMMA:  return Key_Comma;
        case VK_OEM_PERIOD: return Key_Period;
        case VK_OEM_2:      return Key_Slash;
        case VK_OEM_1:      return Key_Semicolon;
        case VK_OEM_MINUS:  return Key_Minus;
        case VK_OEM_PLUS:   return Key_Equals;
    }
    return Key_None;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CLOSE:
        case WM_DESTROY:
            g_quit = true;
            return 0;
        case WM_KILLFOCUS:
            memset(g_keys, 0, sizeof(g_keys));  // no stuck keys after alt-tab
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYUP: {
            Key k = MapVirtualKeyToKey(wp, lp);
            if (k != Key_None) g_keys[k] = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
            if (wp == VK_F4 && (msg == WM_SYSKEYDOWN)) g_quit = true;  // keep Alt+F4 working
            return 0;
        }
        case WM_CHAR:
            if (wp >= 32 && wp < 127 && g_textLen < (int)sizeof(g_text)) g_text[g_textLen++] = (char)wp;
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

i16 ApplyStickDeadzone(SHORT v, SHORT deadzone) {
    if (v > -deadzone && v < deadzone) return 0;
    return (i16)Clamp<int>(v, -32767, 32767);
}

void PollPads() {
    if (!g_xinputGetState) return;
    for (DWORD i = 0; i < kMaxPads; ++i) {
        GamepadState& pad = g_pads[i];
        // XInputGetState on an empty slot can stall for a long time, so only
        // re-probe disconnected slots about once a second.
        if (!pad.connected && g_padRetryCountdown[i]-- > 0) continue;
        XINPUT_STATE xs{};
        if (g_xinputGetState(i, &xs) != ERROR_SUCCESS) {
            pad                   = {};
            g_padRetryCountdown[i] = 60;
            continue;
        }
        const XINPUT_GAMEPAD& gp = xs.Gamepad;
        static constexpr struct { WORD mask; PadButton button; } kMap[] = {
            {XINPUT_GAMEPAD_A, Pad_A}, {XINPUT_GAMEPAD_B, Pad_B}, {XINPUT_GAMEPAD_X, Pad_X},
            {XINPUT_GAMEPAD_Y, Pad_Y}, {XINPUT_GAMEPAD_LEFT_SHOULDER, Pad_LB},
            {XINPUT_GAMEPAD_RIGHT_SHOULDER, Pad_RB}, {XINPUT_GAMEPAD_BACK, Pad_Back},
            {XINPUT_GAMEPAD_START, Pad_Start}, {XINPUT_GAMEPAD_LEFT_THUMB, Pad_LStick},
            {XINPUT_GAMEPAD_RIGHT_THUMB, Pad_RStick}, {XINPUT_GAMEPAD_DPAD_UP, Pad_DUp},
            {XINPUT_GAMEPAD_DPAD_DOWN, Pad_DDown}, {XINPUT_GAMEPAD_DPAD_LEFT, Pad_DLeft},
            {XINPUT_GAMEPAD_DPAD_RIGHT, Pad_DRight},
        };
        pad.connected = true;
        pad.buttons   = 0;
        for (const auto& m : kMap)
            if (gp.wButtons & m.mask) pad.buttons |= 1u << m.button;
        pad.axes[Axis_LX] = ApplyStickDeadzone(gp.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        pad.axes[Axis_LY] = ApplyStickDeadzone(gp.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        pad.axes[Axis_RX] = ApplyStickDeadzone(gp.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        pad.axes[Axis_RY] = ApplyStickDeadzone(gp.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        pad.axes[Axis_LT] = (i16)(gp.bLeftTrigger * 32767 / 255);
        pad.axes[Axis_RT] = (i16)(gp.bRightTrigger * 32767 / 255);
    }
}
}  // namespace

bool Init(const WindowDesc* window) {
    QueryPerformanceFrequency(&g_freq);
    timeBeginPeriod(1);
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;

    HMODULE xinput = LoadLibraryA("xinput1_4.dll");
    if (!xinput) xinput = LoadLibraryA("xinput9_1_0.dll");
    if (xinput) g_xinputGetState = (XInputGetStateFn)(void*)GetProcAddress(xinput, "XInputGetState");

    if (!window) return true;

    // Launched by double-click? Then the console exists only for us: hide it.
    // (From a terminal, keep it so logs stay visible.)
    DWORD consoleProcs[2];
    if (GetConsoleProcessList(consoleProcs, 2) == 1) FreeConsole();

    SetProcessDPIAware();
    HINSTANCE inst = GetModuleHandleA(nullptr);
    WNDCLASSA wc{};
    wc.style         = CS_OWNDC;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "SlipstreamWindow";
    if (!RegisterClassA(&wc)) return false;

    DWORD style = WS_OVERLAPPEDWINDOW;
    RECT  r{0, 0, window->width * window->scale, window->height * window->scale};
    AdjustWindowRect(&r, style, FALSE);
    g_hwnd = CreateWindowA(wc.lpszClassName, window->title, style | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    return g_hwnd != nullptr;
}

void Shutdown() {
    if (g_hwnd) DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
    WSACleanup();
    timeEndPeriod(1);
}

void PumpEvents(InputFrame* out) {
    g_textLen = 0;
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    PollPads();
    memcpy(out->keys, g_keys, sizeof(g_keys));
    memcpy(out->pads, g_pads, sizeof(g_pads));
    memcpy(out->text, g_text, (size_t)g_textLen);
    out->textLen       = g_textLen;
    out->quitRequested = g_quit;
}

void Present(const u32* pixels, int width, int height) {
    if (!g_hwnd) return;
    RECT cr;
    GetClientRect(g_hwnd, &cr);
    int cw = cr.right - cr.left, ch = cr.bottom - cr.top;
    // Aspect-correct fit (not integer-only: 480x270 should fill common windows).
    int dw = cw, dh = cw * height / width;
    if (dh > ch) { dh = ch; dw = ch * width / height; }
    int dx = (cw - dw) / 2, dy = (ch - dh) / 2;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth       = width;
    bmi.bmiHeader.biHeight      = -height;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC dc = GetDC(g_hwnd);
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, dx, dy, dw, dh, 0, 0, width, height, pixels, &bmi, DIB_RGB_COLORS, SRCCOPY);
    HBRUSH black   = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RECT   bars[4] = {{0, 0, cw, dy}, {0, dy + dh, cw, ch}, {0, dy, dx, dy + dh}, {dx + dw, dy, cw, dy + dh}};
    for (RECT& br : bars) FillRect(dc, &br, black);
    ReleaseDC(g_hwnd, dc);
}

u64 Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (u64)t.QuadPart;
}
u64  Frequency() { return (u64)g_freq.QuadPart; }
u32  NowMs() { return (u32)(Now() * 1000 / Frequency()); }
void SleepMs(u32 ms) { Sleep(ms); }

void* ReserveMemory(size_t size) { return VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); }
void  ReleaseMemory(void* p, size_t) { VirtualFree(p, 0, MEM_RELEASE); }

bool LoadFile(const char* path, Arena& arena, u8** outData, size_t* outSize) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(f, &size) && (size_t)size.QuadPart + 1 < arena.capacity - arena.used;
    if (ok) {
        u8*   data = (u8*)arena.Alloc((size_t)size.QuadPart + 1, 16);
        DWORD read = 0;
        ok = ReadFile(f, data, (DWORD)size.QuadPart, &read, nullptr) && read == (DWORD)size.QuadPart;
        data[size.QuadPart] = 0;  // convenient for text files
        *outData = data;
        *outSize = (size_t)size.QuadPart;
    }
    CloseHandle(f);
    return ok;
}

bool SaveFile(const char* path, const void* data, size_t size) {
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool  ok      = WriteFile(f, data, (DWORD)size, &written, nullptr) && written == (DWORD)size;
    CloseHandle(f);
    return ok;
}

void Log(const char* fmt, ...) {
    char    buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    fputs(buf, stdout);
    fflush(stdout);
    OutputDebugStringA(buf);
}

// ---- UDP ----

Socket UdpOpen(u16 port, bool loopbackOnly) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return kInvalidSocket;
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    // Windows quirk: an ICMP "port unreachable" makes the *next* recvfrom on a
    // UDP socket fail with WSAECONNRESET. Turn that behaviour off.
    BOOL  off = FALSE;
    DWORD bytes = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &bytes, nullptr, nullptr);

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
    addr.sin_port        = htons(port);
    if (bind(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
        closesocket(s);
        return kInvalidSocket;
    }
    return (Socket)s;
}

void UdpClose(Socket s) {
    if (s != kInvalidSocket) closesocket((SOCKET)s);
}

bool UdpSend(Socket s, NetAddress to, const void* data, int size) {
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(to.ip);
    addr.sin_port        = htons(to.port);
    return sendto((SOCKET)s, (const char*)data, size, 0, (sockaddr*)&addr, sizeof(addr)) == size;
}

int UdpRecv(Socket s, NetAddress* from, void* buffer, int capacity) {
    for (;;) {
        sockaddr_in addr{};
        int         len = sizeof(addr);
        int         n   = recvfrom((SOCKET)s, (char*)buffer, capacity, 0, (sockaddr*)&addr, &len);
        if (n < 0) {
            int err = WSAGetLastError();
            if (err == WSAECONNRESET || err == WSAEMSGSIZE) continue;  // skip bad datagrams
            return -1;
        }
        from->ip   = ntohl(addr.sin_addr.s_addr);
        from->port = ntohs(addr.sin_port);
        return n;
    }
}

}  // namespace ss::plat

#endif  // _WIN32
