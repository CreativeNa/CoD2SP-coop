/* coopapi.h - the small interface between the d3d9 proxy (permanent) and the bridge (reloadable).
 *
 * Why it exists: the engine looks up a built-in GSC function by name when a script is COMPILED and caches the handler pointer in its own
 * runtime table. If that pointer pointed into the bridge, a live reload (the old bridge DLL is freed) would leave compiled scripts
 * calling freed memory. So the pointers the engine caches are STUBS that live in the proxy and never go away; the stub forwards to
 * whatever implementation the bridge has registered at that moment (a no-op while none is).
 *
 * The proxy calls the bridge's exported CoopInit(&api) right after loading a bridge copy; the bridge registers its built-ins there and
 * calls api->UnregisterAll() from CoopStop before it is freed.
 */
#ifndef COOPAPI_H
#define COOPAPI_H

typedef void (__cdecl *CoopFunctionImpl)(void);            /* a GSC function: void f(void), params and results go through the script stack */
typedef void (__cdecl *CoopMethodImpl)(unsigned selfRef);  /* a GSC method:   void m(unsigned selfRef) */

typedef struct CoopApi {
    int version;                                           /* 1 */
    int hooksInstalled;                                    /* 1 when the proxy patched both lookup call sites; 0: registrations are accepted but no script can see them */
    int  (__cdecl *RegisterFunction)(const char* name, CoopFunctionImpl impl);   /* 1 = ok, 0 = table full */
    int  (__cdecl *RegisterMethod)(const char* name, CoopMethodImpl impl);
    void (__cdecl *UnregisterAll)(void);
} CoopApi;

typedef void (__cdecl *CoopInitFn)(const CoopApi* api);    /* exported by the bridge as "CoopInit" */

#endif
