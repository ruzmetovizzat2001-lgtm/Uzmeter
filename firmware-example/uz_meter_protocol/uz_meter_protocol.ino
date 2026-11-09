/*
 * Uz-Meter — qurilma tomonidagi protokol namunasi (Arduino / ESP32).
 * PROTOCOL.md ga to'liq mos. Serial: 9600 8N1 (dasturda tanlangan baudrate bilan bir xil bo'lsin).
 *
 * Bu faqat namuna: sozlamalar RAM da saqlanadi, telemetriya soxta qiymatlar.
 * Haqiqiy qurilmada EEPROM/NVS ga yozish va sensor/ADC dan o'qishni qo'shing.
 */

#define SOF0 0xAA
#define SOF1 0x55
#define CMD_SET_CONFIG 0x01
#define CMD_GET_DATA   0x02
#define CMD_SET_ACK    0x81
#define CMD_DATA       0x82

struct Config {
  char     device_no[16];   // 15 + '\0'
  char     m_no[15];        // 14 + '\0'
  uint8_t  valve;           // 0 yopiq, 1 ochiq
  uint8_t  unit;            // 0..3
  uint8_t  sends_per_day;   // 1..24
  uint8_t  meter_type;      // 0..5
  char     url[129];        // <=128 + '\0'
} cfg = { "000000000000000", "00000000000000", 1, 0, 6, 0, "http://example.uz/" };

uint32_t water_volume_l = 12345;   // litr
uint16_t battery_mv     = 3720;    // mV
uint8_t  battery_pct    = 78;

uint16_t crc16(const uint8_t* d, size_t n) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    crc ^= d[i];
    for (uint8_t b = 0; b < 8; b++) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
  }
  return crc;
}

void sendFrame(uint8_t cmd, const uint8_t* payload, uint8_t len) {
  uint8_t buf[4 + 255 + 2];
  buf[0] = SOF0; buf[1] = SOF1; buf[2] = cmd; buf[3] = len;
  memcpy(buf + 4, payload, len);
  uint16_t c = crc16(buf + 2, len + 2);
  buf[4 + len] = c & 0xFF; buf[5 + len] = c >> 8;
  Serial.write(buf, 6 + len);
}

size_t encodeConfig(uint8_t* p) {
  size_t n = 0;
  memcpy(p + n, cfg.device_no, 15); n += 15;
  memcpy(p + n, cfg.m_no, 14);      n += 14;
  p[n++] = cfg.valve; p[n++] = cfg.unit; p[n++] = cfg.sends_per_day; p[n++] = cfg.meter_type;
  uint8_t ul = strlen(cfg.url);
  p[n++] = ul; memcpy(p + n, cfg.url, ul); n += ul;
  return n;
}

// 0 = OK, 2 = noto'g'ri qiymat
uint8_t applyConfig(const uint8_t* p, uint8_t len) {
  if (len < 34) return 2;
  uint8_t ul = p[33];
  if (ul > 128 || len < 34 + ul) return 2;
  if (p[30] > 3 || p[31] < 1 || p[31] > 24 || p[32] > 5) return 2;
  memcpy(cfg.device_no, p, 15); cfg.device_no[15] = 0;
  memcpy(cfg.m_no, p + 15, 14); cfg.m_no[14] = 0;
  cfg.valve = p[29]; cfg.unit = p[30]; cfg.sends_per_day = p[31]; cfg.meter_type = p[32];
  memcpy(cfg.url, p + 34, ul); cfg.url[ul] = 0;
  // TODO: EEPROM/NVS ga saqlash, klapanni boshqarish
  return 0;
}

void handleFrame(uint8_t cmd, const uint8_t* payload, uint8_t len) {
  if (cmd == CMD_SET_CONFIG) {
    uint8_t st = applyConfig(payload, len);
    sendFrame(CMD_SET_ACK, &st, 1);
  } else if (cmd == CMD_GET_DATA) {
    uint8_t out[34 + 128 + 7];
    size_t n = encodeConfig(out);
    out[n++] = water_volume_l & 0xFF; out[n++] = (water_volume_l >> 8) & 0xFF;
    out[n++] = (water_volume_l >> 16) & 0xFF; out[n++] = (water_volume_l >> 24) & 0xFF;
    out[n++] = battery_mv & 0xFF; out[n++] = battery_mv >> 8;
    out[n++] = battery_pct;
    sendFrame(CMD_DATA, out, n);
  }
}

// ---- qabul qilish avtomati ----
uint8_t rx[4 + 255 + 2];
size_t rxLen = 0;

void setup() { Serial.begin(9600); }

void loop() {
  while (Serial.available()) {
    uint8_t b = Serial.read();
    if (rxLen == 0 && b != SOF0) continue;
    if (rxLen == 1 && b != SOF1) { rxLen = 0; continue; }
    rx[rxLen++] = b;
    if (rxLen >= 4) {
      size_t total = 4 + rx[3] + 2;
      if (rxLen == total) {
        uint16_t c = rx[total - 2] | (rx[total - 1] << 8);
        if (crc16(rx + 2, rx[3] + 2) == c) {
          handleFrame(rx[2], rx + 4, rx[3]);
        } else if (rx[2] == CMD_SET_CONFIG) {
          uint8_t st = 1; sendFrame(CMD_SET_ACK, &st, 1);   // CRC xato
        }
        rxLen = 0;
      }
    }
    if (rxLen >= sizeof(rx)) rxLen = 0;
  }
}
