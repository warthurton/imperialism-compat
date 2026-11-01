/* Start here. Windows loads this file as the game's MSVFW32 DLL replacement.
 * DllMain wires the modules together; MCIWndCreateA forwards the real video call.
 * The game uses MFC, but this shim is ordinary C using the Windows C API. */
#include "shim_internal.h"
#include <string.h>

/* Windows calls DllMain when this DLL is loaded or the process is ending; the game
 * does not call it directly. "instance" identifies this loaded copy of our DLL.
 * Startup here runs under Windows' loader lock. Preserve the existing startup
 * protocol; do not add window creation or blocking work to this callback.
 *
 * Process attach initializes logging, DPI awareness and the main executable's IAT,
 * reads the adjacent INI once, optionally opens the text log, then starts summaries.
 * Detach only writes a final summary: it does not restore IAT/subclasses/hooks or close
 * all resources. This DLL is intended to stay loaded for the process lifetime. */
BOOL WINAPI DllMain(HINSTANCE instance, DWORD notification, LPVOID reservedContext)
{
    (void)reservedContext;
    if (notification == DLL_PROCESS_ATTACH) {
        char logPath[MAX_PATH], *filename;
        diagnostics_initialize();
        /* We need process start/end notifications, not a callback for every new
         * thread. Logs/settings live beside our DLL, not beside the working folder. */
        DisableThreadLibraryCalls(instance);
        GetModuleFileNameA(instance, logPath, MAX_PATH);
        filename = strrchr(logPath, '\\');
        if (filename)
            strcpy(filename + 1, "imperialism_gdi_log.txt");
        diagnostics_open(logPath);
        /* DPI scaling is Windows' own enlargement for high-density screens.
         * Opt out of that automatic enlargement: our geometry uses physical pixels. */
        SetProcessDPIAware();
        diagnostics_log("shim loaded; patched %d imports", hooks_patch_executable());
        {
            /* INI getters supply defaults when a key is absent. Configure the
             * display module before the game resumes and begins creating windows. */
            char iniPath[MAX_PATH], *iniFilename;
            DisplayOptions options;
            strcpy(iniPath, logPath);
            iniFilename = strrchr(iniPath, '\\');
            if (iniFilename)
                strcpy(iniFilename + 1, "imperialism_shim.ini");
            options.integer_scale =
                GetPrivateProfileIntA("display", "fullscreen_integer_scale", 1, iniPath) != 0;
            options.block_mode_change =
                GetPrivateProfileIntA("display", "block_mode_change", 1, iniPath) != 0;
            options.brown_titlebar =
                GetPrivateProfileIntA("display", "brown_titlebar", 1, iniPath) != 0;
            options.original_backdrop =
                GetPrivateProfileIntA("display", "original_backdrop", 1, iniPath) != 0;
            display_configure(&options);
            if (GetPrivateProfileIntA("text_log", "enabled", 0, iniPath) != 0) {
                char textPath[MAX_PATH], *textFilename;
                strcpy(textPath, logPath);
                textFilename = strrchr(textPath, '\\');
                if (textFilename)
                    strcpy(textFilename + 1, "imperialism_text.log");
                text_log_open(textPath);
            }
        }
        diagnostics_start_worker();
    } else if (notification == DLL_PROCESS_DETACH) {
        diagnostics_write_summary();
    }
    return TRUE;
}

/* ---- msvfw32 forwarding: the only export the game imports ---- */
/* Forward the game's sole required MSVFW32 export to the Windows system DLL by an
 * explicit system-directory path, avoiding this proxy's filename. Preserve __cdecl
 * and the export ABI. Cache the module/function for process lifetime; return NULL if
 * loading or resolving fails. This is independent of the separate WINMM music proxy. */
HWND __cdecl MCIWndCreateA(HWND parent, HINSTANCE instance, DWORD style, LPCSTR filename)
{
    typedef HWND(__cdecl * SystemVideoWindowFunction)(HWND, HINSTANCE, DWORD, LPCSTR);
    static SystemVideoWindowFunction systemCreate;
    if (!systemCreate) {
        char systemPath[MAX_PATH];
        HMODULE systemModule;
        /* Loading by bare filename would find this proxy again. Use Windows'
         * system directory explicitly, then ask that module for its real function. */
        GetSystemDirectoryA(systemPath, MAX_PATH);
        strcat(systemPath, "\\msvfw32.dll");
        systemModule = LoadLibraryA(systemPath);
        if (systemModule)
            systemCreate = (void *)GetProcAddress(systemModule, "MCIWndCreateA");
    }
    return systemCreate ? systemCreate(parent, instance, style, filename) : NULL;
}
