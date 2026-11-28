/* bridge_threads.c - see bridge_threads.h. No dependency on the rest of the bridge: the logger is passed in. */
#include "bridge_threads.h"

#define THREADS_MAX 16

static HANDLE g_threads[THREADS_MAX];
static int g_count = 0;
static CRITICAL_SECTION g_lock;
static int g_lockReady = 0;

static void Lock(void) {
    if (!g_lockReady) {                       /* first call races are avoided by the caller's order: DllMain starts the first thread */
        InitializeCriticalSection(&g_lock);
        g_lockReady = 1;
    }
    EnterCriticalSection(&g_lock);
}
static void Unlock(void) { LeaveCriticalSection(&g_lock); }

int ThreadsStart(LPTHREAD_START_ROUTINE fn, const char* name, void (*logf)(const char*, ...)) {
    Lock();
    if (g_count >= THREADS_MAX) {
        Unlock();
        if (logf) logf("thread %s NOT started: the thread table is full (%d)", name, THREADS_MAX);
        return 0;
    }
    HANDLE h = CreateThread(0, 0, fn, 0, 0, 0);
    if (!h) {
        Unlock();
        if (logf) logf("thread %s failed to start (error %lu)", name, GetLastError());
        return 0;
    }
    g_threads[g_count++] = h;
    Unlock();
    return 1;
}

int ThreadsWaitAll(DWORD timeoutMs) {
    Lock();
    int n = g_count;
    HANDLE local[THREADS_MAX];
    for (int i = 0; i < n; i++) local[i] = g_threads[i];
    Unlock();
    if (n == 0) return 1;
    DWORD w = WaitForMultipleObjects((DWORD)n, local, TRUE, timeoutMs);
    if (w == WAIT_TIMEOUT) return 0;
    Lock();
    for (int i = 0; i < n; i++) { CloseHandle(local[i]); if (i < g_count) g_threads[i] = NULL; }
    g_count = 0;
    Unlock();
    return 1;
}

int ThreadsCount(void) {
    Lock();
    int n = g_count;
    Unlock();
    return n;
}
