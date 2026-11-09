# Uz-Meter USB protokoli (v1)

Ikkala dastur (Rust/Tauri va .NET/Avalonia) bir xil protokolda ishlaydi.
Aloqa: USB-Serial (COM port), 8N1, baudrate 1200..115200.

## Kadr (frame) tuzilishi

| Ofset | Hajm | Tavsif |
|-------|------|--------|
| 0     | 1    | `0xAA` — start bayt 1 |
| 1     | 1    | `0x55` — start bayt 2 |
| 2     | 1    | `CMD` — buyruq |
| 3     | 1    | `LEN` — payload uzunligi (0..255) |
| 4     | LEN  | `PAYLOAD` |
| 4+LEN | 2    | `CRC16` (CRC-16/MODBUS, poly 0x8005 reflected = 0xA001, init 0xFFFF), little-endian: lo, hi |

CRC `CMD`, `LEN` va `PAYLOAD` baytlari ustidan hisoblanadi (start baytlar kirmaydi).

## Buyruqlar

| CMD    | Yo'nalish     | Payload                | Tavsif |
|--------|---------------|------------------------|--------|
| `0x01` | PC → qurilma  | `CONFIG`               | Sozlamalarni yozish |
| `0x02` | PC → qurilma  | (bo'sh)                | Ma'lumot so'rash |
| `0x81` | qurilma → PC  | `[status]`             | Yozish javobi: 0=OK, 1=CRC xato, 2=noto'g'ri qiymat |
| `0x82` | qurilma → PC  | `CONFIG` + `TELEMETRY` | Ma'lumot javobi |

## CONFIG (34 + N bayt)

| Ofset | Hajm | Maydon | Qiymat |
|-------|------|--------|--------|
| 0     | 15   | `device_no`     | ASCII, 15 ta raqam |
| 15    | 14   | `m_no`          | ASCII, 14 ta raqam |
| 29    | 1    | `valve`         | 0 = yopiq, 1 = ochiq |
| 30    | 1    | `unit`          | 0 = 1 L, 1 = 10 L, 2 = 100 L, 3 = 1000 L |
| 31    | 1    | `sends_per_day` | 1..24 (kuniga necha marta yuboradi) |
| 32    | 1    | `meter_type`    | 0..5 (6 xil hisoblagich turi) |
| 33    | 1    | `url_len`       | N, 0..128 |
| 34    | N    | `server_url`    | ASCII |
| 34+N  | 4    | `initial_value` | *Ixtiyoriy*, u32 LE, litr. Faqat `0x01` kadrida va faqat `meter_type = 4` (impulsli) bo'lganda yuboriladi; qurilma hisobni shu qiymatdan boshlaydi. Boshqa turlarda e'tiborga olinmaydi |

## TELEMETRY (7 bayt) — faqat `0x82` javobida, CONFIG dan keyin

| Ofset | Hajm | Maydon | Qiymat |
|-------|------|--------|--------|
| 0     | 4    | `water_volume`  | u32 LE, litr |
| 4     | 2    | `battery_mv`    | u16 LE, millivolt |
| 6     | 1    | `battery_pct`   | 0..100 % |

## Misol: sozlama yozish

```
AA 55 01 2E
31 32 33 34 35 36 37 38 39 30 31 32 33 34 35    "123456789012345"
39 38 37 36 35 34 33 32 31 30 39 38 37 36       "98765432109876"
01  00  06  02                                   ochiq, 1L, 6 marta/kun, tur 3
0C                                               url_len = 12
68 74 74 70 3A 2F 2F 61 2E 75 7A 2F              "http://a.uz/"
xx xx                                            CRC16 lo, hi
```

Qurilma javobi: `AA 55 81 01 00 CRC_lo CRC_hi` (OK).
