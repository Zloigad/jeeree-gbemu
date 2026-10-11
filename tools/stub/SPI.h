#pragma once
#include "Arduino.h"
struct SPISettings {
  uint32_t freq;
  SPISettings(uint32_t f, uint8_t, uint8_t) : freq(f) {}
};
struct SPIClassRP2040 {
  bool setRX(int); bool setCS(int); bool setSCK(int); bool setTX(int);
  void begin(); void begin(bool hwCS);
  void beginTransaction(SPISettings); void endTransaction();
  void transfer(const void *, void *, size_t);
  void transfer(void *, size_t);
  uint8_t transfer(uint8_t);
  bool transferAsync(const void *, void *, size_t);
  bool finishedAsync();
  void abortAsync();
};
extern SPIClassRP2040 SPI;
