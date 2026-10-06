#include "scenes.h"

#include <windows.h>

#include <cmath>
#include <string>
#include <vector>

#include <gdiplus.h>

namespace dfx {

using namespace Gdiplus;

namespace {

ULONG_PTR g_gdiplusToken = 0;

constexpr float kCenter = kImageSize / 2.0f;
constexpr float kScale = kCenter / kQuadHalfTan;  // pixels per unit of tan(angle)
constexpr float kMaxDeg = 40.0f;

const wchar_t* kSceneNames[] = {
    L"Color grid (left = red, right = cyan)", L"White grid (for fusing)",   L"Dot lattice (for fusing)",
    L"Target rings (for fusing)",             L"Lines + tilt (rotation check)", L"Reading chart",
    L"3D test room (look around, near and far)"};
const wchar_t* kStepNames[] = {L"coarse", L"medium", L"fine"};

float Px(float deg) { return kCenter + kScale * std::tan(deg * (float)kPi / 180.0f); }

Color EyeColor(int eye, bool colorCoded) {
    if (!colorCoded) return Color(255, 255, 255, 255);
    return eye == 0 ? Color(255, 255, 70, 70) : Color(255, 40, 220, 255);
}

void DrawGrid(Graphics& g, Color col, bool labels) {
    Pen minor(Color(150, col.GetR(), col.GetG(), col.GetB()), 2.0f);
    Pen major(col, 3.5f);
    SolidBrush text(col);
    FontFamily ff(L"Consolas");
    Font font(&ff, 26, FontStyleRegular, UnitPixel);
    float lo = Px(-kMaxDeg), hi = Px(kMaxDeg);
    for (int d = -40; d <= 40; d += 5) {
        Pen& p = (d % 10 == 0) ? major : minor;
        float x = Px((float)d);
        g.DrawLine(&p, x, lo, x, hi);
        g.DrawLine(&p, lo, x, hi, x);
        if (labels && d % 10 == 0 && d != 0) {
            std::wstring s = std::to_wstring(d);
            g.DrawString(s.c_str(), -1, &font, PointF(x + 3, kCenter + 4), &text);
        }
    }
    Pen center(col, 6.0f);
    g.DrawLine(&center, kCenter - 60, kCenter, kCenter + 60, kCenter);
    g.DrawLine(&center, kCenter, kCenter - 60, kCenter, kCenter + 60);
}

void DrawDots(Graphics& g, Color col) {
    SolidBrush b(col);
    for (int y = -40; y <= 40; y += 5)
        for (int x = -40; x <= 40; x += 5) {
            float r = (x == 0 && y == 0) ? 14.0f : 6.0f;
            g.FillEllipse(&b, Px((float)x) - r, Px((float)y) - r, 2 * r, 2 * r);
        }
    Pen p(col, 3.0f);
    g.DrawLine(&p, kCenter - 90, kCenter, kCenter + 90, kCenter);
    g.DrawLine(&p, kCenter, kCenter - 90, kCenter, kCenter + 90);
}

void DrawTarget(Graphics& g, Color col) {
    Pen thin(Color(130, col.GetR(), col.GetG(), col.GetB()), 2.0f);
    Pen thick(col, 4.0f);
    for (int d = 5; d <= 40; d += 5) {
        float r = Px((float)d) - kCenter;
        g.DrawEllipse(d % 10 == 0 ? &thick : &thin, kCenter - r, kCenter - r, 2 * r, 2 * r);
    }
    g.DrawLine(&thin, kCenter, Px(-40), kCenter, Px(40));
    g.DrawLine(&thin, Px(-40), kCenter, Px(40), kCenter);
    SolidBrush b(col);
    g.FillEllipse(&b, kCenter - 12, kCenter - 12, 24.0f, 24.0f);
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2) {
            float x = Px(sx * 25.0f), y = Px(sy * 25.0f);
            g.FillRectangle(&b, x - 16, y - 16, 32.0f, 32.0f);
        }
}

void DrawLines(Graphics& g, Color col) {
    Pen thin(Color(150, col.GetR(), col.GetG(), col.GetB()), 2.0f);
    Pen thick(col, 4.0f);
    for (int d = -30; d <= 30; d += 10) {
        float x = Px((float)d);
        g.DrawLine(d == 0 ? &thick : &thin, x, Px(-40), x, Px(40));
        g.DrawLine(d == 0 ? &thick : &thin, Px(-40), x, Px(40), x);
    }
    // Tilt gauge: ring with ticks every 15 degrees; any rotation mismatch shows as a doubled tick.
    float r = Px(12) - kCenter;
    g.DrawEllipse(&thick, kCenter - r, kCenter - r, 2 * r, 2 * r);
    for (int a = 0; a < 360; a += 15) {
        float rad = a * (float)kPi / 180.0f;
        float r2 = r + (a % 90 == 0 ? 70.0f : 35.0f);
        g.DrawLine(&thick, kCenter + r * std::cos(rad), kCenter + r * std::sin(rad), kCenter + r2 * std::cos(rad),
                   kCenter + r2 * std::sin(rad));
    }
}

void DrawReading(Graphics& g, Color col) {
    SolidBrush b(col);
    Pen p(col, 4.0f);
    FontFamily ff(L"Segoe UI");
    StringFormat center;
    center.SetAlignment(StringAlignmentCenter);
    const wchar_t* lines[] = {L"Reading test: does this look like one sharp line of text?",
                              L"THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG",
                              L"the quick brown fox jumps over the lazy dog 0123456789",
                              L"Small print: pack my box with five dozen liquor jugs."};
    float sizes[] = {50, 62, 46, 34};
    float y = kCenter - 260;
    for (int i = 0; i < 4; i++) {
        Font f(&ff, sizes[i], FontStyleRegular, UnitPixel);
        g.DrawString(lines[i], -1, &f, PointF(kCenter, y), &center, &b);
        y += sizes[i] * 1.9f;
    }
    float a = Px(-30), z = Px(30), len = 90;  // corner brackets
    g.DrawLine(&p, a, a, a + len, a);  g.DrawLine(&p, a, a, a, a + len);
    g.DrawLine(&p, z, a, z - len, a);  g.DrawLine(&p, z, a, z, a + len);
    g.DrawLine(&p, a, z, a + len, z);  g.DrawLine(&p, a, z, a, z - len);
    g.DrawLine(&p, z, z, z - len, z);  g.DrawLine(&p, z, z, z, z - len);
}

std::wstring Fmt(const wchar_t* f, double a, double b, double c) {
    wchar_t buf[128];
    swprintf_s(buf, f, a, b, c);
    return buf;
}

void DrawHud(Graphics& g, const UiState& ui, int eye) {
    FontFamily ff(L"Consolas");
    Font font(&ff, 38, FontStyleRegular, UnitPixel);
    Font big(&ff, 40, FontStyleRegular, UnitPixel);
    SolidBrush white(Color(255, 255, 255, 255)), dim(Color(255, 190, 190, 190)), yellow(Color(255, 255, 220, 80));
    SolidBrush panel(Color(235, 12, 12, 24));
    Pen border(Color(255, 120, 120, 160), 3.0f);
    StringFormat center;
    center.SetAlignment(StringAlignmentCenter);

    // Status panel
    float px = Px(-26), py = 1560, pw = Px(26) - px;
    g.FillRectangle(&panel, px, py, pw, 280.0f);
    g.DrawRectangle(&border, px, py, pw, 280.0f);
    std::wstring l1 = L"Scene " + std::to_wstring(ui.scene + 1) + L"/" + std::to_wstring(SceneCount()) + L": " +
                      SceneName(ui.scene);
    g.DrawString(l1.c_str(), -1, &font, PointF(kCenter, py + 14), &center, &white);
    const wchar_t* view[] = {L"both eyes", L"LEFT eye only", L"RIGHT eye only"};
    std::wstring l2 = std::wstring(L"Step: ") + StepName(ui.stepMode) + L"   Viewing: " + view[ui.eyeMode];
    g.DrawString(l2.c_str(), -1, &font, PointF(kCenter, py + 62), &center, &dim);
    const wchar_t* names[] = {L"LEFT ", L"RIGHT"};
    for (int i = 0; i < 2; i++) {
        const EyeCorrection& e = ui.values.eye[i];
        std::wstring s = std::wstring(names[i]) +
                         Fmt(L"  H %+.2fΔ  V %+.2fΔ  Rot %+.2f°", e.hPrism, e.vPrism, e.rollDeg);
        g.DrawString(s.c_str(), -1, &font, PointF(kCenter, py + 118 + i * 50), &center, &white);
    }
    std::wstring hint = ui.saveProgress > 0
                            ? L"Saving... keep holding right trigger"
                            : L"Menu: help    Hold RIGHT TRIGGER: save and exit";
    g.DrawString(hint.c_str(), -1, &font, PointF(kCenter, py + 224), &center, ui.saveProgress > 0 ? &yellow : &dim);
    if (ui.saveProgress > 0) {
        SolidBrush bar(Color(255, 255, 220, 80));
        g.FillRectangle(&bar, px, py + 270, pw * ui.saveProgress, 10.0f);
    }

    // Help panel
    if (ui.help) {
        float hx = Px(-30), hy = 250, hw = Px(30) - hx, hh = 1230;
        g.FillRectangle(&panel, hx, hy, hw, hh);
        g.DrawRectangle(&border, hx, hy, hw, hh);
        const wchar_t* lines[] = {
            L"VIRTUAL PRISM - VR calibration",
            L"",
            L"Move each eye's image until the two images overlap as one.",
            L"(Text doubled? Press LEFT TRIGGER to look through one eye.)",
            L"",
            L"Right stick ....... move RIGHT eye image",
            L"Left stick ........ move LEFT eye image",
            L"A / Y ............. rotate clockwise (right / left eye)",
            L"B / X ............. rotate counter-clockwise (right / left eye)",
            L"Stick click ....... reset that eye to zero",
            L"Left trigger ...... both eyes / left only / right only",
            L"Left grip ......... next scene (last one: 3D room)",
            L"Right grip ........ step size: coarse / medium / fine",
            L"Menu button ....... show / hide this help",
            L"Right trigger ..... HOLD to save and exit",
            L"",
            L"Keep your head LEVEL and look straight ahead while aligning."};
        float y = hy + 26;
        for (int i = 0; i < 17; i++) {
            g.DrawString(lines[i], -1, i == 0 ? &big : &font, PointF(hx + 40, y), i == 0 ? &yellow : &white);
            y += 66;
        }
    }
    (void)eye;
}

}  // namespace

int SceneCount() { return (int)(sizeof(kSceneNames) / sizeof(kSceneNames[0])); }
const wchar_t* SceneName(int s) { return kSceneNames[s]; }
const wchar_t* StepName(int s) { return kStepNames[s]; }

bool InitScenes() {
    GdiplusStartupInput in;
    return GdiplusStartup(&g_gdiplusToken, &in, nullptr) == Ok;
}
void ShutdownScenes() {
    if (g_gdiplusToken) GdiplusShutdown(g_gdiplusToken);
    g_gdiplusToken = 0;
}

void RenderEyeImage(int eye, const UiState& ui, uint32_t* bgra) {
    Bitmap bmp(kImageSize, kImageSize, kImageSize * 4, PixelFormat32bppPARGB, (BYTE*)bgra);
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    // The 3D room is drawn separately; here the image is transparent except for the HUD.
    g.Clear(ui.scene == kRoomScene ? Color(0, 0, 0, 0) : Color(255, 0, 0, 0));

    bool coded = ui.scene == 0;
    Color col = EyeColor(eye, coded);
    switch (ui.scene) {
        case 0:
        case 1: DrawGrid(g, col, true); break;
        case 2: DrawDots(g, col); break;
        case 3: DrawTarget(g, col); break;
        case 4: DrawLines(g, col); break;
        case 5: DrawReading(g, col); break;
        default: break;
    }
    DrawHud(g, ui, eye);
}

}  // namespace dfx
