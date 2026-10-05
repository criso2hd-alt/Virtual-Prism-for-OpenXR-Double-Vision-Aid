// Virtual Prism: flat-screen settings window + launcher for the in-VR calibration.
#include <windows.h>

#include <commctrl.h>

#include <atomic>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>
#include <string>
#include <thread>

#include <gdiplus.h>
#include <shellapi.h>

#include "branding.h"
#include "scenes.h"
#include "shared.h"
#include "vr_calibration.h"

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace dfx;

namespace {

const wchar_t* kLayerName = L"XR_APILAYER_NOVENDOR_virtual_prism";
const wchar_t* kRegKey = L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit";

enum {
    ID_ENABLE = 100, ID_SAVE, ID_START, ID_STOP, ID_INSTALL, ID_UNINSTALL, ID_STATUS, ID_LAYERSTATE,
    ID_EYE_BASE = 200  // + eye*10 + {0: H edit, 1: H base, 2: V edit, 3: V base, 4: rotation edit}
};
constexpr UINT WM_VR_STATUS = WM_APP + 1, WM_VR_DONE = WM_APP + 2;

HWND g_wnd;
HFONT g_font;
int g_dpi = 96;
std::thread g_vrThread;
std::atomic<bool> g_cancel{false};
std::atomic<bool> g_vrRunning{false};
Preview g_preview;
constexpr UINT_PTR kPreviewTimer = 1;
bool g_previewDirty = true;

// Preview layout (96-dpi pixels): big combined view + two small per-eye views.
const RECT kBig = {700, 36, 1060, 396};
const RECT kSmall[2] = {{600, 430, 870, 700}, {890, 430, 1160, 700}};
const RECT kPreviewArea = {590, 0, 1180, 730};
const int kClientW = 1180, kClientH = 760;
const RECT kBanner = {0, 730, 1180, 760};
bool g_hideBanner = false;
RECT g_repoLink = {}, g_bannerRect = {}, g_bannerClose = {};  // device pixels, set while painting

int S(int v) { return MulDiv(v, g_dpi, 96); }

HWND Add(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id = 0,
         DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_wnd,
                             (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

std::wstring ExeDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p;
    return s.substr(0, s.find_last_of(L'\\'));
}
std::wstring ManifestPath() { return ExeDir() + L"\\" + kLayerName + L".json"; }

bool LayerInstalled() {
    DWORD v;
    DWORD sz = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, kRegKey, ManifestPath().c_str(), RRF_RT_REG_DWORD, nullptr, &v, &sz) ==
           ERROR_SUCCESS;
}

bool InstallLayer(std::wstring& err) {
    if (GetFileAttributesW(ManifestPath().c_str()) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW((ExeDir() + L"\\" + kLayerName + L".dll").c_str()) == INVALID_FILE_ATTRIBUTES) {
        err = L"Layer files not found next to the program.";
        return false;
    }
    DWORD zero = 0;
    LSTATUS s = RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, ManifestPath().c_str(), REG_DWORD, &zero, sizeof(zero));
    if (s != ERROR_SUCCESS) { err = L"Could not write the registry entry."; return false; }
    return true;
}
void UninstallLayer() { RegDeleteKeyValueW(HKEY_CURRENT_USER, kRegKey, ManifestPath().c_str()); }

// ---- Conversions between the prescription fields and signed image shifts ----
// A prism moves the image toward its apex (away from the base).
//   H: right eye base-out / left eye base-in => image moves left; the opposite => right.
//   V: base-up => image moves down; base-down => image moves up.
float ReadEdit(int id) {
    wchar_t buf[64] = {};
    GetWindowTextW(GetDlgItem(g_wnd, id), buf, 64);
    for (wchar_t* p = buf; *p; ++p) if (*p == L',') *p = L'.';
    return (float)_wtof(buf);
}
void WriteEdit(int id, float v) {
    wchar_t buf[64];
    swprintf_s(buf, L"%.2f", v);
    SetWindowTextW(GetDlgItem(g_wnd, id), buf);
}
int ComboSel(int id) { return (int)SendMessageW(GetDlgItem(g_wnd, id), CB_GETCURSEL, 0, 0); }

Config ReadGui() {
    Config c;
    c.enabled = SendMessageW(GetDlgItem(g_wnd, ID_ENABLE), BM_GETCHECK, 0, 0) == BST_CHECKED;
    for (int e = 0; e < 2; e++) {
        int b = ID_EYE_BASE + e * 10;
        float h = std::fabs(ReadEdit(b + 0)), v = std::fabs(ReadEdit(b + 2));
        bool baseOut = ComboSel(b + 1) == 1;  // 0 = base in, 1 = base out
        bool towardRight = (e == 1) ? !baseOut : baseOut;
        c.eye[e].hPrism = towardRight ? h : -h;
        bool baseDown = ComboSel(b + 3) == 1;  // 0 = base up, 1 = base down
        c.eye[e].vPrism = baseDown ? v : -v;
        c.eye[e].rollDeg = ReadEdit(b + 4);
    }
    return c;
}

void WriteGui(const Config& c) {
    SendMessageW(GetDlgItem(g_wnd, ID_ENABLE), BM_SETCHECK, c.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    for (int e = 0; e < 2; e++) {
        int b = ID_EYE_BASE + e * 10;
        const EyeCorrection& k = c.eye[e];
        bool towardRight = k.hPrism >= 0;
        bool baseOut = (e == 1) ? !towardRight : towardRight;
        WriteEdit(b + 0, std::fabs(k.hPrism));
        SendMessageW(GetDlgItem(g_wnd, b + 1), CB_SETCURSEL, baseOut ? 1 : 0, 0);
        WriteEdit(b + 2, std::fabs(k.vPrism));
        SendMessageW(GetDlgItem(g_wnd, b + 3), CB_SETCURSEL, k.vPrism > 0 ? 1 : 0, 0);
        WriteEdit(b + 4, k.rollDeg);
    }
}

void SetStatus(const std::wstring& s) { SetWindowTextW(GetDlgItem(g_wnd, ID_STATUS), s.c_str()); }

void RefreshLayerState() {
    SetWindowTextW(GetDlgItem(g_wnd, ID_LAYERSTATE),
                   LayerInstalled() ? L"Correction layer: INSTALLED (applies to OpenXR games)"
                                    : L"Correction layer: NOT installed - click \"Install layer\"");
}

void RefreshButtons() {
    bool run = g_vrRunning;
    EnableWindow(GetDlgItem(g_wnd, ID_START), !run);
    EnableWindow(GetDlgItem(g_wnd, ID_STOP), run);
}

void StartVr() {
    if (g_vrRunning) return;
    if (g_vrThread.joinable()) g_vrThread.join();
    Config start = ReadGui();
    g_cancel = false;
    g_vrRunning = true;
    RefreshButtons();
    SetStatus(L"Starting VR session...");
    g_vrThread = std::thread([start] {
        VrResult r = RunVrCalibration(start, g_cancel, [](const std::string& m) {
            PostMessageW(g_wnd, WM_VR_STATUS, 0, (LPARAM) new std::wstring(m.begin(), m.end()));
        }, &g_preview);
        PostMessageW(g_wnd, WM_VR_DONE, 0, (LPARAM) new VrResult(r));
    });
}

// Draws one eye's image the way the wearer's correction displaces it: shifted by the prism
// and rotated about the centre. `add` accumulates (saturating) so both eyes overlap like in the headset.
void ComposeEye(const std::vector<uint32_t>& src, const EyeCorrection& ec, std::vector<uint32_t>& dst, bool add) {
    const int N = kPreviewSize;
    if (src.size() != (size_t)N * N) return;
    const float k = N / (2.0f * kQuadHalfTan);  // pixels per unit tan(angle)
    const float dx = k * ec.hPrism / 100.0f, dy = -k * ec.vPrism / 100.0f;
    const float th = ec.rollDeg * (float)kPi / 180.0f, cs = std::cos(th), sn = std::sin(th);
    const float c = N / 2.0f;
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            float ux = x - c - dx, uy = y - c - dy;
            int sx = (int)(c + cs * ux + sn * uy), sy = (int)(c - sn * ux + cs * uy);
            if (sx < 0 || sy < 0 || sx >= N || sy >= N) continue;
            uint32_t p = src[(size_t)sy * N + sx];
            uint32_t& d = dst[(size_t)y * N + x];
            if (!add) { d = p; continue; }
            uint32_t b = std::min(255u, (d & 0xFF) + (p & 0xFF));
            uint32_t g = std::min(255u, ((d >> 8) & 0xFF) + ((p >> 8) & 0xFF));
            uint32_t r = std::min(255u, ((d >> 16) & 0xFF) + ((p >> 16) & 0xFF));
            d = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
}

void BlitPreview(HDC dc, const std::vector<uint32_t>& px, const RECT& r96, int ox, int oy) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = kPreviewSize;
    bi.bmiHeader.biHeight = -kPreviewSize;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    StretchDIBits(dc, S(r96.left) - ox, S(r96.top) - oy, S(r96.right - r96.left), S(r96.bottom - r96.top), 0, 0,
                  kPreviewSize, kPreviewSize, px.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
}

// Draws the whole spectator area into an off-screen bitmap, then copies it to the window in one go (no flicker).
void PaintPreview(HDC target) {
    std::vector<uint32_t> img[2];
    Config v;
    int eyeMode;
    bool have;
    {
        std::lock_guard<std::mutex> l(g_preview.m);
        have = g_preview.active && g_preview.hasImage;
        img[0] = g_preview.img[0];
        img[1] = g_preview.img[1];
        v = g_preview.values;
        eyeMode = g_preview.eyeMode;
    }
    const int ox = S(kPreviewArea.left), oy = S(kPreviewArea.top);
    const int w = S(kPreviewArea.right - kPreviewArea.left), h = S(kPreviewArea.bottom - kPreviewArea.top);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, w, h);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    RECT all = {0, 0, w, h};
    FillRect(dc, &all, (HBRUSH)(COLOR_BTNFACE + 1));

    const size_t n = (size_t)kPreviewSize * kPreviewSize;
    std::vector<uint32_t> eyeImg[2] = {std::vector<uint32_t>(n, 0xFF000000u), std::vector<uint32_t>(n, 0xFF000000u)};
    std::vector<uint32_t> both(n, 0xFF000000u);
    for (int e = 0; e < 2; e++)
        if (have && (eyeMode == 0 || eyeMode == e + 1)) {
            ComposeEye(img[e], v.eye[e], eyeImg[e], false);
            ComposeEye(img[e], v.eye[e], both, true);
        }
    BlitPreview(dc, both, kBig, ox, oy);
    for (int e = 0; e < 2; e++) BlitPreview(dc, eyeImg[e], kSmall[e], ox, oy);

    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, g_font);
    auto text = [&](int x, int y, const wchar_t* t) { TextOutW(dc, S(x) - ox, S(y) - oy, t, (int)wcslen(t)); };
    text(700, 14, L"What the wearer sees: both eyes combined");
    text(600, 406, L"Left eye");
    text(890, 406, L"Right eye");
    if (!have) {
        SetTextColor(dc, RGB(200, 200, 200));
        text(722, 208, L"Starts when VR calibration runs");
    }
    SelectObject(dc, oldFont);

    BitBlt(target, ox, oy, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

// ---- Logo + branding ----
void DrawLogo(Gdiplus::Graphics& g, float x, float y, float size) {
    using namespace Gdiplus;
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    float k = size / 512.0f;
    auto pt = [&](float px, float py) { return PointF(x + px * k, y + py * k); };
    GraphicsPath bg;
    float r = 104 * k, d = 2 * r;
    bg.AddArc(x, y, d, d, 180, 90);
    bg.AddArc(x + size - d, y, d, d, 270, 90);
    bg.AddArc(x + size - d, y + size - d, d, d, 0, 90);
    bg.AddArc(x, y + size - d, d, d, 90, 90);
    bg.CloseFigure();
    LinearGradientBrush grad(pt(0, 0), pt(512, 512), Color(255, 27, 31, 58), Color(255, 11, 13, 28));
    g.FillPath(&grad, &bg);
    // the two eyes' images, fused (white) where they overlap
    SolidBrush red(Color(255, 255, 70, 70)), cyan(Color(255, 40, 220, 255)), white(Color(255, 255, 255, 255));
    float R = 118 * k;
    g.FillEllipse(&red, x + (206 - 118) * k, y + (256 - 118) * k, 2 * R, 2 * R);
    g.FillEllipse(&cyan, x + (306 - 118) * k, y + (256 - 118) * k, 2 * R, 2 * R);
    GraphicsPath left;
    left.AddEllipse(x + (206 - 118) * k, y + (256 - 118) * k, 2 * R, 2 * R);
    Region clip(&left);
    g.SetClip(&clip);
    g.FillEllipse(&white, x + (306 - 118) * k, y + (256 - 118) * k, 2 * R, 2 * R);
    g.ResetClip();
    // prism
    PointF tri[3] = {pt(256, 190), pt(326, 318), pt(186, 318)};
    SolidBrush fill(Color(36, 11, 13, 28));
    Pen pen(Color(255, 11, 13, 28), 13 * k);
    pen.SetLineJoin(LineJoinRound);
    g.FillPolygon(&fill, tri, 3);
    g.DrawPolygon(&pen, tri, 3);
}

HICON MakeLogoIcon(int size) {
    Gdiplus::Bitmap bmp(size, size, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics g(&bmp);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        DrawLogo(g, 0, 0, (float)size);
    }
    HICON icon = nullptr;
    bmp.GetHICON(&icon);
    return icon;
}

void OpenUrl(const wchar_t* url) { ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL); }

void DismissBanner() {
    g_hideBanner = true;
    CreateDirectoryW(ConfigDir().c_str(), nullptr);
    WritePrivateProfileStringW(L"General", L"HideDonateBanner", L"1", ConfigPath().c_str());
    InvalidateRect(g_wnd, nullptr, TRUE);
}

void PaintBranding(HDC dc) {
    {
        using namespace Gdiplus;
        Graphics g(dc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        DrawLogo(g, (float)S(24), (float)S(468), (float)S(128));

        FontFamily ff(L"Segoe UI");
        auto px = [&](float pt) { return pt * g_dpi / 72.0f; };
        Font nameFont(&ff, px(17), FontStyleBold, UnitPixel), tagFont(&ff, px(12), FontStyleRegular, UnitPixel),
            smallFont(&ff, px(10), FontStyleRegular, UnitPixel), linkFont(&ff, px(10), FontStyleUnderline, UnitPixel);
        SolidBrush dark(Color(255, 30, 32, 48)), grey(Color(255, 100, 104, 120)), link(Color(255, 0, 102, 204));
        float x = (float)S(168);
        g.DrawString(brand::kName, -1, &nameFont, PointF(x, (float)S(486)), &dark);
        g.DrawString(brand::kTagline, -1, &tagFont, PointF(x, (float)S(520)), &grey);
        std::wstring by = std::wstring(L"v") + brand::kVersion + L"  \u00B7  Free to use  \u00B7  by " + brand::kAuthor;
        g.DrawString(by.c_str(), -1, &smallFont, PointF(x, (float)S(552)), &grey);
        PointF org(x, (float)S(576));
        RectF box;
        g.MeasureString(brand::kRepoText, -1, &linkFont, org, &box);
        g.DrawString(brand::kRepoText, -1, &linkFont, org, &link);
        g_repoLink = {(LONG)box.X, (LONG)box.Y, (LONG)(box.X + box.Width), (LONG)(box.Y + box.Height)};
    }

    // Discreet "Buy me a coffee" strip along the bottom edge, dismissable with the X.
    g_bannerRect = g_bannerClose = {};
    if (!g_hideBanner) {
        g_bannerRect = {S(kBanner.left), S(kBanner.top), S(kBanner.right), S(kBanner.bottom)};
        HBRUSH yellow = CreateSolidBrush(RGB(255, 221, 0));
        FillRect(dc, &g_bannerRect, yellow);
        DeleteObject(yellow);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(30, 30, 30));
        HGDIOBJ old = SelectObject(dc, g_font);
        const wchar_t* msg = L"\u2615  Enjoying Virtual Prism? You can buy me a coffee";
        SIZE sz;
        GetTextExtentPoint32W(dc, msg, (int)wcslen(msg), &sz);
        int ty = g_bannerRect.top + (g_bannerRect.bottom - g_bannerRect.top - sz.cy) / 2;
        TextOutW(dc, S(14), ty, msg, (int)wcslen(msg));
        int cs = S(30);
        g_bannerClose = {g_bannerRect.right - cs - S(6), g_bannerRect.top, g_bannerRect.right - S(6), g_bannerRect.bottom};
        const wchar_t* cross = L"\u2715";
        GetTextExtentPoint32W(dc, cross, 1, &sz);
        TextOutW(dc, g_bannerClose.left + (cs - sz.cx) / 2, ty, cross, 1);
        SelectObject(dc, old);
    }
}

void BuildUi() {
    int dpi = g_dpi;
    NONCLIENTMETRICSW ncm = {sizeof(ncm)};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight = -MulDiv(9, dpi, 72);
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    g_font = CreateFontIndirectW(&lf);

    Add(L"BUTTON", L"Enable correction in VR games (turn off to share your headset with someone else)",
        BS_AUTOCHECKBOX, 20, 14, 560, 22, ID_ENABLE);

    const wchar_t* title[2] = {L"Left eye", L"Right eye"};
    for (int e = 0; e < 2; e++) {
        int x = 20 + e * 290, b = ID_EYE_BASE + e * 10;
        Add(L"BUTTON", title[e], BS_GROUPBOX, x, 46, 270, 170);
        Add(L"STATIC", L"Horizontal prism (Δ)", 0, x + 12, 76, 130, 20);
        Add(L"EDIT", L"0.00", ES_AUTOHSCROLL, x + 12, 98, 60, 24, b + 0, WS_EX_CLIENTEDGE);
        HWND c1 = Add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, x + 80, 98, 100, 120, b + 1);
        SendMessageW(c1, CB_ADDSTRING, 0, (LPARAM)L"Base in");
        SendMessageW(c1, CB_ADDSTRING, 0, (LPARAM)L"Base out");
        Add(L"STATIC", L"Vertical prism (Δ)", 0, x + 12, 128, 130, 20);
        Add(L"EDIT", L"0.00", ES_AUTOHSCROLL, x + 12, 150, 60, 24, b + 2, WS_EX_CLIENTEDGE);
        HWND c2 = Add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, x + 80, 150, 100, 120, b + 3);
        SendMessageW(c2, CB_ADDSTRING, 0, (LPARAM)L"Base up");
        SendMessageW(c2, CB_ADDSTRING, 0, (LPARAM)L"Base down");
        Add(L"STATIC", L"Rotation (°, + = clockwise)", 0, x + 12, 180, 160, 20);
        Add(L"EDIT", L"0.00", ES_AUTOHSCROLL, x + 180, 177, 60, 24, b + 4, WS_EX_CLIENTEDGE);
    }

    Add(L"STATIC",
        L"Type your prism prescription above, or press \"Start VR calibration\" and align the images by eye "
        "inside the headset (recommended - it measures exactly what you need). Changes are picked up by running "
        "games within a second.",
        0, 20, 226, 560, 48);
    Add(L"BUTTON", L"Save settings", BS_PUSHBUTTON | WS_TABSTOP, 20, 282, 130, 32, ID_SAVE);
    Add(L"BUTTON", L"Start VR calibration", BS_DEFPUSHBUTTON | WS_TABSTOP, 160, 282, 170, 32, ID_START);
    Add(L"BUTTON", L"Stop VR", BS_PUSHBUTTON | WS_TABSTOP, 340, 282, 100, 32, ID_STOP);
    Add(L"STATIC", L"", 0, 20, 328, 560, 20, ID_LAYERSTATE);
    Add(L"BUTTON", L"Install layer", BS_PUSHBUTTON | WS_TABSTOP, 20, 352, 130, 30, ID_INSTALL);
    Add(L"BUTTON", L"Uninstall layer", BS_PUSHBUTTON | WS_TABSTOP, 160, 352, 130, 30, ID_UNINSTALL);
    Add(L"STATIC", L"Ready.", SS_LEFT, 20, 398, 560, 44, ID_STATUS);

    WriteGui(LoadConfig());
    RefreshLayerState();
    RefreshButtons();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            g_wnd = hwnd;
            g_hideBanner = GetPrivateProfileIntW(L"General", L"HideDonateBanner", 0, ConfigPath().c_str()) != 0;
            SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)MakeLogoIcon(32));
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)MakeLogoIcon(16));
            BuildUi(); SetTimer(hwnd, kPreviewTimer, 33, nullptr); return 0;
        case WM_LBUTTONDOWN: {
            POINT p = {(short)LOWORD(lp), (short)HIWORD(lp)};
            if (!g_hideBanner && PtInRect(&g_bannerClose, p)) DismissBanner();
            else if (!g_hideBanner && PtInRect(&g_bannerRect, p)) OpenUrl(brand::kCoffeeUrl);
            else if (PtInRect(&g_repoLink, p)) OpenUrl(brand::kRepoUrl);
            return 0;
        }
        case WM_SETCURSOR: {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            if (LOWORD(lp) == HTCLIENT && ((!g_hideBanner && PtInRect(&g_bannerRect, p)) || PtInRect(&g_repoLink, p))) {
                SetCursor(LoadCursor(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        }
        case WM_TIMER: {
            if (g_vrRunning || g_previewDirty) {
                RECT pr = {S(kPreviewArea.left), S(kPreviewArea.top), S(kPreviewArea.right), S(kPreviewArea.bottom)};
                InvalidateRect(hwnd, &pr, FALSE);
            }
            g_previewDirty = g_vrRunning;
            return 0;
        }
        case WM_ERASEBKGND: {  // the preview area is fully painted off-screen; only clear the rest
            RECT rest;
            GetClientRect(hwnd, &rest);
            RECT left = rest, bottom = rest;
            left.right = S(kPreviewArea.left);
            bottom.top = S(kPreviewArea.bottom);
            FillRect((HDC)wp, &left, (HBRUSH)(COLOR_BTNFACE + 1));
            FillRect((HDC)wp, &bottom, (HBRUSH)(COLOR_BTNFACE + 1));
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT all;
            GetClientRect(hwnd, &all);
            PaintPreview(dc);
            PaintBranding(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case ID_ENABLE: {
                    Config c = LoadConfig();
                    c.enabled = SendMessageW(GetDlgItem(hwnd, ID_ENABLE), BM_GETCHECK, 0, 0) == BST_CHECKED;
                    SaveConfig(c);
                    SetStatus(c.enabled ? L"Correction ON." : L"Correction OFF (games show the normal image).");
                    break;
                }
                case ID_SAVE: SaveConfig(ReadGui()); SetStatus(L"Settings saved."); break;
                case ID_START: StartVr(); break;
                case ID_STOP: g_cancel = true; SetStatus(L"Stopping VR session..."); break;
                case ID_INSTALL: {
                    std::wstring err;
                    if (InstallLayer(err)) SetStatus(L"Layer installed. Restart any running VR game to pick it up.");
                    else SetStatus(err);
                    RefreshLayerState();
                    break;
                }
                case ID_UNINSTALL: UninstallLayer(); SetStatus(L"Layer uninstalled."); RefreshLayerState(); break;
            }
            return 0;
        case WM_VR_STATUS: {
            auto* s = (std::wstring*)lp;
            SetStatus(*s);
            delete s;
            return 0;
        }
        case WM_VR_DONE: {
            auto* r = (VrResult*)lp;
            if (g_vrThread.joinable()) g_vrThread.join();
            g_vrRunning = false;
            std::wstring msg(r->message.begin(), r->message.end());
            if (r->saved) {
                Config c = r->config;
                c.enabled = true;
                SaveConfig(c);
                WriteGui(c);
                msg += L" Values are applied to OpenXR games";
                msg += LayerInstalled() ? L"." : L" once you click \"Install layer\".";
            }
            SetStatus(msg);
            RefreshButtons();
            delete r;
            return 0;
        }
        case WM_CLOSE:
            if (g_vrRunning) {
                g_cancel = true;
                if (g_vrThread.joinable()) g_vrThread.join();
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetProcessDPIAware();
    HDC dc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    InitCommonControls();
    InitScenes();

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"VirtualPrismWnd";
    RegisterClassW(&wc);

    RECT rc = {0, 0, S(kClientW), S(kClientH)};
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    HWND w = CreateWindowW(wc.lpszClassName, L"Virtual Prism for OpenXR (Double Vision Aid)", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                           CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr,
                           inst, nullptr);
    ShowWindow(w, show);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(w, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    ShutdownScenes();
    return 0;
}
