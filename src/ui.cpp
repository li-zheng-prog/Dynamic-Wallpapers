// ============================================================================
//  ui.cpp — WinUI3 风格视觉令牌与控件绘制实现
// ============================================================================
#include "ui.h"
#include <d2d1_1.h>
#include <d2d1helper.h>
#include <dwmapi.h>
#include <cwchar>
#include <cmath>

D2D1_COLOR_F Col(float r, float g, float b, float a) {
    return D2D1::ColorF(r, g, b, a);
}
D2D1_COLOR_F ColHex(unsigned rgb, float a) {
    return D2D1::ColorF((float)((rgb >> 16) & 0xFF) / 255.0f,
                        (float)((rgb >> 8) & 0xFF) / 255.0f,
                        (float)(rgb & 0xFF) / 255.0f, a);
}
D2D1_COLOR_F ColAlpha(const D2D1_COLOR_F& c, float a) { return Col(c.r, c.g, c.b, a); }
D2D1_COLOR_F ColBlend(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
    return Col(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t,
               a.a + (b.a - a.a) * t);
}
D2D1_COLOR_F ColLighten(const D2D1_COLOR_F& c, float t) {
    return ColBlend(c, Col(1, 1, 1, c.a), t);
}

// ---------------------------------------------------------------------------
//  系统深浅色 / 强调色
//
//  Windows 11 把强调色连同完整色阶放在
//  HKCU\...\Explorer\Accent\AccentPalette（32 字节 = 8 组 RGBA：浅3/浅2/浅1/
//  基准/深1/深2/深3/其它）。不要用 DWM\AccentColor：那是"上色(colorization)"
//  值，比强调色暗一大截（实测强调色 #5A75A7，DWM 值 #113864），拿它当按钮
//  强调色会和系统 UI 明显不一致。拿不到 AccentPalette 时退回兜底色。
// ---------------------------------------------------------------------------
struct AccentRamp {
    D2D1_COLOR_F l3, l2, l1, base, d1, d2, d3;
    bool ok = false;
};
static AccentRamp SystemAccentRamp() {
    AccentRamp r;
    unsigned char buf[32] = {};
    DWORD sz = sizeof(buf), type = 0;
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                     L"AccentPalette", RRF_RT_REG_BINARY, &type, buf, &sz) != ERROR_SUCCESS ||
        sz < 28)
        return r;
    auto px = [&](int i) {
        return Col(buf[i * 4] / 255.0f, buf[i * 4 + 1] / 255.0f, buf[i * 4 + 2] / 255.0f, 1.0f);
    };
    r.l3 = px(0); r.l2 = px(1); r.l1 = px(2); r.base = px(3);
    r.d1 = px(4); r.d2 = px(5); r.d3 = px(6);
    r.ok = true;
    return r;
}

bool SystemUsesDarkMode() {
    DWORD v = 1, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS)
        return v == 0;
    return false;
}

D2D1_COLOR_F SystemAccentColor() {
    // DWM\AccentColor 格式 0xAABBGGRR
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor",
                     RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS && (v & 0x00FFFFFF)) {
        float r = (float)(v & 0xFF) / 255.0f;
        float g = (float)((v >> 8) & 0xFF) / 255.0f;
        float b = (float)((v >> 16) & 0xFF) / 255.0f;
        return Col(r, g, b, 1.0f);
    }
    DWORD color = 0; BOOL opaque = FALSE;
    if (SUCCEEDED(DwmGetColorizationColor(&color, &opaque)) && (color & 0x00FFFFFF)) {
        float b = (float)(color & 0xFF) / 255.0f;
        float g = (float)((color >> 8) & 0xFF) / 255.0f;
        float r = (float)((color >> 16) & 0xFF) / 255.0f;
        return Col(r, g, b, 1.0f);
    }
    return ColHex(0x0067C0);   // Windows 默认蓝
}

Palette MakePalette(bool dark, const D2D1_COLOR_F& accent) {
    Palette p;
    p.dark = dark;
    if (!dark) {
        p.windowBgTop    = ColHex(0xF3F3F3);   // Mica 浅色底
        p.windowBgBottom = ColHex(0xEDEDED);
        p.textPrimary    = Col(0, 0, 0, 0.89f);
        p.textSecondary  = Col(0, 0, 0, 0.61f);
        p.textTertiary   = Col(0, 0, 0, 0.45f);
        p.textDisabled   = Col(0, 0, 0, 0.36f);
        p.textOnAccent   = Col(1, 1, 1, 1.0f);
        p.cardFill       = Col(1, 1, 1, 0.70f);
        p.cardFillHover  = Col(1, 1, 1, 0.85f);
        p.cardStroke     = Col(0, 0, 0, 0.06f);
        // 按钮令牌跟随 Win11 的 ControlFill/ControlStroke，观感与系统一致
        p.ctrlFill       = Col(1, 1, 1, 0.70f);            // ControlFillColorDefault #B3FFFFFF
        p.ctrlFillHover  = Col(0.976f, 0.976f, 0.976f, 0.50f);  // ControlFillColorSecondary
        p.ctrlFillPressed= Col(0, 0, 0, 0.30f);            // ControlFillColorTertiary
        p.ctrlStroke     = Col(0, 0, 0, 0.06f);            // ControlStrokeColorDefault
        p.ctrlStrokeHover= Col(0, 0, 0, 0.16f);            // ControlStrokeColorSecondary
        p.toggleOffFill      = Col(0, 0, 0, 0.42f);
        p.toggleOffFillHover = Col(0, 0, 0, 0.55f);
        p.toggleOffKnob      = Col(1, 1, 1, 1.0f);
        p.divider        = Col(0, 0, 0, 0.08f);
        p.captionHover   = Col(0, 0, 0, 0.06f);
        p.shadowTint     = Col(0, 0, 0, 0.10f);
        p.dayDot         = ColHex(0xFFB900);
        p.nightDot       = ColHex(0x8A8AF0);
        p.closeHover     = ColHex(0xC42B1C);
    } else {
        p.windowBgTop    = ColHex(0x202020);
        p.windowBgBottom = ColHex(0x1A1A1A);
        p.textPrimary    = Col(1, 1, 1, 0.89f);
        p.textSecondary  = Col(1, 1, 1, 0.61f);
        p.textTertiary   = Col(1, 1, 1, 0.45f);
        p.textDisabled   = Col(1, 1, 1, 0.36f);
        p.textOnAccent   = Col(1, 1, 1, 1.0f);
        p.cardFill       = Col(1, 1, 1, 0.06f);
        p.cardFillHover  = Col(1, 1, 1, 0.10f);
        p.cardStroke     = Col(1, 1, 1, 0.09f);
        p.ctrlFill       = Col(1, 1, 1, 0.06f);            // #0FFFFFFF
        p.ctrlFillHover  = Col(1, 1, 1, 0.082f);           // #15FFFFFF
        p.ctrlFillPressed= Col(1, 1, 1, 0.039f);           // #0AFFFFFF
        p.ctrlStroke     = Col(1, 1, 1, 0.06f);
        p.ctrlStrokeHover= Col(1, 1, 1, 0.09f);
        p.toggleOffFill      = Col(1, 1, 1, 0.45f);
        p.toggleOffFillHover = Col(1, 1, 1, 0.58f);
        p.toggleOffKnob      = ColHex(0x1F1F1F);
        p.divider        = Col(1, 1, 1, 0.09f);
        p.captionHover   = Col(1, 1, 1, 0.07f);
        p.shadowTint     = Col(0, 0, 0, 0.30f);
        p.dayDot         = ColHex(0xFFC83D);
        p.nightDot       = ColHex(0x9C9CF5);
        p.closeHover     = ColHex(0xC42B1C);
    }
    // 强调色：浅色主题用 基准/深1/深2；深色主题基准色在暗底上发暗，改用浅色档
    AccentRamp ar = SystemAccentRamp();
    D2D1_COLOR_F acc = ar.ok ? ar.base : accent;
    if (!dark) {
        p.accent        = acc;
        p.accentHover   = ar.ok ? ar.d1 : ColBlend(acc, Col(0, 0, 0, 1.0f), 0.10f);
        p.accentPressed = ar.ok ? ar.d2 : ColBlend(acc, Col(0, 0, 0, 1.0f), 0.20f);
        p.accentText    = acc;
    } else {
        p.accent        = ar.ok ? ar.l1 : ColLighten(acc, 0.14f);
        p.accentHover   = ar.ok ? ar.l2 : ColLighten(acc, 0.26f);
        p.accentPressed = ar.ok ? ar.l3 : ColLighten(acc, 0.38f);
        p.accentText    = ar.ok ? ar.l2 : ColLighten(acc, 0.42f);
    }
    p.accentDisabled = ColAlpha(p.accent, 0.40f);
    p.cardStrokeSelected = p.accent;
    // 强调色上的文字按亮度取黑或白（系统也是按对比度选的）
    {
        float lum = 0.299f * p.accent.r + 0.587f * p.accent.g + 0.114f * p.accent.b;
        p.textOnAccent = lum > 0.62f ? Col(0, 0, 0, 0.89f) : Col(1, 1, 1, 1.0f);
    }
    return p;
}

// ---------------------------------------------------------------------------
//  基础控件
// ---------------------------------------------------------------------------
void UiSectionTitle(Gfx& g, float x, float y, float w, const std::wstring& title,
                    const Palette& p, bool divider) {
    IDWriteTextFormat* f = g.format(FONT_UI, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g.text(title, f, D2D1::RectF(x, y, x + w, y + 20), p.textPrimary);
    if (divider) {
        ID2D1SolidColorBrush* b = g.brush(p.divider);
        if (b) g.dc()->FillRectangle(D2D1::RectF(x, y + 26, x + w, y + 27), b);
    }
}

void UiCard(Gfx& g, const D2D1_RECT_F& r, const Palette& p, bool hovered, bool selected) {
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, Ui::RadiusCard, Ui::RadiusCard);
    g.fillRound(rr, hovered ? p.cardFillHover : p.cardFill);
    g.strokeRound(rr, selected ? p.cardStrokeSelected : p.cardStroke, selected ? 1.6f : 1.0f);
}

void UiButton(Gfx& g, const D2D1_RECT_F& r, const std::wstring& label, const Palette& p,
              bool primary, bool hovered, bool pressed, bool enabled, const wchar_t* iconGlyph) {
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, Ui::RadiusControl, Ui::RadiusControl);
    D2D1_COLOR_F fill, stroke, txt;
    if (primary) {
        fill = !enabled ? p.accentDisabled : (pressed ? p.accentPressed : (hovered ? p.accentHover : p.accent));
        stroke = Col(0, 0, 0, 0);
        txt = enabled ? p.textOnAccent : ColAlpha(p.textOnAccent, 0.5f);
    } else {
        // 禁用态不跟 hover/press 变色
        fill = !enabled ? p.ctrlFill
                        : (pressed ? p.ctrlFillPressed : (hovered ? p.ctrlFillHover : p.ctrlFill));
        stroke = (!enabled || !hovered) ? p.ctrlStroke : p.ctrlStrokeHover;
        txt = enabled ? p.textPrimary : p.textDisabled;
    }
    // 禁用态也要画：主按钮描边全透明，不画就只剩半透明的字
    g.fillRound(rr, fill);
    if (stroke.a > 0.001f) g.strokeRound(rr, stroke, 1.0f);
    else if (pressed) g.strokeRound(rr, p.divider, 1.0f);

    float textLeft = r.left, textRight = r.right;
    if (iconGlyph && *iconGlyph) {
        IDWriteTextFormat* gf = g.format(FONT_ICON, 14.0f, DWRITE_FONT_WEIGHT_NORMAL,
                                         DWRITE_TEXT_ALIGNMENT_CENTER);
        float gw = 22.0f;
        g.textOptical(iconGlyph, gf, D2D1::RectF(r.left + 10, r.top, r.left + 10 + gw, r.bottom),
                      txt, DWRITE_TEXT_ALIGNMENT_CENTER);
        textLeft += 10 + gw;
    }
    IDWriteTextFormat* f = g.format(FONT_UI, 13.5f, DWRITE_FONT_WEIGHT_NORMAL,
                                    DWRITE_TEXT_ALIGNMENT_CENTER);
    g.textOptical(label, f, D2D1::RectF(textLeft, r.top, textRight, r.bottom), txt,
                  DWRITE_TEXT_ALIGNMENT_CENTER);
}

void UiCaptionButton(Gfx& g, const D2D1_RECT_F& r, int mode, bool hovered, bool pressed,
                     const Palette& p) {
    bool isClose = (mode == Ui::CAP_CLOSE);
    if (hovered || pressed) {
        D2D1_COLOR_F c = isClose ? p.closeHover : p.captionHover;
        if (pressed) c = ColBlend(c, Col(0, 0, 0, 1.0f), 0.1f);
        g.dc()->FillRectangle(r, g.brush(c));
    }
    float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    float s = 5.0f;                              // 图标半径（10px 字形）
    ID2D1SolidColorBrush* b = g.brush(isClose && hovered ? Col(1, 1, 1) : p.textPrimary);
    if (!b) return;
    switch (mode) {
    case Ui::CAP_CLOSE:
        g.dc()->DrawLine(D2D1::Point2F(cx - s, cy - s), D2D1::Point2F(cx + s, cy + s), b, 1.0f);
        g.dc()->DrawLine(D2D1::Point2F(cx - s, cy + s), D2D1::Point2F(cx + s, cy - s), b, 1.0f);
        break;
    case Ui::CAP_MIN:
        g.dc()->DrawLine(D2D1::Point2F(cx - s, cy), D2D1::Point2F(cx + s, cy), b, 1.0f);
        break;
    case Ui::CAP_MAX:                            // 圆角方框（Win11 最大化图标形状）
        g.dc()->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(cx - s, cy - s + 0.5f, cx + s, cy + s - 0.5f), 1.5f, 1.5f),
            b, 1.1f);
        break;
    case Ui::CAP_RESTORE:                        // 两个错开的方框
        g.dc()->DrawRectangle(D2D1::RectF(cx - s, cy - s + 2, cx + s - 2, cy + s), b, 1.0f);
        g.dc()->DrawLine(D2D1::Point2F(cx - s + 2, cy - s), D2D1::Point2F(cx + s, cy - s), b, 1.0f);
        g.dc()->DrawLine(D2D1::Point2F(cx + s, cy - s), D2D1::Point2F(cx + s, cy + s - 2), b, 1.0f);
        break;
    default:
        break;
    }
}

void UiToggle(Gfx& g, float x, float y, bool on, float animT, bool hovered, const Palette& p) {
    (void)on;                                    // 画面上一切看 animT，on 只用于配置判断
    float w = Ui::ToggleW, h = Ui::ToggleH;
    float t = animT;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    D2D1_ROUNDED_RECT track = D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), h / 2, h / 2);
    D2D1_COLOR_F onFill  = hovered ? p.accentHover : p.accent;
    D2D1_COLOR_F offFill = hovered ? p.toggleOffFillHover : p.toggleOffFill;
    g.fillRound(track, ColBlend(offFill, onFill, t));    // 轨道：关→开 颜色渐变

    // 滑块：位置与颜色同步插值（关=深/浅色，开=反色白）
    float knobR = Ui::ToggleKnob / 2;
    float inset = (h - Ui::ToggleKnob) / 2 + 1.0f;
    float left = x + inset + knobR, right = x + w - inset - knobR;
    float cx = left + (right - left) * t;
    g.dc()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, y + h / 2), knobR, knobR),
                        g.brush(ColBlend(p.toggleOffKnob, p.textOnAccent, t)));
    // 关闭态滑块的一圈描边：随 t 淡出（浅色模式下才明显）
    float ringA = (1.0f - t) * (p.dark ? 0.22f : 0.10f);
    if (ringA > 0.004f) {
        D2D1_COLOR_F ring = Col(0, 0, 0, ringA);
        if (p.dark) ring = Col(1, 1, 1, ringA);
        ID2D1SolidColorBrush* b = g.brush(ring);
        if (b) g.dc()->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, y + h / 2), knobR, knobR), b, 1.0f);
    }
}

void UiCheckbox(Gfx& g, float x, float y, const std::wstring& label, bool checked,
                bool hovered, const Palette& p) {
    const float box = 20.0f;
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF(x, y, x + box, y + box), 4, 4);
    if (checked) {
        g.fillRound(rr, hovered ? p.accentHover : p.accent);
    } else {
        g.fillRound(rr, hovered ? p.ctrlFillHover : p.ctrlFill);
        g.strokeRound(rr, hovered ? p.ctrlStrokeHover : p.ctrlStroke, 1.0f);
    }
    if (checked) {
        IDWriteTextFormat* gf = g.format(FONT_ICON, 12.0f, DWRITE_FONT_WEIGHT_NORMAL,
                                         DWRITE_TEXT_ALIGNMENT_CENTER);
        g.textOptical(L"\uE73E", gf, D2D1::RectF(x, y, x + box, y + box), p.textOnAccent,
                      DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    IDWriteTextFormat* f = g.format(FONT_UI, 13.5f);
    g.textOptical(label, f, D2D1::RectF(x + box + 10, y, x + box + 400, y + box), p.textPrimary);
}

void UiStepper(Gfx& g, const D2D1_RECT_F& r, const std::wstring& value, const Palette& p,
               int hoverPart, int pressPart) {
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, Ui::RadiusControl, Ui::RadiusControl);
    g.fillRound(rr, p.ctrlFill);
    g.strokeRound(rr, p.ctrlStroke, 1.0f);
    float h = r.bottom - r.top;
    float btnW = h;   // 左右各一个正方形按钮
    // 分隔线
    ID2D1SolidColorBrush* line = g.brush(p.divider);
    if (line) {
        g.dc()->FillRectangle(D2D1::RectF(r.left + btnW, r.top + 6, r.left + btnW + 1, r.bottom - 6), line);
        g.dc()->FillRectangle(D2D1::RectF(r.right - btnW - 1, r.top + 6, r.right - btnW, r.bottom - 6), line);
    }
    auto seg = [&](int part) {                 // part: 0=减 1=加
        float x0 = part == 0 ? r.left : r.right - btnW;
        if (hoverPart != part && pressPart != part) return;
        D2D1_ROUNDED_RECT s = D2D1::RoundedRect(
            D2D1::RectF(x0 + 1, r.top + 1, x0 + btnW - 1, r.bottom - 1), 3, 3);
        g.fillRound(s, pressPart == part ? p.ctrlFillPressed : p.ctrlFillHover);
    };
    seg(0); seg(1);
    ID2D1SolidColorBrush* b = g.brush(p.textPrimary);
    if (b) {
        float cy = (r.top + r.bottom) / 2, s = 5.0f;
        float mx = r.left + btnW / 2;          // 减号
        g.dc()->DrawLine(D2D1::Point2F(mx - s, cy), D2D1::Point2F(mx + s, cy), b, 1.2f);
        float px = r.right - btnW / 2;         // 加号
        g.dc()->DrawLine(D2D1::Point2F(px - s, cy), D2D1::Point2F(px + s, cy), b, 1.2f);
        g.dc()->DrawLine(D2D1::Point2F(px, cy - s), D2D1::Point2F(px, cy + s), b, 1.2f);
    }
    IDWriteTextFormat* f = g.format(FONT_UI, 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                    DWRITE_TEXT_ALIGNMENT_CENTER);
    g.textOptical(value, f, D2D1::RectF(r.left + btnW, r.top, r.right - btnW, r.bottom),
                  p.textPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
}

// ---------------------------------------------------------------------------
//  主题卡片（删/改按钮的命中矩形与绘制位置同源，尺寸集中在这里）
// ---------------------------------------------------------------------------
static const float kCardPad = 6.0f;
static const float kCardThumbH = 96.0f;
static const float kDelRad = 10.0f;      // 右上角圆形按钮的半径

static D2D1_RECT_F CardThumbRect(const D2D1_RECT_F& r) {
    return D2D1::RectF(r.left + kCardPad, r.top + kCardPad, r.right - kCardPad,
                       r.top + kCardPad + kCardThumbH);
}

D2D1_RECT_F UiThemeCardDeleteRect(const D2D1_RECT_F& card) {
    D2D1_RECT_F tr = CardThumbRect(card);
    float cxc = tr.right - 13, cyc = tr.top + 13;
    return D2D1::RectF(cxc - kDelRad, cyc - kDelRad, cxc + kDelRad, cyc + kDelRad);
}

D2D1_RECT_F UiThemeCardRenameRect(const D2D1_RECT_F& card) {
    D2D1_RECT_F dr = UiThemeCardDeleteRect(card);
    float shift = kDelRad * 2 + 6;                 // 与"×"留 6 DIP 间隔
    return D2D1::RectF(dr.left - shift, dr.top, dr.right - shift, dr.bottom);
}

void UiThemeCard(Gfx& g, const D2D1_RECT_F& r, ID2D1Bitmap* thumb, const std::wstring& name,
                 const std::wstring& sub, bool selected, bool hovered, const Palette& p,
                 bool showDelete, bool delHovered, bool renHovered) {
    UiCard(g, r, p, hovered, selected);
    D2D1_RECT_F tr = CardThumbRect(r);
    D2D1_ROUNDED_RECT trr = D2D1::RoundedRect(tr, 5, 5);
    if (thumb) {
        g.pushClip(trr);
        // 等比铺满（cover）：按位图宽高比裁剪
        D2D1_SIZE_F sz = thumb->GetSize();
        float tw = tr.right - tr.left, th = tr.bottom - tr.top;
        float scale = (sz.width / sz.height > tw / th) ? (th / sz.height) : (tw / sz.width);
        float dw = sz.width * scale, dh = sz.height * scale;
        D2D1_RECT_F dst = D2D1::RectF(tr.left + (tw - dw) / 2, tr.top + (th - dh) / 2,
                                      tr.left + (tw - dw) / 2 + dw, tr.top + (th - dh) / 2 + dh);
        g.dc()->DrawBitmap(thumb, dst, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        g.popClip();
        g.strokeRound(trr, ColAlpha(p.textPrimary, 0.08f), 1.0f);
    } else {
        g.fillRound(trr, ColAlpha(p.textPrimary, 0.05f));
    }
    // 选中徽标：左上角圆形 + 对勾（右上角留给删除/重命名按钮）
    if (selected) {
        float cxc = tr.left + 13, cyc = tr.top + 13, rad = 10.0f;
        g.dc()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cxc, cyc), rad, rad), g.brush(p.accent));
        IDWriteTextFormat* gf = g.format(FONT_ICON, 10.5f, DWRITE_FONT_WEIGHT_NORMAL,
                                         DWRITE_TEXT_ALIGNMENT_CENTER);
        g.textOptical(L"\uE73E", gf, D2D1::RectF(cxc - rad, cyc - rad, cxc + rad, cyc + rad),
                      p.textOnAccent, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    if (showDelete) {
        // "✎"（在"×"左边）：平时的底是半透明黑，悬停时用强调色
        D2D1_RECT_F nr = UiThemeCardRenameRect(r);
        float nxc = (nr.left + nr.right) / 2, nyc = (nr.top + nr.bottom) / 2;
        g.dc()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(nxc, nyc), kDelRad, kDelRad),
                            g.brush(renHovered ? p.accent : Col(0, 0, 0, 0.55f)));
        IDWriteTextFormat* gf = g.format(FONT_ICON, 11.0f, DWRITE_FONT_WEIGHT_NORMAL,
                                         DWRITE_TEXT_ALIGNMENT_CENTER);
        g.textOptical(L"\uE70F", gf, D2D1::RectF(nr.left, nr.top, nr.right, nr.bottom),
                      Col(1, 1, 1, 1), DWRITE_TEXT_ALIGNMENT_CENTER);

        // "×"：平时半透明黑（压住缩略图），悬停时用危险色；两条线画，比字形锐利
        D2D1_RECT_F dr = UiThemeCardDeleteRect(r);
        float cxc = (dr.left + dr.right) / 2, cyc = (dr.top + dr.bottom) / 2;
        g.dc()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cxc, cyc), kDelRad, kDelRad),
                            g.brush(delHovered ? ColHex(0xC42B1C) : Col(0, 0, 0, 0.55f)));
        float s = 4.2f;
        D2D1_COLOR_F ink = Col(1, 1, 1, 1);
        g.dc()->DrawLine(D2D1::Point2F(cxc - s, cyc - s), D2D1::Point2F(cxc + s, cyc + s),
                         g.brush(ink), 1.5f);
        g.dc()->DrawLine(D2D1::Point2F(cxc + s, cyc - s), D2D1::Point2F(cxc - s, cyc + s),
                         g.brush(ink), 1.5f);
    }
    IDWriteTextFormat* f = g.format(FONT_UI, 13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                    DWRITE_TEXT_ALIGNMENT_CENTER);
    g.textOptical(name, f, D2D1::RectF(r.left + 4, tr.bottom + 4, r.right - 4, tr.bottom + 24),
                  p.textPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
    if (!sub.empty()) {
        IDWriteTextFormat* f2 = g.format(FONT_UI, 11.0f, DWRITE_FONT_WEIGHT_NORMAL,
                                         DWRITE_TEXT_ALIGNMENT_CENTER);
        g.textOptical(sub, f2, D2D1::RectF(r.left + 4, tr.bottom + 24, r.right - 4, tr.bottom + 42),
                      p.textSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

void UiProgress(Gfx& g, const D2D1_RECT_F& r, float progress, const Palette& p) {
    float rad = (r.bottom - r.top) / 2;
    D2D1_ROUNDED_RECT track = D2D1::RoundedRect(r, rad, rad);
    g.fillRound(track, ColAlpha(p.textPrimary, 0.12f));
    float w = (r.right - r.left) * (progress < 0 ? 0 : (progress > 1 ? 1 : progress));
    if (w > 1.0f) {
        D2D1_ROUNDED_RECT fill = D2D1::RoundedRect(D2D1::RectF(r.left, r.top, r.left + w, r.bottom), rad, rad);
        g.fillRound(fill, p.accent);
    }
}

void UiStatusDot(Gfx& g, float cx, float cy, bool night, const Palette& p) {
    ID2D1SolidColorBrush* b = g.brush(night ? p.nightDot : p.dayDot);
    if (b) g.dc()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 5, 5), b);
}

// ---------------------------------------------------------------------------
//  文字按钮 / 分段控件 / 遮罩
// ---------------------------------------------------------------------------
void UiTextButton(Gfx& g, const D2D1_RECT_F& r, const std::wstring& label, const Palette& p,
                  bool hovered, bool pressed, const wchar_t* glyph, DWRITE_TEXT_ALIGNMENT align) {
    if (hovered || pressed) {
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, 4, 4);
        g.fillRound(rr, pressed ? ColAlpha(p.accent, 0.22f) : ColAlpha(p.accent, 0.14f));
    }
    IDWriteTextFormat* f = g.format(FONT_UI, 13.0f);
    IDWriteTextFormat* gf = g.format(FONT_ICON, 12.0f);
    float textW = g.textWidth(label, f);
    float glyphW = (glyph && *glyph) ? 20.0f : 0.0f;
    float totalW = textW + glyphW;
    float startX = (align == DWRITE_TEXT_ALIGNMENT_TRAILING) ? (r.right - totalW - 8)
                                                             : (r.left + 8);
    D2D1_COLOR_F c = p.accentText;
    if (glyph && *glyph)
        g.textOptical(glyph, gf, D2D1::RectF(startX, r.top, startX + glyphW, r.bottom), c,
                      DWRITE_TEXT_ALIGNMENT_CENTER);
    g.textOptical(label, f, D2D1::RectF(startX + glyphW, r.top, startX + glyphW + textW + 4,
                                        r.bottom), c);
}

// 两个矩形在水平方向上的重叠宽度（算"指示条盖住某段多少"用）
static float OverlapX(const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
    float l = a.left > b.left ? a.left : b.left;
    float r = a.right < b.right ? a.right : b.right;
    return r > l ? r - l : 0.0f;
}

void UiSegmented(Gfx& g, const D2D1_RECT_F& r, const std::vector<std::wstring>& items,
                 int selected, int hotSeg, const Palette& p, float anim, float pressScale) {
    if (items.empty()) return;
    const int n = (int)items.size();
    if (anim < 0.0f) anim = (float)selected;              // 不传 anim = 静止在 selected
    if (anim > (float)(n - 1)) anim = (float)(n - 1);
    if (anim < 0.0f) anim = 0.0f;
    if (pressScale <= 0.0f) pressScale = 1.0f;

    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, Ui::RadiusControl, Ui::RadiusControl);
    g.fillRound(rr, p.ctrlFill);
    g.strokeRound(rr, p.ctrlStroke, 1.0f);

    const float segW = (r.right - r.left) / (float)n;
    // 任意浮点位置的段矩形（与整数段同一套内缩，指示条滑动时不跳变）
    auto segRect = [&](float idx) {
        return D2D1::RectF(r.left + idx * segW + 2, r.top + 2,
                           r.left + (idx + 1) * segW - 2, r.bottom - 2);
    };

    // 悬停底色：只画在"没被指示条占据"的段上
    for (int i = 0; i < n; ++i) {
        if (i == hotSeg && fabsf(anim - (float)i) > 0.02f)
            g.fillRound(D2D1::RoundedRect(segRect((float)i), 3, 3), ColAlpha(p.textPrimary, 0.06f));
    }

    // 指示条：移动中按剩余距离左右各鼓一点，形成"拉伸"手感；按下围绕中心缩小
    D2D1_RECT_F pk = segRect(anim);
    float travel = fabsf((float)selected - anim);
    float stretch = 6.0f * (travel > 1.0f ? 1.0f : travel);
    D2D1_RECT_F pill = D2D1::RectF(pk.left - stretch, pk.top, pk.right + stretch, pk.bottom);
    float cx = (pill.left + pill.right) / 2, cy = (pill.top + pill.bottom) / 2;
    if (pressScale < 0.999f) {
        float hw = (pill.right - pill.left) / 2 * pressScale;
        float hh = (pill.bottom - pill.top) / 2 * pressScale;
        pill = D2D1::RectF(cx - hw, cy - hh, cx + hw, cy + hh);
    }
    g.fillRound(D2D1::RoundedRect(pill, 3, 3), p.accent);

    // 文字颜色按"指示条覆盖该段的程度"在 主文字色 ↔ 反色 之间渐变
    for (int i = 0; i < n; ++i) {
        D2D1_RECT_F seg = segRect((float)i);
        float cov = OverlapX(pill, seg) / (seg.right - seg.left);
        if (cov < 0.0f) cov = 0.0f;
        if (cov > 1.0f) cov = 1.0f;
        cov = cov * cov * (3.0f - 2.0f * cov);                      // smoothstep
        D2D1_COLOR_F tc = ColBlend(p.textPrimary, p.textOnAccent, cov);
        IDWriteTextFormat* f = g.format(FONT_UI, 12.5f,
                                        i == selected ? DWRITE_FONT_WEIGHT_SEMI_BOLD
                                                      : DWRITE_FONT_WEIGHT_NORMAL,
                                        DWRITE_TEXT_ALIGNMENT_CENTER);
        g.textOptical(items[i], f, seg, tc, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

void UiDim(Gfx& g, float w, float h, const Palette& p) {
    ID2D1SolidColorBrush* b = g.brush(Col(0, 0, 0, p.dark ? 0.45f : 0.28f));
    if (b) g.dc()->FillRectangle(D2D1::RectF(0, 0, w, h), b);
}
