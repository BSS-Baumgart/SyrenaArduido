# ESP32-S3 WAV Manager (3 Slots + Buttons + Relay + Web UI)

Ten projekt robi z ESP32-S3 prosty "player" plików WAV z LittleFS + panel WWW:

- **3 sloty** (przypisanie pliku do slotu) + **3 fizyczne przyciski** (toggle) + **3 LEDy** (aktywny slot)
- **Upload / list / play / delete** plików WAV przez www
- **Głośność** z suwaka (0–21)
- **Przekaźniki 2-kanałowe** przełączają głośnik: **syrena ↔ ESP**
- **Wi-Fi STA** (Twoja sieć) + **fallback AP** gdy STA nie wstanie

---

## 1) Wymagania

### Sprzęt

- ESP32-S3
- Moduł przekaźników 2-kanałowy (IN1/IN2, aktywne LOW)
- Wzmacniacz I2S (np. MAX98357A) lub inny tor audio zgodny z `Audio.h`
- 3 przyciski (zwierają do GND)
- 3 LED + rezystory

### Biblioteki (Arduino IDE)

- ESPAsyncWebServer
- AsyncTCP
- Audio (ta sama, której używasz w projekcie: `Audio.h` z `connecttoFS()` / `setPinout()` / `setVolume()`)

> **Uwaga:** na ESP32-S3 AsyncTCP / ESPAsyncWebServer muszą być w wersjach zgodnych z core ESP32.

---

## 2) Piny (nie zmieniaj, jeśli masz już polutowane tak jak działa)

### I2S (ESP32-S3)

```c
#define I2S_BCK  6
#define I2S_LRC  7
#define I2S_DOUT 5
```

### Przekaźniki (2-kanałowe)

```c
#define RELAY1_PIN 18  // IN1
#define RELAY2_PIN 8   // IN2
```

**Logika:**

| Stan            | Znaczenie                                  |
| --------------- | ------------------------------------------ |
| `HIGH` / `HIGH` | startowo **SYRENA** (ESP nie gra)          |
| `LOW` / `LOW`   | przełączenie na **ESP** (gra z pliku WAV)  |

### Przyciski / LED (3 sloty)

```c
static const uint8_t BTN_PINS[3] = { 10, 11, 12 };  // przycisk -> GND
static const uint8_t LED_PINS[3] = { 15, 16, 17 };  // LED przez rezystor -> GND
```

- Przyciski są na `INPUT_PULLUP` → wciśnięty = `LOW`
- LED świeci gdy slot jest aktywny (odtwarzany)
- Jeśli chcesz zamienić LED1 z LED3 — zamieniasz kolejność w tablicy `LED_PINS` (już masz ustawione `{15, 16, 17}`)

---

## 3) Wi-Fi

### Tryb STA (domyślnie)

```c
const char *WIFI_SSID = "Bongenet";
const char *WIFI_PASS = "BongEe1008!";
```

### Fallback AP (gdy STA nie połączy się w ~10 s)

```c
const char *AP_SSID = "ESP-SIREN";
const char *AP_PASS = "12345678";
```

W trybie AP wchodzisz na adres z logu: `WiFi.softAPIP()` (zwykle `192.168.4.1`).

---

## 4) System plików: LittleFS

Projekt używa **LittleFS**:

- Pliki WAV są zapisywane do pamięci flash (LittleFS).
- Sloty są zapamiętywane w pliku konfiguracyjnym `/slots.cfg` — 3 linie (po jednej na slot).

Przykładowa zawartość `/slots.cfg`:

```
/1.wav
/2.wav
/3.wav
```

Jeśli pliku nie ma → używane są domyślne: `{ "/1.wav", "/2.wav", "/3.wav" }`.

---

## 5) Web UI

### Strona główna

**`GET /`** — pokazuje:

- Status IP + tryb (STA/AP) + użycie LittleFS
- Suwak głośności
- Przypisania slotów + przyciski **SAVE** / **PLAY-TOGGLE**
- Upload WAV
- Listę plików z **PLAY** / **DEL**

---

## 6) API HTTP (endpointy)

### Status / info

**`GET /api/status`** — zwraca JSON:

```json
{ "ip": "192.168.x.x", "mode": "STA|AP", "total": ..., "used": ..., "free": ... }
```

**`GET /api/list`** — zwraca listę plików:

```json
{ "files": [ { "name": "/1.wav", "size": 12345 }, ... ] }
```

### Sloty

**`GET /api/slots`**

```json
{ "slots": ["/1.wav", "/2.wav", "/3.wav"] }
```

**`GET /api/slot/set?idx=0&name=/plik.wav`**
Ustawia plik dla slotu `idx` (0–2) i zapisuje do `/slots.cfg`.

**`GET /api/slot/toggle?idx=0`**
Toggle: jeśli slot gra → STOP. Jeśli nie gra → start slotu.

### Odtwarzanie i stop

**`GET /api/stop`**
Zatrzymuje audio, gasi LEDy i przełącza przekaźnik z powrotem na SYRENĘ.

**`GET /api/play?name=/plik.wav`**
Odtwarza "ad-hoc" (nie ustawia slotu, LEDy zgaszone). Najpierw robi `stopAll()`, przełącza na ESP i gra plik.

### Usuwanie i upload

**`GET /api/delete?name=/plik.wav`**
Stopuje odtwarzanie i usuwa plik (z retry, jeśli plik jest jeszcze otwarty).

**`POST /upload`**
Upload pliku WAV do `/<filename>`.

### Głośność

- **`GET /api/volume/set?v=0..21`**
- **`GET /api/volume/get`**

---

## 7) Zasady odtwarzania / logika

### Slot playback

**Start slotu:**

1. `stopAll()` → wraca na SYRENĘ, zatrzymuje audio, resetuje LED
2. Sprawdza czy plik istnieje w LittleFS
3. Przełącza przekaźnik na ESP
4. `audio.connecttoFS(LittleFS, path)`
5. Zapala LED dla aktywnego slotu

### Auto-loop

Jeśli slot grał (`currentSlot >= 0`) i `audio.isRunning() == false`:
→ uruchomi ponownie ten sam plik (restart) po guardzie `RESTART_GUARD_MS`.

### Przyciski

- Debounce **40 ms**
- Zdarzenie na zboczu `HIGH` → `LOW` (wciśnięcie)
- Działa jak **PLAY/TOGGLE** w UI

---

## 8) Format plików WAV (ważne!)

Najbezpieczniej:

- **WAV PCM** (uncompressed)
- **16-bit**
- **Mono**
- **22050 Hz** (może być 16000 / 44100, ale 22050 jest lekkie i zwykle działa stabilnie)

### Jak konwertować MP3 → WAV (ffmpeg)

Przykład (polecany):

```bash
ffmpeg -i input.mp3 -ac 1 -ar 22050 -c:a pcm_s16le output.wav
```

Jeśli chcesz jeszcze mniejsze WAV — obniż sample rate do 16000:

```bash
ffmpeg -i input.mp3 -ac 1 -ar 16000 -c:a pcm_s16le output.wav
```

> WAV często będzie większy niż MP3, bo MP3 jest skompresowany. WAV PCM to "surowe" próbki.

---

## 9) Typowe problemy

### "Pliki nie wchodzą" / mało miejsca

Zwiększ partycję LittleFS/SPIFFS w ustawieniach (**Partition Scheme**) albo custom partitions.

Po zmianie partycji często trzeba:

1. **Erase flash** / "Erase all flash before upload" → Enabled
2. Wgrać ponownie sketch

### "MP3 nie gra"

Ten kod gra WAV z LittleFS (PCM). MP3 wymagałby innego dekodera / innej ścieżki.

### "Dziwne rozmiary w UI"

Rozmiary w UI są w **B** (bajtach).
1 MB = 1 048 576 B (w systemach binarnych).

---

## 10) Szybki start

1. Wgraj sketch.
2. Podepnij się do Wi-Fi:
   - normalnie w sieci **Bongenet** → sprawdź IP w Serial
   - albo AP: **ESP-SIREN** / `12345678`
3. Wejdź w przeglądarce na IP urządzenia.
4. Uploaduj pliki WAV.
5. Przypisz WAV do slotów (**SAVE**).
6. Testuj:
   - **PLAY/TOGGLE** w UI
   - przyciski fizyczne

---

## Licencja

Do uzupełnienia (np. MIT) — jak chcesz, dopiszę gotowy blok licencji.
