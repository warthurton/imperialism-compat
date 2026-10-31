/* Native x86 load/IAT/text-log smoke host; never loads the game or opens a window. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    HMODULE module;
    HDC context;
    if (argc != 2) return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    module = LoadLibraryA(argv[1]);
    if (!module) {
        fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }
    if (!GetProcAddress(module, "MCIWndCreateA")) return 3;
    /* CDS_TEST avoids a desktop mode change even if interception fails. */
    if (ChangeDisplaySettingsA(NULL, CDS_TEST) != DISP_CHANGE_SUCCESSFUL) return 4;
    context = CreateCompatibleDC(NULL);
    if (!context) return 5;
    TextOutA(context, 10, 20, "shim smoke", 10);
    DeleteDC(context);
    puts("loaded x86 shim and resolved MCIWndCreateA");
    /* The shim owns a process-lifetime worker; terminate the host rather than unloading it. */
    return 0;
}
