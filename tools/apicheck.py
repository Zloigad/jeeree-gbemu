#!/usr/bin/env python3
"""
apicheck.py — быстрый статический контроль «граблей» arduino-pico, на которых
можно уронить плату в чёрный экран ещё до вывода картинки.

Проверяет вызовы SPI.setRX/setCS/setSCK/setTX: у SPI0 допустимы только
    RX {0,4,16,20}   CS {1,5,17,21}   SCK {2,6,18,22}   TX {3,7,19,23}
(SPI1: RX {8,12,24,28}, CS {9,13,25,29}, SCK {10,14,26,30}, TX {11,15,27,31})
иначе arduino-pico вызывает panic(): в Serial появляется
    FATAL: Attempting to set SPI0.CS to illegal pin N
и плата НАМЕРТВО встаёт (подсветка горит, экран чёрный). Ровно это случилось
в первой сборке v7 из-за SPI.setCS(16).

Использование:  python3 tools/apicheck.py gbemu/gbemu.ino
"""
import re, sys

VALID = {
    0: {"RX": {0, 4, 16, 20}, "CS": {1, 5, 17, 21}, "SCK": {2, 6, 18, 22}, "TX": {3, 7, 19, 23}},
    1: {"RX": {8, 12, 24, 28}, "CS": {9, 13, 25, 29}, "SCK": {10, 14, 26, 30}, "TX": {11, 15, 27, 31}},
}

def macro_map(src):
    """#define PIN_X 17  ->  {'PIN_X': 17}"""
    out = {}
    for m in re.finditer(r"^\s*#define\s+(\w+)\s+(\d+)\s*$", src, re.M):
        out[m.group(1)] = int(m.group(2))
    return out

def resolve(expr, macros):
    expr = expr.strip()
    if re.fullmatch(r"\d+", expr):
        return int(expr)
    if expr in macros:
        return macros[expr]
    return None

def strip_comments(src):
    out, i, n = [], 0, len(src)
    while i < n:
        if src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
        else:
            out.append(src[i]); i += 1
    return "".join(out)

def main(path):
    raw = open(path, encoding="utf-8").read()
    src = strip_comments(raw)
    macros = macro_map(src)
    problems = 0
    for m in re.finditer(r"\b(SPI1?|spi)\s*\.\s*set(RX|MISO|CS|SCK|TX|MOSI)\s*\(([^)]*)\)", src):
        bus_name, what, arg = m.group(1), m.group(2), m.group(3)
        bus = 1 if bus_name == "SPI1" else 0
        kind = {"MISO": "RX", "MOSI": "TX"}.get(what, what)
        line = src[:m.start()].count("\n") + 1
        if kind == "CS" and arg.strip() == "-1":
            print(f"[!] строка {line}: setCS(-1) — в arduino-pico это НЕ «выключить CS», "
                  f"а недопустимый пин (panic). Просто не вызывайте setCS().")
            problems += 1
            continue
        pin = resolve(arg, macros)
        if pin is None:
            print(f"[?] строка {line}: {bus_name}.set{what}({arg}) — пин не разобрать статически, "
                  f"проверьте руками: допустимые для SPI{bus} — {sorted(VALID[bus][kind])}")
            continue
        if pin not in VALID[bus][kind]:
            print(f"[ОШИБКА] строка {line}: SPI{bus}.set{what}({pin}) — НЕДОПУСТИМЫЙ пин. "
                  f"arduino-pico вызовет panic() и плата встанет. Допустимые: {sorted(VALID[bus][kind])}")
            problems += 1
        else:
            print(f"[ok] строка {line}: SPI{bus}.set{what}({pin}) — допустимо")
    if not re.search(r"\.set(CS|RX|SCK|TX|MISO|MOSI)\s*\(", src):
        print("[ok] вызовов SPI.set* нет — паниковать нечему")
    print("APICHECK: " + ("проблемы найдены" if problems else "чисто"))
    return 1 if problems else 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "gbemu/gbemu.ino"))
