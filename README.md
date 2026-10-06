# Dynamic Wallpapers

按时间或系统深浅色，自动在主题的浅色、深色两张壁纸之间切换的 Windows 桌面工具。
纯自绘的 WinUI3 风格界面。

## 功能

- **主题管理**：主题卡片网格（自动换行 + 滚动），添加主题只需浅色/深色两张图，
  支持重命名、删除（移到回收站，可恢复）；兼容 WinDynamicDesktop 的
  `theme.json` 格式
- **切换方式**：按时段（日间开始/夜间开始两个时间点），或跟随系统深浅色
  （读注册表，变更即时生效）
- **外观**：跟随系统 / 浅色 / 深色，强调色取自系统
- **壁纸填充**：居中/平铺/拉伸/适应/填充/跨屏，多显示器可用
- **窗口底色**：Win11 用系统亚克力实时模糊（SystemBackdrop）；Win10 用跟随深浅色的
  纯色底；`--self-glass` 可强制自绘磨砂（壁纸缩小再放大 + 色调纱 + 颗粒）
- **托盘常驻**：关闭窗口收进托盘继续切换；托盘菜单可暂停/改设置/退出
- **开机自启**：静默进托盘，不弹界面
- **单实例**：重复启动会把参数转交给已运行的实例
- **中文输入**：主题名输入框完整支持输入法

命令行参数、卸载流程等细节见 `src/`、`setup/` 源码。

## 下载安装

到 [Releases](../../releases) 页面下载：

| 文件 | 平台 | 说明 |
|---|---|---|
| `DynamicWallpapersSetup.exe` | Windows 10+ | 安装版（含示例主题，写入卸载项） |
| `DynamicWallpapers-portable.exe` | Windows 10+ | 便携版（旁边放 `themes/` 文件夹即可用） |

## 从源码构建

### Windows（MinGW-w64）

依赖：[MinGW-w64](https://winlibs.com/)（g++，放在 `D:\mingw64` 或改
`build/build.sh` 里的路径）、Python 3 + Pillow（生成图标）。

```bash
bash build/build.sh
# 产物：dist/DynamicWallpapers-portable.exe、dist/DynamicWallpapersSetup.exe
```

## 主题格式

一个主题 = `themes/` 下的一个文件夹，内含若干图片 + 可选的 `theme.json`
（WinDynamicDesktop 兼容，本程序只取日间、夜间两张）：

```json
{
  "imageFilename": "Big Sur Graphic_*.jpg",
  "displayName": "Big Sur Graphic",
  "dayImageList": [1],
  "nightImageList": [2]
}
```

没有 `theme.json` 时：文件名排第一张当浅色图、最后一张当深色图。
界面里点「添加主题」也可以直接挑两张图生成主题。

## 目录结构

```
├── src/        Windows 主程序（D2D 自绘 UI）
├── setup/      Windows 安装程序（payload 以 RCDATA 内嵌）
├── build/      构建脚本（编译 / 图标 / 打包 payload）
└── themes/     随安装包分发的示例主题
```

## 命令行

```
--show                 显示主界面
--apply                立即应用当前壁纸后退出
--set-theme=<名字>     切换到指定主题（已运行时转发给主实例）
--add-theme=<文件夹>   把文件夹里的图片导入为主题（导入后打开主界面）
--delete-theme=<名字>  把主题移到回收站后退出
--autostart            开机自启专用：只在托盘静默运行
--simulate-hour=<n>    模拟时刻（测试用）
--opaque               关闭毛玻璃（排错用）
--self-glass           强制自绘磨砂，不用系统材质（排错用）
--solid                强制纯色底，不模糊（排错用）
```

## 许可证

代码以 [MIT](LICENSE) 许可证发布。

注意：`themes/Big Sur Graphic/` 内的壁纸图片版权归 Apple 所有，仅作为示例
主题供个人使用与演示；分发安装包时请自行留意相关版权。

## 致谢

- [WinDynamicDesktop](https://github.com/t1m0thyj/WinDynamicDesktop) —
  主题格式与壁纸切换思路的参考
