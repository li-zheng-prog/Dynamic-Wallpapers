// ============================================================================
//  core.h — 业务核心：路径 / 配置 / 主题扫描 / 时间调度 / 壁纸设置 / 开机自启
//
//  主题格式与壁纸设置方式参考 WinDynamicDesktop：
//    · theme.json 字段：imageFilename / dayImageList / nightImageList /
//      sunriseImageList / sunsetImageList（本程序只取日间与夜间两张）
//    · 壁纸切换：IDesktopWallpaper（多显示器/填充方式）+ 通知 Progman 启用淡入
// ============================================================================
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>

// 一个主题 = 一个文件夹，含 theme.json（WDD 兼容，可选）+ 若干图片
struct ThemeInfo {
    std::wstring name;        // 显示名（theme.json 里的 displayName，缺省用文件夹名）
    std::wstring folder;      // 主题文件夹完整路径
    std::wstring dayImage;    // 日间壁纸（完整路径）
    std::wstring nightImage;  // 夜间壁纸（完整路径）
    int imageCount = 0;       // 文件夹内图片总数
    bool valid() const { return !dayImage.empty() && !nightImage.empty(); }
};

// 配置保存在 %APPDATA%\Dynamic Wallpapers\config.ini
//（更名前为 %APPDATA%\TimeWall，首次运行自动迁移）
struct Config {
    std::wstring theme;          // 选中的主题名
    int dayStart = 6 * 60;       // 日间开始（分钟，从 0:00 起算）
    int nightStart = 18 * 60;    // 夜间开始
    bool autoSwitch = true;      // 自动切换总开关
    // 切换方式：0=按时段（日间/夜间图按上面两个时间点换）
    //           1=跟随系统深浅色（系统浅色模式→浅色图，深色模式→深色图）
    int switchMode = 0;
    bool startOnBoot = false;    // 开机自动启动
    bool minimizeToTray = true;  // 点关闭按钮时最小化到托盘
    // 壁纸填充方式：0=居中 1=平铺 2=拉伸 3=适应 4=填充 5=跨屏（与 IDesktopWallpaper::SetPosition 一致）
    int wallpaperStyle = 4;
    int appearance = 0;          // 外观：0=跟随系统 1=浅色 2=深色
};

// ---- 路径 ----
std::wstring ExeDir();
std::wstring ExePath();
std::wstring AppDataDir();                       // %APPDATA%\Dynamic Wallpapers（自动从 TimeWall 迁移）
std::wstring ConfigPath();
std::vector<std::wstring> ThemeDirs();           // 主题搜索目录（exe 同级 themes、用户目录 themes）
std::wstring JoinPath(const std::wstring& a, const std::wstring& b);
bool FileExists(const std::wstring& p);

// ---- 配置 ----
Config LoadConfig();
void SaveConfig(const Config& c);

// ---- 主题 ----
std::vector<ThemeInfo> ScanThemes();
// 按名字找主题；找不到就返回 nullptr（绝不能退回"第一个"——调用方拿它去删文件夹）
const ThemeInfo* FindTheme(const std::vector<ThemeInfo>& list, const std::wstring& name);

// ---- 用户自定义主题（"添加主题"功能用）----
std::wstring UserThemeDir();                       // %APPDATA%\Dynamic Wallpapers\themes
std::vector<std::wstring> ListImagesIn(const std::wstring& folder);  // 图片列表（自然排序）
bool RenameTheme(const std::wstring& folder, const std::wstring& newName, std::wstring& err);
// 名字可用性 = ThemeNameValid + 不与任何主题目录里的主题重名（exceptFolder 是"自己"，跳过）
bool ThemeNameAvailable(const std::wstring& name, const std::wstring& exceptFolder, std::wstring& err);
// 名字形状校验：非空、≤60 字、不含 \ / : * ? " < > |、不是 "." / ".."。
// 纯字符串判断、无文件 I/O，可逐帧调用（添加主题弹层的即时提示）。
bool ThemeNameValid(const std::wstring& name, std::wstring& err);
// 按主题规则找出某文件夹的日间/夜间图片（支持没有 theme.json 的情况）
bool DiscoverThemeImages(const std::wstring& folder, const std::wstring& name,
                         std::wstring& dayImage, std::wstring& nightImage);
// 把选中的两张图导入为用户主题：复制为 <名字>_1.jpg / _2.jpg 并生成 theme.json
bool ImportTheme(const std::wstring& name, const std::wstring& dayImage,
                 const std::wstring& nightImage, std::wstring& outName,
                 std::wstring* err = nullptr);
// 把主题文件夹移到回收站（可恢复）；失败时 outErr 给原因
bool DeleteThemeFolder(const std::wstring& folder, std::wstring& outErr);

// ---- 时间与阶段 ----
int StageAt(const Config& c, int minuteOfDay);       // 0=日间 1=夜间
int NextSwitchAt(const Config& c, int minuteOfDay);  // 下一次切换的时刻
int MinutesUntilSwitch(const Config& c, int minuteOfDay);
std::wstring FormatHM(int minutes);                  // "06:00"
std::wstring FormatDuration(int minutes);            // "2 小时 14 分"

// ---- 壁纸 ----
std::wstring CurrentWallpaperPath();                 // 当前系统壁纸文件（毛玻璃底图用）
bool ApplyWallpaper(const std::wstring& image, int style);

// ---- 开机自启（HKCU\Software\Microsoft\Windows\CurrentVersion\Run）----
bool AutoStartEnabled();
// 自启命令行里是否已带 --autostart / --background / --tray（老版本迁移用）
bool AutoStartHasBackgroundFlag();
void SetAutoStart(bool on);

// ---- 单实例 ----
bool SingleInstanceAcquire();
// 通知已在运行的实例（msg 默认 0x8001 = WM_APP+1 = 显示窗口）
void NotifyExistingInstance(int msg = 0x8001);

// ---- 其他 ----
// 同步系统深/浅色到窗口标题栏（DWM 深色模式属性）
void SetDarkModeAttribute(HWND hwnd, bool dark);
