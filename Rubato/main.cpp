// Rubato - a native Win32 MIDI file processor (humanizes MIDI files so they sound played, not programmed).
//
//   * multi-track view: track list (M / S / channel / patch / FX / volume) with a coloured strip per track,
//     notes and parameter line-graphs (pitch bend, expression, modulation, breath, volume, velocity) drawn on the strips
//   * per-track MFX chains edited in one dialog (the "FX" button of a track): insert / delete / reorder effects,
//     mute (bypass), bar ranges (All / Exclude / Use / From / To), presets (open / save), "Apply All FXs"
//   * NON-DESTRUCTIVE: chains are a live preview (graphs + playback + export) and only change the song
//     when you press "Apply All FXs" / "Apply all FXs"
//   * real-time playback through any Windows MIDI output (winmm)
// (c) 2026, O.Jagannatha Rao (JagiChan)
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#ifndef _RICHEDIT_VER
#define _RICHEDIT_VER 0x0800                 // CHARFORMAT2 and the rich-text 5.0 class
#endif
#include <richedit.h>
#include <olectl.h>                       // OleLoadPicture (splash screen JPEG)
#include <shlwapi.h>                      // SHCreateMemStream
#include <shellapi.h>
#include <algorithm>
#include <mmsystem.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include "midi_core.h"
#include "midi_fx.h"
#include "player.h"

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#define WM_ROLL_CLICKED (WM_APP + 2)
#define APP_VERSION L"1.4.0"

static const wchar_t* APP_TITLE   = L"Rubato";
static const wchar_t* MAIN_CLASS  = L"RubatoMain";
static const wchar_t* ARR_CLASS   = L"RubatoArrange";
static const wchar_t* DLG_CLASS   = L"RubatoChainDlg";

enum {
    ID_OPEN = 101, ID_SAVE, ID_EXIT, ID_UNDO, ID_COMMITALL, ID_BYPASS, ID_CHAIN, ID_COPYALL, ID_HELP, ID_ABOUT, ID_SELCLEAR,
    ID_PLAY = 120, ID_STOP, ID_HOME, ID_LOOP,
    ID_GHOST = 130, ID_TRK_BIGGER, ID_TRK_SMALLER, ID_ZOOMIN, ID_ZOOMOUT,
    ID_LANE0 = 140,                       // ID_LANE0 .. ID_LANE0 + 5
    ID_DEV = 150, ID_BEND, ID_TOOLBAR, ID_ARRANGE,
    // MFX chain dialog
    IDC_TB = 300, IDC_CLOSE, IDC_LIST, IDC_INSERT, IDC_DELETE, IDC_APPLYALL, IDC_UP, IDC_DOWN,
    IDC_ALL, IDC_EXC, IDC_USEM, IDC_FROM, IDC_TO, IDC_FROMSEL, IDC_REROLL, IDC_DESC,
    IDT_PRESETS = 330, IDT_SAVE, IDT_HELP,
    IDC_PARAM0 = 340,                     // IDC_PARAM0 .. IDC_PARAM0 + 7
    ID_FXMENU0 = 400,                     // + effect type
    ID_PRESET_BROWSE = 499,
    ID_PRESETMENU0 = 500,                 // + preset index
    ID_ABOUT_OK = 700, ID_ABOUT_LINK, ID_ABOUT_LICENSE = 710,
    ID_PATCH_FILE = 1000,                 // Patch column menu: "as in the file"
    ID_PATCH0 = 1001,                     // + General MIDI program 0..127
    ID_SAVEAS = 1200, ID_EXPORT, ID_OPENPROJ, ID_RECENT_CLEAR, ID_TRK_DELETE,
    ID_MK_INSERT = 1210, ID_MK_EDIT, ID_MK_DELETE, ID_MK_CLEAR,
    ID_RECENT0 = 1300,                    // + 0..9: the "Open recent" entries
    ID_CHAN0 = 1400,                      // + 0..15: channel menu
    IDC_SPIN0 = 360,                      // + parameter index: spinner of a parameter box
    IDC_SPINFROM = 380, IDC_SPINTO, ID_BENDSPIN
};

// ===========================================================================
//  Data
// ===========================================================================
enum Lane { LANE_BEND, LANE_EXPR, LANE_MOD, LANE_BREATH, LANE_VOL, LANE_VEL, LANE_COUNT };

struct LaneMeta { const wchar_t* label; const wchar_t* button; COLORREF col; int def; };
static const LaneMeta LM[LANE_COUNT] = {
    { L"Pitch",  L"PITCH",  RGB(255, 190, 70),  8192 },
    { L"Expr",   L"EXP",    RGB(100, 215, 255), 127 },
    { L"Mod",    L"MOD",    RGB(215, 140, 255), 0 },
    { L"Breath", L"BREATH", RGB(120, 240, 180), 0 },
    { L"Vol",    L"VOL",    RGB(250, 235, 100), 100 },
    { L"Vel",    L"VEL",    RGB(255, 125, 145), -1 },
};
static const int LANE_CC[LANE_COUNT] = { -1, 11, 1, 2, 7, -1 };

typedef std::vector<std::pair<uint32_t, int> > Pts;     // (tick, value), sorted by tick
typedef mc::FxInstance ChainItem;
typedef mc::FxChain Chain;

struct Marker { uint32_t tick = 0; std::string text; };            // text marker of the Marker Track (alphanumeric only)

struct Snapshot {
    mc::Song base; std::vector<Chain> chains;
    std::vector<char> muted, solo; std::vector<int> trkVol, trkPan, trkProg;   // so that deleting a track can be undone
};
struct PresetEntry { std::wstring category, label, path; Chain items; bool user = false; };

// per-track data used for drawing (computed from the preview song)
struct TrackVD {
    std::vector<mc::Note> notes;
    int minKey = 60, maxKey = 72, ch = 0, prog = -1, vol = -1, pan = -1;
    double bendScale = 50.0;
    Pts cur[LANE_COUNT];
};

struct DlgState {
    HWND hwnd = NULL, tb = NULL, close = NULL, list = NULL, insert = NULL, del = NULL, applyAll = NULL;
    HWND spin[mc::FX_MAXP] = {}, spinFrom = NULL, spinTo = NULL;
    HWND up = NULL, down = NULL, rbAll = NULL, rbExc = NULL, rbUse = NULL, lblFrom = NULL, edFrom = NULL;
    HWND lblTo = NULL, edTo = NULL, reroll = NULL, desc = NULL;
    HWND lbl[mc::FX_MAXP] = {}, ed[mc::FX_MAXP] = {};
    int  track = -1, focusParam = -1;
    bool refreshing = false;
    HWND fromSel = NULL;
};

struct App {
    HWND hwnd = NULL, hTb = NULL, hArr = NULL, hDev = NULL, hBend = NULL, hLoop = NULL;
    HWND hStatus = NULL, hProc = NULL, hGhost = NULL, hLblDev = NULL, hLblBend = NULL, hLblTrk = NULL;
    HWND hBtnHome = NULL, hBtnPlay = NULL, hBtnStop = NULL, hTrkMinus = NULL, hTrkPlus = NULL;
    HWND hZoomMinus = NULL, hZoomPlus = NULL, hLblZoom = NULL, hBendSpin = NULL;
    HWND hLane[LANE_COUNT] = {};
    HMENU hMenu = NULL;
    HFONT font = NULL, fontBig = NULL;
    HBRUSH brRed = NULL;
    bool procOn = false;                  // some track has an effect in progress: the indicator says "Rubatoing"

    mc::Song base;                        // committed song data
    mc::Song preview;                     // base + per-track FX chains (what you see, hear and export)
    std::vector<Chain> chains;            // one effect chain per track
    std::vector<char> muted, solo;
    std::vector<int> trkVol, trkPan;      // mixer overrides per track (-1 = as in the file)
    std::vector<int> trkProg;             // instrument (General MIDI program) chosen in the Patch column (-1 = as in the file)
    std::vector<mc::TempoSeg> tempo;
    std::vector<Snapshot> undo;
    std::vector<PresetEntry> presets;
    std::wstring path;                    // the MIDI file the song came from
    std::wstring projPath;                // the .rpj project file (empty = not saved yet)
    std::vector<Marker> markers;          // Marker Track
    int selMarker = -1;
    std::vector<std::wstring> recent;     // most recently opened files (MIDI or project), newest first
    double bendRange = 2.0;
    bool loaded = false, dirty = false, bypassAll = false;
    Player player;
    DlgState dlg;
};
static App g;
static const wchar_t* RICH_CLASS = L"RICHEDIT50W";   // class name of the control in Msftedit.dll
static bool s_bendSilent = false;         // true while the program itself sets the bend box (project load): no recompute from EN_CHANGE
static bool g_rich = false;               // the rich-text control (Msftedit.dll) is available: coloured help text

// ===========================================================================
//  View state
// ===========================================================================
static const int INFOH = 86, HDRH = 20, TOPH = INFOH + HDRH, RULH = 20;      // info panel (two rows of graph values), header, ruler
static const int MRKH = 24, TRKTOP = TOPH + MRKH;                             // the fixed Marker Track row sits between the ruler and the tracks
static const int NCOL = 9;                                                    // N Name Mute Solo Ch Patch FX Vol Pan
static int COLW[NCOL] = { 24, 92, 40, 40, 26, 96, 40, 52, 56 };               // column widths (drag the header dividers to change)
static const int COLMIN[NCOL] = { 18, 40, 34, 34, 22, 40, 30, 30, 30 };
static int COLX[NCOL + 1] = {};
static int LPW = 0;                                                           // width of the track list = sum of the columns
static void RecalcColumns() {
    COLX[0] = 0;
    for (int c = 0; c < NCOL; c++) COLX[c + 1] = COLX[c] + COLW[c];
    LPW = COLX[NCOL];
}

struct ViewState {
    double   pxPerBeat = 40.0;
    int      rowH = 72, scrollY = 0;
    uint32_t scrollTick = 0, lenTicks = 0, startTick = 0, playTick = 0;
    bool     playing = false;
    int      sel = -1;
    int      tsNum = 4, tsDen = 4, ppq = 480;
    long     hoverTick = -1;
    int      hoverRow = -1;
    bool     tracking = false;
    uint32_t selStart = 0, selEnd = 0;                  // timeline selection (active when selEnd > selStart)
    uint32_t selAnchor = 0;
    bool     dragSel = false;
    int      dragCol = -1, dragStartX = 0, dragStartW = 0;      // column divider being dragged
    int      dragSlider = 0, dragRow = -1;                      // 7 = volume slider, 8 = pan slider
    std::vector<uint32_t> phraseMarks;                  // phrase starts of the selected track (shown when phrase effects are active)
    bool     laneOn[LANE_COUNT] = { true, true, true, true, false, true };
    bool     showGhost = true;
    int      ghostTrack = -1;
    std::vector<mc::Note> ghostNotes;
    Pts      ghost[LANE_COUNT];
};
static ViewState V;
static std::vector<TrackVD> TV;

static const char* const GM_NAMES_A[128] = {
    "Acoustic Grand Piano", "Bright Acoustic Piano", "Electric Grand Piano", "Honky-tonk Piano", "Electric Piano 1",
    "Electric Piano 2", "Harpsichord", "Clavinet", "Celesta", "Glockenspiel", "Music Box", "Vibraphone", "Marimba",
    "Xylophone", "Tubular Bells", "Dulcimer", "Drawbar Organ", "Percussive Organ", "Rock Organ", "Church Organ",
    "Reed Organ", "Accordion", "Harmonica", "Tango Accordion", "Nylon Guitar", "Steel Guitar", "Jazz Guitar",
    "Clean Guitar", "Muted Guitar", "Overdriven Guitar", "Distortion Guitar", "Guitar Harmonics", "Acoustic Bass",
    "Fingered Bass", "Picked Bass", "Fretless Bass", "Slap Bass 1", "Slap Bass 2", "Synth Bass 1", "Synth Bass 2",
    "Violin", "Viola", "Cello", "Contrabass", "Tremolo Strings", "Pizzicato Strings", "Orchestral Harp", "Timpani",
    "String Ensemble 1", "String Ensemble 2", "Synth Strings 1", "Synth Strings 2", "Choir Aahs", "Voice Oohs",
    "Synth Voice", "Orchestra Hit", "Trumpet", "Trombone", "Tuba", "Muted Trumpet", "French Horn", "Brass Section",
    "Synth Brass 1", "Synth Brass 2", "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax", "Oboe", "English Horn",
    "Bassoon", "Clarinet", "Piccolo", "Flute", "Recorder", "Pan Flute", "Blown Bottle", "Shakuhachi", "Whistle",
    "Ocarina", "Square Lead", "Sawtooth Lead", "Calliope Lead", "Chiff Lead", "Charang Lead", "Voice Lead",
    "Fifths Lead", "Bass + Lead", "New Age Pad", "Warm Pad", "Polysynth Pad", "Choir Pad", "Bowed Pad",
    "Metallic Pad", "Halo Pad", "Sweep Pad", "Rain FX", "Soundtrack FX", "Crystal FX", "Atmosphere FX",
    "Brightness FX", "Goblins FX", "Echoes FX", "Sci-Fi FX", "Sitar", "Banjo", "Shamisen", "Koto", "Kalimba",
    "Bagpipe", "Fiddle", "Shanai", "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock", "Taiko Drum", "Melodic Tom",
    "Synth Drum", "Reverse Cymbal", "Guitar Fret Noise", "Breath Noise", "Seashore", "Bird Tweet", "Telephone Ring",
    "Helicopter", "Applause", "Gunshot"
};

// ===========================================================================
//  Small helpers
// ===========================================================================
static void SetStatus(const wchar_t* fmt, ...) {
    wchar_t b[512];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf(b, 511, fmt, ap); b[511] = 0;
    va_end(ap);
    if (g.hStatus) SetWindowTextW(g.hStatus, b);
}

static std::string WToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}
static std::wstring Utf8ToW(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::wstring TrackName(const mc::Track& t, int idx) {
    for (const mc::Event& e : t.events)
        if (e.IsMeta() && e.d1 == 0x03 && !e.data.empty()) {
            int n = MultiByteToWideChar(CP_ACP, 0, (const char*)e.data.data(), (int)e.data.size(), NULL, 0);
            if (n > 0) {
                std::wstring w((size_t)n, L'\0');
                MultiByteToWideChar(CP_ACP, 0, (const char*)e.data.data(), (int)e.data.size(), &w[0], n);
                while (!w.empty() && w.back() == 0) w.pop_back();
                if (!w.empty()) return w;
            }
        }
    wchar_t b[32]; _snwprintf(b, 32, L"Track%02d", idx + 1);
    return b;
}

static bool ReadAll(const std::wstring& p, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > (256LL << 20)) { CloseHandle(h); return false; }
    out.resize((size_t)sz.QuadPart);
    DWORD rd = 0; BOOL ok = TRUE;
    if (!out.empty()) ok = ReadFile(h, out.data(), (DWORD)out.size(), &rd, NULL);
    CloseHandle(h);
    return ok && rd == out.size();
}

static bool WriteAll(const std::wstring& p, const void* d, size_t n) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    BOOL ok = WriteFile(h, d, (DWORD)n, &wr, NULL);
    CloseHandle(h);
    return ok && wr == n;
}

static std::wstring FileStem(const std::wstring& path) {
    size_t s = path.find_last_of(L"\\/");
    std::wstring f = path.substr(s == std::wstring::npos ? 0 : s + 1);
    size_t d = f.find_last_of(L'.');
    return d == std::wstring::npos ? f : f.substr(0, d);
}

static std::wstring ExeDir() {
    wchar_t b[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, b, MAX_PATH * 2);
    std::wstring p(b, n);
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"." : p.substr(0, s);
}

static void Fill(HDC dc, int l, int t, int r, int b, COLORREF c) {
    if (r <= l || b <= t) return;
    RECT rc = { l, t, r, b };
    SetBkColor(dc, c);
    ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &rc, NULL, 0, NULL);
}
static void Frame(HDC dc, int l, int t, int r, int b, COLORREF c) {
    Fill(dc, l, t, r, t + 1, c); Fill(dc, l, b - 1, r, b, c);
    Fill(dc, l, t, l + 1, b, c); Fill(dc, r - 1, t, r, b, c);
}
static void Txt(HDC dc, int x, int y, const std::wstring& s, COLORREF c) {
    SetTextColor(dc, c);
    TextOutW(dc, x, y, s.c_str(), (int)s.size());
}
static void TxtIn(HDC dc, RECT r, const std::wstring& s, COLORREF c, UINT fmt) {
    SetTextColor(dc, c);
    DrawTextW(dc, s.c_str(), (int)s.size(), &r, fmt | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
}

// reads a number from an edit box; false when the box holds no number (empty, "-" ...)
static bool EditNumber(HWND ed, double& v) {
    wchar_t b[64] = L""; GetWindowTextW(ed, b, 64);
    wchar_t* end = b; v = wcstod(b, &end);
    return end != b;
}
static void SetEditNumber(HWND ed, double v) { wchar_t b[40]; _snwprintf(b, 40, L"%g", v); SetWindowTextW(ed, b); }

// ===========================================================================
//  Base64, recent files, alphanumeric text prompt
// ===========================================================================
static std::string B64Enc(const std::vector<uint8_t>& d) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o; o.reserve((d.size() + 2) / 3 * 4);
    for (size_t i = 0; i < d.size(); i += 3) {
        uint32_t v = (uint32_t)d[i] << 16;
        if (i + 1 < d.size()) v |= (uint32_t)d[i + 1] << 8;
        if (i + 2 < d.size()) v |= d[i + 2];
        o += T[(v >> 18) & 63]; o += T[(v >> 12) & 63];
        o += (i + 1 < d.size()) ? T[(v >> 6) & 63] : '=';
        o += (i + 2 < d.size()) ? T[v & 63] : '=';
    }
    return o;
}
static bool B64Dec(const std::string& s, std::vector<uint8_t>& out) {
    out.clear(); uint32_t acc = 0; int bits = 0;
    for (char c : s) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62; else if (c == '/') v = 63;
        else if (c == '=' || c == '\r' || c == '\n' || c == ' ') continue;
        else return false;
        acc = (acc << 6) | (uint32_t)v; bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((uint8_t)((acc >> bits) & 0xFF)); }
    }
    return true;
}

static bool IsAlnumAscii(wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z'); }
static std::string SanitizeAlnum(const std::string& in) {          // marker text: letters and digits only
    std::string o;
    for (char c : in) if (IsAlnumAscii((wchar_t)(unsigned char)c) && o.size() < 32) o += c;
    return o;
}

// ---- recent files: kept in HKCU\Software\Rubato (Recent0 .. Recent9), newest first
static const int MAX_RECENT = 10;
static HMENU s_recentMenu = NULL;

static void LoadRecent() {
    g.recent.clear();
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Rubato", 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    for (int i = 0; i < MAX_RECENT; i++) {
        wchar_t name[16], buf[MAX_PATH * 2]; DWORD sz = sizeof buf, type = 0;
        _snwprintf(name, 16, L"Recent%d", i);
        if (RegQueryValueExW(k, name, NULL, &type, (LPBYTE)buf, &sz) == ERROR_SUCCESS && type == REG_SZ && buf[0]) {
            buf[MAX_PATH * 2 - 1] = 0; g.recent.push_back(buf);
        }
    }
    RegCloseKey(k);
}
static void SaveRecent() {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Rubato", 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS) return;
    for (int i = 0; i < MAX_RECENT; i++) {
        wchar_t name[16]; _snwprintf(name, 16, L"Recent%d", i);
        if (i < (int)g.recent.size())
            RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)g.recent[(size_t)i].c_str(), (DWORD)((g.recent[(size_t)i].size() + 1) * sizeof(wchar_t)));
        else RegDeleteValueW(k, name);
    }
    RegCloseKey(k);
}
static std::wstring FullPath(const std::wstring& p) {
    wchar_t b[MAX_PATH * 2]; DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH * 2, b, NULL);
    return (n > 0 && n < MAX_PATH * 2) ? std::wstring(b, n) : p;
}
static void RebuildRecentMenu() {
    if (!s_recentMenu) return;
    while (GetMenuItemCount(s_recentMenu) > 0) DeleteMenu(s_recentMenu, 0, MF_BYPOSITION);
    if (g.recent.empty()) AppendMenuW(s_recentMenu, MF_STRING | MF_GRAYED, 0, L"(empty)");
    for (size_t i = 0; i < g.recent.size(); i++) {
        std::wstring p = g.recent[i];
        if (p.size() > 64) p = L"..." + p.substr(p.size() - 60);
        std::wstring t; for (wchar_t c : p) { t += c; if (c == L'&') t += L'&'; }          // '&' must not become a mnemonic
        wchar_t pre[16]; _snwprintf(pre, 16, L"&%d  ", (int)((i + 1) % 10));
        AppendMenuW(s_recentMenu, MF_STRING, (UINT_PTR)(ID_RECENT0 + (int)i), (pre + t).c_str());
    }
    AppendMenuW(s_recentMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(s_recentMenu, MF_STRING | (g.recent.empty() ? MF_GRAYED : 0), ID_RECENT_CLEAR, L"&Clear list");
    if (g.hwnd) DrawMenuBar(g.hwnd);
}
static void AddRecent(const std::wstring& path) {
    const std::wstring full = FullPath(path);
    for (size_t i = 0; i < g.recent.size();)
        if (_wcsicmp(g.recent[i].c_str(), full.c_str()) == 0) g.recent.erase(g.recent.begin() + (long)i); else i++;
    g.recent.insert(g.recent.begin(), full);
    if ((int)g.recent.size() > MAX_RECENT) g.recent.resize(MAX_RECENT);
    SaveRecent(); RebuildRecentMenu();
}
static void RemoveRecent(const std::wstring& path) {
    for (size_t i = 0; i < g.recent.size();)
        if (_wcsicmp(g.recent[i].c_str(), path.c_str()) == 0) g.recent.erase(g.recent.begin() + (long)i); else i++;
    SaveRecent(); RebuildRecentMenu();
}

// ---- small modal prompt for the text of a marker: the box accepts letters and digits only (typing and pasting)
static const wchar_t* PROMPT_CLASS = L"RubatoPrompt";
struct PromptState { std::wstring title, label, text; bool ok = false; HWND edit = NULL; };
static PromptState* s_prompt = NULL;

static LRESULT CALLBACK AlnumEditProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_CHAR) {
        const wchar_t c = (wchar_t)w;
        if (c >= 32 && !IsAlnumAscii(c)) { MessageBeep(MB_OK); return 0; }          // control keys (backspace ...) pass
    } else if (m == WM_PASTE) {
        std::wstring in;
        if (OpenClipboard(h)) {
            HANDLE hd = GetClipboardData(CF_UNICODETEXT);
            if (hd) { const wchar_t* t = (const wchar_t*)GlobalLock(hd); if (t) { in = t; GlobalUnlock(hd); } }
            CloseClipboard();
        }
        std::wstring f; for (wchar_t c : in) if (IsAlnumAscii(c)) f += c;
        if (!f.empty()) SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)f.c_str());
        else MessageBeep(MB_OK);
        return 0;
    }
    return DefSubclassProc(h, m, w, l);
}

static LRESULT CALLBACK PromptProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h, DWORD ex = 0) {
            HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, hwnd, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.font, TRUE); return c;
        };
        mk(L"STATIC", s_prompt->label.c_str(), SS_LEFT, 0, 14, 12, 330, 32);
        s_prompt->edit = mk(L"EDIT", s_prompt->text.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, 100, 14, 48, 316, 24, WS_EX_CLIENTEDGE);
        SendMessageW(s_prompt->edit, EM_LIMITTEXT, 32, 0);
        SetWindowSubclass(s_prompt->edit, AlnumEditProc, 1, 0);
        mk(L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK, 232, 86, 84, 26);
        mk(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL, 326, 86, 84, 26);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            wchar_t b[64] = L""; GetWindowTextW(s_prompt->edit, b, 64);
            std::wstring t; for (const wchar_t* c = b; *c; c++) if (IsAlnumAscii(*c)) t += *c;
            if (t.empty()) { MessageBeep(MB_ICONEXCLAMATION); SetFocus(s_prompt->edit); return 0; }
            s_prompt->text = t; s_prompt->ok = true; DestroyWindow(hwnd);
        } else if (LOWORD(wp) == IDCANCEL) DestroyWindow(hwnd);
        return 0;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool PromptAlnum(const wchar_t* title, const wchar_t* label, const std::string& initial, std::string& out) {
    PromptState st; st.title = title; st.label = label; st.text = Utf8ToW(SanitizeAlnum(initial));
    s_prompt = &st;
    const int cw = 424, ch = 126; const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    RECT r = { 0, 0, cw, ch }; AdjustWindowRectEx(&r, style, FALSE, WS_EX_DLGMODALFRAME);
    RECT mr; GetWindowRect(g.hwnd, &mr);
    const int w = r.right - r.left, h = r.bottom - r.top;
    const int x = mr.left + ((mr.right - mr.left) - w) / 2, y = mr.top + ((mr.bottom - mr.top) - h) / 2;
    EnableWindow(g.hwnd, FALSE);
    HWND a = CreateWindowExW(WS_EX_DLGMODALFRAME, PROMPT_CLASS, title, style, x, y, w, h, g.hwnd, NULL, GetModuleHandleW(NULL), NULL);
    if (a) {
        ShowWindow(a, SW_SHOW); UpdateWindow(a); SetFocus(st.edit); SendMessageW(st.edit, EM_SETSEL, 0, -1);
        MSG m;
        while (IsWindow(a) && GetMessage(&m, NULL, 0, 0) > 0)
            if (!IsDialogMessage(a, &m)) { TranslateMessage(&m); DispatchMessage(&m); }
    }
    EnableWindow(g.hwnd, TRUE); SetForegroundWindow(g.hwnd);
    s_prompt = NULL;
    if (st.ok) out = WToUtf8(st.text);
    return st.ok;
}

// ===========================================================================
//  Effect chains and presets
// ===========================================================================
static int NewSeed() {
    static unsigned s = (unsigned)GetTickCount() * 2654435761u + 12345u;
    s = s * 1664525u + 1013904223u;
    return 1 + (int)((s >> 8) % 999999u);
}

static int FxIndexById(const std::wstring& id) {
    for (int i = 0; i < mc::FX_COUNT; i++) if (id == mc::GetFxDesc(i).id) return i;
    return -1;
}

static ChainItem MakeItem(int type) {
    ChainItem c; c.type = type;
    const mc::FxDesc& d = mc::GetFxDesc(type);
    for (int k = 0; k < mc::FX_MAXP; k++) c.p[k] = d.def[k];
    c.seed = NewSeed();
    return c;
}

static ChainItem MkItem(const wchar_t* id, std::initializer_list<double> v) {
    ChainItem c = MakeItem(std::max(0, FxIndexById(id)));
    size_t k = 0;
    for (double x : v) if (k < (size_t)mc::FX_MAXP) c.p[k++] = x;
    return c;
}

static std::string SerializePreset(const std::wstring& name, const Chain& items) {
    std::string s = "Rubato-Preset 4\r\nname=" + WToUtf8(name) + "\r\n";
    for (const ChainItem& it : items) {
        s += "fx=" + WToUtf8(mc::GetFxDesc(it.type).id) + ";" + (it.enabled ? "1" : "0");
        for (int k = 0; k < mc::FX_MAXP; k++) { char b[48]; snprintf(b, sizeof b, ";%.6g", it.p[k]); s += b; }
        char r[64]; snprintf(r, sizeof r, ";%d;%d;%d", it.mode, it.fromBar, it.toBar);
        s += r; s += "\r\n";
    }
    return s;
}

static bool ParsePreset(const std::string& txt, std::wstring& name, Chain& items) {
    items.clear(); name.clear();
    size_t pos = 0; bool header = false; int version = 1;
    while (pos <= txt.size()) {
        size_t e = txt.find('\n', pos);
        if (e == std::string::npos) e = txt.size();
        std::string line = txt.substr(pos, e - pos);
        pos = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) {
            if (!header) {
                if (line.compare(0, 13, "MidiFX-Preset") != 0 && line.compare(0, 13, "Rubato-Preset") != 0) return false;   // both spellings: old files still load
                header = true; version = atoi(line.c_str() + 13);
            }
            else if (line.compare(0, 5, "name=") == 0) name = Utf8ToW(line.substr(5));
            else if (line.compare(0, 3, "fx=") == 0) {
                std::vector<std::string> tok; size_t a = 3;
                for (;;) {
                    size_t b = line.find(';', a);
                    tok.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
                    if (b == std::string::npos) break;
                    a = b + 1;
                }
                int type = tok.size() >= 2 ? FxIndexById(Utf8ToW(tok[0])) : -1;
                const bool oldLayout = version < 3 && (type == mc::FX_HUMANCC || type == mc::FX_BREATH || type == mc::FX_QUANTIZE);
                if (type >= 0 && !oldLayout) {            // effects whose parameters changed since the old file format are skipped                                       // unknown effects are skipped
                    ChainItem c = MakeItem(type);
                    c.enabled = atoi(tok[1].c_str()) != 0;
                    for (int k = 0; k < mc::FX_MAXP && (size_t)(k + 2) < tok.size(); k++) c.p[k] = strtod(tok[(size_t)k + 2].c_str(), NULL);
                    if (type == mc::FX_AUTOPHRASE && version < 4)                 // phrase lengths were eighth notes, now sixteenth notes
                        for (int k = 0; k < 3; k++) c.p[k] *= 2.0;
                    for (int k = 0; k < mc::GetFxDesc(type).np; k++) c.p[k] = mc::ClampParam(type, k, c.p[k]);   // stay within min / max
                    const size_t r = 2 + (size_t)mc::FX_MAXP;
                    if (version >= 2 && tok.size() >= r + 3) {
                        c.mode = std::max(0, std::min(2, atoi(tok[r].c_str())));
                        c.fromBar = std::max(1, atoi(tok[r + 1].c_str())); c.toBar = std::max(1, atoi(tok[r + 2].c_str()));
                    } else if (version < 2 && tok.size() >= r + 4) {          // old format: all;exc;from;useBars
                        bool all = atoi(tok[r].c_str()) != 0, exc = atoi(tok[r + 1].c_str()) != 0;
                        c.mode = all ? mc::RANGE_ALL : (exc ? mc::RANGE_EXC : mc::RANGE_USE);
                        c.fromBar = std::max(1, atoi(tok[r + 2].c_str()));
                        c.toBar = c.fromBar + std::max(1, atoi(tok[r + 3].c_str())) - 1;
                    }
                    items.push_back(c);
                }
            }
        }
        if (e >= txt.size()) break;
    }
    return header;
}

static std::wstring PresetDir() { return ExeDir() + L"\\Presets"; }

static void BuildFactoryPresets(std::vector<PresetEntry>& out) {
    using std::wstring;
    auto add = [&](const wchar_t* cat, const wchar_t* name, Chain c) {
        PresetEntry e; e.category = cat; e.label = name; e.items = c; out.push_back(e);
    };
    // short names for the effects:  AP = Phrase Finder {min, max, pause (in 1/16 notes), sens, prefer}   HU = Humanize {ms, vel, len%, seed}
    //   VE = Velocity {scale, offset}   QU = Quantizer {grid, strength, offset, swing, durGrid, reduce}
    //   PM = Human Pitch/Mod/Expr {drift, vibrato, scoop, scoopOn, glide, fall, mod, longMs, expr%, seed}
    //   BR = Breath & Phrase Dynamics {depth, arc, pulse, turb, gapMs, gapDip, velArc, output, seed}
    //   ST = Strum {spread ms, direction, window ms, taper %, random ms, seed}
    auto AP = [](double a, double b, double c, double d, double e) { return MkItem(L"autophrase", { a, b, c, d, e }); };
    auto HU = [](double a, double b, double c) { return MkItem(L"humanize", { a, b, c, 0 }); };
    auto VE = [](double a, double b) { return MkItem(L"velocity", { a, b }); };
    auto QU = [](double a, double b, double c, double d, double e, double f) { return MkItem(L"quantize", { a, b, c, d, e, f }); };
    auto PM = [](double a, double b, double c, double d, double e, double f, double g_, double h) { return MkItem(L"humancc", { a, b, c, d, e, f, g_, h, 0, 0 }); };
    auto BR = [](double a, double b, double c, double d, double e, double f, double g_, double h) { return MkItem(L"breath", { a, b, c, d, e, f, g_, h, 0 }); };
    auto ST = [](double a, double b, double c, double d, double e) { return MkItem(L"strum", { a, b, c, d, e, 0 }); };

    const wchar_t* W = L"Wind - flutes and pipes";
    add(W, L"Flute - legato breathy (matches the reference sample)", Chain{ AP(12,20,4,40,3), HU(2,3,1), PM(4,0,61,2,0,0,0,450), BR(100,25,100,0,76,70,22,1) });
    add(W, L"Flute - lyrical, song-length lines", Chain{ AP(24,44,4,40,3), HU(3,4,2), PM(4,7,55,2,0,0,35,500), BR(85,35,80,15,90,65,18,3) });
    add(W, L"Flute - soft studio", Chain{ AP(24,48,4,40,3), HU(2,2,1), PM(3,5,35,2,0,0,25,500), BR(60,30,60,10,70,55,12,3) });
    add(W, L"Bansuri (Indian bamboo flute)", Chain{ AP(20,40,4,50,3), HU(4,5,3), PM(5,9,75,2,18,35,40,450), BR(90,45,90,40,95,70,22,3) });
    add(W, L"Piccolo", Chain{ AP(16,28,4,40,3), HU(2,4,1), PM(3,4,30,2,0,0,20,400), BR(70,30,70,15,55,60,18,3) });
    add(W, L"Recorder / whistle", Chain{ AP(16,32,4,40,3), HU(3,4,2), PM(3,2,20,2,0,0,10,500), BR(65,30,60,45,60,60,14,3) });
    add(W, L"Pan flute (breathy)", Chain{ AP(16,32,4,40,3), HU(4,5,3), PM(5,5,45,2,0,25,25,450), BR(90,40,85,70,70,65,18,3) });
    add(W, L"Ocarina", Chain{ AP(16,32,4,40,3), HU(3,3,2), PM(4,6,40,1,0,0,15,500), BR(60,35,50,10,60,55,10,1) });

    const wchar_t* R = L"Reed - sax, clarinet, oboe, accordion family";
    add(R, L"Alto sax - expressive", Chain{ AP(16,32,4,50,3), HU(5,5,3), PM(5,18,40,2,0,60,50,600), BR(85,45,90,30,85,70,24,3) });
    add(R, L"Tenor sax - warm", Chain{ AP(16,32,4,50,3), HU(6,5,3), PM(5,20,35,2,0,70,55,650), BR(80,45,85,35,90,70,22,3) });
    add(R, L"Soprano sax", Chain{ AP(16,32,4,50,3), HU(4,5,3), PM(4,22,40,2,0,50,60,550), BR(80,40,80,25,80,65,22,3) });
    add(R, L"Baritone sax - growly", Chain{ AP(16,32,4,50,3), HU(6,6,3), PM(4,14,30,2,0,75,45,650), BR(80,45,90,45,95,70,24,3) });
    add(R, L"Clarinet - smooth", Chain{ AP(20,40,4,40,3), HU(3,5,2), PM(3,3,10,1,0,0,0,600), BR(90,60,50,15,60,60,22,3) });
    add(R, L"Oboe", Chain{ AP(20,40,4,40,3), HU(3,4,2), PM(4,12,20,2,0,0,35,500), BR(70,40,60,20,60,60,16,3) });
    add(R, L"English horn / bassoon", Chain{ AP(20,40,4,40,3), HU(4,4,2), PM(4,10,25,2,0,0,30,550), BR(70,40,60,20,70,60,16,3) });
    add(R, L"Harmonica", Chain{ AP(12,24,4,40,3), HU(4,6,3), MkItem(L"humancc", { 4, 3, 50, 3, 0, 30, 20, 400, 25, 0 }), BR(70,35,70,50,45,55,20,3) });
    add(R, L"Shehnai (Indian reed)", Chain{ AP(16,32,4,50,3), HU(5,5,3), PM(6,16,45,2,25,40,50,450), BR(85,45,85,30,70,65,22,3) });
    add(R, L"Accordion / harmonium (bellows)", Chain{ AP(16,32,4,40,3), HU(4,5,2), ST(3,3,25,8,2), PM(2,0,0,0,0,0,0,600), BR(60,60,25,0,35,60,14,1) });

    // ---- presets derived from the reference sample MIDI files (raw file -> expressive file, measured with this program's own effects:
    //      velocity level / spread, pitch-bend depth, expression and modulation range, chord spread)
    add(R, L"Sax - crying: scoops and falls (matches the reference sample)", Chain{ AP(16,32,4,50,3), VE(100,-3), HU(2,14,0), PM(0,14,130,1,25,80,50,300), BR(100,70,40,0,105,75,4,1) });
    add(R, L"Sax - legato, soft attack (matches the reference sample)", Chain{ AP(16,32,4,50,3), VE(98,0), HU(2,10,10), PM(2,3,65,1,0,0,95,650), BR(100,70,40,0,0,55,0,1) });
    add(R, L"Sax - chords and fills (matches the reference sample)", Chain{ AP(16,32,4,50,3), VE(100,3), HU(2,17,4), PM(0,0,55,1,4,0,45,475), BR(100,70,25,0,60,45,4,1) });
    add(R, L"Accordion - lyrical solo (matches the reference sample)", Chain{ AP(16,32,4,40,3), VE(92,-2), HU(3,7,3), PM(0,4,90,1,6,5,15,475), BR(90,70,45,0,0,40,6,1) });
    add(R, L"Accordion - rhythm fill (matches the reference sample)", Chain{ HU(3,10,6), ST(10,3,25,3,0), PM(4,3,0,0,0,0,0,550) });

    const wchar_t* B = L"Brass";
    add(B, L"Trumpet - bright", Chain{ AP(16,28,4,50,3), HU(4,6,2), PM(4,6,30,2,0,60,25,700), BR(80,40,95,10,85,65,26,3) });
    add(B, L"Trombone - slides", Chain{ AP(16,28,4,50,3), HU(5,6,3), PM(4,5,25,3,18,50,20,700), BR(75,40,85,10,90,65,22,3) });
    add(B, L"French horn - warm", Chain{ AP(20,40,4,40,3), HU(6,5,3), PM(3,3,15,1,0,0,10,700), BR(70,50,40,10,100,60,20,3) });
    add(B, L"Brass section", Chain{ AP(16,32,4,40,3), HU(8,6,3), PM(3,4,25,1,0,40,0,700), BR(70,40,70,5,60,60,22,1) });

    const wchar_t* S = L"Strings - bowed";
    add(S, L"Violin - expressive", Chain{ AP(16,32,4,50,3), HU(5,5,3), PM(4,22,15,0,12,0,60,350), BR(55,40,60,0,25,35,16,1) });
    add(S, L"Viola", Chain{ AP(16,32,4,50,3), HU(5,5,3), PM(4,18,15,0,12,0,55,400), BR(55,40,60,0,30,35,16,1) });
    add(S, L"Cello - deep", Chain{ AP(16,32,4,50,3), HU(6,5,3), PM(4,16,10,0,15,0,50,450), BR(55,40,55,0,35,35,16,1) });
    add(S, L"Contrabass", Chain{ AP(16,32,4,50,3), HU(8,5,3), PM(3,6,10,0,10,0,20,600), BR(50,30,50,0,40,30,14,1) });
    add(S, L"String ensemble - slow swells", Chain{ AP(32,64,4,40,3), HU(14,5,3), PM(3,8,0,0,0,0,25,500), BR(50,50,70,0,0,0,14,1) });
    add(S, L"Pizzicato strings", Chain{ HU(6,10,5) });
    add(S, L"Strings - soft, gentle vibrato (matches the reference sample)", Chain{ AP(32,64,4,40,3), VE(100,3), HU(1,1,0), PM(6,18,35,0,2,0,105,300), BR(100,70,0,0,90,30,4,1) });
    add(S, L"Strings - chords and fills (matches the reference sample)", Chain{ AP(32,64,4,40,3), VE(100,2), HU(1,24,2), PM(7,4,5,1,0,0,20,475), BR(100,70,45,0,8,35,6,1) });

    const wchar_t* P = L"Plucked - guitar, harp, sitar";
    add(P, L"Guitar solo - frets gliding (matches the reference sample)", Chain{ AP(12,20,4,40,3), VE(80,0), HU(4,36,0), PM(2,0,0,0,10,0,60,600) });
    add(P, L"Guitar solo - lyrical, bends and slides (matches the reference sample)", Chain{ AP(12,20,4,40,3), VE(95,0), HU(4,2,2), PM(3,10,55,0,22,35,100,500) });
    add(P, L"Guitar solo - vibrato and bending (matches the reference sample)", Chain{ AP(12,20,4,40,3), VE(100,-6), HU(4,19,1), PM(7,30,40,0,25,60,90,375) });
    add(P, L"Guitar solo - slow attack, swells (matches the reference sample)", Chain{ AP(12,20,4,40,3), VE(95,0), HU(4,2,4), PM(4,3,40,3,8,5,90,300), BR(100,70,100,0,60,80,0,1) });
    add(P, L"Guitar - tight strumming (matches the reference sample)", Chain{ VE(95,-16), HU(1,20,4), ST(3,1,20,0,3) });
    add(P, L"Guitar - rhythm strumming (matches the reference sample)", Chain{ VE(100,-3), HU(1,19,3), ST(3,1,20,0,3) });
    add(P, L"Guitar - lyric rhythm fill, spread chords (matches the reference sample)", Chain{ VE(100,-4), HU(2,17,2), ST(16,3,60,5,10) });
    add(P, L"Guitar - lyrical accompaniment", Chain{ HU(8,5,3), ST(12,3,25,15,3), PM(2,0,30,3,10,0,0,600) });
    add(P, L"Guitar - pop / rock accompaniment", Chain{ HU(6,6,3), ST(6,1,25,12,2), PM(1,0,25,3,0,0,0,600) });
    add(P, L"Guitar - strumming", Chain{ HU(8,7,3), ST(9,3,25,18,3) });
    add(P, L"Nylon / acoustic guitar", Chain{ HU(7,5,3), ST(10,3,25,14,3), PM(2,0,35,3,12,0,60,500) });
    add(P, L"Bass guitar", Chain{ HU(4,5,3), PM(2,0,25,3,15,0,0,600) });
    add(P, L"Sitar (meend slides)", Chain{ HU(5,6,3), PM(3,0,45,3,35,0,0,600) });
    add(P, L"Harp", Chain{ HU(6,5,3), ST(12,1,40,15,4) });

    const wchar_t* MA = L"Mallets - vibraphone, marimba";
    add(MA, L"Vibraphone - natural dynamics (matches the reference sample)", Chain{ VE(90,-2), HU(1,24,10) });

    const wchar_t* K = L"Keys and voice";
    add(K, L"Piano - gentle human feel", Chain{ HU(8,7,3), ST(4,3,25,8,2) });
    add(K, L"Organ", Chain{ HU(3,0,2) });
    add(K, L"Choir / voice oohs", Chain{ AP(24,48,4,40,3), HU(12,5,3), PM(3,12,30,1,0,0,45,450), BR(65,50,70,15,90,60,16,3) });
    add(K, L"Vocal - Indian gamaka style", Chain{ AP(20,40,4,40,3), HU(6,5,3), PM(4,16,50,2,28,30,55,400), BR(80,45,80,25,90,65,20,3) });

    const wchar_t* G = L"General";
    add(G, L"Any instrument - gentle human feel", Chain{ HU(8,6,3) });
    add(G, L"Any instrument - loose, laid-back", Chain{ HU(14,8,5) });
    add(G, L"Tidy up: quantize to 1/16 (60%)", Chain{ QU(16,60,0,0,0,0) });
    add(G, L"Swing the eighth notes", Chain{ QU(8,100,0,33,0,0) });
}

static void ScanPresets() {
    g.presets.clear();
    BuildFactoryPresets(g.presets);
    // *.rps are Rubato preset files; *.mfxp is the old extension and is still read
    for (const wchar_t* pat : { L"*.rps", L"*.mfxp" }) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((PresetDir() + L"\\" + pat).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            std::wstring path = PresetDir() + L"\\" + fd.cFileName;
            std::vector<uint8_t> b;
            if (!ReadAll(path, b)) continue;
            PresetEntry e; std::wstring name;
            if (!ParsePreset(std::string(b.begin(), b.end()), name, e.items)) continue;
            e.category = L"User presets"; e.label = name.empty() ? FileStem(path) : name;
            e.path = path; e.user = true;
            g.presets.push_back(e);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

// ===========================================================================
//  Non-destructive pipeline:  base --(per-track chain)--> preview --> TrackVD
// ===========================================================================
static bool ChainActive(int i) {
    if (g.bypassAll || i < 0 || i >= (int)g.chains.size()) return false;
    for (const ChainItem& c : g.chains[(size_t)i]) if (c.enabled) return true;
    return false;
}

static void RecomputeTrack(int i) {
    g.preview.tracks[(size_t)i] = g.base.tracks[(size_t)i];
    if (ChainActive(i)) mc::ApplyChain(g.preview.tracks[(size_t)i], g.base, g.chains[(size_t)i]);
}

static void BuildCurves(const mc::Track& t, int ch, Pts* out) {
    for (int k = 0; k < LANE_COUNT; k++) out[k].clear();
    for (const mc::Event& e : t.events) {
        if (!e.IsChannel() || e.Channel() != ch) continue;
        int hi = e.status & 0xF0;
        if (hi == 0xE0) out[LANE_BEND].push_back(std::make_pair(e.tick, (int)(e.d1 | (e.d2 << 7))));
        else if (hi == 0xB0) {
            for (int k = 0; k < LANE_COUNT; k++) if (LANE_CC[k] == e.d1) out[k].push_back(std::make_pair(e.tick, (int)e.d2));
        } else if (e.IsNoteOn()) out[LANE_VEL].push_back(std::make_pair(e.tick, (int)e.d2));
    }
}

static void ComputeBendScale(int i) {
    double mx = 0;
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1 && V.ghostTrack != i) break;
        const Pts& c = pass == 0 ? TV[(size_t)i].cur[LANE_BEND] : V.ghost[LANE_BEND];
        for (const auto& p : c) mx = std::max(mx, std::fabs((p.second - 8192) * g.bendRange * 100.0 / 8192.0));
    }
    static const double nice[] = { 25, 50, 100, 200, 400, 800, 1200, 2400 };
    TV[(size_t)i].bendScale = 2400;
    for (double n : nice) if (mx <= n) { TV[(size_t)i].bendScale = n; break; }
}

static void UpdateTrackView(int i) {
    TrackVD& d = TV[(size_t)i];
    d.notes.clear();
    std::vector<mc::Event> o;
    const mc::Track& t = g.preview.tracks[(size_t)i];
    mc::ExtractNotes(t, d.notes, o);
    int cnt[16] = {}; d.minKey = 127; d.maxKey = 0;
    for (const mc::Note& n : d.notes) { cnt[n.ch & 15]++; d.minKey = std::min<int>(d.minKey, n.key); d.maxKey = std::max<int>(d.maxKey, n.key); }
    if (d.notes.empty()) { d.minKey = 60; d.maxKey = 72; }
    d.ch = 0;
    for (int c = 1; c < 16; c++) if (cnt[c] > cnt[d.ch]) d.ch = c;
    d.prog = -1; d.vol = -1; d.pan = -1;
    for (const mc::Event& e : t.events) {
        if (!e.IsChannel() || e.Channel() != d.ch) continue;
        int hi = e.status & 0xF0;
        if (hi == 0xC0 && d.prog < 0) d.prog = e.d1;
        if (hi == 0xB0 && e.d1 == 7 && d.vol < 0) d.vol = e.d2;
        if (hi == 0xB0 && e.d1 == 10 && d.pan < 0) d.pan = e.d2;
    }
    BuildCurves(t, d.ch, d.cur);
    ComputeBendScale(i);
}

// original notes and curves of the selected track (drawn as a faint "ghost" while an effect is active)
static void UpdateGhost() {
    V.ghostNotes.clear(); V.ghostTrack = -1;
    for (int k = 0; k < LANE_COUNT; k++) V.ghost[k].clear();
    int s = V.sel;
    if (s < 0 || s >= (int)TV.size()) return;
    if (ChainActive(s)) {
        V.ghostTrack = s;
        BuildCurves(g.base.tracks[(size_t)s], TV[(size_t)s].ch, V.ghost);
        std::vector<mc::Event> o;
        mc::ExtractNotes(g.base.tracks[(size_t)s], V.ghostNotes, o);
    }
    // phrase starts (shown as red marks) while a phrase-aware effect is active on this track
    V.phraseMarks.clear();
    bool phr = false;
    if (!g.bypassAll)
        for (const ChainItem& it : g.chains[(size_t)s])
            if (it.enabled && (it.type == mc::FX_AUTOPHRASE || it.type == mc::FX_HUMANCC || it.type == mc::FX_BREATH)) phr = true;
    if (phr) {
        std::vector<mc::Note> all, n; std::vector<mc::Event> o;
        mc::ExtractNotes(g.base.tracks[(size_t)s], all, o);
        for (const mc::Note& x : all) if (x.ch == TV[(size_t)s].ch) n.push_back(x);
        std::sort(n.begin(), n.end(), [](const mc::Note& a, const mc::Note& b) { return a.start < b.start; });
        std::vector<mc::Phrase> ph;
        mc::DetectPhrases(n, g.base.ppq, mc::ChainPhraseCfgInOrder(g.chains[(size_t)s]), ph);
        for (const mc::Phrase& f : ph) V.phraseMarks.push_back(f.t0);
    }
    ComputeBendScale(s);
}

static bool AnySolo() { for (char c : g.solo) if (c) return true; return false; }
static bool EffectivelyMuted(int r) {
    if (r < 0 || r >= (int)g.muted.size()) return false;
    return g.muted[(size_t)r] || (AnySolo() && !g.solo[(size_t)r]);
}
static std::vector<bool> AudibleTracks() {
    std::vector<bool> v;
    for (size_t i = 0; i < g.muted.size(); i++) v.push_back(!EffectivelyMuted((int)i));
    return v;
}

// ===========================================================================
//  Multi-track view
// ===========================================================================
static double PPT() { return V.pxPerBeat / (double)V.ppq; }
static int TickToX(double tick) { return LPW + (int)((tick - (double)V.scrollTick) * PPT()); }
static double XToTick(int x) { return (double)V.scrollTick + (x - LPW) / PPT(); }
static int RowTop(int r) { return TRKTOP + r * V.rowH - V.scrollY; }

static void UpdateArrangeScroll() {
    static bool busy = false;
    if (busy || !g.hArr) return;
    busy = true;
    RECT rc; GetClientRect(g.hArr, &rc);
    const int visH = std::max(1, (int)rc.bottom - TRKTOP);
    const int total = (int)TV.size() * V.rowH;
    V.scrollY = std::max(0, std::min(V.scrollY, std::max(0, total - visH)));
    const double visT = std::max(1.0, (rc.right - LPW) / PPT());
    const double maxScroll = std::max(0.0, (double)V.lenTicks + V.ppq * 8.0 - visT);
    if ((double)V.scrollTick > maxScroll) V.scrollTick = (uint32_t)maxScroll;

    SCROLLINFO si; memset(&si, 0, sizeof si); si.cbSize = sizeof si;
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0; si.nMax = std::max(0, total - 1); si.nPage = (UINT)visH; si.nPos = V.scrollY;
    SetScrollInfo(g.hArr, SB_VERT, &si, TRUE);
    si.nMin = 0; si.nMax = (int)(V.lenTicks + V.ppq * 8); si.nPage = (UINT)visT; si.nPos = (int)V.scrollTick;
    SetScrollInfo(g.hArr, SB_HORZ, &si, TRUE);
    busy = false;
}

static void EnsureVisible(uint32_t tick) {
    RECT rc; GetClientRect(g.hArr, &rc);
    int x = TickToX(tick);
    if (x < LPW || x > rc.right - 30) {
        double vis = (rc.right - LPW) / PPT();
        V.scrollTick = (uint32_t)std::max(0.0, (double)tick - vis * 0.1);
        UpdateArrangeScroll();
    }
}

static int ValueAt(const Pts& c, uint32_t tick, int def) {
    auto it = std::upper_bound(c.begin(), c.end(), tick,
        [](uint32_t v, const std::pair<uint32_t, int>& p) { return v < p.first; });
    if (it == c.begin()) return def;
    return (it - 1)->second;
}

static int CurveY(int lane, double bendScale, int val, int y0, int y1) {
    double frac;
    if (lane == LANE_BEND) {
        double cents = (val - 8192) * g.bendRange * 100.0 / 8192.0;
        frac = 0.5 - cents / (2.0 * bendScale);
    } else {
        frac = 1.0 - val / 127.0;
    }
    frac = std::max(0.0, std::min(1.0, frac));
    return y0 + 2 + (int)(frac * (y1 - y0 - 4));
}

// step-line graph of one controller inside [LPW,W) x [y0,y1)
static void DrawCurve(HDC dc, const Pts& c, int lane, double bendScale, int y0, int y1, COLORREF col, int penW, int W) {
    const uint32_t t0 = V.scrollTick, t1 = t0 + (uint32_t)((W - LPW) / PPT()) + 1;
    if (lane == LANE_VEL) {                                 // velocity: stems at the note starts
        // Every note-on in view gets its stem.  (The list is scanned completely instead of binary-searched: a track whose
        // events are not strictly in tick order used to lose its velocity graph.)
        HPEN pen = CreatePen(PS_SOLID, 1, col);
        HGDIOBJ old = SelectObject(dc, pen);
        for (const auto& pt : c) {
            if (pt.first < t0 || pt.first > t1) continue;
            const int v = std::max(0, std::min(127, pt.second));
            int x = TickToX(pt.first);
            int y = y1 - 2 - (int)((v / 127.0) * (y1 - y0) * 0.6);
            MoveToEx(dc, x, y1 - 2, NULL); LineTo(dc, x, y);
            Fill(dc, x - 1, y - 1, x + 2, y + 2, col);
        }
        SelectObject(dc, old); DeleteObject(pen);
        return;
    }
    if (c.empty()) return;
    auto it = std::upper_bound(c.begin(), c.end(), t0,
        [](uint32_t v, const std::pair<uint32_t, int>& p) { return v < p.first; });
    size_t i = (size_t)(it - c.begin());
    int prevVal = i > 0 ? c[i - 1].second : LM[lane].def;
    int prevY = CurveY(lane, bendScale, prevVal, y0, y1);
    std::vector<POINT> v;
    POINT p0 = { LPW, prevY }; v.push_back(p0);
    for (; i < c.size() && c[i].first <= t1; i++) {
        int x = TickToX(c[i].first), y = CurveY(lane, bendScale, c[i].second, y0, y1);
        POINT a = { x, prevY }, b = { x, y };
        v.push_back(a); v.push_back(b); prevY = y;
    }
    POINT pe = { W, prevY }; v.push_back(pe);
    HPEN pen = CreatePen(PS_SOLID, penW, col);
    HGDIOBJ old = SelectObject(dc, pen);
    Polyline(dc, v.data(), (int)v.size());
    SelectObject(dc, old); DeleteObject(pen);
}

static std::wstring LaneValueText(int track, int lane, long probe) {
    wchar_t b[32];
    if (probe < 0) probe = 0;
    const TrackVD& d = TV[(size_t)track];
    int v = ValueAt(d.cur[lane], (uint32_t)probe, LM[lane].def);
    if (lane == LANE_VEL) { if (v < 0) return L"-"; _snwprintf(b, 32, L"%d", v); return b; }
    if (lane == LANE_BEND) { int cents = mc::RoundI((v - 8192) * g.bendRange * 100.0 / 8192.0); _snwprintf(b, 32, L"%+dc", cents); return b; }
    _snwprintf(b, 32, L"%d", v);
    return b;
}

static void BarBeatTick(uint32_t tick, int& bar, int& beat, int& tk) {
    int beatTicks = std::max(1, V.ppq * 4 / std::max(1, V.tsDen));
    int barTicks = beatTicks * std::max(1, V.tsNum);
    bar = (int)(tick / (uint32_t)barTicks) + 1;
    int rem = (int)(tick % (uint32_t)barTicks);
    beat = rem / beatTicks + 1; tk = rem % beatTicks;
}

static COLORREF BandColor(int r, bool sel, bool mute) {
    static const COLORREF pal[9] = { RGB(104,108,122), RGB(116,100,128), RGB(96,124,112), RGB(132,108,100), RGB(120,120,92),
                                     RGB(92,118,134), RGB(130,100,114), RGB(100,112,140), RGB(136,118,96) };
    COLORREF c = pal[r % 9];
    int f = sel ? 128 : (mute ? 62 : 100);
    return RGB(std::min(255, GetRValue(c) * f / 100), std::min(255, GetGValue(c) * f / 100), std::min(255, GetBValue(c) * f / 100));
}

static void DrawLeftRow(HDC dc, int r, int y) {
    const TrackVD& d = TV[(size_t)r];
    const bool sel = (r == V.sel), mute = EffectivelyMuted(r);
    const int h = V.rowH;
    Fill(dc, 0, y, LPW, y + h, sel ? RGB(178, 172, 190) : RGB(236, 236, 238));
    Fill(dc, 0, y + h - 1, LPW, y + h, RGB(120, 120, 130));
    for (int c = 1; c <= NCOL; c++) Fill(dc, COLX[c] - 1, y, COLX[c], y + h, RGB(190, 190, 198));
    SetBkMode(dc, TRANSPARENT);
    RECT rc;
    wchar_t b[40];

    _snwprintf(b, 40, L"%d", r + 1);
    rc.left = COLX[0]; rc.right = COLX[1]; rc.top = y; rc.bottom = y + h;
    TxtIn(dc, rc, b, RGB(30, 30, 40), DT_CENTER);
    rc.left = COLX[1] + 4; rc.right = COLX[2] - 2;
    TxtIn(dc, rc, TrackName(g.base.tracks[(size_t)r], r), RGB(20, 20, 30), DT_LEFT | DT_END_ELLIPSIS);

    const int by = y + h / 2 - 10;
    RECT m = { COLX[2] + 2, by, COLX[3] - 2, by + 20 };                       // Mute
    Fill(dc, m.left, m.top, m.right, m.bottom, g.muted[(size_t)r] ? RGB(235, 130, 40) : RGB(214, 214, 220));
    Frame(dc, m.left, m.top, m.right, m.bottom, RGB(110, 110, 120));
    TxtIn(dc, m, L"Mute", RGB(20, 20, 30), DT_CENTER);
    RECT s = { COLX[3] + 2, by, COLX[4] - 2, by + 20 };                       // Solo
    Fill(dc, s.left, s.top, s.right, s.bottom, g.solo[(size_t)r] ? RGB(60, 200, 70) : RGB(214, 214, 220));
    Frame(dc, s.left, s.top, s.right, s.bottom, RGB(110, 110, 120));
    TxtIn(dc, s, L"Solo", RGB(20, 20, 30), DT_CENTER);

    _snwprintf(b, 40, L"%d", d.ch + 1);
    if (d.notes.empty()) {
        rc.left = COLX[4]; rc.right = COLX[5];
        TxtIn(dc, rc, L"-", RGB(30, 30, 40), DT_CENTER);
    } else {                                                                  // click = choose another MIDI channel
        RECT cb = { COLX[4] + 1, by, COLX[5] - 1, by + 20 };
        Fill(dc, cb.left, cb.top, cb.right, cb.bottom, RGB(244, 244, 248));
        Frame(dc, cb.left, cb.top, cb.right, cb.bottom, RGB(110, 110, 120));
        TxtIn(dc, cb, b, RGB(20, 20, 30), DT_CENTER);
    }

    std::wstring patch = L"-";
    bool pickable = false;                                                    // the instrument can be changed (click)
    if (!d.notes.empty()) {
        if (d.ch == 9) patch = L"Drum Kit";
        else {
            const int pg = g.trkProg[(size_t)r] >= 0 ? g.trkProg[(size_t)r] : d.prog;
            if (pg >= 0 && pg < 128) patch = Utf8ToW(GM_NAMES_A[pg]);
            pickable = true;
        }
    }
    if (pickable) {                                                           // looks like a drop-down: orange = changed from the file
        RECT pb = { COLX[5] + 2, by, COLX[6] - 2, by + 20 };
        Fill(dc, pb.left, pb.top, pb.right, pb.bottom, g.trkProg[(size_t)r] >= 0 ? RGB(255, 214, 150) : RGB(244, 244, 248));
        Frame(dc, pb.left, pb.top, pb.right, pb.bottom, RGB(110, 110, 120));
        if (pb.right - pb.left > 28) {
            const int ax = pb.right - 12, ay = by + 8;
            for (int i = 0; i < 4; i++) Fill(dc, ax + i, ay + i, ax + 9 - i, ay + i + 1, RGB(60, 60, 70));
            pb.right -= 14;
        }
        pb.left += 4;
        TxtIn(dc, pb, patch, RGB(20, 20, 30), DT_LEFT | DT_END_ELLIPSIS);
    } else {
        rc.left = COLX[5] + 4; rc.right = COLX[6] - 2;
        TxtIn(dc, rc, patch, RGB(20, 20, 30), DT_LEFT | DT_END_ELLIPSIS);
    }

    int nfx = 0; for (const ChainItem& c : g.chains[(size_t)r]) if (c.enabled) nfx++;     // FX button
    RECT f = { COLX[6] + 3, by, COLX[7] - 3, by + 20 };
    bool active = ChainActive(r);
    Fill(dc, f.left, f.top, f.right, f.bottom, active ? RGB(70, 130, 220) : RGB(214, 214, 220));
    Frame(dc, f.left, f.top, f.right, f.bottom, RGB(110, 110, 120));
    if (nfx) _snwprintf(b, 40, L"FX%d", nfx); else wcscpy(b, L"FX");
    TxtIn(dc, f, b, active ? RGB(255, 255, 255) : RGB(20, 20, 30), DT_CENTER);

    // Vol and Pan: small sliders (drag with the mouse, double-click = reset)
    if (!d.notes.empty()) {
        const bool ovV = g.trkVol[(size_t)r] >= 0, ovP = g.trkPan[(size_t)r] >= 0;
        const int vv = ovV ? g.trkVol[(size_t)r] : (d.vol >= 0 ? d.vol : 100);
        const int pp = ovP ? g.trkPan[(size_t)r] : (d.pan >= 0 ? d.pan : 64);
        for (int k = 0; k < 2; k++) {
            const int col = 7 + k;
            RECT cell = { COLX[col] + 3, by + 2, COLX[col + 1] - 3, by + 18 };
            const int w = cell.right - cell.left;
            Fill(dc, cell.left, cell.top, cell.right, cell.bottom, RGB(204, 204, 212));
            const bool ov = k == 0 ? ovV : ovP;
            const COLORREF fc = ov ? RGB(238, 160, 70) : RGB(110, 160, 225);
            if (k == 0) Fill(dc, cell.left, cell.top, cell.left + (int)(w * vv / 127.0), cell.bottom, fc);
            else {
                int cx = cell.left + w / 2, px = cell.left + (int)(w * pp / 127.0);
                Fill(dc, std::min(cx, px), cell.top, std::max(cx, px), cell.bottom, fc);
                Fill(dc, cx, cell.top, cx + 1, cell.bottom, RGB(90, 90, 100));
            }
            Frame(dc, cell.left, cell.top, cell.right, cell.bottom, RGB(110, 110, 120));
            if (k == 0) _snwprintf(b, 40, L"%d", vv);
            else if (pp == 64) wcscpy(b, L"C");
            else if (pp < 64) _snwprintf(b, 40, L"L%d", 64 - pp);
            else _snwprintf(b, 40, L"R%d", pp - 64);
            TxtIn(dc, cell, b, RGB(20, 20, 30), DT_CENTER);
        }
    }
    if (mute) Fill(dc, 0, y, 3, y + h, RGB(235, 130, 40));
}

static void DrawStrip(HDC dc, int r, int y, int W) {
    const TrackVD& d = TV[(size_t)r];
    const bool sel = (r == V.sel), mute = EffectivelyMuted(r);
    const int h = V.rowH;
    Fill(dc, LPW, y, W, y + h, BandColor(r, sel, mute));

    SaveDC(dc);
    IntersectClipRect(dc, LPW, std::max(y, TRKTOP), W, y + h);
    const uint32_t t0 = V.scrollTick, t1 = (uint32_t)((double)t0 + (W - LPW) / PPT() + 1);
    const int beatTicks = std::max(1, V.ppq * 4 / std::max(1, V.tsDen));
    const int measTicks = beatTicks * std::max(1, V.tsNum);
    const uint32_t first = (t0 / (uint32_t)beatTicks) * (uint32_t)beatTicks;
    const bool showBeats = V.pxPerBeat >= 24.0;
    for (uint32_t t = first; t <= t1 + (uint32_t)beatTicks; t += (uint32_t)beatTicks) {
        int x = TickToX(t);
        if ((t % (uint32_t)measTicks) == 0) Fill(dc, x, y, x + 1, y + h, RGB(28, 28, 34));
        else if (showBeats) Fill(dc, x, y, x + 1, y + h, RGB(70, 72, 82));
    }

    // notes, scaled to the pitch range of the track
    int lo = d.minKey, hi = d.maxKey;
    if (hi - lo < 11) { int mid = (lo + hi) / 2; lo = mid - 6; hi = mid + 5; }
    const int ytop = y + 16, ybot = y + h - 5;
    const double kh = (double)(ybot - ytop) / (double)(hi - lo + 1);
    auto noteRect = [&](const mc::Note& n, int& x1, int& x2, int& ya, int& yb) {
        x1 = TickToX(n.start); x2 = TickToX(n.end); if (x2 - x1 < 2) x2 = x1 + 2;
        ya = ybot - (int)((n.key - lo + 1) * kh); yb = ya + std::max(2, (int)kh);
    };
    if (sel && V.ghostTrack == r && V.showGhost) {
        for (const mc::Note& n : V.ghostNotes) {
            if (n.end < t0 || n.start > t1) continue;
            int x1, x2, ya, yb; noteRect(n, x1, x2, ya, yb);
            Frame(dc, x1, ya - 1, x2, yb + 1, RGB(185, 188, 200));
        }
    }
    const COLORREF noteCol = mute ? RGB(170, 170, 176) : RGB(246, 246, 250);
    for (const mc::Note& n : d.notes) {
        if (n.end < t0 || n.start > t1) continue;
        int x1, x2, ya, yb; noteRect(n, x1, x2, ya, yb);
        Fill(dc, x1, ya, x2 - 1, yb, noteCol);
    }

    // controller graphs on top of the notes
    for (int l = 0; l < LANE_COUNT; l++) {
        if (!V.laneOn[l]) continue;
        if (sel && V.ghostTrack == r && V.showGhost)
            DrawCurve(dc, V.ghost[l], l, d.bendScale, y + 14, y + h - 3, RGB(140, 142, 156), 1, W);
        DrawCurve(dc, d.cur[l], l, d.bendScale, y + 14, y + h - 3, LM[l].col, 2, W);
    }

    if (sel && !V.phraseMarks.empty()) {                        // phrase starts
        for (uint32_t pt : V.phraseMarks) {
            if (pt < t0 || pt > t1) continue;
            int x = TickToX(pt);
            for (int i = 0; i < 4; i++) Fill(dc, x - (3 - i), y + 1 + i, x + (3 - i) + 1, y + 2 + i, RGB(255, 90, 90));
            Fill(dc, x, y + 5, x + 1, y + 11, RGB(255, 90, 90));
        }
    }
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g.font);
    Txt(dc, LPW + 5, y + 1, TrackName(g.base.tracks[(size_t)r], r), RGB(255, 255, 255));
    RestoreDC(dc, -1);
    Fill(dc, LPW, y + h - 1, W, y + h, RGB(30, 30, 36));
}

static void DrawInfoPanel(HDC dc, long probe) {
    Fill(dc, 0, 0, LPW, INFOH, RGB(244, 244, 246));
    Fill(dc, 0, INFOH - 1, LPW, INFOH, RGB(150, 150, 160));
    SetBkMode(dc, TRANSPARENT);
    double ms = (g.loaded && V.playing) ? g.player.PosMs() : (g.loaded ? mc::TickToMs(g.tempo, V.ppq, (uint32_t)std::max(0L, probe)) : 0.0);
    long tms = (long)ms;
    wchar_t b[64];
    _snwprintf(b, 64, L"%02ld:%02ld:%02ld,%03ld", tms / 3600000, (tms / 60000) % 60, (tms / 1000) % 60, tms % 1000);
    SelectObject(dc, g.fontBig);
    Txt(dc, 8, 2, b, RGB(0, 150, 70));
    SelectObject(dc, g.font);
    int bar = 1, beat = 1, tk = 0; BarBeatTick((uint32_t)std::max(0L, probe), bar, beat, tk);
    double bpm = 120;
    if (!g.tempo.empty()) {
        size_t i = g.tempo.size() - 1;
        while (i > 0 && g.tempo[i].tick > (uint32_t)std::max(0L, probe)) i--;
        bpm = 60000000.0 / g.tempo[i].uspq;
    }
    _snwprintf(b, 64, L"%02d:%02d:%03d     %.1f BPM    %d/%d", bar, beat, tk, bpm, V.tsNum, V.tsDen);
    Txt(dc, 10, 32, b, RGB(30, 30, 40));

    // values of the selected track's graphs at the playhead / mouse position
    if (V.sel >= 0 && V.sel < (int)TV.size()) {
        // The values wrap into a second row when the list is narrow, so that no graph (the velocity one is last) is cut off.
        int x = 10, y = 50;
        for (int l = 0; l < LANE_COUNT; l++) {
            if (!V.laneOn[l]) continue;
            std::wstring t = std::wstring(LM[l].label) + L" " + LaneValueText(V.sel, l, probe);
            SIZE sz; GetTextExtentPoint32W(dc, t.c_str(), (int)t.size(), &sz);
            if (x > 10 && x + 9 + sz.cx > LPW - 6) { x = 10; y += 16; }
            if (y + 14 > INFOH) break;
            Fill(dc, x, y + 3, x + 6, y + 11, LM[l].col);
            Txt(dc, x + 9, y, t, RGB(30, 30, 40));
            x += sz.cx + 20;
        }
    }
}

static void DrawNavigator(HDC dc, int W) {
    const int y1 = TOPH - RULH;
    Fill(dc, LPW, 0, W, y1, RGB(40, 42, 54));
    if (!g.loaded || V.lenTicks == 0) return;
    const double w = (double)(W - LPW);
    int r = V.sel >= 0 ? V.sel : -1;
    int lo = 127, hi = 0;
    auto scan = [&](const TrackVD& d) { for (const mc::Note& n : d.notes) { lo = std::min<int>(lo, n.key); hi = std::max<int>(hi, n.key); } };
    if (r >= 0) scan(TV[(size_t)r]); else for (const TrackVD& d : TV) scan(d);
    if (hi <= lo) { lo = 48; hi = 84; }
    auto draw = [&](const TrackVD& d, COLORREF c) {
        for (const mc::Note& n : d.notes) {
            int x = LPW + (int)(n.start * w / V.lenTicks), x2 = LPW + (int)(n.end * w / V.lenTicks);
            int y = 4 + (int)((double)(hi - n.key) / (double)(hi - lo + 1) * (y1 - 8));
            Fill(dc, x, y, std::max(x + 1, x2), y + 2, c);
        }
    };
    if (r >= 0) draw(TV[(size_t)r], RGB(120, 230, 160));
    else for (size_t i = 0; i < TV.size(); i++) draw(TV[i], RGB(170, 175, 195));
    int vx1 = LPW + (int)(V.scrollTick * w / V.lenTicks);
    int vx2 = LPW + (int)((V.scrollTick + (W - LPW) / PPT()) * w / V.lenTicks);
    Frame(dc, vx1, 1, std::max(vx1 + 3, std::min(vx2, W)), y1 - 1, RGB(230, 230, 240));
    if (V.playing) { int x = LPW + (int)(V.playTick * w / V.lenTicks); Fill(dc, x, 0, x + 1, y1, RGB(255, 80, 80)); }
}

static void DrawRuler(HDC dc, int W) {
    const int y0 = TOPH - RULH;
    Fill(dc, LPW, y0, W, TOPH, RGB(214, 214, 222));
    Fill(dc, LPW, TOPH - 1, W, TOPH, RGB(90, 90, 100));
    const int beatTicks = std::max(1, V.ppq * 4 / std::max(1, V.tsDen));
    const int measTicks = beatTicks * std::max(1, V.tsNum);
    const uint32_t t0 = V.scrollTick, t1 = (uint32_t)((double)t0 + (W - LPW) / PPT() + 1);
    const uint32_t first = (t0 / (uint32_t)beatTicks) * (uint32_t)beatTicks;
    SetBkMode(dc, TRANSPARENT);
    for (uint32_t t = first; t <= t1 + (uint32_t)beatTicks; t += (uint32_t)beatTicks) {
        int x = TickToX(t);
        if (x < LPW) continue;
        if ((t % (uint32_t)measTicks) == 0) {
            Fill(dc, x, y0 + 4, x + 1, TOPH, RGB(60, 60, 70));
            wchar_t b[16]; int len = _snwprintf(b, 16, L"%u", t / (uint32_t)measTicks + 1);
            SetTextColor(dc, RGB(20, 20, 30)); TextOutW(dc, x + 3, y0 + 3, b, len);
        } else if (V.pxPerBeat >= 24.0) Fill(dc, x, TOPH - 5, x + 1, TOPH, RGB(100, 100, 110));
    }
}

// Marker Track: a fixed row under the ruler with the text markers (flags); double-click inserts / edits, right-click opens a menu.
static void DrawMarkerRow(HDC dc, int W) {
    const int y0 = TOPH, y1 = TRKTOP;
    Fill(dc, 0, y0, LPW, y1, RGB(232, 224, 196));
    Fill(dc, 0, y1 - 1, LPW, y1, RGB(120, 120, 130));
    Fill(dc, LPW - 1, y0, LPW, y1, RGB(150, 150, 160));
    SetBkMode(dc, TRANSPARENT);
    RECT lr = { 8, y0, LPW - 4, y1 };
    wchar_t cnt[48]; _snwprintf(cnt, 48, L"Marker Track  (%d)", (int)g.markers.size());
    TxtIn(dc, lr, cnt, RGB(60, 45, 0), DT_LEFT | DT_END_ELLIPSIS);

    Fill(dc, LPW, y0, W, y1, RGB(52, 54, 68));
    Fill(dc, LPW, y1 - 1, W, y1, RGB(30, 30, 36));
    SaveDC(dc);
    IntersectClipRect(dc, LPW, y0, W, y1);
    const int beatTicks = std::max(1, V.ppq * 4 / std::max(1, V.tsDen));
    const int measTicks = beatTicks * std::max(1, V.tsNum);
    const uint32_t t0 = V.scrollTick, t1 = (uint32_t)((double)t0 + (W - LPW) / PPT() + 1);
    for (uint32_t t = (t0 / (uint32_t)measTicks) * (uint32_t)measTicks; t <= t1 + (uint32_t)measTicks; t += (uint32_t)measTicks)
        Fill(dc, TickToX(t), y0, TickToX(t) + 1, y1, RGB(78, 80, 96));
    for (size_t i = 0; i < g.markers.size(); i++) {
        const Marker& mk = g.markers[i];
        const int x = TickToX(mk.tick);
        const std::wstring t = Utf8ToW(mk.text);
        SIZE sz; GetTextExtentPoint32W(dc, t.c_str(), (int)t.size(), &sz);
        if (x > W || x + 10 + sz.cx < LPW) continue;
        const bool sel = (int)i == g.selMarker;
        if (sel) Fill(dc, x, y0 + 2, x + 10 + sz.cx, y1 - 3, RGB(120, 96, 24));
        const COLORREF fc = RGB(255, 205, 70);
        Fill(dc, x, y0 + 1, x + 2, y1 - 1, fc);                                   // flag pole
        for (int k = 0; k < 4; k++) Fill(dc, x + 2, y0 + 2 + k, x + 8 - k, y0 + 3 + k, fc);   // flag
        SetTextColor(dc, sel ? RGB(255, 255, 255) : RGB(255, 226, 140));
        TextOutW(dc, x + 10, y0 + (MRKH - sz.cy) / 2, t.c_str(), (int)t.size());
    }
    RestoreDC(dc, -1);
}

static void DrawHeader(HDC dc) {
    Fill(dc, 0, INFOH, LPW, TOPH, RGB(226, 226, 232));
    Fill(dc, 0, TOPH - 1, LPW, TOPH, RGB(90, 90, 100));
    static const wchar_t* names[NCOL] = { L"N", L"Name", L"Mute", L"Solo", L"Ch", L"Patch", L"FX", L"Vol", L"Pan" };
    SetBkMode(dc, TRANSPARENT);
    for (int c = 0; c < NCOL; c++) {
        RECT rc = { COLX[c], INFOH, COLX[c + 1], TOPH };
        TxtIn(dc, rc, names[c], RGB(30, 30, 40), DT_CENTER);
        Fill(dc, COLX[c + 1] - 1, INFOH, COLX[c + 1], TOPH, RGB(150, 150, 160));
    }
}

static void Tint(HDC dc, int l, int t, int r, int b, COLORREF c, BYTE alpha) {
    if (r <= l || b <= t) return;
    HDC m = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, 1, 1);
    HGDIOBJ old = SelectObject(m, bmp);
    Fill(m, 0, 0, 1, 1, c);
    BLENDFUNCTION bf; bf.BlendOp = AC_SRC_OVER; bf.BlendFlags = 0; bf.SourceConstantAlpha = alpha; bf.AlphaFormat = 0;
    AlphaBlend(dc, l, t, r - l, b - t, m, 0, 0, 1, 1, bf);
    SelectObject(m, old); DeleteObject(bmp); DeleteDC(m);
}

static void PaintArrange(HWND hwnd) {
    PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
    RECT rc; GetClientRect(hwnd, &rc);
    const int W = rc.right, H = rc.bottom;
    if (W <= 0 || H <= 0) { EndPaint(hwnd, &ps); return; }

    HDC m = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, W, H);
    HGDIOBJ oldBmp = SelectObject(m, bmp);
    HGDIOBJ oldFont = SelectObject(m, g.font);
    SetBkMode(m, TRANSPARENT);
    Fill(m, 0, 0, W, H, RGB(70, 72, 84));

    const long probe = V.playing ? (long)V.playTick : (V.hoverTick >= 0 ? V.hoverTick : (long)V.startTick);
    SaveDC(m);
    IntersectClipRect(m, 0, TRKTOP, W, H);
    for (int r = 0; r < (int)TV.size(); r++) {
        int y = RowTop(r);
        if (y + V.rowH < TRKTOP || y > H) continue;
        DrawLeftRow(m, r, y);
        DrawStrip(m, r, y, W);
    }
    RestoreDC(m, -1);

    DrawInfoPanel(m, probe);
    DrawHeader(m);
    DrawNavigator(m, W);
    DrawRuler(m, W);
    DrawMarkerRow(m, W);

    SaveDC(m);
    IntersectClipRect(m, LPW, TOPH - RULH, W, H);
    if (V.hoverTick >= 0 && !V.playing) { int x = TickToX((double)V.hoverTick); Fill(m, x, TOPH, x + 1, H, RGB(210, 210, 220)); }
    {   int x = TickToX(V.startTick);                                                 // start marker
        Fill(m, x, TOPH - RULH, x + 1, H, RGB(70, 150, 255));
        for (int i = 0; i < 5; i++) Fill(m, x - i, TOPH - RULH + i, x + i + 1, TOPH - RULH + i + 1, RGB(220, 40, 40)); }
    if (V.playing) { int x = TickToX(V.playTick); Fill(m, x, TOPH - RULH, x + 2, H, RGB(255, 60, 60)); }
    if (V.selEnd > V.selStart) {                                                      // timeline selection
        int x1 = TickToX(V.selStart), x2 = TickToX(V.selEnd);
        Tint(m, std::max(x1, LPW), TOPH - RULH, std::min(x2, W), H, RGB(70, 140, 255), 70);
        Fill(m, x1, TOPH - RULH, x1 + 2, H, RGB(90, 170, 255));
        Fill(m, x2 - 1, TOPH - RULH, x2 + 1, H, RGB(90, 170, 255));
        Fill(m, x1, TOPH - RULH, x2, TOPH - RULH + 3, RGB(90, 170, 255));
    }
    RestoreDC(m, -1);

    BitBlt(dc, 0, 0, W, H, m, 0, 0, SRCCOPY);
    SelectObject(m, oldFont); SelectObject(m, oldBmp);
    DeleteObject(bmp); DeleteDC(m);
    EndPaint(hwnd, &ps);
}

// ---- forward declarations of commands used by the view
static void SelectTrack(int i);
static void OpenChainDialog();
static void ToggleMute(int r);
static void ToggleSolo(int r);
static void DoPlayAt(uint32_t startTick);
static uint32_t SnapTick(double t);
static void MarkerInsertAt(uint32_t tick);
static void MarkerEdit(int i);
static void MarkerDelete(int i);
static void MarkerClearAll();
static void DeleteTrack(int r);
static void ChooseChannel(int r);

// index of the marker flag (or its text) under x in the Marker Track, or -1
static int MarkerAt(HWND hwnd, int x) {
    HDC dc = GetDC(hwnd);
    HGDIOBJ of = SelectObject(dc, g.font);
    int hit = -1;
    for (int i = (int)g.markers.size() - 1; i >= 0 && hit < 0; i--) {
        const std::wstring t = Utf8ToW(g.markers[(size_t)i].text);
        SIZE sz; GetTextExtentPoint32W(dc, t.c_str(), (int)t.size(), &sz);
        const int mx = TickToX(g.markers[(size_t)i].tick);
        if (x >= mx - 3 && x <= mx + 10 + sz.cx) hit = i;
    }
    SelectObject(dc, of); ReleaseDC(hwnd, dc);
    return hit;
}

static void ShowMarkerMenu(HWND hwnd, int x, int hit) {
    const uint32_t tick = SnapTick(XToTick(std::max(x, LPW)));
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_MK_INSERT, L"Insert marker here...");
    if (hit >= 0) {
        AppendMenuW(m, MF_STRING, ID_MK_EDIT, (L"Edit marker \"" + Utf8ToW(g.markers[(size_t)hit].text) + L"\"...").c_str());
        AppendMenuW(m, MF_STRING, ID_MK_DELETE, (L"Delete marker \"" + Utf8ToW(g.markers[(size_t)hit].text) + L"\"").c_str());
    }
    if (!g.markers.empty()) { AppendMenuW(m, MF_SEPARATOR, 0, NULL); AppendMenuW(m, MF_STRING, ID_MK_CLEAR, L"Delete all markers..."); }
    POINT pt; GetCursorPos(&pt);
    const int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(m);
    if (cmd == ID_MK_INSERT) MarkerInsertAt(tick);
    else if (cmd == ID_MK_EDIT && hit >= 0) MarkerEdit(hit);
    else if (cmd == ID_MK_DELETE && hit >= 0) MarkerDelete(hit);
    else if (cmd == ID_MK_CLEAR) MarkerClearAll();
}

static void ShowTrackMenu(HWND hwnd, int r) {
    HMENU m = CreatePopupMenu();
    std::wstring t = L"Delete track " + std::to_wstring(r + 1) + L" (" + TrackName(g.base.tracks[(size_t)r], r) + L")...";
    for (size_t i = t.find(L'&'); i != std::wstring::npos; i = t.find(L'&', i + 2)) t.insert(i, L"&");
    AppendMenuW(m, MF_STRING, ID_TRK_DELETE, t.c_str());
    POINT pt; GetCursorPos(&pt);
    const int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(m);
    if (cmd == ID_TRK_DELETE) DeleteTrack(r);
}

static int ScrollCalc(HWND h, int bar, WPARAM wp, int line) {
    SCROLLINFO si; memset(&si, 0, sizeof si); si.cbSize = sizeof si; si.fMask = SIF_ALL;
    GetScrollInfo(h, bar, &si);
    int pos = si.nPos;
    switch (LOWORD(wp)) {
    case SB_LINEUP:   pos -= line; break;
    case SB_LINEDOWN: pos += line; break;
    case SB_PAGEUP:   pos -= (int)si.nPage; break;
    case SB_PAGEDOWN: pos += (int)si.nPage; break;
    case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = si.nTrackPos; break;
    case SB_TOP: pos = si.nMin; break;
    case SB_BOTTOM: pos = si.nMax; break;
    }
    return pos;
}

static void SetStartFromX(int x) {
    double t = XToTick(x);
    int beat = std::max(1, V.ppq * 4 / std::max(1, V.tsDen));
    V.startTick = (uint32_t)std::max(0.0, std::floor(t / beat + 0.5) * beat);
}

static uint32_t SnapTick(double t) {
    int beat = std::max(1, V.ppq * 4 / std::max(1, V.tsDen));
    return (uint32_t)std::max(0.0, std::floor(t / beat + 0.5) * beat);
}
static int DividerAt(int x) {                                  // column divider near x (0..NCOL-1), or -1
    for (int c = 0; c < NCOL; c++) if (std::abs(x - COLX[c + 1]) <= 3) return c;
    return -1;
}
static void SetSliderFromX(int r, int col, int x) {            // volume (col 7) and pan (col 8) sliders
    if (r < 0 || r >= (int)TV.size() || TV[(size_t)r].notes.empty()) return;
    const int l = COLX[col] + 3, rr = COLX[col + 1] - 3;
    int v = (int)std::floor((double)(x - l) / (double)std::max(1, rr - l) * 127.0 + 0.5);
    v = std::max(0, std::min(127, v));
    if (col == 8 && std::abs(v - 64) <= 2) v = 64;             // snap to the centre
    const int ch = TV[(size_t)r].ch;
    if (col == 7) { g.trkVol[(size_t)r] = v; g.player.SendLive((DWORD)(0xB0 | ch) | (7u << 8) | ((DWORD)v << 16)); }
    else          { g.trkPan[(size_t)r] = v; g.player.SendLive((DWORD)(0xB0 | ch) | (10u << 8) | ((DWORD)v << 16)); }
    g.dirty = true;
}

static void UpdateTitle();

// Patch column: a menu with the 128 General MIDI instruments (16 families) changes the instrument of the track's
// channel.  It is a mixer-like override (as Vol / Pan): it is heard at once, saved with Save As, and "As in the file" undoes it.
static void ChoosePatch(int r) {
    if (r < 0 || r >= (int)TV.size() || TV[(size_t)r].notes.empty()) return;
    const TrackVD& d = TV[(size_t)r];
    if (d.ch == 9) { SetStatus(L"Channel 10 is the drum kit: the drum notes choose the sounds, there is no instrument to change."); return; }
    static const wchar_t* const fam[16] = { L"Piano", L"Chromatic Percussion", L"Organ", L"Guitar", L"Bass", L"Strings", L"Ensemble", L"Brass",
                                            L"Reed", L"Pipe", L"Synth Lead", L"Synth Pad", L"Synth Effects", L"Ethnic", L"Percussive", L"Sound Effects" };
    const bool changed = g.trkProg[(size_t)r] >= 0;
    const int cur = changed ? g.trkProg[(size_t)r] : d.prog;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (changed ? 0 : MF_CHECKED), ID_PATCH_FILE, L"As in the file");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    for (int f = 0; f < 16; f++) {
        HMENU sub = CreatePopupMenu();
        for (int i = 0; i < 8; i++) {
            const int pg = f * 8 + i;
            std::wstring label = std::to_wstring(pg + 1) + L"   " + Utf8ToW(GM_NAMES_A[pg]);
            AppendMenuW(sub, MF_STRING | (pg == cur ? MF_CHECKED : 0), (UINT_PTR)(ID_PATCH0 + pg), label.c_str());
        }
        AppendMenuW(m, MF_POPUP | (cur >= 0 && cur / 8 == f ? MF_CHECKED : 0), (UINT_PTR)sub, fam[f]);
    }
    POINT pt; GetCursorPos(&pt);
    const int id = (int)TrackPopupMenu(m, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, g.hArr, NULL);
    DestroyMenu(m);                                                           // the sub menus go with it
    if (id == ID_PATCH_FILE) g.trkProg[(size_t)r] = -1;
    else if (id >= ID_PATCH0 && id < ID_PATCH0 + 128) g.trkProg[(size_t)r] = id - ID_PATCH0;
    else return;
    const int prog = g.trkProg[(size_t)r] >= 0 ? g.trkProg[(size_t)r] : d.prog;
    if (prog >= 0) g.player.SendLive((DWORD)(0xC0 | d.ch) | ((DWORD)prog << 8));      // audible at once while playing
    g.dirty = true; UpdateTitle();
    InvalidateRect(g.hArr, NULL, FALSE);
    if (prog >= 0) SetStatus(L"Track %d: instrument %d - %ls.", r + 1, prog + 1, Utf8ToW(GM_NAMES_A[prog]).c_str());
}

// moves the timeline by a number of ticks (negative = back), the scroll bar is updated by the caller
static void ScrollTimeBy(double ticks) {
    const long s = (long)V.scrollTick + (long)std::lround(ticks);
    V.scrollTick = (uint32_t)std::max(0L, s);
}

static LRESULT CALLBACK ArrangeProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: PaintArrange(hwnd); return 0;
    case WM_SIZE: UpdateArrangeScroll(); InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_HSCROLL: {
        int pos = ScrollCalc(hwnd, SB_HORZ, wp, V.ppq);
        V.scrollTick = (uint32_t)std::max(0, pos);
        UpdateArrangeScroll(); InvalidateRect(hwnd, NULL, FALSE); return 0;
    }
    case WM_VSCROLL: {
        int pos = ScrollCalc(hwnd, SB_VERT, wp, V.rowH / 2);
        V.scrollY = std::max(0, pos);
        UpdateArrangeScroll(); InvalidateRect(hwnd, NULL, FALSE); return 0;
    }
    case WM_MOUSEWHEEL: {
        // The amounts are proportional to the wheel delta (120 = one notch), so mouse wheels and the
        // fine-grained messages of a touch pad both scroll smoothly.
        const double notches = GET_WHEEL_DELTA_WPARAM(wp) / 120.0;
        const WORD keys = GET_KEYSTATE_WPARAM(wp);
        RECT rc; GetClientRect(hwnd, &rc);
        const double vis = std::max(1.0, (rc.right - LPW) / PPT());
        if (keys & MK_CONTROL) {                                            // zoom
            V.pxPerBeat = std::max(4.0, std::min(600.0, V.pxPerBeat * std::pow(1.25, notches)));
        } else if (keys & MK_SHIFT) {                                       // Shift + wheel: sideways
            ScrollTimeBy(-notches * vis / 6.0);
        } else {                                                            // up / down through the tracks
            V.scrollY -= (int)std::lround(notches * (V.rowH / 2.0));
        }
        UpdateArrangeScroll(); InvalidateRect(hwnd, NULL, FALSE); return 0;
    }
    case WM_MOUSEHWHEEL: {                                                  // touch pad / tilt wheel: sideways
        const double notches = GET_WHEEL_DELTA_WPARAM(wp) / 120.0;
        RECT rc; GetClientRect(hwnd, &rc);
        const double vis = std::max(1.0, (rc.right - LPW) / PPT());
        ScrollTimeBy(notches * vis / 6.0);
        UpdateArrangeScroll(); InvalidateRect(hwnd, NULL, FALSE); return 0;
    }
    case WM_SETCURSOR: {
        POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
        if (V.dragCol >= 0 || (pt.y >= INFOH && pt.y < TOPH && pt.x < LPW + 4 && DividerAt(pt.x) >= 0)) {
            SetCursor(LoadCursor(NULL, IDC_SIZEWE)); return TRUE;
        }
        break;
    }
    case WM_MOUSEMOVE: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        if (V.dragCol >= 0) {                                              // resizing a column
            COLW[V.dragCol] = std::max(COLMIN[V.dragCol], V.dragStartW + x - V.dragStartX);
            RecalcColumns(); UpdateArrangeScroll(); InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (V.dragSlider) { SetSliderFromX(V.dragRow, V.dragSlider, x); InvalidateRect(hwnd, NULL, FALSE); return 0; }
        if (V.dragSel) {                                                   // selecting on the ruler
            uint32_t t = SnapTick(XToTick(x));
            V.selStart = std::min(V.selAnchor, t); V.selEnd = std::max(V.selAnchor, t);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        long oldT = V.hoverTick;
        V.hoverTick = (x >= LPW && y >= TOPH) ? (long)std::max(0.0, XToTick(x)) : -1;
        V.hoverRow = (y >= TRKTOP) ? (y - TRKTOP + V.scrollY) / std::max(1, V.rowH) : -1;
        if (!V.tracking) {
            TRACKMOUSEEVENT te; memset(&te, 0, sizeof te);
            te.cbSize = sizeof te; te.dwFlags = TME_LEAVE; te.hwndTrack = hwnd;
            TrackMouseEvent(&te); V.tracking = true;
        }
        if (oldT != V.hoverTick && !V.playing) InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_MOUSELEAVE:
        V.tracking = false; V.hoverTick = -1; V.hoverRow = -1;
        if (!V.playing) InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        SetFocus(hwnd);
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        RECT rc; GetClientRect(hwnd, &rc);
        if (y < TOPH - RULH) {                                      // navigator: jump
            if (x >= LPW && V.lenTicks > 0) {
                double t = (double)(x - LPW) / (double)(rc.right - LPW) * V.lenTicks;
                double vis = (rc.right - LPW) / PPT();
                V.scrollTick = (uint32_t)std::max(0.0, t - vis / 2);
                UpdateArrangeScroll(); InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        if (y < TOPH) {
            if (x >= LPW) {                                         // ruler: click = start marker, drag = select a part
                if (msg == WM_LBUTTONDBLCLK) { V.selStart = V.selEnd = 0; InvalidateRect(hwnd, NULL, FALSE); PostMessage(GetParent(hwnd), WM_ROLL_CLICKED, 0, 0); return 0; }
                V.dragSel = true; V.selAnchor = SnapTick(XToTick(x)); V.selStart = V.selEnd = V.selAnchor;
                SetCapture(hwnd);
            } else {                                                // column header: drag a divider to resize
                int c = DividerAt(x);
                if (c >= 0) { V.dragCol = c; V.dragStartX = x; V.dragStartW = COLW[c]; SetCapture(hwnd); }
            }
            return 0;
        }
        if (y < TRKTOP) {                                           // Marker Track: click selects, double-click inserts / edits
            if (g.loaded && x >= LPW) {
                const int hit = MarkerAt(hwnd, x);
                if (msg == WM_LBUTTONDBLCLK) { if (hit >= 0) MarkerEdit(hit); else MarkerInsertAt(SnapTick(XToTick(x))); }
                else { g.selMarker = hit; InvalidateRect(hwnd, NULL, FALSE); }
            }
            return 0;
        }
        int r = (y - TRKTOP + V.scrollY) / std::max(1, V.rowH);
        if (r < 0 || r >= (int)TV.size()) return 0;
        if (x < LPW) {
            int col = 0; while (col < NCOL - 1 && x >= COLX[col + 1]) col++;
            SelectTrack(r);
            if (col == 7 || col == 8) {                              // volume / pan slider
                if (msg == WM_LBUTTONDBLCLK) {                       // double-click: back to the file value (volume) / centre (pan)
                    if (col == 7) g.trkVol[(size_t)r] = -1; else g.trkPan[(size_t)r] = 64;
                    if (col == 8 && !TV[(size_t)r].notes.empty()) g.player.SendLive((DWORD)(0xB0 | TV[(size_t)r].ch) | (10u << 8) | (64u << 16));
                    g.dirty = true; InvalidateRect(hwnd, NULL, FALSE);
                } else { V.dragSlider = col; V.dragRow = r; SetCapture(hwnd); SetSliderFromX(r, col, x); InvalidateRect(hwnd, NULL, FALSE); }
            }
            else if (col == 2) ToggleMute(r);
            else if (col == 3) ToggleSolo(r);
            else if (col == 4) ChooseChannel(r);
            else if (col == 5) ChoosePatch(r);
            else if (col == 6) OpenChainDialog();
            else if (msg == WM_LBUTTONDBLCLK && col == 1) OpenChainDialog();
        } else {
            SelectTrack(r);
            SetStartFromX(x);
            InvalidateRect(hwnd, NULL, FALSE);
            PostMessage(GetParent(hwnd), WM_ROLL_CLICKED, 0, 0);
            if (msg == WM_LBUTTONDBLCLK) OpenChainDialog();
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (V.dragSel) {
            V.dragSel = false;
            const uint32_t beat = (uint32_t)std::max(1, V.ppq * 4 / std::max(1, V.tsDen));
            const bool plainClick = V.selEnd < V.selStart + beat;
            if (plainClick) { V.startTick = V.selAnchor; V.selStart = V.selEnd = 0; }
            else V.startTick = V.selStart;
            PostMessage(GetParent(hwnd), WM_ROLL_CLICKED, 0, 0);
            if (plainClick && g.loaded && g.player.IsPlaying()) DoPlayAt(V.startTick);      // click on the ruler while playing: play on from there
        }
        V.dragCol = -1; V.dragSlider = 0; V.dragRow = -1;
        if (GetCapture() == hwnd) ReleaseCapture();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_CAPTURECHANGED:
        V.dragCol = -1; V.dragSlider = 0; V.dragRow = -1; V.dragSel = false;
        return 0;
    case WM_RBUTTONDOWN: {
        SetFocus(hwnd);
        const int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        if (!g.loaded || y < TOPH) return 0;
        if (y < TRKTOP) {                                           // Marker Track
            const int hit = x >= LPW ? MarkerAt(hwnd, x) : -1;
            g.selMarker = hit; InvalidateRect(hwnd, NULL, FALSE);
            ShowMarkerMenu(hwnd, x, hit);
            return 0;
        }
        const int r = (y - TRKTOP + V.scrollY) / std::max(1, V.rowH);
        if (r < 0 || r >= (int)TV.size()) return 0;
        SelectTrack(r);
        ShowTrackMenu(hwnd, r);                                     // delete track (asks for confirmation)
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_DELETE && g.selMarker >= 0) { MarkerDelete(g.selMarker); return 0; }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ===========================================================================
//  Pipeline refresh and edits
// ===========================================================================
static void UpdateProcessed() {
    bool any = false;
    for (size_t i = 0; i < g.chains.size() && !any; i++) any = ChainActive((int)i);
    g.procOn = any;
    SetWindowTextW(g.hProc, any ? L"Rubatoing" : L"Original");
    InvalidateRect(g.hProc, NULL, TRUE);
}

static void UpdateTitle() {
    std::wstring t = APP_TITLE;
    if (g.loaded) {
        size_t s = g.path.find_last_of(L"\\/");
        const std::wstring& shown = !g.projPath.empty() ? g.projPath : g.path;
        s = shown.find_last_of(L"\\/");
        t += L" - [" + (shown.empty() ? std::wstring(L"untitled") : shown.substr(s == std::wstring::npos ? 0 : s + 1)) + L"]";
        if (g.dirty) t += L" *";
    }
    SetWindowTextW(g.hwnd, t.c_str());
}

static void SyncDialog();

static void RefreshAfterRecompute() {
    for (size_t i = 0; i < g.preview.tracks.size(); i++) UpdateTrackView((int)i);
    V.lenTicks = g.preview.EndTick();
    g.tempo = mc::BuildTempoMap(g.preview);
    UpdateGhost();
    UpdateArrangeScroll();
    InvalidateRect(g.hArr, NULL, FALSE);
    UpdateProcessed();
    UpdateTitle();
}

static void RebuildEverything(bool keepSel) {
    const size_t n = g.base.tracks.size();
    g.preview.ppq = g.base.ppq; g.preview.format = g.base.format; g.preview.bendRange = g.base.bendRange;
    g.preview.tracks.assign(n, mc::Track());
    g.chains.resize(n);
    if (!keepSel || g.muted.size() != n) { g.muted.assign(n, 0); g.solo.assign(n, 0); g.trkVol.assign(n, -1); g.trkPan.assign(n, -1); g.trkProg.assign(n, -1); }
    TV.assign(n, TrackVD());
    V.ppq = g.base.ppq;
    mc::GetTimeSig(g.base, V.tsNum, V.tsDen);
    for (size_t i = 0; i < n; i++) RecomputeTrack((int)i);
    if (!keepSel || V.sel >= (int)n) V.sel = -1;
    RefreshAfterRecompute();
    if (V.startTick > V.lenTicks) V.startTick = 0;
    SyncDialog();
}

static void OnChainChanged(int ti) {
    if (ti < 0 || ti >= (int)g.chains.size()) return;
    RecomputeTrack(ti);
    UpdateTrackView(ti);
    V.lenTicks = g.preview.EndTick();
    if (ti == V.sel) UpdateGhost();
    UpdateArrangeScroll();
    InvalidateRect(g.hArr, NULL, FALSE);
    UpdateProcessed();
    g.dirty = true;
    UpdateTitle();
    if (g.player.IsPlaying()) SetTimer(g.hwnd, 2, 150, NULL);      // restart playback so you hear the change
}

static void SelectTrack(int i) {
    if (i == V.sel) return;
    V.sel = i;
    UpdateGhost();
    SyncDialog();
    InvalidateRect(g.hArr, NULL, FALSE);
}

static void ToggleMute(int r) { g.muted[(size_t)r] = !g.muted[(size_t)r]; InvalidateRect(g.hArr, NULL, FALSE); if (g.player.IsPlaying()) SetTimer(g.hwnd, 2, 150, NULL); }
static void ToggleSolo(int r) { g.solo[(size_t)r] = !g.solo[(size_t)r]; InvalidateRect(g.hArr, NULL, FALSE); if (g.player.IsPlaying()) SetTimer(g.hwnd, 2, 150, NULL); }

// ===========================================================================
//  Commands
// ===========================================================================
static void StopUI() {
    V.playing = false;
    KillTimer(g.hwnd, 1);
    InvalidateRect(g.hArr, NULL, FALSE);
}
static void DoStop() { g.player.Stop(); StopUI(); KillTimer(g.hwnd, 2); }

static void LoadFile(const std::wstring& path) {
    std::vector<uint8_t> bytes;
    if (!ReadAll(path, bytes)) { MessageBoxW(g.hwnd, L"Cannot read the file.", APP_TITLE, MB_ICONERROR); return; }
    mc::Song s; std::wstring err;
    if (!mc::ReadSMF(bytes, s, err)) { MessageBoxW(g.hwnd, err.c_str(), APP_TITLE, MB_ICONERROR); return; }
    // Marker meta events (0x06) move into the Marker Track (alphanumeric text only); they are written back on export
    std::vector<Marker> mk;
    for (mc::Track& t : s.tracks)
        t.events.erase(std::remove_if(t.events.begin(), t.events.end(), [&](const mc::Event& e) {
            if (!(e.IsMeta() && e.d1 == 0x06)) return false;
            Marker m; m.tick = e.tick; m.text = SanitizeAlnum(std::string(e.data.begin(), e.data.end()));
            if (!m.text.empty()) mk.push_back(m);
            return true; }), t.events.end());
    std::stable_sort(mk.begin(), mk.end(), [](const Marker& a, const Marker& b) { return a.tick < b.tick; });
    const bool split = mc::SplitFormat0(s);          // format 0 keeps all channels in one track: one track per channel instead
    DoStop();
    g.markers = mk; g.selMarker = -1; g.projPath.clear();
    g.base = std::move(s); g.base.bendRange = g.bendRange;
    g.path = path; g.loaded = true; g.dirty = false;
    g.chains.assign(g.base.tracks.size(), Chain());
    g.undo.clear();
    V.startTick = 0; V.scrollTick = 0; V.scrollY = 0; V.sel = -1;
    RebuildEverything(false);

    int firstTrack = -1;
    for (size_t i = 0; i < TV.size(); i++) if (!TV[i].notes.empty()) { firstTrack = (int)i; break; }
    if (firstTrack >= 0) SelectTrack(firstTrack);
    InvalidateRect(g.hArr, NULL, FALSE);
    AddRecent(path);

    unsigned total = 0; for (auto& d : TV) total += (unsigned)d.notes.size();
    SetStatus(L"%ls%u tracks, %u notes, %d PPQ, %.1f BPM.  Click a track's FX button to add effects - nothing changes until you press Apply All FXs.",
              split ? L"Loaded (format 0 file, split into one track per channel): " : L"Loaded: ", (unsigned)g.base.tracks.size(), total, g.base.ppq, 60000000.0 / g.tempo[0].uspq);
}

static bool SaveProject(bool saveAs);
static void OpenAny(const std::wstring& path);

// Yes = save the project first, No = discard the changes, Cancel = stay
static bool ConfirmDiscard() {
    if (!g.loaded || !g.dirty) return true;
    const int r = MessageBoxW(g.hwnd, L"The project has unsaved changes (effect chains, mixer, markers ...).\r\n\r\nSave the project before continuing?",
                              APP_TITLE, MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDCANCEL) return false;
    if (r == IDYES) return SaveProject(false);
    return true;
}

static void OpenAny(const std::wstring& path);
static void LoadProject(const std::wstring& path);

static void DoOpen(bool projectOnly) {
    if (!ConfirmDiscard()) return;
    wchar_t buf[MAX_PATH * 2] = L"";
    OPENFILENAMEW o; memset(&o, 0, sizeof o);
    o.lStructSize = sizeof o; o.hwndOwner = g.hwnd;
    o.lpstrFilter = projectOnly ? L"Rubato projects (*.rpj)\0*.rpj\0All files (*.*)\0*.*\0"
                                : L"MIDI files and Rubato projects (*.mid;*.midi;*.kar;*.rpj)\0*.mid;*.midi;*.kar;*.rpj\0"
                                  L"MIDI files (*.mid;*.midi;*.kar)\0*.mid;*.midi;*.kar\0Rubato projects (*.rpj)\0*.rpj\0All files (*.*)\0*.*\0";
    o.lpstrFile = buf; o.nMaxFile = MAX_PATH * 2;
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&o)) OpenAny(buf);
}

static void ApplyMixer(mc::Song& out);

// MIDI export: exactly what you see and hear (base + effect chains + mixer), plus the Marker Track, always as a format 0 file.
static void DoExport() {
    if (!g.loaded) return;
    wchar_t buf[MAX_PATH * 2] = L"";
    const std::wstring& src = !g.projPath.empty() ? g.projPath : g.path;
    if (!src.empty()) {
        size_t dot = src.find_last_of(L'.');
        std::wstring suggestion = src.substr(0, dot == std::wstring::npos ? src.size() : dot) + L"_fx.mid";
        wcsncpy(buf, suggestion.c_str(), MAX_PATH * 2 - 1);
    }
    OPENFILENAMEW o; memset(&o, 0, sizeof o);
    o.lStructSize = sizeof o; o.hwndOwner = g.hwnd;
    o.lpstrFilter = L"MIDI files (*.mid)\0*.mid\0All files (*.*)\0*.*\0";
    o.lpstrFile = buf; o.nMaxFile = MAX_PATH * 2; o.lpstrDefExt = L"mid";
    o.lpstrTitle = L"Export MIDI file (format 0, with markers)";
    o.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_HIDEREADONLY;
    if (!GetSaveFileNameW(&o)) return;
    mc::Song out = g.preview; mc::AddBendRangeRPN(out); ApplyMixer(out);
    for (const Marker& m : g.markers) {                                   // Marker Track -> marker meta events (0x06)
        mc::Event e; e.tick = m.tick; e.status = 0xFF; e.d1 = 0x06; e.data.assign(m.text.begin(), m.text.end());
        out.tracks[0].events.push_back(e);
    }
    std::vector<uint8_t> bytes = mc::WriteSMF0(out);                      // one single track, format 0
    if (!WriteAll(buf, bytes.data(), bytes.size())) { MessageBoxW(g.hwnd, L"Cannot write the file.", APP_TITLE, MB_ICONERROR); return; }
    SetStatus(L"Exported as format 0 with %d marker(s) (all active effects applied): %ls", (int)g.markers.size(), buf);
}

// Volume / pan sliders: replace CC7 / CC10 of the track's channel by a single value at tick 0.
static void ApplyMixer(mc::Song& out) {
    for (size_t r = 0; r < out.tracks.size() && r < TV.size(); r++) {
        if (TV[r].notes.empty()) continue;
        const int ch = TV[r].ch;
        const int ccs[2] = { 7, 10 };
        const int vals[2] = { g.trkVol[r], g.trkPan[r] };
        if (r < g.trkProg.size() && g.trkProg[r] >= 0 && ch != 9) {              // instrument chosen in the Patch column
            std::vector<mc::Event>& pev = out.tracks[r].events;
            pev.erase(std::remove_if(pev.begin(), pev.end(), [&](const mc::Event& e) {
                if (!e.IsChannel() || e.Channel() != ch) return false;
                const int hi = e.status & 0xF0;
                return hi == 0xC0 || (hi == 0xB0 && (e.d1 == 0 || e.d1 == 32));   // the file's own program changes and bank selects
            }), pev.end());
            mc::Event pe; pe.tick = 0; pe.status = (uint8_t)(0xC0 | ch); pe.d1 = (uint8_t)g.trkProg[r]; pe.d2 = 0;
            pev.insert(pev.begin(), pe);
        }
        for (int k = 0; k < 2; k++) {
            if (vals[k] < 0) continue;
            std::vector<mc::Event>& ev = out.tracks[r].events;
            const int cc = ccs[k];
            ev.erase(std::remove_if(ev.begin(), ev.end(), [&](const mc::Event& e) {
                return e.IsChannel() && (e.status & 0xF0) == 0xB0 && e.Channel() == ch && e.d1 == cc; }), ev.end());
            mc::Event e; e.tick = 0; e.status = (uint8_t)(0xB0 | ch); e.d1 = (uint8_t)cc; e.d2 = (uint8_t)vals[k];
            ev.insert(ev.begin(), e);
        }
    }
}

static void DoPlayAt(uint32_t startTick) {
    if (!g.loaded) return;
    int sel = (int)SendMessageW(g.hDev, CB_GETCURSEL, 0, 0);
    UINT dev = (UINT)(INT_PTR)SendMessageW(g.hDev, CB_GETITEMDATA, (WPARAM)(sel < 0 ? 0 : sel), 0);
    bool loop = SendMessageW(g.hLoop, BM_GETCHECK, 0, 0) == BST_CHECKED;
    mc::Song out = g.preview; mc::AddBendRangeRPN(out); ApplyMixer(out);
    uint32_t loopStart = startTick, endT = 0;
    if (V.selEnd > V.selStart) {                          // play / loop only the selected part
        if (startTick < V.selStart || startTick >= V.selEnd) startTick = V.selStart;
        loopStart = V.selStart; endT = V.selEnd;
    }
    std::wstring err;
    V.playing = false;
    if (!g.player.Start(out, dev, AudibleTracks(), startTick, loopStart, endT, loop, g.hwnd, err)) {
        MessageBoxW(g.hwnd, err.c_str(), APP_TITLE, MB_ICONERROR);
        return;
    }
    V.playing = true; V.playTick = startTick;
    SetTimer(g.hwnd, 1, 40, NULL);
}

static void PushUndo() {
    Snapshot s; s.base = g.base; s.chains = g.chains;
    s.muted = g.muted; s.solo = g.solo; s.trkVol = g.trkVol; s.trkPan = g.trkPan; s.trkProg = g.trkProg;
    g.undo.push_back(std::move(s));
    if (g.undo.size() > 10) g.undo.erase(g.undo.begin());
}

static void DoUndo() {
    if (g.undo.empty()) { SetStatus(L"Nothing to undo."); return; }
    DoStop();
    Snapshot s = std::move(g.undo.back()); g.undo.pop_back();
    const bool trackCountChanged = s.base.tracks.size() != g.base.tracks.size();       // e.g. a deleted track comes back
    g.base = std::move(s.base); g.chains = std::move(s.chains);
    if (trackCountChanged && s.muted.size() == g.base.tracks.size()) {
        g.muted = s.muted; g.solo = s.solo; g.trkVol = s.trkVol; g.trkPan = s.trkPan; g.trkProg = s.trkProg;
    }
    g.dirty = true;
    if (trackCountChanged) V.sel = -1;
    RebuildEverything(true);
    SetStatus(L"Undone (%u step(s) left).", (unsigned)g.undo.size());
}

// Bakes the chain(s) into the song.  only < 0: all tracks.
static void DoCommit(int only) {
    if (!g.loaded) return;
    if (g.bypassAll) { SetStatus(L"Switch off \"Bypass all FX\" first - applying uses the effects you are hearing."); return; }
    bool any = false;
    for (size_t i = 0; i < g.chains.size(); i++) if ((only < 0 || (int)i == only) && ChainActive((int)i)) any = true;
    if (!any) { SetStatus(L"There are no active effects to apply."); return; }
    DoStop();
    PushUndo();
    for (size_t i = 0; i < g.chains.size(); i++) {
        if (only >= 0 && (int)i != only) continue;
        if (ChainActive((int)i)) g.base.tracks[i] = g.preview.tracks[i];
        g.chains[i].clear();
    }
    g.dirty = true;
    RebuildEverything(true);
    SetStatus(L"Effects applied to the song data. Use Undo to take them back.");
}

// ===========================================================================
//  Marker Track, delete track, channel, play / stop
// ===========================================================================
static void SetLane(int i, bool on);
static void SetGhost(bool on);

static void SortMarkers() {
    int selTick = -1;
    if (g.selMarker >= 0 && g.selMarker < (int)g.markers.size()) selTick = (int)g.markers[(size_t)g.selMarker].tick;
    std::stable_sort(g.markers.begin(), g.markers.end(), [](const Marker& a, const Marker& b) { return a.tick < b.tick; });
    g.selMarker = -1;
    if (selTick >= 0) for (size_t i = 0; i < g.markers.size(); i++) if ((int)g.markers[i].tick == selTick) g.selMarker = (int)i;
}
static void MarkersChanged() { g.dirty = true; UpdateTitle(); InvalidateRect(g.hArr, NULL, FALSE); }

static void MarkerInsertAt(uint32_t tick) {
    if (!g.loaded) { SetStatus(L"Open a MIDI file first."); return; }
    for (size_t i = 0; i < g.markers.size(); i++) if (g.markers[i].tick == tick) { MarkerEdit((int)i); return; }   // one marker per position
    std::string txt;
    int bar, beat, tk; BarBeatTick(tick, bar, beat, tk);
    wchar_t lbl[160]; _snwprintf(lbl, 160, L"Marker text at bar %d, beat %d (letters and digits only):", bar, beat);
    if (!PromptAlnum(L"Insert marker", lbl, "", txt)) return;
    Marker m; m.tick = tick; m.text = txt;
    g.markers.push_back(m); g.selMarker = (int)g.markers.size() - 1; SortMarkers();
    MarkersChanged();
    SetStatus(L"Marker \"%ls\" inserted at bar %d, beat %d.", Utf8ToW(txt).c_str(), bar, beat);
}
static void MarkerEdit(int i) {
    if (i < 0 || i >= (int)g.markers.size()) return;
    std::string txt;
    if (!PromptAlnum(L"Edit marker", L"Marker text (letters and digits only):", g.markers[(size_t)i].text, txt)) return;
    g.markers[(size_t)i].text = txt; g.selMarker = i;
    MarkersChanged();
}
static void MarkerDelete(int i) {
    if (i < 0 || i >= (int)g.markers.size()) return;
    g.markers.erase(g.markers.begin() + i); g.selMarker = -1;
    MarkersChanged();
}
static void MarkerClearAll() {
    if (g.markers.empty()) return;
    if (MessageBoxW(g.hwnd, L"Delete all markers of the Marker Track?", APP_TITLE, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    g.markers.clear(); g.selMarker = -1;
    MarkersChanged();
}

// Right-click on a track -> "Delete track..." (asks first; Undo brings the track back)
static void DeleteTrack(int r) {
    if (!g.loaded || r < 0 || r >= (int)g.base.tracks.size()) return;
    if (g.base.tracks.size() <= 1) { SetStatus(L"The last track of the song cannot be deleted."); return; }
    const std::wstring name = TrackName(g.base.tracks[(size_t)r], r);
    bool conductor = false;
    for (const mc::Event& e : g.base.tracks[(size_t)r].events) if (e.IsMeta() && (e.d1 == 0x51 || e.d1 == 0x58)) conductor = true;
    std::wstring msg = L"Delete track " + std::to_wstring(r + 1) + L" \"" + name + L"\"?\r\n\r\n"
                       L"The track with its notes and its effect chain is removed from the song. Undo (Ctrl+Z) brings it back.";
    if (conductor) msg += L"\r\n\r\nThis track holds tempo / time-signature data. Without it the song plays at 120 BPM in 4/4.";
    if (MessageBoxW(g.hwnd, msg.c_str(), APP_TITLE, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    DoStop();
    PushUndo();
    const size_t i = (size_t)r;
    g.base.tracks.erase(g.base.tracks.begin() + (long)i);
    g.chains.erase(g.chains.begin() + (long)i);
    g.muted.erase(g.muted.begin() + (long)i); g.solo.erase(g.solo.begin() + (long)i);
    g.trkVol.erase(g.trkVol.begin() + (long)i); g.trkPan.erase(g.trkPan.begin() + (long)i); g.trkProg.erase(g.trkProg.begin() + (long)i);
    V.sel = std::min(r, (int)g.base.tracks.size() - 1);
    g.dirty = true;
    RebuildEverything(true);
    SetStatus(L"Track %d \"%ls\" deleted. Undo (Ctrl+Z) brings it back.", r + 1, name.c_str());
}

// Click on the Ch column -> choose the MIDI channel of the track (all events of its channel move to the new one)
static void ChooseChannel(int r) {
    if (r < 0 || r >= (int)TV.size() || TV[(size_t)r].notes.empty()) return;
    const int cur = TV[(size_t)r].ch;
    HMENU m = CreatePopupMenu();
    for (int c = 0; c < 16; c++) {
        wchar_t t[40]; _snwprintf(t, 40, c == 9 ? L"Channel %d (drums)" : L"Channel %d", c + 1);
        AppendMenuW(m, MF_STRING | (c == cur ? MF_CHECKED : 0), (UINT_PTR)(ID_CHAN0 + c), t);
    }
    POINT pt; GetCursorPos(&pt);
    const int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, g.hArr, NULL);
    DestroyMenu(m);
    if (cmd < ID_CHAN0 || cmd >= ID_CHAN0 + 16) return;
    const int nc = cmd - ID_CHAN0;
    if (nc == cur) return;
    PushUndo();
    for (mc::Event& e : g.base.tracks[(size_t)r].events)
        if (e.IsChannel() && e.Channel() == cur) e.status = (uint8_t)((e.status & 0xF0) | nc);
    int other = -1;
    for (size_t i = 0; i < TV.size(); i++) if ((int)i != r && !TV[i].notes.empty() && TV[i].ch == nc) other = (int)i;
    g.dirty = true;
    RebuildEverything(true);
    if (g.player.IsPlaying()) SetTimer(g.hwnd, 2, 150, NULL);                // hear it at once
    if (other >= 0) SetStatus(L"Track %d: channel %d -> %d.  Note: track %d also uses channel %d (they share instrument and controllers).", r + 1, cur + 1, nc + 1, other + 1, nc + 1);
    else SetStatus(L"Track %d: channel %d -> %d.", r + 1, cur + 1, nc + 1);
}

static void TogglePlay() {                                                  // Space bar
    if (!g.loaded) return;
    if (g.player.IsPlaying()) DoStop(); else DoPlayAt(V.startTick);
}

// ===========================================================================
//  Projects (*.rpj): the song, the effect chains, the mixer, the markers and the view settings in one file
// ===========================================================================
static std::string SerializeProject() {
    std::string o = "Rubato-Project 1\r\n";
    char b[320];
    o += "source=" + WToUtf8(g.path) + "\r\n";
    snprintf(b, sizeof b, "bend=%g\r\nstart=%u\r\nzoom=%g\r\nrowh=%d\r\nsel=%d\r\nghost=%d\r\nbypass=%d\r\nlanes=%d,%d,%d,%d,%d,%d\r\n",
             g.bendRange, (unsigned)V.startTick, V.pxPerBeat, V.rowH, V.sel, V.showGhost ? 1 : 0, g.bypassAll ? 1 : 0,
             V.laneOn[0] ? 1 : 0, V.laneOn[1] ? 1 : 0, V.laneOn[2] ? 1 : 0, V.laneOn[3] ? 1 : 0, V.laneOn[4] ? 1 : 0, V.laneOn[5] ? 1 : 0);
    o += b;
    for (size_t i = 0; i < g.base.tracks.size(); i++) {
        snprintf(b, sizeof b, "track=%u;%d;%d;%d;%d;%d\r\n", (unsigned)i, (int)g.muted[i], (int)g.solo[i], g.trkVol[i], g.trkPan[i], g.trkProg[i]);
        o += b;
        for (const ChainItem& it : g.chains[i]) {
            snprintf(b, sizeof b, "fx=%u;%s;%d", (unsigned)i, WToUtf8(mc::GetFxDesc(it.type).id).c_str(), it.enabled ? 1 : 0);
            o += b;
            for (int k = 0; k < mc::FX_MAXP; k++) { snprintf(b, sizeof b, ";%.9g", it.p[k]); o += b; }
            snprintf(b, sizeof b, ";%d;%d;%d;%d\r\n", it.mode, it.fromBar, it.toBar, it.seed);
            o += b;
        }
    }
    for (const Marker& m : g.markers) { snprintf(b, sizeof b, "marker=%u;", (unsigned)m.tick); o += b; o += m.text + "\r\n"; }
    mc::Song song = g.base;                       // the committed song (the chains above are the live, not yet applied part)
    o += "midi=" + B64Enc(mc::WriteSMF(song)) + "\r\n";
    return o;
}

static std::vector<std::string> SplitSemi(const std::string& s) {
    std::vector<std::string> v; size_t a = 0;
    for (;;) { size_t b = s.find(';', a); v.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a)); if (b == std::string::npos) break; a = b + 1; }
    return v;
}

static void LoadProject(const std::wstring& path) {
    std::vector<uint8_t> bytes;
    if (!ReadAll(path, bytes)) { MessageBoxW(g.hwnd, L"Cannot read the project file.", APP_TITLE, MB_ICONERROR); RemoveRecent(FullPath(path)); return; }
    const std::string txt(bytes.begin(), bytes.end());
    std::string midi, source, lanes;
    std::vector<std::string> trackL, fxL, markL;
    double bend = 2.0, zoom = V.pxPerBeat; int rowh = V.rowH, sel = -1, ghost = 1, bypass = 0; unsigned startTick = 0;
    bool header = false;
    size_t pos = 0;
    while (pos <= txt.size()) {
        size_t e = txt.find('\n', pos); if (e == std::string::npos) e = txt.size();
        std::string line = txt.substr(pos, e - pos); pos = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) {
            if (!header) { if (line.compare(0, 14, "Rubato-Project") != 0) break; header = true; }
            else if (line.compare(0, 5, "midi=") == 0) midi = line.substr(5);
            else if (line.compare(0, 7, "source=") == 0) source = line.substr(7);
            else if (line.compare(0, 5, "bend=") == 0) bend = atof(line.c_str() + 5);
            else if (line.compare(0, 6, "start=") == 0) startTick = (unsigned)strtoul(line.c_str() + 6, NULL, 10);
            else if (line.compare(0, 5, "zoom=") == 0) zoom = atof(line.c_str() + 5);
            else if (line.compare(0, 5, "rowh=") == 0) rowh = atoi(line.c_str() + 5);
            else if (line.compare(0, 4, "sel=") == 0) sel = atoi(line.c_str() + 4);
            else if (line.compare(0, 6, "ghost=") == 0) ghost = atoi(line.c_str() + 6);
            else if (line.compare(0, 7, "bypass=") == 0) bypass = atoi(line.c_str() + 7);
            else if (line.compare(0, 6, "lanes=") == 0) lanes = line.substr(6);
            else if (line.compare(0, 6, "track=") == 0) trackL.push_back(line.substr(6));
            else if (line.compare(0, 3, "fx=") == 0) fxL.push_back(line.substr(3));
            else if (line.compare(0, 7, "marker=") == 0) markL.push_back(line.substr(7));
        }
        if (e >= txt.size()) break;
    }
    mc::Song song; std::wstring err; std::vector<uint8_t> smf;
    if (!header || midi.empty() || !B64Dec(midi, smf) || !mc::ReadSMF(smf, song, err)) {
        MessageBoxW(g.hwnd, L"This is not a valid Rubato project file.", APP_TITLE, MB_ICONERROR); return;
    }
    DoStop();
    const size_t n = song.tracks.size();
    g.base = std::move(song);
    g.bendRange = std::max(0.5, std::min(24.0, bend)); g.base.bendRange = g.bendRange;
    g.path = Utf8ToW(source); g.projPath = FullPath(path); g.loaded = true; g.dirty = false;
    g.undo.clear();
    g.muted.assign(n, 0); g.solo.assign(n, 0); g.trkVol.assign(n, -1); g.trkPan.assign(n, -1); g.trkProg.assign(n, -1);
    g.chains.assign(n, Chain());
    for (const std::string& t : trackL) {
        std::vector<std::string> k = SplitSemi(t);
        if (k.size() < 6) continue;
        const size_t i = (size_t)atoi(k[0].c_str()); if (i >= n) continue;
        g.muted[i] = atoi(k[1].c_str()) != 0; g.solo[i] = atoi(k[2].c_str()) != 0;
        g.trkVol[i] = std::max(-1, std::min(127, atoi(k[3].c_str()))); g.trkPan[i] = std::max(-1, std::min(127, atoi(k[4].c_str())));
        g.trkProg[i] = std::max(-1, std::min(127, atoi(k[5].c_str())));
    }
    for (const std::string& f : fxL) {
        std::vector<std::string> k = SplitSemi(f);
        const size_t base = 3 + (size_t)mc::FX_MAXP;
        if (k.size() < base + 3) continue;
        const size_t ti = (size_t)atoi(k[0].c_str()); if (ti >= n) continue;
        const int type = FxIndexById(Utf8ToW(k[1])); if (type < 0) continue;
        ChainItem c = MakeItem(type);
        c.enabled = atoi(k[2].c_str()) != 0;
        for (int q = 0; q < mc::FX_MAXP; q++) c.p[q] = strtod(k[3 + (size_t)q].c_str(), NULL);
        for (int q = 0; q < mc::GetFxDesc(type).np; q++) c.p[q] = mc::ClampParam(type, q, c.p[q]);
        c.mode = std::max(0, std::min(2, atoi(k[base].c_str())));
        c.fromBar = std::max(1, atoi(k[base + 1].c_str())); c.toBar = std::max(1, atoi(k[base + 2].c_str()));
        if (k.size() > base + 3) c.seed = std::max(1, atoi(k[base + 3].c_str()));
        g.chains[ti].push_back(c);
    }
    g.markers.clear(); g.selMarker = -1;
    for (const std::string& m : markL) {
        const size_t sc = m.find(';'); if (sc == std::string::npos) continue;
        Marker mk; mk.tick = (uint32_t)strtoul(m.substr(0, sc).c_str(), NULL, 10); mk.text = SanitizeAlnum(m.substr(sc + 1));
        if (!mk.text.empty()) g.markers.push_back(mk);
    }
    std::stable_sort(g.markers.begin(), g.markers.end(), [](const Marker& a, const Marker& b) { return a.tick < b.tick; });

    V.scrollTick = 0; V.scrollY = 0; V.sel = std::min(sel, (int)n - 1); V.selStart = V.selEnd = 0;
    V.pxPerBeat = std::max(4.0, std::min(600.0, zoom)); V.rowH = std::max(40, std::min(240, rowh));
    g.bypassAll = bypass != 0;
    if (g.hTb) SendMessageW(g.hTb, TB_CHECKBUTTON, ID_BYPASS, MAKELPARAM(g.bypassAll ? TRUE : FALSE, 0));
    if (g.hMenu) CheckMenuItem(g.hMenu, ID_BYPASS, MF_BYCOMMAND | (g.bypassAll ? MF_CHECKED : MF_UNCHECKED));
    { int v[6] = { 1, 1, 1, 1, 0, 1 };
      if (!lanes.empty()) { std::vector<std::string> q; size_t a = 0; for (;;) { size_t b = lanes.find(',', a); q.push_back(lanes.substr(a, b == std::string::npos ? std::string::npos : b - a)); if (b == std::string::npos) break; a = b + 1; }
                            for (int i = 0; i < 6 && i < (int)q.size(); i++) v[i] = atoi(q[(size_t)i].c_str()) != 0; }
      for (int i = 0; i < LANE_COUNT; i++) SetLane(i, v[i] != 0); }
    SetGhost(ghost != 0);
    { wchar_t b[32]; _snwprintf(b, 32, L"%g", g.bendRange); s_bendSilent = true; SetWindowTextW(g.hBend, b); s_bendSilent = false; }
    RebuildEverything(true);
    V.startTick = std::min<uint32_t>(startTick, V.lenTicks);
    if (V.sel >= 0) { const int s2 = V.sel; V.sel = -1; SelectTrack(s2); }
    UpdateArrangeScroll(); InvalidateRect(g.hArr, NULL, FALSE);
    AddRecent(path); UpdateTitle();
    unsigned fx = 0; for (auto& c : g.chains) fx += (unsigned)c.size();
    SetStatus(L"Project loaded: %u tracks, %u effects, %u markers.", (unsigned)n, fx, (unsigned)g.markers.size());
}

static bool SaveProject(bool saveAs) {
    if (!g.loaded) return false;
    wchar_t buf[MAX_PATH * 2] = L"";
    if (g.projPath.empty() || saveAs) {
        const std::wstring& src = !g.projPath.empty() ? g.projPath : g.path;
        if (!src.empty()) {
            size_t sl = src.find_last_of(L"\\/"), dot = src.find_last_of(L'.');
            std::wstring dir = sl == std::wstring::npos ? L"" : src.substr(0, sl + 1);
            std::wstring name = FileStem(src);
            (void)dot;
            wcsncpy(buf, (dir + name + L".rpj").c_str(), MAX_PATH * 2 - 1);
        }
        OPENFILENAMEW o; memset(&o, 0, sizeof o);
        o.lStructSize = sizeof o; o.hwndOwner = g.hwnd;
        o.lpstrFilter = L"Rubato projects (*.rpj)\0*.rpj\0All files (*.*)\0*.*\0";
        o.lpstrFile = buf; o.nMaxFile = MAX_PATH * 2; o.lpstrDefExt = L"rpj";
        o.lpstrTitle = L"Save Rubato project";
        o.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_HIDEREADONLY;
        if (!GetSaveFileNameW(&o)) return false;
    } else wcsncpy(buf, g.projPath.c_str(), MAX_PATH * 2 - 1);
    const std::string txt = SerializeProject();
    if (!WriteAll(buf, txt.data(), txt.size())) { MessageBoxW(g.hwnd, L"Cannot write the project file.", APP_TITLE, MB_ICONERROR); return false; }
    g.projPath = FullPath(buf); g.dirty = false;
    AddRecent(g.projPath); UpdateTitle();
    SetStatus(L"Project saved: %ls", buf);
    return true;
}

static bool HasExt(const std::wstring& p, const wchar_t* ext) {
    const size_t d = p.find_last_of(L'.');
    return d != std::wstring::npos && _wcsicmp(p.c_str() + d, ext) == 0;
}
// the file decides: *.rpj = project, everything else is read as a MIDI file
static void OpenAny(const std::wstring& path) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(g.hwnd, (L"The file no longer exists:\r\n" + path).c_str(), APP_TITLE, MB_ICONERROR);
        RemoveRecent(FullPath(path)); return;
    }
    if (HasExt(path, L".rpj")) LoadProject(path); else LoadFile(path);
}

static void SetBypass(bool on) {
    g.bypassAll = on;
    if (g.hTb) SendMessageW(g.hTb, TB_CHECKBUTTON, ID_BYPASS, MAKELPARAM(on ? TRUE : FALSE, 0));
    if (g.hMenu) CheckMenuItem(g.hMenu, ID_BYPASS, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
    if (!g.loaded) return;
    for (size_t i = 0; i < g.base.tracks.size(); i++) RecomputeTrack((int)i);
    RefreshAfterRecompute();
    SyncDialog();
    if (g.player.IsPlaying()) SetTimer(g.hwnd, 2, 150, NULL);
    SetStatus(on ? L"All effects bypassed - you are seeing and hearing the original." : L"Effects active.");
}

static void SetLane(int i, bool on) {
    V.laneOn[i] = on;
    SendMessageW(g.hLane[i], BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g.hMenu) CheckMenuItem(g.hMenu, ID_LANE0 + i, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
    InvalidateRect(g.hArr, NULL, FALSE);
}

static void SetGhost(bool on) {
    V.showGhost = on;
    SendMessageW(g.hGhost, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g.hMenu) CheckMenuItem(g.hMenu, ID_GHOST, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
    InvalidateRect(g.hArr, NULL, FALSE);
}

static void ChangeRowHeight(int dir) {
    static const int sizes[] = { 40, 56, 72, 96, 128, 176, 240 };
    int idx = 2;
    for (int i = 0; i < 7; i++) if (sizes[i] <= V.rowH) idx = i;
    idx = std::max(0, std::min(6, idx + dir));
    V.rowH = sizes[idx];
    UpdateArrangeScroll(); InvalidateRect(g.hArr, NULL, FALSE);
}

static void ChangeZoom(double f) {
    V.pxPerBeat = std::max(4.0, std::min(600.0, V.pxPerBeat * f));
    UpdateArrangeScroll(); InvalidateRect(g.hArr, NULL, FALSE);
}

static void OnBendRangeEdited() {
    if (s_bendSilent) return;
    double v;
    if (!EditNumber(g.hBend, v)) return;                       // empty / half-typed
    v = std::max(0.5, std::min(24.0, v));                      // limited to 0.5 .. 24 semitones
    g.bendRange = v;
    if (!g.loaded || g.preview.tracks.size() != g.base.tracks.size() || TV.size() != g.base.tracks.size()) return;   // views not built yet
    g.base.bendRange = v; g.preview.bendRange = v;
    for (size_t i = 0; i < g.base.tracks.size(); i++) if (ChainActive((int)i)) RecomputeTrack((int)i);
    RefreshAfterRecompute();
    if (g.player.IsPlaying()) SetTimer(g.hwnd, 2, 150, NULL);
}

static void DoCopyAll() {
    int s = V.sel;
    if (s < 0 || g.chains[(size_t)s].empty()) { SetStatus(L"Select a track that has effects first."); return; }
    Chain src = g.chains[(size_t)s];
    int n = 0;
    for (size_t i = 0; i < g.chains.size(); i++) {
        if ((int)i == s || TV[i].notes.empty()) continue;
        g.chains[i] = src;
        for (ChainItem& it : g.chains[i]) it.seed = NewSeed();
        RecomputeTrack((int)i);
        n++;
    }
    g.dirty = true;
    RefreshAfterRecompute();
    SyncDialog();
    SetStatus(L"Chain copied to %d other track(s).", n);
}

static const wchar_t* HELP_TEXT =
    L"QUICK GUIDE\r\n\r\n"
    L"1. Open a MIDI file. Each track has a coloured strip with its notes; the coloured line graphs on top are the "
    L"controllers (pitch bend, expression, modulation, breath, volume, velocity). Use the buttons at the bottom to show or hide them.\r\n\r\n"
    L"2. Click a track, then its FX button (or double-click the strip) to open the MFX Chain dialog. Insert effects, "
    L"change their parameters and watch the graphs change live. Press Play to hear it - playback restarts automatically "
    L"when you change a parameter.\r\n\r\n"
    L"3. Nothing is changed in the song until you press Apply All FXs. Until then the red \"Rubatoing\" "
    L"indicator shows that the preview differs from the original (it reads \"Original\" when no track has an effect in progress). \"Bypass FX\" switches back to the original for A/B listening.\r\n\r\n"
    L"4. Export MIDI writes exactly what you see and hear. Undo restores the song to before the last Apply (or brings back a deleted track).\r\n\r\n"
    L"5. Drag on the ruler to select a part of the song (blue). Play then plays only that part and Loop repeats it, so you can "
    L"check an effect on a few bars. Switch Loop off while it plays and the music simply continues to the end of the song. "
    L"In the chain dialog, \"From selection\" turns the selection into the From/To bars of an effect.\r\n\r\n"
    L"6. In the track list, drag the Vol and Pan sliders (double-click resets). Click the Patch box to choose another General MIDI "
    L"instrument for the track (orange = changed from the file; \"As in the file\" undoes it). Drag the column dividers in the header "
    L"to resize the columns. Mute and Solo work while playing.\r\n\r\n"
    L"7. A format 0 MIDI file (all channels in one track) is split into one track per channel when it is opened.\r\n\r\n"
    L"8. Marker Track (top row): double-click to insert a text marker (letters and digits only), double-click a marker to edit it, \r\n"
    L"right-click for the menu, Delete removes the selected marker. Markers are written into the exported MIDI file.\r\n\r\n"
    L"9. Right-click a track to delete it (asks first, Undo brings it back). Click the Ch box to move a track to another MIDI channel.\r\n\r\n"
    L"10. Space bar = play / stop. Clicking the ruler while the music plays continues from there.\r\n\r\n"
    L"11. File menu: Save project (*.rpj) keeps the song, effect chains, mixer and markers; Export MIDI file always writes format 0. \r\n"
    L"Presets use the extension *.rps. The File menu lists the recently opened files.\r\n\r\n"
    L"Mouse: wheel = scroll tracks, Shift+wheel or touch-pad sideways swipe = scroll time, Ctrl+wheel = zoom. Click a strip to set the "
    L"start position (blue line). The overview above the ruler jumps to any part of the song.";

// ===========================================================================
//  MFX Chain dialog (modeless)
// ===========================================================================
static Chain* DlgChain() {
    int t = g.dlg.track;
    if (t < 0 || t >= (int)g.chains.size()) return NULL;
    return &g.chains[(size_t)t];
}
static int DlgSel() { return ListView_GetNextItem(g.dlg.list, -1, LVNI_SELECTED); }
static ChainItem* DlgItem() {
    Chain* c = DlgChain(); if (!c) return NULL;
    int i = DlgSel();
    if (i < 0 || i >= (int)c->size()) return NULL;
    return &(*c)[(size_t)i];
}

static void DlgSetRow(int i, bool insert) {
    Chain* c = DlgChain(); if (!c) return;
    const ChainItem& it = (*c)[(size_t)i];
    const mc::FxDesc& d = mc::GetFxDesc(it.type);
    wchar_t n[16], f[16], t[16];
    _snwprintf(n, 16, L"%d", i + 1);
    _snwprintf(f, 16, L"%d", it.fromBar);
    _snwprintf(t, 16, L"%d", it.toBar);
    if (insert) {
        LVITEMW lv; memset(&lv, 0, sizeof lv);
        lv.mask = LVIF_TEXT; lv.iItem = i; lv.pszText = n;
        SendMessageW(g.dlg.list, LVM_INSERTITEMW, 0, (LPARAM)&lv);
    } else {
        ListView_SetItemText(g.dlg.list, i, 0, n);
    }
    ListView_SetItemText(g.dlg.list, i, 1, (LPWSTR)(it.enabled ? L"" : L"x"));
    ListView_SetItemText(g.dlg.list, i, 2, (LPWSTR)d.name);
    ListView_SetItemText(g.dlg.list, i, 3, (LPWSTR)(it.mode == mc::RANGE_ALL ? L"x" : L""));
    ListView_SetItemText(g.dlg.list, i, 4, (LPWSTR)(it.mode == mc::RANGE_EXC ? L"x" : L""));
    ListView_SetItemText(g.dlg.list, i, 5, (LPWSTR)(it.mode == mc::RANGE_USE ? L"x" : L""));
    ListView_SetItemText(g.dlg.list, i, 6, it.mode == mc::RANGE_ALL ? (LPWSTR)L"" : f);
    ListView_SetItemText(g.dlg.list, i, 7, it.mode == mc::RANGE_ALL ? (LPWSTR)L"" : t);
}

// A piece of text of the description box in the chain dialog, with its own colour.
struct DescSeg { std::wstring text; COLORREF col; bool bold; COLORREF bg = CLR_INVALID; };   // bg: highlight behind the text (CLR_INVALID = none)
static const COLORREF COL_HOWTO = RGB(0, 70, 200);     // "HOW IT WORKS" and its text: blue
static const COLORREF COL_PARAM = RGB(210, 105, 0);    // (kept for reference: the former orange)
static const COLORREF COL_PARAMHINT = RGB(0, 100, 30); // "PARAMETER: ..." text, its hint and the "Click in a parameter box" line: dark green
static const COLORREF COL_LANEBG = RGB(34, 38, 48);    // dark chip behind a result name, so that the light graph colours stay readable
static const COLORREF COL_PLAIN = RGB(0, 0, 0);

static void AddSeg(std::vector<DescSeg>& v, const std::wstring& t, COLORREF c, bool bold = false) {
    DescSeg d; d.text = t; d.col = c; d.bold = bold; v.push_back(d);
}
static void AddLane(std::vector<DescSeg>& v, int lane, const std::wstring& t) {     // a name in the colour of its graph line
    DescSeg d; d.text = t; d.col = LM[lane].col; d.bold = true; d.bg = COL_LANEBG; v.push_back(d);
}
static void AddFmt(std::vector<DescSeg>& v, COLORREF c, const wchar_t* fmt, ...) {
    wchar_t b[256];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf(b, 255, fmt, ap); b[255] = 0;
    va_end(ap);
    AddSeg(v, b, c);
}

// Result of the chain on the selected track: each name has the colour of its graph line.
static void AppendSummary(int ti, std::vector<DescSeg>& out) {
    if (ti < 0 || ti >= (int)g.chains.size()) return;
    if (g.chains[(size_t)ti].empty()) { AddSeg(out, L"This track has no effects. Choose Insert to add one, or load a preset with the folder button.", COL_PLAIN); return; }
    if (!ChainActive(ti)) { AddSeg(out, L"The chain is bypassed (all effects switched off with M, or \"Bypass FX\" is on): the original is used.", COL_PLAIN); return; }
    struct C { int notes = 0, bend = 0, cc1 = 0, cc2 = 0, cc11 = 0; double vel = 0, len = 0; };
    auto count = [](const mc::Track& t) {
        C c;
        for (const mc::Event& e : t.events) {
            if (e.IsNoteOn()) { c.notes++; c.vel += e.d2; }
            else if (e.IsChannel()) {
                int hi = e.status & 0xF0;
                if (hi == 0xE0) c.bend++;
                else if (hi == 0xB0) { if (e.d1 == 1) c.cc1++; else if (e.d1 == 2) c.cc2++; else if (e.d1 == 11) c.cc11++; }
            }
        }
        std::vector<mc::Note> n; std::vector<mc::Event> o; mc::ExtractNotes(t, n, o);
        for (const mc::Note& x : n) c.len += (double)(x.end - x.start);
        return c;
    };
    C a = count(g.base.tracks[(size_t)ti]), b = count(g.preview.tracks[(size_t)ti]);
    AddSeg(out, L"Result of this chain on the track  (original -> processed)\r\n", COL_PLAIN, true);
    AddSeg(out, L"Notes:", COL_PLAIN, true);
    AddFmt(out, COL_PLAIN, L" %d -> %d     ", a.notes, b.notes);
    AddLane(out, LANE_VEL, L" Average velocity: ");
    AddFmt(out, COL_PLAIN, L" %.0f -> %.0f     ", a.notes ? a.vel / a.notes : 0.0, b.notes ? b.vel / b.notes : 0.0);
    AddSeg(out, L"Total note length:", COL_PLAIN, true);
    AddFmt(out, COL_PLAIN, L" %.0f%%\r\n", a.len > 0 ? 100.0 * b.len / a.len : 100.0);
    AddLane(out, LANE_BEND, L" Pitch-bend events: ");
    AddFmt(out, COL_PLAIN, L" %d -> %d     ", a.bend, b.bend);
    AddLane(out, LANE_MOD, L" Modulation (CC1): ");
    AddFmt(out, COL_PLAIN, L" %d -> %d\r\n", a.cc1, b.cc1);
    AddLane(out, LANE_BREATH, L" Breath (CC2): ");
    AddFmt(out, COL_PLAIN, L" %d -> %d     ", a.cc2, b.cc2);
    AddLane(out, LANE_EXPR, L" Expression (CC11): ");
    AddFmt(out, COL_PLAIN, L" %d -> %d", a.cc11, b.cc11);
}

// Puts the coloured pieces into the description box (a rich-text control; plain text when that is not available).
static void SetDescText(const std::vector<DescSeg>& segs) {
    HWND h = g.dlg.desc;
    if (!h) return;
    if (!g_rich) {
        std::wstring all;
        for (const DescSeg& d : segs) all += d.text;
        SetWindowTextW(h, all.c_str());
        return;
    }
    std::wstring all;                                      // a rich-text control counts "\r\n" as one character: use "\r" only
    std::vector<std::pair<LONG, LONG> > rng;
    for (const DescSeg& d : segs) {
        const LONG a = (LONG)all.size();
        for (size_t i = 0; i < d.text.size(); i++) {
            if (d.text[i] == L'\n' && i > 0 && d.text[i - 1] == L'\r') continue;
            all += d.text[i];
        }
        rng.push_back(std::make_pair(a, (LONG)all.size()));
    }
    SendMessageW(h, WM_SETREDRAW, FALSE, 0);
    SendMessageW(h, EM_SETREADONLY, FALSE, 0);             // (formatting is applied as an edit)
    SetWindowTextW(h, all.c_str());
    CHARFORMAT2W cf; memset(&cf, 0, sizeof cf); cf.cbSize = sizeof cf;
    cf.dwMask = CFM_COLOR | CFM_BOLD | CFM_BACKCOLOR;
    cf.crTextColor = COL_PLAIN; cf.dwEffects = CFE_AUTOBACKCOLOR;
    SendMessageW(h, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
    for (size_t i = 0; i < segs.size(); i++) {
        if (rng[i].second <= rng[i].first) continue;
        CHARRANGE cr; cr.cpMin = rng[i].first; cr.cpMax = rng[i].second;
        SendMessageW(h, EM_EXSETSEL, 0, (LPARAM)&cr);
        cf.crTextColor = segs[i].col;
        cf.dwEffects = segs[i].bold ? CFE_BOLD : 0;
        if (segs[i].bg == CLR_INVALID) cf.dwEffects |= CFE_AUTOBACKCOLOR; else cf.crBackColor = segs[i].bg;
        SendMessageW(h, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    }
    CHARRANGE top; top.cpMin = 0; top.cpMax = 0;
    SendMessageW(h, EM_EXSETSEL, 0, (LPARAM)&top);          // back to the top, nothing selected
    SendMessageW(h, EM_SETREADONLY, TRUE, 0);
    SendMessageW(h, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(h, NULL, TRUE);
}

static void DlgUpdateDesc() {
    if (!g.dlg.hwnd) return;
    ChainItem* it = DlgItem();
    std::vector<DescSeg> v;
    if (it) {
        const mc::FxDesc& d = mc::GetFxDesc(it->type);
        AddSeg(v, L"HOW IT WORKS\r\n", COL_HOWTO, true);                     // blue
        AddSeg(v, d.hint, COL_HOWTO);
        AddSeg(v, L"\r\n\r\n", COL_PLAIN);
        const int k = g.dlg.focusParam;
        if (k >= 0 && k < d.np) {                                           // orange
            AddSeg(v, std::wstring(L"PARAMETER: ") + d.label[k] + L"\r\n", COL_PARAMHINT, true);   // dark green
            AddSeg(v, d.phint[k], COL_PARAMHINT);
        } else {
            AddSeg(v, L"Click in a parameter box to see what it does.", COL_PARAMHINT);
        }
        AddSeg(v, L"\r\n\r\n", COL_PLAIN);
    }
    AppendSummary(g.dlg.track, v);
    SetDescText(v);
}

static void DlgShowParams() {
    ChainItem* it = DlgItem();
    g.dlg.refreshing = true;
    g.dlg.focusParam = -1;
    for (int k = 0; k < mc::FX_MAXP; k++) {
        bool on = it && k < mc::GetFxDesc(it->type).np;
        ShowWindow(g.dlg.lbl[k], on ? SW_SHOW : SW_HIDE);
        ShowWindow(g.dlg.ed[k], on ? SW_SHOW : SW_HIDE);
        ShowWindow(g.dlg.spin[k], on ? SW_SHOW : SW_HIDE);
        if (on) {
            SetWindowTextW(g.dlg.lbl[k], mc::GetFxDesc(it->type).label[k]);
            wchar_t b[40]; _snwprintf(b, 40, L"%g", it->p[k]);
            SetWindowTextW(g.dlg.ed[k], b);
        }
    }
    const int show = it ? SW_SHOW : SW_HIDE;
    ShowWindow(g.dlg.rbAll, show); ShowWindow(g.dlg.rbExc, show); ShowWindow(g.dlg.rbUse, show);
    ShowWindow(g.dlg.lblFrom, show); ShowWindow(g.dlg.edFrom, show);
    ShowWindow(g.dlg.lblTo, show); ShowWindow(g.dlg.edTo, show);
    ShowWindow(g.dlg.spinFrom, show); ShowWindow(g.dlg.spinTo, show);
    ShowWindow(g.dlg.reroll, show); ShowWindow(g.dlg.fromSel, show);
    if (it) {
        SendMessageW(g.dlg.rbAll, BM_SETCHECK, it->mode == mc::RANGE_ALL ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(g.dlg.rbExc, BM_SETCHECK, it->mode == mc::RANGE_EXC ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(g.dlg.rbUse, BM_SETCHECK, it->mode == mc::RANGE_USE ? BST_CHECKED : BST_UNCHECKED, 0);
        wchar_t b[24];
        _snwprintf(b, 24, L"%d", it->fromBar); SetWindowTextW(g.dlg.edFrom, b);
        _snwprintf(b, 24, L"%d", it->toBar);    SetWindowTextW(g.dlg.edTo, b);
        EnableWindow(g.dlg.edFrom, it->mode != mc::RANGE_ALL);
        EnableWindow(g.dlg.edTo, it->mode != mc::RANGE_ALL);
        EnableWindow(g.dlg.spinFrom, it->mode != mc::RANGE_ALL);
        EnableWindow(g.dlg.spinTo, it->mode != mc::RANGE_ALL);
    }
    g.dlg.refreshing = false;
}

static void DlgFillList(int select) {
    g.dlg.refreshing = true;
    ListView_DeleteAllItems(g.dlg.list);
    Chain* c = DlgChain();
    if (c) for (size_t i = 0; i < c->size(); i++) DlgSetRow((int)i, true);
    if (c && select >= 0 && select < (int)c->size())
        ListView_SetItemState(g.dlg.list, select, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    g.dlg.refreshing = false;
    DlgShowParams();
    DlgUpdateDesc();
}

static void SyncDialog() {
    if (!g.dlg.hwnd) return;
    g.dlg.track = V.sel;
    wchar_t t[96];
    if (V.sel >= 0) _snwprintf(t, 96, L"MFX Chain: Track: %d", V.sel + 1); else wcscpy(t, L"MFX Chain: (select a track)");
    SetWindowTextW(g.dlg.hwnd, t);
    DlgFillList(0);
}

static void DlgInsert() {
    Chain* c = DlgChain();
    if (!c) { SetStatus(L"Select a track first."); return; }
    HMENU m = CreatePopupMenu();
    for (int i = 0; i < mc::FX_COUNT; i++) AppendMenuW(m, MF_STRING, (UINT_PTR)(ID_FXMENU0 + i), mc::GetFxDesc(i).name);
    RECT r; GetWindowRect(g.dlg.insert, &r);
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_BOTTOMALIGN, r.left, r.top, 0, g.dlg.hwnd, NULL);
    DestroyMenu(m);
    if (cmd < ID_FXMENU0 || cmd >= ID_FXMENU0 + mc::FX_COUNT) return;
    int at = DlgSel() >= 0 ? DlgSel() + 1 : (int)c->size();
    c->insert(c->begin() + at, MakeItem(cmd - ID_FXMENU0));
    DlgFillList(at);
    OnChainChanged(g.dlg.track);
    DlgUpdateDesc();
}

static void DlgDelete() {
    Chain* c = DlgChain(); int s = DlgSel();
    if (!c || s < 0 || s >= (int)c->size()) return;
    c->erase(c->begin() + s);
    DlgFillList(std::min(s, (int)c->size() - 1));
    OnChainChanged(g.dlg.track);
    DlgUpdateDesc();
}

static void DlgMove(int dir) {
    Chain* c = DlgChain(); int s = DlgSel();
    if (!c || s < 0 || s + dir < 0 || s + dir >= (int)c->size()) return;
    std::swap((*c)[(size_t)s], (*c)[(size_t)(s + dir)]);
    DlgFillList(s + dir);
    OnChainChanged(g.dlg.track);
    DlgUpdateDesc();
}

static void DlgParamEdited() {
    if (g.dlg.refreshing) return;
    ChainItem* it = DlgItem(); if (!it) return;
    const mc::FxDesc& d = mc::GetFxDesc(it->type);
    for (int k = 0; k < d.np; k++) {
        double v;
        if (EditNumber(g.dlg.ed[k], v)) it->p[k] = mc::ClampParam(it->type, k, v);      // always within min / max (a half-typed "-" keeps the old value)
    }
    OnChainChanged(g.dlg.track);
    DlgUpdateDesc();
}

static void DlgRangeEdited() {
    if (g.dlg.refreshing) return;
    ChainItem* it = DlgItem(); if (!it) return;
    if (SendMessageW(g.dlg.rbExc, BM_GETCHECK, 0, 0) == BST_CHECKED) it->mode = mc::RANGE_EXC;
    else if (SendMessageW(g.dlg.rbUse, BM_GETCHECK, 0, 0) == BST_CHECKED) it->mode = mc::RANGE_USE;
    else it->mode = mc::RANGE_ALL;
    wchar_t b[24] = L"";
    double v;
    if (EditNumber(g.dlg.edFrom, v)) it->fromBar = std::max(1, std::min(9999, (int)v));
    if (EditNumber(g.dlg.edTo, v))   it->toBar   = std::max(1, std::min(9999, (int)v));
    (void)b;
    EnableWindow(g.dlg.edFrom, it->mode != mc::RANGE_ALL);
    EnableWindow(g.dlg.edTo, it->mode != mc::RANGE_ALL);
    EnableWindow(g.dlg.spinFrom, it->mode != mc::RANGE_ALL);
    EnableWindow(g.dlg.spinTo, it->mode != mc::RANGE_ALL);
    DlgSetRow(DlgSel(), false);
    OnChainChanged(g.dlg.track);
    DlgUpdateDesc();
}

static void DlgFromSelection() {
    ChainItem* it = DlgItem(); if (!it) return;
    if (V.selEnd <= V.selStart) { SetStatus(L"First select a part of the song: drag on the ruler above the tracks."); return; }
    const int beatTicks = std::max(1, V.ppq * 4 / std::max(1, V.tsDen)), barTicks = beatTicks * std::max(1, V.tsNum);
    it->fromBar = (int)(V.selStart / (uint32_t)barTicks) + 1;
    it->toBar = (int)((V.selEnd - 1) / (uint32_t)barTicks) + 1;
    if (it->mode == mc::RANGE_ALL) it->mode = mc::RANGE_USE;
    DlgSetRow(DlgSel(), false);
    DlgShowParams();
    OnChainChanged(g.dlg.track);
    DlgUpdateDesc();
    SetStatus(L"Range set from the timeline selection: bars %d to %d.", it->fromBar, it->toBar);
}

static void DlgReroll() {
    ChainItem* it = DlgItem(); if (!it) return;
    it->seed = NewSeed();
    const mc::FxDesc& d = mc::GetFxDesc(it->type);
    if (d.seedIdx >= 0 && mc::RoundI(it->p[d.seedIdx]) != 0) {
        it->p[d.seedIdx] = 0;                                  // back to "auto" so that the new seed takes effect
        DlgShowParams();
    }
    OnChainChanged(g.dlg.track);
    DlgUpdateDesc();
    SetStatus(L"New random variation generated.");
}

static void ApplyPresetItems(const Chain& items, const std::wstring& label) {
    Chain* c = DlgChain();
    if (!c) { SetStatus(L"Select a track first."); return; }
    *c = items;
    for (ChainItem& it : *c) it.seed = NewSeed();
    DlgFillList(0);
    OnChainChanged(g.dlg.track);
    DlgUpdateDesc();
    SetStatus(L"Preset \"%ls\" loaded into this track's chain as a live preview. Press Apply All FXs to keep it.", label.c_str());
}

static void DlgPresets() {
    if (!DlgChain()) { SetStatus(L"Select a track first."); return; }
    ScanPresets();
    HMENU m = CreatePopupMenu();
    std::vector<std::wstring> cats; std::vector<HMENU> subs;
    for (size_t i = 0; i < g.presets.size(); i++) {                  // one sub-menu per category, in the order they appear
        size_t c = 0;
        while (c < cats.size() && cats[c] != g.presets[i].category) c++;
        if (c == cats.size()) { cats.push_back(g.presets[i].category); subs.push_back(CreatePopupMenu()); }
        AppendMenuW(subs[c], MF_STRING, (UINT_PTR)(ID_PRESETMENU0 + i), g.presets[i].label.c_str());
    }
    for (size_t c = 0; c < cats.size(); c++) AppendMenuW(m, MF_POPUP, (UINT_PTR)subs[c], cats[c].c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_PRESET_BROWSE, L"Open preset file...");
    RECT r; GetWindowRect(g.dlg.tb, &r);
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, r.left + 4, r.bottom, 0, g.dlg.hwnd, NULL);
    DestroyMenu(m);                                                   // also destroys the sub-menus
    if (cmd == ID_PRESET_BROWSE) {
        wchar_t buf[MAX_PATH * 2] = L"";
        std::wstring dir = PresetDir();
        OPENFILENAMEW o; memset(&o, 0, sizeof o);
        o.lStructSize = sizeof o; o.hwndOwner = g.dlg.hwnd;
        o.lpstrFilter = L"Rubato presets (*.rps;*.mfxp)\0*.rps;*.mfxp\0All files (*.*)\0*.*\0";
        o.lpstrFile = buf; o.nMaxFile = MAX_PATH * 2; o.lpstrInitialDir = dir.c_str();
        o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_HIDEREADONLY;
        if (!GetOpenFileNameW(&o)) return;
        std::vector<uint8_t> b; Chain items; std::wstring name;
        if (!ReadAll(buf, b) || !ParsePreset(std::string(b.begin(), b.end()), name, items)) {
            MessageBoxW(g.dlg.hwnd, L"This is not a valid Rubato preset file.", APP_TITLE, MB_ICONERROR); return;
        }
        ApplyPresetItems(items, name.empty() ? FileStem(buf) : name);
    } else if (cmd >= ID_PRESETMENU0 && cmd < ID_PRESETMENU0 + (int)g.presets.size()) {
        const PresetEntry e = g.presets[(size_t)(cmd - ID_PRESETMENU0)];
        ApplyPresetItems(e.items, e.label);
    }
}

static void DlgSavePreset() {
    Chain* c = DlgChain();
    if (!c || c->empty()) { SetStatus(L"This track's chain is empty - nothing to save."); return; }
    CreateDirectoryW(PresetDir().c_str(), NULL);
    wchar_t buf[MAX_PATH * 2] = L"My preset.rps";
    std::wstring dir = PresetDir();
    OPENFILENAMEW o; memset(&o, 0, sizeof o);
    o.lStructSize = sizeof o; o.hwndOwner = g.dlg.hwnd;
    o.lpstrFilter = L"Rubato presets (*.rps)\0*.rps\0";
    o.lpstrFile = buf; o.nMaxFile = MAX_PATH * 2; o.lpstrDefExt = L"rps"; o.lpstrInitialDir = dir.c_str();
    o.lpstrTitle = L"Save effect chain as preset (the file name becomes the preset name)";
    o.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_HIDEREADONLY;
    if (!GetSaveFileNameW(&o)) return;
    std::string txt = SerializePreset(FileStem(buf), *c);
    if (!WriteAll(buf, txt.data(), txt.size())) { MessageBoxW(g.dlg.hwnd, L"Cannot write the preset file.", APP_TITLE, MB_ICONERROR); return; }
    SetStatus(L"Preset saved: %ls", buf);
}

static void DlgHelp() {
    MessageBoxW(g.dlg.hwnd,
        L"MFX CHAIN - effects of one track, applied from top to bottom.\r\n\r\n"
        L"Folder button: load a preset (built-in, your own, or from a file).\r\n"
        L"Disk button: save this chain as a preset.\r\n\r\n"
        L"Columns\r\n"
        L"  N      order in the chain\r\n"
        L"  Mute   click to bypass (mute) this effect - an x means it is muted\r\n"
        L"  FX     effect name\r\n"
        L"  All    x = the effect works on everything on the track\r\n"
        L"  Exclude  x = work on everything EXCEPT the part from bar From to bar To\r\n"
        L"  Use    x = work ONLY on the part from bar From to bar To (the opposite of Exclude)\r\n"
        L"  From   first bar of the part (Exclude / Use)\r\n"
        L"  To     last bar of the part (Exclude / Use)\r\n"
        L"  (\"From selection\" fills From and To from the part you selected on the timeline ruler)\r\n\r\n"
        L"Insert: add an effect after the selected one.   Delete: remove it.   Up / Down: change the order.\r\n"
        L"Apply All FXs: bake this chain into the song data (Undo available).\r\n\r\n"
        L"Everything before that is a non-destructive preview: the coloured graphs, playback and Save As already use it.\r\n"
        L"\"New variation\" draws a different random result for effects that use random numbers.",
        L"MFX Chain - help", MB_OK | MB_ICONINFORMATION);
}

static void DlgCellClick(int item, int sub) {
    Chain* c = DlgChain();
    if (!c || item < 0 || item >= (int)c->size()) return;
    if (DlgSel() != item) {
        g.dlg.refreshing = true;
        ListView_SetItemState(g.dlg.list, item, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        g.dlg.refreshing = false;
        DlgShowParams();
    }
    ChainItem& it = (*c)[(size_t)item];
    if (sub == 1) it.enabled = !it.enabled;
    else if (sub == 3) it.mode = mc::RANGE_ALL;
    else if (sub == 4) it.mode = mc::RANGE_EXC;
    else if (sub == 5) it.mode = mc::RANGE_USE;
    else { DlgUpdateDesc(); return; }
    DlgSetRow(item, false);
    DlgShowParams();
    OnChainChanged(g.dlg.track);
    DlgUpdateDesc();
}

// Spinner (up-down control) attached to the right edge of an edit box.  The new value is computed in UDN_DELTAPOS
// (the control itself never changes a number), so fractions and the min / max limits of every parameter work.
static HWND MkSpin(HWND parent, int id, HWND buddy) {
    HWND u = CreateWindowExW(0, UPDOWN_CLASSW, L"", WS_CHILD | UDS_ARROWKEYS | UDS_ALIGNRIGHT | UDS_NOTHOUSANDS,
                             0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(u, UDM_SETRANGE32, -1000000, 1000000);
    SendMessageW(u, UDM_SETBUDDY, (WPARAM)buddy, 0);
    return u;
}
static HWND DMk(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parent,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g.font, TRUE);
    return c;
}

static void TbAdd(HWND tb, int bmp, int cmd, const wchar_t* text, BYTE style = BTNS_BUTTON) {
    TBBUTTON b; memset(&b, 0, sizeof b);
    b.iBitmap = bmp; b.idCommand = cmd; b.fsState = TBSTATE_ENABLED;
    b.fsStyle = (BYTE)(style | BTNS_AUTOSIZE | (text ? BTNS_SHOWTEXT : 0));
    b.iString = text ? (INT_PTR)text : 0;
    SendMessageW(tb, TB_ADDBUTTONSW, 1, (LPARAM)&b);
}
static void TbSep(HWND tb) {
    TBBUTTON b; memset(&b, 0, sizeof b);
    b.iBitmap = 8; b.fsStyle = BTNS_SEP; b.fsState = TBSTATE_ENABLED;
    SendMessageW(tb, TB_ADDBUTTONSW, 1, (LPARAM)&b);
}
static HWND MakeToolbar(HWND parent, int id) {
    HWND tb = CreateWindowExW(0, TOOLBARCLASSNAMEW, NULL,
        WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_LIST | TBSTYLE_TRANSPARENT | CCS_NODIVIDER | CCS_NOPARENTALIGN | CCS_NORESIZE,
        0, 0, 100, 30, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(tb, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    SendMessageW(tb, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_MIXEDBUTTONS);
    TBADDBITMAP ab; ab.hInst = HINST_COMMCTRL; ab.nID = IDB_STD_SMALL_COLOR;
    SendMessageW(tb, TB_ADDBITMAP, 0, (LPARAM)&ab);
    return tb;
}

static void DlgLayout(int W, int H) {
    DlgState& d = g.dlg;
    const int M = 6, rows = 5, rowH = 26;
    MoveWindow(d.tb, M, 4, 340, 28, TRUE);
    MoveWindow(d.close, W - 80, 5, 74, 26, TRUE);
    const int fixed = 38 + 8 + (rows * rowH + 6) + 34 + 8 + 44;          // toolbar, params, range row, buttons
    const int flex = std::max(180, H - fixed);
    const int listH = std::max(90, flex * 42 / 100), descH = std::max(90, flex - listH);
    int y = 38;
    MoveWindow(d.list, M, y, W - 2 * M, listH, TRUE); y += listH + 8;
    const int colW = (W - 2 * M) / 2;
    for (int k = 0; k < mc::FX_MAXP; k++) {                                // two columns of parameters
        int col = k / rows, row = k % rows;
        int x0 = M + col * colW, lw = colW * 56 / 100;
        MoveWindow(d.lbl[k], x0 + 4, y + row * rowH + 3, lw - 4, 20, TRUE);
        MoveWindow(d.ed[k], x0 + lw, y + row * rowH, std::max(40, colW - lw - 12), 22, TRUE);
        SendMessageW(d.spin[k], UDM_SETBUDDY, (WPARAM)d.ed[k], 0);              // the spinner snaps to the edit box again
    }
    y += rows * rowH + 6;
    int x = M + 4;                                                          // range row
    MoveWindow(d.rbAll, x, y + 2, 46, 22, TRUE); x += 50;
    MoveWindow(d.rbExc, x, y + 2, 72, 22, TRUE); x += 76;
    MoveWindow(d.rbUse, x, y + 2, 46, 22, TRUE); x += 54;
    MoveWindow(d.lblFrom, x, y + 6, 52, 18, TRUE); x += 54;
    MoveWindow(d.edFrom, x, y + 2, 56, 22, TRUE); SendMessageW(d.spinFrom, UDM_SETBUDDY, (WPARAM)d.edFrom, 0); x += 64;
    MoveWindow(d.lblTo, x, y + 6, 36, 18, TRUE); x += 38;
    MoveWindow(d.edTo, x, y + 2, 56, 22, TRUE); SendMessageW(d.spinTo, UDM_SETBUDDY, (WPARAM)d.edTo, 0); x += 66;
    MoveWindow(d.fromSel, x, y, 112, 26, TRUE);
    y += 34;
    MoveWindow(d.desc, M, y, W - 2 * M, descH, TRUE);
    const int by = H - 38;
    MoveWindow(d.insert, M, by, 70, 28, TRUE);
    MoveWindow(d.del, M + 74, by, 70, 28, TRUE);
    MoveWindow(d.applyAll, M + 148, by, 118, 28, TRUE);
    MoveWindow(d.reroll, M + 270, by, 112, 28, TRUE);
    MoveWindow(d.up, W - 126, by, 58, 28, TRUE);
    MoveWindow(d.down, W - 64, by, 58, 28, TRUE);
}

static LRESULT CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    DlgState& d = g.dlg;
    switch (msg) {
    case WM_CREATE: {
        d.hwnd = hwnd;
        // toolbar with the standard "open" and "save" icons, like the original design
        d.tb = MakeToolbar(hwnd, IDC_TB);
        TbAdd(d.tb, STD_FILEOPEN, IDT_PRESETS, L"Presets");
        TbAdd(d.tb, STD_FILESAVE, IDT_SAVE, L"Save preset");
        TbSep(d.tb);
        TbAdd(d.tb, STD_HELP, IDT_HELP, L"Help");
        d.close = DMk(hwnd, L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, IDC_CLOSE);
        d.list = DMk(hwnd, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, IDC_LIST, WS_EX_CLIENTEDGE);
        ListView_SetExtendedListViewStyle(d.list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
        { const wchar_t* cn[8] = { L"N", L"Mute", L"FX", L"All", L"Exclude", L"Use", L"From", L"To" };
          int cw[8] = { 28, 46, 200, 38, 60, 38, 46, 42 };
          for (int i = 0; i < 8; i++) {
              LVCOLUMNW c; memset(&c, 0, sizeof c);
              c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT; c.fmt = (i == 2) ? LVCFMT_LEFT : LVCFMT_CENTER;
              c.pszText = (LPWSTR)cn[i]; c.cx = cw[i];
              SendMessageW(d.list, LVM_INSERTCOLUMNW, (WPARAM)i, (LPARAM)&c);
          } }
        for (int k = 0; k < mc::FX_MAXP; k++) {
            d.lbl[k] = DMk(hwnd, L"STATIC", L"", SS_LEFT, 0);
            d.ed[k]  = DMk(hwnd, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_PARAM0 + k, WS_EX_CLIENTEDGE);
        }
        d.rbAll   = DMk(hwnd, L"BUTTON", L"All", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, IDC_ALL);
        d.rbExc   = DMk(hwnd, L"BUTTON", L"Exclude", BS_AUTORADIOBUTTON, IDC_EXC);
        d.rbUse   = DMk(hwnd, L"BUTTON", L"Use", BS_AUTORADIOBUTTON, IDC_USEM);
        d.lblFrom = DMk(hwnd, L"STATIC", L"From bar:", SS_LEFT, 0);
        d.edFrom  = DMk(hwnd, L"EDIT", L"1", ES_AUTOHSCROLL | ES_NUMBER | WS_TABSTOP, IDC_FROM, WS_EX_CLIENTEDGE);
        d.lblTo   = DMk(hwnd, L"STATIC", L"To bar:", SS_LEFT, 0);
        d.edTo    = DMk(hwnd, L"EDIT", L"4", ES_AUTOHSCROLL | ES_NUMBER | WS_TABSTOP, IDC_TO, WS_EX_CLIENTEDGE);
        for (int k = 0; k < mc::FX_MAXP; k++) d.spin[k] = MkSpin(hwnd, IDC_SPIN0 + k, d.ed[k]);
        d.spinFrom = MkSpin(hwnd, IDC_SPINFROM, d.edFrom);
        d.spinTo   = MkSpin(hwnd, IDC_SPINTO, d.edTo);
        d.fromSel = DMk(hwnd, L"BUTTON", L"From selection", BS_PUSHBUTTON | WS_TABSTOP, IDC_FROMSEL);
        d.reroll  = DMk(hwnd, L"BUTTON", L"New variation", BS_PUSHBUTTON | WS_TABSTOP, IDC_REROLL);
        d.desc    = DMk(hwnd, g_rich ? RICH_CLASS : L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, IDC_DESC, WS_EX_CLIENTEDGE);
        if (g_rich) SendMessageW(d.desc, EM_SETBKGNDCOLOR, 0, (LPARAM)RGB(255, 255, 255));
        d.insert  = DMk(hwnd, L"BUTTON", L"Insert", BS_PUSHBUTTON | WS_TABSTOP, IDC_INSERT);
        d.del     = DMk(hwnd, L"BUTTON", L"Delete", BS_PUSHBUTTON | WS_TABSTOP, IDC_DELETE);
        d.applyAll = DMk(hwnd, L"BUTTON", L"Apply All FXs", BS_PUSHBUTTON | WS_TABSTOP, IDC_APPLYALL);
        d.up      = DMk(hwnd, L"BUTTON", L"Up", BS_PUSHBUTTON | WS_TABSTOP, IDC_UP);
        d.down    = DMk(hwnd, L"BUTTON", L"Down", BS_PUSHBUTTON | WS_TABSTOP, IDC_DOWN);
        RECT rc; GetClientRect(hwnd, &rc);
        DlgLayout(rc.right, rc.bottom);
        return 0;
    }
    case WM_SIZE:
        if (d.reroll) DlgLayout((int)LOWORD(lp), (int)HIWORD(lp));
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)lp; mm->ptMinTrackSize.x = 600; mm->ptMinTrackSize.y = 600; return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id >= IDC_PARAM0 && id < IDC_PARAM0 + mc::FX_MAXP) {
            if (code == EN_SETFOCUS) { g.dlg.focusParam = id - IDC_PARAM0; DlgUpdateDesc(); }
            else if (code == EN_CHANGE) DlgParamEdited();
            else if (code == EN_KILLFOCUS) {                                 // show the value that is really used (within min / max)
                ChainItem* it = DlgItem();
                if (it) { g.dlg.refreshing = true; SetEditNumber(g.dlg.ed[id - IDC_PARAM0], it->p[id - IDC_PARAM0]); g.dlg.refreshing = false; }
            }
            return 0;
        }
        switch (id) {
        case IDC_CLOSE: ShowWindow(hwnd, SW_HIDE); break;
        case IDT_PRESETS: DlgPresets(); break;
        case IDT_SAVE: DlgSavePreset(); break;
        case IDT_HELP: DlgHelp(); break;
        case IDC_INSERT: DlgInsert(); break;
        case IDC_DELETE: DlgDelete(); break;
        case IDC_UP: DlgMove(-1); break;
        case IDC_DOWN: DlgMove(+1); break;
        case IDC_APPLYALL: DoCommit(g.dlg.track); break;
        case IDC_REROLL: DlgReroll(); break;
        case IDC_FROMSEL: DlgFromSelection(); break;
        case IDC_ALL: case IDC_EXC: case IDC_USEM: if (code == BN_CLICKED) DlgRangeEdited(); break;
        case IDC_FROM: case IDC_TO:
            if (code == EN_CHANGE) DlgRangeEdited();
            else if (code == EN_KILLFOCUS) {
                ChainItem* it = DlgItem();
                if (it) { g.dlg.refreshing = true; SetEditNumber(id == IDC_FROM ? g.dlg.edFrom : g.dlg.edTo, id == IDC_FROM ? it->fromBar : it->toBar); g.dlg.refreshing = false; }
            }
            break;
        }
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR* h = (NMHDR*)lp;
        if (h->code == UDN_DELTAPOS) {                                       // spinner arrows (and the Up / Down keys inside a box)
            NMUPDOWN* ud = (NMUPDOWN*)lp;
            ChainItem* it = DlgItem();
            if (it && h->idFrom >= IDC_SPIN0 && h->idFrom < IDC_SPIN0 + mc::FX_MAXP) {
                const int k = (int)h->idFrom - IDC_SPIN0;
                if (k < mc::GetFxDesc(it->type).np) {
                    const mc::FxLimit lim = mc::GetFxLimit(it->type, k);
                    double cur; if (!EditNumber(d.ed[k], cur)) cur = it->p[k];
                    cur = mc::ClampParam(it->type, k, cur);
                    double nv = cur + (double)ud->iDelta * lim.step * ((GetKeyState(VK_SHIFT) & 0x8000) ? 10.0 : 1.0);
                    nv = std::floor(nv / lim.step + 0.5) * lim.step;              // stay on the step grid (0.5, 5 ...)
                    SetEditNumber(d.ed[k], mc::ClampParam(it->type, k, nv));      // EN_CHANGE applies it
                }
            } else if (it && (h->idFrom == IDC_SPINFROM || h->idFrom == IDC_SPINTO)) {
                HWND ed = h->idFrom == IDC_SPINFROM ? d.edFrom : d.edTo;
                double cur; if (!EditNumber(ed, cur)) cur = 1;
                SetEditNumber(ed, std::max(1.0, std::min(9999.0, std::floor(cur) + ud->iDelta)));
            }
            return 1;                                                         // the control must not change anything itself
        }
        if (h->idFrom == IDC_LIST) {
            if (h->code == LVN_ITEMCHANGED && !d.refreshing) {
                NMLISTVIEW* nm = (NMLISTVIEW*)lp;
                if ((nm->uChanged & LVIF_STATE) && ((nm->uNewState ^ nm->uOldState) & LVIS_SELECTED)) { DlgShowParams(); DlgUpdateDesc(); }
            } else if (h->code == NM_CLICK) {
                NMITEMACTIVATE* ia = (NMITEMACTIVATE*)lp;
                LVHITTESTINFO ht; memset(&ht, 0, sizeof ht);
                ht.pt = ia->ptAction;
                ListView_SubItemHitTest(d.list, &ht);
                if (ht.iItem >= 0) DlgCellClick(ht.iItem, ht.iSubItem);
            }
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == d.desc) { SetBkColor((HDC)wp, RGB(255, 255, 255)); SetTextColor((HDC)wp, RGB(0, 0, 0)); return (LRESULT)GetStockObject(WHITE_BRUSH); }
        break;
    case WM_CLOSE: ShowWindow(hwnd, SW_HIDE); return 0;
    case WM_DESTROY: d.hwnd = NULL; return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void OpenChainDialog() {
    if (!g.loaded) { SetStatus(L"Open a MIDI file first."); return; }
    if (V.sel < 0) { SetStatus(L"Select a track first (click it in the list)."); return; }
    if (!g.dlg.hwnd) {
        const int cw = 620, ch = 700;
        DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MAXIMIZEBOX;
        RECT r = { 0, 0, cw, ch };
        AdjustWindowRectEx(&r, style, FALSE, 0);
        RECT mr; GetWindowRect(g.hwnd, &mr);
        int x = std::max(0, (int)mr.right - (int)(r.right - r.left) - 30), y = (int)mr.top + 140;
        CreateWindowExW(0, DLG_CLASS, L"MFX Chain", style, x, y, r.right - r.left, r.bottom - r.top,
                        g.hwnd, NULL, GetModuleHandleW(NULL), NULL);
    }
    ShowWindow(g.dlg.hwnd, SW_SHOW);
    SetForegroundWindow(g.dlg.hwnd);
    SyncDialog();
}

// ===========================================================================
//  Main window
// ===========================================================================
static HWND Mk(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parent,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(h, WM_SETFONT, (WPARAM)g.font, TRUE);
    return h;
}

static void Layout() {
    RECT rc; GetClientRect(g.hwnd, &rc);
    const int W = rc.right, H = rc.bottom;
    const int TBH = 30, ROWB = 30, BOT = 30, STH = 22;

    MoveWindow(g.hTb, 0, 0, W, TBH, TRUE);
    int y = TBH + 2;                                           // transport row
    MoveWindow(g.hBtnHome, 6, y, 32, 26, TRUE);
    MoveWindow(g.hBtnPlay, 42, y, 40, 26, TRUE);
    MoveWindow(g.hBtnStop, 86, y, 40, 26, TRUE);
    MoveWindow(g.hLoop, 134, y, 56, 26, TRUE);
    MoveWindow(g.hLblDev, 214, y + 5, 60, 18, TRUE);
    MoveWindow(g.hDev, 276, y + 1, 300, 240, TRUE);

    const int arrTop = TBH + ROWB + 2;
    MoveWindow(g.hArr, 0, arrTop, W, std::max(10, H - arrTop - BOT - STH), TRUE);

    int by = H - STH - BOT + 2;                                // bottom toolbar
    for (int i = 0; i < LANE_COUNT; i++) MoveWindow(g.hLane[i], 6 + i * 66, by, 64, 26, TRUE);
    int x = 6 + LANE_COUNT * 66 + 8;
    MoveWindow(g.hGhost, x, by, 78, 26, TRUE); x += 92;
    MoveWindow(g.hLblTrk, x, by + 5, 80, 18, TRUE); x += 80;
    MoveWindow(g.hTrkMinus, x, by, 28, 26, TRUE); x += 30;
    MoveWindow(g.hTrkPlus, x, by, 28, 26, TRUE); x += 44;
    MoveWindow(g.hLblZoom, x, by + 5, 42, 18, TRUE); x += 44;
    MoveWindow(g.hZoomMinus, x, by, 28, 26, TRUE); x += 30;
    MoveWindow(g.hZoomPlus, x, by, 28, 26, TRUE); x += 44;
    MoveWindow(g.hLblBend, x, by + 5, 92, 18, TRUE); x += 94;
    MoveWindow(g.hBend, x, by + 2, 56, 22, TRUE);
    SendMessageW(g.hBendSpin, UDM_SETBUDDY, (WPARAM)g.hBend, 0);

    MoveWindow(g.hStatus, 0, H - STH, std::max(10, W - 126), STH, TRUE);
    MoveWindow(g.hProc, W - 124, H - STH, 124, STH, TRUE);
}

static void DrawTransport(DRAWITEMSTRUCT* d) {
    UINT st = DFCS_BUTTONPUSH | ((d->itemState & ODS_SELECTED) ? DFCS_PUSHED : 0);
    DrawFrameControl(d->hDC, &d->rcItem, DFC_BUTTON, st);
    int cx = (d->rcItem.left + d->rcItem.right) / 2, cy = (d->rcItem.top + d->rcItem.bottom) / 2;
    if (d->itemState & ODS_SELECTED) { cx++; cy++; }
    HGDIOBJ oldPen = SelectObject(d->hDC, GetStockObject(NULL_PEN));
    COLORREF col = (d->CtlID == ID_PLAY) ? RGB(20, 150, 60) : RGB(40, 40, 50);
    HBRUSH br = CreateSolidBrush(col);
    HGDIOBJ oldBr = SelectObject(d->hDC, br);
    if (d->CtlID == ID_PLAY) {
        POINT p[3] = { { cx - 5, cy - 7 }, { cx - 5, cy + 8 }, { cx + 8, cy } };
        Polygon(d->hDC, p, 3);
    } else if (d->CtlID == ID_STOP) {
        Rectangle(d->hDC, cx - 6, cy - 6, cx + 7, cy + 7);
    } else if (d->CtlID == ID_HOME) {
        Rectangle(d->hDC, cx - 8, cy - 7, cx - 5, cy + 8);
        POINT p[3] = { { cx + 8, cy - 7 }, { cx + 8, cy + 8 }, { cx - 4, cy } };
        Polygon(d->hDC, p, 3);
    }
    SelectObject(d->hDC, oldBr); SelectObject(d->hDC, oldPen); DeleteObject(br);
}

static void UpdateSelStatus() {
    int b1, t1, k1, b2, t2, k2;
    if (V.selEnd > V.selStart) {
        BarBeatTick(V.selStart, b1, t1, k1); BarBeatTick(V.selEnd, b2, t2, k2);
        double sec = (mc::TickToMs(g.tempo, V.ppq, V.selEnd) - mc::TickToMs(g.tempo, V.ppq, V.selStart)) / 1000.0;
        SetStatus(L"Selection: bar %d beat %d  to  bar %d beat %d  (%.1f s).  Play plays only this part - switch Loop on to repeat it.  Click the ruler to clear.", b1, t1, b2, t2, sec);
    } else {
        BarBeatTick(V.startTick, b1, t1, k1);
        SetStatus(L"Start marker at bar %d beat %d.  Drag on the ruler to select a part of the song to play, loop or process.", b1, t1);
    }
}

static const wchar_t* ABOUT_CLASS = L"RubatoAbout";

static HFONT s_aboutItalic = NULL;
static HWND  s_aboutTagline = NULL;

static LRESULT CALLBACK AboutProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h) {
            HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, hwnd,
                                     (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.font, TRUE);
            return c;
        };
        HWND ic = mk(L"STATIC", L"", SS_ICON, 0, 20, 14, 48, 48);                              // the Rubato icon
        HICON hi = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1), IMAGE_ICON, 48, 48, LR_DEFAULTCOLOR | LR_SHARED);
        if (hi) SendMessageW(ic, STM_SETIMAGE, IMAGE_ICON, (LPARAM)hi);
        HWND t = mk(L"STATIC", L"Rubato", SS_LEFT, 0, 84, 12, 400, 36);
        SendMessageW(t, WM_SETFONT, (WPARAM)g.fontBig, TRUE);
        mk(L"STATIC", L"Version " APP_VERSION, SS_LEFT, 0, 86, 48, 400, 18);

        LOGFONTW lf; memset(&lf, 0, sizeof lf);                                                // tagline: italic, purple
        GetObjectW(g.font, sizeof lf, &lf);
        lf.lfItalic = TRUE; lf.lfHeight = lf.lfHeight * 118 / 100;
        s_aboutItalic = CreateFontIndirectW(&lf);
        s_aboutTagline = mk(L"STATIC", L"Because music happens between the lines", SS_LEFT | SS_NOPREFIX, 0, 86, 70, 410, 22);
        SendMessageW(s_aboutTagline, WM_SETFONT, (WPARAM)(s_aboutItalic ? s_aboutItalic : g.font), TRUE);

        mk(L"STATIC", L"Designed and Engineered : O.Jagannatha Rao (JagiChan)", SS_LEFT, 0, 20, 108, 480, 18);
        mk(L"STATIC", L"Coded using Claude AI : Model Sonnet 5.5", SS_LEFT, 0, 20, 130, 480, 18);
        mk(WC_LINK, L"Copyright \u00A9 2026 O.Jagannatha Rao. This program is free software: you can redistribute it and/or modify it under the "
                    L"terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, "
                    L"or (at your option) any later version.\n\n"
                    L"This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied "
                    L"warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details. "
                    L"You should have received a copy of the GNU General Public License along with this program. If not, see "
                    L"<a href=\"https://www.gnu.org/licenses/\">https://www.gnu.org/licenses/</a>.",
           WS_TABSTOP, ID_ABOUT_LINK, 20, 160, 480, 140);
        mk(WC_LINK, L"Source code: The whole thing is on GitHub: <a href=\"https://github.com/Jagichan/Rubato/\">https://github.com/Jagichan/Rubato/</a>",
           WS_TABSTOP, ID_ABOUT_LINK + 1, 20, 306, 480, 36);
        mk(WC_LINK, L"If you like my work, you can buy me a coffee to keep me awake and motivated: <a href=\"https://buymeacoffee.com/jagichan\">https://buymeacoffee.com/jagichan</a>",
           WS_TABSTOP, ID_ABOUT_LINK + 2, 20, 346, 480, 36);
        mk(L"BUTTON", L"License", BS_PUSHBUTTON | WS_TABSTOP, ID_ABOUT_LICENSE, 150, 394, 100, 28);
        mk(L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, ID_ABOUT_OK, 270, 394, 100, 28);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == s_aboutTagline) {                                                      // purple tagline
            SetTextColor((HDC)wp, RGB(128, 0, 160));
            SetBkMode((HDC)wp, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_ABOUT_LICENSE) {                      // full GPL v3 text, from gnu.org
            ShellExecuteW(hwnd, L"open", L"https://www.gnu.org/licenses/gpl-3.0.txt", NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }
        if (LOWORD(wp) == ID_ABOUT_OK || LOWORD(wp) == IDCANCEL) DestroyWindow(hwnd);
        return 0;
    case WM_NOTIFY: {
        NMHDR* h = (NMHDR*)lp;
        if (h->code == NM_CLICK || h->code == NM_RETURN) {
            NMLINK* pn = (NMLINK*)lp;
            ShellExecuteW(hwnd, L"open", pn->item.szUrl, NULL, NULL, SW_SHOWNORMAL);
        }
        return 0;
    }
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        s_aboutTagline = NULL;
        if (s_aboutItalic) { DeleteObject(s_aboutItalic); s_aboutItalic = NULL; }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ShowAbout() {
    const int cw = 520, ch = 436;
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    RECT r = { 0, 0, cw, ch };
    AdjustWindowRectEx(&r, style, FALSE, WS_EX_DLGMODALFRAME);
    RECT mr; GetWindowRect(g.hwnd, &mr);
    const int w = (int)(r.right - r.left), h = (int)(r.bottom - r.top);
    const int x = (int)mr.left + ((int)(mr.right - mr.left) - w) / 2, y = (int)mr.top + ((int)(mr.bottom - mr.top) - h) / 2;
    EnableWindow(g.hwnd, FALSE);                                   // modal
    HWND a = CreateWindowExW(WS_EX_DLGMODALFRAME, ABOUT_CLASS, L"About Rubato", style, x, y, w, h,
                             g.hwnd, NULL, GetModuleHandleW(NULL), NULL);
    if (a) {
        ShowWindow(a, SW_SHOW); UpdateWindow(a);
        MSG m;
        while (IsWindow(a) && GetMessage(&m, NULL, 0, 0) > 0) {
            if (!IsDialogMessage(a, &m)) { TranslateMessage(&m); DispatchMessage(&m); }
        }
    }
    EnableWindow(g.hwnd, TRUE);
    SetForegroundWindow(g.hwnd);
}

static HMENU BuildMenu() {
    HMENU mb = CreateMenu();
    HMENU f = CreatePopupMenu();
    AppendMenuW(f, MF_STRING, ID_OPEN, L"&Open MIDI file or project...\tCtrl+O");
    AppendMenuW(f, MF_STRING, ID_OPENPROJ, L"Open &project (*.rpj)...");
    s_recentMenu = CreatePopupMenu();
    AppendMenuW(f, MF_POPUP, (UINT_PTR)s_recentMenu, L"Open &recent");
    RebuildRecentMenu();
    AppendMenuW(f, MF_SEPARATOR, 0, NULL);
    AppendMenuW(f, MF_STRING, ID_SAVE, L"&Save project\tCtrl+S");
    AppendMenuW(f, MF_STRING, ID_SAVEAS, L"Save project &as...");
    AppendMenuW(f, MF_SEPARATOR, 0, NULL);
    AppendMenuW(f, MF_STRING, ID_EXPORT, L"&Export MIDI file (format 0)...\tCtrl+Shift+S");
    AppendMenuW(f, MF_SEPARATOR, 0, NULL);
    AppendMenuW(f, MF_STRING, ID_EXIT, L"E&xit");
    AppendMenuW(mb, MF_POPUP, (UINT_PTR)f, L"&File");

    HMENU e = CreatePopupMenu();
    AppendMenuW(e, MF_STRING, ID_UNDO, L"&Undo\tCtrl+Z");
    AppendMenuW(e, MF_SEPARATOR, 0, NULL);
    AppendMenuW(e, MF_STRING, ID_MK_INSERT, L"Insert &marker at the start position...");
    AppendMenuW(mb, MF_POPUP, (UINT_PTR)e, L"&Edit");

    HMENU p = CreatePopupMenu();
    AppendMenuW(p, MF_STRING, ID_CHAIN, L"MFX &chain of selected track...\tCtrl+E");
    AppendMenuW(p, MF_STRING, ID_COPYALL, L"Copy chain of selected track to &all tracks");
    AppendMenuW(p, MF_SEPARATOR, 0, NULL);
    AppendMenuW(p, MF_STRING, ID_BYPASS, L"&Bypass all FX (A/B)");
    AppendMenuW(p, MF_STRING, ID_COMMITALL, L"Apply &all FXs to the song");
    AppendMenuW(mb, MF_POPUP, (UINT_PTR)p, L"&MIDI Process");

    HMENU v = CreatePopupMenu();
    for (int i = 0; i < LANE_COUNT; i++) {
        std::wstring t = std::wstring(L"Show ") + LM[i].label + L" graph";
        AppendMenuW(v, MF_STRING | (V.laneOn[i] ? MF_CHECKED : 0), (UINT_PTR)(ID_LANE0 + i), t.c_str());
    }
    AppendMenuW(v, MF_STRING | MF_CHECKED, ID_GHOST, L"Show &original (ghost) while editing");
    AppendMenuW(v, MF_SEPARATOR, 0, NULL);
    AppendMenuW(v, MF_STRING, ID_TRK_BIGGER, L"Taller tracks");
    AppendMenuW(v, MF_STRING, ID_TRK_SMALLER, L"Shorter tracks");
    AppendMenuW(v, MF_STRING, ID_ZOOMIN, L"Zoom in");
    AppendMenuW(v, MF_STRING, ID_ZOOMOUT, L"Zoom out");
    AppendMenuW(mb, MF_POPUP, (UINT_PTR)v, L"&View");

    HMENU pb = CreatePopupMenu();
    AppendMenuW(pb, MF_STRING, ID_PLAY, L"&Play from start marker");
    AppendMenuW(pb, MF_STRING, ID_STOP, L"&Stop");
    AppendMenuW(pb, MF_STRING, ID_HOME, L"Go to &beginning");
    AppendMenuW(pb, MF_SEPARATOR, 0, NULL);
    AppendMenuW(pb, MF_STRING, ID_SELCLEAR, L"&Clear timeline selection");
    AppendMenuW(mb, MF_POPUP, (UINT_PTR)pb, L"&Playback");

    HMENU h = CreatePopupMenu();
    AppendMenuW(h, MF_STRING, ID_HELP, L"&Quick guide");
    AppendMenuW(h, MF_STRING, ID_ABOUT, L"&About");
    AppendMenuW(mb, MF_POPUP, (UINT_PTR)h, L"&Help");
    return mb;
}

static void OnCreate(HWND hwnd) {
    g.hwnd = hwnd;
    g.font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    g.fontBig = CreateFontW(-26, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    g.brRed = CreateSolidBrush(RGB(214, 64, 52));
    LoadRecent();
    g.hMenu = BuildMenu();
    SetMenu(hwnd, g.hMenu);

    // main toolbar (standard icons)
    g.hTb = MakeToolbar(hwnd, ID_TOOLBAR);
    TbAdd(g.hTb, STD_FILEOPEN, ID_OPEN, L"Open");
    TbAdd(g.hTb, STD_FILESAVE, ID_SAVE, L"Save project");
    TbAdd(g.hTb, I_IMAGENONE, ID_EXPORT, L"Export MIDI");
    TbSep(g.hTb);
    TbAdd(g.hTb, STD_UNDO, ID_UNDO, L"Undo");
    TbSep(g.hTb);
    TbAdd(g.hTb, I_IMAGENONE, ID_CHAIN, L"MFX Chain...");
    TbAdd(g.hTb, I_IMAGENONE, ID_BYPASS, L"Bypass FX", BTNS_CHECK);
    TbAdd(g.hTb, I_IMAGENONE, ID_COMMITALL, L"Apply all FXs");
    TbSep(g.hTb);
    TbAdd(g.hTb, STD_HELP, ID_HELP, L"Help");

    // transport row
    g.hBtnHome = Mk(hwnd, L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP, ID_HOME);
    g.hBtnPlay = Mk(hwnd, L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP, ID_PLAY);
    g.hBtnStop = Mk(hwnd, L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP, ID_STOP);
    g.hLoop    = Mk(hwnd, L"BUTTON", L"Loop", BS_AUTOCHECKBOX | BS_PUSHLIKE | WS_TABSTOP, ID_LOOP);
    g.hLblDev  = Mk(hwnd, L"STATIC", L"MIDI out:", SS_LEFT, 0);
    g.hDev     = Mk(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_DEV);

    g.hArr = CreateWindowExW(WS_EX_CLIENTEDGE, ARR_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_HSCROLL | WS_VSCROLL | WS_CLIPSIBLINGS,
                             0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_ARRANGE, GetModuleHandleW(NULL), NULL);

    // bottom toolbar: graph toggles etc.
    for (int i = 0; i < LANE_COUNT; i++) {
        g.hLane[i] = Mk(hwnd, L"BUTTON", LM[i].button, BS_AUTOCHECKBOX | BS_PUSHLIKE | WS_TABSTOP, ID_LANE0 + i);
        SendMessageW(g.hLane[i], BM_SETCHECK, V.laneOn[i] ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    g.hGhost    = Mk(hwnd, L"BUTTON", L"Original", BS_AUTOCHECKBOX | BS_PUSHLIKE | WS_TABSTOP, ID_GHOST);
    SendMessageW(g.hGhost, BM_SETCHECK, BST_CHECKED, 0);
    CheckMenuItem(g.hMenu, ID_GHOST, MF_BYCOMMAND | MF_CHECKED);
    g.hLblTrk   = Mk(hwnd, L"STATIC", L"Track height:", SS_LEFT, 0);
    g.hTrkMinus = Mk(hwnd, L"BUTTON", L"-", BS_PUSHBUTTON | WS_TABSTOP, ID_TRK_SMALLER);
    g.hTrkPlus  = Mk(hwnd, L"BUTTON", L"+", BS_PUSHBUTTON | WS_TABSTOP, ID_TRK_BIGGER);
    g.hLblZoom  = Mk(hwnd, L"STATIC", L"Zoom:", SS_LEFT, 0);
    g.hZoomMinus = Mk(hwnd, L"BUTTON", L"-", BS_PUSHBUTTON | WS_TABSTOP, ID_ZOOMOUT);
    g.hZoomPlus  = Mk(hwnd, L"BUTTON", L"+", BS_PUSHBUTTON | WS_TABSTOP, ID_ZOOMIN);
    g.hLblBend  = Mk(hwnd, L"STATIC", L"Bend range (st):", SS_LEFT, 0);
    g.hBend     = Mk(hwnd, L"EDIT", L"2", ES_AUTOHSCROLL | WS_TABSTOP, ID_BEND, WS_EX_CLIENTEDGE);
    g.hBendSpin = MkSpin(hwnd, ID_BENDSPIN, g.hBend); ShowWindow(g.hBendSpin, SW_SHOW);

    g.hStatus = Mk(hwnd, L"STATIC", L"Open a MIDI file (or drag it onto this window).", SS_LEFT | SS_SUNKEN | SS_CENTERIMAGE, 0);
    g.hProc   = Mk(hwnd, L"STATIC", L"Original", SS_CENTER | SS_SUNKEN | SS_CENTERIMAGE, 0);

    // MIDI output devices
    int idx = (int)SendMessageW(g.hDev, CB_ADDSTRING, 0, (LPARAM)L"Windows default (MIDI Mapper)");
    SendMessageW(g.hDev, CB_SETITEMDATA, (WPARAM)idx, (LPARAM)(INT_PTR)(INT)-1);   // MIDI_MAPPER
    UINT n = midiOutGetNumDevs();
    for (UINT i = 0; i < n; i++) {
        MIDIOUTCAPSW caps;
        if (midiOutGetDevCapsW(i, &caps, sizeof caps) == MMSYSERR_NOERROR) {
            idx = (int)SendMessageW(g.hDev, CB_ADDSTRING, 0, (LPARAM)caps.szPname);
            SendMessageW(g.hDev, CB_SETITEMDATA, (WPARAM)idx, (LPARAM)i);
        }
    }
    SendMessageW(g.hDev, CB_SETCURSEL, 0, 0);
    ScanPresets();
    Layout();
    UpdateTitle();
}

// ===========================================================================
//  Quick guide window: resizable, with scroll bars (vertical, and horizontal when "Wrap lines" is off)
// ===========================================================================
static const wchar_t* HELP_CLASS = L"RubatoHelp";
enum { IDC_HELPWRAP = 9001 };
static HWND s_help = NULL, s_helpEdit = NULL, s_helpWrap = NULL;
static bool s_helpWrapOn = true;

static void HelpLayout() {
    if (!s_help) return;
    RECT rc; GetClientRect(s_help, &rc);
    const int bh = 28;
    if (s_helpEdit) MoveWindow(s_helpEdit, 0, 0, rc.right, std::max(0, (int)rc.bottom - bh), TRUE);
    if (s_helpWrap) MoveWindow(s_helpWrap, 10, std::max(0, (int)rc.bottom - bh + 3), 160, 22, TRUE);
}

// (Re)creates the text box: with wrapping the text always fits the width; without it a horizontal scroll bar appears.
static void HelpMakeEdit() {
    if (!s_help) return;
    if (s_helpEdit) { DestroyWindow(s_helpEdit); s_helpEdit = NULL; }
    DWORD st = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL;
    if (!s_helpWrapOn) st |= WS_HSCROLL | ES_AUTOHSCROLL;
    s_helpEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", st, 0, 0, 100, 100, s_help, (HMENU)(INT_PTR)1, GetModuleHandleW(NULL), NULL);
    SendMessageW(s_helpEdit, WM_SETFONT, (WPARAM)g.font, TRUE);
    SetWindowTextW(s_helpEdit, HELP_TEXT);
    SendMessageW(s_helpEdit, EM_SETSEL, 0, 0);
    HelpLayout();
}

static LRESULT CALLBACK HelpProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        s_help = hwnd;
        s_helpWrap = CreateWindowExW(0, L"BUTTON", L"Wrap lines", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                     0, 0, 100, 22, hwnd, (HMENU)(INT_PTR)IDC_HELPWRAP, GetModuleHandleW(NULL), NULL);
        SendMessageW(s_helpWrap, WM_SETFONT, (WPARAM)g.font, TRUE);
        SendMessageW(s_helpWrap, BM_SETCHECK, s_helpWrapOn ? BST_CHECKED : BST_UNCHECKED, 0);
        HelpMakeEdit();
        return 0;
    case WM_SIZE: HelpLayout(); return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)lp; mm->ptMinTrackSize.x = 320; mm->ptMinTrackSize.y = 200; return 0;
    }
    case WM_SETFOCUS: if (s_helpEdit) SetFocus(s_helpEdit); return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_HELPWRAP && HIWORD(wp) == BN_CLICKED) {
            s_helpWrapOn = SendMessageW(s_helpWrap, BM_GETCHECK, 0, 0) == BST_CHECKED;
            HelpMakeEdit();
        }
        return 0;
    case WM_KEYDOWN: if (wp == VK_ESCAPE) { DestroyWindow(hwnd); return 0; } break;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: s_help = NULL; s_helpEdit = NULL; s_helpWrap = NULL; return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ShowHelpWindow(HWND owner) {
    if (s_help) { ShowWindow(s_help, SW_RESTORE); SetForegroundWindow(s_help); return; }
    RECT r; GetWindowRect(owner, &r);
    const int w = 640, h = 540;
    int x = r.left + ((r.right - r.left) - w) / 2, y = r.top + ((r.bottom - r.top) - h) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    HWND hw = CreateWindowExW(0, HELP_CLASS, L"Quick guide", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              x, y, w, h, owner, NULL, GetModuleHandleW(NULL), NULL);
    if (!hw) return;
    ShowWindow(hw, SW_SHOW);
    UpdateWindow(hw);
}

// ===========================================================================
//  Splash screen (splash_screen.jpg, linked in as RCDATA resource 2 - see Rubato.rc)
// ===========================================================================
static const wchar_t* SPLASH_CLASS = L"RubatoSplash";
static HBITMAP s_splashBmp = NULL;
static bool s_splashDismissed = false;

// Decodes the JPEG resource and draws it, scaled to about 70% of the screen width (at most 900 px), into a bitmap.
static HBITMAP LoadSplashBitmap(HINSTANCE hInst, int& outW, int& outH) {
    HRSRC res = FindResourceW(hInst, MAKEINTRESOURCEW(2), RT_RCDATA);
    if (!res) return NULL;
    HGLOBAL hg = LoadResource(hInst, res);
    const DWORD size = SizeofResource(hInst, res);
    const void* data = hg ? LockResource(hg) : NULL;
    if (!data || !size) return NULL;
    IStream* stm = SHCreateMemStream((const BYTE*)data, size);
    if (!stm) return NULL;
    IPicture* pic = NULL;
    HRESULT hr = OleLoadPicture(stm, 0, FALSE, IID_IPicture, (void**)&pic);
    stm->Release();
    if (FAILED(hr) || !pic) return NULL;
    OLE_XSIZE_HIMETRIC hw = 0; OLE_YSIZE_HIMETRIC hh = 0;
    pic->get_Width(&hw); pic->get_Height(&hh);
    if (hw <= 0 || hh <= 0) { pic->Release(); return NULL; }
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int tw = std::min(900, (int)((wa.right - wa.left) * 0.7));
    int th = (int)((double)tw * (double)hh / (double)hw + 0.5);
    HDC scr = GetDC(NULL);
    HDC mdc = CreateCompatibleDC(scr);
    HBITMAP bmp = CreateCompatibleBitmap(scr, tw, th);
    HGDIOBJ old = SelectObject(mdc, bmp);
    SetStretchBltMode(mdc, HALFTONE);
    SetBrushOrgEx(mdc, 0, 0, NULL);
    hr = pic->Render(mdc, 0, 0, tw, th, 0, hh, hw, -hh, NULL);
    SelectObject(mdc, old);
    DeleteDC(mdc); ReleaseDC(NULL, scr);
    pic->Release();
    if (FAILED(hr)) { DeleteObject(bmp); return NULL; }
    outW = tw; outH = th;
    return bmp;
}

static LRESULT CALLBACK SplashProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
        if (s_splashBmp) {
            RECT rc; GetClientRect(hwnd, &rc);
            HDC mdc = CreateCompatibleDC(dc);
            HGDIOBJ old = SelectObject(mdc, s_splashBmp);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mdc, 0, 0, SRCCOPY);
            SelectObject(mdc, old); DeleteDC(mdc);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: case WM_KEYDOWN: s_splashDismissed = true; return 0;       // click / key = skip
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND ShowSplash(HINSTANCE hInst) {
    int w = 0, h = 0;
    s_splashBmp = LoadSplashBitmap(hInst, w, h);
    if (!s_splashBmp) return NULL;                         // picture missing / undecodable: start without a splash
    WNDCLASSEXW wc; memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc; wc.hInstance = hInst; wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpfnWndProc = SplashProc; wc.lpszClassName = SPLASH_CLASS;
    if (!RegisterClassExW(&wc)) return NULL;
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    const int x = wa.left + ((wa.right - wa.left) - w) / 2, y = wa.top + ((wa.bottom - wa.top) - h) / 2;
    HWND hw = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, SPLASH_CLASS, APP_TITLE, WS_POPUP | WS_VISIBLE, x, y, w, h, NULL, NULL, hInst, NULL);
    if (hw) { ShowWindow(hw, SW_SHOWNOACTIVATE); UpdateWindow(hw); }
    return hw;
}

// Keeps the splash up until 'until' (GetTickCount time) or a click, then removes it.
static void EndSplash(HWND splash, DWORD until) {
    if (!splash) return;
    while (!s_splashDismissed && (LONG)(until - GetTickCount()) > 0) {
        MSG m;
        while (PeekMessage(&m, NULL, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) { PostQuitMessage((int)m.wParam); s_splashDismissed = true; break; }
            TranslateMessage(&m); DispatchMessage(&m);
        }
        if (s_splashDismissed) break;
        MsgWaitForMultipleObjects(0, NULL, FALSE, 30, QS_ALLINPUT);
    }
    DestroyWindow(splash);
    if (s_splashBmp) { DeleteObject(s_splashBmp); s_splashBmp = NULL; }
}

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: OnCreate(hwnd); return 0;
    case WM_SIZE: if (g.hProc) Layout(); return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)lp; mm->ptMinTrackSize.x = 980; mm->ptMinTrackSize.y = 560; return 0;
    }
    case WM_DRAWITEM:
        if (wp == ID_PLAY || wp == ID_STOP || wp == ID_HOME) { DrawTransport((DRAWITEMSTRUCT*)lp); return TRUE; }
        break;
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == g.hProc && g.procOn) {
            SetBkColor((HDC)wp, RGB(214, 64, 52)); SetTextColor((HDC)wp, RGB(255, 255, 255));
            return (LRESULT)g.brRed;
        }
        break;
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id >= ID_LANE0 && id < ID_LANE0 + LANE_COUNT) {
            int i = id - ID_LANE0;
            bool on = lp ? (SendMessageW(g.hLane[i], BM_GETCHECK, 0, 0) == BST_CHECKED) : !V.laneOn[i];
            SetLane(i, on);
            return 0;
        }
        if (id >= ID_RECENT0 && id < ID_RECENT0 + MAX_RECENT) {
            const size_t i = (size_t)(id - ID_RECENT0);
            if (i < g.recent.size()) { const std::wstring path = g.recent[i]; if (ConfirmDiscard()) OpenAny(path); }
            return 0;
        }
        switch (id) {
        case ID_OPEN: DoOpen(false); break;
        case ID_OPENPROJ: DoOpen(true); break;
        case ID_SAVE: SaveProject(false); break;
        case ID_SAVEAS: SaveProject(true); break;
        case ID_EXPORT: DoExport(); break;
        case ID_MK_INSERT: MarkerInsertAt(V.startTick); break;
        case ID_RECENT_CLEAR: g.recent.clear(); SaveRecent(); RebuildRecentMenu(); break;
        case ID_EXIT: SendMessageW(hwnd, WM_CLOSE, 0, 0); break;
        case ID_UNDO: DoUndo(); break;
        case ID_COMMITALL: DoCommit(-1); break;
        case ID_BYPASS: SetBypass(!g.bypassAll); break;
        case ID_CHAIN: OpenChainDialog(); break;
        case ID_COPYALL: DoCopyAll(); break;
        case ID_HELP: ShowHelpWindow(hwnd); break;
        case ID_ABOUT: ShowAbout(); break;
        case ID_SELCLEAR: V.selStart = V.selEnd = 0; InvalidateRect(g.hArr, NULL, FALSE); UpdateSelStatus(); break;
        case ID_LOOP:                                   // leaving the loop while playing: the music continues to the end
            if (code == BN_CLICKED) {
                const bool on = SendMessageW(g.hLoop, BM_GETCHECK, 0, 0) == BST_CHECKED;
                const bool wasPlaying = g.player.IsPlaying();
                g.player.SetLoop(on);
                if (wasPlaying) SetStatus(on ? L"Loop on." : L"Loop off - playing on to the end of the song.");
            }
            break;
        case ID_PLAY: DoPlayAt(V.startTick); break;
        case ID_STOP: DoStop(); break;
        case ID_HOME:
            V.startTick = 0; V.scrollTick = 0;
            UpdateArrangeScroll(); InvalidateRect(g.hArr, NULL, FALSE);
            break;
        case ID_GHOST: SetGhost(lp ? (SendMessageW(g.hGhost, BM_GETCHECK, 0, 0) == BST_CHECKED) : !V.showGhost); break;
        case ID_TRK_BIGGER: ChangeRowHeight(+1); break;
        case ID_TRK_SMALLER: ChangeRowHeight(-1); break;
        case ID_ZOOMIN: ChangeZoom(1.25); break;
        case ID_ZOOMOUT: ChangeZoom(0.8); break;
        case ID_BEND:
            if (code == EN_CHANGE) OnBendRangeEdited();
            else if (code == EN_KILLFOCUS) SetEditNumber(g.hBend, g.bendRange);          // show the limited value (0.5 .. 24)
            break;
        }
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR* h = (NMHDR*)lp;
        if (h->idFrom == ID_BENDSPIN && h->code == UDN_DELTAPOS) {                    // bend range: spinner, 0.5 .. 24 semitones
            NMUPDOWN* ud = (NMUPDOWN*)lp;
            double cur; if (!EditNumber(g.hBend, cur)) cur = g.bendRange;
            double nv = std::floor(cur * 2.0 + 0.5) / 2.0 + 0.5 * ud->iDelta;
            SetEditNumber(g.hBend, std::max(0.5, std::min(24.0, nv)));
            return 1;
        }
        return 0;
    }
    case WM_ROLL_CLICKED: if (g.loaded) UpdateSelStatus(); return 0;
    case WM_TIMER:
        if (wp == 1) {
            if (g.player.IsPlaying()) {
                double ms = g.player.PosMs();
                V.playTick = mc::MsToTick(g.tempo, g.preview.ppq, ms);
                V.playing = true;
                EnsureVisible(V.playTick);
                InvalidateRect(g.hArr, NULL, FALSE);
            } else if (V.playing) {
                StopUI();
            }
        } else if (wp == 2) {
            KillTimer(hwnd, 2);
            if (g.player.IsPlaying()) DoPlayAt(V.playTick);   // restart from the playhead with the new settings
        }
        return 0;
    case WM_PLAYER_DONE:
        if (!g.player.IsPlaying()) StopUI();
        return 0;
    case WM_DROPFILES: {
        wchar_t buf[MAX_PATH * 2];
        if (DragQueryFileW((HDROP)wp, 0, buf, MAX_PATH * 2) && ConfirmDiscard()) OpenAny(buf);
        DragFinish((HDROP)wp);
        return 0;
    }
    case WM_CLOSE:
        if (!ConfirmDiscard()) return 0;
        break;
    case WM_DESTROY:
        g.player.Stop();
        if (g.dlg.hwnd) DestroyWindow(g.dlg.hwnd);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nShow) {
    CoInitialize(NULL);                                       // (OleLoadPicture of the splash picture)
    HWND hSplash = ShowSplash(hInst);                         // shown at once, while the rest starts up
    const DWORD splashUntil = GetTickCount() + 2500;          // minimum display time
    RecalcColumns();
    INITCOMMONCONTROLSEX ic = { sizeof ic, ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_LINK_CLASS | ICC_UPDOWN_CLASS };
    InitCommonControlsEx(&ic);

    g_rich = LoadLibraryW(L"Msftedit.dll") != NULL;          // rich-text control for the coloured help text

    // the Rubato icon (resource 1, see Rubato.rc); the standard icon if it is missing
    HICON hIconBig = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    HICON hIconSmall = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!hIconBig) hIconBig = LoadIcon(NULL, IDI_APPLICATION);
    if (!hIconSmall) hIconSmall = hIconBig;

    WNDCLASSEXW wc; memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc; wc.hInstance = hInst; wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = hIconBig; wc.hIconSm = hIconSmall;
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = ArrangeProc; wc.lpszClassName = ARR_CLASS;
    if (!RegisterClassExW(&wc)) return 1;

    wc.style = 0;
    wc.lpfnWndProc = DlgProc; wc.lpszClassName = DLG_CLASS;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    if (!RegisterClassExW(&wc)) return 1;

    wc.lpfnWndProc = AboutProc; wc.lpszClassName = ABOUT_CLASS;
    if (!RegisterClassExW(&wc)) return 1;

    wc.lpfnWndProc = HelpProc; wc.lpszClassName = HELP_CLASS;
    if (!RegisterClassExW(&wc)) return 1;

    wc.lpfnWndProc = PromptProc; wc.lpszClassName = PROMPT_CLASS;
    if (!RegisterClassExW(&wc)) return 1;

    wc.lpfnWndProc = MainProc; wc.lpszClassName = MAIN_CLASS;
    if (!RegisterClassExW(&wc)) return 1;

    HWND hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, MAIN_CLASS, APP_TITLE,
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1360, 780, NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;
    EndSplash(hSplash, splashUntil);
    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    ACCEL acc[5] = { { FVIRTKEY | FCONTROL, 'O', ID_OPEN }, { FVIRTKEY | FCONTROL, 'S', ID_SAVE },
                     { FVIRTKEY | FCONTROL | FSHIFT, 'S', ID_EXPORT },
                     { FVIRTKEY | FCONTROL, 'Z', ID_UNDO }, { FVIRTKEY | FCONTROL, 'E', ID_CHAIN } };
    HACCEL hAcc = CreateAcceleratorTable(acc, 5);

    int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) { if (argc > 1) OpenAny(argv[1]); LocalFree(argv); }

    MSG m;
    while (GetMessage(&m, NULL, 0, 0) > 0) {
        if (m.message == WM_MOUSEWHEEL || m.message == WM_MOUSEHWHEEL) {          // wheel goes to the control under the cursor
            POINT pt; GetCursorPos(&pt);
            if (WindowFromPoint(pt) == g.hArr) m.hwnd = g.hArr;
        }
        if ((m.message == WM_KEYDOWN || m.message == WM_KEYUP) && m.wParam == VK_SPACE && g.loaded && IsWindowEnabled(hwnd)) {
            // Space bar = play / stop, unless a text box has the focus (there it types a space)
            HWND f = GetFocus();
            const bool mine = f && (f == hwnd || IsChild(hwnd, f) || (g.dlg.hwnd && (f == g.dlg.hwnd || IsChild(g.dlg.hwnd, f))));
            wchar_t cn[32] = L""; if (f) GetClassNameW(f, cn, 32);
            const bool text = !_wcsicmp(cn, L"Edit") || !_wcsicmp(cn, L"RICHEDIT50W") || !_wcsicmp(cn, L"ComboBox") || !_wcsicmp(cn, L"ComboLBox");
            if (mine && !text) {
                if (m.message == WM_KEYDOWN && !(m.lParam & (1 << 30))) TogglePlay();       // not on key repeat
                continue;
            }
        }
        if (!TranslateAccelerator(hwnd, hAcc, &m)) {
            TranslateMessage(&m);
            DispatchMessage(&m);
        }
    }
    DestroyAcceleratorTable(hAcc);
    CoUninitialize();
    return (int)m.wParam;
}
