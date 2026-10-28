/*
 * NX Control - tiny Windows tray app for Nothing Headphone (1) / Nothing Ear.
 *
 * Speaks the Nothing X binary protocol over Bluetooth RFCOMM (channel 15),
 * the same protocol the phone app uses. Protocol details come from three
 * MIT-licensed projects: omarchy-nothing-ear (Raphael Witz), nothing-toolbox
 * (Marc-Alexis) and nothing-x-mac (Younes Khelifi). See THIRD_PARTY_NOTICES.md.
 *
 * Features: noise control (High / Mid / Low / Adaptive / Transparency / Off),
 * equalizer presets, bass enhancement, low latency, dual connection, battery,
 * device picker, start with Windows, hotkeys.
 *
 * Unofficial. Not affiliated with or endorsed by Nothing Technology Limited.
 *
 * No background polling of the radio: the control channel opens only for a
 * status refresh or a setting change, then closes again.
 */
#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00

#include <winsock2.h>
#include <ws2bth.h>
#include <windows.h>
#include <bluetoothapis.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "resource.h"

/* ---------------------------------------------------------------- protocol */

#define SOF 0x55
#define RFCOMM_CHANNEL 15

#define CMD_DEVICE_INFO 0x06
#define CMD_BATTERY     0x07
#define CMD_ANC_GET     0x1E
#define CMD_ANC_SET     0x0F
#define CMD_LATENCY_GET 0x41
#define CMD_LATENCY_SET 0x40
#define CMD_EQ_GET      0x1F   /* preset: 0 balanced, 1 voice, 2 treble, 3 bass, 5 custom */
#define CMD_EQ_SET      0x10
#define CMD_ADVEQ_GET   0x4C   /* advanced (8-band) EQ switch */
#define CMD_ADVEQ_SET   0x4F
#define CMD_BASS_GET    0x4E   /* bass enhancement: [on, level*2] */
#define CMD_BASS_SET    0x51
#define CMD_DUAL_GET    0x27   /* dual connection (multipoint): 1 on, 0 off */
#define CMD_DUAL_SET    0x1A

#define EQ_BALANCED 0
#define EQ_VOICE    1
#define EQ_TREBLE   2
#define EQ_BASS     3
#define EQ_CUSTOM   5

#define DIR_GET      0xC0
#define DIR_SET      0xF0
#define DIR_RESPONSE 0x40
#define DIR_ACK      0x70

/* ANC wire values */
#define ANC_HIGH  1
#define ANC_MID   2
#define ANC_LOW   3
#define ANC_ADAPT 4
#define ANC_OFF   5
#define ANC_TRANS 7

/* Nothing X SPP service UUID, used only as a fallback if channel 15 fails. */
static const GUID NOTHING_SPP_GUID =
    {0xaeac4a03, 0xdff5, 0x498f, {0x84, 0x3a, 0x34, 0x48, 0x7c, 0xf1, 0x33, 0xeb}};

static unsigned short crc16(const unsigned char *d, int n) {
    unsigned short crc = 0xFFFF;
    for (int i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (unsigned short)((crc >> 1) ^ 0xA001) : (unsigned short)(crc >> 1);
    }
    return crc;
}

static int build_packet(unsigned char *out, int cmd, int dir, const unsigned char *payload, int plen) {
    unsigned short wire = (unsigned short)((cmd & 0xFF) | ((dir & 0xFF) << 8));
    out[0] = SOF;
    out[1] = 0x60; out[2] = 0x01;             /* control word 0x0160, CRC present */
    out[3] = wire & 0xFF; out[4] = wire >> 8;
    out[5] = plen & 0xFF; out[6] = (plen >> 8) & 0xFF;
    out[7] = 1;                               /* operation id */
    if (plen) memcpy(out + 8, payload, plen);
    unsigned short c = crc16(out, 8 + plen);
    out[8 + plen] = c & 0xFF;
    out[9 + plen] = c >> 8;
    return 10 + plen;
}

typedef struct {
    SOCKET s;
    unsigned char buf[4096];
    int len;
} Session;

/* Try to pull one complete frame out of the buffer. Returns 1 and fills
 * cmd/dir/payload when a frame is ready, 0 when more data is needed. */
static int next_frame(Session *ss, int *cmd, int *dir, unsigned char *payload, int *plen) {
    for (;;) {
        int start = -1;
        for (int i = 0; i < ss->len; i++) if (ss->buf[i] == SOF) { start = i; break; }
        if (start < 0) { ss->len = 0; return 0; }
        if (start > 0) { memmove(ss->buf, ss->buf + start, ss->len - start); ss->len -= start; }
        if (ss->len < 8) return 0;
        int ctrl = ss->buf[1] | (ss->buf[2] << 8);
        int command = ss->buf[3] | (ss->buf[4] << 8);
        int length = ss->buf[5] | (ss->buf[6] << 8);
        if (length > 1024) {                 /* garbage SOF, skip it */
            memmove(ss->buf, ss->buf + 1, ss->len - 1); ss->len -= 1; continue;
        }
        int total = 8 + length + ((ctrl & 0x20) ? 2 : 0);
        if (ss->len < total) return 0;
        *cmd = command & 0xFF;
        *dir = (command >> 8) & 0xFF;
        *plen = length;
        memcpy(payload, ss->buf + 8, length);
        memmove(ss->buf, ss->buf + total, ss->len - total);
        ss->len -= total;
        return 1;
    }
}

/* Send a request and wait for its response. Returns payload length, or -1. */
static int request(Session *ss, int cmd, int dir, const unsigned char *p, int plen, unsigned char *out) {
    unsigned char pkt[64];
    int n = build_packet(pkt, cmd, dir, p, plen);
    if (send(ss->s, (const char *)pkt, n, 0) != n) return -1;
    ULONGLONG deadline = GetTickCount64() + 1600;
    while (GetTickCount64() < deadline) {
        int c, d, l;
        while (next_frame(ss, &c, &d, out, &l))
            if (c == (cmd & 0xFF) && (d == DIR_RESPONSE || d == DIR_ACK)) return l;
        if (ss->len >= (int)sizeof(ss->buf) - 512) ss->len = 0;
        int r = recv(ss->s, (char *)ss->buf + ss->len, 512, 0);
        if (r == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAETIMEDOUT) continue;
            return -1;
        }
        if (r == 0) return -1;
        ss->len += r;
    }
    return -1;
}

static int bt_open(Session *ss, ULONGLONG addr, wchar_t *err, int errlen) {
    memset(ss, 0, sizeof *ss);
    for (int attempt = 0; attempt < 2; attempt++) {
        SOCKET s = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
        if (s == INVALID_SOCKET) {
            swprintf(err, errlen, L"Bluetooth socket unavailable (%d)", WSAGetLastError());
            return 0;
        }
        SOCKADDR_BTH sa;
        memset(&sa, 0, sizeof sa);
        sa.addressFamily = AF_BTH;
        sa.btAddr = addr;
        if (attempt == 0) sa.port = RFCOMM_CHANNEL;
        else { sa.serviceClassId = NOTHING_SPP_GUID; sa.port = 0; }
        if (connect(s, (SOCKADDR *)&sa, sizeof sa) == 0) {
            DWORD tv = 250;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);
            ss->s = s;
            return 1;
        }
        int e = WSAGetLastError();
        closesocket(s);
        swprintf(err, errlen, L"Could not reach headphones (error %d). Are they connected to this PC?", e);
    }
    return 0;
}

static void bt_close(Session *ss) {
    if (ss->s && ss->s != INVALID_SOCKET) {
        shutdown(ss->s, SD_BOTH);
        closesocket(ss->s);
    }
    ss->s = 0;
}

/* ------------------------------------------------------------------- state */

typedef struct {
    int ok;               /* protocol answered */
    int headset, left, right, kase;   /* battery %, -1 unknown */
    int headset_chg, left_chg, right_chg, kase_chg;
    int anc;              /* wire value, -1 unknown */
    int latency;          /* 1 on, 0 off, -1 unknown */
    int eq;               /* preset byte, -1 unknown */
    int adv_eq;           /* advanced 8-band EQ active */
    int bass_on, bass_level;   /* bass enhancement, level 1..5; bass_on -1 unknown */
    int dual;             /* dual connection 1/0, -1 unknown */
    wchar_t err[256];
    wchar_t done[128];    /* message for a completed change */
} Status;

typedef struct {
    ULONGLONG addr;
    int action;           /* 0 refresh, 1 anc, 2 latency, 3 eq, 4 bass, 5 dual */
    int value;
    int value2;           /* bass: level to keep when turning off */
} Job;

#define WM_TRAY   (WM_APP + 1)
#define WM_RESULT (WM_APP + 2)

#define ID_ANC_BASE    100
#define ID_LATENCY     200
#define ID_REFRESH     201
#define ID_AUTOSTART   202
#define ID_BTSETTINGS  203
#define ID_EXIT        204
#define ID_ABOUT       205
#define ID_DUAL        206
#define ID_EQ_BASE     220    /* + preset byte */
#define ID_BASS_OFF    230
#define ID_BASS_BASE   230    /* + level 1..5 */
#define ID_DEV_BASE    300
#define MAX_DEVS       16

#define HK_ANC_CYCLE 1
#define HK_LATENCY   2

static HINSTANCE g_inst;
static HWND g_wnd;
static NOTIFYICONDATAW g_nid;
static HICON g_icon_on, g_icon_off;
static Status g_status;
static volatile LONG g_busy;
static ULONGLONG g_addr;
static wchar_t g_name[248] = L"";
static UINT g_taskbar_created;
static int g_last_anc = ANC_HIGH;     /* remembered ANC level for the hotkey cycle */

static ULONGLONG g_dev_addr[MAX_DEVS];
static wchar_t g_dev_name[MAX_DEVS][248];
static int g_dev_connected[MAX_DEVS];
static int g_dev_count;

static const wchar_t *REG_KEY = L"Software\\NXControl";
static const wchar_t *RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t *RUN_VAL = L"NXControl";

static const wchar_t *anc_label(int v) {
    switch (v) {
    case ANC_HIGH: return L"Noise cancelling: High";
    case ANC_MID: return L"Noise cancelling: Mid";
    case ANC_LOW: return L"Noise cancelling: Low";
    case ANC_ADAPT: return L"Noise cancelling: Adaptive";
    case ANC_TRANS: return L"Transparency";
    case ANC_OFF: return L"Noise control off";
    default: return L"Noise control: unknown";
    }
}

static const wchar_t *eq_label(int v) {
    switch (v) {
    case EQ_BALANCED: return L"Balanced";
    case EQ_BASS: return L"More bass";
    case EQ_TREBLE: return L"More treble";
    case EQ_VOICE: return L"Voice";
    case EQ_CUSTOM: return L"Custom";
    default: return L"Unknown";
    }
}

static void parse_battery(Status *st, const unsigned char *p, int n) {
    if (n < 1) return;
    int count = p[0];
    for (int i = 0; i < count; i++) {
        int off = 1 + i * 2;
        if (off + 1 >= n) break;
        int comp = p[off], raw = p[off + 1];
        int level = raw & 0x7F, chg = (raw & 0x80) != 0;
        if (level > 100) continue;
        if (comp == 2) { st->left = level; st->left_chg = chg; }
        else if (comp == 3) { st->right = level; st->right_chg = chg; }
        else if (comp == 4) { st->kase = level; st->kase_chg = chg; }
        else if (comp == 6) { st->headset = level; st->headset_chg = chg; }
    }
}

static int parse_anc(const unsigned char *p, int n) {
    int mode = -1, level = -1;
    for (int off = 0; off + 1 < n; off += 3) {
        int kind = p[off], value = p[off + 1];
        if (kind == 1) mode = value;
        else if (kind == 2 && value >= 1 && value <= 4) level = value;
    }
    if (mode == 7) return ANC_TRANS;
    if (mode == 0 || mode == 5) return ANC_OFF;
    if (mode >= 1 && mode <= 4) return mode;
    if (mode != -1 && level != -1) return level;   /* "anc on" + level byte */
    return mode == -1 ? -1 : ANC_HIGH;
}

static DWORD WINAPI worker(LPVOID arg) {
    Job job = *(Job *)arg;
    HeapFree(GetProcessHeap(), 0, arg);

    Status *st = (Status *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Status));
    st->headset = st->left = st->right = st->kase = -1;
    st->anc = -1; st->latency = -1;
    st->eq = -1; st->bass_on = -1; st->bass_level = 0; st->dual = -1;

    Session ss;
    unsigned char out[1100];
    if (!bt_open(&ss, job.addr, st->err, 256)) goto finish;

    /* Activation probe: Nothing X always asks for device info first. */
    request(&ss, CMD_DEVICE_INFO, DIR_GET, NULL, 0, out);

    if (job.action == 1) {
        unsigned char pl[3] = {1, (unsigned char)job.value, 0};
        if (request(&ss, CMD_ANC_SET, DIR_SET, pl, 3, out) < 0)
            swprintf(st->err, 256, L"Headphones did not accept the noise mode change.");
        else
            swprintf(st->done, 128, L"%ls", anc_label(job.value));
        Sleep(350);
    } else if (job.action == 2) {
        unsigned char pl[1] = {(unsigned char)(job.value ? 1 : 2)};
        if (request(&ss, CMD_LATENCY_SET, DIR_SET, pl, 1, out) < 0)
            swprintf(st->err, 256, L"Headphones did not accept the low latency change.");
        else
            swprintf(st->done, 128, L"Low latency %ls", job.value ? L"ON" : L"OFF");
        Sleep(350);
    } else if (job.action == 3) {
        /* Same order as Nothing X: leave the advanced EQ first, then pick the preset. */
        int n = request(&ss, CMD_ADVEQ_GET, DIR_GET, NULL, 0, out);
        if (n > 0 && out[0] == 1) {
            unsigned char off[1] = {0};
            request(&ss, CMD_ADVEQ_SET, DIR_SET, off, 1, out);
        }
        unsigned char pl[1] = {(unsigned char)job.value};
        if (request(&ss, CMD_EQ_SET, DIR_SET, pl, 1, out) < 0)
            swprintf(st->err, 256, L"Headphones did not accept the equalizer change.");
        else
            swprintf(st->done, 128, L"Equalizer: %ls", eq_label(job.value));
        Sleep(250);
    } else if (job.action == 4) {
        int level = job.value ? job.value : (job.value2 >= 1 && job.value2 <= 5 ? job.value2 : 3);
        unsigned char pl[2] = {(unsigned char)(job.value ? 1 : 0), (unsigned char)(level * 2)};
        if (request(&ss, CMD_BASS_SET, DIR_SET, pl, 2, out) < 0)
            swprintf(st->err, 256, L"Headphones did not accept the bass enhancement change.");
        else if (job.value)
            swprintf(st->done, 128, L"Bass enhancement: level %d", job.value);
        else
            swprintf(st->done, 128, L"Bass enhancement OFF");
        Sleep(250);
    } else if (job.action == 5) {
        unsigned char pl[1] = {(unsigned char)(job.value ? 1 : 0)};
        if (request(&ss, CMD_DUAL_SET, DIR_SET, pl, 1, out) < 0)
            swprintf(st->err, 256, L"Headphones did not accept the dual connection change.");
        else
            swprintf(st->done, 128, L"Dual connection %ls", job.value ? L"ON" : L"OFF");
        Sleep(350);
    }

    int n = request(&ss, CMD_BATTERY, DIR_GET, NULL, 0, out);
    if (n >= 0) { st->ok = 1; parse_battery(st, out, n); }
    n = request(&ss, CMD_ANC_GET, DIR_GET, NULL, 0, out);
    if (n >= 0) { st->ok = 1; st->anc = parse_anc(out, n); }
    n = request(&ss, CMD_LATENCY_GET, DIR_GET, NULL, 0, out);
    if (n > 0) { st->ok = 1; st->latency = out[0] == 1; }
    n = request(&ss, CMD_EQ_GET, DIR_GET, NULL, 0, out);
    if (n > 0) { st->ok = 1; st->eq = out[0]; }
    n = request(&ss, CMD_ADVEQ_GET, DIR_GET, NULL, 0, out);
    if (n > 0) st->adv_eq = out[0] == 1;
    if (st->eq == 0x64) { st->eq = EQ_CUSTOM; st->adv_eq = 1; }
    n = request(&ss, CMD_BASS_GET, DIR_GET, NULL, 0, out);
    if (n > 1) {
        st->ok = 1;
        st->bass_on = out[0] == 1;
        st->bass_level = out[1] / 2;
        if (st->bass_level < 1 || st->bass_level > 5) st->bass_level = 5;
    }
    n = request(&ss, CMD_DUAL_GET, DIR_GET, NULL, 0, out);
    if (n > 0) { st->ok = 1; st->dual = out[0] == 1; }
    if (!st->ok && !st->err[0])
        swprintf(st->err, 256, L"Connected, but headphones did not answer. Try Refresh.");
    bt_close(&ss);

finish:
    PostMessageW(g_wnd, WM_RESULT, 0, (LPARAM)st);
    return 0;
}

static void start_job2(int action, int value, int value2) {
    if (!g_addr) {
        MessageBoxW(g_wnd, L"No Nothing device selected.\n\nPair your headphones with this PC in Bluetooth settings, then pick them under \"Device\".",
                    L"NX Control", MB_ICONINFORMATION);
        return;
    }
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) return;   /* one radio session at a time */
    Job *j = (Job *)HeapAlloc(GetProcessHeap(), 0, sizeof(Job));
    j->addr = g_addr; j->action = action; j->value = value; j->value2 = value2;
    HANDLE h = CreateThread(NULL, 0, worker, j, 0, NULL);
    if (h) CloseHandle(h); else { HeapFree(GetProcessHeap(), 0, j); g_busy = 0; }
}

static void start_job(int action, int value) { start_job2(action, value, 0); }

/* ----------------------------------------------------------------- devices */

static int looks_nothing(const wchar_t *n) {
    wchar_t low[248];
    wcsncpy(low, n, 247); low[247] = 0;
    _wcslwr(low);
    return wcsstr(low, L"nothing") || wcsstr(low, L"headphone (1)") || wcsstr(low, L"ear") || wcsstr(low, L"cmf");
}

static void scan_devices(void) {
    g_dev_count = 0;
    BLUETOOTH_DEVICE_SEARCH_PARAMS sp;
    memset(&sp, 0, sizeof sp);
    sp.dwSize = sizeof sp;
    sp.fReturnAuthenticated = TRUE;
    sp.fReturnRemembered = TRUE;
    sp.fReturnConnected = TRUE;
    sp.fReturnUnknown = FALSE;
    sp.fIssueInquiry = FALSE;
    BLUETOOTH_DEVICE_INFO di;
    memset(&di, 0, sizeof di);
    di.dwSize = sizeof di;
    HBLUETOOTH_DEVICE_FIND f = BluetoothFindFirstDevice(&sp, &di);
    if (!f) return;
    do {
        int dup = 0;
        for (int i = 0; i < g_dev_count; i++) if (g_dev_addr[i] == di.Address.ullLong) dup = 1;
        if (!dup && g_dev_count < MAX_DEVS) {
            g_dev_addr[g_dev_count] = di.Address.ullLong;
            wcsncpy(g_dev_name[g_dev_count], di.szName[0] ? di.szName : L"(unnamed)", 247);
            g_dev_connected[g_dev_count] = di.fConnected;
            g_dev_count++;
        }
        di.dwSize = sizeof di;
    } while (BluetoothFindNextDevice(f, &di));
    BluetoothFindDeviceClose(f);
}

static void save_device(void) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, L"Device", 0, REG_QWORD, (const BYTE *)&g_addr, sizeof g_addr);
        RegCloseKey(k);
    }
}

static void choose_device(void) {
    scan_devices();
    ULONGLONG saved = 0;
    DWORD sz = sizeof saved;
    RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"Device", RRF_RT_REG_QWORD, NULL, &saved, &sz);
    g_addr = 0; g_name[0] = 0;
    for (int i = 0; i < g_dev_count; i++)
        if (saved && g_dev_addr[i] == saved) { g_addr = saved; wcscpy(g_name, g_dev_name[i]); return; }
    for (int pass = 0; pass < 2 && !g_addr; pass++)
        for (int i = 0; i < g_dev_count; i++)
            if (looks_nothing(g_dev_name[i]) && (pass == 1 || g_dev_connected[i])) {
                g_addr = g_dev_addr[i]; wcscpy(g_name, g_dev_name[i]); break;
            }
    if (g_addr) save_device();
}

/* --------------------------------------------------------------- autostart */

static int autostart_enabled(void) {
    wchar_t v[MAX_PATH * 2];
    DWORD sz = sizeof v;
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, RUN_VAL, RRF_RT_REG_SZ, NULL, v, &sz) == ERROR_SUCCESS;
}

static void set_autostart(int on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_WRITE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t path[MAX_PATH], cmd[MAX_PATH + 4];
        GetModuleFileNameW(NULL, path, MAX_PATH);
        swprintf(cmd, MAX_PATH + 4, L"\"%ls\"", path);
        RegSetValueExW(k, RUN_VAL, 0, REG_SZ, (const BYTE *)cmd, (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, RUN_VAL);
    }
    RegCloseKey(k);
}

/* -------------------------------------------------------------------- tray */

static void fmt_battery(wchar_t *out, int n, const Status *st) {
    out[0] = 0;
    if (st->headset >= 0)
        swprintf(out, n, L"Battery %d%%%ls", st->headset, st->headset_chg ? L" (charging)" : L"");
    else if (st->left >= 0 || st->right >= 0) {
        swprintf(out, n, L"L %d%%  R %d%%", st->left, st->right);
        if (st->kase >= 0) {
            size_t l = wcslen(out);
            swprintf(out + l, n - l, L"  Case %d%%", st->kase);
        }
    }
}

static void update_tray(void) {
    wchar_t tip[128], bat[64];
    fmt_battery(bat, 64, &g_status);
    if (!g_addr)
        swprintf(tip, 128, L"NX Control\nNo Nothing device found");
    else if (!g_status.ok)
        swprintf(tip, 128, L"%ls\nNot connected", g_name);
    else
        swprintf(tip, 128, L"%ls\n%ls%ls%ls", g_name,
                 g_status.anc >= 0 ? anc_label(g_status.anc) : L"",
                 bat[0] ? L"\n" : L"", bat);
    wcsncpy(g_nid.szTip, tip, 127);
    g_nid.uFlags = NIF_TIP | NIF_ICON | NIF_SHOWTIP;
    g_nid.hIcon = g_status.ok ? g_icon_on : g_icon_off;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void balloon(const wchar_t *title, const wchar_t *text, int warn) {
    g_nid.uFlags = NIF_INFO;
    wcsncpy(g_nid.szInfoTitle, title, 63);
    wcsncpy(g_nid.szInfo, text, 255);
    g_nid.dwInfoFlags = (warn ? NIIF_WARNING : NIIF_INFO) | NIIF_NOSOUND | NIIF_RESPECT_QUIET_TIME;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void add_tray(void) {
    memset(&g_nid, 0, sizeof g_nid);
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_wnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_icon_off;
    wcscpy(g_nid.szTip, L"NX Control");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}

static int show_menu(void) {
    HMENU m = CreatePopupMenu();
    wchar_t line[300], bat[64];

    if (g_addr) {
        fmt_battery(bat, 64, &g_status);
        if (g_busy) swprintf(line, 300, L"%ls  -  working...", g_name);
        else if (g_status.ok) swprintf(line, 300, L"%ls%ls%ls", g_name, bat[0] ? L"  -  " : L"", bat);
        else swprintf(line, 300, L"%ls  -  not connected", g_name);
    } else {
        swprintf(line, 300, L"No Nothing device found");
    }
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, line);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);

    static const struct { int v; const wchar_t *t; } modes[] = {
        {ANC_HIGH, L"Noise cancelling - High"}, {ANC_MID, L"Noise cancelling - Mid"},
        {ANC_LOW, L"Noise cancelling - Low"}, {ANC_ADAPT, L"Noise cancelling - Adaptive"},
        {ANC_TRANS, L"Transparency"}, {ANC_OFF, L"Off"},
    };
    UINT dis = (g_addr && !g_busy) ? 0 : MF_GRAYED;
    for (int i = 0; i < 6; i++) {
        AppendMenuW(m, MF_STRING | dis, ID_ANC_BASE + modes[i].v, modes[i].t);
        if (g_status.anc == modes[i].v)
            CheckMenuRadioItem(m, ID_ANC_BASE + modes[i].v, ID_ANC_BASE + modes[i].v,
                               ID_ANC_BASE + modes[i].v, MF_BYCOMMAND);
    }
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    /* Equalizer submenu */
    HMENU eq = CreatePopupMenu();
    static const int eq_order[] = {EQ_BALANCED, EQ_BASS, EQ_TREBLE, EQ_VOICE, EQ_CUSTOM};
    for (int i = 0; i < 5; i++) {
        AppendMenuW(eq, MF_STRING | dis, ID_EQ_BASE + eq_order[i], eq_label(eq_order[i]));
        if (g_status.eq == eq_order[i] && !(g_status.adv_eq && eq_order[i] != EQ_CUSTOM))
            CheckMenuRadioItem(eq, ID_EQ_BASE + eq_order[i], ID_EQ_BASE + eq_order[i],
                               ID_EQ_BASE + eq_order[i], MF_BYCOMMAND);
    }
    AppendMenuW(eq, MF_SEPARATOR, 0, NULL);
    AppendMenuW(eq, MF_STRING | MF_GRAYED, 0, L"Custom = the curve you saved in the phone app");
    if (g_status.eq >= 0)
        swprintf(line, 300, L"Equalizer  (%ls)", g_status.adv_eq ? L"Custom" : eq_label(g_status.eq));
    else
        swprintf(line, 300, L"Equalizer");
    AppendMenuW(m, MF_POPUP | dis, (UINT_PTR)eq, line);

    /* Bass enhancement submenu */
    HMENU bass = CreatePopupMenu();
    AppendMenuW(bass, MF_STRING | dis, ID_BASS_OFF, L"Off");
    for (int lv = 1; lv <= 5; lv++) {
        wchar_t t[32];
        swprintf(t, 32, L"Level %d%ls", lv, lv == 1 ? L"  (light)" : lv == 5 ? L"  (max)" : L"");
        AppendMenuW(bass, MF_STRING | dis, ID_BASS_BASE + lv, t);
    }
    if (g_status.bass_on == 0)
        CheckMenuRadioItem(bass, ID_BASS_OFF, ID_BASS_OFF, ID_BASS_OFF, MF_BYCOMMAND);
    else if (g_status.bass_on == 1 && g_status.bass_level >= 1)
        CheckMenuRadioItem(bass, ID_BASS_BASE + g_status.bass_level, ID_BASS_BASE + g_status.bass_level,
                           ID_BASS_BASE + g_status.bass_level, MF_BYCOMMAND);
    if (g_status.bass_on == 1) swprintf(line, 300, L"Bass enhancement  (Level %d)", g_status.bass_level);
    else if (g_status.bass_on == 0) swprintf(line, 300, L"Bass enhancement  (Off)");
    else swprintf(line, 300, L"Bass enhancement");
    AppendMenuW(m, MF_POPUP | dis, (UINT_PTR)bass, line);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);

    AppendMenuW(m, MF_STRING | dis | (g_status.latency == 1 ? MF_CHECKED : 0), ID_LATENCY, L"Low latency mode");
    AppendMenuW(m, MF_STRING | dis | (g_status.dual == 1 ? MF_CHECKED : 0), ID_DUAL, L"Dual connection");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);

    HMENU dev = CreatePopupMenu();
    scan_devices();
    for (int i = 0; i < g_dev_count; i++) {
        swprintf(line, 300, L"%ls%ls", g_dev_name[i], g_dev_connected[i] ? L"  (connected)" : L"");
        AppendMenuW(dev, MF_STRING | (g_dev_addr[i] == g_addr ? MF_CHECKED : 0), ID_DEV_BASE + i, line);
    }
    if (!g_dev_count) AppendMenuW(dev, MF_STRING | MF_GRAYED, 0, L"No paired Bluetooth devices");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)dev, L"Device");
    AppendMenuW(m, MF_STRING | (g_busy ? MF_GRAYED : 0), ID_REFRESH, L"Refresh");
    AppendMenuW(m, MF_STRING, ID_BTSETTINGS, L"Bluetooth settings...");
    AppendMenuW(m, MF_STRING | (autostart_enabled() ? MF_CHECKED : 0), ID_AUTOSTART, L"Start with Windows");
    AppendMenuW(m, MF_STRING, ID_ABOUT, L"Shortcuts / About");
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_wnd);
    int chosen = (int)TrackPopupMenuEx(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RETURNCMD | TPM_NONOTIFY,
                                       pt.x, pt.y, g_wnd, NULL);
    PostMessageW(g_wnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    return chosen;
}

static void hotkey_cycle(void) {
    /* Ctrl+Alt+Shift+N: ANC -> Transparency -> Off -> ANC (last used level) */
    int next;
    if (g_status.anc >= ANC_HIGH && g_status.anc <= ANC_ADAPT) next = ANC_TRANS;
    else if (g_status.anc == ANC_TRANS) next = ANC_OFF;
    else next = g_last_anc;
    start_job(1, next);
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbar_created && g_taskbar_created) {   /* Explorer restarted */
        add_tray(); update_tray();
        return 0;
    }
    switch (msg) {
    case WM_TRAY:
        switch (LOWORD(lp)) {
        case WM_CONTEXTMENU:
        case WM_LBUTTONUP:
        case NIN_SELECT:
        case NIN_KEYSELECT:
        {
            int chosen = show_menu();
            if (chosen) SendMessageW(h, WM_COMMAND, chosen, 0);
            else if (!g_busy && g_addr) start_job(0, 0);   /* menu closed: quiet refresh */
            break;
        }
        }
        return 0;

    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id > ID_ANC_BASE && id <= ID_ANC_BASE + 7) {
            int v = id - ID_ANC_BASE;
            if (v >= ANC_HIGH && v <= ANC_ADAPT) g_last_anc = v;
            start_job(1, v);
        } else if (id == ID_LATENCY) {
            start_job(2, g_status.latency == 1 ? 0 : 1);
        } else if (id == ID_DUAL) {
            start_job(5, g_status.dual == 1 ? 0 : 1);
        } else if (id >= ID_EQ_BASE && id <= ID_EQ_BASE + EQ_CUSTOM) {
            start_job(3, id - ID_EQ_BASE);
        } else if (id >= ID_BASS_OFF && id <= ID_BASS_BASE + 5) {
            start_job2(4, id - ID_BASS_BASE, g_status.bass_level);
        } else if (id == ID_REFRESH) {
            choose_device();
            start_job(0, 0);
        } else if (id == ID_AUTOSTART) {
            set_autostart(!autostart_enabled());
        } else if (id == ID_BTSETTINGS) {
            ShellExecuteW(NULL, L"open", L"ms-settings:bluetooth", NULL, NULL, SW_SHOWNORMAL);
        } else if (id == ID_ABOUT) {
            MessageBoxW(h,
                L"NX Control\n\n"
                L"Click the tray icon to switch noise control and low latency.\n\n"
                L"Shortcuts (work from any app):\n"
                L"   Ctrl + Alt + Shift + N   Cycle ANC > Transparency > Off\n"
                L"   Ctrl + Alt + Shift + L   Toggle low latency\n\n"
                L"Headphones must be paired and connected to this PC.\n"
                L"Protocol from MIT-licensed projects: omarchy-nothing-ear,\n"
                L"nothing-toolbox and nothing-x-mac.\n"
                L"Unofficial. Not affiliated with Nothing Technology.",
                L"NX Control", MB_ICONINFORMATION);
        } else if (id == ID_EXIT) {
            DestroyWindow(h);
        } else if (id >= ID_DEV_BASE && id < ID_DEV_BASE + g_dev_count) {
            int i = id - ID_DEV_BASE;
            g_addr = g_dev_addr[i];
            wcscpy(g_name, g_dev_name[i]);
            save_device();
            memset(&g_status, 0, sizeof g_status);
            g_status.anc = g_status.latency = g_status.eq = g_status.bass_on = g_status.dual = -1;
            update_tray();
            start_job(0, 0);
        }
        return 0;
    }

    case WM_HOTKEY:
        if (wp == HK_ANC_CYCLE) hotkey_cycle();
        else if (wp == HK_LATENCY) start_job(2, g_status.latency == 1 ? 0 : 1);
        return 0;

    case WM_RESULT: {
        Status *st = (Status *)lp;
        g_status = *st;
        HeapFree(GetProcessHeap(), 0, st);
        InterlockedExchange(&g_busy, 0);
        if (g_status.anc >= ANC_HIGH && g_status.anc <= ANC_ADAPT) g_last_anc = g_status.anc;
        update_tray();
        if (g_status.done[0]) balloon(g_name, g_status.done, 0);
        else if (g_status.err[0] && wcsstr(g_status.err, L"accept")) balloon(L"NX Control", g_status.err, 1);
        return 0;
    }

    case WM_TIMER:
        if (!g_busy && g_addr) start_job(0, 0);
        return 0;

    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        UnregisterHotKey(h, HK_ANC_CYCLE);
        UnregisterHotKey(h, HK_LATENCY);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show) {
    (void)prev; (void)cmd; (void)show;
    HANDLE mutex = CreateMutexW(NULL, TRUE, L"NXControl.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"NX Control is already running.\nLook for the headphone icon near the clock (click ^ to see hidden icons).",
                    L"NX Control", MB_ICONINFORMATION);
        return 0;
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;

    g_inst = inst;
    g_icon_on = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_ON), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    g_icon_off = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_OFF), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);

    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.lpszClassName = L"NXControlWnd";
    wc.hIcon = g_icon_on;
    RegisterClassW(&wc);
    g_wnd = CreateWindowW(L"NXControlWnd", L"NX Control", 0, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

    g_status.anc = g_status.latency = g_status.eq = g_status.bass_on = g_status.dual = -1;
    g_status.headset = g_status.left = g_status.right = g_status.kase = -1;
    add_tray();
    choose_device();
    update_tray();

    RegisterHotKey(g_wnd, HK_ANC_CYCLE, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'N');
    RegisterHotKey(g_wnd, HK_LATENCY, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'L');

    if (!g_addr)
        balloon(L"NX Control", L"No Nothing device paired with this PC yet. Pair it in Bluetooth settings, then click the icon.", 1);
    else
        start_job(0, 0);

    SetTimer(g_wnd, 1, 10 * 60 * 1000, NULL);   /* battery refresh every 10 min */

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    WSACleanup();
    CloseHandle(mutex);
    return 0;
}
