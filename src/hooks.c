/* Import-table interception, isolated from window and drawing behavior.
 *
 * A Windows executable contains a table of addresses for DLL functions it calls.
 * This is the Import Address Table (IAT). Replacing one address redirects future
 * calls through that slot. The original address is saved in Hook.original.
 * We edit only the game's table: our DLL's own Windows calls still go directly
 * to Windows, so a replacement does not recursively call itself.
 *
 * PE (Portable Executable) headers describe where to find the table in memory.
 * Their offsets are RVAs: add the executable's base address to obtain a pointer.
 * This is specifically the existing 32-bit, named-import, process-lifetime hook.
 */
#include "shim_internal.h"
#include <string.h>

static HookTable tables[4];

static Hook *find_hook(const char *library, const char *function)
{
    size_t group, index;
    for (group = 0; group < sizeof tables / sizeof tables[0]; group++) {
        for (index = 0; index < tables[group].count; index++) {
            Hook *hook = tables[group].entries[index];
            if (!_stricmp(library, hook->library_name) && !strcmp(function, hook->function_name))
                return hook;
        }
    }
    return NULL;
}

int hooks_patch_executable(void)
{
    BYTE *imageBase = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dosHeader = (IMAGE_DOS_HEADER *)imageBase;
    IMAGE_NT_HEADERS *ntHeaders = (IMAGE_NT_HEADERS *)(imageBase + dosHeader->e_lfanew);
    IMAGE_DATA_DIRECTORY imports =
        ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *library;
    int patched = 0;

    tables[0] = graphics_hook_table();
    tables[1] = display_hook_table();
    tables[2] = input_hook_table();
    tables[3] = text_hook_table();
    if (!imports.VirtualAddress)
        return 0;

    /* One descriptor per imported DLL. A zero Name marks the end. */
    for (library = (IMAGE_IMPORT_DESCRIPTOR *)(imageBase + imports.VirtualAddress); library->Name;
         library++) {
        const char *libraryName = (const char *)(imageBase + library->Name);
        DWORD nameOffset =
            library->OriginalFirstThunk ? library->OriginalFirstThunk : library->FirstThunk;
        IMAGE_THUNK_DATA *name = (IMAGE_THUNK_DATA *)(imageBase + nameOffset);
        IMAGE_THUNK_DATA *address = (IMAGE_THUNK_DATA *)(imageBase + library->FirstThunk);

        /* The name and address arrays describe the same calls in the same order.
         * One tells us which function this is; the other is the slot to replace. */
        for (; name->u1.AddressOfData; name++, address++) {
            const IMAGE_IMPORT_BY_NAME *namedImport;
            Hook *hook;
            DWORD previousProtection;
            if (IMAGE_SNAP_BY_ORDINAL32(name->u1.Ordinal))
                continue;
            namedImport = (const IMAGE_IMPORT_BY_NAME *)(imageBase + name->u1.AddressOfData);
            hook = find_hook(libraryName, (const char *)namedImport->Name);
            if (!hook)
                continue;

            /* Import slots are normally read-only. Open just this slot for writing,
             * save Windows' address, install our wrapper, and restore protection. */
            if (!VirtualProtect(
                    &address->u1.Function, sizeof(void *), PAGE_READWRITE, &previousProtection))
                continue;
            hook->original = (void *)address->u1.Function;
            address->u1.Function = (DWORD)(ULONG_PTR)hook->replacement;
            VirtualProtect(
                &address->u1.Function, sizeof(void *), previousProtection, &previousProtection);
            patched++;
        }
    }
    return patched;
}

void hooks_write_counts(FILE *destination)
{
    size_t group, index;
    for (group = 0; group < sizeof tables / sizeof tables[0]; group++) {
        for (index = 0; index < tables[group].count; index++) {
            Hook *hook = tables[group].entries[index];
            fprintf(destination,
                    "%-24s %s %ld\n",
                    hook->function_name,
                    hook->original ? "hooked" : "NOT-IMPORTED",
                    hook->call_count);
        }
    }
}
