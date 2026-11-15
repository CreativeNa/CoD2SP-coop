// cod2mem.c - read-only inspector for a running 32-bit CoD2 image.
//
// Why: the on-disk exe is packed (Steam CEG), so the only readable truth is the module
// already mapped in the game process. This tool never writes, never injects, never
// allocates in the target: OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ) and
// ReadProcessMemory only.
//
// What it answers:
//   1. module base, SizeOfImage, entry point, section table  -> enables a later image dump
//   2. import directory DLL names                            -> does the SP exe itself use
//                                                               ws2_32 / dinput8 / ddraw?
//   3. printable string census of the loaded image            -> does the engine still carry
//                                                               SV_/CL_/NET_/svc_/clc_ code?
//
// Build (x86):
//   cl /nologo /MT /O2 cod2mem.c /Fe:cod2mem.exe psapi.lib

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#pragma comment(lib, "psapi.lib")

static int g_hits = 0;

static void die(const char* what) {
    printf("FAIL %s (err=%lu)\n", what, GetLastError());
    exit(1);
}

static unsigned char* read_remote(HANDLE hp, ULONGLONG base, SIZE_T size) {
    unsigned char* buf = (unsigned char*)malloc(size);
    if (!buf) die("malloc");
    SIZE_T got = 0;
    if (!ReadProcessMemory(hp, (LPCVOID)(ULONG_PTR)base, buf, size, &got)) {
        printf("ReadProcessMemory failed at 0x%08llX size=0x%zX err=%lu got=%zu\n",
               base, size, GetLastError(), got);
        free(buf);
        return NULL;
    }
    return buf;
}

// Find the module matching exe_name; returns base and size.
static int find_module(DWORD pid, const char* exe_name, ULONGLONG* base, DWORD* size) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32 me;
    me.dwSize = sizeof(me);
    int found = 0;
    if (Module32First(snap, &me)) {
        do {
            if (_stricmp(me.szModule, exe_name) == 0) {
                *base = (ULONGLONG)(ULONG_PTR)me.modBaseAddr;
                *size = me.modBaseSize;
                found = 1;
                break;
            }
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

static void census(HANDLE hp, ULONGLONG base, SIZE_T size, const char* needle, int max_show) {
    const SIZE_T CH = 1 << 20, OVERLAP = 64;
    unsigned char* buf = (unsigned char*)malloc(CH + OVERLAP);
    if (!buf) die("malloc chunk");
    SIZE_T nlen = strlen(needle);
    int count = 0, shown = 0;
    SIZE_T tail = 0;
    for (SIZE_T off = 0; off < size; off += CH) {
        SIZE_T want = (size - off > CH) ? CH : (size - off);
        SIZE_T got = 0;
        SIZE_T chunk_start = off - tail; // absolute offset of buf[0]
        if (!ReadProcessMemory(hp, (LPCVOID)(ULONG_PTR)(base + off), buf + tail, want, &got)) {
            tail = 0;
            continue;
        }
        SIZE_T total = tail + got;
        // count printable runs containing the needle
        for (SIZE_T i = 0; i + nlen <= total; i++) {
            if (buf[i] < 0x20 || buf[i] > 0x7E) continue;
            if (_memicmp(buf + i, needle, nlen) != 0) continue;
            // expand to the run start
            SIZE_T s = i;
            while (s > 0 && buf[s - 1] >= 0x20 && buf[s - 1] <= 0x7E) s--;
            SIZE_T e = i;
            while (e < total && buf[e] >= 0x20 && buf[e] <= 0x7E) e++;
            if (e - s >= 3) {
                count++;
                if (shown < max_show && e - s < 200) {
                    char tmp[201];
                    SIZE_T l = e - s; if (l > 200) l = 200;
                    memcpy(tmp, buf + s, l); tmp[l] = 0;
                    DWORD rva = (DWORD)(chunk_start + s);
                    printf("    [0x%08lX] %s\n", (unsigned long)rva, tmp);
                    shown++;
                }
            }
            i = e;
        }
        tail = (total >= OVERLAP) ? OVERLAP : total;
        memmove(buf, buf + total - tail, tail);
    }
    free(buf);
    printf("  %-10s count=%d\n", needle, count);
    g_hits += count;
}

// ---- cvar / command name census -------------------------------------------------
// The engine keeps cvar names as plain strings in the image, so the loaded module shows
// which server-side cvars survived the singleplayer build (sv_maxclients, sv_fps, ...).
static char* g_list[20000];
static int g_n = 0;

static void add_unique(const char* s, int len) {
    if (len <= 0 || len > 64) return;
    for (int i = 0; i < g_n; i++) {
        if ((int)strlen(g_list[i]) == len && memcmp(g_list[i], s, len) == 0) return;
    }
    if (g_n >= 20000) return;
    char* p = (char*)malloc(len + 1);
    if (!p) return;
    memcpy(p, s, len);
    p[len] = 0;
    g_list[g_n++] = p;
}

static int cmpstr(const void* a, const void* b) {
    return _stricmp(*(const char**)a, *(const char**)b);
}

static void scan_cvars(HANDLE hp, ULONGLONG base, SIZE_T size) {
    static const char* prefixes[] = { "sv_", "net_", "cl_", "com_", "fs_", "cg_", NULL };
    const SIZE_T CH = 1 << 20, OVERLAP = 96;
    unsigned char* buf = (unsigned char*)malloc(CH + OVERLAP);
    if (!buf) return;
    SIZE_T tail = 0;
    for (SIZE_T off = 0; off < size; off += CH) {
        SIZE_T want = (size - off > CH) ? CH : (size - off);
        SIZE_T got = 0;
        if (!ReadProcessMemory(hp, (LPCVOID)(ULONG_PTR)(base + off), buf + tail, want, &got)) {
            tail = 0;
            continue;
        }
        SIZE_T total = tail + got;
        SIZE_T i = 0;
        while (i < total) {
            if (buf[i] < 0x20 || buf[i] > 0x7E) { i++; continue; }
            SIZE_T s = i;
            while (i < total && buf[i] >= 0x20 && buf[i] <= 0x7E) i++;
            SIZE_T len = i - s;
            if (len >= 3 && len <= 64) {
                for (int k = 0; prefixes[k]; k++) {
                    SIZE_T pl = strlen(prefixes[k]);
                    if (len > pl && _memicmp(buf + s, prefixes[k], pl) == 0) {
                        add_unique((const char*)(buf + s), (int)len);
                        break;
                    }
                }
            }
        }
        tail = (total >= OVERLAP) ? OVERLAP : total;
        memmove(buf, buf + total - tail, tail);
    }
    free(buf);
    qsort(g_list, g_n, sizeof(char*), cmpstr);
    printf("--- cvar/command census (%d unique names) ---\n", g_n);
    for (int i = 0; i < g_n; i++) printf("  %s\n", g_list[i]);
}

int main(int argc, char** argv) {
    DWORD pid = 0;
    if (argc > 1) pid = (DWORD)atoi(argv[1]);
    if (!pid) {
        printf("usage: cod2mem.exe <pid>\n");
        return 2;
    }
    printf("=== cod2mem - read-only image inspector ===\n");
    printf("built %s %s\n", __DATE__, __TIME__);
    printf("pid=%lu\n", (unsigned long)pid);

    HANDLE hp = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hp) die("OpenProcess");

    ULONGLONG base = 0;
    DWORD msize = 0;
    if (!find_module(pid, "CoD2SP_s.exe", &base, &msize)) die("find_module");
    printf("module base=0x%08llX  modBaseSize=0x%lX (%lu MB)\n", base, msize, msize / (1024 * 1024));

    unsigned char hdr[0x1000];
    SIZE_T got = 0;
    if (!ReadProcessMemory(hp, (LPCVOID)(ULONG_PTR)base, hdr, sizeof(hdr), &got)) die("read headers");
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)hdr;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { printf("no MZ\n"); return 1; }
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(hdr + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { printf("no PE\n"); return 1; }
    printf("machine=0x%04X  Sections=%u  SizeOfImage=0x%lX  EntryPoint=0x%08lX  TimeDateStamp=0x%08lX\n",
           nt->FileHeader.Machine, nt->FileHeader.NumberOfSections,
           nt->OptionalHeader.SizeOfImage, nt->OptionalHeader.AddressOfEntryPoint,
           nt->FileHeader.TimeDateStamp);

    // ---- image dump: cod2mem.exe <pid> --dump <file> ----
    // Writes the mapped image (headers + every section at its real RVA, file offset == RVA, so a
    // raw Ghidra import at the module base lines up). Read-only on the target; unreadable pages
    // become zeros and are counted.
    if (argc > 3 && strcmp(argv[2], "--dump") == 0) {
        DWORD soi = nt->OptionalHeader.SizeOfImage;
        FILE* f = fopen(argv[3], "wb");
        if (!f) die("fopen dump");
        unsigned char* page = (unsigned char*)malloc(0x1000);
        unsigned char zero[0x1000] = { 0 };
        DWORD bad = 0;
        for (DWORD off = 0; off < soi; off += 0x1000) {
            SIZE_T g2 = 0;
            if (ReadProcessMemory(hp, (LPCVOID)(ULONG_PTR)(base + off), page, 0x1000, &g2) && g2 == 0x1000)
                fwrite(page, 1, 0x1000, f);
            else { fwrite(zero, 1, 0x1000, f); bad++; }
        }
        fclose(f);
        printf("dumped SizeOfImage=0x%lX base=0x%08llX unreadable_pages=%lu -> %s\n",
               (unsigned long)soi, base, (unsigned long)bad, argv[3]);
        CloseHandle(hp);
        return 0;
    }

    // ---- imports ----
    DWORD impRva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    printf("--- imports (rva 0x%lX) ---\n", (unsigned long)impRva);
    if (impRva) {
        int i = 0;
        for (;;) {
            IMAGE_IMPORT_DESCRIPTOR d;
            if (!ReadProcessMemory(hp, (LPCVOID)(ULONG_PTR)(base + impRva + i * sizeof(d)), &d, sizeof(d), &got)) break;
            if (!d.Name) break;
            char name[256] = { 0 };
            ReadProcessMemory(hp, (LPCVOID)(ULONG_PTR)(base + d.Name), name, sizeof(name) - 1, &got);
            // count thunks
            int fns = 0;
            for (;;) {
                DWORD thunk = 0;
                if (!ReadProcessMemory(hp, (LPCVOID)(ULONG_PTR)(base + d.FirstThunk + fns * 4), &thunk, 4, &got)) break;
                if (!thunk) break;
                fns++;
            }
            printf("  %-20s %3d functions\n", name, fns);
            // function names (helps decide whether Winsock is real transport or just WSAStartup)
            for (int k = 0; k < fns && k < 8; k++) {
                DWORD thunk = 0;
                if (!ReadProcessMemory(hp, (LPCVOID)(ULONG_PTR)(base + d.FirstThunk + k * 4), &thunk, 4, &got)) break;
                if (!thunk) break;
                if (thunk & 0x80000000) { printf("      #%lu (ordinal)\n", thunk & 0xFFFF); continue; }
                // The Steam CEG loader rewrites the thunk array with absolute VAs; accept both forms.
                ULONGLONG rva = thunk;
                if (rva >= base && rva < base + nt->OptionalHeader.SizeOfImage) rva -= base;
                char fn[128] = { 0 };
                ReadProcessMemory(hp, (LPCVOID)(ULONG_PTR)(base + rva + 2), fn, sizeof(fn) - 1, &got);
                printf("      %s\n", fn);
            }
            i++;
            if (i > 200) break;
        }
    }

    // ---- sections ----
    printf("--- sections ---\n");
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        char nm[9] = { 0 };
        memcpy(nm, sec[i].Name, 8);
        printf("  %-9s VA=0x%08lX VSize=0x%08lX RawSize=0x%08lX\n",
               nm, sec[i].VirtualAddress, sec[i].Misc.VirtualSize, sec[i].SizeOfRawData);
    }

    printf("--- engine symbol census in the loaded image ---\n");
    census(hp, base, nt->OptionalHeader.SizeOfImage, "SV_", 4);
    census(hp, base, nt->OptionalHeader.SizeOfImage, "CL_", 4);
    census(hp, base, nt->OptionalHeader.SizeOfImage, "NET_", 4);
    census(hp, base, nt->OptionalHeader.SizeOfImage, "SVC_", 4);
    census(hp, base, nt->OptionalHeader.SizeOfImage, "CLC_", 4);
    census(hp, base, nt->OptionalHeader.SizeOfImage, "snapshot", 4);
    census(hp, base, nt->OptionalHeader.SizeOfImage, "netchan", 4);
    census(hp, base, nt->OptionalHeader.SizeOfImage, "sv_running", 4);
    census(hp, base, nt->OptionalHeader.SizeOfImage, "level.player", 4);
    census(hp, base, nt->OptionalHeader.SizeOfImage, "script_model", 4);
    census(hp, base, nt->OptionalHeader.SizeOfImage, "dinput", 4);
    printf("total hits=%d\n", g_hits);

    // Extra needles from the command line: every string in the loaded image that contains them.
    for (int a = 2; a < argc; a++) {
        printf("--- needle '%s' ---\n", argv[a]);
        census(hp, base, nt->OptionalHeader.SizeOfImage, argv[a], 6);
    }

    scan_cvars(hp, base, nt->OptionalHeader.SizeOfImage);

    CloseHandle(hp);
    printf("=== done (read-only, nothing written to the target) ===\n");
    return 0;
}
