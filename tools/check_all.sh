#!/bin/bash
# check_all.sh — полный прогон проверок на компьютере (без консоли).
#  1) синтаксис скетча
#  2) «грабли» arduino-pico (недопустимые пины SPI -> panic -> чёрный экран)
#  3) сверка с pico-sdk: нет ли заголовков/функций, которых в 1.5.1 не существует
#     (именно на этом упала сборка: hardware/dreq.h)
#  4) имитация предсборки Arduino (автопрототипы)
#  5) стенд: панель + виртуальный flash + сейвы + звук + имитация загрузки setup()/loop()
#  6) тесты APU + превью WAV
set -e
set -o pipefail        # иначе «| tail» съедает код возврата, и стадия может провалиться незаметно
cd "$(dirname "$0")/.."
echo "=== 1. синтаксис скетча ==="
bash tools/stub/check.sh gbemu/gbemu.ino
echo
echo "=== 2. проверка граблей arduino-pico ==="
python3 tools/apicheck.py gbemu/gbemu.ino
echo
echo "=== 3. сверка с pico-sdk (заголовки и функции) ==="
python3 tools/sdkcheck.py
echo
echo "=== 4а. свои типы в подписях функций (грабли Arduino IDE) ==="
python3 tools/idecheck.py gbemu/gbemu.ino
echo
echo "=== 4в. встроенный ассемблер: только Thumb-1 (наш M0+) ==="
python3 tools/asmcheck.py gbemu/gbemu.ino
echo
echo "=== 4б. имитация сборки Arduino (автопрототипы) ==="
if ! python3 tools/arduino_sim.py gbemu/gbemu.ino -Itools/stub -Igbemu > /tmp/_arduino_sim.log 2>&1; then
  cat /tmp/_arduino_sim.log            # показываем ошибку целиком, а не последние строки
  echo "СТАДИЯ 4 ПРОВАЛЕНА: в Arduino IDE это не соберётся"
  exit 1
fi
tail -3 /tmp/_arduino_sim.log
echo
echo "=== 4в. упаковщик ROM'ов (несколько игр во flash) ==="
python3 tools/roms_pack/selftest.py > /tmp/_pack.log 2>&1 || { tail -25 /tmp/_pack.log; echo "УПАКОВЩИК: самопроверка провалена"; exit 1; }
tail -2 /tmp/_pack.log
echo
echo "=== 5. стенд: панель + flash + сейвы + звук + загрузка ==="
g++ -std=gnu++17 -O1 -DGBEMU_HOST=1 -I tools/stub -I tools/fakerom -I gbemu tools/harness/harness.cpp -o tools/harness/harness2
GBEMU_TEST_REGION=tools/roms_pack/_region.bin \
GBEMU_TEST_REGION_TXT=tools/roms_pack/_region.txt ./tools/harness/harness2 | tail -7
echo
echo "=== 6. память и выравнивание (санитайзеры) ==="
g++ -std=gnu++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -DGBEMU_HOST=1 -I tools/stub -I tools/fakerom -I gbemu tools/harness/harness.cpp -o tools/harness/harness_asan
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=0 \
GBEMU_TEST_REGION=tools/roms_pack/_region.bin GBEMU_TEST_REGION_TXT=tools/roms_pack/_region.txt \
timeout 300 ./tools/harness/harness_asan > /tmp/_asan.log 2>&1 || {
  echo "САНИТАЙЗЕР уронил стенд:"; grep -E "ERROR|runtime error" /tmp/_asan.log | head -10; exit 1; }
if grep -qE "runtime error|AddressSanitizer" /tmp/_asan.log; then
  echo "САНИТАЙЗЕР нашёл проблему (память/выравнивание):"
  grep -E "runtime error|ERROR: AddressSanitizer" /tmp/_asan.log | sort -u | head -10
  exit 1
fi
echo "SANITIZERS: чисто (ни выходов за границы, ни невыровненных обращений)"
echo
echo "=== 6. тесты APU ==="
g++ -std=gnu++17 -O2 -Wall tools/apu_test.cpp -o tools/apu_test
./tools/apu_test | tail -3
