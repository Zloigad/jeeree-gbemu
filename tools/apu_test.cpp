// apu_test.cpp — проверка APU (apu_gb.h) на компьютере + рендер превью в WAV.
// Сборка и запуск:
//   cd /home/user && g++ -std=gnu++17 -O2 -Wall gbemu/apu_gb.h tools/apu_test.cpp -o tools/apu_test && ./tools/apu_test
// Результат: tools/apu/apu_preview.wav (можно послушать на ПК — то же самое
// уйдёт в ШИМ на GP17 консоли) и отчёт проверок в консоль.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include "../gbemu/apu_gb.h"

static int fails = 0, checks = 0;
static void chk(bool ok, const char *what) {
  checks++;
  printf("%s %s\n", ok ? "[ok]  " : "[FAIL]", what);
  if (!ok) fails++;
}

// ------------------------------------------------------------------ утилиты
static uint16_t freqOf(double hz) {          // регистр 11 бит для нужной частоты
  double f = 131072.0 / hz;
  int v = (int)lround(2048.0 - f);
  if (v < 0) v = 0;
  if (v > 2047) v = 2047;
  return (uint16_t)v;
}
static void trigSq(int ch, uint16_t freq, uint8_t duty, uint8_t env, uint8_t lenLoad, bool lenEn) {
  const uint16_t A = ch ? 0xFF16 : 0xFF11;
  apuWrite(A, (uint8_t)((duty << 6) | (lenLoad & 0x3F)));
  apuWrite((uint16_t)(A + 1), env);
  apuWrite((uint16_t)(A + 2), (uint8_t)(freq & 0xFF));
  apuWrite((uint16_t)(A + 3), (uint8_t)(0x80 | (lenEn ? 0x40 : 0) | ((freq >> 8) & 7)));
}
static void trigNoise(uint8_t nr43, uint8_t env, uint8_t lenLoad, bool lenEn) {
  apuWrite(0xFF20, (uint8_t)(lenLoad & 0x3F));
  apuWrite(0xFF21, env);
  apuWrite(0xFF22, nr43);
  apuWrite(0xFF23, (uint8_t)(0x80 | (lenEn ? 0x40 : 0)));
}
static void trigWave(uint16_t freq, uint8_t lvl, uint8_t lenLoad, bool lenEn) {
  apuWrite(0xFF1A, 0x80);
  apuWrite(0xFF1B, lenLoad);
  apuWrite(0xFF1C, (uint8_t)(lvl << 5));
  apuWrite(0xFF1D, (uint8_t)(freq & 0xFF));
  apuWrite(0xFF1E, (uint8_t)(0x80 | (lenEn ? 0x40 : 0) | ((freq >> 8) & 7)));
}

// Считаем переходы через ноль — по ним измеряем частоту прямоугольного канала.
struct Stats { int minv, maxv, crossings, n; double rms; };
static Stats run(int n, void (*step)(int) = nullptr) {
  Stats s; s.minv = 1 << 30; s.maxv = -(1 << 30); s.crossings = 0; s.n = n;
  double acc = 0; int prev = 0; bool first = true;
  for (int i = 0; i < n; i++) {
    if (step) step(i);
    const int v = (int)apuTick();
    if (v < s.minv) s.minv = v;
    if (v > s.maxv) s.maxv = v;
    acc += (double)v * v;
    if (!first && ((prev < 0 && v >= 0) || (prev > 0 && v <= 0))) s.crossings++;
    prev = v; first = false;
  }
  s.rms = sqrt(acc / n);
  return s;
}

// ------------------------------------------------------------------ превью
static void renderPreview(const char *path) {
  const int rate = APU_RATE_HZ;
  std::vector<int16_t> pcm;
  apuReset();
  apuWrite(0xFF24, 0x77);
  apuWrite(0xFF25, 0xFF);
  // волновая RAM: мягкая «пила» — как у DMG
  for (int i = 0; i < 16; i++) apuWrite((uint16_t)(0xFF30 + i), (uint8_t)((i << 4) | (15 - i)));

  const uint16_t mel[] = { freqOf(523.25), freqOf(587.33), freqOf(659.25), freqOf(783.99),
                           freqOf(659.25), freqOf(587.33), freqOf(523.25), freqOf(392.00) };
  const uint16_t bas[] = { freqOf(130.81), freqOf(130.81), freqOf(164.81), freqOf(196.00) };

  const int step = rate / 4;                     // четверти по 250 мс
  for (int t = 0; t < 24; t++) {
    const int m = t % 8, b = (t / 2) % 4;
    trigSq(0, mel[m], 2, 0xF2, 0, false);        // мелодия: громко, конверт вниз
    trigSq(1, bas[b], 1, 0x93, 0, false);        // бас: 25 %, тише
    if (t % 4 == 2) trigNoise(0x25, 0xF2, 0, false);   // «тарелка»
    if (t % 8 == 6) { trigWave(freqOf(1046.5), 1, 0, false); }
    else if (t % 8 == 7) { apuWrite(0xFF1A, 0); }
    for (int i = 0; i < step; i++) {
      int v = (int)apuTick();
      if (v > APU_FULL_SCALE) v = APU_FULL_SCALE;
      if (v < -APU_FULL_SCALE) v = -APU_FULL_SCALE;
      pcm.push_back((int16_t)(v * 40));          // -16800..+16800
    }
  }

  FILE *f = fopen(path, "wb");
  if (!f) { printf("не смог открыть %s\n", path); return; }
  const int n = (int)pcm.size();
  const int dataBytes = n * 2;
  auto w32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
  auto w16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
  fwrite("RIFF", 1, 4, f); w32(36 + dataBytes); fwrite("WAVEfmt ", 1, 8, f);
  w32(16); w16(1); w16(1); w32(rate); w32(rate * 2); w16(2); w16(16);
  fwrite("data", 1, 4, f); w32(dataBytes);
  fwrite(pcm.data(), 2, n, f);
  fclose(f);
  printf("превью: %s, %.2f с, %d семплов\n", path, (double)n / rate, n);
}

// ------------------------------------------------------------------ проверки
int main(void) {
  // 1. после сброса — тишина
  apuReset();
  {
    Stats s = run(2000);
    chk(s.minv == 0 && s.maxv == 0, "после сброса звука нет (все каналы выключены)");
  }

  // 2. частота канала 1 и амплитуда
  apuReset();
  apuWrite(0xFF24, 0x77); apuWrite(0xFF25, 0x11);       // канал 1 в оба уха, громкость 7/7
  trigSq(0, freqOf(440.0), 2, 0xF0, 0, false);          // 50 %, громкость 15, без конверта
  {
    Stats s = run(APU_RATE_HZ);                         // ровно секунда
    const double hz = s.crossings / 2.0;
    printf("   канал 1: переходов %d -> %.1f Гц, размах %d..%d, RMS %.1f\n",
           s.crossings, hz, s.minv, s.maxv, s.rms);
    chk(fabs(hz - 440.0) < 440.0 * 0.02, "прямоугольный канал держит 440 Гц (погрешность < 2 %)");
    chk(s.maxv <= 105 && s.minv >= -105 && s.maxv > 90 && s.minv < -90,
        "амплитуда ровно как у DMG (громкость 15 + NR50 7/7 = 105)");
  }

  // 3. конверт (затухание)
  apuReset();
  apuWrite(0xFF24, 0x77); apuWrite(0xFF25, 0x11);
  trigSq(0, freqOf(220.0), 2, 0xF3, 0, false);          // период конверта 3 (медленно)
  {
    Stats a = run(APU_RATE_HZ / 10);                    // первые 100 мс
    Stats b = run(APU_RATE_HZ / 10);                    // следующие 100 мс
    printf("   конверт: RMS %.1f -> %.1f\n", a.rms, b.rms);
    chk(b.rms < a.rms * 0.95, "конверт затухает (громкость падает со временем)");
  }

  // 4. счётчик длины выключает канал
  apuReset();
  apuWrite(0xFF24, 0x77); apuWrite(0xFF25, 0x11);
  trigSq(0, freqOf(440.0), 2, 0xF0, 60, true);          // длина 4 тика по 1/256 с
  {
    run(APU_RATE_HZ / 20);                              // 50 мс — заведомо больше 4/256 с
    printf("   NR52 после длины: 0x%02X\n", apuRead(0xFF26));
    chk(apuRead(0xFF26) == 0xF0, "после истечения длины канал выключен (NR52 = 0xF0)");
    Stats s = run(APU_RATE_HZ / 20);                    // и ещё 50 мс — там уже тишина
    chk(s.maxv == 0 && s.minv == 0, "и звук после этого полностью стих");
  }

  // 5. волновой канал
  apuReset();
  apuWrite(0xFF24, 0x77); apuWrite(0xFF25, 0x44);
  for (int i = 0; i < 16; i++) apuWrite((uint16_t)(0xFF30 + i), 0x12);   // 0,1,2
  trigWave(freqOf(440.0), 1, 0, false);
  {
    Stats s = run(APU_RATE_HZ / 4);
    printf("   волновой: размах %d..%d\n", s.minv, s.maxv);
    chk(s.maxv < 0 && s.minv < 0, "волновой канал звучит (данные ниже середины — сплошной минус)");
    const int before = s.maxv;
    apuWrite(0xFF1A, 0x00);                             // ЦАП выключен
    Stats t = run(2000);
    chk(t.minv == 0 && t.maxv == 0, "выключение ЦАП волнового канала даёт тишину");
    chk(before <= -10, "и до этого он был слышен (уровень ниже середины)");
  }

  // 6. шумовой канал
  apuReset();
  apuWrite(0xFF24, 0x77); apuWrite(0xFF25, 0x88);
  trigNoise(0x14, 0xF0, 0, false);                       // 4096 Гц — быстрый «треск»
  {
    Stats s = run(APU_RATE_HZ / 10);
    printf("   шум: переходов %d за 100 мс, размах %d..%d\n", s.crossings, s.minv, s.maxv);
    chk(s.crossings > 100, "шумовой канал «трещит» (больше 1000 переходов в секунду)");
  }

  // 7. биты активности в NR52
  apuReset();
  chk(apuRead(0xFF26) == 0xF0, "NR52 после сброса = 0xF0 (звук включён, каналы молчат)");
  trigSq(0, 440, 2, 0xF0, 0, false);
  trigSq(1, 440, 2, 0xF0, 0, false);
  printf("   NR52 до выключения: 0x%02X\n", apuRead(0xFF26));
  chk(apuRead(0xFF26) == 0xF3, "каналы 1 и 2 отмечены активными (NR52 = 0xF3)");
  apuWrite(0xFF26, 0x00);                               // выключить APU
  chk(apuRead(0xFF26) == 0x70, "выключение APU через NR52 гасит все каналы");
  chk(apuTick() == 0 && apuTick() == 0, "и семплы становятся нулевыми");

  // 8. микшер: панорама и громкость
  apuReset();
  apuWrite(0xFF24, 0x77);
  trigSq(0, freqOf(440.0), 2, 0xF0, 0, false);
  apuWrite(0xFF25, 0x00);
  { Stats s = run(4000); chk(s.maxv == 0, "NR51 = 0 — канал никуда не направлен, тишина"); }
  apuWrite(0xFF25, 0x10);                               // только в левый
  { Stats s = run(4000); chk(s.maxv > 40 && s.maxv <= 53, "только в левый — половина амплитуды"); }
  apuWrite(0xFF25, 0x11);                               // в оба
  { Stats s = run(4000); chk(s.maxv > 100 && s.maxv <= 106, "в оба уха — полная амплитуда 105"); }
  apuWrite(0xFF24, 0x11);                               // громкость 1/1
  { Stats s = run(4000); chk(s.maxv <= 16 && s.maxv > 8, "NR50 1/1 — тихо (15 уровней)"); }

  // 9. четыре канала вместе на полной громкости не выходят за шкалу
  apuReset();
  apuWrite(0xFF24, 0x77); apuWrite(0xFF25, 0xFF);
  trigSq(0, freqOf(440.0), 2, 0xF0, 0, false);
  trigSq(1, freqOf(220.0), 2, 0xF0, 0, false);
  for (int i = 0; i < 16; i++) apuWrite((uint16_t)(0xFF30 + i), 0xFF);
  trigWave(freqOf(880.0), 1, 0, false);
  trigNoise(0x14, 0xF0, 0, false);
  {
    Stats s = run(APU_RATE_HZ / 2);
    printf("   всё вместе: размах %d..%d\n", s.minv, s.maxv);
    chk(s.maxv <= APU_FULL_SCALE && s.minv >= -APU_FULL_SCALE, "полная шкала не превышена (нет клиппинга)");
    chk(s.maxv > 300, "и на полной громкости действительно громко");
  }

  // 10. ШИМ-уровни для GP17
  chk(apuLevel(0) == APU_PWM_IDLE, "тишина = уровень 344 (как в стоковой прошивке)");
  {
    const uint16_t hi = apuLevel(APU_FULL_SCALE), lo = apuLevel(-APU_FULL_SCALE);
    printf("   ШИМ: тишина %d, максимум %d, минимум %d (TOP = %d)\n",
           (int)APU_PWM_IDLE, (int)hi, (int)lo, (int)APU_PWM_TOP);
    chk(hi > 600 && hi < APU_PWM_TOP, "максимум около 624 и не упирается в TOP");
    chk(lo > 0 && lo < 100, "минимум около 64 и не упирается в 0");
    chk(apuLevelVol(APU_FULL_SCALE, 50, APU_PWM_MAXAMP) < hi, "громкость 50 % реально тише");
  }

  // 11. v9.3: DC-блокер (цифровой «разделительный конденсатор»)
  apuDcReset();
  {
    int32_t last = 0;
    for (int i = 0; i < 5000; i++) last = apuDcBlock(-52);
    chk(last == 0, "DC-блокер: постоянка каналов (-52) гаснет до нуля");
    apuDcReset();
    chk(apuDcBlock(105) == 105, "ступень/щелчок проходит как есть");
    int32_t tail = 0;
    for (int i = 0; i < 2000; i++) tail = apuDcBlock(105);
    chk(tail == 0, "а хвост ступени гаснет — щелчка не остаётся");
  }

  // 12. v9.3: антиалиасинг — высокая нота приглушается (boxcar за семпл)
  apuReset();
  apuWrite(0xFF24, 0x77); apuWrite(0xFF25, 0x11);
  trigSq(0, freqOf(16000.0), 2, 0xF0, 0, false);
  {
    Stats s = run(APU_RATE_HZ / 10);
    printf("   высокая нота 16 кГц: размах %d..%d (без усреднения было бы ±105)\n", s.minv, s.maxv);
    chk(s.maxv < 90 && s.maxv > 10, "16 кГц приглушается усреднением (как динамик DMG)");
  }

  renderPreview("tools/apu/apu_preview.wav");

  printf("\n%s (проверок %d, провалено %d)\n", fails ? "=== ЕСТЬ ОШИБКИ ===" : "=== ВСЁ ПРОШЛО ===", checks, fails);
  return fails ? 1 : 0;
}
