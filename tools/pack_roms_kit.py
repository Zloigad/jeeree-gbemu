#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Упаковать папку roms_kit/ в архив jeeree_roms_kit.zip (для релиза на GitHub).

Использование:
    python3 tools/pack_roms_kit.py [выходной_файл.zip]

Имена файлов внутри архива — в UTF-8 (кириллица не ломается при распаковке
в Windows 10/11, 7-Zip, WinRAR).
"""
import os
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "roms_kit")


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "jeeree_roms_kit.zip")
    files = []
    for dirpath, dirnames, filenames in os.walk(SRC):
        dirnames.sort()
        for fn in sorted(filenames):
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, SRC).replace(os.sep, "/")
            files.append((full, rel))
    if not files:
        sys.exit("roms_kit/ пуста — нечего упаковывать")
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for full, rel in files:
            z.write(full, rel)
    print("создан %s (%d файлов):" % (out, len(files)))
    for _, rel in files:
        print("  ", rel)


if __name__ == "__main__":
    main()
