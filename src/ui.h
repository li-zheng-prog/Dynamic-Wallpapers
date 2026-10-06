// ============================================================================
//  ui.h — WinUI3 风格视觉令牌与控件绘制
//
//  不依赖 Windows App SDK，纯 D2D 自绘出 WinUI3 的观感：圆角(4/8) + 半透明
//  层次 + 强调色 + Segoe 字体 + 灰度抗锯齿文字。主程序与安装程序共用。
//  改颜色/改尺寸：所有数值集中在 MakePalette() 与本文件的常量里。
// ============================================================================
#pragma once
#include "gfx.h"
#include <string>

// 字体族（逗号分隔表示候选顺序；找不到时自动用下一个）
#define FONT_UI      L"Microsoft YaHei UI,Segoe UI Variable Text,Segoe UI"
#define FONT_ICON    L"Segoe MDL2 Assets"

// ---- 尺寸常量（DIP）----
namespace Ui {
    const float RadiusCard    = 8.0f;
    const float RadiusControl = 4.0f;
    const float ToggleW       = 40.0f;
    const float ToggleH       = 20.0f;
    const float ToggleKnob    = 12.0f;
    const float ButtonH       = 34.0f;
    const float TitleBarH     = 40.0f;
}

// 颜色令牌：WinUI3 主题色板近似值（sRGB + alpha，与 DIP 无关）
struct Palette {
    bool dark = false;
    D2D1_COLOR_F windowBgTop, windowBgBottom;   // 窗口底（Mica 风格渐变）
    D2D1_COLOR_F textPrimary, textSecondary, textTertiary, textOnAccent, textDisabled;
    D2D1_COLOR_F cardFill, cardFillHover, cardStroke, cardStrokeSelected;
    D2D1_COLOR_F ctrlFill, ctrlFillHover, ctrlFillPressed, ctrlStroke, ctrlStrokeHover;
    D2D1_COLOR_F accent, accentHover, accentPressed, accentDisabled;
    D2D1_COLOR_F toggleOffFill, toggleOffFillHover, toggleOffKnob;
    D2D1_COLOR_F divider, closeHover, captionHover;
    D2D1_COLOR_F shadowTint;
    D2D1_COLOR_F nightDot, dayDot;      // 状态圆点（夜=蓝紫，日=金黄）
    D2D1_COLOR_F accentText;            // 强调色文字（深色模式下要提亮，否则对比度不足）
};

// 由"深/浅色 + 强调色"生成整套令牌。accent 为系统强调色。
Palette MakePalette(bool dark, const D2D1_COLOR_F& accent);

// 从系统读取：是否深色模式 / 系统强调色
bool SystemUsesDarkMode();
D2D1_COLOR_F SystemAccentColor();

// ---- 颜色工具 ----
D2D1_COLOR_F Col(float r, float g, float b, float a = 1.0f);
D2D1_COLOR_F ColHex(unsigned rgb, float a = 1.0f);        // 0xRRGGBB
D2D1_COLOR_F ColAlpha(const D2D1_COLOR_F& c, float a);    // 改透明度
D2D1_COLOR_F ColBlend(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t);
D2D1_COLOR_F ColLighten(const D2D1_COLOR_F& c, float t);  // 向白混合

// ---- 控件（矩形均为 DIP；hover/pressed 由调用方根据鼠标命中传入）----
void UiSectionTitle(Gfx& g, float x, float y, float w, const std::wstring& title,
                    const Palette& p, bool divider = true);
void UiButton(Gfx& g, const D2D1_RECT_F& r, const std::wstring& label, const Palette& p,
              bool primary, bool hovered, bool pressed, bool enabled = true,
              const wchar_t* iconGlyph = nullptr);
// 标题栏按钮模式：最小化 / 最大化 / 还原 / 关闭
namespace Ui {
    enum CaptionBtn { CAP_MIN = 0, CAP_CLOSE = 1, CAP_MAX = 2, CAP_RESTORE = 3 };
}
void UiCaptionButton(Gfx& g, const D2D1_RECT_F& r, int mode, bool hovered, bool pressed,
                     const Palette& p);
// 开关：animT = 当前插值位置（0=关 1=开），轨道/滑块位置/滑块颜色都按它插值
void UiToggle(Gfx& g, float x, float y, bool on, float animT, bool hovered, const Palette& p);
void UiCheckbox(Gfx& g, float x, float y, const std::wstring& label, bool checked,
                bool hovered, const Palette& p);
void UiStepper(Gfx& g, const D2D1_RECT_F& r, const std::wstring& value, const Palette& p,
               int hoverPart, int pressPart);      // hoverPart: -1无 /0减 /1加
void UiThemeCard(Gfx& g, const D2D1_RECT_F& r, ID2D1Bitmap* thumb, const std::wstring& name,
                 const std::wstring& sub, bool selected, bool hovered, const Palette& p,
                 bool showDelete = false, bool delHovered = false, bool renHovered = false);
// 主题卡片右上角"×"/"✎"的命中矩形（与 UiThemeCard 内部绘制位置同源）
D2D1_RECT_F UiThemeCardDeleteRect(const D2D1_RECT_F& card);
D2D1_RECT_F UiThemeCardRenameRect(const D2D1_RECT_F& card);
void UiCard(Gfx& g, const D2D1_RECT_F& r, const Palette& p, bool hovered = false,
            bool selected = false);
void UiProgress(Gfx& g, const D2D1_RECT_F& r, float progress, const Palette& p);
void UiStatusDot(Gfx& g, float cx, float cy, bool night, const Palette& p);
// 文字按钮：强调色文字的次级入口（如"添加主题"），可选前置图标字形
void UiTextButton(Gfx& g, const D2D1_RECT_F& r, const std::wstring& label, const Palette& p,
                  bool hovered, bool pressed, const wchar_t* glyph = nullptr,
                  DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_TRAILING);
// 分段控件：所有段等宽，选中段用强调色填充。
//   anim       指示条当前位置（浮点，0..items-1），静止时等于 selected
//   pressScale 按下时的缩放比例（1.0 = 常态），围绕指示条中心缩放
void UiSegmented(Gfx& g, const D2D1_RECT_F& r, const std::vector<std::wstring>& items,
                 int selected, int hotSeg, const Palette& p, float anim = -1.0f,
                 float pressScale = 1.0f);
// 模态遮罩：在整窗上盖一层暗色（弹层背景）
void UiDim(Gfx& g, float w, float h, const Palette& p);
