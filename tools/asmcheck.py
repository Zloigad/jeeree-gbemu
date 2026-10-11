#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
asmcheck.py — проверка встроенного ассемблера скетча на совместимость с Thumb-1.

Зачем: ваш Cortex-M0+ понимает ТОЛЬКО Thumb-1. Именно на этом упала сборка v8.8:

    {standard input}:3223: Error: unshifted register required -- `orr r1,r1,#1'

Ни g++ на компьютере, ни проверка синтаксиса этого не видят: строка ассемблера для
них просто текст. Здесь мы разбираем все `__asm volatile ("...")` в скетче и
проверяем каждую инструкцию по правилам ARMv6-M (M0+):

  * `orr`, `and`, `eor`, `bic`, `mvn`, `tst` с НЕПОСРЕДСТВЕННЫМ числом — НЕ существуют
    (в Thumb-1 у них только форма «регистр с регистром») → ОШИБКА;
  * суффикс `.w` (широкие инструкции), `it`/`ite`, `udf` — только Thumb-2 → ОШИБКА;
  * `ldr rX, =выражение` — допустимо, но требует литерального пула; если это адрес
    ФУНКЦИИ, то перед `bx` нужен бит Thumb (на M0+ берётся от компилятора) → ПРЕДУПРЕЖДЕНИЕ.

Использование: python3 tools/asmcheck.py gbemu/gbemu.ino
"""
import re
import sys

# инструкции, которых в Thumb-1 нет в форме «с числом»
NO_IMM = {"orr", "and", "eor", "bic", "mvn", "tst", "mul"}
# инструкции/суффиксы, которых на M0+ нет вовсе
THUMB2_ONLY = {"it", "ite", "itt", "ittt", "itte", "itet", "udf", "cps", "mrs.w"}
ALLOWED_PREFIX = ("mov", "mrs", "msr", "add", "sub", "cmp", "cmn", "lsl", "lsr", "asr",
                  "and", "orr", "eor", "bic", "tst", "mvn", "mul", "ldr", "str", "ldrb",
                  "strb", "ldrh", "strh", "ldrsb", "ldrsh", "push", "pop", "bx", "bl",
                  "blx", "nop", "dsb", "dmb", "isb", "wfi", "wfe", "sev", "b", "beq",
                  "bne", "bgt", "blt", "bge", "ble", "bhi", "blo", "bhs", "bcs", "bcc",
                  "bkpt", "svc", "swi", "rev", "rev16", "revsh", "sxth", "sxtb", "uxth",
                  "uxtb", "adr", "cpsid", "cpsie", "vpush", "vpop", "vmov", "vldr", "vstr",
                  "vsadd", "vsld8", "vdup", "vmovl", "vrev", "vadd", "vsub", "vmul", "vneg",
                  "vabs", "vmax", "vmin", "vcmp", "vpush", "ldm", "stm", "ldmia", "stmia")


def strip_comment(line):
    return line.split("//")[0]


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "gbemu/gbemu.ino"
    src = open(path, encoding="utf-8", errors="replace").read()
    bad = warn = 0
    n_asm = 0
    for m in re.finditer(r'__asm\s+volatile\s*\(\s*"((?:[^"\\]|\\.)*)"', src):
        line_no = src[:m.start()].count("\n") + 1
        body = m.group(1)
        # раскрыть C-экранирование: строки ассемблера пишутся как "ins\n\tins"
        body = (body.replace("\\r", "\r").replace("\\n", "\n").replace("\\t", "\t")
                    .replace('\\"', '"').replace("\\\\", "\\"))
        for raw in body.split("\n"):
            ins = raw.strip().lstrip("\\t").strip()
            if not ins or ins.startswith("//") or ins.startswith("/*"):
                continue
            n_asm += 1
            parts = ins.split(None, 1)
            mn = parts[0].lower().rstrip(",")
            ops = parts[1] if len(parts) > 1 else ""
            base = mn.split(".")[0]
            if mn in THUMB2_ONLY:
                print(f"[ОШИБКА] {path}:{line_no}: «{ins}» — на Cortex-M0+ этого нет (это Thumb-2)")
                bad += 1; continue
            if "." in mn:
                suffix = mn.split(".", 1)[1]
                if suffix in ("w", "n") and base not in ("dsb", "dmb", "isb"):
                    print(f"[ОШИБКА] {path}:{line_no}: «{ins}» — широкая форма (.{suffix}) только в Thumb-2")
                    bad += 1; continue
            if base not in ALLOWED_PREFIX:
                print(f"[ПРЕДУПРЕЖДЕНИЕ] {path}:{line_no}: «{ins}» — незнакомая инструкция, проверьте вручную")
                warn += 1; continue
            if base in NO_IMM and re.search(r",\s*#", ops):
                print(f"[ОШИБКА] {path}:{line_no}: «{ins}» — в Thumb-1 у {base.upper()} НЕТ формы с числом "
                      f"(именно на этом упала сборка v8.8). Замените на обычный вызов (bl) или на ORR из регистра.")
                bad += 1; continue
            if re.match(r"^ldr\s+\w+\s*,\s*=", ins):
                print(f"[ПРЕДУПРЕЖДЕНИЕ] {path}:{line_no}: «{ins}» — литеральный пул: если это адрес "
                      f"функции, перед bx нужен бит Thumb (надёжнее обычный bl)")
                warn += 1
    print(f"ASMCHECK: проверено инструкций {n_asm}; ошибок {bad}, предупреждений {warn}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
