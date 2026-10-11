#!/usr/bin/env python3
"""
arduino_sim.py — повторить то, что делает Arduino IDE перед сборкой скетча:
вставить прототипы всех функций в начало файла (в конец ведущего блока
комментариев и директив #include/#define) и только потом компилировать.

Зачем: Arduino вставляет прототипы ПЕРЕД первой «настоящей» строкой файла.
Если первой такой строкой окажется определение функции, стоящее выше
#include (например выше #include "peanut_gb.h"), то прототипы уедут выше
подключения заголовка, и типы из него (enum gb_error_e и т.п.) будут
неизвестны — сборка падает с ошибкой, которую не видно при обычной проверке.

Использование:
    python3 tools/arduino_sim.py gbemu/gbemu.ino [доп. пути -I]
"""
import os, re, subprocess, sys, tempfile

def strip_and_find_insert(lines):
    """Возвращает (insert_index, defs) для «как это делает Arduino».
    insert_index — строка, перед которой вставляются прототипы
    (конец ведущего блока пустых строк/комментариев/#директив).
    defs — список (номер строки, текст прототипа) для всех функций."""
    in_block_comment = False
    insert_index = None
    defs = []
    for i, raw in enumerate(lines):
        line = raw.rstrip("\n")
        stripped = line.strip()

        if in_block_comment:
            if "*/" in stripped:
                in_block_comment = False
            continue
        if stripped.startswith("/*"):
            if "*/" not in stripped:
                in_block_comment = True
            continue
        if stripped == "" or stripped.startswith("//") or stripped.startswith("#"):
            continue

        # первая «настоящая» строка файла — здесь Arduino и вставит прототипы
        if insert_index is None:
            insert_index = i

        if line[:1] in (" ", "\t"):
            continue                                     # тело функций и т.п.
        m = re.match(r"^[A-Za-z_][\w:<>\*&,\s]*?\b(\w+)\s*\(([^;{]*)\)\s*\{", line)
        if not m:
            continue
        name, args = m.group(1), m.group(2)
        if name in ("if", "for", "while", "switch", "return", "else"):
            continue
        sig = line[:m.end()].rstrip()
        sig = sig[:sig.rindex("{")].rstrip()
        defs.append((i, "static ".join([]) + sig + ";"))
    return insert_index, defs

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    extra_inc = [a for a in sys.argv[2:]]
    sketch_dir = os.path.dirname(os.path.abspath(path)) or "."

    lines = open(path, encoding="utf-8").read().splitlines(keepends=True)
    idx, defs = strip_and_find_insert(lines)
    if idx is None:
        print("не нашёл места для вставки прототипов")
        return 2

    proto_block = ["// --- прототипы, сгенерированные Arduino (имитация) ---\n"]
    proto_block += [d[1] + "\n" for d in defs]
    proto_block.append("\n")
    out = lines[:idx] + proto_block + lines[idx:]

    tmp = tempfile.NamedTemporaryFile("w", suffix=".cpp", delete=False, encoding="utf-8")
    tmp.writelines(out)
    tmp.close()

    print(f"первая «настоящая» строка файла: {idx + 1} → прототипы вставлены туда")
    print(f"вставлено прототипов: {len(defs)}")
    first_def_line = min((d[0] for d in defs), default=None)
    if first_def_line is not None and first_def_line <= idx:
        print(f"ВНИМАНИЕ: первая функция файла (строка {first_def_line + 1}) выше "
              f"точки вставки — ровно так ломается сборка!")
    for d in defs:
        if "enum gb_error_e" in d[1] or "struct gb_s" in d[1]:
            print(f"   тип из ядра в прототипе (строка {d[0] + 1}): {d[1][:70]}")
            break

    inc = ["-I" + sketch_dir] + extra_inc
    # F_CPU=250 МГц — как в Arduino IDE (Tools -> CPU Speed -> 250 MHz): без него
    # сторож v9.2 падает #error, хотя сама IDE эту константу задаёт всегда.
    cmd = ["g++", "-fsyntax-only", "-std=gnu++17", "-Wall", "-DF_CPU=250000000L"] + inc + [tmp.name]
    r = subprocess.run(cmd, capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    sys.stderr.write(r.stderr)
    os.unlink(tmp.name)
    if r.returncode == 0:
        print(f"ARDUINO-СТИЛЬ OK: {path}")
    return r.returncode

if __name__ == "__main__":
    sys.exit(main())
