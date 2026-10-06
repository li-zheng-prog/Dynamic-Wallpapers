// ============================================================================
//  main.cpp — 主程序：窗口 / 布局 / 交互 / 托盘 / 定时切换
//
//  只在两个时间点之间切换壁纸：日间开始 ~ 夜间开始用主题的日间图，其余时间用
//  夜间图；或跟随系统深浅色。设置改动即时保存到 %APPDATA%\Dynamic Wallpapers\config.ini。
//
//  窗口：无边框 + 全自绘标题栏的 WS_POPUP 窗口，Win11 圆角/阴影由 DWM 提供。
//  最大化/最小化动画自己插值窗口矩形（系统动画对 DComp 无重定向位图的窗口不生效，
//  见 StartZoom / StartMinAnim 的说明）。
//
//  命令行：--show 显示窗口 / --apply 立即应用并退出 / --autostart 开机静默进托盘 /
//          --set-theme=名字 / --add-theme=文件夹 / --delete-theme=名字 /
//          --uninstall 卸载 / --simulate-hour=19 模拟时间（测试）/ --opaque 关毛玻璃
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>       // GET_X_LPARAM/GET_Y_LPARAM、GET_WHEEL_DELTA_WPARAM
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <imm.h>            // IME：主题名输入框自己处理 WM_IME_* 消息
#include <winternl.h>       // RTL_OSVERSIONINFOW（判断系统版本决定用不用系统亚克力）
#include <d2d1_1.h>
#include <d2d1helper.h>
#include <dwmapi.h>
#include <string>
#include <vector>
#include <cmath>            // powf（全屏切换动画的缓出曲线）
// timeBeginPeriod(1) 把本进程计时器精度提到 1ms：默认 WM_TIMER 只有 ~15.6ms 的
// 系统节拍（64fps），120Hz 屏上动画必然发卡。注意它提不高 GetTickCount64 的
// 分辨率——动画时间基准一律用 QPC（见 qpcMs）。
extern "C" __declspec(dllimport) unsigned int __stdcall timeBeginPeriod(unsigned int);
extern "C" __declspec(dllimport) unsigned int __stdcall timeEndPeriod(unsigned int);
#include "gfx.h"
#include "ui.h"
#include "core.h"
#include "resource.h"

#pragma comment(lib, "shell32")

// ---------------------------------------------------------------------------
//  常量与全局状态
// ---------------------------------------------------------------------------
static const wchar_t* kWindowClass = L"DynamicWallpapersMainWindow";
static const wchar_t* kAppDisplay = L"Dynamic Wallpapers";
static const wchar_t* kLegacyDisplay = L"壁纸随时间变化";   // 更名前：旧快捷方式/开始菜单文件夹名
static const float kWinW = 700.0f;      // 默认窗口宽（DIP）
static const float kWinH = 764.0f;      // 默认窗口高（DIP）— 与 ComputeLayout 的自然高度一致
                                        // （改布局加/删分区后这里要跟着改，否则底部被裁）
static const float kPadX = 24.0f;
static const float kCardW = 152.0f;     // 主题卡片尺寸（DIP）
static const float kCardH = 148.0f;
static const float kCardGapY = 16.0f;   // 卡片行与行之间的竖直间距（DIP）
static const int   kMaxCards = 6;       // 主题卡片每行最多几张（窗口拉宽后能放更多）
static const int   kThemeSlots = 48;    // 卡片网格容量上限（同时决定卡片控件 ID 区间）

// 控件 ID。主题卡片 ID = 基址 + 主题下标：
//   100..147 选中 / 200..247 "×"删除 / 300..347 "✎"重命名
enum CtrlId {
    CID_NONE = -1,
    CID_MIN = 1, CID_CLOSE, CID_MAX,
    CID_THEME0 = 100,
    CID_THEME_DEL0 = 200,
    CID_THEME_REN0 = 300,
    // 滚动条：右侧滑块 + 轨道（整页滚动，见 DrawScrollbar / HitTest）
    CID_SCROLLBAR = 400, CID_SCROLLTRACK,
    CID_DAY_MINUS = 30, CID_DAY_PLUS, CID_NIGHT_MINUS, CID_NIGHT_PLUS,
    CID_AUTO = 40, CID_BOOT,
    CID_OPENTHEMES = 51, CID_ADD_THEME,
    CID_APPEARANCE0 = 60, CID_APPEARANCE1, CID_APPEARANCE2,
    CID_SWITCH0 = 64, CID_SWITCH1,          // 切换方式分段控件：按时段 / 跟随系统
    CID_OV_CANCEL = 70, CID_OV_OK, CID_OV_NAME = 76, CID_OV_LIGHT_SLOT, CID_OV_DARK_SLOT,
    CID_DLG_CANCEL = 80, CID_DLG_OK,
    CID_REN_CANCEL = 83, CID_REN_OK, CID_REN_NAME,
};

enum TimerId { TM_TICK = 1, TM_ANIM = 2, TM_CARET = 3 };

// 布局（每帧重算；绘制与命中测试共用同一套矩形）。
// 坐标系：所有矩形都是内容坐标，窗口坐标 y = 内容坐标 y - scroll。
// 内容可整体上滚（滚轮/滚动条），标题栏与弹层不参与滚动。
struct Layout {
    float contentL = kPadX, contentR = 700.0f - kPadX;   // 宽窗口时居中并限宽
    D2D1_RECT_F captionMin, captionClose, captionMax;
    std::vector<D2D1_RECT_F> themeCards;    // 卡片网格（自动换行）
    std::vector<D2D1_RECT_F> themeDel;      // 卡片右上角"×"
    std::vector<D2D1_RECT_F> themeRen;      // 卡片右上角"✎"
    int cardCount = 0;
    int perRow = 1;
    float contentH = 0;                     // 内容总高（比窗口矮时 = 窗口高，不滚）
    float visibleH = 0;
    float maxScroll = 0;
    float scrollTrackTop = 0, scrollTrackH = 0;   // 滚动条轨道
    int firstVis = 0, lastVis = -1;         // 视口内的卡片下标范围（含两端）
    D2D1_RECT_F addThemeBtn;
    D2D1_RECT_F dayStepper, nightStepper;
    D2D1_RECT_F appearanceSeg;
    D2D1_RECT_F switchSeg;
    D2D1_RECT_F autoToggle, bootToggle;
    D2D1_RECT_F openThemesBtn;
    D2D1_RECT_F themeSectionTitle, timeSectionTitle, appearanceSectionTitle, optionSectionTitle;
    D2D1_RECT_F switchSectionTitle;
    D2D1_RECT_F statusRow;
    D2D1_RECT_F ovCard, ovName;             // 添加主题弹层（两方框 + 名称输入框）
    D2D1_RECT_F ovLightSlot, ovDarkSlot;
    D2D1_RECT_F ovCancel, ovOk;
    D2D1_RECT_F dlgCard, dlgCancel, dlgOk;  // 删除主题确认框
    D2D1_RECT_F renCard, renField, renCancel, renOk;   // 重命名弹层
};

// 删除主题确认框状态：hover 到卡片"×"后弹出，确认才真正删除
struct DelUI {
    bool active = false;
    int idx = -1;
    std::wstring err;                       // 删除失败的提示
};

// 自绘文本框的编辑状态（本程序唯一的输入框：主题名/重命名）。
// 字符、←→ Home End Backspace Delete、Shift 扩展选择、Ctrl+A/V、光标闪烁；
// 中文输入法走 WM_IME_* 消息，预编辑串自己画在光标处、带下划线。
struct TextEdit {
    std::wstring text;
    int caret = 0;                          // 光标位置（0..text.size()）
    int sel = 0;                            // 选择锚点（sel == caret 表示没选中）
    std::wstring preedit;                   // 输入法预编辑串
    bool hasSel() const { return sel != caret; }
    void selRange(int& a, int& b) const { a = sel < caret ? sel : caret; b = sel < caret ? caret : sel; }
    void clearSel() { sel = caret; }
};

// "添加主题"弹层：不弹文件夹选择——点两个方框各挑一张图，再起名字。
// 主题 = 浅色（日间用）+ 深色（夜间用）两张不同的图片。
struct AddThemeUI {
    bool active = false;
    std::wstring lightPath, darkPath;
    ID2D1Bitmap* lightBmp = nullptr;        // 预览缩略图
    ID2D1Bitmap* darkBmp = nullptr;
    TextEdit name;
    std::wstring hint;
};

// 主题重命名弹层（卡片右上角"✎"打开，输入框预填当前名字）
struct RenameUI {
    bool active = false;
    int idx = -1;
    TextEdit name;
    std::wstring hint;
};

struct App {
    HWND hwnd = nullptr;
    Gfx gfx;
    Config cfg;
    std::vector<ThemeInfo> themes;
    int selIdx = 0;
    Palette pal;
    bool glass = true;                      // 自绘磨砂底（--self-glass 强制时用）
    bool liveGlass = false;                 // 系统亚克力（DWM 实时模糊窗口背后，Win11 22H2+）
    bool solidBg = false;                   // 纯色底（Win10 默认 / --solid），跟随深浅色
    float winWDip = kWinW, winHDip = kWinH; // 当前客户区尺寸（DIP）——全屏/分屏后布局跟着走
    bool maximized = false;
    // 全屏切换动画：无边框 + 自绘标题栏的 WS_POPUP 窗口拿不到系统自带的
    // 最大化/最小化缩放动画，直接发 SC_MAXIMIZE 是"啪"地一下，所以自己插值窗口矩形。
    bool zooming = false;
    bool zoomToMax = false;
    RECT zoomFrom = {}, zoomTo = {};
    RECT normRect = {};                     // 自记的"还原矩形"（不依赖系统的 rcNormalPosition）
    double zoomStartMs = 0;                 // 动画起点：QPC 毫秒（不要用 GetTickCount64）
    float zoomMs = 200.0f;                  // 全屏切换动画时长（--zoom-ms=N 可改）
    // 整页滚动：scroll = 当前量，scrollTarget = 目标量（滚轮改它，动画缓动追上）
    float scroll = 0.0f, scrollTarget = 0.0f;
    double lastScrollMs = 0;
    float sbAlpha = 0.0f;                   // 滚动条可见度（0~1，带淡入淡出）
    bool sbDragging = false;
    float sbDragGrab = 0.0f;
    // 主题缩略图按主题下标缓存：只解码视口内（±一行缓冲）的主题，滚走就释放
    std::vector<ID2D1Bitmap*> thumbs;
    std::vector<char> thumbTried;           // 试过且失败的别再每帧重试
    std::vector<char> delVisible;           // 上一帧该卡片是否画出了"×/✎"（命中只认画出来的）
    ID2D1Bitmap* logoBmp = nullptr;         // 标题栏标记（exe 资源，退回手绘"左日右月"）
    bool logoTried = false;
    ID2D1LinearGradientBrush* bgBrush = nullptr;
    ID2D1Layer* uiLayer = nullptr;          // 整帧透明度图层（窗口淡入淡出/时段置灰）
    Layout L;
    AddThemeUI addUI;
    DelUI delUI;
    RenameUI renUI;
    // 本帧是否已压整帧透明度图层：里面再嵌一层（时段置灰）会让 D2D 只画出前半帧，
    // 所以有外层图层时时段那块不再自己压层（淡出仅 150ms，观感察觉不到）。
    bool frameLayerPushed = false;
    bool caretOn = true;                    // 输入框光标闪烁（统一 500ms 节拍）
    HIMC himc = nullptr;                    // 输入法上下文（启动时摘下，有输入框时才挂上）

    // 交互状态
    int hot = CID_NONE, pressed = CID_NONE;
    float toggleAnim[2] = {1.0f, 0.0f};     // 0=自动切换 1=开机自启
    float toggleTarget[2] = {1.0f, 0.0f};
    float segAnim = 0.0f;                   // 外观分段控件指示条位置（0..2）
    int segTarget = 0;
    float segScale = 1.0f;                  // 按下时缩到 0.93，松手弹回
    bool segPressed = false;
    float swAnim = 0.0f;                    // 切换方式分段控件（0..1），与外观同款
    int swTarget = 0;
    float swScale = 1.0f;
    bool swPressed = false;
    bool trayAdded = false;
    HICON trayIcon = nullptr;               // 托盘图标：只加载一次（重加时复用），WM_DESTROY 销毁

    // 窗口显示/隐藏动画（淡入淡出 + 由下方轻推入）
    float winAnim = 1.0f;
    float winAnimTarget = 1.0f;
    bool winAnimating = false;
    bool hideAfterAnim = false;

    // 最小化 / 从任务栏还原的缩放动画：按系统原生动画的实测参数复刻（见 StartMinAnim）
    bool  minAnimating = false;             // 动画进行中（布局冻结、命中测试暂停）
    bool  minToIcon = true;                 // true=收进任务栏，false=从任务栏展开
    RECT  minFrom = {}, minTo = {}, minRect = {};   // 屏幕坐标：起点/终点/当前帧
    int   minRefW = 0;                      // 冻结时的窗口物理宽度（缩放系数 = 当前宽/minRefW）
    float minMs = 150.0f;                   // 动画时长（实测系统 ≈150ms；--min-ms= 可放慢）
    bool  minWasMaximized = false;          // 还原方向：最小化前是不是最大化

    // 测试/调试
    int simulateHour = -1;
    std::wstring statusMsg;                 // 临时提示（如"已应用"）
    ULONGLONG statusMsgUntil = 0;
};
static App g_app;

// 前向声明（本文件内互相调用）
static void ComputeLayout(Layout& L);
static void UpdateScrollView(Layout& L);
static void RefreshPalette();
static void ApplyCurrent(bool force);
static void StartAnim();
static void StartZoom(bool toMax);
static void RunZoomAnim();
static void StartWindowAnim(bool show);
static void TrackNormRect();
static void ShowMainWindow();
static void HideMainWindow();
static void SelectTheme(int idx);
static void OpenThemesFolder();
static void OpenAddTheme();
static void CloseAddTheme();
static void OpenRenameTheme(int idx);
static void CloseRenameTheme();
static void OpenDelTheme(int idx);
static void CloseDelTheme();
static void EnsureGlassBitmap(bool force);
static void ClearThumbs();
static void SyncThumbs();
static void EnsureCardVisible(int idx, bool instant = false);
static void MarkScrolled();
static double qpcMs();

// ---------------------------------------------------------------------------
//  时间（--simulate-hour 测试钩子：只替换"小时"，分钟仍走真实时钟）
// ---------------------------------------------------------------------------
static int CurrentMinutes() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return g_app.simulateHour >= 0 ? g_app.simulateHour * 60 + st.wMinute
                                   : st.wHour * 60 + st.wMinute;
}

// ---------------------------------------------------------------------------
//  布局计算
// ---------------------------------------------------------------------------
static void ComputeLayout(Layout& L) {
    const float W = g_app.winWDip, H = g_app.winHDip;   // 实时窗口尺寸：全屏/分屏后跟着走

    // 标题栏按钮从右往左排，顺序严格是 min → max → close（宽 46 DIP，与 Win11 一致）
    L.captionClose = D2D1::RectF(W - 46, 0, W, Ui::TitleBarH);
    L.captionMax   = D2D1::RectF(W - 92, 0, W - 46, Ui::TitleBarH);
    L.captionMin   = D2D1::RectF(W - 138, 0, W - 92, Ui::TitleBarH);

    // 内容区：窗口很宽（全屏）时限制宽度并居中，避免控件被拉得又长又空
    const float kMaxContent = 1060.0f;
    float contentW = W - 2 * kPadX;
    if (contentW > kMaxContent) contentW = kMaxContent;
    if (contentW < 300) contentW = 300;
    const float lx = (W - contentW) / 2, rx = lx + contentW;
    L.contentL = lx;
    L.contentR = rx;

    // ---- 主题卡片网格：一行放不下就换行，超出窗口的内容靠滚动看 ----
    float y = 56;
    L.themeSectionTitle = D2D1::RectF(lx, y, rx, y + 27);
    L.addThemeBtn = D2D1::RectF(rx - 116, y - 1, rx, y + 25);
    y += 36;                                             // 区块标题 + 分隔线
    const float gap = 12;
    int total = (int)g_app.themes.size();
    if (total > kThemeSlots) total = kThemeSlots;
    int perRow = (int)((contentW + gap) / (kCardW + gap));
    if (perRow < 1) perRow = 1;
    if (perRow > kMaxCards) perRow = kMaxCards;
    L.cardCount = total;
    L.perRow = perRow;
    L.themeCards.assign(total, D2D1::RectF(0, 0, 0, 0));
    L.themeDel.assign(total, D2D1::RectF(0, 0, 0, 0));
    L.themeRen.assign(total, D2D1::RectF(0, 0, 0, 0));
    // 列位置按"铺满一整行"算一次，之后每行共用同一组列——最后一行张数少时
    // 卡片仍落在上面各行同一列的下方（上下对齐的标准网格行为）
    float gy = y;
    if (total == 0) {
        gy += kCardH + kCardGapY;                        // 没有主题也留一块高度放提示文字
    } else {
        float used = perRow * kCardW;
        float sp = perRow > 1 ? (contentW - used) / (perRow - 1) : 0;
        if (sp > 30) sp = 30;
        float x0 = lx;
        if (perRow > 1) x0 = lx + (contentW - (used + sp * (perRow - 1))) / 2;
        for (int r = 0; r * perRow < total; ++r) {
            int cnt = total - r * perRow;
            if (cnt > perRow) cnt = perRow;
            for (int i = 0; i < cnt; ++i) {
                int ci = r * perRow + i;
                float cx0 = x0 + i * (kCardW + sp);
                L.themeCards[ci] = D2D1::RectF(cx0, gy, cx0 + kCardW, gy + kCardH);
                L.themeDel[ci] = UiThemeCardDeleteRect(L.themeCards[ci]);
                L.themeRen[ci] = UiThemeCardRenameRect(L.themeCards[ci]);
            }
            gy += kCardH + kCardGapY;
        }
    }
    y = gy - kCardGapY + 18;                             // 去掉最后一行行距，留区块间距

    // ---- 切换（切换方式：按时段 / 跟随系统深浅色）----
    L.switchSectionTitle = D2D1::RectF(lx, y, rx, y + 27);
    y += 36;
    L.switchSeg = D2D1::RectF(rx - 196, y + 2, rx, y + 2 + 34);   // 宽 196 = 每段 98
    y += 34 + 18;

    // ---- 时段 ----
    L.timeSectionTitle = D2D1::RectF(lx, y, rx, y + 27);
    y += 36;
    const float stepperW = 132, stepperH = 36;
    L.dayStepper   = D2D1::RectF(rx - stepperW, y, rx, y + stepperH);
    y += stepperH + 10;
    L.nightStepper = D2D1::RectF(rx - stepperW, y, rx, y + stepperH);
    y += stepperH + 18;

    // ---- 外观 ----
    L.appearanceSectionTitle = D2D1::RectF(lx, y, rx, y + 27);
    y += 36;
    L.appearanceSeg = D2D1::RectF(rx - 234, y, rx, y + 34);
    y += 34 + 18;

    // ---- 选项 ----
    L.optionSectionTitle = D2D1::RectF(lx, y, rx, y + 27);
    y += 36;
    L.autoToggle  = D2D1::RectF(rx - Ui::ToggleW, y + 8, rx, y + 8 + Ui::ToggleH);
    y += 36;
    L.bootToggle  = D2D1::RectF(rx - Ui::ToggleW, y + 8, rx, y + 8 + Ui::ToggleH);
    y += 36;

    // ---- 状态行 + 底部按钮：窗口比自然高度高（全屏）时贴到窗口底部 ----
    const float bottomBlock = 24 + 10 + Ui::ButtonH + 24;
    float statusTop = y;
    float stickTop = H - bottomBlock;
    bool stick = stickTop > statusTop;
    if (stick) statusTop = stickTop;
    L.statusRow = D2D1::RectF(lx, statusTop, rx, statusTop + 24);
    L.openThemesBtn = D2D1::RectF(rx - 158, statusTop + 24 + 10, rx,
                                  statusTop + 24 + 10 + Ui::ButtonH);
    // 内容总高 = 滚动的依据：真超出窗口才加 24 底部留白，差几个 DIP 时不留白
    //（否则平白多出一条几乎滚不动的滚动条）
    const float naturalBottom = statusTop + 24 + 10 + Ui::ButtonH;
    float contentBottom = naturalBottom;
    if (naturalBottom - H > 4.0f) contentBottom = naturalBottom + 24.0f;
    if (contentBottom < H) contentBottom = H;
    L.contentH = contentBottom;

    // ---- 添加主题弹层（居中卡片；窗口变高时也居中）----
    const float cw = 560, ch = 400;
    float cx = (W - cw) / 2, cy0 = (H - ch) / 2 - 40;
    if (cy0 < 90) cy0 = 90;
    L.ovCard = D2D1::RectF(cx, cy0, cx + cw, cy0 + ch);
    float px = L.ovCard.left + 24, pr = L.ovCard.right - 24;
    L.ovName      = D2D1::RectF(px, L.ovCard.top + 76, pr, L.ovCard.top + 112);
    L.ovLightSlot = D2D1::RectF(px, L.ovCard.top + 152, px + 240, L.ovCard.top + 292);
    L.ovDarkSlot  = D2D1::RectF(px + 272, L.ovCard.top + 152, px + 512, L.ovCard.top + 292);
    L.ovCancel = D2D1::RectF(L.ovCard.right - 24 - 108 - 116, L.ovCard.bottom - 24 - 34,
                             L.ovCard.right - 24 - 116, L.ovCard.bottom - 24);
    L.ovOk     = D2D1::RectF(L.ovCard.right - 24 - 108, L.ovCard.bottom - 24 - 34,
                             L.ovCard.right - 24, L.ovCard.bottom - 24);

    // ---- 删除主题确认框 ----
    const float dw = 470, dh = 214;
    float dx = (W - dw) / 2, dy0 = (H - dh) / 2 - 24;
    L.dlgCard = D2D1::RectF(dx, dy0, dx + dw, dy0 + dh);
    L.dlgCancel = D2D1::RectF(L.dlgCard.right - 24 - 116 - 108, L.dlgCard.bottom - 24 - 34,
                              L.dlgCard.right - 24 - 116, L.dlgCard.bottom - 24);
    L.dlgOk     = D2D1::RectF(L.dlgCard.right - 24 - 108, L.dlgCard.bottom - 24 - 34,
                              L.dlgCard.right - 24, L.dlgCard.bottom - 24);

    // ---- 主题重命名弹层 ----
    const float rw = 470, rh = 246;
    float renX = (W - rw) / 2, renY = (H - rh) / 2 - 24;
    L.renCard  = D2D1::RectF(renX, renY, renX + rw, renY + rh);
    L.renField = D2D1::RectF(L.renCard.left + 24, L.renCard.top + 104,
                             L.renCard.right - 24, L.renCard.top + 140);
    L.renCancel = D2D1::RectF(L.renCard.right - 24 - 116 - 108, L.renCard.bottom - 24 - 34,
                              L.renCard.right - 24 - 116, L.renCard.bottom - 24);
    L.renOk     = D2D1::RectF(L.renCard.right - 24 - 108, L.renCard.bottom - 24 - 34,
                              L.renCard.right - 24, L.renCard.bottom - 24);

    // 滚动视图度量必须在最后重算
    UpdateScrollView(L);
}

// ---------------------------------------------------------------------------
//  整页滚动：布局矩形是内容坐标，画的时候整体平移 -scroll；
//  鼠标窗口坐标 y 对应的内容坐标是 y + scroll（见 HitTest / RenderFrame）。
// ---------------------------------------------------------------------------
static void VisibleCardRange(const Layout& L, float scroll, int& first, int& last) {
    first = 0;
    last = L.cardCount - 1;
    const float viewTop = scroll + Ui::TitleBarH;        // 视口上缘（标题栏以下才算可见）
    const float viewBot = scroll + g_app.winHDip;
    while (first < L.cardCount && L.themeCards[first].bottom <= viewTop) ++first;
    while (last >= first && L.themeCards[last].top >= viewBot) --last;
}

// 重算滚动度量：最大可滚量、滚动条轨道，并把 scroll/scrollTarget 夹回合法范围。
// ComputeLayout 结尾必须调它；窗口尺寸或主题数一变，可滚量就变了。
static void UpdateScrollView(Layout& L) {
    const float H = g_app.winHDip;
    L.visibleH = H;
    float maxS = L.contentH - H;
    if (maxS < 0) maxS = 0;
    L.maxScroll = maxS;
    if (maxS <= 0.0f) {
        g_app.scroll = 0.0f;                 // 内容放得下：回顶部，否则多出一条空白
        g_app.scrollTarget = 0.0f;
    } else {
        if (g_app.scrollTarget > maxS) g_app.scrollTarget = maxS;
        if (g_app.scrollTarget < 0) g_app.scrollTarget = 0;
        if (g_app.scroll > maxS) g_app.scroll = maxS;    // 窗口缩小/主题变少立刻夹回
        if (g_app.scroll < 0) g_app.scroll = 0;
    }
    L.scrollTrackTop = Ui::TitleBarH + 6;
    L.scrollTrackH = H - Ui::TitleBarH - 12;
    if (L.scrollTrackH < 20) L.scrollTrackH = 20;
}

static void ClampScrollTarget() {
    float maxS = g_app.L.maxScroll;
    if (g_app.scrollTarget > maxS) g_app.scrollTarget = maxS;
    if (g_app.scrollTarget < 0) g_app.scrollTarget = 0;
}

// 滚动条滑块矩形（按当前 scroll 实时算；绘制与命中测试共用）。
// 滑块长 = 轨道长 × (窗口高 / 内容高)，最短 36 DIP。
static D2D1_RECT_F ScrollThumbRect() {
    const Layout& L = g_app.L;
    float trackH = L.scrollTrackH;
    float thumbH = (L.contentH > 1.0f) ? trackH * (L.visibleH / L.contentH) : trackH;
    if (thumbH < 36.0f) thumbH = 36.0f;
    if (thumbH > trackH) thumbH = trackH;
    float travel = trackH - thumbH;
    float t = (L.maxScroll > 0.0f) ? (g_app.scroll / L.maxScroll) : 0.0f;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    float ty = L.scrollTrackTop + t * travel;
    float right = g_app.winWDip - 5.0f;
    return D2D1::RectF(right - 6.0f, ty, right, ty + thumbH);
}

static void MarkScrolled() {
    g_app.lastScrollMs = qpcMs();
}

// 选中的主题一定要看得见：只在卡片被视口上/下缘切掉时才动滚动位置。
// instant = true 直接就位（启动时用，不要开机滚一下）。
static void EnsureCardVisible(int idx, bool instant) {
    const Layout& L = g_app.L;
    if (idx < 0 || idx >= (int)L.themeCards.size()) return;
    if (L.maxScroll <= 0.0f) return;
    const float pad = 16.0f;
    const float vt = g_app.scrollTarget + Ui::TitleBarH;
    const float vb = g_app.scrollTarget + g_app.winHDip;
    float t = g_app.scrollTarget;
    if (L.themeCards[idx].top - pad < vt)
        t = L.themeCards[idx].top - pad - Ui::TitleBarH;
    else if (L.themeCards[idx].bottom + pad > vb)
        t = L.themeCards[idx].bottom + pad - g_app.winHDip;
    if (t < 0) t = 0;
    if (t > L.maxScroll) t = L.maxScroll;
    if (t == g_app.scrollTarget) return;                     // 没变化就别白播一段动画
    g_app.scrollTarget = t;
    if (instant) g_app.scroll = t;
    MarkScrolled();
}

// 命中测试：把鼠标位置（DIP，窗口坐标）映射成控件 id
static int HitTest(float x, float y) {
    const Layout& L = g_app.L;
    auto in = [&](const D2D1_RECT_F& r) { return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom; };

    // 弹层打开时只响应弹层内的控件（弹层不随内容滚动）
    if (g_app.addUI.active) {
        if (in(L.ovCancel)) return CID_OV_CANCEL;
        if (in(L.ovOk)) return CID_OV_OK;
        if (in(L.ovName)) return CID_OV_NAME;
        if (in(L.ovLightSlot)) return CID_OV_LIGHT_SLOT;
        if (in(L.ovDarkSlot)) return CID_OV_DARK_SLOT;
        return CID_NONE;
    }
    if (g_app.delUI.active) {
        if (in(L.dlgCancel)) return CID_DLG_CANCEL;
        if (in(L.dlgOk)) return CID_DLG_OK;
        return CID_NONE;
    }
    if (g_app.renUI.active) {
        if (in(L.renCancel)) return CID_REN_CANCEL;
        if (in(L.renOk)) return CID_REN_OK;
        if (in(L.renField)) return CID_REN_NAME;
        return CID_NONE;
    }

    // 标题栏：固定不滚动（窗口坐标直接判断）。
    // 最大化按钮走非客户区命中（WM_NCHITTEST 返回 HTMAXBUTTON），这里不管；
    // 内容即使被滚到标题栏底下也不该被点到（视觉上在裁剪区外）。
    if (y < Ui::TitleBarH) {
        if (in(L.captionClose)) return CID_CLOSE;
        if (in(L.captionMin)) return CID_MIN;
        return CID_NONE;
    }

    // 滚动条：看得见（不透明度够）才点得到；滑块拖动 / 轨道翻页
    if (L.maxScroll > 0.0f && g_app.sbAlpha > 0.15f && x >= g_app.winWDip - 14.0f) {
        if (in(ScrollThumbRect())) return CID_SCROLLBAR;
        D2D1_RECT_F track = D2D1::RectF(g_app.winWDip - 14.0f, L.scrollTrackTop,
                                        g_app.winWDip, L.scrollTrackTop + L.scrollTrackH);
        if (in(track)) return CID_SCROLLTRACK;
    }

    // 内容区：鼠标 y → 内容坐标（y + scroll）再和布局矩形比
    const float cy = y + g_app.scroll;
    auto inC = [&](const D2D1_RECT_F& r) {
        return x >= r.left && x <= r.right && cy >= r.top && cy <= r.bottom;
    };

    if (inC(L.addThemeBtn)) return CID_ADD_THEME;
    // 卡片右上角小按钮：只在悬停该卡片（按钮画出来了）时才能点，避免误删/误改
    for (int i = 0; i < L.cardCount; ++i)
        if (i < (int)g_app.delVisible.size() && g_app.delVisible[i] && inC(L.themeDel[i]))
            return CID_THEME_DEL0 + i;
    for (int i = 0; i < L.cardCount; ++i)
        if (i < (int)g_app.delVisible.size() && g_app.delVisible[i] && inC(L.themeRen[i]))
            return CID_THEME_REN0 + i;
    for (int i = 0; i < L.cardCount; ++i)
        if (inC(L.themeCards[i])) return CID_THEME0 + i;
    // 时间步进器：左右各 36 宽。跟随系统模式下时段不生效：不响应点击（画成半透明）
    if (g_app.cfg.switchMode == 0) {
        if (inC(L.dayStepper))
            return x < L.dayStepper.left + 36 ? CID_DAY_MINUS : CID_DAY_PLUS;
        if (inC(L.nightStepper))
            return x < L.nightStepper.left + 36 ? CID_NIGHT_MINUS : CID_NIGHT_PLUS;
    }
    if (inC(L.appearanceSeg)) {
        float segW = (L.appearanceSeg.right - L.appearanceSeg.left) / 3.0f;
        int seg = (int)((x - L.appearanceSeg.left) / segW);
        if (seg < 0) seg = 0;
        if (seg > 2) seg = 2;
        return CID_APPEARANCE0 + seg;
    }
    if (inC(L.switchSeg))
        return x < (L.switchSeg.left + L.switchSeg.right) / 2 ? CID_SWITCH0 : CID_SWITCH1;
    if (inC(L.autoToggle)) return CID_AUTO;
    if (inC(L.bootToggle)) return CID_BOOT;
    if (inC(L.openThemesBtn)) return CID_OPENTHEMES;
    return CID_NONE;
}

// ---------------------------------------------------------------------------
//  壁纸应用
// ---------------------------------------------------------------------------
static const ThemeInfo* SelectedTheme() {
    if (g_app.themes.empty()) return nullptr;
    if (g_app.selIdx < 0 || g_app.selIdx >= (int)g_app.themes.size()) g_app.selIdx = 0;
    return &g_app.themes[g_app.selIdx];
}

// 当前该显示哪张壁纸：0=浅色（主题里的日间图）1=深色（夜间图）。
// 切换方式 0 = 按时段；1 = 跟随系统深浅色。两种方式共用同一对图片，只是依据不同。
static int TargetStage() {
    if (g_app.cfg.switchMode == 1) return SystemUsesDarkMode() ? 1 : 0;
    return StageAt(g_app.cfg, CurrentMinutes());
}

static void ApplyCurrent(bool force) {
    const ThemeInfo* t = SelectedTheme();
    if (!t) return;
    int stage = TargetStage();
    const std::wstring& img = (stage == 0) ? t->dayImage : t->nightImage;
    if (!force && !g_app.cfg.autoSwitch) return;
    if (ApplyWallpaper(img, g_app.cfg.wallpaperStyle)) {
        g_app.statusMsg = std::wstring(L"已应用") + (stage == 0 ? L"浅色" : L"深色") + L"壁纸";
        g_app.statusMsgUntil = GetTickCount64() + 4000;
        if (g_app.glass) EnsureGlassBitmap(true);    // 换壁纸后立刻重采磨砂底
    }
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
//  绘制
// ---------------------------------------------------------------------------
static void DrawTitleBar(Gfx& g, const Palette& p) {
    // 应用标记：logo（exe 资源 IDR_LOGO_PNG，圆形裁切已烘在图片里），20 DIP 见方；
    // 取不到时退回手绘的"左日右月"
    float ix = 24, iy = Ui::TitleBarH / 2;
    if (!g_app.logoBmp && !g_app.logoTried) {
        g_app.logoTried = true;
        g_app.logoBmp = g.loadImageFromResource(IDR_LOGO_PNG, 64);
    }
    if (g_app.logoBmp) {
        float s = 20.0f;
        D2D1_RECT_F dst = D2D1::RectF(ix - s / 2, iy - s / 2, ix + s / 2, iy + s / 2);
        g.dc()->DrawBitmap(g_app.logoBmp, dst, 1.0f,
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
    g.textOptical(kAppDisplay, f, D2D1::RectF(46, 0, 400, Ui::TitleBarH), p.textPrimary);

    // 最大化/还原、最小化、关闭（最大化用 HTMAXBUTTON，悬停时系统弹"贴靠布局"浮出）
    int maxMode = g_app.maximized ? Ui::CAP_RESTORE : Ui::CAP_MAX;
    UiCaptionButton(g, g_app.L.captionMax, maxMode, g_app.hot == CID_MAX,
                    g_app.pressed == CID_MAX, p);
    UiCaptionButton(g, g_app.L.captionMin, Ui::CAP_MIN, g_app.hot == CID_MIN,
                    g_app.pressed == CID_MIN, p);
    UiCaptionButton(g, g_app.L.captionClose, Ui::CAP_CLOSE, g_app.hot == CID_CLOSE,
                    g_app.pressed == CID_CLOSE, p);
}

static void DrawThemes(Gfx& g, const Palette& p) {
    const Layout& L = g_app.L;
    if ((int)g_app.delVisible.size() != kThemeSlots) g_app.delVisible.assign(kThemeSlots, 0);
    for (int i = 0; i < kThemeSlots; ++i) g_app.delVisible[i] = 0;   // 清上一帧的可见标记
    UiSectionTitle(g, L.themeSectionTitle.left, L.themeSectionTitle.top,
                   L.themeSectionTitle.right - L.themeSectionTitle.left, L"主题", p);
    UiTextButton(g, L.addThemeBtn, L"添加主题", p, g_app.hot == CID_ADD_THEME,
                 g_app.pressed == CID_ADD_THEME, L"\uE710");
    if (L.cardCount == 0) {
        IDWriteTextFormat* f = g.format(FONT_UI, 12.5f);
        g.text(L"还没有主题：点右上角「添加主题」，选一个放着壁纸图片的文件夹，挑出浅色、深色两张即可",
               f, D2D1::RectF(L.contentL, L.themeSectionTitle.top + 40, L.contentR,
                              L.themeSectionTitle.top + 64), p.textSecondary);
        return;
    }
    // 只画视口内的卡片（firstVis/lastVis 由 SyncThumbs 按滚动位置算好）
    for (int ti = L.firstVis; ti <= L.lastVis; ++ti) {
        if (ti < 0 || ti >= L.cardCount || ti >= (int)g_app.themes.size()) continue;
        // 主题固定是"浅色 + 深色"两张；老主题（多图）仍显示张数
        std::wstring sub = (g_app.themes[ti].imageCount == 2)
                               ? L"浅色 + 深色"
                               : (std::to_wstring(g_app.themes[ti].imageCount) + L" 张图片");
        bool showDel = (g_app.hot == CID_THEME0 + ti || g_app.hot == CID_THEME_DEL0 + ti ||
                        g_app.hot == CID_THEME_REN0 + ti);
        bool delHot  = (g_app.hot == CID_THEME_DEL0 + ti);
        bool renHot  = (g_app.hot == CID_THEME_REN0 + ti);
        g_app.delVisible[ti] = showDel ? 1 : 0;   // 命中测试用：只认"画出来了"的按钮
        ID2D1Bitmap* thumb = (ti < (int)g_app.thumbs.size()) ? g_app.thumbs[ti] : nullptr;
        UiThemeCard(g, L.themeCards[ti], thumb, g_app.themes[ti].name, sub,
                    ti == g_app.selIdx, g_app.hot == CID_THEME0 + ti, p,
                    showDel, delHot, renHot);
    }
}

static void DrawAppearance(Gfx& g, const Palette& p) {
    const Layout& L = g_app.L;
    UiSectionTitle(g, L.appearanceSectionTitle.left, L.appearanceSectionTitle.top,
                   L.appearanceSectionTitle.right - L.appearanceSectionTitle.left, L"外观", p);
    IDWriteTextFormat* f = g.format(FONT_UI, 13.5f);
    g.textOptical(L"界面模式", f, D2D1::RectF(L.contentL, L.appearanceSeg.top, L.appearanceSeg.left - 12, L.appearanceSeg.bottom),
                  p.textPrimary);
    std::vector<std::wstring> items = {L"跟随系统", L"浅色", L"深色"};
    int hotSeg = -1;
    if (g_app.hot >= CID_APPEARANCE0 && g_app.hot <= CID_APPEARANCE2)
        hotSeg = g_app.hot - CID_APPEARANCE0;
    // 外观从托盘菜单等处被改动时这里也会自动开始滑动（不用各处分别触发）
    if (g_app.segTarget != g_app.cfg.appearance) {
        g_app.segTarget = g_app.cfg.appearance;
        StartAnim();
    }
    UiSegmented(g, L.appearanceSeg, items, g_app.cfg.appearance, hotSeg, p,
                g_app.segAnim, g_app.segScale);
}

static void DrawSwitchSection(Gfx& g, const Palette& p) {
    const Layout& L = g_app.L;
    UiSectionTitle(g, L.switchSectionTitle.left, L.switchSectionTitle.top,
                   L.switchSectionTitle.right - L.switchSectionTitle.left, L"切换", p);
    IDWriteTextFormat* f = g.format(FONT_UI, 13.5f);
    g.textOptical(L"切换方式", f, D2D1::RectF(L.contentL, L.switchSeg.top, L.switchSeg.left - 12, L.switchSeg.bottom),
                  p.textPrimary);
    // 指示条平滑滑动 + 按下缩一下；从托盘菜单改了模式时这里也会自动开始滑动
    if (g_app.swTarget != g_app.cfg.switchMode) {
        g_app.swTarget = g_app.cfg.switchMode;
        StartAnim();
    }
    std::vector<std::wstring> modes = {L"按时段", L"跟随系统"};
    int hotSw = (g_app.hot == CID_SWITCH0) ? 0 : (g_app.hot == CID_SWITCH1 ? 1 : -1);
    UiSegmented(g, L.switchSeg, modes, g_app.cfg.switchMode, hotSw, p,
                g_app.swAnim, g_app.swScale);
}

static void DrawTimeSection(Gfx& g, const Palette& p) {
    const Layout& L = g_app.L;
    // 跟随系统模式下这一整块画成半透明（视觉上="不生效"），右下角另加一句说明。
    // 外层已有整帧透明度图层时不再嵌套（嵌套会让 D2D 只画出前半帧，
    // 见 App::frameLayerPushed）
    bool dim = (g_app.cfg.switchMode == 1) && !g_app.frameLayerPushed;
    if (dim) {
        if (!g_app.uiLayer) g.dc()->CreateLayer(nullptr, &g_app.uiLayer);
        if (g_app.uiLayer) {
            D2D1_LAYER_PARAMETERS lp = D2D1::LayerParameters(
                D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                D2D1::IdentityMatrix(), 0.4f, nullptr, D2D1_LAYER_OPTIONS_NONE);
            g.dc()->PushLayer(&lp, g_app.uiLayer);
        } else {
            dim = false;
        }
    }
    UiSectionTitle(g, L.timeSectionTitle.left, L.timeSectionTitle.top,
                   L.timeSectionTitle.right - L.timeSectionTitle.left, L"时段", p);
    IDWriteTextFormat* f = g.format(FONT_UI, 13.5f);
    // 行内标签与右侧控件同高，按墨迹居中（中文/数字不会看着偏上）
    g.textOptical(L"日间开始", f, D2D1::RectF(L.contentL, L.dayStepper.top, L.dayStepper.left - 12, L.dayStepper.bottom),
                  p.textPrimary);
    g.textOptical(L"夜间开始", f, D2D1::RectF(L.contentL, L.nightStepper.top, L.nightStepper.left - 12, L.nightStepper.bottom),
                  p.textPrimary);
    UiStepper(g, L.dayStepper, FormatHM(g_app.cfg.dayStart), p,
              g_app.hot == CID_DAY_MINUS ? 0 : (g_app.hot == CID_DAY_PLUS ? 1 : -1),
              g_app.pressed == CID_DAY_MINUS ? 0 : (g_app.pressed == CID_DAY_PLUS ? 1 : -1));
    UiStepper(g, L.nightStepper, FormatHM(g_app.cfg.nightStart), p,
              g_app.hot == CID_NIGHT_MINUS ? 0 : (g_app.hot == CID_NIGHT_PLUS ? 1 : -1),
              g_app.pressed == CID_NIGHT_MINUS ? 0 : (g_app.pressed == CID_NIGHT_PLUS ? 1 : -1));
    if (dim) {
        g.dc()->PopLayer();
        IDWriteTextFormat* fHint = g.format(FONT_UI, 12.0f);
        g.textOptical(L"当前跟随系统深浅色，时段设置不生效", fHint,
                      D2D1::RectF(L.contentL, L.timeSectionTitle.top, L.contentR, L.timeSectionTitle.top + 20),
                      p.textSecondary, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
}

static void DrawOptions(Gfx& g, const Palette& p) {
    const Layout& L = g_app.L;
    UiSectionTitle(g, L.optionSectionTitle.left, L.optionSectionTitle.top,
                   L.optionSectionTitle.right - L.optionSectionTitle.left, L"选项", p);
    IDWriteTextFormat* f = g.format(FONT_UI, 13.5f);
    g.textOptical(L"自动切换壁纸", f, D2D1::RectF(L.contentL, L.autoToggle.top, L.autoToggle.left - 12, L.autoToggle.bottom),
                  p.textPrimary);
    g.textOptical(L"开机自动启动", f, D2D1::RectF(L.contentL, L.bootToggle.top, L.bootToggle.left - 12, L.bootToggle.bottom),
                  p.textPrimary);
    UiToggle(g, L.autoToggle.left, L.autoToggle.top, g_app.cfg.autoSwitch, g_app.toggleAnim[0],
             g_app.hot == CID_AUTO, p);
    UiToggle(g, L.bootToggle.left, L.bootToggle.top, g_app.cfg.startOnBoot, g_app.toggleAnim[1],
             g_app.hot == CID_BOOT, p);
}

static void DrawStatus(Gfx& g, const Palette& p) {
    const Layout& L = g_app.L;
    const ThemeInfo* t = SelectedTheme();
    float cy = (L.statusRow.top + L.statusRow.bottom) / 2;
    int stage = TargetStage();
    UiStatusDot(g, L.statusRow.left + 5, cy, stage == 1, p);

    IDWriteTextFormat* fBold = g.format(FONT_UI, 13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    IDWriteTextFormat* fSmall = g.format(FONT_UI, 12.0f);
    // 从左到右依次排布文本块：当前状态 -> 模拟标记/提示 -> 图片名
    float x = L.statusRow.left + 18;
    auto run = [&](const std::wstring& s, IDWriteTextFormat* f, const D2D1_COLOR_F& c) {
        if (s.empty()) return;
        float w = g.textWidth(s, f);
        g.textOptical(s, f, D2D1::RectF(x, L.statusRow.top, x + w + 2, L.statusRow.bottom), c);
        x += w + 7;
    };
    std::wstring cur = (stage == 0) ? L"浅色" : L"深色";
    std::wstring src = (g_app.cfg.switchMode == 1) ? L"跟随系统"
                                                   : (stage == 0 ? L"日间时段" : L"夜间时段");
    run(std::wstring(L"当前：") + cur + L"（" + src + L"）", fBold, p.textPrimary);
    if (g_app.simulateHour >= 0) run(L"模拟", fSmall, p.accentText);
    if (!g_app.statusMsg.empty() && GetTickCount64() < g_app.statusMsgUntil) {
        run(g_app.statusMsg, fSmall, p.accentText);
    } else if (t) {
        std::wstring img = stage == 0 ? t->dayImage : t->nightImage;
        size_t sl = img.find_last_of(L"\\/");
        run(sl == std::wstring::npos ? img : img.substr(sl + 1), fSmall, p.textSecondary);
    }

    // 右侧：下次切换倒计时 / 跟随系统时的来源说明 / 已暂停
    IDWriteTextFormat* fRight = g.format(FONT_UI, 12.0f);
    if (!g_app.cfg.autoSwitch) {
        g.textOptical(L"自动切换已暂停（托盘菜单里可打开）", fRight,
                      D2D1::RectF((L.contentL + L.contentR) / 2, L.statusRow.top,
                                  L.statusRow.right, L.statusRow.bottom),
                      p.textSecondary, DWRITE_TEXT_ALIGNMENT_TRAILING);
    } else if (g_app.cfg.switchMode == 1) {
        std::wstring right = std::wstring(L"跟随系统深浅色（系统当前：") +
                             (SystemUsesDarkMode() ? L"深色" : L"浅色") + L"）";
        g.textOptical(right, fRight,
                      D2D1::RectF((L.contentL + L.contentR) / 2, L.statusRow.top,
                                  L.statusRow.right, L.statusRow.bottom),
                      p.textSecondary, DWRITE_TEXT_ALIGNMENT_TRAILING);
    } else {
        std::wstring right = std::wstring(L"下次切换 ") + FormatHM(NextSwitchAt(g_app.cfg, CurrentMinutes())) +
                             L"（还有 " + FormatDuration(MinutesUntilSwitch(g_app.cfg, CurrentMinutes())) + L"）";
        g.textOptical(right, fRight,
                      D2D1::RectF((L.contentL + L.contentR) / 2, L.statusRow.top,
                                  L.statusRow.right, L.statusRow.bottom),
                      p.textSecondary, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
}

// ---------------------------------------------------------------------------
//  文本输入框：编辑逻辑（主题名 / 重命名都用）
// ---------------------------------------------------------------------------
static std::wstring TrimStr(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

// 有选区时先删掉选区（输入/粘贴/退格都先走这一步）
static void EditDeleteSelection(TextEdit& e) {
    if (!e.hasSel()) return;
    int a, b;
    e.selRange(a, b);
    e.text.erase(a, b - a);
    e.caret = a;
    e.clearSel();
}

static void EditInsert(TextEdit& e, const std::wstring& s) {
    std::wstring add;
    for (wchar_t c : s) if (c >= 32 && c != 127) add += c;      // 过滤控制字符
    if (add.empty()) return;
    EditDeleteSelection(e);
    if (e.caret < 0) e.caret = 0;
    if (e.caret > (int)e.text.size()) e.caret = (int)e.text.size();
    e.text.insert(e.caret, add);
    e.caret += (int)add.size();
    e.clearSel();
}

// 前/后一个"字符"的位置（代理对一个字符，退格不会只吃半个 emoji）
static int PrevPos(const std::wstring& s, int pos) {
    if (pos <= 0) return 0;
    int p = pos - 1;
    if (p > 0 && s[p] >= 0xDC00 && s[p] <= 0xDFFF && s[p - 1] >= 0xD800 && s[p - 1] <= 0xDBFF) p--;
    return p;
}
static int NextPos(const std::wstring& s, int pos) {
    int n = (int)s.size();
    if (pos >= n) return n;
    int p = pos + 1;
    if (p < n && s[pos] >= 0xD800 && s[pos] <= 0xDBFF && s[p] >= 0xDC00 && s[p] <= 0xDFFF) p++;
    return p;
}

static void EditPaste(TextEdit& e) {
    if (!OpenClipboard(g_app.hwnd)) return;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t* p = (const wchar_t*)GlobalLock(h);
        if (p) {
            EditInsert(e, p);        // EditInsert 会过滤换行等控制字符
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
}

// vk：按键；shift：是否按住 Shift（扩展选择）
static void EditKeyDown(TextEdit& e, WPARAM vk, bool shift) {
    int n = (int)e.text.size();
    auto move = [&](int to) {
        if (shift) {
            if (!e.hasSel()) e.sel = e.caret;     // 开始扩展
            e.caret = to;
        } else {
            e.caret = to;
            e.clearSel();
        }
    };
    switch (vk) {
    case VK_LEFT:  move(PrevPos(e.text, e.caret)); break;
    case VK_RIGHT: move(NextPos(e.text, e.caret)); break;
    case VK_HOME:  move(0); break;
    case VK_END:   move(n); break;
    case VK_BACK:
        if (e.hasSel()) EditDeleteSelection(e);
        else if (e.caret > 0) {
            int p = PrevPos(e.text, e.caret);
            e.text.erase(p, e.caret - p);
            e.caret = p;
            e.clearSel();
        }
        break;
    case VK_DELETE:
        if (e.hasSel()) EditDeleteSelection(e);
        else if (e.caret < n) {
            e.text.erase(e.caret, NextPos(e.text, e.caret) - e.caret);
            e.clearSel();
        }
        break;
    default: break;
    }
}

// 渲染用的字符串（把输入法预编辑串插在光标处）+ 光标在其中的位置
static std::wstring EditDisplay(const TextEdit& e, int* caretOut) {
    std::wstring s = e.text;
    int c = e.caret < 0 ? 0 : (e.caret > (int)e.text.size() ? (int)e.text.size() : e.caret);
    if (!e.preedit.empty()) {
        s.insert(c, e.preedit);
        c += (int)e.preedit.size();
    }
    if (caretOut) *caretOut = c;
    return s;
}

static void EditSelectAll(TextEdit& e) {
    e.sel = 0;
    e.caret = (int)e.text.size();
}

// 按点击位置算光标（点击 = 取消选择）。x 是相对文本起点（已扣掉内边距）的距离
static void EditSetCaretFromX(Gfx& g, TextEdit& e, float x) {
    if (e.text.empty()) { e.caret = 0; e.clearSel(); return; }
    int caretPos = 0;
    std::wstring disp = EditDisplay(e, &caretPos);
    int pre = (int)e.preedit.size();
    int start = e.caret;
    IDWriteTextFormat* f = g.format(FONT_UI, 13.5f);
    int bestI = caretPos;
    float bestD = 1e9f;
    for (int i = 0; i <= (int)disp.size(); ++i) {
        float d = fabsf(g.textWidth(disp.substr(0, i), f) - x);
        if (d < bestD) { bestD = d; bestI = i; }
    }
    int pos;
    if (bestI <= start) pos = bestI;
    else if (bestI >= start + pre) pos = bestI - pre;
    else pos = start;                        // 落在预编辑串里：放到串前面
    e.caret = pos;
    e.clearSel();
}

// 有输入框的弹层打开时：挂上输入法 + 开始光标闪烁（关掉时反过来）
static void PrepareTextInput() {
    if (g_app.himc) ImmAssociateContext(g_app.hwnd, g_app.himc);
    g_app.caretOn = true;
    SetTimer(g_app.hwnd, TM_CARET, 500, nullptr);
}
static void EndTextInput() {
    KillTimer(g_app.hwnd, TM_CARET);
    ImmAssociateContext(g_app.hwnd, nullptr);      // 平时不挂输入法，免得随手按键弹候选窗
}

static TextEdit* ActiveEdit() {
    if (g_app.addUI.active) return &g_app.addUI.name;
    if (g_app.renUI.active) return &g_app.renUI.name;
    return nullptr;
}
static bool AnyDialogOpen() {
    return g_app.addUI.active || g_app.delUI.active || g_app.renUI.active;
}

// 把输入法的候选窗贴到当前输入框下面
static void ImeFollowEdit() {
    TextEdit* e = ActiveEdit();
    if (!e) return;
    HIMC hImc = ImmGetContext(g_app.hwnd);
    if (!hImc) return;
    const D2D1_RECT_F& r = g_app.addUI.active ? g_app.L.ovName : g_app.L.renField;
    POINT pt = {(int)(r.left * g_app.gfx.scale()), (int)((r.bottom + 4) * g_app.gfx.scale())};
    ClientToScreen(g_app.hwnd, &pt);
    COMPOSITIONFORM cf = {};
    cf.dwStyle = CFS_POINT;
    cf.ptCurrentPos = pt;
    ImmSetCompositionWindow(hImc, &cf);
    CANDIDATEFORM cdf = {};
    cdf.dwIndex = 0;
    cdf.dwStyle = CFS_CANDIDATEPOS;
    cdf.ptCurrentPos = pt;
    ImmSetCandidateWindow(hImc, &cdf);
    ImmReleaseContext(g_app.hwnd, hImc);
}

// WM_IME_COMPOSITION：取"结果串"（确定上屏）与"预编辑串"（还没确定，自己画）
static void ImeHandleComposition(LPARAM l) {
    TextEdit* e = ActiveEdit();
    if (!e) return;
    HIMC hImc = ImmGetContext(g_app.hwnd);
    if (!hImc) return;
    if (l & GCS_RESULTSTR) {
        LONG n = ImmGetCompositionStringW(hImc, GCS_RESULTSTR, nullptr, 0);
        if (n > 0) {
            std::wstring s((size_t)n / sizeof(wchar_t), 0);
            ImmGetCompositionStringW(hImc, GCS_RESULTSTR, &s[0], n);
            EditInsert(*e, s);
        }
        e->preedit.clear();
    }
    if (l & GCS_COMPSTR) {
        LONG n = ImmGetCompositionStringW(hImc, GCS_COMPSTR, nullptr, 0);
        e->preedit.assign(n > 0 ? (size_t)n / sizeof(wchar_t) : 0, 0);
        if (n > 0) ImmGetCompositionStringW(hImc, GCS_COMPSTR, &e->preedit[0], n);
    }
    ImmReleaseContext(g_app.hwnd, hImc);
    ImeFollowEdit();
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
//  文件对话框（系统的"打开文件"；只在点方框时弹，弹层本身不弹资源管理器）
// ---------------------------------------------------------------------------
static bool PickImageFile(const std::wstring& title, std::wstring& outPath) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IFileOpenDialog, (void**)&dlg)))
        return false;
    COMDLG_FILTERSPEC filters[] = {{L"图片", L"*.jpg;*.jpeg;*.png;*.bmp"},
                                   {L"所有文件", L"*.*"}};
    dlg->SetFileTypes(2, filters);
    dlg->SetFileTypeIndex(1);
    dlg->SetTitle(title.c_str());
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
    bool ok = false;
    if (SUCCEEDED(dlg->Show(g_app.hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                outPath = path;
                CoTaskMemFree(path);
                ok = true;
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

// 文件名（不含扩展名）——选完图后自动填进名字框（只在名字还空着时填）
static std::wstring FileBaseName(const std::wstring& path) {
    std::wstring s = path;
    size_t sl = s.find_last_of(L"\\/");
    if (sl != std::wstring::npos) s = s.substr(sl + 1);
    size_t dot = s.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) s = s.substr(0, dot);
    return s;
}

// ---------------------------------------------------------------------------
//  添加主题弹层
// ---------------------------------------------------------------------------
static void CloseAddTheme() {
    AddThemeUI& a = g_app.addUI;
    if (a.lightBmp) { a.lightBmp->Release(); a.lightBmp = nullptr; }
    if (a.darkBmp) { a.darkBmp->Release(); a.darkBmp = nullptr; }
    a.active = false;
    a.lightPath.clear();
    a.darkPath.clear();
    a.name.text.clear();
    a.name.caret = 0;
    a.name.preedit.clear();
    a.hint.clear();
    EndTextInput();
    g_app.hot = CID_NONE;
    g_app.pressed = CID_NONE;
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

static void OpenAddTheme() {
    AddThemeUI& a = g_app.addUI;
    if (a.lightBmp) { a.lightBmp->Release(); a.lightBmp = nullptr; }
    if (a.darkBmp) { a.darkBmp->Release(); a.darkBmp = nullptr; }
    a.lightPath.clear();
    a.darkPath.clear();
    a.name.text.clear();
    a.name.caret = 0;
    a.name.preedit.clear();
    a.hint.clear();
    a.active = true;
    g_app.hot = CID_NONE;
    g_app.pressed = CID_NONE;
    PrepareTextInput();
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

// 点方框 → 弹文件对话框挑一张图；选完立刻出缩略图
static void PickAddThemeImage(bool light) {
    AddThemeUI& a = g_app.addUI;
    std::wstring path;
    if (!PickImageFile(light ? L"选择浅色壁纸" : L"选择深色壁纸", path))
        return;
    ID2D1Bitmap* bmp = g_app.gfx.loadImage(path, 640);
    if (!bmp) {
        a.hint = L"这张图片打不开（支持 jpg / png / bmp）";
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
        return;
    }
    if (light) {
        if (a.lightBmp) a.lightBmp->Release();
        a.lightBmp = bmp;
        a.lightPath = path;
    } else {
        if (a.darkBmp) a.darkBmp->Release();
        a.darkBmp = bmp;
        a.darkPath = path;
    }
    if (TrimStr(a.name.text).empty()) {                 // 名字还空着就先填文件名
        a.name.text = FileBaseName(path);
        a.name.caret = (int)a.name.text.size();
        a.name.clearSel();
    }
    // 校验 + 清错误提示
    if (!a.lightPath.empty() && !a.darkPath.empty() &&
        _wcsicmp(a.lightPath.c_str(), a.darkPath.c_str()) == 0)
        a.hint = L"浅色和深色必须是两张不同的图片";
    else
        a.hint.clear();
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

static void ConfirmAddTheme() {
    AddThemeUI& a = g_app.addUI;
    std::wstring nm = TrimStr(a.name.text);
    if (a.lightPath.empty() || a.darkPath.empty()) {
        a.hint = L"请给浅色、深色各挑一张图片";
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
        return;
    }
    if (_wcsicmp(a.lightPath.c_str(), a.darkPath.c_str()) == 0) {
        a.hint = L"浅色和深色必须是两张不同的图片";
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
        return;
    }
    if (nm.empty()) {
        a.hint = L"请给主题起个名字";
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
        return;
    }
    // 名字形状的即时校验在弹层绘制里做过，这里再拦一道是给"回车确认"兜底：
    // 非法名字以前会被 CreateDirectoryW 拒绝、却报成"目录不可写"
    std::wstring nameErr;
    if (!ThemeNameValid(nm, nameErr)) {
        a.hint = nameErr;
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
        return;
    }
    std::wstring outName, err;
    if (!ImportTheme(nm, a.lightPath, a.darkPath, outName, &err)) {
        a.hint = L"添加失败" + (err.empty() ? std::wstring(L"") : (L"：" + err));
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
        return;
    }
    g_app.themes = ScanThemes();
    g_app.selIdx = 0;
    for (size_t i = 0; i < g_app.themes.size(); ++i)
        if (g_app.themes[i].name == outName) g_app.selIdx = (int)i;
    g_app.cfg.theme = outName;
    SaveConfig(g_app.cfg);
    ClearThumbs();
    ComputeLayout(g_app.L);
    EnsureCardVisible(g_app.selIdx);     // 新主题若排在第二行之后，滚过去让它露出来
    ApplyCurrent(true);
    g_app.statusMsg = L"已添加主题：" + outName;
    g_app.statusMsgUntil = GetTickCount64() + 6000;
    CloseAddTheme();
}

// ---------------------------------------------------------------------------
//  主题重命名（卡片右上角"✎"打开）
// ---------------------------------------------------------------------------
static void CloseRenameTheme() {
    RenameUI& r = g_app.renUI;
    r.active = false;
    r.idx = -1;
    r.name.text.clear();
    r.name.caret = 0;
    r.name.preedit.clear();
    r.hint.clear();
    EndTextInput();
    g_app.hot = CID_NONE;
    g_app.pressed = CID_NONE;
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

static void OpenRenameTheme(int idx) {
    if (idx < 0 || idx >= (int)g_app.themes.size()) return;
    RenameUI& r = g_app.renUI;
    r.idx = idx;
    r.name.text = g_app.themes[idx].name;
    r.name.caret = (int)r.name.text.size();
    r.name.clearSel();
    r.name.preedit.clear();
    r.hint.clear();
    r.active = true;
    g_app.hot = CID_NONE;
    g_app.pressed = CID_NONE;
    PrepareTextInput();
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

static void ConfirmRenameTheme() {
    RenameUI& r = g_app.renUI;
    if (r.idx < 0 || r.idx >= (int)g_app.themes.size()) { CloseRenameTheme(); return; }
    std::wstring oldName = g_app.themes[r.idx].name;
    std::wstring folder = g_app.themes[r.idx].folder;
    std::wstring nm = TrimStr(r.name.text);
    if (nm == oldName) { CloseRenameTheme(); return; }        // 没改就什么也不做
    std::wstring err;
    if (!RenameTheme(folder, nm, err)) {
        r.hint = err;
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
        return;
    }
    bool wasSelected = (g_app.cfg.theme == oldName);
    g_app.themes = ScanThemes();
    if (wasSelected) g_app.cfg.theme = nm;
    g_app.selIdx = 0;
    for (size_t i = 0; i < g_app.themes.size(); ++i)
        if (g_app.themes[i].name == g_app.cfg.theme) g_app.selIdx = (int)i;
    SaveConfig(g_app.cfg);
    ClearThumbs();
    ComputeLayout(g_app.L);
    EnsureCardVisible(g_app.selIdx);
    ApplyCurrent(true);
    g_app.statusMsg = L"已重命名主题：" + oldName + L" → " + nm;
    g_app.statusMsgUntil = GetTickCount64() + 6000;
    CloseRenameTheme();
}

// ---------------------------------------------------------------------------
//  删除主题确认框
// ---------------------------------------------------------------------------
static void OpenDelTheme(int idx) {
    if (idx < 0 || idx >= (int)g_app.themes.size()) return;
    g_app.delUI.active = true;
    g_app.delUI.idx = idx;
    g_app.delUI.err.clear();
    g_app.hot = CID_NONE;
    g_app.pressed = CID_NONE;
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

static void CloseDelTheme() {
    g_app.delUI.active = false;
    g_app.delUI.idx = -1;
    g_app.delUI.err.clear();
    g_app.hot = CID_NONE;
    g_app.pressed = CID_NONE;
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

static void ConfirmDeleteTheme() {
    DelUI& d = g_app.delUI;
    if (d.idx < 0 || d.idx >= (int)g_app.themes.size()) { CloseDelTheme(); return; }
    ThemeInfo t = g_app.themes[d.idx];       // 先拷一份，后面要重建列表
    bool wasSelected = (d.idx == g_app.selIdx);

    std::wstring err;
    if (!DeleteThemeFolder(t.folder, err)) {
        d.err = err;                         // 留在框里提示，用户可以直接取消
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
        return;
    }

    // 重建主题列表，选中项挪到合理位置（删的在选中项前面时前移一位）
    g_app.themes = ScanThemes();
    int n = (int)g_app.themes.size();
    int sel = g_app.selIdx - (d.idx < g_app.selIdx ? 1 : 0);
    if (sel > n - 1) sel = n - 1;
    if (sel < 0) sel = 0;
    g_app.selIdx = sel;
    g_app.cfg.theme = n == 0 ? L"" : g_app.themes[sel].name;
    SaveConfig(g_app.cfg);
    ClearThumbs();
    ComputeLayout(g_app.L);
    if (wasSelected) ApplyCurrent(true);     // 删的正是当前主题：立刻切到下一个

    g_app.statusMsg = L"已删除主题：" + t.name + L"（可在回收站里找回）";
    g_app.statusMsgUntil = GetTickCount64() + 6000;
    CloseDelTheme();
}

// 等比铺满（cover）绘制缩略图
static void DrawThumbCover(Gfx& g, ID2D1Bitmap* bmp, const D2D1_RECT_F& r, float radius,
                          const Palette& p) {
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, radius, radius);
    g.fillRound(rr, ColAlpha(p.textPrimary, 0.05f));
    if (bmp) {
        D2D1_SIZE_F sz = bmp->GetSize();
        float tw = r.right - r.left, th = r.bottom - r.top;
        if (sz.width > 0 && sz.height > 0) {
            float s = (sz.width / sz.height > tw / th) ? (th / sz.height) : (tw / sz.width);
            float dw = sz.width * s, dh = sz.height * s;
            D2D1_RECT_F dst = D2D1::RectF(r.left + (tw - dw) / 2, r.top + (th - dh) / 2,
                                          r.left + (tw - dw) / 2 + dw, r.top + (th - dh) / 2 + dh);
            g.pushClip(rr);
            g.dc()->DrawBitmap(bmp, dst, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            g.popClip();
        }
    }
    g.strokeRound(rr, ColAlpha(p.textPrimary, 0.12f), 1.0f);
}

// 输入框：圆角底 + 边框（聚焦态用强调色）+ 文本 + 光标 + 占位符。
// 文本比框宽时自动左移，光标始终留在框内。
static void DrawTextEdit(Gfx& g, const D2D1_RECT_F& r, TextEdit& e, const Palette& p,
                         const std::wstring& placeholder, bool focused) {
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, 5, 5);
    g.fillRound(rr, p.dark ? Col(1, 1, 1, 0.07f) : Col(0, 0, 0, 0.035f));
    g.strokeRound(rr, focused ? p.accent : (p.dark ? Col(1, 1, 1, 0.12f) : Col(0, 0, 0, 0.10f)),
                  focused ? 1.4f : 1.0f);
    IDWriteTextFormat* f = g.format(FONT_UI, 13.5f);
    float pad = 10.0f, avail = (r.right - r.left) - pad * 2;
    bool empty = e.text.empty() && e.preedit.empty();
    if (empty)
        g.textOptical(placeholder, f, D2D1::RectF(r.left + pad, r.top, r.right - pad, r.bottom),
                      p.textTertiary);
    g.pushClip(rr);
    float shift = 0;
    if (!empty) {
        int caretPos = 0;
        std::wstring disp = EditDisplay(e, &caretPos);
        float caretX = g.textWidth(disp.substr(0, caretPos), f);
        if (caretX > avail) shift = caretX - avail;
        // 选中高亮：先画色块，再画文字（强调色的半透明底）
        if (e.hasSel()) {
            int a, b;
            e.selRange(a, b);
            float x1 = g.textWidth(disp.substr(0, a), f);
            float x2 = g.textWidth(disp.substr(0, b), f);
            D2D1_RECT_F hl = D2D1::RectF(r.left + pad + x1 - shift, r.top + 5,
                                         r.left + pad + x2 - shift, r.bottom - 5);
            g.dc()->FillRoundedRectangle(D2D1::RoundedRect(hl, 2, 2), g.brush(ColAlpha(p.accent, 0.35f)));
        }
        g.textOptical(disp, f, D2D1::RectF(r.left + pad - shift, r.top, r.right - pad - shift, r.bottom),
                      p.textPrimary);
        if (focused && g_app.caretOn) {
            float cy = (r.top + r.bottom) / 2, chh = (r.bottom - r.top) * 0.46f;
            float x = r.left + pad + caretX - shift;
            g.dc()->DrawLine(D2D1::Point2F(x, cy - chh / 2), D2D1::Point2F(x, cy + chh / 2),
                             g.brush(p.textPrimary), 1.2f);
        }
    } else if (focused && g_app.caretOn) {
        float cy = (r.top + r.bottom) / 2, chh = (r.bottom - r.top) * 0.46f;
        g.dc()->DrawLine(D2D1::Point2F(r.left + pad, cy - chh / 2),
                         D2D1::Point2F(r.left + pad, cy + chh / 2), g.brush(p.textPrimary), 1.2f);
    }
    g.popClip();
}

// 选图方框：空 = 加号 + 提示；已选 = 图片（cover 铺满），悬停时压一层纱提示更换
static void DrawImageSlot(Gfx& g, const D2D1_RECT_F& r, ID2D1Bitmap* bmp, const std::wstring& hint,
                          const Palette& p, bool hot, bool pressed) {
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, 6, 6);
    if (bmp) {
        DrawThumbCover(g, bmp, r, 6, p);
        if (hot || pressed) {
            g.fillRound(rr, Col(0, 0, 0, pressed ? 0.34f : 0.24f));
            IDWriteTextFormat* f = g.format(FONT_UI, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                            DWRITE_TEXT_ALIGNMENT_CENTER);
            g.textOptical(L"点击更换", f, D2D1::RectF(r.left, r.top, r.right, r.bottom),
                          Col(1, 1, 1, 0.95f), DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    } else {
        g.fillRound(rr, hot || pressed ? ColAlpha(p.accent, 0.10f)
                                       : (p.dark ? Col(1, 1, 1, 0.04f) : Col(0, 0, 0, 0.025f)));
        g.strokeRound(rr, hot || pressed ? p.accent
                                         : (p.dark ? Col(1, 1, 1, 0.16f) : Col(0, 0, 0, 0.14f)), 1.0f);
        IDWriteTextFormat* gi = g.format(FONT_ICON, 22.0f, DWRITE_FONT_WEIGHT_NORMAL,
                                         DWRITE_TEXT_ALIGNMENT_CENTER);
        g.textOptical(L"\uE710", gi, D2D1::RectF(r.left, r.top + 36, r.right, r.top + 74),
                      hot || pressed ? p.accentText : p.textSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
        IDWriteTextFormat* f = g.format(FONT_UI, 12.0f, DWRITE_FONT_WEIGHT_NORMAL,
                                        DWRITE_TEXT_ALIGNMENT_CENTER);
        g.textOptical(hint, f, D2D1::RectF(r.left + 8, r.top + 78, r.right - 8, r.top + 102),
                      p.textSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

static void DrawAddThemeOverlay(Gfx& g, const Palette& p) {
    UiDim(g, g_app.winWDip, g_app.winHDip, p);
    const Layout& L = g_app.L;
    AddThemeUI& a = g_app.addUI;
    // 弹层卡片用不透明底色（半透明卡片会让背后内容透出来，像坏掉一样）
    D2D1_ROUNDED_RECT card = D2D1::RoundedRect(L.ovCard, 8, 8);
    g.fillRound(card, p.dark ? ColHex(0x272727) : ColHex(0xFAFAFA));
    g.strokeRound(card, p.dark ? Col(1, 1, 1, 0.10f) : Col(0, 0, 0, 0.08f), 1.0f);

    IDWriteTextFormat* fTitle = g.format(FONT_UI, 18.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    IDWriteTextFormat* fSub   = g.format(FONT_UI, 12.5f);
    IDWriteTextFormat* fLabel = g.format(FONT_UI, 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    float x0 = L.ovCard.left + 24, x1 = L.ovCard.right - 24;

    g.textOptical(L"添加主题", fTitle, D2D1::RectF(x0, L.ovCard.top + 20, x1, L.ovCard.top + 50),
                  p.textPrimary);
    g.textOptical(L"主题名称", fLabel, D2D1::RectF(x0, L.ovCard.top + 54, x1, L.ovCard.top + 74),
                  p.textSecondary);
    DrawTextEdit(g, L.ovName, a.name, p, L"例如：沙漠、海边日出…", true);

    auto caption = [&](const D2D1_RECT_F& slot, const wchar_t* t) {
        g.textOptical(t, fLabel, D2D1::RectF(slot.left, slot.top - 22, slot.right, slot.top - 2),
                      p.textSecondary);
    };
    caption(L.ovLightSlot, L"浅色");
    caption(L.ovDarkSlot, L"深色");
    DrawImageSlot(g, L.ovLightSlot, a.lightBmp, L"点击选择浅色壁纸", p,
                  g_app.hot == CID_OV_LIGHT_SLOT, g_app.pressed == CID_OV_LIGHT_SLOT);
    DrawImageSlot(g, L.ovDarkSlot, a.darkBmp, L"点击选择深色壁纸", p,
                  g_app.hot == CID_OV_DARK_SLOT, g_app.pressed == CID_OV_DARK_SLOT);

    // 底部提示：出错用强调色；就绪时写明将要保存到哪里。
    // 名字即时校验每帧做（纯字符串判断）：非法字符当场提示并禁用「添加」
    std::wstring nm = TrimStr(a.name.text);
    std::wstring nameErr;
    bool nameOk = nm.empty() || ThemeNameValid(nm, nameErr);
    bool sameImage = !a.lightPath.empty() && !a.darkPath.empty() &&
                     _wcsicmp(a.lightPath.c_str(), a.darkPath.c_str()) == 0;
    bool ready = !a.lightPath.empty() && !a.darkPath.empty() && !sameImage &&
                 !nm.empty() && nameOk;
    std::wstring hint;
    bool hintIsError = false;
    if (!a.hint.empty()) { hint = a.hint; hintIsError = true; }
    else if (!nameOk) { hint = nameErr; hintIsError = true; }
    else if (ready) {
        std::wstring dir = UserThemeDir(), exe = ExeDir(), shown = dir;
        if (!exe.empty() && dir.rfind(exe, 0) == 0)          // 在程序目录里显示成相对形式
            shown = L"程序目录" + dir.substr(exe.size());
        hint = L"将这两张图片复制到 " + shown + L"\\" + nm +
               L"\\（之后与原始图片位置无关）";
    }
    if (!hint.empty())
        g.textOptical(hint, fSub, D2D1::RectF(x0, L.ovCard.bottom - 96, x1, L.ovCard.bottom - 72),
                      hintIsError ? p.accentText : p.textSecondary);

    UiButton(g, L.ovCancel, L"取消", p, false, g_app.hot == CID_OV_CANCEL,
             g_app.pressed == CID_OV_CANCEL);
    UiButton(g, L.ovOk, L"添加", p, true, g_app.hot == CID_OV_OK, g_app.pressed == CID_OV_OK,
             ready, L"\uE710");
}

static void DrawRenameDialog(Gfx& g, const Palette& p) {
    UiDim(g, g_app.winWDip, g_app.winHDip, p);
    const Layout& L = g_app.L;
    RenameUI& r = g_app.renUI;
    D2D1_ROUNDED_RECT card = D2D1::RoundedRect(L.renCard, 8, 8);
    g.fillRound(card, p.dark ? ColHex(0x272727) : ColHex(0xFAFAFA));
    g.strokeRound(card, p.dark ? Col(1, 1, 1, 0.10f) : Col(0, 0, 0, 0.08f), 1.0f);

    std::wstring oldName, folder;
    if (r.idx >= 0 && r.idx < (int)g_app.themes.size()) {
        oldName = g_app.themes[r.idx].name;
        folder = g_app.themes[r.idx].folder;
    }
    IDWriteTextFormat* fTitle = g.format(FONT_UI, 16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    IDWriteTextFormat* fSub   = g.format(FONT_UI, 12.5f);
    IDWriteTextFormat* fPath  = g.format(FONT_UI, 11.0f);
    float x0 = L.renCard.left + 24, x1 = L.renCard.right - 24;

    g.textOptical(L"重命名主题「" + oldName + L"」", fTitle,
                  D2D1::RectF(x0, L.renCard.top + 20, x1, L.renCard.top + 48), p.textPrimary);
    g.textOptical(L"改的是名字（文件夹会一起改名），主题里的图片不会动。", fSub,
                  D2D1::RectF(x0, L.renCard.top + 54, x1, L.renCard.top + 76), p.textSecondary);
    // 路径显示成"程序目录\..."的短形式（长路径会被卡片截断）
    std::wstring shown = folder;
    std::wstring exe = ExeDir();
    if (!exe.empty() && folder.rfind(exe, 0) == 0) shown = L"程序目录" + folder.substr(exe.size());
    g.textOptical(shown, fPath, D2D1::RectF(x0, L.renCard.top + 78, x1, L.renCard.top + 98),
                  p.textTertiary);
    DrawTextEdit(g, L.renField, r.name, p, L"新名称", true);
    if (!r.hint.empty()) {
        g.textOptical(r.hint, fSub, D2D1::RectF(x0, L.renCard.top + 146, x1, L.renCard.top + 170),
                      p.accentText);
    }
    UiButton(g, L.renCancel, L"取消", p, false, g_app.hot == CID_REN_CANCEL,
             g_app.pressed == CID_REN_CANCEL);
    UiButton(g, L.renOk, L"保存", p, true, g_app.hot == CID_REN_OK, g_app.pressed == CID_REN_OK,
             !TrimStr(r.name.text).empty());
}

static void DrawDelThemeDialog(Gfx& g, const Palette& p) {
    UiDim(g, g_app.winWDip, g_app.winHDip, p);
    const Layout& L = g_app.L;
    DelUI& d = g_app.delUI;
    D2D1_ROUNDED_RECT card = D2D1::RoundedRect(L.dlgCard, 8, 8);
    g.fillRound(card, p.dark ? ColHex(0x272727) : ColHex(0xFAFAFA));
    g.strokeRound(card, p.dark ? Col(1, 1, 1, 0.10f) : Col(0, 0, 0, 0.08f), 1.0f);

    std::wstring name, folder;
    if (d.idx >= 0 && d.idx < (int)g_app.themes.size()) {
        name = g_app.themes[d.idx].name;
        folder = g_app.themes[d.idx].folder;
    }
    IDWriteTextFormat* fTitle = g.format(FONT_UI, 16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    IDWriteTextFormat* fBody  = g.format(FONT_UI, 12.5f, DWRITE_FONT_WEIGHT_NORMAL,
                                         DWRITE_TEXT_ALIGNMENT_LEADING, true);
    IDWriteTextFormat* fPath  = g.format(FONT_UI, 11.0f, DWRITE_FONT_WEIGHT_NORMAL,
                                         DWRITE_TEXT_ALIGNMENT_LEADING, true);
    float x0 = L.dlgCard.left + 24, x1 = L.dlgCard.right - 24, y0 = L.dlgCard.top;

    g.textOptical(L"删除主题「" + name + L"」？", fTitle,
                  D2D1::RectF(x0, y0 + 20, x1, y0 + 48), p.textPrimary);
    g.text(L"主题文件夹会被移到回收站（可从回收站拖回来恢复），不会影响你的原始图片。",
           fBody, D2D1::RectF(x0, y0 + 56, x1, y0 + 92), p.textSecondary);
    g.text(folder, fPath, D2D1::RectF(x0, y0 + 92, x1, y0 + 132), p.textTertiary);
    if (!d.err.empty()) {
        IDWriteTextFormat* fErr = g.format(FONT_UI, 12.0f, DWRITE_FONT_WEIGHT_NORMAL,
                                           DWRITE_TEXT_ALIGNMENT_LEADING, true);
        g.textOptical(d.err, fErr, D2D1::RectF(x0, y0 + 132, x1, y0 + 156), ColHex(0xC42B1C));
    }

    UiButton(g, L.dlgCancel, L"取消", p, false, g_app.hot == CID_DLG_CANCEL,
             g_app.pressed == CID_DLG_CANCEL);
    // 删除用"危险色"按钮（临时替换调色板里的强调色）
    Palette pd = p;
    pd.accent        = ColHex(0xC42B1C);
    pd.accentHover   = ColHex(0xD13A2B);
    pd.accentPressed = ColHex(0xA82318);
    pd.textOnAccent  = Col(1, 1, 1, 1);
    UiButton(g, L.dlgOk, L"删除", pd, true, g_app.hot == CID_DLG_OK, g_app.pressed == CID_DLG_OK);
}

// ---------------------------------------------------------------------------
//  窗口底色
//
//  Win11 22H2+：DwmSetWindowAttribute(DWMWA_SYSTEMBACKDROP_TYPE = 3)，DWM 实时
//  模糊窗口背后的所有内容（含其他窗口）。前提：客户区真的透明（不能铺不透明底），
//  窗口必须带 WS_EX_NOREDIRECTIONBITMAP。
//  Win10：纯色底（跟随深浅色）。AccentPolicy 亚克力拖动时模糊滞后、自绘磨砂只是
//  模拟壁纸，都不如纯色干净。
//  --self-glass 强制自绘磨砂（DrawGlassBackdrop），--opaque 强制不透明渐变。
// ---------------------------------------------------------------------------
static bool IsWin11_22H2OrLater() {
    typedef LONG (WINAPI* PFN_RtlGetVersion)(PRTL_OSVERSIONINFOW);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    if (!nt) return false;
    PFN_RtlGetVersion fn = (PFN_RtlGetVersion)(void*)GetProcAddress(nt, "RtlGetVersion");
    if (!fn) return false;
    RTL_OSVERSIONINFOW vi = {};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (fn(&vi) != 0) return false;
    return vi.dwMajorVersion > 10 ||
           (vi.dwMajorVersion == 10 && vi.dwBuildNumber >= 22621);
}

// 自绘磨砂：把当前壁纸缩到约 1/16 再放大铺满窗口（缩小再放大即模糊），
// 上面叠色调纱 + 淡颗粒噪点。壁纸按"填充(cover)"映射到屏幕坐标——窗口移动时
// 看到的是它背后那一块壁纸；壁纸换了（本程序切主题、或系统里换）约 1.5 秒内重采。
struct Glass {
    std::wstring path;                      // 已加载的壁纸路径
    ID2D1Bitmap* small = nullptr;           // 缩小版壁纸（模糊源）
    ID2D1Bitmap* noise = nullptr;           // 颗粒噪点（128×128，循环铺）
    ID2D1BitmapBrush* noiseBrush = nullptr;
    int screenW = 0, screenH = 0;
};
static Glass g_glass;

// 毛玻璃浓度：alpha 越小越"透"，越大越"雾"
static D2D1_COLOR_F GlassVeil(const Palette& p) {
    return p.dark ? Col(0.05f, 0.05f, 0.06f, 0.38f)
                  : Col(1.0f, 1.0f, 1.0f, 0.46f);
}

static void EnsureGlassBitmap(bool force = false) {
    static ULONGLONG lastCheck = 0;
    ULONGLONG now = GetTickCount64();
    if (!force && now - lastCheck < 1500) return;   // 注册表最多 1.5 秒查一次
    lastCheck = now;
    std::wstring wp = CurrentWallpaperPath();
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    if (g_glass.small && wp == g_glass.path && sw == g_glass.screenW && sh == g_glass.screenH)
        return;
    if (g_glass.small) { g_glass.small->Release(); g_glass.small = nullptr; }
    g_glass.path = wp;
    g_glass.screenW = sw;
    g_glass.screenH = sh;
    if (wp.empty()) return;
    int smallW = sw / 16;                           // 缩得越小越模糊
    if (smallW < 48) smallW = 48;
    g_glass.small = g_app.gfx.loadImage(wp, (UINT)smallW);
}

// 颗粒：固定种子，深/浅两种极淡的小点交替（预乘 alpha 0~13）
static void EnsureGlassNoise(Gfx& g) {
    if (g_glass.noiseBrush) return;
    const int N = 128;
    unsigned* px = new unsigned[N * N];
    unsigned s = 0x9E3779B9u;
    for (int i = 0; i < N * N; ++i) {
        s = s * 1664525u + 1013904223u;
        unsigned v = (s >> 16) & 0xFF;
        unsigned a = v % 14;
        unsigned c = (v & 1) ? a : 0;
        px[i] = (a << 24) | (c << 16) | (c << 8) | c;
    }
    g_glass.noise = g.createBitmapFromPixels(px, N, N);
    delete[] px;
    if (g_glass.noise) {
        D2D1_BITMAP_BRUSH_PROPERTIES bp = D2D1::BitmapBrushProperties(
            D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        g.dc()->CreateBitmapBrush(g_glass.noise, bp, &g_glass.noiseBrush);
    }
}

static void DrawGlassBackdrop(Gfx& g, const Palette& p) {
    const float w = g_app.winWDip, h = g_app.winHDip;
    EnsureGlassBitmap();
    if (g_glass.small) {
        // 壁纸按 cover 铺满屏幕：把窗口的屏幕矩形映射回位图坐标
        D2D1_SIZE_F bm = g_glass.small->GetSize();
        float sw = (float)g_glass.screenW, sh = (float)g_glass.screenH;
        if (bm.width > 0 && bm.height > 0) {
            float k = (sw / bm.width > sh / bm.height) ? (sw / bm.width) : (sh / bm.height);
            float ox = (sw - bm.width * k) / 2, oy = (sh - bm.height * k) / 2;
            RECT wr;
            GetWindowRect(g_app.hwnd, &wr);
            float sc = g_app.gfx.scale();
            D2D1_RECT_F src = D2D1::RectF(
                ((float)wr.left - ox) / k, ((float)wr.top - oy) / k,
                ((float)wr.left + w * sc - ox) / k, ((float)wr.top + h * sc - oy) / k);
            g.dc()->DrawBitmap(g_glass.small, D2D1::RectF(0, 0, w, h), 1.0f,
                               D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, src);
        }
    } else {
        // 取不到壁纸（纯色桌面）：退回纯色底
        g.dc()->Clear(p.dark ? ColHex(0x1C1C1C) : ColHex(0xF3F3F3));
    }
    // 色调纱：保证上层文字/控件的可读性
    g.dc()->FillRectangle(D2D1::RectF(0, 0, w, h), g.brush(GlassVeil(p)));
    // 颗粒锚在屏幕上（拖动窗口时颗粒不"跟着滑"，和系统亚克力的手感一致）
    EnsureGlassNoise(g);
    if (g_glass.noiseBrush) {
        RECT wr;
        GetWindowRect(g_app.hwnd, &wr);
        float sc = g_app.gfx.scale();
        float ox = (float)(((int)(wr.left / sc)) % 128);
        float oy = (float)(((int)(wr.top / sc)) % 128);
        g_glass.noiseBrush->SetTransform(D2D1::Matrix3x2F::Translation(ox, oy));
        g.dc()->FillRectangle(D2D1::RectF(0, 0, w, h), g_glass.noiseBrush);
    }
}

// 滚动条：右侧一根细滑块（WinUI 风格），滚动时出现，停手约 1 秒后淡出。
// 轨道本身不画（和 Win11 一致），鼠标停在滑块上会加浓一点。
static void DrawScrollbar(Gfx& g, const Palette& p) {
    const Layout& L = g_app.L;
    if (L.maxScroll <= 0.0f || g_app.sbAlpha <= 0.01f) return;
    D2D1_RECT_F th = ScrollThumbRect();
    bool hot = (g_app.hot == CID_SCROLLBAR || g_app.sbDragging);
    float a = g_app.sbAlpha * (hot ? 0.42f : 0.26f);
    D2D1_COLOR_F c = p.dark ? Col(1, 1, 1, a) : Col(0, 0, 0, a);
    g.fillRound(D2D1::RoundedRect(th, 3.0f, 3.0f), c);
}

// 最小化动画的整帧缩放矩阵：窗口矩形每帧在缩，但布局冻结在动画开始时的尺寸
//（WM_SIZE 被忽略）→ 绘制时把整帧等比缩到"当前窗口宽 / 冻结宽"，和系统动画
// "把窗口图整体缩下去"的观感一致。不在动画里时 = 单位矩阵。
static D2D1::Matrix3x2F MinAnimMatrix() {
    if (!g_app.minAnimating || g_app.minRefW <= 0) return D2D1::Matrix3x2F::Identity();
    RECT wr;
    if (!GetWindowRect(g_app.hwnd, &wr)) return D2D1::Matrix3x2F::Identity();
    float k = (float)(wr.right - wr.left) / (float)g_app.minRefW;
    if (k <= 0.0f || k > 1.0f) k = 1.0f;
    return D2D1::Matrix3x2F::Scale(k, k);
}

static void RenderFrame() {
    Gfx& g = g_app.gfx;
    const Palette& p = g_app.pal;
    SyncThumbs();                            // 可能释放/新建 D2D 位图，放在 begin 之前
    if (!g.begin()) return;
    g.dc()->Clear(Col(0, 0, 0, 0));

    // 窗口动画：整体淡入淡出 + 由下方轻推入（winAnim: 0=透明 1=完全显示）
    float frameAlpha = g_app.winAnim;
    bool useLayer = frameAlpha < 0.999f;
    float baseTy = 0.0f;                     // 内容整体的竖直偏移（淡入轻推 + 滚动）
    if (useLayer) {
        if (!g_app.uiLayer) g.dc()->CreateLayer(nullptr, &g_app.uiLayer);
        if (g_app.uiLayer) {
            D2D1_LAYER_PARAMETERS lp = D2D1::LayerParameters(
                D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                D2D1::IdentityMatrix(), frameAlpha, nullptr, D2D1_LAYER_OPTIONS_NONE);
            baseTy = (1.0f - g_app.winAnim) * 16.0f;
            g.dc()->PushLayer(&lp, g_app.uiLayer);
        } else {
            useLayer = false;
        }
    }
    g_app.frameLayerPushed = useLayer;

    // 窗口底色：系统亚克力 / 自绘磨砂 / 不透明渐变
    if (g_app.liveGlass) {
        // 客户区必须真的透明，DWM 的模糊才透得上来；再叠一层薄色调纱保证文字可读
        g.dc()->Clear(Col(0, 0, 0, 0));
        D2D1_COLOR_F veil = p.dark ? Col(0.04f, 0.04f, 0.05f, 0.32f)
                                   : Col(1.0f, 1.0f, 1.0f, 0.40f);
        g.dc()->FillRectangle(D2D1::RectF(0, 0, g_app.winWDip, g_app.winHDip), g.brush(veil));
    } else if (g_app.glass) {
        DrawGlassBackdrop(g, p);
    } else if (g_app.solidBg) {
        // Win10 默认：不模糊，一块跟随深浅色的纯色（浅 0xF3F3F3 / 深 0x202020）
        g.dc()->Clear(p.windowBgTop);
    } else {
        if (!g_app.bgBrush) {
            ID2D1GradientStopCollection* stops = nullptr;
            D2D1_GRADIENT_STOP gs[2] = {{0.0f, p.windowBgTop}, {1.0f, p.windowBgBottom}};
            if (SUCCEEDED(g.dc()->CreateGradientStopCollection(gs, 2, &stops)) && stops) {
                D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES props =
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0),
                                                        D2D1::Point2F(0, g_app.winHDip));
                g.dc()->CreateLinearGradientBrush(props, stops, &g_app.bgBrush);
                stops->Release();
            }
        }
        if (g_app.bgBrush)
            g.dc()->FillRectangle(D2D1::RectF(0, 0, g_app.winWDip, g_app.winHDip), g_app.bgBrush);
        else
            g.dc()->Clear(p.windowBgTop);
    }

    // 内容区：裁剪到标题栏以下 + 按滚动量整体上移（标题栏与弹层不参与滚动）
    g.dc()->PushAxisAlignedClip(
        D2D1::RectF(0, Ui::TitleBarH, g_app.winWDip, g_app.winHDip),
        D2D1_ANTIALIAS_MODE_ALIASED);
    g.dc()->SetTransform(MinAnimMatrix() *
                         D2D1::Matrix3x2F::Translation(0, baseTy - g_app.scroll));

    DrawThemes(g, p);
    DrawSwitchSection(g, p);
    DrawTimeSection(g, p);
    DrawAppearance(g, p);
    DrawOptions(g, p);
    DrawStatus(g, p);

    // 底部按钮（主题/时间/开关一改就自动应用，"立即应用"只在托盘菜单里）
    UiButton(g, g_app.L.openThemesBtn, L"打开主题文件夹", p, false,
             g_app.hot == CID_OPENTHEMES, g_app.pressed == CID_OPENTHEMES, true, L"\uE838");

    g.dc()->SetTransform(MinAnimMatrix());
    g.dc()->PopAxisAlignedClip();

    DrawTitleBar(g, p);
    DrawScrollbar(g, p);

    if (g_app.delUI.active) DrawDelThemeDialog(g, p);
    if (g_app.renUI.active) DrawRenameDialog(g, p);
    if (g_app.addUI.active) DrawAddThemeOverlay(g, p);   // 添加弹层盖在最上面

    if (useLayer) {
        g.dc()->PopLayer();
        g.dc()->SetTransform(MinAnimMatrix());
    }
    g.end();
}

// ---------------------------------------------------------------------------
//  托盘
// ---------------------------------------------------------------------------
static void TrayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"显示主界面");
    AppendMenuW(m, MF_STRING, 2, L"立即应用当前壁纸");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_app.cfg.autoSwitch ? MF_CHECKED : 0), 3, L"自动切换壁纸");
    // 切换方式二级菜单（与主界面的"切换方式"分段控件同一个设置）
    HMENU swmode = CreatePopupMenu();
    AppendMenuW(swmode, MF_STRING | (g_app.cfg.switchMode == 0 ? MF_CHECKED : 0), 21, L"按时段");
    AppendMenuW(swmode, MF_STRING | (g_app.cfg.switchMode == 1 ? MF_CHECKED : 0), 22, L"跟随系统深浅色");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)swmode, L"切换方式");
    AppendMenuW(m, MF_STRING | (g_app.cfg.startOnBoot ? MF_CHECKED : 0), 4, L"开机自动启动");
    // 外观二级菜单（与主界面的"外观"区域同一个设置）
    HMENU appearance = CreatePopupMenu();
    AppendMenuW(appearance, MF_STRING | (g_app.cfg.appearance == 0 ? MF_CHECKED : 0), 11, L"跟随系统");
    AppendMenuW(appearance, MF_STRING | (g_app.cfg.appearance == 1 ? MF_CHECKED : 0), 12, L"浅色");
    AppendMenuW(appearance, MF_STRING | (g_app.cfg.appearance == 2 ? MF_CHECKED : 0), 13, L"深色");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)appearance, L"外观");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 9, L"退出");
    SetForegroundWindow(g_app.hwnd);
    POINT pt;
    GetCursorPos(&pt);
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_app.hwnd, nullptr);
    DestroyMenu(m);
    switch (cmd) {
    case 1:
        ShowMainWindow();
        break;
    case 2: ApplyCurrent(true); break;
    case 3:
        g_app.cfg.autoSwitch = !g_app.cfg.autoSwitch;
        g_app.toggleTarget[0] = g_app.cfg.autoSwitch ? 1.0f : 0.0f;
        SaveConfig(g_app.cfg);
        if (g_app.cfg.autoSwitch) ApplyCurrent(true);
        StartAnim();
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
        break;
    case 4:
        g_app.cfg.startOnBoot = !g_app.cfg.startOnBoot;
        g_app.toggleTarget[1] = g_app.cfg.startOnBoot ? 1.0f : 0.0f;
        SetAutoStart(g_app.cfg.startOnBoot);
        SaveConfig(g_app.cfg);
        StartAnim();
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
        break;
    case 11: case 12: case 13:
        g_app.cfg.appearance = cmd - 11;
        SaveConfig(g_app.cfg);
        RefreshPalette();
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
        break;
    case 21: case 22:
        g_app.cfg.switchMode = cmd - 21;
        g_app.swTarget = g_app.cfg.switchMode;
        SaveConfig(g_app.cfg);
        if (g_app.cfg.autoSwitch) ApplyCurrent(true);
        StartAnim();
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
        break;
    case 9: DestroyWindow(g_app.hwnd); break;
    default: break;
    }
}

// Explorer 重启会广播 "TaskbarCreated"：托盘图标挂在旧任务栏上会随它消失，
// 收到这条消息必须自己把图标加回来。
static UINT g_taskbarCreatedMsg = 0;

static void AddTrayIcon() {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_app.hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP + 2;
    // 托盘图标清晰度：按系统小图标尺寸（随 DPI 变化，200% 缩放下是 32×32）取图，
    // 固定 16×16 会被系统放大而发虚。句柄只加载一次：每次新建不销毁会漏 GDI 句柄
    if (!g_app.trayIcon) {
        int cx = GetSystemMetrics(SM_CXSMICON);
        int cy = GetSystemMetrics(SM_CYSMICON);
        if (cx <= 0) cx = 16;
        if (cy <= 0) cy = 16;
        g_app.trayIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON),
                                           IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR);
        if (!g_app.trayIcon)
            g_app.trayIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON),
                                               IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_DEFAULTCOLOR);
    }
    nid.hIcon = g_app.trayIcon;
    wcscpy_s(nid.szTip, kAppDisplay);
    g_app.trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
}

static void RemoveTrayIcon() {
    if (!g_app.trayAdded) return;
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_app.hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_app.trayAdded = false;
}

// ---------------------------------------------------------------------------
//  缩略图 / 调色板
// ---------------------------------------------------------------------------
static void ClearThumbs() {
    for (ID2D1Bitmap* b : g_app.thumbs)
        if (b) b->Release();
    g_app.thumbs.clear();
    g_app.thumbTried.clear();
}

// 只保留"视口内 + 上下各一行缓冲"的缩略图：主题多时不占内存，
// 滚动到哪儿解到哪儿（滚动中每帧调用，只有范围变了才真的干活）。
static void SyncThumbs() {
    Layout& L = g_app.L;
    const int n = (int)g_app.themes.size();
    if ((int)g_app.thumbs.size() != n) {     // 主题列表变了（增/删/改名/重扫）
        ClearThumbs();
        g_app.thumbs.assign(n, nullptr);
        g_app.thumbTried.assign(n, 0);
    }
    if (n == 0) {
        L.firstVis = 0;
        L.lastVis = -1;
        return;
    }
    VisibleCardRange(L, g_app.scroll, L.firstVis, L.lastVis);
    if (L.lastVis < L.firstVis) {            // 一张卡片都不在视口里
        L.firstVis = 0;
        L.lastVis = -1;
    }
    const int perRow = L.perRow > 0 ? L.perRow : 1;
    const int keepA = L.firstVis - perRow;
    const int keepB = L.lastVis + perRow;
    for (int i = 0; i < n; ++i) {
        bool keep = (i >= keepA && i <= keepB);
        if (!keep && g_app.thumbs[i]) {
            g_app.thumbs[i]->Release();
            g_app.thumbs[i] = nullptr;
            g_app.thumbTried[i] = 0;
        }
    }
    for (int i = keepA; i <= keepB; ++i) {
        if (i < 0 || i >= n) continue;
        if (!g_app.thumbs[i] && !g_app.thumbTried[i]) {
            g_app.thumbTried[i] = 1;
            g_app.thumbs[i] = g_app.gfx.loadImage(g_app.themes[i].dayImage, 420);
        }
    }
}

// 外观 0=跟随系统 1=浅色 2=深色
static void RefreshPalette() {
    bool dark = g_app.cfg.appearance == 2
                    ? true
                    : (g_app.cfg.appearance == 1 ? false : SystemUsesDarkMode() != 0);
    if (g_app.bgBrush) { g_app.bgBrush->Release(); g_app.bgBrush = nullptr; }
    g_app.pal = MakePalette(dark, SystemAccentColor());
    SetDarkModeAttribute(g_app.hwnd, dark);
}

// ---------------------------------------------------------------------------
//  动画
// ---------------------------------------------------------------------------
// 动画时间基准用 QPC（µs 级）。GetTickCount64 只有 ~15.6ms 粒度
//（timeBeginPeriod(1) 也提不高它），用它插值动画位置会隔 2~3 个刷新周期才更新
// 一次、步长不匀——120Hz 屏上看着一顿一顿。
static double qpcMs() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}
static int g_paintCount = 0;        // TW_ANIMLOG 调试钩子用：WM_PAINT 次数
static double g_lastFrameEnd = 0;   // TW_ANIMLOG 调试钩子用：上一帧结束的 QPC 时间

static void StartAnim() {
    // 开关/分段控件/窗口淡入淡出按实际经过时间插值，8ms 定时器足够。
    // 全屏切换动画不走这个定时器：StartZoom() 直接调用 RunZoomAnim() 一口气跑完、
    // 用 DwmFlush() 自控节奏（WM_TIMER 最小间隔 ~15.6ms 会砍到 64fps）。
    SetTimer(g_app.hwnd, TM_ANIM, 8, nullptr);
}

// WINDOWPLACEMENT 的 rcNormalPosition 用"工作区坐标"（原点 = 所在显示器 rcWork
// 左上角），GetWindowRect 拿的是屏幕坐标，两者差一个工作区原点——不换算还原会
// 整体偏移（如任务栏高度 76px）。
static RECT ScreenToWorkArea(const RECT& r) {
    RECT w = r;
    MONITORINFO mi = {sizeof(mi)};
    if (GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi)) {
        w.left -= mi.rcWork.left;  w.right -= mi.rcWork.left;
        w.top -= mi.rcWork.top;    w.bottom -= mi.rcWork.top;
    }
    return w;
}
static RECT WorkAreaToScreen(const RECT& w, HWND h) {
    RECT r = w;
    MONITORINFO mi = {sizeof(mi)};
    if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) {
        r.left += mi.rcWork.left;  r.right += mi.rcWork.left;
        r.top += mi.rcWork.top;    r.bottom += mi.rcWork.top;
    }
    return r;
}

// 全屏切换动画的收尾：真正切到最大化/还原状态（此刻窗口矩形已与目标一致，
// 不会跳）。SetWindowPlacement 只拿来改系统里的"还原矩形"记录，切状态一律用
// ShowWindow 单步完成——直接 SetWindowPlacement(目标状态) 会被系统做成两步
//（先按 rcNormalPosition 摆一次再切状态），收尾时窗口会"弹"回去又弹回来。
//
// 两个方向的顺序必须相反，关键在 rcNormalPosition 的语义：
//   · 窗口处于最大化态时它只是"还原位置"记录，写进去屏幕零变化；
//   · 窗口处于普通态时它就是窗口的活位置，写进去系统会真的把窗口挪过去。
// 所以：
//   · 去最大化：先 SW_MAXIMIZE（此刻窗口已被动画摆到最大化矩形，切状态不动
//     矩形，零视觉变化），变成最大化态后再补写 rcNormalPosition（纯数据修改）。
//     反过来的顺序会让窗口先闪回旧位置再被放大（实测复现过）。
//   · 还原：窗口是最大化态，先写 rcNormalPosition（纯数据修改），再 SW_RESTORE。
// DWMWA_TRANSITIONS_FORCEDISABLED 关掉本窗口的 DWM 过渡：否则 DWM 会替这个
// 窗口再播一遍系统自带的"最大化/还原"视觉动画（一层视觉变换，能看到收尾弹一下）。
#ifndef DWMWA_TRANSITIONS_FORCEDISABLED
#define DWMWA_TRANSITIONS_FORCEDISABLED 3
#endif

static void ApplyZoomState(bool toMax) {
    g_app.maximized = toMax;
    BOOL noTrans = TRUE;
    DwmSetWindowAttribute(g_app.hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &noTrans, sizeof(noTrans));
    WINDOWPLACEMENT wp;
    wp.length = sizeof(wp);
    if (toMax) {
        ShowWindow(g_app.hwnd, SW_MAXIMIZE);
        if (g_app.normRect.right > g_app.normRect.left && GetWindowPlacement(g_app.hwnd, &wp)) {
            wp.showCmd = SW_SHOWMAXIMIZED;               // = 当前状态 → 纯数据修改
            wp.flags = 0;
            wp.rcNormalPosition = ScreenToWorkArea(g_app.normRect);
            SetWindowPlacement(g_app.hwnd, &wp);
        }
    } else {
        if (GetWindowPlacement(g_app.hwnd, &wp)) {
            wp.showCmd = IsZoomed(g_app.hwnd) ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;  // = 当前状态
            wp.flags = 0;
            if (g_app.normRect.right > g_app.normRect.left)
                wp.rcNormalPosition = ScreenToWorkArea(g_app.normRect);
            SetWindowPlacement(g_app.hwnd, &wp);
        }
        ShowWindow(g_app.hwnd, SW_RESTORE);
        if (g_app.normRect.right > g_app.normRect.left) {
            // 收尾后立刻把追踪值钉回目标，避免放置过程的中间帧污染它
            g_app.zooming = false;
            TrackNormRect();
        }
    }
    noTrans = FALSE;
    DwmSetWindowAttribute(g_app.hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &noTrans, sizeof(noTrans));
    // 收尾定圆角：最大化 = 直角（圆角会在屏幕四角露出桌面），还原 = 圆角。
    // 矩形不变时系统也可能不发 WM_SIZE，必须在状态切完的这一刻显式定死。
    int corner = toMax ? 1 /*DONOTROUND*/ : 2 /*ROUND*/;
    DwmSetWindowAttribute(g_app.hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    g_app.zooming = false;
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

// 记住"普通状态"下的窗口矩形（还原动画用）。缩放动画进行中、最大化、最小化时
// 不记，否则会把动画中间帧或满屏矩形当成还原目标。
static void TrackNormRect() {
    if (g_app.zooming || g_app.maximized || g_app.minAnimating) return;
    if (IsZoomed(g_app.hwnd) || IsIconic(g_app.hwnd)) return;
    RECT r;
    if (GetWindowRect(g_app.hwnd, &r) && r.right > r.left && r.bottom > r.top)
        g_app.normRect = r;
}

// 全屏切换动画·播放：把窗口矩形从 zoomFrom 过渡到 zoomTo 一口气跑完。
// 每帧 = 改窗口矩形（→ WM_SIZE → 布局跟着变）→ 立刻重画 → DwmFlush() 等下一次
// 合成（节奏 = 屏幕刷新率，120Hz 屏上 ~8.3ms/帧）。
// 不用 WM_TIMER 驱动：最小间隔 ~10~15.6ms（≈64fps），120Hz 屏上正好砍掉一半帧；
// 直接由 StartZoom 调用，点击后 ~1ms 就开跑（定时器会让首帧迟到 15~31ms）。
// 进度用 QPC：见 qpcMs 的说明。
static void RunZoomAnim() {
    float dur = g_app.zoomMs > 40.0f ? g_app.zoomMs : 40.0f;
    g_app.gfx.presentSync = 0;      // 节奏交给 DwmFlush，Present 自己不再等垂直同步
    // 调试钩子：环境变量 TW_ANIMLOG=1 时把每帧耗时拆解（set/paint/flush）追加写
    // %TEMP%\tw_anim.log。看两项：相邻 frame 的 t 步长（应 ≈ 刷新间隔）、flush 值。
    static FILE* alog = nullptr;
    static bool alogInit = false;
    if (!alogInit) {
        alogInit = true;
        if (_wgetenv(L"TW_ANIMLOG")) {
            wchar_t tmp[MAX_PATH]; DWORD n = GetTempPathW(MAX_PATH, tmp);
            if (n && n < MAX_PATH - 20) wcscat_s(tmp, L"tw_anim.log");
            alog = _wfopen(tmp, L"a, ccs=UTF-8");
        }
    }
    int frameNo = 0;
    double tStartWall = qpcMs();
    for (;;) {
        double c0 = qpcMs();
        float t = (float)((c0 - g_app.zoomStartMs) / dur);
        if (t > 1.0f) t = 1.0f;
        if (t < 0.0f) t = 0.0f;
        float e = 1.0f - powf(1.0f - t, 3.0f);               // 三次缓出（先快后慢）
        RECT r;
        r.left   = g_app.zoomFrom.left   + (LONG)((g_app.zoomTo.left   - g_app.zoomFrom.left)   * e);
        r.top    = g_app.zoomFrom.top    + (LONG)((g_app.zoomTo.top    - g_app.zoomFrom.top)    * e);
        r.right  = g_app.zoomFrom.right  + (LONG)((g_app.zoomTo.right  - g_app.zoomFrom.right)  * e);
        r.bottom = g_app.zoomFrom.bottom + (LONG)((g_app.zoomTo.bottom - g_app.zoomFrom.bottom) * e);
        if (r.right - r.left < 1) r.right = r.left + 1;
        if (r.bottom - r.top < 1) r.bottom = r.top + 1;
        int paintBefore = g_paintCount;
        SetWindowPos(g_app.hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        double c1 = qpcMs();
        UpdateWindow(g_app.hwnd);       // 这一帧立刻画出来（Present 在 WM_PAINT 里）
        double c2 = qpcMs();
        DwmFlush();                     // 等下一次合成：帧率跟着刷新走
        double c3 = qpcMs();
        if (alog)
            fwprintf(alog, L"frame %d t=%.4f rect=%ld,%ld,%ld,%ld set=%.2f paint=%.2f(repaint=%d) flush=%.2f total=%.2f gap_us=%.0f\n",
                     frameNo, t, r.left, r.top, r.right, r.bottom,
                     (c1 - c0) * 1000.0, (c2 - c1) * 1000.0, g_paintCount - paintBefore,
                     (c3 - c2) * 1000.0, (c3 - c0) * 1000.0,
                     g_lastFrameEnd > 0 ? (c0 - g_lastFrameEnd) * 1000.0 : 0.0);
        g_lastFrameEnd = c3;
        ++frameNo;
        if (t >= 1.0f) {
            double z0 = qpcMs();
            ApplyZoomState(g_app.zoomToMax);
            if (alog) {
                fwprintf(alog, L"apply %.2f us_total=%.2f frames=%d\n",
                         (qpcMs() - z0) * 1000.0, (qpcMs() - tStartWall) * 1000.0, frameNo);
                fflush(alog);
            }
            break;
        }
    }
    g_app.gfx.presentSync = 1;
}

// 全屏切换动画·入口：窗口矩形从"现在"平滑过渡到 工作区 / 还原矩形。
// 时长 200ms、三次缓出，和 Win11 自带的最大化手感接近。每一帧都走
// SetWindowPos -> WM_SIZE -> ComputeLayout，内容跟着窗口连续变化。
static void StartZoom(bool toMax) {
    HWND h = g_app.hwnd;
    if (g_app.zooming) return;                       // 正在播就忽略新的请求
    RECT cur;
    GetWindowRect(h, &cur);
    RECT target;
    if (toMax) {
        MONITORINFO mi = {sizeof(mi)};
        if (!GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) return;
        target = mi.rcWork;                          // 最大化目标 = 工作区（与 WM_GETMINMAXINFO 一致）
    } else {
        // 还原目标用我们自己记的矩形（系统那份可能已被"动画到工作区再收尾"弄脏）
        if (g_app.normRect.right > g_app.normRect.left) {
            target = g_app.normRect;
        } else {
            WINDOWPLACEMENT wp;
            wp.length = sizeof(wp);
            if (!GetWindowPlacement(h, &wp)) return;
            target = WorkAreaToScreen(wp.rcNormalPosition, h);
            g_app.normRect = target;
        }
    }
    if (EqualRect(&cur, &target)) { ApplyZoomState(toMax); return; }
    if (toMax) g_app.normRect = cur;   // 关键：开始去全屏的这一刻，当前矩形就是还原落点
    g_app.zoomFrom = cur;
    g_app.zoomTo = target;
    g_app.zoomToMax = toMax;
    g_app.zooming = true;
    g_app.zoomStartMs = qpcMs();
    RunZoomAnim();                     // 立即开跑：不等 WM_TIMER
}

// ---------------------------------------------------------------------------
//  最小化 / 从任务栏还原的缩放动画（按系统原生动画的实测参数复刻）
//
//  系统动画对本窗口不生效：DWM 的"收进任务栏"缩放动画只对普通表面窗口播，
//  本窗口为毛玻璃带 WS_EX_NOREDIRECTIONBITMAP（创建期样式，运行期摘不掉），
//  实测各种组合都触发不了系统动画。按 120fps 录屏量出的参数复刻：
//    · 时长 ≈150ms；缓动 = ease-in（进度 ≈ t²，像被"吸"进任务栏）；还原方向反向
//    · 画面整体等比缩小，终点 ≈ 原窗口的 8%，落在本窗口任务栏按钮中心
//    · 不淡出（系统动画过程中对比度不变）
// ---------------------------------------------------------------------------
static POINT TaskbarButtonCenterPoint() {
    POINT pt = { GetSystemMetrics(SM_CXSCREEN) / 2, GetSystemMetrics(SM_CYSCREEN) - 2 };
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (tray) {
        RECT r; GetWindowRect(tray, &r);
        pt.x = (r.left + r.right) / 2;
        pt.y = r.top + (r.bottom - r.top) / 2;
    }
    return pt;
}

// 收进任务栏时的终态矩形：缩到 ~8%，中心压在任务栏按钮上
static RECT CollapsedMinRect(const RECT& from) {
    int w = from.right - from.left, h = from.bottom - from.top;
    int nw = w * 8 / 100, nh = h * 8 / 100;
    if (nw < 10) nw = 10;
    if (nh < 10) nh = 10;
    POINT c = TaskbarButtonCenterPoint();
    RECT r = { c.x - nw / 2, c.y - nh / 2, c.x - nw / 2 + nw, c.y - nh / 2 + nh };
    return r;
}

// rcNormalPosition 用工作区坐标（相对主屏工作区左上角），写 placement 前不换算
// 的话窗口整体会偏移一个任务栏高度。
static RECT ScreenToWorkspaceRect(const RECT& r) {
    MONITORINFO mi = { sizeof(mi) };
    RECT wa = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    if (GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi)) wa = mi.rcWork;
    RECT o = { r.left - wa.left, r.top - wa.top, r.right - wa.left, r.bottom - wa.top };
    return o;
}

// 最小化动画的一帧：按当前进度 p 摆好窗口矩形并立刻重画
static void ApplyMinRect(float p) {
    const RECT& a = g_app.minFrom;
    const RECT& b = g_app.minTo;
    g_app.minRect.left   = (LONG)lroundf(a.left   + (b.left   - a.left)   * p);
    g_app.minRect.top    = (LONG)lroundf(a.top    + (b.top    - a.top)    * p);
    g_app.minRect.right  = (LONG)lroundf(a.right  + (b.right  - a.right)  * p);
    g_app.minRect.bottom = (LONG)lroundf(a.bottom + (b.bottom - a.bottom) * p);
    SetWindowPos(g_app.hwnd, nullptr, g_app.minRect.left, g_app.minRect.top,
                 g_app.minRect.right - g_app.minRect.left, g_app.minRect.bottom - g_app.minRect.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    UpdateWindow(g_app.hwnd);     // 立刻画出来（别等 WM_PAINT 排队）
    DwmFlush();                   // 等下一次合成，帧率跟着刷新走
}

// 动画主循环：触发点直接跑完（QPC 计时；不等 WM_TIMER，否则首帧会迟到还跳段）
static void RunMinAnim() {
    const float durMs = g_app.minMs;
    LARGE_INTEGER f, t0;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    for (;;) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float t = (float)(double(now.QuadPart - t0.QuadPart) / (double(f.QuadPart) * durMs / 1000.0));
        if (t > 1.0f) t = 1.0f;
        // 收进任务栏 = ease-in；从任务栏展开 = 同曲线反向（先快后慢）
        float p = g_app.minToIcon ? (t * t) : (1.0f - (1.0f - t) * (1.0f - t));
        ApplyMinRect(p);
        if (t >= 1.0f) break;
    }
    g_app.minAnimating = false;
    if (g_app.minToIcon) {
        // 动画把窗口挪到了任务栏小矩形上，系统会把"还原矩形"记成它——要改回真实
        // 矩形。先 SW_MINIMIZE 再写记录，顺序不能反：收尾那一刻窗口还是普通态，
        // rcNormalPosition 就是活位置，先写会让系统真的把窗口搬回完整矩形（闪一帧）；
        // 最小化后窗口不可见，rcNormalPosition 只是记录，此时补写才是纯数据修改。
        ShowWindow(g_app.hwnd, SW_MINIMIZE);
        WINDOWPLACEMENT wp = { sizeof(wp) };
        if (GetWindowPlacement(g_app.hwnd, &wp)) {
            wp.rcNormalPosition = ScreenToWorkspaceRect(g_app.normRect);
            SetWindowPlacement(g_app.hwnd, &wp);
        }
    } else {
        SetForegroundWindow(g_app.hwnd);
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
        if (g_app.minWasMaximized) {
            ShowWindow(g_app.hwnd, SW_MAXIMIZE);   // 最小化前是最大化 → 补回状态
            // 补写还原矩形：展开动画前 SetWindowPlacement 把 rcNormalPosition 写成
            // 了任务栏小矩形，而 SW_MAXIMIZE 又会把当前矩形记成还原位置。此刻窗口
            // 已是最大化态，rcNormalPosition 只是记录，补写是纯数据修改。
            WINDOWPLACEMENT wp = { sizeof(wp) };
            if (GetWindowPlacement(g_app.hwnd, &wp)) {
                wp.showCmd = SW_SHOWMAXIMIZED;                 // = 当前状态 → 纯数据修改
                if (g_app.normRect.right > g_app.normRect.left)
                    wp.rcNormalPosition = ScreenToWorkspaceRect(g_app.normRect);
                SetWindowPlacement(g_app.hwnd, &wp);
            }
        }
    }
    g_app.minWasMaximized = false;
}

// 入口：minimize=true 收进任务栏；false 从任务栏展开回 normRect
static void StartMinAnim(bool minimize) {
    HWND h = g_app.hwnd;
    if (g_app.minAnimating || g_app.zooming) return;
    if (minimize && IsIconic(h)) return;
    if (!minimize && !IsIconic(h)) { ShowMainWindow(); return; }

    if (minimize) {
        RECT cur; GetWindowRect(h, &cur);
        g_app.minRefW = cur.right - cur.left;
        g_app.minFrom = cur;
        g_app.minToIcon = true;
        g_app.minTo = CollapsedMinRect(cur);
        g_app.minAnimating = true;      // 先置位：动画每帧改矩形，TrackNormRect 必须让开
        RunMinAnim();                   // 先把"收进去"演完，最后才真正 SW_MINIMIZE
    } else {
        // 从任务栏展开：先把"还原矩形"改成收起时的小矩形（窗口还在任务栏里，
        // 纯数据修改不闪），再 SW_RESTORE 让它出现在小矩形位置，然后反向演一遍。
        // 最大化时最小化过（WPF_RESTORETOMAXIMIZED）→ 展开目标 = 工作区，
        // 动画结束后再真正切回最大化状态。
        WINDOWPLACEMENT wp0 = { sizeof(wp0) };
        GetWindowPlacement(h, &wp0);
        bool wasMaximized = (wp0.flags & WPF_RESTORETOMAXIMIZED) != 0 || wp0.showCmd == SW_SHOWMAXIMIZED;
        RECT target = g_app.normRect;
        if (wasMaximized) {
            MONITORINFO mi = { sizeof(mi) };
            if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) target = mi.rcWork;
        }
        RECT small = CollapsedMinRect(target);
        // 先打动画标记：SetWindowPlacement/SW_RESTORE 会触发 WM_MOVE/WM_SIZE，
        // 那时 TrackNormRect 绝不能把 normRect 改写成任务栏小矩形
        g_app.minAnimating = true;
        WINDOWPLACEMENT wp = { sizeof(wp) };
        GetWindowPlacement(h, &wp);
        wp.rcNormalPosition = ScreenToWorkspaceRect(small);
        SetWindowPlacement(h, &wp);
        ShowWindow(h, SW_RESTORE);
        // normRect 只在普通态展开时才更新；最大化分支不能写——会把"最大化之前的
        // 普通矩形"覆盖成工作区，之后取消最大化就会"还原"成满屏（实测复现过）
        if (!wasMaximized) g_app.normRect = target;
        g_app.minRefW = target.right - target.left;
        g_app.minFrom = small;
        g_app.minTo = target;
        g_app.minWasMaximized = wasMaximized;
        g_app.minToIcon = false;
        SetWindowPos(h, nullptr, small.left, small.top, small.right - small.left, small.bottom - small.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        UpdateWindow(h);
        RunMinAnim();
    }
}

// 窗口动画：show=true 淡入（从下方轻推入），false 淡出后自动隐藏
static void StartWindowAnim(bool show) {
    g_app.winAnimTarget = show ? 1.0f : 0.0f;
    g_app.winAnimating = true;
    g_app.hideAfterAnim = !show;
    if (show) g_app.winAnim = 0.0f;
    StartAnim();
}

static void ShowMainWindow() {
    if (IsIconic(g_app.hwnd)) {
        StartMinAnim(false);              // 从任务栏里唤回来：反向播一遍缩放动画
        return;
    }
    ShowWindow(g_app.hwnd, SW_SHOW);
    SetForegroundWindow(g_app.hwnd);
    StartWindowAnim(true);
}

static void HideMainWindow() {
    // 第一次"关闭到托盘"时气泡提示一下，否则用户会以为程序退出了
    static bool tipped = false;
    if (!tipped && g_app.trayAdded) {
        tipped = true;
        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = g_app.hwnd;
        nid.uID = 1;
        nid.uFlags = NIF_INFO;
        wcscpy_s(nid.szInfoTitle, L"仍在后台运行");
        wcscpy_s(nid.szInfo, L"窗口已收进托盘，自动切换壁纸不会停。单击托盘图标可重新打开，右键托盘图标可退出。");
        nid.dwInfoFlags = NIIF_INFO;
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
    StartWindowAnim(false);
}

static void StepAnim() {
    bool busy = false;
    // 所有指数缓动的系数按实际经过的 dt 换算（帧率无关），时间用 QPC
    static double lastMs = 0;
    double nowMs = qpcMs();
    float dt = lastMs > 0 ? (float)(nowMs - lastMs) : 15.6f;
    lastMs = nowMs;
    if (dt < 2.0f) dt = 2.0f;
    if (dt > 60.0f) dt = 60.0f;
    auto ease = [&](float k0) { return 1.0f - powf(1.0f - k0, dt / 15.6f); };
    for (int i = 0; i < 2; ++i) {        // 开关滑块
        float d = g_app.toggleTarget[i] - g_app.toggleAnim[i];
        if (d > 0.001f || d < -0.001f) {
            g_app.toggleAnim[i] += d * ease(0.35f);
            busy = true;
        } else {
            g_app.toggleAnim[i] = g_app.toggleTarget[i];
        }
    }
    {   // 外观分段控件：指示条滑动（约 0.15~0.2 秒到位）+ 按下缩放
        float d = (float)g_app.segTarget - g_app.segAnim;
        if (d > 0.002f || d < -0.002f) {
            g_app.segAnim += d * ease(0.32f);
            busy = true;
        } else {
            g_app.segAnim = (float)g_app.segTarget;
        }
        float s = g_app.segPressed ? 0.93f : 1.0f;
        float ds = s - g_app.segScale;
        if (ds > 0.002f || ds < -0.002f) {
            g_app.segScale += ds * ease(0.45f);
            busy = true;
        } else {
            g_app.segScale = s;
        }
    }
    {   // 切换方式分段控件：与外观同款
        float d = (float)g_app.swTarget - g_app.swAnim;
        if (d > 0.002f || d < -0.002f) {
            g_app.swAnim += d * ease(0.32f);
            busy = true;
        } else {
            g_app.swAnim = (float)g_app.swTarget;
        }
        float s = g_app.swPressed ? 0.93f : 1.0f;
        float ds = s - g_app.swScale;
        if (ds > 0.002f || ds < -0.002f) {
            g_app.swScale += ds * ease(0.45f);
            busy = true;
        } else {
            g_app.swScale = s;
        }
    }
    if (g_app.winAnimating) {
        float d = g_app.winAnimTarget - g_app.winAnim;
        if (d > 0.004f || d < -0.004f) {
            g_app.winAnim += d * ease(0.30f);
            busy = true;
        } else {
            g_app.winAnim = g_app.winAnimTarget;
            g_app.winAnimating = false;
            if (g_app.hideAfterAnim) {
                g_app.hideAfterAnim = false;
                ShowWindow(g_app.hwnd, SW_HIDE);
            }
        }
    }
    {   // 整页滚动：滚轮只改 scrollTarget，这里缓动追上去（指数缓出 = 网页手感）
        float d = g_app.scrollTarget - g_app.scroll;
        if (d > 0.3f || d < -0.3f) {
            g_app.scroll += d * ease(0.28f);
            busy = true;
        } else {
            g_app.scroll = g_app.scrollTarget;
        }
    }
    {   // 滚动条：滚动时淡入、停手约 0.9 秒后淡出；淡出期间定时器得继续跑
        bool recent = (qpcMs() - g_app.lastScrollMs) < 900.0;
        float targetA = (g_app.L.maxScroll > 0.0f && (recent || g_app.sbDragging)) ? 1.0f : 0.0f;
        float da = targetA - g_app.sbAlpha;
        if (da > 0.004f || da < -0.004f) {
            g_app.sbAlpha += da * ease(0.22f);
            busy = true;
        } else {
            g_app.sbAlpha = targetA;
        }
        if (targetA > 0.5f && g_app.sbAlpha > 0.002f) busy = true;   // 等淡出走完再停
    }
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
    if (!busy && !g_app.winAnimating) KillTimer(g_app.hwnd, TM_ANIM);
}

// ---------------------------------------------------------------------------
//  选择主题 / 应用
// ---------------------------------------------------------------------------
static void SelectTheme(int idx) {
    if (idx < 0 || idx >= (int)g_app.themes.size()) return;
    g_app.selIdx = idx;
    g_app.cfg.theme = g_app.themes[idx].name;
    SaveConfig(g_app.cfg);
    ComputeLayout(g_app.L);
    EnsureCardVisible(g_app.selIdx);     // 卡片在第二行以后就平滑滚过去
    StartAnim();
    ApplyCurrent(true);
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

static void OpenThemesFolder() {
    std::wstring dir = ThemeDirs().front();
    CreateDirectoryW(dir.c_str(), nullptr);
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------------------
//  卸载
// ---------------------------------------------------------------------------
// 读安装程序写下的安装位置（卸载项的 InstallLocation）。必须在删除该键之前读。
static std::wstring RegisteredInstallDir() {
    wchar_t buf[MAX_PATH * 2] = {0};
    DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\DynamicWallpapers",
                     L"InstallLocation", RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS)
        return L"";
    return buf;
}

// 两个路径是否指向同一个目录（忽略大小写与末尾分隔符）
static bool SameDir(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return false;
    auto trim = [](std::wstring s) {
        while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
        return s;
    };
    return _wcsicmp(trim(a).c_str(), trim(b).c_str()) == 0;
}

// 能不能把 exe 所在目录整个删掉。只有两种情况算数：
//   ① 卸载项里的 InstallLocation 就是这里（安装程序装的，用户可能自选过目录）；
//   ② 没写卸载项时，只认默认安装位置 %LOCALAPPDATA%\Programs\Dynamic Wallpapers。
// 便携版常和别的文件放在同一个文件夹里（桌面、下载目录…），无条件删会把
// 用户整个文件夹删掉——拿不准就只删 exe 本身。
static bool ShouldRemoveInstallDir(const std::wstring& exeDir, const std::wstring& registeredDir) {
    if (exeDir.empty()) return false;
    if (SameDir(exeDir, registeredDir)) return true;
    wchar_t local[MAX_PATH * 2] = {0};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local))) return false;
    return SameDir(exeDir, JoinPath(JoinPath(local, L"Programs"), kAppDisplay));
}

static void DoUninstall() {
    const std::wstring regDir = RegisteredInstallDir();     // 下面会删掉卸载项，先读出来
    const bool removeDir = ShouldRemoveInstallDir(ExeDir(), regDir);

    // 卸载前先跟用户说清楚"到底会删什么"：便携版不会动程序所在的文件夹
    std::wstring msg = L"确定要卸载“Dynamic Wallpapers”吗？\n\n将删除：";
    msg += removeDir ? L"程序文件夹、" : L"程序文件（不删它所在的文件夹）、";
    msg += L"桌面与开始菜单快捷方式、开机启动项，以及配置与用户主题目录"
           L"（%APPDATA%\\Dynamic Wallpapers）。";
    if (MessageBoxW(g_app.hwnd, msg.c_str(), L"卸载确认", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;

    // 请正在运行的实例退出（否则程序文件被占用无法删除）。
    // 辅助窗口用了另一个类名，按主窗口类名查找不会误伤自己。
    for (int round = 0; round < 3; ++round) {
        bool any = false;
        HWND h = nullptr;
        while ((h = FindWindowExW(nullptr, h, kWindowClass, nullptr)) != nullptr) {
            PostMessageW(h, WM_APP + 3, 0, 0);
            any = true;
        }
        // 更名前的老进程（过渡期）：一并请它退出
        while ((h = FindWindowExW(nullptr, h, L"TimeWallMainWindow", nullptr)) != nullptr) {
            PostMessageW(h, WM_APP + 3, 0, 0);
            any = true;
        }
        if (!any) break;
        Sleep(700);
    }
    SetAutoStart(false);
    // 快捷方式（新名字 + 更名前的老名字，一起清）
    wchar_t appdata[MAX_PATH * 2] = {0};
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata);
    for (const wchar_t* nm : { kAppDisplay, kLegacyDisplay }) {
        std::wstring smDir = JoinPath(JoinPath(appdata,
            L"Microsoft\\Windows\\Start Menu\\Programs"), nm);
        DeleteFileW(JoinPath(smDir, std::wstring(nm) + L".lnk").c_str());
        RemoveDirectoryW(smDir.c_str());          // 顺带删掉空的开始菜单文件夹
    }
    wchar_t desktop[MAX_PATH * 2] = {0};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desktop))) {
        DeleteFileW(JoinPath(desktop, std::wstring(kAppDisplay) + L".lnk").c_str());
        DeleteFileW(JoinPath(desktop, std::wstring(kLegacyDisplay) + L".lnk").c_str());
    }
    // 注册表卸载项（新键 + 更名前的旧键）
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\DynamicWallpapers");
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\TimeWall");
    // 配置与用户主题目录（新目录 + 更名前的旧目录）
    std::vector<std::wstring> dirs = { AppDataDir() };
    if (appdata[0]) dirs.push_back(JoinPath(appdata, L"TimeWall"));
    for (const std::wstring& ad : dirs) {
        if (ad.empty()) continue;
        std::wstring cmd = L"cmd.exe /c rmdir /s /q \"" + ad + L"\"";
        STARTUPINFOW si = {sizeof(si)};
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = {};
        if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                           nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    }
    // 顺序很关键：先让用户确认"卸载完成"，再安排自删——否则这个提示框会让
    // 进程一直活着，exe 无法被删除
    MessageBoxW(nullptr, L"卸载完成。程序文件将在几秒后自动删除。", kAppDisplay,
                MB_OK | MB_ICONINFORMATION);

    std::wstring exeDir = ExeDir();
    std::wstring self = ExePath();
    // 只删 exe；只有"确认是安装目录"时才连目录一起删（见 ShouldRemoveInstallDir）
    std::wstring bat = L"cmd.exe /c ping 127.0.0.1 -n 3 > nul & del /f /q \"" + self + L"\"";
    if (removeDir) bat += L" & rmdir /s /q \"" + exeDir + L"\"";
    STARTUPINFOW si2 = {sizeof(si2)};
    si2.dwFlags = STARTF_USESHOWWINDOW;
    si2.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi2 = {};
    if (CreateProcessW(nullptr, &bat[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                       nullptr, &si2, &pi2)) {
        CloseHandle(pi2.hProcess);
        CloseHandle(pi2.hThread);
    }
}

// ---------------------------------------------------------------------------
//  窗口过程
// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    // Explorer 重启后要重新加回托盘图标
    if (g_taskbarCreatedMsg && msg == g_taskbarCreatedMsg) {
        g_app.trayAdded = false;
        AddTrayIcon();
        return 0;
    }
    switch (msg) {
    case WM_NCCALCSIZE:
        if (w) {
            // 客户区铺满整个窗口（去掉系统标题栏）。最大化时不要再让出边框：
            // 那 13px 会被 DWM 用亚克力材质刷一遍，露在窗口顶部/底部就是两条
            // "灰边"。窗口外框 = 工作区（见 WM_GETMINMAXINFO），没有多余边框。
            return 0;
        }
        break;

    case WM_NCHITTEST: {
        POINT pt = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        RECT wr;
        GetWindowRect(h, &wr);
        // 四边/四角 8 DIP 内是缩放区（Windows 分屏、Aero Snap 靠它）
        int bw = (int)(8 * g_app.gfx.scale());
        bool onL = pt.x < wr.left + bw, onR = pt.x >= wr.right - bw;
        bool onT = pt.y < wr.top + bw,  onB = pt.y >= wr.bottom - bw;
        if (!IsZoomed(h)) {
            if (onT && onL) return HTTOPLEFT;
            if (onT && onR) return HTTOPRIGHT;
            if (onB && onL) return HTBOTTOMLEFT;
            if (onB && onR) return HTBOTTOMRIGHT;
            if (onL) return HTLEFT;
            if (onR) return HTRIGHT;
            if (onT) return HTTOP;
            if (onB) return HTBOTTOM;
        }
        POINT c = pt;
        ScreenToClient(h, &c);
        float s = g_app.gfx.scale();
        float x = c.x / s, y = c.y / s;
        const Layout& L = g_app.L;
        auto in = [&](const D2D1_RECT_F& r) {
            return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
        };
        if (y < Ui::TitleBarH) {
            // 最大化按钮返回 HTMAXBUTTON，系统才会显示 Win11 的"贴靠布局"浮出
            if (in(L.captionMax)) return HTMAXBUTTON;
            // 最小化/关闭是自己画的，走客户区消息
            if (in(L.captionMin) || in(L.captionClose)) return HTCLIENT;
            return HTCAPTION;        // 其余标题栏：拖动、双击最大化、拖到边缘分屏
        }
        return HTCLIENT;
    }

    // 最大化按钮的悬停/按下高亮（HTMAXBUTTON 区域的鼠标消息是 NC 消息）
    case WM_NCMOUSEMOVE: {
        int id = (w == HTMAXBUTTON) ? CID_MAX : CID_NONE;
        if (id != g_app.hot) {
            g_app.hot = id;
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE | TME_NONCLIENT, h, 0};
            TrackMouseEvent(&tme);
            InvalidateRect(h, nullptr, FALSE);
        }
        break;                       // 交给 DefWindowProc 处理默认行为
    }

    case WM_NCMOUSELEAVE:
        if (g_app.hot == CID_MAX) {
            g_app.hot = CID_NONE;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;

    case WM_NCLBUTTONDOWN:
        if (w == HTMAXBUTTON) {
            // 点击动画自己插值窗口矩形过渡到全屏/还原（见 StartZoom）；
            // 返回 HTMAXBUTTON 只是为了拿到系统的"贴靠布局"浮出
            g_app.pressed = CID_MAX;
            InvalidateRect(h, nullptr, FALSE);
            StartZoom(!g_app.maximized);
            return 0;
        }
        break;
    case WM_NCLBUTTONUP:
        if (g_app.pressed == CID_MAX) {
            g_app.pressed = CID_NONE;
            InvalidateRect(h, nullptr, FALSE);
        }
        break;
    case WM_NCLBUTTONDBLCLK:
        if (w == HTCAPTION) {          // 双击标题栏 = 最大化/还原（同样带动画）
            StartZoom(!g_app.maximized);
            return 0;
        }
        break;

    case WM_GETMINMAXINFO: {
        // 必须先让系统把 ptMaxSize/ptMaxPosition 填好再改，否则最大化会忽略任务栏。
        // WS_POPUP 窗口系统默认"按整个显示器最大化"，要显式给工作区尺寸；
        // 不加边框余量（配合 WM_NCCALCSIZE 恒返回 0）：窗口外框 = 工作区，
        // 不会在顶部/底部留下被材质刷亮的"灰边"。
        LRESULT res = DefWindowProcW(h, msg, w, l);
        MINMAXINFO* mmi = (MINMAXINFO*)l;
        UINT dpi = GetDpiForWindow(h);
        HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {sizeof(mi)};
        if (GetMonitorInfoW(mon, &mi)) {
            mmi->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
            mmi->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
            mmi->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
            mmi->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
        }
        // 最小尺寸：宽至少放 3 张卡片，高至少放得下全部区块
        int minW = (int)((kPadX * 2 + 3 * (kCardW + 12) + 12) * dpi / 96.0f);
        int minH = (int)((kWinH + 8) * dpi / 96.0f);
        if (mmi->ptMinTrackSize.x < minW) mmi->ptMinTrackSize.x = minW;
        if (mmi->ptMinTrackSize.y < minH) mmi->ptMinTrackSize.y = minH;
        // 最小化动画要把窗口缩到 8%，这里必须把最小跟踪尺寸整个放开；
        // 写在"取 max"之后（写在前面会被原样盖回，窗口缩到一半就被夹住）
        if (g_app.minAnimating) {
            mmi->ptMinTrackSize.x = 1;
            mmi->ptMinTrackSize.y = 1;
        }
        return res;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        ++g_paintCount;                 // TW_ANIMLOG 调试钩子用
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        RenderFrame();
        EndPaint(h, &ps);
        return 0;
    }

    case WM_SIZE:
        // 最小化动画期间窗口尺寸每帧都在变小，但布局必须冻结在动画开始时的尺寸
        //（RenderFrame 里整体等比缩放）。另外最小化瞬间系统会送 WM_SIZE(SIZE_MINIMIZED)，
        // 客户区只有图标占位尺寸，拿它重排会毁掉布局——最小化尺寸一律不参与重排。
        if (w == SIZE_MINIMIZED || IsIconic(h) || g_app.minAnimating) return 0;
        if (g_app.gfx.dc()) {
            RECT rc;
            GetClientRect(h, &rc);
            UINT dpi = GetDpiForWindow(h);
            g_app.gfx.resize(rc.right - rc.left, rc.bottom - rc.top, dpi);
            // 客户区尺寸（DIP）——布局/命中测试全部基于它
            float sc = dpi / 96.0f;
            g_app.winWDip = (rc.right - rc.left) / sc;
            g_app.winHDip = (rc.bottom - rc.top) / sc;
            // 缩放动画进行中时以动画目标为准（否则还原过程中图标/圆角慢半拍）
            g_app.maximized = g_app.zooming ? g_app.zoomToMax : (IsZoomed(h) != 0);
            // 最大化时窗口贴着屏幕，不需要圆角；动画过程中仍用圆角，收尾那一帧再变方
            bool square = g_app.maximized && !g_app.zooming;
            int corner = square ? 1 /*DONOTROUND*/ : 2 /*ROUND*/;
            DwmSetWindowAttribute(h, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
            if (g_app.bgBrush) {          // 渐变端点跟着高度走，重建
                g_app.bgBrush->Release();
                g_app.bgBrush = nullptr;
            }
            ComputeLayout(g_app.L);       // 窗口一变就重排
            TrackNormRect();              // 非最大化状态下记住当前矩形，供还原动画使用
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;

    case WM_MOVE:
        TrackNormRect();                  // 只挪位置不改尺寸时也记一下
        break;

    case WM_DPICHANGED: {
        RECT* r = (RECT*)l;
        UINT dpi = HIWORD(w);
        // 保持同样的 DIP 尺寸（100% → 150% 时窗口物理尺寸跟着放大）
        int nw = (int)(g_app.winWDip * dpi / 96.0f + 0.5f);
        int nh = (int)(g_app.winHDip * dpi / 96.0f + 0.5f);
        SetWindowPos(h, nullptr, r->left, r->top, nw, nh, SWP_NOZORDER | SWP_NOACTIVATE);
        g_app.gfx.resize(nw, nh, dpi);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case WM_SETTINGCHANGE: {
        RefreshPalette();
        // 跟随系统：系统深浅色一改就立刻换壁纸（5 秒轮询只是兜底，这里更快）
        static int lastSysStage = -1;
        if (g_app.cfg.switchMode == 1 && g_app.cfg.autoSwitch) {
            int s = SystemUsesDarkMode() ? 1 : 0;
            if (s != lastSysStage) {
                lastSysStage = s;
                ApplyCurrent(true);
            }
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    // 滚轮滚内容（交给 StepAnim 缓动）：一格 = 96 DIP；
    // 触摸板/高精度滚轮给更小的增量，按比例换算是平滑的
    case WM_MOUSEWHEEL: {
        if (AnyDialogOpen()) return 0;              // 弹层打开时内容不滚
        const Layout& L = g_app.L;
        if (L.maxScroll <= 0.0f) return 0;          // 内容放得下，没得滚
        int delta = GET_WHEEL_DELTA_WPARAM(w);
        g_app.scrollTarget -= (float)delta / (float)WHEEL_DELTA * 96.0f;
        ClampScrollTarget();
        MarkScrolled();
        StartAnim();
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case WM_MOUSEMOVE: {
        float s = g_app.gfx.scale();
        float my = GET_Y_LPARAM(l) / s;
        // 正在拖滚动条滑块：滑块跟着光标走（内容即时跟手，不做缓动）
        if (g_app.sbDragging) {
            const Layout& L = g_app.L;
            D2D1_RECT_F th = ScrollThumbRect();
            float travel = L.scrollTrackH - (th.bottom - th.top);
            float t = travel > 0.5f ? (my - g_app.sbDragGrab - L.scrollTrackTop) / travel : 0.0f;
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            g_app.scroll = g_app.scrollTarget = t * L.maxScroll;
            MarkScrolled();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        int id = HitTest(GET_X_LPARAM(l) / s, my);
        if (id != g_app.hot) {
            g_app.hot = id;
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_app.hot = CID_NONE;
        g_app.pressed = CID_NONE;
        InvalidateRect(h, nullptr, FALSE);
        return 0;

    case WM_LBUTTONDOWN: {
        float s = g_app.gfx.scale();
        g_app.pressed = HitTest(GET_X_LPARAM(l) / s, GET_Y_LPARAM(l) / s);
        // 滚动条：按住滑块 = 开始拖动；点轨道 = 翻一页（滚动半个视口多点）
        if (g_app.pressed == CID_SCROLLBAR) {
            D2D1_RECT_F th = ScrollThumbRect();
            g_app.sbDragging = true;
            g_app.sbDragGrab = GET_Y_LPARAM(l) / s - th.top;
            MarkScrolled();
        } else if (g_app.pressed == CID_SCROLLTRACK) {
            float my = GET_Y_LPARAM(l) / s;
            D2D1_RECT_F th = ScrollThumbRect();
            float page = (g_app.winHDip - Ui::TitleBarH) * 0.9f;
            if (my < th.top) g_app.scrollTarget -= page;
            else if (my > th.bottom) g_app.scrollTarget += page;
            ClampScrollTarget();
            MarkScrolled();
            StartAnim();
        }
        // 按在外观分段控件上：指示条先缩一下，松手弹回
        if (g_app.pressed >= CID_APPEARANCE0 && g_app.pressed <= CID_APPEARANCE2) {
            g_app.segPressed = true;
            StartAnim();
        }
        if (g_app.pressed >= CID_SWITCH0 && g_app.pressed <= CID_SWITCH1) {
            g_app.swPressed = true;
            StartAnim();
        }
        SetCapture(h);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONUP: {
        float s = g_app.gfx.scale();
        int id = HitTest(GET_X_LPARAM(l) / s, GET_Y_LPARAM(l) / s);
        ReleaseCapture();
        int pressed = g_app.pressed;
        g_app.pressed = CID_NONE;
        if (g_app.sbDragging) {              // 拖动滚动条结束
            g_app.sbDragging = false;
            MarkScrolled();
        }
        if (g_app.segPressed) {              // 松手：按下缩放弹回
            g_app.segPressed = false;
            StartAnim();
        }
        if (g_app.swPressed) {
            g_app.swPressed = false;
            StartAnim();
        }
        if (id != pressed || id == CID_NONE) { InvalidateRect(h, nullptr, FALSE); return 0; }

        // 主题卡片 ID = 基址 + 主题下标
        if (id >= CID_THEME0 && id < CID_THEME0 + kThemeSlots) {
            SelectTheme(id - CID_THEME0);
        } else if (id == CID_DAY_MINUS || id == CID_DAY_PLUS) {
            g_app.cfg.dayStart = (g_app.cfg.dayStart + (id == CID_DAY_PLUS ? 30 : -30) + 1440) % 1440;
            SaveConfig(g_app.cfg);
            if (g_app.cfg.autoSwitch) ApplyCurrent(true);
        } else if (id == CID_NIGHT_MINUS || id == CID_NIGHT_PLUS) {
            g_app.cfg.nightStart = (g_app.cfg.nightStart + (id == CID_NIGHT_PLUS ? 30 : -30) + 1440) % 1440;
            SaveConfig(g_app.cfg);
            if (g_app.cfg.autoSwitch) ApplyCurrent(true);
        } else if (id == CID_AUTO) {
            g_app.cfg.autoSwitch = !g_app.cfg.autoSwitch;
            g_app.toggleTarget[0] = g_app.cfg.autoSwitch ? 1.0f : 0.0f;
            SaveConfig(g_app.cfg);
            if (g_app.cfg.autoSwitch) ApplyCurrent(true);
            StartAnim();
        } else if (id == CID_BOOT) {
            g_app.cfg.startOnBoot = !g_app.cfg.startOnBoot;
            g_app.toggleTarget[1] = g_app.cfg.startOnBoot ? 1.0f : 0.0f;
            SetAutoStart(g_app.cfg.startOnBoot);
            SaveConfig(g_app.cfg);
            StartAnim();
        } else if (id >= CID_THEME_DEL0 && id < CID_THEME_DEL0 + kThemeSlots) {
            OpenDelTheme(id - CID_THEME_DEL0);
        } else if (id == CID_OPENTHEMES) {
            OpenThemesFolder();
        } else if (id == CID_DLG_CANCEL) {
            CloseDelTheme();
        } else if (id == CID_DLG_OK) {
            ConfirmDeleteTheme();
        } else if (id == CID_ADD_THEME) {
            OpenAddTheme();
        } else if (id >= CID_APPEARANCE0 && id <= CID_APPEARANCE2) {
            g_app.cfg.appearance = id - CID_APPEARANCE0;
            g_app.segTarget = g_app.cfg.appearance;
            SaveConfig(g_app.cfg);
            RefreshPalette();
            StartAnim();
        } else if (id >= CID_SWITCH0 && id <= CID_SWITCH1) {
            // 切换方式：立刻按新方式重算并应用壁纸
            g_app.cfg.switchMode = id - CID_SWITCH0;
            g_app.swTarget = g_app.cfg.switchMode;
            SaveConfig(g_app.cfg);
            if (g_app.cfg.autoSwitch) ApplyCurrent(true);
            StartAnim();
        } else if (id == CID_OV_CANCEL) {
            CloseAddTheme();
        } else if (id == CID_OV_OK) {
            ConfirmAddTheme();
        } else if (id == CID_OV_LIGHT_SLOT) {
            PickAddThemeImage(true);
        } else if (id == CID_OV_DARK_SLOT) {
            PickAddThemeImage(false);
        } else if (id == CID_OV_NAME) {
            // 点名称框：按点击位置摆光标（点击也取消选择）
            float px = GET_X_LPARAM(l) / g_app.gfx.scale() - (g_app.L.ovName.left + 10);
            EditSetCaretFromX(g_app.gfx, g_app.addUI.name, px);
        } else if (id == CID_REN_NAME) {
            float px = GET_X_LPARAM(l) / g_app.gfx.scale() - (g_app.L.renField.left + 10);
            EditSetCaretFromX(g_app.gfx, g_app.renUI.name, px);
        } else if (id == CID_REN_CANCEL) {
            CloseRenameTheme();
        } else if (id == CID_REN_OK) {
            ConfirmRenameTheme();
        } else if (id >= CID_THEME_REN0 && id < CID_THEME_REN0 + kThemeSlots) {
            OpenRenameTheme(id - CID_THEME_REN0);
        } else if (id == CID_MIN) {
            StartMinAnim(true);          // 收进任务栏（复刻系统原生动画）
        } else if (id == CID_CLOSE) {
            if (g_app.cfg.minimizeToTray) HideMainWindow();
            else DestroyWindow(h);
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case WM_TIMER:
        if (w == TM_ANIM) { StepAnim(); return 0; }
        if (w == TM_CARET) {                     // 输入框光标闪烁
            g_app.caretOn = !g_app.caretOn;
            if (!AnyDialogOpen()) KillTimer(h, TM_CARET);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (w == TM_TICK) {
            static int lastStage = -1;
            int stage = TargetStage();          // 按时段 / 跟随系统统一在这里比对
            if (g_app.cfg.autoSwitch && stage != lastStage) {
                lastStage = stage;
                ApplyCurrent(true);
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        break;

    case WM_APP + 1:      // 来自第二个实例：显示自己
        ShowMainWindow();
        InvalidateRect(h, nullptr, FALSE);
        return 0;

    case WM_APP + 4: {    // 来自第二个实例：--set-theme 接力——重读配置并立即切换
        g_app.cfg = LoadConfig();
        g_app.themes = ScanThemes();
        g_app.selIdx = 0;
        for (size_t i = 0; i < g_app.themes.size(); ++i)
            if (g_app.themes[i].name == g_app.cfg.theme) { g_app.selIdx = (int)i; break; }
        ClearThumbs();                    // 主题列表重建了：缩略图缓存全套作废
        ApplyCurrent(true);
        if (g_app.glass) EnsureGlassBitmap(true);
        ShowMainWindow();
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case WM_SYSCOMMAND: {
        const UINT sc = w & 0xFFF0;
        // 最小化：收进任务栏的缩放动画（任务栏右键菜单里的"最小化"也走这里）
        if (sc == SC_MINIMIZE && !g_app.zooming && !IsIconic(h)) {
            StartMinAnim(true);
            return 0;
        }
        // 从任务栏还原：反向播一遍
        if (sc == SC_RESTORE && IsIconic(h)) {
            StartMinAnim(false);
            return 0;
        }
        // 最大化 / 从最大化还原（任务栏右键菜单、系统菜单）走矩形过渡动画
        if (sc == SC_MAXIMIZE && !g_app.zooming) {
            StartZoom(true);
            return 0;
        }
        if (sc == SC_RESTORE && !g_app.zooming && !IsIconic(h)) {
            StartZoom(false);
            return 0;
        }
        if (sc == SC_RESTORE) {          // 兜底：让系统还原，然后补一个淡入
            LRESULT r = DefWindowProcW(h, msg, w, l);
            if (g_app.winAnim < 0.999f) StartWindowAnim(true);
            return r;
        }
        break;
    }

    case WM_KEYDOWN: {
        // 弹层里有输入框时，编辑键自己处理（光标/退格/粘贴），其它按键也吃掉——
        // 免得系统"叮"一声，或者把 Esc 之类交给默认处理
        TextEdit* ed = ActiveEdit();
        if (ed) {
            if (w == VK_ESCAPE) {
                if (g_app.addUI.active) CloseAddTheme();
                else if (g_app.renUI.active) CloseRenameTheme();
                else if (g_app.delUI.active) CloseDelTheme();
                return 0;
            }
            if (w == VK_RETURN) {                 // 回车 = 直接确认
                if (g_app.addUI.active) ConfirmAddTheme();
                else if (g_app.renUI.active) ConfirmRenameTheme();
                return 0;
            }
            if ((GetKeyState(VK_CONTROL) & 0x8000) && (w == 'A')) {   // Ctrl+A 全选
                EditSelectAll(*ed);
                g_app.caretOn = true;
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if ((GetKeyState(VK_CONTROL) & 0x8000) && (w == 'V')) {   // Ctrl+V 粘贴
                EditPaste(*ed);
                g_app.caretOn = true;
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (w == VK_LEFT || w == VK_RIGHT || w == VK_HOME || w == VK_END || w == VK_BACK ||
                w == VK_DELETE) {
                EditKeyDown(*ed, w, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
                g_app.caretOn = true;
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            return 0;
        }
        if (w == VK_ESCAPE && AnyDialogOpen()) {  // 没有输入框的弹层：Esc 关闭
            if (g_app.addUI.active) CloseAddTheme();
            if (g_app.delUI.active) CloseDelTheme();
            if (g_app.renUI.active) CloseRenameTheme();
            return 0;
        }
        break;
    }

    case WM_CHAR: {
        // 直接敲的字符（中文走 WM_IME_*，这里只管普通字符）
        TextEdit* ed = ActiveEdit();
        if (ed) {
            wchar_t c = (wchar_t)w;
            if (c >= 32 && c != 127) {
                EditInsert(*ed, std::wstring(1, c));
                g_app.caretOn = true;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        break;
    }

    // 中文输入法：开始组合 / 组合变化 / 结束组合。
    // 预编辑串自己画，确定后的结果串进输入框。
    case WM_IME_STARTCOMPOSITION:
        if (ActiveEdit()) {
            ImeFollowEdit();
            return 0;
        }
        break;
    case WM_IME_COMPOSITION:
        if (ActiveEdit()) {
            ImeHandleComposition(l);
            return 0;
        }
        break;
    case WM_IME_ENDCOMPOSITION:
        if (TextEdit* ed = ActiveEdit()) {
            ed->preedit.clear();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        break;

    case WM_APP + 2:      // 托盘图标消息
        // 左键单击 = 显示/隐藏主界面（标准托盘习惯）；右键 = 菜单
        if (l == WM_LBUTTONUP) {
            if (IsWindowVisible(h) && !IsIconic(h)) HideMainWindow();
            else ShowMainWindow();
        } else if (l == WM_RBUTTONUP || l == WM_CONTEXTMENU) {
            TrayMenu();
        }
        return 0;

    case WM_APP + 3:      // 安装程序/卸载流程请求退出
        DestroyWindow(h);
        return 0;

    case WM_POWERBROADCAST:
        if (w == PBT_APMRESUMEAUTOMATIC || w == PBT_APMRESUMESUSPEND) ApplyCurrent(true);
        return TRUE;

    case WM_CLOSE:
        if (g_app.cfg.minimizeToTray) { HideMainWindow(); return 0; }
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        RemoveTrayIcon();
        if (g_app.trayIcon) { DestroyIcon(g_app.trayIcon); g_app.trayIcon = nullptr; }
        KillTimer(h, TM_TICK);
        if (g_app.uiLayer) { g_app.uiLayer->Release(); g_app.uiLayer = nullptr; }
        SetAutoStart(g_app.cfg.startOnBoot);
        SaveConfig(g_app.cfg);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

// ---------------------------------------------------------------------------
//  入口
// ---------------------------------------------------------------------------
int wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    // DPI 感知：Per-Monitor V2（保证 200% 缩放下的清晰度）
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetProcessShutdownParameters(0x3FF, SHUTDOWN_NORETRY);
    timeBeginPeriod(1);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // ---- 命令行 ----
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool wantApplyOnly = false, wantShow = false, wantUninstall = false, opaque = false,
         selfGlass = false, wantHidden = false, forceSolid = false;
    int hoverCard = -1;
    std::wstring setTheme, addThemeFolder, addThemeUiFolder, delThemeName;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--apply") wantApplyOnly = true;
        else if (a == L"--show") wantShow = true;
        // 开机自启的注册表 Run 项里带 --autostart：开机只进托盘静默运行，不弹主界面
        else if (a == L"--autostart" || a == L"--background" || a == L"--tray") wantHidden = true;
        else if (a == L"--uninstall") wantUninstall = true;
        else if (a.rfind(L"--simulate-hour=", 0) == 0)
            g_app.simulateHour = _wtoi(a.substr(16).c_str());
        else if (a.rfind(L"--set-theme=", 0) == 0)
            setTheme = a.substr(12);
        else if (a.rfind(L"--add-theme-ui=", 0) == 0)
            addThemeUiFolder = a.substr(15);      // 测试/演示：直接打开弹层并预填
        else if (a.rfind(L"--add-theme=", 0) == 0)
            addThemeFolder = a.substr(12);        // 脚本用：直接导入主题
        else if (a.rfind(L"--delete-theme=", 0) == 0)
            delThemeName = a.substr(15);          // 脚本用：直接删除主题（移到回收站）
        else if (a.rfind(L"--hover-card=", 0) == 0)
            hoverCard = _wtoi(a.substr(13).c_str());   // 测试：模拟鼠标停在第 N 张卡片上
        else if (a.rfind(L"--zoom-ms=", 0) == 0)
            g_app.zoomMs = (float)_wtoi(a.substr(10).c_str());  // 测试：全屏动画时长
        else if (a.rfind(L"--min-ms=", 0) == 0)
            g_app.minMs = (float)_wtoi(a.substr(9).c_str());    // 测试：最小化动画时长
        else if (a == L"--opaque") opaque = true;    // 关毛玻璃，用不透明底（排错用）
        else if (a == L"--self-glass" || a == L"--no-acrylic")
            selfGlass = true;                          // 强制自绘磨砂（不用系统材质）
        else if (a == L"--acrylic")
            selfGlass = false;                         // 显式指定系统亚克力（默认）
        else if (a == L"--solid")
            forceSolid = true;                         // 强制纯色底，不模糊（排错/测试用）
    }
    if (argv) LocalFree(argv);

    // ---- 单实例 ----
    if (!SingleInstanceAcquire() && !wantApplyOnly && !wantUninstall) {
        // 一次性的命令行动作（导入/删除主题）没法在旧实例里生效：
        // 返回码 4 让脚本知道"需要先关掉正在运行的程序"
        if (!addThemeFolder.empty() || !delThemeName.empty()) return 4;
        // --set-theme=<名字>：写进配置，再让正在运行的实例重读配置并立即切换
        if (!setTheme.empty()) {
            Config c = LoadConfig();
            c.theme = setTheme;
            SaveConfig(c);
            NotifyExistingInstance(WM_APP + 4);
            return 0;
        }
        NotifyExistingInstance();
        return 0;
    }

    if (wantUninstall) {
        // 卸载流程用一个独立的辅助窗口类（避免与主窗口同名导致 FindWindow 混淆）
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = hInst;
        wc.lpszClassName = L"DynamicWallpapersHelperWindow";
        RegisterClassExW(&wc);
        g_app.hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr,
                                     nullptr, hInst, nullptr);
        DoUninstall();
        CoUninitialize();
        return 0;
    }

    // ---- 配置与主题 ----
    g_app.cfg = LoadConfig();
    // 开机自启以注册表为准（安装程序可能已写入自启项，配置里还是旧值）
    {
        bool regAuto = AutoStartEnabled();
        if (regAuto != g_app.cfg.startOnBoot) {
            g_app.cfg.startOnBoot = regAuto;
            SaveConfig(g_app.cfg);
        }
        // 顺手把自启命令行迁移成带 --autostart 的新格式（老版本写的是不带参数的）
        if (g_app.cfg.startOnBoot && !AutoStartHasBackgroundFlag()) SetAutoStart(true);
        g_app.toggleTarget[1] = g_app.toggleAnim[1] = regAuto ? 1.0f : 0.0f;
    }
    if (!setTheme.empty()) {
        g_app.cfg.theme = setTheme;
        SaveConfig(g_app.cfg);        // 命令行指定主题时立即落盘
    }
    g_app.themes = ScanThemes();
    if (!g_app.cfg.theme.empty()) {
        for (size_t i = 0; i < g_app.themes.size(); ++i)
            if (g_app.themes[i].name == g_app.cfg.theme) { g_app.selIdx = (int)i; break; }
    }
    if (!g_app.themes.empty() && g_app.themes[g_app.selIdx].name != g_app.cfg.theme) {
        // 配置里的主题已经不存在了（被删掉/改名）→ 落到第一个可用主题并写回配置
        g_app.cfg.theme = g_app.themes[g_app.selIdx].name;
        SaveConfig(g_app.cfg);
    }
    if (!g_app.themes.empty() && g_app.cfg.theme.empty()) {
        g_app.cfg.theme = g_app.themes[0].name;
        SaveConfig(g_app.cfg);
    }

    // --add-theme=<文件夹>：把该文件夹里的图片直接导入为用户主题（脚本/测试用）
    if (!addThemeFolder.empty()) {
        std::wstring name = addThemeFolder;
        size_t sl = name.find_last_of(L"\\/");
        if (sl != std::wstring::npos) name = name.substr(sl + 1);
        while (!name.empty() && (name.back() == L'\\' || name.back() == L'/')) name.pop_back();
        std::vector<std::wstring> imgs = ListImagesIn(addThemeFolder);
        if (!imgs.empty()) {
            std::wstring dayImg, nightImg;
            DiscoverThemeImages(addThemeFolder, name, dayImg, nightImg);
            if (dayImg.empty()) dayImg = imgs.front();
            if (nightImg.empty()) nightImg = imgs.size() > 1 ? imgs.back() : imgs.front();
            std::wstring outName, err;
            // 只允许两张不同的图片（浅色、深色各一张）
            if (dayImg == nightImg) {
                g_app.statusMsg = L"未导入：需要至少两张不同的图片（浅色、深色各一张）";
                g_app.statusMsgUntil = GetTickCount64() + 8000;
            } else if (ImportTheme(name, dayImg, nightImg, outName, &err)) {
                g_app.cfg.theme = outName;
                SaveConfig(g_app.cfg);
                g_app.themes = ScanThemes();
                for (size_t i = 0; i < g_app.themes.size(); ++i)
                    if (g_app.themes[i].name == outName) g_app.selIdx = (int)i;
                g_app.statusMsg = L"已添加主题：" + outName;
                g_app.statusMsgUntil = GetTickCount64() + 8000;
            } else {
                g_app.statusMsg = L"未导入" + (err.empty() ? std::wstring(L"") : (L"：" + err));
                g_app.statusMsgUntil = GetTickCount64() + 8000;
            }
        }
    }
    if (wantApplyOnly) {                       // 脚本模式：应用一次就退出
        ApplyCurrent(true);
        CoUninitialize();
        return 0;
    }

    // --delete-theme=<名字>：脚本用，直接把主题移到回收站后退出
    if (!delThemeName.empty()) {
        const ThemeInfo* t = FindTheme(g_app.themes, delThemeName);
        int rc = 2;                            // 2 = 没找到这个主题
        if (t) {
            std::wstring err;
            rc = DeleteThemeFolder(t->folder, err) ? 0 : 3;
        }
        CoUninitialize();
        return rc;
    }

    // ---- 窗口 ----
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // 图标按系统尺寸取（随 DPI），避免大图标被放大后发虚
    int bigIcon = GetSystemMetrics(SM_CXICON), smallIcon = GetSystemMetrics(SM_CXSMICON);
    if (bigIcon <= 0) bigIcon = 32;
    if (smallIcon <= 0) smallIcon = 16;
    wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, bigIcon, bigIcon, LR_DEFAULTCOLOR);
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, smallIcon, smallIcon, LR_DEFAULTCOLOR);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    UINT dpi = GetDpiForSystem();
    int w = (int)(kWinW * dpi / 96.0f + 0.5f);
    int h = (int)(kWinH * dpi / 96.0f + 0.5f);
    int sx = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    g_app.hwnd = CreateWindowExW(WS_EX_APPWINDOW
                                     | WS_EX_NOREDIRECTIONBITMAP,  // 毛玻璃的关键
                                 kWindowClass, kAppDisplay,
                                 WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
                                 sx, sy, w, h, nullptr, nullptr, hInst, nullptr);
    if (!g_app.hwnd) return 1;

    // Win11 圆角 + 阴影；深色标题栏跟随系统
    int corner = 2;   // DWMWCP_ROUND
    DwmSetWindowAttribute(g_app.hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    int backdrop = 2; // DWMSBT_MAINWINDOW（Mica；本程序自绘底色，此属性保证观感一致）
    DwmSetWindowAttribute(g_app.hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));

    RefreshPalette();
    if (!g_app.gfx.init(g_app.hwnd)) {
        MessageBoxW(nullptr, L"图形初始化失败（需要 DirectX 11 / Windows 10 及以上）",
                    kAppDisplay, MB_OK | MB_ICONERROR);
        return 2;
    }
    g_app.gfx.resize(w, h, dpi);
    {
        float sc = dpi / 96.0f;
        g_app.winWDip = w / sc;
        g_app.winHDip = h / sc;
    }
    // 窗口底色按系统能力选，前提都是窗口带 WS_EX_NOREDIRECTIONBITMAP（否则窗口
    // 自带的重定向表面是一块不透明黑底，透不出系统材质）：
    //   Win11 22H2+：DWMWA_SYSTEMBACKDROP_TYPE，DWM 实时模糊窗口背后的所有内容。
    //   Win10：纯色底（跟随深浅色）。
    //   --self-glass 强制自绘磨砂；--solid 强制纯色底（排错/测试用）；--opaque 不透明渐变。
    if (!opaque) {
        bool backdrop = false;
        if (!selfGlass && !forceSolid && IsWin11_22H2OrLater()) {
            int backdropAcrylic = 3;            // DWMSBT_TRANSIENTWINDOW（亚克力）
            backdrop = SUCCEEDED(DwmSetWindowAttribute(g_app.hwnd, DWMWA_SYSTEMBACKDROP_TYPE,
                                                       &backdropAcrylic, sizeof(backdropAcrylic)));
        }
        if (backdrop) {
            g_app.liveGlass = true;
            g_app.glass = false;
            g_app.statusMsg = L"毛玻璃：系统亚克力（实时模糊）";
        } else if (selfGlass && !forceSolid) {
            g_app.glass = true;
            g_app.liveGlass = false;
            EnsureGlassBitmap(true);
            g_app.statusMsg = L"毛玻璃：自绘磨砂（跟随壁纸）";
        } else {
            g_app.glass = false;
            g_app.liveGlass = false;
            g_app.solidBg = true;
            g_app.statusMsg = L"窗口底色：纯色（跟随深浅色）";
        }
        g_app.statusMsgUntil = GetTickCount64() + 5000;
    } else {
        g_app.glass = false;                    // --opaque：明确的纯色底
        g_app.liveGlass = false;
    }

    // 普通窗口不该常驻顶层：启动时清掉可能残留的置顶标志（外部工具会留下它）
    if (GetWindowLongW(g_app.hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST)
        SetWindowPos(g_app.hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    // 外观分段控件初始就停在配置选中的那段（不要开机时滑一下）
    g_app.segTarget = g_app.cfg.appearance;
    g_app.segAnim = (float)g_app.cfg.appearance;
    g_app.swTarget = g_app.cfg.switchMode;
    g_app.swAnim = (float)g_app.cfg.switchMode;
    ComputeLayout(g_app.L);
    EnsureCardVisible(g_app.selIdx, true);
    // 输入法：启动时先摘下 IME 上下文（没有输入框时不挂，免得随手按键弹候选窗）；
    // 打开带输入框的弹层时再挂上（PrepareTextInput），关掉时再摘（EndTextInput）
    g_app.himc = ImmAssociateContext(g_app.hwnd, nullptr);
    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    AddTrayIcon();

    // 开机自启（--autostart）时不弹主界面，只在托盘里静默跑：壁纸该切照切；
    // 用户点托盘图标/开始菜单快捷方式（带 --show）时才出现。--show 显式压过它。
    const bool showNow = !wantHidden || wantShow;
    ShowWindow(g_app.hwnd, showNow ? SW_SHOWNORMAL : SW_HIDE);
    UpdateWindow(g_app.hwnd);
    TrackNormRect();                                  // 记下初始矩形（还原动画的落点）
    SetTimer(g_app.hwnd, TM_TICK, 5000, nullptr);     // 5 秒一次：到点切壁纸 + 重采磨砂底

    // 首次出现也走一遍淡入动画（后台启动就不播了）
    g_app.winAnim = showNow ? 0.0f : 1.0f;
    if (showNow) StartWindowAnim(true);

    // --add-theme-ui=<文件夹>：测试/演示钩子——打开"添加主题"弹层，
    // 并把这个文件夹里的前两张图预填进浅色/深色方框（省去手动点文件对话框）
    if (!addThemeUiFolder.empty()) {
        std::vector<std::wstring> imgs = ListImagesIn(addThemeUiFolder);
        OpenAddTheme();
        if (!imgs.empty()) {
            ID2D1Bitmap* b = g_app.gfx.loadImage(imgs[0], 640);
            if (b) {
                g_app.addUI.lightBmp = b;
                g_app.addUI.lightPath = imgs[0];
            }
        }
        if (imgs.size() > 1) {
            ID2D1Bitmap* b = g_app.gfx.loadImage(imgs[1], 640);
            if (b) {
                g_app.addUI.darkBmp = b;
                g_app.addUI.darkPath = imgs[1];
            }
        }
        std::wstring nm = addThemeUiFolder;
        size_t sl = nm.find_last_of(L"\\/");
        if (sl != std::wstring::npos) nm = nm.substr(sl + 1);
        while (!nm.empty() && (nm.back() == L'\\' || nm.back() == L'/')) nm.pop_back();
        g_app.addUI.name.text = nm;
        g_app.addUI.name.caret = (int)nm.size();
        g_app.addUI.name.clearSel();
    }

    // --hover-card=N：第 N 张卡片置为悬停（可看到右上角的"×/✎"）；
    // 卡片在第二行以后时先滚到它那里（截图/验证第二三行卡片用）
    if (hoverCard >= 0 && hoverCard < (int)g_app.themes.size()) {
        g_app.hot = CID_THEME0 + hoverCard;
        EnsureCardVisible(hoverCard, true);
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
    }

    // 启动时对齐一次壁纸
    ApplyCurrent(true);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    RemoveTrayIcon();
    g_app.gfx.shutdown();
    ClearThumbs();
    timeEndPeriod(1);
    CoUninitialize();
    return 0;
}
