#!/bin/bash
# check.sh <файл.ino|файл.cpp> — проверка синтаксиса скетча
# рядом с файлом должны лежать peanut_gb.h и rom.h (или используйте заглушку)
f="$1"
cp "$f" /tmp/_chk.cpp
# 1) путь «стенд»: подменные регистры ядра (так собирает harness)
g++ -fsyntax-only -std=gnu++17 -Wall -Wextra -DGBEMU_HOST=1 -DF_CPU=250000000L -I"$(dirname "$0")" -I"$(dirname "$f")" /tmp/_chk.cpp || exit 1
# 2) путь «устройство»: настоящие адреса регистров и встроенный ассемблер (для M0+).
#    -fsyntax-only не ассемблирует, но ловит опечатки в этом коде.
g++ -fsyntax-only -std=gnu++17 -Wall -Wextra -DF_CPU=250000000L -I"$(dirname "$0")" -I"$(dirname "$f")" /tmp/_chk.cpp || exit 1
echo "SYNTAX OK: $f (оба пути: стенд и устройство)"
