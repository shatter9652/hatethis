#include "debug_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <signal.h>
#include <stdarg.h>
#include <string.h>

static char g_logPath[1024];
static int g_installed = 0;

const char* BSC_debugLogPath(void) {
    if (g_logPath[0] != '\0') return g_logPath;
    const char* env = getenv("BUTTERSCOTCH_DEBUG_LOG");
    if (env != NULL && env[0] != '\0') {
        snprintf(g_logPath, sizeof(g_logPath), "%s", env);
    } else {
        snprintf(g_logPath, sizeof(g_logPath), "butterscotch_debug.log");
    }
    return g_logPath;
}

static void stamp(FILE* f, const char* category) {
    time_t t = time(NULL);
    struct tm tmv;
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] [%s] ",
        tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
        tmv.tm_hour, tmv.tm_min, tmv.tm_sec, category ? category : "log");
}

void BSC_debugLogV(const char* category, const char* fmt, va_list args) {
    const char* path = BSC_debugLogPath();
    FILE* f = fopen(path, "ab");
    if (!f) return;
    stamp(f, category);
    vfprintf(f, fmt, args);
    fputc('\n', f);
    fclose(f);
}

void BSC_debugLog(const char* category, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    BSC_debugLogV(category, fmt, args);
    va_end(args);
}

static void onSignal(int sig) {
    BSC_debugLog("fatal", "process received signal %d; last terminal output may be incomplete. Check audio/path/vm lines above.", sig);
    signal(sig, SIG_DFL);
    raise(sig);
}

void BSC_debugInstallHandlers(void) {
    if (g_installed) return;
    g_installed = 1;
    FILE* f = fopen(BSC_debugLogPath(), "ab");
    if (f) {
        fprintf(f, "\n===== Butterscotch debug log start =====\n");
        fclose(f);
    }
    signal(SIGABRT, onSignal);
    signal(SIGSEGV, onSignal);
#ifdef SIGBUS
    signal(SIGBUS, onSignal);
#endif
#ifdef SIGILL
    signal(SIGILL, onSignal);
#endif
#ifdef SIGFPE
    signal(SIGFPE, onSignal);
#endif
    BSC_debugLog("init", "debug log path: %s", BSC_debugLogPath());
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((constructor))
#endif
static void BSC_debugConstructor(void) {
    BSC_debugInstallHandlers();
}
