// ============================================================================
//  gfx.cpp — 渲染层实现
// ============================================================================
#include "gfx.h"
#include <wincodec.h>
#include <d2d1helper.h>
#include <algorithm>

// 兼容旧版 SDK：这些 DWM 属性/常量在 MinGW 老头文件里可能没有
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

// DComp 的 IID（MinGW 头文件未导出该符号，这里自行声明）
static const GUID kIID_IDCompositionDevice =
    {0xc37ea93a, 0xe7aa, 0x450d, {0xb1, 0x6f, 0x97, 0x46, 0xcb, 0x04, 0x07, 0xf3}};

static bool ColorEq(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

bool Gfx::init(HWND hwnd) {
    hwnd_ = hwnd;

    // 失败统一走 bail：局部 DXGI 对象和已建好的成员一起放干净，不留半初始化状态
    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* dxgiFactory = nullptr;
    auto bail = [&]() {
        if (dxgiFactory) dxgiFactory->Release();
        if (adapter) adapter->Release();
        if (dxgiDevice) dxgiDevice->Release();
        shutdown();
        return false;
    };

    // ---- D3D11 设备（BGRA 支持是 D2D 互操作的前提）----
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL level;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                 nullptr, 0, D3D11_SDK_VERSION, &d3d_, &level, nullptr)))
        return bail();

    if (FAILED(d3d_->QueryInterface(IID_IDXGIDevice, (void**)&dxgiDevice))) return bail();

    // ---- D2D / DWrite / WIC ----
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                 IID_ID2D1Factory1, nullptr, (void**)&factory_)))
        return bail();
    if (FAILED(factory_->CreateDevice(dxgiDevice, &d2dDevice_)))
        return bail();
    if (FAILED(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_)))
        return bail();
    // 透明底上用灰度抗锯齿，避免 ClearType 出现彩边
    dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dc_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   (IUnknown**)&dwrite_)))
        return bail();
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     __uuidof(IWICImagingFactory), (void**)&wic_);

    // ---- 交换链（组合用交换链，支持预乘透明）----
    if (FAILED(dxgiDevice->GetAdapter(&adapter)) || !adapter) return bail();
    if (FAILED(adapter->GetParent(IID_IDXGIFactory2, (void**)&dxgiFactory)) || !dxgiFactory)
        return bail();

    RECT rc; GetClientRect(hwnd_, &rc);
    w_ = (UINT)(rc.right - rc.left);
    h_ = (UINT)(rc.bottom - rc.top);
    if (w_ == 0) w_ = 1;
    if (h_ == 0) h_ = 1;

    // 交换链一次开成"整块显示器"大小：窗口在动画中逐帧变大变小时只改可见区域、
    // 不重建缓冲。每帧 ResizeBuffers + 重建 D2D 目标实测要 4~13ms，120Hz 屏上
    // 一帧吃掉两个刷新周期。窗口本身被 DWM 裁到自己的尺寸，多出的缓冲看不到。
    bufW_ = w_; bufH_ = h_;
    MONITORINFO mi; mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi)) {
        UINT monW = (UINT)(mi.rcMonitor.right - mi.rcMonitor.left);
        UINT monH = (UINT)(mi.rcMonitor.bottom - mi.rcMonitor.top);
        if (monW > bufW_) bufW_ = monW;
        if (monH > bufH_) bufH_ = monH;
    }

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = bufW_;
    sd.Height = bufH_;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    if (FAILED(dxgiFactory->CreateSwapChainForComposition(d3d_, &sd, nullptr, &swap_)))
        return bail();

    // ---- DComp 视觉树：交换链挂到窗口上 ----
    if (FAILED(DCompositionCreateDevice(dxgiDevice, kIID_IDCompositionDevice, (void**)&dcomp_)))
        return bail();
    if (FAILED(dcomp_->CreateTargetForHwnd(hwnd, TRUE, &dcompTarget_))) return bail();
    if (FAILED(dcomp_->CreateVisual(&visual_))) return bail();
    visual_->SetContent(swap_);
    dcompTarget_->SetRoot(visual_);
    dcomp_->Commit();

    if (!createTargetFromSwapChain()) return bail();

    dxgiFactory->Release();
    adapter->Release();
    dxgiDevice->Release();
    return true;
}

bool Gfx::createTargetFromSwapChain() {
    IDXGISurface* surface = nullptr;
    if (FAILED(swap_->GetBuffer(0, IID_IDXGISurface, (void**)&surface))) return false;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        (float)dpi_, (float)dpi_);
    HRESULT hr = dc_->CreateBitmapFromDxgiSurface(surface, &bp, &target_);
    surface->Release();
    if (FAILED(hr)) return false;
    dc_->SetTarget(target_);
    return true;
}

void Gfx::resize(UINT physicalW, UINT physicalH, UINT dpi) {
    if (!swap_) return;
    if (physicalW == 0) physicalW = 1;
    if (physicalH == 0) physicalH = 1;
    if (physicalW == w_ && physicalH == h_ && dpi == dpi_) return;

    bool dpiChanged = (dpi != dpi_);
    w_ = physicalW; h_ = physicalH;

    // 正常情况下这里只记下窗口大小：缓冲是整块显示器尺寸，窗口 = 缓冲的可见
    // 区域（左上角），逐帧改窗口大小不碰 DXGI。只有窗口长到超出缓冲（换显示器）
    // 或 DPI 变了才真的重建一次。
    if (dpiChanged || w_ > bufW_ || h_ > bufH_) {
        UINT nw = (w_ > bufW_) ? w_ : bufW_;
        UINT nh = (h_ > bufH_) ? h_ : bufH_;
        dpi_ = dpi;
        if (target_) { dc_->SetTarget(nullptr); target_->Release(); target_ = nullptr; }
        if (SUCCEEDED(swap_->ResizeBuffers(0, nw, nh, DXGI_FORMAT_UNKNOWN, 0))) {
            bufW_ = nw; bufH_ = nh;
        }
        // ResizeBuffers 失败（设备被移除等）也要把目标位图建回来：旧缓冲还在，画面
        // 暂时不跟窗口大小走，好过 target_ 空着、begin() 从此失败、黑屏到重启
        createTargetFromSwapChain();
        // 之后所有绘制坐标都是 DIP
        dc_->SetDpi((float)dpi_, (float)dpi_);
    }
}

void Gfx::shutdown() {
    for (auto& b : brushes_) if (b.second) b.second->Release();
    brushes_.clear();
    for (auto& f : formats_) if (f.second) f.second->Release();
    formats_.clear();
    if (target_) { target_->Release(); target_ = nullptr; }
    if (visual_) { visual_->Release(); visual_ = nullptr; }
    if (dcompTarget_) { dcompTarget_->Release(); dcompTarget_ = nullptr; }
    if (dcomp_) { dcomp_->Release(); dcomp_ = nullptr; }
    if (wic_) { wic_->Release(); wic_ = nullptr; }
    if (dwrite_) { dwrite_->Release(); dwrite_ = nullptr; }
    if (dc_) { dc_->Release(); dc_ = nullptr; }
    if (d2dDevice_) { d2dDevice_->Release(); d2dDevice_ = nullptr; }
    if (factory_) { factory_->Release(); factory_ = nullptr; }
    if (swap_) { swap_->Release(); swap_ = nullptr; }
    if (d3d_) { d3d_->Release(); d3d_ = nullptr; }
}

bool Gfx::begin() {
    if (!dc_ || !target_) return false;
    dc_->BeginDraw();
    dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    return true;
}

void Gfx::end() {
    HRESULT hr = dc_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        // 设备丢失：重建目标位图（罕见，但必须处理）
        if (target_) { dc_->SetTarget(nullptr); target_->Release(); target_ = nullptr; }
        createTargetFromSwapChain();
        return;
    }
    DXGI_PRESENT_PARAMETERS pp = {};
    swap_->Present1(presentSync, 0, &pp);
}

ID2D1SolidColorBrush* Gfx::brush(const D2D1_COLOR_F& color) {
    for (auto& b : brushes_)
        if (ColorEq(b.first, color)) return b.second;
    ID2D1SolidColorBrush* b = nullptr;
    if (SUCCEEDED(dc_->CreateSolidColorBrush(color, &b))) {
        brushes_.push_back({color, b});
        if (brushes_.size() > 64) {   // 缓存上限，防止无限增长
            brushes_.front().second->Release();
            brushes_.erase(brushes_.begin());
        }
    }
    return b;
}

IDWriteTextFormat* Gfx::format(const wchar_t* families, float dipSize,
                               DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align,
                               bool wrap) {
    FmtKey key{families, dipSize, (int)weight, (int)align, wrap};
    for (auto& f : formats_)
        if (f.first.fam == key.fam && f.first.size == key.size && f.first.weight == key.weight &&
            f.first.align == key.align && f.first.wrap == key.wrap)
            return f.second;

    // families 支持 "A,B,C"：逐个尝试，全部失败则交给 DWrite 兜底
    std::wstring list = families;
    IDWriteTextFormat* fmt = nullptr;
    size_t start = 0;
    while (true) {
        size_t comma = list.find(L',', start);
        std::wstring one = list.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start);
        if (!one.empty() &&
            SUCCEEDED(dwrite_->CreateTextFormat(one.c_str(), nullptr, weight,
                                                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                dipSize, L"zh-cn", &fmt)))
            break;
        if (comma == std::wstring::npos) break;
        start = comma + 1;
    }
    if (!fmt) return nullptr;
    fmt->SetTextAlignment(align);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    fmt->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    // 关闭行距微调，Windows 11 文字排版默认值
    fmt->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_DEFAULT, 0, 0);
    formats_.push_back({key, fmt});
    return fmt;
}

// 把已打开的解码器变成 D2D 位图（从文件 / 从 exe 资源两条入口共用）
static ID2D1Bitmap* BitmapFromDecoder(IWICImagingFactory* wic, ID2D1DeviceContext* dc,
                                       IWICBitmapDecoder* dec, UINT maxWidthPx) {
    IWICBitmapFrameDecode* frame = nullptr;
    if (FAILED(dec->GetFrame(0, &frame))) return nullptr;

    IWICBitmapSource* src = frame;
    IWICBitmapScaler* scaler = nullptr;
    if (maxWidthPx > 0) {
        UINT w = 0, h = 0;
        frame->GetSize(&w, &h);
        if (w > maxWidthPx) {
            if (SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
                SUCCEEDED(scaler->Initialize(frame, maxWidthPx, maxWidthPx * h / w,
                                             WICBitmapInterpolationModeFant)))
                src = scaler;
        }
    }
    IWICFormatConverter* conv = nullptr;
    ID2D1Bitmap* bmp = nullptr;
    if (SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                   nullptr, 0.0, WICBitmapPaletteTypeMedianCut)))
        dc->CreateBitmapFromWicBitmap(conv, nullptr, &bmp);
    if (conv) conv->Release();
    if (scaler) scaler->Release();
    frame->Release();
    return bmp;
}

ID2D1Bitmap* Gfx::loadImage(const std::wstring& path, UINT maxWidthPx) {
    if (!wic_ || !dc_) return nullptr;
    IWICBitmapDecoder* dec = nullptr;
    if (FAILED(wic_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                               WICDecodeMetadataCacheOnDemand, &dec)))
        return nullptr;
    ID2D1Bitmap* bmp = BitmapFromDecoder(wic_, dc_, dec, maxWidthPx);
    dec->Release();
    return bmp;
}

// 标题栏标记：从 exe 的 RCDATA 资源解码。资源字节走 HGLOBAL 内存流交给 WIC，
// 不落临时文件、也不依赖程序目录里有没有图片。
ID2D1Bitmap* Gfx::loadImageFromResource(int resId, UINT maxWidthPx) {
    if (!wic_ || !dc_) return nullptr;
    HRSRC res = FindResourceW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(resId), RT_RCDATA);
    if (!res) return nullptr;
    DWORD size = SizeofResource(nullptr, res);
    HGLOBAL hGlb = LoadResource(nullptr, res);
    void* data = hGlb ? LockResource(hGlb) : nullptr;
    if (!data || !size) return nullptr;

    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return nullptr;
    ULONG written = 0;
    if (FAILED(stream->Write(data, size, &written))) { stream->Release(); return nullptr; }
    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);

    IWICBitmapDecoder* dec = nullptr;
    if (FAILED(wic_->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &dec))) {
        stream->Release();
        return nullptr;
    }
    ID2D1Bitmap* bmp = BitmapFromDecoder(wic_, dc_, dec, maxWidthPx);
    dec->Release();
    stream->Release();
    return bmp;
}

void Gfx::fillRound(const D2D1_ROUNDED_RECT& r, const D2D1_COLOR_F& c) {
    ID2D1SolidColorBrush* b = brush(c);
    if (b) dc_->FillRoundedRectangle(r, b);
}

// 从内存像素建位图（B8G8R8A8 预乘）——毛玻璃的颗粒噪点用它
ID2D1Bitmap* Gfx::createBitmapFromPixels(const void* pixels, UINT w, UINT h) {
    if (!dc_ || !pixels || !w || !h) return nullptr;
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    ID2D1Bitmap1* bmp = nullptr;
    if (FAILED(dc_->CreateBitmap(D2D1::SizeU(w, h), pixels, w * 4, props, &bmp))) return nullptr;
    return bmp;
}

void Gfx::strokeRound(const D2D1_ROUNDED_RECT& r, const D2D1_COLOR_F& c, float w) {
    ID2D1SolidColorBrush* b = brush(c);
    if (b) dc_->DrawRoundedRectangle(r, b, w);
}

void Gfx::text(const std::wstring& s, IDWriteTextFormat* fmt, const D2D1_RECT_F& r,
               const D2D1_COLOR_F& c) {
    if (!fmt || s.empty()) return;
    ID2D1SolidColorBrush* b = brush(c);
    if (b) dc_->DrawTextW(s.c_str(), (UINT32)s.size(), fmt, r, b,
                          D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float Gfx::textWidth(const std::wstring& s, IDWriteTextFormat* fmt) {
    if (!fmt || s.empty()) return 0;
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(dwrite_->CreateTextLayout(s.c_str(), (UINT32)s.size(), fmt, 4000.0f, 100.0f,
                                         &layout)))
        return 0;
    layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    DWRITE_TEXT_METRICS tm = {};
    layout->GetMetrics(&tm);
    layout->Release();
    return tm.widthIncludingTrailingWhitespace;
}

// 墨迹垂直居中：overhang 是相对"布局盒"而不是文本高度的——
//   inkTop    = -om.top
//   inkBottom = 盒高 + om.bottom（om.bottom 为负时表示墨迹在盒内）
// 用 tm.height 去加会把下边界算成很大的负数，文字就被画到天上去了。
void Gfx::textOptical(const std::wstring& s, IDWriteTextFormat* fmt, const D2D1_RECT_F& r,
                      const D2D1_COLOR_F& c, DWRITE_TEXT_ALIGNMENT align) {
    if (!fmt || s.empty()) return;
    ID2D1SolidColorBrush* b = brush(c);
    if (!b) return;
    float boxW = r.right - r.left;
    if (boxW <= 0) return;
    const float kBoxH = 1000.0f;
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(dwrite_->CreateTextLayout(s.c_str(), (UINT32)s.size(), fmt, boxW, kBoxH, &layout)))
        return;
    layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    DWRITE_TEXT_METRICS tm = {};
    DWRITE_OVERHANG_METRICS om = {};
    layout->GetMetrics(&tm);
    layout->GetOverhangMetrics(&om);
    float inkTop = -om.top;
    float inkBottom = kBoxH + om.bottom;
    float x = r.left;
    float textW = tm.widthIncludingTrailingWhitespace;
    if (align == DWRITE_TEXT_ALIGNMENT_CENTER)        x = r.left + (boxW - textW) / 2.0f;
    else if (align == DWRITE_TEXT_ALIGNMENT_TRAILING) x = r.right - textW;
    // 让墨迹中心落在控件中心
    float y = (r.top + r.bottom) / 2.0f - (inkTop + inkBottom) / 2.0f;
    dc_->DrawTextLayout(D2D1::Point2F(x, y), layout, b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    layout->Release();
}

void Gfx::pushClip(const D2D1_ROUNDED_RECT& r) {
    dc_->PushAxisAlignedClip(r.rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
}

void Gfx::popClip() { dc_->PopAxisAlignedClip(); }
