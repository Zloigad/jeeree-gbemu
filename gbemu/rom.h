/*
 * rom.h — ЗАГЛУШКА (в ней нет игры).
 *
 * Замените этот файл настоящим: сгенерируйте его из своего .gb-файла:
 *
 *   Python:      python make_rom_header.py game.gb
 *   Windows PS:  powershell -ExecutionPolicy Bypass -File make_rom_header.ps1 game.gb
 *
 * Скрипт положит рядом файл rom.h — положите его в папку скетча (заменив этот)
 * и пересоберите прошивку.
 *
 * Пока ROM_SIZE = 0, скетч покажет экран «NO ROM» и будет доступен режим
 * проверки/обучения кнопок (команды t и l в Serial).
 */
#ifndef ROM_H
#define ROM_H

#include <stdint.h>

#define ROM_NAME "PLACEHOLDER"
static const unsigned int ROM_SIZE = 0;
static const uint32_t ROM_MASK = 0;
static const uint8_t ROM_DATA[1] = { 0x00 };

#endif /* ROM_H */
