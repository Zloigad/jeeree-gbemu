#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
selftest.py — самопроверка упаковщика make_roms_uf2.py (без консоли).

Что проверяем:
  * игры складываются в область картриджей подряд, с выравниванием на сектор;
  * таблица игр лежит на TABLE_OFF (GMR2: 0x80000), страница выбора игры НЕ пишется;
  * games.uf2 содержит блоки только по реальным данным (таблица + образы),
    ничего не перезаписывается вне области картриджей;
  * адреса/семейство UF2 верные (иначе загрузчик RP2040 файл отвергнет);
  * имена, размеры, CRC32 и размеры памяти сохранений в таблице совпадают
    с исходными файлами (это ключи сейвов — ошибиться нельзя);
  * что не влезло — честно сообщается, НЕ попадает в файл, и скрипт выходит кодом 2
    (а с ключом --allow-skip — кодом 0);
  * --fit ничего не пишет.

Побочный результат для стенда: tools/roms_pack/_region.bin (сырая область игр)
и tools/roms_pack/_region.txt (что в ней должно быть). Их читает стенд, чтобы
проверить, что скетч разбирает настоящий файл упаковщика.
"""
import io
import contextlib
import os
import struct
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import make_roms_uf2 as pk                                    # noqa: E402

fails = 0
oks = 0


def chk(ok, what):
    global fails, oks
    if ok:
        oks += 1
        print("[ok]   " + what)
    else:
        fails += 1
        print("[FAIL] " + what)


def make_rom(size, title, mbc, ram_byte, seed=1):
    rom = bytearray(seed & 0xFF for _ in range(size))
    rom[0x100:0x104] = bytes([0x00, 0xC3, 0x50, 0x01])
    rom[0x104:0x109] = bytes([0xCE, 0xED, 0x66, 0x66, 0xCC])
    rom[0x134:0x144] = b"\x00" * 16                    # как в настоящих образах: имя и нули
    t = title.encode("ascii")[:16]
    rom[0x134:0x134 + len(t)] = t
    rom[0x147] = mbc
    rom[0x148] = {32768: 0, 65536: 1, 131072: 2, 262144: 3, 524288: 4, 1048576: 5, 2097152: 6}.get(size, 0)
    rom[0x149] = ram_byte
    x = 0
    for b in rom[0x134:0x14D]:
        x = (x - b - 1) & 0xFF
    rom[0x14D] = x
    return bytes(rom)


def parse_uf2(data):
    """Возвращает (список (адрес, байты), семейство, число блоков, магия в порядке)"""
    blocks, fam, order_ok = [], None, True
    n = len(data) // 512
    for i in range(n):
        b = data[i * 512:(i + 1) * 512]
        m0, m1, flags, addr, paylen, blkno, total, family = struct.unpack_from("<8I", b, 0)
        m_end = struct.unpack_from("<I", b, 508)[0]
        if (m0, m1, m_end) != (pk.UF2_MAGIC0, pk.UF2_MAGIC1, pk.UF2_MAGIC_END):
            order_ok = False
        if flags != pk.UF2_FLAG_FAMILY:
            order_ok = False
        if blkno != i or total != n:
            order_ok = False
        fam = family
        blocks.append((addr, b[32:32 + paylen]))
    return blocks, fam, n, order_ok


def image_of(blocks, base=0x10000000):
    """(смещение образа от начала flash, байты образа)"""
    lo = min(a for a, _ in blocks)
    hi = max(a + len(d) for a, d in blocks)
    img = bytearray(b"\xff" * (hi - lo))
    for a, d in blocks:
        img[a - lo:a - lo + len(d)] = d
    return lo - base, img


def main():
    print("=== упаковщик ROM'ов: самопроверка ===")
    tmp = tempfile.mkdtemp(prefix="romspack_")
    roms = {
        "one.gb":   (make_rom(65536,  "TEST ONE",   0x03, 0x02, seed=0x11), 8192),   # MBC1+RAM+батарея
        "two.gb":   (make_rom(65536,  "TEST TWO",   0x19, 0x00, seed=0x22), 0),      # MBC5 без памяти
        "three.gb": (make_rom(524288, "TEST THREE", 0x1B, 0x03, seed=0x33), 32768),  # MBC5+RAM+батарея
    }
    paths = []
    for name, (data, _) in roms.items():
        p = os.path.join(tmp, name)
        open(p, "wb").write(data)
        paths.append(p)
    out = os.path.join(tmp, "games.uf2")
    binp = os.path.join(tmp, "games.bin")

    rc = pk.main(["make_roms_uf2.py", "--out", out, "--bin", binp] + paths)
    chk(rc == 0, "упаковщик отработал без ошибок")
    chk(os.path.isfile(out) and os.path.isfile(binp), "получились games.uf2 и сырой образ")

    data = open(out, "rb").read()
    blocks, fam, nblk, order_ok = parse_uf2(data)
    chk(fam == pk.UF2_FAMILY_RP2040, "в блоках указано семейство RP2040 (загрузчик такой файл примет)")
    chk(order_ok, "все блоки UF2 с верными магиями, номерами и флагами")

    lo, img = image_of(blocks)
    chk(lo == pk.TABLE_OFF, "самый первый блок — таблица игр на 0x%X" % pk.TABLE_OFF)
    touched = set()
    for a, d in blocks:
        for off in range(a - 0x10000000, a - 0x10000000 + len(d), 4096):
            touched.add(off & ~4095)
    chk(pk.STATE_OFF not in touched,
        "страница «какая игра выбрана» (0x%X) НЕ пишется — выбор игры не сбрасывается" % pk.STATE_OFF)
    chk(min(touched) >= pk.TABLE_OFF and max(touched) < 0x1FD000,
        "запись не выходит за область картриджей (прошивка и сейвы не тронуты)")

    magic, count, used = struct.unpack_from("<III", img, pk.TABLE_OFF - lo)
    chk(magic == pk.TABLE_MAGIC, "в таблице верная магия")
    chk(count == 3, "в таблице ровно три игры")
    chk(used == (65536 + 65536 + 524288) // 1024, "объём занятого места посчитан верно")

    for i, (name, (data_i, sram)) in enumerate(roms.items()):
        e = img[pk.TABLE_OFF - lo + 16 + i * 40: pk.TABLE_OFF - lo + 16 + i * 40 + 40]
        nm = e[:24].split(b"\x00")[0].decode("ascii", "replace")
        off, size, crc, sram_b = struct.unpack_from("<IIII", e, 24)
        chk(nm == pk.title_of(data_i, name), "имя игры №%d прочитано из заголовка: %r" % (i + 1, nm))
        chk(size == len(data_i), "размер игры №%d — степень двойки, равная файлу" % (i + 1))
        chk(crc == (zlib.crc32(data_i) & 0xFFFFFFFF), "ключ сейва игры №%d = CRC32 образа" % (i + 1))
        chk(sram_b == sram, "память сохранений игры №%d определена верно (%d Б)" % (i + 1, sram_b))
        chk(off % 4096 == 0, "игра №%d начинается с целого сектора" % (i + 1))
        chk(img[off - lo:off - lo + len(data_i)] == data_i, "образ игры №%d записан побайтово" % (i + 1))

    # --- таблица, которую прочитает скетч, должна указывать на реальные образы ---
    for i in range(count):
        e = img[pk.TABLE_OFF - lo + 16 + i * 40: pk.TABLE_OFF - lo + 16 + i * 40 + 40]
        off, size = struct.unpack_from("<II", e, 24)
        r = img[off - lo:off - lo + size]
        chk(r[0x104:0x106] == b"\xCE\xED", "у игры №%d на месте логотип Nintendo (скетч так проверяет)" % (i + 1))

    # --- сырой образ области: раскладка должна совпадать с адресами в таблице ---
    binf = open(binp, "rb").read()
    chk(len(binf) == pk.DATA_OFF - pk.ROMS_BASE + pk.CAPACITY,
        "сырой образ покрывает ровно область картриджей")
    chk(binf[:4096] == img[pk.TABLE_OFF - lo: pk.TABLE_OFF - lo + 4096],
        "таблица в сыром образе лежит в самом начале области")
    chk(binf[4096:8192] == b"\xff" * 4096, "страница выбора игры в образе не тронута (0xFF)")
    for i, (name, (data_i, sram)) in enumerate(roms.items()):
        e = img[pk.TABLE_OFF - lo + 16 + i * 40: pk.TABLE_OFF - lo + 16 + i * 40 + 40]
        off, size = struct.unpack_from("<II", e, 24)
        pos = off - pk.ROMS_BASE
        chk(binf[pos:pos + len(data_i)] == data_i,
            "в сыром образе игра №%d лежит по адресу из таблицы (можно лить picotool'ом)" % (i + 1))

    # --- что не влезло ---
    big = os.path.join(tmp, "big.gb")
    open(big, "wb").write(make_rom(2097152, "TOO BIG", 0x00, 0x00, seed=9))   # 2 МБ > всей области
    big2 = os.path.join(tmp, "big2.gb")
    open(big2, "wb").write(make_rom(1048576, "TEST ONE", 0x00, 0x00, seed=5))
    out2 = os.path.join(tmp, "games2.uf2")
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = pk.main(["make_roms_uf2.py", "--out", out2] + paths + [big, big, big])
    text2 = buf.getvalue()
    # v8.9q: пропущенные игры — это ЛОВУШКА для пользователя, поэтому скрипт обязан
    # сказать об этом крупно и выйти ненулевым кодом (раньше код был 0 — можно было
    # не заметить, что половина игр не попала в консоль).
    chk(rc == 2, "упаковщик не падает, но выходит кодом 2, когда игры не влезли")
    chk("НЕ ВЛЕЗЛО" in text2 and "в консоли их НЕ БУДЕТ" in text2,
        "про пропущенные игры сказано крупно и прямо")
    chk("образ больше всей области" in text2,
        "у пропущенной игры названа причина: образ больше всей области под игры")
    chk("лимиты: игр максимум %d" % pk.ROM_MAX in text2
        and ("%d КиБ" % (pk.CAPACITY // 1024)) in text2,
        "в логе напечатаны лимиты: игр максимум %d и %d КиБ под образы"
        % (pk.ROM_MAX, pk.CAPACITY // 1024))
    chk("--allow-skip" in text2, "подсказано, как сознательно упаковать без пропущенных")
    d2 = open(out2, "rb").read()
    b2, _, _, _ = parse_uf2(d2)
    lo2, img2 = image_of(b2)
    cnt2 = struct.unpack_from("<I", img2, pk.TABLE_OFF - lo2 + 4)[0]
    chk(cnt2 == 3, "в таблицу попали только те игры, что влезли")

    # то же, но с явным разрешением — код 0
    out2b = os.path.join(tmp, "games2b.uf2")
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = pk.main(["make_roms_uf2.py", "--out", out2b, "--allow-skip"] + paths + [big2])
    chk(rc == 0 and os.path.isfile(out2b), "--allow-skip: упаковывает без них и не ругается кодом")

    # проверка ДО записи: --fit на заведомо не помещающемся наборе молчит файлом и даёт код 2
    out2c = os.path.join(tmp, "games2c.uf2")
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = pk.main(["make_roms_uf2.py", "--out", out2c, "--fit"] + paths + [big2])
    chk(rc == 2 and not os.path.isfile(out2c),
        "--fit: показывает расклад, ничего не пишет и предупреждает кодом 2")

    # много мелких игр: место кончается раньше, чем 48 записей таблицы, и об этом надо
    # сказать той же ловушкой (раньше в таблице оказывался счётчик «50», а записей 48)
    many = []
    for i in range(50):
        p = os.path.join(tmp, "many%02d.gb" % i)
        open(p, "wb").write(make_rom(32768, "MANY %02d" % i, 0x00, 0x00, seed=0x40 + i))
        many.append(p)
    out2d = os.path.join(tmp, "games2d.uf2")
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = pk.main(["make_roms_uf2.py", "--out", out2d] + many)
    text = buf.getvalue()
    d2d = open(out2d, "rb").read()
    b2d, _, _, _ = parse_uf2(d2d)
    lo2d, img2d = image_of(b2d)
    cnt2d, used2d = struct.unpack_from("<II", img2d, pk.TABLE_OFF - lo2d + 4)
    fit_max = pk.CAPACITY // 32768                     # сколько 32-КиБ игр влезает по месту
    chk(cnt2d == fit_max and fit_max < pk.ROM_MAX,
        "в таблицу попали ровно те игры, что влезли по месту (%d), и счётчик не врёт" % fit_max)
    chk(used2d == fit_max * 32, "в таблице записан настоящий занятый объём (%d КиБ)" % used2d)
    chk(rc == 2 and "не влезло по месту" in text,
        "про игры, которым не хватило места, сказано причиной «не влезло по месту»")
    # и «по месту» срабатывает именно тогда, когда игра влезла бы сама по себе
    big3 = os.path.join(tmp, "big3.gb")
    open(big3, "wb").write(make_rom(524288, "BIG THREE", 0x00, 0x00, seed=0x77))
    big3b = os.path.join(tmp, "big3b.gb")
    open(big3b, "wb").write(make_rom(524288, "BIG THREE2", 0x00, 0x00, seed=0x78))
    out2e = os.path.join(tmp, "games2e.uf2")
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = pk.main(["make_roms_uf2.py", "--out", out2e] + paths + [big3, big3b, big])
    text2e = buf.getvalue()
    chk(rc == 2 and "не влезло по месту" in text2e and "образ больше всей области" in text2e,
        "в одном прогоне видны обе причины: «по месту» и «больше всей области»")
    chk("освободите" in text2e and "например:" in text2e,
        "сказано, СКОЛЬКО освободить и какие мелкие игры убрать, чтобы крупная вошла")

    # --- --fit ---
    out3 = os.path.join(tmp, "fit.uf2")
    rc = pk.main(["make_roms_uf2.py", "--out", out3, "--fit"] + paths[:1])
    chk(rc == 0 and not os.path.isfile(out3), "--fit только показывает, файлов не пишет")

    # --- готовим образец для стенда ---
    lab = os.path.join(HERE, "_region.bin")
    labtxt = os.path.join(HERE, "_region.txt")
    open(lab, "wb").write(open(binp, "rb").read())
    with open(labtxt, "w") as f:
        f.write("count=%d\n" % count)
        f.write("used_kib=%d\n" % used)
        for i, (name, (data_i, sram)) in enumerate(roms.items()):
            base_i = pk.TABLE_OFF - lo + 16 + i * 40
            e = img[base_i: base_i + 40]
            off, size, crc, sram_b = struct.unpack_from("<IIII", e, 24)
            f.write("%d|%s|%d|%d|0x%08X|%d\n"
                    % (i, pk.title_of(data_i, name), off, size, crc, sram_b))
    print("образец для стенда: %s + %s" % (os.path.basename(lab), os.path.basename(labtxt)))

    print("\n=== %s (%d проверок, провалено %d) ===" % ("ЕСТЬ ОШИБКИ" if fails else "ВСЁ ПРОШЛО", oks, fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
