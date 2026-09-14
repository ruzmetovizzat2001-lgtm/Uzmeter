# Uz-Meter

IoT suv hisoblagichlarini USB (COM port) orqali sozlash va ma'lumot o'qish dasturi.
Bitta loyiha — **ikki xil texnologiyada** yozilgan ikkita dastur, ikkalasi ham Windows va macOS da ishlaydi va **bir xil protokol**da gaplashadi (`PROTOCOL.md`).

| Papka | Texnologiya | UI |
|---|---|---|
| `rust-app/` | Rust + [Tauri 2](https://tauri.app) | HTML/CSS/JS (WebView) |
| `dotnet-app/` | C# / .NET 9 + [Avalonia UI](https://avaloniaui.net) | XAML (native) |

## Imkoniyatlar

- **Yon panel bo'limlari:** Asosiy (hammasi bitta ekranda), Konfiguratsiya, Qurilma ma'lumotlari, Aloqa jurnali, Sozlamalar, Yordam (Ctrl+1…6, F1)
- **Ulanish paneli:** COM port (avtomatik ro'yxat, har 3 s da yangilanadi), baudrate 1200 … 115200, **Ulanish / Uzish** — port bir marta ochiladi va yopilguncha ochiq turadi (har so'rovda qurilma qayta reset bo'lmaydi); port yo'qolsa ulanish avtomatik uziladi
- **Konfiguratsiya** (raqamlangan bo'limlar): qurilma raqami (15 raqam), M_NO (14 raqam), server URL, klapan (ochiq/yopiq), suv birligi (1/10/100/1000 L), kuniga necha marta yuborish (1…24), hisoblagich turi (6 xil); to'g'ri to'ldirilgan maydonlar yashil belgi bilan
- **Yuboriladigan kadr:** HEX ko'rinishi va **Struktura** (maydonlar jadvali: ofset, uzunlik, baytlar, qiymat), nusxalash, qayta hisoblash
- **Qurilma ma'lumotlari:** suv hajmi, batareya kuchlanishi va foizi (kartochkalar), qurilmadagi sozlamalar, so'nggi aloqa vaqti, holat; "Formaga ko'chirish" tugmasi (avtomatik to'ldirish sozlamalarda o'chirilishi mumkin)
- **Aloqa jurnali:** INFO / TX / RX / WARN / ERROR filtrlari (soni bilan), qidiruv, avtomatik aylantirish, nusxalash, faylga saqlash
- **Sozlamalar** (saqlanadi): til, mavzu, ovoz, avtomatik to'ldirish, javob kutish vaqti, avtomatik ulanish, tiklash
- Menyu: Fayl (sozlamani JSON ga saqlash/ochish, chiqish), Tahrirlash, Ko'rinish (til, mavzu, ovoz), Yordam
- 3 til: O'zbek / Русский / English; yorug' (asosiy) va qorong'i mavzu
- Kirish animatsiyasi (UZMETER logotipi: tomchi → to'lqin → yozuv) — ilova animatsiya to'liq tugagach ochiladi; yumshoq UI ovozlari
- Scrollsiz layout — barcha panellar oynaga sig'adi (min. 1100×680; kichik oynalarda ixcham rejim)

## Ishga tushirish (Windows, ishlab chiqish)

**Rust / Tauri**
```
cd rust-app
npm run dev            # yoki: cd src-tauri && cargo run
cargo test --lib       # protokol testlari (src-tauri ichida)
```

**.NET / Avalonia**
```
cd dotnet-app
dotnet run
dotnet run -- --selftest   # protokol testlari
```

## Release yig'ish

**Windows (shu kompyuterda)**
```
cd rust-app   && npm run build          # → src-tauri/target/release/bundle/{msi,nsis}/
cd dotnet-app && dotnet publish -c Release -r win-x64 --self-contained -p:PublishSingleFile=true -o out/win-x64
```

**macOS** — `.dmg` / `.app` ni Windows'da yig'ib bo'lmaydi. Buning uchun `.github/workflows/build.yml` tayyor:
loyihani GitHub'ga yuklang → **Actions** → artefaktlar ichida Windows va macOS (Intel + Apple Silicon) uchun ikkala dasturning tayyor fayllari bo'ladi.
Imzolanmagan macOS dasturini birinchi marta ochishda: o'ng tugma → *Open* (Gatekeeper).

## Qurilma tomoni

`firmware-example/uz_meter_protocol.ino` — Arduino/ESP32 uchun protokolni to'liq amalga oshiradigan namuna
(kadr qabul qilish, CRC tekshirish, ACK va DATA javoblari). Haqiqiy qurilmada EEPROM/NVS va sensorlarni ulang.

## Brend fayllari

`LOGO_WHITE.avif` — asl logotip. Undan `branding/` ichida shaffof PNG'lar yasalgan:
`logo.png` (to'liq), `logo_dark.png` (yorug' mavzu uchun), `mark.png` (tomchi), `word.png` (yozuv), va `app-icon.png` (ilova ikonkasi).
Ular `rust-app/ui/assets/` va `dotnet-app/Assets/` ga nusxalangan; ikonkalar `npx @tauri-apps/cli icon app-icon.png` bilan yig'ilgan.

## Tuzilma

```
PROTOCOL.md                  umumiy USB protokoli (kadr, CRC-16/MODBUS, maydonlar)
branding/                    logotip PNG variantlari
rust-app/
  ui/                        index.html, style.css, app.js, i18n.js
  ui/assets/                 logotip PNG'lari
  src-tauri/src/             lib.rs (buyruqlar), protocol.rs, serial.rs, menu.rs
dotnet-app/
  Protocol.cs                kadr/CRC/kodlash
  SerialService.cs           System.IO.Ports orqali almashinuv
  I18n.cs                    3 tilli lug'at
  MainWindow.axaml(.cs)      asosiy oyna
  SplashWindow.cs            kirish animatsiyasi
  Sound.cs                   UI ovozlari (WAV sintez, winmm / afplay)
  App.axaml                  mavzu va uslublar
firmware-example/            qurilma uchun namuna
.github/workflows/build.yml  Windows + macOS avtomatik yig'ish
```
