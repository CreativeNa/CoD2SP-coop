#include <windows.h>
#include <stdio.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

typedef HRESULT (WINAPI *PFN_DI8)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
typedef HRESULT (WINAPI *PFN_DIA)(HINSTANCE, DWORD, LPVOID*, LPUNKNOWN);

int main(void) {
    printf("--- DirectInput diagnosis ---\n");
    HINSTANCE hInst = GetModuleHandleA(NULL);
    printf("GetModuleHandle(NULL)=%p\n", hInst);
    HMODULE d8 = LoadLibraryA("dinput8.dll");
    printf("LoadLibrary dinput8=%p err=%lu\n", d8, GetLastError());
    if (d8) {
        PFN_DI8 f = (PFN_DI8)GetProcAddress(d8, "DirectInput8Create");
        printf("DirectInput8Create=%p\n", f);
        if (f) {
            IDirectInput8A* p = NULL;
            HRESULT hr = f(hInst, 0x0800, &IID_IDirectInput8A, (void**)&p, NULL);
            printf("  hInst=module     -> 0x%08lX ptr=%p\n", (unsigned long)hr, p);
            if (p) p->Release();
            p = NULL;
            hr = f((HINSTANCE)0x00400000, 0x0800, &IID_IDirectInput8A, (void**)&p, NULL);
            printf("  hInst=0x400000   -> 0x%08lX ptr=%p\n", (unsigned long)hr, p);
            if (p) p->Release();
        }
    }
    HMODULE d7 = LoadLibraryA("dinput.dll");
    printf("LoadLibrary dinput.dll=%p\n", d7);
    if (d7) {
        PFN_DIA g = (PFN_DIA)GetProcAddress(d7, "DirectInputCreateA");
        if (g) {
            IDirectInputA* p = NULL;
            HRESULT hr = g(hInst, 0x0700, &IID_IDirectInputA, (void**)&p, NULL);
            printf("  DirectInputCreateA(7.0) -> 0x%08lX ptr=%p\n", (unsigned long)hr, p);
            if (p) p->Release();
        }
    }
    printf("--- CoInitializeEx ---\n");
    HRESULT c = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    printf("CoInitializeEx=0x%08lX\n", (unsigned long)c);
    return 0;
}
