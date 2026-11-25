// BonDriver_MB8CFW.cpp -- a BonDriver for tuners running MB8AC031-CFW.
//
// SPDX-License-Identifier: MIT
//
// Everything that talks to the device is libmb8cfw (C); this file is the
// BonDriver face on it:
//
//   OpenTuner        open the device, install CFW if it is not there
//                    (Firmware=)
//   SetChannel       tune, wait for lock, (re)start the stream
//   GetTsStream      whole 188-byte packets from the library's ring
//   GetSignalLevel   1.0 locked / 0.0 not (the C/N has no dB formula yet)
//
// Settings come from BonDriver_MB8CFW.ini beside the DLL (the DLL's own
// name with .ini, so copies under other names get their own file).
// docs/windows.md has the keys.

#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "IBonDriver.h"

extern "C" {
#include "mb8cfw/mb8cfw.h"
}

namespace {

const DWORD CHUNK = 188u * 256u;        // what one GetTsStream hands out
const ULONGLONG DIONE_SETTLE_MS = 1200;
const DWORD NO_CHANNEL = 0xFFFFFFFFu;

enum SpaceKind { NUMBERED, BS, CS };

struct Space {
    const wchar_t *name;
    SpaceKind kind;
    const char *prefix;                 // NUMBERED: mb8_channel_parse()
    const wchar_t *wprefix;             // spelling, and for the name
    unsigned first, last;               // NUMBERED only
};

// BS: transponders 1..23 (odd), each with BS_SLOTS entries "BS01/TS0" ..
// (the n-th TS the transponder lists; a slot it does not carry just does
// not lock).  CS: transponders 2..24 (even), one TS each.
const unsigned BS_SLOTS = 4;

// GV-M2TV: the built-in sequence is measured below 700 MHz only, and CATV
// pass-through not at all.
const Space SPACES[] = {
    { L"UHF",  NUMBERED, "",  L"",  13, 50 },
};
const DWORD NSPACES = sizeof(SPACES) / sizeof(SPACES[0]);

// PIX-DT295: the built-in terrestrial sequence is measured below 700 MHz
// only, and CATV pass-through not at all.  BS and CS: every transponder.
const Space SPACES_DT295[] = {
    { L"UHF",  NUMBERED, "",  L"",  13, 50 },
    { L"BS",   BS,       "",  L"",  0, 0 },
    { L"CS110", CS,      "",  L"",  0, 0 },
};

DWORD SpaceCount(const Space *s)
{
    switch (s->kind) {
    case BS: return 12u * BS_SLOTS;
    case CS: return 12u;
    default: return s->last - s->first + 1u;
    }
}

// The library's name for channel `ch` of space `s` (and the one shown).
void ChannelName(const Space *s, DWORD ch, char *name, size_t n,
                 wchar_t *wname, size_t wn)
{
    switch (s->kind) {
    case BS:
        snprintf(name, n, "BS%02u/TS%u", 1u + 2u * (unsigned)(ch / BS_SLOTS),
                 (unsigned)(ch % BS_SLOTS));
        if (wname)
            _snwprintf(wname, wn, L"BS%02u/TS%u",
                       1u + 2u * (unsigned)(ch / BS_SLOTS),
                       (unsigned)(ch % BS_SLOTS));
        break;
    case CS:
        snprintf(name, n, "CS%u", 2u + 2u * (unsigned)ch);
        if (wname)
            _snwprintf(wname, wn, L"ND%02u", 2u + 2u * (unsigned)ch);
        break;
    default:
        snprintf(name, n, "%s%u", s->prefix, s->first + (unsigned)ch);
        if (wname)
            _snwprintf(wname, wn, L"%ls%uch", s->wprefix,
                       s->first + (unsigned)ch);
        break;
    }
    if (n)
        name[n - 1] = '\0';
    if (wname && wn)
        wname[wn - 1] = L'\0';
}
const DWORD NSPACES_DT295 = sizeof(SPACES_DT295) / sizeof(SPACES_DT295[0]);

HMODULE g_module;

void Log(const char *fmt, ...)
{
    char line[512];
    va_list ap;

    va_start(ap, fmt);
    _vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    line[sizeof(line) - 2] = '\0';
    strcat(line, "\n");
    OutputDebugStringA("BonDriver_MB8CFW: ");
    OutputDebugStringA(line);
}

void LogInstall(void *, const char *line) { Log("install: %s", line); }

// The ini beside the DLL, and paths in it relative to the DLL's folder.
struct Config {
    char device[32];
    int index;
    char firmware[MAX_PATH];
    unsigned lockWaitMs;
    unsigned bufferMiB;
    wchar_t name[64];

    void Load()
    {
        wchar_t dll[MAX_PATH], ini[MAX_PATH], dir[MAX_PATH], w[MAX_PATH];
        wchar_t *dot, *slash;

        GetModuleFileNameW(g_module, dll, MAX_PATH);
        wcscpy(ini, dll);
        dot = wcsrchr(ini, L'.');
        if (dot != NULL)
            wcscpy(dot, L".ini");
        wcscpy(dir, dll);
        slash = wcsrchr(dir, L'\\');
        if (slash != NULL)
            slash[1] = L'\0';

        GetPrivateProfileStringW(L"MB8CFW", L"Device", L"", w, MAX_PATH, ini);
        WideCharToMultiByte(CP_UTF8, 0, w, -1, device, sizeof(device), NULL,
                            NULL);
        index = (int)GetPrivateProfileIntW(L"MB8CFW", L"Index", 0, ini);
        Path(ini, dir, L"Firmware", firmware);
        lockWaitMs = GetPrivateProfileIntW(L"MB8CFW", L"LockWaitMs", 2000,
                                           ini);
        bufferMiB = GetPrivateProfileIntW(L"MB8CFW", L"BufferMiB", 32, ini);
        GetPrivateProfileStringW(L"MB8CFW", L"Name", L"MB8AC031-CFW", name,
                                 64, ini);
    }

    // The library opens files with fopen(), which takes the ANSI code page:
    // go through the short path so a non-ASCII folder still works.
    static void Path(const wchar_t *ini, const wchar_t *dir,
                     const wchar_t *key, char out[MAX_PATH])
    {
        wchar_t v[MAX_PATH], full[MAX_PATH], shortp[MAX_PATH];

        out[0] = '\0';
        GetPrivateProfileStringW(L"MB8CFW", key, L"", v, MAX_PATH, ini);
        if (v[0] == L'\0')
            return;
        if (v[1] == L':' || (v[0] == L'\\' && v[1] == L'\\'))
            wcscpy(full, v);
        else
            _snwprintf(full, MAX_PATH, L"%ls%ls", dir, v);
        full[MAX_PATH - 1] = L'\0';
        if (GetShortPathNameW(full, shortp, MAX_PATH) == 0)
            wcscpy(shortp, full);
        WideCharToMultiByte(CP_ACP, 0, shortp, -1, out, MAX_PATH, NULL,
                            NULL);
    }
};

class CBonTuner;
CBonTuner *g_tuner;

class CBonTuner final : public IBonDriver2 {
public:
    CBonTuner() : m_dev(NULL), m_space(NO_CHANNEL), m_channel(NO_CHANNEL),
                  m_dioneLocked(0), m_discardUntil(0), m_buf(NULL)
    {
        memset(&m_cfg, 0, sizeof(m_cfg));
        m_cfg.Load();
        m_buf = (BYTE *)malloc(CHUNK);
        InitializeCriticalSection(&m_lock);
    }

    ~CBonTuner()
    {
        CloseTuner();
        free(m_buf);
        DeleteCriticalSection(&m_lock);
    }

    // --- IBonDriver ---------------------------------------------------

    const BOOL OpenTuner(void)
    {
        struct mb8_open_opts oo;
        int rc;

        if (m_dev != NULL)
            return TRUE;
        oo.model = m_cfg.device[0] ? m_cfg.device : NULL;
        oo.mailbox = MB8_BOX_MAIN;
        oo.index = m_cfg.index;
        if (mb8_open(&m_dev, &oo) != MB8_OK) {
            Log("open: %s", mb8_open_error());
            m_dev = NULL;
            return FALSE;
        }
        rc = mb8_ensure_firmware(m_dev, m_cfg.firmware, LogInstall, NULL);
        if (rc != MB8_OK) {
            Log("firmware: %s", mb8_error(m_dev));
            Close();
            return FALSE;
        }
        Log("opened %s", mb8_model_of(m_dev)->product);
        return TRUE;
    }

    void CloseTuner(void) { Close(); }

    const BOOL SetChannel(const BYTE channel)
    {
        // The IBonDriver (v1) numbering: 13..50 are the UHF channels.
        if (channel < 13 || channel > Sp()[0].last)
            return FALSE;
        return SetChannel(0, (DWORD)(channel - 13));
    }

    const float GetSignalLevel(void)
    {
        struct mb8_signal s;

        if (m_dev == NULL)
            return 0.0f;
        // This driver reports a lock flag, not a measured signal level.
        // Avoid waiting on the GV-M2TV mailbox from TVTest's poll loop.
        if (mb8_model_of(m_dev)->gen == MB8_GEN_DIONE)
            return InterlockedCompareExchange(&m_dioneLocked, 0, 0) ? 1.0f : 0.0f;
        if (m_channel == NO_CHANNEL)
            return 0.0f;
        if (mb8_signal(m_dev, &s) != MB8_OK)
            return 0.0f;
        return s.locked ? 1.0f : 0.0f;
    }

    const DWORD WaitTsStream(const DWORD timeoutMs)
    {
        DWORD timeout = timeoutMs ? timeoutMs : INFINITE;
        ULONGLONG end = GetTickCount64() + timeout;

        if (m_dev == NULL || !mb8_stream_running(m_dev))
            return WAIT_ABANDONED;
        for (;;) {
            if (mb8_stream_ready(m_dev) >= 188u)
                return WAIT_OBJECT_0;
            if (timeout != INFINITE && GetTickCount64() >= end)
                return WAIT_TIMEOUT;
            Sleep(5);
        }
    }

    const DWORD GetReadyCount(void)
    {
        if (m_dev == NULL || !mb8_stream_running(m_dev))
            return 0;
        return (DWORD)((mb8_stream_ready(m_dev) + CHUNK - 1) / CHUNK);
    }

    const BOOL GetTsStream(BYTE *dst, DWORD *size, DWORD *remaining)
    {
        BYTE *p = NULL;

        if (!GetTsStream(&p, size, remaining))
            return FALSE;
        if (*size)
            memcpy(dst, p, *size);
        return TRUE;
    }

    const BOOL GetTsStream(BYTE **dst, DWORD *size, DWORD *remaining)
    {
        long n;

        *size = 0;
        if (remaining)
            *remaining = 0;
        if (m_dev == NULL || m_buf == NULL || !mb8_stream_running(m_dev))
            return FALSE;
        *dst = m_buf;
        if (m_discardUntil != 0 && GetTickCount64() >= m_discardUntil) {
            mb8_stream_purge(m_dev);
            m_discardUntil = 0;
        }
        n = mb8_stream_read(m_dev, m_buf, CHUNK, 0);
        if (n < 0) {
            Log("stream: %s", mb8_error(m_dev));
            return FALSE;
        }
        if (remaining)
            *remaining = GetReadyCount();
        if (m_discardUntil != 0)
            return TRUE;
        *size = (DWORD)n;
        return TRUE;
    }

    void PurgeTsStream(void)
    {
        if (m_dev != NULL && mb8_stream_running(m_dev))
            mb8_stream_purge(m_dev);
    }

    void Release(void)
    {
        if (g_tuner == this)
            g_tuner = NULL;
        delete this;
    }

    // --- IBonDriver2 --------------------------------------------------

    LPCTSTR GetTunerName(void) { return m_cfg.name; }

    const BOOL IsTunerOpening(void) { return m_dev != NULL; }

    LPCTSTR EnumTuningSpace(const DWORD space)
    {
        return space < NSp() ? Sp()[space].name : NULL;
    }

    LPCTSTR EnumChannelName(const DWORD space, const DWORD channel)
    {
        char name[16];

        if (space >= NSp() || channel >= SpaceCount(&Sp()[space]))
            return NULL;
        ChannelName(&Sp()[space], channel, name, sizeof(name), m_chname,
                    16);
        return m_chname;
    }

    const BOOL SetChannel(const DWORD space, const DWORD channel)
    {
        struct mb8_stream_opts so;
        struct mb8_channel ch;
        char name[16];
        int rc;

        if (m_dev == NULL || space >= NSp() ||
            channel >= SpaceCount(&Sp()[space]))
            return FALSE;
        ChannelName(&Sp()[space], channel, name, sizeof(name), NULL, 0);
        if (mb8_channel_parse(name, &ch) != MB8_OK)
            return FALSE;

        EnterCriticalSection(&m_lock);
        // On GV-M2TV, the USB stream pump competes with tuning for the
        // same mailbox. Stop it first so the channel change can complete.
        const bool m2tv = mb8_model_of(m_dev)->gen == MB8_GEN_DIONE;
        if (m2tv)
            InterlockedExchange(&m_dioneLocked, 0);
        if (m2tv && mb8_stream_running(m_dev) &&
            mb8_stream_stop(m_dev) != MB8_OK) {
            Log("stop before tuning: %s", mb8_error(m_dev));
            LeaveCriticalSection(&m_lock);
            return FALSE;
        }
        rc = mb8_tune_channel(m_dev, &ch, m_cfg.lockWaitMs);
        if (rc < 0) {
            Log("tune %s: %s", name, mb8_error(m_dev));
            LeaveCriticalSection(&m_lock);
            return FALSE;
        }
        Log("tune %s (%s%u kHz): %s", name, ch.sat ? "IF " : "", ch.khz,
            rc ? "locked" : "no lock");
        if ((!m2tv || rc != 0) && !mb8_stream_running(m_dev)) {
            memset(&so, 0, sizeof(so));
            so.buffer_bytes = (size_t)m_cfg.bufferMiB << 20;
            // A small device ring bounds the previous channel's backlog.
            if (m2tv)
                so.ring_mib = 1;
            if (mb8_stream_start(m_dev, &so) != MB8_OK) {
                Log("stream: %s", mb8_error(m_dev));
                LeaveCriticalSection(&m_lock);
                return FALSE;
            }
        }
        if (mb8_stream_running(m_dev))
            mb8_stream_purge(m_dev);
        m_space = space;
        m_channel = channel;
        // Drain data from the previous tune before exposing the new TS.
        m_discardUntil = m2tv && rc != 0 ? GetTickCount64() + DIONE_SETTLE_MS : 0;
        if (m2tv)
            InterlockedExchange(&m_dioneLocked, rc != 0);
        LeaveCriticalSection(&m_lock);
        // A channel with no lock is still "set": the host shows the level
        // and decides.  That is what other BonDrivers do too.
        return TRUE;
    }

    const DWORD GetCurSpace(void) { return m_space; }
    const DWORD GetCurChannel(void) { return m_channel; }

private:
    void Close()
    {
        if (m_dev == NULL)
            return;
        mb8_close(m_dev);       // stops the stream (STOP) first
        m_dev = NULL;
        m_space = m_channel = NO_CHANNEL;
        InterlockedExchange(&m_dioneLocked, 0);
    }

    Config m_cfg;
    mb8_dev *m_dev;
    // The channel spaces of the open model (all of them before open).
    bool Dt295(void) const
    {
        return m_dev != NULL &&
               (mb8_model_of(m_dev)->can & MB8_CAN_SAT) != 0;
    }
    const Space *Sp(void) const { return Dt295() ? SPACES_DT295 : SPACES; }
    DWORD NSp(void) const { return Dt295() ? NSPACES_DT295 : NSPACES; }

    DWORD m_space, m_channel;
    volatile LONG m_dioneLocked;
    ULONGLONG m_discardUntil;
    BYTE *m_buf;
    wchar_t m_chname[16];
    CRITICAL_SECTION m_lock;
};

} // namespace

extern "C" IBonDriver *CreateBonDriver(void)
{
    if (g_tuner == NULL)
        g_tuner = new CBonTuner();
    return g_tuner;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = inst;
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}
