/*
 * WindroseFXCore - runtime CPU scheduling optimizer for Windrose (Unreal Engine 5)
 * https://github.com/mcirish3/WindroseFXCore  (MIT License)
 *
 * Loaded into the game as a version.dll proxy. It does NOT change any graphics
 * settings, config files or game files. It changes how Windows runs the game's
 * threads, aimed at older CPUs (AMD FX, older Intel Core, early Ryzen):
 *
 *   1. High-resolution system timer (0.5 ms) so UE's frame limiter and thread
 *      waits wake on time instead of overshooting by up to ~15 ms.
 *   2. Opts the process out of Windows power throttling (EcoQoS) and timer
 *      coalescing, so the scheduler never parks it on a slow clock.
 *   3. Topology-aware thread placement. UE's GameThread, RenderThread and
 *      RHIThread decide the frame rate. When two land on the same AMD FX module
 *      (two cores sharing one FPU, L2 and decoder) or the same hyperthreaded
 *      core, they slow each other down. This mod reads the real cache topology,
 *      gives each its own module/core, and keeps UE's background worker pool
 *      off those cores.
 *   4. Priority floors for the critical-path threads and the audio mixer, so
 *      background streaming and decompression can't preempt them (also the
 *      usual cause of audio crackle on 8-thread FX chips under load).
 *
 * Everything is configurable in WindroseFXCore.ini and logged to
 * WindroseFXCore.log next to the DLL.
 */
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>

#define FXCORE_VERSION "1.0.0-beta"

/* ------------------------------------------------------------------ */
/* Config & logging                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    int enabled;
    int timerResolution100ns;     /* 0 = off, 5000 = 0.5 ms                  */
    int disablePowerThrottling;
    int processPriority;          /* 0 normal, 1 above normal, 2 high        */
    int placeCriticalThreads;     /* GameThread/RenderThread/RHIThread       */
    int isolateBackgroundWorkers;
    int boostCriticalPriority;
    int boostAudioPriority;
    int scanFastSeconds;          /* rescan every 1 s for this long          */
    int scanSlowMs;               /* then rescan at this interval            */
    int verboseLog;
} Config;

static Config  g_cfg;
static wchar_t g_dir[MAX_PATH];
static FILE   *g_log;

static void LogLine(const char *fmt, ...)
{
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static int IniInt(const wchar_t *ini, const wchar_t *key, int def)
{
    return (int)GetPrivateProfileIntW(L"FXCore", key, def, ini);
}

static void LoadConfig(void)
{
    wchar_t ini[MAX_PATH];
    swprintf(ini, MAX_PATH, L"%ls\\WindroseFXCore.ini", g_dir);
    g_cfg.enabled                  = IniInt(ini, L"Enabled", 1);
    g_cfg.timerResolution100ns     = IniInt(ini, L"TimerResolution100ns", 5000);
    g_cfg.disablePowerThrottling   = IniInt(ini, L"DisablePowerThrottling", 1);
    g_cfg.processPriority          = IniInt(ini, L"ProcessPriority", 1);
    g_cfg.placeCriticalThreads     = IniInt(ini, L"PlaceCriticalThreads", 1);
    g_cfg.isolateBackgroundWorkers = IniInt(ini, L"IsolateBackgroundWorkers", 1);
    g_cfg.boostCriticalPriority    = IniInt(ini, L"BoostCriticalPriority", 1);
    g_cfg.boostAudioPriority       = IniInt(ini, L"BoostAudioPriority", 1);
    g_cfg.scanFastSeconds          = IniInt(ini, L"ScanFastSeconds", 180);
    g_cfg.scanSlowMs               = IniInt(ini, L"ScanSlowMs", 5000);
    g_cfg.verboseLog               = IniInt(ini, L"VerboseLog", 0);
}

/* ------------------------------------------------------------------ */
/* CPU topology                                                        */
/*                                                                     */
/* An "execution group" is a set of logical processors that share an   */
/* L2 cache, and therefore compete for the same execution resources:   */
/*   AMD FX (Bulldozer/Piledriver): one module, 2 cores sharing an FPU  */
/*   Intel/AMD with SMT: one physical core, 2 hyperthreads              */
/*   CPUs without SMT: one core                                         */
/*   Intel E-cores: a cluster of 4                                       */
/* Critical threads each get their own group. Groups are chosen from    */
/* the fastest cores (Intel hybrid) inside one L3 domain (so older      */
/* multi-CCX Ryzen chips don't bounce frame data between CCXs).         */
/* ------------------------------------------------------------------ */

#define MAX_GROUPS 64
typedef struct { KAFFINITY mask; int eff; int l3; } ExecGroup;

static ExecGroup g_grp[MAX_GROUPS];
static int       g_ngrp;
static KAFFINITY g_allMask;
static int       g_crit[3] = { -1, -1, -1 };   /* preferred LP for game/render/rhi */
static KAFFINITY g_bgMask;

static int LowestBit(KAFFINITY m) { for (int i = 0; i < 64; i++) if (m & ((KAFFINITY)1 << i)) return i; return -1; }
static int PopCount(KAFFINITY m)  { int n = 0; while (m) { n += (int)(m & 1); m >>= 1; } return n; }

static BYTE *QueryLPI(LOGICAL_PROCESSOR_RELATIONSHIP rel, DWORD *len)
{
    *len = 0;
    GetLogicalProcessorInformationEx(rel, NULL, len);
    if (!*len) return NULL;
    BYTE *buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, *len);
    if (buf && !GetLogicalProcessorInformationEx(rel, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, len)) {
        HeapFree(GetProcessHeap(), 0, buf);
        buf = NULL;
    }
    return buf;
}

static void AddGroup(KAFFINITY m)
{
    m &= g_allMask;
    if (!m || g_ngrp >= MAX_GROUPS) return;
    for (int i = 0; i < g_ngrp; i++) if (g_grp[i].mask == m) return;
    g_grp[g_ngrp].mask = m; g_grp[g_ngrp].eff = 0; g_grp[g_ngrp].l3 = 0;
    g_ngrp++;
}

static void LogCpuName(void)
{
    wchar_t name[128] = L"unknown";
    DWORD sz = sizeof(name);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                 L"ProcessorNameString", RRF_RT_REG_SZ, NULL, name, &sz);
    char narrow[128];
    WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof(narrow), NULL, NULL);
    LogLine("CPU: %s", narrow);
}

static void DetectTopology(void)
{
    DWORD_PTR proc, sys;
    GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
    g_allMask = (KAFFINITY)proc;
    LogCpuName();

    DWORD len;
    KAFFINITY l3[MAX_GROUPS]; int nl3 = 0;

    /* L2 groups and L3 domains */
    BYTE *buf = QueryLPI(RelationCache, &len);
    for (DWORD off = 0; buf && off < len; ) {
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off);
        if (e->Cache.GroupMask.Group == 0 && (e->Cache.Type == CacheUnified || e->Cache.Type == CacheData)) {
            if (e->Cache.Level == 2) AddGroup(e->Cache.GroupMask.Mask);
            if (e->Cache.Level == 3 && nl3 < MAX_GROUPS) l3[nl3++] = e->Cache.GroupMask.Mask & g_allMask;
        }
        off += e->Size;
    }
    if (buf) HeapFree(GetProcessHeap(), 0, buf);

    /* Fallback (some VMs report no L2): one group per physical core */
    buf = QueryLPI(RelationProcessorCore, &len);
    if (g_ngrp == 0)
        for (DWORD off = 0; buf && off < len; ) {
            PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off);
            if (e->Processor.GroupMask[0].Group == 0) AddGroup(e->Processor.GroupMask[0].Mask);
            off += e->Size;
        }

    /* Efficiency class per group (Intel hybrid: P-cores > E-cores; 0 everywhere else) */
    for (DWORD off = 0; buf && off < len; ) {
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off);
        if (e->Processor.GroupMask[0].Group == 0)
            for (int i = 0; i < g_ngrp; i++)
                if ((g_grp[i].mask & e->Processor.GroupMask[0].Mask) && e->Processor.EfficiencyClass > g_grp[i].eff)
                    g_grp[i].eff = e->Processor.EfficiencyClass;
        off += e->Size;
    }
    if (buf) HeapFree(GetProcessHeap(), 0, buf);

    for (int i = 0; i < g_ngrp; i++)
        for (int j = 0; j < nl3; j++)
            if (g_grp[i].mask & l3[j]) { g_grp[i].l3 = j; break; }

    /* sort by lowest logical processor */
    for (int i = 0; i < g_ngrp; i++)
        for (int j = i + 1; j < g_ngrp; j++)
            if (LowestBit(g_grp[j].mask) < LowestBit(g_grp[i].mask)) { ExecGroup t = g_grp[i]; g_grp[i] = g_grp[j]; g_grp[j] = t; }

    LogLine("Topology: %d logical processors, %d execution groups, %d L3 domain(s)",
            PopCount(g_allMask), g_ngrp, nl3 ? nl3 : 1);
    for (int i = 0; i < g_ngrp; i++)
        LogLine("  group %d: mask 0x%llx  efficiency %d  L3 %d",
                i, (unsigned long long)g_grp[i].mask, g_grp[i].eff, g_grp[i].l3);

    /* Pick the L3 domain with the most top-efficiency groups. */
    int topEff = 0;
    for (int i = 0; i < g_ngrp; i++) if (g_grp[i].eff > topEff) topEff = g_grp[i].eff;
    int bestL3 = 0, bestCount = -1;
    for (int d = 0; d < (nl3 ? nl3 : 1); d++) {
        int c = 0;
        for (int i = 0; i < g_ngrp; i++) if (g_grp[i].l3 == d && g_grp[i].eff == topEff) c++;
        if (c > bestCount) { bestCount = c; bestL3 = d; }
    }

    /* Need 3 separate fast groups and at least 4 groups overall, otherwise
       pinning would do more harm than good (dual cores, plain quad cores). */
    if (bestCount >= 3 && g_ngrp >= 4) {
        int n = 0;
        for (int i = 0; i < g_ngrp && n < 3; i++)
            if (g_grp[i].l3 == bestL3 && g_grp[i].eff == topEff) g_crit[n++] = LowestBit(g_grp[i].mask);

        /* Background workers: everything except the three critical LPs,
           but never shrink the pool below half the machine. */
        KAFFINITY m = g_allMask;
        for (int s = 0; s < 3; s++) m &= ~((KAFFINITY)1 << g_crit[s]);
        if (PopCount(m) * 2 >= PopCount(g_allMask)) g_bgMask = m;
    } else {
        LogLine("Topology: fewer than 3 separate fast core groups; thread placement disabled (priority and timer fixes still apply)");
    }
}

/* ------------------------------------------------------------------ */
/* Thread classification by UE thread name                            */
/* ------------------------------------------------------------------ */

typedef HRESULT (WINAPI *PFN_GetThreadDescription)(HANDLE, PWSTR *);
static PFN_GetThreadDescription pGetThreadDescription;

enum { T_OTHER, T_GAME, T_RENDER, T_RHI, T_AUDIO_MIX, T_AUDIO, T_BACKGROUND };

static int Classify(const wchar_t *n)
{
    if (!n || !*n) return T_OTHER;
    if (!wcsncmp(n, L"GameThread", 10))               return T_GAME;
    if (!wcsncmp(n, L"RenderThread", 12))             return T_RENDER;
    if (!wcscmp (n, L"RHIThread"))                    return T_RHI;
    if (!wcsncmp(n, L"AudioMixerRenderThread", 22))   return T_AUDIO_MIX;
    if (!wcsncmp(n, L"AudioThread", 11))              return T_AUDIO;
    if (!wcsncmp(n, L"Background Worker", 17))        return T_BACKGROUND;
    if (!wcsncmp(n, L"BackgroundThreadPool", 20))     return T_BACKGROUND;
    return T_OTHER;
}

static const char *KindName(int k)
{
    switch (k) {
        case T_GAME: return "GameThread"; case T_RENDER: return "RenderThread";
        case T_RHI: return "RHIThread";   case T_AUDIO_MIX: return "AudioMixer";
        case T_AUDIO: return "AudioThread"; case T_BACKGROUND: return "BackgroundWorker";
    }
    return "other";
}

/* track threads already handled */
#define MAX_SEEN 4096
static DWORD g_seen[MAX_SEEN];
static int   g_nseen;
static int Seen(DWORD tid) { for (int i = 0; i < g_nseen; i++) if (g_seen[i] == tid) return 1; return 0; }
static void MarkSeen(DWORD tid) { if (g_nseen < MAX_SEEN) g_seen[g_nseen++] = tid; }

static void RaisePriorityTo(HANDLE h, int target)
{
    int cur = GetThreadPriority(h);
    if (cur != THREAD_PRIORITY_ERROR_RETURN && cur < target) SetThreadPriority(h, target);
}

/* Logical processor that each critical thread should prefer: the first LP of
   its own L2 group, so GameThread, RenderThread and RHIThread never share a
   module (FX) or a physical core (SMT CPUs). */
static int CriticalLP(int kind)
{
    return g_crit[(kind == T_GAME) ? 0 : (kind == T_RENDER) ? 1 : 2];
}

static KAFFINITY BackgroundMask(void) { return g_bgMask; }

static int ApplyToThread(DWORD tid, int kind)
{
    HANDLE h = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h) return 0;

    switch (kind) {
    case T_GAME: case T_RENDER: case T_RHI:
        if (g_cfg.placeCriticalThreads) {
            int lp = CriticalLP(kind);
            if (lp >= 0) {
                PROCESSOR_NUMBER pn = { 0, (BYTE)lp, 0 };
                SetThreadIdealProcessorEx(h, &pn, NULL);   /* soft preference */
            }
        }
        if (g_cfg.boostCriticalPriority) RaisePriorityTo(h, THREAD_PRIORITY_ABOVE_NORMAL);
        break;
    case T_AUDIO_MIX:
        if (g_cfg.boostAudioPriority) RaisePriorityTo(h, THREAD_PRIORITY_HIGHEST);
        break;
    case T_AUDIO:
        if (g_cfg.boostAudioPriority) RaisePriorityTo(h, THREAD_PRIORITY_ABOVE_NORMAL);
        break;
    case T_BACKGROUND:
        if (g_cfg.isolateBackgroundWorkers) {
            KAFFINITY m = BackgroundMask();
            if (m) SetThreadAffinityMask(h, (DWORD_PTR)m);
        }
        break;
    }
    if (g_cfg.verboseLog || kind != T_BACKGROUND)
        LogLine("  %-16s tid %5lu  prio %d", KindName(kind), tid, GetThreadPriority(h));
    CloseHandle(h);
    return 1;
}

static int ScanThreads(void)
{
    int applied = 0;
    DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te; te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || Seen(te.th32ThreadID)) continue;
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        PWSTR name = NULL;
        int kind = T_OTHER;
        if (pGetThreadDescription && SUCCEEDED(pGetThreadDescription(h, &name)) && name) {
            kind = Classify(name);
            /* Only mark a thread as handled once UE has named it. */
            if (*name) {
                MarkSeen(te.th32ThreadID);
                if (kind != T_OTHER) applied += ApplyToThread(te.th32ThreadID, kind);
            }
            LocalFree(name);
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    return applied;
}

/* ------------------------------------------------------------------ */
/* Process-wide settings                                              */
/* ------------------------------------------------------------------ */

typedef LONG (NTAPI *PFN_NtSetTimerResolution)(ULONG, BOOLEAN, PULONG);

static void ApplyProcessSettings(void)
{
    if (g_cfg.timerResolution100ns > 0) {
        PFN_NtSetTimerResolution pSet =
            (PFN_NtSetTimerResolution)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetTimerResolution");
        ULONG actual = 0;
        if (pSet && pSet((ULONG)g_cfg.timerResolution100ns, TRUE, &actual) == 0)
            LogLine("Timer resolution: %.3f ms", actual / 10000.0);
        else
            LogLine("Timer resolution: request failed");
    }

    if (g_cfg.disablePowerThrottling) {
        PROCESS_POWER_THROTTLING_STATE s = { 0 };
        s.Version     = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        s.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | 0x4 /* IGNORE_TIMER_RESOLUTION */;
        s.StateMask   = 0;  /* 0 = never throttle */
        BOOL ok = SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof(s));
        LogLine("Power throttling opt-out: %s", ok ? "ok" : "failed");
    }

    DWORD cls = NORMAL_PRIORITY_CLASS;
    if (g_cfg.processPriority == 1) cls = ABOVE_NORMAL_PRIORITY_CLASS;
    if (g_cfg.processPriority == 2) cls = HIGH_PRIORITY_CLASS;
    if (cls != NORMAL_PRIORITY_CLASS)
        LogLine("Process priority: %s", SetPriorityClass(GetCurrentProcess(), cls) ? "raised" : "failed");
}

/* ------------------------------------------------------------------ */
/* Worker                                                             */
/* ------------------------------------------------------------------ */

static DWORD WINAPI Worker(LPVOID unused)
{
    (void)unused;
    pGetThreadDescription = (PFN_GetThreadDescription)
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
    if (!pGetThreadDescription)
        pGetThreadDescription = (PFN_GetThreadDescription)
            GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "GetThreadDescription");

    DetectTopology();
    ApplyProcessSettings();

    KAFFINITY bg = BackgroundMask();
    LogLine("Critical LPs: game %d, render %d, rhi %d | background mask 0x%llx",
            CriticalLP(T_GAME), CriticalLP(T_RENDER), CriticalLP(T_RHI), (unsigned long long)bg);

    ULONGLONG start = GetTickCount64();
    for (;;) {
        int n = ScanThreads();
        if (n && !g_cfg.verboseLog) LogLine("Scan: tuned %d new thread(s)", n);
        ULONGLONG elapsed = GetTickCount64() - start;
        Sleep(elapsed < (ULONGLONG)g_cfg.scanFastSeconds * 1000 ? 1000 : (DWORD)g_cfg.scanSlowMs);
    }
    return 0;
}

void FXCore_Start(HMODULE self)
{
    GetModuleFileNameW(self, g_dir, MAX_PATH);
    wchar_t *slash = wcsrchr(g_dir, L'\\');
    if (slash) *slash = 0;

    /* Only run inside the game executable, never in crash reporters or tools. */
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (!wcsstr(exe, L"Shipping")) return;

    LoadConfig();
    if (!g_cfg.enabled) return;

    wchar_t logPath[MAX_PATH];
    swprintf(logPath, MAX_PATH, L"%ls\\WindroseFXCore.log", g_dir);
    g_log = _wfopen(logPath, L"w");
    LogLine("WindroseFXCore " FXCORE_VERSION " loaded into %ls", exe);

    HANDLE t = CreateThread(NULL, 0, Worker, NULL, 0, NULL);
    if (t) CloseHandle(t);
}
