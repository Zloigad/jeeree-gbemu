// Заглушка pico-sdk (hardware/pwm.h): только объявления. Реализации — в
// tools/harness/harness.cpp, чтобы стенд видел, что скетч делает с ШИМ.
#pragma once
#include <stdint.h>

typedef unsigned int uint;

typedef enum {
  GPIO_FUNC_SIO = 5,
  GPIO_FUNC_PWM = 4
} gpio_function_t;

struct pwm_config { float clkdiv; uint16_t wrap; };

pwm_config pwm_get_default_config(void);
void pwm_config_set_clkdiv(pwm_config *c, float div);
void pwm_config_set_wrap(pwm_config *c, uint16_t wrap);
void pwm_init(uint slice_num, pwm_config *c, bool start);
void pwm_set_gpio_level(uint gpio, uint16_t level);
uint pwm_gpio_to_slice_num(uint gpio);
void gpio_set_function(uint gpio, gpio_function_t fn);
