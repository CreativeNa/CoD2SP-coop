// cod2bridge.dll - x86 bridge between an external UDP sender and CoD2's script layer.
//
// Why this shape (see cod2-coop\NOTES.md):
//   * GSC can only read cvars (getcvar/getcvarfloat); it has no file or socket access.
//   * CoD2's on-disk exe is CEG-packed, so nothing can be patched statically.
//   * Therefore: a packet arrives on 127.0.0.1, this DLL writes the pose into three float
//     cvars (coop_px / coop_py / coop_pz) that the mod creates at level start, and the
//     GSC puppet hook reads them with getcvarfloat().
//
// Finding a cvar without engine symbols:
//   1. locate the ASCII cvar name in this process's memory,
//   2. scan writable committed memory for a dword pointing at it -> cvar_t.name at offset 0,
//   3. cvar_t.name+4 is the value string pointer (kept in sync for completeness),
//   4. the float cache used by getcvarfloat is calibrated once: the mod sets coop_cal to the
//      sentinel 12345.25 and we find the dword inside the struct that holds exactly that.
//
// Everything is logged to cod2-coop\logs\bridge.log with timestamps, because a silent bridge
// is a lying bridge (coop-kit PLAYBOOK section 10).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#pragma comment(lib, "ws2_32.lib")

#define LOG_PATH "C:\\Projectsssssss\\cod2-coop\\logs\\bridge.log"
#define BIND_PORT 27099
#define SENTINEL 12345.25f

static CRITICAL_SECTION g_lock;
static char g_pose[128] = "0 0 0";

static void logf_(const char *fmt, ...) {
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

// ---- memory scanning -------------------------------------------------------------

typedef struct { void *addr; size_t size; } Region;

static int regions(Region *out, int max) {
    int n = 0;
    unsigned char *p = NULL;
    MEMORY_BASIC_INFORMATION mbi;
    while (VirtualQuery(p, &mbi, sizeof mbi) && n < max) {
        if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) &&
            !(mbi.Protect & PAGE_GUARD) && mbi.RegionSize < 256u * 1024 * 1024) {
            out[n].addr = mbi.BaseAddress;
            out[n].size = mbi.RegionSize;
            n++;
        }
        p = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
    }
    return n;
}

// Find the literal name in memory. Returns the address of the string or NULL.
static unsigned char *find_string(const char *name, Region *regs, int nregs) {
    size_t len = strlen(name);
    for (int r = 0; r < nregs; r++) {
        unsigned char *base = (unsigned char *)regs[r].addr;
        size_t size = regs[r].size;
        __try {
            for (size_t i = 0; i + len < size; i++) {
                if (base[i] == (unsigned char)name[0] && memcmp(base + i, name, len) == 0 &&
                    base[i + len] == 0) {
                    return base + i;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
    return NULL;
}

// Find cvar_t struct whose first field points at name_ptr.
static unsigned char *find_cvar_struct(unsigned char *name_ptr, Region *regs, int nregs) {
    DWORD target = (DWORD)(ULONG_PTR)name_ptr;
    for (int r = 0; r < nregs; r++) {
        unsigned char *base = (unsigned char *)regs[r].addr;
        size_t size = regs[r].size;
        __try {
            for (size_t i = 0; i + 32 < size; i += 4) {
                if (*(DWORD *)(base + i) == target) {
                    // sanity: the string pointer at +4 must be a readable pointer
                    DWORD sp = *(DWORD *)(base + i + 4);
                    if (sp > 0x10000 && sp < 0x80000000) return base + i;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
    return NULL;
}

// Offset inside cvar_t where the float cache lives, calibrated with the sentinel value.
static int find_value_offset(unsigned char *cvar, float sentinel) {
    __try {
        for (int off = 0; off <= 128 - 4; off += 4) {
            float v = *(float *)(cvar + off);
            if (v == sentinel) return off;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    return -1;
}

typedef struct {
    unsigned char *cvar;
    char *str;
    size_t str_cap;
    int value_off;
    float value;
    int ok;
} CVar;

static CVar resolve(const char *name, Region *regs, int nregs, int value_off) {
    CVar c;
    memset(&c, 0, sizeof c);
    c.value_off = value_off;
    unsigned char *np = find_string(name, regs, nregs);
    if (!np) return c;
    unsigned char *st = find_cvar_struct(np, regs, nregs);
    if (!st) return c;
    c.cvar = st;
    c.str = *(char **)(st + 4);
    // the value string lives in the game's zone heap; assume the mod padded it generously
    c.str_cap = c.str ? 63 : 0;
    c.ok = 1;
    return c;
}

static void set_cvar(CVar *c, float v, const char *text) {
    if (!c->ok) return;
    c->value = v;
    __try {
        if (c->str && text) {
            size_t n = strlen(text);
            if (n > 62) n = 62;
            memcpy(c->str, text, n);
            c->str[n] = 0;
        }
        if (c->value_off >= 0) *(float *)(c->cvar + c->value_off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf_("write fault on cvar (value_off=%d)", c->value_off);
        c->ok = 0;
    }
}

// ---- udp ---------------------------------------------------------------------------

static DWORD WINAPI udp_thread(LPVOID param) {
    (void)param;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { logf_("WSAStartup failed"); return 1; }
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) { logf_("socket failed: %d", WSAGetLastError()); return 1; }
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(BIND_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr *)&a, sizeof a) == SOCKET_ERROR) {
        logf_("bind 127.0.0.1:%d failed: %d", BIND_PORT, WSAGetLastError());
        return 1;
    }
    logf_("udp listening on 127.0.0.1:%d", BIND_PORT);

    char buf[256];
    int count = 0;
    for (;;) {
        int n = recv(s, buf, sizeof buf - 1, 0);
        if (n <= 0) continue;
        buf[n] = 0;
        EnterCriticalSection(&g_lock);
        strncpy(g_pose, buf, sizeof g_pose - 1);
        LeaveCriticalSection(&g_lock);
        if (count < 5 || (count % 50) == 0) logf_("packet #%d: %s", count, buf);
        count++;
    }
    return 0;
}

// ---- pose application ---------------------------------------------------------------

static DWORD WINAPI apply_thread(LPVOID param) {
    (void)param;
    static Region regs[4096];

    // Wait for the mod's cvars (they are created when a level starts).
    CVar cal, px, py, pz;
    int value_off = -1;
    for (int attempt = 0; attempt < 600; attempt++) {
        int nr = regions(regs, 4096);
        cal = resolve("coop_cal", regs, nr, -1);
        if (cal.ok) {
            value_off = find_value_offset(cal.cvar, SENTINEL);
            if (value_off >= 0) break;
        }
        Sleep(500);
    }
    if (value_off < 0) { logf_("cvar coop_cal not found - inject after the level starts"); return 1; }
    logf_("calibrated: value offset %d in cvar_t", value_off);

    int nr = regions(regs, 4096);
    px = resolve("coop_px", regs, nr, value_off);
    py = resolve("coop_py", regs, nr, value_off);
    pz = resolve("coop_pz", regs, nr, value_off);
    logf_("cvars: px=%d py=%d pz=%d", px.ok, py.ok, pz.ok);
    if (!px.ok || !py.ok || !pz.ok) { logf_("one or more cvars missing"); return 1; }

    int applied = 0;
    for (;;) {
        char pose[128];
        EnterCriticalSection(&g_lock);
        strncpy(pose, g_pose, sizeof pose - 1);
        pose[sizeof pose - 1] = 0;
        LeaveCriticalSection(&g_lock);

        float x, y, z;
        if (sscanf(pose, "%f %f %f", &x, &y, &z) == 3) {
            char b[64];
            sprintf(b, "%.4f", x); set_cvar(&px, x, b);
            sprintf(b, "%.4f", y); set_cvar(&py, y, b);
            sprintf(b, "%.4f", z); set_cvar(&pz, z, b);
            if (applied < 5 || (applied % 50) == 0)
                logf_("applied #%d: %.2f %.2f %.2f", applied, x, y, z);
            applied++;
        }
        Sleep(50);
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        InitializeCriticalSection(&g_lock);
        logf_("=== cod2bridge loaded into pid %lu (dll base %p) ===", GetCurrentProcessId(), (void *)mod);
        HANDLE t = CreateThread(NULL, 0, udp_thread, NULL, 0, NULL);
        if (t) CloseHandle(t);
        t = CreateThread(NULL, 0, apply_thread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
