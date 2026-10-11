// Заглушка pico-sdk (hardware/spi.h) — только то, что использует скетч.
// Имена/поля — как в настоящем pico-sdk 1.5.1 (hardware/structs/spi.h).
#pragma once
#include <stdint.h>
#include <stdbool.h>

// pico-sdk объявляет uint в pico/types.h; в заглушке достаточно этого
typedef unsigned int uint;

typedef struct spi_inst spi_inst_t;
extern spi_inst_t *const spi0;
extern spi_inst_t *const spi1;

// Регистры SPI (SSP/PL022). В железе FIFO 16-битная, и ОДНА запись в dr кладёт
// в неё ровно одну запись; при 8-битных кадрах сдвигаются младшие 8 бит — то есть
// одна запись = один байт в панель. Поэтому dr у нас тип с перегрузкой присваивания:
// так стенд видит каждый байт, ушедший в панель.
struct SpiDrReg {
  uint32_t v = 0;
  operator uint32_t() const { return v; }        // (void)spi_get_hw(spi0)->dr;
  SpiDrReg &operator=(uint32_t x);               // определяется в стенде
};
typedef struct {
  volatile uint32_t cr0;        // SSPCR0: DSS (биты 3:0) и SCR (биты 15:8)
  volatile uint32_t cr1;        // SSPCR1: SSE (бит 1) — включено
  SpiDrReg dr;                  // FIFO данных: запись = кадр в панель
  volatile uint32_t sr;         // SSPSR: TNF (бит 1), RNE (бит 2), BSY (бит 4)
  volatile uint32_t cpsr;       // тактовый делитель (чётный, 2..254)
  volatile uint32_t dmacr;      // TDMAE=1: SPI тянет данные из DMA
} spi_hw_t;

#define SPI_SSPSR_TNF_BITS 0x2u
#define SPI_SSPSR_RNE_BITS 0x4u
#define SPI_SSPSR_BSY_BITS 0x10u

spi_hw_t *spi_get_hw(spi_inst_t *spi);
uint spi_get_baudrate(const spi_inst_t *spi);

// есть в настоящем hardware/spi.h (SDK 1.5.1) — скетч ими пользуется
uint spi_get_dreq(spi_inst_t *spi, bool is_tx);
bool spi_is_busy(const spi_inst_t *spi);
bool spi_is_writable(const spi_inst_t *spi);   // есть место в FIFO
bool spi_is_readable(const spi_inst_t *spi);   // в FIFO есть принятый байт

// хук стенда: сюда попадает каждый байт, записанный прямо в FIFO
void spiHostByte(uint8_t b);
