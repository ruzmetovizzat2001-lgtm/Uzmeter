/*
 * Uz-Meter — STM32F103C8Tx (STM32duino 3.0.0) proshivkasi (uzmeter3 dan ko'chirilgan)
 *
 *  - Sozlash (IR) : SoftwareSerial (TIM4), PB10 = TX, PB11 = RX, 2400 8N1 (PROTOCOL.md)
 *  - IR yoqish    : PB5 — DRV5033 (Hall), magnit yaqinlashsa LOW -> IR sozlash oynasi ochiladi
 *  - Hisoblagich  : USART1,  PA9 = TX, PA10 = RX, 2400 8E1 (CJ/T188: Olish / Ochish / Yopish)
 *  - Impulsli     : PB14, PB15 — 2 ta Hall datchik (PC dasturida "Impulsli" tanlansa RS485 o'rniga)
 *  - APN          : SIM IMSI (MCC+MNC) bo'yicha avtomatik (Beeline, Ucell, Perfectum, Mobiuz, Uzmobile)
 *  - MAX enable   : PB1  (hisoblagich UART interfeysi)
 *  - 13V enable   : PB2  (hisoblagich / klapan). F103 da PB2 = BOOT1 — yuklanishdan keyin oddiy GPIO
 *  - BG95-M3      : USART2,  PA2 = TX, PA3 = RX, 115200
 *  - 4V enable    : PB9  (BG95 quvvati, HIGH = yoqilgan)
 *  - PWRKEY       : PB8  (tranzistor orqali HIGH impuls)
 *  - Batareya     : PA4, 1:2 bo'luvchi (Vbat = Vpin * 2), VDDA VREFINT (1.20 V) orqali
 *  - LED          : PC13 — IR oynasi ochiq paytda yonadi (sxemangizga moslang; Blue Pill da LED_ON = LOW)
 *  - Sozlama      : EEPROM emulyatsiyasi (flash'ning oxirgi sahifasi)
 *
 * Kutubxonalar (Library Manager): "STM32duino Low Power", "STM32duino RTC"
 * Board: Generic STM32F1 series -> Generic F103C8Tx (yoki BluePill F103C8)
 * Tools -> U(S)ART support: "Enabled (no generic 'Serial')"
 */

#include <Arduino.h>
#include <EEPROM.h>
#include <SoftwareSerial.h>
#include <STM32LowPower.h>
#include <STM32RTC.h>
#include <time.h>

// ================= SOZLAMALAR =================
#define CFG_BAUD 2400
#define METER_BAUD 2400
#define MODEM_BAUD 115200

#define PIN_HALL PB5  // IR almashishni yoqish (magnit bor = LOW)
// Impulsli hisoblagich: 1 aylanishda ikkala datchik bittadan yonib o'chadi (magnit bor = LOW).
// Aylanish A datchik B dan keyin ishlaganda sanaladi — bitta datchik oldida tebranish hisobni oshirmaydi.
#define PIN_PULSE_A PB14
#define PIN_PULSE_B PB15
#define METER_KIND_PULSE 4  // PC dasturidagi "Tur 5 — Impulsli" (meter_type = 4); boshqa turlarda RS485 (CJ/T188)
#define PIN_BAT PA4
// #define PIN_BAT_EN PA5  // bo'luvchi kaliti bo'lsa izohdan chiqaring
#define PIN_LED PC13
#define LED_ON HIGH  // Blue Pill: PC13 LED katod tomoni MCU ga ulangan — LOW
#define LED_OFF (!LED_ON)

#define PIN_MAX_EN PB1  // MAX + IR qismi quvvati (PB1 = HIGH bo'lsa IR ishlamaydi)
#define MAX_ON LOW      // PB1 PNP tranzistorga ulangan: LOW = yoqilgan, HIGH = o'chiq
#define MAX_OFF (!MAX_ON)
#define MAX_RS485_DIR 0  // 1 = PB1 MAX485 ning DE/RE oyog'i: faqat uzatish paytida yoqiladi
#define PIN_13V_EN PB2   // 13V (HIGH = yoqilgan)
#define V13_SETTLE_MS 300
#define USE_13V 0  // 0 = 13V (PB2) umuman yoqilmaydi, PB2 doim LOW; 1 = hisoblagich/klapan bilan ishlashda yoqiladi
#define VALVE_USE_13V 1  // 1 = klapan buyrug'i paytida 13V yoqiladi (motor quvvati), USE_13V 0 bo'lsa ham
#define MAX_SETTLE_MS 20  // MAX485 enable'dan keyin uzatishgacha kutish

#define PIN_4V_EN PB9   // BG95 quvvati
#define PIN_PWRKEY PB8  // BG95 PWRKEY

// Bo'luvchi: batareya -> R1 -> pin -> R2 -> GND (500k / 500k).
// Qarshilik katta — ADC namuna olish vaqti build_opt.h da uzaytirilgan (239.5 takt)
#define BAT_R1 500000UL
#define BAT_R2 500000UL
#define BAT_EMPTY_MV 3300
#define BAT_FULL_MV 3600  // 3.6 V = 100%
#define BAT_LOW_MV 3000      // o'lchov bundan past chiqsa —
#define BAT_LOW_ADD_MV 1300  // shuncha qo'shib hisoblanadi

#define RTC_SRC STM32RTC::LSI_CLOCK  // 32.768 kHz kvars bo'lsa: STM32RTC::LSE_CLOCK (tavsiya etiladi)

#define TZ_OFFSET_S (5 * 3600L)  // Toshkent, UTC+5
#define NTP_SERVER "pool.ntp.org"

#define CONFIG_WINDOW_MS 60000UL
#define CONFIG_WINDOW_MAX_MS 600000UL  // Hall LOW qotib qolsa, oyna 10 daqiqadan oshmasin
#define SEND_AFTER_CONFIG 1
#define TEST_INTERVAL_S 0  // 0 = 86400/sends_per_day; sinov uchun masalan 60
#define RETRY_S 600UL
#define MAX_RETRIES 2

// HTTP qayta yuborish: birinchi urinish + HTTP_RETRIES ta qayta urinish.
// HTTP 4xx/5xx (masalan 400) ham, timeout ham qayta yuboriladi.
#define HTTP_RETRIES 5                 // qo'shimcha urinishlar soni (jami 1 + 5 = 6)
#define HTTP_RETRY_DELAY_MS 5000UL     // urinishlar orasidagi kutish
#define HTTP_RECONFIG_ON_RETRY 1       // 1 = har qayta yuborishdan oldin internet sozlamalari qaytadan o'rnatiladi
#define HTTP_TOTAL_BUDGET_MS 300000UL  // bitta hisobot uchun umumiy chegara (batareya uchun), 0 = cheksiz
#define AT_CFG_GAP_MS 200              // ketma-ket sozlash buyruqlari orasidagi tanaffus
#define NET_REG_TIMEOUT_MS 60000UL  // CEREG/CGATT ro'yxatdan o'tishi uchun kutish

// Hisoblagich (CJ/T188). Manzil sozlamadagi M_NO dan olinadi (meterAddr), masalan
// M_NO "17022604000046" -> 46 00 00 04 26 02 17 (BCD, kichik bayt birinchi).
// M_NO 14 ta raqam bo'lmasa yoki hammasi 0 bo'lsa — shu zaxira manzil ishlatiladi
static const uint8_t METER_ADDR[7] = { 0x37, 0x01, 0x00, 0x12, 0x25, 0x02, 0x17 };
#define METER_TYPE 0x10  // suv hisoblagichi
#define METER_READ_TIMEOUT_MS 1000
// IR orqali o'qishda (CMD_GET_DATA) kompyuter dasturi javobni 2.5 s kutadi:
// 13V (300 ms) + MAX (20 ms) + shu vaqt + javobni IR orqali uzatish (~0.4 s) undan oshmasligi kerak
#define METER_IR_TIMEOUT_MS 1000
// 0 = IR o'qishda hisoblagich o'qilmaydi (13V yoqilmaydi), javob darhol chiqadi.
//     Hajm sifatida oxirgi serverga yuborishda (doSend) o'qilgan qiymat ketadi.
#define IR_READ_METER 1  // 1 = IR o'qishda hisoblagich o'qiladi; javob bermasa oxirgi qiymat ketadi

// IR diagnostikasi (debug portsiz, LED qutbiga bog'liq emas):
//   oyna ochiq          — LED sekin lipillaydi (0.5 s yonadi / 0.5 s o'chadi)
//   to'g'ri buyruq keldi — 5 marta juda tez lipillaydi
//   CRC xato kadr        — 3 marta o'rtacha tezlikda lipillaydi
//   oyna yopiq / ishga tushish — LED o'zgarmaydi
#define IR_DIAG_LED 1
#define METER_VALVE_TIMEOUT_MS 5000
#define VALVE_SETTLE_MS 5000  // klapan harakatini tugatishi uchun kutish, keyin holat qayta o'qiladi

// F103C8 da 3 ta USART bor: USART1 — hisoblagich, USART2 — modem, USART3 (PB10/PB11) — IR pinlarida,
// lekin IR SoftwareSerial'da qoldirildi (L051 platasida shu ishlagan). DEBUG 1 bo'lsa log SoftwareSerial orqali
// PA0 (TX) / PA1 (RX) ga 9600 8N1 da chiqadi (PB14/PB15 endi impuls datchiklari). Seriyada 0 qiling.
#define DEBUG 0
#define DBG_BAUD 9600
#define PIN_DBG_TX PA0
#define PIN_DBG_RX PA1

// APN jadvali qatori. Arduino IDE funksiya prototiplarini birinchi funksiyadan oldin
// avtomatik qo'yadi, shuning uchun bu struct undan ham oldinroqda e'lon qilinishi kerak.
struct ApnEntry {
  const char* mccmnc;  // IMSI ning birinchi 5 raqami (MCC + MNC)
  const char* op;
  const char* apn;
  const char* user;
  const char* pass;
  uint8_t auth;  // 0 yo'q, 1 PAP, 2 CHAP
};
// M2M / korporativ SIM'ning maxsus APN'i bo'lsa — izohdan chiqaring, jadval ishlatilmaydi
// #define APN_OVERRIDE "m2m.example.uz"
// ==============================================

Uart CfgSerial(PB11, PB10);  // (RX, TX) — IR. Apparat varianti: Uart CfgSerial(PB11, PB10) — USART3
Uart MeterSerial(PA10, PA9);           // USART1  (RX, TX) — hisoblagich
Uart Modem(PA3, PA2);                  // USART2  (RX, TX)

#if DEBUG
SoftwareSerial DbgSerial(PIN_DBG_RX, PIN_DBG_TX);  // log (RX, TX)
#define DBG(...) DbgSerial.print(__VA_ARGS__)
#define DBGLN(...) DbgSerial.println(__VA_ARGS__)
#else
#define DBG(...)
#define DBGLN(...)
#endif

STM32RTC& rtc = STM32RTC::getInstance();

// Takt: STM32duino standart sozlamasi (F103C8 variantida). L051 dagi o'z SystemClock_Config olib tashlandi.

// ================= PROTOKOL =================
#define SOF0 0xAA
#define SOF1 0x55
#define CMD_SET_CONFIG 0x01
#define CMD_GET_DATA 0x02
#define CMD_SET_ACK 0x81
#define CMD_DATA 0x82

struct Config {
  char device_no[16];
  char m_no[15];
  uint8_t valve;  // 0 yopiq, 1 ochiq
  uint8_t unit;
  uint8_t sends_per_day;
  uint8_t meter_type;
  char url[129];
};

const Config DEFAULT_CFG = {
  "000000000000000", "00000000000000", 1, 0, 6, 0,
  "https://cabinet.uzmeter.uz/dictionary/water"
};
Config cfg;

// Telemetriya
uint32_t water_volume_l = 0;  // hisoblagichdan (Olish)
uint16_t battery_mv = 0;
uint8_t battery_pct = 0;
int mcu_temp_c = 0;
int csq_raw = 99;
uint8_t signal_level = 1;
bool timeSynced = false;
int8_t valveState = -1;  // -1 noma'lum, 0 ochiq, 1 yopiq, 2 nosoz

// Rejalashtirish
uint32_t nextSendEpoch = 0;
bool sendNow = false;
uint8_t retries = 0;
volatile bool hallWake = false;
bool rtcReady = false;  // rtc.begin() chaqirilgandan keyin true

// Impuls hisobi (PB14/PB15)
volatile uint32_t pulseRevs = 0;  // hali litrga o'tkazilmagan aylanishlar
volatile uint8_t pulseLast = 0;   // oxirgi ishlagan datchik: 1 = A, 2 = B
uint32_t pulseLiters = 0;         // jami hajm (litr), flash'da saqlanadi
uint32_t pulseSavedLiters = 0;    // flash'dagi qiymat (o'zgarmasa qayta yozilmaydi)

uint16_t crc16(const uint8_t* d, size_t n) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    crc ^= d[i];
    for (uint8_t b = 0; b < 8; b++) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
  }
  return crc;
}

uint32_t nowEpoch() {
  return (uint32_t)rtc.getEpoch();
}

uint32_t intervalSec() {
#if TEST_INTERVAL_S
  return TEST_INTERVAL_S;
#else
  uint8_t spd = cfg.sends_per_day;
  if (spd < 1) spd = 1;  // 0 ga bo'linishdan himoya
  if (spd > 24) spd = 24;
  return 86400UL / spd;
#endif
}

// ================= O'LCHOVLAR =================
// F1 da VREFINT kalibrlash qiymati yo'q — datasheet bo'yicha tipik 1.20 V (1.16..1.24 V)
#define VREFINT_MV 1200
// Ichki harorat sensori (F103 datasheet): V25 = 1.43 V, qiyalik 4.3 mV/°C
#define TS_V25_MV 1430
#define TS_SLOPE_UV 4300

// VREFINT (1.20 V) dan haqiqiy ta'minot (VDD) — 16 ta o'qish, har biri orasida 1 ms
uint32_t readVddaMv() {
  uint32_t raw = 0;
  for (int i = 0; i < 16; i++) {
    raw += analogRead(AVREF);
    delay(1);
  }
  raw /= 16;
  if (raw == 0) return 3300;
  return VREFINT_MV * 4095UL / raw;
}

// Batareya pinidan 10 ta namuna (orasida 1 ms), eng kattasi olinadi —
// 500k bo'luvchida ADC kondensatori to'liq zaryadlanmay qolgan past namunalar tashlanadi
#define BAT_SAMPLES 10
uint32_t readBatteryRaw() {
  uint32_t best = 0;
  for (int i = 0; i < BAT_SAMPLES; i++) {
    uint32_t r = analogRead(PIN_BAT);
    if (r > best) best = r;
    delay(1);
  }
  return best;
}

void readSensors() {
  uint32_t vdda = readVddaMv();

#ifdef PIN_BAT_EN
  digitalWrite(PIN_BAT_EN, HIGH);
  delay(2);
#endif
  // VDD = 1.20 V * 4095 / VREFINT;  Vpin = raw / 4095 * VDD;  Vbat = Vpin * (R1 + R2) / R2
  uint32_t vpin = readBatteryRaw() * vdda / 4095;
  uint32_t mv = (uint32_t)((uint64_t)vpin * (BAT_R1 + BAT_R2) / BAT_R2);
#ifdef PIN_BAT_EN
  digitalWrite(PIN_BAT_EN, LOW);
#endif
  if (mv < BAT_LOW_MV) mv += BAT_LOW_ADD_MV;  // eng kattasi ham 3 V dan past bo'lsa — 1.3 V qo'shiladi
  battery_mv = (uint16_t)mv;

  int32_t pct = ((int32_t)battery_mv - BAT_EMPTY_MV) * 100 / (BAT_FULL_MV - BAT_EMPTY_MV);
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  battery_pct = (uint8_t)pct;

  int32_t vsense = (int32_t)(analogRead(ATEMP) * vdda / 4095);
  mcu_temp_c = (int)((TS_V25_MV - vsense) * 1000L / TS_SLOPE_UV + 25);

  DBG("Bat=");
  DBG(battery_mv);
  DBG(" mV (");
  DBG(battery_pct);
  DBG("%), T=");
  DBGLN(mcu_temp_c);
}

uint8_t csqToLevel(int c) {
  if (c == 99 || c < 5) return 1;
  if (c < 10) return 2;
  if (c < 15) return 3;
  if (c < 20) return 4;
  return 5;
}

// ================= IMPULSLI HISOBLAGICH =================
bool pulseMode() {
  return cfg.meter_type == METER_KIND_PULSE;
}

// cfg.unit: 0 = 1 L, 1 = 10 L, 2 = 100 L, 3 = 1000 L bir aylanishga
uint32_t litersPerRev() {
  static const uint16_t L[] = { 1, 10, 100, 1000 };
  return L[cfg.unit & 3];
}

void onPulseA() {
  if (pulseLast == 2) pulseRevs++;  // B dan keyin A — to'liq aylanish
  pulseLast = 1;
}

void onPulseB() {
  pulseLast = 2;
}

// To'plangan aylanishlarni litrga o'tkazadi va water_volume_l ni yangilaydi
void pulseUpdate() {
  noInterrupts();
  uint32_t r = pulseRevs;
  pulseRevs = 0;
  interrupts();
  pulseLiters += r * litersPerRev();
  water_volume_l = pulseLiters;
}

// ================= HISOBLAGICH (CJ/T188) =================
bool meterPowered = false;
// PB1 PNP kaliti IR qismini ham quvvatlaydi: IR oynasi ochiq paytda PB1 o'chirilmaydi
bool irActive = false;

// MAX485 ishlatishdan oldin: avval quvvat (PB2, 13V), u barqarorlashgach enable (PB1), keyin USART1
void meterOn() {
  if (meterPowered) return;
#if USE_13V
  digitalWrite(PIN_13V_EN, HIGH);
  delay(V13_SETTLE_MS);
#endif
#if !MAX_RS485_DIR
  digitalWrite(PIN_MAX_EN, MAX_ON);
  delay(MAX_SETTLE_MS);
#endif
  MeterSerial.begin(METER_BAUD, SERIAL_8E1);
  meterPowered = true;
  DBGLN("MAX485: 13V (PB2) + enable (PB1) yoqildi");
}

// USART1 yopiladi, pinlar analog (quvvatsiz MAX ga TX orqali tok bermaslik uchun), 13V + MAX o'chadi
void meterOff() {
  if (!meterPowered) return;
  MeterSerial.flush();
  MeterSerial.end();
  pinMode(PA9, INPUT_ANALOG);
  pinMode(PA10, INPUT_ANALOG);
  if (!irActive) digitalWrite(PIN_MAX_EN, MAX_OFF);
  digitalWrite(PIN_13V_EN, LOW);
  meterPowered = false;
  DBGLN("MAX485: PB1 + PB2 o'chirildi");
}

uint8_t mresp[128];

static uint8_t bcd(uint8_t b) {
  return (b >> 4) * 10 + (b & 0x0F);
}

// To'liq, CS to'g'ri javob freymi (68 ... CS 16) boshining indeksi; topilmasa -1
static int meterFindFrame(const uint8_t* b, size_t n) {
  for (size_t s = 0; s + 13 <= n; s++) {
    if (b[s] != 0x68 || !(b[s + 9] & 0x80)) continue;  // faqat hisoblagich -> MCU (exo o'tkazib yuboriladi)
    size_t end = s + 12 + b[s + 10];
    if (end >= n || b[end] != 0x16) continue;
    uint8_t cs = 0;
    for (size_t i = s; i < end - 1; i++) cs += b[i];
    if (cs == b[end - 1]) return (int)s;
  }
  return -1;
}

static int meterTransactRaw(uint8_t ctrl, const uint8_t* data, uint8_t len, uint32_t timeoutMs);

// Freym yuboradi va javobni kutadi. Javob mresp[] da, natija — uning boshi yoki -1.
// MAX485 yoqilmagan bo'lsa, PB2 + PB1 shu yerda yoqiladi va almashinuvdan keyin o'chiriladi.
static int meterTransact(uint8_t ctrl, const uint8_t* data, uint8_t len, uint32_t timeoutMs) {
  bool was = meterPowered;
  meterOn();
  int r = meterTransactRaw(ctrl, data, len, timeoutMs);
  if (!was) meterOff();
  return r;
}

// cfg.m_no (14 ta raqam) -> 7 bayt BCD manzil, oxirgi juft raqam birinchi bayt
static void meterAddr(uint8_t* a) {
  bool ok = strlen(cfg.m_no) == 14, nonzero = false;
  for (int i = 0; ok && i < 14; i++) {
    if (cfg.m_no[i] < '0' || cfg.m_no[i] > '9') ok = false;
    else if (cfg.m_no[i] != '0') nonzero = true;
  }
  if (!ok || !nonzero) {
    memcpy(a, METER_ADDR, 7);
    return;
  }
  for (int i = 0; i < 7; i++) {
    const char* d = cfg.m_no + 12 - 2 * i;
    a[i] = ((d[0] - '0') << 4) | (d[1] - '0');
  }
}

static int meterTransactRaw(uint8_t ctrl, const uint8_t* data, uint8_t len, uint32_t timeoutMs) {
  if (len > 16) return -1;
  // 4xFE preambula + (68, tur, manzil[7], CTRL, LEN) + ma'lumot[<=16] + CS + 16
  uint8_t f[4 + 12 + 16 + 2];
  size_t n = 0;
  for (int i = 0; i < 4; i++) f[n++] = 0xFE;  // uyg'otish preambulasi
  size_t s = n;
  f[n++] = 0x68;
  f[n++] = METER_TYPE;
  meterAddr(f + n);
  n += 7;
  f[n++] = ctrl;
  f[n++] = len;
  memcpy(f + n, data, len);
  n += len;
  uint8_t cs = 0;
  for (size_t i = s; i < n; i++) cs += f[i];
  f[n++] = cs;
  f[n++] = 0x16;

  while (MeterSerial.available()) MeterSerial.read();
#if MAX_RS485_DIR
  digitalWrite(PIN_MAX_EN, MAX_ON);
#endif
  MeterSerial.write(f, n);
  MeterSerial.flush();
#if MAX_RS485_DIR
  if (!irActive) digitalWrite(PIN_MAX_EN, MAX_OFF);
#endif

  size_t rl = 0;
  uint32_t t = millis();
  while (millis() - t < timeoutMs) {
    while (MeterSerial.available()) {
      uint8_t b = MeterSerial.read();
      if (rl >= sizeof(mresp)) {  // bufer to'ldi: eng eski yarmini tashlaymiz, yangi baytlar yo'qolmasin
        size_t h = rl / 2;
        memmove(mresp, mresp + h, rl - h);
        rl -= h;
      }
      mresp[rl++] = b;
      if (b == 0x16) {
        int st = meterFindFrame(mresp, rl);
        if (st >= 0) return st;
      }
    }
  }
  return -1;
}

// Hajm va klapan holatini o'qiydi; hisoblagich javobi timeoutMs gacha kutiladi
bool OlishT(uint32_t timeoutMs) {
  static const uint8_t d[] = { 0x1F, 0x90, 0x00 };  // DI = 901F, SER = 00
  int s = meterTransact(0x01, d, sizeof(d), timeoutMs);
  if (s < 0 || mresp[s + 9] != 0x81 || mresp[s + 10] < 22) {
    DBGLN("Hisoblagich javob bermadi");
    return false;
  }
  // Ma'lumot: DI0 DI1 SER | hajm[4] birlik | hisob hajmi[4] birlik | vaqt[7] | ST0 ST1
  const uint8_t* p = mresp + s + 11;

  uint32_t v = 0;
  for (int i = 6; i >= 3; i--) v = v * 100 + bcd(p[i]);  // eski koddagi response[19..16]
  water_volume_l = v;

  switch (p[20] & 0x03) {  // ST0: 00 ochiq, 01 yopiq, 11 nosoz
    case 0: valveState = 0; break;
    case 1: valveState = 1; break;
    default: valveState = 2; break;
  }

  DBG("Hajm=");
  DBG(water_volume_l);
  DBG(" klapan=");
  DBGLN(valveState);
  return true;
}

bool Olish() {
  return OlishT(METER_READ_TIMEOUT_MS);
}

static bool valveCmd(bool open) {
  uint8_t d[] = { 0xA0, 0x17, 0x05, (uint8_t)(open ? 0x55 : 0x99) };
#if VALVE_USE_13V && !USE_13V
  digitalWrite(PIN_13V_EN, HIGH);
  delay(V13_SETTLE_MS);
#endif
  int s = meterTransact(0x04, d, sizeof(d), METER_VALVE_TIMEOUT_MS);
  bool ok = (s >= 0 && mresp[s + 9] == 0x84);
#if VALVE_USE_13V && !USE_13V
  if (ok) delay(VALVE_SETTLE_MS);  // motor harakatini tugatguncha 13V yoniq turadi
  digitalWrite(PIN_13V_EN, LOW);
#endif
  if (ok) valveState = open ? 0 : 1;
  DBG(open ? "Ochish: " : "Yopish: ");
  DBGLN(ok ? "OK" : "javob yo'q");
  return ok;
}

bool Ochish() {
  return valveCmd(true);
}
bool Yopish() {
  return valveCmd(false);
}

// Hisoblagich quvvatini o'zi boshqaradi. force = false bo'lsa, klapan allaqachon shu holatda bo'lsa hech narsa qilmaydi
void setValve(bool open, bool force) {
  if (!force && valveState == (open ? 0 : 1)) return;
  bool was = meterPowered;
  meterOn();
  if (open) Ochish();
  else Yopish();
  if (!was) meterOff();
}

// ================= EEPROM'DA SAQLASH =================
#define CFG_MAGIC 0x557A4D32UL
struct Stored {
  uint32_t magic;
  Config cfg;
  uint16_t crc;
};

// F103 da EEPROM flash'da emulyatsiya qilinadi: EEPROM.write() har bayt uchun butun sahifani o'chirib yozadi.
// Shuning uchun bufer orqali o'qiymiz/yozamiz — sahifa bitta saqlashda bir marta yoziladi.
void loadConfig() {
  Stored s;
  uint8_t* p = (uint8_t*)&s;
  eeprom_buffer_fill();
  for (size_t i = 0; i < sizeof(s); i++) p[i] = eeprom_buffered_read_byte(i);

  if (s.magic == CFG_MAGIC && s.crc == crc16((uint8_t*)&s.cfg, sizeof(Config))) {
    cfg = s.cfg;
    DBGLN("Sozlama EEPROM'dan o'qildi");
  } else {
    cfg = DEFAULT_CFG;
    DBGLN("EEPROM bo'sh — standart sozlama");
  }
  cfg.device_no[15] = 0;
  cfg.m_no[14] = 0;
  cfg.url[128] = 0;
  if (cfg.sends_per_day < 1 || cfg.sends_per_day > 24) cfg.sends_per_day = 6;
}

void saveConfig() {
  Stored s;
  memset(&s, 0, sizeof(s));
  s.magic = CFG_MAGIC;
  s.cfg = cfg;
  s.crc = crc16((uint8_t*)&s.cfg, sizeof(Config));

  const uint8_t* p = (const uint8_t*)&s;
  bool changed = false;
  eeprom_buffer_fill();
  for (size_t i = 0; i < sizeof(s); i++) {
    if (eeprom_buffered_read_byte(i) != p[i]) {
      eeprom_buffered_write_byte(i, p[i]);
      changed = true;
    }
  }
  if (changed) eeprom_buffer_flush();  // o'zgarmagan bo'lsa flash'ga yozilmaydi
  DBGLN(changed ? "Sozlama saqlandi" : "Sozlama o'zgarmagan");
}

// Impuls hajmi sozlamadan keyin alohida joyda saqlanadi (flash resursi uchun faqat yuborishda yoziladi)
#define CNT_OFFSET 256
#define CNT_MAGIC 0x506C7331UL
struct StoredCnt {
  uint32_t magic;
  uint32_t liters;
  uint32_t inv;  // ~liters — tekshiruv
};

void loadCounter() {
  StoredCnt c;
  uint8_t* p = (uint8_t*)&c;
  eeprom_buffer_fill();
  for (size_t i = 0; i < sizeof(c); i++) p[i] = eeprom_buffered_read_byte(CNT_OFFSET + i);
  pulseLiters = (c.magic == CNT_MAGIC && c.inv == ~c.liters) ? c.liters : 0;
  pulseSavedLiters = pulseLiters;
  water_volume_l = pulseLiters;
}

void saveCounter() {
  if (pulseLiters == pulseSavedLiters) return;
  StoredCnt c = { CNT_MAGIC, pulseLiters, ~pulseLiters };
  const uint8_t* p = (const uint8_t*)&c;
  eeprom_buffer_fill();
  for (size_t i = 0; i < sizeof(c); i++) eeprom_buffered_write_byte(CNT_OFFSET + i, p[i]);
  eeprom_buffer_flush();
  pulseSavedLiters = pulseLiters;
  DBGLN("Impuls hajmi saqlandi");
}

// ================= SOZLASH UART (IR) =================
void sendFrame(uint8_t cmd, const uint8_t* payload, uint8_t len) {
  uint8_t buf[4 + 255 + 2];
  buf[0] = SOF0;
  buf[1] = SOF1;
  buf[2] = cmd;
  buf[3] = len;
  memcpy(buf + 4, payload, len);
  uint16_t c = crc16(buf + 2, len + 2);
  buf[4 + len] = c & 0xFF;
  buf[5 + len] = c >> 8;
  CfgSerial.write(buf, 6 + len);
}

size_t encodeConfig(uint8_t* p) {
  size_t n = 0;
  memcpy(p + n, cfg.device_no, 15);
  n += 15;
  memcpy(p + n, cfg.m_no, 14);
  n += 14;
  p[n++] = cfg.valve;
  p[n++] = cfg.unit;
  p[n++] = cfg.sends_per_day;
  p[n++] = cfg.meter_type;
  uint8_t ul = strlen(cfg.url);
  p[n++] = ul;
  memcpy(p + n, cfg.url, ul);
  n += ul;
  return n;
}

static bool safeText(const uint8_t* s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    uint8_t c = s[i];
    if (c == 0) break;
    if (c <= 0x20 || c > 0x7E || c == '"' || c == '\\') return false;
  }
  return true;
}

bool initialSet = false;       // PC boshlang'ich hajm yubordi (faqat impulsli)
uint32_t initialLiters = 0;    // shu hajm (litr)

uint8_t applyConfig(const uint8_t* p, uint8_t len) {
  if (len < 34) return 2;
  uint8_t ul = p[33];
  if (ul == 0 || ul > 128 || len < 34 + ul) return 2;
  if (p[29] > 1 || p[30] > 3 || p[31] < 1 || p[31] > 24 || p[32] > 5) return 2;
  if (!safeText(p, 15) || !safeText(p + 15, 14) || !safeText(p + 34, ul)) return 2;

  memcpy(cfg.device_no, p, 15);
  cfg.device_no[15] = 0;
  memcpy(cfg.m_no, p + 15, 14);
  cfg.m_no[14] = 0;
  cfg.valve = p[29];
  cfg.unit = p[30];
  cfg.sends_per_day = p[31];
  cfg.meter_type = p[32];
  memset(cfg.url, 0, sizeof(cfg.url));
  memcpy(cfg.url, p + 34, ul);

  // Ixtiyoriy: CONFIG oxirida u32 LE boshlang'ich hajm (litr) — FAQAT impulsli hisoblagich (meter_type = 4).
  // Boshqa turlarda (masalan ultratovushli) ortiqcha baytlar e'tiborga olinmaydi.
  initialSet = false;
  if (cfg.meter_type == METER_KIND_PULSE && len >= 34 + ul + 4) {
    const uint8_t* q = p + 34 + ul;
    initialLiters = (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
    initialSet = true;
  }
  return 0;
}

void handleFrame(uint8_t cmd, const uint8_t* payload, uint8_t len) {
  if (cmd == CMD_SET_CONFIG) {
    uint8_t oldValve = cfg.valve;
    uint8_t st = applyConfig(payload, len);
    if (st == 0) {
      saveConfig();
      if (initialSet) {
        // Impulsli hisoblagich shu qiymatdan boshlab sanaydi
        noInterrupts();
        pulseRevs = 0;
        interrupts();
        pulseLiters = initialLiters;
        water_volume_l = pulseLiters;
        saveCounter();
        initialSet = false;
      }
      // Yoqilgandagi birinchi oynada RTC hali ishga tushmagan — reja setup() oxirida sendNow orqali tuziladi
      if (rtcReady) nextSendEpoch = nowEpoch() + intervalSec();
      retries = 0;
#if SEND_AFTER_CONFIG
      sendNow = true;
#endif
    }
    sendFrame(CMD_SET_ACK, &st, 1);
    // Sozlamadagi klapan o'zgarganda yoki haqiqiy holat (hisoblagichdan o'qilgan) undan farq qilganda
    if (st == 0 && !pulseMode() && (cfg.valve != oldValve || valveState != (cfg.valve ? 0 : 1))) {
      CfgSerial.flush();
      setValve(cfg.valve, true);  // avval ACK, keyin klapan
    }
  } else if (cmd == CMD_GET_DATA) {
    readSensors();
    if (pulseMode()) {
      pulseUpdate();
    } else {
#if IR_READ_METER
      meterOn();
      OlishT(METER_IR_TIMEOUT_MS);  // javob dastur timeout'idan (2.5 s) oldin chiqishi uchun
      meterOff();
#endif
    }

    uint8_t out[34 + 128 + 7 + 8];  // zaxira bilan
    size_t n = encodeConfig(out);
    out[n++] = water_volume_l & 0xFF;
    out[n++] = (water_volume_l >> 8) & 0xFF;
    out[n++] = (water_volume_l >> 16) & 0xFF;
    out[n++] = (water_volume_l >> 24) & 0xFF;
    out[n++] = battery_mv & 0xFF;
    out[n++] = battery_mv >> 8;
    out[n++] = battery_pct;
    sendFrame(CMD_DATA, out, n);
  }
}

uint8_t rx[4 + 255 + 2];
size_t rxLen = 0;
uint32_t rxLast = 0;

void ledBlink(uint8_t times, uint16_t ms) {
  for (uint8_t i = 0; i < times; i++) {
    digitalWrite(PIN_LED, LED_ON);
    delay(ms);
    digitalWrite(PIN_LED, LED_OFF);
    delay(ms);
  }
}

bool pollConfigUart() {
  bool got = false;
  if (rxLen && millis() - rxLast > 200) rxLen = 0;
  while (CfgSerial.available()) {
    uint8_t b = CfgSerial.read();
    got = true;
    rxLast = millis();

    if (rxLen == 0 && b != SOF0) continue;
    if (rxLen == 1 && b != SOF1) {
      rxLen = (b == SOF0) ? 1 : 0;
      continue;
    }
    rx[rxLen++] = b;

    if (rxLen >= 4) {
      size_t total = 4 + rx[3] + 2;
      if (rxLen == total) {
        uint16_t c = (uint16_t)rx[total - 2] | ((uint16_t)rx[total - 1] << 8);
        if (crc16(rx + 2, rx[3] + 2) == c) {
#if IR_DIAG_LED
          // o'z uzatishimizning aks-sadosi (0x81/0x82) emas, kompyuterdan kelgan buyruq bo'lsa
          if (rx[2] == CMD_SET_CONFIG || rx[2] == CMD_GET_DATA) ledBlink(5, 40);
#endif
          handleFrame(rx[2], rx + 4, rx[3]);
        } else {
#if IR_DIAG_LED
          ledBlink(3, 150);
#endif
          if (rx[2] == CMD_SET_CONFIG) {
            uint8_t st = 1;
            sendFrame(CMD_SET_ACK, &st, 1);
          }
        }
        rxLen = 0;
      }
    }
    if (rxLen >= sizeof(rx)) rxLen = 0;
  }
  return got;
}

// IR sozlash oynasi: PB5 magnit bilan ochiladi, oxirgi bayt/magnitdan keyin CONFIG_WINDOW_MS ochiq turadi.
// Hall sensor LOW holatda qotib qolsa, oyna CONFIG_WINDOW_MAX_MS dan oshmaydi (qattiq cheklov).
void configWindow() {
  DBGLN("\nIR oynasi ochiq");
  irActive = true;
  digitalWrite(PIN_MAX_EN, MAX_ON);  // IR qismining quvvati (PB1 PNP kaliti)
  delay(MAX_SETTLE_MS);
  CfgSerial.begin(CFG_BAUD);
  while (CfgSerial.available()) CfgSerial.read();
  rxLen = 0;
  digitalWrite(PIN_LED, LED_ON);

  uint32_t start = millis();
  uint32_t last = start;
  while (millis() - last < CONFIG_WINDOW_MS && millis() - start < CONFIG_WINDOW_MAX_MS) {
    if (pollConfigUart()) last = millis();
    if (digitalRead(PIN_HALL) == LOW) last = millis();  // magnit turibdi — oyna ochiq
#if IR_DIAG_LED
    digitalWrite(PIN_LED, ((millis() - start) / 500) & 1 ? LED_OFF : LED_ON);  // oyna ochiq — sekin lipillash
#endif
  }

  CfgSerial.flush();
  CfgSerial.end();
  pinMode(PB10, INPUT_ANALOG);
  pinMode(PB11, INPUT_ANALOG);
  irActive = false;
  if (!meterPowered) digitalWrite(PIN_MAX_EN, MAX_OFF);
  digitalWrite(PIN_LED, LED_OFF);
  hallWake = false;
  DBGLN("IR oynasi yopildi");
}

// ================= MODEM =================
char mbuf[512];
size_t mlen = 0;

static void mAppend(char c) {
  if (mlen >= sizeof(mbuf) - 1) {
    size_t h = mlen / 2;
    memmove(mbuf, mbuf + h, mlen - h);
    mlen -= h;
  }
  mbuf[mlen++] = c;
  mbuf[mlen] = 0;
}

void readMore(uint32_t ms) {
  size_t before = mlen;
  uint32_t t = millis();
  while (millis() - t < ms) {
    while (Modem.available()) mAppend(Modem.read());
  }
  if (before > mlen) before = 0;  // mAppend bufer to'lganda siljitgan bo'lishi mumkin
  if (mlen > before) DBG(mbuf + before);
}

int waitFor(const char* ok, const char* err, uint32_t timeoutMs) {
  mlen = 0;
  mbuf[0] = 0;
  int r = -1;
  uint32_t t = millis();
  while (millis() - t < timeoutMs) {
    while (Modem.available()) mAppend(Modem.read());
    if (ok && strstr(mbuf, ok)) { r = 1; break; }
    if (err && strstr(mbuf, err)) { r = 0; break; }
  }
  if (r >= 0) {  // satr oxiri va "+CME ERROR: <sabab>" dumi ham logga tushsin
    uint32_t t2 = millis();
    while (millis() - t2 < 50) {
      while (Modem.available()) mAppend(Modem.read());
    }
  }
  DBG(mbuf);  // modem javobi logga (javob to'liq kelgach, UART buferi to'lib ketmasligi uchun)
  // Faqat umuman javob kelmasa yoziladi. Kutilgan satr kelmay "+CGATT: 0 OK" kabi javob kelsa,
  // u yuqorida chiqarilgan bo'ladi — bu xato emas, so'rov sikli davom etadi.
  if (r < 0 && mlen == 0) DBGLN("[javob yo'q]");
  return r;
}

bool at(const char* cmd, uint32_t timeoutMs = 1000, const char* ok = "OK") {
  while (Modem.available()) Modem.read();
  DBG("\n>> ");
  DBGLN(cmd);
  Modem.println(cmd);
  return waitFor(ok, "ERROR", timeoutMs) == 1;
}

// SSL / HTTP sozlash buyruqlari ba'zan birinchi urinishda "+CME ERROR" qaytaradi
// (modem to'liq ishga tushib ulgurmagan, SSL konteksti band va h.k.). 5 marta urinamiz,
// urinishlar o'rtasida 1500 ms kutamiz — modem ichki holatini tiklashga ulgursin.
bool atRetry(const char* cmd, uint8_t tries = 5, uint32_t timeoutMs = 3000, const char* ok = "OK") {
  for (uint8_t i = 0; i < tries; i++) {
    if (at(cmd, timeoutMs, ok)) {
      delay(AT_CFG_GAP_MS);  // modem buyruqni ichki holatiga yozib ulgursin
      return true;
    }
    DBG("Qayta urinish ");
    DBG(i + 1);
    DBG("/");
    DBGLN(tries);
    delay(1500);
  }
  DBG("Buyruq bajarilmadi: ");
  DBGLN(cmd);
  return false;
}

void modemPinsIdle() {
  pinMode(PA2, INPUT_ANALOG);
  pinMode(PA3, INPUT_ANALOG);
}

bool modemPowerOn() {
  digitalWrite(PIN_4V_EN, HIGH);
  delay(3000);  // 4V barqarorlashsin (sketch_sep22a da shu qiymat ishlagan)
  Modem.begin(MODEM_BAUD);
  digitalWrite(PIN_PWRKEY, HIGH);
  delay(1000);
  digitalWrite(PIN_PWRKEY, LOW);

  waitFor("APP RDY", NULL, 15000);
  for (int i = 0; i < 10; i++) {
    if (at("AT", 500)) return true;
    delay(500);
  }
  DBGLN("Modem javob bermadi");
  return false;
}

void modemPowerOff() {
  if (at("AT+QPOWD=1", 2000)) waitFor("POWERED DOWN", NULL, 15000);
  Modem.end();
  modemPinsIdle();
  digitalWrite(PIN_4V_EN, LOW);
}

// ---------- vaqt ----------
static uint32_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  int era = y / 400;
  int yoe = y - era * 400;
  int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (uint32_t)(era * 146097 + doe - 719468);
}

static bool parseModemTime(const char* p, uint32_t* utc) {
  if (!p) return false;
  p = strchr(p, '"');
  if (!p) return false;
  int Y, M, D, h, m, s, tz = 0;
  if (sscanf(p + 1, "%d/%d/%d,%d:%d:%d%d", &Y, &M, &D, &h, &m, &s, &tz) < 6) return false;
  if (Y < 100) Y += 2000;
  if (Y < 2024 || M < 1 || M > 12 || D < 1 || D > 31) return false;
  int32_t local = (int32_t)(daysFromCivil(Y, M, D) * 86400UL + h * 3600UL + m * 60UL + s);
  *utc = (uint32_t)(local - tz * 900L);
  return true;
}

void setRtcUtc(uint32_t utc) {
  int32_t delta = (int32_t)(utc - nowEpoch());
  rtc.setEpoch((time_t)utc);
  nextSendEpoch += delta;
  timeSynced = true;
  DBG("RTC sinxronlandi, farq ");
  DBG(delta);
  DBGLN(" s");
}

bool syncTimeFromModem() {
  uint32_t utc;
  if (at("AT+QLTS=2") && parseModemTime(strstr(mbuf, "+QLTS:"), &utc)) {
    setRtcUtc(utc);
    return true;
  }
  if (at("AT+QNTP=1,\"" NTP_SERVER "\",123", 2000) && waitFor("+QNTP:", NULL, 30000) == 1) {
    readMore(300);
    const char* p = strstr(mbuf, "+QNTP:");
    int err = -1;
    if (p && sscanf(p, "+QNTP: %d", &err) == 1 && err == 0 && parseModemTime(p, &utc)) {
      setRtcUtc(utc);
      return true;
    }
  }
  DBGLN("Vaqt olinmadi — RTC dagi vaqt ishlatiladi");
  return false;
}

void formatLocalTime(char* out, size_t n) {
  time_t t = (time_t)nowEpoch() + TZ_OFFSET_S;
  struct tm tmv;
  gmtime_r(&t, &tmv);
  snprintf(out, n, "%04d-%02d-%02d/%02d:%02d:%02d",
           tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

// ---------- signal ----------
void readSignal() {
  csq_raw = 99;
  for (int i = 0; i < 5; i++) {
    if (at("AT+CSQ")) {
      const char* p = strstr(mbuf, "+CSQ:");
      if (p) sscanf(p, "+CSQ: %d", &csq_raw);
    }
    if (csq_raw != 99) break;
    delay(1000);
  }
  signal_level = csqToLevel(csq_raw);
  DBG("CSQ=");
  DBG(csq_raw);
  DBG(" -> daraja ");
  DBGLN(signal_level);
}

// ---------- tarmoq ro'yxatdan o'tish ----------
// CEREG: 0,1 (home) yoki 0,5 (roaming) — ikkala holatda ham tarmoqda
static bool regOk(const char* cmd, const char* tag) {
  if (!at(cmd, 2000)) return false;
  const char* p = strstr(mbuf, tag);
  if (!p) return false;
  int n, stat;
  if (sscanf(p + strlen(tag), " %d,%d", &n, &stat) != 2) return false;
  return (stat == 1 || stat == 5);
}

bool waitNetwork(uint32_t timeoutMs) {
  uint32_t t = millis();
  while (millis() - t < timeoutMs) {
    if (regOk("AT+CEREG?", "+CEREG:")) {
      DBGLN("Tarmoqqa ulandi (LTE)");
      return true;
    }
    // BG95-M3 EGPRS (2G) ga tushsa CEREG 0 bo'lib qolishi mumkin — CGREG ni ham ko'ramiz
    if (regOk("AT+CGREG?", "+CGREG:")) {
      DBGLN("Tarmoqqa ulandi (GPRS)");
      return true;
    }
    delay(2000);
  }
  DBGLN("Tarmoqqa ulanmadi");
  return false;
}

// ---------- operator / APN ----------
// SIM kartaning IMSI'sidan operator aniqlanadi va unga mos APN o'rnatiladi.
// Bir operator uchun bir nechta qator bo'lishi mumkin — PDP ochilmasa keyingisi sinaladi.
static const ApnEntry APN_TABLE[] = {
  { "43404", "Beeline", "internet.beeline.uz", "beeline", "beeline", 1 },
  { "43405", "Ucell", "internet", "", "", 0 },
  { "43406", "Perfectum", "internet", "", "", 0 },
  { "43407", "Mobiuz", "internet", "", "", 0 },
  { "43407", "Mobiuz", "net.ums.uz", "", "", 0 },
  { "43408", "Uzmobile", "internet.uzmobile.uz", "", "", 0 },
  { "43408", "Uzmobile", "internet", "", "", 0 },
};
static const ApnEntry APN_FALLBACK = { "", "Noma'lum", "internet", "", "", 0 };

#define APN_MAX 4
char simMccMnc[6] = "";
char lastMccMnc[6] = "";
uint8_t apnGood = 0;  // oxirgi ishlagan nomzod; SIM almashsa 0 ga qaytadi

// "\r\n434051234567890\r\n\r\nOK" -> "43405"
static bool parseImsi(const char* s, char* out) {
  while (*s) {
    if (*s >= '0' && *s <= '9') {
      size_t n = 0;
      while (s[n] >= '0' && s[n] <= '9') n++;
      if (n >= 14 && n <= 15) {
        memcpy(out, s, 5);
        out[5] = 0;
        return true;
      }
      s += n;
    } else {
      s++;
    }
  }
  return false;
}

// SIM operatorini o'qiydi; SIM almashgan bo'lsa APN tanlovi boshidan boshlanadi
void readSimOperator() {
  simMccMnc[0] = 0;
  for (int i = 0; i < 3; i++) {
    if (at("AT+CIMI", 2000) && parseImsi(mbuf, simMccMnc)) break;
    delay(1000);
  }
  if (strcmp(lastMccMnc, simMccMnc) != 0) {
    strcpy(lastMccMnc, simMccMnc);
    apnGood = 0;
  }
}

uint8_t buildApnList(const ApnEntry** list) {
  uint8_t n = 0;
#ifdef APN_OVERRIDE
  static const ApnEntry ov = { "", "Override", APN_OVERRIDE, "", "", 0 };
  list[n++] = &ov;
#else
  for (size_t i = 0; i < sizeof(APN_TABLE) / sizeof(APN_TABLE[0]) && n < APN_MAX - 1; i++) {
    if (strcmp(APN_TABLE[i].mccmnc, simMccMnc) == 0) list[n++] = &APN_TABLE[i];
  }
  bool hasFallback = false;
  for (uint8_t k = 0; k < n; k++) {
    if (!strcmp(list[k]->apn, APN_FALLBACK.apn) && !list[k]->user[0]) hasFallback = true;
  }
  if (!hasFallback) list[n++] = &APN_FALLBACK;  // noma'lum operator yoki jadvaldagilar ishlamasa
#endif
  return n;
}

// 1 = APN o'zgartirildi (qayta ro'yxatdan o'tish kerak), 0 = allaqachon shu APN, -1 = xato
int apnSet(const ApnEntry* e) {
  char cmd[128];
  snprintf(cmd, sizeof(cmd), "\"%s\",\"%s\",\"%s\",%u", e->apn, e->user, e->pass, e->auth);
  if (at("AT+QICSGP=1", 1000) && strstr(mbuf, cmd)) return 0;
  snprintf(cmd, sizeof(cmd), "AT+QICSGP=1,1,\"%s\",\"%s\",\"%s\",%u", e->apn, e->user, e->pass, e->auth);
  return atRetry(cmd) ? 1 : -1;
}

// ---------- tarmoq ----------
// SIM operatoriga mos APN o'rnatadi va PDP kontekstini faollashtiradi.
// Ishlamasa jadvaldagi keyingi APN (oxirida "internet") sinaladi.
bool pdpActivate() {
  if (!simMccMnc[0]) readSimOperator();
  const ApnEntry* list[APN_MAX];
  uint8_t n = buildApnList(list);
  if (apnGood >= n) apnGood = 0;

  for (uint8_t k = 0; k < n; k++) {
    uint8_t idx = (apnGood + k) % n;
    const ApnEntry* e = list[idx];
    DBG("SIM ");
    DBG(simMccMnc[0] ? simMccMnc : "?");
    DBG(" (");
    DBG(e->op);
    DBG(") APN=");
    DBGLN(e->apn);

    int s = apnSet(e);
    if (s < 0) {
      DBGLN("APN o'rnatilmadi");
      continue;
    }
    if (s == 1) {
      // LTE da APN ro'yxatdan o'tishda ishlatiladi — yangi APN bilan qayta ro'yxatdan o'tamiz
      at("AT+CFUN=0", 15000);
      at("AT+CFUN=1", 15000);
      if (!waitNetwork(NET_REG_TIMEOUT_MS)) continue;
    }
    for (uint8_t i = 0; i < 2; i++) {
      if (at("AT+QIACT=1", 60000)) {
        delay(AT_CFG_GAP_MS);  // kontekst to'liq ko'tarilsin
        at("AT+QIACT?", 2000);
        delay(AT_CFG_GAP_MS);
        apnGood = idx;
        return true;
      }
      DBG("PDP faollashmadi, qayta urinish ");
      DBG(i + 1);
      DBGLN("/2");
      at("AT+QIDEACT=1", 10000);  // osib qolgan kontekstni tozalaymiz
      delay(2000);
    }
  }
  DBGLN("PDP faollashmadi");
  return false;
}

// HTTP/SSL sozlash buyruqlari. Har biri ERROR qaytarsa atRetry() o'sha buyruqni
// 5 martagacha qayta yuboradi. required = true bo'lganlari bajarilmasa yuborish bekor qilinadi.
struct AtCfg {
  const char* cmd;
  bool required;
};

bool httpConfigure() {
  static const AtCfg HTTP_CFG[] = {
    { "AT+QHTTPCFG=\"contextid\",1", true },
    { "AT+QHTTPCFG=\"sslctxid\",1", true },
    // TLS: 4 = Hammasi (modem avtomatik tanlaydi). seclevel=0 -- sertifikat tekshirilmaydi.
    { "AT+QSSLCFG=\"sslversion\",1,4", false },
    { "AT+QSSLCFG=\"seclevel\",1,0", false },
    { "AT+QSSLCFG=\"ignorelocaltime\",1,1", false },
    { "AT+QHTTPCFG=\"contenttype\",1", false },     // application/json
    { "AT+QHTTPCFG=\"requestheader\",0", false },   // modem o'zi header quradi
    { "AT+QHTTPCFG=\"responseheader\",0", false },  // faqat body o'qiladi
  };
  for (uint8_t i = 0; i < sizeof(HTTP_CFG) / sizeof(HTTP_CFG[0]); i++) {
    if (atRetry(HTTP_CFG[i].cmd)) continue;
    if (HTTP_CFG[i].required) {
      DBG("Majburiy sozlama o'rnatilmadi: ");
      DBGLN(HTTP_CFG[i].cmd);
      return false;
    }
    DBG("Ixtiyoriy sozlama o'tkazib yuborildi: ");
    DBGLN(HTTP_CFG[i].cmd);
  }
  return true;
}

// Qayta yuborishdan oldin internet sozlamalarini noldan qaytadan o'rnatadi:
// osib qolgan HTTP sessiyasi yopiladi, PDP konteksti uzib qayta ko'tariladi,
// APN va HTTP/SSL buyruqlari qaytadan yuboriladi.
// QIDEACT majburiy: QICSGP faqat faol bo'lmagan kontekstga yoziladi.
bool netReconfigure() {
  DBGLN("\n--- Internet sozlamalari qaytadan o'rnatilmoqda ---");
  at("AT+QHTTPSTOP", 5000);   // tugallanmagan HTTP sessiyasini yopamiz (ERROR bo'lsa ham zarari yo'q)
  delay(AT_CFG_GAP_MS);
  at("AT+QIDEACT=1", 10000);  // kontekstni uzamiz
  delay(1000);                // kontekst butunlay yopilishi uchun biroz ko'proq kutamiz

  // Modem tarmoqdan tushib qolgan bo'lishi mumkin — ro'yxatdan o'tishini tekshiramiz
  if (!waitNetwork(NET_REG_TIMEOUT_MS)) return false;
  if (!pdpActivate()) return false;

  // Vaqt hali olinmagan bo'lsa yana urinamiz: TIME noto'g'ri bo'lsa server 400 qaytarishi mumkin
  if (!timeSynced) syncTimeFromModem();
  readSignal();

  return httpConfigure();
}

// sketch_sep22a dagi setupModem() mantiqi; APN SIM operatoriga qarab tanlanadi (pdpActivate)
bool modemConnect() {
  at("ATE0");
  delay(AT_CFG_GAP_MS);
  at("AT+CMEE=2");  // xatolar "+CME ERROR: <sabab>" ko'rinishida (tashxis uchun)
  delay(AT_CFG_GAP_MS);

  // Birinchi yoqilishda SIM ba'zan 10 sekundda ham tayyor bo'lmaydi — ~30 s kutamiz
  bool sim = false;
  for (int i = 0; i < 20 && !sim; i++) {
    sim = at("AT+CPIN?", 1500, "READY");
    if (!sim) delay(1500);
  }
  if (!sim) {
    DBGLN("SIM tayyor emas");
    return false;
  }
  readSimOperator();  // APN shu SIM operatoriga qarab tanlanadi

  // Tarmoqqa ro'yxatdan o'tishni kutamiz (BG95 Cat-M1/NB-IoT da 30-60s)
  if (!waitNetwork(NET_REG_TIMEOUT_MS)) return false;
  at("AT+CGATT?");

  if (!pdpActivate()) return false;

  syncTimeFromModem();
  readSignal();

  return httpConfigure();
}

// valveCmdOut: -1 buyruq yo'q, 0 yopish, 1 ochish
// Qaytish: 1 = muvaffaqiyat (HTTP 2xx)
//          0 = server javob berdi, lekin 2xx emas (400/500 ...) — vaqtinchalik bo'lishi mumkin, qayta urinamiz
//         -1 = aloqa/timeout xatosi — qayta urinamiz, kerak bo'lsa PDP/HTTP qayta quriladi
//         -2 = lokal sozlama xatosi (URL noto'g'ri) — qayta urinish behuda
// sketch_sep22a dagi sendJSON() mantiqi: faqat AT komandalari, HTTP header'ni modem o'zi quradi
int httpPost(const char* json, int8_t* valveCmdOut) {
  char cmd[48];
  *valveCmdOut = -1;

  size_t urlLen = strlen(cfg.url);
  if (urlLen == 0 || urlLen > 128) {
    DBGLN("URL noto'g'ri");
    return -2;  // lokal sozlama xatosi — qayta urinish behuda
  }

  snprintf(cmd, sizeof(cmd), "AT+QHTTPURL=%u,80", (unsigned)urlLen);
  if (!at(cmd, 5000, "CONNECT")) {
    DBGLN("URL CONNECT kelmadi");
    return -1;
  }
  Modem.print(cfg.url);
  if (waitFor("OK", "ERROR", 10000) != 1) {
    DBGLN("URL o'rnatilmadi");
    return -1;
  }

  size_t jsonLen = strlen(json);
  snprintf(cmd, sizeof(cmd), "AT+QHTTPPOST=%u,80,80", (unsigned)jsonLen);
  if (!at(cmd, 30000, "CONNECT")) {
    DBGLN("POST CONNECT kelmadi");
    return -1;
  }
  Modem.write((const uint8_t*)json, jsonLen);

  if (waitFor("+QHTTPPOST:", "ERROR", 90000) != 1) {
    DBGLN("POST natijasi kelmadi");
    return -1;
  }
  readMore(300);

  int err = -1, code = 0;
  const char* p = strstr(mbuf, "+QHTTPPOST:");
  if (p) sscanf(p, "+QHTTPPOST: %d,%d", &err, &code);
  DBG("POST err=");
  DBG(err);
  DBG(" HTTP=");
  DBGLN(code);

  // err != 0 modem tomondan xatolik (aloqa) — qayta urinishga arziydi
  if (err != 0) return -1;
  // Server javob qaytargan, lekin 2xx emas (400/500 ...) — postWithRetry() qayta yuboradi
  if (code < 200 || code >= 300) {
#if DEBUG
    // Server nimadan norozi ekanini ko'rish uchun javob matnini logga chiqaramiz
    DBGLN("Server xato javobi:");
    if (at("AT+QHTTPREAD=80", 10000, "+QHTTPREAD:")) readMore(200);
#endif
    return 0;
  }

  // Server javobidagi klapan buyrug'i ("Open" / "Close").
  // +QHTTPREAD: <err> keladi; err=0 bo'lsa, body mbuf ichida qolgan bo'ladi.
  if (at("AT+QHTTPREAD=80", 10000, "+QHTTPREAD:")) {
    readMore(200);
    int rerr = -1;
    const char* r = strstr(mbuf, "+QHTTPREAD:");
    if (r) sscanf(r, "+QHTTPREAD: %d", &rerr);
    if (rerr == 0) {
      bool c = strstr(mbuf, "Close") != NULL;
      bool o = strstr(mbuf, "Open") != NULL;
      if (c != o) *valveCmdOut = o ? 1 : 0;  // ikkalasi ham bo'lsa — noaniq, e'tiborsiz
    }
  }
  return 1;
}

// ================= JSON / YUBORISH =================
void buildJSON(char* out, size_t n) {
  char ts[24];
  formatLocalTime(ts, sizeof(ts));

  const char* valveStr;
  if (valveState == 0) valveStr = "Open";
  else if (valveState == 1) valveStr = "Close";
  else valveStr = cfg.valve ? "Open" : "Close";  // hisoblagichdan o'qilmagan bo'lsa

  int k = snprintf(out, n,
                   "{\"TIME\":\"%s\",\"RTV\":\"%luL\",\"N_RTV\":\"0L\",\"M_NO.\":\"%s\",\"DevId\":\"%s\","
                   "\"UA\":\"UzMeter\",\"Bat_P\":\"%u\",\"Bat_V\":\"%u.%02uV\",\"Bat_S\":\"00\","
                   "\"CSQ\":\"%u\",\"Tem\":\"%d\",\"Alr\":\"0\"",
                   ts, (unsigned long)water_volume_l, cfg.m_no, cfg.device_no,
                   (unsigned)battery_pct, (unsigned)(battery_mv / 1000), (unsigned)((battery_mv % 1000) / 10),
                   (unsigned)signal_level, mcu_temp_c);
  if (k < 0 || (size_t)k >= n) return;
  // Impulsli hisoblagichda klapan yo'q — "Valve" yuborilmaydi
  if (pulseMode()) snprintf(out + k, n - k, "}");
  else snprintf(out + k, n - k, ",\"Valve\":\"%s\"}", valveStr);
}

// Joriy holatni JSON qilib serverga yuboradi. cmd: serverdan kelgan klapan buyrug'i (-1 yo'q)
// Qaytish: 1 = OK, 0 = server rad etdi, -1 = aloqa/timeout xatosi (modemni qayta sozlab urinish kerak)
int postReport(int8_t* cmd) {
  char json[512];
  buildJSON(json, sizeof(json));
  DBGLN(json);
  return httpPost(json, cmd);
}

// Server buyrug'ini bajaradi va hisoblagichdan haqiqiy holatni qayta o'qiydi.
// true = klapan harakatlantirildi; allaqachon shu holatda bo'lsa false
bool executeValveCmd(bool open) {
  if (valveState == (open ? 0 : 1)) return false;
  meterOn();
  if (open) Ochish();
  else Yopish();
  delay(VALVE_SETTLE_MS);
  Olish();  // yangi klapan holati va hajm
  meterOff();
  return true;
}

// Hisobotni yuboradi. HTTP 4xx/5xx (400 ham) yoki timeout bo'lsa HTTP_RETRIES marta
// qayta yuboradi; har urinishda JSON qaytadan quriladi, ya'ni TIME yangilanadi.
// HTTP_RECONFIG_ON_RETRY = 1 bo'lsa, har qayta yuborishdan oldin internet sozlamalari
// (PDP konteksti, APN, HTTP/SSL buyruqlari) noldan qaytadan o'rnatiladi.
bool postWithRetry(int8_t* cmd) {
  uint32_t t0 = millis();

  for (uint8_t tryNo = 1; tryNo <= 1 + HTTP_RETRIES; tryNo++) {
    DBG("\n=== HTTP urinish #");
    DBG(tryNo);
    DBG("/");
    DBG(1 + HTTP_RETRIES);
    DBGLN(" ===");

    int r = postReport(cmd);
    if (r == 1) return true;    // muvaffaqiyat
    if (r == -2) return false;  // URL noto'g'ri — qayta urinish behuda
    // r == 0 (HTTP 4xx/5xx) yoki r == -1 (aloqa/timeout) — ikkalasida ham qayta yuboramiz

    if (tryNo > HTTP_RETRIES) break;  // urinishlar tugadi
    if (HTTP_TOTAL_BUDGET_MS && millis() - t0 > HTTP_TOTAL_BUDGET_MS) {
      DBGLN("HTTP uchun ajratilgan vaqt tugadi");
      break;
    }

    DBG("Xato, ");
    DBG(HTTP_RETRY_DELAY_MS / 1000);
    DBGLN(" s dan keyin qayta yuboriladi");
    delay(HTTP_RETRY_DELAY_MS);

#if HTTP_RECONFIG_ON_RETRY
    // Internet sozlamalarini noldan qaytadan o'rnatamiz — modemning PDP yoki HTTP
    // holati osib qolgan bo'lsa, shunchaki qayta POST qilish yordam bermaydi
    if (!netReconfigure()) break;
#endif
  }
  return false;
}

bool doSend() {
  if (!cfg.url[0]) return false;
  readSensors();  // batareya yuksiz holatda o'lchanadi

  if (pulseMode()) {
    pulseUpdate();  // hajm Hall datchiklaridan (PB14/PB15)
    saveCounter();
  } else {
    meterOn();  // 13V + MAX
    Olish();    // hajm + klapan holati
    meterOff();
  }

  bool ok = false;
  int8_t cmd = -1;

  if (modemPowerOn() && modemConnect()) {
    ok = postWithRetry(&cmd);

    if (ok && cmd >= 0 && !pulseMode()) {  // server klapan buyrug'i (impulslida klapan yo'q)
      if (cfg.valve != (uint8_t)cmd) {
        cfg.valve = cmd;
        saveConfig();
      }
      if (executeValveCmd(cmd == 1)) {
        DBGLN("Klapan holati qayta yuborilmoqda");
        delay(3000);  // server tez-tez kelgan POSTlarni ba'zan 400 bilan rad etadi
        int8_t ignored;  // ikkinchi javobdagi buyruq e'tiborsiz (sikl bo'lmasligi uchun)
        ok = postWithRetry(&ignored);
      }
    }
  }
  modemPowerOff();  // 4V ham o'chadi

  DBGLN(ok ? "Yuborildi" : "Yuborishda xatolik");
  return ok;
}

// ================= UYQU =================
void onHall() {
  hallWake = true;
}

void goToSleep() {
  uint32_t now = nowEpoch();
  if (sendNow || hallWake || now >= nextSendEpoch) return;

  uint32_t ms = (nextSendEpoch - now) * 1000UL;
  DBG("Uyqu: ");
  DBG(ms / 1000);
  DBGLN(" s");
#if DEBUG
  DbgSerial.flush();
  DbgSerial.end();  // SoftwareSerial taymerda ishlaydi - STOP dan oldin to'xtatiladi
#endif
  LowPower.deepSleep(ms);  // RTC yoki PB5 (magnit) uyg'otadi
#if DEBUG
  DbgSerial.begin(DBG_BAUD);
  DbgSerial.listen();
#endif
}

// ================= SETUP / LOOP =================
void setup() {
  pinMode(PIN_LED, OUTPUT);
  // Yoqilish belgisi: 3 marta lipillaydi, keyin darhol IR oynasi ochiladi (LED sekin lipillaydi).
  for (uint8_t i = 0; i < 3; i++) {
    digitalWrite(PIN_LED, LED_ON);
    delay(150);
    digitalWrite(PIN_LED, LED_OFF);
    delay(150);
  }

  // Quvvat kalitlari o'chiq holatda turishi shart (modem/13V/MAX o'z-o'zidan yonib ketmasin) —
  // bu faqat GPIO, hech qanday periferiya ishga tushmaydi
  pinMode(PIN_PWRKEY, OUTPUT);
  digitalWrite(PIN_PWRKEY, LOW);
  pinMode(PIN_4V_EN, OUTPUT);
  digitalWrite(PIN_4V_EN, LOW);
  pinMode(PIN_13V_EN, OUTPUT);
  digitalWrite(PIN_13V_EN, LOW);
  pinMode(PIN_MAX_EN, OUTPUT);
  digitalWrite(PIN_MAX_EN, MAX_OFF);
#ifdef PIN_BAT_EN
  pinMode(PIN_BAT_EN, OUTPUT);
  digitalWrite(PIN_BAT_EN, LOW);
#endif
  pinMode(PIN_BAT, INPUT_ANALOG);
  pinMode(PIN_HALL, INPUT_PULLUP);  // DRV5033 open-drain (oyna magnit bilan ochiq turadi)
  // Impuls datchiklari IR oynasidan oldin ulanadi — oyna ochiq paytdagi aylanishlar ham sanaladi
  pinMode(PIN_PULSE_A, INPUT_PULLUP);
  pinMode(PIN_PULSE_B, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_PULSE_A), onPulseA, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_PULSE_B), onPulseB, FALLING);
  analogReadResolution(12);
#if DEBUG
  DbgSerial.begin(DBG_BAUD);  // DBG() begin'siz chaqirilsa SoftwareSerial osilib qoladi
  DbgSerial.listen();
#endif
  loadConfig();  // faqat flash'dan o'qish — IR javobida joriy sozlama ketishi uchun
  loadCounter();

  // 1) Yoqilganda birinchi bo'lib faqat IR ishga tushadi
  configWindow();

  // 2) IR oynasi yopilgach qolgan hamma narsa
  modemPinsIdle();
  pinMode(PA9, INPUT_ANALOG);
  pinMode(PA10, INPUT_ANALOG);

  rtc.setClockSource(RTC_SRC);
  rtc.begin();
  rtcReady = true;
  LowPower.begin();
  LowPower.attachInterruptWakeup(PIN_HALL, onHall, FALLING, DEEP_SLEEP_MODE);
  // Uyqu paytida ham har bir impuls MCU ni qisqa uyg'otib sanaladi
  LowPower.attachInterruptWakeup(PIN_PULSE_A, onPulseA, FALLING, DEEP_SLEEP_MODE);
  LowPower.attachInterruptWakeup(PIN_PULSE_B, onPulseB, FALLING, DEEP_SLEEP_MODE);
  DBGLN("\nUz-Meter (F103) ishga tushdi");

  sendNow = true;
}

void loop() {
  if (hallWake) configWindow();

  if (sendNow || nowEpoch() >= nextSendEpoch) {
    sendNow = false;
    bool ok = doSend();
    uint32_t now = nowEpoch();
    if (!ok && retries < MAX_RETRIES) {
      retries++;
      nextSendEpoch = now + RETRY_S;
    } else {
      retries = 0;
      nextSendEpoch = now + intervalSec();
    }
  }

  goToSleep();
}
