// Заглушка pico-sdk (hardware/dma.h) — только то, что использует скетч.
// Имена и поля ровно такие же, как в настоящем SDK 1.5.1 (проверяются sdkcheck).
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// pico-sdk объявляет uint в pico/types.h; в заглушке достаточно этого
typedef unsigned int uint;

enum dma_channel_transfer_size { DMA_SIZE_8 = 0, DMA_SIZE_16 = 1, DMA_SIZE_32 = 2 };

#define NUM_DMA_CHANNELS 12

typedef struct {
  uint32_t ctrl;
} dma_channel_config;

// регистры одного канала (как в hardware/structs/dma.h)
typedef struct {
  uint32_t read_addr;
  uint32_t write_addr;
  uint32_t transfer_count;
  uint32_t ctrl_trig;
  uint32_t al1_ctrl;
} dma_channel_hw_t;

typedef struct {
  dma_channel_hw_t ch[NUM_DMA_CHANNELS];
  uint32_t _pad0[64];
  uint32_t intr, inte0, intf0, ints0, _pad1;
  uint32_t inte1, intf1, ints1;
  uint32_t abort;
} dma_hw_t;
extern dma_hw_t *dma_hw;

static inline dma_channel_config dma_channel_get_default_config(uint ch) {
  dma_channel_config c; c.ctrl = 0; (void)ch; return c;
}
static inline void channel_config_set_transfer_data_size(dma_channel_config *c, int size) { (void)c; (void)size; }
static inline void channel_config_set_read_increment(dma_channel_config *c, bool inc)    { (void)c; (void)inc; }
static inline void channel_config_set_write_increment(dma_channel_config *c, bool inc)   { (void)c; (void)inc; }
static inline void channel_config_set_dreq(dma_channel_config *c, int dreq)              { (void)c; (void)dreq; }
static inline void channel_config_set_irq_quiet(dma_channel_config *c, bool quiet)       { (void)c; (void)quiet; }

int  dma_claim_unused_channel(bool required);
void dma_channel_configure(uint ch, const dma_channel_config *c, volatile void *write,
                           const volatile void *read, uint count, bool trigger);
void dma_channel_set_irq1_enabled(uint ch, bool enabled);
bool dma_channel_is_busy(uint ch);
void dma_channel_abort(uint ch);
