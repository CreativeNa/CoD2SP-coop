// d3d9proxy.c - a d3d9.dll that sits in the game folder and starts the co-op bridge when the game starts.
//
// Why: a menu cannot load a DLL, so the in-game Co-op menu needs the bridge to be in the process already.
// The renderer (gfx_d3d_x86_s.dll, and the MP one) imports exactly one function from d3d9.dll, Direct3DCreate9,
// and d3d9 is not a KnownDLL, so a d3d9.dll next to the exe is loaded first. This proxy exports that one
// function and forwards it to the real d3d9.dll in the system folder (full path, never searched).
//
// What it does besides forwarding, in CoD2SP_s.exe only (CoD2MP_s.exe / PunkBuster: forward and nothing else):
//   * LIVE RELOAD. It does not load cod2net5.dll itself; it loads a private COPY (cod2net5_live_<n>.dll), so the original file
//     is never locked and can be replaced while the game runs. A watcher thread polls the original once a second; when its
//     size or time changes it asks the running copy to stop (the exported CoopStop), frees it, and loads a fresh copy.
//     If the old copy does not stop it is left alone (never freed under running threads) and the log says so.
//   * everything is logged as "[proxy] ..." lines in coop5.log.
//
// Build (x86): cl /nologo /MT /O2 /LD d3d9proxy.c /Fe:d3d9.dll

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#pragma comment(linker, "/EXPORT:Direct3DCreate9=_Direct3DCreate9@4")
#pragma comment(lib, "user32.lib")

// Marker the launcher looks for to tell THIS proxy (of any build) from somebody else's d3d9.dll (ReShade etc.). Exported so
// the linker keeps it; comparing the whole file by hash would call an older build of ours "foreign".
__declspec(dllexport) const char Cod2CoopProxyMarker[] = "COD2COOP_D3D9_PROXY_V1";

typedef void* (WINAPI *PFN_CREATE)(UINT);
typedef int (__cdecl *PFN_STOP)(void);
static PFN_CREATE g_create = NULL;

static char g_dir[MAX_PATH];        // the folder of this proxy (= the game folder), with a trailing backslash
static HMODULE g_bridge = NULL;     // the live copy that is loaded right now
static char g_livePath[MAX_PATH];   // its file name, deleted again after it has been freed

static int IsSinglePlayerExe(void) {
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
    if (!n || n >= MAX_PATH) return 0;
    const char* base = strrchr(exe, '\\');
    base = base ? base + 1 : exe;
    return _stricmp(base, "CoD2SP_s.exe") == 0;
}

// one line "[proxy] hh:mm:ss.mmm text" appended to coop5.log (opened and closed each time: the bridge keeps its own handle)
static void PLog(const char* fmt, ...) {
    char path[MAX_PATH], msg[300];
    _snprintf(path, sizeof path - 1, "%scoop5.log", g_dir); path[sizeof path - 1] = 0;
    va_list a; va_start(a, fmt); _vsnprintf(msg, sizeof msg - 1, fmt, a); va_end(a); msg[sizeof msg - 1] = 0;
    SYSTEMTIME t; GetLocalTime(&t);
    FILE* f = fopen(path, "a");
    if (f) { fprintf(f, "[proxy] %02d:%02d:%02d.%03d %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg); fclose(f); }
}

static int StatSrc(FILETIME* ft, DWORD* size) {
    char src[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA d;
    _snprintf(src, sizeof src - 1, "%scod2net5.dll", g_dir); src[sizeof src - 1] = 0;
    if (!GetFileAttributesExA(src, GetFileExInfoStandard, &d)) return 0;
    *ft = d.ftLastWriteTime; *size = d.nFileSizeLow;
    return 1;
}

// copy cod2net5.dll to a private name and load that copy
static int LoadLive(void) {
    char src[MAX_PATH];
    static int counter = 0;
    _snprintf(src, sizeof src - 1, "%scod2net5.dll", g_dir); src[sizeof src - 1] = 0;
    _snprintf(g_livePath, sizeof g_livePath - 1, "%scod2net5_live_%lu_%d.dll", g_dir, GetTickCount(), counter++); g_livePath[sizeof g_livePath - 1] = 0;
    if (!CopyFileA(src, g_livePath, FALSE)) { PLog("cannot copy the bridge (error %lu)", GetLastError()); g_livePath[0] = 0; return 0; }
    g_bridge = LoadLibraryA(g_livePath);
    if (!g_bridge) { PLog("cannot load the bridge copy (error %lu)", GetLastError()); DeleteFileA(g_livePath); g_livePath[0] = 0; return 0; }
    return 1;
}

// leftovers of an earlier run that crashed: a loaded copy cannot be deleted (that is fine), a stale one can
static void RemoveStaleCopies(void) {
    char pat[MAX_PATH];
    WIN32_FIND_DATAA fd;
    _snprintf(pat, sizeof pat - 1, "%scod2net5_live_*.dll", g_dir); pat[sizeof pat - 1] = 0;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char p[MAX_PATH];
        _snprintf(p, sizeof p - 1, "%s%s", g_dir, fd.cFileName); p[sizeof p - 1] = 0;
        DeleteFileA(p);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// stop the running copy and free it. 1 = freed (or nothing was loaded), 0 = it did not stop: leave it loaded
static int UnloadLive(void) {
    if (!g_bridge) return 1;
    PFN_STOP stop = (PFN_STOP)GetProcAddress(g_bridge, "CoopStop");
    if (!stop) { PLog("the loaded bridge has no CoopStop (an older version): it cannot be reloaded, restart the game"); return 0; }
    if (!stop()) { PLog("the loaded bridge did not stop in time: keeping it, restart the game to load the new file"); return 0; }
    FreeLibrary(g_bridge); g_bridge = NULL;
    if (g_livePath[0]) { DeleteFileA(g_livePath); g_livePath[0] = 0; }
    return 1;
}

// ================================================================================================================
// Crash and hang reports (own implementation; idea only from the CoD2x patch). Written with the smallest code path on plain Win32
// calls (CreateFile/WriteFile/wvsprintf), no CRT stdio, so a damaged process can still write them. Content: exception code and
// address, module+offset, registers, an EBP chain, a scan of the raw stack for code addresses, the loaded modules. Only module
// FILE names are printed, never a folder, so a report holds no user name.
// ================================================================================================================
static LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = NULL;
static volatile LONG g_inCrash = 0;

static HANDLE ReportOpen(const char* kind) {
    char p[MAX_PATH]; SYSTEMTIME t; GetLocalTime(&t);
    wsprintfA(p, "%scoop_%s_%04d%02d%02d-%02d%02d%02d.txt", g_dir, kind, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return CreateFileA(p, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}
static void Out(HANDLE f, const char* fmt, ...) {
    char b[600]; va_list a; DWORD w;
    va_start(a, fmt); int n = wvsprintfA(b, fmt, a); va_end(a);
    if (n > 0) WriteFile(f, b, (DWORD)n, &w, NULL);
}
// "name.dll+0xOFFSET" for an address inside a loaded module, otherwise the bare address
static void Where(DWORD addr, char* out) {
    MEMORY_BASIC_INFORMATION mbi;
    out[0] = 0;
    if (VirtualQuery((void*)addr, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT && mbi.AllocationBase) {
        char path[MAX_PATH];
        if (GetModuleFileNameA((HMODULE)mbi.AllocationBase, path, MAX_PATH)) {
            const char* b = strrchr(path, '\\'); b = b ? b + 1 : path;
            wsprintfA(out, "%s+0x%X", b, addr - (DWORD)(ULONG_PTR)mbi.AllocationBase);
            return;
        }
    }
    wsprintfA(out, "0x%08X", addr);
}
static int Readable(DWORD addr, DWORD len) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void*)addr, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    return addr + len <= (DWORD)(ULONG_PTR)mbi.BaseAddress + mbi.RegionSize;
}
static int ExecAddr(DWORD addr) {
    MEMORY_BASIC_INFORMATION mbi;
    return VirtualQuery((void*)addr, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}
static void WriteModules(HANDLE f) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    Out(f, "\r\nLoaded modules (name, base, size):\r\n");
    if (snap == INVALID_HANDLE_VALUE) { Out(f, "  (the module list is not available)\r\n"); return; }
    MODULEENTRY32 me; me.dwSize = sizeof me;
    for (BOOL ok = Module32First(snap, &me); ok; ok = Module32Next(snap, &me))
        Out(f, "  %-28s 0x%08X  0x%X\r\n", me.szModule, (DWORD)(ULONG_PTR)me.modBaseAddr, me.modBaseSize);
    CloseHandle(snap);
}
// words: a copy of the stack taken at `esp` (n DWORDs); the EBP chain and the scan both read only validated memory
static void WriteStack(HANDLE f, DWORD ebp, DWORD esp, const DWORD* words, int n) {
    char w[160];
    Out(f, "\r\nEBP chain (frames of code compiled with frame pointers):\r\n");
    DWORD cur = ebp;
    for (int i = 0; i < 24 && cur && !(cur & 3) && Readable(cur, 8); i++) {
        DWORD next = *(DWORD*)(ULONG_PTR)cur, ret = *(DWORD*)(ULONG_PTR)(cur + 4);
        Where(ret, w); Out(f, "  #%d  %s\r\n", i, w);
        if (next <= cur) break;
        cur = next;
    }
    Out(f, "\r\nStack scan (words at ESP that point into executable code; the nearest are the likeliest callers):\r\n");
    int shown = 0;
    for (int i = 0; i < n && shown < 40; i++) {
        if (words[i] > 0x10000 && ExecAddr(words[i])) { Where(words[i], w); Out(f, "  [esp+0x%X]  %s\r\n", i * 4, w); shown++; }
    }
    if (!shown) Out(f, "  (nothing found)\r\n");
}

static const char* ExcName(DWORD c) {
    switch (c) {
    case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_PRIV_INSTRUCTION: return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR: return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    default: return "(other exception)";
    }
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    if (!InterlockedExchange(&g_inCrash, 1) && ep && ep->ExceptionRecord && ep->ContextRecord) {
        EXCEPTION_RECORD* er = ep->ExceptionRecord; CONTEXT* c = ep->ContextRecord;
        HANDLE f = ReportOpen("crash");
        if (f != INVALID_HANDLE_VALUE) {
            char w[160];
            Out(f, "CoD2 co-op crash report\r\nThe game process hit an unhandled exception.\r\n\r\n");
            Out(f, "Exception: 0x%08X %s\r\n", er->ExceptionCode, ExcName(er->ExceptionCode));
            Where((DWORD)(ULONG_PTR)er->ExceptionAddress, w); Out(f, "At:        %s\r\n", w);
            if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
                Out(f, "Access:    %s address 0x%08X\r\n", er->ExceptionInformation[0] == 0 ? "READ of" : (er->ExceptionInformation[0] == 1 ? "WRITE to" : "EXECUTE at"), (DWORD)er->ExceptionInformation[1]);
#ifdef _M_IX86
            Out(f, "\r\nRegisters: EIP=%08X ESP=%08X EBP=%08X EFLAGS=%08X\r\n", c->Eip, c->Esp, c->Ebp, c->EFlags);
            Out(f, "           EAX=%08X EBX=%08X ECX=%08X EDX=%08X ESI=%08X EDI=%08X\r\n", c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi);
            DWORD words[96]; int n = 0;
            __try { for (; n < 96; n++) words[n] = ((DWORD*)(ULONG_PTR)c->Esp)[n]; } __except (EXCEPTION_EXECUTE_HANDLER) {}
            WriteStack(f, c->Ebp, c->Esp, words, n);
#endif
            WriteModules(f);
            CloseHandle(f);
        }
    }
    return g_prevFilter ? g_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;   // then whatever the game or Windows does next
}

// the game (or a library) may install its own filter later: put ours back every second and chain to theirs
static void KeepCrashFilter(void) {
    LPTOP_LEVEL_EXCEPTION_FILTER cur = SetUnhandledExceptionFilter(CrashFilter);
    if (cur && cur != CrashFilter) g_prevFilter = cur;
}

// ================================================================================================================
// Script built-ins for the bridge. The addresses, byte guards and calling conventions come from docs/ENGINE.md section
// "Script and dvar entry points" (derived from our own SP image; nothing from third-party tables).
//   * the engine resolves a built-in GSC function / method NAME at script compile time through two call sites;
//   * both call sites are patched here (one rel32 each) to wrappers that call the original lookup first and fall back to our table;
//   * what the engine then caches is a STUB in this proxy, so a bridge reload never leaves a dangling handler (see bridge/coopapi.h).
// Every patch is guarded by the expected original bytes; if anything differs nothing is patched and the log says why.
// ================================================================================================================
#include "../bridge/coopapi.h"

#define NSTUB 64            /* script built-ins the bridge may register (the engine caches these stubs; 24 ran out at the 25th built-in, then 48 ran out again at the 47th) */
#define ADDR_SITE_FUNC  0x00457A3Fu     /* CALL 0x4F8B60 inside the script compiler (function lookup) */
#define ADDR_SITE_METH  0x00457C5Bu     /* CALL 0x4F8CB0 (method lookup dispatcher, usercall ESI/EDI) */
#define ADDR_LOOKUP_FUNC 0x004F8B60u
#define ADDR_LOOKUP_METH 0x004F8CB0u

static CoopFunctionImpl g_fimpl[NSTUB];
static CoopMethodImpl g_mimpl[NSTUB];
static char g_fname[NSTUB][48], g_mname[NSTUB][48];
static int g_fcount = 0, g_mcount = 0;
static int g_hooksOk = 0;

#define STUBS(X) X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15) X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31) X(32) X(33) X(34) X(35) X(36) X(37) X(38) X(39) X(40) X(41) X(42) X(43) X(44) X(45) X(46) X(47) \
                       X(48) X(49) X(50) X(51) X(52) X(53) X(54) X(55) X(56) X(57) X(58) X(59) X(60) X(61) X(62) X(63)
#define DEF_FSTUB(n) static void __cdecl SF##n(void) { CoopFunctionImpl f = g_fimpl[n]; if (f) f(); }
#define DEF_MSTUB(n) static void __cdecl SM##n(unsigned self) { CoopMethodImpl m = g_mimpl[n]; if (m) m(self); }
#define REF_F(n) SF##n,
#define REF_M(n) SM##n,
STUBS(DEF_FSTUB)
STUBS(DEF_MSTUB)
static void* const g_fstub[NSTUB] = { STUBS(REF_F) };
static void* const g_mstub[NSTUB] = { STUBS(REF_M) };

static int __cdecl ApiRegisterFunction(const char* name, CoopFunctionImpl impl) {
    int i;
    for (i = 0; i < g_fcount; i++) if (!_stricmp(g_fname[i], name)) break;
    if (i == g_fcount) { if (g_fcount >= NSTUB) return 0; strncpy(g_fname[i], name, sizeof g_fname[i] - 1); g_fcount++; }
    g_fimpl[i] = impl;                    /* a name registered before keeps its stub: scripts compiled earlier still reach it */
    return 1;
}
static int __cdecl ApiRegisterMethod(const char* name, CoopMethodImpl impl) {
    int i;
    for (i = 0; i < g_mcount; i++) if (!_stricmp(g_mname[i], name)) break;
    if (i == g_mcount) { if (g_mcount >= NSTUB) return 0; strncpy(g_mname[i], name, sizeof g_mname[i] - 1); g_mcount++; }
    g_mimpl[i] = impl;
    return 1;
}
static void __cdecl ApiUnregisterAll(void) {
    memset(g_fimpl, 0, sizeof g_fimpl);   /* the stubs stay; with no implementation they do nothing (the script gets 'undefined') */
    memset(g_mimpl, 0, sizeof g_mimpl);
}
static CoopApi g_api = { 1, 0, ApiRegisterFunction, ApiRegisterMethod, ApiUnregisterAll };

/* lookup tails: called only when the engine's own lookup found nothing */
static void* OurFunction(const char** pName, int* pDev) {
    for (int i = 0; i < g_fcount; i++)
        if (!_stricmp(*pName, g_fname[i])) { *pName = g_fname[i]; *pDev = 0; return g_fstub[i]; }
    return 0;
}
static void* __cdecl OurMethod(const char** pName, int* pDev) {
    for (int i = 0; i < g_mcount; i++)
        if (!_stricmp(*pName, g_mname[i])) { *pName = g_mname[i]; *pDev = 0; return g_mstub[i]; }
    return 0;
}

/* function lookup: plain cdecl (const char** pName, int* pDevFlag) -> handler or 0 */
typedef void* (__cdecl *PFN_LOOKUP)(const char**, int*);
static void* __cdecl HookFunctionLookup(const char** pName, int* pDev) {
    void* r = ((PFN_LOOKUP)ADDR_LOOKUP_FUNC)(pName, pDev);
    return r ? r : OurFunction(pName, pDev);
}
/* method lookup: usercall, ESI = const char** name cell, EDI = int* devflag out, result in EAX. Call the original with the registers
   untouched, and only when it found nothing try our table (a cdecl C call: it preserves ESI/EDI). No other register is changed. */
static __declspec(naked) void HookMethodLookup(void) {
    __asm {
        mov eax, ADDR_LOOKUP_METH
        call eax
        test eax, eax
        jnz done
        push edi
        push esi
        call OurMethod
        add esp, 8
    done:
        ret
    }
}

static int PatchCallSite(DWORD site, const unsigned char* expect16, const unsigned char* expectEntry16, DWORD entry, DWORD hook, const char* what) {
    if (!Readable(site, 16) || !Readable(entry, 16)) { PLog("script hook %s: the code is not readable at the expected address, not patched", what); return 0; }
    if (memcmp((void*)(ULONG_PTR)site, expect16, 16) != 0) { PLog("script hook %s: unexpected bytes at the call site, not patched (a different game build?)", what); return 0; }
    if (memcmp((void*)(ULONG_PTR)entry, expectEntry16, 16) != 0) { PLog("script hook %s: unexpected bytes at the original lookup, not patched", what); return 0; }
    DWORD old = 0;
    if (!VirtualProtect((void*)(ULONG_PTR)(site + 1), 4, PAGE_EXECUTE_READWRITE, &old)) { PLog("script hook %s: VirtualProtect failed (error %lu), not patched", what, GetLastError()); return 0; }
    *(DWORD*)(ULONG_PTR)(site + 1) = hook - (site + 5);          /* the rel32 of the E8 call */
    VirtualProtect((void*)(ULONG_PTR)(site + 1), 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)(ULONG_PTR)site, 8);
    return 1;
}

static void InstallScriptHooks(void) {
    static const unsigned char siteF[16]  = { 0xE8,0x1C,0x11,0x0A,0x00,0x8B,0xF8,0x8D,0x43,0x01,0x8B,0xCD,0x8B,0xD6,0xE8,0x4E };
    static const unsigned char siteM[16]  = { 0xE8,0x50,0x10,0x0A,0x00,0x8B,0x15,0x04,0xD7,0x82,0x00,0x8B,0xF8,0x8D,0x43,0x01 };
    static const unsigned char entryF[16] = { 0x8B,0x44,0x24,0x04,0x8B,0x10,0x53,0x55,0x56,0x57,0x33,0xED,0x33,0xFF,0x8B,0xFF };
    static const unsigned char entryM[16] = { 0x56,0xC7,0x07,0x00,0x00,0x00,0x00,0xE8,0x34,0x82,0x04,0x00,0x83,0xC4,0x04,0x85 };
    if ((DWORD)(ULONG_PTR)GetModuleHandleA(NULL) != 0x00400000u) { PLog("script hooks: the game exe is not at its preferred base, not patched"); return; }
    /* both or none: a half-patched compiler would be worse than none */
    int f = Readable(ADDR_SITE_FUNC, 16) && !memcmp((void*)(ULONG_PTR)ADDR_SITE_FUNC, siteF, 16) && !memcmp((void*)(ULONG_PTR)ADDR_LOOKUP_FUNC, entryF, 16);
    int m = Readable(ADDR_SITE_METH, 16) && !memcmp((void*)(ULONG_PTR)ADDR_SITE_METH, siteM, 16) && !memcmp((void*)(ULONG_PTR)ADDR_LOOKUP_METH, entryM, 16);
    if (!f || !m) { PLog("script hooks: the expected original bytes are not there (function site %s, method site %s): nothing patched", f ? "ok" : "DIFFERENT", m ? "ok" : "DIFFERENT"); return; }
    if (!PatchCallSite(ADDR_SITE_FUNC, siteF, entryF, ADDR_LOOKUP_FUNC, (DWORD)(ULONG_PTR)HookFunctionLookup, "function")) return;
    if (!PatchCallSite(ADDR_SITE_METH, siteM, entryM, ADDR_LOOKUP_METH, (DWORD)(ULONG_PTR)HookMethodLookup, "method")) {
        /* the function site is already patched; it is harmless on its own (it only adds our names), so say so and go on */
        PLog("script hooks: the method site was not patched; function built-ins still work");
        g_api.hooksInstalled = 1; g_hooksOk = 1;
        return;
    }
    g_api.hooksInstalled = 1; g_hooksOk = 1;
    PLog("script hooks installed (function lookup call at 0x%08X, method lookup call at 0x%08X)", ADDR_SITE_FUNC, ADDR_SITE_METH);
}

// give the freshly loaded bridge copy the registration API (older bridges have no CoopInit: skipped)
static void InitBridge(void) {
    if (!g_bridge) return;
    CoopInitFn init = (CoopInitFn)GetProcAddress(g_bridge, "CoopInit");
    if (init) init(&g_api);
}

static HWND FindGameWindow(void) {
    HWND h = NULL; DWORD me = GetCurrentProcessId();
    while ((h = FindWindowExA(NULL, h, "CoD2", NULL)) != NULL) {
        DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        if (pid == me) return h;
    }
    return NULL;
}

// Hang report: the game window's thread must answer a WM_NULL; no answer for ~8 s writes ONE report per hang. The UI thread is
// suspended only long enough to read its registers and copy 384 bytes of its stack, then resumed BEFORE anything is formatted
// (formatting may take locks the suspended thread could hold).
static void HangWatch(HWND* hwnd, DWORD* since, int* reported) {
    if (!*hwnd || !IsWindow(*hwnd)) { *hwnd = FindGameWindow(); *since = 0; return; }
    DWORD_PTR res = 0;
    if (SendMessageTimeoutA(*hwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &res)) { *since = 0; *reported = 0; return; }
    DWORD now = GetTickCount();
    if (!*since) *since = now;                       // the first poll without an answer starts the clock
    DWORD secs = (now - *since) / 1000;
    if (secs < 8 || *reported) return;
    *reported = 1;
    DWORD tid = GetWindowThreadProcessId(*hwnd, NULL);
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    CONTEXT c; DWORD words[96]; int n = 0, haveCtx = 0;
    memset(&c, 0, sizeof c);
    if (th) {
        if (SuspendThread(th) != (DWORD)-1) {
            c.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(th, &c)) {
                haveCtx = 1;
#ifdef _M_IX86
                __try { for (; n < 96; n++) words[n] = ((DWORD*)(ULONG_PTR)c.Esp)[n]; } __except (EXCEPTION_EXECUTE_HANDLER) {}
#endif
            }
            ResumeThread(th);
        }
        CloseHandle(th);
    }
    HANDLE f = ReportOpen("hang");
    if (f == INVALID_HANDLE_VALUE) return;
    Out(f, "CoD2 co-op hang report\r\nThe UI thread of the game window did not answer for about %u seconds.\r\n", secs);
    Out(f, "(A long level load can look like this too; one report is written per hang.)\r\n");
#ifdef _M_IX86
    if (haveCtx) {
        char w[160]; Where(c.Eip, w);
        Out(f, "\r\nUI thread (id %u) stopped at %s\r\n", tid, w);
        Out(f, "Registers: EIP=%08X ESP=%08X EBP=%08X\r\n", c.Eip, c.Esp, c.Ebp);
        WriteStack(f, c.Ebp, c.Esp, words, n);
    } else Out(f, "\r\n(the registers of the UI thread could not be read)\r\n");
#endif
    WriteModules(f);
    CloseHandle(f);
    PLog("a hang report was written (coop_hang_*.txt)");
}

static DWORD WINAPI Watcher(LPVOID self) {
    if (!IsSinglePlayerExe()) return 0;
    char me[MAX_PATH];
    DWORD n = GetModuleFileNameA((HMODULE)self, me, MAX_PATH);   // this proxy's own path -> the folder
    if (!n || n >= MAX_PATH) return 0;
    char* sl = strrchr(me, '\\');
    if (!sl) return 0;
    sl[1] = 0;
    strcpy(g_dir, me);

    KeepCrashFilter();
    InstallScriptHooks();            // before any mission script is compiled: the lookups run at compile time
    HWND hwnd = NULL; DWORD stuckSince = 0; int reported = 0;

    FILETIME last; DWORD lastSize = 0;
    int haveBridge = StatSrc(&last, &lastSize);            // not installed next to us: crash/hang reports only
    if (haveBridge) {
        RemoveStaleCopies();
        if (LoadLive()) { PLog("bridge loaded as a live copy (reload on change: replace cod2net5.dll while the game runs)"); InitBridge(); }
        else haveBridge = 0;
    }

    for (;;) {
        Sleep(1000);
        KeepCrashFilter();
        HangWatch(&hwnd, &stuckSince, &reported);
        if (!haveBridge) continue;
        FILETIME ft; DWORD sz;
        if (!StatSrc(&ft, &sz)) continue;
        if (!CompareFileTime(&ft, &last) && sz == lastSize) continue;
        // changed: wait until it stops changing (a copy in progress), then reload
        Sleep(1500);
        FILETIME ft2; DWORD sz2;
        if (!StatSrc(&ft2, &sz2) || CompareFileTime(&ft2, &ft) || sz2 != sz) continue;
        last = ft2; lastSize = sz2;
        PLog("cod2net5.dll changed: reloading the bridge");
        if (!UnloadLive()) continue;                      // old copy stays; the next change tries again
        if (LoadLive()) { PLog("bridge reloaded"); InitBridge(); }
        else PLog("the new bridge could not be loaded: no bridge is running now, restart the game");
    }
}

BOOL APIENTRY DllMain(HMODULE m, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(m);
        HANDLE t = CreateThread(NULL, 0, Watcher, m, 0, NULL);   // never from DllMain itself: the loader lock is held here
        if (t) CloseHandle(t);
    }
    return TRUE;
}

void* WINAPI Direct3DCreate9(UINT sdkVersion) {
    if (!g_create) {
        char sys[MAX_PATH];
        UINT n = GetSystemDirectoryA(sys, MAX_PATH);                  // SysWOW64 for a 32-bit process on 64-bit Windows
        if (!n || n + 12 >= MAX_PATH) return NULL;
        strcat(sys, "\\d3d9.dll");
        HMODULE real = LoadLibraryA(sys);
        if (!real) return NULL;
        g_create = (PFN_CREATE)GetProcAddress(real, "Direct3DCreate9");
        if (!g_create) return NULL;
    }
    return g_create(sdkVersion);
}
