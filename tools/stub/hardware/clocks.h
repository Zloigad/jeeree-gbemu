// Заглушка pico-sdk (hardware/clocks.h) — только то, что использует скетч.
#pragma once
#include <stdint.h>

typedef unsigned int uint;

enum clock_index {
  clk_gpout0, clk_gpout1, clk_gpout2, clk_gpout3,
  clk_ref, clk_sys, clk_peri, clk_usb, clk_adc, clk_rtc, CLK_COUNT
};

#define CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS 0

uint32_t clock_get_hz(enum clock_index clk_index);
void clock_configure(enum clock_index clk_index, uint32_t src, uint32_t auxsrc,
                     uint32_t src_freq, uint32_t freq);
