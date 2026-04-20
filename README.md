# 📡 ESP32 + CC1101 — Odczyt liczników wM-Bus

> **Projekt:** Bezprzewodowy odczyt licznika energii elektrycznej GAMA350 oraz licznika wody Diehl IZAR RC 868 przy użyciu ESP32 z modułem radiowym CC1101.

Projekt oferuje **dwa niezależne podejścia** do tego samego sprzętu — wybierz jedno:

| Podejście | Plik konfiguracyjny | Dokumentacja |
|-----------|---------------------|--------------|
| 🏠 **ESPHome** (integracja z Home Assistant) | `kod_esp_home_c1101_wgrany.yaml` | [docs/ESPHOME.md](docs/ESPHOME.md) |
| ⚙️ **PlatformIO / C++** (samodzielny firmware z web UI) | `src/main.cpp` | [docs/PLATFORMIO.md](docs/PLATFORMIO.md) |

---

## 🔧 Sprzęt — wspólny dla obu podejść

### Wymagane komponenty

| Komponent | Opis |
|-----------|------|
| **ESP32** | Płytka deweloperska `esp32dev` |
| **CC1101** | Moduł radiowy 868 MHz (transceiver) |
| Przewody | Połączenia SPI + zasilanie |
| Zasilacz | USB 5V lub ładowarka |

### Podłączenie CC1101 → ESP32 (SPI)

| CC1101 | GPIO ESP32 | Opis |
|--------|-----------|------|
| SCK / CLK | GPIO18 | Zegar SPI |
| MOSI | GPIO23 | Dane do CC1101 |
| MISO | GPIO19 | Dane z CC1101 |
| CSN / CS | GPIO5 | Chip Select ⚠️ |
| GDO0 | GPIO4 | Przerwanie IRQ |
| VCC | 3.3V | Zasilanie |
| GND | GND | Masa |

> ⚠️ **GPIO5** to strapping pin ESP32. Nie należy podciągać go do GND podczas startu urządzenia — może spowodować problemy z bootowaniem.

---

## 📊 Odbierane liczniki

| # | Licznik | Typ | Tryb | ID |
|---|---------|-----|------|----|
| 1 | GAMA350 (energia elektryczna) | `amiplus` | T1 | `0x31676464` |
| 2 | Diehl IZAR RC 868 (woda) | `izar` | T1 | `0x41004355` |

---

## 📁 Struktura projektu

```
ESP32_WMBUS_GAMA350/
├── kod_esp_home_c1101_wgrany.yaml   # Konfiguracja ESPHome
├── src/
│   ├── main.cpp                      # Firmware PlatformIO (C++)
│   ├── wmbus_gama350.cpp             # Parser wM-Bus / AES dla GAMA350
│   └── wmbus_gama350.h
├── include/
│   └── secrets.h                     # Dane dostępowe (nie commitować!)
├── lib/
│   ├── aes/                          # Biblioteka AES-128
│   └── ELECHOUSE_CC1101_SRC_DRV/    # Sterownik CC1101
├── platformio.ini                    # Konfiguracja PlatformIO
├── docs/
│   ├── ESPHOME.md                    # Dokumentacja ESPHome
│   └── PLATFORMIO.md                 # Dokumentacja firmware C++
└── README.md                         # Ten plik
```

---

## 🧪 Analiza logów i dokumentacja

- **ESPHome:** konfiguracja i analiza logów w `docs/ESPHOME.md`
- **Kod `main.cpp` (PlatformIO):** opis działania i diagnostyka w `docs/PLATFORMIO.md`

Przy analizie zgłoszeń sprawdź: błędy, status połączenia Wi‑Fi, oraz czy ramki z liczników są odbierane i dekodowane.

> 🔒 Nie publikuj prawdziwych danych logowania ani kluczy (`secrets.yaml`, `include/secrets.h`, API/OTA/AES).

Jeśli musisz cofnąć ostatni lokalny commit:

```bash
git reset HEAD~1
```

---

## 🔗 Przydatne linki

| Link | Opis |
|------|------|
| [ESPHome](https://esphome.io) | Dokumentacja ESPHome |
| [SzczepanLeon/esphome-components](https://github.com/SzczepanLeon/esphome-components) | Komponenty wM-Bus dla ESPHome |
| [wmbusmeters.org/analyze](https://wmbusmeters.org/analyze) | Analizator telegramów wM-Bus online |
| [wmbusmeters](https://github.com/weetmuts/wmbusmeters) | Lista driverów liczników |
