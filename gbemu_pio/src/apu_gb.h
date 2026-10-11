/*
 * apu_gb.h — звук Game Boy (APU) для нашего эмулятора.
 *
 * Зачем: у консоли звук выведен ШИМ-ом на GPIO17 (в стоке PWM с TOP = 689,
 * «тишина» = уровень 344), а сам звук считает эмулятор. Здесь живёт ровно то,
 * что нужно: 4 канала DMG (2 прямоугольных, волновой, шумовой), конверты,
 * длина, свип, рамки 512 Гц и микшер NR50/NR51.
 *
 * Как это включено в ядро peanut: в скетче объявлены
 *     static uint8_t audio_read (uint16_t addr);
 *     static void    audio_write(uint16_t addr, uint8_t val);
 * и определён ENABLE_SOUND — ядро само зовёт их на запись/чтение 0xFF10..0xFF3F.
 * Регистры копятся здесь, а apuTick() считает по одному семплу. Семпл берётся
 * по таймеру 22222 Гц (45 мкс) в прерывании — независимо от скорости эмуляции,
 * поэтому звук не «плывёт», когда игра тормозит.
 *
 * Никаких зависимостей от Arduino: файл собирается и на устройстве, и на
 * компьютере (tools/apu_test.cpp), поэтому математику можно проверить дома.
 */
#ifndef APU_GB_H
#define APU_GB_H

#include <stdint.h>
#include <string.h>

#ifndef APU_RATE_HZ
#define APU_RATE_HZ 22222                  /* частота вызова apuTick(), Гц */
#endif

/* --------------------------------------------------------------- состояние */

struct ApuEnv {                            /* конверт (NRx2) */
  uint8_t vol, init, dir, prd, cnt, dacOn;
};

struct ApuSquare {
  ApuEnv   e;
  uint8_t  duty;                           /* 0..3 -> 12.5/25/50/75 % */
  uint8_t  on, lenEn;
  uint16_t freq, len, lenReload;           /* freq: 11 бит (2048 - f) */
  uint32_t phase, inc;                     /* фаза 16.16 */
  uint8_t  swPrd, swDir, swShift, swCnt, swEn;
};

struct ApuWave {
  uint8_t  lvl;                            /* NR32: 0 = выкл, 1 = 100 %, 2 = 50 %, 3 = 25 % */
  uint8_t  dacOn, on, lenEn;
  uint16_t freq, len, lenReload;
  uint32_t phase, inc;
};

struct ApuNoise {
  ApuEnv   e;
  uint8_t  divCode, width, shift;
  uint8_t  on, lenEn;
  uint16_t lfsr, len, lenReload;
  uint32_t phase, inc;
};

struct ApuState {
  uint8_t   regs[0x30];                    /* что писали в 0xFF10..0xFF3F (для чтения) */
  uint8_t   wave[16];
  uint8_t   powerOn, outVol, outMap;       /* NR52, NR50, NR51 */
  uint8_t   fsStep;                        /* шаг рамки 0..7 */
  uint32_t  fsAcc;
  ApuSquare sq[2];
  ApuWave   wv;
  ApuNoise  ns;
};

static ApuState apu;

/* Громкость, с которой отдаётся семпл при полной шкале (см. apuTick). */
#define APU_FULL_SCALE 420

static const uint8_t apuDutyTab[4][8] = {
  { 0, 0, 0, 0, 0, 0, 0, 1 },              /* 12.5 % */
  { 1, 0, 0, 0, 0, 0, 0, 1 },              /* 25 %   */
  { 1, 0, 0, 0, 0, 1, 1, 1 },              /* 50 %   */
  { 0, 1, 1, 1, 1, 1, 1, 0 }               /* 75 %   */
};

static const uint16_t apuDivTab[8] = { 8, 16, 32, 48, 64, 80, 96, 112 };

/* ---------------------------------------------------------------- утилиты */

/* Инкремент фазы 16.16: stepsPerSec шагов в секунду при нашей частоте семплов. */
static inline uint32_t apuIncFromSteps(uint32_t stepsPerSec) {
  return (uint32_t)(((uint64_t)stepsPerSec << 16) / APU_RATE_HZ);
}

/* Рекомендуемая скорость для прямоугольного канала: 8 шагов на период. */
static inline uint32_t apuSqInc(uint16_t freq) {
  const uint16_t d = (uint16_t)(2048u - (freq & 0x7FFu));
  return d ? apuIncFromSteps((uint32_t)(8u * 131072u / d)) : 0;
}

/* Волновой канал: 32 шага на период. */
static inline uint32_t apuWaveInc(uint16_t freq) {
  const uint16_t d = (uint16_t)(2048u - (freq & 0x7FFu));
  return d ? (uint32_t)(((uint64_t)31u * 131072u * 2u << 16) / ((uint64_t)d * APU_RATE_HZ)) : 0;
}

/* Шум: тактовая частота 524288 / (делитель << сдвиг). */
static inline uint32_t apuNoiseInc(const ApuNoise *c) {
  const uint32_t div = (uint32_t)apuDivTab[c->divCode & 7] << (c->shift & 0x0F);
  return div ? apuIncFromSteps(524288u / div) : 0;
}

static inline void apuEnvApply(ApuEnv *e, uint8_t val) {
  e->init  = (uint8_t)(val >> 4);
  e->dir   = (uint8_t)((val >> 3) & 1);
  e->prd   = (uint8_t)(val & 7);
  e->dacOn = (val & 0xF8) ? 1 : 0;
  if (!e->dacOn) e->vol = 0;
}

static inline void apuEnvStep(ApuEnv *e) {                 /* 64 Гц */
  if (!e->prd) return;
  if (e->cnt) e->cnt--;
  if (e->cnt) return;
  e->cnt = e->prd;
  if (e->dir) { if (e->vol < 15) e->vol++; }
  else        { if (e->vol) e->vol--; }
}

static inline void apuLfsrStep(ApuNoise *c) {
  uint16_t l = c->lfsr;
  const uint16_t bit = (uint16_t)((l ^ (l >> 1)) & 1u);
  l = (uint16_t)((l >> 1) | (bit << 14));
  if (c->width) l = (uint16_t)((l & ~(1u << 6)) | (bit << 6));   /* 7-битный режим */
  c->lfsr = l;
}

/* ---------------------------------------------------------------- сброс */

static void apuReset(void) {
  memset(&apu, 0, sizeof(apu));
  apu.powerOn = 1;                          /* после включения NR52 = 0x80 */
  apu.outVol  = 0x77;                       /* как настоящий DMG после Reset */
  apu.wave[0] = 0x00; apu.wave[1] = 0xFF;   /* у DMG в RAM «пила» 0..F */
  for (int i = 2; i < 16; i++) apu.wave[i] = 0x00;
}

/* ---------------------------------------------------------------- запись */

static void apuTriggerSq(ApuSquare *c) {
  if (!c->e.dacOn) { c->on = 0; return; }
  c->on    = 1;
  c->phase = 0;
  c->inc   = apuSqInc(c->freq);
  c->e.vol = c->e.init;
  c->e.cnt = c->e.prd;
  c->len   = c->lenEn ? (c->lenReload ? c->lenReload : 64) : 0;
  if (c->swPrd || c->swShift) { c->swCnt = c->swPrd ? c->swPrd : 8; }
}

static void apuTriggerWave(ApuWave *c) {
  if (!c->dacOn) { c->on = 0; return; }
  c->on    = 1;
  c->phase = 0;
  c->inc   = apuWaveInc(c->freq);
  c->len   = c->lenEn ? (c->lenReload ? c->lenReload : 256) : 0;
}

static void apuTriggerNoise(ApuNoise *c) {
  if (!c->e.dacOn) { c->on = 0; return; }
  c->on    = 1;
  c->phase = 0;
  c->inc   = apuNoiseInc(c);
  c->lfsr  = 0x7FFF;
  c->e.vol = c->e.init;
  c->e.cnt = c->e.prd;
  c->len   = c->lenEn ? (c->lenReload ? c->lenReload : 64) : 0;
}

static void apuWrite(uint16_t addr, uint8_t val) {
  if (addr < 0xFF10 || addr > 0xFF3F) return;

  if (addr >= 0xFF30) {                                 /* волновая RAM */
    apu.wave[addr - 0xFF30] = val;
    return;
  }
  const uint8_t i = (uint8_t)(addr - 0xFF10);
  const uint8_t keep = apu.regs[i];

  /* при выключенном APU (NR52 bit7 = 0) регистры каналов у DMG не пишутся */
  if (!apu.powerOn && addr != 0xFF26) return;

  switch (addr) {
    case 0xFF10:                                        /* NR10 — свип канала 1 */
      apu.sq[0].swPrd   = (uint8_t)((val >> 4) & 7);
      apu.sq[0].swDir   = (uint8_t)((val >> 3) & 1);
      apu.sq[0].swShift = (uint8_t)(val & 7);
      apu.sq[0].swEn    = 1;
      break;
    case 0xFF11:                                        /* NR11 — форма + длина */
      apu.sq[0].duty      = (uint8_t)(val >> 6);
      apu.sq[0].lenReload = (uint16_t)(64 - (val & 0x3F));
      break;
    case 0xFF12: apuEnvApply(&apu.sq[0].e, val); if (!apu.sq[0].e.dacOn) apu.sq[0].on = 0; break;
    case 0xFF13: apu.sq[0].freq = (uint16_t)((apu.sq[0].freq & 0x700u) | val); break;
    case 0xFF14:
      apu.sq[0].freq   = (uint16_t)((apu.sq[0].freq & 0x0FFu) | ((uint16_t)(val & 7) << 8));
      apu.sq[0].lenEn  = (uint8_t)((val >> 6) & 1);
      if (!(val & 0x80)) break;                         /* без бита 7 — просто запись */
      if (apu.sq[0].len == 0 && apu.sq[0].lenEn) apu.sq[0].len = apu.sq[0].lenReload;
      apuTriggerSq(&apu.sq[0]);
      break;
    case 0xFF16:
      apu.sq[1].duty      = (uint8_t)(val >> 6);
      apu.sq[1].lenReload = (uint16_t)(64 - (val & 0x3F));
      break;
    case 0xFF17: apuEnvApply(&apu.sq[1].e, val); if (!apu.sq[1].e.dacOn) apu.sq[1].on = 0; break;
    case 0xFF18: apu.sq[1].freq = (uint16_t)((apu.sq[1].freq & 0x700u) | val); break;
    case 0xFF19:
      apu.sq[1].freq  = (uint16_t)((apu.sq[1].freq & 0x0FFu) | ((uint16_t)(val & 7) << 8));
      apu.sq[1].lenEn = (uint8_t)((val >> 6) & 1);
      if (!(val & 0x80)) break;
      if (apu.sq[1].len == 0 && apu.sq[1].lenEn) apu.sq[1].len = apu.sq[1].lenReload;
      apuTriggerSq(&apu.sq[1]);
      break;
    case 0xFF1A:                                        /* NR30 — ЦАП волнового канала */
      apu.wv.dacOn = (uint8_t)(val >> 7);
      if (!apu.wv.dacOn) apu.wv.on = 0;
      break;
    case 0xFF1B: apu.wv.lenReload = (uint16_t)(256 - val); break;
    case 0xFF1C: apu.wv.lvl = (uint8_t)((val >> 5) & 3); break;
    case 0xFF1D: apu.wv.freq = (uint16_t)((apu.wv.freq & 0x700u) | val); break;
    case 0xFF1E:
      apu.wv.freq  = (uint16_t)((apu.wv.freq & 0x0FFu) | ((uint16_t)(val & 7) << 8));
      apu.wv.lenEn = (uint8_t)((val >> 6) & 1);
      if (!(val & 0x80)) break;
      if (apu.wv.len == 0 && apu.wv.lenEn) apu.wv.len = apu.wv.lenReload;
      apuTriggerWave(&apu.wv);
      break;
    case 0xFF20: apu.ns.lenReload = (uint16_t)(64 - (val & 0x3F)); break;
    case 0xFF21: apuEnvApply(&apu.ns.e, val); if (!apu.ns.e.dacOn) apu.ns.on = 0; break;
    case 0xFF22:
      apu.ns.divCode = (uint8_t)(val & 7);
      apu.ns.width   = (uint8_t)((val >> 3) & 1);
      apu.ns.shift   = (uint8_t)(val >> 4);
      break;
    case 0xFF23:
      apu.ns.lenEn = (uint8_t)((val >> 6) & 1);
      if (!(val & 0x80)) break;
      if (apu.ns.len == 0 && apu.ns.lenEn) apu.ns.len = apu.ns.lenReload;
      apuTriggerNoise(&apu.ns);
      break;
    case 0xFF24: apu.outVol = val; break;                /* NR50 */
    case 0xFF25: apu.outMap = val; break;                /* NR51 */
    case 0xFF26:                                         /* NR52 */
      if (!(val & 0x80)) {                               /* выключение звука */
        uint8_t wsave[16];
        memcpy(wsave, apu.wave, 16);
        memset(apu.regs, 0, sizeof(apu.regs));
        apu.sq[0].on = apu.sq[1].on = apu.wv.on = apu.ns.on = 0;
        apu.sq[0].e.vol = apu.sq[1].e.vol = apu.ns.e.vol = 0;
        apu.sq[0].e.dacOn = apu.sq[1].e.dacOn = apu.ns.e.dacOn = 0;
        apu.sq[0].len = apu.sq[1].len = apu.wv.len = apu.ns.len = 0;
        apu.powerOn = 0;                       /* пока не напишут 0x80 в NR52, звука нет */
        apu.outVol = 0; apu.outMap = 0;
        memcpy(apu.wave, wsave, 16);
        return;
      }
      apu.powerOn = 1;
      break;
    default: break;                                      /* 0xFF15, 0xFF1F — не используются */
  }
  apu.regs[i] = val ? val : keep;                        /* храним последнее значение */
}

/* ---------------------------------------------------------------- чтение */

/* Так же, как в peanut: часть регистров читается с «навязанными» единицами. */
static const uint8_t apuOrtab[0x30] = {
  0x80, 0x3F, 0x00, 0xFF, 0xBF,                    /* FF10-FF14 */
  0xFF, 0x3F, 0x00, 0xFF, 0xBF,                    /* FF15-FF19 */
  0x7F, 0xFF, 0x9F, 0xFF, 0xBF,                    /* FF1A-FF1E */
  0xFF, 0xFF, 0x00, 0x00, 0xBF,                    /* FF1F-FF23 */
  0x00, 0x00, 0x70,                                /* FF24-FF26 */
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  /* FF27-FF2E */
  0xFF                                             /* FF2F */
};

static uint8_t apuRead(uint16_t addr) {
  if (addr < 0xFF10 || addr > 0xFF3F) return 0xFF;
  if (addr >= 0xFF30) return apu.wave[addr - 0xFF30];
  if (addr == 0xFF26) {
    uint8_t v = (uint8_t)(apu.powerOn ? 0x80 : 0x00) | 0x70;
    if (apu.sq[0].on) v |= 0x01;
    if (apu.sq[1].on) v |= 0x02;
    if (apu.wv.on)    v |= 0x04;
    if (apu.ns.on)    v |= 0x08;
    return v;
  }
  const uint8_t i = (uint8_t)(addr - 0xFF10);
  return (uint8_t)(apu.regs[i] | apuOrtab[i]);
}

/* ---------------------------------------------------------------- синтез */

/* ВАЖНО: канал отдаёт сигнал, центрированный вокруг нуля.
   У настоящего DMG ЦАП выдаёт 0..15, а «середина» = 7.5. Если эту середину не
   вычесть, в выходе останется постоянка 344 уровня (наш уровень тишины):
   усилитель с разделительным конденсатором её срежет и звук станет вдвое тише,
   а без конденсатора динамик будет подмагничен. Поэтому вычитаем 7.5 (в наших
   целых числах это (2*v - 15)). */
#define APU_CENTER(v) (2 * (v) - 15)
static inline int32_t apuSqSample(ApuSquare *c) {
  if (!c->on || !c->e.dacOn) return 0;
  const uint32_t inc = c->inc;
  const uint32_t f0  = c->phase & 0xFFFFu;
  const uint32_t i0  = (uint32_t)(c->phase >> 16) & 7u;
  c->phase += inc;
  if (!inc) return APU_CENTER(apuDutyTab[c->duty & 3][i0] ? (int32_t)c->e.vol : 0);
  /* v9.3: среднее за семпл (boxcar), а не точечный отсчёт. Высокие ноты раньше
     сворачивались в «журчащий» мусор на частоте семплов; теперь они честно
     приглушаются — как через динамик настоящего DMG. */
  const uint8_t *tab = apuDutyTab[c->duty & 3];
  uint32_t ones16 = 0, u = 0, idx = i0, left = 0x10000u - f0;
  while (u < inc) {
    const uint32_t seg = (left < inc - u) ? left : (inc - u);
    if (tab[idx]) ones16 += seg;
    u += seg;
    if (seg == left) idx = (idx + 1) & 7u;   // дошли до границы шага фазы
    left = 0x10000u;
  }
  const int32_t avg = (int32_t)(((uint32_t)c->e.vol * ones16 + (inc >> 1)) / inc);
  return APU_CENTER(avg);
}

static inline int32_t apuWaveSample(ApuWave *c) {
  if (!c->on || !c->dacOn || !c->lvl) return 0;
  c->phase += c->inc;
  const uint32_t p = (c->phase >> 16) & 31;
  const uint8_t  b = apu.wave[(p >> 1) & 15];
  const uint8_t  n = (p & 1) ? (uint8_t)(b & 0x0F) : (uint8_t)(b >> 4);
  /* 1 = 100 %, 2 = 50 %, 3 = 25 % — сдвигаем уже центрированный сигнал */
  return (int32_t)(APU_CENTER((int32_t)n) >> (c->lvl - 1));
}

static inline int32_t apuNoiseSample(ApuNoise *c) {
  if (!c->on || !c->e.dacOn) return 0;
  const uint32_t inc = c->inc;
  const uint32_t f0  = c->phase & 0xFFFFu;
  c->phase += inc;
  if (!inc) return APU_CENTER(((~c->lfsr) & 1u) ? (int32_t)c->e.vol : 0);
  /* v9.3: среднее бита LFSR за семпл (boxcar). Раньше брался последний бит, и
     частоты LFSR выше частоты семплов сворачивались в «журчание/треск». */
  uint32_t ones16 = 0, u = 0, left = 0x10000u - f0;
  uint32_t bit = ((~c->lfsr) & 1u) ? 1u : 0u;
  while (u < inc) {
    const uint32_t seg = (left < inc - u) ? left : (inc - u);
    if (bit) ones16 += seg;
    u += seg;
    if (seg == left) {                       // граница шага LFSR
      apuLfsrStep(c);
      bit = ((~c->lfsr) & 1u) ? 1u : 0u;
    }
    left = 0x10000u;
  }
  const int32_t avg = (int32_t)(((uint32_t)c->e.vol * ones16 + (inc >> 1)) / inc);
  return APU_CENTER(avg);
}

/* ------------------------------------------------- кадровая последовательность */

static inline void apuFsLength(void) {                 /* 256 Гц */
  if (apu.sq[0].lenEn && apu.sq[0].on && apu.sq[0].len) { if (!--apu.sq[0].len) apu.sq[0].on = 0; }
  if (apu.sq[1].lenEn && apu.sq[1].on && apu.sq[1].len) { if (!--apu.sq[1].len) apu.sq[1].on = 0; }
  if (apu.wv.lenEn    && apu.wv.on    && apu.wv.len)    { if (!--apu.wv.len)    apu.wv.on    = 0; }
  if (apu.ns.lenEn    && apu.ns.on    && apu.ns.len)    { if (!--apu.ns.len)    apu.ns.on    = 0; }
}

static inline void apuFsSweep(void) {                  /* 128 Гц, канал 1 */
  ApuSquare *c = &apu.sq[0];
  if (!c->swEn || !c->swPrd) return;
  if (c->swCnt) c->swCnt--;
  if (c->swCnt) return;
  c->swCnt = c->swPrd;
  if (!c->swShift) return;
  const uint16_t d = (uint16_t)(c->freq >> c->swShift);
  const uint16_t nf = (uint16_t)(c->swDir ? (c->freq - d) : (c->freq + d));
  if (nf > 0x7FF) { c->on = 0; return; }               /* переполнение — канал замолкает */
  c->freq = nf;
  c->inc  = apuSqInc(c->freq);
}

static inline void apuFsEnv(void) {                    /* 64 Гц */
  apuEnvStep(&apu.sq[0].e);
  apuEnvStep(&apu.sq[1].e);
  apuEnvStep(&apu.ns.e);
}

static inline void apuFrameStep(void) {
  const uint8_t s = apu.fsStep;
  if (!(s & 1)) apuFsLength();                         /* шаги 0, 2, 4, 6 */
  if (s == 2 || s == 6) apuFsSweep();
  if (s == 7) apuFsEnv();
  apu.fsStep = (uint8_t)((s + 1) & 7);
}

/* ---------------------------------------------------------------- семпл */

/* Один семпл: примерно -420..+420, 0 = тишина. Зовётся 22222 раза в секунду. */
static int32_t apuTick(void) {
  apu.fsAcc += 512;
  if (apu.fsAcc >= APU_RATE_HZ) { apu.fsAcc -= APU_RATE_HZ; apuFrameStep(); }

  if (!apu.powerOn) return 0;

  const int32_t c0 = apuSqSample(&apu.sq[0]);
  const int32_t c1 = apuSqSample(&apu.sq[1]);
  const int32_t c2 = apuWaveSample(&apu.wv);
  const int32_t c3 = apuNoiseSample(&apu.ns);

  const int32_t ch[4] = { c0, c1, c2, c3 };
  const uint8_t m = apu.outMap;
  int32_t l = 0, r = 0;
  for (int i = 0; i < 4; i++) {
    if (!ch[i]) continue;
    if (m & (0x10 << i)) l += ch[i];
    if (m & (0x01 << i)) r += ch[i];
  }
  const int32_t vl = (apu.outVol >> 4) & 7;
  const int32_t vr = apu.outVol & 7;
  return (l * vl + r * vr) / 2;                        /* моно: (лев + прав) / 2 */
}

/* Уровень ШИМ для пина звука: 0..689, тишина = APU_PWM_IDLE.
   sample — из apuTick(), vol — 0..100 %, amp — размах в уровнях ШИМ. */
#define APU_PWM_TOP   689
#define APU_PWM_IDLE  344
#define APU_PWM_MAXAMP 280

static inline uint16_t apuLevelVol(int32_t sample, uint8_t vol, int32_t amp) {
  int32_t lvl = APU_PWM_IDLE + (sample * amp * (int32_t)vol) / (APU_FULL_SCALE * 100);
  if (lvl < 16) lvl = 16;
  if (lvl > APU_PWM_TOP - 16) lvl = APU_PWM_TOP - 16;
  return (uint16_t)lvl;
}

static inline uint16_t apuLevel(int32_t sample) {
  return apuLevelVol(sample, 100, APU_PWM_MAXAMP);
}

/* v9.3: ЦИФРОВОЙ «РАЗДЕЛИТЕЛЬНЫЙ КОНДЕНСАТОР» (ФВЧ ~35 Гц).
   У настоящего DMG за ЦАПом стоит конденсатор: постоянка не доходит до динамика.
   У нас её не было: канал «включён, но молчит» давал −15 уровней смещения (с учётом
   микшера — десятки уровней ШИМ), динамик подмагничивался, а каждое включение/
   выключение канала — щелчок (на паузе это и слышалось как «треск/журчание»).
   Фильтр y = x − x[−1] + R·y[−1] срезает постоянку и превращает ступеньки в
   короткое затухание. */
static int32_t apuHpX = 0, apuHpY = 0;
static inline void apuDcReset(void) { apuHpX = 0; apuHpY = 0; }
static inline int32_t apuDcBlock(int32_t x) {
  /* R ≈ 0,99. Деление, а не сдвиг: арифметический сдвиг отрицательных значений
     округляет вниз и «залипает» на −1 (постоянка бы не сходилась к нулю). */
  const int32_t y = x - apuHpX + (apuHpY * 32440 / 32768);
  apuHpX = x;
  apuHpY = y;
  return y;
}

/* ------------------------------------------------- хуки для ядра peanut_gb.h
 *
 * Ядро (при ENABLE_SOUND = 1) вызывает audio_read()/audio_write() на каждый
 * доступ к 0xFF10..0xFF3F, но само их НЕ объявляет. Объявления обязаны стоять
 * выше #include "peanut_gb.h" — и вот почему они живут здесь, а не в скетче:
 * Arduino IDE перед сборкой вставляет автопрототипы ВСЕХ функций скетча в место
 * первой «настоящей» строки файла. Если такой строкой окажется объявление хука
 * (а оно обязательно выше ядра), то прототипы уедут выше подключения ядра,
 * и сборка упадёт с
 *     error: use of enum 'gb_error_e' without previous declaration.
 * Объявления внутри заголовка эту ловушку обходят: в скетче до ядра остаются
 * только комментарии, #include и #define.
 *
 * Определения даём здесь же, как static inline — в скетче ничего добавлять не надо.
 * Включается макросом: скетч пишет  #define APU_GB_PEANUT_HOOKS 1  до include.
 */
#if APU_GB_PEANUT_HOOKS
static inline uint8_t audio_read(uint16_t addr)               { return apuRead(addr); }
static inline void    audio_write(uint16_t addr, uint8_t val) { apuWrite(addr, val); }
#endif

#endif /* APU_GB_H */
