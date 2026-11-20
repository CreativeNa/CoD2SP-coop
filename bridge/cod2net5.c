// cod2net5.c - THE WORKING BRIDGE (session 1 final).
//
// External UDP sender -> this DLL -> cvar strings -> GSC getcvarfloat -> puppet.origin.
//
// Verified facts this code depends on (do not "fix" them without re-verifying):
//   * CoD2 cvar_t layout: +0 name, +4 hash, +8 string, +12 resetString, +16 latchedString.
//     Writing at +8 is what getcvarfloat sees. +4 is a hash - writing there does nothing.
//     Observed stride between cvars is 0x24 (36 bytes) in this build, not the 28 an earlier
//     note claimed; resolution is by name text, so the size does not matter to this code.
//   * There is no float cache: getcvarfloat parses the string on every call.
//   * cvars are created by the GSC hook at level start with padded values, so the string buffer
//     has room for a formatted number. The GSC must run before this DLL resolves them.
//
// Build (x86):
//   cl /nologo /MT /O2 /D_CRT_SECURE_NO_WARNINGS cod2net5.c /Fe:cod2net5.dll /LD ws2_32.lib
// Inject:
//   cod2ldr.exe cod2net5.dll           (after the level has started)
// Send:
//   python tools\send_live.py          (UDP 127.0.0.1:27100, ASCII "x y z")

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include "coopapi.h"
#include "coop_text.h"
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

#define PORT 27100

// Files live next to the game exe, not at a hard-coded path: the second PC has another install folder.
static char g_dir[MAX_PATH];
static void InitDir(void) {
    DWORD n = GetModuleFileNameA(NULL, g_dir, MAX_PATH);
    while (n > 0 && g_dir[n - 1] != '\\' && g_dir[n - 1] != '/') n--;
    g_dir[n] = 0;
}
/* Script channel (built-ins coop_publish_state / coop_peer_*): the local state the GSC publishes and the last state of the peer. */
static volatile float g_pub[4];            /* x y z yaw */
static volatile LONG  g_pubSeq, g_pubMs;   /* counter, GetTickCount of the last publish */
static volatile float g_peer[4];
static volatile LONG  g_helloMs;           /* GetTickCount of the last hello ("H 1") from a guest that has not started a mission yet */
static volatile LONG  g_mute;              /* debug: while the file coop_mute.txt exists, peer packets are not shown to the script (tests the disconnect path) */
/* Enemy kill events (script channel): the script reports each enemy death with its position, the peer kills the nearest enemy there. */
typedef struct { float p[3]; LONG id; char key[32]; } KillEv;
static char g_lastKillKey[32];                                             /* the key of the kill event popped last (coop_kill_key) */
static KillEv g_kIn[16]; static volatile LONG g_kInHead, g_kInTail;    /* received, waiting for the script */
static volatile LONG g_kSeq;                                           /* our outgoing ids */
static LONG g_kSeen[32]; static int g_kSeenN;                          /* ids already accepted (the sender sends each event twice) */
/* mission flags of the peer ("F <name>"): a ring of names the script pops (coop_pop_flag) */
/* replay of the host's flags: the host remembers every flag/trigger key it sent since its latest checkpoint; a guest that loads that checkpoint with F7 asks ("Q") and gets them again,
   because the keys sent after the checkpoint died with the world the guest has just replaced */
#define HF_MAX 160
static char g_hostFlags[HF_MAX][40]; static volatile LONG g_hostFN = 0;
static volatile LONG g_replayIdx = -1; static volatile DWORD g_replayNext = 0;   /* host: next key to re-send, and when */
static volatile DWORD g_qAt[2] = {0, 0}; static DWORD g_f7Last = 0;               /* guest: when to ask the host to replay (twice) */
static char g_flagQ[16][40];
static volatile LONG g_flagHead = 0, g_flagTail = 0;
typedef struct { char name[40]; float o[3]; float v[3]; } GrenEv;                       /* a thrown grenade of the peer: weapon, start OFFSET from the thrower, velocity */
typedef struct { float o[3]; float a[3]; float t[3]; int fire; DWORD tick; } TankSt;     /* the peer's tank as last reported */
static TankSt g_tankIn;
typedef struct { char map[48]; int skill; DWORD tick; } RdyEv;                          /* the peer's "I am in this mission and ready" report */
static RdyEv g_rdyPeer;
static char g_rdyMap[48]; static volatile LONG g_rdySkill = 0, g_rdySet = 0, g_rdySetTick = 0;             /* this PC's own report (set by the mission script) */
static volatile LONG g_verBadMs = 0;                    /* GetTickCount of the last "V" report from a peer with ANOTHER version (0 = none) */
static GrenEv g_gIn[8]; static GrenEv g_gLast; static volatile LONG g_gHead = 0, g_gTail = 0;
static volatile LONG g_tpPending = 0;                       /* F8 was pressed in the game window: the script teleports the local player to the partner */
static volatile LONG g_peerDeathFx = 0;                      /* 1 once when the partner's death is answered: the script fades the screen out for the survivor */
static volatile DWORD g_deathLoadAt = 0; static char g_deathLoadCmd[160];   /* the survivor's explicit loadgame, delayed so that the fade can play */
static volatile LONG g_deathEvt = 0, g_deathPending = 0;   /* the peer died (event type 5): this game loads its last save too (MissionFollow) */
static char g_hostMap[40];                                 /* guest: the mission the host announced ("M <name>"), g_hostMapMs = when it arrived */
static volatile LONG g_hostSkill = -1;                      /* guest: the difficulty (g_gameskill 0..3) the host announced with its mission, -1 = unknown */
static volatile LONG g_hostMapMs = 0, g_hostMapFresh = 0;                /* g_hostMapFresh: a new "M" arrived, the guest acts on it at the next 200 ms tick */
static char g_pendLevel[40];                               /* UDP "level <name>": the script changes the level (test automation, no Steam prompt) */
static volatile LONG  g_pubHealth = 100, g_peerHealth = 100;   /* 11th packet field: health of the sender (0..999) */
static volatile LONG  g_pubParent, g_peerParent;       /* 10th packet field: entity number of the vehicle the sender is linked to (0 = none) */
static volatile LONG  g_pubWeapon, g_peerWeapon;    /* 9th packet field: id of the weapon in the player's hands (table in the GSC; 0 = unknown) */
static volatile LONG  g_pubEvt, g_peerEvt;          /* 8th packet field: event counter * 8 + type (1 melee, 2 grenade) */
static volatile LONG  g_pubStance, g_peerStance;   /* 0 stand, 1 crouch, 2 prone, 3 airborne (7th packet field; old peers ignore it) */
static volatile LONG  g_peerSeq;           /* number of accepted peer packets */
static void PathIn(char* out, size_t cap, const char* name) { _snprintf(out, cap, "%s%s", g_dir, name); out[cap - 1] = 0; }

// The port is read from coop_port.txt when it exists. Needed because re-injecting into a running
// game is not possible: the first copy's UDP thread keeps the port bound even after its work
// thread has exited ("cvars not resolved"), and it silently swallows the packets.
// Returns 0 when there is no coop_port.txt (menu-controlled mode: the port is chosen when Host/Join is clicked).
static int ReadPort(void) {
    char p0[MAX_PATH]; PathIn(p0, sizeof p0, "coop_port.txt");
    FILE* f = fopen(p0, "r");
    if (!f) return 0;
    int p = 0;
    if (fscanf(f, "%d", &p) != 1 || p <= 0 || p > 65535) p = PORT;
    fclose(f);
    return p;
}

#define BRIDGE_VERSION "cod2net5 0.7.8"
#define BEACON_PORT 27202     // the host announces itself here (UDP broadcast); a guest in "auto" mode listens here
#define BEACON_MAGIC "COD2COOP1"
#define MENU_PORT 27201       // both players listen / send here when the in-game menu is used
#define MENU_LOGPORT 27300    // the guest forwards its log lines to the host here (tools\log_collector.py or none)

// How this bridge is driven. File mode (the launcher): coop_port.txt / coop_peer.txt are read at load.
// Menu mode (the d3d9 proxy loaded us at game start): nothing is read; the in-game Co-op menu sets the dvars
// coop_cmd ("host" | "join" | "off") and coop_ip, a control thread applies them, and nothing listens until then.
static int g_listenPort = 0;                 // 0 until known
static volatile LONG g_enabled = 0;          // 1 once a mode is active: the UDP thread binds, the work thread scans
static volatile LONG g_mode = 0;             // 0 off, 1 host, 2 join (menu mode)
static int g_fileMode = 0;                   // 1 when coop_port.txt / coop_peer.txt exist at load (the launcher path)
static volatile LONG g_discover = 0;         // 1 while a guest in "auto" mode is waiting for a host beacon
#define LOOPBACK_OK 0                         /* a peer must be another PC */
static char g_lastHost[64];                  // the host that last answered us (coop_lasthost.txt): the fallback of "auto"

// ---- live reload: the d3d9 proxy can stop this copy (CoopStop), free it and load a newer file without a game restart ----
static volatile LONG g_quit = 0;             // set by CoopStop: every thread leaves its loop
static HANDLE g_th[8];                       // our threads: CoopStop waits for them before the DLL may be freed
static int g_nth = 0;
static SOCKET g_udpSock = INVALID_SOCKET;    // the listen socket: closed by CoopStop to wake the blocked recvfrom
static HANDLE g_mutex = NULL;                // the one-bridge-per-process mutex: released by CoopStop for the next copy
static void SleepQ(int ms) { while (ms > 0 && !g_quit) { Sleep(ms > 100 ? 100 : ms); ms -= 100; } }   // a sleep that wakes up on stop

/* ---- remembered menu settings ----
   main\coop_prefs.cfg holds the four settings of the CO-OP screen as console commands; the screen runs it when it is opened ("exec coop_prefs.cfg"),
   so the buttons and the address field show what was chosen last time. The bridge keeps the file up to date (SavePrefs) and reads the mission follow
   and the address from it at start. */
static void LOGF(const char* f, ...);
static int CbufAdd(const char* text);
static char g_curMap[96]; static volatile int g_curN = -1;      /* the newest checkpoint this PC knows (own or received): map name and number (map.svg = 0, map-1.svg = 1) */
static char g_curLoadName[96];                                   /* its file name under save\\autosave (own file, or "coopshared"): what the explicit loadgame after a death loads */
static volatile unsigned long g_lastRecvCrc = 0;                 /* CRC of the last received checkpoint: a file with this content is never sent back */
static char g_follow[8] = "on";           /* mission follow: "on" | "off" */
static char g_prefIp[40] = "auto";        /* the address field: "auto" or the typed host address of the last JOIN */
static void PrefsLoad(void) {
    char p[MAX_PATH], line[128]; PathIn(p, sizeof p, "main\\coop_prefs.cfg");
    FILE* f = fopen(p, "r"); if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char v[40];
        if (sscanf(line, "set coop_follow \"%7[^\"]\"", v) == 1 && (!_stricmp(v, "on") || !_stricmp(v, "off"))) { strcpy(g_follow, _stricmp(v, "on") ? "off" : "on"); }
        else if (sscanf(line, "set coop_ip \"%39[^\"]\"", v) == 1 && v[0]) strcpy(g_prefIp, v);
    }
    fclose(f);
}
static void PrefsSave(const char* win, const char* mouse) {
    static char last[400];
    char txt[400], p[MAX_PATH];
    _snprintf(txt, sizeof txt, "set coop_borderless \"%s\"\nset coop_rawmouse \"%s\"\nset coop_follow \"%s\"\nset coop_ip \"%s\"\n", win, mouse, g_follow, g_prefIp);
    txt[sizeof txt - 1] = 0;
    if (!strcmp(txt, last)) return;
    PathIn(p, sizeof p, "main\\coop_prefs.cfg");
    FILE* f = fopen(p, "w"); if (!f) return;
    fputs(txt, f); fclose(f); strcpy(last, txt);
    LOGF("prefs: saved (window %s, mouse %s, follow %s, address %s)", win, mouse, g_follow, g_prefIp);
}
static void LastHostPath(char* out, size_t cap) { PathIn(out, cap, "coop_lasthost.txt"); }
static void ReadLastHost(void) {
    char p0[MAX_PATH]; LastHostPath(p0, sizeof p0);
    FILE* f = fopen(p0, "r");
    if (!f) return;
    if (fscanf(f, "%63s", g_lastHost) != 1) g_lastHost[0] = 0;
    if (!LOOPBACK_OK && !strncmp(g_lastHost, "127.", 4)) g_lastHost[0] = 0;
    fclose(f);
}
static void WriteLastHost(const char* ip) {
    char p0[MAX_PATH]; LastHostPath(p0, sizeof p0);
    FILE* f = fopen(p0, "w");
    if (f) { fprintf(f, "%s\n", ip); fclose(f); }
    strncpy(g_lastHost, ip, sizeof g_lastHost - 1); g_lastHost[sizeof g_lastHost - 1] = 0;
}

// coop_peer.txt: "<ip> <port> [<logport>]" of the other player's bridge. Absent = receive only (the old
// slice). With <logport> every log line is also sent to <ip>:<logport> (tools\log_collector.py there).
// "auto <port>" = host mode: the peer's address is not known yet; it is learned from the first valid pose
// packet that arrives (the guest only has to know the host's address), and sends go to <that ip>:<port>.
static int g_hasPeer = 0, g_peerPort = 0, g_logPort = 0;
static int g_peerAuto = 0;                       // host mode
static volatile LONG g_peerKnown = 0;            // 0 until the first guest packet in host mode
static volatile unsigned long g_peerAddr = 0;    // network-order IPv4 of the learned peer
static char g_peerIp[64];
static void ReadPeer(void) {
    char p0[MAX_PATH]; PathIn(p0, sizeof p0, "coop_peer.txt");
    FILE* f = fopen(p0, "r");
    if (!f) return;
    char first[64] = "";
    if (fscanf(f, "%63s", first) == 1) {
        if (!_stricmp(first, "auto")) {
            int n = fscanf(f, "%d", &g_peerPort);
            if (n == 1 && g_peerPort > 0 && g_peerPort < 65536) { g_hasPeer = 1; g_peerAuto = 1; strcpy(g_peerIp, "(auto: first guest packet)"); }
            g_logPort = 0;   // the guest's address is not known yet, so the host forwards no log lines
        } else {
            strncpy(g_peerIp, first, sizeof g_peerIp - 1);
            int n = fscanf(f, "%d %d", &g_peerPort, &g_logPort);
            if (n >= 1 && g_peerPort > 0 && g_peerPort < 65536) g_hasPeer = 1;
            if (n < 2 || g_logPort <= 0 || g_logPort > 65535) g_logPort = 0;
            if (g_hasPeer) {
                unsigned long a = inet_addr(g_peerIp);
                if (a == INADDR_NONE) g_hasPeer = 0; else { g_peerAddr = a; g_peerKnown = 1; }
            }
        }
    }
    fclose(f);
}

static FILE* F;       // coop5.log: readable text, one line per event
static FILE* CSVF;    // coop_net.csv: one row per packet, for latency / loss analysis
static CRITICAL_SECTION L;
static CRITICAL_SECTION LSCAN;   // Collect() / ResolveCvar() share one region table and one buffer: the work thread and
                                 // the menu control thread both scan, so every scan takes this lock
static char g_pose[128] = "0 0 0";
static volatile LONG g_pkts = 0;
static volatile LONG g_anim = 0, g_animNew = 0;   // pose requested over UDP ("anim N")
static int g_tag = 0;   // port of this instance: stale instances stay alive in the process, so tag every line

// ---- clocks: wall clock for humans, QueryPerformanceCounter for intervals ----
static LARGE_INTEGER g_qf;
static double NowMs(void) { LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart * 1000.0 / (double)g_qf.QuadPart; }
static unsigned long NowUs(void) { return (unsigned long)(NowMs() * 1000.0); }   // wraps after ~71 min: only differences are used
static void WallStr(char* out, size_t cap) {
    SYSTEMTIME t; GetLocalTime(&t);
    _snprintf(out, cap, "%02d:%02d:%02d.%03d", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    out[cap - 1] = 0;
}

// ---- log forwarding: every line also goes to <peer ip>:<logport> when coop_peer.txt names one ----
static SOCKET g_logSock = INVALID_SOCKET;
static struct sockaddr_in g_logAddr;

/* Addresses in the log are masked ("26.56.245.0" -> "26.56.x.x"; 127.x and 0.x stay): a log may be posted or sent around. A file coop_unmask_ips.txt
   in the game folder switches the masking off (for the developer). */
static void MaskIps(char* m) {
    char out[400]; size_t o = 0;
    for (size_t i = 0; m[i] && o < sizeof out - 8; ) {
        int prevDigit = i > 0 && ((m[i - 1] >= '0' && m[i - 1] <= '9') || m[i - 1] == '.');
        int a, b, c, d, n = 0;
        if (!prevDigit && m[i] >= '0' && m[i] <= '9' && sscanf(m + i, "%d.%d.%d.%d%n", &a, &b, &c, &d, &n) == 4 && n >= 7 &&
            a >= 0 && a <= 255 && b >= 0 && b <= 255 && c >= 0 && c <= 255 && d >= 0 && d <= 255 && !(m[i + n] >= '0' && m[i + n] <= '9')) {
            if (a == 127 || a == 0) { memcpy(out + o, m + i, (size_t)n); o += (size_t)n; }
            else o += (size_t)_snprintf(out + o, sizeof out - o - 4, "%d.%d.x.x", a, b);
            i += (size_t)n;
        } else out[o++] = m[i++];
    }
    out[o] = 0;
    strncpy(m, out, 399); m[399] = 0;
}
static void LOGF(const char* f, ...) {
    if (!F) return;
    char msg[400], ts[16];
    va_list a; va_start(a, f); _vsnprintf(msg, sizeof msg - 1, f, a); va_end(a);
    msg[sizeof msg - 1] = 0;
    { static int unmask = -1;
      if (unmask < 0) { char up[MAX_PATH]; PathIn(up, sizeof up, "coop_unmask_ips.txt"); unmask = GetFileAttributesA(up) != INVALID_FILE_ATTRIBUTES; }
      if (!unmask) MaskIps(msg); }
    WallStr(ts, sizeof ts);
    EnterCriticalSection(&L);
    fprintf(F, "[%d] %s %s\n", g_tag, ts, msg);
    fflush(F);
    if (g_logSock != INVALID_SOCKET) {
        char pkt[460];
        int len = _snprintf(pkt, sizeof pkt - 1, "[%d] %s %s", g_tag, ts, msg);
        if (len > 0) sendto(g_logSock, pkt, len, 0, (struct sockaddr*)&g_logAddr, sizeof g_logAddr);
    }
    LeaveCriticalSection(&L);
}

// one CSV row per packet: wall clock, monotonic ms, direction (tx/rx/echo), seq, the pose, a note
static void CSV(const char* dir, long seq, float x, float y, float z, float yaw, const char* note) {
    if (!CSVF) return;
    static int rows = 0;
    if (rows > 40000) return;                       /* about 4 MB per game start: the file used to grow without limit (143 MB on a guest PC) */
    char ts[16]; WallStr(ts, sizeof ts);
    EnterCriticalSection(&L);
    fprintf(CSVF, "%s,%.3f,%s,%ld,%.3f,%.3f,%.3f,%.3f,%s\n", ts, NowMs(), dir, seq, x, y, z, yaw, note ? note : "");
    if (++rows % 25 == 0) fflush(CSVF);
    LeaveCriticalSection(&L);
}

// ---- link statistics, summarised in the log every 5 s ----
static volatile LONG g_txn = 0, g_rxn = 0, g_lost = 0, g_reord = 0, g_echo = 0;
static long g_lastSeq = -1;
static double g_rttMin = 1e9, g_rttMax = 0, g_rttSum = 0; static long g_rttN = 0;
static double g_lastRxMs = 0, g_maxGapMs = 0;

static int SC(void* d, const void* s, size_t n) {
    __try { memcpy(d, s, n); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

typedef struct { unsigned char* p; size_t s; } R;
static R g[4096]; static int nr; static unsigned char* BU;
#define CH (1u<<20)

static void Collect(void) {
    EnterCriticalSection(&LSCAN);
    int k = 0; unsigned char* q = 0; MEMORY_BASIC_INFORMATION b;
    while (VirtualQuery(q, &b, sizeof b) && k < 4096) {
        if (b.State == MEM_COMMIT && (b.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE))
            && !(b.Protect & PAGE_GUARD)) {
            g[k].p = (unsigned char*)b.BaseAddress; g[k].s = b.RegionSize; k++;
        }
        q = (unsigned char*)b.BaseAddress + b.RegionSize;
    }
    nr = k;
    LeaveCriticalSection(&LSCAN);
}

static int FindStr(const char* nd, DWORD* out, int max) {
    size_t nl = strlen(nd); int n = 0;
    for (int k = 0; k < nr && n < max; k++) {
        size_t s = g[k].s, o = 0;
        while (o < s && n < max) {
            size_t w = (s - o > CH) ? CH : (s - o);
            if (SC(BU, g[k].p + o, w)) {
                for (size_t i = 0; i < w; i++) {
                    if (BU[i] != nd[0]) continue;
                    if (memcmp(BU + i, nd, nl)) continue;
                    out[n++] = (DWORD)(ULONG_PTR)(g[k].p + o + i);
                    if (n >= max) break;
                }
            }
            o += w;
        }
    }
    return n;
}

static int FindPtr(DWORD t, DWORD* out, int max) {
    int n = 0;
    for (int k = 0; k < nr && n < max; k++) {
        size_t s = g[k].s, o = 0;
        while (o < s && n < max) {
            size_t w = (s - o > CH) ? CH : (s - o);
            if (SC(BU, g[k].p + o, w)) {
                for (size_t j = 0; j + 4 < w; j += 4) {
                    DWORD v; memcpy(&v, BU + j, 4);
                    if (v == t) { out[n++] = (DWORD)(ULONG_PTR)(g[k].p + o + j); if (n >= max) break; }
                }
            }
            o += w;
        }
    }
    return n;
}

static void RStr(DWORD p, char* out, int cap) {
    int i = 0;
    for (; i < cap - 1; i++) {
        char c;
        if (!SC(&c, (void*)(p + i), 1)) break;
        if (!c) break;
        if (c < 0x20 || c > 0x7e) { i++; break; }
        out[i] = c;
    }
    out[i] = 0;
}

// A cvar is identified by: the name text equals the requested name, and +8 points at a numeric string.
// anyText: also accept non-numeric strings ("none", "spawned", "removed") - the readback cvar holds
// words, so the numeric test below made it unresolvable (prx=0) and the readback channel dead.
// anyText == 3: like 1 (text accepted) but silent: used by the menu control thread, which asks again every few seconds.
static DWORD ResolveCvarInner(const char* nm, int anyText) {
    int text = (anyText == 1 || anyText == 3), noisy = (anyText == 1);
    DWORD firstHit = 0;
    DWORD ns[16]; int k = FindStr(nm, ns, 16);
    for (int a = 0; a < k; a++) {
        char t[40]; RStr(ns[a], t, 39);
        if (strcmp(t, nm)) continue;
        DWORD ps[24]; int m = FindPtr(ns[a], ps, 24);
        for (int b = 0; b < m; b++) {
            DWORD p8 = 0;
            if (!SC(&p8, (void*)(ps[b] + 8), 4)) continue;
            if (p8 < 0x10000 || p8 > 0x80000000) continue;
            char v[40]; RStr(p8, v, 39);
            // a menu dvar can hold an empty string (coop_ip before anything is typed): the silent mode 3 keeps it,
            // the in-image check below still weeds out stray copies
            if (!v[0]) { if (noisy) LOGF("cand %s struct=%08lX str@%08lX EMPTY/unprintable", nm, ps[b], p8); if (anyText != 3) continue; }
            int num = 1;
            // anyText == 2: digits with an optional leading 'p'. The GSC cannot turn a float into a bare
            // string ("" + float is empty), so the net5 publisher writes "p" + x and the reader strips the p.
            for (int i = (anyText == 2 && v[0] == 'p') ? 1 : 0; v[i]; i++) {
                char c = v[i];
                if (!((c >= '0' && c <= '9') || c == '-' || c == '.' || c == '+')) { num = 0; break; }
            }
            if (noisy) LOGF("cand %s struct=%08lX str@%08lX '%s' num=%d", nm, ps[b], p8, v, num);
            // log every candidate, keep the first plausible one: copies of our own log text
            // ("coop_rx='' ...") also sit in memory and contain a quote, a real cvar value never does
            // text mode: the real cvar structs sit in the static pool (0x00786xxx); heap candidates are
            // copies of our own log lines. Take a static one at once, a heap one only as a fallback.
            // The real cvar structs are in the game's image (base 0x400000, size 0x18CA000: the pool is around
            // 0x0078xxxx). A struct below the image (seen: 0x000DEB6C for coop_lyaw) or in the heap is a
            // stray copy; taking one made tx silently send nothing. Image range only, in every mode.
            int inImage = ps[b] >= 0x00400000 && ps[b] < 0x01CCA000;
            if (text) {
                if (inImage) return ps[b];
                // mode 3 (menu dvars) takes no heap fallback: the only heap hit seen was a copy of the menu's own action
                // text ("coop_cmd" "off" ; "close" ...), which would be read as the host address. A dvar that does not
                // exist yet is simply not found yet (the editfield creates coop_ip only once something is typed).
                if (anyText != 3 && !firstHit && !strchr(v, '\'')) firstHit = ps[b];
                continue;
            }
            if (num && inImage) return ps[b];
        }
    }
    return firstHit;
}

static DWORD ResolveCvar(const char* nm, int anyText) {
    EnterCriticalSection(&LSCAN);
    DWORD r = ResolveCvarInner(nm, anyText);
    LeaveCriticalSection(&LSCAN);
    return r;
}

// The current string of a cvar, only if it is a heap string at least `need` chars long (never the exe image,
// never a short literal): the pointer in the cvar struct is read fresh on every call.
// The length is checked once per new pointer (a string the GSC has just created is the full padded
// 30+ chars); after our own write the content is shorter, so a later call accepts the pointer it already
// verified, until the GSC replaces it.
static struct { DWORD cvar, ptr; } g_cv[16];
static char* CvStr(DWORD cvar, size_t need) {
    DWORD sp = 0;
    if (!SC(&sp, (void*)(cvar + 8), 4) || sp < 0x01000000) return NULL;
    int slot = -1;
    for (int i = 0; i < 16; i++) { if (g_cv[i].cvar == cvar) { slot = i; break; } if (slot < 0 && !g_cv[i].cvar) slot = i; }
    if (slot >= 0 && g_cv[slot].cvar == cvar && g_cv[slot].ptr == sp) return (char*)sp;
    size_t n = 0; char c;
    while (n < need && SC(&c, (void*)(sp + n), 1) && c) n++;
    if (n < need) return NULL;
    if (slot >= 0) { g_cv[slot].cvar = cvar; g_cv[slot].ptr = sp; }
    return (char*)sp;
}
// plain read for logging
static const char* CvPeek(DWORD cvar, char* out, size_t cap) {
    DWORD sp = 0; out[0] = 0;
    if (SC(&sp, (void*)(cvar + 8), 4) && sp >= 0x00010000) RStr(sp, out, (int)cap);
    return out;
}

// Log lines also go to <ip>:<port> by UDP while a sink is open (the guest -> the host's log_collector).
static void OpenLogSink(const char* ip, int port) {
    EnterCriticalSection(&L);
    if (g_logSock != INVALID_SOCKET) { closesocket(g_logSock); g_logSock = INVALID_SOCKET; }
    if (ip && port > 0) {
        unsigned long a = inet_addr(ip);
        if (a != INADDR_NONE) {
            g_logSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            memset(&g_logAddr, 0, sizeof g_logAddr);
            g_logAddr.sin_family = AF_INET; g_logAddr.sin_port = htons((u_short)port); g_logAddr.sin_addr.s_addr = a;
        }
    }
    LeaveCriticalSection(&L);
}

// Strictly a.b.c.d with each part 0..255 (inet_addr also accepts "26." / "1.2" / hex, which a half-typed field would hit).
static int ValidIPv4(const char* s) {
    int parts = 0, digits = 0, val = 0;
    if (!s || !*s) return 0;
    for (const char* p = s;; p++) {
        if (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); digits++; if (digits > 3 || val > 255) return 0; }
        else if (*p == '.' || *p == 0) {
            if (!digits) return 0;
            parts++; digits = 0; val = 0;
            if (*p == 0) break;
        } else return 0;
    }
    return parts == 4;
}

// Menu mode: apply the player's choice. mode 0 off, 1 host (the guest is learned from its first packet), 2 join <ip>.
static void MenuEnable(int mode) {
    g_mode = mode; g_hasPeer = 1;
    if (!g_listenPort) { g_listenPort = MENU_PORT; g_tag = MENU_PORT; }
    InterlockedExchange(&g_enabled, 1);   // lets the UDP thread bind and the work thread start looking for the mission
}

// Join a host whose address is known. keepDiscover: the last-host fallback keeps listening for a real beacon.
static void SetPeerJoin(const char* ip, int keepDiscover) {
    if (!LOOPBACK_OK && !strncmp(ip, "127.", 4)) { LOGF("menu: JOIN %s refused: enter the address of the other PC (or 'auto')", ip); return; }
    strncpy(g_peerIp, ip, sizeof g_peerIp - 1); g_peerIp[sizeof g_peerIp - 1] = 0;
    g_peerAuto = 0; g_peerAddr = inet_addr(ip); g_peerKnown = 1;
    g_peerPort = MENU_PORT; g_logPort = MENU_LOGPORT;
    OpenLogSink(ip, MENU_LOGPORT);
    if (!keepDiscover) g_discover = 0;
    MenuEnable(2);
    LOGF("menu: JOIN %s (send to %s:%d, listen %d, log lines forwarded to %d)", ip, ip, MENU_PORT, MENU_PORT, MENU_LOGPORT);
}

static void ApplyMode(int mode, const char* ip) {
    if (mode == 0) {
        g_discover = 0; g_mode = 0; g_hasPeer = 0; OpenLogSink(NULL, 0);
        LOGF("menu: co-op off, nothing is sent any more");
        return;
    }
    if (mode == 2) {
        if (!ip || !ip[0] || !_stricmp(ip, "auto")) strcpy(g_prefIp, "auto");
        else if (ValidIPv4(ip)) { strncpy(g_prefIp, ip, sizeof g_prefIp - 1); g_prefIp[sizeof g_prefIp - 1] = 0; }
        if (!ip || !ip[0] || !_stricmp(ip, "auto")) {
            // no address typed: find the host on the network by its beacon (see BEACON / DISC)
            strcpy(g_peerIp, "(auto: waiting for a host beacon)");
            g_peerAuto = 0; g_peerKnown = 0; g_peerPort = MENU_PORT; g_logPort = 0;
            OpenLogSink(NULL, 0);
            g_discover = 1;
            MenuEnable(2);
            LOGF("menu: JOIN auto - looking for a host on the network (beacons on UDP %d)%s%s", BEACON_PORT,
                 g_lastHost[0] ? "; fallback after 10 s: the last host " : "", g_lastHost);
            return;
        }
        if (!ValidIPv4(ip)) { LOGF("menu: JOIN needs 'auto' or a valid IPv4 address of the host (a.b.c.d), got '%s'", ip); return; }
        SetPeerJoin(ip, 0);
        return;
    }
    g_discover = 0;
    strcpy(g_peerIp, "(auto: first guest packet)");
    g_peerAuto = 1; g_peerKnown = 0; g_peerPort = MENU_PORT; g_logPort = 0;
    OpenLogSink(NULL, 0);
    MenuEnable(1);
    LOGF("menu: HOST (listen %d; the guest is learned from its first packet; announcing on UDP %d)", MENU_PORT, BEACON_PORT);
}

// Broadcast targets: 255.255.255.255 plus the directed broadcast of every active IPv4 adapter (a VPN adapter such as
// Radmin's does not get the limited broadcast, only the one for its own subnet, e.g. 26.255.255.255).
static int BroadcastTargets(unsigned long* out, int max) {
    int n = 0;
    out[n++] = 0xFFFFFFFFul;
    ULONG size = 16384;
    IP_ADAPTER_ADDRESSES* buf = (IP_ADAPTER_ADDRESSES*)malloc(size);
    if (!buf) return n;
    ULONG fl = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG r = GetAdaptersAddresses(AF_INET, fl, NULL, buf, &size);
    if (r == ERROR_BUFFER_OVERFLOW) { free(buf); buf = (IP_ADAPTER_ADDRESSES*)malloc(size); if (!buf) return n; r = GetAdaptersAddresses(AF_INET, fl, NULL, buf, &size); }
    if (r == NO_ERROR) {
        for (IP_ADAPTER_ADDRESSES* a = buf; a && n < max; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u && n < max; u = u->Next) {
                struct sockaddr_in* sa = (struct sockaddr_in*)u->Address.lpSockaddr;
                if (!sa || sa->sin_family != AF_INET) continue;
                unsigned long ip = ntohl(sa->sin_addr.s_addr);
                if ((ip >> 16) == 0xA9FE) continue;                              // 169.254.x.x: link-local, no use
                unsigned p = u->OnLinkPrefixLength;
                if (p == 0 || p > 30) continue;
                unsigned long mask = 0xFFFFFFFFul << (32 - p);
                unsigned long b = htonl((ip & mask) | ~mask);
                int dup = 0; for (int i = 0; i < n; i++) if (out[i] == b) dup = 1;
                if (!dup) out[n++] = b;
            }
        }
    }
    free(buf);
    return n;
}

// HOST side: while hosting, announce ourselves so a guest can click JOIN without typing an address. Quiet when not hosting.
static DWORD WINAPI BEACON(LPVOID p) {
    (void)p;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    BOOL on = TRUE;
    if (s == INVALID_SOCKET) return 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (char*)&on, sizeof on);
    int seq = 0, logged = 0;
    while (!g_quit) {
        if (g_mode != 1 || !g_hasPeer) { SleepQ(500); logged = 0; continue; }
        unsigned long t[24]; int n = BroadcastTargets(t, 24);
        char msg[64]; int len = _snprintf(msg, sizeof msg - 1, BEACON_MAGIC " %d %d", MENU_PORT, seq++);
        struct sockaddr_in to; memset(&to, 0, sizeof to); to.sin_family = AF_INET; to.sin_port = htons(BEACON_PORT);
        for (int i = 0; i < n; i++) { to.sin_addr.s_addr = t[i]; sendto(s, msg, len, 0, (struct sockaddr*)&to, sizeof to); }
        if (!logged) { LOGF("host: announcing on the network (UDP %d, %d broadcast targets); a guest can click JOIN with 'auto'", BEACON_PORT, n); logged = 1; }
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK); sendto(s, msg, len, 0, (struct sockaddr*)&to, sizeof to);   // a guest on this very PC (solo test)
        SleepQ(g_peerKnown ? 1000 : 500);    // a guest that clicks JOIN again must find the host within a second (it was 8 s once a guest had been seen)
    }
    closesocket(s);
    return 0;
}

// GUEST side ("auto"): listen for the host's beacon and take the first sender's address. If nothing is heard for 10 s,
// fall back to the host that answered last time; a real beacon that arrives later still wins.
static DWORD WINAPI DISC(LPVOID p) {
    (void)p;
    while (!g_quit) {
        if (!g_discover) { SleepQ(300); continue; }
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) { SleepQ(2000); continue; }
        BOOL re = TRUE; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char*)&re, sizeof re);
        struct sockaddr_in a; memset(&a, 0, sizeof a);
        a.sin_family = AF_INET; a.sin_port = htons(BEACON_PORT); a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s, (struct sockaddr*)&a, sizeof a) == SOCKET_ERROR) {
            LOGF("discovery: cannot listen on UDP %d (error %d); type the host's address instead", BEACON_PORT, WSAGetLastError());
            closesocket(s); SleepQ(5000); continue;
        }
        DWORD to = 500; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof to);
        DWORD t0 = GetTickCount(); int fb = 0;
        LOGF("discovery: listening for a host beacon on UDP %d", BEACON_PORT);
        while (g_discover && !g_quit) {
            char b[128]; struct sockaddr_in from; int fl = sizeof from;
            int r = recvfrom(s, b, sizeof b - 1, 0, (struct sockaddr*)&from, &fl);
            if (r > 0) {
                b[r] = 0;
                if (strncmp(b, BEACON_MAGIC " ", sizeof BEACON_MAGIC) != 0) continue;
                char ipstr[32]; strncpy(ipstr, inet_ntoa(from.sin_addr), sizeof ipstr - 1); ipstr[sizeof ipstr - 1] = 0;
                if (!ValidIPv4(ipstr)) continue;
                LOGF("discovery: host found at %s (its beacon)", ipstr);
                SetPeerJoin(ipstr, 0);          // clears g_discover: the loop ends
                break;
            }
            if (!fb && g_lastHost[0] && ValidIPv4(g_lastHost) && (LOOPBACK_OK || strncmp(g_lastHost, "127.", 4)) && GetTickCount() - t0 > 10000) {
                fb = 1;
                LOGF("discovery: no beacon for 10 s; trying the last host %s (still listening for a beacon)", g_lastHost);
                SetPeerJoin(g_lastHost, 1);
            }
        }
        closesocket(s);
    }
    return 0;
}

static DWORD WINAPI UDP(LPVOID p) {
    (void)p;
    while (!g_enabled && !g_quit) Sleep(250);        // menu mode: nothing listens until Host / Join is clicked
    if (g_quit) return 0;
    int port = g_listenPort;
    g_tag = port;
    LOGF("peer %s, log forwarding %s", g_hasPeer ? g_peerIp : "none (receive only)", g_logSock != INVALID_SOCKET ? "on" : "off");
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) { LOGF("socket fail"); return 1; }
    g_udpSock = s;                          // CoopStop closes it to unblock the recvfrom below
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(port);
    // loopback unless the peer is on another machine (then the packets arrive from the LAN).
    // The listener only parses numbers into cvar strings; it is still reachable from the LAN while open.
    // ALWAYS all interfaces: a socket bound to loopback cannot send to any other address (WSAENETUNREACH), so the peer may change at any time.
    int lan = 1;
    a.sin_addr.s_addr = htonl(lan ? INADDR_ANY : INADDR_LOOPBACK);
    LOGF("listen on %s", lan ? "all interfaces (peer is remote)" : "loopback");
    if (bind(s, (struct sockaddr*)&a, sizeof a) == SOCKET_ERROR) {
        LOGF("bind %d FAILED err=%d", port, WSAGetLastError());
        return 1;
    }
    LOGF("udp bound %d", port);
    char b[256];
    while (!g_quit) {
        struct sockaddr_in from; int fl = sizeof from;
        int r = recvfrom(s, b, sizeof b - 1, 0, (struct sockaddr*)&from, &fl);
        if (r <= 0) { if (g_quit) break; LOGF("recv err %d", WSAGetLastError()); Sleep(50); continue; }
        b[r] = 0;
        {   /* WHO may talk to us. Before this check every packet type ("level <map>" included: it changes the mission) was accepted from ANY address that
               could reach the port. Now: this PC (the test tools), the partner, and nobody else. A pose packet of a new sender is still handed to the
               existing "reconnect after 5 s of silence" rule below; before a peer is known only "H" (hello) and pose packets pass. */
            unsigned long fsrc = from.sin_addr.s_addr;
            int isLoop = (fsrc == inet_addr("127.0.0.1"));
            int isPose = ((b[0] >= '0' && b[0] <= '9') || b[0] == '-' || b[0] == '.');
            int isHello = (b[0] == 'H' && b[1] == ' ');
            int allowed = isLoop;
            if (!allowed && g_peerKnown && fsrc == g_peerAddr) allowed = 1;
            if (!allowed && g_peerAuto && (isPose || isHello)) allowed = 1;       /* the host learns / re-learns the guest from these two */
            if (!allowed) {
                static DWORD lastBadLog = 0; static unsigned long lastBadSrc = 0; DWORD tnow = GetTickCount();
                if (fsrc != lastBadSrc || tnow - lastBadLog > 30000) {
                    lastBadSrc = fsrc; lastBadLog = tnow;
                    LOGF("ignored a packet from %s: that is not the partner (first byte '%c', %d bytes)", inet_ntoa(from.sin_addr), (b[0] >= 32 && b[0] < 127) ? b[0] : '?', r);
                }
                continue;
            }
            if ((!strncmp(b, "anim ", 5) || !strncmp(b, "level ", 6)) && !isLoop) { LOGF("ignored a test command from %s (only this PC may send it)", inet_ntoa(from.sin_addr)); continue; }
        }
        if (!strncmp(b, "anim ", 5)) {   // pose command: "anim N" -> cvar coop_anim, no console needed
            g_anim = atoi(b + 5);
            g_animNew = 1;
            LOGF("anim command %d", g_anim);
            continue;
        }
        if (!strncmp(b, "C anim ", 7) && from.sin_addr.s_addr == inet_addr("127.0.0.1")) {   // dev test from this PC only: "C anim <0-9>" plays a test pose on the puppet (console variable coop_anim)
            int an = atoi(b + 7); char cmd[48]; _snprintf(cmd, sizeof cmd, "set coop_anim %d\n", (an >= 1 && an <= 6) ? an + 10 : 1);   /* test poses are 11-16: the script sets coop_anim to 1 by itself (the calm default) */
            LOGF("test: puppet pose %d %s", an, CbufAdd(cmd) ? "queued" : "FAILED"); continue;
        }
        if (!strncmp(b, "C fade", 6) && from.sin_addr.s_addr == inet_addr("127.0.0.1")) { InterlockedExchange(&g_peerDeathFx, 1); LOGF("test: kill of the local player requested"); continue; }   // dev test from this PC only
        if (!strncmp(b, "C loadgame ", 11) && from.sin_addr.s_addr == inet_addr("127.0.0.1")) {   // dev test from this PC only: "C loadgame <save name>"
            char nm[80]; int n2 = 0;
            for (const char* q = b + 11; *q && n2 < 70; q++) {
                if ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '_' || *q == '-' || *q == '/') nm[n2++] = *q; else break;
            }
            nm[n2] = 0;
            if (n2 > 0) { char cmd[120]; _snprintf(cmd, sizeof cmd, "loadgame %s\n", nm); LOGF("test: console command loadgame %s %s", nm, CbufAdd(cmd) ? "queued" : "FAILED"); }
            continue;
        }
        if (!strncmp(b, "level ", 6)) {  // test automation: "level <name>" -> the script calls changelevel
            char nm[40]; int n2 = 0;
            for (const char* q = b + 6; *q && n2 < 38; q++) {
                if ((*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9') || *q == '_') nm[n2++] = *q; else break;
            }
            nm[n2] = 0;
            if (n2 > 0) { EnterCriticalSection(&L); strcpy(g_pendLevel, nm); LeaveCriticalSection(&L); LOGF("level change requested: %s", nm); }
            continue;
        }
        if (!strncmp(b, "K ", 2)) {      // enemy kill event from the peer: "K x y z id"
            float kx, ky, kz; long kid = 0;
            if (sscanf(b + 2, "%f %f %f %ld", &kx, &ky, &kz, &kid) == 4 && fabsf(kx) < 1e6f && fabsf(ky) < 1e6f && fabsf(kz) < 1e6f) {
                int dup = 0;
                for (int i = 0; i < 32; i++) if (g_kSeen[i] == kid && kid != 0) dup = 1;
                if (!dup) {
                    g_kSeen[g_kSeenN++ & 31] = kid;
                    LONG h = g_kInHead;
                    if (h - g_kInTail < 16) {
                        KillEv* e = &g_kIn[h & 15]; e->p[0] = kx; e->p[1] = ky; e->p[2] = kz; e->id = kid; e->key[0] = 0;
                        {   /* an optional key of the killed character ("K x y z id key"): letters, digits and "_" only */
                            const char* kp = b + 2; int sp = 0;
                            while (*kp && sp < 4) { if (*kp == ' ') sp++; kp++; }
                            int n = 0;
                            if (sp >= 4) for (; *kp && n < 30; kp++) { if ((*kp >= 'a' && *kp <= 'z') || (*kp >= 'A' && *kp <= 'Z') || (*kp >= '0' && *kp <= '9') || *kp == '_') e->key[n++] = *kp; else break; }
                            e->key[n] = 0;
                        }
                        g_kInHead = h + 1;
                        if (e->key[0]) LOGF("kill event received #%ld at %.0f %.0f %.0f key %s", kid, kx, ky, kz, e->key);
                        else LOGF("kill event received #%ld at %.0f %.0f %.0f", kid, kx, ky, kz);
                    }
                }
            }
            continue;
        }
        if (!strncmp(b, "R ", 2)) {      // the peer is in a mission and ready: "R <map> <skill>"
            RdyEv re; memset(&re, 0, sizeof re); char mp[48]; int sk = 0, ok = 1;
            if (sscanf(b + 2, "%47s %d", mp, &sk) == 2) {
                for (const char* q = mp; *q; q++) if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '_' || *q == '-')) ok = 0;
                if (ok) { strcpy(re.map, mp); re.skill = sk; re.tick = GetTickCount(); EnterCriticalSection(&L); g_rdyPeer = re; LeaveCriticalSection(&L); }
            }
            continue;
        }
        if (!strncmp(b, "V ", 2)) {      // the peer's version: "V 0.7.4"
            char pv[24]; memset(pv, 0, sizeof pv);
            if (sscanf(b + 2, "%23s", pv) == 1) {
                const char* mine = strchr(BRIDGE_VERSION, ' '); mine = mine ? mine + 1 : BRIDGE_VERSION;
                size_t ml = strcspn(mine, " ");
                if (strlen(pv) != ml || strncmp(pv, mine, ml)) {
                    static char lastBad[24];
                    if (strcmp(lastBad, pv)) { strncpy(lastBad, pv, sizeof lastBad - 1); LOGF("VERSION MISMATCH: the partner runs %s, this PC runs %.*s - install the same release on both PCs", pv, (int)ml, mine); }
                    g_verBadMs = (LONG)GetTickCount() | 1;
                } else g_verBadMs = 0;
            }
            continue;
        }
        if (!strncmp(b, "T ", 2)) {      // the peer's tank: "T ox oy oz ax ay az tx ty tz fire" (position, hull angles, turret angles, shot counter)
            TankSt ts; memset(&ts, 0, sizeof ts); int ok = 1;
            if (sscanf(b + 2, "%f %f %f %f %f %f %f %f %f %d", &ts.o[0], &ts.o[1], &ts.o[2], &ts.a[0], &ts.a[1], &ts.a[2], &ts.t[0], &ts.t[1], &ts.t[2], &ts.fire) != 10) ok = 0;
            for (int i = 0; ok && i < 3; i++) if (!(fabsf(ts.o[i]) < 1e6f && fabsf(ts.a[i]) < 1e6f && fabsf(ts.t[i]) < 1e6f)) ok = 0;
            if (ok) { ts.tick = GetTickCount(); EnterCriticalSection(&L); g_tankIn = ts; LeaveCriticalSection(&L); }
            continue;
        }
        if (!strncmp(b, "G ", 2)) {      // a grenade the peer threw: "G <weapon> ox oy oz vx vy vz"
            GrenEv ev; memset(&ev, 0, sizeof ev); char wn[40];
            if (sscanf(b + 2, "%39s %f %f %f %f %f %f", wn, &ev.o[0], &ev.o[1], &ev.o[2], &ev.v[0], &ev.v[1], &ev.v[2]) == 7) {
                int ok = 1; for (const char* q = wn; *q; q++) if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '_' || *q == '-')) ok = 0;
                for (int i = 0; i < 3; i++) if (!(fabsf(ev.o[i]) < 1e5f && fabsf(ev.v[i]) < 1e5f)) ok = 0;
                static char lastG[160]; static DWORD lastGt = 0;      /* every grenade packet is sent twice (loss): the copy must not start a second grenade */
                if (ok && lastGt && GetTickCount() - lastGt < 400 && !strncmp(lastG, b, sizeof lastG - 1)) ok = 0;
                if (ok) { strncpy(lastG, b, sizeof lastG - 1); lastG[sizeof lastG - 1] = 0; lastGt = GetTickCount(); }
                if (ok) {
                    strcpy(ev.name, wn);
                    EnterCriticalSection(&L);
                    if (g_gHead - g_gTail < 8) { g_gIn[g_gHead & 7] = ev; g_gHead++; LOGF("grenade received: %s offset %.0f %.0f %.0f velocity %.0f %.0f %.0f", wn, ev.o[0], ev.o[1], ev.o[2], ev.v[0], ev.v[1], ev.v[2]); }
                    LeaveCriticalSection(&L);
                }
            }
            continue;
        }
        if (!strncmp(b, "Q", 1) && (b[1] == 0 || b[1] == ' ') && g_mode == 1) {   // the guest took the host's checkpoint (F7): send the keys sent since it again
            if (g_hostFN > 0) { g_replayNext = GetTickCount(); InterlockedExchange(&g_replayIdx, 0); LOGF("replay: the guest asked, re-sending %ld flag keys", (long)g_hostFN); }
            else LOGF("replay: the guest asked, nothing to re-send");
            continue;
        }
        if (!strncmp(b, "F ", 2)) {      // a mission flag the peer's script set: "F <name>"
            char nm[40]; int n2 = 0;
            for (const char* q = b + 2; *q && n2 < 38; q++) {
                if ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '_' || *q == ' ') nm[n2++] = *q; else break;
            }
            nm[n2] = 0;
            if (n2 > 0) {
                EnterCriticalSection(&L);
                int dup = 0;
                for (LONG i = g_flagTail; i < g_flagHead; i++) if (!strcmp(g_flagQ[i & 15], nm)) dup = 1;
                if (!dup && g_flagHead - g_flagTail < 16) { strcpy(g_flagQ[g_flagHead & 15], nm); g_flagHead++; LOGF("flag received: %s", nm); }
                LeaveCriticalSection(&L);
            }
            continue;
        }
        if (!strncmp(b, "M ", 2)) {      // the host's current mission (guest side): "M <name>"
            char nm[40]; int n2 = 0;
            for (const char* q = b + 2; *q && n2 < 38; q++) {
                if ((*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9') || *q == '_') nm[n2++] = *q; else break;
            }
            nm[n2] = 0;
            { const char* sp = b + 2 + n2; while (*sp == ' ') sp++; if (*sp >= '0' && *sp <= '3' && g_mode == 2) g_hostSkill = *sp - '0'; }
            if (n2 > 0 && g_mode == 2) {
                EnterCriticalSection(&L); strcpy(g_hostMap, nm); LeaveCriticalSection(&L);
                g_hostMapMs = (LONG)GetTickCount(); g_hostMapFresh = 1;
            }
            continue;
        }
        if (!strncmp(b, "H ", 2)) {      // hello from a guest that has clicked JOIN but is not in a mission yet (host side only)
            if (g_mode == 1) {
                if (!g_peerKnown) { g_peerAddr = from.sin_addr.s_addr; g_peerKnown = 1; LOGF("peer learned: %s (hello, before any pose packet)", inet_ntoa(from.sin_addr)); }
                g_helloMs = (LONG)GetTickCount();
            }
            continue;
        }
        if (!strncmp(b, "E ", 2)) {      // echo of one of our packets: "E <seq> <our us timestamp>"
            long eseq = 0; unsigned long eus = 0;
            if (sscanf(b + 2, "%ld %lu", &eseq, &eus) == 2) {
                double rtt = (double)(unsigned long)(NowUs() - eus) / 1000.0;   // ms, same clock as the sender's
                EnterCriticalSection(&L);
                if (rtt < g_rttMin) g_rttMin = rtt;
                if (rtt > g_rttMax) g_rttMax = rtt;
                g_rttSum += rtt; g_rttN++;
                LeaveCriticalSection(&L);
                InterlockedIncrement(&g_echo);
                char note[32]; _snprintf(note, sizeof note - 1, "rtt_ms=%.2f", rtt); note[sizeof note - 1] = 0;
                CSV("echo", eseq, 0, 0, 0, 0, note);
            }
            continue;
        }
        // a pose packet: "x y z yaw [seq us]"
        float px_, py_, pz_, pyw_ = 0; long seq = -1; unsigned long sus = 0; int stn_ = 0; long evt_ = 0; int wid_ = 0; int par_ = 0; int hp_ = 100;
        int nf = sscanf(b, "%f %f %f %f %ld %lu %d %ld %d %d %d", &px_, &py_, &pz_, &pyw_, &seq, &sus, &stn_, &evt_, &wid_, &par_, &hp_);
        double now = NowMs();
        // host mode: learn the guest from the first well-formed pose packet; follow a new sender only when the
        // old one has been silent for 5 s (a guest that reconnected from another address)
        if (g_peerAuto && nf >= 6) {
            unsigned long src = from.sin_addr.s_addr;
            EnterCriticalSection(&L);
            double since = g_lastRxMs > 0 ? now - g_lastRxMs : 1e9;
            LeaveCriticalSection(&L);
            if (!g_peerKnown) {
                g_peerAddr = src; g_peerKnown = 1;
                LOGF("peer learned: %s (first pose packet)", inet_ntoa(from.sin_addr));
            } else if (src != g_peerAddr && since > 5000.0) {
                g_peerAddr = src;
                LOGF("peer switched to %s (the previous one was silent for %.0f s)", inet_ntoa(from.sin_addr), since / 1000.0);
            }
        }
        // a guest that is being answered by its host remembers it: the fallback of the next "auto" when no beacon is heard
        static int savedHost = 0;
        if (!savedHost && nf >= 6 && g_mode == 2 && g_peerKnown && g_peerIp[0] != '(') {
            savedHost = 1;
            if (strcmp(g_lastHost, g_peerIp)) { WriteLastHost(g_peerIp); LOGF("remembered the host %s for the next 'auto' (coop_lasthost.txt)", g_peerIp); }
        }
        if (nf >= 6) {                   // from the other bridge: echo at once (it measures RTT), track loss and gaps
            char e[64];
            int el = _snprintf(e, sizeof e - 1, "E %ld %lu", seq, sus);
            // the packet came from the sender's TX socket (a random port nobody reads); the echo must go to
            // the sender's LISTENING port, which is the peer port from our own coop_peer.txt
            struct sockaddr_in to = from;
            if (g_hasPeer) to.sin_port = htons((u_short)g_peerPort);
            sendto(s, e, el, 0, (struct sockaddr*)&to, sizeof to);
            EnterCriticalSection(&L);
            if (g_lastSeq >= 0) {
                if (seq > g_lastSeq + 1) g_lost += (LONG)(seq - g_lastSeq - 1);
                else if (seq <= g_lastSeq && g_lastSeq - seq < 1000) g_reord++;   // late or duplicate
            }
            if (seq > g_lastSeq || g_lastSeq - seq >= 1000) g_lastSeq = seq;      // a far smaller seq = the peer restarted
            if (g_lastRxMs > 0 && now - g_lastRxMs > g_maxGapMs) g_maxGapMs = now - g_lastRxMs;
            g_lastRxMs = now;
            LeaveCriticalSection(&L);
        } else {
            EnterCriticalSection(&L);
            g_lastRxMs = now;
            LeaveCriticalSection(&L);
        }
        if (nf >= 3) { InterlockedIncrement(&g_rxn); CSV("rx", seq, px_, py_, pz_, pyw_, ""); }
        if (!g_mute && nf >= 3 && fabsf(px_) < 1e6f && fabsf(py_) < 1e6f && fabsf(pz_) < 1e6f && fabsf(pyw_) < 1e6f) {   /* script channel */
            g_peer[0] = px_; g_peer[1] = py_; g_peer[2] = pz_; g_peer[3] = nf >= 4 ? pyw_ : 0.0f;
            g_peerStance = (nf >= 7 && stn_ >= 0 && stn_ <= 4) ? stn_ : 0;
            if (nf >= 8) g_peerEvt = (LONG)evt_;
            if (nf >= 8 && (evt_ % 8) != 5) g_deathEvt = 0;   /* another event: the next death counts again (the counter restarts after a reload from a save) */
            if (nf >= 8 && (evt_ % 8) == 5 && (LONG)evt_ != g_deathEvt) { g_deathEvt = (LONG)evt_; g_deathPending = 1; LOGF("follow: the peer died (event %d)", (int)evt_); }
            g_peerWeapon = (nf >= 9 && wid_ >= 0 && wid_ <= 99) ? wid_ : 0;
            g_peerParent = (nf >= 10 && par_ >= 0 && par_ < 2048) ? par_ : 0;
            g_peerHealth = (nf >= 11 && hp_ >= 0 && hp_ <= 999) ? hp_ : 100;
            InterlockedIncrement(&g_peerSeq);
        }
        EnterCriticalSection(&L);
        strncpy(g_pose, b, sizeof g_pose - 1);
        LeaveCriticalSection(&L);
        LONG n = InterlockedIncrement(&g_pkts);
        if (n <= 3 || n % 100 == 0) LOGF("pkt %ld: %s", n, b);
    }
    closesocket(s);                          // (a stop already closed it; closing again only returns an error)
    return 0;
}

static DWORD WINAPI WORK(LPVOID p) {
    (void)p;
    if (!F || !BU) return 0;
    while (!g_enabled && !g_quit) Sleep(250);        // menu mode: do not scan the game's memory until a mode is chosen
    if (g_quit) return 0;

    // The mod's cvars appear when a mission starts. Instead of giving up (the old behaviour left the UDP
    // port bound and the log closed, so a second try could not work) wait here, up to 20 minutes.
    DWORD px = 0, py = 0, pz = 0;
    for (int waited = 0;; waited++) {
        Collect();
        px = ResolveCvar("coop_px", 0); py = ResolveCvar("coop_py", 0); pz = ResolveCvar("coop_pz", 0);
        if (px && py && pz) break;
        if (waited % 5 == 0) LOGF("waiting for a mission: the mod's cvars do not exist yet (start a NEW mission, not a save)");
        if (waited >= 400) { LOGF("gave up waiting for a mission after 20 min"); return 0; }
        SleepQ(3000);
        if (g_quit) return 0;
    }
    LOGF("regions=%d", nr);
    LOGF("structs px=%08lX py=%08lX pz=%08lX", px, py, pz);

    // optional readback channel: the GSC writes what getcvarfloat() returned into coop_rx
    DWORD prx = ResolveCvar("coop_rx", 1);
    LOGF("readback cvar %08lX", prx);

    // incoming yaw (net5 mod): optional, 0 when the loaded mod does not create it
    DWORD pyaw = ResolveCvar("coop_pyaw", 0);
    LOGF("coop_pyaw cvar %08lX", pyaw);

    // outgoing side: the local player's state, published by the net5 GSC in coop_lx/ly/lz/lyaw
    // The socket always exists; WHERE it sends is read from the runtime state (g_peerAddr / g_peerPort) at every send,
    // so a Host / Join change from the menu takes effect without restarting anything.
    SOCKET tx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in peer; memset(&peer, 0, sizeof peer);
    peer.sin_family = AF_INET;
    // (the main loop below runs `while (!g_quit)`)
    if (tx == INVALID_SOCKET) LOGF("tx socket failed (error %d): nothing can be sent", WSAGetLastError());
    else if (g_hasPeer && g_peerAuto) LOGF("host mode: waiting for the guest's first packet, then tx -> <guest>:%d", g_peerPort);
    else if (g_hasPeer) LOGF("tx ready (peer %s)", g_peerIp);
    DWORD lc[4] = { 0, 0, 0, 0 };   // coop_lx coop_ly coop_lz coop_lyaw
    int txtick = 0, resolvetick = 0, txn = 0;
    static const char* lname[4] = { "coop_lx", "coop_ly", "coop_lz", "coop_lyaw" };

    LONG last = 0;
    int n = 0;
    int rbtick = 0, stattick = 0, peerQuiet = 0;
    while (!g_quit) {
        LONG pk = g_pkts;
        if (g_hasPeer) {
            int pubFresh = g_pubSeq && (LONG)(GetTickCount() - (DWORD)g_pubMs) < 1000;
            if (!lc[0] && !pubFresh && ++resolvetick >= 50) {   // once a second until the GSC has created them
                resolvetick = 0;
                int ok = 1;
                for (int i = 0; i < 4; i++) { lc[i] = ResolveCvar(lname[i], 2); if (!lc[i]) ok = 0; }
                if (ok) LOGF("local state cvars %08lX %08lX %08lX %08lX", lc[0], lc[1], lc[2], lc[3]);
                else { memset(lc, 0, sizeof lc); }
            }
            if (pubFresh && ++txtick >= 2) {       /* published by the script through coop_publish: no cvar strings */
                txtick = 0;
                float pv[4]; for (int i = 0; i < 4; i++) pv[i] = g_pub[i];
                char pkt[200];
                int len = _snprintf(pkt, sizeof pkt - 1, "%.3f %.3f %.3f %.3f %d %lu %ld %ld %ld %ld %ld", pv[0], pv[1], pv[2], pv[3], txn, NowUs(), (long)g_pubStance, (long)g_pubEvt, (long)g_pubWeapon, (long)g_pubParent, (long)g_pubHealth);
                if (!g_peerKnown || tx == INVALID_SOCKET) len = 0;
                else { peer.sin_port = htons((u_short)g_peerPort); peer.sin_addr.s_addr = g_peerAddr; }
                if (len > 0) {
                    sendto(tx, pkt, len, 0, (struct sockaddr*)&peer, sizeof peer);
                    CSV("tx", txn, pv[0], pv[1], pv[2], pv[3], "");
                    InterlockedIncrement(&g_txn);
                    if (txn < 3 || txn % 100 == 0) LOGF("tx %d (script channel): %s", txn, pkt);
                    txn++;
                }
            } else if (lc[0] && ++txtick >= 2) {   // ~40 ms
                txtick = 0;
                char v[4][40]; int ok = 1, bad = -1;
                for (int i = 0; i < 4; i++) {
                    DWORD sp = 0;
                    if (!SC(&sp, (void*)(lc[i] + 8), 4) || sp < 0x01000000) { ok = 0; bad = i; break; }
                    RStr(sp, v[i], 39);
                }
                // a silent failure cost a whole test (tx=0 with no hint): say why, every ~5 s
                static int skipped = 0;
                if (!ok && (skipped++ % 125) == 0)
                    LOGF("tx skipped: local cvar %d (%s at %08lX) has no readable heap string; resolving again", bad, lname[bad], lc[bad]);
                if (!ok) { static int fails = 0; if (++fails >= 50) { fails = 0; memset(lc, 0, sizeof lc); } }   // re-resolve
                if (ok) {
                    char pkt[200];
                    const char* f[4];
                    for (int i = 0; i < 4; i++) f[i] = (v[i][0] == 'p') ? v[i] + 1 : v[i];   // strip the GSC's "p"
                    // "x y z yaw seq us": seq and our own timestamp let the peer echo them back (RTT) and count loss
                    int len = _snprintf(pkt, sizeof pkt - 1, "%s %s %s %s %d %lu", f[0], f[1], f[2], f[3], txn, NowUs());
                    if (!g_peerKnown || tx == INVALID_SOCKET) len = 0;   // nobody to send to yet (host: guest not seen yet)
                    else { peer.sin_port = htons((u_short)g_peerPort); peer.sin_addr.s_addr = g_peerAddr; }
                    if (len > 0) {
                        sendto(tx, pkt, len, 0, (struct sockaddr*)&peer, sizeof peer);
                        CSV("tx", txn, (float)atof(f[0]), (float)atof(f[1]), (float)atof(f[2]), (float)atof(f[3]), "");
                        InterlockedIncrement(&g_txn);
                        if (txn < 3 || txn % 100 == 0) LOGF("tx %d: %s", txn, pkt);
                        txn++;
                    }
                }
            }
        }
        // pose command: the GSC polls coop_anim; a single digit fits the 2-byte string the GSC created
        if (g_animNew) {
            g_animNew = 0;
            // numeric mode: coop_anim is always a string of digits, and the text mode also matches
            // copies of our own log lines in memory (it wrote into one of those: the pose never changed)
            DWORD pan = ResolveCvar("coop_anim", 0);
            if (!pan) LOGF("coop_anim not resolved (is the net4 mod loaded?)");
            else {
                DWORD ap = *(DWORD*)(pan + 8);
                int v = (int)g_anim;
                if (v < 0 || v > 999) v = 0;
                // never write into the exe image: a short literal like "0" is shared by other cvars
                // (that is how a first version of this corrupted it). The GSC creates coop_anim as a
                // 10-char zero string in the heap, so write 9 zeros + digit and keep the length.
                if (ap < 0x01000000) LOGF("coop_anim string is in the exe image (%08lX): refusing to write", ap);
                else {
                    char* as = (char*)ap;
                    __try {
                        if (strlen(as) < 10) { LOGF("coop_anim string too short (%u): refusing", (unsigned)strlen(as)); }
                        else { memset(as, '0', 7); as[7] = (char)('0' + v / 100); as[8] = (char)('0' + (v / 10) % 10); as[9] = (char)('0' + v % 10); LOGF("coop_anim <- %d", v); }
                    } __except (EXCEPTION_EXECUTE_HANDLER) { LOGF("coop_anim write fault"); }
                }
            }
        }
        // only real pose packets: an injection without traffic must not zero the puppet's position
        if (pk != last) {
            last = pk;
            char q[128];
            EnterCriticalSection(&L);
            strncpy(q, g_pose, sizeof q - 1);
            q[sizeof q - 1] = 0;
            LeaveCriticalSection(&L);
            float x, y, z, yaw = 0;
            int nf = sscanf(q, "%f %f %f %f", &x, &y, &z, &yaw);
            // packets may now come from the LAN: "%.3f" of 1e38 would overflow the 30-char cvar buffer
            // (and NaN fails every comparison), so only sane map coordinates are accepted
            if (nf >= 3 && !(fabsf(x) < 1e6f && fabsf(y) < 1e6f && fabsf(z) < 1e6f && fabsf(yaw) < 1e6f)) {
                LOGF("rx rejected (out of range): %s", q);
                nf = 0;
            }
            if (nf >= 3) {
                int w = 1;
                // the GSC re-creates these strings at every mission start: read the pointer each time
                char* s0 = CvStr(px, 12); char* s1 = CvStr(py, 12); char* s2 = CvStr(pz, 12);
                if (!s0 || !s1 || !s2) w = 0;
                else __try {
                    sprintf(s0, "%.3f", x);
                    sprintf(s1, "%.3f", y);
                    sprintf(s2, "%.3f", z);
                } __except (EXCEPTION_EXECUTE_HANDLER) { w = 0; LOGF("write fault"); }
                if (nf >= 4 && pyaw) {   // heap string only, same rule as the position cvars
                    char* yp = CvStr(pyaw, 12);
                    if (yp) {
                        __try { sprintf(yp, "%.3f", yaw); }
                        __except (EXCEPTION_EXECUTE_HANDLER) { LOGF("yaw write fault"); }
                    }
                }
                if (w && (n < 3 || n % 100 == 0))
                    LOGF("apply %d (pkt %ld): %.2f %.2f %.2f str='%s'", n, pk, x, y, z, s0);
                n++;
            }
        }
        // link summary every 5 s, and a note when the peer goes quiet or comes back
        if (++stattick >= 250) {
            stattick = 0;
            EnterCriticalSection(&L);
            long rn = g_rttN; double rmin = g_rttMin, rmax = g_rttMax, ravg = rn ? g_rttSum / rn : 0, gap = g_maxGapMs;
            g_rttMin = 1e9; g_rttMax = 0; g_rttSum = 0; g_rttN = 0; g_maxGapMs = 0;
            LeaveCriticalSection(&L);
            LOGF("stats tx=%ld rx=%ld echo=%ld lost=%ld reordered=%ld | rtt ms min/avg/max=%.1f/%.1f/%.1f (n=%ld) | max rx gap %.0f ms",
                 (long)g_txn, (long)g_rxn, (long)g_echo, (long)g_lost, (long)g_reord,
                 rn ? rmin : 0.0, ravg, rmax, rn, gap);
        }
        {
            double nowm = NowMs();
            EnterCriticalSection(&L);
            double lr = g_lastRxMs;
            LeaveCriticalSection(&L);
            if (lr > 0) {
                int quiet = (nowm - lr) > 5000.0;
                if (quiet && !peerQuiet) { LOGF("peer silent for > 5 s (no packets received)"); peerQuiet = 1; }
                else if (!quiet && peerQuiet) { LOGF("peer is sending again"); peerQuiet = 0; }
            }
        }
        // readback is logged on its own timer: after the packets stop nothing else is printed,
        // and that is exactly when the GSC writes its "removed" marker
        if (++rbtick >= 50) {   // ~1 s
            rbtick = 0;
            char rb[96];
            rb[0] = 0;
            if (prx) {
                void* sp = (void*)(*(DWORD*)(prx + 8));
                if (sp) RStr((DWORD)(ULONG_PTR)sp, rb, sizeof rb);   // byte-wise: a block copy fails at a page edge
            }
            char cur[40];
            LOGF("gsc readback coop_rx='%s' (cvar string now '%s')", rb, CvPeek(px, cur, sizeof cur));
        }
        Sleep(20);
    }
    if (tx != INVALID_SOCKET) closesocket(tx);
    return 0;                                // F is closed by CoopStop, after every thread has left
}

// Instant dvar lookup through the engine's own Dvar_FindVar (0x431260, cdecl (const char*) -> dvar* or 0; ENGINE.md section 3).
// The call is guarded (SEH) and the result is checked: inside the image range and its name at +0 must match. 0 = not (yet) there.
static DWORD FastDvar(const char* name) {
    DWORD r = 0;
    __try { r = (DWORD)(ULONG_PTR)((void* (__cdecl*)(const char*))(ULONG_PTR)0x00431260u)(name); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (r < 0x00400000u || r > 0x01CCA000u) return 0;
    DWORD np = 0; char nm[40];
    if (!SC(&np, (void*)(ULONG_PTR)r, 4) || np < 0x00010000u) return 0;
    RStr(np, nm, sizeof nm);
    return !strcmp(nm, name) ? r : 0;
}

// Menu control: the in-game Co-op menu sets the dvars coop_cmd ("host" | "join" | "off") and coop_ip; this thread
// applies a change of that pair. The dvars do not exist until the menu has been opened and a value set, so it looks
// for them every ~2.5 s (a scan of the game's memory, silent) and then only reads their strings (read-only).
/* Cbuf_AddText (0x41AFC0, usercall: EAX = the text, which must end with a newline): queues a console command, here "map <level>
" - exactly what
   the mission-select menu of the game does. */
static void CbufAddRaw(const char* text) {
    __asm {
        push ebx
        push esi
        push edi
        mov eax, text
        mov ecx, 0x0041AFC0
        call ecx
        pop edi
        pop esi
        pop ebx
    }
}
static int CbufAdd(const char* text) {
    __try { CbufAddRaw(text); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}
static int DvarInt(const char* name, int* out) {   /* int dvar through the engine lookup: value at +8; 0 = the dvar does not exist yet */
    DWORD d = FastDvar(name); DWORD v = 0;
    if (!d || !SC(&v, (void*)(ULONG_PTR)(d + 8), 4)) return 0;
    *out = (int)v; return 1;
}
/* Mission follow: the host announces the mission it is in ("M <name>" every 2 s); the guest starts the same one by itself. In a mission the proven
   script path is used (changelevel), in the menus the console command. At most once per 25 s, and never when the guest is already there. */
static int SoloMirror(void);
static int SaveBase(char* out, size_t cap);
/* START BARRIER (pause based): the script announces the mission with coop_barrier_begin; the game is paused with the console command "pause" (it stops every script
   and the whole world, no matter what the script does) and released by THIS thread, in real time, when the partner reports "ready" for the same mission and
   difficulty, or after 20 s. (Slowing the game with timescale is locked without cheats; waiting inside main() breaks the level set-up.) */
static volatile LONG g_bHold = 0, g_bPaused = 0; static LONG g_bReadyAt = 0, g_bDeadline = 0, g_bBegin = 0; static char g_bMap[48]; static int g_bSkill = 0;
static int PeerReadyState(const char* mp, int sk, DWORD since) {
    RdyEv e; EnterCriticalSection(&L); e = g_rdyPeer; LeaveCriticalSection(&L);
    if (!e.tick || GetTickCount() - e.tick >= 3000) return 0;
    if (since && (LONG)(e.tick - since) < 0) return 0;          /* a report from before this barrier began */
    return strcmp(e.map, mp) ? 2 : (e.skill == sk ? 1 : 3);
}
static int SetPaused(int on) {                       /* the engine has no "pause" command: its own pause state is the variable cl_paused (int at +8 of the dvar struct) */
    DWORD d = FastDvar("cl_paused"); DWORD v = 0;
    if (!d || !SC(&v, (void*)(ULONG_PTR)(d + 8), 4)) return 0;
    int ok = 1;
    __try { *(volatile DWORD*)(ULONG_PTR)(d + 8) = on ? 1 : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    LOGF("barrier: cl_paused %lu -> %d: %s", (unsigned long)v, on ? 1 : 0, ok ? "written" : "FAILED");
    return ok;
}
static void BarrierRelease(const char* why) {
    int p = 0; DvarInt("cl_paused", &p);
    if (g_bPaused && p) SetPaused(0);
    g_bPaused = 0; g_bHold = 0;
    LOGF("barrier: GO (%s)", why);
}
static void BarrierTick(void) {
    if (!g_bHold) return;
    LONG now = (LONG)GetTickCount();
    if (g_bPaused) {                                    /* the engine may reset cl_paused while the level comes up: put it back */
        int pp = 0; DvarInt("cl_paused", &pp);
        if (!pp) { static LONG lastLog = 0; if ((LONG)(now - lastLog) > 2000) { lastLog = now; LOGF("barrier: the engine dropped the pause, set again"); } SetPaused(1); }
    }
    if (!g_rdySet && now - g_bReadyAt >= 0) {
        EnterCriticalSection(&L); strcpy(g_rdyMap, g_bMap); g_rdySkill = g_bSkill; g_rdySet = 1; g_rdySetTick = now; LeaveCriticalSection(&L);
        LOGF("barrier: this PC is ready in '%s'", g_bMap);
    }
    if (g_rdySet) {
        int r = PeerReadyState(g_bMap, g_bSkill, (DWORD)g_bBegin);
        if (r == 1) { BarrierRelease("partner ready"); return; }
        if (r == 3) { BarrierRelease("the partner is ready, on another difficulty"); return; }
    }
    if (now - g_bDeadline >= 0) BarrierRelease("timeout, no partner report");
}
static void MissionFollow(int tick) {
    static LONG lastFollow = 0;
    if (g_deathLoadAt && (LONG)(GetTickCount() - g_deathLoadAt) >= 0) {   /* the delayed explicit load after the partner's death */
        char c[160]; strncpy(c, g_deathLoadCmd, sizeof c - 1); c[sizeof c - 1] = 0; g_deathLoadAt = 0;
        LOGF("follow: loading the shared checkpoint now: %s", CbufAdd(c) ? "queued" : "FAILED");
    }
    {   /* timeline of the engine's own mission state (for the earliest moment a level change can be announced) */
        static int lrun = -9; static char lmap[40]; int run = 0; DWORD dm = FastDvar("mapname"); char mp[40] = "";
        DvarInt("sv_running", &run); if (dm) CvPeek(dm, mp, sizeof mp);
        if (run != lrun || strcmp(mp, lmap)) {
            static char lastMission[48];
            if (mp[0] && strcmp(mp, lastMission)) { if (lastMission[0]) { g_curMap[0] = 0; g_curN = -1; g_curLoadName[0] = 0; } strncpy(lastMission, mp, sizeof lastMission - 1); lastMission[sizeof lastMission - 1] = 0; }
            lrun = run; strcpy(lmap, mp); LOGF("dvar watch: sv_running=%d mapname='%s'", run, mp);
            if (g_rdySet && (LONG)(GetTickCount() - (DWORD)g_rdySetTick) > 1500) { g_rdySet = 0; LOGF("barrier: the mission changed or reloaded, this PC is no longer 'ready'"); }
        }
    }
    if (g_udpSock == INVALID_SOCKET || !g_peerKnown) return;
    if (g_mode == 1) {                                         /* host: announce at every change at once, and every 2 s */
        static char lastSent[40]; static LONG lastSentMs = 0;
        int run = 0; DWORD dm = FastDvar("mapname"); char mp0[48] = "", mp[48] = "";
        if (DvarInt("sv_running", &run) && run && dm) {
            CvPeek(dm, mp0, sizeof mp0);
            /* a briefing (slide show / movie map) names the mission that follows it (level.levelToLoad in the game's own briefing scripts): the guest starts
               that mission NOW and skips the briefing, so its slower load runs in parallel with the host's briefing instead of after it */
            { static const char* const brief[][2] = { {"movie_africa", "decoytrenches"}, {"movie_eastern", "moscow"}, {"movie_europe", "duhoc_assault"},
                {"slideshow_bergstein", "bergstein"}, {"slideshow_caen", "beltot"}, {"slideshow_earlygrad", "tankhunt"}, {"slideshow_lategrad", "downtown_assault"},
                {"slideshow_libya", "libya"}, {"slideshow_rhine", "rhine"}, {"slideshow_tunisia", "toujane_ride"} };
              strcpy(mp, mp0);
              for (int bi = 0; bi < (int)(sizeof brief / sizeof brief[0]); bi++) if (!strcmp(mp0, brief[bi][0])) { strcpy(mp, brief[bi][1]); break; } }
            if (strcmp(mp, lastSent) || tick % 10 == 0) { strncpy(lastSent, mp, sizeof lastSent - 1); lastSent[sizeof lastSent - 1] = 0; lastSentMs = (LONG)GetTickCount(); }
            else mp[0] = 0;                                    /* nothing new and not the 2 s repeat */
            if (mp[0] && strncmp(mp, "movie", 5) && strncmp(mp, "slideshow", 9) && strncmp(mp, "credits", 7) && strncmp(mp, "cinematic", 9)) {
                int hs = -1; DvarInt("g_gameskill", &hs);
                char pkt[64]; int len = (hs >= 0 && hs <= 3) ? _snprintf(pkt, sizeof pkt, "M %s %d", mp, hs) : _snprintf(pkt, sizeof pkt, "M %s", mp);
                struct sockaddr_in to; memset(&to, 0, sizeof to);
                to.sin_family = AF_INET; to.sin_addr.s_addr = g_peerAddr; to.sin_port = htons((u_short)g_peerPort);
                sendto(g_udpSock, pkt, len, 0, (struct sockaddr*)&to, sizeof to);
                static char shown[40];
                if (strcmp(shown, mp)) { strcpy(shown, mp); LOGF("follow: announcing the mission '%s' to the guest", mp); }
            }
        }
    }
    if (g_deathPending && tick % 5 == 0) {                       /* either side: the peer died, so both continue from the last checkpoint */
        static LONG lastDeath = 0; LONG now = (LONG)GetTickCount(); int run = 0;
        g_deathPending = 0;
        DvarInt("sv_running", &run);
        if (SoloMirror()) { LOGF("follow: the peer died: solo test (the peer is this game), the game reloads itself, nothing to do"); return; }
        if (_stricmp(g_follow, "off") && run && now - lastDeath > 6000) {
            lastDeath = now;
            {   /* explicit: the guest loads coopshared (the host's checkpoint), the host its newest checkpoint; loadgame_continue read a different file */
                char cmd[160] = "loadgame_continue" ; char why[120];
                if (g_curLoadName[0]) _snprintf(cmd, sizeof cmd, "loadgame autosave/%s", g_curLoadName);
                else if (g_mode == 2) {
                    char cp[MAX_PATH], base[MAX_PATH];
                    if (SaveBase(base, sizeof base)) { _snprintf(cp, sizeof cp, "%ssave\\autosave\\coopshared.svg", base); cp[sizeof cp - 1] = 0; if (GetFileAttributesA(cp) != INVALID_FILE_ATTRIBUTES) strcpy(cmd, "loadgame autosave/coopshared"); }
                }
                _snprintf(why, sizeof why, "%s", cmd); why[sizeof why - 1] = 0;
                strncat(cmd, "\n", sizeof cmd - strlen(cmd) - 1);
                strncpy(g_deathLoadCmd, cmd, sizeof g_deathLoadCmd - 1); g_deathLoadCmd[sizeof g_deathLoadCmd - 1] = 0;
                InterlockedExchange(&g_hostFN, 0); InterlockedExchange(&g_replayIdx, -1);   /* both worlds go back to the checkpoint: what was sent after it is undone */
                g_deathLoadAt = GetTickCount() + 3500; if (!g_deathLoadAt) g_deathLoadAt = 1;   /* the explicit load of the shared checkpoint while the engine's own death screen is shown */
                InterlockedExchange(&g_peerDeathFx, 1);
                LOGF("follow: the peer died: the local player is killed by the script (the original death screen), the shared checkpoint is loaded by name in 3.5 s: %s", why);
            }
        } else LOGF("follow: the peer died: no reload (%s)", !_stricmp(g_follow, "off") ? "follow is off" : (!run ? "no mission here" : "just reloaded"));
    }
    if (g_mode == 2 && (tick % 5 == 0 || g_hostMapFresh) && g_hostMap[0]) {         /* guest */
        g_hostMapFresh = 0;
        static LONG pendSince = 0;
        LONG now = (LONG)GetTickCount();
        if (pendSince && now - pendSince > 3000) {               /* the script did not take the request (a mission without our script, e.g. a loaded save): use the console */
            char pl[40], cmd[64]; EnterCriticalSection(&L); strcpy(pl, g_pendLevel); g_pendLevel[0] = 0; LeaveCriticalSection(&L);
            pendSince = 0;
            if (pl[0]) { _snprintf(cmd, sizeof cmd, "map %s\n", pl); LOGF("follow: the script did not change the level: console command \"map %s\" %s", pl, CbufAdd(cmd) ? "queued" : "FAILED"); }
        }
        if (now - g_hostMapMs > 8000) return;
        if (!_stricmp(g_follow, "off")) return;                 /* g_follow follows the menu dvar (WIN thread) and is remembered between runs */
        char want[40]; EnterCriticalSection(&L); strcpy(want, g_hostMap); LeaveCriticalSection(&L);
        int run = 0; DWORD dm = FastDvar("mapname"); char cur[40] = "";
        DvarInt("sv_running", &run);
        if (run && dm) CvPeek(dm, cur, sizeof cur);
        if (run && !strcmp(cur, want)) return;                  /* already there */
        { static char lastTarget[40];                            /* the same target is not asked again within 25 s (it is loading); a NEW target is followed at once (after 4 s) */
          if (!strcmp(lastTarget, want) ? now - lastFollow < 25000 : now - lastFollow < 4000) return;
          strcpy(lastTarget, want); }
        lastFollow = now;
        if (run && g_txn > 0) {                                  /* a mission with our script: the script changes the level */
            EnterCriticalSection(&L); strcpy(g_pendLevel, want); LeaveCriticalSection(&L);
            pendSince = now;
            LOGF("follow: the host is in '%s', this game in '%s': changing the level by script", want, cur);
        } else {
            char cmd[64]; _snprintf(cmd, sizeof cmd, "map %s\n", want);
            LOGF("follow: the host is in '%s' (this game: %s): console command \"map %s\" %s", want, run ? cur : "no mission", want, CbufAdd(cmd) ? "queued" : "FAILED");
        }
    }
}

/* Ctrl+V in the CO-OP screen: the engine's edit field cannot paste. While the game window has the focus and Ctrl+V goes down, a clipboard text that is
   exactly an IPv4 address (spaces and line breaks around it are ignored) is written into the dvar coop_ip by the console command  set coop_ip "a.b.c.d"
   (the dvar exists once the CO-OP screen has been opened; elsewhere the key does nothing). The command is queued ~120 ms after the key so that it
   comes after the 'v' the edit field may have typed itself. */
static void PasteIpPoll(void) {
    static int wasDown = 0; static char pend[24]; static DWORD pendAt = 0;
    int down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState('V') & 0x8000);
    if (down && !wasDown) {
        wasDown = 1;
        HWND fg = GetForegroundWindow(); DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        if (pid == GetCurrentProcessId() && FastDvar("coop_ip") && OpenClipboard(NULL)) {
            HANDLE h = GetClipboardData(CF_TEXT);
            if (h) {
                const char* s = (const char*)GlobalLock(h);
                if (s) {
                    char t[256]; size_t n = 0;
                    while (*s && n < sizeof t - 1) t[n++] = *s++;
                    t[n] = 0;
                    GlobalUnlock(h);
                    /* the first run of digits and dots that is a valid IPv4 address: "26.1.2.3", "26.1.2.3:27201", "Host IP: 26.1.2.3" all work */
                    char found[24]; found[0] = 0; size_t i = 0, tl = strlen(t);
                    while (i < tl && !found[0]) {
                        if (!((t[i] >= '0' && t[i] <= '9') || t[i] == '.')) { i++; continue; }
                        size_t j = i; while (j < tl && ((t[j] >= '0' && t[j] <= '9') || t[j] == '.')) j++;
                        size_t b = i, e = j; while (b < e && t[b] == '.') b++; while (e > b && t[e - 1] == '.') e--;
                        if (e > b && e - b < sizeof found) { memcpy(found, t + b, e - b); found[e - b] = 0; if (!ValidIPv4(found)) found[0] = 0; }
                        i = j;
                    }
                    if (found[0]) { strncpy(pend, found, sizeof pend - 1); pend[sizeof pend - 1] = 0; pendAt = GetTickCount() + 120; if (!pendAt) pendAt = 1; }
                    else LOGF("paste: no IPv4 address in the clipboard text (%d characters), ignored", (int)tl);
                }
            }
            CloseClipboard();
        }
    } else if (!down) wasDown = 0;
    if (pendAt && (LONG)(GetTickCount() - pendAt) >= 0) {
        char cmd[64]; _snprintf(cmd, sizeof cmd - 1, "set coop_ip \"%s\"\n", pend); cmd[sizeof cmd - 1] = 0;
        LOGF("paste: coop_ip <- %s (console command %s)", pend, CbufAdd(cmd) ? "queued" : "FAILED");
        pendAt = 0;
    }
}

static DWORD WINAPI CTRL(LPVOID p) {
    (void)p;
    char lastCmd[40] = "", lastIp[80] = "";
    DWORD cmdC = 0, ipC = 0;
    for (int tick = 0;; tick++) {
        for (int k = 0; k < 5; k++) { SleepQ(20); PasteIpPoll(); }   // 100 ms in all; the key is polled every 20 ms so a short Ctrl+V is not missed
        if (g_quit) return 0;
        if (g_fileMode) return 0;                       // the launcher's files drive this instance instead
        if (!cmdC || !ipC) {                            // the engine's own lookup first: instant, no memory scan
            DWORD a = cmdC ? cmdC : FastDvar("coop_cmd"), b = ipC ? ipC : FastDvar("coop_ip");
            if (a && b && (!cmdC || !ipC)) LOGF("menu: dvars found by the engine lookup (coop_cmd %08lX, coop_ip %08lX)", a, b);
            cmdC = a; ipC = b;
        }
        if ((!cmdC || !ipC) && tick % 20 == 0) {                 // fallback: scan
            Collect();
            if (!cmdC) cmdC = ResolveCvar("coop_cmd", 3);
            if (!ipC) ipC = ResolveCvar("coop_ip", 3);
            if (cmdC && ipC) LOGF("menu: dvars found (coop_cmd %08lX, coop_ip %08lX)", cmdC, ipC);
        }
        if (!cmdC) continue;
        char cmd[40], ip[80];
        CvPeek(cmdC, cmd, sizeof cmd);
        if (ipC) CvPeek(ipC, ip, sizeof ip); else ip[0] = 0;
        for (char* e = cmd + strlen(cmd); e > cmd && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
        for (char* e = ip + strlen(ip); e > ip && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
        if (!strcmp(cmd, lastCmd) && !strcmp(ip, lastIp)) continue;
        strcpy(lastCmd, cmd); strcpy(lastIp, ip);
        LOGF("menu: coop_cmd='%s' coop_ip='%s'", cmd, ip);
        /* "idle" is what the menu puts there when it opens (so the dvar exists early): no change */
        if (!_stricmp(cmd, "host")) ApplyMode(1, NULL);
        else if (!_stricmp(cmd, "join")) ApplyMode(2, ip);
        else if (!_stricmp(cmd, "off")) ApplyMode(0, NULL);
    }
}

// Borderless window: the game's window (class "CoD2") of this process loses its frame and is centered on its monitor
// at the same client size. Menu dvar coop_borderless = "on" | "off"; the choice is remembered in coop_window.txt.
// Only Win32 window calls: no engine address is involved.
static HWND g_gameWnd;
static BOOL CALLBACK FindWnd(HWND h, LPARAM l) {
    DWORD pid = 0; char cls[32];
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    GetClassNameA(h, cls, sizeof cls);
    if (strcmp(cls, "CoD2")) return TRUE;
    *(HWND*)l = h; return FALSE;
}
static LONG g_origStyle, g_origEx; static RECT g_origRect; static int g_haveOrig;
static void ApplyBorderless(int on) {
    HWND h = NULL; EnumWindows(FindWnd, (LPARAM)&h);
    if (!h) return;
    RECT r, c; GetWindowRect(h, &r); GetClientRect(h, &c);
    LONG st = GetWindowLongA(h, GWL_STYLE), ex = GetWindowLongA(h, GWL_EXSTYLE);
    LOGF("window: %s requested; now style %08lX rect %ld,%ld %ldx%ld client %ldx%ld", on ? "borderless" : "normal",
         st, r.left, r.top, r.right - r.left, r.bottom - r.top, c.right, c.bottom);
    if (on) {
        if (st & WS_CAPTION) { g_origStyle = st; g_origEx = ex; g_origRect = r; g_haveOrig = 1; }
        else {
            MONITORINFO m2; m2.cbSize = sizeof m2;
            GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &m2);
            LOGF("window: already borderless, covering the whole monitor above the taskbar");
            SetWindowPos(h, HWND_TOPMOST, m2.rcMonitor.left, m2.rcMonitor.top, m2.rcMonitor.right - m2.rcMonitor.left,
                         m2.rcMonitor.bottom - m2.rcMonitor.top, SWP_NOACTIVATE);
            goto done;
        }
        MONITORINFO mi; mi.cbSize = sizeof mi;
        GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
        /* the whole monitor: D3D stretches the 1920x1071 back buffer over the client area, so no strip is left */
        int cw = mi.rcMonitor.right - mi.rcMonitor.left, ch = mi.rcMonitor.bottom - mi.rcMonitor.top;
        int x = mi.rcMonitor.left, y = mi.rcMonitor.top;
        SetWindowLongA(h, GWL_STYLE, (st & ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU)) | WS_POPUP | WS_VISIBLE);
        SetWindowLongA(h, GWL_EXSTYLE, ex & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));
        SetWindowPos(h, HWND_TOPMOST, x, y, cw, ch, SWP_FRAMECHANGED | SWP_SHOWWINDOW);   /* above the taskbar */
    } else {
        if (st & WS_CAPTION) { LOGF("window: already normal"); return; }
        LONG nst = g_haveOrig ? g_origStyle : (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE);
        LONG nex = g_haveOrig ? g_origEx : ex;
        SetWindowLongA(h, GWL_STYLE, nst);
        SetWindowLongA(h, GWL_EXSTYLE, nex);
        RECT w = { 0, 0, c.right, c.bottom };
        AdjustWindowRectEx(&w, (DWORD)nst, FALSE, (DWORD)nex);
        int ww = w.right - w.left, wh = w.bottom - w.top;
        if (g_haveOrig) { ww = g_origRect.right - g_origRect.left; wh = g_origRect.bottom - g_origRect.top; }
        MONITORINFO mn; mn.cbSize = sizeof mn;
        GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mn);
        /* centered on its monitor: the framed window is a few pixels wider than the screen, so splitting the excess keeps
           it off the neighbouring monitor as far as possible */
        int x = mn.rcMonitor.left + ((mn.rcMonitor.right - mn.rcMonitor.left) - ww) / 2;
        int y = g_haveOrig ? g_origRect.top : mn.rcMonitor.top;
        SetWindowPos(h, HWND_NOTOPMOST, x, y, ww, wh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
done:
    GetWindowRect(h, &r); GetClientRect(h, &c);
    LOGF("window: after: style %08lX rect %ld,%ld %ldx%ld client %ldx%ld", GetWindowLongA(h, GWL_STYLE),
         r.left, r.top, r.right - r.left, r.bottom - r.top, c.right, c.bottom);
}
/* Menu status line: the menu creates the dvar coop_status as 40 blanks, this thread repaints it in place (same rule as every
   other write: heap string only, the length is kept). */
/* language of the menu texts: the engine's loc_language (0 en, 1 fr, 2 de, 3 it, 4 es, 6 ru) -> row of COOP_TXT; anything else English */
static int CoopLang(void) {
    static DWORD d = 0; static int last = -2;
    if (!d) d = FastDvar("loc_language");
    int row = 0;
    { char lp[MAX_PATH]; PathIn(lp, sizeof lp, "coop_lang.txt"); FILE* lf = fopen(lp, "r");   /* test aid: a row number 0..5 forces the text set */
      if (lf) { int v = -1; if (fscanf(lf, "%d", &v) == 1 && v >= 0 && v <= 5) { fclose(lf); static int shown = 0; if (!shown) { shown = 1; LOGF("menu language: FORCED text set %d by coop_lang.txt", v); } return v; } fclose(lf); } }
    if (d) {
        DWORD v = 0;
        if (SC(&v, (void*)(ULONG_PTR)(d + 8), 4)) { switch ((int)v) { case 1: row = 1; break; case 2: row = 2; break; case 3: row = 3; break; case 4: row = 4; break; case 6: row = 5; break; default: row = 0; } if ((int)v != last) { LOGF("menu language: loc_language=%d -> text set %d", (int)v, row); last = (int)v; } }
    }
    return row;
}
static void PaintStatus(DWORD dv) {
    char t[64];
    double now = NowMs(); EnterCriticalSection(&L); double lr = g_lastRxMs; LeaveCriticalSection(&L);
    int live = lr > 0 && (now - lr) < 2000.0;
    LONG m = g_mode;
    int L = CoopLang();
    int verBad = g_verBadMs && (LONG)(GetTickCount() - (DWORD)g_verBadMs) < 6000;
    if (g_bHold) strcpy(t, COOP_TXT[L][CT_WAITING]);       /* the loading screen shows this line while the game is held for the partner */
    else if (verBad && m != 0) strcpy(t, "VERSION MISMATCH - update both PCs");
    else if (m == 0) strcpy(t, COOP_TXT[L][CT_IDLE]);
    else if (m == 1) {
        int hello = g_helloMs && (LONG)(GetTickCount() - (DWORD)g_helloMs) < 3000;
        strcpy(t, COOP_TXT[L][!g_peerKnown ? CT_HOST_WAIT : live ? CT_HOST_CONN : hello ? CT_HOST_FOUND : CT_HOST_SILENT]);
    }
    else if (!g_peerKnown) strcpy(t, COOP_TXT[L][CT_JOIN_LOOK]);
    else if (live) _snprintf(t, sizeof t - 1, COOP_TXT[L][CT_JOIN_LINK], g_peerIp);
    else if (g_txn == 0) strcpy(t, COOP_TXT[L][CT_JOIN_START]);   /* nothing is sent before a mission runs */
    else strcpy(t, COOP_TXT[L][CT_JOIN_NOANS]);
    t[sizeof t - 1] = 0;
    char* d = CvStr(dv, 40);
    if (!d) return;
    { static char lastT[64]; if (strcmp(lastT, t)) { LOGF("status text -> '%s'", t); strcpy(lastT, t); } }   /* latency measurement: compare with the "menu: coop_cmd" line */
    char out[41]; _snprintf(out, sizeof out, "%-40.40s", t); out[40] = 0;
    __try { if (memcmp(d, out, 40)) memcpy(d, out, 40); } __except (EXCEPTION_EXECUTE_HANDLER) { LOGF("status write fault"); }
}
// ---- Raw mouse input -------------------------------------------------------------------------------------------------------------------
// The engine polls the mouse once per frame (0x44F0A0): GetCursorPos, delta against the window centre (0x825BC4 x, 0x825BBC y), SetCursorPos back
// to the centre. Both API calls go through two function pointers in the exe's data (0x56A270 GetCursorPos, 0x56A274 SetCursorPos). With raw
// input on, those two pointers are swapped for ours: GetCursorPos returns "centre + the raw movement since the last call" (unaccelerated,
// independent of the cursor, the window size and the Windows pointer speed), SetCursorPos still recentres the OS cursor. Nothing in the game's
// code is patched; the pointers are only swapped when they hold exactly user32's functions, and they are restored on stop.
#define RAW_PTR_GET 0x0056A270u
#define RAW_PTR_SET 0x0056A274u
#define RAW_CENTRE_X 0x00825BC4u
#define RAW_CENTRE_Y 0x00825BBCu
static volatile LONG g_rawDx, g_rawDy, g_rawEvents, g_rawOn;
static DWORD g_rawOrigGet, g_rawOrigSet;
static HANDLE g_rawThread; static DWORD g_rawThreadId;

static LRESULT CALLBACK RawWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_INPUT) {
        UINT sz = 0;
        GetRawInputData((HRAWINPUT)l, RID_INPUT, NULL, &sz, sizeof(RAWINPUTHEADER));
        if (sz && sz <= 256) {
            BYTE buf[256];
            if (GetRawInputData((HRAWINPUT)l, RID_INPUT, buf, &sz, sizeof(RAWINPUTHEADER)) == sz) {
                RAWINPUT* ri = (RAWINPUT*)buf;
                if (ri->header.dwType == RIM_TYPEMOUSE && !(ri->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                    InterlockedExchangeAdd(&g_rawDx, ri->data.mouse.lLastX);
                    InterlockedExchangeAdd(&g_rawDy, ri->data.mouse.lLastY);
                    InterlockedIncrement(&g_rawEvents);
                }
            }
        }
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}
static DWORD WINAPI RAWTHREAD(LPVOID p) {
    (void)p;
    WNDCLASSA wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = RawWndProc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "coop_rawmouse_sink";
    RegisterClassA(&wc);
    HWND h = CreateWindowExA(0, wc.lpszClassName, "", WS_POPUP, 0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
    if (!h) { LOGF("raw mouse: window creation failed (%lu)", GetLastError()); return 1; }
    RAWINPUTDEVICE rid; rid.usUsagePage = 1; rid.usUsage = 2; rid.dwFlags = RIDEV_INPUTSINK; rid.hwndTarget = h;
    if (!RegisterRawInputDevices(&rid, 1, sizeof rid)) { LOGF("raw mouse: RegisterRawInputDevices failed (%lu)", GetLastError()); DestroyWindow(h); return 1; }
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    rid.dwFlags = RIDEV_REMOVE; rid.hwndTarget = NULL; RegisterRawInputDevices(&rid, 1, sizeof rid);
    DestroyWindow(h); UnregisterClassA(wc.lpszClassName, wc.hInstance);
    return 0;
}
static BOOL WINAPI MyGetCursorPos(LPPOINT pt) {
    DWORD cx = 0, cy = 0;
    SC(&cx, (void*)(ULONG_PTR)RAW_CENTRE_X, 4); SC(&cy, (void*)(ULONG_PTR)RAW_CENTRE_Y, 4);
    LONG dx = InterlockedExchange(&g_rawDx, 0), dy = InterlockedExchange(&g_rawDy, 0);
    if (pt) { pt->x = (LONG)cx + dx; pt->y = (LONG)cy + dy; }
    return TRUE;
}
static BOOL WINAPI MySetCursorPos(int x, int y) { return SetCursorPos(x, y); }
static void RemoveRaw(void) {
    if (!g_rawOn) return;
    DWORD old;
    if (VirtualProtect((void*)(ULONG_PTR)RAW_PTR_GET, 8, PAGE_READWRITE, &old)) {
        *(DWORD*)(ULONG_PTR)RAW_PTR_GET = g_rawOrigGet; *(DWORD*)(ULONG_PTR)RAW_PTR_SET = g_rawOrigSet;
        VirtualProtect((void*)(ULONG_PTR)RAW_PTR_GET, 8, old, &old);
    }
    g_rawOn = 0;
    if (g_rawThread) { PostThreadMessageA(g_rawThreadId, WM_QUIT, 0, 0); WaitForSingleObject(g_rawThread, 2000); CloseHandle(g_rawThread); g_rawThread = NULL; }
    LOGF("raw mouse: off (pointers restored, %ld raw events seen)", (long)g_rawEvents);
}
static void InstallRaw(void) {
    if (g_rawOn) return;
    HMODULE u = GetModuleHandleA("user32.dll");
    DWORD wantGet = (DWORD)(ULONG_PTR)GetProcAddress(u, "GetCursorPos"), wantSet = (DWORD)(ULONG_PTR)GetProcAddress(u, "SetCursorPos");
    DWORD curGet = 0, curSet = 0;
    if (!SC(&curGet, (void*)(ULONG_PTR)RAW_PTR_GET, 4) || !SC(&curSet, (void*)(ULONG_PTR)RAW_PTR_SET, 4)) { LOGF("raw mouse: engine pointers unreadable, not installed"); return; }
    if (curGet != wantGet || curSet != wantSet) {
        LOGF("raw mouse: NOT installed, the engine pointers are not user32's functions (GetCursorPos engine %08lX user32 %08lX, SetCursorPos engine %08lX user32 %08lX)", curGet, wantGet, curSet, wantSet);
        return;
    }
    g_rawOrigGet = curGet; g_rawOrigSet = curSet; g_rawDx = g_rawDy = 0; g_rawEvents = 0;
    g_rawThread = CreateThread(0, 0, RAWTHREAD, 0, 0, &g_rawThreadId);
    if (!g_rawThread) { LOGF("raw mouse: thread failed"); return; }
    DWORD old;
    if (!VirtualProtect((void*)(ULONG_PTR)RAW_PTR_GET, 8, PAGE_READWRITE, &old)) { LOGF("raw mouse: VirtualProtect failed (%lu)", GetLastError()); PostThreadMessageA(g_rawThreadId, WM_QUIT, 0, 0); return; }
    *(DWORD*)(ULONG_PTR)RAW_PTR_GET = (DWORD)(ULONG_PTR)MyGetCursorPos; *(DWORD*)(ULONG_PTR)RAW_PTR_SET = (DWORD)(ULONG_PTR)MySetCursorPos;
    VirtualProtect((void*)(ULONG_PTR)RAW_PTR_GET, 8, old, &old);
    g_rawOn = 1;
    LOGF("raw mouse: ON (engine pointers swapped; raw input registered)");
}

/* ---- shared checkpoints (both directions) ----
   WHOEVER reaches a checkpoint first writes it: each PC watches its own autosave folder (main\players\<id>\save\autosave\*.svg, ~2 MB) and sends every
   new checkpoint to the other PC over TCP (port 27203); the receiver writes it to coopshared.svg, over the file its sv_lastSaveGame points to and over
   save\<map>.svg. A checkpoint older than the newest one this PC already knows (same map, lower number: "map.svg" = 0, "map-1.svg" = 1, ...) is ignored,
   and a file whose content is what was just received is never sent back (CRC). After a death both PCs load the newest known checkpoint by name. */
#define SAVE_PORT 27203
#define SAVE_MAX  (12 * 1024 * 1024)

static unsigned long Crc32Buf(const unsigned char* d, size_t n) {
    static unsigned long t[256]; static int init = 0;
    if (!init) { for (unsigned long i = 0; i < 256; i++) { unsigned long c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320UL ^ (c >> 1) : c >> 1; t[i] = c; } init = 1; }
    unsigned long c = 0xFFFFFFFFUL;
    for (size_t i = 0; i < n; i++) c = t[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFUL;
}

static int SaveBase(char* out, size_t cap) {          /* <game>\main\players\<profile id>\ */
    char p[MAX_PATH], id[48] = ""; PathIn(p, sizeof p, "main\\players\\active.txt");
    FILE* f = fopen(p, "r"); if (!f) return 0;
    if (fgets(id, sizeof id, f)) { size_t n = strlen(id); while (n && (id[n - 1] == '\r' || id[n - 1] == '\n' || id[n - 1] == ' ')) id[--n] = 0; }
    fclose(f);
    if (!id[0]) return 0;
    for (const char* c = id; *c; c++) if (!((*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || *c == '_' || *c == '-')) return 0;
    _snprintf(out, cap, "%smain\\players\\%s\\", g_dir, id); out[cap - 1] = 0;
    return 1;
}

static void CkptParse(const char* nm, char* map, size_t cap, int* n) {   /* "newvillers-1" -> map "newvillers", n 1; "newvillers" -> n 0 */
    strncpy(map, nm, cap - 1); map[cap - 1] = 0; *n = 0;
    char* dash = strrchr(map, '-');
    if (dash && dash[1] >= '0' && dash[1] <= '9') { *n = atoi(dash + 1); *dash = 0; }
}

static int SaveSendBuf(const char* name, const char* buf, long sz) {   /* header line + bytes to the other PC */
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;
    u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
    struct sockaddr_in to; memset(&to, 0, sizeof to);
    to.sin_family = AF_INET; to.sin_addr.s_addr = g_peerAddr; to.sin_port = htons(SAVE_PORT);
    connect(s, (struct sockaddr*)&to, sizeof to);
    fd_set w; FD_ZERO(&w); FD_SET(s, &w); struct timeval tv; tv.tv_sec = 5; tv.tv_usec = 0;
    int ok = select(0, NULL, &w, NULL, &tv) > 0;
    if (ok) {
        nb = 0; ioctlsocket(s, FIONBIO, &nb);
        DWORD tmo = 15000; setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tmo, sizeof tmo);
        char hdr[160]; int hl = _snprintf(hdr, sizeof hdr - 1, "COOPSAVE %s %ld\n", name, sz);
        ok = send(s, hdr, hl, 0) == hl;
        long sent = 0;
        while (ok && sent < sz) { int n = send(s, buf + sent, (int)(sz - sent > 32768 ? 32768 : sz - sent), 0); if (n <= 0) ok = 0; else sent += n; }
    }
    closesocket(s);
    return ok;
}

static void SaveReceive(SOCKET c) {                    /* one incoming checkpoint */
    DWORD tmo = 10000; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof tmo);
    char hdr[160]; int hl = 0;
    while (hl < (int)sizeof hdr - 1) { char ch; if (recv(c, &ch, 1, 0) != 1) return; if (ch == '\n') break; hdr[hl++] = ch; }
    hdr[hl] = 0;
    char nm[80]; long sz = 0;
    if (sscanf(hdr, "COOPSAVE %79s %ld", nm, &sz) != 2 || sz <= 0 || sz > SAVE_MAX) return;
    for (const char* q = nm; *q; q++) if (!((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9') || *q == '_' || *q == '-')) return;
    char* buf = (char*)malloc((size_t)sz); if (!buf) return;
    long got = 0;
    while (got < sz) { int n = recv(c, buf + got, (int)(sz - got > 65536 ? 65536 : sz - got), 0); if (n <= 0) break; got += n; }
    if (got != sz) { LOGF("checkpoint %s: receive incomplete (%ld of %ld bytes)", nm, got, sz); free(buf); return; }
    char rmap[96]; int rn; CkptParse(nm, rmap, sizeof rmap, &rn);
    /* the guest takes EVERY checkpoint of the host: the "-N" counters of the two PCs are not comparable (the guest numbered its own saves 1..12 while the host was at 9 far ahead,
       so the host's tankhunt-9 was thrown away and a death reloaded the guest's own early save) */
    if (g_mode != 2 && g_curMap[0] && !strcmp(rmap, g_curMap) && rn < g_curN) {      /* older than what this PC already has for this map */
        LOGF("checkpoint %s ignored: older than %s-%d that this PC already has", nm, g_curMap, g_curN); free(buf); return;
    }
    unsigned long crc = Crc32Buf((const unsigned char*)buf, (size_t)sz);
    char base[MAX_PATH]; if (!SaveBase(base, sizeof base)) { LOGF("checkpoint %s: no profile folder", nm); free(buf); return; }
    char tmp[MAX_PATH], shared[MAX_PATH];
    _snprintf(tmp, sizeof tmp, "%ssave\\autosave\\coopshared.tmp", base); tmp[sizeof tmp - 1] = 0;
    _snprintf(shared, sizeof shared, "%ssave\\autosave\\coopshared.svg", base); shared[sizeof shared - 1] = 0;
    FILE* f = fopen(tmp, "wb");
    if (!f) { LOGF("checkpoint %s: cannot write %s", nm, tmp); free(buf); return; }
    size_t wr = fwrite(buf, 1, (size_t)sz, f); fclose(f); free(buf);
    if (wr != (size_t)sz) { LOGF("checkpoint %s: write failed", nm); DeleteFileA(tmp); return; }
    /* every place the engine may reload from: coopshared.svg (loaded by name), the file sv_lastSaveGame points to, the profile save of the map */
    char last[96] = ""; DWORD dl = FastDvar("sv_lastSaveGame"); if (dl) CvPeek(dl, last, sizeof last);
    char target[MAX_PATH] = ""; int okTarget = 0;
    if (!strncmp(last, "players", 7) && !strstr(last, "..") && strlen(last) > 4 && !_stricmp(last + strlen(last) - 4, ".svg")) {
        okTarget = 1;
        for (const char* q = last; *q; q++) if (!((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9') || *q == '_' || *q == '-' || *q == '.' || *q == '\\' || *q == '/')) okTarget = 0;
        if (okTarget) { _snprintf(target, sizeof target, "%smain\\%s", g_dir, last); target[sizeof target - 1] = 0; for (char* q = target; *q; q++) if (*q == '/') *q = '\\'; }
    }
    char perMap[MAX_PATH]; _snprintf(perMap, sizeof perMap, "%ssave\\%s.svg", base, rmap); perMap[sizeof perMap - 1] = 0;
    g_lastRecvCrc = crc;                                /* set BEFORE the copies: the scanner must see these files as "received", never as new */
    int a = MoveFileExA(tmp, shared, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) ? 1 : 0;
    int b2 = okTarget ? (CopyFileA(shared, target, FALSE) ? 1 : 0) : -1;
    int c2 = CopyFileA(shared, perMap, FALSE) ? 1 : 0;
    strncpy(g_curMap, rmap, sizeof g_curMap - 1); g_curMap[sizeof g_curMap - 1] = 0; g_curN = rn;
    strcpy(g_curLoadName, "coopshared");
    LOGF("checkpoint received: %s (%ld bytes, crc32 %08lX) -> coopshared.svg %s; last save '%s' %s; save\\%s.svg %s", nm, sz, crc, a ? "ok" : "FAILED",
         last[0] ? last : "(unknown)", b2 < 0 ? "left alone" : (b2 ? "replaced" : "NOT replaced"), rmap, c2 ? "replaced" : "NOT replaced");
}

static DWORD WINAPI SAVESYNC(LPVOID p) {
    (void)p;
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls != INVALID_SOCKET) {
        BOOL one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
        struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(SAVE_PORT);
        u_long nb = 1;
        if (bind(ls, (struct sockaddr*)&a, sizeof a) == 0 && listen(ls, 2) == 0) ioctlsocket(ls, FIONBIO, &nb);
        else { closesocket(ls); ls = INVALID_SOCKET; }
    }
    char lastName[96] = ""; FILETIME lastTime; memset(&lastTime, 0, sizeof lastTime);
    char pendName[96] = ""; FILETIME pendTime; memset(&pendTime, 0, sizeof pendTime); DWORD pendSize = 0;
    int first = 1, linkPrev = 0;
    for (int tick = 0;; tick++) {
        SleepQ(100);
        if (g_quit) break;
        if (ls != INVALID_SOCKET) {                         /* both PCs take what the other sends (not in the solo test unless a test file is sent) */
            struct sockaddr_in ra; int ral = sizeof ra; memset(&ra, 0, sizeof ra);
            SOCKET c = accept(ls, (struct sockaddr*)&ra, &ral);
            if (c != INVALID_SOCKET) {
                /* only the linked partner (or this PC itself in the solo test) may send a checkpoint: a file written into the save folder must never come from a stranger */
                int fromPeer = g_peerKnown && ra.sin_addr.s_addr == g_peerAddr;
                int fromSelf = ra.sin_addr.s_addr == htonl(INADDR_LOOPBACK);
                if (g_mode == 1 && fromPeer && !fromSelf) LOGF("checkpoint from the guest ignored (checkpoints are host-authoritative)");
                else if ((g_mode == 1 || g_mode == 2) && (fromPeer || fromSelf)) { u_long b = 0; ioctlsocket(c, FIONBIO, &b); SaveReceive(c); }
                else LOGF("checkpoint connection from %s refused (not the linked partner)", inet_ntoa(ra.sin_addr));
                closesocket(c);
            }
        }
        int linked = ((g_mode == 1 || g_mode == 2) && g_peerKnown && !SoloMirror());
        if (!linked) { linkPrev = 0; first = 1; continue; }
        if (!linkPrev) { linkPrev = 1; first = 1; }
        if (tick % 10 != 0) continue;                       /* look at the own autosave folder once a second */
        char base[MAX_PATH], pat[MAX_PATH];
        if (!SaveBase(base, sizeof base)) continue;
        _snprintf(pat, sizeof pat, "%ssave\\autosave\\*.svg", base); pat[sizeof pat - 1] = 0;
        WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        char bestName[96] = ""; FILETIME bestTime; memset(&bestTime, 0, sizeof bestTime); DWORD bestSize = 0;
        do {
            if (!_stricmp(fd.cFileName, "coopshared.svg")) continue;
            if (CompareFileTime(&fd.ftLastWriteTime, &bestTime) > 0) { bestTime = fd.ftLastWriteTime; bestSize = fd.nFileSizeLow; strncpy(bestName, fd.cFileName, sizeof bestName - 1); bestName[sizeof bestName - 1] = 0; }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        if (!bestName[0]) continue;
        if (!strcmp(bestName, lastName) && CompareFileTime(&bestTime, &lastTime) == 0) continue;     /* already handled */
        /* checkpoints are HOST-authoritative: the guest never sends its own autosaves. With the trigger/flag mirror the guest's script autosaves at the guest's place
           (e.g. its spawn) and that save used to replace the host's progress, so a death sent both PCs back to the start of the mission */
        if (g_mode == 2 && !SoloMirror()) { strcpy(lastName, bestName); lastTime = bestTime; continue; }
        if (first) {                                                                                   /* at the link: only the HOST offers its newest, and only if it belongs to the running mission */
            first = 0;
            int send = 0; DWORD dm = FastDvar("mapname"); char mp[40] = "";
            if (g_mode == 1 && dm) { CvPeek(dm, mp, sizeof mp); if (mp[0] && !_strnicmp(bestName, mp, strlen(mp))) send = 1; }
            if (!send) { strcpy(lastName, bestName); lastTime = bestTime; continue; }
        }
        if (!(!strcmp(bestName, pendName) && CompareFileTime(&bestTime, &pendTime) == 0 && bestSize == pendSize)) {   /* wait until it has been unchanged for a full second */
            strcpy(pendName, bestName); pendTime = bestTime; pendSize = bestSize; continue;
        }
        char path[MAX_PATH]; _snprintf(path, sizeof path, "%ssave\\autosave\\%s", base, bestName); path[sizeof path - 1] = 0;
        char nm[96]; strncpy(nm, bestName, sizeof nm - 1); nm[sizeof nm - 1] = 0; char* dot = strrchr(nm, '.'); if (dot) *dot = 0;
        strcpy(lastName, bestName); lastTime = bestTime;
        FILE* f = fopen(path, "rb"); if (!f) continue;
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        char* buf = (sz > 0 && sz <= SAVE_MAX) ? (char*)malloc((size_t)sz) : NULL;
        size_t got = buf ? fread(buf, 1, (size_t)sz, f) : 0; fclose(f);
        if (!buf || got != (size_t)sz) { free(buf); continue; }
        unsigned long crc = Crc32Buf((const unsigned char*)buf, (size_t)sz);
        char smap[96]; int sn; CkptParse(nm, smap, sizeof smap, &sn);
        if (crc == g_lastRecvCrc) { free(buf); continue; }                                              /* this is the file we just received (echo) */
        if (g_curMap[0] && !strcmp(smap, g_curMap) && sn < g_curN) { LOGF("checkpoint %s not sent: older than %s-%d that this PC already has", nm, g_curMap, g_curN); free(buf); continue; }
        int okS = SaveSendBuf(nm, buf, sz);
        free(buf);
        LOGF("checkpoint sent: %s (%ld bytes, crc32 %08lX) %s", nm, sz, crc, okS ? "ok" : "FAILED (the other PC may be offline or its firewall blocks TCP 27203)");
        if (okS && g_mode == 1) { InterlockedExchange(&g_hostFN, 0); InterlockedExchange(&g_replayIdx, -1); }   /* what was sent before is inside this checkpoint now */
        strncpy(g_curMap, smap, sizeof g_curMap - 1); g_curMap[sizeof g_curMap - 1] = 0; g_curN = sn;
        strncpy(g_curLoadName, nm, sizeof g_curLoadName - 1); g_curLoadName[sizeof g_curLoadName - 1] = 0;
        {   /* this PC reloads the same checkpoint after its own death: its per-map save gets it too */
            char pm[MAX_PATH]; _snprintf(pm, sizeof pm, "%ssave\\%s.svg", base, smap); pm[sizeof pm - 1] = 0;
            CopyFileA(path, pm, FALSE);
        }
    }
    if (ls != INVALID_SOCKET) closesocket(ls);
    return 0;
}

// ---- The "waiting for the other player" notice: a small top-most window of OUR OWN above the game window. The game draws nothing of its own on the loading
// screen and while it is held, so the engine cannot show this; a Win32 window does not depend on the engine. Click-through, never takes the focus.
static HWND g_ovl;
static LRESULT CALLBACK OvlProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT r; GetClientRect(h, &r);
        HBRUSH br = CreateSolidBrush(RGB(6, 6, 6)); FillRect(dc, &r, br); DeleteObject(br);
        const COLORREF gold = RGB(214, 158, 38);
        HBRUSH gb = CreateSolidBrush(gold);
        RECT top = { 0, 0, r.right, 2 }, bot = { 0, r.bottom - 2, r.right, r.bottom }, sep = { 36, 40, r.right - 36, 42 };
        FillRect(dc, &top, gb); FillRect(dc, &bot, gb); FillRect(dc, &sep, gb); DeleteObject(gb);
        int L = CoopLang(); DWORD cs = (L == 5) ? RUSSIAN_CHARSET : ANSI_CHARSET;
        HFONT f1 = CreateFontA(-24, 0, 0, 0, FW_NORMAL, 0, 0, 0, cs, 0, 0, CLEARTYPE_QUALITY, 0, "Palatino Linotype");
        HFONT f2 = CreateFontA(-22, 0, 0, 0, FW_NORMAL, 0, 0, 0, cs, 0, 0, CLEARTYPE_QUALITY, 0, "Palatino Linotype");
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ old = SelectObject(dc, f1);
        RECT hr = { 0, 8, r.right, 38 }; SetTextColor(dc, gold); DrawTextA(dc, "CO-OP", -1, &hr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, f2);
        RECT tr = { 12, 46, r.right - 12, r.bottom - 6 }; SetTextColor(dc, RGB(244, 244, 244));
        DrawTextA(dc, COOP_TXT[L][CT_WAITING], -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old); DeleteObject(f1); DeleteObject(f2);
        EndPaint(h, &ps); return 0;
    }
    return DefWindowProcA(h, m, w, l);
}
static void OverlayTick(void) {          /* called by the WIN thread every 100 ms; it owns the window, so it also pumps its messages */
    int want = g_bHold ? 1 : 0;
    if (want && !g_ovl) {
        HWND gw = NULL; EnumWindows(FindWnd, (LPARAM)&gw);
        if (!gw) return;
        static int reg = 0;
        if (!reg) { WNDCLASSA wc; memset(&wc, 0, sizeof wc); wc.lpfnWndProc = OvlProc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "CoopWaitOvl"; wc.hCursor = LoadCursor(NULL, IDC_ARROW); RegisterClassA(&wc); reg = 1; }
        RECT r; GetWindowRect(gw, &r);
        int w = 560, h = 96, x = r.left + ((r.right - r.left) - w) / 2, y = r.top + (int)((r.bottom - r.top) * 0.78) - h / 2;
        g_ovl = CreateWindowExA(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, "CoopWaitOvl", "", WS_POPUP, x, y, w, h, NULL, NULL, GetModuleHandleA(NULL), NULL);
        if (g_ovl) { SetLayeredWindowAttributes(g_ovl, 0, 225, LWA_ALPHA); ShowWindow(g_ovl, SW_SHOWNOACTIVATE); UpdateWindow(g_ovl); LOGF("barrier: notice window shown"); }
        else LOGF("barrier: notice window could not be created (error %lu)", GetLastError());
    } else if (!want && g_ovl) {
        DestroyWindow(g_ovl); g_ovl = NULL; LOGF("barrier: notice window removed");
    }
    if (g_ovl) { MSG msg; while (PeekMessageA(&msg, g_ovl, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); } }
}
static DWORD WINAPI WIN(LPVOID p) {
    (void)p;
    char path[MAX_PATH], want[8] = "off", lastDv[8] = "", applied[8] = "";
    PathIn(path, sizeof path, "coop_window.txt");
    FILE* f = fopen(path, "r");
    if (f) { char b[16] = ""; if (fgets(b, sizeof b, f) && !_strnicmp(b, "on", 2)) strcpy(want, "on"); fclose(f); }
    char mpath[MAX_PATH], mwant[8] = "off", mlast[8] = "", mapplied[8] = "off";
    PathIn(mpath, sizeof mpath, "coop_mouse.txt");
    FILE* mf = fopen(mpath, "r");
    if (mf) { char b[16] = ""; if (fgets(b, sizeof b, mf) && !_strnicmp(b, "on", 2)) strcpy(mwant, "on"); fclose(mf); }
    DWORD dv = 0, dst = 0, dmouse = 0, dfollow = 0;
    int g_f8Down = 0, g_f7Down = 0;
    PrefsLoad();
    for (int tick = 0;; tick++) {
        SleepQ(100);
        OverlayTick();
        if (g_quit) { RemoveRaw(); return 0; }
        if ((GetAsyncKeyState(VK_F8) & 0x8000) && !g_f8Down) {   /* F8: go to the partner (only while the game window is in front) */
            HWND gw = NULL; EnumWindows(FindWnd, (LPARAM)&gw);
            if (gw && GetForegroundWindow() == gw) { g_tpPending = 1; LOGF("teleport: F8 pressed, going to the partner"); }
            g_f8Down = 1;
        } else if (!(GetAsyncKeyState(VK_F8) & 0x8000)) g_f8Down = 0;
        /* F7 (guest only): take the host's world. The host's latest checkpoint (coopshared.svg, written whenever the host autosaves) is loaded exactly as after a death:
           the whole world, objectives and mission chapter of the host come over; the guest appears where the host saved. Only while the game window is in front. */
        if ((GetAsyncKeyState(VK_F7) & 0x8000) && !g_f7Down) {
            g_f7Down = 1;
            HWND gw = NULL; EnumWindows(FindWnd, (LPARAM)&gw);
            if (GetTickCount() - g_f7Last < 6000) { /* a load is under way: no second one (a held key repeated it every 0.2 s) */ }
            else if (gw && GetForegroundWindow() == gw) {
                int run = 0; DvarInt("sv_running", &run);
                char base[MAX_PATH], cp[MAX_PATH];
                if (g_mode != 2) LOGF("resync: F7 pressed, but this PC is not the guest: nothing to do");
                else if (!run) LOGF("resync: F7 pressed, but no mission is running here");
                else if (!SaveBase(base, sizeof base)) LOGF("resync: F7 pressed, the save folder is unknown");
                else {
                    _snprintf(cp, sizeof cp, "%ssave\\autosave\\coopshared.svg", base); cp[sizeof cp - 1] = 0;
                    if (GetFileAttributesA(cp) == INVALID_FILE_ATTRIBUTES) LOGF("resync: F7 pressed, but the host has not sent a checkpoint yet");
                    else {
                        int q = CbufAdd("loadgame autosave/coopshared\n");
                        LOGF("resync: F7 pressed, loading the host's latest checkpoint: %s", q ? "queued" : "FAILED");
                        if (q) { g_f7Last = GetTickCount(); g_qAt[0] = g_f7Last + 8000; g_qAt[1] = g_f7Last + 16000; if (!g_qAt[0]) g_qAt[0] = 1; }
                    }
                }
            }
        } else if (!(GetAsyncKeyState(VK_F7) & 0x8000)) g_f7Down = 0;
        for (int qi = 0; qi < 2; qi++) {                                  /* guest: after the load, ask the host to send the keys it sent after that checkpoint */
            if (g_qAt[qi] && (LONG)(GetTickCount() - g_qAt[qi]) >= 0) {
                g_qAt[qi] = 0;
                if (g_peerKnown && g_udpSock != INVALID_SOCKET) {
                    struct sockaddr_in to; memset(&to, 0, sizeof to);
                    to.sin_family = AF_INET; to.sin_addr.s_addr = g_peerAddr; to.sin_port = htons((u_short)g_peerPort);
                    sendto(g_udpSock, "Q", 1, 0, (struct sockaddr*)&to, sizeof to);
                    sendto(g_udpSock, "Q", 1, 0, (struct sockaddr*)&to, sizeof to);
                    LOGF("resync: asked the host to re-send the keys sent after its checkpoint");
                }
            }
        }
        if (g_mode == 1 && g_replayIdx >= 0 && (LONG)(GetTickCount() - g_replayNext) >= 0) {   /* host: re-send one key every 0.3 s (the script takes one per 0.25 s) */
            LONG i = g_replayIdx;
            if (i >= g_hostFN) InterlockedExchange(&g_replayIdx, -1);
            else {
                char pkt[64]; int len = _snprintf(pkt, sizeof pkt - 1, "F %s", g_hostFlags[i]);
                if (len > 0 && g_peerKnown && g_udpSock != INVALID_SOCKET) {
                    struct sockaddr_in to; memset(&to, 0, sizeof to);
                    to.sin_family = AF_INET; to.sin_addr.s_addr = g_peerAddr; to.sin_port = htons((u_short)g_peerPort);
                    sendto(g_udpSock, pkt, len, 0, (struct sockaddr*)&to, sizeof to);
                    sendto(g_udpSock, pkt, len, 0, (struct sockaddr*)&to, sizeof to);
                    LOGF("replay: flag re-sent: %s", g_hostFlags[i]);
                }
                InterlockedExchange(&g_replayIdx, i + 1); g_replayNext = GetTickCount() + 300;
            }
        }
        if (!dfollow) dfollow = FastDvar("coop_follow");
        if (dfollow) {
            char v[16]; CvPeek(dfollow, v, sizeof v);
            for (char* e = v + strlen(v); e > v && (e[-1] == ' ' || e[-1] == '	'); ) *--e = 0;
            if ((!_stricmp(v, "on") || !_stricmp(v, "off")) && _stricmp(v, g_follow)) strcpy(g_follow, _stricmp(v, "on") ? "off" : "on");
        }
        if (tick % 5 == 0) PrefsSave(want, mwant);
        if (!dmouse) dmouse = FastDvar("coop_rawmouse");
        if (dmouse) {
            char v[16]; CvPeek(dmouse, v, sizeof v);
            for (char* e = v + strlen(v); e > v && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
            if ((!_stricmp(v, "on") || !_stricmp(v, "off")) && _stricmp(v, mlast)) {
                strcpy(mlast, v); strcpy(mwant, _stricmp(v, "on") ? "off" : "on");
                FILE* o = fopen(mpath, "w"); if (o) { fputs(mwant, o); fclose(o); }
            }
        }
        if (strcmp(mwant, mapplied)) {
            if (!strcmp(mwant, "on")) InstallRaw(); else RemoveRaw();
            strcpy(mapplied, mwant);
        }
        if (g_rawOn && tick % 99 == 0) LOGF("raw mouse: %ld raw events so far", (long)g_rawEvents);
        { char mp[MAX_PATH]; PathIn(mp, sizeof mp, "coop_mute.txt"); LONG m = GetFileAttributesA(mp) != INVALID_FILE_ATTRIBUTES;
          if (m != g_mute) { g_mute = m; LOGF("debug: coop_mute.txt %s, peer state is %s for the script", m ? "present" : "removed", m ? "frozen" : "live"); } }
        if (!strcmp(applied, "on")) {   /* topmost only while the game is the active window, so Alt+Tab / other monitors work */
            HWND gw = NULL; EnumWindows(FindWnd, (LPARAM)&gw);
            if (gw && !(GetWindowLongA(gw, GWL_STYLE) & WS_CAPTION)) {
                int fg = GetForegroundWindow() == gw;
                int top = (GetWindowLongA(gw, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
                if (fg && !top) SetWindowPos(gw, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                else if (!fg && top) SetWindowPos(gw, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        }
        if (!dst) { dst = FastDvar("coop_status"); if (dst) LOGF("menu: status dvar found by the engine lookup (%08lX)", dst); }
        if (!dst && tick % 9 == 1) { Collect(); dst = ResolveCvar("coop_status", 3); if (dst) LOGF("menu: status dvar found (%08lX)", dst); }
        if (dst) PaintStatus(dst);
        if ((g_mode == 1 || g_mode == 2) && g_peerKnown && g_udpSock != INVALID_SOCKET && tick % 18 == 3) {   /* tell the partner which release this is (it warns on a mismatch) */
            struct sockaddr_in to; memset(&to, 0, sizeof to);
            to.sin_family = AF_INET; to.sin_addr.s_addr = g_peerAddr; to.sin_port = htons((u_short)g_peerPort);
            const char* v = strchr(BRIDGE_VERSION, ' '); v = v ? v + 1 : BRIDGE_VERSION;
            char vp[40]; int vl = _snprintf(vp, sizeof vp - 1, "V %.*s", (int)strcspn(v, " "), v);
            if (vl > 0) sendto(g_udpSock, vp, vl, 0, (struct sockaddr*)&to, sizeof to);
        }
        if (g_rdySet && (g_mode == 1 || g_mode == 2) && g_peerKnown && g_udpSock != INVALID_SOCKET) {   /* repeat "I am ready" (a lost packet must not hold the partner) */
            struct sockaddr_in to; memset(&to, 0, sizeof to);
            to.sin_family = AF_INET; to.sin_addr.s_addr = g_peerAddr; to.sin_port = htons((u_short)g_peerPort);
            char rp[80]; EnterCriticalSection(&L); int rl = _snprintf(rp, sizeof rp - 1, "R %s %ld", g_rdyMap, (long)g_rdySkill); LeaveCriticalSection(&L);
            if (rl > 0) sendto(g_udpSock, rp, rl, 0, (struct sockaddr*)&to, sizeof to);
        }
        if (g_mode == 2 && g_peerKnown && g_txn == 0 && g_udpSock != INVALID_SOCKET && tick % 9 == 0) {   /* guest, no mission yet: tell the host we are here */
            struct sockaddr_in to; memset(&to, 0, sizeof to);
            to.sin_family = AF_INET; to.sin_addr.s_addr = g_peerAddr; to.sin_port = htons((u_short)g_peerPort);
            if (sendto(g_udpSock, "H 1", 3, 0, (struct sockaddr*)&to, sizeof to) < 0) { static DWORD lastSE; if (GetTickCount() - lastSE > 5000) { lastSE = GetTickCount(); LOGF("send to the host FAILED, error %d", WSAGetLastError()); } }
        }
        BarrierTick();
        MissionFollow(tick);
        if (!dv) { dv = FastDvar("coop_borderless"); if (dv) LOGF("window: dvar found by the engine lookup (%08lX)", dv); }
        if (!dv && tick % 9 == 0) { Collect(); dv = ResolveCvar("coop_borderless", 3); if (dv) LOGF("window: dvar found (%08lX)", dv); }
        if (dv) {
            char v[16]; CvPeek(dv, v, sizeof v);
            for (char* e = v + strlen(v); e > v && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
            if ((!_stricmp(v, "on") || !_stricmp(v, "off")) && _stricmp(v, lastDv)) {
                strcpy(lastDv, v); strcpy(want, _stricmp(v, "on") ? "off" : "on");
                FILE* o = fopen(path, "w"); if (o) { fputs(want, o); fclose(o); }
            }
        }
        if (strcmp(want, applied)) {
            HWND h = NULL; EnumWindows(FindWnd, (LPARAM)&h);
            if (!h) continue;
            if (!strcmp(want, "on") || *applied) ApplyBorderless(!strcmp(want, "on"));
            strcpy(applied, want);
        }
    }
}

BOOL APIENTRY DllMain(HMODULE m, DWORD r, LPVOID x) {
    (void)x;
    if (r == DLL_PROCESS_ATTACH) {
        // Only ever active in the single-player exe. The d3d9 proxy loads this DLL from the game folder and the
        // multiplayer exe (PunkBuster) shares that folder: there this DLL must do nothing at all.
        char exe[MAX_PATH];
        DWORD en = GetModuleFileNameA(NULL, exe, MAX_PATH);
        const char* base = strrchr(exe, '\\');
        base = base ? base + 1 : exe;
        if (!en || _stricmp(base, "CoD2SP_s.exe") != 0) return TRUE;
        DisableThreadLibraryCalls(m);
        InitializeCriticalSection(&L);
        InitializeCriticalSection(&LSCAN);
        QueryPerformanceFrequency(&g_qf);
        { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); }
        InitDir();
        // opened before the threads start: otherwise the first lines are dropped
        char lp[MAX_PATH]; PathIn(lp, sizeof lp, "coop5.log");
        {   /* coop5.log would grow without limit: above 3 MB the old one becomes coop5.1.log (one generation is kept) */
            WIN32_FILE_ATTRIBUTE_DATA fa; char lp1[MAX_PATH];
            if (GetFileAttributesExA(lp, GetFileExInfoStandard, &fa) && fa.nFileSizeHigh == 0 && fa.nFileSizeLow > 3u * 1024u * 1024u) {
                PathIn(lp1, sizeof lp1, "coop5.1.log"); DeleteFileA(lp1); MoveFileA(lp, lp1);
            }
        }
        F = fopen(lp, "a");
        // one bridge per game process: a second one (an old injected copy, the launcher on top of the autoload)
        // would fight for the port. It says so in the log and exits; a new version needs a game restart.
        // per PROCESS (the pid is in the name): a named mutex is shared by the whole Windows session, so a fixed name also blocked a
        // second game process or a test run next to the game
        char mxname[64]; _snprintf(mxname, sizeof mxname - 1, "Local\\cod2coop_bridge_v1_%lu", GetCurrentProcessId()); mxname[sizeof mxname - 1] = 0;
        HANDLE mx = CreateMutexA(NULL, TRUE, mxname);
        g_mutex = mx;
        if (mx && GetLastError() == ERROR_ALREADY_EXISTS) {
            LOGF("another bridge is already running in this game process: this copy (%s) does nothing. Restart the game to load a new version.", BRIDGE_VERSION);
            return TRUE;
        }
        char cp[MAX_PATH]; PathIn(cp, sizeof cp, "coop_net.csv");
        long csvsize = 0;
        { FILE* t0 = fopen(cp, "rb"); if (t0) { fseek(t0, 0, SEEK_END); csvsize = ftell(t0); fclose(t0); } }
        if (csvsize > 4L * 1024 * 1024) csvsize = 0;                  /* an old file above 4 MB is started again */
        CSVF = fopen(cp, csvsize == 0 ? "w" : "a");
        if (CSVF && csvsize == 0) fprintf(CSVF, "wall,t_ms,dir,seq,x,y,z,yaw,note\n");
        BU = (unsigned char*)malloc(CH);

        {
            WIN32_FILE_ATTRIBUTE_DATA fa; DWORD sz = 0;
            char ex[MAX_PATH]; GetModuleFileNameA(NULL, ex, MAX_PATH);   // only the size is logged, never the path
            if (GetFileAttributesExA(ex, GetFileExInfoStandard, &fa)) sz = fa.nFileSizeLow;
            unsigned long gcrc = 0; int gcrcOk = 0;                      // CRC-32 of the exe (identifies the build in a log; the installer checks SHA-256)
            if (sz && sz < 40u * 1024u * 1024u) {
                FILE* gf = fopen(ex, "rb");
                unsigned char* gb = gf ? (unsigned char*)malloc(sz) : NULL;
                if (gb && fread(gb, 1, sz, gf) == sz) { gcrc = Crc32Buf(gb, sz); gcrcOk = 1; }
                free(gb); if (gf) fclose(gf);
            }
            const char* gtag = (sz == 662528 && gcrcOk && gcrc == 0xB274602Eul) ? "the Steam build" : (sz == 25980416) ? "size of the retail 1.3 build" : "NOT a build we know: the engine hooks may not apply (see the proxy's 'script hooks' lines)";
            LOGF("=== %s, built %s %s, game exe %lu bytes, crc32 %08lX (%s), pid %lu ===", BRIDGE_VERSION, __DATE__, __TIME__, sz, gcrc, gtag, GetCurrentProcessId());
        }
        ReadPeer();
        int pf = ReadPort();
        // Who loaded us decides the mode. The d3d9 proxy loads "cod2net5.dll" (autoload: the in-game menu drives us); the
        // launcher's injected copies are named cod2net5_<n>.dll (files drive us). Leftover coop_port.txt / coop_peer.txt
        // from an earlier launcher run must not override the menu: that cost a test (the menu was ignored).
        char self[MAX_PATH]; DWORD sn = GetModuleFileNameA(m, self, MAX_PATH);
        const char* selfBase = strrchr(self, '\\'); selfBase = selfBase ? selfBase + 1 : self;
        // "cod2net5.dll" (loaded directly) or "cod2net5_live_<n>.dll" (the proxy's reloadable copy) = autoload; the launcher's
        // injected copies are "cod2net5_<digits>.dll" and do NOT match the second form
        int autoloaded = sn && (!_stricmp(selfBase, "cod2net5.dll") || !_strnicmp(selfBase, "cod2net5_live_", 14));
        if (autoloaded && (g_hasPeer || pf > 0)) {
            LOGF("autoloaded: ignoring leftover coop_port.txt / coop_peer.txt (the in-game menu decides; 'uninstall' removes them)");
            g_hasPeer = 0; g_peerAuto = 0; g_peerKnown = 0; g_logPort = 0; pf = 0;
        }
        g_fileMode = (g_hasPeer || pf > 0);
        if (g_fileMode) {
            g_listenPort = pf ? pf : PORT; g_tag = g_listenPort;
            if (g_hasPeer && g_logPort && !g_peerAuto) OpenLogSink(g_peerIp, g_logPort);
            InterlockedExchange(&g_enabled, 1);
            LOGF("mode: files (coop_port.txt / coop_peer.txt), listen %d", g_listenPort);
        } else {
            LOGF("mode: in-game menu (open Co-op in the main menu and click Host or Join); nothing listens until then");
        }
        ReadLastHost();
        // the thread handles are kept: CoopStop waits for them before the DLL can be freed
        g_th[g_nth] = CreateThread(0, 0, UDP, 0, 0, 0); if (g_th[g_nth]) g_nth++;
        g_th[g_nth] = CreateThread(0, 0, WORK, 0, 0, 0); if (g_th[g_nth]) g_nth++;
        g_th[g_nth] = CreateThread(0, 0, CTRL, 0, 0, 0); if (g_th[g_nth]) g_nth++;
        if (!g_fileMode) { g_th[g_nth] = CreateThread(0, 0, WIN, 0, 0, 0); if (g_th[g_nth]) g_nth++; }
        g_th[g_nth] = CreateThread(0, 0, SAVESYNC, 0, 0, 0); if (g_th[g_nth]) g_nth++;
        if (!g_fileMode) {   // menu mode only: both are idle until Host / Join-auto is chosen
            g_th[g_nth] = CreateThread(0, 0, BEACON, 0, 0, 0); if (g_th[g_nth]) g_nth++;
            g_th[g_nth] = CreateThread(0, 0, DISC, 0, 0, 0); if (g_th[g_nth]) g_nth++;
        }
    }
    return TRUE;
}

// ================================================================================================================
// Script built-ins: GSC functions implemented here. The engine's own helpers are called by address (docs/ENGINE.md, section
// "Script and dvar entry points", all derived from our SP image). Rules from that analysis: a handler reads ALL its parameters
// BEFORE the first Add* (every Add* pops and releases the remaining inputs); results are pushed with Add*; the handler is cdecl
// void f(void). The proxy has already patched the engine's lookups (CoopApi.hooksInstalled) and owns the stubs the engine caches.
// ================================================================================================================
#define E_PARAMCOUNT  0x00A0189Cu     /* number of parameters of the current built-in call (global, no accessor) */
#define E_GETINT      0x0046E4A0u     /* usercall: EAX = param index -> int in EAX (script error if missing or not an int) */
#define E_ADDINT      0x0046EEC0u     /* cdecl (int) */
#define E_ADDSTRING   0x0046F0C0u     /* usercall: ESI = const char* (copied into the script string table) */

static int E_GetInt(int n) {
    int r;
    __asm {
        push ebx
        push esi
        push edi
        mov eax, n
        mov ecx, E_GETINT
        call ecx
        mov r, eax
        pop edi
        pop esi
        pop ebx
    }
    return r;
}
static void E_AddInt(int v) { ((void (__cdecl*)(int))(ULONG_PTR)E_ADDINT)(v); }
static void E_AddString(const char* s) {
    __asm {
        push ebx
        push esi
        push edi
        mov esi, s
        mov eax, E_ADDSTRING
        call eax
        pop edi
        pop esi
        pop ebx
    }
}

#define E_GETFLOAT    0x0046E6D0u     /* usercall: EAX = param index, result in ST0 */
#define E_GETVECTOR   0x0046EA90u     /* usercall: EAX = param index, ECX = float[3]* destination */
#define E_ADDFLOAT    0x0046EF10u     /* cdecl (float) */
#define E_ADDVECTOR   0x0046F200u     /* usercall: ESI = const float[3]* */
static float E_GetFloat(int n) {
    float r;
    __asm {
        push ebx
        push esi
        push edi
        mov eax, n
        mov ecx, E_GETFLOAT
        call ecx
        fstp dword ptr [r]
        pop edi
        pop esi
        pop ebx
    }
    return r;
}
static void E_GetVector(int n, float* dst) {
    __asm {
        push ebx
        push esi
        push edi
        mov eax, n
        mov ecx, dst
        mov edx, E_GETVECTOR
        call edx
        pop edi
        pop esi
        pop ebx
    }
}
static void E_AddFloat(float f) { ((void (__cdecl*)(float))(ULONG_PTR)E_ADDFLOAT)(f); }
static void E_AddVector(const float* v) {
    __asm {
        push ebx
        push esi
        push edi
        mov esi, v
        mov eax, E_ADDVECTOR
        call eax
        pop edi
        pop esi
        pop ebx
    }
}

static const CoopApi* g_capi = NULL;

static void __cdecl H_coop_publish(void) {              /* coop_publish_state(origin, yaw): the local state the bridge sends to the peer */
    int count = *(volatile int*)(ULONG_PTR)E_PARAMCOUNT;
    if (count < 2) return;
    float o[3], yaw; int stance = 0;
    E_GetVector(0, o);                                  /* all parameters are read before anything is pushed */
    yaw = E_GetFloat(1);
    int evt = 0;
    if (count >= 3) stance = E_GetInt(2);
    int wid = 0;
    if (count >= 4) evt = E_GetInt(3);
    int par = 0;
    if (count >= 5) wid = E_GetInt(4);
    if (count >= 6) par = E_GetInt(5);
    int hp = 100;
    if (count >= 7) hp = E_GetInt(6);
    g_pubHealth = (hp >= 0 && hp <= 999) ? hp : 100;
    g_pubParent = (par >= 0 && par < 2048) ? par : 0;
    if (!(fabsf(o[0]) < 1e6f && fabsf(o[1]) < 1e6f && fabsf(o[2]) < 1e6f && fabsf(yaw) < 1e6f)) return;
    g_pub[0] = o[0]; g_pub[1] = o[1]; g_pub[2] = o[2]; g_pub[3] = yaw;
    { LONG ns = (stance >= 0 && stance <= 4) ? stance : 0; if (ns != g_pubStance) LOGF("script: stance %d -> %d", (int)g_pubStance, (int)ns); g_pubStance = ns; }
    if (wid != g_pubWeapon) { LOGF("script: weapon id %d -> %d", (int)g_pubWeapon, wid); g_pubWeapon = (wid >= 0 && wid <= 99) ? wid : 0; }
    if (evt != g_pubEvt) {
        LOGF("script: event %d (counter %d, type %d)", evt, evt / 8, evt % 8); g_pubEvt = evt;
        if ((evt % 8) == 5 && g_curLoadName[0] && g_peerKnown && !SoloMirror()) {      /* own death: the newest known checkpoint is loaded by name while the death screen is on */
            _snprintf(g_deathLoadCmd, sizeof g_deathLoadCmd, "loadgame autosave/%s\n", g_curLoadName); g_deathLoadCmd[sizeof g_deathLoadCmd - 1] = 0;
            g_deathLoadAt = GetTickCount() + 3500; if (!g_deathLoadAt) g_deathLoadAt = 1;
            LOGF("own death: the newest known checkpoint '%s' is loaded by name in 3.5 s", g_curLoadName);
        }
    }
    g_pubMs = (LONG)GetTickCount();
    InterlockedIncrement(&g_pubSeq);
    static int n = 0;
    if (n < 3 || n % 200 == 0) LOGF("script: coop_publish_state #%d (%.1f %.1f %.1f yaw %.1f)", n, o[0], o[1], o[2], yaw);
    n++;
}
/* solo test (JOIN 127.0.0.1: the peer is this very game): the puppet would stand inside the player, so it is shown 150 units in front of
   the player, facing him. Only in that mode. */
static int SoloMirror(void) { return g_mode == 2 && !strncmp(g_peerIp, "127.", 4); }
static void __cdecl H_coop_peer_state(void) {           /* vector coop_peer_state(): last origin received from the peer */
    float v[3] = { g_peer[0], g_peer[1], g_peer[2] };
    if (SoloMirror()) {
        /* in a vehicle (stance 4) the test puppet sits 45 units to the right of the player on the same deck, otherwise 150 units in front of him */
        int veh = (g_peerStance == 4);
        if (veh) {                       /* in a vehicle: 45 units to the right of the view direction (the link is made once, later turns do not matter) */
            double r = ((double)g_peer[3] - 90.0) * 3.14159265358979 / 180.0;
            v[0] += (float)(cos(r) * 45.0); v[1] += (float)(sin(r) * 45.0);
        } else {                         /* on foot: a FIXED place in the world (150 units east of the player), so it does not swing round with his camera */
            v[0] += 150.0f;
        }
    }
    E_AddVector(v);
}
static void __cdecl H_coop_peer_yaw(void) { float y = g_peer[3]; if (SoloMirror() && g_peerStance != 4) y = 180.0f; E_AddFloat(y); }   /* solo test on foot: the puppet faces west, toward the player */       /* float coop_peer_yaw() */
static void __cdecl H_coop_peer_stance(void) {
    static LONG lastRet = -1;
    LONG v = g_peerStance;
    if (v != lastRet) { LOGF("script: coop_peer_stance() returns %d (was %d)", (int)v, (int)lastRet); lastRet = v; }
    E_AddInt((int)v);
}   /* int coop_peer_stance(): 0 stand 1 crouch 2 prone 3 airborne */
static void __cdecl H_coop_peer_weapon(void) { E_AddInt((int)g_peerWeapon); }   /* int coop_peer_weapon(): id of the peer's weapon (0 = unknown) */
#define E_GETSTRING   0x0046E760u     /* usercall: EAX = param index -> a string handle in EAX; the characters are at *(char**)0x8AED00 + handle * 8 + 4 */
static int E_GetStringCopy(int n, char* out, size_t cap) {
    int h;
    __asm {
        push ebx
        push esi
        push edi
        mov eax, n
        mov ecx, E_GETSTRING
        call ecx
        mov h, eax
        pop edi
        pop esi
        pop ebx
    }
    char* base = *(char**)(ULONG_PTR)0x008AED00u;
    if (!base || h <= 0 || h > 0xFFFF) { out[0] = 0; return 0; }
    strncpy(out, base + (size_t)h * 8 + 4, cap - 1); out[cap - 1] = 0;
    return 1;
}
static void __cdecl H_coop_send_grenade(void) {         /* coop_send_grenade(weaponName, offset, velocity): the local player threw a grenade */
    int count = *(volatile int*)(ULONG_PTR)E_PARAMCOUNT;
    if (count < 3) return;
    char nm[48]; float o[3], v[3];
    if (!E_GetStringCopy(0, nm, sizeof nm)) return;
    E_GetVector(1, o); E_GetVector(2, v);
    for (char* q = nm; *q; q++) if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '_' || *q == '-')) { *q = 0; break; }
    if (!nm[0] || strlen(nm) > 38) return;
    char pkt[160]; int len = _snprintf(pkt, sizeof pkt - 1, "G %s %.1f %.1f %.1f %.1f %.1f %.1f", nm, o[0], o[1], o[2], v[0], v[1], v[2]);
    if (len > 0 && g_peerKnown && g_udpSock != INVALID_SOCKET) {
        struct sockaddr_in to; memset(&to, 0, sizeof to);
        to.sin_family = AF_INET; to.sin_addr.s_addr = g_peerAddr; to.sin_port = htons((u_short)g_peerPort);
        sendto(g_udpSock, pkt, len, 0, (struct sockaddr*)&to, sizeof to);
        sendto(g_udpSock, pkt, len, 0, (struct sockaddr*)&to, sizeof to);
        LOGF("grenade sent: %s velocity %.0f %.0f %.0f", nm, v[0], v[1], v[2]);
    }
}
static void __cdecl H_coop_send_tank(void) {            /* coop_send_tank(origin, hullAngles, turretAngles, shotCounter): the local player's tank, ~20 times a second */
    int count = *(volatile int*)(ULONG_PTR)E_PARAMCOUNT;
    if (count < 4) return;
    float o[3], a[3], t[3];
    E_GetVector(0, o); E_GetVector(1, a); E_GetVector(2, t);
    int fire = E_GetInt(3);
    char pkt[200]; int len = _snprintf(pkt, sizeof pkt - 1, "T %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %d", o[0], o[1], o[2], a[0], a[1], a[2], t[0], t[1], t[2], fire);
    if (len > 0 && g_peerKnown && g_udpSock != INVALID_SOCKET) {
        struct sockaddr_in to; memset(&to, 0, sizeof to);
        to.sin_family = AF_INET; to.sin_addr.s_addr = g_peerAddr; to.sin_port = htons((u_short)g_peerPort);
        sendto(g_udpSock, pkt, len, 0, (struct sockaddr*)&to, sizeof to);
    }
}
static TankSt TankGet(void) { TankSt s; EnterCriticalSection(&L); s = g_tankIn; LeaveCriticalSection(&L); return s; }
static void __cdecl H_coop_tank_pos(void)    { TankSt s = TankGet(); E_AddVector(s.o); }       /* vector coop_tank_pos(): the peer's tank position */
static void __cdecl H_coop_tank_ang(void)    { TankSt s = TankGet(); E_AddVector(s.a); }       /* vector coop_tank_ang(): hull angles */
static void __cdecl H_coop_tank_turret(void) { TankSt s = TankGet(); E_AddVector(s.t); }       /* vector coop_tank_turret(): turret angles (world) */
static void __cdecl H_coop_tank_fire(void)   { TankSt s = TankGet(); E_AddInt(s.fire); }       /* int coop_tank_fire(): the peer's shot counter */
static void __cdecl H_coop_tank_age(void)    { TankSt s = TankGet(); E_AddInt(s.tick ? (int)(GetTickCount() - s.tick) : 999999); }   /* int coop_tank_age(): ms since the last report (999999 = never) */
static void __cdecl H_coop_barrier_begin(void) {        /* coop_barrier_begin(map, skill, delaySeconds): hold the game (console command "pause") until the partner is ready in the same mission, 20 s at most */
    InterlockedExchange(&g_hostFN, 0); InterlockedExchange(&g_replayIdx, -1);   /* a new mission: nothing to replay from the old one */
    int count = *(volatile int*)(ULONG_PTR)E_PARAMCOUNT;
    char mp[48];
    if (count < 3 || !E_GetStringCopy(0, mp, sizeof mp)) return;
    for (char* q = mp; *q; q++) if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '_' || *q == '-')) { *q = 0; break; }
    int sk = E_GetInt(1); float delay = E_GetFloat(2);
    if (!(delay >= 0.0f && delay < 120.0f)) delay = 0.0f;
    LONG now = (LONG)GetTickCount();
    strcpy(g_bMap, mp); g_bSkill = sk; g_bBegin = now; g_bReadyAt = now + (LONG)(delay * 1000.0f); g_bDeadline = now + 20000 + (LONG)(delay * 1000.0f);
    g_rdySet = 0; g_bHold = 1;
    int p = 0; DvarInt("cl_paused", &p);
    g_bPaused = SetPaused(1) ? 1 : 0;      /* while a level loads the engine keeps cl_paused at 1 itself and clears it when the level goes live: hold it either way */
    LOGF("barrier: begin in '%s' (difficulty %d), this PC reports ready in %.1f s, game %s", mp, sk, delay, g_bPaused ? (p ? "held (the engine had it paused already)" : "paused") : "NOT paused (write failed)");
}
static void __cdecl H_coop_barrier_active(void) { E_AddInt(g_bHold ? 1 : 0); }    /* int coop_barrier_active(): 1 while the game is held for the partner */
static void __cdecl H_coop_apply_host_skill(void) {     /* coop_apply_host_skill(): on the guest, write the host's difficulty into g_gameskill (read only for the console, reset by the engine at the level start);
                                                           called from main() of the mission, BEFORE the level's own script reads it (maps\_gameskill: level.gameSkill = getcvarint("g_gameskill")) */
    LONG hs = g_hostSkill;
    if (g_mode != 2 || hs < 0 || hs > 3 || SoloMirror()) return;
    DWORD d = FastDvar("g_gameskill"); DWORD v = 0;
    if (!d || !SC(&v, (void*)(ULONG_PTR)(d + 8), 4)) { LOGF("difficulty: dvar g_gameskill not readable"); return; }
    if (v > 3) { LOGF("difficulty: g_gameskill holds %lu, not a difficulty: not touched", (unsigned long)v); return; }
    if ((LONG)v == hs) { LOGF("difficulty: %ld here and on the host, nothing to do", (long)hs); return; }
    int ok = 1;
    __try { *(volatile DWORD*)(ULONG_PTR)(d + 8) = (DWORD)hs; } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    LOGF("difficulty: %lu here, %ld on the host: written into g_gameskill before the level's script reads it: %s", (unsigned long)v, (long)hs, ok ? "ok" : "FAILED");
}
static void __cdecl H_coop_link_active(void) {          /* int coop_link_active(): 1 while a partner is linked (host: packets or its hello within seconds; guest: the host was found) */
    double now = NowMs(); EnterCriticalSection(&L); double lr = g_lastRxMs; LeaveCriticalSection(&L);
    int fresh = lr > 0 && (now - lr) < 5000.0;
    int hello = g_helloMs && (LONG)(GetTickCount() - (DWORD)g_helloMs) < 10000;
    int on = (g_mode == 1 && (fresh || hello)) || (g_mode == 2 && g_peerKnown);
    E_AddInt(on ? 1 : 0);
}
static void __cdecl H_coop_pop_grenade(void) {          /* string coop_pop_grenade(): weapon name of the oldest received grenade (its offset and velocity are then read with the two functions below), or "" */
    char nm[40] = "";
    EnterCriticalSection(&L);
    if (g_gTail < g_gHead) { g_gLast = g_gIn[g_gTail & 7]; g_gTail++; strcpy(nm, g_gLast.name); }
    LeaveCriticalSection(&L);
    E_AddString(nm);
}
static void __cdecl H_coop_grenade_org(void) { E_AddVector(g_gLast.o); }    /* vector coop_grenade_org(): start offset of the last popped grenade */
static void __cdecl H_coop_grenade_vel(void) { E_AddVector(g_gLast.v); }    /* vector coop_grenade_vel(): its velocity */
static void __cdecl H_coop_send_flag(void) {            /* coop_send_flag(name): tell the peer that this mission flag was set here */
    int count = *(volatile int*)(ULONG_PTR)E_PARAMCOUNT;
    if (count < 1) return;
    char nm[48]; if (!E_GetStringCopy(0, nm, sizeof nm)) return;
    for (char* q = nm; *q; q++) if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '_' || *q == ' ')) { *q = 0; break; }
    if (!nm[0] || strlen(nm) > 38) return;
    char pkt[64]; int len = _snprintf(pkt, sizeof pkt - 1, "F %s", nm);
    if (g_mode == 1 && g_hostFN < HF_MAX) {                   /* the host remembers it (until its next checkpoint) */
        int have = 0; for (LONG i = 0; i < g_hostFN; i++) if (!strcmp(g_hostFlags[i], nm)) { have = 1; break; }
        if (!have) { strcpy(g_hostFlags[g_hostFN], nm); g_hostFN++; }
    }
    if (len > 0 && g_peerKnown && g_udpSock != INVALID_SOCKET) {
        struct sockaddr_in to; memset(&to, 0, sizeof to);
        to.sin_family = AF_INET; to.sin_addr.s_addr = g_peerAddr; to.sin_port = htons((u_short)g_peerPort);
        sendto(g_udpSock, pkt, len, 0, (struct sockaddr*)&to, sizeof to);
        sendto(g_udpSock, pkt, len, 0, (struct sockaddr*)&to, sizeof to);
        LOGF("flag sent: %s", nm);
    } else LOGF("flag %s NOT sent (no peer yet)", nm);
}
static void __cdecl H_coop_flag_applied(void) {         /* coop_flag_applied(name): the map script set a flag the peer reported (a log line, so the application is visible in the log) */
    char nm[48]; if (*(volatile int*)(ULONG_PTR)E_PARAMCOUNT >= 1 && E_GetStringCopy(0, nm, sizeof nm)) { nm[39] = 0; LOGF("flag applied: %s", nm); }
}
static void __cdecl H_coop_log(void) {                  /* coop_log(text): a line "script: <text>" in coop5.log (diagnostics that must NOT appear on the player's screen) */
    char m[200]; if (*(volatile int*)(ULONG_PTR)E_PARAMCOUNT >= 1 && E_GetStringCopy(0, m, sizeof m)) {
        m[sizeof m - 1] = 0; for (char* q = m; *q; q++) if ((unsigned char)*q < 32) *q = ' ';
        LOGF("script: %s", m);
    }
}
static void __cdecl H_coop_pop_flag(void) {             /* string coop_pop_flag(): the oldest received flag name, or "" */
    char nm[40] = "";
    EnterCriticalSection(&L);
    if (g_flagTail < g_flagHead) { strcpy(nm, g_flagQ[g_flagTail & 15]); g_flagTail++; }
    LeaveCriticalSection(&L);
    E_AddString(nm);
}
static void __cdecl H_coop_send_kill(void) {            /* coop_send_kill(origin): tell the peer an enemy died here */
    int count = *(volatile int*)(ULONG_PTR)E_PARAMCOUNT;
    if (count < 1) return;
    float o[3];
    E_GetVector(0, o);
    LONG id = InterlockedIncrement(&g_kSeq);
    char pkt[96];
    int len = _snprintf(pkt, sizeof pkt - 1, "K %.1f %.1f %.1f %ld", o[0], o[1], o[2], (long)id);
    if (len > 0 && g_peerKnown && g_udpSock != INVALID_SOCKET) {
        struct sockaddr_in to; memset(&to, 0, sizeof to);
        to.sin_family = AF_INET; to.sin_addr.s_addr = g_peerAddr; to.sin_port = htons((u_short)g_peerPort);
        sendto(g_udpSock, pkt, len, 0, (struct sockaddr*)&to, sizeof to);
        sendto(g_udpSock, pkt, len, 0, (struct sockaddr*)&to, sizeof to);
        LOGF("kill event sent #%ld at %.0f %.0f %.0f", (long)id, o[0], o[1], o[2]);
    } else LOGF("kill event #%ld NOT sent (no peer yet)", (long)id);
}
static void __cdecl H_coop_send_kill_key(void) {        /* coop_send_kill_key(origin, key): like coop_send_kill, plus the key of the killed character (test switch coop_keykill) */
    int count = *(volatile int*)(ULONG_PTR)E_PARAMCOUNT;
    if (count < 2) return;
    float o[3]; char key[40] = "";
    E_GetVector(0, o);
    if (!E_GetStringCopy(1, key, sizeof key)) key[0] = 0;
    { char* q; for (q = key; *q; q++) if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '_')) { *q = 0; break; } }
    if (strlen(key) > 30) key[30] = 0;
    LONG id = InterlockedIncrement(&g_kSeq);
    char pkt[112];
    int len = key[0] ? _snprintf(pkt, sizeof pkt - 1, "K %.1f %.1f %.1f %ld %s", o[0], o[1], o[2], (long)id, key) : _snprintf(pkt, sizeof pkt - 1, "K %.1f %.1f %.1f %ld", o[0], o[1], o[2], (long)id);
    if (len > 0 && g_peerKnown && g_udpSock != INVALID_SOCKET) {
        struct sockaddr_in to; memset(&to, 0, sizeof to);
        to.sin_family = AF_INET; to.sin_addr.s_addr = g_peerAddr; to.sin_port = htons((u_short)g_peerPort);
        sendto(g_udpSock, pkt, len, 0, (struct sockaddr*)&to, sizeof to);
        sendto(g_udpSock, pkt, len, 0, (struct sockaddr*)&to, sizeof to);
        LOGF("kill event sent #%ld at %.0f %.0f %.0f key %s", (long)id, o[0], o[1], o[2], key[0] ? key : "-");
    } else LOGF("kill event #%ld NOT sent (no peer yet)", (long)id);
}
static void __cdecl H_coop_kill_count(void) { E_AddInt((int)(g_kInHead - g_kInTail)); }       /* int coop_kill_count(): received kills waiting */
static void __cdecl H_coop_pop_kill(void) {                                                    /* vector coop_pop_kill(): the oldest one */
    float v[3] = { 0, 0, 0 };
    LONG t = g_kInTail;
    g_lastKillKey[0] = 0;
    if (g_kInHead - t > 0) { KillEv* e = &g_kIn[t & 15]; v[0] = e->p[0]; v[1] = e->p[1]; v[2] = e->p[2]; strcpy(g_lastKillKey, e->key); g_kInTail = t + 1; }
    E_AddVector(v);
}
static void __cdecl H_coop_kill_key(void) { E_AddString(g_lastKillKey); }                      /* string coop_kill_key(): the key of the kill event popped last, or "" */
static void __cdecl H_coop_pending_level(void) {        /* string coop_pending_level(): a requested level name, or "" */
    char nm[40];
    EnterCriticalSection(&L); strcpy(nm, g_pendLevel); g_pendLevel[0] = 0; LeaveCriticalSection(&L);
    E_AddString(nm);
}
/* int coop_player_link(): 1 while the local player is linked to another entity (a vehicle, a ride rig, a cut-scene mover), else 0.
   Engine facts (ENGINE.md): a linked entity has a link node at entity+0x1C8 whose first field is the parent entity; the player is entity 0
   (base 0x1B15F00, stride 0x1FC). Changes are logged with the parent's entity number and classname. */
static volatile LONG g_lastParent = -1;
static void __cdecl H_coop_player_parent(void) { E_AddInt(g_lastParent > 0 ? (int)g_lastParent : 0); }   /* int coop_player_parent(): entity number of the parent we are linked to (after coop_player_link) */
static void __cdecl H_coop_is_guest(void) { E_AddInt(g_mode == 2 && !SoloMirror() ? 1 : 0); }   /* int coop_is_guest(): 1 on the guest PC (not in the solo test) */
static void __cdecl H_coop_pop_peer_death(void) { E_AddInt((int)InterlockedExchange(&g_peerDeathFx, 0)); }   /* int coop_pop_peer_death(): 1 once when the partner died: the script kills the local player (the engine then shows its own death screen and reloads the checkpoint) */
static void __cdecl H_coop_death_started(void) { LOGF("follow: the local player died by the script (the death screen is on); the explicit load of the shared checkpoint follows"); }   /* coop_death_started(): a log line only (the engine's own reload picked a different file in testing, so the explicit load is always made) */
static void __cdecl H_coop_pop_teleport(void) { E_AddInt((int)InterlockedExchange(&g_tpPending, 0)); }   /* int coop_pop_teleport(): 1 once after F8 */
static void __cdecl H_coop_peer_health(void) { E_AddInt((int)g_peerHealth); }                             /* int coop_peer_health(): the peer's health (0..999, 100 = full) */
static void __cdecl H_coop_peer_parent(void) { E_AddInt((int)g_peerParent); }                              /* int coop_peer_parent(): the peer's parent entity number */
static void __cdecl H_coop_player_link(void) {
    DWORD node = 0; int r = 0;
    if (SC(&node, (void*)(ULONG_PTR)(0x01B15F00u + 0x1C8u), 4) && node) r = 1;
    static LONG last = -1;
    if (r != last) {
        char cls[48] = ""; DWORD par = 0; int pn = -1;
        if (r) {
            if (SC(&par, (void*)(ULONG_PTR)node, 4) && par >= 0x01B15F00u && par < 0x01B15F00u + 0x7FEu * 0x1FCu) {
                pn = (int)((par - 0x01B15F00u) / 0x1FCu);
                unsigned short h = 0; DWORD sb = 0;
                if (SC(&h, (void*)(ULONG_PTR)(par + 0x118u), 2) && SC(&sb, (void*)0x008AED00, 4) && h) RStr(sb + (DWORD)h * 8u + 4u, cls, sizeof cls);
            }
        }
        LOGF("script: player link %d -> %d (parent entity %d, classname '%s')", (int)last, r, pn, cls);
        g_lastParent = (r && strstr(cls, "vehicle")) ? pn : 0;   /* only a real vehicle is a seat; ride rigs and cut-scene cameras (script_model) are not */
        last = r;
    }
    E_AddInt(r);
}
static void __cdecl H_coop_peer_solo(void) { E_AddInt(SoloMirror() ? 1 : 0); }   /* int coop_peer_solo(): 1 when the peer is this very game (JOIN 127.0.0.1) */
static void __cdecl H_coop_peer_event(void) { E_AddInt((int)g_peerEvt); }   /* int coop_peer_event(): counter*8+type, changes on every melee/grenade of the peer */
static void __cdecl H_coop_peer_seq(void) { E_AddInt((int)g_peerSeq); }    /* int coop_peer_seq(): changes with every peer packet */

static void __cdecl H_coop_version(void) {              /* string coop_version() */
    LOGF("script: coop_version() was called from GSC");
    E_AddString(BRIDGE_VERSION);
}
static void __cdecl H_coop_modver(void) {                /* string coop_modver() -> the version alone, e.g. "0.7.5" */
    const char* s = strchr(BRIDGE_VERSION, ' ');
    E_AddString(s ? s + 1 : BRIDGE_VERSION);
}
static void __cdecl H_coop_ping(void) {                 /* int coop_ping(int n) -> n + 1: proves parameters in and a result out */
    int count = *(volatile int*)(ULONG_PTR)E_PARAMCOUNT;
    int v = count >= 1 ? E_GetInt(0) : 0;                /* read first, push after */
    LOGF("script: coop_ping(%d) -> %d (%d parameter%s)", v, v + 1, count, count == 1 ? "" : "s");
    E_AddInt(v + 1);
}

// Called by the d3d9 proxy right after it loads this copy: register the built-ins. Older proxies never call it.
__declspec(dllexport) void __cdecl CoopInit(const CoopApi* api) {
    g_capi = api;
    if (!api || api->version != 1) { LOGF("script built-ins: unknown CoopApi version, none registered"); return; }
    int a = api->RegisterFunction("coop_version", H_coop_version);
    int b = api->RegisterFunction("coop_ping", H_coop_ping);
    api->RegisterFunction("coop_modver", H_coop_modver);
    int c = api->RegisterFunction("coop_publish_state", H_coop_publish);
    int d = api->RegisterFunction("coop_peer_state", H_coop_peer_state);
    int e = api->RegisterFunction("coop_peer_yaw", H_coop_peer_yaw);
    int f = api->RegisterFunction("coop_peer_seq", H_coop_peer_seq);
    int g = api->RegisterFunction("coop_peer_stance", H_coop_peer_stance);
    int h = api->RegisterFunction("coop_peer_event", H_coop_peer_event);
    int k = api->RegisterFunction("coop_peer_solo", H_coop_peer_solo);
    int w = api->RegisterFunction("coop_peer_weapon", H_coop_peer_weapon);
    int ks = api->RegisterFunction("coop_send_kill", H_coop_send_kill);
    api->RegisterFunction("coop_pending_level", H_coop_pending_level);
    api->RegisterFunction("coop_player_link", H_coop_player_link);
    api->RegisterFunction("coop_player_parent", H_coop_player_parent);
    api->RegisterFunction("coop_peer_parent", H_coop_peer_parent);
    api->RegisterFunction("coop_peer_health", H_coop_peer_health);
    api->RegisterFunction("coop_pop_teleport", H_coop_pop_teleport);
    api->RegisterFunction("coop_pop_peer_death", H_coop_pop_peer_death);
    api->RegisterFunction("coop_death_started", H_coop_death_started);
    api->RegisterFunction("coop_is_guest", H_coop_is_guest);
    api->RegisterFunction("coop_send_flag", H_coop_send_flag);
    api->RegisterFunction("coop_send_grenade", H_coop_send_grenade);
    api->RegisterFunction("coop_send_tank", H_coop_send_tank);
    api->RegisterFunction("coop_barrier_begin", H_coop_barrier_begin);
    api->RegisterFunction("coop_apply_host_skill", H_coop_apply_host_skill);
    api->RegisterFunction("coop_barrier_active", H_coop_barrier_active);
    api->RegisterFunction("coop_link_active", H_coop_link_active);
    api->RegisterFunction("coop_tank_pos", H_coop_tank_pos);
    api->RegisterFunction("coop_tank_ang", H_coop_tank_ang);
    api->RegisterFunction("coop_tank_turret", H_coop_tank_turret);
    api->RegisterFunction("coop_tank_fire", H_coop_tank_fire);
    api->RegisterFunction("coop_tank_age", H_coop_tank_age);
    api->RegisterFunction("coop_pop_grenade", H_coop_pop_grenade);
    api->RegisterFunction("coop_grenade_org", H_coop_grenade_org);
    api->RegisterFunction("coop_grenade_vel", H_coop_grenade_vel);
    api->RegisterFunction("coop_pop_flag", H_coop_pop_flag);
    api->RegisterFunction("coop_flag_applied", H_coop_flag_applied);
    api->RegisterFunction("coop_log", H_coop_log);
    int kc = api->RegisterFunction("coop_kill_count", H_coop_kill_count);
    int kp = api->RegisterFunction("coop_pop_kill", H_coop_pop_kill);
    api->RegisterFunction("coop_send_kill_key", H_coop_send_kill_key);
    api->RegisterFunction("coop_kill_key", H_coop_kill_key);
    LOGF("script built-ins registered (coop_version %s, coop_ping %s, publish %s, peer_state %s, peer_yaw %s, peer_seq %s, peer_stance %s, peer_event %s, peer_solo %s, peer_weapon %s, kill built-ins %s); engine lookup hooks %s",
         a ? "ok" : "FAILED", b ? "ok" : "FAILED", c ? "ok" : "FAILED", d ? "ok" : "FAILED", e ? "ok" : "FAILED", f ? "ok" : "FAILED", g ? "ok" : "FAILED", h ? "ok" : "FAILED", k ? "ok" : "FAILED", w ? "ok" : "FAILED", (ks && kc && kp) ? "ok" : "FAILED", api->hooksInstalled ? "installed" : "NOT installed: scripts cannot see them");
}

// Called by the d3d9 proxy before it frees this copy and loads a newer file (live reload). Returns 1 when every thread has
// left and everything is released (the DLL may be freed), 0 when a thread did not stop in time (do NOT free the DLL).
__declspec(dllexport) int __cdecl CoopStop(void) {
    if (!F) return 1;                                  // never started (not the single-player exe): nothing to stop
    LOGF("bridge %s: stop requested (live reload)", BRIDGE_VERSION);
    RemoveRaw();
    InterlockedExchange(&g_quit, 1);
    if (g_capi) g_capi->UnregisterAll();               // the proxy's stubs stay, but stop pointing into this DLL
    if (g_udpSock != INVALID_SOCKET) { SOCKET s = g_udpSock; g_udpSock = INVALID_SOCKET; closesocket(s); }   // wakes recvfrom
    if (g_nth > 0 && WaitForMultipleObjects((DWORD)g_nth, g_th, TRUE, 8000) == WAIT_TIMEOUT) {
        LOGF("bridge stop: a thread is still running after 8 s, this copy stays loaded (restart the game to load a newer bridge)");
        return 0;
    }
    for (int i = 0; i < g_nth; i++) CloseHandle(g_th[i]);
    g_nth = 0;
    OpenLogSink(NULL, 0);
    LOGF("bridge %s: stopped", BRIDGE_VERSION);
    if (CSVF) { fclose(CSVF); CSVF = NULL; }
    if (g_mutex) { ReleaseMutex(g_mutex); CloseHandle(g_mutex); g_mutex = NULL; }   // the next copy takes it
    { FILE* f = F; F = NULL; fclose(f); }
    return 1;
}
