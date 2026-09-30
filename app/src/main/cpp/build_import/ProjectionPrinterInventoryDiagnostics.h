#ifndef INFINITE_TEXTURE_PROJECTION_PRINTER_INVENTORY_DIAGNOSTICS_H
#define INFINITE_TEXTURE_PROJECTION_PRINTER_INVENTORY_DIAGNOSTICS_H

#include <cstdarg>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace build_import {

// Deliberately independent of the application's disabled general logging.
// Call only at inventory transaction boundaries; never include item NBT,
// account information, authentication tokens, or per-tick inventory dumps.
#if defined(__MINGW32__) && defined(__GNUC__)
__attribute__((format(gnu_printf, 1, 2)))
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
inline void LogProjectionPrinterInventoryDiagnostic(const char* format, ...) {
#if defined(__ANDROID__)
    va_list arguments;
    va_start(arguments, format);
    __android_log_vprint(ANDROID_LOG_INFO, "BuildToolInv", format, arguments);
    va_end(arguments);
#else
    (void)format;
#endif
}

}  // namespace build_import

#endif
