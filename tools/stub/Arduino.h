// Заглушка для проверки синтаксиса скетчей (только компиляция, не запуск)
#pragma once
#include <stdint.h>
#include <stddef.h>
#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define INPUT 0
#define INPUT_PULLUP 2
#define HEX 16
#define MSBFIRST 1
#define SPI_MODE0 0
class __FlashStringHelper { public: const char *p; __FlashStringHelper(const char *s) : p(s) {} };
#define F(x) __FlashStringHelper(x)
unsigned long millis(void); unsigned long micros(void);
void delay(unsigned long); void delayMicroseconds(unsigned int);
void pinMode(uint8_t, uint8_t); void digitalWrite(uint8_t, uint8_t); int digitalRead(uint8_t);
void analogWrite(uint8_t, int); void analogWriteFreq(uint32_t);
struct SerialStub {
  void begin(unsigned long);
  void print(const char *); void println(const char * = "");
  void print(const __FlashStringHelper &); void println(const __FlashStringHelper &);
  void print(int, int = 10); void println(int, int = 10);
  void print(unsigned int, int = 10); void println(unsigned int, int = 10);
  void print(unsigned long, int = 10); void println(unsigned long, int = 10);
  void print(long, int = 10); void println(long, int = 10);
  void print(double, int = 2); void println(double, int = 2);
  void print(char); void flush();
  void write(const uint8_t *p, size_t n); void write(char c);
  int availableForWrite();
  int available(); int read();
  operator bool();
};
extern SerialStub Serial;
#ifndef F_CPU
#define F_CPU 133000000UL
#endif

// --- заглушки ядра arduino-pico (для проверки синтаксиса) ---
struct RpStub { void reboot(); void restart(); void rebootToBootloader(); };
extern RpStub rp2040;
inline void tight_loop_contents() {}
inline void __wfi() {}

inline int g_irqLevel = 0;               // v9.7.2: следим за балансом запрета прерываний
inline void noInterrupts() { g_irqLevel++; }
inline void interrupts() { if (g_irqLevel > 0) g_irqLevel--; }
