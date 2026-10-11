#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Проверка «наш скетч собирается тем же pico-sdk, что стоит в Arduino IDE».

Зачем: 22.09 на консоли вылезла ошибка
    hardware/dreq.h: No such file or directory
Заголовка hardware/dreq.h в pico-sdk НЕТ (константы DREQ_* лежат в
hardware/regs/dreq.h и подключаются из hardware/dma.h / hardware/spi.h).
Хостовый стенд такое поймать не может: у него свои заглушки. Поэтому здесь
отдельная проверка: каждый заголовок pico-sdk, который мы подключаем, и каждая
«рискованная» функция сверяются со списком того, что РЕАЛЬНО есть в SDK, который
тянет за собой arduino-pico 4.4.1 (pico-sdk 1.5.1).

Обе таблицы составлены по исходникам pico-sdk 1.5.1; при переходе на другой SDK
их надо обновлять (см. hardware/*/include/hardware/*.h в дереве SDK).
"""

import re
import sys
import os

# --- 1. заголовки pico-sdk, которые существуют в 1.5.1 ---
HEADERS_OK = {
    "hardware/adc.h", "hardware/clocks.h", "hardware/dma.h", "hardware/flash.h",
    "hardware/gpio.h", "hardware/i2c.h", "hardware/irq.h", "hardware/pwm.h",
    "hardware/resets.h", "hardware/rtc.h", "hardware/spi.h", "hardware/sync.h",
    "hardware/timer.h", "hardware/uart.h", "hardware/watchdog.h", "hardware/xip.h",
    "pico/stdlib.h", "pico/time.h", "pico/multicore.h", "pico/platform.h",
    "pico/types.h", "pico/stdio.h", "pico/assert.h", "pico/sem.h", "pico/util/queue.h",
}
# шаблоны: hardware/structs/* и hardware/regs/* в SDK есть (это сгенерированные файлы)
HEADER_PAT_OK = re.compile(r"^hardware/(structs|regs)/[a-z0-9_]+\.h$")

# --- 2. имена, которых в 1.5.1 НЕТ (чтобы не наступить снова) ---
SYMBOLS_BAD = {
    "dma_channel_clear_irq1_status": "в 1.5.1 нет: используйте dma_channel_acknowledge_irq1",
    "dma_channel_clear_irq0_status": "в 1.5.1 нет: используйте dma_channel_acknowledge_irq0",
    "spi_set_baudrate(":             "есть в SDK, но менять частоту на ходу нельзя: SPI деинициализируется",
    "abortAsync":                    "есть только в новых arduino-pico (в 4.4.1 нет)",
}

# --- 3. идентификаторы pico-sdk, которые есть в 1.5.1 (белый список) ---
# Всё, что начинается с этих приставок, должно быть в списке: иначе опечатка.
SDK_PREFIXES = ("dma_", "channel_config_", "irq_", "spi_", "pwm_", "gpio_", "flash_",
                "hw_", "reset_", "xosc_", "clock_", "add_repeating_timer", "cancel_repeating_timer",
                "tight_loop_contents", "get_core_num", "multicore_", "sleep_ms", "sleep_us",
                "watchdog_", "uart_", "i2c_", "adc_", "rtc_", "busy_wait")
SYMBOLS_OK = {
    # dma
    "dma_claim_unused_channel", "dma_channel_get_default_config", "dma_channel_configure",
    "dma_channel_set_read_addr", "dma_channel_set_trans_count", "dma_channel_start",
    "dma_channel_abort", "dma_channel_acknowledge_irq1", "dma_channel_set_irq1_enabled",
    "dma_channel_is_busy", "dma_channel_wait_for_finish_blocking", "dma_channel_cleanup",
    "dma_channel_unclaim", "dma_timer_claim", "dma_timer_set_fraction",
    # channel_config
    "channel_config_set_transfer_data_size", "channel_config_set_read_increment",
    "channel_config_set_write_increment", "channel_config_set_dreq", "channel_config_set_chain_to",
    "channel_config_set_irq_quiet", "channel_config_set_enable", "channel_config_set_high_priority",
    "channel_config_set_ring", "channel_config_set_bswap", "channel_config_set_sniff_enable",
    # irq
    "irq_set_exclusive_handler", "irq_add_shared_handler", "irq_set_enabled", "irq_set_priority",
    "irq_is_enabled", "irq_set_mask_enabled", "irq_set_pending", "irq_has_shared_handler",
    # spi
    "spi_init", "spi_deinit", "spi_get_baudrate", "spi_set_baudrate", "spi_get_index",
    "spi_get_hw", "spi_set_format", "spi_is_writable", "spi_is_readable", "spi_is_busy",
    "spi_write_blocking", "spi_read_blocking", "spi_write_read_blocking", "spi_get_dreq",
    "spi_is_writable", "spi_is_readable", "spi_get_hw", "spi_get_const_hw", "spi_set_format",
    # pwm / gpio / flash
    "pwm_get_default_config", "pwm_config_set_clkdiv", "pwm_config_set_clkdiv_mode",
    "pwm_config_set_wrap", "pwm_init", "pwm_set_gpio_level", "pwm_set_enabled",
    "pwm_set_wrap", "pwm_set_clkdiv", "pwm_gpio_to_slice_num", "pwm_gpio_to_channel",
    "gpio_set_function", "gpio_init", "gpio_put", "gpio_get", "gpio_set_dir",
    "gpio_set_pulls", "gpio_pull_up", "gpio_pull_down", "gpio_is_pulled_up",   # hardware/gpio.h
    "flash_range_erase", "flash_range_program", "flash_get_unique_id",
    # прочее
    "add_repeating_timer_us", "add_repeating_timer_ms", "cancel_repeating_timer",
    "tight_loop_contents", "get_core_num", "hw_write_masked", "hw_set_bits", "hw_clear_bits",
    "reset_block", "unreset_block_wait", "sleep_ms", "sleep_us", "busy_wait_us_32",
    "xosc_dormant", "xosc_init", "xosc_disable",   # hardware/xosc.h
    "clock_get_hz", "clock_configure", "clock_get_pll_sys",   # hardware/clocks.h
    "adc_init", "adc_gpio_init", "adc_select_input", "adc_read",     # hardware/adc.h
    "adc_set_clkdiv", "adc_fifo_drain",
    "watchdog_update", "watchdog_enable", "watchdog_start_tick", "watchdog_get_count",
    "watchdog_reboot", "watchdog_caused_reboot", "watchdog_enable_caused_reboot",  # hardware/watchdog.h
}
SKIP_IDENT = {"spi0", "spi1", "dma_hw", "channel_config", "get_dreq", "watchdog_hw"}   # не функции/переменные SDK

FILES = ["gbemu/gbemu.ino", "gbemu/apu_gb.h"]


def main():
    bad = 0
    seen = set()
    for fn in FILES:
        if not os.path.exists(fn):
            print("[ОШИБКА] нет файла", fn)
            bad += 1
            continue
        for ln, line in enumerate(open(fn, encoding="utf-8", errors="replace"), 1):
            code = line.split("//")[0]
            m = re.match(r'\s*#\s*include\s+"([^"]+)"', code)
            if m:
                h = m.group(1)
                if h in HEADERS_OK or HEADER_PAT_OK.match(h):
                    continue
                if h.startswith(("hardware/", "pico/")):
                    if h not in seen:
                        seen.add(h)
                        print(f"[ОШИБКА] {fn}:{ln}: заголовка \"{h}\" нет в pico-sdk 1.5.1")
                        print(f"          (подключать его нельзя — сборка в Arduino IDE упадёт)")
                        bad += 1
            for sym, why in SYMBOLS_BAD.items():
                if sym in code:
                    print(f"[ОШИБКА] {fn}:{ln}: {sym} — {why}")
                    bad += 1
            # белый список: любое имя SDK-приставки должно быть известно
            for name in re.findall(r"\b([a-z_][a-z0-9_]*)\s*\(", code):
                if not name.startswith(SDK_PREFIXES):
                    continue
                if name in SKIP_IDENT or name in SYMBOLS_OK or name in SYMBOLS_BAD:
                    continue
                print(f"[ОШИБКА] {fn}:{ln}: «{name}()» — не знаю такого имени в pico-sdk 1.5.1")
                print(f"          (если это ваша функция — переименуйте, чтобы не путать с SDK)")
                bad += 1

    if bad:
        print(f"\nSDKCHECK: есть ошибки ({bad})")
        return 1
    print("SDKCHECK: чисто (все заголовки и функции есть в pico-sdk 1.5.1)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
