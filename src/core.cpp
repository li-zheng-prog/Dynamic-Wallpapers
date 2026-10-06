// ============================================================================
//  core.cpp — 业务核心实现
// ============================================================================
#include "core.h"
#include <shobjidl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <cwchar>
#include <algorithm>

// FOF_* 是稳定 ABI 的位标志：0x40=可撤销(移到回收站) 0x10=不确认 0x04=静默 0x400=无错误 UI。
// MinGW 在较新的 NTDDI_VERSION 下可能不暴露，缺了就用标准值补上。
#ifndef FOF_ALLOWUNDO
#define FOF_ALLOWUNDO 0x0040
#endif
#ifndef FOF_NOCONFIRMATION
#define FOF_NOCONFIRMATION 0x0010
#endif
#ifndef FOF_SILENT
#define FOF_SILENT 0x0004
#endif
#ifndef FOF_NOERRORUI
#define FOF_NOERRORUI 0x0400
#endif

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kAppName = L"Dynamic Wallpapers";
static const wchar_t* kLegacyName = L"TimeWall";              // 旧内部名：配置目录、自启项、卸载项
static const wchar_t* kIniSection = L"Dynamic Wallpapers";    // config.ini 的段名
static const wchar_t* kLegacyIniSection = L"TimeWall";        // 旧段名：读到就迁到新段

// ---------------------------------------------------------------------------
//  路径工具
// ---------------------------------------------------------------------------
std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    std::wstring r = a;
    if (r.back() != L'\\' && r.back() != L'/') r += L'\\';
    if (!b.empty() && (b.front() == L'\\' || b.front() == L'/')) return r + b.substr(1);
    return r + b;
}

bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 2] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    return buf;
}

std::wstring ExeDir() {
    std::wstring p = ExePath();
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"" : p.substr(0, s);
}

// 更名迁移（2026-09-29）：老版本叫 TimeWall，配置在 %APPDATA%\TimeWall。首次以
// 新名字启动时把整个目录搬到 %APPDATA%\Dynamic Wallpapers——设置与自加主题不丢。
// 只做一次、只在新目录不存在时才搬；搬不动就退一步只拷 config.ini，
// 主题由 ThemeDirs() 里的旧路径继续兜底扫描。
static void MigrateLegacyAppData(const std::wstring& appdata, const std::wstring& newDir) {
    static bool done = false;
    if (done) return;
    done = true;
    if (GetFileAttributesW(newDir.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    std::wstring oldDir = JoinPath(appdata, kLegacyName);
    if (GetFileAttributesW(oldDir.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    if (!MoveFileExW(oldDir.c_str(), newDir.c_str(), MOVEFILE_COPY_ALLOWED)) {
        CreateDirectoryW(newDir.c_str(), nullptr);
        CopyFileW(JoinPath(oldDir, L"config.ini").c_str(),
                  JoinPath(newDir, L"config.ini").c_str(), FALSE);
    }
}

std::wstring AppDataDir() {
    wchar_t buf[MAX_PATH * 2] = {0};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buf))) return L"";
    std::wstring d = JoinPath(buf, kAppName);
    MigrateLegacyAppData(buf, d);
    CreateDirectoryW(d.c_str(), nullptr);   // 不存在则创建（已存在返回错误，忽略）
    return d;
}

std::wstring ConfigPath() { return JoinPath(AppDataDir(), L"config.ini"); }

// 主题搜索目录：1) 程序（安装）目录 themes\（随程序安装的主题 + 用户自加主题）
//               2) %APPDATA%\Dynamic Wallpapers\themes\（用户目录）
//               3) %APPDATA%\TimeWall\themes\（旧路径，迁移失败时兜底）
std::vector<std::wstring> ThemeDirs() {
    std::vector<std::wstring> dirs;
    dirs.push_back(JoinPath(ExeDir(), L"themes"));
    std::wstring user = JoinPath(AppDataDir(), L"themes");
    wchar_t buf[MAX_PATH * 2] = {0};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buf))) {
        std::wstring legacy = JoinPath(JoinPath(buf, kLegacyName), L"themes");
        if (std::find(dirs.begin(), dirs.end(), legacy) == dirs.end()) dirs.push_back(legacy);
    }
    if (std::find(dirs.begin(), dirs.end(), user) == dirs.end()) dirs.push_back(user);
    return dirs;
}

// ---------------------------------------------------------------------------
//  配置读写（INI；系统 API 自动处理 UTF-16）
// ---------------------------------------------------------------------------
static std::wstring ReadIniRaw(const wchar_t* section, const wchar_t* key) {
    wchar_t buf[512] = {0};
    GetPrivateProfileStringW(section, key, L"", buf, 512, ConfigPath().c_str());
    return buf;
}

// 新段读不到时去老段（[TimeWall]）读——老用户的 config.ini 不用手动改，
// SaveConfig 会把值写进新段并删掉老段。
static int GetInt(const wchar_t* key, int def) {
    std::wstring s = ReadIniRaw(kIniSection, key);
    if (s.empty()) s = ReadIniRaw(kLegacyIniSection, key);
    return s.empty() ? def : _wtoi(s.c_str());
}
static std::wstring GetStr(const wchar_t* key, const std::wstring& def) {
    std::wstring s = ReadIniRaw(kIniSection, key);
    if (s.empty()) s = ReadIniRaw(kLegacyIniSection, key);
    return s.empty() ? def : s;
}
static void PutInt(const wchar_t* key, int v) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%d", v);
    WritePrivateProfileStringW(kIniSection, key, buf, ConfigPath().c_str());
}
static void PutStr(const wchar_t* key, const std::wstring& v) {
    WritePrivateProfileStringW(kIniSection, key, v.c_str(), ConfigPath().c_str());
}

Config LoadConfig() {
    Config c;
    c.theme        = GetStr(L"Theme", L"");
    c.dayStart     = GetInt(L"DayStart", 6 * 60);
    c.nightStart   = GetInt(L"NightStart", 18 * 60);
    c.autoSwitch   = GetInt(L"AutoSwitch", 1) != 0;
    c.switchMode   = GetInt(L"SwitchMode", 0);
    if (c.switchMode < 0 || c.switchMode > 1) c.switchMode = 0;
    c.startOnBoot  = GetInt(L"StartOnBoot", 0) != 0;
    c.minimizeToTray = GetInt(L"MinimizeToTray", 1) != 0;
    c.wallpaperStyle = GetInt(L"WallpaperStyle", 4);
    c.appearance   = GetInt(L"Appearance", 0);
    if (c.appearance < 0 || c.appearance > 2) c.appearance = 0;
    // 容错：时间必须在 0..1439，且不能相同
    auto clamp = [](int v) { return v < 0 ? 0 : (v > 1439 ? 1439 : v); };
    c.dayStart = clamp(c.dayStart);
    c.nightStart = clamp(c.nightStart);
    if (c.dayStart == c.nightStart) c.nightStart = (c.dayStart + 720) % 1440;
    if (c.wallpaperStyle < 0 || c.wallpaperStyle > 5) c.wallpaperStyle = 4;
    return c;
}

void SaveConfig(const Config& c) {
    PutStr(L"Theme", c.theme);
    PutInt(L"DayStart", c.dayStart);
    PutInt(L"NightStart", c.nightStart);
    PutInt(L"AutoSwitch", c.autoSwitch ? 1 : 0);
    PutInt(L"SwitchMode", c.switchMode);
    PutInt(L"StartOnBoot", c.startOnBoot ? 1 : 0);
    PutInt(L"MinimizeToTray", c.minimizeToTray ? 1 : 0);
    PutInt(L"WallpaperStyle", c.wallpaperStyle);
    PutInt(L"Appearance", c.appearance);
    // 值都写进新段后删掉老段（[TimeWall]），config.ini 里只留一份
    WritePrivateProfileStringW(kLegacyIniSection, nullptr, nullptr, ConfigPath().c_str());
}

// ---------------------------------------------------------------------------
//  主题扫描
// ---------------------------------------------------------------------------
static bool IsImageFile(const std::wstring& name) {
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = name.substr(dot);
    for (auto& c : ext) c = towlower(c);
    return ext == L".jpg" || ext == L".jpeg" || ext == L".png" || ext == L".bmp";
}

// 从文件名里取末尾数字（The Desert_5.jpg -> 5），用于自然排序与索引匹配
static int TrailingNumber(const std::wstring& file) {
    size_t dot = file.find_last_of(L'.');
    std::wstring stem = dot == std::wstring::npos ? file : file.substr(0, dot);
    size_t p = stem.find_last_of(L'_');
    if (p == std::wstring::npos) return 0;
    return (int)wcstol(stem.substr(p + 1).c_str(), nullptr, 10);
}

static void ListImages(const std::wstring& folder, std::vector<std::wstring>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(JoinPath(folder, L"*.*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring n = fd.cFileName;
        if (IsImageFile(n)) out.push_back(n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) {
        int na = TrailingNumber(a), nb = TrailingNumber(b);
        if (na != nb) return na < nb;
        return a < b;
    });
}

// ---- theme.json 的极简解析（只认我们需要的字段）----
static bool JsonString(const std::wstring& js, const wchar_t* key, std::wstring& out) {
    std::wstring pat = std::wstring(L"\"") + key + L"\"";
    size_t p = js.find(pat);
    if (p == std::wstring::npos) return false;
    p = js.find(L':', p + pat.size());
    if (p == std::wstring::npos) return false;
    p = js.find(L'"', p);
    if (p == std::wstring::npos) return false;
    size_t q = js.find(L'"', p + 1);
    if (q == std::wstring::npos) return false;
    out = js.substr(p + 1, q - p - 1);
    return true;
}

static bool JsonIntArray(const std::wstring& js, const wchar_t* key, std::vector<int>& out) {
    std::wstring pat = std::wstring(L"\"") + key + L"\"";
    size_t p = js.find(pat);
    if (p == std::wstring::npos) return false;
    p = js.find(L'[', p);
    if (p == std::wstring::npos) return false;
    size_t q = js.find(L']', p);
    if (q == std::wstring::npos) return false;
    std::wstring body = js.substr(p + 1, q - p - 1);
    int cur = 0; bool has = false;
    for (wchar_t ch : body) {
        if (ch >= L'0' && ch <= L'9') { cur = cur * 10 + (ch - L'0'); has = true; }
        else { if (has) { out.push_back(cur); cur = 0; has = false; } }
    }
    if (has) out.push_back(cur);
    return !out.empty();
}

// 把 theme.json 的 imageFilename 模板（如 "The Desert_*.jpg"）解析成真实文件名
static std::wstring ResolveImageName(const std::wstring& folder, const std::wstring& pattern,
                                     int index, const std::vector<std::wstring>& files) {
    if (!pattern.empty()) {
        std::wstring name = pattern;
        size_t star = name.find(L'*');
        if (star != std::wstring::npos) {
            name = name.substr(0, star) + std::to_wstring(index) + name.substr(star + 1);
            if (FileExists(JoinPath(folder, name))) return name;
        } else if (FileExists(JoinPath(folder, name))) {
            return name;
        }
        // 模板没匹配上：退化为"文件名含该数字"
        for (auto& f : files)
            if (TrailingNumber(f) == index) return f;
    }
    for (auto& f : files)
        if (TrailingNumber(f) == index) return f;
    return L"";
}

static bool LoadTheme(const std::wstring& folder, const std::wstring& folderName, ThemeInfo& t) {
    std::vector<std::wstring> files;
    ListImages(folder, files);
    t.folder = folder;
    t.name = folderName;
    t.imageCount = (int)files.size();
    if (files.empty()) return false;

    std::wstring json;
    std::wstring jsonPath = JoinPath(folder, L"theme.json");
    if (FileExists(jsonPath)) {
        HANDLE h = CreateFileW(jsonPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD size = GetFileSize(h, nullptr);
            if (size > 0 && size < 1024 * 1024) {
                std::string buf(size, 0);
                DWORD read = 0;
                ReadFile(h, &buf[0], size, &read, nullptr);
                int wlen = MultiByteToWideChar(CP_UTF8, 0, buf.data(), (int)read, nullptr, 0);
                json.resize(wlen);
                MultiByteToWideChar(CP_UTF8, 0, buf.data(), (int)read, &json[0], wlen);
                // 去掉 UTF-8 BOM 转成的 U+FEFF
                if (!json.empty() && json[0] == 0xFEFF) json.erase(0, 1);
            }
            CloseHandle(h);
        }
    }

    std::wstring display, pattern, dayName, nightName;
    if (!json.empty()) {
        JsonString(json, L"displayName", display);
        JsonString(json, L"imageFilename", pattern);
    }
    if (!display.empty()) t.name = display;

    std::vector<int> dayList, nightList, sunriseList, sunsetList;
    if (!json.empty()) {
        JsonIntArray(json, L"dayImageList", dayList);
        JsonIntArray(json, L"nightImageList", nightList);
        JsonIntArray(json, L"sunriseImageList", sunriseList);
        JsonIntArray(json, L"sunsetImageList", sunsetList);
    }

    // 取值规则：日间 = dayImageList 第一张（没有则用 sunrise 最后一张）；
    //           夜间 = nightImageList 第一张（没有则用 sunset 最后一张）
    if (!dayList.empty())        dayName = ResolveImageName(folder, pattern, dayList.front(), files);
    else if (!sunriseList.empty()) dayName = ResolveImageName(folder, pattern, sunriseList.back(), files);
    if (!nightList.empty())      nightName = ResolveImageName(folder, pattern, nightList.front(), files);
    else if (!sunsetList.empty()) nightName = ResolveImageName(folder, pattern, sunsetList.back(), files);

    // 没有 theme.json（或字段缺失）时：第一张当白天，最后一张当夜晚
    if (dayName.empty()) dayName = files.front();
    if (nightName.empty()) nightName = files.size() > 1 ? files.back() : files.front();

    t.dayImage = JoinPath(folder, dayName);
    t.nightImage = JoinPath(folder, nightName);
    return true;
}

std::vector<ThemeInfo> ScanThemes() {
    std::vector<ThemeInfo> out;
    for (auto& dir : ThemeDirs()) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(JoinPath(dir, L"*.*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            ThemeInfo t;
            if (LoadTheme(JoinPath(dir, name), name, t) && t.valid()) {
                bool dup = false;   // 同名主题只保留第一个（exe 目录优先）
                for (auto& e : out) if (e.name == t.name) dup = true;
                if (!dup) out.push_back(t);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(out.begin(), out.end(), [](const ThemeInfo& a, const ThemeInfo& b) {
        return a.name < b.name;
    });
    return out;
}

const ThemeInfo* FindTheme(const std::vector<ThemeInfo>& list, const std::wstring& name) {
    for (auto& t : list) if (t.name == name) return &t;
    // 唯一调用方是 --delete-theme=<名字>：退回"第一个"会让名字打错时
    // 把别的主题移进回收站；返回码 2（没这个主题）也会变成不可达。
    return nullptr;
}

// ---------------------------------------------------------------------------
//  用户自定义主题（"添加主题"）
// ---------------------------------------------------------------------------
// 目录可写性探测：真写一个小文件试试（装在 Program Files 又没权限时会被拒绝）
static void EnsureDir(const std::wstring& dir);      // 定义见下方
static bool DirWritable(const std::wstring& dir) {
    EnsureDir(dir);
    std::wstring probe = JoinPath(dir, L".write_test");
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

// 用户主题目录 = 程序（安装）目录下的 themes\：添加主题时把图片复制进这个独立
// 文件夹，放好之后与原始图片再无关系。程序目录不可写（Program Files）时自动
// 退回 %APPDATA%\Dynamic Wallpapers\themes。
std::wstring UserThemeDir() {
    std::wstring prog = JoinPath(ExeDir(), L"themes");
    if (DirWritable(prog)) return prog;
    std::wstring app = JoinPath(AppDataDir(), L"themes");
    EnsureDir(app);
    return app;
}

// 逐级创建目录（已存在时忽略错误）
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

std::vector<std::wstring> ListImagesIn(const std::wstring& folder) {
    std::vector<std::wstring> names, full;
    ListImages(folder, names);
    for (auto& n : names) full.push_back(JoinPath(folder, n));
    return full;
}

bool DiscoverThemeImages(const std::wstring& folder, const std::wstring& name,
                         std::wstring& dayImage, std::wstring& nightImage) {
    ThemeInfo t;
    if (!LoadTheme(folder, name, t)) return false;
    dayImage = t.dayImage;
    nightImage = t.nightImage;
    return !dayImage.empty() && !nightImage.empty();
}

static bool DirExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// 名字是否已被任何主题目录占用：文件夹名（不区分大小写）或某个主题的显示名。
// ScanThemes 按显示名去重、exe 目录优先——只查自己目录的话，撞名的主题会被
// 去重顶掉，看起来"添加/改名成功却找不到"。exceptFolder 是"自己"，跳过。
static bool ThemeNameTaken(const std::wstring& name, const std::wstring& exceptFolder) {
    for (const std::wstring& dir : ThemeDirs()) {
        WIN32_FIND_DATAW fd = {};
        HANDLE h = FindFirstFileW(JoinPath(dir, L"*.*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            std::wstring fn = fd.cFileName;
            if (fn == L"." || fn == L"..") continue;
            std::wstring full = JoinPath(dir, fn);
            if (!exceptFolder.empty() && _wcsicmp(exceptFolder.c_str(), full.c_str()) == 0) continue;
            if (_wcsicmp(fn.c_str(), name.c_str()) == 0) { FindClose(h); return true; }
            ThemeInfo t;
            if (LoadTheme(full, fn, t) && t.name == name) { FindClose(h); return true; }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return false;
}

bool ImportTheme(const std::wstring& name, const std::wstring& dayImage,
                 const std::wstring& nightImage, std::wstring& outName, std::wstring* err) {
    // 失败统一从这里走，让界面/命令行拿到"为什么失败"而不是一句笼统的提示
    auto fail = [&](const std::wstring& why) {
        if (err) *err = why;
        return false;
    };
    if (dayImage.empty() || nightImage.empty()) return fail(L"没有选够两张图片");
    // 名字校验与"重命名主题"同一套规则：否则 "..\..\x" 会把文件写到主题目录之外
    std::wstring nameErr;
    if (!ThemeNameValid(name, nameErr)) return fail(nameErr);
    std::wstring root = UserThemeDir();
    if (root.empty()) return fail(L"主题目录不可用");
    EnsureDir(root);

    // 重名时自动加后缀，避免覆盖已有主题（占用判断覆盖所有主题目录，见 ThemeNameTaken）
    std::wstring finalName = name;
    for (int i = 2; ThemeNameTaken(finalName, L""); ++i) {
        wchar_t suffix[16];
        swprintf(suffix, 16, L" (%d)", i);
        finalName = name + suffix;
        if (i > 99) return fail(L"同名主题太多了，换个名字");
    }
    std::wstring target = JoinPath(root, finalName);
    if (!CreateDirectoryW(target.c_str(), nullptr))
        return fail(L"无法创建主题文件夹（程序目录\\themes 不可写，且 %APPDATA% 也失败）");

    // 复制两张图（统一用 .jpg 后缀，内容仍按实际格式被解码）
    auto copyAs = [&](const std::wstring& src, int index) -> bool {
        std::wstring dst = JoinPath(target, finalName + L"_" + std::to_wstring(index) + L".jpg");
        return CopyFileW(src.c_str(), dst.c_str(), FALSE) != FALSE;
    };
    if (!copyAs(dayImage, 1)) return fail(L"复制浅色图片失败（原文件可能已被移动或删除）");
    if (!copyAs(nightImage, 2)) return fail(L"复制深色图片失败（原文件可能已被移动或删除）");

    // 生成 theme.json（WDD 兼容格式，日间=1，夜间=2），UTF-8 编码写入
    std::wstring json = L"{\r\n"
                        L"  \"imageFilename\": \"" + finalName + L"_*.jpg\",\r\n"
                        L"  \"displayName\": \"" + finalName + L"\",\r\n"
                        L"  \"dayImageList\": [1],\r\n"
                        L"  \"nightImageList\": [2]\r\n"
                        L"}\r\n";
    std::wstring jsonPath = JoinPath(target, L"theme.json");
    HANDLE h = CreateFileW(jsonPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return fail(L"写入 theme.json 失败");
    int n = WideCharToMultiByte(CP_UTF8, 0, json.c_str(), (int)json.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, json.c_str(), (int)json.size(), &utf8[0], n, nullptr, nullptr);
    DWORD written = 0;
    BOOL ok = WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
    CloseHandle(h);
    if (!ok) return fail(L"写入 theme.json 失败");

    outName = finalName;
    return true;
}

// ---------------------------------------------------------------------------
//  主题重命名：改文件夹名（主题的身份），并同步 theme.json 的 displayName，
//  否则重新扫描时显示的还是老名字。图片文件名不动（imageFilename 是通配模式）。
// ---------------------------------------------------------------------------
bool ThemeNameValid(const std::wstring& name, std::wstring& err) {
    if (name.empty()) { err = L"名字不能为空"; return false; }
    if (name.size() > 60) { err = L"名字太长了（最多 60 个字符）"; return false; }
    for (wchar_t c : name) {
        if (c < 32 || wcschr(L"\\/:*?\"<>|", c)) {
            err = L"名字里不能有 \\ / : * ? \" < > | 这些字符";
            return false;
        }
    }
    if (name == L"." || name == L"..") { err = L"这个名字不能用"; return false; }
    return true;
}

bool ThemeNameAvailable(const std::wstring& name, const std::wstring& exceptFolder, std::wstring& err) {
    if (!ThemeNameValid(name, err)) return false;
    if (ThemeNameTaken(name, exceptFolder)) {
        err = L"已经有同名的主题了，换个名字";
        return false;
    }
    return true;
}

// 改 theme.json 里的 displayName（找不到就插一条；文件不存在就跳过）
static void UpdateThemeJsonDisplayName(const std::wstring& folder, const std::wstring& newName) {
    std::wstring path = JoinPath(folder, L"theme.json");
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD size = GetFileSize(h, nullptr), read = 0;
    std::string bytes(size, 0);
    if (size) ReadFile(h, &bytes[0], size, &read, nullptr);
    CloseHandle(h);

    // UTF-8 → UTF-16（去掉 BOM）
    if (read >= 3 && (unsigned char)bytes[0] == 0xEF) bytes.erase(0, 3);
    int wn = MultiByteToWideChar(CP_UTF8, 0, bytes.c_str(), (int)bytes.size(), nullptr, 0);
    std::wstring w((size_t)(wn > 0 ? wn : 0), 0);
    if (wn > 0) MultiByteToWideChar(CP_UTF8, 0, bytes.c_str(), (int)bytes.size(), &w[0], wn);

    // 找 "displayName" : "..." 替换引号里的内容；没有就插在第一行 { 之后
    const std::wstring key = L"\"displayName\"";
    size_t k = w.find(key);
    if (k != std::wstring::npos) {
        size_t c = w.find(L':', k + key.size());
        size_t q1 = (c == std::wstring::npos) ? std::wstring::npos : w.find(L'"', c + 1);
        size_t q2 = (q1 == std::wstring::npos) ? std::wstring::npos : w.find(L'"', q1 + 1);
        if (q1 == std::wstring::npos || q2 == std::wstring::npos) return;
        w = w.substr(0, q1 + 1) + newName + w.substr(q2);
    } else {
        size_t brace = w.find(L'{');
        if (brace == std::wstring::npos) return;
        w.insert(brace + 1, L"\r\n  \"displayName\": \"" + newName + L"\",");
    }

    // 写回 UTF-8（无 BOM）
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)(n > 0 ? n : 0), 0);
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr, nullptr);
    h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                    nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    if (!out.empty()) WriteFile(h, out.data(), (DWORD)out.size(), &written, nullptr);
    CloseHandle(h);
}

bool RenameTheme(const std::wstring& folder, const std::wstring& newName, std::wstring& err) {
    if (folder.empty()) { err = L"主题目录不存在"; return false; }
    if (!ThemeNameAvailable(newName, folder, err)) return false;
    size_t sl = folder.find_last_of(L"\\/");
    if (sl == std::wstring::npos) { err = L"主题目录路径异常"; return false; }
    std::wstring parent = folder.substr(0, sl);
    std::wstring target = JoinPath(parent, newName);
    if (target == folder) return true;                       // 没变
    if (!MoveFileExW(folder.c_str(), target.c_str(), 0)) {
        err = L"改名失败：目标文件夹已存在，或被占用（关掉资源管理器里的主题文件夹再试）";
        return false;
    }
    UpdateThemeJsonDisplayName(target, newName);
    return true;
}

// 安全：拒绝删除盘符根目录这类危险路径（例如 "C:\"）
static bool SafeToDelete(const std::wstring& folder) {
    if (folder.empty() || folder.size() <= 3) return false;
    // 路径必须以 <盘符>:\ 开头，且不是盘符本身
    if (folder.size() >= 3 && folder[1] == L':' && (folder[2] == L'\\' || folder[2] == L'/'))
        return folder.size() > 3;
    return false;
}

// 删除主题：移到回收站（可恢复）。用 IFileOperation 而不是 SHFileOperation——
// MinGW 在较新的 NTDDI_VERSION 下会隐藏老 API，两者配合 FOF_ALLOWUNDO 等价。
bool DeleteThemeFolder(const std::wstring& folder, std::wstring& outErr) {
    outErr.clear();
    if (folder.empty() || !DirExists(folder)) {
        outErr = L"主题文件夹不存在：可能已经被删掉了";
        return false;
    }
    if (!SafeToDelete(folder)) {
        outErr = L"路径不安全，已拒绝删除";
        return false;
    }

    bool ok = false;
    IFileOperation* pfo = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&pfo))) && pfo) {
        pfo->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI);
        IShellItem* item = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&item))) && item) {
            if (SUCCEEDED(pfo->DeleteItem(item, nullptr)))
                ok = SUCCEEDED(pfo->PerformOperations());
            item->Release();
        }
        pfo->Release();
    }
    // 失败时不退化成直接删除：PerformOperations 失败多半是文件被占用，硬删会把
    // 没被占用的文件永久删掉、整个文件夹却删不掉（回收站也找不回）。真没有回收
    // 站的盘 IFileOperation 自己会永久删除，这里只需报错让用户重试。
    if (!ok || DirExists(folder)) {
        outErr = L"删除失败：文件可能正被占用，或没有权限";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
//  时间 / 阶段
// ---------------------------------------------------------------------------
int StageAt(const Config& c, int m) {
    int d = c.dayStart, n = c.nightStart;
    if (d == n) return 0;
    if (d < n) return (m >= d && m < n) ? 0 : 1;   // 日间时段不跨午夜
    return (m >= d || m < n) ? 0 : 1;              // 日间时段跨午夜（如 22:00–06:00）
}

int NextSwitchAt(const Config& c, int m) {
    int d = c.dayStart, n = c.nightStart;
    if (d == n) return d;
    int best = -1, bestDelta = 0;
    int cands[2] = {d, n};
    for (int i = 0; i < 2; ++i) {
        int delta = ((cands[i] - m) % 1440 + 1440) % 1440;
        if (delta == 0) delta = 1440;
        if (best < 0 || delta < bestDelta) { best = cands[i]; bestDelta = delta; }
    }
    return best;
}

int MinutesUntilSwitch(const Config& c, int m) {
    int d = NextSwitchAt(c, m);
    return ((d - m) % 1440 + 1440) % 1440;
}

std::wstring FormatHM(int minutes) {
    minutes = ((minutes % 1440) + 1440) % 1440;
    wchar_t buf[16];
    swprintf(buf, 16, L"%02d:%02d", minutes / 60, minutes % 60);
    return buf;
}

std::wstring FormatDuration(int minutes) {
    if (minutes <= 0) return L"即将";
    int h = minutes / 60, m = minutes % 60;
    wchar_t buf[64];
    if (h > 0 && m > 0) swprintf(buf, 64, L"%d 小时 %d 分", h, m);
    else if (h > 0)     swprintf(buf, 64, L"%d 小时", h);
    else                swprintf(buf, 64, L"%d 分钟", m);
    return buf;
}

// 当前系统壁纸的路径（注册表里那份）；读不到或文件不存在时返回空串。
// 先量大小再读：路径可能超过 MAX_PATH，固定缓冲会把长路径截断成不存在的文件
std::wstring CurrentWallpaperPath() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", 0, KEY_READ, &k) != ERROR_SUCCESS)
        return std::wstring();
    DWORD n = 0, type = 0;
    LSTATUS st = RegQueryValueExW(k, L"WallPaper", nullptr, &type, nullptr, &n);
    std::wstring path;
    if (st == ERROR_SUCCESS && type == REG_SZ && n >= sizeof(wchar_t)) {
        path.resize(n / sizeof(wchar_t));
        st = RegQueryValueExW(k, L"WallPaper", nullptr, nullptr, (LPBYTE)&path[0], &n);
    }
    RegCloseKey(k);
    while (!path.empty() && path.back() == L'\0') path.pop_back();   // 值自带结尾的 '\0'
    if (st != ERROR_SUCCESS || path.empty() || !FileExists(path)) return std::wstring();
    return path;
}

// ---------------------------------------------------------------------------
//  壁纸（填充方式值与 IDesktopWallpaper::SetPosition 的枚举一致）
// ---------------------------------------------------------------------------
bool ApplyWallpaper(const std::wstring& image, int style) {
    if (image.empty() || !FileExists(image)) return false;

    // 通知资源管理器允许壁纸淡入过渡（WinDynamicDesktop 同款做法）
    if (HWND progman = FindWindowW(L"Progman", nullptr)) {
        DWORD_PTR res = 0;
        SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_ABORTIFHUNG, 1000, &res);
    }

    bool ok = false;
    IDesktopWallpaper* dw = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL,
                                   IID_IDesktopWallpaper, (void**)&dw))) {
        dw->SetPosition((DESKTOP_WALLPAPER_POSITION)style);
        ok = SUCCEEDED(dw->SetWallpaper(nullptr, image.c_str()));
        dw->Release();
    }
    if (!ok) {
        // 回退：老 API（同时写入注册表并广播设置变更）
        ok = SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, (void*)image.c_str(),
                                   SPIF_UPDATEINIFILE | SPIF_SENDCHANGE) != FALSE;
    }
    return ok;
}

// ---------------------------------------------------------------------------
//  开机自启
// ---------------------------------------------------------------------------
bool AutoStartEnabled() {
    wchar_t buf[MAX_PATH * 2] = {0};
    DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kAppName, RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS)
        return false;
    return buf[0] != 0;
}

bool AutoStartHasBackgroundFlag() {
    wchar_t buf[MAX_PATH * 2] = {0};
    DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kAppName, RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS)
        return true;                               // 没读到就算了，别反复重写
    std::wstring s = buf;
    return s.find(L"--autostart") != std::wstring::npos ||
           s.find(L"--background") != std::wstring::npos ||
           s.find(L"--tray") != std::wstring::npos;
}

void SetAutoStart(bool on) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                        &key, nullptr) != ERROR_SUCCESS)
        return;
    if (on) {
        // 带上 --autostart：程序启动就知道"这次是开机自启"，只在托盘静默运行
        std::wstring cmd = L"\"" + ExePath() + L"\" --autostart";
        RegSetValueExW(key, kAppName, 0, REG_SZ, (const BYTE*)cmd.c_str(),
                       (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kAppName);
    }
    RegDeleteValueW(key, kLegacyName);          // 更名前的项一并清掉，注册表里只留一条
    RegCloseKey(key);
}

// ---------------------------------------------------------------------------
//  单实例
// ---------------------------------------------------------------------------
static HANDLE g_mutex = nullptr;

bool SingleInstanceAcquire() {
    g_mutex = CreateMutexW(nullptr, FALSE, L"Local\\DynamicWallpapers_SingleInstance_Mutex");
    if (!g_mutex) return true;
    if (GetLastError() == ERROR_ALREADY_EXISTS) return false;
    return true;
}

// 主窗口类名固定，找到后发一条自定义消息（默认让它显示自己）。
// 更名前的老进程用旧类名，也查一下：过渡期两个版本可能短暂并存。
void NotifyExistingInstance(int msg) {
    if (HWND h = FindWindowW(L"DynamicWallpapersMainWindow", nullptr))
        PostMessageW(h, msg, 0, 0);
    else if (HWND h2 = FindWindowW(L"TimeWallMainWindow", nullptr))
        PostMessageW(h2, msg, 0, 0);
}

// ---------------------------------------------------------------------------
void SetDarkModeAttribute(HWND hwnd, bool dark) {
    BOOL v = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &v, sizeof(v));
}
