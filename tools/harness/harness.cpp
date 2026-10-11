// Виртуальная панель для gbemu v3: прогоняем настоящий скетч на хосте и проверяем
// каждый байт, который уходит в SPI (включая поведение DMA: буфер не должен
// меняться, пока «передача идёт»).
// Сборка: g++ -std=gnu++17 -O1 -I tools/stub -I gbemu -I <фейковый rom.h> harness.cpp
#include "Arduino.h"
#include "hardware/adc.h"          // регистры АЦП (adc_hw) — нужны ловушке просадки
#include "SPI.h"
#include "hardware/flash.h"
#include "hardware/pwm.h"
#include "pico/time.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/spi.h"
#include "hardware/watchdog.h"
#include <cstdio>
#include <string>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>

// ---------------- заглушки Arduino ----------------
// Виртуальные часы: каждый вызов чуть-чуть двигает время. Так циклы ожидания
// (пейсинг кадров и т.п.) завершаются, а «зависший» код ловится счётчиком шагов.
static uint64_t g_nowUs = 100000;
int g_dmaAtFrameStart = 0;   // снимок счётчика запусков DMA (проверка «синхронный путь не трогает DMA»)
static uint64_t g_steps = 0;
static const uint64_t STEP_LIMIT = 40000000ULL;   // ~40 млн шагов = явное зависание
static void dmaTick(void);
static void stepGuard(void) {
  if (++g_steps > STEP_LIMIT) {
    printf("\n[FAIL] похоже на зависание: %llu шагов виртуального времени без прогресса\n",
           (unsigned long long)g_steps);
    fflush(stdout);
    _Exit(9);
  }
}
unsigned long millis(void) { g_nowUs += 1000; dmaTick(); stepGuard(); return (unsigned long)(g_nowUs / 1000); }
unsigned long micros(void) { g_nowUs += 100; dmaTick(); stepGuard(); return (unsigned long)g_nowUs; }
void delay(unsigned long ms) { g_nowUs += (uint64_t)ms * 1000ULL + 1000; dmaTick(); stepGuard(); }
void delayMicroseconds(unsigned int) {}
void pinMode(uint8_t, uint8_t) {}
static int g_dc = 0;
static int g_pinLevel[32];              // -1 = не трогали
// модель «ключ питания»: если GPIO1 отпущен в LOW, на устройстве питание снимается.
// Стенд это только фиксирует (сам себя выключить не может).
static bool     g_powerCut = false;
static uint32_t g_powerCutMs = 0;
static int      g_rstWrites = 0;        // сколько раз дёргали RST (переинициализация панели)
void digitalWrite(uint8_t pin, uint8_t v) {
  if (pin == 2) g_dc = v;
  if (pin < 32) g_pinLevel[pin] = v;
  // номера взяты из скетча (здесь ещё нельзя использовать его #define):
  // PIN_RST = 4, PIN_PWR_HOLD = 1
  if (pin == 4) g_rstWrites++;
  if (pin == 1 && v == LOW && !g_powerCut) { g_powerCut = true; g_powerCutMs = (uint32_t)(g_nowUs / 1000); }
}
// кнопки: битовая маска пинов, прижатых к земле (LOW = нажато). По умолчанию 0.
static uint32_t g_lowPins = 0;
int  digitalRead(uint8_t pin) { return (g_lowPins & (1u << pin)) ? LOW : HIGH; }
// двигаем виртуальное время вперёд (для проверки удержаний кнопок)
static void harnessAdvanceMs(uint32_t ms) { g_nowUs += (uint64_t)ms * 1000ULL + 1000; }
static uint64_t g_analogSlices = 0;               // как pwmInitted в arduino-pico
int g_blZeroWrites = 0;                            // v8.9q: сколько раз гасили подсветку
void analogWrite(uint8_t pin, int v) {
  g_analogSlices |= (1u << ((pin >> 1) & 7));
  if (pin == 7 && v == 0) g_blZeroWrites++;        // «тихая запись» гасит подсветку
}
static uint32_t g_analogFreq = 0;                 // v9.6: частота ШИМ подсветки
void analogWriteFreq(uint32_t f) { g_analogFreq = f; }
void xosc_dormant(void) {}
// --- эмулируем flash 2 МБ: erase -> 0xFF, program может только стирать биты ---
// важно: на устройстве flash начинается с ровного адреса 0x10000000, поэтому
// буфер стенда тоже выравниваем — иначе разъезжаются абсолютные/относительные
// вычисления адресов
alignas(4096) static uint8_t g_flash[2 * 1024 * 1024];
static int      g_flashErrors = 0;      // программирование без стирания (0->1) — так нельзя
static int      g_flashOps = 0;
static int      g_flashPageRewrite = 0;
static int      g_flashEraseMisaligned = 0;
static uint8_t  g_flashTouched[65536];
extern int g_flashPageRewrite;
static int g_flashErases = 0, g_flashPrograms = 0;      // отдельно: стирания и записи
static int g_flashUnprot = 0;      // v9.7.2: запись во flash с ВКЛЮЧЁННЫМИ прерываниями (так нельзя!)
void flash_range_erase(uint32_t off, size_t n) {
  g_flashErases++;
  if (g_irqLevel == 0) g_flashUnprot++;
  if (off + n > sizeof(g_flash)) { g_flashErrors++; return; }
  if ((off % 4096) || (n % 4096)) g_flashEraseMisaligned++;   // требование pico-sdk
  memset(g_flash + off, 0xFF, n);
  for (size_t i = 0; i < n; i += 256) { uint32_t pg = (off + i) / 256; if (pg < 65536) g_flashTouched[pg] = 0; }
  g_flashOps++;
}
// Требования pico-sdk: смещение кратно 256 и длина кратна 256. Нарушение —
// именно та ошибка, из-за которой сейвстейты не загружались (v6). Стенд это
// ловит двумя счётчиками: невыровненная запись и повторная запись страницы.
static int g_flashMisaligned = 0;       // невыровненное смещение/длина (запрещено pico-sdk)

void flash_range_program(uint32_t off, const uint8_t *data, size_t n) {
  g_flashPrograms++;
  if (g_irqLevel == 0) g_flashUnprot++;
  if (off + n > sizeof(g_flash)) { g_flashErrors++; return; }
  if ((off % 256) || (n % 256)) g_flashMisaligned++;
  for (size_t i = 0; i < n; i += 256) {
    const uint32_t page = (off + i) / 256;
    if (page < sizeof(g_flashTouched) && g_flashTouched[page]++ > 0) g_flashPageRewrite++;
  }
  for (size_t i = 0; i < n; i++) if ((g_flash[off + i] & data[i]) != data[i]) g_flashErrors++;
  for (size_t i = 0; i < n; i++) g_flash[off + i] &= data[i];
  g_flashOps++;
}
// ---------------- ШИМ (заглушка pico-sdk) ----------------
static uint   g_pwmSlice = 99;
static uint16_t g_pwmTop = 0;
static float  g_pwmDiv = 0;
static int    g_pwmInitCalls = 0;
static uint16_t g_pwmLevel = 0xFFFF;
static int    g_gpioFuncPin = -1, g_gpioFuncVal = -1;
static unsigned g_pwmLevelPin = 999;
static int    g_timerAdds = 0;

pwm_config pwm_get_default_config(void) { pwm_config c; c.clkdiv = 1.0f; c.wrap = 0xFFFF; return c; }
void pwm_config_set_clkdiv(pwm_config *c, float d) { c->clkdiv = d; }
void pwm_config_set_wrap(pwm_config *c, uint16_t w) { c->wrap = w; }
void pwm_init(uint slice, pwm_config *c, bool) { g_pwmSlice = slice; g_pwmTop = c->wrap; g_pwmDiv = c->clkdiv; g_pwmInitCalls++; }
void pwm_set_gpio_level(uint gpio, uint16_t level) { g_pwmLevelPin = gpio; g_pwmLevel = level; }
uint pwm_gpio_to_slice_num(uint gpio) { return (gpio >> 1) & 7; }
void gpio_set_function(uint gpio, gpio_function_t fn) { g_gpioFuncPin = (int)gpio; g_gpioFuncVal = (int)fn; }

// ---------------- таймер звука ----------------
static bool  (*g_timerCb)(repeating_timer_t *) = nullptr;
static int64_t g_timerUs = 0;
static repeating_timer_t g_timer;
bool add_repeating_timer_us(int64_t us, bool (*cb)(repeating_timer_t *), void *, repeating_timer_t *out) {
  g_timerCb = cb; g_timerUs = us; g_timerAdds++;
  if (out) { out->delay_us = us; g_timer = *out; }
  return true;
}
bool cancel_repeating_timer(repeating_timer_t *) { g_timerCb = nullptr; return true; }

// Проиграть n семплов звука так, как это делал бы таймер 22222 Гц.
static void pumpSamples(int n) {
  repeating_timer_t t{0};
  for (int i = 0; i < n && g_timerCb; i++) g_timerCb(&t);
}

// ---------------- SPI периферия (заглушка) ----------------
struct spi_inst { int dummy; };
static spi_inst s_spi0, s_spi1;
spi_inst_t *const spi0 = &s_spi0;
spi_inst_t *const spi1 = &s_spi1;

// --- часы как на устройстве (это и была причина «SPI 24.0 МГц») ---
// clk_sys = F_CPU, а clk_peri на устройстве оказался 48 МГц. SPI на RP2040 не может
// быть быстрее clk_peri/2 (минимальный делитель PL022), поэтому при clk_peri = 48 МГц
// ЛЮБАЯ просьба о частоте даёт ровно 24 МГц. Повторяем эту арифметику SDK здесь,
// чтобы стенд ловил такие вещи: он должен увидеть, что после поднятия clk_peri
// просьба 62,5 МГц стала давать именно 62,5.
static uint32_t g_clkSys  = 250000000u;
static uint32_t g_clkPeri = 48000000u;      // как было на устройстве
static uint32_t g_spiWant = 0, g_spiGot = 0;
uint32_t clock_get_hz(enum clock_index i) {
  if (i == clk_sys) return g_clkSys;
  if (i == clk_peri) return g_clkPeri;
  return 0;
}
void clock_configure(enum clock_index i, uint32_t, uint32_t, uint32_t src_freq, uint32_t) {
  if (i == clk_peri) g_clkPeri = src_freq;   // вывели clk_peri от clk_sys
}
static void spiHwSetRegs(uint32_t baud);          // определена ниже (после g_spiHw)
static uint32_t sdkBaud(uint32_t peri, uint32_t want);
static uint32_t sdkBaudProbe(uint32_t peri, uint32_t want) { return sdkBaud(peri, want); }
static uint32_t sdkBaud(uint32_t peri, uint32_t want) {
  uint32_t b = want > peri ? peri : want;
  uint32_t psc = peri / b + (peri % b ? 1u : 0u);
  if (psc < 2) psc = 2;
  if (psc & 1u) psc++;                        // PL022 требует чётный делитель
  if (psc > 254) psc = 254;
  return peri / psc;
}
static uint32_t g_spiBaud = 0;                // фактическая частота SPI на панели
uint spi_get_baudrate(const spi_inst_t *) { return g_spiBaud; }

SerialStub Serial;
// «Эхо» Serial: включается в тестах, чтобы увидеть РОВНО ту строку, которую
// пользователь видит на устройстве (иначе опечатки в форматировании не видны).
static bool g_serialEcho = false;
// вход для тестов: строка, которую «пришлют с клавиатуры» (нуль-терминатор)
static const char *g_serialIn = nullptr;
static int g_serialInPos = 0;
static void harnessSend(const char *s) { g_serialIn = s; g_serialInPos = 0; }
// перехват вывода: пишем в файл и отдаём строкой, чтобы проверять текст лога
static int  captureStart(void) {
  fflush(stdout);
  int saved = dup(fileno(stdout));
  if (!freopen("/tmp/harness_capture.txt", "w", stdout)) return saved;
  setvbuf(stdout, NULL, _IONBF, 0);
  return saved;
}
static std::string captureStop(int saved) {
  fflush(stdout);
  dup2(saved, fileno(stdout)); close(saved);
  std::string out; FILE *f = fopen("/tmp/harness_capture.txt", "r");
  if (f) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof(b), f)) > 0) out.append(b, n); fclose(f); }
  setvbuf(stdout, NULL, _IONBF, 0);
  return out;
}
static bool has(const std::string &s, const char *what) { return s.find(what) != std::string::npos; }
static void echoStr(const char *s) { if (g_serialEcho) fputs(s, stdout); }
static void echoNum(long v, int base) { if (g_serialEcho) { if (base == 16) printf("%lX", v); else printf("%ld", v); } }
void SerialStub::begin(unsigned long) {}
void SerialStub::print(const char *s) { echoStr(s); }
void SerialStub::println(const char *s) { echoStr(s); echoStr("\n"); }
void SerialStub::print(const __FlashStringHelper &s) { echoStr(s.p); }
void SerialStub::println(const __FlashStringHelper &s) { echoStr(s.p); echoStr("\n"); }
void SerialStub::print(int v, int b) { echoNum(v, b); }        void SerialStub::println(int v, int b) { echoNum(v, b); echoStr("\n"); }
void SerialStub::print(unsigned int v, int b) { echoNum(v, b); } void SerialStub::println(unsigned int v, int b) { echoNum(v, b); echoStr("\n"); }
void SerialStub::print(unsigned long v, int b) { echoNum((long)v, b); } void SerialStub::println(unsigned long v, int b) { echoNum((long)v, b); echoStr("\n"); }
void SerialStub::print(long v, int b) { echoNum(v, b); }       void SerialStub::println(long v, int b) { echoNum(v, b); echoStr("\n"); }
void SerialStub::print(double v, int d) { if (g_serialEcho) printf("%.*f", d, v); }
void SerialStub::println(double v, int d) { if (g_serialEcho) printf("%.*f\n", d, v); }
void SerialStub::print(char c) { if (g_serialEcho) putchar(c); }
void SerialStub::flush() { if (g_serialEcho) fflush(stdout); }
int  g_serialFree = 4096;                       // сколько байт «влезет» в USB (для теста переполнения)
int  g_serialWriteBytes = 0;
void SerialStub::write(const uint8_t *p, size_t n) {
  g_serialWriteBytes += (int)n;
  if (g_serialFree < (int)n) g_serialFree = 0; else g_serialFree -= (int)n;
  if (g_serialEcho) fwrite(p, 1, n, stdout);
}
void SerialStub::write(char c) { write((const uint8_t *)&c, 1); }
int  SerialStub::availableForWrite() { return g_serialFree; }
int SerialStub::available() { return (g_serialIn && g_serialIn[g_serialInPos]) ? 1 : 0; }
int SerialStub::read() { return (g_serialIn && g_serialIn[g_serialInPos]) ? (unsigned char)g_serialIn[g_serialInPos++] : -1; }
SerialStub::operator bool() { return true; }
RpStub rp2040;
void RpStub::reboot() {} void RpStub::restart() {} void RpStub::rebootToBootloader() {}
char __flash_binary_end = 0;        // как у линкера на устройстве

// ---------------- регистрирующий SPI («панель») ----------------
static std::vector<uint8_t> g_data;      // сырые пиксельные данные (для порядка байт)
static std::vector<uint8_t> g_cmdSeq;    // команды и параметры
static int g_lastCmd = -1;
static bool g_dcOk = true;
// ---- виртуальная панель ST7789 (v9.5): окна, курсор и кадр ----
// Кадровый дифф шлёт строки сериями со своими окнами — как настоящая панель,
// модель ведёт курсор внутри окна и запоминает кадр целиком (пропущенные
// строки остаются от прошлого кадра — ровно как на железе).
static std::vector<uint16_t> g_fb;      // кадр 240x240 «как на панели»
static int g_winX0 = 0, g_winX1 = 239, g_winY0 = 0, g_winY1 = 239;
static int g_curX = 0, g_curY = 0;
static int g_parLeft = 0, g_parN = 0;   // параметры команды (0x2A/0x2B): 4 байта
static int g_par[4];
static int g_pxHi = -1;                 // старший байт пикселя (big-endian)

static const uint8_t *inflight = nullptr;
static size_t inflightN = 0;
static std::vector<uint8_t> snapshot;
static int inflightPolls = 0;
static int dmaErrors = 0;

static void logByteDC(uint8_t b) {
  if (!g_dc) {
    g_lastCmd = b; g_cmdSeq.push_back(b); g_pxHi = -1;
    g_parLeft = (b == 0x2A || b == 0x2B) ? 4 : 0; g_parN = 0;
    return;
  }
  if (g_parLeft > 0) {
    g_par[g_parN++] = b; g_parLeft--; g_cmdSeq.push_back(b);
    if (g_parLeft == 0) {
      if (g_lastCmd == 0x2A) { g_winX0 = (g_par[0] << 8) | g_par[1]; g_winX1 = (g_par[2] << 8) | g_par[3]; g_curX = g_winX0; }
      if (g_lastCmd == 0x2B) { g_winY0 = (g_par[0] << 8) | g_par[1]; g_winY1 = (g_par[2] << 8) | g_par[3]; g_curY = g_winY0; }
    }
    return;
  }
  if (g_lastCmd == 0x2C) {
    g_data.push_back(b);
    if (g_pxHi < 0) { g_pxHi = b; return; }
    const uint16_t pxv = (uint16_t)((g_pxHi << 8) | b);
    g_pxHi = -1;
    if (g_curY >= 0 && g_curY < 240 && g_curX >= 0 && g_curX < 240) g_fb[g_curY * 240 + g_curX] = pxv;
    if (++g_curX > g_winX1) { g_curX = g_winX0; g_curY++; }
    return;
  }
  g_cmdSeq.push_back(b);
}

SPIClassRP2040 SPI;
// Правила arduino-pico 4.4.1 (SPI.cpp): допустимые пины для SPI0
//   RX {0,4,16,20}  CS {1,5,17,21}  SCK {2,6,18,22}  TX {3,7,19,23}
// Всё остальное -> panic() и НАМЕРТВОЕ зависание платы (чёрный экран).
// Именно на этом v7 и повис: SPI.setCS(16).
static const unsigned SPI0_RX = (1u<<0)|(1u<<4)|(1u<<16)|(1u<<20);
static const unsigned SPI0_CS = (1u<<1)|(1u<<5)|(1u<<17)|(1u<<21);
static const unsigned SPI0_SCK= (1u<<2)|(1u<<6)|(1u<<18)|(1u<<22);
static const unsigned SPI0_TX = (1u<<3)|(1u<<7)|(1u<<19)|(1u<<23);
static void spiPinOk(unsigned mask, const char *what, int pin) {
  if (pin < 0 || pin > 31 || !((1u << pin) & mask)) {
    printf("\n[FAIL] ПАНИКА как на устройстве: SPI0.%s не может быть пином %d "
           "(arduino-pico вызовет panic() и плата встанет)\n", what, pin);
    fflush(stdout);
    _Exit(8);
  }
}
bool SPIClassRP2040::setRX(int p) { spiPinOk(SPI0_RX, "RX", p); return true; }
bool SPIClassRP2040::setCS(int p) { spiPinOk(SPI0_CS, "CS", p); return true; }
bool SPIClassRP2040::setSCK(int p) { spiPinOk(SPI0_SCK, "SCK", p); return true; }
bool SPIClassRP2040::setTX(int p) { spiPinOk(SPI0_TX, "TX", p); return true; }
void SPIClassRP2040::begin() {} void SPIClassRP2040::begin(bool) {}
void SPIClassRP2040::beginTransaction(SPISettings s) {
  const uint32_t got = sdkBaud(g_clkPeri, s.freq);   // как в spi_init() из SDK
  // при смене частоты библиотека переинициализирует SPI (spi_deinit+spi_init)
  g_spiWant = s.freq; g_spiGot = got; g_spiBaud = got;
  // и прописывает делители в регистры — по ним скетч печатает «правду о шине»
  spiHwSetRegs(got);
}
void SPIClassRP2040::endTransaction() {}
void SPIClassRP2040::abortAsync() {}

bool SPIClassRP2040::transferAsync(const void *p, void *, size_t n) {
  inflight = (const uint8_t *)p; inflightN = n;
  snapshot.assign((const uint8_t *)p, (const uint8_t *)p + n);
  inflightPolls = 2;                      // «передача идёт» ещё две проверки finishedAsync()
  return true;
}
bool SPIClassRP2040::finishedAsync() {
  if (!inflight) return true;
  if (inflightPolls-- > 0) return false;
  if (memcmp(inflight, snapshot.data(), inflightN) != 0) {
    dmaErrors++;                          // буфер изменили, пока DMA его читала — так нельзя
    snapshot.assign(inflight, inflight + inflightN);
  }
  for (size_t i = 0; i < snapshot.size(); i++) logByteDC(snapshot[i]);   // v9.5: и DMA — в модель панели
  inflight = nullptr;
  return true;
}
void SPIClassRP2040::transfer(const void *p, void *, size_t n) {
  if (!g_dc) g_dcOk = false;
  for (size_t i = 0; i < n; i++) logByteDC(((const uint8_t *)p)[i]);
}
void SPIClassRP2040::transfer(void *, size_t) { std::abort(); }
uint8_t SPIClassRP2040::transfer(uint8_t b) { logByteDC(b); return 0; }

// ---------------- DMA (v8.2: канал без прерываний, готовность — по transfer_count) --------
// На устройстве канал читает буфер и сам пишет в FIFO SPI по DREQ, а «передача
// кончилась» видно по счётчику: transfer_count становится 0. В стенде передача
// завершается с ходом ВИРТУАЛЬНОГО времени: как только время дошло до «времени
// окончания», байты уезжают в панель и счётчик обнуляется. Так проверяется, что
// логика очереди не зависит от того, когда именно придёт завершение.
dma_hw_t *dma_hw = nullptr;
static dma_hw_t g_dmaHw;
static spi_hw_t g_spiHw;
spi_hw_t *spi_get_hw(spi_inst_t *spi) { (void)spi; return &g_spiHw; }
static uint64_t g_drBytes = 0;              // сколько байт ушло в панель ПРЯМЫМ путём
void spiHostByte(uint8_t b) { g_drBytes++; logByteDC(b); }
// ВАЖНО (это и есть грабли v8.5 на устройстве): FIFO у SPI на RP2040 16-битная,
// а ОДНА запись в регистр данных кладёт в FIFO РОВНО ОДНУ запись. При 8-битных
// кадрах сдвигаются младшие 8 бит. Поэтому запись 32-битным словом отправляет в
// панель ОДИН байт (младший) — именно так v8.5 потерял 3/4 картинки.
// Стенд повторяет это поведение, чтобы такая ошибка больше не прошла.
static uint32_t g_lastDr = 0;
SpiDrReg &SpiDrReg::operator=(uint32_t x) {
  v = x; g_lastDr = x;
  spiHostByte((uint8_t)(x & 0xFF));      // одна запись FIFO = один байт (при 8 битах на кадр)
  return *this;
}
// Делители в регистрах — как их выставил бы spi_init() из SDK
static void spiHwSetRegs(uint32_t baud) {
  uint32_t psc = baud ? (g_clkPeri / baud) : 2;
  if (psc < 2) psc = 2;
  if (psc & 1u) psc++;
  if (psc > 254) psc = 254;
  g_spiHw.cpsr = psc;
  g_spiHw.cr0  = 0;                  // SCR=0 -> делитель 1
  g_spiHw.cr1  = 2;                  // SSE=1
  g_spiHw.sr   = 0x2;                // TNF=1: в FIFO есть место
  g_spiHw.dmacr = 3;                 // как spi_init() в arduino-pico: TXDMAE|RXDMAE
}

// Управляемое поведение шины для тестов: «панель ещё занята», «FIFO залип» и
// «FIFO залип ТОЛЬКО НАЧИНАЯ С N-го обращения» (это и былa дыра предохранителя:
// если залипание начинается в середине строки, старый код ждал вечно).
static bool g_spiBusy = false, g_spiJam = false;
static long g_jamAfterCalls = -1;         // с какого обращения «FIFO занят»
static long g_jamCalls = 0;               // сколько раз спросили готовность
// Залипание длится ограниченно (2 млн обращений): так регрессия «предохранитель не
// сработал» в стенде даёт ОШИБКУ, а не вечное зависание самого стенда.
#define JAM_SPAN 2000000L
static void harnessJamAfter(long n) { g_jamAfterCalls = n; g_jamCalls = 0; }
bool spi_is_writable(const spi_inst_t *) {
  if (g_spiJam) return false;
  if (g_jamAfterCalls >= 0) {
    const long c = ++g_jamCalls;
    if (c > g_jamAfterCalls && c < g_jamAfterCalls + JAM_SPAN) return false;
  }
  return true;
}

// --- поддельный АЦП: датчик питания (v8.9) ---
// 5 каналов: 0..3 = GP26..GP29, 4 = внутренний термодатчик (как в RP2040)
static uint16_t g_adcCh[5] = { 1450, 0, 0, 0, 880 };
static void adcSetCh(int ch, uint16_t v) { if (ch >= 0 && ch < 5) g_adcCh[ch] = v; }
static uint16_t adcSetAll(uint16_t v) { for (int i = 0; i < 5; i++) g_adcCh[i] = v; return v; }  // для тестов
static int      g_adcReads = 0, g_adcInits = 0, g_adcSel = -1;
// Регистры АЦП: адрес пишется-читается как на живой железяке. Неблокирующее чтение
// (записали START_ONCE — сразу читаем result) стенд изображает мгновенно готовым.
static adc_hw_t g_adcHwRegs;
adc_hw_t *adc_hw = &g_adcHwRegs;
static int      g_adcStarts = 0;                 // сколько раз запускали преобразование
static int      g_adcConvCh = 4;                 // какой канал преобразуется сейчас
static uint16_t adcChanVal(int ch) { return g_adcCh[(ch < 0 || ch > 4) ? 0 : ch]; }
// Модель АЦП: пока преобразование идёт, в result лежит ПРЕДЫДУЩИЙ отсчёт.
// Именно поэтому «запустил и сразу прочитал» в прерывании безопасно: значение отстаёт
// на одно преобразование (0,36 мс при нашем темпе), но чтение никогда не ждёт.
AdcCsReg &AdcCsReg::operator=(uint32_t x) {
  if (x & ADC_CS_START_ONCE_BITS) {
    g_adcHwRegs.result = adcChanVal(g_adcConvCh);   // закончилось предыдущее
    g_adcConvCh = (int)((x >> ADC_CS_AINSEL_LSB) & 7u);
    g_adcStarts++;
  }
  v = x | ADC_CS_READY_BITS;
  return *this;
}
static int      g_adcPins[4] = {0,0,0,0}; static int g_adcPinN = 0;
void adc_init(void) { g_adcInits++; }
void adc_gpio_init(uint gpio) { if (g_adcPinN < 4) g_adcPins[g_adcPinN++] = (int)gpio; }
void adc_select_input(uint input) {
  g_adcSel = (int)input;
  g_adcHwRegs.cs = (g_adcHwRegs.cs & ~(7u << ADC_CS_AINSEL_LSB)) | ((uint32_t)input << ADC_CS_AINSEL_LSB);
}
uint16_t adc_read(void) {           // как в pico-sdk: START_ONCE, ЖДЁМ готовности, читаем
  g_adcReads++;
  g_adcHwRegs.cs |= ADC_CS_START_ONCE_BITS | ADC_CS_EN_BITS;
  const uint16_t v = adcChanVal(g_adcSel);        // подождали — отсчёт текущего канала
  g_adcHwRegs.result = v;
  return v;
}
void adc_set_clkdiv(float) {}
void adc_fifo_drain(void) {}

// --- «обратный отсчёт» сторожа: сколько микросекунд оставалось бы до сброса ---
uint32_t watchdog_get_count(void) {
  const uint32_t c = 3900000u - (uint32_t)(g_nowUs / 8u);
  return (c > 3900000u) ? 1000u : c;         // не переполняемся: «время ушло далеко» считаем малым остатком
}

// --- «регистры ядра» для стенда: векторная таблица и регистры сбоя (v8.8) ---
uint32_t gHostVec[64];                            // подставная векторная таблица
volatile uint32_t gHostVtor = 0;                  // «SCB->VTOR» (куда записали)
static const uint32_t *g_hostVtorPtr = gHostVec;  // сама таблица (указатель, не 32 бита)
const uint32_t *gHostVtorSrc(void) { return g_hostVtorPtr; }
void gHostVtorSet(const uint32_t *tab) { g_hostVtorPtr = tab; gHostVtor = 1; }   // 1 = «переставили»
volatile uint32_t gHostCfsr = 0, gHostHfsr = 0, gHostMmfar = 0, gHostBfar = 0;
volatile uint32_t gHostDfsr = 0, gHostAfsr = 0;   // v8.9d: регистры debug/внешнего сбоя
volatile uint32_t gHostChipReset = (1u << 8);     // v8.9f: «последний сброс от схемы питания»
static bool g_pullUpOnPwr = false;                          // подтяжка на ключе питания включена?
void gpio_set_pulls(uint gpio, bool up, bool) { if (gpio == 1 && up) g_pullUpOnPwr = true; }
bool spi_is_readable(const spi_inst_t *) { return false; }
uint spi_get_dreq(spi_inst_t *, bool) { return 16; }        // DREQ_SPI0_TX на устройстве
bool spi_is_busy(const spi_inst_t *) { return g_spiBusy; }   // в тестах можно «задержать» панель

int  g_dmaStarts = 0;                 // сколько раз запускали передачу
int  g_dmaBadStart = 0;               // запуск без адреса/длины или без TDMAE — так нельзя
int  g_dmaTrigOk = 0;                 // запусков именно через trigger=true (как в доках SDK)
int  g_dmaAborts = 0;                 // сбросов канала (предохранитель DMA сработал)
unsigned long g_dmaBytes = 0;         // сколько байт ушло «в панель»
static bool g_dmaStall = false;       // «канал завис»: завершение не приходит никогда
static bool g_dmaAsync = false;       // завершение приходит не сразу, а с ходом времени
static uint64_t g_dmaDoneAt = 0;      // когда завершится текущая передача
static std::vector<uint8_t> g_dmaSnap;   // снимок буфера: если в полёте его изменили — ошибка
static bool g_dmaSnapOk = false;
static int   g_dmaPendingCh = -1;
static const uint8_t *g_dmaPendingRead = nullptr;
static size_t g_dmaPendingCount = 0;

int dma_claim_unused_channel(bool) { return 3; }            // канал 3 свободен

static bool g_irqEnabled[32] = {};
static void dmaComplete(void) {                             // «байты уехали в панель»
  if (g_dmaPendingCh < 0) return;
  if (g_dmaPendingRead && g_dmaPendingCount) {
    // Буфер изменили, пока передача шла? Значит двойная буферизация сломана —
    // так в панель уехали бы «перемешанные» строки. Считаем это ошибкой.
    if (g_dmaSnapOk && g_dmaSnap.size() == g_dmaPendingCount &&
        memcmp(g_dmaSnap.data(), g_dmaPendingRead, g_dmaPendingCount) != 0) dmaErrors++;
    for (size_t i = 0; i < g_dmaPendingCount; i++) logByteDC(g_dmaPendingRead[i]);
    g_drBytes += g_dmaPendingCount;                         // байты реально ушли в DR (счётчик шины)
    g_dmaBytes += (unsigned long)g_dmaPendingCount;
    g_dmaStarts++;
  }
  g_dmaHw.ch[g_dmaPendingCh].transfer_count = 0;             // счётчик обнулился
  g_dmaPendingCh = -1; g_dmaPendingRead = nullptr; g_dmaPendingCount = 0;
}
static void dmaTick(void) {                                 // вызывается с ходом времени
  if (g_dmaPendingCh < 0) return;
  if (g_dmaStall) return;                                   // завис — завершения не будет
  if (g_spiJam) return;                                     // FIFO панели встал -> DREQ не приходит, DMA не кончается
  if (g_dmaAsync && g_nowUs < g_dmaDoneAt) return;
  dmaComplete();
}
static void dmaStart(uint ch, const void *read, size_t count, bool trigger) {
  if (!read || !count) { g_dmaBadStart++; return; }
  if ((g_spiHw.dmacr & 1u) == 0) g_dmaBadStart++;           // TXDMAE не выставлен: шина не тянет данные
  g_irqEnabled[0] = false;
  g_dmaPendingCh = (int)ch;
  g_dmaPendingRead = (const uint8_t *)read;
  g_dmaPendingCount = count;
  g_dmaDoneAt = g_nowUs + (g_dmaAsync ? 400 : 0);           // 400 мкс виртуального времени
  g_dmaHw.ch[ch].transfer_count = (uint32_t)count;          // счётчик выставлен, идёт передача
  g_dmaSnap.assign(g_dmaPendingRead, g_dmaPendingRead + count); g_dmaSnapOk = true;
  if (trigger) g_dmaTrigOk++;
  dmaTick();
}
void dma_channel_configure(uint ch, const dma_channel_config *c, volatile void *write,
                           const volatile void *read, uint count, bool trigger) {
  (void)c; (void)write;
  dmaStart(ch, (const void *)read, count, trigger);
}
bool dma_channel_is_busy(uint ch) {
  dmaTick();                                   // время может завершить передачу
  return g_dmaPendingCh == (int)ch;
}

// Сброс канала (pico-sdk: dma_channel_abort). Скетч делает его, если передача «встала».
void dma_channel_abort(uint ch) {
  if (g_dmaPendingCh == (int)ch) {
    g_dmaAborts++;
    g_dmaHw.ch[ch].transfer_count = 0;
    g_dmaPendingCh = -1; g_dmaPendingRead = nullptr; g_dmaPendingCount = 0;
  }
}

// ---------------- сам скетч ----------------
// ВАЖНО: скетч делает #include "rom.h" и первым делом ищет его в СВОЕЙ папке, где
// лежит заглушка с ROM_SIZE = 0. С ней setup() выходит раньше инициализации звука,
// и загрузку устройства в стенде не проверить. Поэтому закрываем заглушку её же
// стражем и подсовываем тестовый ROM.
#define ROM_H
#include "rom_fake.h"
#include "gbemu.ino"

// dma_hw в скетче — указатель (в настоящем pico-sdk это адрес регистров)
struct dma_hw_init_t { dma_hw_init_t() { dma_hw = &g_dmaHw; } } g_dmaInit;

// ---------------- проверки ----------------
static int fails = 0;
static void chk(bool ok, const char *what) {
  printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what);
  if (!ok) fails++;
}

// Отметка в ящик + выгрузка (как при выключении): с v8.9k обычная blkWrite() во flash
// НЕ пишет — она копит отметки в ОЗУ, а во flash их отправляет выгрузка.
static void blkWriteFlash(void) { blkWrite(); blkFlush(true); }

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  // Векторная таблица «ядра»: заводим её ДО setup() — там ставится ловушка сбоя
  for (int i = 0; i < 64; i++) gHostVec[i] = 0x1000u + 4u * (unsigned)i;   // «старые» векторы
  gHostVtor = 0; g_hostVtorPtr = gHostVec;
  // ручная инициализация того, что setup() делает для рисования
  rebuildPalette();
  g_dmaAtFrameStart = 0;

  const uint8_t BGP = 0xE4;
  gb.hram_io[0x47] = BGP; gb.hram_io[0x48] = BGP; gb.hram_io[0x49] = BGP;

  // ---------- дефолты v9.3/v9.4 (проверяем ПЕРВЫМИ: дальше тесты меняют настройки) ----------
  chk(bright == 100, "дефолт: яркость 100 %");
  chk(contr == 120, "дефолт: контраст 120 % (v9.3: было 140 — многовато)");
  chk(volPct == 10, "дефолт: громкость 10 % (v8.9u: тихий старт)");
  chk(!strcmp(PALS[palIdx].name, "BLUE"), "дефолт: палитра BLUE (v9.3: в цвет корпуса)");
  // v9.7: настройки переживают выключение — частота панели тоже. Стартуем на
  // самой быстрой (125 МГц = ровные 60 кадр/с, как сток); 62,5 — первое нажатие
  // 'o', и теперь этот выбор ПОМНИТСЯ (раньше индекс стартовал с 62,5, но так и
  // не применялся до первого нажатия — реальный старт всегда был 125 МГц)
  chk(spiHzIdx == 0, "дефолт: панель на 125 МГц (ровные 60 кадр/с, как сток)");

  static uint8_t px[144][160];
  for (int L = 0; L < 144; L++)
    for (int x = 0; x < 160; x++)
      px[L][x] = (uint8_t)(((L * 7 + x * 3) & 3) | 0x20);   // 0x20 = фон

  // ---------- кадр игры ----------
  g_data.clear(); g_cmdSeq.clear(); g_lastCmd = -1;
  rowsValid = false;                 // холодные хэши: первый кадр обязан уйти целиком
  g_fb.assign(240u * 240u, 0xFFFFu); // модель панели: пусто
  frameStart();
  rowBl(GAME_TOP);
  for (int L = 0; L < 144; L++) cb_lcd_line(&gb, px[L], L);
  rowBl(MEM_H - (GAME_TOP + GAME_ROWS));
  frameFinish();

  const size_t expect = 240UL * 240UL * 2UL;
  printf("\nигра: отправлено %zu байт (ожидалось %zu)\n", g_data.size(), expect);
  chk(g_data.size() == expect, "ровно 240 строк x 240 точек x 2 байта");
  chk(g_dcOk, "DC был HIGH на всех данных");
  chk(dmaErrors == 0, "буферы DMA не изменялись, пока передача шла (двойная буферизация)");

  const uint8_t want[] = {0x2A,0,0,0,0xEF, 0x2B,0,0,0,0xEF, 0x2C};
  bool win = g_cmdSeq.size() >= sizeof(want);
  if (win) for (size_t i = 0; i < sizeof(want); i++) if (g_cmdSeq[i] != want[i]) win = false;
  chk(win, "окно = весь экран 0..239 по обеим осям");

  if (g_data.size() == expect) {
    // независимая модель содержимого кадра
    std::vector<uint16_t> model[216];
    for (int d = 0; d < 216; d++) model[d].resize(240);
    std::vector<int> cover(216, 0);
    for (int L = 0; L < 144; L++) {
      int y0 = (int)floor(L * 216.0 / 144.0), y1 = (int)floor((L + 1) * 216.0 / 144.0) - 1;
      for (int d = y0; d <= y1; d++) {
        cover[d]++;
        for (int x = 0; x < 240; x++) {
          int sx = (int)floor(x * 160.0 / 240.0);
          model[d][x] = curPal[(BGP >> (2 * (px[L][sx] & 3))) & 3];
        }
      }
    }
    bool cov = true; for (int d = 0; d < 216; d++) if (cover[d] != 1) cov = false;
    chk(cov, "каждая из 216 строк кадра нарисована ровно один раз");

    bool strip = true, content = true, order = true;
    int shown = 0;
    for (int r = 0; r < 240 && (strip || content || order); r++)
      for (int x = 0; x < 240; x++) {
        uint16_t got = (uint16_t)((g_data[(r * 240 + x) * 2] << 8) | g_data[(r * 240 + x) * 2 + 1]);
        if (r < 12 || r >= 228) { if (got != 0) strip = false; }
        else {
          uint16_t c = model[r - 12][x];
          if (got != c) { content = false;
            if (shown < 6) { printf("   ! строка %d x=%d: получено 0x%04X, модель 0x%04X (байты %02X %02X)\n",
                r, x, got, c, g_data[(r*240+x)*2], g_data[(r*240+x)*2+1]); shown++; } }
          // сырые байты должны идти старшим-вперёд: b0 = c>>8, b1 = c&0xFF
          if (g_data[(r * 240 + x) * 2] != (uint8_t)(c >> 8) ||
              g_data[(r * 240 + x) * 2 + 1] != (uint8_t)(c & 0xFF)) order = false;
        }
      }
    chk(strip,   "верхние 12 и нижние 12 строк — чёрные (полосы шума вылечены)");
    chk(content, "пиксели совпали с независимой моделью (масштаб 1.5x и палитра)");
    chk(order,   "порядок байт верный (big-endian, как ждёт ST7789)");
  }

  // ---------- v9.5: кадровый дифф — неизменившиеся строки не шлём ----------
  {
    // (а) ТОТ ЖЕ кадр ещё раз: в панель не уходит ни байта (строки те же)
    g_data.clear();
    frameStart(); rowBl(GAME_TOP);
    for (int L = 0; L < 144; L++) cb_lcd_line(&gb, px[L], L);
    rowBl(MEM_H - (GAME_TOP + GAME_ROWS));
    frameFinish();
    printf("   диag: повтор того же кадра: ушло %zu байт (ожидалось 0), строк %lu, проп %lu\n",
           g_data.size(), (unsigned long)frameRows, (unsigned long)frameSkip);
    chk(g_data.size() == 0, "v9.5: неизменившийся кадр не шлёт НИ БАЙТА (трафик шины = ноль)");
    chk(frameRows == MEM_H && frameSkip == MEM_H,
        "v9.5: все 240 строк кадра учтены и все пропущены");

    // (б) изменили ОДНУ строку игры — уходит только её строка панели
    const int y0m = (int)floor(50 * 216.0 / 144.0);
    const uint16_t nbBefore = g_fb[(GAME_TOP + y0m + 6) * 240 + 15];   // соседняя строка до правки
    px[50][10] ^= 1;
    g_data.clear();
    frameStart(); rowBl(GAME_TOP);
    for (int L = 0; L < 144; L++) cb_lcd_line(&gb, px[L], L);
    rowBl(MEM_H - (GAME_TOP + GAME_ROWS));
    frameFinish();
    const int y0 = (int)floor(50 * 216.0 / 144.0), y1 = (int)floor(51 * 216.0 / 144.0) - 1;
    const size_t wantBytes = (size_t)(y1 - y0 + 1) * 480;
    printf("   диag: одна строка игры изменилась: ушло %zu байт (ожидалось %zu), проп %lu\n",
           g_data.size(), wantBytes, (unsigned long)frameSkip);
    chk(g_data.size() == wantBytes, "v9.5: ушли ТОЛЬКО строки панели изменившейся строки игры");
    chk(frameRows == MEM_H && frameSkip == MEM_H - (uint32_t)(y1 - y0 + 1),
        "v9.5: учтено 240 строк, пропущены все кроме изменившейся группы");
    // модель панели: строка обновилась новым цветом, соседняя осталась прежней
    const uint16_t wantPx = curPal[(BGP >> (2 * (px[50][10] & 3))) & 3];
    const int yChk = GAME_TOP + y0;
    uint16_t gotPx = g_fb[yChk * 240 + 15];    // sx=10 при масштабе 1.5x -> x=15
    printf("   диag: пиксель после правки: 0x%04X (модель 0x%04X)\n", gotPx, wantPx);
    chk(gotPx == wantPx, "v9.5: изменившаяся строка в модели панели действительно обновилась");
    chk(g_fb[(yChk + 6) * 240 + 15] == nbBefore,
        "v9.5: соседние строки не тронуты (кадр в панели сохраняется)");
    px[50][10] ^= 1;   // вернуть как было
  }

  // ---------- главное: палитра игры не должна применяться дважды ----------
  // имитируем строку от ядра: пиксель = готовый оттенок + метка слоя
  {
    g_data.clear();
    rowsValid = false;                 // полный кадр: проверяем сырые байты по индексам
    uint8_t line160[160];
    // OBJ0: оттенки 0,1,2,3 так, как их отдаёт ядро (уже по палитре игры)
    for (int x = 0; x < 160; x++) line160[x] = (uint8_t)((x % 4) | 0x00);
    gb.direct.frame_skip = false;
    frameStart(); rowBl(GAME_TOP);
    cb_lcd_line(&gb, line160, 0);
    for (int L = 1; L < 144; L++) cb_lcd_line(&gb, line160, L);
    rowBl(MEM_H - (GAME_TOP + GAME_ROWS)); frameFinish();
    // в строке 12 кадра: пиксели чередуют оттенки 0,1,2,3 -> цвета должны совпасть с curPal
    bool ok1 = true;
    for (int i = 0; i < 4; i++) {
      // горизонталь 1.5x: чётные пиксели ядра занимают по две точки,
      // поэтому первая точка пикселя i стоит на x = i + (i+1)/2
      const int px = i + (i + 1) / 2;
      const uint8_t b0 = g_data[(12 * 240 + px) * 2], b1 = g_data[(12 * 240 + px) * 2 + 1];
      if (b0 != (uint8_t)(curPal[i] >> 8) || b1 != (uint8_t)(curPal[i] & 0xFF)) {
        ok1 = false;
        printf("   ! оттенок %d: байты %02X %02X, ожидалось %02X %02X\n",
               i, b0, b1, curPal[i] >> 8, curPal[i] & 0xFF);
      }
    }
    chk(ok1, "оттенок из ядра -> ровно тот же цвет палитры (нет двойного применения)");
  }

  // ---------- кадр интерфейса ----------
  g_data.clear(); g_cmdSeq.clear(); g_lastCmd = -1;
  rowsValid = false;                 // полный кадр: проверяем сырые байты по индексам
  uiClear(0);
  uiRect(10, 20, 30, 10, 3);          // тёмный прямоугольник
  uiTextCentered(100, 4, "TEST", 2);
  uiPush();
  chk(g_data.size() == expect, "интерфейс: тоже 240 строк по 240 точек");
  if (g_data.size() == expect) {
    uint16_t p = (uint16_t)((g_data[(25 * 240 + 15) * 2] << 8) | g_data[(25 * 240 + 15) * 2 + 1]);
    chk(p == curPal[3], "цвет прямоугольника = 4-й оттенок палитры (и байты в порядке)");
    chk(g_data[(25*240+15)*2] == (uint8_t)(curPal[3] >> 8) && g_data[(25*240+15)*2+1] == (uint8_t)(curPal[3] & 0xFF),
        "интерфейс: старший байт идёт первым");
  }

  // ---------- палитра: яркость и контраст должны РЕАЛЬНО менять цвета ----------
  auto lum = [](uint16_t c) {
    double r = ((c >> 11) & 31) * 255.0 / 31, g = ((c >> 5) & 63) * 255.0 / 63, b = (c & 31) * 255.0 / 31;
    return 0.299 * r + 0.587 * g + 0.114 * b;
  };
  palIdx = 0; bright = 100; contr = 100; invertShades = false; rebuildPalette();
  const double y0 = lum(curPal[0]);
  bright = 80; rebuildPalette();
  const double y80 = lum(curPal[0]);
  // v9.7: палитра BLUE теперь тёмная (под корпус), поэтому пороги ОТНОСИТЕЛЬНЫЕ:
  // «ручка» обязана реально работать при любой яркости палитры
  chk(y0 - y80 > 0.15 * y0, "яркость 80% заметно опускает светлый оттенок (было 100%)");
  bright = 100; rebuildPalette();

  const double spread0 = lum(curPal[0]) - lum(curPal[3]);
  contr = 150; rebuildPalette();
  const double y150 = lum(curPal[0]);
  const double spread1 = lum(curPal[0]) - lum(curPal[3]);
  const double dY = (y150 > y0) ? (y150 - y0) : (y0 - y150);
  printf("   диаг: палитра: светлый %.1f -> (80%%) %.1f; при контрасте 150%%: %.1f; разброс %.1f -> %.1f\n",
         y0, y80, y150, spread0, spread1);
  chk(dY > 0.10 * y0, "контраст 150% реально меняет оттенки (светлый заметно сдвигается)");
  contr = 100; rebuildPalette();
  chk((int)lum(curPal[0]) == (int)y0, "возврат настроек возвращает исходный цвет");

  invertShades = true; rebuildPalette();
  chk(curPal[0] == to565(PALS[0].c[3][0], PALS[0].c[3][1], PALS[0].c[3][2]), "инверсия оттенков работает");
  invertShades = false; rebuildPalette();

  // ---------- звук: ШИМ на GP17 + APU через хук peanut ----------
  soundOn = 1; volPct = 70; soundPause = 0;
  soundInit();
  chk(g_pwmSlice == 0, "звук: ШИМ-слайс 0 (это и есть GP17)");
  chk(g_pwmTop == APU_PWM_TOP, "звук: TOP = 689 — ровно как в стоковой прошивке");
  chk(g_gpioFuncPin == PIN_AUDIO && g_gpioFuncVal == GPIO_FUNC_PWM, "звук: GP17 переключён в режим ШИМ");
  chk(g_pwmLevel == APU_PWM_IDLE, "звук: в покое на выходе 344 (середина = тишина)");

  audio_write(0xFF24, 0x77); audio_write(0xFF25, 0xFF);
  audio_write(0xFF12, 0xF0); audio_write(0xFF11, 0x80);
  audio_write(0xFF13, 0xD6); audio_write(0xFF14, 0x86);   // канал 1, 440 Гц
  audio_write(0xFF21, 0xF0); audio_write(0xFF22, 0x04);
  audio_write(0xFF23, 0x80);                              // и шумовой канал
  chk(apu.sq[0].on && apu.ns.on, "звук: регистры дошли до APU (триггеры сработали)");

  soundPause = 0;
  int lmin = 100000, lmax = -1;
  for (int i = 0; i < 4000; i++) {
    if (g_timerCb) g_timerCb(&g_timer);
    if ((int)g_pwmLevel < lmin) lmin = (int)g_pwmLevel;
    if ((int)g_pwmLevel > lmax) lmax = (int)g_pwmLevel;
  }
  printf("   звук: уровень ШИМ гуляет %d..%d (тишина 344)\n", lmin, lmax);
  chk(g_timerCb && g_timerUs == -45, "звук: таймер 45 мкс = 22222 Гц");
  chk(lmin < APU_PWM_IDLE - 50 && lmax > APU_PWM_IDLE + 50, "звук: сигнал уходит в обе стороны от тишины");
  chk(lmin >= 16 && lmax <= APU_PWM_TOP - 16, "звук: уровни не упираются в края ШИМ");

  volPct = 35;                                              // громкость вдвое меньше
  int hmin = 100000, hmax = -1;
  for (int i = 0; i < 4000; i++) {
    if (g_timerCb) g_timerCb(&g_timer);
    if ((int)g_pwmLevel < hmin) hmin = (int)g_pwmLevel;
    if ((int)g_pwmLevel > hmax) hmax = (int)g_pwmLevel;
  }
  printf("   звук: при 35%% громкости %d..%d\n", hmin, hmax);
  chk((hmax - APU_PWM_IDLE) < (lmax - APU_PWM_IDLE) * 6 / 10, "звук: громкость реально уменьшает амплитуду");
  volPct = 70;

  // v9.6: ограничитель пиков — ток усилителя (мерцание подсветки) пропорционален размаху
  chk(limSwing(0) == 0 && limSwing(50) == 50 && limSwing(-50) == -50,
      "ограничитель: обычные уровни проходят без изменений");
  chk(limSwing(280) == 120 + 160 / 3 && limSwing(-280) == -(120 + 160 / 3),
      "ограничитель: пики сжимаются мягко (3:1 выше 120 уровней ШИМ)");
  printf("   диag: ограничитель: размах 280 -> %d, 200 -> %d\n",
         (int)limSwing(280), (int)limSwing(200));
  chk(limSwing(280) < 200, "ограничитель: полный размах прижат — «дыхание» питания меньше");

  soundPause = 1; if (g_timerCb) g_timerCb(&g_timer);
  chk(g_pwmLevel == APU_PWM_IDLE, "звук: в меню/на паузе молчит (нет залипшей ноты)");
  soundPause = 0;

  soundSetOn(1);
  // v9.4: проба пользователя: AMP=HIGH = МОЛЧАЩИЙ режим (звука нет вообще), поэтому
  // рабочий режим только LOW и выбора AMP в меню нет.
  digitalWrite(PIN_AMP, HIGH); ampApply();
  chk(g_pinLevel[PIN_AMP] == LOW,
      "усилитель: AMP=HIGH (глухо) — возвращает GP20 в 0 (LOW = единственный рабочий режим)");
  { // v9.4: гейт по тишине: короткая пауза — не трогаем (это между нотами, без щелчков),
    // 2 с математической тишины — глушим усилитель, любой звук — включаем сразу.
    apuReset();                                            // чистый APU = тишина
    soundPause = 0; soundOn = 1; ampApply();
    gAmpOn = 1; g_pinLevel[PIN_AMP] = LOW; gAmpSil = 0;    // как будто только что играло
    for (uint32_t i = 0; i < AMP_MUTE_TICKS / 10; i++) g_timerCb(&g_timer);   // ~0,2 с
    printf("   диag: усилитель: после 0,2 с тишины GP20=%d (ждали 0=LOW)\n", g_pinLevel[PIN_AMP]);
    chk(g_pinLevel[PIN_AMP] == LOW,
        "усилитель: пауза 0,2 с — GP20 остаётся 0 (пауза между нотами без щелчков)");
    for (uint32_t i = 0; i < AMP_MUTE_TICKS + 10; i++) g_timerCb(&g_timer);   // ещё >2 с
    printf("   диag: усилитель: после 2 с тишины GP20=%d (ждали 1=HIGH)\n", g_pinLevel[PIN_AMP]);
    chk(g_pinLevel[PIN_AMP] == HIGH,
        "усилитель: 2 с математической тишины — GP20 = 1 (усилитель глушится: гул/журчание не накапливаются)");
    // (2) звук вернулся: GP20 опускается сразу, без ожидания (нота — как в тесте APU выше)
    audio_write(0xFF24, 0x77); audio_write(0xFF25, 0xFF);
    audio_write(0xFF12, 0xF0); audio_write(0xFF11, 0x80);
    audio_write(0xFF13, 0xD6); audio_write(0xFF14, 0x86);  // канал 1: нота 440 Гц
    bool low = false;
    for (int i = 0; i < 200 && !low; i++) { g_timerCb(&g_timer); low = (g_pinLevel[PIN_AMP] == LOW); }
    printf("   диag: усилитель: как только пошёл звук, GP20=%d (ждали 0=LOW)\n", g_pinLevel[PIN_AMP]);
    chk(low, "усилитель: звук вернулся — GP20 снова 0 (усилитель включён)");
  }

  soundSetOn(0);
  chk(g_pwmLevel == APU_PWM_IDLE && g_timerCb == nullptr, "звук: выключение останавливает таймер и ставит тишину");
  soundSetOn(1);

  // темп звука: при 60 эмулированных кадрах/с — ровно 45 мкс (норма DMG),
  // при 32 — примерно в 60/32 раза реже, чтобы мелодия шла синхронно с игрой
  soundSync = 1; syncEmaFps = 60; statFps = 60; soundSyncTick();
  chk(g_timerUs == -45, "темп звука: на полной скорости период 45 мкс (как у настоящего DMG)");
  syncEmaFps = 32; statFps = 32; soundSyncTick();
  const int64_t p32 = -g_timerUs;
  printf("   темп звука: при 32 кадрах/с период %lld мкс (ожидалось ~84)\n", (long long)p32);
  chk(p32 >= 80 && p32 <= 90, "темп звука: при 32 кадрах/с семпл идёт ~в 1.9 раза реже");
  soundSync = 0; soundSyncTick();
  chk(g_timerUs == -45, "темп звука: режим REAL возвращает 45 мкс");
  syncEmaFps = 60; soundSync = 1;
  syncEmaFps = 5; statFps = 5; soundSyncTick();
  printf("   темп звука: при 5 кадрах/с период %lld мкс (ограничение 130)\n", (long long)-g_timerUs);
  chk(-g_timerUs == 130, "темп звука: слишком медленный ход ограничен 130 мкс");
  syncEmaFps = 60; statFps = 60; soundSyncTick();

  // ---------- сейвы и сейвстейты (на эмуляции flash) ----------
  flashBase = (uintptr_t)g_flash;          // «flash» лежит в памяти хоста
  flashEnd  = flashBase + 0x40000UL;       // образ кончился на 256 КиБ
  for (int i = 0; i < (int)sizeof(g_flash); i++) g_flash[i] = 0xFF;
  saveInit(8192, 1);
  chk(saveLen > 0 && saveData >= 8192, "под 8 КиБ сохранения отведено не меньше 8 КиБ (была потеря хвоста)");
  chk(saveLen % 4096 == 0, "область сейвов кратна сектору 4096 (требование flash_range_erase)");
  saveInit(32768, 1);
  chk(saveData >= 32768, "и для картриджа с 32 КиБ SRAM тоже хватает места");
  chk(saveLen % 4096 == 0, "и для 32 КиБ SRAM область тоже кратна сектору (была 33024 — не кратна)");
  chk(saveData <= (int)sizeof(cartRam), "данные сейва не выходят за размер картриджной RAM");

  // ---- v8.5: прямой путь вывода, предохранитель, тест панели, разбор кадра ----
  {
    static uint8_t px2[144][160];
    for (int L = 0; L < 144; L++)
      for (int x = 0; x < 160; x++) px2[L][x] = (uint8_t)(((L * 5 + x * 3) & 3) | 0x20);

    // (1) арифметика SDK: при clk_peri 48 МГц ЛЮБАЯ просьба даёт ровно 24 МГц
    chk(sdkBaudProbe(48000000u, 62500000u) == 24000000u,
        "проверка арифметики: при clk_peri 48 МГц просьба 62,5 даёт ровно 24 МГц (как и было на устройстве)");

    // (2) кадр игры прямой записью: ровно 240 строк по 480 байт
    g_data.clear();
    const uint64_t dr0 = g_drBytes;
    rowsValid = false; frameStart(); rowBl(GAME_TOP);
    for (int L = 0; L < 144; L++) cb_lcd_line(&gb, px2[L], (uint_fast8_t)L);
    rowBl(MEM_H - (GAME_TOP + GAME_ROWS));
    frameFinish();
    printf("   диag: прямой путь: строк %u, байт %u (в FIFO ушло %llu)\n",
           (unsigned)frameRows, (unsigned)(frameRows * ROW_BYTES), (unsigned long long)(g_drBytes - dr0));
    chk(frameRows == 240, "прямой путь: в панель ушло ровно 240 строк");
    chk(g_data.size() == 240UL * 240UL * 2UL, "прямой путь: кадр уходит целиком (240x240x2)");
    chk(g_drBytes - dr0 == 240ULL * ROW_BYTES, "прямой путь: ровно 115 200 байт в FIFO");
    chk(g_dmaStarts == g_dmaAtFrameStart, "вывод кадра больше вообще не трогает DMA");
    chk(!frameOpen, "после кадра окно закрыто (вне кадра строки не пишутся)");
    chk((((uintptr_t)tx_rowbuf) & 3) == 0 && (ROW_BYTES % 4) == 0,
        "буфер строки выровнен на 4 байта (иначе 32-битная запись = HardFault)");
    // содержимое: верхние 12 и нижние 12 строк — чёрные
    {
      bool strip = true;
      for (int r = 0; r < 12 && strip; r++)
        for (int x = 0; x < 240; x++)
          if (g_data[(r * 240 + x) * 2] || g_data[(r * 240 + x) * 2 + 1]) { strip = false; break; }
      chk(strip, "прямой путь: верхняя чёрная полоса на месте (12 строк)");
    }

    // (3) предохранитель: FIFO «встал» — ждать вечность не стали, и о сбое сказано в логе
    const uint32_t stuck0 = spiStuck, fail0 = txRowsFail, restarts0 = spiRestarts;
    g_spiJam = true;
    const uint32_t t0 = micros();
    int sv3 = captureStart(); g_serialEcho = true;
    rowsValid = false; frameStart(); rowBl(4); frameFinish();       // конец кадра сам проверяет здоровье шины
    std::string logStuck = captureStop(sv3); g_serialEcho = false;
    const uint32_t dtUs = micros() - t0;
    g_spiJam = false;
    printf("   диag: залипший FIFO: кадр занял %.1f мс виртуального времени, сработал предохранитель %u раз\n",
           dtUs / 1000.0, (unsigned)(spiStuck - stuck0));
    printf("--- сообщение о сбое SPI (как увидит пользователь) ---\n%s--- конец ---\n", logStuck.c_str());
    chk(spiRestarts > restarts0, "залипший FIFO: предохранитель сработал и шина перезапущена (в v8.3 здесь было вечное ожидание)");
    chk(spiStuck == stuck0, "после перезапуска шлейф сбоя не тянется (счётчик обнулён)");
    chk(txRowsFail > fail0, "залипший FIFO: потерянные строки посчитаны");
    chk(dtUs < 50000u, "залипший FIFO: ожидание ограничено по времени (нет зависания)");
    chk(has(logStuck, "SPI панели встал") && has(logStuck, "Перезапускаю"),
        "о сбое SPI сказано в логе и шина перезапущена (сообщает сам конец кадра)");
    chk(!spiHealthOk, "состояние «шина перезапускалась» видно в имени пути вывода");

    // (6d) v8.8: предохранитель ловит залипание, начавшееся В СЕРЕДИНЕ строки.
    // Старый код проверял время по номеру байта, а номер не меняется, пока FIFO
    // залип — проверка не срабатывала никогда, и строка ждала вечно.
    {
      spiRestarts = 0; spiStuck = 0; spiStuckSeen = 0; txRowsFail = 0;
      harnessJamAfter(5);                        // первые 5 байт уходят, дальше FIFO залип
      const uint32_t t0 = micros();
      const uint32_t tb0 = txBytes;
      int sv = captureStart(); g_serialEcho = true;
      rowsValid = false; frameStart(); rowBl(2); frameFinish();
      std::string log = captureStop(sv); g_serialEcho = false;
      const uint32_t dtUs = micros() - t0;
      printf("   диag: (проверка) байт записано %u (нужно 960), обращений к готовности FIFO %ld\n",
             (unsigned)(txBytes - tb0), g_jamCalls);
      harnessJamAfter(-1);
      printf("   диag: залипание с середины строки: вернулись за %.1f мс, неудачных строк %u, строк на панель %u\n",
             dtUs / 1000.0, (unsigned)txRowsFail, (unsigned)frameRows);
      printf("--- что сказал скетч ---\n%s--- конец ---\n", log.c_str());
      chk(dtUs < 60000u, "предохранитель срабатывает, даже если FIFO встал в середине строки (раньше — вечное ожидание)");
      chk(txRowsFail > 0, "такое залипание посчитано как сбой (видно в логе и в ящике)");
      chk(spiRestarts > 0, "после такого залипания шина перезапускается автоматически");
      chk(has(log, "SPI панели встал"), "в логе сказано про сбой шины (пользователь не гадает)");
    }

    // ---- v8.9m: тот же кадр, но строки увозит DMA ----
    {
      // 1) эталон: синхронный вывод (как было)
      txUseDma = false;
      g_data.clear();
      rowsValid = false; frameStart(); rowBl(GAME_TOP);
      for (int L = 0; L < 144; L++) cb_lcd_line(&gb, px2[L], (uint_fast8_t)L);
      rowBl(MEM_H - (GAME_TOP + GAME_ROWS));
      frameFinish();
      const std::vector<uint8_t> sync = g_data;

      // 2) то же самое, но через DMA. Завершение приходит НЕ сразу, а с ходом времени
      //    (так ведёт себя железо) — именно здесь ловится «буфер изменили в полёте».
      txUseDma = true; txDmaInit();
      chk(txDmaReady && txUseDma, "DMA: канал занят, шина разрешила передачу (TXDMAE выставлен)");
      const int bad0 = g_dmaBadStart, ab0 = g_dmaAborts, err0 = dmaErrors, starts0 = g_dmaStarts;
      const uint32_t stuck0 = spiStuck, fail0 = txRowsFail;
      txDmaRows = 0; txDmaAborts = 0; txDmaWaitAcc = 0;
      g_data.clear();
      g_dmaAsync = true;                              // завершение — по ходу времени
      rowsValid = false; frameStart(); rowBl(GAME_TOP);
      for (int L = 0; L < 144; L++) cb_lcd_line(&gb, px2[L], (uint_fast8_t)L);
      rowBl(MEM_H - (GAME_TOP + GAME_ROWS));
      frameFinish();
      g_dmaAsync = false;
      printf("   диag: DMA: строк через канал %u, ожидание канала %u мкс за кадр, сбросов канала %u (стартов DMA %d, плохих %d)\n",
             (unsigned)txDmaRows, (unsigned)txDmaWaitAcc, (unsigned)txDmaAborts,
             g_dmaStarts, g_dmaBadStart - bad0);
      chk(g_dmaStarts - starts0 == 240, "DMA: строки кадра реально ушли через канал (240 стартов)");
      chk(txDmaRows == 240, "DMA: через канал ушли все 240 строк");
      chk(g_data == sync && !g_data.empty(),
          "DMA: кадр совпадает с синхронным БАЙТ В БАЙТ (порядок строк не поехал)");
      chk(dmaErrors == err0, "DMA: буферы не менялись, пока передача шла (двойная буферизация цела)");
      chk(g_dmaBadStart == bad0, "DMA: ни одного пуска без адреса/длины (канал настроен верно)");
      chk(txDmaAborts == 0 && spiStuck == stuck0 && txRowsFail == fail0,
          "DMA: предохранитель не понадобился (ни сбоев, ни потерь строк)");
      chk(txDmaWaitAcc > 0, "DMA: ожидание канала посчитано (видно в статистике)");
      chk(!txDmaArmed, "DMA: после кадра в шине не осталось передач");
      chk(!frameOpen, "DMA: окно закрыто после кадра");

      // 3) переключение назад клавишей u: синхронный вывод снова работает
      txUseDma = false;
      g_data.clear();
      rowsValid = false; frameStart(); rowBl(4); frameFinish();
      chk(g_data.size() == 4UL * ROW_BYTES, "переключение на синхронный вывод (клавиша u) работает");
      txUseDma = true;

      // 4) предохранитель DMA: панель встала -> DREQ не приходит -> передача не кончается.
      //    Ждать вечно нельзя: канал сбрасывается, сбой считается, шина перезапускается.
      {
        const uint32_t st0 = spiStuck; const int ab1 = g_dmaAborts; const uint32_t rf0 = txRowsFail;
        const uint32_t rs0 = spiRestarts;
        g_dmaAsync = true; g_spiJam = true;
        const uint32_t t0 = micros();
        int sv = captureStart(); g_serialEcho = true;
        rowsValid = false; frameStart(); rowBl(4); frameFinish();
        std::string log = captureStop(sv); g_serialEcho = false;
        const uint32_t dtUs = micros() - t0;
        g_spiJam = false; g_dmaAsync = false;
        printf("   диag: DMA встал: кадр занял %.1f мс, сбросов канала %d, предохранитель %u раз\n",
               dtUs / 1000.0, g_dmaAborts - ab1, (unsigned)(spiStuck - st0));
        chk(g_dmaAborts > ab1, "DMA встал: канал сброшен (вечного ожидания нет)");
        (void)st0;
        chk(spiRestarts > rs0, "DMA встал: шина перезапущена автоматически (как в синхронном пути)");
        chk(txRowsFail > rf0, "DMA встал: потерянные строки посчитаны");
        chk(dtUs < 60000u, "DMA встал: ожидание ограничено по времени");
        chk(has(log, "SPI панели встал"), "DMA встал: сказано в логе, шина перезапущена");
      }
    }

  // сейвстейты: инициализация и проверка размеров
  emuInited = true;
  printf("   диag: flashBase=%p flashEnd=%p(0x%llX) saveOff=%u saveLen=%d\n", (void*)flashBase, (void*)flashEnd,
         (unsigned long long)(flashEnd - flashBase), saveOff, saveLen);
  saveInit(8192, 1);
  printf("   диag: после saveInit(8192,1): saveOff=%u saveLen=%d saveData=%d\n", saveOff, saveLen, saveData);
  ssInit();
  printf("   диag: после ssInit: ssOff[0]=%u ssOff[1]=%u ssLen=%d\n", ssOff[0], ssOff[1], ssLen);
  chk(ssReady && ssOff[0] != 0 && ssOff[1] > ssOff[0], "два слота сейвстейтов размещены во flash");
  chk(ssLen >= SAVE_HDR + (int)sizeof(struct gb_s) + (int)sizeof(cartRam), "слот вмещает gb_s + картриджную RAM");
  printf("   размер сейвстейта: %d КиБ (gb_s = %d Б)\n", ssLen / 1024, (int)sizeof(struct gb_s));

  // круговой тест: сохранить -> испортить -> загрузить
  for (int i = 0; i < (int)sizeof(cartRam); i++) cartRam[i] = (uint8_t)(i * 7 + 1);
  gb.hram_io[0x47] = 0xAB;
  gb.cpu_reg.a = 0x5A;
  apu.outVol = 0x33; apu.sq[0].freq = 1234; apu.wave[3] = 0x5A; apu.fsStep = 5;
  chk(ssSave(0), "сейвстейт сохранён в слот 1");
  const bool savedOk = (g_flashErrors == 0);
  chk(savedOk, "flash записан корректно (не было записи без стирания)");
  memset(cartRam, 0, sizeof(cartRam));
  gb.hram_io[0x47] = 0; gb.cpu_reg.a = 0;
  apu.outVol = 0; apu.sq[0].freq = 0; apu.wave[3] = 0; apu.fsStep = 0;
  {   // диагностика: почему не читается
    const uint8_t *p = (const uint8_t *)(flashBase + ssOff[0]);
    uint32_t magic,len,romH,gbB,apuB,h;
    memcpy(&magic,p,4); memcpy(&len,p+4,4); memcpy(&romH,p+8,4);
    memcpy(&gbB,p+12,4); memcpy(&apuB,p+16,4); memcpy(&h,p+20,4);
    printf("   диag: off=%u magic=%08X len=%u(нужно %d) gbB=%u(%u) apuB=%u(%u) romH=%08X(нужно %08X)\n",
           ssOff[0], magic, len, ssLen, gbB, (unsigned)sizeof(struct gb_s), apuB,
           (unsigned)sizeof(struct ApuState), romH, romFingerprint());
    if (gbB == (uint32_t)sizeof(struct gb_s) && apuB == (uint32_t)sizeof(struct ApuState)) {
      const uint32_t h2 = fnv1a(p+256,(int)gbB) ^ fnv1a(p+256+gbB,(int)sizeof(cartRam))
                        ^ fnv1a(p+256+gbB+(uint32_t)sizeof(cartRam), (int)apuB);
      printf("   диag: h=%08X h2=%08X  %s\n", h, h2, h==h2?"хеш сходится":"ХЕШ РАСХОДИТСЯ");
    }
    uint32_t found = 0; int nf = 0;
    for (uint32_t i = 0; i + 4 <= (uint32_t)sizeof(g_flash); i += 4)
      if (g_flash[i] == 0x53 && g_flash[i+1] == 0x53 && g_flash[i+2] == 0x54 && g_flash[i+3] == 0x33) { nf++; if (!found) found = i; }
    printf("   диag: ssOff[0]=%u ssOff[1]=%u ssLen=%d saveOff=%u saveLen=%d; магии 'SST3': %d, первая по 0x%X\n",
           ssOff[0], ssOff[1], ssLen, saveOff, saveLen, nf, found);
    for (int i=0;i<(int)sizeof(struct gb_s); i++) if (p[256+i] != ((const uint8_t*)&gb)[i]) {
      printf("   диag: первое расхождение gb на байте %d: во flash %02X, в памяти %02X\n",
             i, p[256+i], ((const uint8_t*)&gb)[i]); break; }
  }
  chk(ssLoad(0), "сейвстейт прочитан из слота 1");
  bool restored = (gb.cpu_reg.a == 0x5A) && (gb.hram_io[0x47] == 0xAB)
               && (apu.outVol == 0x33) && (apu.sq[0].freq == 1234)
               && (apu.wave[3] == 0x5A) && (apu.fsStep == 5);
  for (int i = 0; i < 256; i++) if (cartRam[i] != (uint8_t)(i * 7 + 1)) restored = false;
  chk(restored, "состояние восстановилось полностью (регистры + SRAM + состояние APU)");
  chk(!ssLoad(1), "пустой слот не загружается");

  // подделка данных должна быть замечена контрольной суммой
  g_flash[ssOff[0] + 300] ^= 0xFF;     // адрес уже от начала flash
  chk(!ssLoad(0), "испорченный сейвстейт отвергается (контрольная сумма)");

  // сейв SRAM (автосохранение) — круговой тест на картридже с полными 32 КиБ
  saveInit(32768, 1);
  saveActive = true;                                   // у этой игры есть память сохранений
  for (int i = 0; i < (int)sizeof(cartRam); i++) cartRam[i] = (uint8_t)(i ^ 0x5A);
  cartDirty = true;
  saveStore(true);
  for (int i = 0; i < (int)sizeof(cartRam); i++) cartRam[i] = 0;
  chk(saveLoad(), "автосейв SRAM читается обратно");
  bool sramOk = true;
  for (int i = 0; i < (int)sizeof(cartRam); i++) if (cartRam[i] != (uint8_t)(i ^ 0x5A)) sramOk = false;
  chk(sramOk, "автосейв SRAM восстановил картриджную RAM побайтово");
  chk(g_flashErrors == 0, "за всё время ни одной записи во flash без стирания");
  printf("   flash: невыровненных записей %d, повторных записей страниц %d\n",
         g_flashMisaligned, g_flashPageRewrite);
  chk(g_flashMisaligned == 0, "все записи во flash выровнены на 256 байт (причина бага сейвстейтов)");

  // --- v8.9l: БЕРЕЖЁМ FLASH — сейвы SRAM пишутся только изменившимися секторами ---
  {
    cartRam[0] ^= 0x5A;                                  // игра поменяла один байт
    const uint8_t want0 = cartRam[0];
    const int er0 = g_flashErases, pr0 = g_flashPrograms;
    saveStore(true);
    printf("   диag: сейв после правки 1 байта: стираний секторов %d, записей страниц %d (область = %d сектора)\n",
           g_flashErases - er0, g_flashPrograms - pr0, (int)(saveLen / 4096));
    chk(g_flashErases - er0 == 1,
        "сейвы: при правке одного байта стирается ТОЛЬКО один сектор (износ в 3 раза меньше)");
    chk(g_flashPrograms - pr0 == 16, "сейвы: переписываются 16 страниц одного сектора, а не вся область");
    memset(cartRam, 0, sizeof(cartRam));
    chk(saveLoad(), "сейвы: после частичной перезаписи сейв читается верно");
    chk(cartRam[0] == want0, "сейвы: правка доехала до flash");

    const int er1 = g_flashErases, pr1 = g_flashPrograms;
    saveStore(true);                                     // содержимое не менялось
    printf("   диag: сейв без изменений: стираний %d, записей %d (ждали 0 и 0)\n",
           g_flashErases - er1, g_flashPrograms - pr1);
    chk(g_flashErases == er1 && g_flashPrograms == pr1,
        "сейвы: если игра ничего не меняла — flash не трогаем вовсе");
    for (int i = 0; i < (int)sizeof(cartRam); i++) cartRam[i] = (uint8_t)(i ^ 0x5A);   // вернуть узор
  }
  printf("   flash: невыровненных стираний %d\n", g_flashEraseMisaligned);
  chk(g_flashEraseMisaligned == 0, "все стирания — целыми секторами 4096 (требование pico-sdk)");
  chk(g_flashPageRewrite == 0, "ни одна страница flash не записана дважды");

  // ---------- v8.9o: область картриджей (несколько игр) ----------
  {
    printf("\n=== v8.9o: список игр во flash и ячейки сейвов по играм ===\n");
    for (int i = 0; i < (int)sizeof(g_flash); i++) g_flash[i] = 0xFF;
    flashBase = (uintptr_t)g_flash;
    flashEnd  = flashBase + 0x40000UL;
    const uint32_t offA = 0x102000UL, offB = 0x110000UL;
    memcpy(g_flash + offA, ROM_DATA, ROM_SIZE);          // образ №1 — как есть (FAKEGAME)
    memcpy(g_flash + offB, ROM_DATA, ROM_SIZE);          // образ №2 — с другим именем
    const char *nm2 = "SECOND GAME";
    memset(g_flash + offB + 0x134, 0, 16);
    memcpy(g_flash + offB + 0x134, nm2, 11);
    uint8_t x = 0;                                        // и пересчитанной суммой заголовка
    for (uint32_t k = 0x134; k <= 0x14C; k++) x = (uint8_t)(x - g_flash[offB + k] - 1);
    g_flash[offB + 0x14D] = x;

    auto put32 = [](uint32_t off, uint32_t v) { memcpy(g_flash + off, &v, 4); };
    auto putEntry = [&](int i, const char *name, uint32_t off, uint32_t size, uint32_t crc, uint32_t sram) {
      RomEntry e; memset(&e, 0, sizeof(e));
      snprintf(e.name, sizeof(e.name), "%s", name);
      e.off = off; e.size = size; e.crc = crc; e.sram = sram;
      memcpy(g_flash + ROMS_TABLE_OFF + 16 + (uint32_t)i * sizeof(RomEntry), &e, sizeof(e));
    };
    put32(ROMS_TABLE_OFF, ROMS_MAGIC); put32(ROMS_TABLE_OFF + 4, 2);
    put32(ROMS_TABLE_OFF + 8, 2 * ROM_SIZE / 1024);
    putEntry(0, "FAKEGAME",    offA, ROM_SIZE, 0x11111111u, 8192);
    putEntry(1, "SECOND GAME", offB, ROM_SIZE, 0x22222222u, 0);

    int sv = captureStart(); g_serialEcho = true;
    romScan();
    std::string log = captureStop(sv); g_serialEcho = false;
    printf("--- список игр (как увидит пользователь) ---\n%s--- конец ---\n", log.c_str());
    chk(romCount == 2, "список игр: обе игры во flash найдены");
    chk(has(log, "2 игр"), "в логе сказано, сколько игр найдено");
    chk(strcmp(romList[0].name, "FAKEGAME") == 0 && strcmp(romList[1].name, "SECOND GAME") == 0,
        "имена игр прочитаны из таблицы (их показывает экран)");
    chk(strcmp(romList[1].name, "SECOND GAME") == 0 && romList[1].crc == 0x22222222u,
        "у каждой игры свой ключ сейва (CRC32 образа из таблицы)");
    chk(romSramMax == 8192, "размер ячейки сейва берётся по самой «жадной» игре списка");
    chk(romKib == 2 * ROM_SIZE / 1024, "занятый объём посчитан верно");

    // запись, ведущая вне области картриджей, — не игра
    putEntry(1, "SECOND GAME", 0x1000, ROM_SIZE, 0x22222222u, 0);
    romScan();
    chk(romCount == 1, "запись, ведущая вне области картриджей, отбрасывается");
    // мусор вместо образа: ни логотипа, ни верной суммы заголовка — не игра
    putEntry(1, "SECOND GAME", offB, ROM_SIZE, 0x22222222u, 0);
    g_flash[offB + 0x14D] ^= 0xFF;
    romScan();
    chk(romCount == 1, "мусор в область игр в список не попадает (не запустим чужой сектор)");
    // пропатченная игра: логотип на месте, сумма не сходится — её берём
    g_flash[offB + 0x104] = 0xCE; g_flash[offB + 0x105] = 0xED;
    romScan();
    chk(romCount == 2, "игру с логотипом берём, даже если сумма заголовка не сходится");
    g_flash[offB + 0x14D] = x;
    romScan();
    chk(romCount == 2, "после починки заголовка игра снова в списке");

    // v8.9q: «какая игра» пишется В СПИСКЕ ИГР — сразу, пока эмулятор ещё не запущен.
    // В игре flash не трогаем ВООБЩЕ: стирание сектора в игре = сброс по RUN (баг v8.9p).
    {
      adcSetCh(3, 2339); battRead();                    // питание в норме
      gDipWarn = false;                                 // доклад о просадке уже обработан циклом
      const int er0 = g_flashErases;
      romStateSave(1);
      printf("   диag: выбор игры в списке: pending=%d, стираний %d\n",
             romStatePending, g_flashErases - er0);
      chk(romStatePending == -1 && g_flashErases - er0 == 1,
          "«какая игра»: в списке игр пишется сразу, ровно одним сектором");
      int got = -1;
      chk(romStateLoad(&got) && got == 1, "«какая игра» читается обратно");
      printf("   диag: тихая запись: начало %u, конец %u, гашений подсветки %d\n",
             (unsigned)gQuietN, (unsigned)gQuietBack, g_blZeroWrites);
      chk(gQuietN >= 1 && gQuietBack == gQuietN,
          "тихая запись: счётчики «запись началась/кончилась» сходятся");
      chk(g_blZeroWrites >= 1,
          "тихая запись: подсветка в самом деле гасится на время записи (GPIO7 = 0)");
      const int er2 = g_flashErases;
      romStateSave(1);
      chk(romStatePending == -1 && g_flashErases == er2,
          "выбор той же игры flash НЕ трогает (это про износ)");
      // а при просевшем питании выбор откладывается — но и тогда в игре во flash тихо
      adcSetCh(3, 2000); battRead();
      const int er3 = g_flashErases;
      romStateSave(0);
      chk(romStatePending == 0 && g_flashErases == er3,
          "«какая игра»: при низкой батарее запись отложена, flash не тронут");
      harnessAdvanceMs(4100);                           // время идёт — а в игре писать всё равно нельзя
      {   // экран списка: цикл крутится, время идёт — во flash при этом не пишется
        const UiMode uiSave = ui; ui = UI_ROMS;
        int sv3 = captureStart(); g_serialEcho = true; loop(); captureStop(sv3); g_serialEcho = false;
        ui = uiSave;
      }
      chk(romStatePending == 0 && g_flashErases == er3,
          "«какая игра»: сама игра flash не трогает (ни через 4 с, ни позже)");
      romStateFlushAtExit();                            // при выключении — дописывается
      chk(romStatePending == -1 && g_flashErases > er3 && romStateLoad(&got) && got == 0,
          "«какая игра»: отложенный выбор дописывается при выключении");
      adcSetCh(3, 2339); battRead();
      romStateSave(1); romStateFlushAtExit();           // вернуть как было
      romStatePending = -1;
    }

    // ячейки сейвов: у каждой игры своя
    memset(g_flash + 0x40000, 0xFF, 0x100000 - 0x40000);          // чистим место под сейвы
    saveInit(8192, 2);
    chk(bankCount == 2 && saveLen % 4096 == 0,
        "под каждую игру выделена своя ячейка (и она кратна сектору)");
    chk(bankOff[1] == bankOff[0] + (uint32_t)saveLen, "ячейки идут подряд, без дыр");

    const uint8_t patternA = 0x11, patternB = 0x22;
    saveActive = true;
    saveBind(0x11111111u);
    const int bankA = saveBank;
    for (int i = 0; i < (int)sizeof(cartRam); i++) cartRam[i] = (uint8_t)(i ^ patternA);
    cartDirty = true; saveStore(true);
    chk(g_flashErrors == 0, "сейв первой игры записан без ошибок flash");

    saveBind(0x22222222u);
    chk(saveBank != bankA, "у второй игры — ДРУГАЯ ячейка (сейв первой не затирается)");
    chk(!saveLoad(), "у второй игры своего сейва ещё нет (чужой не подхватывается)");
    for (int i = 0; i < (int)sizeof(cartRam); i++) cartRam[i] = (uint8_t)(i ^ patternB);
    cartDirty = true; saveStore(true);

    saveBind(0x11111111u);
    chk(saveBank == bankA, "своя ячейка узнаётся по клейму игры (CRC32)");
    chk(saveLoad(), "сейв первой игры читается обратно");
    bool okA = true;
    for (int i = 0; i < saveData; i++) if (cartRam[i] != (uint8_t)(i ^ patternA)) okA = false;
    chk(okA, "сейв первой игры не пострадал от игры во вторую");
    saveBind(0x22222222u);
    chk(saveLoad(), "и сейв второй игры на месте");
    bool okB = true;
    for (int i = 0; i < saveData; i++) if (cartRam[i] != (uint8_t)(i ^ patternB)) okB = false;
    chk(okB, "сейв второй игры не перепутан с первым");

    // ячейка прежних версий (без клейма — так писал v8.9n): не пропадает и не путается
    memset(g_flash + 0x40000, 0xFF, 0x100000 - 0x40000);
    saveInit(8192, 2);
    saveActive = true;
    saveBind(0);
    for (int i = 0; i < (int)sizeof(cartRam); i++) cartRam[i] = (uint8_t)(i ^ 0x33);
    cartDirty = true; saveStore(true);
    const int bankOld = saveBank;
    memset(g_flash + bankOff[bankOld] + SAVE_HDR_KEY, 0xFF, 8);    // как заголовок v8.9n
    saveInit(8192, 2);
    saveBind(0x77777777u);
    chk(saveBank != bankOld, "ячейка прежних версий не отдаётся первой попавшейся игре");
    saveBind(0);
    chk(saveBank == bankOld, "сейв встроенной игры (прежний формат) всё ещё узнаётся");
    chk(saveLoad(), "и грузится — обновление прошивки сейв не теряет");

    // «какая игра»: смена игры в списке запускает её и запоминает выбор
    for (int i = 0; i < (int)sizeof(g_flash); i++) g_flash[i] = 0xFF;
    memcpy(g_flash + offA, ROM_DATA, ROM_SIZE);
    memcpy(g_flash + offB, ROM_DATA, ROM_SIZE);
    uint8_t x2 = 0; for (uint32_t k = 0x134; k <= 0x14C; k++) x2 = (uint8_t)(x2 - g_flash[offB + k] - 1);
    g_flash[offB + 0x14D] = x2;
    put32(ROMS_TABLE_OFF, ROMS_MAGIC); put32(ROMS_TABLE_OFF + 4, 2);
    put32(ROMS_TABLE_OFF + 8, 2 * ROM_SIZE / 1024);
    putEntry(0, "FAKEGAME",    offA, ROM_SIZE, 0x11111111u, 8192);
    putEntry(1, "SECOND GAME", offB, ROM_SIZE, 0x22222222u, 8192);
    romScan();
    chk(romCount == 2, "список снова из двух игр (перед проверкой запуска)");
    emuInited = true; ui = UI_GAME; paused = false;
    gb.display.lcd_draw_line = cb_lcd_line;
    {
      sv = captureStart(); g_serialEcho = true;
      harnessSend("c"); handleSerial();                            // клавиша c — список игр
      log = captureStop(sv); g_serialEcho = false;
      chk(ui == UI_ROMS, "клавиша c открывает список игр");
      chk(has(log, "СПИСОК ИГР"), "пользователь видит, как управлять списком");
    }
    adcSetCh(3, 2339); battRead();                 // v8.9q: питание в норме — выбор пишется сразу
    const bool started = romStart(1, false);
    chk(started, "игру из списка можно запустить на ходу (ядро переинициализируется)");
    chk(romCur == 1 && romSize == ROM_SIZE && romMask == ROM_SIZE - 1,
        "образ берётся из области картриджей (свой размер и маска по всей игре)");
    chk(romSaveKey == 0x22222222u, "ключ сейва переключился на новую игру");
    // v8.9q: выбор игры пишется в момент выбора (в списке) — и ничего не ждёт
    int lastIdx = -1;
    chk(romStateLoad(&lastIdx) && lastIdx == 1,
        "выбор игры запомнен: запись ушла в сектор ещё в списке, до старта эмулятора");
    chk(romFingerprint() == 0x22222222u, "сейвстейты теперь метятся ключом новой игры");
    romStatePending = -1;                       // отложенную запись гасим: её проверили выше

    // уборка: пусть дальнейшие проверки видят устройство без картриджей
    memset(g_flash + ROMS_TABLE_OFF, 0xFF, 0x4000);
    romScan();
    chk(romCount == 0, "списка нет — скетч возвращается к встроенной игре (совместимость)");
  }

  // ---------- v8.9o: область игр, собранная НАСТОЯЩИМ упаковщиком ----------
  // Файлы готовит tools/roms_pack/selftest.py (он же проверяет упаковщик целиком),
  // а здесь проверяем, что скетч читает именно этот файл так, как задумано:
  // имена, размеры, ключи сейвов и сами байты образов.
  {
    const char *reg = getenv("GBEMU_TEST_REGION");
    const char *txt = getenv("GBEMU_TEST_REGION_TXT");
    if (reg && txt && access(reg, R_OK) == 0 && access(txt, R_OK) == 0) {
      printf("\n=== v8.9o: разбор области, собранной упаковщиком make_roms_uf2.py ===\n");
      for (int i = 0; i < (int)sizeof(g_flash); i++) g_flash[i] = 0xFF;
      flashBase = (uintptr_t)g_flash;
      flashEnd  = flashBase + 0x40000UL;
      FILE *f = fopen(reg, "rb");
      const size_t got = f ? fread(g_flash + ROMS_TABLE_OFF, 1, 0x100000, f) : 0;
      if (f) fclose(f);
      chk(got > 0, "область игр упаковщика прочитана в стенде");
      int sv = captureStart(); g_serialEcho = true;
      romScan();
      std::string log = captureStop(sv); g_serialEcho = false;
      printf("--- список игр от упаковщика ---\n%s--- конец ---\n", log.c_str());

      int expCount = -1, expUsed = -1;
      char line[256];
      FILE *t = fopen(txt, "r");
      while (t && fgets(line, sizeof(line), t)) {
        int i, off, size, sram; unsigned crc; char nm[64];
        if (sscanf(line, "count=%d", &expCount) == 1) continue;
        if (sscanf(line, "used_kib=%d", &expUsed) == 1) continue;
        if (sscanf(line, "%d|%63[^|]|%d|%d|0x%x|%d", &i, nm, &off, &size, &crc, &sram) == 6) {
          if (i >= 0 && i < romCount) {
            const RomEntry &e = romList[i];
            char what[160];
            snprintf(what, sizeof(what), "упаковщик→скетч, игра %d: имя, смещение, размер, ключ, память",
                     i + 1);
            chk(strcmp(e.name, nm) == 0 && (int)e.off == off && (int)e.size == size &&
                e.crc == crc && (int)e.sram == sram, what);
          }
        }
      }
      if (t) fclose(t);
      char what[96];
      snprintf(what, sizeof(what), "упаковщик→скетч: в списке %d игр (как собрал упаковщик)", expCount);
      chk(expCount > 0 && romCount == expCount, what);
      chk(expUsed > 0 && (int)romKib == expUsed, "упаковщик→скетч: занятый объём совпал");
      chk(romSramMax == 32768, "упаковщик→скетч: размер ячейки сейва — по самой «жадной» игре");

      // запуск третьей игры (512 КиБ, MBC5+RAM+батарея) — как это делает пользователь
      const uint32_t wantKey = romList[2].crc;
      const int started3 = romStart(2, false) ? 1 : 0;
      chk(started3, "упаковщик→скетч: игру из области можно запустить (ядро инициализировалось)");
      chk(romCur == 2 && romSize == romList[2].size && romMask == romMask, "образ выбран из области игр");
      chk(romSaveKey == wantKey, "ключ сейва — тот, что посчитал упаковщик");
      chk(cb_rom_read(&gb, 0x104) == 0xCE && cb_rom_read(&gb, 0x105) == 0xED,
          "байты образа читаются из области игр (логотип на месте)");
      chk(cb_rom_read(&gb, 0x100) == 0x00 && cb_rom_read(&gb, 0x101) == 0xC3,
          "и по смещению 0x100 — тоже из образа (не из встроенной игры)");

      // уборка, чтобы дальнейшие проверки видели устройство без картриджей
      memset(g_flash + ROMS_TABLE_OFF, 0xFF, 0x4000);
      romScan();
      chk(romCount == 0, "после уборки скетч снова видит только встроенную игру");
    } else {
      printf("\n(стенд: GBEMU_TEST_REGION не задан — проверка с упаковщиком пропущена)\n");
    }
  }

  // ---------- имитация загрузки устройства: setup() и первые кадры loop() ----------
  // Ловит то, что видно только при запуске: панику на недопустимом пине SPI,
  // зависание в инициализации, взаимные помехи звука и подсветки по слайсам ШИМ.
  printf("\n=== имитация загрузки (setup + 3 кадра loop) ===\n");
  flashBase = (uintptr_t)g_flash;
  flashEnd  = flashBase + 0x40000UL;
  for (int i = 0; i < (int)sizeof(g_flash); i++) g_flash[i] = 0xFF;
  g_analogSlices = 0;

  // ДО setup(): как было бы без подъёма clk_peri — просьба 62,5 даёт 24 МГц
  const uint32_t periBefore = g_clkPeri;
  printf("--- ожидаемый лог загрузки (эхо Serial) ---\n");
  g_serialEcho = true;
  setup();
  g_serialEcho = false;
  printf("--- конец лога загрузки ---\n");
  chk(true, "setup() прошёл целиком (без паники и без зависания)");
  // ГЛАВНОЕ: частота SPI панели. clk_peri на устройстве был 48 МГц, SPI не мог быть
  // быстрее 24 МГц — отсюда 38 мс на передачу кадра и «кадр с выводом 45.8 мс».
  printf("   диag: clk_peri было %.1f МГц, стало %.1f МГц; SPI панели %.1f МГц\n",
         periBefore / 1e6, g_clkPeri / 1e6, g_spiBaud / 1e6);
  chk(periBefore == 48000000u, "стенд начинает с того же, что было на устройстве: clk_peri = 48 МГц");
  chk(g_clkPeri == g_clkSys, "clk_peri поднят до системной частоты (иначе SPI навсегда 24 МГц)");
  // v8.9n: по умолчанию берём САМУЮ БЫСТРУЮ частоту (125 МГц) — на консоли видно,
  // что именно на ней игра идёт ровно 60 кадр/с, а на 62,5 МГц кадр тяжелее панелью.
  chk(g_spiBaud == 125000000u, "SPI панели: по умолчанию 125 МГц (на консоли это ровные 60 кадр/с)");
  chk(SPI_HZ_TAB[0] == 125000000UL && SPI_HZ_TAB[1] == 62500000UL,
      "клавиша o перебирает: 125 -> 62,5 -> 41,7 -> 20,8 МГц (первой — самая быстрая)");
  {
    // кадр = 240 строк x 480 байт = 115200 байт = 921600 бит
    const double bits = 240.0 * 480.0 * 8.0;
    const double tOld = bits / 24000000.0 * 1000.0;    // мс при 24 МГц (как было)
    const double tNew = bits / 125000000.0 * 1000.0;   // мс при 125 МГц (по умолчанию)
    printf("   диag: передача кадра: было %.1f мс при 24 МГц, стало %.1f мс при 125 МГц\n", tOld, tNew);
    chk(tOld - tNew > 30.0, "передача кадра стала короче более чем на 30 мс (это и был тормоз)");
  }
  chk((g_analogSlices & (1u << 0)) == 0,
      "звук (слайс 0) и подсветка (analogWrite) не делят один слайс ШИМ");
  chk(g_analogFreq >= 20000,
      "подсветка: ШИМ ультразвуковой >=20 кГц (v9.6: 1 кГц видимо мерцал)");
  chk(emuInited, "эмулятор инициализирован (rom.h из стенда подхватился)");
  printf("   диag: soundOn=%d gAmpOn=%d gAmpSil=%u GP20=%d\n",
         (int)soundOn, (int)gAmpOn, (unsigned)gAmpSil, g_pinLevel[PIN_AMP]);
  chk(g_pinLevel[PIN_AMP] == (gAmpOn ? LOW : HIGH),
      "усилитель: GP20 = LOW когда включён и HIGH когда заглушён (v9.4)");

  for (int i = 0; i < 3; i++) loop();
  chk(true, "три кадра loop() прошли без зависания");

  // ================= v8.4: сторож, отчёт «где встали», журнал удержания, клавиша e =========
  // Причина: после интро экран чернел и консоль замолкала; оживить можно было только
  // вынув батарейку, и НИКАКОЙ информации о том, где встало, не оставалось.
  // Теперь сторож перезагружает консоль, а перед этим оставляет отметку фазы.
  printf("\n=== v8.4: сторож и отчёт о зависаниях ===\n");

  chk(g_wdEnabledMs == 4000u, "сторож включён на 4 с ещё в setup()");
  chk(g_pullUpOnPwr, "на ключе питания (GPIO1) включена подтяжка вверх — сброс не выключит консоль");

  // --- v8.9: режимы-изоляторы работают так, как обещано в справке ---
  {
    ui = UI_GAME; emuInited = true; paused = false;
    // «панель не трогаем»: эмуляция идёт, а в SPI не уходит ни одного байта
    noPanelMode = true;
    const uint32_t tb0 = txBytes, fr0 = gFramesTotal;
    loop(); loop();
    const uint32_t sent = txBytes - tb0;
    printf("   диag: режим «панель не трогаем»: кадров %u, байт в SPI %u\n",
           (unsigned)(gFramesTotal - fr0), (unsigned)sent);
    chk(gFramesTotal >= fr0 + 2, "режим LEFT: эмуляция продолжается (кадры идут)");
    chk(sent == 0, "режим LEFT: в панель не уходит ни одного байта (панель не трогаем)");
    noPanelMode = false;
    // «чёрный ящик выключен»: ни одной записи во flash
    blkOn = false;
    const int ops0 = g_flashOps, w0 = blkWrites;
    blkWrite(); blkWrite();
    chk(g_flashOps == ops0 && blkWrites == w0, "режим DOWN: с выключенным ящиком во flash ничего не пишется");
    blkOn = true;

    // --- v8.9j/k: ВО ВРЕМЯ ИГРЫ FLASH НЕ ТРОГАЕМ ВООБЩЕ (это и есть причина смерти) ---
    {
      blkEraseInPlay = false; blkSafeFlush = false;
      blkWipe();                                        // как при запуске: чистое кольцо
      const int er0 = g_flashErases, pr0 = g_flashPrograms;
      for (int i = 0; i < 40; i++) { gFramesTotal = (uint32_t)i; blkWrite(); }
      gFramesTotal = 0;
      printf("   диag: 40 отметок по умолчанию: стираний %d, записей страниц %d, в кольце ОЗУ %u\n",
             g_flashErases - er0, g_flashPrograms - pr0, (unsigned)blkRamN);
      chk(g_flashErases == er0 && g_flashPrograms == pr0,
          "ящик: по умолчанию во время игры во flash НЕ ЛЕЗЕМ ВООБЩЕ (ни записи, ни стирания)");
      chk(blkRamN == BLK_RAM_N, "ящик: отметки копятся в кольце ОЗУ (16 последних)");
      chk(blkTotal == 40 && blkFlushed == 0, "ящик: счётчики видят, что во flash ещё ничего не ушло");

      // выгрузка пишет ТОЛЬКО новые отметки, и повторная выгрузка ничего не дублирует
      const int pr1 = g_flashPrograms;
      blkFlush(true);
      printf("   диag: выгрузка кольца: записей %d (ждали 16), всего во flash %u из %u\n",
             g_flashPrograms - pr1, (unsigned)blkFlushed, (unsigned)blkTotal);
      chk(g_flashPrograms - pr1 == BLK_RAM_N, "ящик: выгрузка пишет ровно 16 последних отметок");
      const int pr2 = g_flashPrograms;
      blkFlush(true);
      chk(g_flashPrograms == pr2, "ящик: повторная выгрузка без новых отметок ничего не пишет");
      // и запись из flash читается как свежая
      const uint32_t *p = (const uint32_t *)(flashBase + blkSlotOff((uint32_t)blkNewestSlot));
      printf("   диag: в записи: отметок %u, стираний %u, операций с flash %u\n",
             (unsigned)p[49], (unsigned)p[48], (unsigned)p[50]);
      chk(p[0] == 0x314B4C42UL, "ящик: выгруженная запись на месте (магия на месте)");
      blkWipe();
    }

    // --- v8.9k: режим START — выгрузка МЕЖДУ кадрами (и только когда панель молчит) ---
    {
      blkSafeFlush = true; blkEraseInPlay = false; blkWipe();
      blkWrite(); blkWrite();                           // две отметки в ОЗУ
      const int pr0 = g_flashPrograms;
      g_spiBusy = true;                                 // панель ещё занята
      blkLastFlushMs = 0; g_nowUs = 10000ULL * 1000ULL;  // «прошло» 10 с (стенд считает время сам)
      blkSafeTick();
      printf("   диag: START-выгрузка при занятой панели: записей %d (ждали 0)\n", g_flashPrograms - pr0);
      chk(g_flashPrograms == pr0, "режим START: пока панель занята — во flash не пишем (ждём безопасной точки)");
      g_spiBusy = false;
      blkSafeTick();
      printf("   диag: START-выгрузка при свободной панели: записей %d (ждали 2)\n", g_flashPrograms - pr0);
      chk(g_flashPrograms - pr0 == 2, "режим START: между кадрами отметки уходят во flash");
      const int pr1 = g_flashPrograms;
      g_nowUs = 11000ULL * 1000ULL;                     // только 1 с спустя — рано
      blkWrite(); blkSafeTick();
      chk(g_flashPrograms == pr1, "режим START: чаще, чем раз в 5 с, во flash не лезем");
      g_nowUs = 20000ULL * 1000ULL; blkSafeTick();
      chk(g_flashPrograms == pr1 + 1, "режим START: через 5 с — выгружает (одну новую отметку)");
      blkSafeFlush = false; blkWipe();
    }

    // --- v8.9j/k: режим A — контрольная проверка «как раньше» (стирание в игре бывает) ---
    {
      blkEraseInPlay = true;
      const int er0 = g_flashErases, pr0 = g_flashPrograms;
      for (int i = 0; i < BLK_SLOTS + 2; i++) { gFramesTotal = (uint32_t)i; blkWrite(); }
      gFramesTotal = 0;
      printf("   диag: режим A (как раньше): стираний %d, записей страниц %d\n",
             g_flashErases - er0, g_flashPrograms - pr0);
      chk(g_flashErases > er0 && g_flashPrograms > pr0,
          "ящик (режим A): пишет в игре со стираниями — контрольная проверка работает");
      blkEraseInPlay = false;
      blkWipe();
    }

    // --- v8.9l: ящик — КОЛЬЦО БЕЗ СТИРАНИЯ ПРИ ВКЛЮЧЕНИИ (бережём flash) ---
    {
      blkEraseInPlay = true; blkWipe();                    // чистая площадка (2 сектора)
      for (int i = 0; i < BLK_SLOTS; i++) { gFramesTotal = (uint32_t)i; blkWrite(); }
      const int er1 = g_flashErases;
      blkInit();                                           // «включили консоль заново»
      printf("   диag: повторное включение: стираний %d (ждали 0), записей в кольце найдено %d, следующий слот %u\n",
             g_flashErases - er1, (int)blkNewestSlot, (unsigned)(blkSeq % BLK_SLOTS));
      chk(g_flashErases == er1, "ящик: при включении flash НЕ стирается (стираем только по мере надобности)");
      chk(blkNewestSlot == BLK_SLOTS - 1, "ящик: после включения самая свежая запись найдена по номеру");
      chk(blkRingFull, "ящик: видно, что все 32 слота заняты (кольцо полно)");
      // следующая запись попадёт в занятый слот 0 -> должен быть ровно ОДИН стирания сектора
      const int er2 = g_flashErases;
      blkWrite();
      printf("   диag: запись в полное кольцо: стираний %d (ждали 1)\n", g_flashErases - er2);
      chk(g_flashErases - er2 == 1, "ящик: кольцо идёт по кругу — стирается ровно один сектор на виток");
      chk(blkSeq % BLK_SLOTS == 1, "ящик: после витка кольца пишем в следующий слот");
      blkEraseInPlay = false; blkWipe();
    }

    // --- v8.9j: режимы при включении вокруг ящика ---
    {
      blkSafeFlush = false; blkEraseInPlay = false; g_lowPins = 0;
      g_lowPins |= (1u << BTN_GPIO[6]);                 // START — умный ящик
      int sv = captureStart(); g_serialEcho = true; applyBootModes();
      std::string log = captureStop(sv); g_serialEcho = false; g_lowPins = 0;
      printf("─── режим START при включении ───\n%s─── конец ───\n", log.c_str());
      chk(blkSafeFlush, "режим START при включении: ящик выгружает во flash между кадрами");
      chk(has(log, "УМНЫЙ ЯЩИК") && has(log, "МЕЖДУ кадрами"), "режим START: сказано словами, что включено");
      blkSafeFlush = false; blkEraseInPlay = false;
      g_lowPins |= (1u << BTN_GPIO[4]);                 // A
      sv = captureStart(); g_serialEcho = true; applyBootModes();
      log = captureStop(sv); g_serialEcho = false; g_lowPins = 0;
      printf("─── режим A при включении ───\n%s─── конец ───\n", log.c_str());
      chk(blkEraseInPlay, "режим A при включении: ящик как раньше (со стираниями в игре)");
      chk(has(log, "КАК РАНЬШЕ"), "режим A: сказано словами, что включено");
      blkEraseInPlay = false;
      // и обычный запуск рассказывает, как теперь ведёт себя ящик
      sv = captureStart(); g_serialEcho = true; applyBootModes();
      log = captureStop(sv); g_serialEcho = false;
      printf("─── обычный запуск: как ведёт себя ящик ───\n%s─── конец ───\n", log.c_str());
      chk(has(log, "НЕ ПИШЕМ ВООБЩЕ"), "обычный запуск: в логе сказано, что в игре во flash не пишем");
    }
    // справка рассказывает про режимы
    { int sv = captureStart(); g_serialEcho = true;
      printHelp();
      std::string log = captureStop(sv); g_serialEcho = false;
      chk(has(log, "SELECT") && has(log, "лёгкий") && has(log, "DOWN") && has(log, "панель не трогаем"),
          "справка называет все четыре режима при включении (SELECT/B/DOWN/LEFT)");
      chk(has(log, "START") && has(log, "умный ящик"), "справка называет режим START (умный ящик)");
    }
  }

  // --- v8.9: прибор питания (АЦП) ---
  {
    printf("   диag: АЦП: инициализаций %d, выводы GP%d/GP%d, чтений %d, GP26=%u, GP29=%u, термодатчик=%u мВ\n",
           g_adcInits, g_adcPins[0], g_adcPins[1], g_adcReads,
           (unsigned)battARaw, (unsigned)battBRaw, (unsigned)battTMv);
    chk(g_adcInits > 0, "прибор питания: АЦП инициализирован");
    chk(g_adcPins[0] == 26 && g_adcPins[1] == 29, "прибор питания: выводы GP26 и GP29 переведены в аналоговый режим");
    chk(battARaw == g_adcCh[0], "прибор питания: значение канала 0 (GP26) доходит до прошивки");
    chk(battAMv == (uint32_t)g_adcCh[0] * 3300u / 4096u, "прибор питания: отсчёты пересчитаны в милливольты");
    chk(battTRaw == g_adcCh[4], "прибор питания: читается и внутренний термодатчик (самопроверка АЦП)");
  }
  { int sv = captureStart(); g_serialEcho = true;
    battPrint();
    std::string log = captureStop(sv); g_serialEcho = false;
    printf("--- строка про питание (как увидит пользователь) ---\n%s--- конец ---\n", log.c_str());
    chk(has(log, "сторож питания") && has(log, "A0=") && has(log, "A3="), "сторож питания: печатает все четыре канала АЦП (GP26..GP29)");
    chk(has(log, "термодатчик"), "сторож питания: печатает термодатчик (видно, что АЦП живой)");
    chk(has(log, "делитель 1:2"), "сторож питания: сказано, что A3 — батарея через делитель 1:2");
  }
  { // v8.9g: батарея читается с A3 (GP29) через делитель 1:2
    adcSetCh(3, 2332); battRead();
    int sv = captureStart(); g_serialEcho = true;
    battPrintBattery();
    std::string log = captureStop(sv); g_serialEcho = false;
    printf("--- строка про батарею (как увидит пользователь) ---\n%s--- конец ---\n", log.c_str());
    chk(has(log, "батарея: A3=") && has(log, "3.7"), "батарея: печатается напряжение в вольтах (A3 × 2)");
    chk(has(log, "норма") || has(log, "полная"), "батарея: состояние словами (полная/норма/разряжается/низко)");
    adcSetCh(3, 1900); battRead();                 // 1900 отсч. = 1530 мВ -> 3,06 В: низко
    sv = captureStart(); g_serialEcho = true; battPrint();
    log = captureStop(sv); g_serialEcho = false;
    chk(has(log, "НИЗКО") || has(log, "пора заряжать"), "батарея: при низком напряжении предупреждает по делу");
    adcSetAll(1450); battRead();
  }

  { // v8.9e: четыре канала читаются и попадают в переменные
    adcSetCh(0, 3000); adcSetCh(1, 1111); adcSetCh(2, 2222); adcSetCh(3, 2500);
    battRead();
    printf("   диag: каналы АЦП: A0=%u A1=%u A2=%u A3=%u\n",
           (unsigned)battCh[0], (unsigned)battCh[1], (unsigned)battCh[2], (unsigned)battCh[3]);
    chk(battCh[0] == 3000 && battCh[1] == 1111 && battCh[2] == 2222 && battCh[3] == 2500,
        "сторож питания: читаются все четыре внешних канала (A0..A3 = GP26..GP29)");
    chk(battChBoot[0] != 0, "сторож питания: запоминает значения ПРИ ВКЛЮЧЕНИИ (для сравнения)");
    // просадка: канал 3 упал вдвое против включения — скетч обязан сказать
    battChBoot[0] = 3000; battChBoot[1] = 1111; battChBoot[2] = 2222; battChBoot[3] = 2500;
    const uint16_t keep = battChBoot[3];
    adcSetCh(3, 700); battRead();
    int sv = captureStart(); g_serialEcho = true;
    battCompare();
    std::string log = captureStop(sv); g_serialEcho = false;
    printf("--- предупреждение о посадке ---\n%s--- конец ---\n", log.c_str());
    chk(has(log, "просел"), "сторож питания: заметил, что канал просел против включения");
    battChBoot[3] = keep; adcSetCh(0, 1450); adcSetCh(3, 0); battRead();
    adcSetAll(1450); battRead();
  }

  // --- v8.9e: дежурство в ящике — первые минуты ЧАСТО (0,25 с), потом редко ---
  {
    const uint32_t w0 = blkWrites;
    blkLastMs = millis();                       // начинаем отсчёт дежурства с этого места
    while (millis() < 60000UL) { delay(50); blkTick(); }   // «первая минута» жизни
    const uint32_t fast = blkWrites - w0;
    printf("   диag: за первую минуту дежурства записей в ящик: %u\n", (unsigned)fast);
    chk(fast > 100, "сторож питания: в первую минуту дежурство пишет часто (примерно 4 раза в секунду)");
    const uint32_t w1 = blkWrites;
    blkLastMs = millis();
    for (int i = 0; i < 40; i++) { delay(100); blkTick(); }   // 4 секунды во «взрослой» жизни
    chk(blkWrites == w1, "сторож питания: потом пишет редко (за 4 с — ни одной записи, flash бережём)");
    for (int i = 0; i < 20; i++) { delay(100); blkTick(); }   // ещё 2 с = перешагнули 5 с
    chk(blkWrites > w1, "сторож питания: и всё равно раз в 5 с пишет");
  }

  // --- v8.9e: снятие питания всегда оставляет след в ящике и в логе ---
  {
    const uint32_t cut0 = gPowerCutBy;
    int sv = captureStart(); g_serialEcho = true;
    powerOff();                                  // тот же путь, что при удержании START
    std::string log = captureStop(sv); g_serialEcho = false;
    printf("--- снятие питания (как увидит пользователь) ---\n%s--- конец ---\n",
           log.substr(0, 400).c_str());
    chk(gPowerCutBy == cut0 + 1, "снятие питания: счётчик «сколько раз снимала наша прошивка» растёт");
    chk(has(log, "СНИМАЮ ПИТАНИЕ"), "снятие питания: в логе сказано, кто и почему снял питание");
    const uint32_t *p = (const uint32_t *)(flashBase + blkSlotOff((uint32_t)blkNewestSlot));
    printf("   диag: запись ящика после снятия питания: фаза %u (ждали %u), ключ %u, счётчик %u\n",
           (unsigned)p[1], (unsigned)PH_POWERCUT, (unsigned)p[39], (unsigned)p[38]);
    chk(p[1] == PH_POWERCUT, "снятие питания: в ящике есть запись «снято питание» (по ней видно, что это не сбой)");
    chk(p[38] == gPowerCutBy, "снятие питания: счётчик попал в ящик");
    g_powerCut = false;
  }

  // --- v8.9h: ловушка просадки питания (читаем батарею в прерывании звука) ---
  {
    const uint32_t frSave = gFramesTotal;                 // вернём как было: дальше идут тесты кадров
    blkWipe();                                           // ящик пуст: доклад о просадке должен попасть во flash
    // (1) норма: минимум за окно равен текущему значению
    adcSetCh(3, 2339); battRead();
    gDipMin = 4095; gDipMax = 0; gDipWorst = 4095; gDipWarn = false;
    // после перебора каналов ловушка пропускает 4 отсчёта (v8.9t), поэтому тиков больше:
    // 64 тика = 8 замеров, из них 4 пропущены, 4 записаны
    for (int i = 0; i < 64; i++) g_timerCb(&g_timer);
    printf("   диag: просадка: после 16 тиков мин=%u макс=%u (батарея 2339)\n",
           (unsigned)gDipMin, (unsigned)gDipMax);
    chk(gDipMin == 2339 && gDipMax == 2339, "ловушка просадки: в норме минимум и максимум = текущему значению");

    // (2) просадка: батарея клюнула — ловушка обязана заметить и попросить напечатать
    adcSetCh(3, 1900);                                     // ~3,06 В на батарее
    // v8.9t: просадка = низкий участок >= 10 мс. Каждый замер ловушки «стоит» ~1 мс
    // условного времени, поэтому гоняем заведомо дольше (150 тиков ~ 18 замеров).
    for (int i = 0; i < 150; i++) g_timerCb(&g_timer);
    printf("   диag: просадка: мин=%u (ждали 1900), самый глубокий=%u, флаг=%d\n",
           (unsigned)gDipMin, (unsigned)gDipWorst, (int)gDipWarn);
    chk(gDipMin == 1900 && gDipWorst == 1900, "ловушка просадки: минимум за окно и самый глубокий провал записаны");
    chk(gDipWarn, "ловушка просадки: попросила главный цикл напечатать предупреждение");

    // (3) главный цикл печатает это в лог (с выгрузкой буфера — чтобы успеть до смерти)
    ui = UI_GAME; emuInited = true; paused = false;
    int sv = captureStart(); g_serialEcho = true;
    loop();
    std::string log = captureStop(sv); g_serialEcho = false;
    printf("─── предупреждение о просадке (как увидит пользователь) ───\n%s─── конец ───\n",
           log.substr(0, 500).c_str());
    chk(has(log, "ПРОСАДКА ПИТАНИЯ"), "ловушка просадки: доклад доходит до лога");
    chk(has(log, "клавиша [") && has(log, "SPI панели ниже"), "ловушка просадки: подсказано, что делать");
    chk(gDipEvents >= 1, "ловушка просадки: счётчик срабатываний растёт");

    // (4) v8.9p: доклад о просадке БОЛЬШЕ НЕ ПИШЕТ ВО FLASH (это и убивало консоль:
    //     стирание и запись сектора в момент, когда питание и так на грани). Отметка
    //     остаётся в кольце ОЗУ и уходит во flash при выключении.
    {
      const int er0 = g_flashErases, pr0 = g_flashPrograms, ops0 = g_flashOps;
      const uint32_t dipMinBefore = gDipMin;
      gDipWarn = true;                                   // как будто ловушка только что сработала
      {
        int sv2 = captureStart(); g_serialEcho = true;
        loop();                                          // доклад печатается из цикла
        std::string log2 = captureStop(sv2); g_serialEcho = false;
        printf("─── просадка (v8.9p: без записи во flash) ───\n%s─── конец ───\n", log2.c_str());
        chk(has(log2, "во flash не пишу и в игре вообще не буду"),
            "просадка: в логе прямо сказано, что в игре во flash не пишем");

      }
      printf("   диag: просадка: стираний %d, записей %d, операций %d (ждали 0 и 0)\n",
             g_flashErases - er0, g_flashPrograms - pr0, g_flashOps - ops0);
      chk(g_flashErases == er0 && g_flashPrograms == pr0 && g_flashOps == ops0,
          "просадка: обработчик НЕ трогает flash (главная причина сброса по RUN устранена)");
      chk(blkRamN > 0, "просадка: отметки живут в кольце ОЗУ (во flash не лезем)");
      chk(gDipEvents >= 1, "просадка: счётчик просадок растёт (уйдёт в ящик при выключении)");
      // v8.9q: главное — чтобы и ПОСЛЕ просадки в игре во flash ничего не писалось

      // в записи чёрного ящика счётчик просадок и отложенные автозаписи упакованы в v[42]
      {
        uint32_t v[BLK_FIELD];
        blkPrepare(v);
        printf("   диag: в ящике: просадок %u, отложенных автозаписей %u\n",
               (unsigned)((v[42] >> 1) & 0xFFFFu), (unsigned)((v[42] >> 17) & 0x7FFFu));
        chk(((v[42] >> 1) & 0xFFFFu) >= 1, "просадка: в записи ящика видно, сколько раз она была");
      }
      blkWrite();                       // запись в ящик — вот она окно и закрывает
      chk(gDipMin == 4095, "просадка: окно (мин/макс) закрывает запись в чёрный ящик, а не доклад");
      (void)dipMinBefore;
    }
    adcSetAll(1450); battRead(); gDipWorst = 4095; gFramesTotal = frSave;

    // (5) строка статуса показывает просадки
    sv = captureStart(); g_serialEcho = true; printStatus();
    log = captureStop(sv); g_serialEcho = false;
    chk(has(log, "просадка за сеанс"), "ловушка просадки: статистика 's' показывает просадку");
    // v8.9r: в логе должна быть НАСТОЯЩАЯ версия сборки (было: баннер печатал v8.9p,
    // хотя внутри работал v8.9q — путаница в отчётах), и температура чипа в градусах
    chk(has(log, "сборка " GBEMU_BUILD), "статистика 's' печатает версию сборки");
    chk(has(log, "нагрев чипа"),
        "статистика 's' печатает нагрев чипа (перегрев — главный риск разгона)");
    {
      // v8.9r: проверяем саму формулу даташита T = 27 - (V - 0,706)/0,001721.
      // 838 отсчёта = 675 мВ -> 45 °C; 877 отсчётов = 706 мВ -> 27 °C (округление до целых °C).
      adcSetCh(4, 838); battRead();
      const int tc45 = tempDegC();
      adcSetCh(4, 877); battRead();
      const int tc27 = tempDegC();
      printf("   диag: температура: 675 мВ -> %d °C (ждали 45), 706 мВ -> %d °C (ждали 27)\n",
             tc45, tc27);
      chk(tc45 == 45 && tc27 == 27, "температура чипа считается по формуле даташита (45 и 27 °C)");
      adcSetCh(4, 838); battRead();                     // 675 мВ — самый тёплый замер теста
      int svT = captureStart(); g_serialEcho = true; printStatus();
      std::string logT = captureStop(svT); g_serialEcho = false;
      {
        const size_t p = logT.find("нагрев");
        printf("─── температура в статусе (v8.9t) ───\n%s─── конец ───\n",
               (p == std::string::npos) ? "(нагрева в логе нет!)" : logT.substr(p, 120).c_str());
      }
      // v8.9t: печатаем НАГРЕВ относительно базы (самого холодного замера сеанса).
      // Датчик этой платы врёт на ~140 мВ, абсолютной цифре верить нельзя, а росту — можно.
      chk(has(logT, "нагрев чипа +"), "в статусе печатается НАГРЕВ чипа (а не врущее абсолютное значение)");
      chk(has(logT, "база ") && has(logT, "мВ"), "и видно базу датчика, от которой считаем");
      // и бессмыслица датчика не попадает в лог как «температура»
      adcSetCh(4, 1450); battRead();
      chk(tempDegC() == 999, "если датчик выдаёт бессмыслицу — так и говорим, а не пугаем цифрой");
      adcSetCh(4, 880); battRead();
    }

    // (5б) v9.2: АВТОСБРОС НАГРУЗКИ УДАЛЁН — батареей яркость/частоту не трогаем вообще.
    // Просадку ловушка видит (gShedAsk), но никаких ступеней частоты больше нет.
    printf("   диag: автосброс нагрузки (v9.2): shedLoad — заглушка, инд частоты %d\n", (int)spiHzIdx);
    {
      const uint8_t idx0 = spiHzIdx; const uint32_t baud0 = g_spiBaud;
      shedLoad(true); shedLoad(true); shedTick();
      chk(spiHzIdx == idx0 && g_spiBaud == baud0,
          "v9.2: shedLoad/shedTick ничего не трогают (автосброс удалён, частота панели как была)");
    }
    // v8.9q: имена частот должны совпадать с таблицей (раньше были сдвинуты и лог врал)
    chk(!strcmp(SPI_HZ_NAME[0], "125") && !strcmp(SPI_HZ_NAME[1], "62,5")
        && !strcmp(SPI_HZ_NAME[2], "41,7") && !strcmp(SPI_HZ_NAME[3], "20,8"),
        "частоты панели: имя в логе совпадает с настоящей частотой (125/62,5/41,7/20,8)");
    chk(SPI_HZ_TAB[0] == 125000000UL && SPI_HZ_TAB[1] == 62500000UL,
        "частоты панели: таблица частот на месте (125 -> 62,5)");

    {   // клавиша a теперь только ЧЕСТНО сообщает, что автосброса больше нет
      int sv2 = captureStart(); g_serialEcho = true;
      const bool autoBefore = gShedAuto;
      harnessSend("a"); handleSerial();
      std::string log2 = captureStop(sv2); g_serialEcho = false;
      chk(has(log2, "автосброс") && has(log2, "удалён"),
          "клавиша a: сказано, что автосброс удалён (а не тихо ничего не делать)");
      chk(has(log2, "яркость не трогаем"),
          "клавиша a: объяснено новое правило — батареей яркость не трогаем");
      chk(gShedAuto == autoBefore, "клавиша a: никаких скрытых переключателей не осталось");
    }

    // v8.9q: и через 3 с после просадки, когда питание уже выправилось, игра во flash
    // по-прежнему НЕ пишет. Раньше здесь была выгрузка ящика — и именно она (стирание
    // сектора в игре) роняла консоль: все пять сбросов в логе оборвались на ней.
    {
      adcSetCh(3, 2339); battRead(); gDipWarn = false;
      cartDirty = false;                                // чтобы не влез автосейв и не мешал счёту
      const int er0 = g_flashErases, pr0 = g_flashPrograms, ops0 = g_flashOps;
      for (int i = 0; i < 3; i++) {
        harnessAdvanceMs(1000);
        int sv3 = captureStart(); loop(); captureStop(sv3);
      }
      printf("   диag: через 3 с после просадки: стираний %d, записей %d, операций %d\n",
             g_flashErases - er0, g_flashPrograms - pr0, g_flashOps - ops0);
      chk(g_flashErases == er0 && g_flashPrograms == pr0 && g_flashOps == ops0,
          "просадка: через 3 с после неё игра по-прежнему НЕ пишет во flash (это и убивало консоль)");
    }
    // --- v8.9r: просадка — считается по двум замерам (сама ловушка осталась) ---
    {
      // (а) одиночный выброс АЦП панель НЕ роняет (шум — не просадка)
      adcSetCh(3, 2339); battRead(); gDipWarn = false;
      gShedAsk = false; gShedLastMs = 0; gShedAuto = true; gShedSteps = 0;
      gDipLowRun = 0;
      for (int i = 0; i < 8; i++) g_timerCb(&g_timer);          // один замер в норме
      adcSetCh(3, 2100);                                        // ~3,39 В: короткий выброс
      for (int i = 0; i < 16; i++) g_timerCb(&g_timer);          // ~2 замера — доли мс
      printf("   диag: короткий выброс (2 замера): просьба сбросить нагрузку=%d, подряд=%u\n",
             (int)gShedAsk, (unsigned)gDipLowRun);
      chk(!gShedAsk, "короткий выброс АЦП: сигнал не поднимаем (это шум, а не просадка)");
      adcSetCh(3, 2339); for (int i = 0; i < 16; i++) g_timerCb(&g_timer);   // отпустило
      adcSetCh(3, 2100);                                        // теперь ДЛИТЕЛЬНАЯ просадка
      for (int i = 0; i < 150; i++) g_timerCb(&g_timer);         // >= 10 мс подряд
      printf("   диag: длительный низкий участок: просьба=%d, подряд=%u\n",
             (int)gShedAsk, (unsigned)gDipLowRun);
      chk(gShedAsk, "низкий участок >= 10 мс: вот это просадка — флаг взведён (см. журнал)");
      gShedAsk = false;

      // (г) v9.2: сколько ни жди — частота панели сама НЕ меняется (автосброса нет)
      adcSetCh(3, 2339); battRead(); gDipWarn = false;
      const uint8_t idx0 = spiHzIdx; const uint32_t baud0 = g_spiBaud;
      gShedSteps = 1; gShedLastMs = 0; gDipLastMs = 0;
      gShedHealthyMs = 0; gShedBlockMs = 0; gShedUps = 0;
      harnessAdvanceMs(SHED_QUIET_MS + 1000);
      shedTick();
      harnessAdvanceMs(SHED_QUIET_MS + 1000);
      shedTick();
      chk(spiHzIdx == idx0 && g_spiBaud == baud0,
          "v9.2: частота панели не меняется ни при каком раскладе (автосброс удалён)");
      chk(gShedUps == 0, "v9.2: счётчик «возвратов» не крутится (возвращать нечего)");
    }

    // --- v8.9s: проверка пользы больше не нужна — ступеней нет ---
    {
      adcSetCh(3, 2339); battRead(); gDipWarn = false;
      gShedAuto = true; gShedUseless = false; gShedCheckMs = 0;
      gShedSteps = 0; gShedHealthyMs = 0; gShedBlockMs = 0;
      const uint8_t idx0 = spiHzIdx;
      shedLoad(true); shedLoad(true); shedLoad(true);
      chk(spiHzIdx == idx0, "v9.2: сколько просадок ни дай — ступеней частоты нет вообще");
      chk(!gShedUseless, "v9.2: флаг «не помогло» не взводится (нечего проверять)");

      // (з) частота панели видна в статусе — иначе непонятно, откуда 76 %
      spiHzIdx = 1; spiSetClock(SPI_HZ_TAB[1]);
      { int sv6 = captureStart(); g_serialEcho = true; printStatus();
        std::string l6 = captureStop(sv6); g_serialEcho = false;
        chk(has(l6, "панель 62,5 МГц"), "в статусе рядом с процентами печатается частота панели"); }
      spiHzIdx = 0; spiSetClock(SPI_HZ_TAB[0]);
    }

    // --- v8.9t: помеха АЦП, дефолты и спрятанная проба звука ---
    {
      // (и) «просадка до 1,12 В» из вашего лога — это помеха, а не батарея
      adcSetCh(3, 2339); battRead(); gDipWarn = false; gDipEvents = 0;
      gDipMin = 4095; gDipMax = 0; gDipWorst = 4095;
      gDipRunStartMs = 0; gDipRunMin = 4095;
      adcSetCh(3, 300);                                    // 1,1 В — чип бы уже не работал
      for (int i = 0; i < 150; i++) g_timerCb(&g_timer);
      printf("   диag: отсчёты 300 (1,1 В) в течение 18 замеров: мин=%u, провал=%u, флаг=%d\n",
             (unsigned)gDipMin, (unsigned)gDipWorst, (int)gDipWarn);
      chk(gDipMin == 4095 && gDipWorst == 4095,
          "помеха ниже 2,0 В не попадает в статистику (в отчёте не будет «провала до 1,1 В»)");
      chk(!gDipWarn, "помеха ниже 2,0 В не поднимает тревогу «просадка питания»");
      adcSetCh(3, 2339); battRead();

      // (л) проба звука спрятана: одно нажатие z только предупреждает
      ui = UI_GAME; emuInited = true; probeArmMs = 0;
      {
        int sv7 = captureStart(); g_serialEcho = true;
        harnessSend("z"); handleSerial();
        std::string l7 = captureStop(sv7); g_serialEcho = false;
        printf("─── z один раз (v8.9t) ───\n%s─── конец ───\n", l7.c_str());
        chk(ui != UI_TONE, "одно нажатие z пробу НЕ запускает (случайно не нажмёте)");
        chk(has(l7, "СПРЯТАНА") && has(l7, "ещё раз"), "и сказано, как её всё-таки запустить");
      }
      {
        int sv8 = captureStart(); g_serialEcho = true;
        harnessSend("z"); handleSerial();
        std::string l8 = captureStop(sv8); g_serialEcho = false;
        chk(ui == UI_TONE, "второе нажатие z в течение 5 с пробу запускает");
        chk(has(l8, "ресет"), "и сразу сказано, что для выхода ресет не нужен");
      }
      // выход кнопкой B (bit 5) — раньше выход был только клавишей в терминале
      {
        int sv9 = captureStart(); g_serialEcho = true;
        toneTick((uint16_t)(1u << 5));
        std::string l9 = captureStop(sv9); g_serialEcho = false;
        printf("─── выход из пробы кнопкой B ───\n%s─── конец ───\n", l9.c_str());
        chk(ui != UI_TONE, "из пробы звука есть выход КНОПКОЙ (A/B) — зубочистка не нужна");
      }
      // и авто-выход, если пробу оставили без внимания
      ui = UI_GAME; probeArmMs = 0;
      harnessSend("z"); handleSerial(); harnessSend("z"); handleSerial();
      chk(ui == UI_TONE, "проба снова запущена (для проверки авто-выхода)");
      probeLastMs = 1;                                     // как будто её оставили давно
      harnessAdvanceMs(21000);
      {
        int svA = captureStart(); g_serialEcho = true;
        toneTick(0);
        std::string lA = captureStop(svA); g_serialEcho = false;
        chk(ui != UI_TONE, "проба кончается САМА после 20 с без действий");
        chk(has(lA, "сама"), "и говорит об этом в логе");
      }
    }

    // возвращаем всё как было (дальше идут проверки про частоты и загрузку)
    gShedAuto = true; gShedSteps = 0; gShedHealthyMs = 0; gShedBlockMs = 0;
    gShedUseless = false; gShedCheckMs = 0;
    gDipRunStartMs = 0; gDipRunMin = 4095;
    spiHzIdx = 0; spiSetClock(SPI_HZ_TAB[0]);

    // (5в) ОХРАНА ПИТАНИЯ: при низкой батарее автоматические записи откладываются
    {
      adcSetCh(3, 2000); battRead();                      // 2000 отсч. = 3,22 В — ниже порога
      printf("   диag: охрана: батарея %u (порог %d) -> battGuardOk=%d\n",
             (unsigned)battBRaw, BATT_GUARD_RAW, (int)battGuardOk());
      chk(!battGuardOk(), "охрана питания: при просевшей батарее автозаписи во flash запрещены");
      const int er0 = g_flashErases;
      blkFlushGuarded();
      chk(g_flashErases == er0, "охрана питания: выгрузка ящика при этом действительно не идёт");
      adcSetCh(3, 2339); battRead();
      chk(battGuardOk(), "охрана питания: при нормальной батарее записи снова разрешены");
    }

    // (5г) ОКНО ПРОСАДКИ после запуска игры считается и печатается
    {
      gSagT0 = millis(); gSagMin = 4095; gSagMax = 0; gSagDone = false;
      adcSetCh(3, 2100);                                 // под нагрузкой 2100
      for (int i = 0; i < 24; i++) g_timerCb(&g_timer);
      harnessAdvanceMs(SAG_WINDOW_MS + 50);
      adcSetCh(3, 2339);
      int sv2 = captureStart(); g_serialEcho = true;
      loop();
      std::string log2 = captureStop(sv2); g_serialEcho = false;
      printf("─── мерка просадки ───\n%s─── конец ───\n", log2.c_str());
      chk(has(log2, "питание после запуска игры"), "мерка просадки: доклад есть в логе");
      chk(has(log2, "просадка"), "мерка просадки: напечатано, на сколько просело");
      chk(gSagDone, "мерка просадки: окно закрылось (второй раз не печатается)");
    }

    // (6) прерывание НЕ ждёт АЦП: читает через регистры, а не через adc_read()
    {
      const int reads0 = g_adcReads;
      gDipMin = 4095; gDipMax = 0;                   // окно с нуля: проверяем именно эти отсчёты
      adcSetCh(3, 2400); gDipSkip = 0;               // v8.9t: пропуск после перебора — не мешать
      for (int i = 0; i < 64; i++) g_timerCb(&g_timer);   // 64 тика = 8 замеров (4 первых — пропуск)
      printf("   диag: неблокирующее чтение: adc_read вызван %d раз (ждали 0), мин окна=%u (ждали 2400)\n",
             g_adcReads - reads0, (unsigned)gDipMin);
      chk(g_adcReads == reads0,
          "ловушка просадки: в прерывании АЦП читается БЕЗ ожидания (adc_read не вызывается — залипнуть нечему)");
      chk(gDipMin == 2400, "ловушка просадки: неблокирующее чтение даёт верный отсчёт батареи (канал A3)");
      chk(g_adcStarts > 0, "ловушка просадки: преобразование в самом деле запускается (START_ONCE)");
    }

    // (7) во время перебора каналов ловушка молчит — иначе переставит АЦП на свой канал
    {
      const uint16_t mn0 = gDipMin;
      gAdcSweep = true;
      for (int i = 0; i < 32; i++) g_timerCb(&g_timer);
      chk(gDipMin == mn0, "ловушка просадки: во время перебора каналов (battRead) молчит");
      adcSetCh(4, 880);                              // термодатчик — последний в переборе
      gDipSkip = 1; gAdcSweep = false;               // как это делает battRead()
      for (int i = 0; i < 16; i++) g_timerCb(&g_timer);
      printf("   диag: после перебора каналов мин окна=%u (ждали 2400, а не термодатчик 880)\n",
             (unsigned)gDipMin);
      chk(gDipMin == 2400,
          "ловушка просадки: после перебора каналов читает именно батарею (чужой отсчёт пропущен)");
    }

  }

  // --- v8.9h: режимы при включении (вынесены в applyBootModes, чтобы проверять) ---
  {
    // RIGHT при включении = тихая панель
    g_lowPins |= (1u << BTN_GPIO[3]);
    quietPanel = false;
    int sv = captureStart(); g_serialEcho = true;
    applyBootModes();
    std::string log = captureStop(sv); g_serialEcho = false;
    g_lowPins &= ~(1u << BTN_GPIO[3]);
    printf("─── режим «тихая панель» ───\n%s─── конец ───\n", log.c_str());
    chk(quietPanel, "режим RIGHT: выставлен флаг «тихая панель»");
    chk(has(log, "ТИХАЯ ПАНЕЛЬ") && has(log, "20,8 МГц"), "режим RIGHT: объяснено словами, что включено");
    // и что режим реально меняет частоту SPI
    quietPanel = false;
    chk(SPI_HZ_TAB[3] == 20833333UL, "режим RIGHT: 20,8 МГц — самая мягкая частота панели");
  }


  // --- v8.9f: причина сброса от самого чипа (HAD_POR / HAD_RUN / HAD_PSM) ---
  {
    // 1) «питание»: HAD_POR выставлен
    gHostChipReset = (1u << 8);
    int sv = captureStart(); g_serialEcho = true;
    bootReport();
    std::string log = captureStop(sv); g_serialEcho = false;
    printf("--- отчёт о причине сброса (питание) ---\n%s--- конец ---\n", log.substr(0, 500).c_str());
    chk(has(log, "причина сброса из чипа") && has(log, "HAD_POR=1"),
        "причина сброса: печатает регистр чипа (HAD_POR=1)");
    chk(has(log, "питание ДЕЙСТВИТЕЛЬНО проседало"), "причина сброса: при HAD_POR прямо говорит про питание");
    // 2) «сброс по выводу RUN»
    gHostChipReset = (1u << 16);
    sv = captureStart(); g_serialEcho = true; bootReport();
    log = captureStop(sv); g_serialEcho = false;
    chk(has(log, "HAD_RUN=1") && has(log, "выводу RUN"), "причина сброса: отличает сброс по выводу RUN");
    // 3) пусто — свежее включение без отметок
    gHostChipReset = 0;
    sv = captureStart(); g_serialEcho = true; bootReport();
    log = captureStop(sv); g_serialEcho = false;
    chk(has(log, "HAD_POR=0") && has(log, "свежее включение"), "причина сброса: при пустом регистре говорит про свежее включение");
    gHostChipReset = (1u << 8);
    // 4) и попадает в чёрный ящик
    gFramesTotal = 3; blkWriteFlash(); gFramesTotal = 0;
    const uint32_t *p = (const uint32_t *)(flashBase + blkSlotOff((uint32_t)blkNewestSlot));
    chk(p[44] == gHostChipReset, "причина сброса: регистр чипа записан в чёрный ящик");
  }

  // --- v8.9e: разбор в логе — «записи о снятии нет, значит питание ушло снаружи» ---
  {
    blkWipe();                                            // свой, чисто однозначный след
    gFramesTotal = 7; mark(PH_TRACE); adcSetAll(1400); battRead(); blkWriteFlash(); gFramesTotal = 0;
    int sv = captureStart(); g_serialEcho = true;
    blkRead();
    std::string log = captureStop(sv); g_serialEcho = false;
    printf("--- разбор чёрного ящика (как увидит пользователь) ---\n%s--- конец ---\n",
           log.substr(0, 700).c_str());
    chk(has(log, "сторож питания в последней записи"), "разбор ящика: печатает состояние питания из записи");
    chk(has(log, "записи о снятии питания НЕТ"), "разбор ящика: прямо говорит, что питание ушло НЕ по вине прошивки");
    chk(has(log, "при включении было"), "разбор ящика: сравнивает с замером при включении");
  }

  // --- v8.9: сторож кормится уже в setup и его остаток виден ---
  chk(g_wdFeeds > 20, "сторож кормится и во время setup (заставка больше не «некормленая» пауза)");
  chk(watchdog_get_count() > 0 && watchdog_get_count() < 4000000u,
      "прибор сторожа: видно, сколько микросекунд оставалось до сброса");
  { int sv = captureStart(); g_serialEcho = true;
    printStatus();
    std::string log = captureStop(sv); g_serialEcho = false;
    chk(has(log, "питание A0=") && has(log, "сторожу оставалось"), "в строке статуса есть и питание (каналы АЦП), и остаток сторожа");
  }

  // --- v8.9: поля питания в чёрном ящике ---
  {
    adcSetAll(1300); battRead();
    gFramesTotal = 5; blkWriteFlash(); gFramesTotal = 0;
    const uint32_t *p = (const uint32_t *)(flashBase + blkSlotOff((uint32_t)blkNewestSlot));
    printf("   диag: в записи ящика питание %u мВ (ждали %u), сторожу оставалось %u мс\n",
           (unsigned)p[26], (unsigned)battAMv, (unsigned)(p[25] / 1000));
    chk(p[26] == battAMv && p[24] == battARaw, "чёрный ящик: пишет питание в момент записи");
    chk(p[25] > 0 && p[25] < 4000000u, "чёрный ящик: пишет, сколько оставалось сторожу до сброса");
    adcSetAll(1450); battRead();
  }

    // (6e) v8.8: ловушка аппаратного сбоя — таблица векторов переставлена в ОЗУ,
    // в ней подменён вектор HardFault (иначе M0+ молча замирает на сбое).
    {
      chk(gHostVtor == 1, "ловушка сбоя: таблица векторов переставлена (VTOR указывает на нашу копию)");
      chk(gHostVtorSrc() == gVecSave, "ловушка сбоя: активная таблица — наша копия в ОЗУ");
      chk(gVecSave[16] == gHostVec[16] && gVecSave[11] == gHostVec[11],
          "ловушка сбоя: остальные векторы скопированы как были (ничего не сломали)");
      chk(gVecSave[3] == (((uint32_t)(uintptr_t)&gbHardFaultEntry) | 1u),
          "ловушка сбоя: вектор HardFault ведёт на наш обработчик (с битом Thumb)");
      chk(gVecSave[2] == gVecSave[3], "ловушка сбоя: NMI идёт тем же путём");
    }

    // (6f) v8.8: сам отчёт о сбое — причина, адрес, ящик, снятие питания
    {
      gDieA = gDieB = gDieC = gDieD = 0;
      gHostCfsr = (1u << 10);                    // «запись в запретную область»
      gHostHfsr = 0; gHostBfar = 0x1FD00000u; gHostMmfar = 0;
      gFramesTotal = 77;
      uint32_t frame[8] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0xDEADBEEFu, 0x1000ABCDu, 0x01000000u };
      g_powerCut = false;
      const bool cut0 = g_powerCut;
      gFaultLr = frame[5]; gFaultPc = frame[6];
      int sv = captureStart(); g_serialEcho = true;
      dieReport(PH_FAULT, gHostCfsr, gFaultPc, gHostBfar, gHostHfsr);   // как если бы сбой
      std::string log = captureStop(sv); g_serialEcho = false;
      printf("--- отчёт о сбое (как увидит пользователь) ---\n%s--- конец ---\n", log.c_str());
      chk(has(log, "АППАРАТНЫЙ СБОЙ"),                      "сбой назван своим именем");
      chk(has(log, "запись в запретную область"),           "причина сбоя объяснена словами");
      chk(has(log, "адрес сбоя 0x1000ABCD"),                "в логе есть АДРЕС сбоя (по карте видно, где)");
      chk(has(log, "кадр 77"),                              "в отчёте видно, на каком кадре это случилось");
      chk(g_powerCut && !cut0, "при сбое консоль гасится сама (иначе сторож убьёт питание и мы ничего не увидим)");
      const uint32_t *p = (const uint32_t *)(flashBase + blkSlotOff((uint32_t)blkNewestSlot));
      chk(p[0] == 0x314B4C42UL && p[1] == PH_FAULT && p[21] == 0x1000ABCDu,
          "подробности сбоя записаны в чёрный ящик (адрес переживёт выключение)");
      gFramesTotal = 0;
      g_powerCut = false;                          // смерть была «понарошку»: снимаем пометки
      watchdog_hw->scratch[5] = 0; watchdog_hw->scratch[3] = 0;
    }

    // (6h) v8.9d: выравнивание буфера строки (невыровненная 32-битная запись на
    // M0+ — аппаратный сбой; санитайзер поймал это на стенде) + свидетели стека.
    {
      chk(((uintptr_t)tx_rowbuf % 4u) == 0u,
          "буфер строки панели выровнен на 4 байта (иначе на M0+ сбой на 32-битной записи)");
      chk(ROW_BYTES % 4u == 0u, "длина строки кратна 4 (буфер объявлен как uint32_t[])");
      chk(((uintptr_t)uiRowBuf % 4u) == 0u, "буфер строк интерфейса тоже выровнен");
      // свидетели стека: заполняются на входе в обработчик сбоя
      uint32_t fr[8] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0xBEEFBEEFu, 0x200001E4u, 0x01000000u };
      gFaultSp = gFaultMsp = gFaultPsp = gFaultXpsr = 0;
      int sv = captureStart(); g_serialEcho = true;
      gHostCfsr = 0; gHostHfsr = 0;                 // «ни одного бита сбоя» — как было на устройстве
      (void)gbHardFaultC(fr, 0xFFFFFFF9u);
      std::string log = captureStop(sv); g_serialEcho = false;
      printf("--- отчёт при CFSR=0 (как на устройстве) ---\n%s--- конец ---\n", log.c_str());
      chk(gFaultPc == fr[6] && gFaultLr == fr[5] && gFaultXpsr == fr[7],
          "обработчик сбоя запоминает PC/LR/xPSR из кадра исключения");
      chk(gFaultSp != 0, "обработчик сбоя запоминает SP (по нему видно, был ли стек плохой)");
      chk(has(log, "стек: SP=0x") && has(log, "xPSR=0x"), "в отчёте печатаются свидетели стека");
      const uint32_t *p = (const uint32_t *)(flashBase + blkSlotOff((uint32_t)blkNewestSlot));
      chk(p[28] == gFaultSp && p[31] == gFaultXpsr, "свидетели стека уходят в чёрный ящик");
      g_powerCut = false; watchdog_hw->scratch[5] = 0; watchdog_hw->scratch[3] = 0;
    }

    // (6g) v8.8: детектор залипания — цикл встал, а прерывание звука живо
    {
      gBeatArm = true; gStalled = false; gBeat = micros() - (STALL_US + 1000);
      g_powerCut = false;
      const bool cut0 = g_powerCut;
      int sv = captureStart(); g_serialEcho = true;
      pumpSamples(1);                            // «прошло» ещё одно прерывание звука
      std::string log = captureStop(sv); g_serialEcho = false;
      printf("--- отчёт о залипании (как увидит пользователь) ---\n%s--- конец ---\n", log.c_str());
      chk(has(log, "ЦИКЛ ЗАЛИП"), "залипание поймано: сказано, что цикл встал, а звук шёл");
      chk(has(log, "это не сбой"), "объяснено, что это НЕ аппаратный сбой");
      chk(has(log, "фаза "),      "в отчёте есть фаза (видно, где именно встали)");
      chk(gStalled,               "второй раз тревога не поднимается (без повторов)");
      chk(g_powerCut && !cut0,    "при залипании консоль гасится сама, но лог уже напечатан");
      const uint32_t *p = (const uint32_t *)(flashBase + blkSlotOff((uint32_t)blkNewestSlot));
      chk(p[1] == PH_STALL,       "залипание записано в чёрный ящик");
      gBeat = micros(); gBeatArm = false; gStalled = false;
      g_powerCut = false;
      watchdog_hw->scratch[5] = 0; watchdog_hw->scratch[3] = 0;
    }
    g_data.clear();
    frameStart(); rowBl(4); frameFinish();
    chk(g_data.size() == 4UL * 240UL * 2UL, "после перезапуска шины строки снова уходят в панель");

    // (6) тест панели (клавиша j): полосы оттенков и ровно 240 строк
    g_data.clear(); const uint64_t dr1 = g_drBytes;
    { int sv = captureStart(); g_serialEcho = true;
      panelTest();
      std::string log = captureStop(sv); g_serialEcho = false;
      printf("--- тест панели (как увидит пользователь) ---\n%s--- конец ---\n", log.c_str());
      chk(has(log, "ТЕСТ ПАНЕЛИ"), "тест панели: в логе понятное сообщение");
      chk(has(log, "Мбит/с"), "тест панели: печатает НАСТОЯЩУЮ скорость шины в Мбит/с");
      chk(has(log, "CPSR") && has(log, "SCR"), "тест панели: печатает делители SPI из регистров (правда о шине)");
      chk(has(log, "строк ушло не 240") == false, "тест панели: все 240 строк ушли");
      chk(has(log, "Если полос не видно"), "тест панели: сказано, что делать, если полос не видно");
    }
    chk(g_drBytes - dr1 == 240ULL * ROW_BYTES, "тест панели: 240 строк по 480 байт");
    {
      bool bands = (g_data.size() == 240UL * 240UL * 2UL);
      printf("   диag: тест панели: байт %zu, упало в панель %llu, оттенки полос %04X %04X %04X %04X\n",
             g_data.size(), (unsigned long long)(g_drBytes - dr1),
             uiColSw[0], uiColSw[1], uiColSw[2], uiColSw[3]);
      if (!bands) printf("   ! байты в панель не попали в модель (проверю ниже по FIFO-счётчику)\n");
      for (int b2 = 0; b2 < 4; b2++) {
        // uiColSw[] уже в порядке отправки (байты переставлены), а g_data мы
        // собираем в «человеческом» порядке (старший байт первым) — сравниваем так же
        const uint16_t want = (uint16_t)((uiColSw[b2] >> 8) | (uiColSw[b2] << 8));
        for (int r = b2 * 60; r < (b2 + 1) * 60; r++) {
          if (r * 240 * 2 + 1 >= (int)g_data.size()) break;
          const uint16_t got = (uint16_t)((g_data[(r * 240) * 2] << 8) | g_data[(r * 240) * 2 + 1]);
          if (got != want) { bands = false;
            printf("   ! полоса %d строка %d: получено %04X, ждали %04X\n", b2, r, got, want); break; }
        }
      }
      chk(bands, "тест панели: четыре полосы оттенков ушли ровно как задумано");
    }

    // (6b) ГЛАВНЫЕ ГРАБЛИ v8.5: сколько байт уходит в FIFO за одну запись.
    // FIFO 16-битная, одна запись = одна запись FIFO; при 8 битах на кадр
    // сдвигаются младшие 8 бит. Значит запись словом отправляет ОДИН байт.
    {
      const uint64_t b0 = g_drBytes;
      spi_get_hw(spi0)->dr = 0xAABBCCDDu;        // «как в v8.5»
      const uint64_t one = g_drBytes - b0;
      chk(one == 1, "запись 32-битным словом = ОДИН байт в панель (на этом v8.5 и потерял 3/4 кадра)");
      chk(g_lastDr == 0xAABBCCDDu && (uint8_t)g_lastDr == 0xDD,
          "в панель уходит младший байт записи (0xDD) — старшие биты FIFO отбрасывает");
      g_data.clear(); const uint64_t b1 = g_drBytes;
      uint8_t three[3] = { 0x11, 0x22, 0x33 };
      frameStart(); txRow(three, 3); frameFinish();
      chk(g_drBytes - b1 == 3 && g_data.size() == 3, "байтовые записи дают ровно столько байт, сколько нужно");
    }

    // (7) разбор кадра (клавиша n): видно, что отдаёт ядро и уходят ли строки
    { int sv = captureStart(); g_serialEcho = true;
      drawDiagStart();
      frameStart(); rowBl(GAME_TOP);
      for (int L = 0; L < 144; L++) cb_lcd_line(&gb, px2[L], (uint_fast8_t)L);
      rowBl(MEM_H - (GAME_TOP + GAME_ROWS)); frameFinish();
      std::string log = captureStop(sv); g_serialEcho = false;
      printf("--- разбор кадра (начало и место с ЖК-состоянием) ---\n%s--- конец ---\n",
             (log.size() > 1400 ? log.substr(0, 700) + "\n...\n" + log.substr(log.find("стр 24"), 700) : log).c_str());
      chk(has(log, "стр 0: л="), "разбор кадра: видно, сколько точек и слоёв отдаёт ядро");
      chk(has(log, "з=12"), "разбор кадра: видно, что строки УХОДЯТ в панель (з=12 после верхней полосы)");
      chk(has(log, "ЖК: LCDC=0x"), "разбор кадра: видно состояние ЖК-контроллера игры (LCDC/STAT/LY/PC)");
    }

    // (8) чёрный ящик: запись во flash и чтение после перезагрузки
    blkWrites = 0;
    gPhase = 3;                                  // «ядро эмулятора»
    blkWriteFlash();                             // отметка + выгрузка во flash
    {
      const uint32_t *p = (const uint32_t *)(flashBase + blkSlotOff((uint32_t)blkNewestSlot));
      chk(p[0] == 0x314B4C42UL && p[1] == 3u, "чёрный ящик: запись во flash прошла (магия и фаза)");
      chk(p[16] + 1u == blkSeq, "ящик: номер записи сквозной (новая всегда новее старых)");
      chk(blkNewestSlot >= 0 && blkNewestSlot < BLK_SLOTS, "ящик: самая свежая запись находится по номеру");
    }
    { int sv = captureStart(); g_serialEcho = true;
      blkRead();
      std::string log = captureStop(sv); g_serialEcho = false;
      printf("--- чёрный ящик (как увидит пользователь) ---\n%s--- конец ---\n", log.c_str());
      chk(has(log, "чёрный ящик") && has(log, "ядро эмулятора"), "чёрный ящик: после перезагрузки видно, где были");
      chk(has(log, "PC=0x") && has(log, "SP=0x"),
          "чёрный ящик: хранит PC и SP игры (по ним видно зависание внутри ядра)");
      chk(has(log, "байт в панель"), "чёрный ящик: хранит, сколько байт успело уйти в панель");
    }
    gPhase = PH_POWERCUT;
    blkWriteFlash();
    { int sv = captureStart(); g_serialEcho = true;
      blkRead();
      std::string log = captureStop(sv); g_serialEcho = false;
      chk(has(log, "ШТАТНОЕ выключение"), "чёрный ящик: штатное выключение не путается с зависанием");
    }
    gPhase = PH_LOOP;
  }

  // --- (8b) ЛОВУШКА v8.7: след по кадрам в чёрном ящике ---
  // (проверяем механику напрямую: эмулятор здесь не гоняем — важно, что на каждый
  // из первых 12 кадров в ящик ложится ДВЕ записи: перед ядром и после него)
  {
    blkWipe();                                   // как при загрузке: чистая площадка
    const uint32_t erase0 = blkErases;
    for (int i = 1; i <= BLK_TRAP; i++) {
      gFramesTotal = (uint32_t)i;
      gPhase = PH_CORE;   blkTrap();
      gPhase = PH_FINISH; blkTrap();
    }
    // v8.9k: во время игры отметки копятся в КОЛЬЦЕ ОЗУ (во flash не лезем), поэтому
    // смотрим, что именно лежит в кольце, и что оно уходит во flash при выгрузке.
    printf("   диag: ловушка: в кольце ОЗУ %u отметок (ждали %d — 2 на кадр × %d кадров), стираний за опыт %u\n",
           (unsigned)blkRamN, 2 * BLK_TRAP, BLK_TRAP, (unsigned)(blkErases - erase0));
    chk(blkRamN == BLK_RAM_N, "ловушка: отметки кадров копятся в кольце ОЗУ (16 последних)");
    {
      // самая свежая отметка в кольце — «конец кадра» последнего кадра трапа
      const uint32_t idx = (blkRamHead + BLK_RAM_N - 1) % BLK_RAM_N;
      printf("   диag: последняя отметка кольца: кадр %u, фаза %u («%s»)\n",
             (unsigned)blkRam[idx][2], (unsigned)blkRam[idx][1], phaseName((int)blkRam[idx][1]));
      chk(blkRam[idx][2] == BLK_TRAP && blkRam[idx][1] == PH_FINISH,
          "ловушка: в кольце — конец последнего из первых 12 кадров (видно, где встали)");
      int coreMarks = 0, finMarks = 0;
      for (uint32_t k = 0; k < BLK_RAM_N; k++) {
        if (blkRam[k][1] == PH_CORE)   coreMarks++;
        if (blkRam[k][1] == PH_FINISH) finMarks++;
      }
      printf("   диag: в кольце метки «ядро» %d, «конец кадра» %d\n", coreMarks, finMarks);
      chk(coreMarks > 0, "ловушка: есть отметки ДО ядра (по ним видно, если встали в ядре)");
      chk(finMarks > 0, "ловушка: есть отметки ПОСЛЕ ядра (по ним видно, что кадр доехал)");
    }
    // и всё это уходит во flash при выгрузке (выключение) — без стираний в первые кадры
    blkFlush(true);
    int total = 0, coreMarks = 0, finMarks = 0, frames = 0;
    int seen[BLK_TRAP + 1] = {0};
    for (uint32_t sl = 0; sl < BLK_SLOTS; sl++) {
      const uint32_t *p = (const uint32_t *)(flashBase + blkSlotOff(sl));
      if (p[0] != 0x314B4C42UL) continue;
      total++;
      if (p[1] == PH_CORE) coreMarks++;
      if (p[1] == PH_FINISH) finMarks++;
      if (p[2] >= 1 && p[2] <= BLK_TRAP) { seen[p[2]] = 1; }
    }
    for (int i = 1; i <= BLK_TRAP; i++) frames += seen[i];
    printf("   диag: после выгрузки во flash записей %d, метки «ядро» %d, «конец кадра» %d, кадры 1..%d покрыты %d\n",
           total, coreMarks, finMarks, BLK_TRAP, frames);
    chk(total == BLK_RAM_N, "ловушка: после выгрузки во flash ровно столько записей, сколько было в кольце");
    chk(coreMarks > 0 && finMarks > 0, "ловушка: во flash есть и «до ядра», и «после ядра»");

  }

  // --- (8c) печать следа не ждёт USB (иначе прошивка «встаёт» вместе с терминалом) ---
  {
    const uint32_t d0 = txDroppedLines;
    g_serialFree = 0;                            // буфер USB забит: хост не читает
    const uint32_t t0 = micros();
    for (int i = 0; i < 5; i++) traceLine("проверка: буфер полон");
    const uint32_t dt = micros() - t0;
    g_serialFree = 4096;
    const uint32_t w0 = g_serialWriteBytes;
    traceLine("проверка: место есть — строка печатается целиком");
    printf("   диag: с забитым USB 5 строк следа заняли %.1f мкс, пропущено %u\n",
           (double)dt, (unsigned)(txDroppedLines - d0));
    chk(txDroppedLines == d0 + 5, "если USB занят — строки следа пропускаются и считаются (не ждём вечно)");
    chk(dt < 1000u, "пропуск строки не ждёт (микросекунды, а не вечность)");
    chk(g_serialWriteBytes > w0, "когда место есть — строка уходит целиком");
  }

  {
    const uint32_t f0 = g_wdFeeds;
    for (int i = 0; i < 5; i++) loop();
    printf("   диag: кормлений сторожа за 5 кадров: %u\n", (unsigned)(g_wdFeeds - f0));
    chk(g_wdFeeds >= f0 + 5, "цикл кадра кормит сторожа (иначе он перезагрузит консоль на ровном месте)");
  }
  chk(watchdog_hw->scratch[3] == 0x47425A31UL, "отметки фаз пишутся в scratch сторожа (по ним узнаём место)");
  {
    const uint32_t fr1 = watchdog_hw->scratch[1];
    for (int i = 0; i < 3; i++) loop();
    printf("   диag: в отметке кадров было %u, стало %u\n", fr1, watchdog_hw->scratch[1]);
    chk(watchdog_hw->scratch[1] > fr1, "в отметке растёт счётчик кадров (видно, сколько прожили)");
  }

  // --- отчёт при обычной загрузке: про зависание молчим ---
  Serial.flush();
  watchdog_hw->reason = 0;
  watchdog_hw->scratch[0] = watchdog_hw->scratch[1] = watchdog_hw->scratch[2] = watchdog_hw->scratch[3] = 0;
  { int sv = captureStart(); g_serialEcho = true;
    bootReport();
    std::string log = captureStop(sv); g_serialEcho = false;
    chk(!has(log, "ОТ СТОРОЖА") && !has(log, "ШТАТНО"), "обычная загрузка: отчёт не паникует зря");
    chk(has(log, "причина перезагрузки:"), "отчёт ВСЕГДА называет причину (сторож / штатно / питание)");
    chk(has(log, "отметок нет"), "если отметок нет — сказано прямо, что питание включали заново");
  }

  // --- отчёт после зависания В ЯДРЕ эмулятора ---
  watchdog_hw->reason = 1;                       // сторож сработал
  watchdog_hw->scratch[0] = 3;                   // PH_CORE
  watchdog_hw->scratch[1] = 123;                 // кадров
  watchdog_hw->scratch[2] = 7;                   // секунд прожили
  watchdog_hw->scratch[3] = 0x47425A31UL;        // наши отметки валидны
  watchdog_hw->scratch[4] = 0x6ab73121UL;        // «сторож включила прошивка», а не reboot()
  { int sv = captureStart(); g_serialEcho = true;
    bootReport();
    std::string log = captureStop(sv); g_serialEcho = false;
    printf("--- отчёт стенда (как это увидит пользователь) ---\n%s--- конец ---\n", log.c_str());
    chk(has(log, "ОТ СТОРОЖА"),          "зависание узнано: в логе есть строка про перезагрузку от сторожа");
    chk(has(log, "ядро эмулятора"),      "в отчёте названо МЕСТО зависания (внутри gb_run_frame)");
    chk(has(log, "кадров 123"),          "в отчёте есть, сколько кадров успело пройти");
    chk(has(log, "прожили 7 с"),         "в отчёте есть, сколько секунд прожила консоль");
  }
  chk(watchdog_hw->scratch[0] == 1u && watchdog_hw->scratch[3] == 0u,
      "после отчёта отметки обнулены (следующий старт не соврёт)");

  // --- удержание START: журнал + реальное снятие питания ---
  { int sv = captureStart(); g_serialEcho = true;
    g_lowPins = (1u << 15);                      // START = GPIO15 прижат к земле
    ui = UI_GAME;
    const uint32_t tArm = millis();
    const uint16_t m = readMask();
    chk(m == (uint16_t)(1u << IDX_START), "START читается как START (маска кнопок)");
    g_powerCut = false;
    holdTick(m);                                 // «нажали»
    harnessAdvanceMs(1600);                      // держим 1,6 с -> начинается отсчёт
    const bool consuming = holdTick(m);
    harnessAdvanceMs(1600);                      // итого 3,2 с -> питание снимается
    holdTick(m);
    std::string log = captureStop(sv); g_serialEcho = false;
    g_lowPins = 0;
    printf("--- журнал удержания START (стенд) ---\n%s--- конец ---\n", log.c_str());
    chk(consuming, "при удержании START события кнопки в игру НЕ уходят (иначе герой прыгает)");
    chk(has(log, "удержание START"), "в логе видно, ЧТО именно держат (START) и что пошёл отсчёт");
    chk(has(log, "отсчёт 3 с"), "в логе сказано про отсчёт 3 секунд");
    chk(has(log, "снимаю ключ питания"), "по истечении 3 с питание реально снимается (как в стоке)");
    {   // v8.9q: порядок выключения — сперва гаснет экран, потом запись, потом ключ питания
      const size_t p1 = log.find("экран погашен"), p2 = log.find("СНИМАЮ ПИТАНИЕ");
      printf("   диag: порядок выключения: «экран погашен» на %d, «снимаю питание» на %d\n",
             (int)p1, (int)p2);
      chk(has(log, "экран погашен: сохраняю SRAM"),
          "перед снятием питания экран гаснет, и SRAM пишется уже при погашенном экране");
      chk(p1 != std::string::npos && p2 != std::string::npos && p1 < p2,
          "порядок выключения: сперва гаснет экран, потом запись, потом снимается питание");
    }
    chk(!has(log, "Выключение отменено"), "не было ложной отмены отсчёта");
    chk(g_powerCut, "GPIO1 в LOW = ключ питания отпущен (консоль гаснет, а не «висит мёртвой»)");
    const int32_t sinceArm = (int32_t)g_powerCutMs - (int32_t)tArm;
    printf("   диag: питание снято через %d мс после нажатия (нужно ~3000)\n", sinceArm);
    chk(sinceArm >= 3000 && sinceArm <= 3600, "питание снимается ровно на 3-й секунде удержания");
    // отметка перед снятием питания: следующая загрузка не должна считать это зависанием
    chk(watchdog_hw->scratch[5] == 0x504F4631UL,
        "перед снятием питания поставлена отметка «это не зависание»");
    watchdog_hw->reason = 1;                     // как после выключения/включения питания
    watchdog_hw->scratch[4] = 0x6ab73121UL;
    int sv2 = captureStart(); g_serialEcho = true;
    bootReport();
    std::string log2 = captureStop(sv2); g_serialEcho = false;
    chk(has(log2, "ШТАТНО") && !has(log2, "ОТ СТОРОЖА"),
        "после штатного выключения следующий запуск говорит «питание снималось штатно», а не паникует");
    watchdog_hw->reason = 0;
    digitalWrite(PIN_PWR_HOLD, HIGH); digitalWrite(PIN_EN12, HIGH);
    lcdInit();
  }

  // --- разметка внутри прерывания звука: отличаем «встал цикл» от «встало прерывание» ---
  {
    watchdog_hw->scratch[6] = 0;
    const uint32_t n0 = apuIrqN;
    pumpSamples(5);
    printf("   диag: семплов сыграно %u, отметка прерывания после них %u\n",
           (unsigned)(apuIrqN - n0), (unsigned)watchdog_hw->scratch[6]);
    chk(apuIrqN > n0, "прерывание звука работает (семплы считаются)");
    chk(watchdog_hw->scratch[6] == 0, "прерывание звука возвращается и снимает свою отметку (значит, оно не залипло)");
    // имитируем «прерывание не вернулось»: отчёт обязан это заметить и сказать
    watchdog_hw->reason = 1;
    watchdog_hw->scratch[0] = 4;                 // фаза «вывод строки»
    watchdog_hw->scratch[3] = 0x47425A31UL;
    watchdog_hw->scratch[4] = 0x6ab73121UL;
    watchdog_hw->scratch[6] = 2;                 // встали в синтезе семпла
    int sv3 = captureStart(); g_serialEcho = true;
    bootReport();
    std::string log3 = captureStop(sv3); g_serialEcho = false;
    chk(has(log3, "прерывание звука не вернулось") && has(log3, "Отметка 2"),
        "отчёт называет залипшее прерывание звука и шаг, на котором оно встало");
    watchdog_hw->reason = 0;
  }

  // --- трассировка первых кадров: по ней видно, где встало, если встанет ---
  {
    const uint32_t keep = gFramesTotal;
    // В следе нам нужны кадры С выводом на панель, поэтому на время теста выключаем
    // режим 30 кадр/с (иначе первый кадр может попасть на «пропущенный» — так делают
    // только из-за чётности счётчика кадров, а не из-за неисправности).
    const bool fsKeep = gb.direct.frame_skip;
    const int  fscKeep = (int)gb.display.frame_skip_count;
    gb.direct.frame_skip = false; gb.display.frame_skip_count = 1;
    gFramesTotal = 0;                            // как в первый кадр после загрузки
    int sv4 = captureStart(); g_serialEcho = true;
    loop(); loop();
    std::string log4 = captureStop(sv4); g_serialEcho = false;
    gFramesTotal = keep;
    gb.direct.frame_skip = fsKeep; gb.display.frame_skip_count = fscKeep;
    printf("--- трассировка кадров (стенд) ---\n%s--- конец ---\n", log4.c_str());
    chk(has(log4, "кадр 1: начало (путь"), "первый кадр помечен в логе ДО отрисовки (видно, что начали)");
    chk(has(log4, "кадр 1: всего ") && has(log4, " мкс, ядро ") && has(log4, " выход "),
        "итог кадра печатает время: всего / ядро / выход (по этим числам видно, где время)");
    printf("--- страж шины (первые кадры) ---\n%s--- конец ---\n", log4.substr(0, 900).c_str());
    chk(has(log4, "из ") && has(log4, "возможных"),
        "страж: в следе кадра печатается предел шины (сколько байт она может успеть за кадр)");
    chk(!has(log4, "БОЛЬШЕ ПРЕДЕЛА"),
        "страж молчит в нормальной работе (значит, данные действительно уходят)");
    chk(has(log4, "байт в панель"), "след кадра печатает, сколько байт реально ушло в панель");
  }

  // --- клавиша e: переинициализация панели, если картинка пропала, а лог идёт ---
  { int sv = captureStart(); g_serialEcho = true;
    const int rst0 = g_rstWrites;
    harnessSend("e");
    handleSerial();
    std::string log = captureStop(sv); g_serialEcho = false;
    chk(has(log, "переинициализирую шину и панель"), "клавиша e: в логе понятное сообщение");
    chk(g_rstWrites > rst0, "клавиша e: RST панели действительно дёргается (сброс и инициализация заново)");
    chk(has(log, "панель инициализирована заново"), "клавиша e: в логе подтверждение, что сброс прошёл");
  }

  // ---------- v8.9p: мягкий старт (громкость и панель) ----------
  {
    printf("\n=== v8.9p: мягкий старт игры ===\n");
    const uint8_t savedVol = volPct;
    volPct = 80; volRampT0 = millis(); volRampMs = VOL_RAMP_MS;
    const uint8_t v0 = volNow();
    harnessAdvanceMs(VOL_RAMP_MS / 2);
    const uint8_t v1 = volNow();
    harnessAdvanceMs(VOL_RAMP_MS);
    const uint8_t v2 = volNow();
    printf("   диag: плавный вход громкости: %u -> %u -> %u (цель %u)\n",
           (unsigned)v0, (unsigned)v1, (unsigned)v2, (unsigned)volPct);
    chk(v0 == 0, "мягкий старт: сразу после смены игры громкость почти нулевая");
    chk(v1 > 0 && v1 < volPct, "мягкий старт: через секунду громкость на полпути (шаг плавный)");
    chk(v2 == volPct && volRampMs == 0, "мягкий старт: через 2 с громкость входит полностью");
    volPct = savedVol; volRampMs = 0;

    // панель: после запуска игры первые 0,4 с кадры идут в эмулятор, но не в панель
    emuInited = true; ui = UI_GAME; paused = false;
    gb.display.lcd_draw_line = cb_lcd_line;
    panelHoldUntil = millis() + PANEL_HOLD_MS;
    g_data.clear();
    { int sv2 = captureStart(); g_serialEcho = true;
      loop();
      std::string log2 = captureStop(sv2); g_serialEcho = false;
      printf("─── кадр сразу после запуска игры ───\n%s─── конец ───\n",
             log2.substr(0, 300).c_str());
      chk(has(log2, "мягкий старт"), "мягкий старт: в логе сказано, почему кадр ушёл без панели");
    }
    chk(g_data.empty(), "мягкий старт: в панель действительно ничего не ушло (нагрузка ниже)");
    harnessAdvanceMs(PANEL_HOLD_MS + 50);
    g_data.clear();
    for (int i = 0; i < 4 && g_data.empty(); i++) loop();   // 30 кадр/с: рисуется каждый второй
    chk(!g_data.empty(), "мягкий старт: через 0,4 с кадры снова идут в панель");
    panelHoldUntil = 0;
  }

  // ================= v8.5: панель «не успела» (только здесь эмулятор уже инициализирован) =====
  emuInited = true; ui = UI_GAME; paused = false;
      // (5) панель «не успела»: кадр пропускается целиком, эмулятор не встаёт
    emuInited = true; ui = UI_GAME; paused = false;   // как в игре (иначе loop() не занимается кадром)
    gb.display.lcd_draw_line = cb_lcd_line;           // колбэк ставит setup(), повторим на всякий случай
    const uint32_t skips0 = gateSkips;
    g_spiBusy = true;
    { int sv = captureStart(); g_serialEcho = true;
      loop(); loop();
      std::string log = captureStop(sv); g_serialEcho = false;
      printf("--- панель не успела (как увидит пользователь) ---\n%s--- конец ---\n", log.c_str());
      chk(has(log, "панель не успела"), "панель не успела: пользователь видит понятную строку");
    }
    g_spiBusy = false;
    chk(gateSkips > skips0, "панель не успела: кадр пропущен целиком (картинка не портится)");

    // ====================== v9.7: настройки и мягкий лимит громкости ======================
    printf("\n--- v9.7: настройки переживают выключение; громкость до 10%% ---\n");
    {
      // мягкий лимит: крутилка меню не выкручивает выше 10%
      const int sm = menuSel; const uint8_t sv = volPct;
      menuSel = MI_VOLUME;
      volPct = 10; menuAdjust(+1);
      chk(volPct == 10, "громкость: крутилка в меню не выше 10%");
      volPct = 6; menuAdjust(+1);
      chk(volPct == 8, "громкость: шаг 2 внутри лимита");
      for (int i = 0; i < 5; i++) menuAdjust(-1);
      chk(volPct == 0, "громкость: вниз до нуля");
      menuSel = sm; volPct = sv;

      // настройки: запись -> чтение -> восстановление; порча -> игнор
      const uint8_t sl = lightUser, sb = bright, sc = contr;
      const uint8_t sp = palIdx, si = invertShades ? 1 : 0, ss = soundOn ? 1 : 0, sh = spiHzIdx;
      lightUser = 100; bright = 90; contr = 150; volPct = 6;
      palIdx = 1; invertShades = true; soundOn = 0; spiHzIdx = 2;
      settingsStore();
      const uint8_t *p = (const uint8_t *)(flashBase + SET_OFF);
      uint32_t magic = 0, crc = 0;
      memcpy(&magic, p, 4); memcpy(&crc, p + 16, 4);
      chk(magic == SET_MAGIC, "настройки: во flash записана метка SET1");
      chk(crc == fnv1a(p, 16), "настройки: контрольная сумма сходится");
      lightUser = 0; bright = 50; contr = 50; volPct = 10;
      palIdx = 0; invertShades = false; soundOn = 1; spiHzIdx = 1;
      settingsLoad();
      chk(lightUser == 100 && bright == 90 && contr == 150 && volPct == 6,
          "настройки: свет/яркость/контраст/громкость восстановились");
      chk(palIdx == 1 && invertShades && !soundOn && spiHzIdx == 2,
          "настройки: палитра/инверсия/звук/частота панели восстановились");
      ((uint8_t *)(uintptr_t)p)[8] ^= 0x5A;          // портим поле — CRC не сойдётся
      lightUser = 33;
      settingsLoad();
      chk(lightUser == 33, "настройки: битые данные игнорируются (остаётся текущее)");
      settingsStore();                                // чистое состояние на место
      lightUser = sl; bright = sb; contr = sc; volPct = sv;
      palIdx = sp; invertShades = si != 0; soundOn = ss != 0; spiHzIdx = sh;
      rebuildPalette();
    }

    // ============ v9.7.1: следы прошлого кадра на экране при включении ============
    printf("\n--- v9.7.1: стартовая зачистка экрана доходит до панели ---\n");
    {
      // баг: тест-кадр смены частоты SPI шёл ДО инициализации панели и помечал
      // все строки «отдаными» — стартовая зачистка их пропускала, и в GRAM панели
      // оставался кадр выключения (цифра отсчёта / цветной шум)
      panelInited = false;
      rowsValid = false;
      spiSetClock(SPI_HZ_TAB[spiHzIdx]);               // как на старте: частота до панели
      chk(!rowsValid, "старт: кадр до инициализации панели не помечает строки отданными");
      // конец lcdInit: панель проснулась, кэш строк недействителен
      panelInited = true;
      rowsValid = false;
      uiClear(0); uiPush();                            // «шаг 2: гашу экран»
      chk(frameRows == MEM_H && frameSkip == 0,
          "старт: зачистка шлёт все 240 строк целиком (след кадра выключения стёрт)");
      uiClear(0); uiPush();                            // повторно
      chk(frameSkip == MEM_H, "дифф: повторная одинаковая зачистка строки пропускает");
    }

    // ============ v9.7.2: настройки сохраняются и переживают перезапуск ============
    printf("\n--- v9.7.2: запись настроек защищена и подтверждается ---\n");
    {
      const int unprot0 = g_flashUnprot;
      const uint8_t sc = contr;
      contr = 133;
      chk(settingsDirty(), "настройки: после правки отличаются от flash — надо записать");
      settingsStore();
      chk(g_flashUnprot == unprot0, "настройки: запись во flash шла с выключенными прерываниями");
      chk(g_irqLevel == 0, "настройки: прерывания после записи возвращены");
      chk(!settingsDirty(), "настройки: после записи совпадают с flash");
      contr = sc;
      chk(settingsDirty(), "настройки: правка снова делает их грязными");
      settingsStore();
      chk(!settingsDirty(), "настройки: контрольная запись прошла");
    }

  printf("\n%s (провалено проверок: %d)\n", fails ? "=== ЕСТЬ ОШИБКИ ===" : "=== ВСЁ ПРОШЛО ===", fails);
  return fails ? 1 : 0;
}
