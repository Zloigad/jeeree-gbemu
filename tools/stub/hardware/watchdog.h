// Заглушка pico-sdk (hardware/watchdog.h) — только то, что использует скетч.
// Имена и поля ровно такие же, как в настоящем SDK 1.5.1 (проверяются sdkcheck):
// заголовок объявляет регистры watchdog_hw (в железе — hardware/structs/watchdog.h)
// и функции для сторожа.
#pragma once
#include <stdint.h>
#include <stdbool.h>

// Регистры сторожа. В железе scratch — это scratch0..scratch7 (восемь 32-битных
// ячеек, сохраняются при перезагрузке). scratch[4] занимает сам pico-sdk.
typedef struct {
  uint32_t ctrl;
  uint32_t load;
  uint32_t reason;
  uint32_t scratch[8];
  uint32_t tick;
} watchdog_hw_t;

// inline-переменные (C++17): один экземпляр регистров на всю программу
inline watchdog_hw_t g_wdHwModel = {};
inline watchdog_hw_t *watchdog_hw = &g_wdHwModel;

// Счётчики для стенда: сколько раз кормили сторожа и сколько раз он «включался».
inline uint32_t g_wdFeeds = 0;
inline uint32_t g_wdEnabledMs = 0;

static inline void watchdog_start_tick(uint cycles) { (void)cycles; }
static inline void watchdog_update(void) { g_wdFeeds++; }
static inline void watchdog_enable(uint32_t delay_ms, bool pause_on_debug) {
  (void)pause_on_debug;
  g_wdEnabledMs = delay_ms;
  watchdog_hw->scratch[4] = 0x6ab73121UL;   // как в SDK: WATCHDOG_NON_REBOOT_MAGIC
}
static inline void watchdog_reboot(uint32_t pc, uint32_t sp, uint32_t delay_ms) {
  (void)pc; (void)sp; (void)delay_ms;
}
static inline bool watchdog_caused_reboot(void) { return watchdog_hw->reason != 0; }
static inline bool watchdog_enable_caused_reboot(void) {
  return watchdog_hw->reason != 0 && watchdog_hw->scratch[4] == 0x6ab73121UL;
}
// определена в стенде (возвращает время до сброса); для проверки синтаксиса хватает объявления
uint32_t watchdog_get_count(void);
