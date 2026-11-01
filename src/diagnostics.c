/* Diagnostic output and its five-second worker. The worker never drives the UI.
 * A critical section is a Windows lock: it stops log lines/ring updates from
 * interleaving across threads. It is not a lock for window/input state. */
#include "shim_internal.h"
#include <stdarg.h>

static FILE *diagnostic_file;
static CRITICAL_SECTION log_guard;

void diagnostics_initialize(void)
{
    InitializeCriticalSection(&log_guard);
}
void diagnostics_open(const char *path)
{
    diagnostic_file = fopen(path, "w");
}
void diagnostics_lock(void)
{
    EnterCriticalSection(&log_guard);
}
void diagnostics_unlock(void)
{
    LeaveCriticalSection(&log_guard);
}

void diagnostics_log(const char *format, ...)
{
    va_list arguments;
    if (!diagnostic_file)
        return;
    /* One complete line is a unit: without the lock, the worker and UI thread
     * could mix their output together. Flush makes recent crash diagnostics useful. */
    diagnostics_lock();
    fprintf(diagnostic_file, "%8lu ", GetTickCount());
    va_start(arguments, format);
    vfprintf(diagnostic_file, format, arguments);
    va_end(arguments);
    fputc('\n', diagnostic_file);
    fflush(diagnostic_file);
    diagnostics_unlock();
}

void diagnostics_write_summary(void)
{
    if (!diagnostic_file)
        return;
    diagnostics_lock();
    /* Hold the same lock across the whole report. Window geometry is sampled,
     * not a synchronized snapshot; the worker is reporting, not controlling UI. */
    display_write_diagnostics(diagnostic_file);
    fprintf(diagnostic_file, "---- call counts ----\n");
    hooks_write_counts(diagnostic_file);
    fflush(diagnostic_file);
    diagnostics_unlock();
}

static DWORD WINAPI summary_thread(LPVOID unusedContext)
{
    (void)unusedContext;
    for (;;) {
        Sleep(5000);
        diagnostics_write_summary();
    }
    return 0;
}

void diagnostics_start_worker(void)
{
    /* This preserves the original process-lifetime worker. There is no stop/join
     * protocol: unloading the DLL while this thread can call it is unsupported. */
    CreateThread(NULL, 0, summary_thread, NULL, 0, NULL);
}
