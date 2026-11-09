/*
 * Uz-Meter — STM32U585CITx (STM32duino 3.x) proshivkasi
 *
 *  - Sozlash UART : UART4, PA0 = TX, PA1 = RX, 2400 8N1 (PROTOCOL.md)
 *  - Uyg'otish    : DRV5033 (Hall) -> PB5. Magnit yaqinlashsa LOW -> sozlash oynasi ochiladi
 *  - Suv hajmi    : PA6, PA7, PA8 — aylana bo'ylab 3 ta Hall datchik (interrupt), oldinga/orqaga aniqlanadi
 *  - LED          : PC13 — sozlash UART'i yoqiq paytda yonib turadi
 *  - Batareya     : PA4, 1:2 bo'luvchi (Vbat = Vpin * 2), VDDA VREFINT orqali aniqlanadi
 *  - BG95-M3      : USART2, PA2 = TX, PA3 = RX, 115200
 *  - Vaqt va signal BG95 dan olinadi, signal 1..5 darajada yuboriladi
 *  - Kuniga sends_per_day marta JSON POST, qolgan vaqtda STOP rejimida uxlaydi
 *
 * Kutubxonalar (Library Manager): "STM32duino Low Power", "STM32duino RTC"
 * Board: Generic STM32U5 series -> Generic U585CITx
 */

#include <Arduino.h>
#include <EEPROM.h>
#include <STM32LowPower.h>
#include <STM32RTC.h>
#include <time.h>
#include <SoftwareSerial.h>

// ================= SOZLAMALAR =================
#define CFG_BAUD 2400
#define MODEM_BAUD 115200

#define PIN_HALL PB5  // DRV5033 chiqishi (open-drain, magnit bor = LOW)
// Suv hisoblagich: aylana bo'ylab 3 ta Hall datchik (magnit bor = LOW), interrupt bilan.
// Oldinga aylanishda datchiklar A -> B -> C -> A tartibida ishlaydi, orqaga — A -> C -> B.
// Har o'tish 1/3 aylanish: oldinga +1, orqaga -1 (orqaga oqqan suv hajmdan ayiriladi).
// Bitta datchik oldida tebranish hisobga ta'sir qilmaydi, ikki datchik orasidagi tebranish o'zaro yeyishadi.
#define PIN_FLOW_A PA6
#define PIN_FLOW_B PA7
#define PIN_FLOW_C PA8
#define FLOW_DIR 1  // hisob teskari chiqsa (oldinga oqim ayirilsa) -1 qiling
#define PIN_BAT PA4   // batareya bo'luvchisi
#define PIN_LED PC13
#define LED_ON HIGH  // LED katod tomoni MCU ga ulangan bo'lsa: LOW
#define LED_OFF (!LED_ON)
#define PIN_MODEM_PWR PB4  // modem quvvati (HIGH = yoqilgan) — sxemangizga moslang
#define PIN_MODEM_KEY PB3  // PWRKEY (tranzistor orqali HIGH impuls)

#define BAT_DIVIDER 2      // Vbat = Vpin * 2
#define BAT_EMPTY_MV 3300  // 0%   — batareya turiga moslang
#define BAT_FULL_MV 3600   // 100% = 3.6 V
#define BAT_MEASURES 10         // shuncha o'lchovdan eng yuqorisi olinadi
#define BAT_MIN_VALID_MV 3000   // bundan past chiqsa —
#define BAT_SUBST_MV 3500       // shu qiymat yuboriladi

#define TZ_OFFSET_S (5 * 3600L)  // Toshkent, UTC+5 (yozgi vaqt yo'q)
#define NTP_SERVER "pool.ntp.org"

#define CONFIG_WINDOW_MS 60000UL  // oxirgi bayt/magnitdan keyin shuncha vaqt tinglaydi
#define SEND_AFTER_CONFIG 1       // yangi sozlama qabul qilingach darhol yuborish
#define TEST_INTERVAL_S 0         // 0 = 86400/sends_per_day; sinov uchun masalan 60
#define RETRY_S 600UL             // xato bo'lsa 10 daqiqadan keyin qayta urinish
#define MAX_RETRIES 2
#define NET_TIMEOUT_MS 120000UL

// Debug log USART1 (PA9 = TX, PA10 = RX) ga chiqadi. Seriyada 0 qiling.
// (Standart "Serial" bu chipda PA0/PA1 — u sozlash porti sifatida band.)
#define DEBUG 1

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

SoftwareSerial CfgSerial(PA1, PA0);          // UART4 (RX, TX) — standart "Serial" pinlariga bog'liq bo'lmasin
// Uart CfgSerial(PA1, PA0);          // UART4 (RX, TX) — standart "Serial" pinlariga bog'liq bo'lmasin
Uart Modem(PA_3_ALT1, PA_2_ALT1);  // USART2 (RX, TX)

#if DEBUG
Uart DbgSerial(PA10, PA9);  // USART1 (RX, TX)
#define DBG(...) DbgSerial.print(__VA_ARGS__)
#define DBGLN(...) DbgSerial.println(__VA_ARGS__)
#define DBGW(c) DbgSerial.write(c)
#else
#define DBG(...)
#define DBGLN(...)
#define DBGW(c)
#endif

STM32RTC& rtc = STM32RTC::getInstance();

// ================= PROTOKOL =================
#define SOF0 0xAA
#define SOF1 0x55
#define CMD_SET_CONFIG 0x01
#define CMD_GET_DATA 0x02
#define CMD_SET_ACK 0x81
#define CMD_DATA 0x82

struct Config {
  char device_no[16];     // 15 + '\0'
  char m_no[15];          // 14 + '\0'
  uint8_t valve;          // 0 yopiq, 1 ochiq
  uint8_t unit;           // 0..3
  uint8_t sends_per_day;  // 1..24
  uint8_t meter_type;     // 0..5
  char url[129];          // <=128 + '\0'
};

const Config DEFAULT_CFG = {
  "000000000000000", "00000000000000", 1, 0, 6, 0,
  "https://cabinet.uzmeter.uz/dictionary/water"
};
Config cfg;

// Telemetriya
uint32_t water_volume_l = 0;  // 3 ta Hall datchikdan (flowUpdate)
uint16_t battery_mv = 0;
uint8_t battery_pct = 0;
int mcu_temp_c = 0;
int csq_raw = 99;
uint8_t signal_level = 1;  // 1..5
bool timeSynced = false;

// Rejalashtirish
uint32_t nextSendEpoch = 0;
bool sendNow = false;
uint8_t retries = 0;
volatile bool hallWake = false;

// Suv hisobi
volatile int32_t flowSteps = 0;  // hali litrga o'tkazilmagan 1/3 aylanishlar (orqaga — manfiy)
volatile int8_t flowPos = -1;    // oxirgi ishlagan datchik: 0 = A, 1 = B, 2 = C, -1 = noma'lum
int32_t flowStepAcc = 0;         // to'liq aylanishga yetmagan qoldiq (-2..2)
int32_t flowLiters = 0;          // jami hajm (litr), orqaga oqim ayirilgan; flash'da saqlanadi
int32_t flowSavedLiters = 0;     // flash'dagi qiymat (o'zgarmasa qayta yozilmaydi)

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
  return 86400UL / cfg.sends_per_day;
#endif
}

// ================= O'LCHOVLAR =================
// U585 ichki VREFINT ~1.216 V
#define VREFINT_MV 1216

// VREFINT dan haqiqiy ta'minot (VDD) — 16 ta o'qish, har biri orasida 1 ms
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

// Batareya pinidan o'rtacha qiymat — 16 ta o'qish, har biri orasida 1 ms
uint32_t readBatteryRaw() {
  uint32_t raw = 0;
  for (int i = 0; i < 16; i++) {
    raw += analogRead(PIN_BAT);
    delay(1);
  }
  return raw / 16;
}

void readSensors() {
  uint32_t vdda = readVddaMv();

  // 10 marta o'lchanadi, eng yuqorisi olinadi (pastga sakragan noto'g'ri o'lchovlar tashlanadi)
  uint32_t best = 0;
  for (int k = 0; k < BAT_MEASURES; k++) {
    uint32_t v = readVddaMv();         // VDD = 1.216 V * 4095 / VREFINT
    uint32_t raw = readBatteryRaw();   // Vpin = raw / 4095 * VDD
    uint32_t mv = raw * v / 4095 * BAT_DIVIDER;  // Vbat = Vpin * (R1 + R2) / R2
    if (mv > best) best = mv;
  }
  if (best < BAT_MIN_VALID_MV) best = BAT_SUBST_MV;  // 3 V dan past chiqsa — o'lchov xato deb 3.5 V
  battery_mv = (uint16_t)best;

  int32_t pct = ((int32_t)battery_mv - BAT_EMPTY_MV) * 100 / (BAT_FULL_MV - BAT_EMPTY_MV);
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  battery_pct = (uint8_t)pct;

  mcu_temp_c = __LL_ADC_CALC_TEMPERATURE(ADC1, vdda, analogRead(ATEMP), LL_ADC_RESOLUTION_12B);

  DBG("VDDA=");
  DBG(vdda);
  DBG(" mV, Bat=");
  DBG(battery_mv);
  DBG(" mV (");
  DBG(battery_pct);
  DBG("%), T=");
  DBGLN(mcu_temp_c);
}

// ================= SUV HISOBLAGICH (3 ta Hall) =================
// cfg.unit: 0 = 1 L, 1 = 10 L, 2 = 100 L, 3 = 1000 L bir aylanishga
int32_t litersPerRev() {
  static const uint16_t L[] = { 1, 10, 100, 1000 };
  return L[cfg.unit & 3];
}

// p — hozir ishlagan datchik. Oldingisidan keyingi datchik (A->B, B->C, C->A) bo'lsa +1, aks holda -1
static void flowStep(int8_t p) {
  int8_t last = flowPos;
  flowPos = p;
  if (last < 0 || last == p) return;
  flowSteps += (p == (last + 1) % 3) ? FLOW_DIR : -FLOW_DIR;
}

void onFlowA() {
  flowStep(0);
}
void onFlowB() {
  flowStep(1);
}
void onFlowC() {
  flowStep(2);
}

// To'plangan qadamlarni litrga o'tkazadi va water_volume_l ni yangilaydi
void flowUpdate() {
  noInterrupts();
  int32_t s = flowSteps;
  flowSteps = 0;
  interrupts();
  flowStepAcc += s;
  int32_t revs = flowStepAcc / 3;  // to'liq aylanishlar (oldinga yoki orqaga)
  flowStepAcc -= revs * 3;
  flowLiters += revs * litersPerRev();
  water_volume_l = flowLiters > 0 ? (uint32_t)flowLiters : 0;  // boshida orqaga oqsa 0 ko'rsatiladi
  DBG("Hajm=");
  DBG(flowLiters);
  DBGLN(" L");
}

void flowBegin() {
  pinMode(PIN_FLOW_A, INPUT_PULLUP);
  pinMode(PIN_FLOW_B, INPUT_PULLUP);
  pinMode(PIN_FLOW_C, INPUT_PULLUP);
  // Magnit hozir qaysi datchik oldida turgan bo'lsa, birinchi o'tish ham to'g'ri sanalsin
  if (digitalRead(PIN_FLOW_A) == LOW) flowPos = 0;
  else if (digitalRead(PIN_FLOW_B) == LOW) flowPos = 1;
  else if (digitalRead(PIN_FLOW_C) == LOW) flowPos = 2;
  // Uyqu (STOP) paytida ham har bir datchik MCU ni qisqa uyg'otib sanaladi
  LowPower.attachInterruptWakeup(PIN_FLOW_A, onFlowA, FALLING, DEEP_SLEEP_MODE);
  LowPower.attachInterruptWakeup(PIN_FLOW_B, onFlowB, FALLING, DEEP_SLEEP_MODE);
  LowPower.attachInterruptWakeup(PIN_FLOW_C, onFlowC, FALLING, DEEP_SLEEP_MODE);
}

// CSQ (0..31, 99 = noma'lum) -> 1..5.  dBm = -113 + 2*CSQ
uint8_t csqToLevel(int c) {
  if (c == 99 || c < 5) return 1;  // <= -105 dBm yoki signal yo'q
  if (c < 10) return 2;            // -103 .. -95 dBm
  if (c < 15) return 3;            // -93  .. -85 dBm
  if (c < 20) return 4;            // -83  .. -75 dBm
  return 5;                        // >= -73 dBm
}

// ================= FLASH'DA SAQLASH =================
#define CFG_MAGIC 0x557A4D32UL
struct Stored {
  uint32_t magic;
  Config cfg;
  uint16_t crc;
};

void loadConfig() {
  Stored s;
  uint8_t* p = (uint8_t*)&s;
  eeprom_buffer_fill();
  for (size_t i = 0; i < sizeof(s); i++) p[i] = eeprom_buffered_read_byte(i);

  if (s.magic == CFG_MAGIC && s.crc == crc16((uint8_t*)&s.cfg, sizeof(Config))) {
    cfg = s.cfg;
    DBGLN("Sozlama flash'dan o'qildi");
  } else {
    cfg = DEFAULT_CFG;
    DBGLN("Flash bo'sh — standart sozlama");
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
  if (changed) eeprom_buffer_flush();
  DBGLN(changed ? "Sozlama saqlandi" : "Sozlama o'zgarmagan");
}

// Suv hajmi sozlamadan keyin alohida joyda saqlanadi (flash resursi uchun faqat yuborishda yoziladi)
#define CNT_OFFSET 256
#define CNT_MAGIC 0x466C7733UL
struct StoredCnt {
  uint32_t magic;
  int32_t liters;
  uint32_t inv;  // ~liters — tekshiruv
};

void loadCounter() {
  StoredCnt c;
  uint8_t* p = (uint8_t*)&c;
  eeprom_buffer_fill();
  for (size_t i = 0; i < sizeof(c); i++) p[i] = eeprom_buffered_read_byte(CNT_OFFSET + i);
  flowLiters = (c.magic == CNT_MAGIC && c.inv == ~(uint32_t)c.liters) ? c.liters : 0;
  flowSavedLiters = flowLiters;
  water_volume_l = flowLiters > 0 ? (uint32_t)flowLiters : 0;
}

void saveCounter() {
  if (flowLiters == flowSavedLiters) return;
  StoredCnt c = { CNT_MAGIC, flowLiters, ~(uint32_t)flowLiters };
  const uint8_t* p = (const uint8_t*)&c;
  eeprom_buffer_fill();
  for (size_t i = 0; i < sizeof(c); i++) eeprom_buffered_write_byte(CNT_OFFSET + i, p[i]);
  eeprom_buffer_flush();
  flowSavedLiters = flowLiters;
  DBGLN("Suv hajmi saqlandi");
}

// ================= SOZLASH UART =================
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

// JSON va AT buyruqlarini buzmasligi uchun: faqat ko'rinadigan ASCII, bo'shliq, " va \ siz
static bool safeText(const uint8_t* s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    uint8_t c = s[i];
    if (c == 0) break;
    if (c <= 0x20 || c > 0x7E || c == '"' || c == '\\') return false;
  }
  return true;
}

// 0 = OK, 2 = noto'g'ri qiymat
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
  return 0;
}

void handleFrame(uint8_t cmd, const uint8_t* payload, uint8_t len) {
  if (cmd == CMD_SET_CONFIG) {
    uint8_t st = applyConfig(payload, len);
    if (st == 0) {
      saveConfig();
      nextSendEpoch = nowEpoch() + intervalSec();
      retries = 0;
#if SEND_AFTER_CONFIG
      sendNow = true;
#endif
    }
    sendFrame(CMD_SET_ACK, &st, 1);
  } else if (cmd == CMD_GET_DATA) {
    readSensors();
    flowUpdate();
    uint8_t out[34 + 128 + 7];
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

bool pollConfigUart() {
  bool got = false;
  if (rxLen && millis() - rxLast > 200) rxLen = 0;  // chala freym
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
          handleFrame(rx[2], rx + 4, rx[3]);
        } else if (rx[2] == CMD_SET_CONFIG) {
          uint8_t st = 1;
          sendFrame(CMD_SET_ACK, &st, 1);
        }
        rxLen = 0;
      }
    }
    if (rxLen >= sizeof(rx)) rxLen = 0;
  }
  return got;
}

// Sozlash oynasi: UART yoqiladi, LED yonadi
void configWindow() {
  DBGLN("\nSozlash oynasi ochiq");
  CfgSerial.begin(CFG_BAUD);
  while (CfgSerial.available()) CfgSerial.read();
  rxLen = 0;
  digitalWrite(PIN_LED, LED_ON);

  uint32_t last = millis();
  while (millis() - last < CONFIG_WINDOW_MS) {

    if (pollConfigUart()) last = millis();
    if (digitalRead(PIN_HALL) == LOW) last = millis();  // magnit turibdi — oyna ochiq
  }

  CfgSerial.flush();
  CfgSerial.end();
  digitalWrite(PIN_LED, LED_OFF);
  hallWake = false;
  DBGLN("Sozlash oynasi yopildi");
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
  uint32_t t = millis();
  while (millis() - t < ms) {
    while (Modem.available()) {
      char c = Modem.read();
      DBGW(c);
      mAppend(c);
    }
  }
}

// 1 = ok topildi, 0 = err topildi, -1 = timeout
int waitFor(const char* ok, const char* err, uint32_t timeoutMs) {
  mlen = 0;
  mbuf[0] = 0;
  uint32_t t = millis();
  while (millis() - t < timeoutMs) {
    while (Modem.available()) {
      char c = Modem.read();
      DBGW(c);
      mAppend(c);
    }
    if (ok && strstr(mbuf, ok)) return 1;
    if (err && strstr(mbuf, err)) return 0;
  }
  return -1;
}

bool at(const char* cmd, uint32_t timeoutMs = 1000, const char* ok = "OK") {
  while (Modem.available()) Modem.read();
  DBG("\n>> ");
  DBGLN(cmd);
  Modem.println(cmd);
  return waitFor(ok, "ERROR", timeoutMs) == 1;
}

void modemPinsIdle() {
  // Modem o'chiq paytda TX orqali uni quvvatlab qo'ymaslik uchun
  pinMode(PA2, INPUT_ANALOG);
  pinMode(PA3, INPUT_ANALOG);
}

bool modemPowerOn() {
  Modem.begin(MODEM_BAUD);
  digitalWrite(PIN_MODEM_PWR, HIGH);
  delay(3000);
  digitalWrite(PIN_MODEM_KEY, HIGH);
  delay(1000);
  digitalWrite(PIN_MODEM_KEY, LOW);

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
  digitalWrite(PIN_MODEM_PWR, LOW);
  Modem.end();
  modemPinsIdle();
}

// ---------- vaqt ----------
static uint32_t daysFromCivil(int y, int m, int d) {  // 1970-01-01 dan kunlar
  y -= m <= 2;
  int era = y / 400;
  int yoe = y - era * 400;
  int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (uint32_t)(era * 146097 + doe - 719468);
}

// "yyyy/MM/dd,hh:mm:ss+zz" (zz — chorak soatlarda) -> UTC epoch
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
  nextSendEpoch += delta;  // jadval siljimasin
  timeSynced = true;
  DBG("\nRTC sinxronlandi, farq ");
  DBG(delta);
  DBGLN(" s");
}

bool syncTimeFromModem() {
  uint32_t utc;
  // 1) Tarmoq vaqti (NITZ)
  if (at("AT+QLTS=2") && parseModemTime(strstr(mbuf, "+QLTS:"), &utc)) {
    setRtcUtc(utc);
    return true;
  }
  // 2) NTP (PDP faol bo'lishi kerak)
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
  DBG("\nCSQ=");
  DBG(csq_raw);
  DBG(" -> daraja ");
  DBGLN(signal_level);
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

// true = APN o'zgartirildi (qayta ro'yxatdan o'tish kerak)
bool apnSet(const ApnEntry* e) {
  char cmd[128];
  snprintf(cmd, sizeof(cmd), "\"%s\",\"%s\",\"%s\",%u", e->apn, e->user, e->pass, e->auth);
  if (at("AT+QICSGP=1", 1000) && strstr(mbuf, cmd)) return false;  // allaqachon shu APN
  snprintf(cmd, sizeof(cmd), "AT+QICSGP=1,1,\"%s\",\"%s\",\"%s\",%u", e->apn, e->user, e->pass, e->auth);
  at(cmd);
  return true;
}

bool waitAttach(uint32_t timeoutMs) {
  uint32_t t = millis();
  while (millis() - t < timeoutMs) {
    if (at("AT+CGATT?", 2000, "+CGATT: 1")) return true;
    delay(2000);
  }
  return false;
}

// ---------- tarmoq ----------
bool modemConnect() {
  at("ATE0");

  bool sim = false;
  for (int i = 0; i < 10 && !sim; i++) {
    sim = at("AT+CPIN?", 1000, "READY");
    if (!sim) delay(1000);
  }
  if (!sim) {
    DBGLN("SIM tayyor emas");
    return false;
  }
  readSimOperator();  // APN shu SIM operatoriga qarab tanlanadi

  const ApnEntry* list[APN_MAX];
  uint8_t n = buildApnList(list);
  if (apnGood >= n) apnGood = 0;

  bool up = false;
  for (uint8_t k = 0; k < n && !up; k++) {
    uint8_t idx = (apnGood + k) % n;
    const ApnEntry* e = list[idx];
    DBG("SIM ");
    DBG(simMccMnc[0] ? simMccMnc : "?");
    DBG(" (");
    DBG(e->op);
    DBG(") APN=");
    DBGLN(e->apn);

    if (apnSet(e)) {  // yangi APN bilan qayta ro'yxatdan o'tish (LTE uchun shart)
      at("AT+CFUN=0", 15000);
      at("AT+CFUN=1", 15000);
    }
    if (!waitAttach(k == 0 ? NET_TIMEOUT_MS : NET_TIMEOUT_MS / 2)) {
      DBGLN("Tarmoqqa ulanmadi");
      continue;
    }
    if (at("AT+QIACT?", 1000, "+QIACT: 1,1") || at("AT+QIACT=1", 60000)) {
      up = true;
      apnGood = idx;
    } else {
      DBGLN("PDP faollashmadi");
    }
  }
  if (!up) return false;

  syncTimeFromModem();
  readSignal();

  at("AT+QHTTPCFG=\"contextid\",1");
  at("AT+QHTTPCFG=\"sslctxid\",1");
  at("AT+QSSLCFG=\"sslversion\",1,4");
  at("AT+QSSLCFG=\"seclevel\",1,0");
  at("AT+QSSLCFG=\"ignorelocaltime\",1,1");
  at("AT+QHTTPCFG=\"contenttype\",4");
  at("AT+QHTTPCFG=\"requestheader\",0");
  at("AT+QHTTPCFG=\"responseheader\",1");
  return true;
}

bool httpPost(const char* json) {
  char cmd[48];

  snprintf(cmd, sizeof(cmd), "AT+QHTTPURL=%u,80", (unsigned)strlen(cfg.url));
  if (!at(cmd, 5000, "CONNECT")) {
    DBGLN("URL CONNECT kelmadi");
    return false;
  }
  Modem.print(cfg.url);
  if (waitFor("OK", "ERROR", 10000) != 1) {
    DBGLN("URL o'rnatilmadi");
    return false;
  }

  snprintf(cmd, sizeof(cmd), "AT+QHTTPPOST=%u,80,80", (unsigned)strlen(json));
  if (!at(cmd, 30000, "CONNECT")) {
    DBGLN("POST CONNECT kelmadi");
    return false;
  }
  Modem.print(json);

  if (waitFor("+QHTTPPOST:", "ERROR", 90000) != 1) {
    DBGLN("POST natijasi kelmadi");
    return false;
  }
  readMore(300);

  int err = -1, code = 0;
  const char* p = strstr(mbuf, "+QHTTPPOST:");
  if (p) sscanf(p, "+QHTTPPOST: %d,%d", &err, &code);
  DBG("\nPOST err=");
  DBG(err);
  DBG(" HTTP=");
  DBGLN(code);

#if DEBUG
  if (err == 0) at("AT+QHTTPREAD=80", 10000, "+QHTTPREAD:");
#endif
  return err == 0 && code >= 200 && code < 300;
}

// ================= JSON / YUBORISH =================
void buildJSON(char* out, size_t n) {
  char ts[24];
  formatLocalTime(ts, sizeof(ts));
  snprintf(out, n,
           "{\"TIME\":\"%s\",\"RTV\":\"%luL\",\"N_RTV\":\"0L\",\"M_NO.\":\"%s\",\"DevId\":\"%s\","
           "\"UA\":\"UzMeter\",\"Bat_P\":\"%u\",\"Bat_V\":\"%u.%02uV\",\"Bat_S\":\"00\","
           "\"CSQ\":\"%u\",\"Tem\":\"%d\",\"Alr\":\"0\"}",  // klapan yo'q — "Valve" yuborilmaydi
           ts, (unsigned long)water_volume_l, cfg.m_no, cfg.device_no,
           (unsigned)battery_pct, (unsigned)(battery_mv / 1000), (unsigned)((battery_mv % 1000) / 10),
           (unsigned)signal_level, mcu_temp_c);
}

bool doSend() {
  if (!cfg.url[0]) return false;
  readSensors();  // batareyani modem yoqilmasdan (yuksiz) o'lchaymiz
  flowUpdate();
  saveCounter();

  bool ok = false;
  if (modemPowerOn() && modemConnect()) {
    char json[512];
    buildJSON(json, sizeof(json));
    DBGLN("\nJSON:");
    DBGLN(json);
    ok = httpPost(json);
  }
  modemPowerOff();

  DBGLN(ok ? "\nYuborildi" : "\nYuborishda xatolik");
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
#endif
  LowPower.deepSleep(ms);  // RTC yoki PB5 (magnit) uyg'otadi
}

// ================= SETUP / LOOP =================
void setup() {
#if DEBUG
  DbgSerial.begin(115200);
#endif
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LED_OFF);
  pinMode(PIN_MODEM_KEY, OUTPUT);
  digitalWrite(PIN_MODEM_KEY, LOW);
  pinMode(PIN_MODEM_PWR, OUTPUT);
  digitalWrite(PIN_MODEM_PWR, LOW);
  modemPinsIdle();
  pinMode(PIN_BAT, INPUT_ANALOG);
  analogReadResolution(12);

  rtc.setClockSource(STM32RTC::LSI_CLOCK);  // 32.768 kHz kvars bo'lmasa: LSI_CLOCK
  rtc.begin();
  LowPower.begin();

  pinMode(PIN_HALL, INPUT_PULLUP);  // DRV5033 open-drain
  LowPower.attachInterruptWakeup(PIN_HALL, onHall, FALLING, DEEP_SLEEP_MODE);

  loadConfig();
  loadCounter();
  flowBegin();  // sozlash oynasidan oldin — oyna ochiq paytdagi aylanishlar ham sanaladi
  DBGLN("\nUz-Meter (U585) ishga tushdi");

  configWindow();  // yoqilganda sozlash imkoniyati

  sendNow = true;  // birinchi yuborish darhol
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
