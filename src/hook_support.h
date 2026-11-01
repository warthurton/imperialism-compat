/* Small, internal conveniences for API wrappers; the lookup/patch algorithm is in hooks.c. */
#ifndef IMPERIALISM_HOOK_SUPPORT_H
#define IMPERIALISM_HOOK_SUPPORT_H

#include "shim_internal.h"

#define DIAGNOSTIC_LOG_LIMIT 40

/* ## joins tokens: DECLARE_HOOK(..., BitBlt) defines the private hook_BitBlt record.
 * # makes the literal "BitBlt" used to match the executable's import by name.
 * Keeping this tiny declaration macro avoids duplicating those names by hand. */
#define DECLARE_HOOK(library, symbol) static Hook hook_##symbol = {library, #symbol, NULL, NULL, 0}

/* Count every call but print only the first forty. InterlockedIncrement is the
 * Windows atomic increment: the summary thread may read while the UI increments.
 * __VA_ARGS__ is the caller's format string and arguments, prefixed with the API name. */
#define LOG_HOOK(symbol, ...)                                                                      \
    do {                                                                                           \
        LONG count = InterlockedIncrement(&hook_##symbol.call_count);                              \
        if (count <= DIAGNOSTIC_LOG_LIMIT)                                                         \
            diagnostics_log(#symbol " " __VA_ARGS__);                                              \
    } while (0)

#endif
