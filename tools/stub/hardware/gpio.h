// Заглушка pico-sdk (hardware/gpio.h) для проверки скетча на компьютере.
#pragma once
#include <stdint.h>
#include "hardware/pwm.h"      // gpio_function_t / GPIO_FUNC_*

// Подтяжка вывода (есть в hardware/gpio.h SDK 1.5.1). Скетч включает подтяжку
// вверх на ключе питания, чтобы сброс не выключил консоль.
void gpio_set_pulls(uint gpio, bool up, bool down);
