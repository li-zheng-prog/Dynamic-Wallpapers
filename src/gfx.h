// ============================================================================
//  gfx.h — 渲染层（D3D11 + DirectComposition + Direct2D + DWrite + WIC）
//
//  本文件只负责"把图画到窗口上"，不含任何业务逻辑。
//  用 DComp 交换链的原因：支持每像素透明（premultiplied alpha），这是圆角窗口
//  + 半透明层次 + 系统亚克力毛玻璃的基础。
//  DPI：坐标一律 DIP（96dpi 逻辑像素）。resize() 按窗口 DPI 调 SetDpi，
//  D2D 自动换算，200% 缩放下文字依然锐利、控件大小一致。
// ============================================================================
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <d3d11.h>
#include <dcomp.h>
#include <string>
#include <vector>

class Gfx {
public:
    // 创建 D3D11 设备 / D2D 上下文 / 交换链 / DComp 视觉树。
    // 必须在窗口创建之后、第一次绘制之前调用。
    bool init(HWND hwnd);

    // 尺寸与 DPI 变化：physicalW/H 为窗口客户区的物理像素大小
    void resize(UINT physicalW, UINT physicalH, UINT dpi);

    void shutdown();

    // 一帧绘制：begin() -> 用 dc() 画 -> end()
    bool begin();
    void end();

    // Present 的垂直同步间隔：1 = 等屏幕刷新（平时，省电无撕裂）；
    // 0 = 不等（全屏切换动画期间交给 DwmFlush 控节奏，否则一帧多耗 ~16ms）
    int presentSync = 1;

    ID2D1DeviceContext* dc() const { return dc_; }
    float scale() const { return dpi_ / 96.0f; }

    // 画刷 / 文本格式均缓存，避免每帧创建
    ID2D1SolidColorBrush* brush(const D2D1_COLOR_F& color);
    // family 可传逗号分隔的候选字体（前一个不存在时自动尝试下一个）。
    // 控件内文字的段落对齐一律用默认 NEAR + textOptical 墨迹居中。
    IDWriteTextFormat* format(const wchar_t* families, float dipSize,
                              DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL,
                              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
                              bool wrap = false);
    // 从文件（jpg/png/bmp）解码，maxWidthPx>0 时用 WIC 缩放以省内存
    ID2D1Bitmap* loadImage(const std::wstring& path, UINT maxWidthPx);
    // 从 exe 的 RCDATA 资源解码（标题栏 logo 不依赖外部文件）
    ID2D1Bitmap* loadImageFromResource(int resId, UINT maxWidthPx);
    // 从内存像素建位图（B8G8R8A8 预乘），毛玻璃的颗粒噪点用
    ID2D1Bitmap* createBitmapFromPixels(const void* premultipliedBGRA, UINT w, UINT h);

    // 便捷绘制工具（内部做 null 检查，失败静默）
    void fillRound(const D2D1_ROUNDED_RECT& r, const D2D1_COLOR_F& c);
    void strokeRound(const D2D1_ROUNDED_RECT& r, const D2D1_COLOR_F& c, float w = 1.0f);
    void text(const std::wstring& s, IDWriteTextFormat* fmt, const D2D1_RECT_F& r,
              const D2D1_COLOR_F& c);
    // 按墨迹（字形实际边界）而非行框做垂直居中——段落居中含字体下缘留白，
    // 中文/数字看着会偏上，所以控件内文字都用这个。
    void textOptical(const std::wstring& s, IDWriteTextFormat* fmt, const D2D1_RECT_F& r,
                     const D2D1_COLOR_F& c,
                     DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
    // 只测宽度（按墨迹宽度，用于并排文本块定位）
    float textWidth(const std::wstring& s, IDWriteTextFormat* fmt);
    // 裁剪（圆角图片等）
    void pushClip(const D2D1_ROUNDED_RECT& r);
    void popClip();

private:
    bool createTargetFromSwapChain();

    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    UINT w_ = 0, h_ = 0;            // 窗口当前大小（物理像素 = 缓冲的"可见区域"）
    UINT bufW_ = 0, bufH_ = 0;      // 交换链/渲染目标大小（一次开成整块显示器，动画期间不再重建）

    ID3D11Device* d3d_ = nullptr;
    IDXGISwapChain1* swap_ = nullptr;
    ID2D1Factory1* factory_ = nullptr;
    ID2D1Device* d2dDevice_ = nullptr;
    ID2D1DeviceContext* dc_ = nullptr;
    ID2D1Bitmap1* target_ = nullptr;
    IDCompositionDevice* dcomp_ = nullptr;
    IDCompositionTarget* dcompTarget_ = nullptr;
    IDCompositionVisual* visual_ = nullptr;
    IDWriteFactory* dwrite_ = nullptr;
    IWICImagingFactory* wic_ = nullptr;

    std::vector<std::pair<D2D1_COLOR_F, ID2D1SolidColorBrush*>> brushes_;
    struct FmtKey { std::wstring fam; float size; int weight; int align; bool wrap; };
    std::vector<std::pair<FmtKey, IDWriteTextFormat*>> formats_;
};
