// Заглушка pico-sdk (hardware/irq.h) — только то, что использует скетч.
#pragma once
#include <stdint.h>

// pico-sdk объявляет uint в pico/types.h; в заглушке достаточно этого
typedef unsigned int uint;
#include <stdbool.h>
// Номера прерываний RP2040 (hardware/regs/intctrl.h): DMA_IRQ_0=11, DMA_IRQ_1=12
#define DMA_IRQ_0 11
#define DMA_IRQ_1 12
typedef void (*irq_handler_t)(void);
#define PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY 0x80
void irq_set_exclusive_handler(uint num, irq_handler_t handler);
void irq_add_shared_handler(uint num, irq_handler_t handler, unsigned char order_priority);
#define TIMER_IRQ_0 0   // RP2040 (hardware/regs/intctrl.h)
void irq_set_enabled(uint num, bool enabled);
static inline void irq_set_priority(uint num, uint8_t prio) { (void)num; (void)prio; }
