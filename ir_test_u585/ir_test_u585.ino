/*
 * IR port diagnostikasi — echo (uzmeter_u585 platasi, STM32U585CITx).
 * SoftwareSerial: PA1 = RX, PA0 = TX, 2400 8N1 — uzmeter_u585 dagi bilan bir xil.
 *
 *  - Kompyuterdan kelgan baytlar yig'iladi va 20 ms jimlikdan keyin aynan o'zi IR orqali qaytariladi.
 *    Terminalni 2400 8N1 da oching, matn yuboring — o'sha matn qaytib kelsa RX ham, TX ham ishlaydi.
 *  - Bayt qabul qilinganda PC13 LED 100 ms yonadi.
 *
 * Board: Generic STM32U5 series -> Generic U585CITx
 */
#include <Arduino.h>
#include <SoftwareSerial.h>

#define IR_BAUD 2400
#define ECHO_GAP_MS 20  // shuncha vaqt yangi bayt kelmasa — paket tugadi deb qaytariladi

#define PIN_LED PC13
#define LED_ON HIGH  // LED katod tomoni MCU ga ulangan bo'lsa: LOW
#define LED_OFF (!LED_ON)

Uart Ir(PB11, PB10);  // (RX, TX)

uint8_t buf[64];  // SoftwareSerial bufer hajmi bilan bir xil
size_t len = 0;
uint32_t lastRx = 0;
uint32_t ledOffAt = 0;

void setup() {
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LED_OFF);
  Ir.begin(IR_BAUD);
}

void loop() {
  while (Ir.available()) {
    uint8_t b = Ir.read();
    if (len < sizeof(buf)) buf[len++] = b;
    lastRx = millis();
    digitalWrite(PIN_LED, LED_ON);
    ledOffAt = millis() + 100;
  }

  // Bufer to'lsa yoki jimlik bo'lsa — qabul qilinganini qaytaramiz
  if (len && (len == sizeof(buf) || millis() - lastRx >= ECHO_GAP_MS)) {
    Ir.write(buf, len);
    Ir.flush();
    len = 0;
    // IR qabul qilgich o'z uzatishini ham eshitishi mumkin — aks holda echo cheksiz aylanadi
    delay(30);
    while (Ir.available()) Ir.read();
  }

  if (ledOffAt && (int32_t)(millis() - ledOffAt) >= 0) {
    ledOffAt = 0;
    digitalWrite(PIN_LED, LED_OFF);
  }
}
