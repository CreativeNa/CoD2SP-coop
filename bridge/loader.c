// loader.c - game-agnostic x86/x64 LoadLibrary loader for a mod DLL.
//
// It knows nothing about any game: you give it a process name and a DLL path. That is the whole
// contract, which is why it can live outside a game repo.
//
// Usage:
//   loader.exe --process CoD2SP_s.exe --dll C:\path\to\mod.dll [--wait] [--timeout 30]
//
//   --wait      poll until the process appears (default: fail immediately)
//   --timeout   seconds to wait for the process (default 60)
//
// Exit codes: 0 injected, 1 load failed inside the target, 2 bad arguments, 3 process not found,
//             4 OpenProcess failed, 5 allocation failed, 6 thread creation failed,
//             7 architecture mismatch (32-bit loader vs 64-bit target or the reverse).
//
// Match the build to the target: build x86 for a 32-bit game, x64 for a 64-bit one. The loader checks
// this before injecting, because a wrong-bitness LoadLibrary call silently fails or crashes the target.
//
// Build (x86 target):
//   cl /nologo /MT /O2 loader.c /Fe:loader32.exe
// Build (x64 target), from an x64 developer prompt:
//   cl /nologo /MT /O2 loader.c /Fe:loader64.exe
//
// This tool is for your own mods on your own machine. It does not bypass anti-cheat and is not obfuscated.

#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void usage(void) {
    printf("usage: loader.exe --process <name.exe> --dll <path> [--wait] [--timeout N]\n");
}

static DWORD find_pid(const char *exe) {
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof pe;
    for (BOOL ok = Process32First(snap, &pe); ok; ok = Process32Next(snap, &pe)) {
        if (!_stricmp(pe.szExeFile, exe)) { pid = pe.th32ProcessID; break; }
    }
    CloseHandle(snap);
    return pid;
}

// Returns 1 = target is 32-bit, 0 = 64-bit, -1 = unknown.
static int target_is_32(DWORD pid) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return -1;
    BOOL wow64 = FALSE;
    int r = -1;
    if (IsWow64Process(p, &wow64)) {
#ifdef _WIN64
        r = wow64 ? 1 : 0;      // x64 loader: 32-bit target runs under WOW64
#else
        (void)wow64;
        r = 1;                  // x86 loader can only see 32-bit targets
#endif
    }
    CloseHandle(p);
    return r;
}

int main(int argc, char **argv) {
    const char *exe = NULL, *dllArg = NULL;
    int wait = 0, timeout = 60;

    for (int i = 1; i < argc; i++) {
        if (!_stricmp(argv[i], "--process") && i + 1 < argc) exe = argv[++i];
        else if (!_stricmp(argv[i], "--dll") && i + 1 < argc) dllArg = argv[++i];
        else if (!_stricmp(argv[i], "--wait")) wait = 1;
        else if (!_stricmp(argv[i], "--timeout") && i + 1 < argc) timeout = atoi(argv[++i]);
        else { usage(); return 2; }
    }
    if (!exe || !dllArg) { usage(); return 2; }

    char dll[MAX_PATH];
    if (!GetFullPathNameA(dllArg, sizeof dll, dll, NULL)) { printf("bad dll path\n"); return 2; }

    DWORD pid = 0;
    int waited = 0;
    for (;;) {
        pid = find_pid(exe);
        if (pid) break;
        if (!wait || waited >= timeout) { printf("%s not found\n", exe); return 3; }
        Sleep(500);
        waited++;
    }

    int bits = target_is_32(pid);
#ifdef _WIN64
    if (bits == 1) { printf("architecture mismatch: 64-bit loader, 32-bit target\n"); return 7; }
#else
    if (bits == 0) { printf("architecture mismatch: 32-bit loader, 64-bit target\n"); return 7; }
#endif

    printf("target %s pid=%lu (%s)\n", exe, pid, bits == 1 ? "32-bit" : "64-bit");

    HANDLE p = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                           PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!p) { printf("OpenProcess failed: %lu\n", GetLastError()); return 4; }

    size_t n = strlen(dll) + 1;
    void *mem = VirtualAllocEx(p, NULL, n, MEM_COMMIT, PAGE_READWRITE);
    if (!mem) { printf("VirtualAllocEx failed: %lu\n", GetLastError()); CloseHandle(p); return 5; }
    WriteProcessMemory(p, mem, dll, n, NULL);

    FARPROC ll = GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    HANDLE t = CreateRemoteThread(p, NULL, 0, (LPTHREAD_START_ROUTINE)ll, mem, 0, NULL);
    if (!t) { printf("CreateRemoteThread failed: %lu\n", GetLastError()); return 6; }
    WaitForSingleObject(t, 15000);
    DWORD mod = 0;
    GetExitCodeThread(t, &mod);
    CloseHandle(t);
    VirtualFreeEx(p, mem, 0, MEM_RELEASE);
    CloseHandle(p);

    printf("dll %s -> module %s\n", dll, mod ? "loaded" : "LOAD FAILED");
    return mod ? 0 : 1;
}
