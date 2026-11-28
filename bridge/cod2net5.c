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
#define _CRT_RAND_S                 /* rand_s: the session token (audit F1) */
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
#include "secure.h"
#include "bridge_threads.h"          /* checks of data from the other PC (audit 2026-11-22), tested by tools/test_secure.c */
#include <tlhelp32.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

/* WSAECONNRESET on a UDP socket is not a real connection error: Windows reports it when an ICMP
   port-unreachable arrives for an earlier send. It made recvfrom() return -1 every few hundred ms
   after the partner closed the game (517 "recv err 10054" lines in one session). Disable it on the
   listening socket with SIO_UDP_CONNRESET; the link timeout below handles a lost peer. */
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

/* A peer that sent nothing for this long may be replaced, even when it had a session token. The
   old code required the same token forever, so a guest that restarted (new random token) could
   never reconnect to a host that was still running. */
#define PEER_TIMEOUT_MS 5000.0

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
/* Stage 1 of the world synchronisation: a death that names the actor ("<key>#<n>", see SecWorldId in secure.h) instead of a position, so the
   partner kills THAT actor and not the nearest one. The host is the authority: the guest sends a claim, the host applies it to its own copy and
   answers with the authoritative death; the script only ever applies "this exact actor dies", never "somebody near this point". */
typedef struct { char wid[44]; LONG seq; } WorldEv;
static WorldEv g_wIn[16]; static volatile LONG g_wInHead, g_wInTail;   /* received deaths, waiting for the script (coop_world_pop) */
static volatile LONG g_wSeq;                                           /* our outgoing sequence numbers */
/* Resync: the guest's world is fresh after its own start, after F7 and after a reconnect, so everything it learned about deaths and health is
   gone. It asks ("WR"), the host answers with WD/WH for every actor it knows (coop_world_snapshot in the script). No new state format: the same
   two events, replayed as a snapshot. */
static volatile LONG g_wSnapDue = 0;                                   /* host: a "WR" arrived, the script has not answered it yet */
/* Stage 2 (coarse): where the host's actors stand. The engine refuses `.origin` on an AI ("cannot directly set the origin on AI. Use the
   teleport command instead."), and the game's own scripts move characters with `ent teleport(origin, angles)`; the guest does the same when
   its copy has drifted away from the host's point. */
typedef struct { char wid[44]; int p[3]; int yaw; } WorldPosEv;
static WorldPosEv g_posIn[32]; static volatile LONG g_posHead, g_posTail;
static volatile int g_lastPos[4];                                      /* x y z yaw of the entry popped last */
static LONG g_wSeen[32]; static int g_wSeenN;                          /* sequence numbers already accepted (each packet is sent 3 times) */
/* Stage 1, second half: health of the same actors. "WH" carries the host's authoritative health of one actor, "HD" a damage claim of the
   guest; both are keyed by the same world id, so a wound and a death always talk about the same character. */
typedef struct { char wid[44]; int val; LONG seq; } WorldNumEv;
static WorldNumEv g_hpIn[16]; static volatile LONG g_hpHead, g_hpTail;      /* WH, waiting for the script (coop_world_state_pop) */
static WorldNumEv g_hitIn[16]; static volatile LONG g_hitHead, g_hitTail;   /* HD, waiting for the script (coop_world_hit_pop) */
static LONG g_hitSeen[32]; static int g_hitSeenN;                           /* HD sequence numbers already applied */
/* Stage 5, enemy fire: the host reports damage its enemies deal to the guest's player; the frozen enemies on the guest never shoot by themselves, so without this channel the guest can be shot at only by the engine's own logic. */
static WorldNumEv g_phIn[16]; static volatile LONG g_phHead, g_phTail;      /* PH, waiting for the script (coop_player_hit_pop) */
static volatile LONG g_lastPhDmg = 0;


static volatile LONG g_hpSeq, g_hitSeq;                                     /* our outgoing sequence numbers for WH / HD */
static volatile int g_lastHpVal = 0, g_lastHitVal = 0;                      /* the value of the entry popped last */
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
/* The version gate. A different build may use another world id format, so applying its world state would corrupt this
   PC's world (a kill on a neighbour, a body moved to the wrong actor) instead of failing visibly. While the mismatch is
   being reported, world packets are neither applied nor sent; the status line already says "VERSION MISMATCH". */
static int VerMismatch(void) { return g_verBadMs != 0; }   /* sticky: only a matching "V" report clears it (was a 10 s window, so a lost packet reopened the gate) */
static GrenEv g_gIn[8]; static GrenEv g_gLast; static volatile LONG g_gHead = 0, g_gTail = 0;
static volatile LONG g_tpPending = 0;                       /* F8 was pressed in the game window: the script teleports the local player to the partner */
static volatile LONG g_peerDeathFx = 0;                      /* 1 once when the partner's death is answered: the script fades the screen out for the survivor */
static volatile DWORD g_deathLoadAt = 0; static char g_deathLoadCmd[160];   /* the survivor's explicit loadgame, delayed so that the fade can play */
static char g_deathLoadMap[40];                          /* the mission that was running when that load was scheduled: it is dropped if the mission has changed meanwhile */
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

#define BRIDGE_VERSION "cod2net5 0.7.9"
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
#ifdef COOP_LOCAL_TEST
#define LOOPBACK_OK 1                         /* a build for local tests: JOIN may point at this very PC (127.x) */
#else
#define LOOPBACK_OK 0                         /* release build: a peer must be another PC */
#endif
static char g_lastHost[64];                  // the host that last answered us (coop_lasthost.txt): the fallback of "auto"

// ---- live reload: the d3d9 proxy can stop this copy (CoopStop), free it and load a newer file without a game restart ----
static volatile LONG g_quit = 0;             // set by CoopStop: every thread leaves its loop
/* threads now live in bridge_threads.c: a thread that is not registered there is not waited for by CoopStop,
   and the proxy frees the DLL as soon as CoopStop returns 1 (the crash of 2026-11-25). */
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
static char g_logPath[MAX_PATH]; static long g_logBytes;   /* the path and the size of this session's part (audit F5: a cap) */
static FILE* CSVF;    // coop_net.csv: one row per packet, for latency / loss analysis
static FILE* WLOGF;   // coop_world.csv: the world dump the script makes for the divergence measurement (tools/compare_world.py)
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

/* audit F1: the guest picks a random 64-bit token when it starts and sends it in the hello and as the last word of every pose packet (" T<digits>").
   The host remembers the token of the guest that holds the slot and does not give the slot to a sender from ANOTHER address without that token. */
static unsigned long long g_myTok = 0;        /* guest: this game's token */
static unsigned long long g_peerTok = 0;      /* host: the token of the guest that holds the slot (0: it sent none, an older version) */
static unsigned long long MyTok(void) {
    if (!g_myTok) { unsigned int a = 0, b = 0; rand_s(&a); rand_s(&b); g_myTok = ((unsigned long long)a << 32) | b; if (!g_myTok) g_myTok = 1; }
    return g_myTok;
}
/* audit F12: test commands ("anim", "level", "C ...") and the loopback address as a partner are accepted only while the file coop_testmode.txt
   exists in the game folder (a build for local tests, LOOPBACK_OK, keeps accepting loopback packets) */
static int TestMode(void) { char tp[MAX_PATH]; PathIn(tp, sizeof tp, "coop_testmode.txt"); return GetFileAttributesA(tp) != INVALID_FILE_ATTRIBUTES; }
/* audit F5: coop5.log keeps at most about 24 MB per part; the full part becomes coop5.2.log (one generation) and a new coop5.log is started. Called with the log lock held. */
static void LogCap(void) {
    if (!F || !g_logPath[0]) return;
    fclose(F); F = NULL;
    char old[MAX_PATH]; PathIn(old, sizeof old, "coop5.2.log");
    DeleteFileA(old); MoveFileA(g_logPath, old);
    F = fopen(g_logPath, "a");
    g_logBytes = 0;
    if (F) fprintf(F, "[%d] log size cap: the earlier part of this session is in coop5.2.log\n", g_tag);
}

/* Addresses in the log are masked ("26.1.2.3" -> "26.1.x.x"; 127.x and 0.x stay): a log may be posted or sent around. A file coop_unmask_ips.txt
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
    va_list a; va_start(a, f); int need = _vsnprintf(msg, sizeof msg - 1, f, a); va_end(a);
    msg[sizeof msg - 1] = 0;
    if (need < 0 || need >= (int)sizeof msg - 1) {   /* the line was cut: say so instead of losing the tail silently */
        size_t l = strlen(msg);
        if (l + 15 < sizeof msg) strcpy(msg + l, "...[truncated]");
    }
    SecSanitize(msg);          /* audit F4: a packet can carry a line feed: a log line stays ONE line of plain text */
    SecHideToken(msg);         /* the session token is not written to a log that is sent around */
    { static int unmask = -1;
      if (unmask < 0) { char up[MAX_PATH]; PathIn(up, sizeof up, "coop_unmask_ips.txt"); unmask = GetFileAttributesA(up) != INVALID_FILE_ATTRIBUTES; }
      if (!unmask) MaskIps(msg); }
    WallStr(ts, sizeof ts);
    EnterCriticalSection(&L);
    if (g_logBytes > 24L * 1024 * 1024) LogCap();
    if (F) { fprintf(F, "[%d] %s %s\n", g_tag, ts, msg); fflush(F); g_logBytes += (long)strlen(msg) + 24; }
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
static double g_lastPeerMs = 0;                 /* last packet of ANY kind from the linked peer: link timeout and reconnect use it */
static double PeerSilentMs(void) {              /* ms since the last accepted peer packet; 1e9 when no packet was ever seen */
    EnterCriticalSection(&L);
    double last = g_lastPeerMs;
    LeaveCriticalSection(&L);
    return last > 0 ? NowMs() - last : 1e9;
}

static int SC(void* d, const void* s, size_t n) {    __try { memcpy(d, s, n); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* guarded write into the game: only into a committed, writable, non-guard page (the rule from PLAYBOOK section 10:
   a blind write into .rdata crashed a copy once). Used by the Stage 5 freeze test on entity+0x138. */
static int SW(void* d, const void* s, size_t n) {    MEMORY_BASIC_INFORMATION b;
    if (!VirtualQuery(d, &b, sizeof b)) return 0;
    if (b.State != MEM_COMMIT) return 0;
    if (!(b.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE))) return 0;
    if (b.Protect & PAGE_GUARD) return 0;
    __try { memcpy(d, s, n); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static void AnimApplyPacket(const char* b);      /* stage 5 animation channel: defined below with the bind table, used by the UDP thread */

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
    if (!nl) return 0;
    for (int k = 0; k < nr && n < max; k++) {
        size_t s = g[k].s, o = 0;
        while (o < s && n < max) {
            size_t w = (s - o > CH) ? CH : (s - o);
            if (SC(BU, g[k].p + o, w)) {
                /* i + nl <= w: memcmp must not read past the chunk (BU holds exactly w bytes here; on the
                   last chunk a cmp could run past the region as well). A name straddling the chunk end is
                   caught by the overlap of the next step, not by reading past the buffer. */
                for (size_t i = 0; i + nl <= w; i++) {
                    if (BU[i] != nd[0]) continue;
                    if (memcmp(BU + i, nd, nl)) continue;
                    out[n++] = (DWORD)(ULONG_PTR)(g[k].p + o + i);
                    if (n >= max) break;
                }
            }
            o += (w > nl) ? (w - (nl - 1)) : w;      /* overlap nl-1 bytes: no name is lost at a chunk edge */
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
                for (size_t j = 0; j + 4 <= w; j += 4) {
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
            g_peerAuto = 0; g_peerKnown = 0; g_peerTok = 0; g_peerPort = MENU_PORT; g_logPort = 0;
            OpenLogSink(NULL, 0);
            int wasHost = LOOPBACK_OK && ((g_mode == 1) || !strcmp(g_lastHost, "127.0.0.1"));                 // this very game was the host a moment ago (solo test): it is the host to join
            g_discover = 1;
            MenuEnable(2);
            LOGF("menu: JOIN auto - looking for a host on the network (beacons on UDP %d)%s%s", BEACON_PORT,
                 g_lastHost[0] ? "; fallback after 10 s: the last host " : "", g_lastHost);
            if (wasHost) { LOGF("menu: JOIN auto: this game was the host / the last host is 127.0.0.1 (solo test): joining 127.0.0.1 now (a real beacon still wins)"); SetPeerJoin("127.0.0.1", 1); }
            return;
        }
        if (!ValidIPv4(ip)) { LOGF("menu: JOIN needs 'auto' or a valid IPv4 address of the host (a.b.c.d), got '%s'", ip); return; }
        SetPeerJoin(ip, 0);
        return;
    }
    g_discover = 0;
    strcpy(g_peerIp, "(auto: first guest packet)");
    g_peerAuto = 1; g_peerKnown = 0; g_peerTok = 0; g_peerPort = MENU_PORT; g_logPort = 0;
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

static void PhApply(const char* b) {   /* "PH <world id> <damage>": queue one player-hit event for the script */
    char wid[44]; long pdmg = -1;
    if (sscanf(b + 3, "%43s %ld", wid, &pdmg) != 2 || !SecWorldId(wid) || pdmg <= 0 || pdmg > 300) {
        LOGF("player hit packet refused: %s", b);
        return;
    }
    LONG h = g_phHead;
    if (h - g_phTail < 16) {
        WorldNumEv* e = &g_phIn[h & 15];
        strcpy(e->wid, wid); e->val = (int)pdmg;
        g_phHead = h + 1;
        LOGF("player hit queued: %s dmg=%ld", wid, pdmg);
    } else { g_phTail = h - 15; LOGF("player hit dropped (the script is not consuming it): %s", wid); }
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
    {   /* keep ICMP "port unreachable" from turning into a permanent WSAECONNRESET stream */
        BOOL off = FALSE; DWORD ignored = 0;
        if (WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &ignored, NULL, NULL) != 0)
            LOGF("udp: could not disable WSAECONNRESET reporting (error %d); a lost peer may log recv errors", WSAGetLastError());
    }
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(port);
    // loopback unless the peer is on another machine (then the packets arrive from the LAN).
    // The listener only parses numbers into cvar strings; it is still reachable from the LAN while open.
    // ALWAYS all interfaces: the socket lives as long as the game and the peer may change from 127.0.0.1 (the fast solo join) to a real address;
    // a socket bound to loopback cannot send to any other address (WSAENETUNREACH), which left a guest silent (host rx=0) after the 0.7.7 fast join.
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
        if (r <= 0) {
            if (g_quit) break;
            int e = WSAGetLastError();
            static DWORD lastErrLog = 0; static int lastErr = 0;
            DWORD nowt = GetTickCount();
            if (e != lastErr || nowt - lastErrLog > 30000) { lastErr = e; lastErrLog = nowt; LOGF("recv err %d", e); }
            Sleep(e == WSAECONNRESET ? 10 : 50);
            continue;
        }
        b[r] = 0;
        double peerSilent = PeerSilentMs();
        int peerStale = peerSilent > PEER_TIMEOUT_MS;
        {   /* WHO may talk to us. Before this check every packet type ("level <map>" included: it changes the mission) was accepted from ANY address that
               could reach the port. Now: this PC (the test tools), the partner, and nobody else. A pose packet of a new sender is still handed to the
               existing "reconnect after 5 s of silence" rule below; before a peer is known only "H" (hello) and pose packets pass. */
            unsigned long fsrc = from.sin_addr.s_addr;
            int isLoop = (fsrc == inet_addr("127.0.0.1"));
            int isPose = ((b[0] >= '0' && b[0] <= '9') || b[0] == '-' || b[0] == '.');
            int isHello = (b[0] == 'H' && b[1] == ' ');
            int allowed = isLoop && (TestMode() || LOOPBACK_OK);       /* audit F12: another process on this PC is not the partner unless a test is on */
            if (!allowed)
                allowed = SecPeerAdmit(g_mode, g_peerKnown, g_peerAuto, fsrc == g_peerAddr,
                                       isPose, isHello, g_peerTok, b, peerStale);
            if (!allowed) {
                static DWORD lastBadLog = 0; static unsigned long lastBadSrc = 0; DWORD tnow = GetTickCount();
                if (fsrc != lastBadSrc || tnow - lastBadLog > 30000) {
                    lastBadSrc = fsrc; lastBadLog = tnow;
                    LOGF("ignored a packet from %s: that is not the partner (first byte '%c', %d bytes)", inet_ntoa(from.sin_addr), (b[0] >= 32 && b[0] < 127) ? b[0] : '?', r);
                }
                continue;
            }
            if ((!strncmp(b, "anim ", 5) || !strncmp(b, "level ", 6) || !strncmp(b, "C ", 2)) && !TestMode()) {
                static DWORD lastTc; if (GetTickCount() - lastTc > 30000) { lastTc = GetTickCount(); LOGF("ignored a test command: create the file coop_testmode.txt in the game folder to allow them"); }
                continue;
            }
            if ((!strncmp(b, "anim ", 5) || !strncmp(b, "level ", 6)) && !isLoop) { LOGF("ignored a test command from %s (only this PC may send it)", inet_ntoa(from.sin_addr)); continue; }
            if (!strncmp(b, "C ", 2)) LOGF("test cmd raw: [%s]", b);
        }
        { EnterCriticalSection(&L); g_lastPeerMs = NowMs(); LeaveCriticalSection(&L); }   /* any accepted packet keeps the link alive */
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
        if (!strncmp(b, "C wp ", 5) && from.sin_addr.s_addr == inet_addr("127.0.0.1")) {   // dev test from this PC only: "C wp <world id> <x> <y> <z> <yaw>" injects one world-position event, exactly as a WP packet from the host would
            char pkt[128]; _snprintf(pkt, sizeof pkt - 1, "WP %s", b + 5); pkt[sizeof pkt - 1] = 0;
            char wid[44]; long wx, wy, wz, wyaw;
            if (SecWorldPosPkt(pkt, wid, sizeof wid, &wx, &wy, &wz, &wyaw)) {
                LONG h = g_posHead;
                if (h - g_posTail < 32) {
                    WorldPosEv* e = &g_posIn[h & 31];
                    strcpy(e->wid, wid);
                    e->p[0] = (int)wx; e->p[1] = (int)wy; e->p[2] = (int)wz; e->yaw = (int)wyaw;
                    g_posHead = h + 1;
                    LOGF("test: world position injected: %s %ld %ld %ld yaw %ld", wid, wx, wy, wz, wyaw);
                } else LOGF("test: world position queue is full");
            } else LOGF("test: bad world position command '%s'", b);
            continue;
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
        if (!strncmp(b, "C map ", 6) && from.sin_addr.s_addr == inet_addr("127.0.0.1")) {   // dev test from this PC only: "C map <name>" starts a level through the engine's own console command (the same route MissionFollow uses)
            char nm[40]; int n2 = 0;
            for (const char* q = b + 6; *q && n2 < 38; q++) {
                if ((*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9') || *q == '_') nm[n2++] = *q; else break;
            }
            nm[n2] = 0;
            if (n2 > 0) { char cmd[64]; _snprintf(cmd, sizeof cmd, "map %s\n", nm); LOGF("test: console \"map %s\" %s", nm, CbufAdd(cmd) ? "queued" : "FAILED"); }
            continue;
        }
        if (!strncmp(b, "C set ", 6) && from.sin_addr.s_addr == inet_addr("127.0.0.1")) {   // dev test from this PC only: "C set <cvar> <value>" queues the console command (test switches at runtime)
            char nm[32]; int n2 = 0; const char* q = b + 6;
            while (*q && *q != ' ' && n2 < 30) { char c = *q++; if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') nm[n2++] = c; else break; }
            nm[n2] = 0;
            char val[32]; int n3 = 0;
            while (*q == ' ') q++;
            while (*q && n3 < 30) { char c = *q++; if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '#' || c == '-') val[n3++] = c; else break; }
            val[n3] = 0;
            if (n2 > 0) { char cmd[96]; _snprintf(cmd, sizeof cmd, "set %s %s\n", nm, val); LOGF("test: console \"set %s %s\" %s", nm, val, CbufAdd(cmd) ? "queued" : "FAILED"); }
            continue;
        }
        if (!strncmp(b, "C freeze ", 9) && from.sin_addr.s_addr == inet_addr("127.0.0.1")) {   // dev test from this PC only: "C freeze <entity number> <0|1>" stops the actor's think by pushing its next-think time out of reach
            /* Stage 5 groundwork: FUN_004DA090 runs an actor's think only when 0 < entity+0x138 <= current time (ms),
               so a value far in the future makes the dispatcher skip that actor forever, per actor and without a code
               patch. The think function itself would re-arm the timer, but it never runs. */
            int ent = -1, on = 1;
            if (sscanf(b + 9, "%d %d", &ent, &on) >= 1 && ent >= 0 && ent < 0x800) {
                DWORD addr = 0x1B15F00u + (DWORD)ent * 0x1FCu + 0x138u;
                /* IMPORTANT: the dispatcher runs the think only when 0 < next-think <= now, so writing 0 does NOT resume the
                   actor, it disables it forever as well. A positive value in the past (1) makes it fire on the next tick; the
                   think function then re-arms the field itself. */
                int v = on ? 0x7FFFFFF0 : 1;
                if (SW((void*)(ULONG_PTR)addr, &v, 4))
                    LOGF("test: think %s for entity %d (next-think at 0x%08lX = 0x%08X)", on ? "frozen" : "resumed", ent, (unsigned long)addr, (unsigned)v);
                else LOGF("test: cannot write entity %d next-think at 0x%08lX", ent, (unsigned long)addr);
            } else LOGF("test: bad freeze command '%s'", b);
            continue;
        }
        if (!strncmp(b, "C ap ", 5) && from.sin_addr.s_addr == inet_addr("127.0.0.1")) {   // dev test from this PC only: "C ap <world id> <8 hex words>" feeds an animation packet through the real handler
            char pkt[200]; _snprintf(pkt, sizeof pkt - 1, "AP %s", b + 5); pkt[sizeof pkt - 1] = 0;
            AnimApplyPacket(pkt);
            LOGF("test: animation packet injected: %s", pkt);
            continue;
        }
        if (!strncmp(b, "C pose ", 7) && from.sin_addr.s_addr == inet_addr("127.0.0.1")) {   // dev test from this PC only: "C pose <srcEnt> <dstEnt>" copies the animation state (pool slot +0xD4, +0x170..0x188) from one actor to another, validated
            int se = -1, de = -1;
            if (sscanf(b + 7, "%d %d", &se, &de) == 2 && se >= 0 && se < 0x800 && de >= 0 && de < 0x800) {
                DWORD ep[2] = { 0x1B15F00u + (DWORD)se * 0x1FCu, 0x1B15F00u + (DWORD)de * 0x1FCu };
                DWORD slot[2] = { 0, 0 };
                if (SC(&slot[0], (void*)(ULONG_PTR)(ep[0] + 0x100u), 4) && SC(&slot[1], (void*)(ULONG_PTR)(ep[1] + 0x100u), 4)
                    && slot[0] > 0x400000u && slot[0] < 0x20000000u && slot[1] > 0x400000u && slot[1] < 0x20000000u) {
                    static const DWORD offs[8] = { 0x0D4, 0x170, 0x174, 0x178, 0x17C, 0x180, 0x184, 0x188 };
                    int wrote = 0;
                    for (int i = 0; i < 8; i++) {
                        DWORD v = 0;
                        if (!SC(&v, (void*)(ULONG_PTR)(slot[0] + offs[i]), 4)) continue;
                        if (i == 0) { if (v > 0x10000u) continue; }                     /* the index word */
                        else {                                                                  /* the pose words: finite, sane magnitude */
                            if ((v & 0x7F800000u) == 0x7F800000u) continue;                   /* NaN / inf */
                            float f; memcpy(&f, &v, 4);
                            if (fabsf(f) > 100000.0f) continue;
                        }
                        if (SW((void*)(ULONG_PTR)(slot[1] + offs[i]), &v, 4)) wrote++;
                    }
                    LOGF("test: pose copied from entity %d to %d (slot 0x%08lX -> 0x%08lX, %d of 8 words)", se, de,
                         (unsigned long)slot[0], (unsigned long)slot[1], wrote);
                } else LOGF("test: pose copy: cannot read the slots of %d/%d", se, de);
            } else LOGF("test: bad pose command '%s'", b);
            continue;
        }
        if (!strncmp(b, "C p", 3)) LOGF("probe: [%s] from=%s", b, inet_ntoa(from.sin_addr));
        if (!strncmp(b, "C ph ", 5) && from.sin_addr.s_addr == inet_addr("127.0.0.1")) {   /* "C ph " is five characters: a length of six compared the literal's NUL against the first letter of the id */   // dev test: "C ph <wid> <dmg>" queues a player-hit event as the host would
            char pkt[64]; _snprintf(pkt, sizeof pkt - 1, "PH %s", b + 5); pkt[sizeof pkt - 1] = 0;
            PhApply(pkt);
            LOGF("test: player hit injected: %s", pkt);
            continue;
        }
        if (!strncmp(b, "C ver ", 6) && from.sin_addr.s_addr == inet_addr("127.0.0.1")) {   // dev test from this PC only: "C ver <version>" sends this PC a fake "V" report, so the version gate can be exercised on one machine
            char pkt[64]; _snprintf(pkt, sizeof pkt - 1, "V %s", b + 6); pkt[sizeof pkt - 1] = 0;
            struct sockaddr_in to; memset(&to, 0, sizeof to);
            to.sin_family = AF_INET; to.sin_addr.s_addr = inet_addr("127.0.0.1"); to.sin_port = htons((u_short)g_peerPort);
            if (g_udpSock != INVALID_SOCKET) sendto(g_udpSock, pkt, (int)strlen(pkt), 0, (struct sockaddr*)&to, sizeof to);
            LOGF("test: injected version report '%s'", pkt);
            continue;
        }        if (!strncmp(b, "C rd ", 5) && from.sin_addr.s_addr == inet_addr("127.0.0.1")) {   // dev test from this PC only: "C rd <hexaddr> [ndwords]" logs a read-only dump of game memory
            unsigned long a2 = 0; int nd = 8;
            if (sscanf(b + 5, "%lx %d", &a2, &nd) >= 1 && a2 >= 0x00400000u && a2 < 0x20000000u) {
                if (nd < 1 || nd > 32) nd = 8;
                char line[512]; int o = 0;
                o += _snprintf(line + o, sizeof line - o, "test: read 0x%08lX:", a2);
                for (int i = 0; i < nd && o < (int)sizeof line - 16; i++) {
                    DWORD v = 0;
                    if (SC(&v, (void*)(ULONG_PTR)(a2 + (unsigned long)i * 4), 4)) o += _snprintf(line + o, sizeof line - o, " %08lX", (unsigned long)v);
                    else { o += _snprintf(line + o, sizeof line - o, " ????????"); }
                }
                LOGF("%s", line);
            } else LOGF("test: bad read command '%s'", b);
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
        if (!strcmp(b, "WR")) {          // stage 1 resync: the guest asks for a snapshot of the world (it has just loaded a world of its own)
            if (g_mode != 2 && !SoloMirror()) {
                InterlockedExchange(&g_wSnapDue, 1);
                LOGF("world snapshot requested by the guest");
            }
            continue;
        }
        /* version gate: world state from another build is dropped, not applied (see VerMismatch) */
        if ((!strncmp(b, "WD ", 3) || !strncmp(b, "WH ", 3) || !strncmp(b, "HD ", 3) || !strncmp(b, "WP ", 3) || !strncmp(b, "AP ", 3)) && VerMismatch()) {
            static LONG vdrop;
            if ((InterlockedIncrement(&vdrop) % 100) == 1) LOGF("sync paused: the partner runs another version (%ld world packets dropped)", (long)vdrop);
            continue;
        }
        if (!strncmp(b, "WD ", 3)) {     // stage 1: a death that names the actor: "WD <world id> <seq>" (a claim from the guest, the authority from the host)
            char wid[44]; long wseq = 0;
            if (SecWorldPkt(b, wid, sizeof wid, &wseq)) {
                int dup = 0;
                for (int i = 0; i < 32; i++) if (g_wSeen[i] == (LONG)wseq && wseq != 0) dup = 1;
                if (dup) { LOGF("world death dropped (copy of sequence %ld): %s", wseq, wid); continue; }
                g_wSeen[g_wSeenN++ & 31] = (LONG)wseq;
                LONG h = g_wInHead;
                if (h - g_wInTail < 16) {
                    WorldEv* e = &g_wIn[h & 15];
                    strcpy(e->wid, wid); e->seq = (LONG)wseq;
                    g_wInHead = h + 1;
                    LOGF("world death received: %s (seq %ld) %s", wid, wseq,
                         g_mode == 2 ? "from the host" : "a kill claim from the guest");
                } else LOGF("world death dropped (the script is not consuming them): %s", wid);
            } else LOGF("world death packet refused: it does not name an actor");
            continue;
        }
        if (!strncmp(b, "WH ", 3) || !strncmp(b, "HD ", 3)) {   // stage 1: health of one actor (WH host -> guest) / a damage claim (HD guest -> host)
            char wid[44]; long wval = -1, wseq = -1;
            const char* tag = b[1] == 'H' ? "WH" : "HD";
            if (SecWorldStatePkt(b, tag, wid, sizeof wid, &wval, &wseq)) {
                LONG h;
                if (b[1] == 'H') {          /* the host's health: idempotent, an out-of-order copy is harmless (the guest only ever lowers health) */
                    h = g_hpHead;
                    if (h - g_hpTail < 16) {
                        WorldNumEv* e = &g_hpIn[h & 15];
                        strcpy(e->wid, wid); e->val = (int)wval; e->seq = (LONG)wseq;
                        g_hpHead = h + 1;
                    } else LOGF("world health dropped (the script is not consuming it): %s", wid);
                } else {                    /* a damage claim must be applied exactly once */
                    int dup = 0;
                    for (int i = 0; i < 32; i++) if (g_hitSeen[i] == (LONG)wseq && wseq != 0) dup = 1;
                    if (dup) continue;
                    g_hitSeen[g_hitSeenN++ & 31] = (LONG)wseq;
                    h = g_hitHead;
                    if (h - g_hitTail < 16) {
                        WorldNumEv* e = &g_hitIn[h & 15];
                        strcpy(e->wid, wid); e->val = (int)wval; e->seq = (LONG)wseq;
                        g_hitHead = h + 1;
                        LOGF("world damage claim received: %s %ld", wid, wval);
                    } else LOGF("world damage dropped (the script is not consuming it): %s", wid);
                }
            } else LOGF("world health packet refused: bad id, value or format");
            continue;
        }
        if (!strncmp(b, "PH ", 3)) {   // stage 5: "PH <world id> <damage>": the host says that actor hit our player
            PhApply(b);
            continue;
        }
        if (!strncmp(b, "WP ", 3)) {     // stage 2: "WP <world id> <x> <y> <z> <yaw>": where that actor stands on the host
            char wid[44]; long wx, wy, wz, wyaw;
            if (SecWorldPosPkt(b, wid, sizeof wid, &wx, &wy, &wz, &wyaw)) {
                LONG h = g_posHead;
                if (h - g_posTail < 32) {
                    WorldPosEv* e = &g_posIn[h & 31];
                    strcpy(e->wid, wid);
                    e->p[0] = (int)wx; e->p[1] = (int)wy; e->p[2] = (int)wz; e->yaw = (int)wyaw;
                    g_posHead = h + 1;
                } else g_posTail = h - 31;      /* full: drop the oldest, a position is only worth the newest */
            } else LOGF("world position packet refused: bad id or format");
            continue;
        }
        if (!strncmp(b, "AP ", 3)) {     // stage 5: "AP <world id> <8 hex words>": the host's animation state for that actor
            AnimApplyPacket(b);
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
                unsigned long src = from.sin_addr.s_addr;
                unsigned long long tok = SecPktToken(b);
                int wasKnown = g_peerKnown;
                if (!wasKnown || peerStale) {
                    if (!wasKnown || src != g_peerAddr || tok != g_peerTok) {
                        EnterCriticalSection(&L);
                        g_peerAddr = src; g_peerKnown = 1; g_peerTok = tok; g_lastSeq = -1;
                        LeaveCriticalSection(&L);
                        LOGF("peer learned: %s (hello%s%s)", inet_ntoa(from.sin_addr),
                             peerStale && wasKnown ? ", previous peer silent" : ", before any pose packet",
                             tok ? "" : ", no token (old version)");
                    }
                }
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
            unsigned long long tok = SecPktToken(b);
            int wasKnown = g_peerKnown;
            if (!wasKnown) {
                EnterCriticalSection(&L);
                g_peerAddr = src; g_peerKnown = 1; g_peerTok = tok; g_lastSeq = -1;
                LeaveCriticalSection(&L);
                LOGF("peer learned: %s (first pose packet)", inet_ntoa(from.sin_addr));
            } else if (peerStale && src == g_peerAddr && tok != g_peerTok) {
                /* same PC, new process: the old session token is gone, the silence rule allows it */
                EnterCriticalSection(&L);
                g_peerTok = tok; g_lastSeq = -1;
                LeaveCriticalSection(&L);
                LOGF("peer token renewed for %s (new process, previous peer was silent for %.1f s)", inet_ntoa(from.sin_addr), peerSilent / 1000.0);
            } else if (src != g_peerAddr && peerStale) {
                EnterCriticalSection(&L);
                g_peerAddr = src; g_peerTok = tok; g_lastSeq = -1;
                LeaveCriticalSection(&L);
                LOGF("peer switched to %s (the previous one was silent for %.1f s)", inet_ntoa(from.sin_addr), peerSilent / 1000.0);
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

/* ================================================================================================================
   Read-only entity probe (engine work, Stage 5 groundwork). The script names one actor by its entity number
   (getentitynumber), the bridge samples that actor's 0x1FC-byte entity structure for a few seconds and writes what
   changed plus the pointers it found to coop_probe.txt. The point is to learn the layout — origin, angles, animation
   phase, the sentient pointer, and through it the code pointer of the AI update — BEFORE any patch is attempted.
   Nothing is written into the game: this is a measurement.
   ================================================================================================================ */
#define ENT_BASE   0x01B15F00u
#define ENT_STRIDE 0x1FCu
#define ENT_COUNT  0x7FEu
#define ACT_BASE   0x01C6051Cu          /* the actor pool (AI state): 32 slots */
#define ACT_STRIDE 0x21A4u
#define ACT_COUNT  32u
#define ACT_INUSE  0x2110u
#define PROBE_MAX  0x21A4u              /* the biggest structure probed: one actor slot */
#define PROBE_WORDS (PROBE_MAX / 4)
#define PROBE_SAMPLES 40

static struct {
    volatile LONG active;
    DWORD addr, started, n;
    DWORD size;
    int samples;
    unsigned long entnum;
    char tag[24];
    unsigned char first[PROBE_MAX], last[PROBE_MAX];
    unsigned short changed[PROBE_WORDS];
    DWORD firstv[PROBE_WORDS], lastv[PROBE_WORDS];
} g_probe;

static int ProbeLooksCode(DWORD v) { return v >= 0x00401000u && v < 0x18C0200u; }   /* the .text range of this image */
static float ProbeAsFloat(DWORD v) { float f; memcpy(&f, &v, 4); return f; }

static int ProbeBeginRaw(DWORD addr, DWORD size, int samples, const char* tag, unsigned long id) {
    if (!F) return 0;
    if (g_probe.active) { LOGF("probe: busy, '%s' ignored", tag ? tag : "?"); return 0; }
    if (!size || size > PROBE_MAX) return 0;
    if (!SC(g_probe.first, (void*)(ULONG_PTR)addr, size)) { LOGF("probe: %08lX is not readable", addr); return 0; }
    g_probe.size = size;
    g_probe.samples = samples > 0 ? samples : PROBE_SAMPLES;
    memcpy(g_probe.last, g_probe.first, size);
    memset(g_probe.changed, 0, sizeof g_probe.changed);
    for (int i = 0; i < (int)(size / 4u); i++) {
        g_probe.firstv[i] = ((DWORD*)(void*)g_probe.first)[i];
        g_probe.lastv[i] = g_probe.firstv[i];
    }
    g_probe.addr = addr; g_probe.entnum = id; g_probe.n = 0;
    g_probe.started = GetTickCount();
    g_probe.tag[0] = 0;
    if (tag) { strncpy(g_probe.tag, tag, sizeof g_probe.tag - 1); g_probe.tag[sizeof g_probe.tag - 1] = 0; }
    g_probe.active = 1;
    LOGF("probe started: '%s' id %lu at %08lX, %u bytes, %d samples", g_probe.tag, id, addr, size, g_probe.samples);
    return 1;
}

static int ProbeBegin(int entnum, const char* tag) {          /* one entity */
    if (entnum < 0 || (unsigned)entnum >= ENT_COUNT) { LOGF("probe: entity number %d is out of range", entnum); return 0; }
    return ProbeBeginRaw(ENT_BASE + (DWORD)entnum * ENT_STRIDE, ENT_STRIDE, PROBE_SAMPLES, tag, (unsigned long)entnum);
}

static int ProbeSlotBegin(int slot, const char* tag) {        /* one slot of the actor pool: the AI state, not the body */
    if (slot < 0 || (unsigned)slot >= ACT_COUNT) { LOGF("probe: actor slot %d is out of range", slot); return 0; }
    /* 0x1C6051C holds a POINTER to the pool (the allocator does `mov edx, dword ptr [0x1c6051c]`), not the pool itself */
    DWORD pool = 0;
    if (!SC(&pool, (void*)(ULONG_PTR)ACT_BASE, 4) || pool < 0x00010000u || pool >= 0x7FFF0000u) {
        LOGF("probe: actor pool pointer at %08lX is not usable (%08lX)", (unsigned long)ACT_BASE, (unsigned long)pool);
        return 0;
    }
    DWORD addr = pool + (DWORD)slot * ACT_STRIDE;
    unsigned char inUse = 0;
    if (!SC(&inUse, (void*)(ULONG_PTR)(addr + ACT_INUSE), 1)) { LOGF("probe: slot %d is not readable", slot); return 0; }
    if (!inUse) return 0;                                     /* a free slot is normal: no log line for it */
    LOGF("probe: actor pool at %08lX, slot %d at %08lX is in use", (unsigned long)pool, slot, (unsigned long)addr);
    return ProbeBeginRaw(addr, ACT_STRIDE, 24, tag, (unsigned long)slot);
}

static void ProbeFinish(void) {
    g_probe.active = 0;
    char path[MAX_PATH]; PathIn(path, sizeof path, "coop_probe.txt");
    FILE* f = fopen(path, "a");
    if (!f) { LOGF("probe: cannot write %s", path); return; }
    char cls[48] = "?";
    unsigned short h = 0; DWORD sb = 0;
    if (g_probe.size == ENT_STRIDE && SC(&h, (void*)(ULONG_PTR)(g_probe.addr + 0x118u), 2) && SC(&sb, (void*)0x008AED00, 4) && h) RStr(sb + (DWORD)h * 8u + 4u, cls, sizeof cls);
    char ts[16]; WallStr(ts, sizeof ts);
    fprintf(f, "\n=== probe %s: id %lu, addr %08lX, %u bytes, %lu samples in %lu ms, wall %s, this PC is %s\n",
            g_probe.tag, g_probe.entnum, g_probe.addr, g_probe.size, g_probe.n, (unsigned long)(GetTickCount() - g_probe.started), ts,
            SoloMirror() ? "solo" : (g_mode == 2 ? "guest" : "host"));
    fprintf(f, "classname: '%s'\n", cls);
    fprintf(f, "-- fields that changed (offset, samples changed, first -> last, as int / as float)\n");
    int nch = 0;
    int words = (int)(g_probe.size / 4u);
    for (int i = 0; i < words; i++) {
        if (!g_probe.changed[i]) continue;
        nch++;
        fprintf(f, "+0x%03X  %3u  %08lX -> %08lX  (%.2f -> %.2f)\n", i * 4, (unsigned)g_probe.changed[i],
                (unsigned long)g_probe.firstv[i], (unsigned long)g_probe.lastv[i],
                ProbeAsFloat(g_probe.firstv[i]), ProbeAsFloat(g_probe.lastv[i]));
    }
    if (!nch) fprintf(f, "(none)\n");
    fprintf(f, "-- pointers in the structure whose value never changed (a '*' marks a dword that looks like a code address)\n");
    for (int i = 0; i < words; i++) {
        DWORD v = g_probe.lastv[i];
        if (v < 0x00010000u || v >= 0x7FFF0000u) continue;
        if (g_probe.firstv[i] != v) continue;
        DWORD t[16];
        if (!SC(t, (void*)(ULONG_PTR)v, sizeof t)) continue;
        fprintf(f, "+0x%03X -> %08lX :", i * 4, (unsigned long)v);
        for (int j = 0; j < 16; j++) fprintf(f, " %08lX%s", (unsigned long)t[j], ProbeLooksCode(t[j]) ? "*" : "");
        fprintf(f, "\n");
    }
    /* Deep dump: an actor's sentient/actor objects point back at the entity (the entity pool lives at 0x1B15F00..), which is how they are
       recognised without knowing the layout. Code addresses are marked '*', so the AI entry points can be read out of the result. */
    fprintf(f, "-- deep dump of pointed-to structures that point back at this entity ('*' = code address)\n");
    for (int i2 = 0; i2 < (g_probe.size == ENT_STRIDE ? words : 0); i2++) {
        DWORD v = g_probe.lastv[i2];
        if (v < 0x00010000u || v >= 0x7FFF0000u) continue;
        DWORD back = 0;
        if (!SC(&back, (void*)(ULONG_PTR)v, 4)) continue;
        if (back != g_probe.addr) continue;
        fprintf(f, "+0x%03X -> %08lX :\n", i2 * 4, (unsigned long)v);
        for (int k = 0; k < 128; k++) {
            DWORD w = 0;
            if (!SC(&w, (void*)(ULONG_PTR)(v + (DWORD)k * 4u), 4)) break;
            if (k % 8 == 0) fprintf(f, "  %08lX:", (unsigned long)(v + (DWORD)k * 4u));
            if (ProbeLooksCode(w)) fprintf(f, " %08lX*", (unsigned long)w);
            else fprintf(f, " %08lX", (unsigned long)w);
            if (k % 8 == 7) fprintf(f, "\n");
        }
        fprintf(f, "\n");
    }
    fclose(f);
    LOGF("probe finished: %s (id %lu, %d fields changed)", path, g_probe.entnum, nch);
}

static void ProbeTick(void) {                   /* one sample, called from the WORK loop (~100 ms) */
    if (!g_probe.active) return;
    unsigned char cur[PROBE_MAX];
    if (!SC(cur, (void*)(ULONG_PTR)g_probe.addr, g_probe.size)) { LOGF("probe: id %lu became unreadable, stopped", g_probe.entnum); g_probe.active = 0; return; }
    DWORD* cw = (DWORD*)(void*)cur;
    for (int i = 0; i < (int)(g_probe.size / 4u); i++) {
        if (cw[i] != g_probe.lastv[i]) {
            if (g_probe.changed[i] < 0xFFFF) g_probe.changed[i]++;
            g_probe.lastv[i] = cw[i];
        }
    }
    memcpy(g_probe.last, cur, g_probe.size);
    if (++g_probe.n >= (DWORD)g_probe.samples) ProbeFinish();
}

/* ================================================================================================================
   Stack sampler (engine work, Stage 5 groundwork). A trigger file coop_stack.txt next to the exe starts this on the next
   bridge load: 400 samples ~10 ms apart, the game thread is suspended for the moment of the read, EIP and the EBP chain are
   captured, and the result goes to coop_stack_out.txt. Nothing is written into the game. The AI update has to be found in
   code (the engine has no vtables in the actor structures), and a hot function shows up as a repeated frame here.
   ================================================================================================================ */
static volatile DWORD g_gameTid = 0;      /* the thread that owns the game window: the main thread */
static BOOL CALLBACK DummyFindWnd(HWND h, LPARAM l) {
    DWORD pid = 0; char cls[32];
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    GetClassNameA(h, cls, sizeof cls);
    if (strcmp(cls, "CoD2")) return TRUE;
    *(HWND*)l = h; return FALSE;
}

static int StackTrigger(void) {
    char p[MAX_PATH]; PathIn(p, sizeof p, "coop_stack.txt");
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

static volatile LONG g_stackRounds = 0;
static volatile LONG g_wdSeen = 0;          /* a world death was sent: the fight has started, this is when the AI update runs */
#define SS_N 400
#define SS_D 10

static DWORD WINAPI STACKSAMP(LPVOID p) {
    (void)p;
    if (!StackTrigger()) return 0;
    /* Wait for the first enemy death instead of a fixed delay after the mission start: the AI update only runs while actors
       fight, and a sample taken at the intro is all WaitMessage (that is what the first attempt produced). */
    for (int w = 0; w < 1200 && !g_quit; w++) {           /* up to 300 s */
        if (g_wdSeen) break;
        Sleep(250);
    }
    if (!g_wdSeen) LOGF("stack: no world death in 5 minutes, sampling anyway (the frames may not be AI)");
    Sleep(1500);
    if (g_quit) return 0;
    /* the thread to sample: the one that owns the game window, else the busiest thread of the process (the game's main thread).
       A minimised game only pumps messages, so wait until its window is up: a sample taken then would be all WaitMessage. */
    for (int w2 = 0; w2 < 600 && !g_quit; w2++) {
        HWND gw0 = NULL; EnumWindows(DummyFindWnd, (LPARAM)&gw0);
        if (gw0 && !IsIconic(gw0)) break;
        Sleep(500);
    }
    HANDLE h = NULL;
    { HWND gw = NULL; EnumWindows(DummyFindWnd, (LPARAM)&gw);
      if (gw) { DWORD wid = 0; DWORD wt = GetWindowThreadProcessId(gw, &wid);   /* the RETURN value is the thread id; the out param is the process id (feeding the pid to OpenThread gave error 87 and 0 samples) */
        if (wt) { h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, wt);
                  if (h) { g_gameTid = wt; LOGF("stack: window thread %lu", (unsigned long)wt); }
                  else LOGF("stack: window thread %lu: OpenThread error %lu", (unsigned long)wt, GetLastError()); } } }
    if (!h) {                                              /* fallback: the thread that burnt the most CPU is the game loop */
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te; memset(&te, 0, sizeof te); te.dwSize = sizeof te;
            ULONGLONG best = 0; DWORD bestTid = 0;
            if (Thread32First(snap, &te)) do {
                if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
                HANDLE th = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                if (!th) continue;
                FILETIME c0, e0, k0, u0; ULONGLONG t = 0;
                if (GetThreadTimes(th, &c0, &e0, &k0, &u0))
                    t = ((ULONGLONG)u0.dwHighDateTime << 32 | u0.dwLowDateTime) + ((ULONGLONG)k0.dwHighDateTime << 32 | k0.dwLowDateTime);
                CloseHandle(th);
                if (t > best) { best = t; bestTid = te.th32ThreadID; }
            } while (Thread32Next(snap, &te));
            CloseHandle(snap);
            if (bestTid) { h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, bestTid);
                           if (h) { g_gameTid = bestTid; LOGF("stack: using the busiest thread %lu", (unsigned long)bestTid); } }
        }
    }
    if (!h) { LOGF("stack: no thread to sample (error %lu)", GetLastError()); return 0; }
    static DWORD eips[SS_N], frames[SS_N][SS_D];
    static int depths[SS_N];
    char path[MAX_PATH]; PathIn(path, sizeof path, "coop_stack_out.txt");
    int got = 0;
again:
    got = 0;
    LOGF("stack: sampling %d times, thread %lu (round %ld)", SS_N, (unsigned long)g_gameTid, (long)g_stackRounds + 1);
    for (int i = 0; i < SS_N * 8 && got < SS_N && !g_quit; i++) {
        if (SuspendThread(h) == (DWORD)-1) break;
        CONTEXT c; memset(&c, 0, sizeof c);
        c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (GetThreadContext(h, &c)) {
            int inCode = (c.Eip >= 0x00401000u && c.Eip < 0x18C0200u);
            if (!inCode) { ResumeThread(h); Sleep(2); continue; }   /* only the game's own code is interesting */
            eips[got] = c.Eip;
            int d = 0;
            DWORD ebp = c.Ebp;
            for (; d < SS_D; d++) {
                DWORD pair[2];
                if (ebp < 0x00010000u || ebp > 0x7FFE0000u) break;
                if (!SC(pair, (void*)(ULONG_PTR)ebp, 8)) break;
                if (pair[1] < 0x00401000u || pair[1] >= 0x18C0200u) break;   /* only frames inside this image's code */
                frames[got][d] = pair[1];
                if (pair[0] <= ebp) break;
                ebp = pair[0];
            }
            if (d == 0) {                                 /* no frame pointer chain: scan the stack for code addresses instead */
                DWORD s[64];
                if (SC(s, (void*)(ULONG_PTR)c.Esp, sizeof s)) {
                    for (int k = 0; k < 64 && d < SS_D; k++)
                        if (s[k] >= 0x00401000u && s[k] < 0x18C0200u) frames[got][d++] = s[k];
                }
            }
            depths[got] = d;
            got++;
        }
        ResumeThread(h);
        Sleep(10);
    }
    FILE* f = fopen(path, g_stackRounds ? "a" : "w");   /* the first round starts the file, later rounds append */
    if (!f) { LOGF("stack: cannot write %s", path); return 0; }
    fprintf(f, "=== stack sample: %d samples ~10 ms apart, game thread %lu, round %ld, wall %s\n", got, (unsigned long)g_gameTid, (long)g_stackRounds + 1, "");
    /* EIP histogram */
    fprintf(f, "-- suspended EIP histogram\n");
    for (int i = 0; i < got; i++) {
        if (!eips[i]) continue;
        int n = 0, first = 1;
        for (int j = 0; j < got; j++) if (eips[j] == eips[i]) { n++; if (j < i) first = 0; }
        if (first && n > 1) fprintf(f, "  %08lX  %d\n", (unsigned long)eips[i], n);
    }
    /* return-address histograms per depth */
    for (int d = 0; d < SS_D; d++) {
        fprintf(f, "-- depth %d return addresses (count > 1)\n", d + 1);
        for (int i = 0; i < got; i++) {
            if (depths[i] <= d) continue;
            DWORD a = frames[i][d];
            if (!a) continue;
            int n = 0, first = 1;
            for (int j = 0; j < got; j++) {
                if (depths[j] > d && frames[j][d] == a) { n++; if (j < i) first = 0; }
            }
            if (first && n > 1) fprintf(f, "  %08lX  %d\n", (unsigned long)a, n);
        }
    }
    fprintf(f, "-- raw chains (first 30)\n");
    for (int i = 0; i < got && i < 30; i++) {
        fprintf(f, "  %3d EIP=%08lX", i + 1, (unsigned long)eips[i]);
        for (int d = 0; d < depths[i]; d++) fprintf(f, " -> %08lX", (unsigned long)frames[i][d]);
        fprintf(f, "\n");
    }
    fclose(f);
    LOGF("stack: written %s (%d samples, round %ld)", path, got, (long)g_stackRounds + 1);
    g_stackRounds++;
    if (!g_quit && g_stackRounds < 60 && StackTrigger()) { Sleep(10000); goto again; }   /* ~4 minutes of the fight, 20 rounds */
    CloseHandle(h);
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
                if (g_mode == 2 && len > 0 && len < (int)sizeof pkt - 32) { len += _snprintf(pkt + len, sizeof pkt - 1 - len, " T%I64u", MyTok()); pkt[len] = 0; }
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
                    if (g_mode == 2 && len > 0 && len < (int)sizeof pkt - 32) { len += _snprintf(pkt + len, sizeof pkt - 1 - len, " T%I64u", MyTok()); pkt[len] = 0; }
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
            double lr = g_lastPeerMs;
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
static void CurMapName(char* out, size_t cap) {          /* the name of the running mission ("" when none) */
    out[0] = 0; DWORD dm = FastDvar("mapname"); if (dm) CvPeek(dm, out, cap); out[cap - 1] = 0;
}
static void MissionFollow(int tick) {
    static LONG lastFollow = 0;
    if (g_deathLoadAt && (LONG)(GetTickCount() - g_deathLoadAt) >= 0) {   /* the delayed explicit load after the partner's death */
        char c[160]; strncpy(c, g_deathLoadCmd, sizeof c - 1); c[sizeof c - 1] = 0; g_deathLoadAt = 0;
        char nowMap[40]; CurMapName(nowMap, sizeof nowMap);
        if (g_deathLoadMap[0] && nowMap[0] && strcmp(nowMap, g_deathLoadMap)) {   /* the mission changed during the 3.5 s: a checkpoint of the old mission would load it AGAIN */
            LOGF("follow: the delayed checkpoint load is dropped: the mission changed from '%s' to '%s' meanwhile", g_deathLoadMap, nowMap);
        } else
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
                CurMapName(g_deathLoadMap, sizeof g_deathLoadMap);
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

/* Autostart (dev): coop_autostart.txt next to the exe holds a level name; the bridge queues the engine console
   command "map <name>" a few seconds after load, so a test level starts without touching the menu. This is the same
   route MissionFollow uses, and it works where "+map" on the command line does not: with the launch argument the level
   starts before the mod's builtins are registered and the co-op script stalls at its first publish. */
static DWORD WINAPI AUTOSTART(LPVOID p) {
    (void)p;
    char path[MAX_PATH]; PathIn(path, sizeof path, "coop_autostart.txt");
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return 0;
    char map[48] = ""; int n = 0;
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    int ch;
    while ((ch = fgetc(f)) != EOF && n < (int)sizeof map - 2) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_') map[n++] = (char)ch;
        else if (n) break;
    }
    fclose(f);
    map[n] = 0;
    if (!n) { LOGF("autostart: coop_autostart.txt is empty or has no level name"); return 0; }
    Sleep(4000);
    for (int attempt = 0; attempt < 15; attempt++) {
        char cmd[80];
        _snprintf(cmd, sizeof cmd, "map %s\n", map);
        if (CbufAdd(cmd)) { LOGF("autostart: console \"map %s\" queued", map); return 0; }
        Sleep(1000);
    }
    LOGF("autostart: could not queue \"map %s\" (the console buffer is not ready)", map);
    return 0;
}

static DWORD WINAPI CTRL(LPVOID p) {
    (void)p;
    ThreadsStart(AUTOSTART, "autostart", LOGF);
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
    double now = NowMs(); EnterCriticalSection(&L); double lr = g_lastPeerMs; LeaveCriticalSection(&L);
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
static int RemoveRaw(void) {   /* 1 = the raw-input thread is gone (or was never there); 0 = it is still running */
    if (!g_rawOn) return 1;
    DWORD old;
    if (VirtualProtect((void*)(ULONG_PTR)RAW_PTR_GET, 8, PAGE_READWRITE, &old)) {
        *(DWORD*)(ULONG_PTR)RAW_PTR_GET = g_rawOrigGet; *(DWORD*)(ULONG_PTR)RAW_PTR_SET = g_rawOrigSet;
        VirtualProtect((void*)(ULONG_PTR)RAW_PTR_GET, 8, old, &old);
    }
    g_rawOn = 0;
    if (g_rawThread) {
        PostThreadMessageA(g_rawThreadId, WM_QUIT, 0, 0);
        DWORD w = WaitForSingleObject(g_rawThread, 2000);
        if (w != WAIT_OBJECT_0) { LOGF("raw mouse: the thread did not stop, the DLL must stay loaded"); return 0; }   /* never free the DLL under a running thread */
        CloseHandle(g_rawThread); g_rawThread = NULL;
    }
    LOGF("raw mouse: off (pointers restored, %ld raw events seen)", (long)g_rawEvents);
    return 1;
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

static int SaveSendBuf(const char* name, const char* buf, long sz) {   /* header line + bytes; 1 = written, 2 = receiver already has a newer one, 0 = retry */
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;
    u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
    struct sockaddr_in to; memset(&to, 0, sizeof to);
    to.sin_family = AF_INET; to.sin_addr.s_addr = g_peerAddr; to.sin_port = htons(SAVE_PORT);
    connect(s, (struct sockaddr*)&to, sizeof to);
    fd_set w; FD_ZERO(&w); FD_SET(s, &w); struct timeval tv; tv.tv_sec = 5; tv.tv_usec = 0;
    int ok = select(0, NULL, &w, NULL, &tv) > 0;
    if (ok) {
        int err = 0, el = sizeof err;
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &el);
        if (err) ok = 0;
    }
    unsigned long crc = Crc32Buf((const unsigned char*)buf, (size_t)sz);
    if (ok) {
        nb = 0; ioctlsocket(s, FIONBIO, &nb);
        DWORD tmo = 15000; setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tmo, sizeof tmo);
        /* The CRC is optional for older receivers: sscanf("%79s %ld") still accepts the extra field.
           A new receiver answers "OK <crc>" / "OK IGN" / "ERR ..."; an older one just closes. */
        char hdr[160]; int hl = _snprintf(hdr, sizeof hdr - 1, "COOPSAVE %s %ld %08lX\n", name, sz, crc);
        ok = hl > 0 && send(s, hdr, hl, 0) == hl;
        long sent = 0;
        while (ok && sent < sz) { int n = send(s, buf + sent, (int)(sz - sent > 32768 ? 32768 : sz - sent), 0); if (n <= 0) ok = 0; else sent += n; }
        if (ok) {
            DWORD rtmo = 5000; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rtmo, sizeof rtmo);
            char ack[128]; int an = recv(s, ack, sizeof ack - 1, 0);
            if (an > 0) {
                ack[an] = 0;
                ok = SecSaveAck(ack, crc);                       /* 1 written / 2 ignored (newer here) / 0 retry */
            } else if (an == 0) {
                /* An older receiver has no ACK and closes after writing. The old protocol only
                   refused malformed files, so a clean close is treated as legacy success. */
                ok = 1;
            } else ok = 0;                                       /* timeout: retry */
        }
    }
    closesocket(s);
    return ok;
}

static void SaveReply(SOCKET c, int ok, unsigned long crc, const char* why) {   /* 0 = ERR, 1 = OK <crc>, 2 = OK IGN (a newer checkpoint exists here) */
    char r[128]; int n;
    if (ok == 2) n = _snprintf(r, sizeof r - 1, "OK IGN\n");
    else if (ok) n = _snprintf(r, sizeof r - 1, "OK %08lX\n", crc);
    else n = _snprintf(r, sizeof r - 1, "ERR %s\n", why && why[0] ? why : "refused");
    if (n > 0) send(c, r, n, 0);
}

/* F16: receiving a saved game is a trust decision. It stays enabled by default because the shared
   checkpoint is a core feature, but a player can set `coop_accept_saves off` (config or console)
   and then no received .svg is written into the save folder at all. */
static int AcceptSaves(void) {
    DWORD d = FastDvar("coop_accept_saves");
    if (!d) return 1;                                   /* an older config does not have the dvar */
    char v[16] = ""; CvPeek(d, v, sizeof v);
    for (char* e = v + strlen(v); e > v && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'); ) *--e = 0;
    return !(!_stricmp(v, "off") || !strcmp(v, "0"));
}

static void SaveReceive(SOCKET c) {                    /* one incoming checkpoint; ACK only after a complete write */
    DWORD tmo = 10000; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof tmo);
    char hdr[160]; int hl = 0;
    while (hl < (int)sizeof hdr - 1) { char ch; if (recv(c, &ch, 1, 0) != 1) return; if (ch == '\n') break; hdr[hl++] = ch; }
    hdr[hl] = 0;
    char nm[80]; long sz = 0; unsigned long hcrc = 0;
    int fields = sscanf(hdr, "COOPSAVE %79s %ld %lx", nm, &sz, &hcrc);
    if (fields < 2 || sz <= 0 || sz > SAVE_MAX) { LOGF("checkpoint refused: bad header"); SaveReply(c, 0, 0, "bad header"); return; }
    for (const char* q = nm; *q; q++) if (!((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9') || *q == '_' || *q == '-')) { SaveReply(c, 0, 0, "bad name"); return; }
    if (SecReservedName(nm)) { LOGF("checkpoint refused: %s is a Windows device name", nm); SaveReply(c, 0, 0, "device name"); return; }          /* audit F13 */
    if (!AcceptSaves()) { LOGF("checkpoint %s ignored: coop_accept_saves is off", nm); SaveReply(c, 2, 0, NULL); return; }
    char* buf = (char*)malloc((size_t)sz); if (!buf) { SaveReply(c, 0, 0, "out of memory"); return; }
    long got = 0;
    while (got < sz) { int n = recv(c, buf + got, (int)(sz - got > 65536 ? 65536 : sz - got), 0); if (n <= 0) break; got += n; }
    if (got != sz) { LOGF("checkpoint %s: receive incomplete (%ld of %ld bytes)", nm, got, sz); free(buf); SaveReply(c, 0, 0, "short body"); return; }
    if (!SecSvgOk((const unsigned char*)buf, sz)) {                      /* audit F3: only what looks like a saved game of this game is written to the save folder */
        LOGF("checkpoint %s refused: it does not look like a saved game of this game (%ld bytes)", nm, sz); free(buf); SaveReply(c, 0, 0, "not a saved game"); return;
    }
    unsigned long crc = Crc32Buf((const unsigned char*)buf, (size_t)sz);
    if (fields >= 3 && hcrc != 0 && hcrc != crc) {
        LOGF("checkpoint %s refused: crc mismatch (header %08lX, body %08lX)", nm, hcrc, crc); free(buf); SaveReply(c, 0, 0, "crc mismatch"); return;
    }
    char rmap[96]; int rn; CkptParse(nm, rmap, sizeof rmap, &rn);
    {   /* F16: the mission inside the file must be the mission in its name (moscow-1 -> moscow) */
        char hmap[40] = "";
        if (!SecSvgMission((const unsigned char*)buf, sz, hmap, sizeof hmap) || !SecMapMatches(hmap, rmap)) {
            LOGF("checkpoint %s refused: file name map '%s' does not match the saved mission '%s'", nm, rmap, hmap);
            free(buf); SaveReply(c, 0, 0, "map mismatch"); return;
        }
    }
    /* the guest takes EVERY checkpoint of the host: the "-N" counters of the two PCs are not comparable (the guest numbered its own saves 1..12 while the host was at 9 far ahead,
       so the host's tankhunt-9 was thrown away and a death reloaded the guest's own early save) */
    if (g_mode != 2 && g_curMap[0] && !strcmp(rmap, g_curMap) && rn < g_curN) {      /* older than what this PC already has for this map */
        LOGF("checkpoint %s ignored: older than %s-%d that this PC already has", nm, g_curMap, g_curN); free(buf); SaveReply(c, 2, 0, NULL); return;
    }
    char base[MAX_PATH]; if (!SaveBase(base, sizeof base)) { LOGF("checkpoint %s: no profile folder", nm); free(buf); SaveReply(c, 0, 0, "no profile"); return; }
    char tmp[MAX_PATH], shared[MAX_PATH];
    _snprintf(tmp, sizeof tmp, "%ssave\\autosave\\coopshared.tmp", base); tmp[sizeof tmp - 1] = 0;
    _snprintf(shared, sizeof shared, "%ssave\\autosave\\coopshared.svg", base); shared[sizeof shared - 1] = 0;
    FILE* f = fopen(tmp, "wb");
    if (!f) { LOGF("checkpoint %s: cannot write %s", nm, tmp); free(buf); SaveReply(c, 0, 0, "cannot write"); return; }
    size_t wr = fwrite(buf, 1, (size_t)sz, f); fclose(f); free(buf);
    if (wr != (size_t)sz) { LOGF("checkpoint %s: write failed", nm); DeleteFileA(tmp); SaveReply(c, 0, 0, "write failed"); return; }
    /* every place the engine may reload from: coopshared.svg (loaded by name), the file sv_lastSaveGame points to, the profile save of the map */
    char last[96] = ""; DWORD dl = FastDvar("sv_lastSaveGame"); if (dl) CvPeek(dl, last, sizeof last);
    char target[MAX_PATH] = ""; int okTarget = 0;
    if (!strncmp(last, "players", 7) && !strstr(last, "..") && strlen(last) > 4 && !_stricmp(last + strlen(last) - 4, ".svg")) {
        okTarget = 1;
        for (const char* q = last; *q; q++) if (!((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9') || *q == '_' || *q == '-' || *q == '.' || *q == '\\' || *q == '/')) okTarget = 0;
        if (okTarget) { _snprintf(target, sizeof target, "%smain\\%s", g_dir, last); target[sizeof target - 1] = 0; for (char* q = target; *q; q++) if (*q == '/') *q = '\\'; }
    }
    char perMap[MAX_PATH]; _snprintf(perMap, sizeof perMap, "%ssave\\%s.svg", base, rmap); perMap[sizeof perMap - 1] = 0;
    char prev[MAX_PATH]; _snprintf(prev, sizeof prev, "%ssave\\autosave\\coopshared.prev.svg", base); prev[sizeof prev - 1] = 0;
    int backed = (GetFileAttributesA(shared) == INVALID_FILE_ATTRIBUTES) ? -1 : (CopyFileA(shared, prev, TRUE) ? 1 : 0);
    int a = MoveFileExA(tmp, shared, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) ? 1 : 0;
    if (!a) { LOGF("checkpoint %s: cannot replace coopshared.svg (error %lu)", nm, GetLastError()); DeleteFileA(tmp); SaveReply(c, 0, 0, "cannot replace"); return; }
    /* The shared file exists now. Only at this point is it safe to advertise it as the file
       loadgame/autosave may use. Copies below are best-effort for the engine's own reload. */
    g_lastRecvCrc = crc;                                /* set BEFORE the copies: the scanner must see these files as "received", never as new */
    int b2 = okTarget ? (CopyFileA(shared, target, FALSE) ? 1 : 0) : -1;
    int c2 = CopyFileA(shared, perMap, FALSE) ? 1 : 0;
    strncpy(g_curMap, rmap, sizeof g_curMap - 1); g_curMap[sizeof g_curMap - 1] = 0; g_curN = rn;
    strcpy(g_curLoadName, "coopshared");
    LOGF("checkpoint received: %s (%ld bytes, crc32 %08lX) -> coopshared.svg ok (previous %s); last save '%s' %s; save\\%s.svg %s", nm, sz, crc,
         backed < 0 ? "absent" : (backed ? "kept in coopshared.prev.svg" : "NOT backed up"),
         last[0] ? last : "(unknown)", b2 < 0 ? "left alone" : (b2 ? "replaced" : "NOT replaced"), rmap, c2 ? "replaced" : "NOT replaced");
    SaveReply(c, 1, crc, NULL);
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
    char retryName[96] = ""; DWORD retryAt = 0; int retryCount = 0;   /* a failed send is retried; it is never marked as handled */
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
                int fromSelf = ra.sin_addr.s_addr == htonl(INADDR_LOOPBACK) && (TestMode() || LOOPBACK_OK);   /* audit F12 */
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
            if (!_stricmp(fd.cFileName, "coopshared.svg") || !_stricmp(fd.cFileName, "coopshared.prev.svg")) continue;   /* our files, never send back */
            if (CompareFileTime(&fd.ftLastWriteTime, &bestTime) > 0) { bestTime = fd.ftLastWriteTime; bestSize = fd.nFileSizeLow; strncpy(bestName, fd.cFileName, sizeof bestName - 1); bestName[sizeof bestName - 1] = 0; }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        if (!bestName[0]) continue;
        if (!strcmp(bestName, lastName) && CompareFileTime(&bestTime, &lastTime) == 0) continue;     /* already acknowledged */
        if (!strcmp(bestName, retryName) && (LONG)(GetTickCount() - retryAt) < 0) continue;           /* wait before the next attempt */
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
        FILE* f = fopen(path, "rb"); if (!f) continue;
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        char* buf = (sz > 0 && sz <= SAVE_MAX) ? (char*)malloc((size_t)sz) : NULL;
        size_t got = buf ? fread(buf, 1, (size_t)sz, f) : 0; fclose(f);
        if (!buf || got != (size_t)sz) { free(buf); continue; }
        unsigned long crc = Crc32Buf((const unsigned char*)buf, (size_t)sz);
        char smap[96]; int sn; CkptParse(nm, smap, sizeof smap, &sn);
        if (crc == g_lastRecvCrc) { strcpy(lastName, bestName); lastTime = bestTime; free(buf); continue; }   /* this is the file we just received (echo) */
        if (g_curMap[0] && !strcmp(smap, g_curMap) && sn < g_curN) { LOGF("checkpoint %s not sent: older than %s-%d that this PC already has", nm, g_curMap, g_curN); strcpy(lastName, bestName); lastTime = bestTime; free(buf); continue; }
        int okS = SaveSendBuf(nm, buf, sz);
        free(buf);
        if (okS == 2) {
            /* The other PC already has a newer checkpoint for this map; do not regress our own
               load name or the replay list because of this older file. */
            LOGF("checkpoint %s ignored by the other PC (it already has a newer one)", nm);
            strcpy(lastName, bestName); lastTime = bestTime; retryName[0] = 0; retryCount = 0;
            continue;
        }
        if (!okS) {
            /* Do not mark it as sent. The next tick retries the same file; a newer autosave still wins. */
            if (strcmp(retryName, bestName)) { strcpy(retryName, bestName); retryCount = 0; }
            retryCount++;
            retryAt = GetTickCount() + (retryCount >= 3 ? 30000 : 5000);
            LOGF("checkpoint %s NOT sent (no ACK; other PC offline or firewall blocks TCP 27203), retry #%d in %d s", nm, retryCount, retryCount >= 3 ? 30 : 5);
            continue;
        }
        strcpy(lastName, bestName); lastTime = bestTime; retryName[0] = 0; retryCount = 0;
        LOGF("checkpoint sent: %s (%ld bytes, crc32 %08lX) acknowledged", nm, sz, crc);
        if (g_mode == 1) { InterlockedExchange(&g_hostFN, 0); InterlockedExchange(&g_replayIdx, -1); }   /* what was sent before is inside this checkpoint now */
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
            char hp[48]; int hl = _snprintf(hp, sizeof hp - 1, "H 1 T%I64u", MyTok()); if (hl < 0) hl = 0;
            if (sendto(g_udpSock, hp, hl, 0, (struct sockaddr*)&to, sizeof to) < 0) { static DWORD lastSE; if (GetTickCount() - lastSE > 5000) { lastSE = GetTickCount(); LOGF("send to the host FAILED, error %d", WSAGetLastError()); } }
        }
        BarrierTick();
        ProbeTick();
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
        char lp[MAX_PATH]; PathIn(lp, sizeof lp, "coop5.log"); strncpy(g_logPath, lp, sizeof g_logPath - 1); g_logPath[sizeof g_logPath - 1] = 0;
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
        {   char wp[MAX_PATH]; PathIn(wp, sizeof wp, "coop_world.csv");   /* a measurement, not a log: a fresh file per game start */
            WLOGF = fopen(wp, "w");
            if (WLOGF) fprintf(WLOGF, "wall,t_ms,role,map,side,key,idx,alive,hp,x,y,z,wid,ent\n"); }
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
            g_hasPeer = 0; g_peerAuto = 0; g_peerKnown = 0; g_peerTok = 0; g_logPort = 0; pf = 0;
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
        ThreadsStart(UDP, "udp", LOGF);
        ThreadsStart(WORK, "work", LOGF);
        ThreadsStart(STACKSAMP, "stacksamp", LOGF);
        ThreadsStart(CTRL, "ctrl", LOGF);
        if (!g_fileMode) ThreadsStart(WIN, "win", LOGF);
        ThreadsStart(SAVESYNC, "savesync", LOGF);
        if (!g_fileMode) {   // menu mode only: both are idle until Host / Join-auto is chosen
            ThreadsStart(BEACON, "beacon", LOGF);
            ThreadsStart(DISC, "discover", LOGF);
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
static int g_regFail = 0;
/* Every built-in goes through this: a failed registration used to be visible only in the "ok/FAILED" line, and a script
   calling an unregistered function then dies at COMPILE time - the level never starts and the game says only
   "script compile error". Counting the failures makes that one loud line. */
static int REG(const char* name, void* fn) {
    int ok = g_capi ? g_capi->RegisterFunction(name, fn) : 0;
    if (!ok) { g_regFail++; LOGF("built-in %s did NOT register (the engine table holds 64)", name); }
    return ok;
}

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
            CurMapName(g_deathLoadMap, sizeof g_deathLoadMap);
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
static void __cdecl H_coop_link_active(void) {          /* int coop_link_active(): 1 while a partner is linked (host: packets or its hello within seconds; guest: packets, or before the first mission) */
    double now = NowMs(); EnterCriticalSection(&L); double lr = g_lastPeerMs; LeaveCriticalSection(&L);
    int fresh = lr > 0 && (now - lr) < 5000.0;
    int hello = g_helloMs && (LONG)(GetTickCount() - (DWORD)g_helloMs) < 10000;
    int on = (g_mode == 1 && (fresh || hello)) ||
             (g_mode == 2 && g_peerKnown && (fresh || g_txn == 0));   /* g_txn == 0: guest is still in the menu, before its first publish */
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
static void __cdecl H_coop_wlog(void) {                 /* coop_wlog(row): one row of the world dump the script makes for the divergence measurement -> coop_world.csv */
    char m[300];
    if (!WLOGF) return;
    if (*(volatile int*)(ULONG_PTR)E_PARAMCOUNT < 1 || !E_GetStringCopy(0, m, sizeof m)) return;
    m[sizeof m - 1] = 0;
    for (char* q = m; *q; q++) if ((unsigned char)*q < 32) *q = ' ';
    char ts[16]; WallStr(ts, sizeof ts);
    const char* role = SoloMirror() ? "solo" : (g_mode == 2 ? "guest" : "host");
    EnterCriticalSection(&L);
    fprintf(WLOGF, "%s,%.0f,%s,%s\n", ts, NowMs(), role, m);
    fflush(WLOGF);
    LeaveCriticalSection(&L);
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
static void __cdecl H_coop_send_world(void) {           /* coop_send_world(worldId): the actor with this name died here (host: authoritative, guest: a claim) */

    if (VerMismatch()) { static LONG vs; if ((InterlockedIncrement(&vs) % 100) == 1) LOGF("sync paused: the partner runs another version (%ld sends dropped)", (long)vs); return; }
    int count = *(volatile int*)(ULONG_PTR)E_PARAMCOUNT;
    if (count < 1) return;
    char wid[44] = "";
    if (!E_GetStringCopy(0, wid, sizeof wid)) return;
    wid[sizeof wid - 1] = 0;
    if (!SecWorldId(wid)) { LOGF("world death NOT sent: '%s' is not a world id", wid); return; }
    g_wdSeen = 1;                                        /* arms the stack sampler: this is combat */
    if (SoloMirror()) { LOGF("world death (solo test, not sent): %s", wid); return; }
    LONG seq = InterlockedIncrement(&g_wSeq);
    char pkt[80];
    int len = _snprintf(pkt, sizeof pkt - 1, "WD %s %ld", wid, (long)seq);
    if (len > 0 && g_peerKnown && g_udpSock != INVALID_SOCKET) {
        struct sockaddr_in to; memset(&to, 0, sizeof to);
        to.sin_family = AF_INET; to.sin_addr.s_addr = g_peerAddr; to.sin_port = htons((u_short)g_peerPort);
        for (int i = 0; i < 3; i++) sendto(g_udpSock, pkt, len, 0, (struct sockaddr*)&to, sizeof to);   /* UDP may drop one; the receiver drops the copies by sequence number */
        LOGF("world death sent: %s (seq %ld, %s)", wid, (long)seq, g_mode == 2 ? "a kill claim for the host" : "authoritative (host)");
    } else LOGF("world death %s NOT sent (no peer yet)", wid);
}
static void __cdecl H_coop_world_pop(void) {            /* string coop_world_pop(): the oldest received world death, or "" when there is none */
    char wid[44] = "";
    LONG t = g_wInTail;
    if (g_wInHead - t > 0) { strcpy(wid, g_wIn[t & 15].wid); g_wInTail = t + 1; }
    E_AddString(wid);
}
/* Stage 1, health: the host owns the health of an axis actor. The guest reports the damage its own copy took ("HD"), the host applies it to
   its copy and sends the resulting health back ("WH"); the guest never raises health from a packet, so its own damage is not rolled back. */
static void __cdecl H_coop_send_world_state(void) {     /* coop_send_world_state(worldId, health): host only */

    if (VerMismatch()) { static LONG vs; if ((InterlockedIncrement(&vs) % 100) == 1) LOGF("sync paused: the partner runs another version (%ld sends dropped)", (long)vs); return; }
    int count = *(volatile int*)(ULONG_PTR)E_PARAMCOUNT;
    if (count < 2) return;
    char wid[44] = "";
    if (!E_GetStringCopy(0, wid, sizeof wid)) return;
    wid[sizeof wid - 1] = 0;
    int hp = E_GetInt(1);
    if (!SecWorldId(wid) || hp < 0 || hp > 1000) { LOGF("world health NOT sent: bad id '%s' or value %d", wid, hp); return; }
    if (SoloMirror() || g_mode != 1) return;             /* only the host is authoritative */
    LONG seq = InterlockedIncrement(&g_hpSeq);
    char pkt[80];
    int len = _snprintf(pkt, sizeof pkt - 1, "WH %s %d %ld", wid, hp, (long)seq);
    if (len > 0 && g_peerKnown && g_udpSock != INVALID_SOCKET) {
        struct sockaddr_in to; memset(&to, 0, sizeof to);
        to.sin_family = AF_INET; to.sin_addr.s_addr = g_peerAddr; to.sin_port = htons((u_short)g_peerPort);
        sendto(g_udpSock, pkt, len, 0, (struct sockaddr*)&to, sizeof to);
        sendto(g_udpSock, pkt, len, 0, (struct sockaddr*)&to, sizeof to);
        static LONG n; if ((InterlockedIncrement(&n) % 25) == 1) LOGF("world health sent: %s %d (packet %ld)", wid, hp, (long)n);
    }
}
static void __cdecl H_coop_send_world_hit(void) {       /* coop_send_world_hit(worldId, damage): guest only, a damage claim for the host */

    if (VerMismatch()) { static LONG vs; if ((InterlockedIncrement(&vs) % 100) == 1) LOGF("sync paused: the partner runs another version (%ld sends dropped)", (long)vs); return; }
    int count = *(volatile int*)(ULONG_PTR)E_PARAMCOUNT;
    if (count < 2) return;
    char wid[44] = "";
    if (!E_GetStringCopy(0, wid, sizeof wid)) return;
    wid[sizeof wid - 1] = 0;
    int dmg = E_GetInt(1);
    if (!SecWorldId(wid) || dmg <= 0 || dmg > 1000) return;
    if (SoloMirror() || g_mode != 2) return;             /* host: its own copy already took the damage */
    LONG seq = InterlockedIncrement(&g_hitSeq);
    char pkt[80];
    int len = _snprintf(pkt, sizeof pkt - 1, "HD %s %d %ld", wid, dmg, (long)seq);
    if (len > 0 && g_peerKnown && g_udpSock != INVALID_SOCKET) {
        struct sockaddr_in to; memset(&to, 0, sizeof to);
        to.sin_family = AF_INET; to.sin_addr.s_addr = g_peerAddr; to.sin_port = htons((u_short)g_peerPort);
        for (int i = 0; i < 3; i++) sendto(g_udpSock, pkt, len, 0, (struct sockaddr*)&to, sizeof to);   /* a lost claim is lost damage */
        static LONG n; if ((InterlockedIncrement(&n) % 25) == 1) LOGF("world damage claim sent: %s %d (claim %ld)", wid, dmg, (long)n);
    } else LOGF("world damage claim %s NOT sent (no peer yet)", wid);
}
static void __cdecl H_coop_world_state_pop(void) {      /* string coop_world_state_pop(): the oldest received health of an actor, or "" */
    char wid[44] = "";
    LONG t = g_hpTail;
    g_lastHpVal = 0;
    if (g_hpHead - t > 0) { strcpy(wid, g_hpIn[t & 15].wid); g_lastHpVal = g_hpIn[t & 15].val; g_hpTail = t + 1; }
    E_AddString(wid);
}
static void __cdecl H_coop_world_state_hp(void) { E_AddInt(g_lastHpVal); }    /* int coop_world_state_hp(): the health of the entry popped last */
static void __cdecl H_coop_world_hit_pop(void) {        /* string coop_world_hit_pop(): "<world id> <damage>" of the oldest claim, or "" - one builtin instead of two (the table holds 64) */
    char out[64] = "";
    LONG t = g_hitTail;
    g_lastHitVal = 0;
    if (g_hitHead - t > 0) {
        g_lastHitVal = g_hitIn[t & 15].val;
        _snprintf(out, sizeof out - 1, "%s %d", g_hitIn[t & 15].wid, (int)g_lastHitVal);
        g_hitTail = t + 1;
    }
    E_AddString(out);
}
static void __cdecl H_coop_send_player_hit(void) {       /* coop_send_player_hit(worldId, damage): host -> guest, an enemy hit the guest player */
    int n = *(volatile int*)(ULONG_PTR)E_PARAMCOUNT;
    if (n < 2) return;
    char wid[44] = "";
    if (!E_GetStringCopy(0, wid, sizeof wid)) return;
    int dmg = E_GetInt(1);
    if (!SecWorldId(wid) || dmg <= 0 || dmg > 300) return;
    if (!(SoloMirror() && SoloLoop()) && (g_mode != 1 || SoloMirror())) return;   /* the solo test may send to itself */
    if (!g_peerKnown || g_udpSock == INVALID_SOCKET) return;
    char pkt[80]; int len = _snprintf(pkt, sizeof pkt - 1, "PH %s %d", wid, dmg);
    if (len > 0) {
        struct sockaddr_in to; memset(&to, 0, sizeof to);
        to.sin_family = AF_INET; to.sin_addr.s_addr = g_peerAddr; to.sin_port = htons((u_short)g_peerPort);
        sendto(g_udpSock, pkt, len, 0, (struct sockaddr*)&to, sizeof to);
        static LONG nph; if ((InterlockedIncrement(&nph) % 100) == 1) LOGF("player hits sent: %s dmg=%d (packet %ld)", wid, dmg, (long)nph);
    }
}
static void __cdecl H_coop_player_hit_pop(void) {         /* string coop_player_hit_pop(): "<world id> <damage>" of the oldest event, or "" */
    char out[64] = "";
    LONG t = g_phTail;
    g_lastPhDmg = 0;
    if (g_phHead - t > 0) {
        g_lastPhDmg = g_phIn[t & 15].val;
        _snprintf(out, sizeof out - 1, "%s %d", g_phIn[t & 15].wid, (int)g_lastPhDmg);
        g_phTail = t + 1;
    }
    E_AddString(out);
}
static void __cdecl H_coop_world_ask(void) {            /* coop_world_ask(): guest -> host, "send me a snapshot of the world" */
    if (SoloMirror() || g_mode != 2) return;
    if (!(g_peerKnown && g_udpSock != INVALID_SOCKET)) { LOGF("world snapshot request NOT sent (no peer yet)"); return; }
    struct sockaddr_in to; memset(&to, 0, sizeof to);
    to.sin_family = AF_INET; to.sin_addr.s_addr = g_peerAddr; to.sin_port = htons((u_short)g_peerPort);
    for (int i = 0; i < 3; i++) sendto(g_udpSock, "WR", 2, 0, (struct sockaddr*)&to, sizeof to);
    LOGF("world snapshot requested from the host");
}
static void __cdecl H_coop_world_resync(void) { E_AddInt((int)InterlockedExchange(&g_wSnapDue, 0)); }   /* int coop_world_resync(): 1 once when the guest asked for a snapshot */
static void __cdecl H_coop_send_world_pos(void) {       /* coop_send_world_pos(worldId, origin, yaw): host only, where that actor stands */

    if (VerMismatch()) { static LONG vs; if ((InterlockedIncrement(&vs) % 100) == 1) LOGF("sync paused: the partner runs another version (%ld sends dropped)", (long)vs); return; }
    int count = *(volatile int*)(ULONG_PTR)E_PARAMCOUNT;
    if (count < 3) return;
    char wid[44] = "";
    if (!E_GetStringCopy(0, wid, sizeof wid)) return;
    wid[sizeof wid - 1] = 0;
    float o[3];
    E_GetVector(1, o);
    int yaw = E_GetInt(2);
    if (!SecWorldId(wid)) return;
    if (!(SoloMirror() && SoloLoop()) && (SoloMirror() || g_mode != 1)) return;   /* the solo test may send to itself to exercise the host half */
    if (!(fabsf(o[0]) < 100000.0f && fabsf(o[1]) < 100000.0f && fabsf(o[2]) < 100000.0f)) return;
    int x = (int)o[0], y = (int)o[1], z = (int)o[2];
    yaw %= 360; if (yaw < 0) yaw += 360;
    char pkt[96];
    int len = _snprintf(pkt, sizeof pkt - 1, "WP %s %d %d %d %d", wid, x, y, z, yaw);
    if (len > 0 && g_peerKnown && g_udpSock != INVALID_SOCKET) {
        struct sockaddr_in to; memset(&to, 0, sizeof to);
        to.sin_family = AF_INET; to.sin_addr.s_addr = g_peerAddr; to.sin_port = htons((u_short)g_peerPort);
        sendto(g_udpSock, pkt, len, 0, (struct sockaddr*)&to, sizeof to);
        sendto(g_udpSock, pkt, len, 0, (struct sockaddr*)&to, sizeof to);
        static LONG n; if ((InterlockedIncrement(&n) % 200) == 1) LOGF("world positions sent: %s %d %d %d (packet %ld)", wid, x, y, z, (long)n);
    }
}
static void __cdecl H_coop_world_pos_pop(void) {        /* string coop_world_pos_pop(): the oldest received position event, or "" */
    char wid[44] = "";
    LONG t = g_posTail;
    g_lastPos[0] = g_lastPos[1] = g_lastPos[2] = g_lastPos[3] = 0;
    if (g_posHead - t > 0) {
        WorldPosEv* e = &g_posIn[t & 31];
        strcpy(wid, e->wid);
        g_lastPos[0] = e->p[0]; g_lastPos[1] = e->p[1]; g_lastPos[2] = e->p[2]; g_lastPos[3] = e->yaw;
        g_posTail = t + 1;
    }
    E_AddString(wid);
}
static void __cdecl H_coop_world_pos_org(void) {        /* vector coop_world_pos_org(): the position of the event popped last */
    float v[3] = { (float)g_lastPos[0], (float)g_lastPos[1], (float)g_lastPos[2] };
    E_AddVector(v);
}
static void __cdecl H_coop_world_pos_yaw(void) { E_AddInt(g_lastPos[3]); }   /* int coop_world_pos_yaw(): the yaw of the event popped last */
static void __cdecl H_coop_probe_slot(void) {           /* coop_probe_slot(slot, tag): read-only probe of one actor-pool slot (the AI state) */
    int count = *(volatile int*)(ULONG_PTR)E_PARAMCOUNT;
    if (count < 1) return;
    int slot = E_GetInt(0);
    char tag[24] = "";
    if (count >= 2 && E_GetStringCopy(1, tag, sizeof tag)) {
        for (char* q = tag; *q; q++) if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '_' || *q == '-')) { *q = 0; break; }
    } else tag[0] = 0;
    E_AddInt(ProbeSlotBegin(slot, tag[0] ? tag : "ai"));
}static void __cdecl H_coop_probe_actor(void) {          /* coop_probe_actor(entityNumber, tag): read-only memory probe of one actor (engine work) */
    int count = *(volatile int*)(ULONG_PTR)E_PARAMCOUNT;
    if (count < 2) return;
    int en = E_GetInt(0);
    char tag[24] = "";
    if (!E_GetStringCopy(1, tag, sizeof tag)) tag[0] = 0;
    for (char* q = tag; *q; q++) if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '_' || *q == '-')) { *q = 0; break; }
    int ok = ProbeBegin(en, tag[0] ? tag : "actor");
    E_AddInt(ok);
}
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
static volatile int g_forceGuest = -1;                  /* -1 = not read yet; 1 when coop_force_guest.txt exists (dev: exercise the guest paths in the solo test) */
static volatile int g_soloLoop = -1;                    /* -1 = not read yet; 1 when coop_solo_loop.txt exists (dev: send packets to this same PC) */
static int SoloLoop(void) {
    int v = g_soloLoop;
    if (v >= 0) return v;
    char p[MAX_PATH]; PathIn(p, sizeof p, "coop_solo_loop.txt");
    v = (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) ? 1 : 0;
    g_soloLoop = v;
    if (v) LOGF("test: coop_solo_loop.txt is present, packets are sent to this same PC (the host half is exercised)");
    return v;
}
static int ForceGuest(void) {
    int v = g_forceGuest;
    if (v >= 0) return v;
    char p[MAX_PATH]; PathIn(p, sizeof p, "coop_force_guest.txt");
    v = (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) ? 1 : 0;
    g_forceGuest = v;
    if (v) LOGF("test: coop_force_guest.txt is present, the script is told it is the guest even in the solo test");
    return v;
}
static void __cdecl H_coop_is_guest(void) { E_AddInt(g_mode == 2 && (!SoloMirror() || ForceGuest()) ? 1 : 0); }   /* int coop_is_guest(): 1 on the guest PC (not in the solo test, unless the test file forces it) */

/* Stage 5: the per-actor think gate. FUN_004DA090 runs an actor's think only when 0 < entity+0x138 <= engine time, so an
   actor that is driven by the host can be switched off by keeping that field out of reach. Data only: no code is patched.
   A small thread re-applies the value, because a respawn or a mode change may reset the field. */
#define FRZ_MAX 64
static volatile int g_frzEnt[FRZ_MAX];
static DWORD g_frzSlot[FRZ_MAX];              /* the sentient/slot pointer the actor had when it was frozen: the identity check below */
static volatile LONG g_frzN = 0;
static volatile LONG g_frzThreadUp = 0;
static DWORD SlotOf(int ent);                 /* defined with the animation channel further down */

static DWORD WINAPI FRZTHR(LPVOID p) {
    (void)p;
    while (!g_quit) {   /* a stop request (live reload) must end this thread, or it runs in freed code */
        LONG n = g_frzN;
        for (LONG k = 0; k < n && k < FRZ_MAX; k++) {
            int ent = g_frzEnt[k];
            /* Identity: if the entity now carries a different sentient, the engine freed this entity and gave it to another
               actor - that one must not be frozen by accident. Drop the entry instead of writing into a stranger. */
            DWORD slot = SlotOf(ent);
            if (!slot) {   /* the entity is gone or its sentient cannot be read: drop the entry instead of writing into whatever is there now */
                DWORD was0 = g_frzSlot[k];
                LONG last0 = g_frzN - 1;
                if (last0 > k) { g_frzEnt[k] = g_frzEnt[last0]; g_frzSlot[k] = g_frzSlot[last0]; }
                InterlockedDecrement(&g_frzN);
                LOGF("stage5: freeze released for entity %d (its sentient is no longer readable, slot was 0x%08lX)", ent, (unsigned long)was0);
                k--;
                continue;
            }
            if (g_frzSlot[k] && slot != g_frzSlot[k]) {
                DWORD was = g_frzSlot[k];
                LONG last = g_frzN - 1;
                if (last > k) { g_frzEnt[k] = g_frzEnt[last]; g_frzSlot[k] = g_frzSlot[last]; }
                InterlockedDecrement(&g_frzN);
                LOGF("stage5: freeze released for entity %d (it now belongs to another actor: slot 0x%08lX -> 0x%08lX)",
                     ent, (unsigned long)was, (unsigned long)slot);
                k--;
                continue;
            }
            DWORD addr = 0x1B15F00u + (DWORD)ent * 0x1FCu + 0x138u;
            int v = 0;
            if (!SC(&v, (void*)(ULONG_PTR)addr, 4)) continue;
            if (v > 0 && v < 0x7FF00000) { int big = 0x7FFFFFF0; SW((void*)(ULONG_PTR)addr, &big, 4); }
        }
        Sleep(100);
    }
}

/* Stage 5, animation channel: the animation state of an actor lives in its pool slot at +0xD4 and +0x170..0x188
   (measured 2026-11-25, see docs/RE/08_AI_ACCESS.md). The script binds every tracked actor's world id to its entity number,
   the host reads those words and sends them, the guest writes them into its own copy - the same scheme as the position
   channel, and no table from index to animation name is needed because the raw state is copied. */
#define BIND_MAX 64
static char g_bindWid[BIND_MAX][44];
static int g_bindEnt[BIND_MAX];
static volatile LONG g_bindN = 0;
static DWORD g_bindLast[BIND_MAX][8];
static DWORD g_bindTick[BIND_MAX];              /* when this actor last had an animation packet (throttle) */
static volatile LONG g_animSent = 0, g_animThrottled = 0;
static int g_bindSent[BIND_MAX];

static int BindIndex(const char* wid) {
    LONG n = g_bindN;
    for (LONG i = 0; i < n && i < BIND_MAX; i++) if (!strcmp(g_bindWid[i], wid)) return (int)i;
    return -1;
}
static DWORD SlotOf(int ent) {
    DWORD slot = 0;
    if (ent < 0 || ent >= 0x800) return 0;   /* the engine caps entities at 0x7FE: a larger number would address other image data */
    if (!SC(&slot, (void*)(ULONG_PTR)(0x1B15F00u + (DWORD)ent * 0x1FCu + 0x100u), 4)) return 0;
    if (slot <= 0x400000u || slot >= 0x20000000u) return 0;
    return slot;
}
static const DWORD g_animOff[8] = { 0x0D4, 0x170, 0x174, 0x178, 0x17C, 0x180, 0x184, 0x188 };
static int AnimWordOk(int i, DWORD v) {
    if (i == 0) return v <= 0x10000u;
    if ((v & 0x7F800000u) == 0x7F800000u) return 0;              /* NaN or inf */
    float f; memcpy(&f, &v, 4);
    return fabsf(f) <= 100000.0f;
}
static int AnimRead(int ent, DWORD w[8]) {
    DWORD slot = SlotOf(ent);
    if (!slot) return 0;
    for (int i = 0; i < 8; i++) if (!SC(&w[i], (void*)(ULONG_PTR)(slot + g_animOff[i]), 4)) return 0;
    return 1;
}
static int AnimWrite(int ent, const DWORD w[8]) {
    DWORD slot = SlotOf(ent);
    if (!slot) return 0;
    int wrote = 0;
    for (int i = 0; i < 8; i++) {
        if (!AnimWordOk(i, w[i])) continue;
        if (SW((void*)(ULONG_PTR)(slot + g_animOff[i]), &w[i], 4)) wrote++;
    }
    return wrote;
}
static void __cdecl H_coop_bind_wid(void) {              /* coop_bind_wid(worldId, entityNumber): remember which entity this actor is here */
    int n = *(volatile int*)(ULONG_PTR)E_PARAMCOUNT;
    if (n < 2) return;
    char wid[44] = "";
    if (!E_GetStringCopy(0, wid, sizeof wid)) return;
    int ent = E_GetInt(1);
    if (!SecWorldId(wid) || ent < 0 || ent >= 0x800) return;
    int idx = BindIndex(wid);
    if (idx >= 0) { g_bindEnt[idx] = ent; g_bindSent[idx] = 0; return; }
    LONG i = g_bindN;
    if (i >= BIND_MAX) {
        static int warned;
        if (!warned) { warned = 1; LOGF("stage5: the world id -> entity table is full (%d entries), later actors get no animation", BIND_MAX); }
        return;
    }
    strcpy(g_bindWid[i], wid);
    g_bindEnt[i] = ent;
    g_bindSent[i] = 0;
    InterlockedIncrement(&g_bindN);
}
static void __cdecl H_coop_anim_send(void) {             /* coop_anim_send(worldId): host side, send this actor's animation words */

    if (VerMismatch()) { static LONG vs; if ((InterlockedIncrement(&vs) % 100) == 1) LOGF("sync paused: the partner runs another version (%ld sends dropped)", (long)vs); return; }
    int n = *(volatile int*)(ULONG_PTR)E_PARAMCOUNT;
    if (n < 1) return;
    char wid[44] = "";
    if (!E_GetStringCopy(0, wid, sizeof wid)) return;
    int idx = BindIndex(wid);
    if (idx < 0) return;
    if (!(SoloMirror() && SoloLoop()) && (g_mode != 1 || SoloMirror())) return;   /* the solo test may send to itself */
    if (!g_peerKnown || g_udpSock == INVALID_SOCKET) return;
    DWORD w[8];
    if (!AnimRead(g_bindEnt[idx], w)) return;
    if (g_bindSent[idx] && !memcmp(w, g_bindLast[idx], sizeof w)) return;   /* unchanged: nothing to send */
    DWORD nowMs = GetTickCount();
    if (g_bindTick[idx] && (nowMs - g_bindTick[idx]) < 120) { InterlockedIncrement(&g_animThrottled); return; }   /* at most ~8 packets/s per actor */
    g_bindTick[idx] = nowMs;
    memcpy(g_bindLast[idx], w, sizeof w);
    g_bindSent[idx] = 1;
    char pkt[160];
    int len = _snprintf(pkt, sizeof pkt - 1, "AP %s %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX", wid,
                        (unsigned long)w[0], (unsigned long)w[1], (unsigned long)w[2], (unsigned long)w[3],
                        (unsigned long)w[4], (unsigned long)w[5], (unsigned long)w[6], (unsigned long)w[7]);
    if (len > 0) {
        struct sockaddr_in to; memset(&to, 0, sizeof to);
        to.sin_family = AF_INET; to.sin_addr.s_addr = g_peerAddr; to.sin_port = htons((u_short)g_peerPort);
        sendto(g_udpSock, pkt, len, 0, (struct sockaddr*)&to, sizeof to);
        if ((InterlockedIncrement(&g_animSent) % 500) == 1)
            LOGF("animation: %ld packets sent, %ld throttled by the 120 ms per-actor limit", (long)g_animSent, (long)g_animThrottled);
    }
}
static void AnimApplyPacket(const char* b) {             /* "AP <wid> <8 hex words>": guest side */
    char wid[44]; unsigned long v[8];
    if (sscanf(b + 3, "%43s %lx %lx %lx %lx %lx %lx %lx %lx", wid, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) != 9) return;
    if (!SecWorldId(wid)) return;
    int idx = BindIndex(wid);
    if (idx < 0) { static LONG miss; if ((InterlockedIncrement(&miss) % 50) == 1) LOGF("animation packet for an unbound actor: %s", wid); return; }
    DWORD w[8];
    for (int i = 0; i < 8; i++) { w[i] = (DWORD)v[i]; if (!AnimWordOk(i, w[i])) { LOGF("animation packet refused (word %d) for %s", i, wid); return; } }
    AnimWrite(g_bindEnt[idx], w);
    static LONG n; if ((InterlockedIncrement(&n) % 200) == 1) LOGF("animation applied: %s (packet %ld)", wid, (long)n);
}

static void __cdecl H_coop_freeze(void) {               /* coop_freeze(entityNumber [, 0]): keep this actor's think off; a second argument 0 releases it again */
    int n = *(volatile int*)(ULONG_PTR)E_PARAMCOUNT;
    if (n < 1) return;
    int ent = E_GetInt(0);
    if (ent < 0 || ent >= 0x800) return;
    int release = (n >= 2 && E_GetInt(1) == 0);
    if (release) {
        LONG m = g_frzN;
        for (LONG k = 0; k < m && k < FRZ_MAX; k++) if (g_frzEnt[k] == ent) {
            LONG last = m - 1;
            if (last > k) { g_frzEnt[k] = g_frzEnt[last]; g_frzSlot[k] = g_frzSlot[last]; }
            InterlockedDecrement(&g_frzN);
            LOGF("stage5: think released for entity %d (its own AI runs again)", ent);
            break;
        }
        return;
    }
    LONG i = g_frzN;
    for (LONG k = 0; k < i && k < FRZ_MAX; k++) if (g_frzEnt[k] == ent) return;
    if (i < FRZ_MAX) {
        g_frzEnt[i] = ent;
        g_frzSlot[i] = SlotOf(ent);
        InterlockedIncrement(&g_frzN);
        if (!g_frzThreadUp) { g_frzThreadUp = 1; ThreadsStart(FRZTHR, "freeze", LOGF); }
    } else if (i == FRZ_MAX) LOGF("stage5: freeze list is full, entity %d not frozen", ent);
}
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
    g_regFail = 0;   /* counted by REG below: the engine table holds 64 entries and silently refuses the rest */
    g_gameTid = GetCurrentThreadId();
    if (!api || api->version != 1) { LOGF("script built-ins: unknown CoopApi version, none registered"); return; }
    /* the engine's script-function table is fixed (66 entries failed, 64 fit): coop_version is not called by the script */
    int b = REG("coop_ping", H_coop_ping);
    REG("coop_modver", H_coop_modver);
    int c = REG("coop_publish_state", H_coop_publish);
    int d = REG("coop_peer_state", H_coop_peer_state);
    int e = REG("coop_peer_yaw", H_coop_peer_yaw);
    int f = REG("coop_peer_seq", H_coop_peer_seq);
    int g = REG("coop_peer_stance", H_coop_peer_stance);
    int h = REG("coop_peer_event", H_coop_peer_event);
    int k = REG("coop_peer_solo", H_coop_peer_solo);
    int w = REG("coop_peer_weapon", H_coop_peer_weapon);
    int ks = REG("coop_send_kill", H_coop_send_kill);
    REG("coop_pending_level", H_coop_pending_level);
    REG("coop_player_link", H_coop_player_link);
    REG("coop_player_parent", H_coop_player_parent);
    REG("coop_peer_parent", H_coop_peer_parent);
    REG("coop_peer_health", H_coop_peer_health);
    REG("coop_pop_teleport", H_coop_pop_teleport);
    REG("coop_pop_peer_death", H_coop_pop_peer_death);
    REG("coop_death_started", H_coop_death_started);
    REG("coop_is_guest", H_coop_is_guest);
    REG("coop_freeze", H_coop_freeze);
    REG("coop_bind_wid", H_coop_bind_wid);
    REG("coop_anim_send", H_coop_anim_send);
    REG("coop_send_flag", H_coop_send_flag);
    REG("coop_send_grenade", H_coop_send_grenade);
    REG("coop_send_tank", H_coop_send_tank);
    REG("coop_barrier_begin", H_coop_barrier_begin);
    REG("coop_apply_host_skill", H_coop_apply_host_skill);
    /* coop_barrier_active is not called by the script either: both slots are needed by the A3 built-ins */
    REG("coop_link_active", H_coop_link_active);
    REG("coop_tank_pos", H_coop_tank_pos);
    REG("coop_tank_ang", H_coop_tank_ang);
    REG("coop_tank_turret", H_coop_tank_turret);
    REG("coop_tank_fire", H_coop_tank_fire);
    REG("coop_tank_age", H_coop_tank_age);
    REG("coop_pop_grenade", H_coop_pop_grenade);
    REG("coop_grenade_org", H_coop_grenade_org);
    REG("coop_grenade_vel", H_coop_grenade_vel);
    REG("coop_pop_flag", H_coop_pop_flag);
    REG("coop_flag_applied", H_coop_flag_applied);
    REG("coop_log", H_coop_log);
    REG("coop_wlog", H_coop_wlog);
    int kc = REG("coop_kill_count", H_coop_kill_count);
    int kp = REG("coop_pop_kill", H_coop_pop_kill);
    REG("coop_send_kill_key", H_coop_send_kill_key);
    int wk = REG("coop_kill_key", H_coop_kill_key);
    int ws = REG("coop_send_world", H_coop_send_world);
    int wp = REG("coop_world_pop", H_coop_world_pop);
    int wh = REG("coop_send_world_state", H_coop_send_world_state);
    int hd = REG("coop_send_world_hit", H_coop_send_world_hit);
    int hp1 = REG("coop_world_state_pop", H_coop_world_state_pop);
    int hp2 = REG("coop_world_state_hp", H_coop_world_state_hp);
    int ht1 = REG("coop_world_hit_pop", H_coop_world_hit_pop);
    REG("coop_send_player_hit", H_coop_send_player_hit);
    REG("coop_player_hit_pop", H_coop_player_hit_pop);
    /* coop_player_hit_dmg is gone for the same reason */
    int ht2 = 1;   /* coop_world_hit_dmg is gone: the pop returns the damage itself */
    int wa = REG("coop_world_ask", H_coop_world_ask);
    int wr = REG("coop_world_resync", H_coop_world_resync);
    int wp1 = REG("coop_send_world_pos", H_coop_send_world_pos);
    int wp2 = REG("coop_world_pos_pop", H_coop_world_pos_pop);
    int wp3 = REG("coop_world_pos_org", H_coop_world_pos_org);
    int wp4 = REG("coop_world_pos_yaw", H_coop_world_pos_yaw);
    int pr = REG("coop_probe_actor", H_coop_probe_actor);
    int ps = REG("coop_probe_slot", H_coop_probe_slot);
    int a = 1;
    LOGF("script built-ins registered (coop_version %s, coop_ping %s, publish %s, peer_state %s, peer_yaw %s, peer_seq %s, peer_stance %s, peer_event %s, peer_solo %s, peer_weapon %s, kill built-ins %s, world built-ins %s, world health built-ins %s, world resync built-ins %s, world position built-ins %s, probe %s, slot probe %s); engine lookup hooks %s",
         a ? "ok" : "FAILED", b ? "ok" : "FAILED", c ? "ok" : "FAILED", d ? "ok" : "FAILED", e ? "ok" : "FAILED", f ? "ok" : "FAILED", g ? "ok" : "FAILED", h ? "ok" : "FAILED", k ? "ok" : "FAILED", w ? "ok" : "FAILED", (ks && kc && kp && wk) ? "ok" : "FAILED", (ws && wp) ? "ok" : "FAILED", (wh && hd && hp1 && hp2 && ht1 && ht2) ? "ok" : "FAILED", (wa && wr) ? "ok" : "FAILED", (wp1 && wp2 && wp3 && wp4) ? "ok" : "FAILED", pr ? "ok" : "FAILED", ps ? "ok" : "FAILED", api->hooksInstalled ? "installed" : "NOT installed: scripts cannot see them");
    if (g_regFail) {
        LOGF("bridge is PARTLY inactive: %d built-in(s) did not register - a script that calls one of them will not compile and the level will not start", g_regFail);
        /* a marker file next to the exe: the game only says "script compile error", and the reason lives in this log */
        char mp[MAX_PATH]; PathIn(mp, sizeof mp, "coop_bridge_incomplete.txt");
        FILE* mf = fopen(mp, "w");
        if (mf) { fprintf(mf, "%s: %d built-in(s) did not register\n", BRIDGE_VERSION, g_regFail); fclose(mf); }
    } else {
        char mp[MAX_PATH]; PathIn(mp, sizeof mp, "coop_bridge_incomplete.txt"); DeleteFileA(mp);   /* a stale marker would lie after a good start */
    }
}

// Called by the d3d9 proxy before it frees this copy and loads a newer file (live reload). Returns 1 when every thread has
// left and everything is released (the DLL may be freed), 0 when a thread did not stop in time (do NOT free the DLL).
__declspec(dllexport) int __cdecl CoopStop(void) {
    if (!F) return 1;                                  // never started (not the single-player exe): nothing to stop
    LOGF("bridge %s: stop requested (live reload)", BRIDGE_VERSION);
    if (!RemoveRaw()) { LOGF("bridge stop: the raw-input thread is still running, this copy stays loaded"); return 0; }
    InterlockedExchange(&g_quit, 1);
    if (g_capi) g_capi->UnregisterAll();               // the proxy's stubs stay, but stop pointing into this DLL
    if (g_udpSock != INVALID_SOCKET) { SOCKET s = g_udpSock; g_udpSock = INVALID_SOCKET; closesocket(s); }   // wakes recvfrom
    if (!ThreadsWaitAll(8000)) {
        LOGF("bridge stop: a thread is still running after 8 s (%d registered), this copy stays loaded (restart the game to load a newer bridge)", ThreadsCount());
        return 0;
    }
    OpenLogSink(NULL, 0);
    LOGF("bridge %s: stopped", BRIDGE_VERSION);
    if (CSVF) { fclose(CSVF); CSVF = NULL; }
    if (WLOGF) { fclose(WLOGF); WLOGF = NULL; }
    if (g_mutex) { ReleaseMutex(g_mutex); CloseHandle(g_mutex); g_mutex = NULL; }   // the next copy takes it
    { FILE* f = F; F = NULL; fclose(f); }
    return 1;
}

