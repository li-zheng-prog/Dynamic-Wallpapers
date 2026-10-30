// Exercise the real UI state machine with system-setting writes replaced by fakes.
#define ApplyWallpaper TestApplyWallpaper
#define SaveConfig TestSaveConfig
#define SetAutoStart TestSetAutoStart
#define ReadWallpaperSnapshot TestReadWallpaperSnapshot
#define SystemUsesDarkMode TestSystemUsesDarkMode
#define wWinMain UnusedApplicationEntry
#include "../src/main.cpp"
#undef ApplyWallpaper
#undef SaveConfig
#undef SetAutoStart
#undef ReadWallpaperSnapshot
#undef SystemUsesDarkMode
#undef wWinMain
#include <cstdio>
#include <cstdlib>
#include <functional>

static int checks = 0, applyCalls = 0;
static bool applySucceeds = true;
static std::function<void()> duringApply;
static std::wstring lastImage;
static int lastStyle = -1;
static bool snapshotAvailable = true, appDark = false;
static Config savedConfig;
static std::vector<std::pair<std::wstring, std::wstring>> wallpaper = {{L"monitor-a", L"initial.jpg"}};

static void Check(bool condition, const char* name) {
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", name); std::exit(1); }
}

bool TestApplyWallpaper(const std::wstring& image, int style) {
    ++applyCalls;
    lastImage = image;
    lastStyle = style;
    if (duringApply) duringApply();
    if (applySucceeds) for (auto& monitor : wallpaper) monitor.second = image;
    return applySucceeds;
}
void TestSaveConfig(const Config& config) { savedConfig = config; }
void TestSetAutoStart(bool) {}
bool TestReadWallpaperSnapshot(std::vector<std::pair<std::wstring, std::wstring>>& out) {
    if (!snapshotAvailable) return false;
    out = wallpaper;
    return !out.empty();
}
bool TestSystemUsesDarkMode() { return appDark; }

static void TestWallpaperState() {
    g_app.cfg = Config{};
    g_app.simulateHour = 12;
    ThemeInfo theme;
    theme.name = L"Test";
    theme.dayImage = L"day.jpg";
    theme.nightImage = L"night.jpg";
    g_app.themes = {theme};
    ApplyCurrent(true);
    Check(applyCalls == 1 && lastImage == theme.dayImage, "initial wallpaper");
    SendMessageW(g_app.hwnd, WM_TIMER, TM_TICK, 0);
    Check(applyCalls == 1, "first scheduler tick skips successful wallpaper");
    ApplyCurrent(true);
    Check(applyCalls == 2, "explicit apply bypasses cache");
    applySucceeds = false;
    ApplyCurrent(true);
    Check(applyCalls == 3 && g_app.appliedImage.empty(), "failed force clears cache");
    ApplyCurrent(false);
    Check(applyCalls == 4, "failed apply is retried");
    applySucceeds = true;
    ApplyCurrent(false);
    ApplyCurrent(false);
    Check(applyCalls == 5, "retry success restores cache");
    g_app.cfg.wallpaperStyle = 3;
    ApplyCurrent(false);
    Check(applyCalls == 6 && lastStyle == 3, "style change invalidates cache");
    g_app.cfg.autoSwitch = false;
    g_app.simulateHour = 23;
    ApplyCurrent(false);
    Check(applyCalls == 6, "disabled auto switch skips schedule");
    ApplyCurrent(true);
    Check(applyCalls == 7 && lastImage == theme.nightImage, "manual night while paused");
    g_app.cfg.autoSwitch = true;
    g_app.simulateHour = 12;
    duringApply = [] { ApplyCurrent(false); };
    ApplyCurrent(false);
    Check(applyCalls == 8 && !g_app.applyingWallpaper, "nested broadcast cannot reenter apply");
    duringApply = [] {
        g_app.themes.clear();
        g_app.cfg.wallpaperStyle = 2;
    };
    ApplyCurrent(true);
    Check(applyCalls == 9 && g_app.appliedImage == theme.dayImage &&
          g_app.appliedStyle == 3, "cache records applied values across nested changes");
    duringApply = {};
    ApplyCurrent(true);
    Check(applyCalls == 9, "empty theme list is safe");
    std::puts("PASS: wallpaper deduplication, retries, schedule, reentrancy");
}

static void TestExternalWallpaper() {
    g_app.cfg = Config{};
    g_app.cfg.theme = L"Test";
    g_app.selIdx = 0;
    g_app.simulateHour = 12;
    ThemeInfo theme;
    theme.name = L"Test"; theme.dayImage = L"day.jpg"; theme.nightImage = L"night.jpg";
    g_app.themes = {theme};
    wallpaper = {{L"monitor-a", L"initial.jpg"}, {L"monitor-b", L"initial.jpg"}};
    g_app.wallpaperSnapshot.clear();
    g_app.appliedImage.clear();
    g_app.glass = false;
    ApplyCurrent(true);
    int calls = applyCalls;
    CheckExternalWallpaper();
    Check(!g_app.cfg.noTheme, "application's wallpaper changes do not deselect theme");
    wallpaper.push_back({L"monitor-c", L"other.jpg"});
    CheckExternalWallpaper();
    Check(!g_app.cfg.noTheme, "new monitor is not mistaken for an external change");
    wallpaper[1].second = L"manual.jpg";
    snapshotAvailable = false;
    CheckExternalWallpaper();
    Check(!g_app.cfg.noTheme, "failed read does not clear selection");
    snapshotAvailable = true;
    g_app.cfg.autoSwitch = false;
    SendMessageW(g_app.hwnd, WM_TIMER, TM_TICK, 0);
    Check(g_app.cfg.noTheme && g_app.selIdx == -1 && savedConfig.noTheme &&
          savedConfig.theme.empty(), "manual change while paused persists no-theme state");
    g_app.cfg = savedConfig;
    g_app.selIdx = 0;
    Check(SelectedTheme() == nullptr, "reloaded no-theme state has no selected theme");
    g_app.cfg.autoSwitch = true;
    ApplyCurrent(true);
    SendMessageW(g_app.hwnd, WM_TIMER, TM_TICK, 0);
    Check(applyCalls == calls, "no-theme state never reapplies automatically or forcibly");
    SelectTheme(0);
    Check(!g_app.cfg.noTheme && !savedConfig.noTheme && savedConfig.theme == theme.name &&
          applyCalls == calls + 1, "choosing a theme resumes and persists selection");
    calls = applyCalls;
    wallpaper[0].second = L"";
    CheckExternalWallpaper();
    Check(g_app.cfg.noTheme && applyCalls == calls, "solid color background exits theme");
    std::puts("PASS: manual wallpaper changes, monitors, failure, persistence and re-selection");
}

static void TestFollowAppMode() {
    g_app.cfg = Config{};
    g_app.cfg.theme = L"Test";
    g_app.cfg.switchMode = 1;
    g_app.cfg.appearance = 0;
    g_app.selIdx = 0;
    appDark = false;
    RefreshPalette();
    Check(!g_app.pal.dark && TargetStage() == 0, "light application mode controls UI and wallpaper");
    appDark = true;
    RefreshPalette();
    Check(g_app.pal.dark && TargetStage() == 1, "dark application mode controls UI and wallpaper");
    g_app.cfg.appearance = 1;
    RefreshPalette();
    Check(!g_app.pal.dark && TargetStage() == 1, "explicit UI override leaves wallpaper following applications");
    appDark = false;
    std::puts("PASS: UI and wallpaper follow Windows application mode");
}

static IDXGISurface* TargetSurface(float* dpi = nullptr) {
    ID2D1Image* image = nullptr;
    g_app.gfx.dc()->GetTarget(&image);
    Check(image != nullptr, "render target exists");
    ID2D1Bitmap1* bitmap = nullptr;
    HRESULT hr = image->QueryInterface(IID_ID2D1Bitmap1, (void**)&bitmap);
    image->Release();
    Check(SUCCEEDED(hr), "target is bitmap");
    if (dpi) { float y; bitmap->GetDpi(dpi, &y); }
    IDXGISurface* surface = nullptr;
    hr = bitmap->GetSurface(&surface);
    bitmap->Release();
    Check(SUCCEEDED(hr), "target has DXGI surface");
    return surface;
}

static void TestLazyGraphicsAndDpi() {
    g_app.glass = true;
    EnsureGlassBitmap(true);
    RenderFrame();
    StartAnim();
    Check(!g_app.gfx.dc() && !g_glass.small && !g_timerPrecisionUsers,
          "hidden startup does not initialize graphics or animation timers");
    g_app.glass = false;
    g_app.solidBg = true;
    Check(EnsureUiInitialized(), "first UI initialization");
    auto* context = g_app.gfx.dc();
    Check(EnsureUiInitialized() && context == g_app.gfx.dc(), "UI initialization is idempotent");
    UINT dpi = GetDpiForWindow(g_app.hwnd);
    float targetDpi = 0;
    IDXGISurface* before = TargetSurface(&targetDpi);
    Check(std::fabs(targetDpi - dpi) < 0.01f && g_app.gfx.scale() == dpi / 96.0f,
          "initial target uses window DPI");
    RECT rc;
    GetClientRect(g_app.hwnd, &rc);
    g_app.gfx.resize(rc.right, rc.bottom, dpi + 48);
    IDXGISurface* after = TargetSurface(&targetDpi);
    IUnknown *a = nullptr, *b = nullptr;
    before->QueryInterface(IID_IUnknown, (void**)&a);
    after->QueryInterface(IID_IUnknown, (void**)&b);
    Check(a == b && std::fabs(targetDpi - (dpi + 48)) < 0.01f,
          "DPI change reuses actual swapchain buffer");
    a->Release(); b->Release(); before->Release(); after->Release();
    g_app.gfx.resize(rc.right, rc.bottom, dpi);
    Check(g_app.gfx.begin(), "drawing remains available after DPI roundtrip");
    g_app.gfx.dc()->Clear(D2D1::ColorF(0, 0.0f));
    g_app.gfx.end();
    {
        AnimationTimerPrecision outer, inner;
        outer.acquire(); outer.acquire(); inner.acquire();
        Check(g_timerPrecisionUsers == 2, "nested precision request is balanced");
        inner.release();
        Check(g_timerPrecisionUsers == 1, "nested precision release preserves outer request");
    }
    Check(g_timerPrecisionUsers == 0, "idle releases timer precision");
    std::puts("PASS: lazy graphics, real GPU buffer reuse on DPI change, timer lifetime");
}

static std::wstring MakeBitmap(const wchar_t* name, LONG width, LONG height) {
    std::wstring path = JoinPath(ExeDir(), name);
    BITMAPFILEHEADER file = {};
    BITMAPINFOHEADER info = {};
    info.biSize = sizeof(info); info.biWidth = width; info.biHeight = -height;
    info.biPlanes = 1; info.biBitCount = 32; info.biCompression = BI_RGB;
    std::vector<DWORD> pixels((size_t)width * height, 0xFF3070B0);
    file.bfType = 0x4D42; file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + (DWORD)(pixels.size() * sizeof(DWORD));
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(handle != INVALID_HANDLE_VALUE, "create isolated image fixture");
    DWORD written;
    bool ok = WriteFile(handle, &file, sizeof(file), &written, nullptr) &&
              WriteFile(handle, &info, sizeof(info), &written, nullptr) &&
              WriteFile(handle, pixels.data(), (DWORD)(pixels.size() * 4), &written, nullptr);
    CloseHandle(handle);
    Check(ok, "write isolated image fixture");
    return path;
}

static std::vector<ThumbResult> WaitResults(ThumbLoader& loader, size_t count) {
    std::vector<ThumbResult> results;
    ULONGLONG until = GetTickCount64() + 5000;
    while (results.size() < count && GetTickCount64() < until) {
        auto ready = loader.takeResults();
        for (auto& result : ready) results.push_back(std::move(result));
        if (results.size() < count) Sleep(1);
    }
    Check(results.size() == count, "background decode finishes within timeout");
    return results;
}

static void TestThumbnails() {
    auto normal = MakeBitmap(L"thumbnail-fixture.bmp", 900, 600);
    auto tall = MakeBitmap(L"thumbnail-tall.bmp", 1, 8192);
    ThumbLoader loader;
    const UINT message = WM_APP + 20;
    loader.submit(g_app.hwnd, message, 1, {{1, 0, normal}, {1, 1, tall}, {1, 2, normal + L".missing"}});
    auto decoded = WaitResults(loader, 3);
    Check(decoded[0].width == 420 && decoded[0].height == 280 &&
          decoded[0].pixels.size() == 420 * 280 * 4, "thumbnail is scaled PBGRA");
    Check(decoded[1].width == 1 && decoded[1].height == 4096, "extreme aspect ratio is bounded");
    Check(decoded[2].pixels.empty(), "missing file returns a failure result");
    for (size_t gen = 2; gen <= 60; ++gen) {
        loader.cancel(gen);
        loader.submit(g_app.hwnd, message, gen, {{gen, 0, normal}, {gen, 1, normal}});
    }
    auto latest = WaitResults(loader, 2);
    Check(latest[0].request.generation == 60 && latest[1].request.generation == 60,
          "rapid viewport replacement discards obsolete generations");
    loader.submit(g_app.hwnd, message, 60, {{60, 2, normal}, {60, 3, normal}});
    loader.stop(); loader.stop();
    Check(loader.takeResults().empty(), "shutdown joins worker and discards queued pixels");

    ThemeInfo theme;
    theme.name = L"Thumbnail fixture"; theme.dayImage = theme.nightImage = normal;
    g_app.themes.assign(48, theme);
    ComputeLayout(g_app.L);
    SyncThumbs();
    // Cancel work while scrolling and changing the theme list.
    g_app.scroll = g_app.scrollTarget = g_app.L.maxScroll;
    SyncThumbs();
    ClearThumbs();
    g_app.themes.assign(1, theme);
    g_app.scroll = g_app.scrollTarget = 0;
    ComputeLayout(g_app.L);
    SyncThumbs();
    ULONGLONG until = GetTickCount64() + 5000;
    while (!g_app.thumbs[0] && GetTickCount64() < until) {
        MSG msg;
        while (PeekMessageW(&msg, g_app.hwnd, kThumbReadyMessage, kThumbReadyMessage, PM_REMOVE))
            DispatchMessageW(&msg);
        Sleep(1);
    }
    Check(g_app.thumbs[0] && g_app.thumbs.size() == 1,
          "UI accepts current thumbnail after scroll and theme replacement");
    ClearThumbs();
    g_thumbLoader.stop();
    DeleteFileW(normal.c_str()); DeleteFileW(tall.c_str());
    std::puts("PASS: WIC decoding, missing images, cancellation, scrolling, UI upload, shutdown");
}

int wmain() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Check(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "initialize COM");
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DynamicWallpapersIsolatedTests";
    Check(RegisterClassW(&wc) != 0, "register isolated window class");
    g_app.hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName,
        L"Dynamic Wallpapers isolated tests", WS_POPUP, 0, 0, 700, 764,
        nullptr, nullptr, wc.hInstance, nullptr);
    Check(g_app.hwnd != nullptr, "create hidden isolated test window");
    TestWallpaperState();
    TestExternalWallpaper();
    TestFollowAppMode();
    TestLazyGraphicsAndDpi();
    TestThumbnails();
    DestroyWindow(g_app.hwnd);
    g_app.gfx.shutdown();
    CoUninitialize();
    std::printf("PASS: all %d checks (no wallpaper/config/autostart writes)\n", checks);
    return 0;
}
