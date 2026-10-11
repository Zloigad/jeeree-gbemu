#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
make_rom_header.py — превращает файл Game Boy-игры (.gb) в rom.h для скетча gbemu.

Использование (запускать в папке со скетчем gbemu):
    python make_rom_header.py game.gb
    python make_rom_header.py game.gb rom.h      # явно указать выходной файл

Требуется Python 3. Ничего ставить не нужно.

Что получится: файл rom.h с массивом ROM_DATA[] и размером ROM_SIZE.
Массив объявлен const, поэтому в RP2040 он лежит во flash (XIP) и НЕ занимает ОЗУ.

Ограничения: прошивка целиком должна влезть в 2 МБ флеш-памяти устройства
(скетч ~250-400 КиБ + игра). Файлы больше ~1.4 МБ не советую.
"""

import os
import sys


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else "rom.h"

    if not os.path.isfile(src):
        print(f"Файл не найден: {src}")
        return 1

    data = open(src, "rb").read()
    size = len(data)
    if size < 0x150:
        print(f"Внимание: файл слишком мал ({size} байт) — это точно образ игры (.gb)?")
    if size > 1400 * 1024:
        print(f"Внимание: файл {size / 1024 / 1024:.2f} МБ. Может не влезть во flash 2 МБ.")

    # название игры из заголовка (0x0134..0x0143, 16 байт, латиница, 0-терминатор)
    title = ""
    try:
        raw = data[0x0134:0x0144]
        keep = bytearray()
        for b in raw:
            if b == 0 or not (0x20 <= b <= 0x7E):      # только печатные символы
                break
            keep.append(b)
        title = keep.decode("ascii", errors="replace").strip()
    except Exception:
        pass

    # Скетч читает ROM по маске (без деления), поэтому массив должен быть
    # степенью двойки. У настоящих .gb размеры уже такие (32/64/128... КиБ) —
    # тогда padding = 0 байт. Иначе добиваем нулями, чтобы не читать за границей.
    if size & (size - 1):
        pad = 1
        while pad < size:
            pad <<= 1
        print(f"Внимание: размер не степень двойки — rom.h будет дополнен нулями до {pad} байт "
              f"(+{pad - size} байт, скетч читает ROM по маске).")
        data = data + bytes(pad - size)
        size = pad

    name = os.path.splitext(os.path.basename(src))[0]
    lines = []
    lines.append("/* rom.h — сгенерировано make_rom_header.py из \"%s\" */\n" % os.path.basename(src))
    lines.append("#ifndef ROM_H\n#define ROM_H\n\n#include <stdint.h>\n\n")
    lines.append('#define ROM_NAME "%s"\n' % (title if title else name))
    lines.append("static const unsigned int ROM_SIZE = %d;\n" % size)
    lines.append("static const uint32_t ROM_MASK = 0x%X;\n" % (size - 1))
    lines.append("static const uint8_t ROM_DATA[%d] = {\n" % size)

    chunk = 16
    for off in range(0, size, chunk):
        part = data[off:off + chunk]
        lines.append("  " + ",".join("0x%02X" % b for b in part) + ",\n")
    lines.append("};\n\n#endif /* ROM_H */\n")

    with open(dst, "w", encoding="utf-8", newline="\n") as f:
        f.writelines(lines)

    print(f"Готово: {dst}")
    print(f"  игра:   {title if title else name}")
    print(f"  размер: {size} байт ({size / 1024:.0f} КиБ)")
    print(f"  размер rom.h: {os.path.getsize(dst) / 1024 / 1024:.1f} МБ (это нормально — это текст)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
