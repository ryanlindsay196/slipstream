// Platform-independent helpers declared in platform.h.
#include "platform/platform.h"

namespace ss::plat {

bool ParseAddress(const char* text, NetAddress* out) {
    const char* p = text;
    u32         ip = 0;
    if (!strncmp(p, "localhost", 9)) {
        ip = 0x7F000001;
        p += 9;
    } else {
        for (int part = 0; part < 4; ++part) {
            if (*p < '0' || *p > '9') return false;
            u32 v = 0;
            while (*p >= '0' && *p <= '9') {
                v = v * 10 + (u32)(*p++ - '0');
                if (v > 255) return false;
            }
            ip = (ip << 8) | v;
            if (part < 3 && *p++ != '.') return false;
        }
    }
    if (*p++ != ':') return false;
    u32 port = 0;
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') {
        port = port * 10 + (u32)(*p++ - '0');
        if (port > 65535) return false;
    }
    if (*p != 0 || port == 0) return false;
    out->ip   = ip;
    out->port = (u16)port;
    return true;
}

}  // namespace ss::plat
