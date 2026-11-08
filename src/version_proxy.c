/*
 * version.dll proxy. Unreal Engine's Core module links version.lib, so the
 * game loads version.dll from its own folder first. Every export is forwarded
 * to the real System32 copy, then FXCore starts its worker thread.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

void FXCore_Start(HMODULE self);

static HMODULE g_real;

static FARPROC Real(const char *name)
{
    if (!g_real) {
        wchar_t path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH);
        lstrcatW(path, L"\\version.dll");
        g_real = LoadLibraryW(path);
    }
    return g_real ? GetProcAddress(g_real, name) : NULL;
}

/* Generic x64 pass-through: forwards up to 8 pointer-sized arguments, which
   covers every version.dll export (the largest, VerInstallFile, takes 8). */
typedef ULONG_PTR (WINAPI *PFN8)(ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR,
                                 ULONG_PTR, ULONG_PTR, ULONG_PTR, ULONG_PTR);

#define FWD(fn) \
    ULONG_PTR WINAPI Proxy_##fn(ULONG_PTR a, ULONG_PTR b, ULONG_PTR c, ULONG_PTR d, \
                                              ULONG_PTR e, ULONG_PTR f, ULONG_PTR g, ULONG_PTR h) { \
        static PFN8 p; if (!p) p = (PFN8)Real(#fn); \
        return p ? p(a, b, c, d, e, f, g, h) : 0; \
    }

FWD(GetFileVersionInfoA)
FWD(GetFileVersionInfoByHandle)
FWD(GetFileVersionInfoExA)
FWD(GetFileVersionInfoExW)
FWD(GetFileVersionInfoSizeA)
FWD(GetFileVersionInfoSizeExA)
FWD(GetFileVersionInfoSizeExW)
FWD(GetFileVersionInfoSizeW)
FWD(GetFileVersionInfoW)
FWD(VerFindFileA)
FWD(VerFindFileW)
FWD(VerInstallFileA)
FWD(VerInstallFileW)
FWD(VerLanguageNameA)
FWD(VerLanguageNameW)
FWD(VerQueryValueA)
FWD(VerQueryValueW)

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        FXCore_Start(inst);   /* spawns a thread; only light kernel32 file I/O under loader lock */
    }
    return TRUE;
}
