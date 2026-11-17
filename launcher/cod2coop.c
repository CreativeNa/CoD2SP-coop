// cod2coop.c - CoD2Coop.exe: the one file the second player needs.
//
// Embeds the bridge DLL and the GSC mod (launcher\blobs.h, made by tools\embed_blob.py) and does what
// tools\run_peer.ps1 and the manual steps did: find the game, check it is the build this was made for,
// install the mod, connect the running game to the other player, show the live log, collect logs.
//
//   CoD2Coop.exe                 interactive menu
//   CoD2Coop.exe check|install|uninstall|connect|logs [options]
//     --game DIR       game folder (default: found through Steam)
//     --peer IP        the other player's address        --port N       this PC listens here (27201)
//     --peer-port N    the other PC's port (27201)       --logport N    forward log lines to IP:N (27300, 0 = off)
//     --send HOST[:PORT]   with `logs`: also upload the zip to a running tools\log_collector.py (TCP 27301)
//     --verbose        show the once-a-second readback lines too
//
// It changes only the game folder it is pointed at: main\zz_coop.iwd, coop_port.txt, coop_peer.txt and the
// logs. It does not touch Steam, the registry (read only), the firewall, or any other process but CoD2SP_s.exe.
// Build: tools\build_release.ps1 (x86: the game is 32-bit and LoadLibrary is injected from a 32-bit process).

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include "blobs.h"
#include "../bridge/coop_text.h"   /* COOP_STR: the whole coop.str of each language */

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

#define VERSION "0.7.7"
#define GAME_EXE "CoD2SP_s.exe"
#define GAME_SHA256 "AB17A89CC3722AE80EDC1F4483CACBCAE08B0373B9128FE4EC053EBC79062281"            /* the Steam build */
#define GAME_SHA256_RETAIL "64E7ADCA30F5C25A858BB537932C7041D4588A0554BBB917B8E25AC43A0E5A48"     /* the retail v1.3 exe (25980416 bytes): the guest PC runs the co-op on it */
#define GAME_SHA256_REPORTED "A450569C269A076ACBD3BCBD52C3EE4E84432BB10E31FBD1D3A458D4335B133D"    /* a build a player reported: accepted, not verified by us */

static volatile int g_stop = 0;
static BOOL WINAPI OnCtrl(DWORD t) { (void)t; g_stop = 1; return TRUE; }

// ---------------------------------------------------------------- small helpers
static void trim(char* s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
    char* p = s; while (*p == ' ' || *p == '\t' || *p == '"') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    n = strlen(s); while (n && s[n - 1] == '"') s[--n] = 0;
}

static int ask(const char* prompt, const char* def, char* out, size_t cap) {
    if (def && *def) printf("%s [%s]: ", prompt, def); else printf("%s: ", prompt);
    fflush(stdout);
    if (!fgets(out, (int)cap, stdin)) { out[0] = 0; return 0; }
    trim(out);
    if (!out[0] && def) { strncpy(out, def, cap - 1); out[cap - 1] = 0; }
    return 1;
}

/* ---- menu texts in the language of the game ----
   The engine reads localizedstrings/coop.str and takes the LAST language line of a record whatever the game language is, and an archive
   beats a loose file, so a small archive main\\zz_coop_lang.iwd carries the file of ONE language, chosen here from localization.txt (the
   engine's own language source). The mod archive itself carries no coop.str. */
static unsigned long crc32_buf(const unsigned char* d, size_t n) {
    static unsigned long t[256]; static int init = 0;
    if (!init) { for (unsigned long i = 0; i < 256; i++) { unsigned long c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320UL ^ (c >> 1) : c >> 1; t[i] = c; } init = 1; }
    unsigned long c = 0xFFFFFFFFUL;
    for (size_t i = 0; i < n; i++) c = t[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFUL;
}
static void put16(unsigned char* p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char* p, unsigned long v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }
static int write_file(const char* path, const void* data, size_t len);
static int build_lang_iwd(const char* path, const unsigned char* data, size_t n) {
    static const char name[] = "localizedstrings/coop.str"; size_t nl = sizeof name - 1;
    size_t total = 30 + nl + n + 46 + nl + 22;
    unsigned char* z = (unsigned char*)calloc(1, total); if (!z) return 0;
    unsigned long crc = crc32_buf(data, n); size_t o = 0;
    put32(z + o, 0x04034B50UL); put16(z + o + 4, 20); put16(z + o + 8, 0); put32(z + o + 14, crc); put32(z + o + 18, (unsigned long)n); put32(z + o + 22, (unsigned long)n); put16(z + o + 26, (unsigned)nl);
    memcpy(z + o + 30, name, nl); memcpy(z + o + 30 + nl, data, n); o += 30 + nl + n;
    size_t cd = o;
    put32(z + o, 0x02014B50UL); put16(z + o + 4, 20); put16(z + o + 6, 20); put32(z + o + 16, crc); put32(z + o + 20, (unsigned long)n); put32(z + o + 24, (unsigned long)n); put16(z + o + 28, (unsigned)nl);
    memcpy(z + o + 46, name, nl); o += 46 + nl;
    put32(z + o, 0x06054B50UL); put16(z + o + 8, 1); put16(z + o + 10, 1); put32(z + o + 12, (unsigned long)(46 + nl)); put32(z + o + 16, (unsigned long)cd); o += 22;
    int ok = write_file(path, z, (unsigned long)o); free(z); return ok;
}
/* row of COOP_STR for the game language: english 0, french 1, german 2, italian 3, spanish 4, russian 5 (anything else: English) */
static int detect_lang_row(const char* game) {
    char p[MAX_PATH], buf[256] = "", w[32] = ""; int row = 0;
    snprintf(p, sizeof p, "%s\\coop_lang.txt", game);     /* test aid: a row number overrides */
    FILE* f = fopen(p, "r");
    if (f) { int v = -1; if (fscanf(f, "%d", &v) == 1 && v >= 0 && v <= 5) { fclose(f); return v; } fclose(f); }
    snprintf(p, sizeof p, "%s\\localization.txt", game);
    f = fopen(p, "r"); if (!f) return 0;
    size_t got = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[got] = 0;
    int k = 0; for (const char* c = buf; *c && k < 31; c++) { if (isalpha((unsigned char)*c)) w[k++] = (char)tolower((unsigned char)*c); else if (k) break; }
    w[k] = 0;
    if (!strcmp(w, "french")) row = 1; else if (!strcmp(w, "german")) row = 2; else if (!strcmp(w, "italian")) row = 3;
    else if (!strcmp(w, "spanish")) row = 4; else if (!strcmp(w, "russian")) row = 5;
    return row;
}
static int file_exists(const char* p) { DWORD a = GetFileAttributesA(p); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
static int dir_exists(const char* p) { DWORD a = GetFileAttributesA(p); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }

static void join(char* out, size_t cap, const char* a, const char* b) {
    _snprintf(out, cap, "%s%s%s", a, (a[0] && a[strlen(a) - 1] != '\\') ? "\\" : "", b);
    out[cap - 1] = 0;
}

static int write_file(const char* path, const void* data, size_t len) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    return w == len;
}

static unsigned char* read_file(const char* path, size_t* len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    unsigned char* b = (unsigned char*)malloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    size_t r = fread(b, 1, (size_t)n, f);
    fclose(f);
    if (r != (size_t)n) { free(b); return NULL; }
    *len = (size_t)n;
    return b;
}

static int sha256_buf(const unsigned char* data, size_t len, char hex[65]) {
    BCRYPT_ALG_HANDLE a = NULL; BCRYPT_HASH_HANDLE h = NULL; DWORD objLen = 0, cb = 0; UCHAR d[32];
    int ok = 0; PUCHAR obj = NULL;
    if (BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0) return 0;
    if (BCryptGetProperty(a, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objLen, sizeof objLen, &cb, 0) < 0) goto done;
    obj = (PUCHAR)malloc(objLen);
    if (!obj || BCryptCreateHash(a, &h, obj, objLen, NULL, 0, 0) < 0) goto done;
    if (BCryptHashData(h, (PUCHAR)data, (ULONG)len, 0) < 0 || BCryptFinishHash(h, d, 32, 0) < 0) goto done;
    for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02X", d[i]);
    ok = 1;
done:
    if (h) BCryptDestroyHash(h);
    if (a) BCryptCloseAlgorithmProvider(a, 0);
    free(obj);
    return ok;
}

static int sha256_file(const char* path, char hex[65]) {
    size_t n = 0; unsigned char* b = read_file(path, &n);
    if (!b) return 0;
    int ok = sha256_buf(b, n, hex);
    free(b);
    return ok;
}

// ---------------------------------------------------------------- this PC's addresses (Radmin VPN etc.)
// Prints every active IPv4 address with its adapter; a Radmin VPN adapter (description contains "Radmin",
// or an address in 26.0.0.0/8, which is Radmin's range) is marked, and its address is copied to
// radminOut so the menu can say "give THIS address to the other player". Returns the number of addresses.
static char g_bestIp[32];   // the address to give the other player: Radmin if present, else the first LAN one
static int list_local_ips(int print, char* radminOut, size_t cap) {
    ULONG size = 16384;
    g_bestIp[0] = 0;
    char firstIp[32] = "";
    IP_ADAPTER_ADDRESSES* buf = (IP_ADAPTER_ADDRESSES*)malloc(size);
    if (!buf) return 0;
    ULONG r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, buf, &size);
    if (r == ERROR_BUFFER_OVERFLOW) { free(buf); buf = (IP_ADAPTER_ADDRESSES*)malloc(size); if (!buf) return 0;
        r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, buf, &size); }
    int n = 0;
    if (radminOut) radminOut[0] = 0;
    if (r == NO_ERROR) {
        for (IP_ADAPTER_ADDRESSES* a = buf; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            char desc[160]; WideCharToMultiByte(CP_ACP, 0, a->Description, -1, desc, sizeof desc, NULL, NULL);
            for (IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u; u = u->Next) {
                struct sockaddr_in* sa = (struct sockaddr_in*)u->Address.lpSockaddr;
                if (!sa || sa->sin_family != AF_INET) continue;
                const char* ip = inet_ntoa(sa->sin_addr);
                if (!strncmp(ip, "169.254.", 8)) continue;                    // link-local: no use
                int radmin = strstr(desc, "Radmin") != NULL || !strncmp(ip, "26.", 3);
                if (radmin && radminOut && !radminOut[0]) { strncpy(radminOut, ip, cap - 1); radminOut[cap - 1] = 0; }
                if (print) printf("  %-16s %s%s\n", ip, desc, radmin ? "   <-- Radmin VPN" : "");
                if (!firstIp[0]) { strncpy(firstIp, ip, sizeof firstIp - 1); firstIp[sizeof firstIp - 1] = 0; }
                n++;
            }
        }
    }
    free(buf);
    { const char* best = (radminOut && radminOut[0]) ? radminOut : firstIp; strncpy(g_bestIp, best, sizeof g_bestIp - 1); g_bestIp[sizeof g_bestIp - 1] = 0; }
    return n;
}

// ---------------------------------------------------------------- finding the game
static int try_game(const char* dir, char* out, size_t cap) {
    char exe[MAX_PATH]; join(exe, sizeof exe, dir, GAME_EXE);
    if (!file_exists(exe)) return 0;
    strncpy(out, dir, cap - 1); out[cap - 1] = 0;
    size_t n = strlen(out); while (n && out[n - 1] == '\\') out[--n] = 0;
    return 1;
}

static int find_game(char* out, size_t cap) {
    char cwd[MAX_PATH];
    if (GetCurrentDirectoryA(sizeof cwd, cwd) && try_game(cwd, out, cap)) return 1;

    char steam[MAX_PATH] = ""; DWORD sz = sizeof steam;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath", RRF_RT_REG_SZ, NULL, steam, &sz) != ERROR_SUCCESS)
        steam[0] = 0;
    for (char* p = steam; *p; p++) if (*p == '/') *p = '\\';

    char cand[MAX_PATH];
    if (steam[0]) {
        join(cand, sizeof cand, steam, "steamapps\\common\\Call of Duty 2");
        if (try_game(cand, out, cap)) return 1;
        // other Steam libraries: "path"  "D:\\SteamLibrary" lines in libraryfolders.vdf
        char vdf[MAX_PATH]; join(vdf, sizeof vdf, steam, "steamapps\\libraryfolders.vdf");
        FILE* f = fopen(vdf, "r");
        if (f) {
            char line[1024];
            while (fgets(line, sizeof line, f)) {
                char* k = strstr(line, "\"path\"");
                if (!k) continue;
                char* q1 = strchr(k + 6, '"'); if (!q1) continue;
                char* q2 = strchr(q1 + 1, '"'); if (!q2) continue;
                char lib[MAX_PATH]; size_t n = (size_t)(q2 - q1 - 1); if (n >= sizeof lib) continue;
                memcpy(lib, q1 + 1, n); lib[n] = 0;
                char* w = lib; for (char* r = lib; *r; r++) { if (r[0] == '\\' && r[1] == '\\') r++; *w++ = *r; } *w = 0;
                join(cand, sizeof cand, lib, "steamapps\\common\\Call of Duty 2");
                if (try_game(cand, out, cap)) { fclose(f); return 1; }
            }
            fclose(f);
        }
    }
    static const char* fixed[] = {
        "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Call of Duty 2",
        "C:\\Program Files\\Steam\\steamapps\\common\\Call of Duty 2",
        "D:\\SteamLibrary\\steamapps\\common\\Call of Duty 2",
        "D:\\Steam\\steamapps\\common\\Call of Duty 2", NULL };
    for (int i = 0; fixed[i]; i++) if (try_game(fixed[i], out, cap)) return 1;
    return 0;
}

// the game exe must be the build the mod and bridge were made for
static int check_exe(const char* game, int verbose) {
    char exe[MAX_PATH], hex[65];
    join(exe, sizeof exe, game, GAME_EXE);
    if (!sha256_file(exe, hex)) { printf("  cannot read %s\n", exe); return 0; }
    int steam = !_stricmp(hex, GAME_SHA256), retail = !_stricmp(hex, GAME_SHA256_RETAIL), reported = !_stricmp(hex, GAME_SHA256_REPORTED);
    int ok = steam || retail || reported;
    if (verbose || !ok || reported) printf("  %s sha256 %s %s\n", GAME_EXE, hex, steam ? "(matches the Steam build)" : retail ? "(matches the retail v1.3 build)" : reported ? "(an exe reported by a player: accepted, but not tested by us; if the game does not start or the co-op does not connect, Uninstall and send coop5.log)" : "<- NOT an expected build");
    if (!ok) printf("  expected (Steam)            %s\n  expected (retail v1.3)      %s\n  This tool is made for Call of Duty 2 single player v1.3. Nothing was changed.\n", GAME_SHA256, GAME_SHA256_RETAIL);
    return ok;
}

// ---------------------------------------------------------------- processes
// all CoD2SP_s.exe processes whose image lives in `game`
static int find_game_pids(const char* game, DWORD* pids, int max) {
    int n = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe; pe.dwSize = sizeof pe;
    for (BOOL ok = Process32First(snap, &pe); ok && n < max; ok = Process32Next(snap, &pe)) {
        if (_stricmp(pe.szExeFile, GAME_EXE)) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) continue;
        char path[MAX_PATH]; DWORD sz = sizeof path;
        if (QueryFullProcessImageNameA(h, 0, path, &sz)) {
            char* sl = strrchr(path, '\\'); if (sl) *sl = 0;
            if (!_stricmp(path, game)) pids[n++] = pe.th32ProcessID;
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    return n;
}

static int inject(DWORD pid, const char* dll) {
    HANDLE p = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                           PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!p) { printf("  OpenProcess failed (error %lu)\n", GetLastError()); return 0; }
    size_t n = strlen(dll) + 1;
    void* mem = VirtualAllocEx(p, NULL, n, MEM_COMMIT, PAGE_READWRITE);
    if (!mem) { printf("  VirtualAllocEx failed (error %lu)\n", GetLastError()); CloseHandle(p); return 0; }
    WriteProcessMemory(p, mem, dll, n, NULL);
    FARPROC ll = GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    HANDLE t = CreateRemoteThread(p, NULL, 0, (LPTHREAD_START_ROUTINE)ll, mem, 0, NULL);
    if (!t) { printf("  CreateRemoteThread failed (error %lu)\n", GetLastError()); VirtualFreeEx(p, mem, 0, MEM_RELEASE); CloseHandle(p); return 0; }
    WaitForSingleObject(t, 15000);
    DWORD mod = 0; GetExitCodeThread(t, &mod);
    CloseHandle(t); VirtualFreeEx(p, mem, 0, MEM_RELEASE); CloseHandle(p);
    return mod != 0;
}

// ---------------------------------------------------------------- commands
typedef struct {
    char game[MAX_PATH];
    char peer[64];
    int port, peerPort, logPort;
    char send[96];
    int verbose;
    int radmin;      // --radmin: the other player is reached over Radmin VPN (26.x.x.x)
    int seconds;     // --seconds N: show the live log for N seconds, then return (0 = until Ctrl+C)
    int host;        // host mode: the guest's address is learned from its first packet, none is typed
    int noAutoload;  // --no-autoload: install only the mod archive (connect with this program instead of the in-game menu)
    int bridgeOnly;  // --bridge-only: replace only cod2net5.dll; a RUNNING game picks it up within seconds (live reload)
} Opt;

// true when `pid` has a module with exactly this file name (the autoloaded bridge is cod2net5.dll; the copies this
// program injects are named cod2net5_<n>.dll, so they do not match)
static int has_module(DWORD pid, const char* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32 me; me.dwSize = sizeof me;
    int found = 0;
    size_t nl = strlen(name);
    for (BOOL ok = Module32First(snap, &me); ok; ok = Module32Next(snap, &me)) {
        // a trailing '*' in `name` means "starts with" (the proxy's live copies are cod2net5_live_<n>.dll)
        if (nl && name[nl - 1] == '*' ? !_strnicmp(me.szModule, name, nl - 1) : !_stricmp(me.szModule, name)) { found = 1; break; }
    }
    CloseHandle(snap);
    return found;
}
// the bridge was loaded together with the game: directly (an older proxy) or as the proxy's reloadable copy
static int has_autoload_bridge(DWORD pid) { return has_module(pid, "cod2net5.dll") || has_module(pid, "cod2net5_live_*"); }

// is the file at `path` one of OUR d3d9 proxies (any build)? It carries a marker string; a hash would call an older
// build of ours "foreign" and a ReShade d3d9.dll must never be mistaken for ours.
static int is_our_proxy(const char* path) {
    size_t n = 0; unsigned char* b = read_file(path, &n);
    if (!b) return 0;
    static const char marker[] = "COD2COOP_D3D9_PROXY_V1";
    int found = 0;
    for (size_t i = 0; i + sizeof marker - 1 <= n && !found; i++)
        if (b[i] == 'C' && !memcmp(b + i, marker, sizeof marker - 1)) found = 1;
    free(b);
    return found;
}

// is the file at `path` byte-identical to the embedded blob?
static int is_ours(const char* path, const unsigned char* blob, unsigned long len) {
    char a[65], b[65];
    return file_exists(path) && sha256_file(path, a) && sha256_buf(blob, len, b) && !_stricmp(a, b);
}

// ---------------------------------------------------------------- the mod archive is built HERE, from the player's own game files
// main\zz_coop.iwd holds edited copies of the game's own scripts and menus, so it is never shipped: this program carries only our own
// build scripts (make_mod.ps1, patch_menu.ps1), our animation list and menu, runs them with the PowerShell that comes with Windows, and
// the result is written next to the game. coop_mod_version.txt remembers which release built it.
static int make_dirs(const char* path) {
    char t[MAX_PATH]; strncpy(t, path, sizeof t - 1); t[sizeof t - 1] = 0;
    for (char* p = t + 3; *p; p++) if (*p == '\\') { *p = 0; CreateDirectoryA(t, NULL); *p = '\\'; }
    return CreateDirectoryA(t, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static int put_payload(const char* root, const char* rel, const unsigned char* data, unsigned long len) {
    char p[MAX_PATH], d[MAX_PATH]; join(p, sizeof p, root, rel);
    strncpy(d, p, sizeof d - 1); d[sizeof d - 1] = 0; char* sl = strrchr(d, '\\'); if (sl) { *sl = 0; make_dirs(d); }
    return write_file(p, data, len);
}

static void run_hidden(char* cmd, const char* cwd, DWORD ms, DWORD* exitCode, int* finished) {
    STARTUPINFOA si; PROCESS_INFORMATION pi; memset(&si, 0, sizeof si); si.cb = sizeof si; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    *finished = 0; *exitCode = 1;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, cwd, &si, &pi)) { printf("  could not start a helper process (windows error %lu)\n", GetLastError()); return; }
    DWORD w = WaitForSingleObject(pi.hProcess, ms);
    if (w == WAIT_OBJECT_0) { GetExitCodeProcess(pi.hProcess, exitCode); *finished = 1; } else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
}

static int build_mod_iwd(const char* game, const char* dest) {
    char tmp[MAX_PATH], root[MAX_PATH];
    GetTempPathA(sizeof tmp, tmp);
    _snprintf(root, sizeof root - 1, "%scod2coop-build-%lu-%lu", tmp, GetCurrentProcessId(), GetTickCount()); root[sizeof root - 1] = 0;
    if (!make_dirs(root)) {          // %TEMP% may be locked down: the work folder then goes into the game folder (removed afterwards)
        DWORD e1 = GetLastError();
        char gg[MAX_PATH]; strncpy(gg, game, sizeof gg - 1); gg[sizeof gg - 1] = 0; { size_t n = strlen(gg); while (n > 3 && gg[n - 1] == '\\') gg[--n] = 0; }
        _snprintf(root, sizeof root - 1, "%s\\cod2coop-build-%lu", gg, GetTickCount()); root[sizeof root - 1] = 0;
        if (!make_dirs(root)) { printf("  cannot create a work folder (temp folder error %lu, game folder error %lu)\n", e1, GetLastError()); return 0; }
    }
    int ok = put_payload(root, "tools\\make_mod.ps1", g_ps_make, g_ps_make_len) && put_payload(root, "tools\\patch_menu.ps1", g_ps_menu, g_ps_menu_len)
          && put_payload(root, "tools\\animtrees\\coop_puppet.atr", g_atr, g_atr_len) && put_payload(root, "launcher\\menu\\coop.menu", g_coopmenu, g_coopmenu_len);
    char scratch[MAX_PATH], scratchMain[MAX_PATH], script[MAX_PATH], logp[MAX_PATH], g[MAX_PATH];
    join(scratch, sizeof scratch, root, "out"); join(scratchMain, sizeof scratchMain, scratch, "main"); make_dirs(scratchMain);
    join(script, sizeof script, root, "tools\\make_mod.ps1"); join(logp, sizeof logp, root, "build.log");
    strncpy(g, game, sizeof g - 1); g[sizeof g - 1] = 0; { size_t n = strlen(g); while (n > 3 && g[n - 1] == '\\') g[--n] = 0; }
    int built = 0;
    if (ok) {
        char cmd[4096];
        char ptmp[MAX_PATH]; join(ptmp, sizeof ptmp, root, "tmp"); make_dirs(ptmp);   // the build gets its own TEMP: the user's one may be locked down
        _snprintf(cmd, sizeof cmd - 1, "cmd.exe /c \"set \"TEMP=%s\"&& set \"TMP=%s\"&& \"powershell.exe\" -NoProfile -ExecutionPolicy Bypass -File \"%s\" -GameDir \"%s\" -TestDir \"%s\" -Probe net7 -Level moscow -Levels all -Install > \"%s\" 2>&1\"",
                  ptmp, ptmp, script, g, scratch, logp);
        cmd[sizeof cmd - 1] = 0;
        printf("  building the mod from your game files (about half a minute)...\n"); fflush(stdout);
        DWORD ec; int fin;
        run_hidden(cmd, root, 300000, &ec, &fin);
        char built_iwd[MAX_PATH]; join(built_iwd, sizeof built_iwd, scratchMain, "zz_coop.iwd");
        if (!file_exists(built_iwd)) { char alt[MAX_PATH]; join(alt, sizeof alt, scratch, "mods\\coop\\z_coop.iwd"); if (file_exists(alt)) strcpy(built_iwd, alt); }   // the same archive, written first
        size_t n = 0; unsigned char* b = (fin && file_exists(built_iwd)) ? read_file(built_iwd, &n) : NULL;
        DWORD wErr = 0; int exists = file_exists(built_iwd);
        if (b && n > 100000) { built = write_file(dest, b, n); if (!built) wErr = GetLastError(); if (built) printf("  mod built: %lu bytes\n", (unsigned long)n); }
        if (!built) printf("  details: script finished=%d exit code=%lu, built archive %s (%lu bytes), write to the game folder %s (windows error %lu, errno %d)\n",
                           fin, (unsigned long)ec, exists ? "present" : "MISSING", (unsigned long)n, (b && n > 100000) ? "failed" : "not tried", (unsigned long)wErr, errno);
        if (!built && wErr == 5) printf("  hint: windows error 5 = access denied to the game folder. Run this program as administrator (right click -> run as administrator),\n"
                                        "        or check that an antivirus / 'controlled folder access' is not blocking it.\n");
        free(b);
    }
    if (!built) {
        printf("  the mod could not be built.%s\n", ok ? "" : " (work files could not be written)");
        size_t n = 0; unsigned char* lg = file_exists(logp) ? read_file(logp, &n) : NULL;
        if (lg && n) { size_t from = n > 1200 ? n - 1200 : 0; printf("  --- last lines of the build log ---\n%.*s\n  ---\n", (int)(n - from), (const char*)lg + from); }
        free(lg);
    }
    { char rc[MAX_PATH * 2]; _snprintf(rc, sizeof rc - 1, "cmd.exe /c rmdir /s /q \"%s\"", root); rc[sizeof rc - 1] = 0;
      DWORD ec; int fin; run_hidden(rc, NULL, 20000, &ec, &fin); }
    return built;
}

static int mod_installed(const char* game, char* state, size_t cap) {
    char iwd[MAX_PATH], vf[MAX_PATH], ver[64] = "";
    join(iwd, sizeof iwd, game, "main\\zz_coop.iwd");
    if (!file_exists(iwd)) { strncpy(state, "not installed", cap); return 0; }
    join(vf, sizeof vf, game, "coop_mod_version.txt");
    FILE* f = fopen(vf, "r"); if (f) { if (fgets(ver, sizeof ver, f)) trim(ver); fclose(f); }
    if (!strcmp(ver, VERSION)) { strncpy(state, "installed (current)", cap); return 1; }
    strncpy(state, "installed, but a different version (run 'install' with the game closed)", cap);
    return 0;
}

static int cmd_check(Opt* o) {
    printf("Game folder : %s\n", o->game);
    int ok = check_exe(o->game, 1);
    char st[96]; mod_installed(o->game, st, sizeof st);
    printf("Mod         : %s\n", st);
    {
        char d3[MAX_PATH], br[MAX_PATH];
        join(d3, sizeof d3, o->game, "d3d9.dll"); join(br, sizeof br, o->game, "cod2net5.dll");
        const char* a = !file_exists(d3) ? "not installed (use host/join from this program)"
                      : is_our_proxy(d3) ? (is_ours(br, g_dll, g_dll_len) ? "installed (current): the in-game CO-OP menu drives the link" : "installed, bridge differs (run 'install' with the game closed)")
                      : "a FOREIGN d3d9.dll is in the game folder (ReShade or similar): not touched, no autoload";
        printf("Autoload    : %s\n", a);
    }
    DWORD pids[4]; int n = find_game_pids(o->game, pids, 4);
    printf("Game process: %s%s\n", n ? "running" : "not running", (n && has_autoload_bridge(pids[0])) ? " (bridge loaded with the game)" : "");
    char pf[MAX_PATH], line[128] = "";
    join(pf, sizeof pf, o->game, "coop_peer.txt");
    FILE* f = fopen(pf, "r"); if (f) { if (fgets(line, sizeof line, f)) trim(line); fclose(f); }
    printf("Peer file   : %s\n", line[0] ? line : "(none yet)");
    printf("This PC     :\n");
    char radmin[32]; list_local_ips(1, radmin, sizeof radmin);
    return ok ? 0 : 1;
}

static int cmd_install(Opt* o) {
    if (!check_exe(o->game, 0)) return 1;
    if (o->bridgeOnly) {
        // live reload: the proxy loads a private COPY of cod2net5.dll and watches the original, so this file can be replaced
        // while the game runs; the proxy swaps the running bridge for the new one in a few seconds
        char d3[MAX_PATH], br[MAX_PATH];
        join(d3, sizeof d3, o->game, "d3d9.dll"); join(br, sizeof br, o->game, "cod2net5.dll");
        if (!is_our_proxy(d3)) { printf("  our d3d9.dll proxy is not installed here: run a full 'install' first (game closed).\n"); return 1; }
        if (is_ours(br, g_dll, g_dll_len)) { printf("  cod2net5.dll is already this version; nothing to do.\n"); return 0; }
        if (!write_file(br, g_dll, g_dll_len)) {
            printf("  could not write cod2net5.dll (windows error %lu). A game started with an OLDER proxy locks it: close the game and run a full 'install' once.\n", GetLastError());
            return 1;
        }
        DWORD pids[4]; int np = find_game_pids(o->game, pids, 4);
        printf("  cod2net5.dll replaced.%s\n", np ? " The running game reloads the bridge within a few seconds (see '[proxy]' lines in coop5.log)." : " The next game start uses it.");
        return 0;
    }
    char main_[MAX_PATH], iwd[MAX_PATH];
    join(main_, sizeof main_, o->game, "main");
    if (!dir_exists(main_)) { printf("  no main folder in %s\n", o->game); return 1; }
    join(iwd, sizeof iwd, main_, "zz_coop.iwd");
    { DWORD pidsG[4]; if (find_game_pids(o->game, pidsG, 4)) { printf("  the game is running: close it first (it keeps the archive open).\n"); return 1; } }
    /* the version file goes first: the menu builder reads it for the version line (a first install used to print "dev") */
    char vfile[MAX_PATH]; join(vfile, sizeof vfile, o->game, "coop_mod_version.txt"); write_file(vfile, VERSION, strlen(VERSION));
    if (!build_mod_iwd(o->game, iwd)) {
        printf("  could not build or write %s. If the game is running, close it first.\n", iwd);
        DeleteFileA(vfile);
        return 1;
    }
    printf("  mod installed: main\\zz_coop.iwd (built from your own game files). Remove it any time with 'uninstall'.\n");
    {
        static const char* langs[6] = { "English", "French", "German", "Italian", "Spanish", "Russian" };
        int lr = detect_lang_row(o->game); char lp[MAX_PATH];
        join(lp, sizeof lp, main_, "zz_coop_lang.iwd");
        if (build_lang_iwd(lp, (const unsigned char*)COOP_STR[lr], strlen(COOP_STR[lr]))) printf("  menu texts: %s (main\\zz_coop_lang.iwd, the language of the game from localization.txt)\n", langs[lr]);
        else printf("  could not write main\\zz_coop_lang.iwd (windows error %lu): the menu texts will be missing\n", GetLastError());
    }

    if (o->noAutoload) {
        printf("  Next: start the game through Steam, start a NEW mission, then run 'host' or 'join'.\n");
        return 0;
    }
    // autoload: the bridge DLL plus a d3d9.dll proxy that loads it when the game starts, so the in-game CO-OP menu can work
    char d3[MAX_PATH], br[MAX_PATH];
    join(d3, sizeof d3, o->game, "d3d9.dll"); join(br, sizeof br, o->game, "cod2net5.dll");
    if (file_exists(d3) && !is_our_proxy(d3)) {
        printf("  A different d3d9.dll is already in the game folder (ReShade or similar). It was NOT touched, so the in-game\n"
               "  CO-OP menu cannot connect. Use 'host' / 'join' from this program instead, or move that d3d9.dll away first.\n");
        return 1;
    }
    // bridge first, proxy last: the proxy is what loads the bridge
    if (!write_file(br, g_dll, g_dll_len)) { printf("  could not write cod2net5.dll (windows error %lu): close the game first.\n", GetLastError()); return 1; }
    if (!write_file(d3, g_d3d9, g_d3d9_len)) { printf("  could not write d3d9.dll (windows error %lu): close the game first.\n", GetLastError()); return 1; }
    // settings left by an earlier host / join / connect run belong to the other way of driving the bridge
    { static const char* stale[] = { "coop_port.txt", "coop_peer.txt", NULL };
      for (int i = 0; stale[i]; i++) { char sp[MAX_PATH]; join(sp, sizeof sp, o->game, stale[i]); if (file_exists(sp) && DeleteFileA(sp)) printf("  removed leftover %s\n", stale[i]); } }
    printf("  autoload installed: d3d9.dll + cod2net5.dll. The bridge now starts with the game (single player only) and does\n"
           "  nothing until you click HOST or JOIN in the CO-OP screen of the main menu. 'uninstall' removes everything.\n");
    printf("  Next: start the game through Steam; main menu -> CO-OP.\n");
    return 0;
}

static int cmd_uninstall(Opt* o) {
    static const char* files[] = { "main\\zz_coop.iwd", "coop_mod_version.txt", "main\\zz_coop_lang.iwd", "main\\coop_prefs.cfg", "main\\localizedstrings\\coop.str", "d3d9.dll", "cod2net5.dll", "coop_port.txt", "coop_peer.txt", "coop_window.txt", "coop_mouse.txt", NULL };
    int bad = 0;
    for (int i = 0; files[i]; i++) {
        char p[MAX_PATH]; join(p, sizeof p, o->game, files[i]);
        if (!file_exists(p)) continue;
        // never delete somebody else's d3d9.dll
        if (!strcmp(files[i], "d3d9.dll") && !is_our_proxy(p)) { printf("  d3d9.dll is not ours: left alone\n"); continue; }
        if (DeleteFileA(p)) printf("  removed %s\n", files[i]);
        else { printf("  could not remove %s (windows error %lu; is the game running?)\n", files[i], GetLastError()); bad = 1; }
    }
    printf("  logs (coop5.log, coop_net.csv) are kept.\n");
    return bad;
}

// print log lines added since `*off`; skip the once-a-second readback lines unless verbose
static void show_new_log(const char* logp, long* off, int verbose, int* sawBindFail) {
    FILE* f = fopen(logp, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END); long end = ftell(f);
    if (end < *off) *off = 0;
    fseek(f, *off, SEEK_SET);
    char line[600];
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line); if (!n || line[n - 1] != '\n') break;   // partial line: next time
        *off = ftell(f);
        trim(line);
        if (strstr(line, "FAILED")) *sawBindFail = 1;
        if (!verbose && strstr(line, "gsc readback")) continue;
        printf("  %s\n", line);
    }
    fclose(f);
}

static int cmd_connect(Opt* o) {
    if (!check_exe(o->game, 0)) return 1;
    DWORD pids[4]; int np = find_game_pids(o->game, pids, 4);
    if (np == 0) { printf("  The game is not running. Start Call of Duty 2 through Steam first (a mission can start afterwards).\n"); return 1; }
    if (np > 1) { printf("  %d game processes from this folder are running; close all but one.\n", np); return 1; }
    if (has_autoload_bridge(pids[0])) {
        printf("  The bridge was loaded together with the game (autoload). Use the CO-OP button in the game's main menu:\n"
               "  HOST on one PC, JOIN with the host's address on the other. (A second bridge would fight for the port.)\n"
               "  To use host / join from this program instead: 'uninstall', start the game again, then run it.\n");
        return 1;
    }

    char st[96];
    if (!mod_installed(o->game, st, sizeof st)) printf("  note: mod is %s. Run 'install' with the game closed, then restart it.\n", st);

    char pp[MAX_PATH], tp[MAX_PATH], buf[128];
    join(pp, sizeof pp, o->game, "coop_port.txt");
    join(tp, sizeof tp, o->game, "coop_peer.txt");
    _snprintf(buf, sizeof buf, "%d\r\n", o->port);
    if (!write_file(pp, buf, strlen(buf))) { printf("  cannot write coop_port.txt\n"); return 1; }
    if (o->host) _snprintf(buf, sizeof buf, "auto %d\r\n", o->peerPort);        // the guest sends first; its address is learned
    else if (o->logPort) _snprintf(buf, sizeof buf, "%s %d %d\r\n", o->peer, o->peerPort, o->logPort);
    else _snprintf(buf, sizeof buf, "%s %d\r\n", o->peer, o->peerPort);
    if (!write_file(tp, buf, strlen(buf))) { printf("  cannot write coop_peer.txt\n"); return 1; }

    // a fresh module name every time: LoadLibrary on an already loaded DLL does not run DllMain again
    // Where to put the DLL: %TEMP%\cod2coop first; if that is not writable (policy, antivirus, a sandbox)
    // the folder of this exe, then the game folder. The game process only needs to be able to read it.
    char tmp[MAX_PATH], exedir[MAX_PATH], dll[MAX_PATH], tried[3][MAX_PATH];
    GetTempPathA(sizeof tmp, tmp);
    GetModuleFileNameA(NULL, exedir, sizeof exedir);
    { char* sl = strrchr(exedir, '\\'); if (sl) *sl = 0; }
    join(tried[0], sizeof tried[0], tmp, "cod2coop");
    strncpy(tried[1], exedir, MAX_PATH - 1); tried[1][MAX_PATH - 1] = 0;
    strncpy(tried[2], o->game, MAX_PATH - 1); tried[2][MAX_PATH - 1] = 0;
    _snprintf(buf, sizeof buf, "cod2net5_%lu.dll", GetTickCount());
    int extracted = 0;
    for (int i = 0; i < 3 && !extracted; i++) {
        CreateDirectoryA(tried[i], NULL);
        join(dll, sizeof dll, tried[i], buf);
        if (write_file(dll, g_dll, g_dll_len)) { extracted = 1; if (i) printf("  note: %%TEMP%% was not writable, bridge extracted to %s\n", tried[i]); }
        else printf("  cannot write the bridge to %s (windows error %lu)\n", tried[i], GetLastError());
    }
    if (!extracted) { printf("  cannot extract the bridge anywhere.\n"); return 1; }

    char logp[MAX_PATH]; join(logp, sizeof logp, o->game, "coop5.log");
    long off = 0;
    { FILE* f = fopen(logp, "rb"); if (f) { fseek(f, 0, SEEK_END); off = ftell(f); fclose(f); } }

    if (o->host || strncmp(o->peer, "127.", 4))
        printf("  firewall: Windows must let %s receive UDP %d. Radmin VPN's network is usually 'Public', so allow it for Public too\n"
               "  (the prompt on the first start has both boxes; or Windows Security > Firewall > Allow an app).\n", GAME_EXE, o->port);
    if (o->host)
        printf("  injecting the bridge into %s (pid %lu) as HOST: listen %d, the guest is learned from its first packet\n", GAME_EXE, pids[0], o->port);
    else
        printf("  injecting the bridge into %s (pid %lu): listen %d, send to %s:%d%s\n", GAME_EXE, pids[0], o->port, o->peer, o->peerPort,
               o->logPort ? ", log lines forwarded" : "");
    if (!inject(pids[0], dll)) { printf("  injection failed.\n"); return 1; }

    int bindFail = 0;
    printf("  --- live log (Ctrl+C to stop; the link keeps running inside the game) ---\n");
    SetConsoleCtrlHandler(OnCtrl, TRUE);
    for (int i = 0; !g_stop; i++) {
        if (o->seconds && i >= o->seconds * 2) break;
        Sleep(500);
        show_new_log(logp, &off, o->verbose, &bindFail);
        if (bindFail) {
            printf("  The port %d is still held by an earlier bridge in this game process. Restart the game, or use --port with a new number on both PCs.\n", o->port);
            return 1;
        }
    }
    return 0;
}

// Host: no address to type. Prints the line the guest needs, then injects the bridge in "auto" mode.
static int cmd_host(Opt* o) {
    char mine[32];
    printf("This PC's addresses:\n");
    if (!list_local_ips(1, mine, sizeof mine)) printf("  (none found)\n");
    o->host = 1;
    if (g_bestIp[0]) {
        printf("\n  >>> Tell the guest to run:  CoD2Coop.exe join %s%s\n", g_bestIp, mine[0] ? "   (Radmin address)" : "   (LAN address; use your Radmin address if you play over Radmin)");
        printf("  The guest must be in the same Radmin network (or LAN), with the game in a new Moscow mission.\n\n");
    } else printf("\n  No network address found: check your network / Radmin VPN.\n\n");
    return cmd_connect(o);
}

static int tcp_send_file(const char* host, int port, const char* path, const char* name) {
    size_t n = 0; unsigned char* data = read_file(path, &n);
    if (!data) { printf("  cannot read %s\n", path); return 0; }
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((u_short)port); a.sin_addr.s_addr = inet_addr(host);
    int ok = 0;
    DWORD to = 8000; setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char*)&to, sizeof to); setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof to);
    if (a.sin_addr.s_addr == INADDR_NONE) { printf("  bad address '%s'\n", host); goto done; }
    if (connect(s, (struct sockaddr*)&a, sizeof a) != 0) { printf("  cannot connect to %s:%d (is tools\\log_collector.py running there, firewall open?)\n", host, port); goto done; }
    {
        char head[200]; int hl = _snprintf(head, sizeof head - 1, "CODLOG1\n%s\n%lu\n", name, (unsigned long)n);
        send(s, head, hl, 0);
        size_t sent = 0;
        while (sent < n) { int r = send(s, (const char*)data + sent, (int)((n - sent) > 65536 ? 65536 : (n - sent)), 0); if (r <= 0) break; sent += (size_t)r; }
        char rep[16] = ""; recv(s, rep, sizeof rep - 1, 0);
        ok = sent == n && !strncmp(rep, "OK", 2);
        printf("  upload %s (%lu bytes)\n", ok ? "done" : "FAILED", (unsigned long)n);
    }
done:
    closesocket(s); free(data); return ok;
}

static int cmd_logs(Opt* o) {
    char stamp[32]; SYSTEMTIME t; GetLocalTime(&t);
    _snprintf(stamp, sizeof stamp, "%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, sizeof exe);
    char* sl = strrchr(exe, '\\'); if (sl) *sl = 0;
    char zip[MAX_PATH], name[96];
    _snprintf(name, sizeof name, "CoD2Coop-logs-%s.zip", stamp);
    join(zip, sizeof zip, exe, name);

    char files[4][MAX_PATH]; const char* names[4] = { "coop5.log", "coop_net.csv", "coop_peer.txt", "coop_port.txt" };
    char list[4 * MAX_PATH + 32] = ""; int cnt = 0;
    for (int i = 0; i < 4; i++) {
        join(files[i], sizeof files[i], o->game, names[i]);
        if (!file_exists(files[i])) continue;
        strcat(list, cnt ? ",'" : "'"); strcat(list, files[i]); strcat(list, "'"); cnt++;
    }
    if (!cnt) { printf("  no logs yet in %s (connect first)\n", o->game); return 1; }

    char cmd[3 * MAX_PATH + 200];
    _snprintf(cmd, sizeof cmd, "powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"Compress-Archive -Force -Path %s -DestinationPath '%s'\"", list, zip);
    STARTUPINFOA si; PROCESS_INFORMATION pi; memset(&si, 0, sizeof si); si.cb = sizeof si; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) { printf("  cannot start PowerShell to make the zip\n"); return 1; }
    WaitForSingleObject(pi.hProcess, 60000);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    if (!file_exists(zip)) { printf("  the zip was not created\n"); return 1; }
    printf("  saved %s\n  Send this file to the other player (it holds coop5.log, coop_net.csv and the two small settings files; no personal data besides the peer address).\n", zip);

    if (o->send[0]) {
        char host[64]; int port = 27301; strncpy(host, o->send, sizeof host - 1); host[sizeof host - 1] = 0;
        char* c = strchr(host, ':'); if (c) { *c = 0; port = atoi(c + 1); }
        tcp_send_file(host, port, zip, name);
    }
    return 0;
}

// ---------------------------------------------------------------- main
static void usage(void) {
    printf("CoD2Coop %s - co-op link for Call of Duty 2 (single player)\n\n"
           "  CoD2Coop.exe install         mod + the CO-OP screen in the game's main menu + the autoloaded bridge (game CLOSED)\n"
           "                               --no-autoload: only the mod archive (then use host / join from this program)\n"
           "  CoD2Coop.exe install --bridge-only   replace only the bridge: a RUNNING game reloads it within seconds (no restart)\n"
           "  CoD2Coop.exe                 interactive menu\n"
           "  CoD2Coop.exe host            you host: prints what the guest runs; no address to type\n"
           "  CoD2Coop.exe join <host>     you are the guest (the host's address; --peer <host> also works)\n"
           "  CoD2Coop.exe check | install | uninstall | connect | logs   [options]   (connect = both addresses typed)\n\n"
           "  --game DIR  --peer IP  --port N  --peer-port N  --logport N  --send HOST[:PORT]  --verbose  --radmin  --seconds N\n\n"
           "  --radmin: reach the other player over Radmin VPN (addresses 26.x.x.x); this PC's Radmin address is shown\n"
           "            by 'connect' and 'check' either way when Radmin VPN is connected.\n", VERSION);
}

static void defaults(Opt* o) {
    memset(o, 0, sizeof *o);
    o->port = 27201; o->peerPort = 27201; o->logPort = 27300;
}

static int ensure_game(Opt* o) {
    if (o->game[0]) {
        char given[MAX_PATH]; strncpy(given, o->game, sizeof given - 1); given[sizeof given - 1] = 0;   // out must not alias the input
        if (!try_game(given, o->game, sizeof o->game)) { printf("No %s in '%s'.\n", GAME_EXE, given); return 0; }
        return 1;
    }
    if (find_game(o->game, sizeof o->game)) return 1;
    printf("Could not find Call of Duty 2 through Steam.\n");
    char b[MAX_PATH]; ask("Game folder (the one with CoD2SP_s.exe)", NULL, b, sizeof b);
    if (!try_game(b, o->game, sizeof o->game)) { printf("No %s in '%s'.\n", GAME_EXE, b); return 0; }
    return 1;
}

static void ask_peer(Opt* o, const char* who) {
    char mine[32];
    printf("This PC's addresses:\n");
    if (!list_local_ips(1, mine, sizeof mine)) printf("  (none found)\n");
    if (mine[0]) { o->radmin = 1; printf("\n  Give the other player your Radmin VPN address: %s\n\n", mine); }
    else if (o->radmin) printf("\n  Radmin VPN is not running or not connected on this PC: no 26.x.x.x address.\n\n");

    if (!o->peer[0]) {
        char pf[MAX_PATH], line[128] = "", def[64] = "";
        join(pf, sizeof pf, o->game, "coop_peer.txt");
        FILE* f = fopen(pf, "r"); if (f) { if (fgets(line, sizeof line, f)) sscanf(line, "%63s", def); fclose(f); }
        char prompt[160];
        _snprintf(prompt, sizeof prompt, "%s %s (127.0.0.1 = test against yourself)", who, o->radmin ? "Radmin VPN address, 26.x.x.x" : "IP address");
        prompt[sizeof prompt - 1] = 0;
        ask(prompt, def, o->peer, sizeof o->peer);
    }
    if (o->radmin && strncmp(o->peer, "26.", 3) && strncmp(o->peer, "127.", 4))
        printf("  note: %s is not a Radmin address (they start with 26.). Over the Internet without Radmin you need a forwarded UDP port.\n", o->peer);
    if (!strncmp(o->peer, "26.", 3) && !mine[0])
        printf("  note: the peer is a Radmin address but Radmin VPN is not connected here, so the packets cannot reach it.\n");
}

int main(int argc, char** argv) {
    Opt o; defaults(&o);
    const char* cmd = NULL;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (!_stricmp(a, "--game") && i + 1 < argc) { strncpy(o.game, argv[++i], sizeof o.game - 1); trim(o.game); }
        else if (!_stricmp(a, "--peer") && i + 1 < argc) strncpy(o.peer, argv[++i], sizeof o.peer - 1);
        else if (!_stricmp(a, "--port") && i + 1 < argc) o.port = atoi(argv[++i]);
        else if (!_stricmp(a, "--peer-port") && i + 1 < argc) o.peerPort = atoi(argv[++i]);
        else if (!_stricmp(a, "--logport") && i + 1 < argc) o.logPort = atoi(argv[++i]);
        else if (!_stricmp(a, "--send") && i + 1 < argc) strncpy(o.send, argv[++i], sizeof o.send - 1);
        else if (!_stricmp(a, "--verbose")) o.verbose = 1;
        else if (!_stricmp(a, "--radmin")) o.radmin = 1;
        else if (!_stricmp(a, "--no-autoload")) o.noAutoload = 1;
        else if (!_stricmp(a, "--bridge-only")) o.bridgeOnly = 1;
        else if (!_stricmp(a, "--seconds") && i + 1 < argc) o.seconds = atoi(argv[++i]);
        else if (!_stricmp(a, "--help") || !_stricmp(a, "-h") || !_stricmp(a, "/?")) { usage(); return 0; }
        else if (a[0] != '-' && !cmd) cmd = a;
        else if (a[0] != '-' && cmd && !o.peer[0]) strncpy(o.peer, a, sizeof o.peer - 1);   // "join 26.x.x.x"
        else { usage(); return 2; }
    }
    if (o.port <= 0 || o.port > 65535 || o.peerPort <= 0 || o.peerPort > 65535 || o.logPort < 0 || o.logPort > 65535) { printf("port out of range\n"); return 2; }

    printf("CoD2Coop %s\n", VERSION);
    if (!ensure_game(&o)) return 1;

    if (cmd) {
        if (!_stricmp(cmd, "check")) return cmd_check(&o);
        if (!_stricmp(cmd, "install")) return cmd_install(&o);
        if (!_stricmp(cmd, "uninstall")) return cmd_uninstall(&o);
        if (!_stricmp(cmd, "host")) return cmd_host(&o);
        if (!_stricmp(cmd, "join")) { ask_peer(&o, "The host's"); return cmd_connect(&o); }
        if (!_stricmp(cmd, "connect")) { ask_peer(&o, "The other player's"); return cmd_connect(&o); }
        if (!_stricmp(cmd, "logs")) return cmd_logs(&o);
        usage(); return 2;
    }

    for (;;) {
        char st[96]; mod_installed(o.game, st, sizeof st);
        DWORD pids[4]; int n = find_game_pids(o.game, pids, 4);
        printf("\nGame: %s\nMod: %s | game %s\n\n", o.game, st, n ? "running" : "not running");
        printf("  1) Install the mod (game must be closed)\n"
               "  2) HOST a game  (nothing to type; the guest joins you)\n"
               "  3) JOIN a game  (type the host's address)\n"
               "  4) Collect logs (zip, optionally send to the host)\n"
               "  5) Check status\n"
               "  6) Uninstall the mod\n"
               "  0) Exit\n\n");
        char c[16]; ask("Choose", NULL, c, sizeof c);
        g_stop = 0;
        switch (c[0]) {
        case '1': cmd_install(&o); break;
        case '2': o.peer[0] = 0; cmd_host(&o); break;
        case '3': o.host = 0; ask_peer(&o, "The host's"); cmd_connect(&o); break;
        case '4': {
            char s[96] = ""; ask("Upload to the host too? host IP (empty = only save the zip)", o.peer, s, sizeof s);
            if (s[0]) strncpy(o.send, s, sizeof o.send - 1);
            cmd_logs(&o); break; }
        case '5': cmd_check(&o); break;
        case '6': cmd_uninstall(&o); break;
        case '0': case 0: return 0;
        default: break;
        }
    }
}
