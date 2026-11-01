/* Private contracts between this DLL's C files; none are exported to the game. */
#ifndef IMPERIALISM_SHIM_INTERNAL_H
#define IMPERIALISM_SHIM_INTERNAL_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

/* Windows normally calls the address in an imported-function slot. A Hook records
 * our substitute and the saved address, so a wrapper can still call real Windows.
 * Only hooks.c changes the executable's slots. Each wrapper owns its counter. */
typedef struct {
    const char *library_name;
    const char *function_name;
    void *replacement;
    void *original;
    volatile LONG call_count;
} Hook;

typedef struct {
    Hook *const *entries;
    size_t count;
} HookTable;

typedef struct {
    BOOL integer_scale;
    BOOL block_mode_change;
    BOOL brown_titlebar;
    BOOL original_backdrop;
} DisplayOptions;

/* Module-owned lists are registered once at startup, before patching any imports. */
HookTable graphics_hook_table(void);
HookTable display_hook_table(void);
HookTable input_hook_table(void);
HookTable text_hook_table(void);
int hooks_patch_executable(void);
void hooks_write_counts(FILE *destination);

/* The display owns window handles and geometry. Input gets handles by value, not
 * access to mutable display globals. HWND is Windows' opaque identifier for a window. */
void display_configure(const DisplayOptions *options);
HWND display_frame_window(void);
HWND display_mirror_window(void);
BOOL display_map_point(LPARAM mirrorPosition, POINT *framePoint);
void display_write_diagnostics(FILE *destination);

/* Called by the mirror's event callback, on the game's UI thread. For mouse events,
 * LPARAM packs coordinates and WPARAM holds button flags. For cursor queries,
 * LPARAM instead packs hit-test/message fields. input.c interprets each payload. */
LRESULT input_relay_mouse(HWND window, UINT message, WPARAM buttons, LPARAM position);
LRESULT input_relay_cursor(HWND window, LPARAM hitTest);
void input_mouse_leave(void);
BOOL input_has_mirror_capture(void);

/* Both log files share this lock. The diagnostic worker is process-lifetime;
 * explicit unloading remains unsupported, as in the original implementation. */
void diagnostics_initialize(void);
void diagnostics_open(const char *path);
void diagnostics_log(const char *format, ...);
void diagnostics_lock(void);
void diagnostics_unlock(void);
void diagnostics_write_summary(void);
void diagnostics_start_worker(void);
void text_log_open(const char *path);

#endif
