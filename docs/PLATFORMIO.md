# ⚙️ PlatformIO / C++ — Samodzielny firmware wM-Bus

> Dokumentacja firmware napisanego w C++ (`src/main.cpp`) dla **PlatformIO**.
> Ten wariant **nie wymaga ESPHome ani Home Assistant** — działa samodzielnie z własnym interfejsem webowym.
> Sprzęt i pinout opisane są w [README.md](../README.md).

---

## 🧩 Co robi ten wariant?

Firmware uruchamia na ESP32 pełny stos do odbioru wM-Bus z webowym panelem zarządzania:

- 📻 Odbiera telegramy wM-Bus przez CC1101 (tryby S1, T1, C1)
- 🔐 Deszyfruje telegramy GAMA350 kluczem AES-128
- 💾 Zapisuje logi ramek na **SPIFFS** w formacie NDJSON
- 🌐 Udostępnia panel zarządzania przez przeglądarkę (port 80)
- 🔄 Umożliwia aktualizację OTA przez przeglądarkę (`/update`)
- 📡 Obsługuje automatyczne skanowanie profili radiowych

---

## 📦 Wymagania

- **PlatformIO** (wtyczka do VS Code lub CLI)
- Biblioteki (zarządzane przez PlatformIO):
  - `ELECHOUSE_CC1101_SRC_DRV` — sterownik CC1101
  - `WiFi`, `WebServer`, `SPIFFS`, `Update` — wbudowane w ESP32 Arduino SDK

---

## ⚙️ Konfiguracja krok po kroku

### Krok 1 — Utwórz plik `include/secrets.h`

Skopiuj przykładowy plik i uzupełnij danymi:

```bash
cp include/secrets.h.example include/secrets.h
```

Zawartość `secrets.h`:

```cpp
#define SECRET_WIFI_SSID     "TwojaNazwaSieci"
#define SECRET_WIFI_PASSWORD "TwojeHaslo"
```

> ⚠️ Plik `secrets.h` **nie powinien być commitowany** do repozytorium — dodaj go do `.gitignore`.

---

### Krok 2 — Skompiluj i wgraj firmware

#### Przez VS Code PlatformIO

Kliknij przycisk **Build** lub **Upload** w pasku PlatformIO.

#### Przez CLI

```bash
pio run --environment esp32dev
pio run --environment esp32dev --target upload
```

#### Podgląd Serial Monitor

```bash
pio device monitor --baud 115200
```

---

## 🌐 Panel webowy

Po uruchomieniu urządzenie jest dostępne pod adresem IP wyświetlonym w Serial Monitor.
Otwórz przeglądarkę i wpisz adres IP, np.: `http://192.168.0.XXX`

### Strona główna `/`

Responsywny panel z przyciskami do sterowania urządzeniem w czasie rzeczywistym. Odświeża się automatycznie co 5 minut.

---

## 📋 Endpointy API

### 📁 Logi

| Endpoint | Metoda | Opis |
|----------|--------|------|
| `/log` | GET | Bieżący plik logu (NDJSON) |
| `/log_prev` | GET | Poprzedni plik logu po rotacji |
| `/log_meter` | GET | Log tylko ramek licznika GAMA350 |
| `/log_clear` | GET/POST | Usuwa bieżący log i log licznika |

### 📻 Radio CC1101

| Endpoint | Metoda | Opis |
|----------|--------|------|
| `/mode_t` | GET | Tryb T1 — 868.950 MHz (Manchester ON) |
| `/mode_s` | GET | Tryb S1 — 868.300 MHz |
| `/radio_wide` | GET | Tryb WIDE — szersza przepustowość RX |
| `/freq_868300` | GET | Ustaw częstotliwość 868.300 MHz |
| `/freq_868950` | GET | Ustaw częstotliwość 868.950 MHz |
| `/freq_869525` | GET | Ustaw częstotliwość 869.525 MHz |
| `/scan_start` | GET | Włącz AUTO-SCAN (hop między profilami) |
| `/scan_stop` | GET | Wyłącz AUTO-SCAN |

### 📊 Diagnostyka

| Endpoint | Metoda | Opis |
|----------|--------|------|
| `/diag` | GET | Status okna diagnostycznego (JSON) |
| `/diag_reset` | GET | Resetuje liczniki diagnostyczne |

### 🔧 System

| Endpoint | Metoda | Opis |
|----------|--------|------|
| `/update` | GET | Formularz OTA — wgraj nowy firmware przez przeglądarkę |
| `/restart` | GET/POST | Restart ESP32 |

---

## 📻 Profile radiowe

Firmware obsługuje cztery profile OMS, między którymi można ręcznie przełączać lub włączyć AUTO-SCAN:

| Profil | Częstotliwość | Tryb | Manchester | Opis |
|--------|--------------|------|-----------|------|
| `S1-868.300` | 868.300 MHz | S1 | NIE | Liczniki jednokierunkowe slow |
| `T1-868.950` | 868.950 MHz | T1 | TAK | Liczniki jednokierunkowe fast (domyślny) |
| `C1-868.950` | 868.950 MHz | C1 | NIE | Liczniki kompaktowe |
| `C1w-868.950` | 868.950 MHz | C1 wide | NIE | C1 z szerszą przepustowością RX |

> AUTO-SCAN przełącza między wszystkimi profilami co kilka sekund — przydatny gdy nie wiesz w jakim trybie nadaje Twój licznik.

---

## 💾 System logowania NDJSON

Każdy odebrany telegram zapisywany jest na SPIFFS jako jedna linia JSON (NDJSON):

```json
{
  "ts": "2026-04-20T12:34:56",
  "epoch": 1745148896,
  "rssi": -72,
  "lqi": 45,
  "len": 34,
  "meter_match": true,
  "has_header_id": true,
  "header_id": "31676464",
  "encrypted": true,
  "channel_mhz": 868.950,
  "energy": 1234567,
  "power": 850,
  "meter_decoded": true,
  "marc_state": "0x0D",
  "data_hex": "2644..."
}
```

### Rotacja logów

Gdy główny log (`/logcc1101.ndjson`) przekroczy **384 KB**, jest automatycznie przenoszony do `/logcc1101_prev.ndjson` i tworzony jest nowy plik. Logi oddzielne dla licznika (`/logcc1101_meter.ndjson`) nie są rotowane — zawierają tylko ramki dopasowane do GAMA350.

---

## 🔐 Deszyfrowanie AES-128 (GAMA350)

Parser `wmbus_gama350.cpp` obsługuje deszyfrowanie telegramów EGM (typ `amiplus`):

- Klucz AES-128 skonfigurowany jest bezpośrednio w `main.cpp` przez stałą `GAMA350_AES_KEY_HEX`
- Parser weryfikuje nagłówek przez numer seryjny licznika w BCD
- Po udanym deszyfrowaniu zwraca strukturę `Gama350Data` z polami `energy` (Wh) i `power` (W)

```cpp
struct Gama350Data {
    uint32_t energy;   // energia w Wh
    uint16_t power;    // moc chwilowa w W
    bool valid;        // czy deszyfrowanie powiodło się
};
```

---

## 📊 Diagnostyka (okno 30 minut)

Endpoint `/diag` zwraca JSON ze statystykami z ostatnich 30 minut:

```json
{
  "active": true,
  "elapsed_sec": 1200,
  "remaining_sec": 600,
  "total_frames": 450,
  "meter_matches": 15,
  "avg_match_interval_sec": 48,
  "radio_mode": "normal",
  "profile": "T1-868.950",
  "channel_mhz": 868.950,
  "rejected_frames": 3,
  "last_match_ago_sec": 42
}
```

Użyj `/diag_reset` aby zresetować liczniki i zacząć nowe okno pomiarowe.

---

## 🔒 Tryb awaryjny Wi-Fi

Jeśli ESP32 nie może połączyć się z siecią Wi-Fi w ciągu 15 sekund, automatycznie uruchamia punkt dostępu AP:

- **SSID:** `ESP32-WMBUS`
- **Hasło:** skonfigurowane w `main.cpp` jako `FALLBACK_AP_PASS`

Po połączeniu z AP otwórz przeglądarkę pod adresem `http://192.168.4.1`.

---

## 🛠️ Rozwiązywanie problemów

### Błąd kompilacji: `SECRET_WIFI_SSID is not defined`

Brak pliku `include/secrets.h`. Utwórz go na podstawie `include/secrets.h.example`.

### Urządzenie nie odbiera żadnych ramek

1. Sprawdź połączenie SPI (CLK/MOSI/MISO/CS/IRQ) — każdy błędny pin blokuje odbiór.
2. Sprawdź profil radiowy — domyślnie T1 na 868.950 MHz. Wypróbuj AUTO-SCAN przez `/scan_start`.
3. Zbliż ESP32 do licznika — metalowe obudowy tłumią sygnał.

### Log SPIFFS nie rośnie

Sprawdź czy SPIFFS został sformatowany: przy pierwszym uruchomieniu `SPIFFS.begin(true)` formatuje partycję. Jeśli widzisz `SPIFFS: BLAD MONTOWANIA` w Serial, wgraj firmware ponownie z czyszczeniem pamięci Flash.

### Deszyfrowanie nie działa (`meter_decoded: false`)

- Sprawdź poprawność klucza AES w stałej `GAMA350_AES_KEY_HEX` — musi to być 32 znaki hex.
- Klucz musi pochodzić od dystrybutora energii (PGE/Tauron itp.).
