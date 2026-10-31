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
static bool deferApply = false;
static std::function<void()> duringApply;
static std::function<void()> duringRead;
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
    if (applySucceeds && !deferApply) for (auto& monitor : wallpaper) monitor.second = image;
    return applySucceeds;
}
void TestSaveConfig(const Config& config) { savedConfig = config; }
void TestSetAutoStart(bool) {}
bool TestReadWallpaperSnapshot(std::vector<std::pair<std::wstring, std::wstring>>& out) {
    if (duringRead) duringRead();
    if (!snapshotAvailable) return false;
    out = wallpaper;
    return !out.empty();
}
bool TestSystemUsesDarkMode() { return appDark; }

static void TestWallpaperTracker() {
    using Result = WallpaperTracker::Result;
    const std::wstring day = L"C:\\Themes\\day.jpg", night = L"C:\\Themes\\night.jpg";
    Check(SameWallpaperPath(day, L"c:/Themes/sub/../DAY.jpg"), "canonical full path comparison");
    Check(SameWallpaperPath(day, L"\\\\?\\C:\\Themes\\day.jpg"), "extended path comparison");
    Check(!SameWallpaperPath(day, L"C:\\Other\\day.jpg"), "same filename elsewhere is external");
    Check(!SameWallpaperPath(day, L""), "empty path is distinct from a theme image");
    WallpaperTracker tracker;
    WallpaperSnapshot current = {{L"a", day}, {L"b", day}};
    auto observe = [&](ULONGLONG now) { return tracker.Observe(current, day, night, now); };
    observe(0);
    tracker.Begin(night, 4, 10);
    tracker.Applied(true);
    Check(observe(20) == Result::None && tracker.pending,
          "successful API call with unchanged desktop remains unconfirmed");
    current[0].second = night;
    Check(observe(30) == Result::None && tracker.pending && !tracker.Blocked(),
          "partial monitor completion keeps request pending");
    current[1].second = L"";
    Check(observe(40) == Result::None && tracker.pending && tracker.Blocked(),
          "transient empty monitor during write is inconclusive");
    current[1].second = night;
    Check(observe(50) == Result::Confirmed && !tracker.pending,
          "all monitors must confirm the requested image");
    Check(observe(60) == Result::None && !tracker.Blocked(), "duplicate notification is harmless");

    tracker.Begin(day, 4, 70); tracker.Applied(true);
    const auto oldGeneration = tracker.generation;
    tracker.Begin(night, 4, 80); tracker.Applied(true);
    current[0].second = day;
    Check(observe(90) == Result::None && tracker.generation > oldGeneration && tracker.target == night,
          "rapid reversal cannot confirm a superseded request");
    current[0].second = night;
    Check(observe(100) == Result::Confirmed, "latest request eventually confirms");

    tracker.Begin(day, 4, 200); tracker.Applied(true);
    current[0].second = L"C:\\manual.jpg";
    Check(observe(210) == Result::None && tracker.Blocked(),
          "manual change during outstanding write immediately blocks automatic writes");
    Check(observe(700) == Result::None, "less than one second cannot confirm external change");
    Check(observe(1210) == Result::External, "manual change during transition is not ignored");
    tracker.ResetSelection();
    current = {{L"a", day}, {L"b", day}}; observe(2000);
    current[0].second = L"C:\\manual.jpg"; observe(2010);
    tracker.ReadFailed();
    Check(observe(4000) == Result::None && tracker.Blocked(), "read failure restarts external confirmation");
    current[0].second = night;
    Check(observe(4500) == Result::None && !tracker.Blocked(), "return to theme cancels external candidate");
    current[0].second = L"C:\\manual-a.jpg"; observe(5000);
    current[0].second = L"C:\\manual-b.jpg";
    Check(observe(6000) == Result::None, "changing external candidate restarts confirmation");
    Check(observe(7000) == Result::External, "stable replacement is confirmed");

    tracker.ResetSelection();
    current = {{L"a", day}, {L"b", day}}; observe(8000);
    current.push_back({L"c", L"C:\\other.jpg"});
    Check(observe(8010) == Result::None && !tracker.Blocked(), "new monitor adopts its own baseline");
    std::reverse(current.begin(), current.end());
    current.pop_back();
    Check(observe(8020) == Result::None && !tracker.Blocked(), "monitor reorder and removal preserve theme");
    current[0].second = L"C:\\manual.jpg"; observe(8030);
    current.erase(current.begin());
    Check(observe(9500) == Result::None && !tracker.Blocked(), "removed monitor cancels its external candidate");

    tracker.ResetSelection();
    current = {{L"a", day}}; observe(10000);
    for (unsigned attempt = 0; attempt < WallpaperTracker::maxAttempts; ++attempt) {
        ULONGLONG now = 10100 + attempt * 6000;
        Check(tracker.CanRequest(night, 4), "bounded retry is available");
        tracker.Begin(night, 4, now); tracker.Applied(true);
        Check(observe(now + WallpaperTracker::settleMs) == Result::TimedOut,
              "unchanged desktop times out without leaving theme");
    }
    Check(!tracker.CanRequest(night, 4), "three unconfirmed requests stop automatic retries");
    current[0].second = night;
    Check(observe(30000) == Result::Confirmed, "late successful update is still recognized");
    tracker.Begin(day, 4, 31000); tracker.Applied(true);
    current[0].second = L"";
    observe(31010);
    Check(observe(36000) == Result::TimedOut && tracker.Blocked(),
          "persistent solid color is reconsidered after transition deadline");
    Check(observe(37000) == Result::External, "persistent solid color exits after confirmation");

    tracker.ResetSelection();
    current = {{L"a", day}}; observe(40000);
    for (unsigned attempt = 0; attempt < WallpaperTracker::maxAttempts; ++attempt) {
        tracker.Begin(night, 4, 40010 + attempt); tracker.Applied(false);
    }
    Check(!tracker.CanRequest(night, 4) && tracker.CanRequest(day, 4),
          "API failure retry budget is per target and position");
    tracker.ResetSelection(); observe(41000);
    Check(tracker.CanRequest(night, 4) && !tracker.Blocked(), "explicit selection clears retry and candidate state");
    std::puts("PASS: delayed writes, rapid reversal, stable external confirmation, topology and bounded retries");
}

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
    Check(applyCalls == 9 && g_app.wallpaper.target == theme.dayImage &&
          g_app.wallpaper.style == 3, "request records applied values across nested changes");
    duringApply = {};
    ApplyCurrent(true);
    Check(applyCalls == 9, "empty theme list is safe");
    std::puts("PASS: wallpaper deduplication, retries, schedule, reentrancy");
}

static void TestDelayedWallpaper() {
    g_app.cfg = Config{};
    g_app.cfg.theme = L"Test";
    g_app.cfg.switchMode = 1;
    g_app.selIdx = 0;
    g_app.glass = false;
    ThemeInfo theme;
    theme.name = L"Test"; theme.dayImage = L"day.jpg"; theme.nightImage = L"night.jpg";
    g_app.themes = {theme};
    wallpaper = {{L"monitor-a", L"initial.jpg"}, {L"monitor-b", L"initial.jpg"}};
    g_app.wallpaper.ResetSelection();
    g_app.appliedImage.clear();
    appDark = false;
    ApplyCurrent(true);
    deferApply = true;
    appDark = true;
    SendMessageW(g_app.hwnd, WM_SETTINGCHANGE, 0, 0);
    Check(g_app.appliedImage.empty() && g_app.wallpaper.pending,
          "API success does not populate cache before desktop confirmation");
    wallpaper[0].second = theme.nightImage;
    CheckExternalWallpaper();
    Check(!g_app.cfg.noTheme, "delayed first monitor does not deselect theme");
    wallpaper[1].second = theme.nightImage;
    SendMessageW(g_app.hwnd, WM_SETTINGCHANGE, SPI_SETDESKWALLPAPER, 0);
    Check(!g_app.cfg.noTheme, "delayed wallpaper broadcast preserves theme");
    deferApply = false;
    appDark = false;
    SendMessageW(g_app.hwnd, WM_SETTINGCHANGE, 0, 0);
    Check(!g_app.cfg.noTheme && lastImage == theme.dayImage,
          "following system still works after delayed completion");
    g_app.cfg.autoSwitch = false;
    wallpaper[0].second = theme.nightImage;
    SendMessageW(g_app.hwnd, WM_TIMER, TM_TICK, 0);
    Check(!g_app.cfg.noTheme && wallpaper[0].second == theme.nightImage,
          "paused theme accepts its other image without automatic writing");
    g_app.cfg.autoSwitch = true;
    SendMessageW(g_app.hwnd, WM_TIMER, TM_TICK, 0);
    Check(!g_app.cfg.noTheme && wallpaper[0].second == theme.dayImage,
          "resuming follows target even when previously applied cache matches");
    // A real external change during the next delayed write must block every
    // automatic entry point, including forced resume and mode changes.
    deferApply = true;
    appDark = true;
    SendMessageW(g_app.hwnd, WM_SETTINGCHANGE, 0, 0);
    wallpaper[0].second = L"manual-during-transition.jpg";
    int calls = applyCalls;
    SendMessageW(g_app.hwnd, WM_SETTINGCHANGE, 0, 0);
    SendMessageW(g_app.hwnd, WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0);
    ApplyCurrent(true);
    Check(applyCalls == calls && !g_app.cfg.noTheme,
          "external candidate blocks automatic writes and forced resume");
    CheckExternalWallpaper(GetTickCount64() + WallpaperTracker::confirmMs);
    Check(g_app.cfg.noTheme && savedConfig.noTheme && applyCalls == calls,
          "external wallpaper during write is preserved and persisted");
    deferApply = false;
    SelectTheme(0);
    Check(!g_app.cfg.noTheme && !g_app.wallpaper.Blocked() && g_app.appliedImage == theme.nightImage,
          "explicit selection clears pending external state");
    appDark = false;
    std::puts("PASS: delayed system following, pause/resume and manual changes during writes");
}

static void TestNestedThemeSelection() {
    g_app.cfg = Config{};
    g_app.cfg.theme = L"Old";
    g_app.selIdx = 0;
    g_app.simulateHour = 12;
    g_app.glass = false;
    ThemeInfo oldTheme, newTheme;
    oldTheme.name = L"Old"; oldTheme.dayImage = L"old-day.jpg"; oldTheme.nightImage = L"old-night.jpg";
    newTheme.name = L"New"; newTheme.dayImage = L"new-day.jpg"; newTheme.nightImage = L"new-night.jpg";
    g_app.themes = {oldTheme, newTheme};
    wallpaper = {{L"a", L"initial.jpg"}};
    int calls = applyCalls;
    duringApply = [] { SelectTheme(0); SelectTheme(1); };
    ApplyCurrent(true, true);
    duringApply = {};
    Check(g_app.cfg.theme == newTheme.name && g_app.appliedImage.empty() && applyCalls == calls + 1,
          "nested selection cancels old request without committing its cache");
    MSG msg;
    while (PeekMessageW(&msg, g_app.hwnd, kApplySelectionMessage, kApplySelectionMessage, PM_REMOVE))
        DispatchMessageW(&msg);
    Check(!g_app.cfg.noTheme && g_app.appliedImage == newTheme.dayImage && applyCalls == calls + 2,
          "nested selections coalesce and apply only the latest theme");
    g_app.cfg.autoSwitch = false;
    wallpaper[0].second = oldTheme.nightImage;
    CheckExternalWallpaper();
    CheckExternalWallpaper(GetTickCount64() + WallpaperTracker::confirmMs);
    Check(g_app.cfg.noTheme, "old theme folder is not a blanket whitelist");
    SelectTheme(0);
    duringRead = [] {
        SendMessageW(g_app.hwnd, WM_SETTINGCHANGE, 0, 0);
        g_app.themes = {g_app.themes[1]};
        SelectTheme(0);
    };
    CheckExternalWallpaper();
    duringRead = {};
    Check(g_app.cfg.theme == newTheme.name && g_app.appliedImage.empty(),
          "selection during snapshot read invalidates the previous result");
    while (PeekMessageW(&msg, g_app.hwnd, kApplySelectionMessage, kApplySelectionMessage, PM_REMOVE))
        DispatchMessageW(&msg);
    Check(g_app.appliedImage == newTheme.dayImage && !g_app.selectionDeferred && !g_app.selectionQueued,
          "snapshot reentrancy defers and applies latest selection safely");
    std::puts("PASS: nested selection generation and deferred latest-theme apply");
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
    g_app.wallpaper.ResetSelection();
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
    Check(!g_app.cfg.noTheme && applyCalls == calls,
          "external change waits for confirmation without overwriting wallpaper");
    CheckExternalWallpaper(GetTickCount64() + WallpaperTracker::confirmMs);
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
    Check(!g_app.cfg.noTheme, "solid color waits for confirmation");
    CheckExternalWallpaper(GetTickCount64() + WallpaperTracker::confirmMs);
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

static void TestNativeMinimize() {
    HWND h = g_app.hwnd;
    const LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE);
    Check((style & WS_OVERLAPPEDWINDOW) == WS_OVERLAPPEDWINDOW && !(style & WS_POPUP),
          "main window retains standard caption and system controls");
    RECT outer, client;
    GetWindowRect(h, &outer); GetClientRect(h, &client);
    Check(outer.right - outer.left == client.right && outer.bottom - outer.top == client.bottom,
          "standard window style keeps the custom full-client frame");
    ShowWindow(h, SW_SHOWNOACTIVATE);
    TrackNormRect();
    WINDOWPLACEMENT normal = {sizeof(normal)};
    GetWindowPlacement(h, &normal);
    const RECT normRect = g_app.normRect;
    const float width = g_app.winWDip, height = g_app.winHDip;

    // Click the actual custom caption button while the startup fade is unfinished.
    g_app.winAnim = 0.35f;
    g_app.winAnimating = true;
    const auto& button = g_app.L.captionMin;
    LPARAM point = MAKELPARAM((int)((button.left + button.right) * 0.5f * g_app.gfx.scale()),
                             (int)((button.top + button.bottom) * 0.5f * g_app.gfx.scale()));
    SendMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, point);
    SendMessageW(h, WM_LBUTTONUP, 0, point);
    Check(IsIconic(h), "custom caption button minimizes through Windows");
    Check(g_app.winWDip == width && g_app.winHDip == height,
          "minimizing does not resize the content layout to icon dimensions");
    Check(!g_app.winAnimating && !g_app.hideAfterAnim && g_app.winAnim == 1.0f,
          "minimizing cancels the unfinished fade");
    WINDOWPLACEMENT minimized = {sizeof(minimized)};
    GetWindowPlacement(h, &minimized);
    Check(EqualRect(&normal.rcNormalPosition, &minimized.rcNormalPosition),
          "minimizing preserves the system restore rectangle");
    Check(EqualRect(&normRect, &g_app.normRect), "minimizing preserves the maximize restore rectangle");
    ShowMainWindow();
    GetWindowRect(h, &outer);
    Check(!IsIconic(h) && !IsZoomed(h) && IsWindowVisible(h) && EqualRect(&outer, &normRect),
          "tray or second-instance show restores the normal position");
    Check(!g_app.winAnimating && g_app.winAnim == 1.0f,
          "restoring from the taskbar adds no application fade");

    // A pending tray hide must not run after a native minimize / restore cycle.
    StartWindowAnim(false);
    SendMessageW(h, WM_SYSCOMMAND, SC_MINIMIZE | 1, 0);
    SendMessageW(h, WM_SYSCOMMAND, SC_RESTORE | 1, 0);
    StepAnim();
    Check(!IsIconic(h) && IsWindowVisible(h) && !g_app.hideAfterAnim,
          "system menu minimize/restore cancels a pending tray hide");

    const float zoomMs = g_app.zoomMs;
    g_app.zoomMs = 40.0f;
    StartZoom(true);
    Check(IsZoomed(h), "existing maximize transition still reaches maximized state");
    SendMessageW(h, WM_SYSCOMMAND, SC_MINIMIZE, 0);
    GetWindowPlacement(h, &minimized);
    Check(IsIconic(h) && (minimized.flags & WPF_RESTORETOMAXIMIZED),
          "Windows remembers minimize from maximized state");
    Check(EqualRect(&normal.rcNormalPosition, &minimized.rcNormalPosition),
          "maximized minimize preserves the original normal placement");
    SendMessageW(h, WM_SYSCOMMAND, SC_RESTORE, 0);
    Check(IsZoomed(h) && g_app.maximized && !IsIconic(h),
          "system restore returns a minimized maximized window to maximized state");
    SendMessageW(h, WM_SYSCOMMAND, SC_MINIMIZE, 0);
    ShowMainWindow();
    Check(IsZoomed(h) && !IsIconic(h) && !g_app.winAnimating,
          "tray restore also preserves maximized state without a custom fade");
    StartZoom(false);
    GetWindowRect(h, &outer);
    Check(!IsZoomed(h) && EqualRect(&outer, &normRect),
          "restore from maximized still returns to the original position");
    g_app.zoomMs = zoomMs;
    ShowWindow(h, SW_HIDE);
    std::puts("PASS: native minimize/restore, placement, maximized state, fade interruption");
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
        L"Dynamic Wallpapers isolated tests", kMainWindowStyle, 0, 0, 700, 764,
        nullptr, nullptr, wc.hInstance, nullptr);
    Check(g_app.hwnd != nullptr, "create hidden isolated test window");
    TestWallpaperState();
    TestWallpaperTracker();
    TestDelayedWallpaper();
    TestExternalWallpaper();
    TestNestedThemeSelection();
    TestFollowAppMode();
    TestLazyGraphicsAndDpi();
    TestNativeMinimize();
    TestThumbnails();
    DestroyWindow(g_app.hwnd);
    g_app.gfx.shutdown();
    CoUninitialize();
    std::printf("PASS: all %d checks (no wallpaper/config/autostart writes)\n", checks);
    return 0;
}
