/* bridge_threads.h - thread lifetime for the bridge, separated from the 3.6k-line monolith.
 *
 * Why this module exists: a thread that is not registered for shutdown is not waited for by CoopStop, and the proxy
 * frees the DLL as soon as CoopStop returns 1 - the thread then executes freed code. That was the crash of
 * 2026-11-25. Keeping the table and the wait in one small module makes the rule hard to forget.
 */
#ifndef BRIDGE_THREADS_H
#define BRIDGE_THREADS_H

#include <windows.h>

/* Starts a thread and registers it. Returns 1 on success; logs through the given function when the table is full or
   CreateThread fails. Registering from several threads is safe (the table is locked). */
int ThreadsStart(LPTHREAD_START_ROUTINE fn, const char* name, void (*logf)(const char*, ...));

/* Waits for every registered thread, closes the handles and clears the table. Returns 1 when all of them are gone,
   0 on timeout (the caller must then NOT free the DLL). */
int ThreadsWaitAll(DWORD timeoutMs);

/* How many threads are registered right now (for logs and tests). */
int ThreadsCount(void);

#endif
