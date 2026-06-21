#ifndef DEBUG_PRINT_H_
#define DEBUG_PRINT_H_

#include <stdarg.h>
#include <stdio.h>


// Set to 1 to enable debug printing, or 0 to disable it
#define ENABLE_DBG_PRINT 0

/**
 * @brief Prints a formatted debug message if ENABLE_DBG_PRINT is set to 1.
 *
 * @param format Format string, followed by standard format arguments.
 */
static inline void DbgPrint(const char *format, ...) {
#if ENABLE_DBG_PRINT
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
#else
    (void)format;
#endif
}

#endif // DEBUG_PRINT_H_
