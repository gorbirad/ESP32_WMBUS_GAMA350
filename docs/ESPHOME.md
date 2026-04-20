# 🏠 ESPHome — Odczyt liczników wM-Bus

> Dokumentacja podejścia opartego o **ESPHome** z integracją **Home Assistant**.
> Sprzęt i pinout opisane są w [README.md](../README.md).

---

## 🧩 Co robi ten wariant?

Układ ESP32 z CC1101 nasłuchuje na **868.95 MHz** protokołu **wM-Bus T1** i automatycznie przekazuje odczyty do Home Assistant przez szyfrowane API ESPHome. Konfiguracja odbywa się wyłącznie przez plik YAML — bez pisania kodu C++.

---

## 📦 Wymagania

- **ESPHome** w wersji z obsługą `esp-idf` (testowane z `2025.11.0`)
- **Home Assistant** z ESPHome Add-on (lub standalone CLI)
- Python + `esphome` CLI

### Instalacja ESPHome CLI

```bash
pip install esphome
```

---

## ⚙️ Konfiguracja krok po kroku

### Krok 1 — Utwórz plik `secrets.yaml`

W tym samym katalogu co plik YAML utwórz `secrets.yaml` z następującymi wpisami:

```yaml
wifi_dol: "TwojaNazwaSieci"
wifi_pass_dol: "TwojeHaslo"
key_gama350: "TwojeHeksadecymalneHasloAES128"
api_encryption_key: "TwojKluczAPI"
ota_password: "TwojeHasloOTA"
fallback_ap_password: "TwojeHasloAP"
```

> 🔑 `key_gama350` — 16-bajtowy klucz AES-128 (32 znaki hex) do odszyfrowania telegramów licznika GAMA350. Klucz uzyskuje się od dystrybutora energii (PGE/Tauron itp.).
>
> 💧 Licznik Diehl IZAR **nie wymaga klucza** — dane są jawne.

---

### Krok 2 — Przegląd konfiguracji YAML

Główny plik: **`kod_esp_home_c1101_wgrany.yaml`**

#### Framework

```yaml
esp32:
  board: esp32dev
  framework:
    type: esp-idf
```

> ⚠️ Wymagany `esp-idf` — komponenty wM-Bus **nie są kompatybilne** z Arduino framework.

#### Zewnętrzne komponenty wM-Bus

```yaml
external_components:
  - source: github://SzczepanLeon/esphome-components@main
    components: [ wmbus_radio, wmbus_meter, wmbus_common ]
```

#### Konfiguracja radia CC1101

```yaml
spi:
  clk_pin: GPIO18
  mosi_pin: GPIO23
  miso_pin: GPIO19

wmbus_radio:
  radio_type: CC1101
  cs_pin: GPIO5
  irq_pin: GPIO4
  frequency: 868.95MHz
```

#### Licznik energii elektrycznej — GAMA350

```yaml
wmbus_meter:
  - id: gama350
    meter_id: 0x31676464
    type: amiplus
    key: !secret key_gama350
    mode:
      - T1
```

> `meter_id` to numer seryjny licznika w formacie hex — znajdziesz go na naklejce urządzenia.

#### Licznik wody — Diehl IZAR RC 868

```yaml
  - id: izar_water
    meter_id: 0x41004355
    type: izar
    mode:
      - T1
```

---

### Krok 3 — Wgraj firmware

#### Pierwsze wgranie (kabel USB)

```bash
esphome run kod_esp_home_c1101_wgrany.yaml
```

#### Aktualizacja OTA (przez sieć Wi-Fi)

```bash
esphome run kod_esp_home_c1101_wgrany.yaml --device 192.168.0.186
```

> Użyj bezpośredniego adresu IP zamiast nazwy `.local` — eliminuje problemy z mDNS.

---

## 📊 Sensory dostępne w Home Assistant

Po wgraniu firmware encje pojawią się automatycznie w HA:

### ⚡ Energia elektryczna (GAMA350)

| Encja | Jednostka | Opis |
|-------|-----------|------|
| `Całkowita energia L1` | kWh | Łączne zużycie energii |
| `Energia RSSI` | dBm | Siła sygnału radiowego licznika |

### 💧 Woda (Diehl IZAR)

| Encja | Jednostka | Opis |
|-------|-----------|------|
| `Woda Łączne Zużycie` | m³ | Łączne zużycie wody |
| `Woda RSSI` | dBm | Siła sygnału radiowego licznika |

### 📶 Diagnostyka urządzenia

| Encja | Jednostka | Opis |
|-------|-----------|------|
| `Radio CC11 WiFi Sygnał` | dBm | Siła sygnału Wi-Fi |
| `WiFi Signal Percent` | % | Sygnał Wi-Fi w procentach |
| `Radio CC11 Czas Pracy` | s | Czas pracy od ostatniego restartu |
| `Radio CC11 IP Adres` | — | Aktualny adres IP |
| `Radio CC11 Status` | — | Status połączenia z HA |

---

## 🔒 Bezpieczeństwo i tryb awaryjny

| Funkcja | Opis |
|---------|------|
| **Safe Mode** | Po 3 nieudanych startach urządzenie uruchamia się w trybie awaryjnym (OTA nadal działa) |
| **Captive Portal** | Brak Wi-Fi → ESP32 tworzy własną sieć AP `C11 Fallback Hotspot` |
| **Szyfrowanie API** | Komunikacja z HA szyfrowana kluczem Noise Protocol z `secrets.yaml` |
| **OTA** | Aktualizacje przez sieć, zabezpieczone hasłem z `secrets.yaml` |

---

## 🔍 Podgląd logów

### Logi na żywo w terminalu

```bash
esphome logs kod_esp_home_c1101_wgrany.yaml --device 192.168.0.186
```

### Zapis logów do pliku

```bash
esphome logs kod_esp_home_c1101_wgrany.yaml --device 192.168.0.186 | tee log_test.txt
```

### Zmiana poziomu logowania

W konfiguracji YAML zmień `level` z `WARN` na `DEBUG` aby zobaczyć wszystkie odbierane telegramy:

```yaml
logger:
  level: DEBUG
```

> Przywróć `WARN` po diagnozie — poziom DEBUG generuje dużo szumu.

### Analiza logów (checklista)

1. **Błędy:** wyszukaj `error`, `failed`, `timeout`, `not handled`.
2. **Połączenie Wi‑Fi:** potwierdź, że urządzenie ma IP i utrzymuje połączenie.
3. **Odbieranie danych:** sprawdź, czy pojawiają się telegramy i aktualizują się sensory.

Do zgłoszenia dołącz fragment logu (start + 1–2 min pracy), ale bez ujawniania danych z `secrets.yaml`.

---

## 🛠️ Rozwiązywanie problemów

### Licznik nie pojawia się w logach

1. Sprawdź, czy ESP32 jest w zasięgu licznika — unikaj metalowych obudów między anteną a licznikiem.
2. Upewnij się, że licznik nadaje w trybie **T1** (nie C1 ani S1).
3. Częstotliwość wM-Bus T1 to **868.95 MHz** — sprawdź czy nie zmieniłeś konfiguracji.
4. Sprawdź poprawność `meter_id` z naklejką na liczniku.

### Komunikat `Telegram not handled by any handler`

Telegram jest odbierany, ale żaden skonfigurowany licznik nie pasuje do nadawcy. Przejdź do logów DEBUG, znajdź komunikat `Check if telegram with address XXXXXXXX` i porównaj z naklejką na liczniku.

### Jak znaleźć ID nieznanego licznika

1. Ustaw `logger: level: DEBUG`.
2. Zbliż się do licznika z działającym ESP32.
3. Szukaj w logach linii: `Check if telegram with address`.
4. Wklej surowy telegram do: [wmbusmeters.org/analyze](https://wmbusmeters.org/analyze)

### Połączenie API rwie się (`is unresponsive; disconnecting`)

Wysoki ping lub jitter na sieci. Rozwiązania:
- Używaj adresu IP zamiast nazwy mDNS: `--device 192.168.0.186`
- Wyłącz VPN po stronie komputera
- Sprawdź siłę sygnału Wi-Fi ESP32 (sensor diagnostyczny w HA)
