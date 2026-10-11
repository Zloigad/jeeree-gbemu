// Заглушка pico-sdk (hardware/adc.h) — только то, что использует скетч.
// В pico-sdk 1.5.1 это hardware/adc.h (функции adc_init / adc_gpio_init /
// adc_select_input / adc_read) — так читается датчик питания (канал 4 = GP29).
#pragma once
#include <stdint.h>
typedef unsigned int uint;      // как в pico/types.h (в SDK uint приходит из него)

void adc_init(void);
void adc_gpio_init(uint gpio);
void adc_select_input(uint input);
uint16_t adc_read(void);
void adc_set_clkdiv(float clkdiv);
void adc_fifo_drain(void);

// Регистры АЦП (в настоящем pico-sdk 1.5.1 — hardware/regs/adc.h + hardware/structs/adc.h,
// которые подключаются из hardware/adc.h). Нужны скетчу для НЕБЛОКИРУЮЩЕГО чтения:
// записали START_ONCE — и сразу читаем result (это предыдущий отсчёт). Так делает сток,
// и так прерывание звука не может залипнуть в ожидании готовности.
// Регистр управления. В настоящем SDK это volatile uint32_t; в заглушке — крошечный
// класс, чтобы стенд мог изобразить задержку АЦП на одно преобразование: запись
// START_ONCE означает «предыдущее преобразование закончилось, его отсчёт готов».
struct AdcCsReg {
    uint32_t v = 0;
    AdcCsReg &operator=(uint32_t x);          // реализация — в стенде (harness.cpp)
    AdcCsReg &operator|=(uint32_t x) { return *this = (v | x); }
    AdcCsReg &operator&=(uint32_t x) { return *this = (v & x); }
    operator uint32_t() const { return v; }
};
typedef struct {
    AdcCsReg cs;                 // управление: EN(0), START_ONCE(2), READY(8), AINSEL(12..14)
    volatile uint32_t result;    // отсчёт (младшие 12 бит)
    volatile uint32_t fcs, fifo, div;
} adc_hw_t;
extern adc_hw_t *adc_hw;
#define ADC_CS_EN_BITS          (1u << 0)
#define ADC_CS_START_ONCE_BITS  (1u << 2)
#define ADC_CS_READY_BITS       (1u << 8)
#define ADC_CS_AINSEL_LSB       12u
