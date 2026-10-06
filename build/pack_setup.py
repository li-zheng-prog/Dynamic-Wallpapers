"""打包安装程序的内嵌资源。
把 payload 目录（主程序 + 主题壁纸）逐文件编号，生成：
  - payload.rc            : 每个文件一条 RCDATA（ID 从 1001 开始）
  - payload_manifest.txt  : 清单文本（同时作为 RCDATA ID 2000 嵌入，
                            运行时由安装程序读取，决定要不要安装某个主题）
清单格式（UTF-8，每行）: id|kind|相对路径|主题名|字节数

用法: python pack_setup.py <payload目录> <输出目录>
"""
import os
import sys


def collect(payload_dir):
    items = []
    nid = 1001
    # 主程序等顶层文件
    for name in sorted(os.listdir(payload_dir)):
        p = os.path.join(payload_dir, name)
        if os.path.isfile(p):
            items.append((nid, "app", name, "", os.path.getsize(p)))
            nid += 1
    # 主题（每个主题一个子目录）
    tdir = os.path.join(payload_dir, "themes")
    if os.path.isdir(tdir):
        for theme in sorted(os.listdir(tdir)):
            tp = os.path.join(tdir, theme)
            if not os.path.isdir(tp):
                continue
            for root, _dirs, files in os.walk(tp):
                for f in sorted(files):
                    full = os.path.join(root, f)
                    rel = os.path.relpath(full, payload_dir).replace("/", "\\")
                    items.append((nid, "theme", rel, theme, os.path.getsize(full)))
                    nid += 1
    return items


def main():
    payload_dir = os.path.abspath(sys.argv[1])
    out_dir = os.path.abspath(sys.argv[2])
    os.makedirs(out_dir, exist_ok=True)
    items = collect(payload_dir)
    if not items:
        print("!! payload 目录为空:", payload_dir)
        sys.exit(1)

    manifest_lines = [f"{i[0]}|{i[1]}|{i[2]}|{i[3]}|{i[4]}" for i in items]
    manifest_path = os.path.join(out_dir, "payload_manifest.txt")
    with open(manifest_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(manifest_lines) + "\n")

    rc = ['#include "resource.h"', "", "/* 由 build/pack_setup.py 自动生成，请勿手工修改 */"]
    for (nid, kind, rel, theme, size) in items:
        full = os.path.join(payload_dir, rel.replace("\\", "/")).replace("\\", "/")
        rc.append(f'{nid} RCDATA "{full}"')
    rc.append(f'2000 RCDATA "{manifest_path.replace(chr(92), "/")}"')
    rc_path = os.path.join(out_dir, "payload.rc")
    with open(rc_path, "w", encoding="utf-8", newline="\r\n") as f:
        f.write("\n".join(rc) + "\n")

    total = sum(i[4] for i in items)
    print(f"payload: {len(items)} 个文件, 共 {total/1024/1024:.1f} MB")
    for (nid, kind, rel, theme, size) in items[:3]:
        print(f"  [{nid}] {kind} {rel} ({size/1024/1024:.1f} MB)")
    print(f"  ... 共 {len(items)} 条 -> {rc_path}")


if __name__ == "__main__":
    main()
