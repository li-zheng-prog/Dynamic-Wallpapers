// ============================================================================
//  setup.cpp — 安装程序（单窗口向导，WinUI3 风格，复用 gfx/ui 渲染层）
//
//  所有安装文件作为 RCDATA 资源嵌在本 exe 里，运行时不依赖外部文件：
//    · 每个待安装文件一个资源 ID（build/pack_setup.py 生成 payload.rc）
//    · 资源 2000 = 清单文本，格式: id|kind|相对路径|主题名|文件大小
//  安装位置：%LOCALAPPDATA%\Programs\Dynamic Wallpapers（无需管理员权限）
//  卸载：写入"应用和功能"条目，卸载命令调用 DynamicWallpapers.exe --uninstall
//  更名清理：旧版本（TimeWall / 壁纸随时间变化）的安装目录、快捷方式、卸载项、
//  自启项在安装时顺手清掉（旧目录里的主题先并入新目录，用户自加的不丢）
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <d2d1_1.h>
#include <d2d1helper.h>
#include <dwmapi.h>
#include <string>
#include <vector>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include "gfx.h"
#include "ui.h"
#include "resource.h"

void RenderFrame();   // 前向声明（安装过程中直接调用重绘）

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

static const wchar_t* kWindowClass = L"DynamicWallpapersSetupWindow";
static const float kWinW = 700.0f;
static const float kWinH = 560.0f;
static const float kPadX = 28.0f;
static const wchar_t* kAppDisplay = L"Dynamic Wallpapers";      // 显示名（标题/快捷方式/卸载项）
static const wchar_t* kAppExe     = L"DynamicWallpapers.exe";   // 主程序文件名
static const wchar_t* kLegacyDisplay = L"壁纸随时间变化";        // 更名前：显示名/快捷方式名
static const wchar_t* kLegacyAppName  = L"TimeWall";            // 更名前：安装目录/卸载项/自启项名
static const wchar_t* kVersion = L"1.0.0";

#define RES_MANIFEST 2000
#define WM_APP_QUIT   (WM_APP + 3)

enum CtrlId {
    CID_NONE = -1,
    CID_MIN = 1, CID_CLOSE,
    CID_BROWSE = 10,
    CID_CHK_DESKTOP, CID_CHK_STARTMENU, CID_CHK_AUTOSTART,
    CID_THEME0 = 20, CID_THEME1, CID_THEME2, CID_THEME3,
    CID_INSTALL = 40, CID_CANCEL, CID_RUN, CID_FINISH,
};

enum Page { PAGE_OPTIONS, PAGE_INSTALLING, PAGE_DONE };

struct PayloadItem {
    int id = 0;
    std::wstring kind;    // app / theme
    std::wstring rel;     // 相对安装目录的路径
    std::wstring theme;   // 所属主题名（kind=theme 时）
    unsigned long long size = 0;
};

struct ThemeGroup {
    std::wstring name;
    unsigned long long size = 0;
    int imageCount = 0;
    bool include = true;
};

struct Layout {
    D2D1_RECT_F captionMin, captionClose;
    D2D1_RECT_F dirBox, browseBtn;
    D2D1_RECT_F chkDesktop, chkStartMenu, chkAutoStart;
    D2D1_RECT_F themeChk[4];
    D2D1_RECT_F installBtn, cancelBtn, runBtn, finishBtn;
    D2D1_RECT_F progress;
};

struct Setup {
    HWND hwnd = nullptr;
    Gfx gfx;
    Palette pal;
    Layout L;
    Page page = PAGE_OPTIONS;
    std::wstring installDir;
    bool desktopLnk = true, startMenuLnk = true, autoStart = true;
    std::vector<PayloadItem> items;
    std::vector<ThemeGroup> themes;
    int hot = CID_NONE, pressed = CID_NONE;
    // 安装进度
    int doneCount = 0, totalCount = 0;
    std::wstring currentFile;
    bool finished = false;
    std::wstring errorText;
    int errorCount = 0;
    ID2D1Bitmap* logoBmp = nullptr;         // 标题栏标记（与主程序同一份资源）
    bool logoTried = false;
};
static Setup g_s;

// ---------------------------------------------------------------------------
//  工具
// ---------------------------------------------------------------------------
static std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    std::wstring r = a;
    if (r.back() != L'\\') r += L'\\';
    return r + b;
}

static void EnsureDir(const std::wstring& dir) {
    std::wstring cur;
    for (size_t i = 0; i < dir.size(); ++i) {
        cur += dir[i];
        if (dir[i] == L'\\' || dir[i] == L'/') {
            if (cur.size() > 3) CreateDirectoryW(cur.c_str(), nullptr);
        }
    }
    CreateDirectoryW(dir.c_str(), nullptr);
}

static std::wstring FormatSize(unsigned long long bytes) {
    wchar_t buf[64];
    if (bytes >= 1024ull * 1024 * 1024)
        swprintf(buf, 64, L"%.1f GB", bytes / 1024.0 / 1024 / 1024);
    else if (bytes >= 1024ull * 1024)
        swprintf(buf, 64, L"%.0f MB", bytes / 1024.0 / 1024);
    else
        swprintf(buf, 64, L"%.0f KB", bytes / 1024.0);
    return buf;
}

static std::wstring DefaultInstallDir() {
    wchar_t buf[MAX_PATH * 2] = {0};
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, buf);
    return JoinPath(JoinPath(buf, L"Programs"), kAppDisplay);
}

// 读取内嵌清单，并按主题聚合出复选框列表
static void LoadManifest() {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(RES_MANIFEST), RT_RCDATA);
    if (!res) return;
    HGLOBAL h = LoadResource(nullptr, res);
    const char* data = (const char*)LockResource(h);
    DWORD size = SizeofResource(nullptr, res);
    if (!data || !size) return;
    std::string text(data, size);
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF) text.erase(0, 3);   // BOM
    std::wstring w;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
    w.resize(wlen);
    MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), &w[0], wlen);

    std::wstringstream ss(w);
    std::wstring line;
    while (std::getline(ss, line)) {
        if (line.empty()) continue;
        PayloadItem it;
        std::wstringstream ls(line);
        std::wstring part;
        int idx = 0;
        while (std::getline(ls, part, L'|')) {
            switch (idx++) {
            case 0: it.id = _wtoi(part.c_str()); break;
            case 1: it.kind = part; break;
            case 2: it.rel = part; break;
            case 3: it.theme = part; break;
            case 4: it.size = _wtoi64(part.c_str()); break;
            }
        }
        if (it.id) g_s.items.push_back(it);
    }
    for (auto& it : g_s.items) {
        if (it.kind != L"theme") continue;
        ThemeGroup* g = nullptr;
        for (auto& t : g_s.themes) if (t.name == it.theme) g = &t;
        if (!g) {
            ThemeGroup ng;
            ng.name = it.theme;
            g_s.themes.push_back(ng);
            g = &g_s.themes.back();
        }
        g->size += it.size;
        if (it.rel.find(L".jpg") != std::wstring::npos || it.rel.find(L".png") != std::wstring::npos)
            g->imageCount++;
    }
}

// ---------------------------------------------------------------------------
//  布局
// ---------------------------------------------------------------------------
static void ComputeLayout(Layout& L) {
    float W = kWinW;
    L.captionMin   = D2D1::RectF(W - 88, 0, W - 44, Ui::TitleBarH);
    L.captionClose = D2D1::RectF(W - 44, 0, W, Ui::TitleBarH);
    // 选项页
    L.dirBox    = D2D1::RectF(kPadX, 158, W - kPadX - 96, 194);
    L.browseBtn = D2D1::RectF(W - kPadX - 88, 158, W - kPadX, 194);
    L.chkDesktop    = D2D1::RectF(kPadX, 208, kPadX + 20, 228);
    L.chkStartMenu  = D2D1::RectF(kPadX, 242, kPadX + 20, 262);
    L.chkAutoStart  = D2D1::RectF(kPadX, 276, kPadX + 20, 296);
    for (int i = 0; i < 4; ++i)
        L.themeChk[i] = D2D1::RectF(kPadX, 344 + i * 32, kPadX + 20, 364 + i * 32);
    // 底部按钮（选项页用 取消/安装，完成页用 完成/立即运行，位置相同）
    const float by = kWinH - 62;
    L.installBtn = D2D1::RectF(W - kPadX - 116, by, W - kPadX, by + Ui::ButtonH);
    L.cancelBtn  = D2D1::RectF(W - kPadX - 116 - 108, by, W - kPadX - 116 - 8, by + Ui::ButtonH);
    L.runBtn     = D2D1::RectF(W - kPadX - 116, by, W - kPadX, by + Ui::ButtonH);
    L.finishBtn  = D2D1::RectF(W - kPadX - 116 - 108, by, W - kPadX - 116 - 8, by + Ui::ButtonH);
    // 进度页
    L.progress   = D2D1::RectF(kPadX, 196, W - kPadX, 202);
}

static int HitTest(float x, float y) {
    const Layout& L = g_s.L;
    auto in = [&](const D2D1_RECT_F& r) {
        return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
    };
    if (in(L.captionClose)) return CID_CLOSE;
    if (in(L.captionMin)) return CID_MIN;
    if (g_s.page == PAGE_OPTIONS) {
        if (in(L.browseBtn)) return CID_BROWSE;
        if (in(L.chkDesktop)) return CID_CHK_DESKTOP;
        if (in(L.chkStartMenu)) return CID_CHK_STARTMENU;
        if (in(L.chkAutoStart)) return CID_CHK_AUTOSTART;
        for (int i = 0; i < (int)g_s.themes.size() && i < 4; ++i)
            if (in(L.themeChk[i])) return CID_THEME0 + i;
        if (in(L.installBtn)) return CID_INSTALL;
        if (in(L.cancelBtn)) return CID_CANCEL;
    } else if (g_s.page == PAGE_DONE) {
        if (in(L.runBtn)) return CID_RUN;
        if (in(L.finishBtn)) return CID_FINISH;
    }
    return CID_NONE;
}

// ---------------------------------------------------------------------------
//  安装步骤
// ---------------------------------------------------------------------------
static bool ExtractResource(int id, const std::wstring& destPath) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!res) return false;
    HGLOBAL h = LoadResource(nullptr, res);
    void* data = LockResource(h);
    DWORD size = SizeofResource(nullptr, res);
    EnsureDir(destPath.substr(0, destPath.find_last_of(L'\\')));
    HANDLE f = CreateFileW(destPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(f, data, size, &written, nullptr);
    CloseHandle(f);
    return ok && written == size;
}

static bool CreateShortcut(const std::wstring& lnk, const std::wstring& target,
                           const std::wstring& args, const std::wstring& desc,
                           const std::wstring& icon) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, (void**)&link)))
        return false;
    link->SetPath(target.c_str());
    if (!args.empty()) link->SetArguments(args.c_str());
    if (!desc.empty()) link->SetDescription(desc.c_str());
    if (!icon.empty()) link->SetIconLocation(icon.c_str(), 0);
    IPersistFile* pf = nullptr;
    bool ok = false;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&pf))) {
        ok = SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
        pf->Release();
    }
    link->Release();
    return ok;
}

// 写"应用和功能"卸载项
static void WriteRegistry() {
    const wchar_t* uninst = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\DynamicWallpapers";
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, uninst, 0, nullptr, 0, KEY_WRITE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
        return;
    std::wstring exe = JoinPath(g_s.installDir, kAppExe);
    std::wstring uninstallCmd = L"\"" + exe + L"\" --uninstall";
    auto setSz = [&](const wchar_t* name, const std::wstring& v) {
        RegSetValueExW(key, name, 0, REG_SZ, (const BYTE*)v.c_str(),
                       (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    };
    setSz(L"DisplayName", kAppDisplay);
    setSz(L"DisplayVersion", kVersion);
    setSz(L"Publisher", kAppDisplay);
    setSz(L"DisplayIcon", exe);
    setSz(L"UninstallString", uninstallCmd);
    setSz(L"InstallLocation", g_s.installDir);
    DWORD one = 1;
    RegSetValueExW(key, L"NoModify", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
    RegSetValueExW(key, L"NoRepair", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
    unsigned long long total = 0;
    for (auto& it : g_s.items) total += it.size;
    DWORD kb = (DWORD)(total / 1024);
    RegSetValueExW(key, L"EstimatedSize", 0, REG_DWORD, (const BYTE*)&kb, sizeof(kb));
    RegCloseKey(key);
}

// 旧版本（TimeWall / 壁纸随时间变化）改名后安装目录、快捷方式、卸载项、自启项
// 都换了名字：旧的那套不管的话，"应用和功能"里会留一个指向不存在文件的条目、
// 开始菜单里多一个旧快捷方式、开机自启会去启动已删掉的旧 exe。
// 旧目录里的 themes\ 先并进新目录（新目录没有的主题才拷，用户自加的不丢）。
static void CopyTree(const std::wstring& src, const std::wstring& dst) {
    EnsureDir(dst);
    WIN32_FIND_DATAW fd = {};
    HANDLE hf = FindFirstFileW(JoinPath(src, L"*").c_str(), &fd);
    if (hf == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        std::wstring s = JoinPath(src, fd.cFileName), d = JoinPath(dst, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) CopyTree(s, d);
        else CopyFileW(s.c_str(), d.c_str(), FALSE);
    } while (FindNextFileW(hf, &fd));
    FindClose(hf);
}

static void CleanLegacyInstall() {
    wchar_t local[MAX_PATH * 2] = {0}, appdata[MAX_PATH * 2] = {0}, desktop[MAX_PATH * 2] = {0};
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local);
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata);
    SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desktop);

    // ① 旧安装目录：themes\ 里新目录没有的主题先搬过去，然后删掉整个旧目录
    std::wstring oldDir = JoinPath(JoinPath(local, L"Programs"), kLegacyAppName);
    if (GetFileAttributesW(oldDir.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::wstring oldThemes = JoinPath(oldDir, L"themes");
        std::wstring newThemes = JoinPath(g_s.installDir, L"themes");
        WIN32_FIND_DATAW fd = {};
        HANDLE hf = FindFirstFileW(JoinPath(oldThemes, L"*").c_str(), &fd);
        if (hf != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                std::wstring dst = JoinPath(newThemes, fd.cFileName);
                if (GetFileAttributesW(dst.c_str()) == INVALID_FILE_ATTRIBUTES)
                    CopyTree(JoinPath(oldThemes, fd.cFileName), dst);
            } while (FindNextFileW(hf, &fd));
            FindClose(hf);
        }
        std::wstring cmd = L"cmd.exe /c rmdir /s /q \"" + oldDir + L"\"";
        STARTUPINFOW si = {sizeof(si)};
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = {};
        if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                           nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 8000);    // 等它删完，别留个半拉的旧目录
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    }
    // ② 旧快捷方式（开始菜单文件夹 + 桌面）
    std::wstring smDir = JoinPath(JoinPath(appdata, L"Microsoft\\Windows\\Start Menu\\Programs"),
                                  kLegacyDisplay);
    DeleteFileW(JoinPath(smDir, std::wstring(kLegacyDisplay) + L".lnk").c_str());
    RemoveDirectoryW(smDir.c_str());
    DeleteFileW(JoinPath(desktop, std::wstring(kLegacyDisplay) + L".lnk").c_str());
    // ③ 旧卸载项 / 旧自启项
    RegDeleteTreeW(HKEY_CURRENT_USER,
                   L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\TimeWall");
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        RegDeleteValueW(key, kLegacyAppName);
        RegCloseKey(key);
    }
}

// 执行安装（分步更新界面；每步后处理一次消息以保持界面响应）
static void RunInstall() {
    g_s.totalCount = 0;
    for (auto& it : g_s.items) {
        if (it.kind == L"theme") {
            bool inc = false;
            for (auto& t : g_s.themes) if (t.name == it.theme && t.include) inc = true;
            if (!inc) continue;
        }
        g_s.totalCount++;
    }
    g_s.doneCount = 0;
    g_s.errorCount = 0;
    g_s.errorText.clear();
    g_s.page = PAGE_INSTALLING;

    auto step = [&](const PayloadItem& it) {
        g_s.currentFile = it.rel;
        RenderFrame();
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!ExtractResource(it.id, JoinPath(g_s.installDir, it.rel))) {
            g_s.errorCount++;
            if (g_s.errorCount == 1) g_s.errorText = it.rel;   // 完成页只放得下一个文件名，其余用个数表达
        }
        g_s.doneCount++;
    };

    // 更新现有安装前，先请运行中的程序退出（新旧两个类名都查：老进程可能还在跑）
    for (const wchar_t* cls : { L"DynamicWallpapersMainWindow", L"TimeWallMainWindow" }) {
        if (HWND other = FindWindowW(cls, nullptr)) {
            PostMessageW(other, WM_APP_QUIT, 0, 0);
            Sleep(600);
        }
    }
    EnsureDir(g_s.installDir);
    CleanLegacyInstall();
    for (auto& it : g_s.items) {
        if (it.kind == L"theme") {
            bool inc = false;
            for (auto& t : g_s.themes) if (t.name == it.theme && t.include) inc = true;
            if (!inc) continue;
        }
        step(it);
    }

    // 快捷方式（带 --show：点快捷方式显示主界面）
    std::wstring exe = JoinPath(g_s.installDir, kAppExe);
    wchar_t buf[MAX_PATH * 2] = {0};
    if (g_s.desktopLnk && SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, buf))) {
        std::wstring lnk = JoinPath(buf, std::wstring(kAppDisplay) + L".lnk");
        CreateShortcut(lnk, exe, L"--show", L"按时间自动切换桌面壁纸", exe);
    }
    if (g_s.startMenuLnk &&
        SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buf))) {
        std::wstring dir = JoinPath(JoinPath(buf, L"Microsoft\\Windows\\Start Menu\\Programs"),
                                    kAppDisplay);
        EnsureDir(dir);
        CreateShortcut(JoinPath(dir, std::wstring(kAppDisplay) + L".lnk"), exe, L"--show",
                       L"按时间自动切换桌面壁纸", exe);
    }
    // 开机自启
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        if (g_s.autoStart) {
            std::wstring cmd = L"\"" + exe + L"\" --autostart";   // 开机静默进托盘
            RegSetValueExW(key, kAppDisplay, 0, REG_SZ, (const BYTE*)cmd.c_str(),
                           (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
        }
        RegDeleteValueW(key, kLegacyAppName);      // 老自启项清掉，注册表里只留一条
        RegCloseKey(key);
    }
    WriteRegistry();
    g_s.finished = true;
    g_s.page = PAGE_DONE;
}

// ---------------------------------------------------------------------------
//  绘制
// ---------------------------------------------------------------------------
static void DrawCaption(Gfx& g, const Palette& p) {
    // 应用标记与主程序同一份 logo（exe 资源 IDR_LOGO_PNG）；
    // 取不到时退回手绘的"左日右月"
    float ix = 28, iy = Ui::TitleBarH / 2;
    if (!g_s.logoBmp && !g_s.logoTried) {
        g_s.logoTried = true;
        g_s.logoBmp = g.loadImageFromResource(IDR_LOGO_PNG, 64);
    }
    if (g_s.logoBmp) {
        float s = 20.0f;
        D2D1_RECT_F dst = D2D1::RectF(ix - s / 2, iy - s / 2, ix + s / 2, iy + s / 2);
        g.dc()->DrawBitmap(g_s.logoBmp, dst, 1.0f,
                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
    } else {
        ID2D1SolidColorBrush* b = g.brush(ColHex(0xFFB900));
        if (b) g.dc()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(ix - 2.5f, iy), 6.5f, 6.5f), b);
        ID2D1SolidColorBrush* mc = g.brush(p.textPrimary);
        if (mc) {
            g.dc()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(ix + 4.0f, iy), 6.5f, 6.5f), mc);
            ID2D1SolidColorBrush* cut = g.brush(p.windowBgTop);
            if (cut) g.dc()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(ix + 6.6f, iy - 1.6f), 5.6f, 5.6f), cut);
        }
    }
    IDWriteTextFormat* f = g.format(FONT_UI, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g.text(std::wstring(kAppDisplay) + L" 安装程序", f, D2D1::RectF(48, 10, 420, 30), p.textPrimary);
    UiCaptionButton(g, g_s.L.captionMin, Ui::CAP_MIN, g_s.hot == CID_MIN, g_s.pressed == CID_MIN, p);
    UiCaptionButton(g, g_s.L.captionClose, Ui::CAP_CLOSE, g_s.hot == CID_CLOSE, g_s.pressed == CID_CLOSE, p);
}

void RenderFrame() {
    Gfx& g = g_s.gfx;
    const Palette& p = g_s.pal;
    if (!g.begin()) return;
    g.dc()->Clear(p.windowBgTop);
    // 背景渐变
    ID2D1GradientStopCollection* stops = nullptr;
    D2D1_GRADIENT_STOP gs[2] = {{0.0f, p.windowBgTop}, {1.0f, p.windowBgBottom}};
    if (SUCCEEDED(g.dc()->CreateGradientStopCollection(gs, 2, &stops)) && stops) {
        ID2D1LinearGradientBrush* bg = nullptr;
        D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES props =
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, kWinH));
        if (SUCCEEDED(g.dc()->CreateLinearGradientBrush(props, stops, &bg)) && bg) {
            g.dc()->FillRectangle(D2D1::RectF(0, 0, kWinW, kWinH), bg);
            bg->Release();
        }
        stops->Release();
    }
    DrawCaption(g, p);

    IDWriteTextFormat* fTitle = g.format(FONT_UI, 21.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    IDWriteTextFormat* fSub   = g.format(FONT_UI, 13.0f);
    IDWriteTextFormat* fSec   = g.format(FONT_UI, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);

    if (g_s.page == PAGE_OPTIONS) {
        g.text(L"安装 " + std::wstring(kAppDisplay) + L" " + kVersion, fTitle,
               D2D1::RectF(kPadX, 62, kWinW - kPadX, 96), p.textPrimary);
        g.text(L"桌面壁纸按时间自动切换：白天一张、夜间一张。无需管理员权限。", fSub,
               D2D1::RectF(kPadX, 96, kWinW - kPadX, 120), p.textSecondary);

        g.text(L"安装位置", fSec, D2D1::RectF(kPadX, 132, 200, 152), p.textSecondary);
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(g_s.L.dirBox, 6, 6);
        g.fillRound(rr, p.ctrlFill);
        g.strokeRound(rr, p.ctrlStroke, 1.0f);
        IDWriteTextFormat* fPath = g.format(FONT_UI, 12.5f);
        g.text(g_s.installDir, fPath,
               D2D1::RectF(g_s.L.dirBox.left + 12, g_s.L.dirBox.top + 10,
                           g_s.L.dirBox.right - 10, g_s.L.dirBox.bottom - 8), p.textPrimary);
        UiButton(g, g_s.L.browseBtn, L"浏览…", p, false, g_s.hot == CID_BROWSE,
                 g_s.pressed == CID_BROWSE);

        UiCheckbox(g, g_s.L.chkDesktop.left, g_s.L.chkDesktop.top, L"创建桌面快捷方式",
                   g_s.desktopLnk, g_s.hot == CID_CHK_DESKTOP, p);
        UiCheckbox(g, g_s.L.chkStartMenu.left, g_s.L.chkStartMenu.top, L"创建开始菜单快捷方式",
                   g_s.startMenuLnk, g_s.hot == CID_CHK_STARTMENU, p);
        UiCheckbox(g, g_s.L.chkAutoStart.left, g_s.L.chkAutoStart.top, L"开机自动启动",
                   g_s.autoStart, g_s.hot == CID_CHK_AUTOSTART, p);

        g.text(L"附带壁纸主题", fSec, D2D1::RectF(kPadX, 314, 300, 334), p.textSecondary);
        for (int i = 0; i < (int)g_s.themes.size() && i < 4; ++i) {
            std::wstring label = g_s.themes[i].name + L"（" +
                std::to_wstring(g_s.themes[i].imageCount) + L" 张图片 · " +
                FormatSize(g_s.themes[i].size) + L"）";
            UiCheckbox(g, g_s.L.themeChk[i].left, g_s.L.themeChk[i].top, label,
                       g_s.themes[i].include, g_s.hot == CID_THEME0 + i, p);
        }

        UiButton(g, g_s.L.cancelBtn, L"取消", p, false, g_s.hot == CID_CANCEL,
                 g_s.pressed == CID_CANCEL);
        UiButton(g, g_s.L.installBtn, L"安装", p, true, g_s.hot == CID_INSTALL,
                 g_s.pressed == CID_INSTALL, true, L"\uE896");
    } else if (g_s.page == PAGE_INSTALLING) {
        g.text(L"正在安装…", fTitle, D2D1::RectF(kPadX, 78, kWinW - kPadX, 112), p.textPrimary);
        float prog = g_s.totalCount ? (float)g_s.doneCount / g_s.totalCount : 0.0f;
        UiProgress(g, g_s.L.progress, prog, p);
        std::wstring line = g_s.currentFile.empty() ? L"准备中…" : g_s.currentFile;
        g.text(line, fSub, D2D1::RectF(kPadX, 220, kWinW - kPadX, 244), p.textSecondary);
        wchar_t buf[64];
        swprintf(buf, 64, L"%d / %d", g_s.doneCount, g_s.totalCount);
        IDWriteTextFormat* fRight = g.format(FONT_UI, 12.5f);
        g.textOptical(buf, fRight, D2D1::RectF(kWinW / 2, 220, kWinW - kPadX, 244),
                      p.textSecondary, DWRITE_TEXT_ALIGNMENT_TRAILING);
    } else {
        g.text(g_s.errorText.empty() ? L"安装完成" : L"安装完成（有错误）", fTitle,
               D2D1::RectF(kPadX, 78, kWinW - kPadX, 112), p.textPrimary);
        IDWriteTextFormat* fBody = g.format(FONT_UI, 13.0f);
        std::wstring body = g_s.errorText.empty()
            ? L"已经装好了。程序会常驻托盘，按设定的时间自动切换壁纸。"
            : (L"有 " + std::to_wstring(g_s.errorCount) + L" 个文件写入失败：" + g_s.errorText);
        g.text(body, fBody, D2D1::RectF(kPadX, 124, kWinW - kPadX, 148),
               g_s.errorText.empty() ? p.textSecondary : p.accent);
        g.text(L"安装位置：" + g_s.installDir, fBody,
               D2D1::RectF(kPadX, 156, kWinW - kPadX, 180), p.textSecondary);
        g.text(L"提示：右键托盘图标可以随时切换主题、暂停切换或退出。", fBody,
               D2D1::RectF(kPadX, 184, kWinW - kPadX, 208), p.textSecondary);

        UiButton(g, g_s.L.finishBtn, L"完成", p, false, g_s.hot == CID_FINISH,
                 g_s.pressed == CID_FINISH);
        UiButton(g, g_s.L.runBtn, L"立即运行", p, true, g_s.hot == CID_RUN,
                 g_s.pressed == CID_RUN, true, L"\uE768");
    }
    g.end();
}

// ---------------------------------------------------------------------------
//  窗口过程
// ---------------------------------------------------------------------------
static void PickFolder() {
    IFileDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IFileDialog, (void**)&dlg)))
        return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"选择安装位置");
    if (SUCCEEDED(dlg->Show(g_s.hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                g_s.installDir = JoinPath(path, kAppDisplay);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_NCCALCSIZE:
        if (w) return 0;               // 客户区铺满整个窗口（去系统标题栏）
        break;
    case WM_NCHITTEST: {
        POINT pt = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ScreenToClient(h, &pt);
        float s = g_s.gfx.scale();
        float x = pt.x / s, y = pt.y / s;
        const Layout& L = g_s.L;
        auto in = [&](const D2D1_RECT_F& r) {
            return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
        };
        if (in(L.captionMin) || in(L.captionClose) || y >= Ui::TitleBarH) return HTCLIENT;
        return HTCAPTION;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        RenderFrame();
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        float s = g_s.gfx.scale();
        int id = HitTest(GET_X_LPARAM(l) / s, GET_Y_LPARAM(l) / s);
        if (id != g_s.hot) {
            g_s.hot = id;
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_s.hot = CID_NONE;
        g_s.pressed = CID_NONE;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        float s = g_s.gfx.scale();
        g_s.pressed = HitTest(GET_X_LPARAM(l) / s, GET_Y_LPARAM(l) / s);
        SetCapture(h);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        float s = g_s.gfx.scale();
        int id = HitTest(GET_X_LPARAM(l) / s, GET_Y_LPARAM(l) / s);
        ReleaseCapture();
        int pressed = g_s.pressed;
        g_s.pressed = CID_NONE;
        if (id != pressed || id == CID_NONE) {
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        switch (id) {
        case CID_MIN: ShowWindow(h, SW_MINIMIZE); break;
        case CID_CLOSE:
            if (g_s.page == PAGE_INSTALLING) break;   // 安装中不许关：文件写到一半没有"取消"语义
            DestroyWindow(h);
            break;
        case CID_CANCEL: DestroyWindow(h); break;
        case CID_BROWSE: PickFolder(); break;
        case CID_CHK_DESKTOP: g_s.desktopLnk = !g_s.desktopLnk; break;
        case CID_CHK_STARTMENU: g_s.startMenuLnk = !g_s.startMenuLnk; break;
        case CID_CHK_AUTOSTART: g_s.autoStart = !g_s.autoStart; break;
        case CID_INSTALL:
            if (g_s.items.empty()) {
                MessageBoxW(h, L"安装包数据缺失（payload 资源为空）", kAppDisplay,
                            MB_OK | MB_ICONERROR);
                break;
            }
            RunInstall();
            break;
        case CID_RUN: {
            std::wstring exe = JoinPath(g_s.installDir, kAppExe);
            ShellExecuteW(nullptr, L"open", exe.c_str(), L"--show", g_s.installDir.c_str(),
                          SW_SHOWNORMAL);
            DestroyWindow(h);
            return 0;
        }
        case CID_FINISH: DestroyWindow(h); return 0;
        default:
            if (id >= CID_THEME0 && id <= CID_THEME3) {
                int i = id - CID_THEME0;
                if (i < (int)g_s.themes.size()) g_s.themes[i].include = !g_s.themes[i].include;
            }
            break;
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_APP_QUIT:                  // 主程序卸载/更新时请求本窗口退出
        DestroyWindow(h);
        return 0;
    case WM_CLOSE:
        if (g_s.page == PAGE_INSTALLING) return 0;   // 与 CID_CLOSE 同款拦截（Alt+F4 / 系统菜单）
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

int wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    LoadManifest();
    g_s.installDir = DefaultInstallDir();

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    UINT dpi = GetDpiForSystem();
    int w = (int)(kWinW * dpi / 96.0f + 0.5f);
    int h = (int)(kWinH * dpi / 96.0f + 0.5f);
    int sx = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, kWindowClass, L"Dynamic Wallpapers 安装程序",
                                WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX,
                                sx, sy, w, h, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    g_s.hwnd = hwnd;

    int corner = 2;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    int dark = 0;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    g_s.pal = MakePalette(SystemUsesDarkMode(), SystemAccentColor());
    if (!g_s.gfx.init(hwnd)) {
        MessageBoxW(nullptr, L"图形初始化失败", kAppDisplay, MB_OK | MB_ICONERROR);
        return 2;
    }
    g_s.gfx.resize(w, h, dpi);
    ComputeLayout(g_s.L);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_s.gfx.shutdown();
    CoUninitialize();
    return 0;
}
