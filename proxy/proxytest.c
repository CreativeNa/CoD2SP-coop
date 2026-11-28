// proxytest.c - checks the d3d9 proxy without the game. Run from a folder that holds d3d9.dll (the proxy) and
// cod2net5.dll; the exe's own name decides whether the proxy acts at all (CoD2SP_s.exe yes, anything else no).
//
//   cl /nologo /MT /O2 proxytest.c /Fe:proxytest.exe
//   proxytest.exe [wait_seconds]   load the proxy, call Direct3DCreate9(32), wait, list the cod2net5 modules loaded
//   proxytest.exe crash            ... then raise a deliberate access violation (the proxy's crash report must appear)
//   proxytest.exe hang             ... then create a window of class "CoD2" and stop answering its messages for ~14 s
//                                  (the proxy's hang report must appear)
// It loads ".\d3d9.dll" by full path (so it is the proxy, not the system one).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void* (WINAPI *PFN)(UINT);

static void Pump(int ms) {
    DWORD t0 = GetTickCount();
    MSG m;
    while (GetTickCount() - t0 < (DWORD)ms) {
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
        Sleep(10);
    }
}

int main(int argc, char** argv) {
    char dir[MAX_PATH];
    GetModuleFileNameA(NULL, dir, MAX_PATH);
    char exe[MAX_PATH]; strcpy(exe, dir);
    char* sl = strrchr(dir, '\\'); if (sl) *sl = 0;
    char proxy[MAX_PATH]; _snprintf(proxy, sizeof proxy, "%s\\d3d9.dll", dir);
    const char* mode = argc > 1 ? argv[1] : "";
    int crash = !strcmp(mode, "crash"), hang = !strcmp(mode, "hang");
    int waitMs = (argc > 1 && !crash && !hang) ? atoi(argv[1]) * 1000 : 2500;

    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);   // no Windows error dialog for the deliberate crash
    HMODULE m = LoadLibraryA(proxy);
    if (!m) { printf("FAIL: cannot load the proxy %s (error %lu)\n", proxy, GetLastError()); return 1; }
    PFN create = (PFN)GetProcAddress(m, "Direct3DCreate9");
    if (!create) { printf("FAIL: Direct3DCreate9 is not exported by the proxy\n"); return 1; }
    void* d3d = create(32);
    printf("Direct3DCreate9(32) -> %s\n", d3d ? "non-null (forwarded to the real d3d9)" : "NULL");
    fflush(stdout);

    if (hang) {
        WNDCLASSA wc; memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "CoD2";
        RegisterClassA(&wc);
        HWND w = CreateWindowA("CoD2", "test", WS_POPUP, 0, 0, 10, 10, NULL, NULL, wc.hInstance, NULL);
        printf("window %s\n", w ? "created" : "FAILED"); fflush(stdout);
        Pump(3000);                       // answers messages: the watchdog must stay quiet
        printf("hanging now\n"); fflush(stdout);
        Sleep(14000);                     // the UI thread stops answering: a hang report must be written
        Pump(1500);
        printf("hang over\n");
        return 0;
    }
    Sleep(crash ? 2500 : waitMs);
    if (crash) { volatile int* p = 0; *p = 1; return 0; }   // deliberate access violation

    int count = 0; char names[512] = "";
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32 me; me.dwSize = sizeof me;
        for (BOOL ok = Module32First(snap, &me); ok; ok = Module32Next(snap, &me))
            if (!_strnicmp(me.szModule, "cod2net5", 8)) { count++; if (strlen(names) + strlen(me.szModule) + 3 < sizeof names) { if (names[0]) strcat(names, ", "); strcat(names, me.szModule); } }
        CloseHandle(snap);
    }
    const char* base = strrchr(exe, '\\'); base = base ? base + 1 : exe;
    printf("exe name: %s | cod2net5.dll loaded into this process: %s\n", base, count ? "YES" : "no");
    printf("bridge modules: %d [%s]\n", count, names);
    return d3d ? 0 : 1;
}
