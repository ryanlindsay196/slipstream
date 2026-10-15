// Headless POSIX backend (Linux/macOS): enough to run --selftest, --nettest
// and dedicated tooling in CI. A windowed backend would add Present + input.
#if !defined(_WIN32)

#include "platform/platform.h"
#include "core/memory.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>

namespace ss::plat {

bool Init(const WindowDesc* window) {
    if (window) {
        Log("[platform] no window backend on this platform yet; try --selftest or --nettest\n");
        return false;
    }
    return true;
}

void Shutdown() {}

void PumpEvents(InputFrame* out) { memset(out, 0, sizeof(*out)); }
void Present(const u32*, int, int) {}

u64 Now() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}
u64 Frequency() { return 1000000000ull; }
u32 NowMs() { return (u32)(Now() / 1000000ull); }

void SleepMs(u32 ms) {
    timespec ts{(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, nullptr);
}

void* ReserveMemory(size_t size) {
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}
void ReleaseMemory(void* p, size_t size) { munmap(p, size); }

bool LoadFile(const char* path, Arena& arena, u8** outData, size_t* outSize) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    bool ok = size >= 0 && (size_t)size + 1 < arena.capacity - arena.used;
    if (ok) {
        u8* data   = (u8*)arena.Alloc((size_t)size + 1, 16);
        ok         = fread(data, 1, (size_t)size, f) == (size_t)size;
        data[size] = 0;
        *outData   = data;
        *outSize   = (size_t)size;
    }
    fclose(f);
    return ok;
}

bool SaveFile(const char* path, const void* data, size_t size) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, size, f) == size;
    fclose(f);
    return ok;
}

void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);
    fflush(stdout);
}

Socket UdpOpen(u16 port, bool loopbackOnly) {
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return kInvalidSocket;
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
    addr.sin_port        = htons(port);
    if (bind(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
        close(s);
        return kInvalidSocket;
    }
    return s;
}

void UdpClose(Socket s) {
    if (s != kInvalidSocket) close((int)s);
}

bool UdpSend(Socket s, NetAddress to, const void* data, int size) {
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(to.ip);
    addr.sin_port        = htons(to.port);
    return sendto((int)s, data, (size_t)size, 0, (sockaddr*)&addr, sizeof(addr)) == size;
}

int UdpRecv(Socket s, NetAddress* from, void* buffer, int capacity) {
    sockaddr_in addr{};
    socklen_t   len = sizeof(addr);
    ssize_t     n   = recvfrom((int)s, buffer, (size_t)capacity, 0, (sockaddr*)&addr, &len);
    if (n < 0) return -1;
    from->ip   = ntohl(addr.sin_addr.s_addr);
    from->port = ntohs(addr.sin_port);
    return (int)n;
}

}  // namespace ss::plat

#endif  // !_WIN32
