#!/usr/bin/env bash
# ============================================================================
#  build.sh — 一键构建（MinGW-w64 g++）
#  产物：dist/DynamicWallpapers-portable.exe（便携版主程序，免安装直接用）
#        dist/DynamicWallpapersSetup.exe（安装程序，内嵌主程序 + 壁纸主题，约 24MB）
#  应用名 2026-09-29 起统一为 “Dynamic Wallpapers”（旧名 TimeWall / 壁纸随时间变化）
#  源码先复制到 D:\DynamicWallpapers_build 再编译：g++/windres 处理不了含中文的路径（乱码），
#  产物最后拷回 dist\。
#  用法：Git Bash 里执行  bash build/build.sh
#  依赖：D:\mingw64（MinGW-w64 g++ 16+）、python（含 Pillow）
# ============================================================================
set -e

GXX=/d/mingw64/bin/g++.exe
WINDRES=/d/mingw64/bin/windres.exe
ROOT="$(cd "$(dirname "$0")/.." && pwd)"       # 项目根目录（MSYS 形式，供 cp/cd 使用）
ROOTW="$(cd "$(dirname "$0")/.." && pwd -W)"   # 项目根目录（Windows 形式，供原生程序使用）
BUILD="D:/DynamicWallpapers_build"
DIST="$ROOT/dist"
# APPEXE 和 PORTABLE 别搞混：
#   APPEXE   = 编译产物名，也是"装进程序目录后的文件名"——安装程序 setup.cpp 里的 kAppExe
#              按这个名字建快捷方式 / 写卸载项，不能改成 -portable：装完的快捷方式会指向不存在的文件。
#   PORTABLE = 只给 dist/ 里的便携版用的名字，跟安装程序（DynamicWallpapersSetup.exe）区分开。
APPEXE="DynamicWallpapers.exe"
PORTABLE="DynamicWallpapers-portable.exe"
SETUPEXE="DynamicWallpapersSetup.exe"

CXXFLAGS="-std=c++17 -municode -O2 -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNTDDI_VERSION=0x0A000000 -Wall -Wno-unknown-pragmas"
LIBS="-lole32 -luuid -luser32 -lgdi32 -lshell32 -lshlwapi -ladvapi32 -ldwmapi -ld2d1 -ldwrite -ld3d11 -ldxgi -ldcomp -lwindowscodecs -lwinmm -limm32 -static -static-libgcc -static-libstdc++"

echo "==> 准备构建目录 $BUILD"
rm -rf "$BUILD"
mkdir -p "$BUILD/src" "$BUILD/setup" "$BUILD/payload/themes" "$DIST"

echo "==> 复制源码"
cp "$ROOT"/src/*.cpp "$ROOT"/src/*.h "$ROOT"/src/*.rc "$BUILD/src/"
cp "$ROOT"/setup/*.cpp "$ROOT"/setup/*.rc "$BUILD/setup/"

echo "==> 生成图标"
python "$ROOTW/build/make_icon.py" "$BUILD/src/app.ico"

echo "==> 编译主程序"
cd "$BUILD/src"
"$WINDRES" -I. --codepage=65001 -i app.rc -o app_res.o
"$GXX" $CXXFLAGS -mwindows -o "$APPEXE" \
    main.cpp gfx.cpp ui.cpp core.cpp app_res.o \
    -include windowsx.h $LIBS
cp "$APPEXE" "$DIST/$PORTABLE"
# 便携版：主题目录跟在 exe 旁边（dist/ 不进仓库，见 .gitignore）。
# 先删旧目录再拷：目标已存在时 cp -r 会把 themes 套成 dist/themes/themes
rm -rf "$DIST/themes"
cp -r "$ROOT/themes" "$DIST/themes"
echo "    便携版: $DIST/$PORTABLE"

# ---- 准备安装包 payload（主题 = 仓库根目录 themes/，随安装程序分发）----
echo "==> 收集安装包内容"
cp "$BUILD/src/$APPEXE" "$BUILD/payload/"
cp -r "$ROOT/themes/." "$BUILD/payload/themes/"

echo "==> 生成 payload 资源"
python "$ROOTW/build/pack_setup.py" "$BUILD/payload" "$BUILD/setup"

echo "==> 编译安装程序"
cd "$BUILD/setup"
"$WINDRES" -I"$BUILD/src" --codepage=65001 -i setup.rc -o setup_res.o
"$WINDRES" -I"$BUILD/src" --codepage=65001 -i payload.rc -o payload_res.o
"$GXX" $CXXFLAGS -mwindows -o "$SETUPEXE" \
    setup.cpp "$BUILD/src/gfx.cpp" "$BUILD/src/ui.cpp" \
    setup_res.o payload_res.o -I"$BUILD/src" -include windowsx.h $LIBS
cp "$SETUPEXE" "$DIST/$SETUPEXE"

echo
echo "==> 构建完成"
ls -la "$DIST/$PORTABLE" "$DIST/$SETUPEXE"
