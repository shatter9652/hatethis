#pragma once
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

void BSC_debugLog(const char* category, const char* fmt, ...);
void BSC_debugLogV(const char* category, const char* fmt, va_list args);
const char* BSC_debugLogPath(void);
void BSC_debugInstallHandlers(void);

#ifdef __cplusplus
}
#endif
