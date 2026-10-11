#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
idecheck.py — проверка «собираемости в Arduino IDE», которую не ловит ни стенд, ни g++.

Зачем: Arduino IDE ПЕРЕД сборкой сама вставляет в начало скетча прототипы всех функций
(«forward declarations»), причём без учёта порядка объявлений в файле. Поэтому функция,
которая возвращает или принимает тип, объявленный в самом скетче, в .ino не собирается:

    gbemu.ino:1142:8: error: 'AdcSrc' does not name a type
      static AdcSrc adcSample(uint ch) {

Прототип вида `static AdcSrc adcSample(uint ch);` оказывается выше объявления `struct
AdcSrc`. На g++ (стенд) и в PlatformIO (там другой путь сборки) такой ошибки нет — вот
и вылезло только у вас в IDE.

Что проверяем: НИ ОДНА функция не возвращает и не принимает тип, объявленный в скетче
(struct/class/union/enum/typedef). Простые типы и типы из подключаемых заголовков
(gb_s и прочие) — можно.

Использование: python3 tools/idecheck.py gbemu/gbemu.ino
"""
import re
import sys

SKIP_RET = {"void", "static", "inline", "bool", "int", "long", "short", "char", "float",
            "double", "unsigned", "signed", "const", "uint8_t", "uint16_t", "uint32_t",
            "int8_t", "int16_t", "int32_t", "uint64_t", "int64_t", "size_t", "byte",
            "String", "auto"}


def strip_comments(src):
    """Убрать /* … */ и // … чтобы не путать код с комментариями."""
    out = []
    i = 0
    n = len(src)
    while i < n:
        if src.startswith("/*", i):
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
            out.append("\n")
        elif src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
        else:
            out.append(src[i]); i += 1
    return "".join(out)


def sketch_types(src):
    """Имена типов, объявленных в самом скетче."""
    names = set()
    for pat in (r"\bstruct\s+(\w+)\s*(?:\{|$)", r"\bclass\s+(\w+)\s*(?:\{|:|$)",
                r"\bunion\s+(\w+)\s*(?:\{|$)", r"\benum\s+(?:class\s+)?(\w+)\s*(?:\{|$)",
                r"\btypedef\b[^;()]*\b(\w+)\s*;"):
        for m in re.finditer(pat, src):
            names.add(m.group(1))
    return names


def top_level_functions(src):
    """Определения функций на верхнем уровне (внутри скобок глубины 0)."""
    depth = 0
    res = []
    for m in re.finditer(r"[^{}\n]*\([^;{}]*\)\s*\{|[{}]", src):
        tok = m.group(0)
        if tok.startswith("{"):
            depth += 1
            continue
        if tok.startswith("}"):
            depth = max(0, depth - 1)
            continue
        if depth == 0:
            res.append((src[:m.start()].count("\n") + 1, tok))
            depth += 1
    return res


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "gbemu/gbemu.ino"
    raw = open(path, encoding="utf-8", errors="replace").read()
    src = strip_comments(raw)
    types = sketch_types(src)
    bad = 0
    print("типы, объявленные в скетче:", ", ".join(sorted(types)) or "(нет)")
    if not types:
        print("IDECHECK: нечего проверять (нет своих типов)")
        return 0
    for line_no, decl in top_level_functions(src):
        d = " ".join(decl.split())
        if "(" not in d:
            continue
        ret = d.split("(")[0].strip()
        for kw in ("extern \"C\"", "__attribute__", "static", "inline"):
            while ret.startswith(kw):
                ret = ret[len(kw):].strip()
        for attr in re.findall(r"__attribute__\s*\(\([^)]*\)\)", ret):
            ret = ret.replace(attr, "").strip()
        ret = re.sub(r"\b(naked|used|noinline|always_inline)\b", "", ret).strip()
        # параметры
        params = d[d.find("(") + 1:d.rfind(")")]
        for t in sorted(types):
            if re.search(r"\b%s\b" % re.escape(t), ret):
                print(f"[ОШИБКА] {path}:{line_no}: функция возвращает тип «{t}», объявленный в скетче")
                print("          Arduino IDE поставит прототип этой функции выше объявления типа,")
                print("          и сборка упадёт: «'%s' does not name a type»." % t)
                print("          Решение: отдавать результат через простые типы или глобальные переменные.")
                bad += 1
            if re.search(r"\b%s\b" % re.escape(t), params) and "*" not in params and "&" not in params:
                print(f"[ОШИБКА] {path}:{line_no}: параметр функции имеет тип «{t}» из скетча "
                      f"(по значению) — прототип от IDE не соберётся")
                bad += 1
    if bad:
        print(f"IDECHECK: ПРОБЛЕМ {bad} (в Arduino IDE не соберётся)")
        return 1
    print("IDECHECK: чисто (ни одна функция не использует свои типы в подписи)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
