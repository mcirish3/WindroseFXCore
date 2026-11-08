/*
 * WindroseFXCore - runtime CPU scheduling optimizer for Windrose (Unreal Engine 5)
 * https://github.com/mcirish3/WindroseFXCore  (MIT License)
 *
 * Loaded into the game as a version.dll proxy. It does NOT change any graphics
 * settings or game files. It changes how Windows runs the game's threads,
 * aimed at older CPUs (AMD FX, older Intel Core, early Ryzen). The only config
 * file it can touch is the user Engine.ini, and only when the optional
 * AudioBufferFix is turned on (see the Engine.ini section below):
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

#define FXCORE_VERSION "1.0.2-beta"

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
    int audioMixerPriority;       /* Windows thread priority for the mixer */
    int placeAudioThread;
    int audioBufferFix;           /* write the audio buffer keys to Engine.ini */
    int audioBufferFrames;        /* AudioCallbackBufferFrameSize            */
    int audioBuffersToEnqueue;    /* AudioNumBuffersToEnqueue                */
    int scanFastSeconds;          /* rescan every 1 s for this long          */
    int scanSlowMs;               /* then rescan at this interval            */
    int verboseLog;
    int perfLogSeconds;           /* 0 = off; else log per-thread CPU load   */
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
    g_cfg.audioMixerPriority       = IniInt(ini, L"AudioMixerPriority", THREAD_PRIORITY_TIME_CRITICAL);
    g_cfg.placeAudioThread         = IniInt(ini, L"PlaceAudioThread", 1);
    g_cfg.audioBufferFix           = IniInt(ini, L"AudioBufferFix", 0);
    g_cfg.audioBufferFrames        = IniInt(ini, L"AudioBufferFrames", 2048);
    g_cfg.audioBuffersToEnqueue    = IniInt(ini, L"AudioBuffersToEnqueue", 4);
    if (g_cfg.audioBufferFrames < 256)  g_cfg.audioBufferFrames = 256;
    if (g_cfg.audioBufferFrames > 8192) g_cfg.audioBufferFrames = 8192;
    if (g_cfg.audioBuffersToEnqueue < 2) g_cfg.audioBuffersToEnqueue = 2;
    if (g_cfg.audioBuffersToEnqueue > 8) g_cfg.audioBuffersToEnqueue = 8;
    g_cfg.scanFastSeconds          = IniInt(ini, L"ScanFastSeconds", 180);
    g_cfg.scanSlowMs               = IniInt(ini, L"ScanSlowMs", 5000);
    g_cfg.verboseLog               = IniInt(ini, L"VerboseLog", 0);
    g_cfg.perfLogSeconds           = IniInt(ini, L"PerfLogSeconds", 0);
    if (g_cfg.perfLogSeconds < 0) g_cfg.perfLogSeconds = 0;
    if (g_cfg.perfLogSeconds > 0 && g_cfg.perfLogSeconds < 2) g_cfg.perfLogSeconds = 2;
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
static int       g_audioLP = -1;                /* preferred LP for the audio mixer */

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
        for (int i = 0; i < g_ngrp; i++) {
            if (g_grp[i].l3 != bestL3 || g_grp[i].eff != topEff) continue;
            if (n < 3) g_crit[n++] = LowestBit(g_grp[i].mask);
            else if (g_audioLP < 0) g_audioLP = LowestBit(g_grp[i].mask);   /* 4th fast group: audio */
        }

        /* Background workers: everything except the critical LPs and the audio
           LP, but never shrink the pool below half the machine. If excluding
           the audio LP would, give up the audio LP rather than the workers. */
        KAFFINITY m = g_allMask;
        for (int s = 0; s < 3; s++) m &= ~((KAFFINITY)1 << g_crit[s]);
        if (g_audioLP >= 0) {
            KAFFINITY ma = m & ~((KAFFINITY)1 << g_audioLP);
            if (PopCount(ma) * 2 >= PopCount(g_allMask)) m = ma; else g_audioLP = -1;
        }
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

enum { T_OTHER, T_GAME, T_RENDER, T_RHI, T_AUDIO_MIX, T_AUDIO, T_BACKGROUND, T_FOREGROUND };

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
    /* UE5 task-graph workers: only measured (PerfLogSeconds), never moved. */
    if (!wcsncmp(n, L"Foreground Worker", 17))        return T_FOREGROUND;
    return T_OTHER;
}

static const char *KindName(int k)
{
    switch (k) {
        case T_GAME: return "GameThread"; case T_RENDER: return "RenderThread";
        case T_RHI: return "RHIThread";   case T_AUDIO_MIX: return "AudioMixer";
        case T_AUDIO: return "AudioThread"; case T_BACKGROUND: return "BackgroundWorker";
        case T_FOREGROUND: return "ForegroundWorker";
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

/* ------------------------------------------------------------------ */
/* Optional diagnostics: per-thread CPU load (PerfLogSeconds)         */
/*                                                                    */
/* A thread near 100% of one core is what limits the frame rate. The  */
/* log shows which one: GameThread (simulation, AI, foliage logic),   */
/* RenderThread / RHIThread (draw calls) or the background workers.   */
/* ------------------------------------------------------------------ */

enum { P_GAME, P_RENDER, P_RHI, P_AUDIO, P_COUNT };
static HANDLE    g_perfThread[P_COUNT];
static ULONGLONG g_perfLast[P_COUNT];
static ULONGLONG g_perfLastProc, g_perfLastWall;
static ULONGLONG g_perfLastWorkers;
static DWORD     g_workerTid[256];
static int       g_nworkers;
static DWORD     g_fgTid[256];
static int       g_nfg;
static ULONGLONG g_perfLastFg;
static double    g_perfPeak[P_COUNT + 1];
static int       g_perfSamples;

static ULONGLONG FtToU64(FILETIME f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }

static ULONGLONG ThreadCpu(HANDLE h)
{
    FILETIME c, e, k, u;
    if (!h || !GetThreadTimes(h, &c, &e, &k, &u)) return 0;
    return FtToU64(k) + FtToU64(u);
}

static ULONGLONG SumCpu(const DWORD *tids, int n)
{
    ULONGLONG sum = 0;
    for (int i = 0; i < n; i++) {
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tids[i]);
        if (h) { sum += ThreadCpu(h); CloseHandle(h); }
    }
    return sum;
}

static double Pct(ULONGLONG now, ULONGLONG last, double dt) { return now >= last ? 100.0 * (double)(now - last) / dt : 0; }

static ULONGLONG WallNow(void) { FILETIME f; GetSystemTimeAsFileTime(&f); return FtToU64(f); }

static void PerfTrack(DWORD tid, int kind)
{
    if (!g_cfg.perfLogSeconds) return;
    int slot = (kind == T_GAME) ? P_GAME : (kind == T_RENDER) ? P_RENDER :
               (kind == T_RHI) ? P_RHI : (kind == T_AUDIO_MIX) ? P_AUDIO : -1;
    if (kind == T_BACKGROUND) { if (g_nworkers < 256) g_workerTid[g_nworkers++] = tid; return; }
    if (kind == T_FOREGROUND) { if (g_nfg < 256) g_fgTid[g_nfg++] = tid; return; }
    if (slot < 0) return;
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (!h) return;
    if (g_perfThread[slot]) CloseHandle(g_perfThread[slot]);   /* newest one wins (UE restarts the mixer) */
    g_perfThread[slot] = h;
    g_perfLast[slot] = ThreadCpu(h);
}

static void PerfSample(void)
{
    ULONGLONG wall = WallNow();
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    ULONGLONG proc = FtToU64(k) + FtToU64(u);
    ULONGLONG workers = SumCpu(g_workerTid, g_nworkers);
    ULONGLONG fg      = SumCpu(g_fgTid, g_nfg);

    if (g_perfLastWall) {
        double dt = (double)(wall - g_perfLastWall);
        if (dt <= 0) return;
        double pct[P_COUNT];
        for (int i = 0; i < P_COUNT; i++) {
            ULONGLONG now = ThreadCpu(g_perfThread[i]);
            pct[i] = (g_perfThread[i] && now >= g_perfLast[i]) ? 100.0 * (double)(now - g_perfLast[i]) / dt : 0;
            g_perfLast[i] = now;
            if (pct[i] > g_perfPeak[i]) g_perfPeak[i] = pct[i];
        }
        double procPct = Pct(proc, g_perfLastProc, dt);
        double wrkPct  = Pct(workers, g_perfLastWorkers, dt);
        double fgPct   = Pct(fg, g_perfLastFg, dt);
        double other   = procPct - pct[P_GAME] - pct[P_RENDER] - pct[P_RHI] - pct[P_AUDIO] - wrkPct - fgPct;
        if (other < 0) other = 0;
        if (procPct > g_perfPeak[P_COUNT]) g_perfPeak[P_COUNT] = procPct;
        LogLine("Perf: game %3.0f%%  render %3.0f%%  rhi %3.0f%%  audio %3.0f%%  bg(%d) %3.0f%%  fg(%d) %3.0f%%  other %3.0f%%  process %3.0f%% of %d00%%",
                pct[P_GAME], pct[P_RENDER], pct[P_RHI], pct[P_AUDIO], g_nworkers, wrkPct, g_nfg, fgPct,
                other, procPct, PopCount(g_allMask));
        if (++g_perfSamples % 60 == 0)
            LogLine("Perf peaks so far: game %.0f%%  render %.0f%%  rhi %.0f%%  audio %.0f%%  process %.0f%%",
                    g_perfPeak[P_GAME], g_perfPeak[P_RENDER], g_perfPeak[P_RHI], g_perfPeak[P_AUDIO], g_perfPeak[P_COUNT]);
    } else {
        for (int i = 0; i < P_COUNT; i++) g_perfLast[i] = ThreadCpu(g_perfThread[i]);
    }
    g_perfLastWall = wall; g_perfLastProc = proc; g_perfLastWorkers = workers; g_perfLastFg = fg;
}

static int ApplyToThread(DWORD tid, int kind)
{
    PerfTrack(tid, kind);
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
        /* The mixer does a tiny amount of work per audio buffer but must never
           miss its deadline, or the output crackles. Pro audio software runs
           this kind of thread at time-critical priority. */
        if (g_cfg.placeAudioThread && g_audioLP >= 0) {
            PROCESSOR_NUMBER pn = { 0, (BYTE)g_audioLP, 0 };
            SetThreadIdealProcessorEx(h, &pn, NULL);
        }
        if (g_cfg.boostAudioPriority) RaisePriorityTo(h, g_cfg.audioMixerPriority);
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
    if (g_cfg.verboseLog || (kind != T_BACKGROUND && kind != T_FOREGROUND))
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
        s.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | 0x4 /* IGNORE_TIMER_RESOLUTION, Windows 11 */;
        s.StateMask   = 0;  /* 0 = never throttle */
        BOOL ok = SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof(s));
        if (!ok) {
            /* Windows 10 rejects the Windows 11-only timer flag; retry without it. */
            s.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
            ok = SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof(s));
            LogLine("Power throttling opt-out: %s (execution speed only)", ok ? "ok" : "failed");
        } else {
            LogLine("Power throttling opt-out: ok");
        }
    }

    DWORD cls = NORMAL_PRIORITY_CLASS;
    if (g_cfg.processPriority == 1) cls = ABOVE_NORMAL_PRIORITY_CLASS;
    if (g_cfg.processPriority == 2) cls = HIGH_PRIORITY_CLASS;
    if (cls != NORMAL_PRIORITY_CLASS)
        LogLine("Process priority: %s", SetPriorityClass(GetCurrentProcess(), cls) ? "raised" : "failed");
}

/* ------------------------------------------------------------------ */
/* Optional: engine audio buffer (user Engine.ini)                    */
/*                                                                    */
/* UE's audio mixer buffer size isn't exposed in Windrose's menus. A  */
/* bigger buffer gives the mixer more slack, which stops dropouts and */
/* crackle on CPUs that can't always deliver audio on time.           */
/*                                                                    */
/* Windrose deletes the user Engine.ini again after reading it, so    */
/* the keys are written again at every launch, before the engine      */
/* reads its config. The first time, the original Engine.ini (if any) */
/* is copied to Engine.ini.WindroseFXCore.bak and the original values */
/* of the two keys are recorded in WindroseFXCore.state next to the   */
/* DLL. Turning AudioBufferFix (or Enabled) off restores those values */
/* on the next launch and removes the section/file if it becomes      */
/* empty.                                                             */
/* ------------------------------------------------------------------ */

static const wchar_t kAudioSection[] = L"/Script/WindowsTargetPlatform.WindowsTargetSettings";
static const wchar_t kKeyFrames[]    = L"AudioCallbackBufferFrameSize";
static const wchar_t kKeyQueue[]     = L"AudioNumBuffersToEnqueue";

/* %LOCALAPPDATA%\<Project>\Saved\Config\Windows\Engine.ini, where <Project> is
   the folder two levels above the DLL (...\Windrose\R5\Binaries\Win64 -> R5). */
static int EngineIniPath(wchar_t *out, size_t cap, wchar_t *dirOut, size_t dirCap)
{
    wchar_t tmp[MAX_PATH];
    wcsncpy(tmp, g_dir, MAX_PATH - 1); tmp[MAX_PATH - 1] = 0;
    for (int i = 0; i < 2; i++) { wchar_t *s = wcsrchr(tmp, L'\\'); if (!s) return 0; *s = 0; }
    wchar_t *proj = wcsrchr(tmp, L'\\');
    if (!proj || !proj[1]) return 0;
    proj++;

    wchar_t lad[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", lad, MAX_PATH);
    if (!n || n >= MAX_PATH) return 0;
    swprintf(dirOut, dirCap, L"%ls\\%ls\\Saved\\Config\\Windows", lad, proj);
    swprintf(out, cap, L"%ls\\Engine.ini", dirOut);
    return 1;
}

static int FileExists(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

static void StatePath(wchar_t *out, size_t cap) { swprintf(out, cap, L"%ls\\WindroseFXCore.state", g_dir); }

/* Create Saved\Config\Windows if the game hasn't yet (fresh install). */
static int EnsureDir(const wchar_t *dir)
{
    if (FileExists(dir)) return 1;
    wchar_t tmp[MAX_PATH];
    wcsncpy(tmp, dir, MAX_PATH - 1); tmp[MAX_PATH - 1] = 0;
    for (wchar_t *p = tmp + 3; *p; p++)
        if (*p == L'\\') { *p = 0; CreateDirectoryW(tmp, NULL); *p = L'\\'; }
    CreateDirectoryW(tmp, NULL);
    return FileExists(dir);
}

/* True when the file has no sections and nothing but whitespace (BOMs included). */
static int IniIsEmpty(const wchar_t *path)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    BYTE buf[512]; DWORD got = 0;
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
    int empty = sz.QuadPart <= (LONGLONG)sizeof(buf) && ReadFile(h, buf, sizeof(buf), &got, NULL);
    CloseHandle(h);
    for (DWORD i = 0; empty && i < got; i++)
        if (!(buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n' || buf[i] == 0 ||
              buf[i] == 0xEF || buf[i] == 0xBB || buf[i] == 0xBF || buf[i] == 0xFF || buf[i] == 0xFE))
            empty = 0;
    return empty;
}

static void ApplyAudioBuffer(void)
{
    wchar_t eng[MAX_PATH], dir[MAX_PATH], st[MAX_PATH], val[64];
    if (!EngineIniPath(eng, MAX_PATH, dir, MAX_PATH)) { LogLine("Audio buffer: could not work out the Engine.ini path"); return; }
    StatePath(st, MAX_PATH);

    if (!FileExists(st)) {
        /* First time: back up the player's Engine.ini and record the original values. */
        int existed = FileExists(eng);
        if (existed) {
            wchar_t bak[MAX_PATH];
            swprintf(bak, MAX_PATH, L"%ls.WindroseFXCore.bak", eng);
            if (!CopyFileW(eng, bak, TRUE) && GetLastError() != ERROR_FILE_EXISTS) {
                LogLine("Audio buffer: could not back up Engine.ini (error %lu); not changing it", GetLastError());
                return;
            }
        }
        const wchar_t *keys[2] = { kKeyFrames, kKeyQueue };
        for (int i = 0; i < 2; i++) {
            DWORD n = existed ? GetPrivateProfileStringW(kAudioSection, keys[i], L"\x01", val, 64, eng) : 0;
            int had = existed && !(n == 1 && val[0] == 1);
            wchar_t k[64];
            swprintf(k, 64, L"Had%ls", keys[i]);  WritePrivateProfileStringW(L"AudioBuffer", k, had ? L"1" : L"0", st);
            swprintf(k, 64, L"Orig%ls", keys[i]); WritePrivateProfileStringW(L"AudioBuffer", k, had ? val : L"", st);
        }
        WritePrivateProfileStringW(L"AudioBuffer", L"EngineIni", eng, st);
        if (!FileExists(st)) { LogLine("Audio buffer: could not write WindroseFXCore.state; not changing Engine.ini"); return; }
    }

    if (!EnsureDir(dir)) { LogLine("Audio buffer: config folder missing and could not be created"); return; }
    wchar_t f[16], q[16];
    swprintf(f, 16, L"%d", g_cfg.audioBufferFrames);
    swprintf(q, 16, L"%d", g_cfg.audioBuffersToEnqueue);
    BOOL ok = WritePrivateProfileStringW(kAudioSection, kKeyFrames, f, eng) &&
              WritePrivateProfileStringW(kAudioSection, kKeyQueue, q, eng);
    LogLine("Audio buffer: %s %d frames x %d buffers in Engine.ini",
            ok ? "set" : "FAILED to set", g_cfg.audioBufferFrames, g_cfg.audioBuffersToEnqueue);
}

static void RevertAudioBuffer(void)
{
    wchar_t st[MAX_PATH], eng[MAX_PATH], val[64], k[64];
    StatePath(st, MAX_PATH);
    if (!FileExists(st)) return;                       /* nothing of ours to undo */
    GetPrivateProfileStringW(L"AudioBuffer", L"EngineIni", L"", eng, MAX_PATH, st);

    if (eng[0] && FileExists(eng)) {
        const wchar_t *keys[2] = { kKeyFrames, kKeyQueue };
        for (int i = 0; i < 2; i++) {
            swprintf(k, 64, L"Had%ls", keys[i]);
            int had = (int)GetPrivateProfileIntW(L"AudioBuffer", k, 0, st);
            swprintf(k, 64, L"Orig%ls", keys[i]);
            GetPrivateProfileStringW(L"AudioBuffer", k, L"", val, 64, st);
            WritePrivateProfileStringW(kAudioSection, keys[i], had ? val : NULL, eng);
        }
        wchar_t sec[8];
        if (GetPrivateProfileSectionW(kAudioSection, sec, 8, eng) == 0)
            WritePrivateProfileStringW(kAudioSection, NULL, NULL, eng);      /* drop the empty section */
        if (IniIsEmpty(eng)) DeleteFileW(eng);
    }
    DeleteFileW(st);
    LogLine("Audio buffer: reverted Engine.ini to its original audio settings");
}

/* ------------------------------------------------------------------ */
/* Worker                                                             */
/* ------------------------------------------------------------------ */

static DWORD WINAPI Worker(LPVOID unused)
{
    (void)unused;
    /* This thread only scans and tunes; it must never compete with the game. */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    pGetThreadDescription = (PFN_GetThreadDescription)
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
    if (!pGetThreadDescription)
        pGetThreadDescription = (PFN_GetThreadDescription)
            GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "GetThreadDescription");

    DetectTopology();
    ApplyProcessSettings();

    KAFFINITY bg = BackgroundMask();
    LogLine("Critical LPs: game %d, render %d, rhi %d, audio %d | background mask 0x%llx%s",
            CriticalLP(T_GAME), CriticalLP(T_RENDER), CriticalLP(T_RHI), g_audioLP, (unsigned long long)bg,
            g_cfg.isolateBackgroundWorkers ? "" : " (not applied: IsolateBackgroundWorkers=0)");

    if (g_cfg.perfLogSeconds)
        LogLine("Perf log: every %d s. 100%% = one full core; a critical thread near 100%% is the bottleneck.",
                g_cfg.perfLogSeconds);

    ULONGLONG start = GetTickCount64(), lastScan = 0, lastPerf = start;
    for (;;) {
        ULONGLONG now = GetTickCount64();
        DWORD scanEvery = (now - start) < (ULONGLONG)g_cfg.scanFastSeconds * 1000 ? 1000 : (DWORD)g_cfg.scanSlowMs;
        if (!lastScan || now - lastScan >= scanEvery) {
            int n = ScanThreads();
            if (n && !g_cfg.verboseLog) LogLine("Scan: tuned %d new thread(s)", n);
            lastScan = now;
        }
        if (g_cfg.perfLogSeconds && now - lastPerf >= (ULONGLONG)g_cfg.perfLogSeconds * 1000) {
            PerfSample();
            lastPerf = now;
        }
        Sleep(g_cfg.perfLogSeconds ? 500 : scanEvery);
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

    wchar_t st[MAX_PATH];
    StatePath(st, MAX_PATH);
    int wantBuffer = g_cfg.enabled && g_cfg.audioBufferFix;
    if (!g_cfg.enabled && !FileExists(st)) return;     /* fully off, nothing to undo */

    wchar_t logPath[MAX_PATH];
    swprintf(logPath, MAX_PATH, L"%ls\\WindroseFXCore.log", g_dir);
    g_log = _wfopen(logPath, L"w");
    LogLine("WindroseFXCore " FXCORE_VERSION " loaded into %ls", exe);

    /* Done here, synchronously, so Engine.ini is in place before the engine
       reads its config. Only a few small kernel32 file calls. */
    if (wantBuffer) ApplyAudioBuffer(); else RevertAudioBuffer();
    if (!g_cfg.enabled) { LogLine("Enabled=0: everything else is off"); return; }

    HANDLE t = CreateThread(NULL, 0, Worker, NULL, 0, NULL);
    if (t) CloseHandle(t);
}
