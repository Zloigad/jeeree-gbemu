# Сборка из исходников и тесты

> EN: Build with PlatformIO (one command) or Arduino IDE; run the self-test
> suite with `bash tools/check_all.sh`. CI does both on every commit.

## Структура репозитория

```
gbemu/         исходники прошивки (Arduino IDE: открой gbemu.ino)
gbemu_pio/     проект PlatformIO (одна команда до .uf2)
tools/         автотесты, упаковка набора игр (pack_roms_kit.py), бэкап-утилита (bin2uf2.py)
roms_kit/      готовый набор сборщика игр — CI кладёт его в релиз как jeeree_roms_kit.zip
docs/          гайды
```

## Сборка (PlatformIO, рекомендуется)

```bash
pip install platformio
python3 -m platformio run -d gbemu_pio
# готовый файл:
#   gbemu_pio/.pio/build/pico/firmware.uf2
```

Файл можно сразу перетаскивать на диск `RPI-RP2` (можно переименовать, например
в `gbemu_v9.7.2_jeeree.uf2`).

## Сборка (Arduino IDE)

1. Arduino IDE → установи поддержку RP2040 (Board Manager →
   «Raspberry Pi Pico/RP2040» от Earle F. Philhower III).
2. Открой `gbemu/gbemu.ino`.
3. Настройки платы: **CPU Speed → 250 MHz**, Optimize → **Fastest (-O3)**
   (это важно: с -Os прошивка будет заметно медленнее).
4. «Загрузка» — IDE сама подготовит файлы; прошивается через тот же BOOTSEL
   (MENU→START) перетаскиванием `.uf2` из папки сборки.

## Тесты

Большой набор самопроверок (панель, flash, сейвы, звук, загрузка, память):

```bash
bash tools/check_all.sh
```

Он собирает прошивку под хост (g++), прогоняет сотни проверок, ловит ошибки
памяти санитайзерами и проверяет звуковой тракт. Перед релизом — обязан быть
зелёным.

Быстрая проверка стенда без полного прогона — см. заголовок `tools/check_all.sh`.

## Воспроизводимость релизов

GitHub Actions (`.github/workflows/build.yml`) собирает прошивку и гоняет
тесты на каждый коммит и тег — бинарники в Releases получаются из этого же
кода, а не «откуда-то».
