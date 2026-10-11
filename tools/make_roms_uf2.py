#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
make_roms_uf2.py — упаковщик игр для консоли с прошивкой gbemu v9.0 и новее (раскладка v9: область игр с 0x80000).

ЧТО ДЕЛАЕТ
  1) читает ваши файлы игр (.gb, .gbc, любые расширения), разбирает их заголовки
     (имя игры, тип картриджа, размер памяти сохранений);
  2) раскладывает их в ОБЛАСТЬ КАРТРИДЖЕЙ во flash консоли:
        0x80000..0x80FFF — таблица игр (кто где лежит)
        0x81000..0x81FFF — «какая игра выбрана» (эту страницу пишет сама консоль,
                           упаковщик её НЕ трогает — настройки не стираются)
        0x82000..0x1FD000 — сами образы: 1516 КиБ (1 552 384 байт) на ВСЕ игры;
  3) пишет файл games.uf2 — его достаточно ПЕРЕТАЩИТЬ на диск RPI-RP2, как обычно.
     Прошивку при этом пересобирать и перезаливать НЕ нужно: область картриджей
     отдельная, скетч её никогда не пишет.

  ВАЖНО ПРО БЕЗОПАСНОСТЬ ДАННЫХ: скрипт ничего никуда не отправляет — он работает
  только с файлами у вас на диске. Записывается ТОЛЬКО область картриджей; загрузчик
  RP2040 стирает лишь те сектора, которые пишет, поэтому прошивка, сейвы SRAM и
  сейвстейты остаются на месте.

КАК ПОЛЬЗОВАТЬСЯ (Windows: python, macOS/Linux: python3)
  Положите этот файл в папку с играми и запустите без аргументов:
      python make_roms_uf2.py
  или укажите файлы/папки явно:
      python make_roms_uf2.py zelda.gb tetris.gb mario.gb
      python make_roms_uf2.py "C:\\игры\\*.gb"        (звёздочки тоже понимает)
      python make_roms_uf2.py --dir C:\\игры
  Порядок игр в консоли — тот же, в каком вы их перечислили (файлы в папке — по имени).

  Полезные ключи:
      --out games.uf2     имя файла (по умолчанию games.uf2 рядом со скриптом)
      --bin games.bin     дополнительно сохранить сырой образ области (для picotool)
      --fit               только показать, что влезет, ничего не писать
      --keep-order        не сортировать файлы из папки (оставить как отдал Windows)
      --allow-skip        разрешить упаковать без тех игр, что не влезли (код 0)


ЧТО ПОТОМ
  1) переведите консоль в режим загрузчика — на этой плате это клавиша 'b' в Serial
     (или зажать BOOTSEL при включении);
  2) перетащите games.uf2 на появившийся диск RPI-RP2 — консоль сама перезагрузится;
     ЛИМИТЫ (их задаёт раскладка flash консоли, обойти нельзя):
       под игры:      1516 КиБ на все игры вместе (область 0x82000..0x1FD000;
                      с v9.0 граница опущена: прошивка ~135 КБ, +512 КиБ под игры);
       игр в списке:  максимум 48;
       только DMG:    игры «только для Game Boy Color» (флаг 0xC0) ядро не тянет —
                      скрипт их упакует, но честно предупредит (детское окно такие не берёт);
       один образ:    не больше 1516 КиБ и обязан быть степенью двойки — значит
                      САМАЯ БОЛЬШАЯ игра, которая влезет, — 1024 КиБ (1 МБ).
                      Покемоны и другие игры на 1 МБ — ВЛЕЗАЮТ. Больше 1 МБ —
                      не влезет никогда: таких игр почти нет, а места нет тем более.
     Если игры не помещаются, скрипт пишет файл БЕЗ них, крупно об этом сообщает
     и завершается кодом 2 (а не 0) — чтобы это нельзя было не заметить.

  3) на экране появится список игр: UP/DOWN — выбор, A — играть.
     Список можно открыть и позже: клавиша c в Serial или меню (m) → GAMES.
"""

import os
import struct
import sys
import zlib

# ---- раскладка области картриджей (должна совпадать с gbemu.ino) ----
ROMS_BASE   = 0x80000                      # смещение от начала flash (v9.0: было 0x100000)
TABLE_OFF   = 0x80000
STATE_OFF   = 0x81000
DATA_OFF    = 0x82000
CAPACITY    = 0x1FD000 - DATA_OFF          # сколько отдано под образы: 1516 КиБ
TABLE_MAGIC = 0x32524D47                   # "GMR2" (v9.0: старые наборы GMR1 прошивка не видит)
ROM_NAME_LEN = 24
ROM_MAX      = 48
ALIGN        = 4096                        # каждый образ начинается с целого сектора


def single_max_kib():
    """Самый большой образ, который влезает сам по себе: степень двойки ≤ CAPACITY."""
    m = 1
    while m * 2 * 1024 <= CAPACITY:
        m *= 2
    return m

# ---- UF2 (формат загрузчика RP2040) ----
UF2_MAGIC0, UF2_MAGIC1, UF2_MAGIC_END = 0x0A324655, 0x9E5D5157, 0x0AB16F30
UF2_FLAG_FAMILY = 0x00002000
UF2_FAMILY_RP2040 = 0xE48BFF56
UF2_CHUNK = 256

# ---- заголовок Game Boy ----
NINTENDO_LOGO_AT = 0x104
TITLE_AT, TITLE_LEN = 0x134, 16
TYPE_AT, ROMSZ_AT, RAMSZ_AT = 0x147, 0x148, 0x149
CKSUM_AT = 0x14D
CGB_AT = 0x143                              # флаг Color: 0x80 = обе, 0xC0 = только GBC

MBC_NAMES = {
    0x00: "ROM only", 0x01: "MBC1", 0x02: "MBC1+RAM", 0x03: "MBC1+RAM+BATT",
    0x05: "MBC2", 0x06: "MBC2+BATT", 0x08: "ROM+RAM", 0x09: "ROM+RAM+BATT",
    0x0B: "MMM01", 0x0C: "MMM01+RAM", 0x0D: "MMM01+RAM+BATT",
    0x0F: "MBC3+TIMER+BATT", 0x10: "MBC3+TIMER+RAM+BATT", 0x11: "MBC3",
    0x12: "MBC3+RAM", 0x13: "MBC3+RAM+BATT", 0x19: "MBC5", 0x1A: "MBC5+RAM",
    0x1B: "MBC5+RAM+BATT", 0x1C: "MBC5+RUMBLE", 0x1D: "MBC5+RUMBLE+RAM",
    0x1E: "MBC5+RUMBLE+RAM+BATT", 0x20: "MBC6", 0x22: "MBC7", 0xFC: "POCKET CAMERA",
}
BATTERY_TYPES = {0x03, 0x06, 0x09, 0x0D, 0x0F, 0x10, 0x13, 0x1B, 0x1E, 0x22, 0xFF}
RAM_KB = {0x00: 0, 0x01: 2, 0x02: 8, 0x03: 32, 0x04: 128, 0x05: 64}


class Info:
    def __init__(self, path, data, name, size, crc, sram, mbc, warns, cgb_only=False):
        self.path, self.data, self.name, self.size = path, data, name, size
        self.crc, self.sram, self.mbc, self.warns = crc, sram, mbc, warns
        self.cgb_only = cgb_only


def title_of(rom: bytes, path: str) -> str:
    raw = rom[TITLE_AT:TITLE_AT + TITLE_LEN]
    txt = "".join(chr(b) if 32 <= b < 127 else " " for b in raw)
    txt = " ".join(txt.split())                      # убираем края и двойные пробелы
    for bad in ('"', "'", "\\", "/"):                # на экране это мешает
        txt = txt.replace(bad, " ")
    if txt.upper() in ("", "GAMEBOY", "UNKNOWN"):
        txt = os.path.splitext(os.path.basename(path))[0]
    return txt[:ROM_NAME_LEN - 1]


def check_header(rom: bytes) -> tuple:
    """(логотип на месте, сумма заголовка сходится)"""
    logo = rom[NINTENDO_LOGO_AT:NINTENDO_LOGO_AT + 2] == b"\xCE\xED"
    x = 0
    for b in rom[TITLE_AT:CKSUM_AT]:
        x = (x - b - 1) & 0xFF
    return logo, (x == rom[CKSUM_AT])


def load_rom(path: str) -> Info:
    with open(path, "rb") as f:
        rom = f.read()
    warns = []
    if len(rom) < 0x150:
        raise ValueError("файл короче 336 байт — это не образ Game Boy")

    logo, sumok = check_header(rom)
    if not logo and not sumok:
        warns.append("заголовок не похож на игру (ни логотипа, ни верной суммы)")
    if len(rom) > 1024 * 1024:
        warns.append("образ больше 1 МБ — в консоль не влезет")
    size = 1
    while size < len(rom):                           # размер всегда степень двойки
        size *= 2
    if size != len(rom):
        warns.append("размер %d Б не степень двойки — дополнил до %d Б (игра может не пойти)" % (len(rom), size))
    if size < 0x8000:
        size = 0x8000

    cgb_only = (rom[CGB_AT] == 0xC0)
    if cgb_only:
        warns.append("ИГРА ТОЛЬКО ДЛЯ GAME BOY COLOR — ядро консоли (DMG) её НЕ потянет")
    mbc = rom[TYPE_AT]
    sram_b = RAM_KB.get(rom[RAMSZ_AT], 0) or (8 if mbc in BATTERY_TYPES else 0)
    if mbc in BATTERY_TYPES and rom[RAMSZ_AT] == 0:
        warns.append("тип картриджа с батареей, а размер памяти сохранений не указан — взял 8 КиБ")
    if mbc not in MBC_NAMES:
        warns.append("незнакомый тип картриджа 0x%02X (может не запуститься)" % mbc)

    crc = zlib.crc32(rom) & 0xFFFFFFFF
    if crc in (0, 0xFFFFFFFF):                       # ключ сейва не должен быть «пустым»
        crc ^= 0x5A5A5A5A
    return Info(path, rom, title_of(rom, path), size, crc, sram_b * 1024, mbc, warns, cgb_only)


def expand_args(args):
    import glob
    out = []
    for a in args:
        if os.path.isdir(a):
            out += sorted(glob.glob(os.path.join(a, "*")))
        elif any(c in a for c in "*?["):
            out += sorted(glob.glob(a))
        else:
            out.append(a)
    return out


def build(infos, verbose=True):
    """Раскладывает игры подряд; возвращает (blob, table, размещённые, пропущенные).

    Пропущенные — список пар (игра, причина). По причине видно, ЧТО делать: убрать
    конкретную игру, убрать крупные, или ничего не делать — игра физически больше
    всей области под игры."""
    blob = bytearray(b"\xff" * CAPACITY)             # стёртая flash = 0xFF
    placed, skipped = [], []
    off = DATA_OFF
    for inf in infos:
        need = (inf.size + ALIGN - 1) // ALIGN * ALIGN
        if inf.size > CAPACITY:
            skipped.append((inf, "образ больше всей области (лимит %d КиБ)" % (CAPACITY // 1024)))
            continue
        if off + need > DATA_OFF + CAPACITY:
            skipped.append((inf, "не влезло по месту"))
            continue
        inf.off = off
        blob[off - DATA_OFF:off - DATA_OFF + len(inf.data)] = inf.data
        off += need
        placed.append(inf)

    # Больше ROM_MAX игр в таблицу не влезает. Лишние отбрасываем ДО записи таблицы:
    # иначе в заголовке оказалось бы «50 игр», а записей — 48 (это ловушка: консоль
    # читала бы пустые записи). По месту 48 игр всё равно недостижимо (48 * 32 КиБ =
    # 1536 КиБ > 1004 КиБ), но пусть защита будет честной.
    if len(placed) > ROM_MAX:
        for inf in placed[ROM_MAX:]:
            skipped.append((inf, "список заполнен: в консоль влезает не больше %d игр" % ROM_MAX))
        placed = placed[:ROM_MAX]
    used = 0
    if placed:
        last = placed[-1]
        used = (last.off - DATA_OFF) + (last.size + ALIGN - 1) // ALIGN * ALIGN

    table = bytearray(b"\xFF" * 4096)
    struct.pack_into("<III", table, 0, TABLE_MAGIC, len(placed), used // 1024)
    for i, inf in enumerate(placed):
        name = inf.name.encode("ascii", "replace")[:ROM_NAME_LEN - 1]
        struct.pack_into("<%ds" % ROM_NAME_LEN, table, 16 + i * 40, name)   # имя + нули
        struct.pack_into("<IIII", table, 16 + i * 40 + 24, inf.off, inf.size, inf.crc, inf.sram)
    return bytes(blob), bytes(table), placed, skipped, used


def write_uf2(blocks, dst, base=0x10000000):
    """blocks: список (смещение_во_flash, байты). Пишем только реальные данные —
    загрузчик стирает ровно те сектора, которые получает."""
    total = sum((len(b) + UF2_CHUNK - 1) // UF2_CHUNK for _, b in blocks)
    n = 0
    with open(dst, "wb") as f:
        for off, data in blocks:
            for i in range(0, len(data), UF2_CHUNK):
                chunk = data[i:i + UF2_CHUNK]
                chunk += b"\x00" * (UF2_CHUNK - len(chunk))
                hdr = struct.pack("<8I", UF2_MAGIC0, UF2_MAGIC1, UF2_FLAG_FAMILY,
                                  base + off + i, UF2_CHUNK, n, total, UF2_FAMILY_RP2040)
                f.write(hdr + chunk + b"\x00" * (476 - UF2_CHUNK) + struct.pack("<I", UF2_MAGIC_END))
                n += 1
    return total


def main(argv):
    files, out, binout, fit, allow_skip = [], "games.uf2", None, False, False
    args = list(argv[1:])
    i = 0
    while i < len(args):
        a = args[i]
        if a in ("-h", "--help"):
            print(__doc__)
            return 0
        elif a in ("--out", "-o") and i + 1 < len(args):
            out = args[i + 1]; i += 2; continue
        elif a == "--bin" and i + 1 < len(args):
            binout = args[i + 1]; i += 2; continue
        elif a == "--dir" and i + 1 < len(args):
            files += expand_args([args[i + 1]]); i += 2; continue
        elif a == "--fit":
            fit = True; i += 1; continue
        elif a == "--keep-order":
            i += 1; continue
        elif a == "--allow-skip":
            allow_skip = True; i += 1; continue
        else:
            files.append(a); i += 1

    if not files:
        here = [f for f in sorted(os.listdir(".")) if os.path.isfile(f)]
        files = [f for f in here if os.path.splitext(f)[1].lower() in (".gb", ".gbc", ".sgb", ".rom", ".bin")]
        if not files:
            print("Не нашёл ни одной игры рядом со скриптом.\n"
                  "Положите сюда .gb-файлы и запустите снова, либо укажите их:\n"
                  "    python make_roms_uf2.py zelda.gb tetris.gb\n")
            return 1
    else:
        files = expand_args(files)

    infos = []
    for p in files:
        if not os.path.isfile(p):
            print("!! нет файла: %s (пропускаю)" % p)
            continue
        try:
            infos.append(load_rom(p))
        except Exception as ex:
            print("!! %s: %s (пропускаю)" % (p, ex))
    if not infos:
        print("Нечего упаковывать.")
        return 1

    blob, table, placed, skipped, used = build(infos)

    print("=== упаковка игр в %s ===" % os.path.basename(out))
    print("область картриджей: 0x%06X..0x%06X, под образы %d КиБ (%.2f МБ)"
          % (ROM_DATA := DATA_OFF, 0x1FD000, CAPACITY // 1024, CAPACITY / 1024.0 / 1024.0))
    print("лимиты: игр максимум %d, один образ не больше %d КиБ и только степень двойки"
          % (ROM_MAX, single_max_kib()))
    total_kib = 0
    for i, inf in enumerate(placed):
        total_kib += (inf.size + ALIGN - 1) // ALIGN * ALIGN // 1024
        extra = ""
        if inf.sram:
            extra += ", сейв %d КиБ" % (inf.sram // 1024)
        print("  %2d) %-20s %4d КиБ  %-18s%s%s" % (
            i + 1, inf.name, inf.size // 1024, MBC_NAMES.get(inf.mbc, "тип 0x%02X" % inf.mbc), extra,
            ("  (" + "; ".join(inf.warns) + ")") if inf.warns else ""))
    for inf, why in skipped:
        print("   -- %-20s %4d КиБ  ПРОПУЩЕНО (%s%s)" % (
            inf.name, inf.size // 1024, why,
            "" if not inf.warns else "; " + "; ".join(inf.warns)))
    free_kib = (CAPACITY - used) // 1024
    print("итого: %d игр, %d из %d КиБ занято, %d КиБ свободно"
          % (len(placed), used // 1024, CAPACITY // 1024, free_kib))
    if not placed:
        print("Ни одна игра не влезла — упаковывать нечего.")
        return 1
    if skipped:
        # ГЛАВНАЯ защита от «случайно упаковал больше, чем влезает»: пропущенные игры
        # в консоли просто НЕ ПОЯВЯТСЯ. Поэтому говорим громко и выходим кодом 2.
        print()
        print("=" * 72)
        print(" ВНИМАНИЕ: игр(ы) НЕ ВЛЕЗЛО — %d, и в консоли их НЕ БУДЕТ:" % len(skipped))
        for inf, why in skipped:
            print("    %-20s %4d КиБ — %s" % (inf.name, inf.size // 1024, why))
        print(" занято %d из %d КиБ, свободно %d КиБ" % (used // 1024, CAPACITY // 1024, free_kib))
        print(" что делать: уберите из списка крупные игры (%d КиБ — самая большая," % single_max_kib())
        print("             которая вообще влезает), либо запустите с ключом --fit,")
        print("             чтобы сперва посмотреть расклад и ничего не записывать")
        # и точная арифметика: сколько освободить, чтобы конкретная игра вошла
        for inf, why in skipped:
            if not why.startswith("не влезло по месту"):
                continue
            need = (inf.size + ALIGN - 1) // ALIGN * ALIGN
            deficit = need - (CAPACITY - used)
            if deficit <= 0:
                continue
            smalls, acc = [], 0
            for p in sorted(placed, key=lambda x: x.size):
                if acc >= deficit:
                    break
                smalls.append(p)
                acc += (p.size + ALIGN - 1) // ALIGN * ALIGN
            print(" чтобы вошла %-20s (%4d КиБ) — освободите %d КиБ, например: %s"
                  % (inf.name, inf.size // 1024, (deficit + 1023) // 1024,
                     ", ".join("%s (%d КиБ)" % (x.name, x.size // 1024) for x in smalls)))
        print("=" * 72)

    if fit:
        print("\n(--fit: файлы не писал)")
        return 2 if (skipped and not allow_skip) else 0

    blocks = [(TABLE_OFF, table)]                    # таблица игр (страницу выбора не трогаем!)
    for inf in placed:
        skip = len(inf.data)                          # хвост до сектора писать не нужно: там 0xFF
        blocks.append((inf.off, inf.data))
    n = write_uf2(blocks, out)
    print("\nзаписано: %s (%d блоков, %d КиБ)" % (out, n, n * 512 // 1024))
    if binout:
        # образ области от начала (0x100000): таблица, страница выбора как есть (0xFF),
        # дальше образы. Смещение образа i в файле = romList[i].off - ROMS_BASE.
        img = bytearray(b"\xFF" * (DATA_OFF - ROMS_BASE))
        img[TABLE_OFF - ROMS_BASE:TABLE_OFF - ROMS_BASE + 4096] = table
        img += blob
        with open(binout, "wb") as f:
            f.write(img)
        print("сырой образ области: %s (смещение в flash 0x%X, длина %d КиБ)"
              % (binout, ROMS_BASE, len(img) // 1024))

    print("""
Дальше:
  1) переведите консоль в режим загрузчика (клавиша 'b' в Serial или BOOTSEL);
  2) перетащите %s на диск RPI-RP2 — консоль сама перезагрузится;
  3) на экране будет список игр: UP/DOWN — выбор, A — играть.
     Позже список открывается клавишей c или через меню (m) -> GAMES.
Прошивку пересобирать не нужно, сейвы и настройки останутся на месте.""" % os.path.basename(out))
    if skipped and not allow_skip:
        print("\n(в файл ушли только %d игр из %d — остальные не влезли; код возврата 2,"
              " чтобы это было заметно. Отключить: --allow-skip)" % (len(placed), len(infos)))
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
