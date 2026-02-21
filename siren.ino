#include <WiFi.h>
#include <LittleFS.h>
#include "Audio.h"

#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

// ===== I2S PINS (S3) =====
#define I2S_BCK 6
#define I2S_LRC 7
#define I2S_DOUT 5

// ===== RELAY PINS (2-ch relay module) =====
#define RELAY1_PIN 18  // IN1
#define RELAY2_PIN 8   // IN2

const char *WIFI_SSID = "Bongenet";
const char *WIFI_PASS = "BongEe1008!";

// ===== Fallback AP (gdy STA nie wstanie) =====
const char *AP_SSID = "ESP-SIREN";
const char *AP_PASS = "12345678";
static bool wifiConnected = false;

Audio audio;
AsyncWebServer server(80);

static File uploadFile;

// ===== logging: używamy Serial0 jako pewniak na S3 =====
#define LOG Serial0

// ===== stan audio: żeby nie stopować zanim wystartuje =====
static bool audioStarted = false;

// ===== 3 sloty (fizyczne przyciski + LED) =====
static const int SLOT_COUNT = 3;

// Piny panelu (możesz zmienić)
static const uint8_t BTN_PINS[SLOT_COUNT] = { 10, 11, 12 };  // przycisk -> GND
static const uint8_t LED_PINS[SLOT_COUNT] = { 15, 16, 17 };  // LED przez rezystor -> GND

// Domyślne przypisania (jeśli brak /slots.cfg)
static String slotFile[SLOT_COUNT] = { "/1.wav", "/2.wav", "/3.wav" };

// Stan odtwarzania slotów
static int currentSlot = -1;  // -1 = nic nie gra

// Debounce przycisków
static const uint32_t DEBOUNCE_MS = 40;
static uint8_t lastRaw[SLOT_COUNT] = { 1, 1, 1 };
static uint8_t stableState[SLOT_COUNT] = { 1, 1, 1 };
static uint32_t lastChangeMs[SLOT_COUNT] = { 0, 0, 0 };

// Auto-loop guard
static uint32_t lastRestartMs = 0;
static const uint32_t RESTART_GUARD_MS = 150;

// Volume 0..21 (Audio.h)
int currentVolume = 18;

// ---------- Helpers / Debug ----------
void logMem(const char *tag) {
  LOG.printf("[%s] freeHeap=%u, minFreeHeap=%u, maxAlloc=%u, psramFree=%u, psramSize=%u\n",
             tag,
             ESP.getFreeHeap(),
             ESP.getMinFreeHeap(),
             ESP.getMaxAllocHeap(),
             ESP.getFreePsram(),
             ESP.getPsramSize());
}

bool isSafeWavName(String name) {
  if (!name.endsWith(".wav")) return false;
  if (name.indexOf("..") >= 0) return false;

  if (name.startsWith("/")) name = name.substring(1);
  if (name.indexOf("/") >= 0 || name.indexOf("\\") >= 0) return false;
  return true;
}

String normalizePath(String name) {
  if (!name.startsWith("/")) name = "/" + name;
  return name;
}

// Bezpieczne zatrzymanie – tylko gdy audio faktycznie grało
void safeStopAudio(const char *reason) {
  LOG.printf("[AUDIO] stop (%s) started=%d\n", reason, (int)audioStarted);
  if (!audioStarted) return;
  audio.stopSong();
  delay(10);
  audio.loop();
  audioStarted = false;
}

// LEDy: świeci tylko aktywny slot
void setActiveLed(int idx) {
  for (int i = 0; i < SLOT_COUNT; i++) {
    digitalWrite(LED_PINS[i], (i == idx) ? HIGH : LOW);
  }
}

// ===== Relay (active LOW by default) =====
void relayInit() {
  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);

  // Startowo WYŁĄCZ (czyli głośnik na syrenie)
  digitalWrite(RELAY1_PIN, HIGH);
  digitalWrite(RELAY2_PIN, HIGH);
}

void relayToEsp(bool on) {
  // on=true: przełącz na ESP (załącz przekaźniki)
  // on=false: wróć na syrenę
  if (on) {
    digitalWrite(RELAY1_PIN, LOW);
    digitalWrite(RELAY2_PIN, LOW);
  } else {
    digitalWrite(RELAY1_PIN, HIGH);
    digitalWrite(RELAY2_PIN, HIGH);
  }
}

void stopAll(const char *reason) {
  relayToEsp(false);  // <-- zawsze wróć na syrenę
  safeStopAudio(reason);
  currentSlot = -1;
  setActiveLed(-1);
}

// Start slotu (zawsze przełącza)
void startSlot(int idx) {
  if (idx < 0 || idx >= SLOT_COUNT) return;

  // stop poprzedniego i wróć na syrenę
  stopAll("start_new_slot");
  lastRestartMs = millis();

  String path = normalizePath(slotFile[idx]);

  if (!LittleFS.exists(path)) {
    LOG.printf("[SLOT] file missing for slot %d: %s\n", idx, path.c_str());
    return;
  }

  // przełącz głośnik na ESP i start
  relayToEsp(true);
  LOG.printf("[SLOT] start slot %d => %s\n", idx, path.c_str());

  bool ok = audio.connecttoFS(LittleFS, path.c_str());
  audioStarted = ok;

  if (ok) {
    currentSlot = idx;
    setActiveLed(idx);
  } else {
    LOG.println("[SLOT] connecttoFS failed");
    relayToEsp(false);  // <-- jak nie gra, wróć na syrenę
    currentSlot = -1;
    setActiveLed(-1);
  }
}

void toggleSlot(int idx) {
  if (currentSlot == idx) {
    stopAll("toggle_stop");
  } else {
    startSlot(idx);
  }
}

// ====== Persist slot mapping (/slots.cfg) ======
static const char *SLOT_CFG_PATH = "/slots.cfg";

void saveSlots() {
  File f = LittleFS.open(SLOT_CFG_PATH, "w");
  if (!f) {
    LOG.println("[CFG] cannot write /slots.cfg");
    return;
  }
  for (int i = 0; i < SLOT_COUNT; i++) {
    String p = normalizePath(slotFile[i]);
    f.println(p);
  }
  f.close();
  LOG.println("[CFG] saved /slots.cfg");
}

void loadSlots() {
  if (!LittleFS.exists(SLOT_CFG_PATH)) {
    LOG.println("[CFG] no /slots.cfg, using defaults");
    return;
  }
  File f = LittleFS.open(SLOT_CFG_PATH, "r");
  if (!f) {
    LOG.println("[CFG] cannot open /slots.cfg");
    return;
  }

  int i = 0;
  while (f.available() && i < SLOT_COUNT) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      slotFile[i] = normalizePath(line);
    }
    i++;
  }
  f.close();

  LOG.println("[CFG] loaded /slots.cfg:");
  for (int k = 0; k < SLOT_COUNT; k++) {
    LOG.printf("  slot %d => %s\n", k, slotFile[k].c_str());
  }
}

// ---------- UI HTML ----------
const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8"/>
  <meta name="viewport" content="width=device-width,initial-scale=1"/>
  <title>ESP32 WAV Manager (3 Slots)</title>
  <style>
    body{font-family:Arial;margin:20px;max-width:900px}
    .box{padding:12px;border:1px solid #ddd;border-radius:10px;margin-bottom:12px}
    button{padding:8px 12px;margin-right:6px;cursor:pointer}
    select,input{padding:8px}
    code{background:#f4f4f4;padding:2px 6px;border-radius:6px}
    #msg{padding:10px;border-radius:10px;background:#f7f7f7}
    .row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
    .slotRow{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin:6px 0}
    .slotName{min-width:60px;font-weight:bold}
    .small{font-size:12px;color:#666}
  </style>
</head>
<body>
  <h2>ESP32 WAV Manager (3 Slots + Buttons)</h2>

  <div class="box" id="status">Ładowanie...</div>

  <div class="box">
    <div class="row">
      <h3 style="margin:0;">Sterowanie</h3>
      <button onclick="stopAll()">STOP</button>
      <span id="msg">—</span>
    </div>
    <div class="small">Sloty 1–3 odpowiadają fizycznym przyciskom. Klik PLAY przełącza slot tak samo jak przycisk.</div>
  </div>

  <div class="box">
    <h3>Głośność</h3>
    <input type="range" min="0" max="21" value="18" id="vol" oninput="setVolume(this.value)">
    <span id="volVal">18</span>
  </div>

  <div class="box">
    <h3>Sloty (przypisania)</h3>
    <div id="slots"></div>
  </div>

  <div class="box">
    <h3>Upload WAV</h3>
    <p>Wymagania: WAV PCM 16-bit, najlepiej mono 22050Hz.</p>
    <form id="up" method="POST" action="/upload" enctype="multipart/form-data">
      <input type="file" name="file" accept=".wav"/>
      <button type="submit">Wyślij</button>
    </form>
  </div>

  <div class="box">
    <h3>Pliki</h3>
    <button onclick="refresh()">Odśwież</button>
    <ul id="files"></ul>
  </div>

<script>
function setMsg(t){ document.getElementById('msg').textContent = t; }

async function apiText(url){
  const r = await fetch(url);
  const t = await r.text();
  return { r, t };
}

async function setVolume(v){
  document.getElementById('volVal').innerText = v;
  await fetch('/api/volume/set?v=' + v);
}

async function loadVolume(){
  const v = await fetch('/api/volume/get').then(r=>r.text());
  document.getElementById('vol').value = v;
  document.getElementById('volVal').innerText = v;
}

async function refresh(){
  const st = await fetch('/api/status').then(r=>r.json());
  document.getElementById('status').innerHTML =
    'IP: <code>' + st.ip + '</code> (mode: <code>' + st.mode + '</code>)<br/>' +
    'LittleFS: ' + st.used + ' / ' + st.total + ' B (free: ' + st.free + ' B)';

  const list = await fetch('/api/list').then(r=>r.json());
  const slots = await fetch('/api/slots').then(r=>r.json());

  // --- render slots ---
  const slotsDiv = document.getElementById('slots');
  slotsDiv.innerHTML = '';

  function mkOption(name){
    const o = document.createElement('option');
    o.value = name;
    o.textContent = name;
    return o;
  }

  for(let i=0;i<3;i++){
    const row = document.createElement('div');
    row.className = 'slotRow';

    const label = document.createElement('div');
    label.className = 'slotName';
    label.textContent = 'Slot ' + (i+1);

    const cur = document.createElement('code');
    cur.textContent = slots.slots[i] || '(brak)';

    const sel = document.createElement('select');
    sel.appendChild(mkOption('(wybierz plik)'));
    list.files.forEach(f => sel.appendChild(mkOption(f.name)));

    if (slots.slots[i]) sel.value = slots.slots[i];

    const bSave = document.createElement('button');
    bSave.textContent = 'SAVE';
    bSave.onclick = async () => {
      if (!sel.value || sel.value === '(wybierz plik)') {
        setMsg('Wybierz plik .wav dla slotu ' + (i+1));
        return;
      }
      const {r,t} = await apiText('/api/slot/set?idx='+i+'&name='+encodeURIComponent(sel.value));
      setMsg('SAVE slot ' + (i+1) + ' => ' + r.status + ' ' + t);
      refresh();
    };

    const bPlay = document.createElement('button');
    bPlay.textContent = 'PLAY/TOGGLE';
    bPlay.onclick = async () => {
      const {r,t} = await apiText('/api/slot/toggle?idx='+i);
      setMsg('TOGGLE slot ' + (i+1) + ' => ' + r.status + ' ' + t);
      refresh();
    };

    row.appendChild(label);
    row.appendChild(document.createTextNode('Aktualnie: '));
    row.appendChild(cur);
    row.appendChild(document.createTextNode('  '));
    row.appendChild(sel);
    row.appendChild(bSave);
    row.appendChild(bPlay);

    slotsDiv.appendChild(row);
  }

  // --- render files list ---
  const ul = document.getElementById('files');
  ul.innerHTML = '';
  list.files.forEach(f=>{
    const li = document.createElement('li');
    const code = document.createElement('code');
    code.textContent = f.name;
    const text = document.createTextNode(' (' + f.size + ' B) ');

    const bPlay = document.createElement('button');
    bPlay.textContent = 'PLAY';
    bPlay.onclick = async () => {
      setMsg('PLAY: ' + f.name);
      const {r,t} = await apiText('/api/play?name='+encodeURIComponent(f.name));
      setMsg('PLAY => ' + r.status + ' ' + t);
    };

    const bDel = document.createElement('button');
    bDel.textContent = 'DEL';
    bDel.onclick = async () => {
      setMsg('DEL: ' + f.name);
      const {r,t} = await apiText('/api/delete?name='+encodeURIComponent(f.name));
      setMsg('DEL => ' + r.status + ' ' + t);
      refresh();
    };

    li.appendChild(code);
    li.appendChild(text);
    li.appendChild(bPlay);
    li.appendChild(document.createTextNode(' '));
    li.appendChild(bDel);
    ul.appendChild(li);
  });

  loadVolume();
}

async function stopAll(){
  const {r,t} = await apiText('/api/stop');
  setMsg('STOP => ' + r.status + ' ' + t);
  refresh();
}

refresh();
</script>

</body>
</html>
)HTML";

void setup() {
  // --- start Serial ---
  Serial.begin(115200);
  Serial0.begin(115200);
  delay(300);

  LOG.println();
  LOG.println("=== BOOT ===");
  logMem("boot");

  // --- LittleFS ---
  if (!LittleFS.begin(true)) {
    LOG.println("LittleFS mount failed!");
    while (true) delay(1000);
  }
  LOG.println("LittleFS OK");
  logMem("after_lfs");

  // load slot mapping (if exists)
  loadSlots();

  // relay
  relayInit();

  // --- Audio/I2S ---
  audio.setPinout(I2S_BCK, I2S_LRC, I2S_DOUT);
  audio.setVolume(currentVolume);
  LOG.printf("I2S pins: BCK=%d LRC=%d DOUT=%d\n", I2S_BCK, I2S_LRC, I2S_DOUT);

  // --- Buttons + LEDs ---
  for (int i = 0; i < SLOT_COUNT; i++) {
    pinMode(BTN_PINS[i], INPUT_PULLUP);
    pinMode(LED_PINS[i], OUTPUT);
    digitalWrite(LED_PINS[i], LOW);

    lastRaw[i] = digitalRead(BTN_PINS[i]);
    stableState[i] = lastRaw[i];
    lastChangeMs[i] = millis();
  }
  LOG.println("Buttons + LEDs ready");

  // --- WiFi (STA -> fallback AP) ---
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  LOG.print("WiFi connecting");
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - startAttempt) < 10000) {
    delay(300);
    LOG.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    LOG.println();
    LOG.print("IP: ");
    LOG.println(WiFi.localIP());
  } else {
    wifiConnected = false;
    LOG.println();
    LOG.println("WiFi failed -> AP mode");

    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);

    LOG.print("AP IP: ");
    LOG.println(WiFi.softAPIP());
  }

  logMem("after_wifi");

  // ---------- Routes ----------
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", INDEX_HTML);
  });

  server.on("/api/stop", HTTP_GET, [](AsyncWebServerRequest *request) {
    LOG.println("[STOP]");
    logMem("before_stop");
    stopAll("api_stop");
    logMem("after_stop");
    request->send(200, "text/plain", "OK");
  });

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    size_t total = LittleFS.totalBytes();
    size_t used = LittleFS.usedBytes();
    size_t freeB = (total >= used) ? (total - used) : 0;

    String ip = wifiConnected ? WiFi.localIP().toString()
                              : WiFi.softAPIP().toString();
    String mode = wifiConnected ? "STA" : "AP";

    String json = "{";
    json += "\"ip\":\"" + ip + "\",";
    json += "\"mode\":\"" + mode + "\",";
    json += "\"total\":" + String(total) + ",";
    json += "\"used\":" + String(used) + ",";
    json += "\"free\":" + String(freeB);
    json += "}";
    request->send(200, "application/json", json);
  });

  server.on("/api/list", HTTP_GET, [](AsyncWebServerRequest *request) {
    File root = LittleFS.open("/");
    File f = root.openNextFile();

    String json = "{\"files\":[";
    bool first = true;
    while (f) {
      String n = String(f.name());
      size_t s = f.size();
      if (!first) json += ",";
      first = false;
      json += "{\"name\":\"" + n + "\",\"size\":" + String(s) + "}";
      f = root.openNextFile();
    }
    json += "]}";
    request->send(200, "application/json", json);
  });

  // slot mapping read
  server.on("/api/slots", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "{\"slots\":[";
    for (int i = 0; i < SLOT_COUNT; i++) {
      if (i) json += ",";
      json += "\"" + slotFile[i] + "\"";
    }
    json += "]}";
    request->send(200, "application/json", json);
  });

  // slot mapping set: /api/slot/set?idx=0&name=/x.wav
  server.on("/api/slot/set", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("idx") || !request->hasParam("name")) {
      request->send(400, "text/plain", "missing idx or name");
      return;
    }
    int idx = request->getParam("idx")->value().toInt();
    String name = request->getParam("name")->value();

    if (idx < 0 || idx >= SLOT_COUNT) {
      request->send(400, "text/plain", "bad idx");
      return;
    }
    if (!isSafeWavName(name)) {
      request->send(400, "text/plain", "bad name");
      return;
    }
    name = normalizePath(name);

    if (!LittleFS.exists(name)) {
      request->send(404, "text/plain", "file not found");
      return;
    }

    slotFile[idx] = name;
    saveSlots();

    // jeśli aktualnie gra ten slot — przeładuj od początku
    if (currentSlot == idx) {
      startSlot(idx);
    }

    request->send(200, "text/plain", "OK");
  });

  // slot toggle play/stop: /api/slot/toggle?idx=0
  server.on("/api/slot/toggle", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("idx")) {
      request->send(400, "text/plain", "missing idx");
      return;
    }
    int idx = request->getParam("idx")->value().toInt();
    if (idx < 0 || idx >= SLOT_COUNT) {
      request->send(400, "text/plain", "bad idx");
      return;
    }
    toggleSlot(idx);
    request->send(200, "text/plain", "OK");
  });

  // manual play any file: /api/play?name=/x.wav
  server.on("/api/play", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("name")) {
      request->send(400, "text/plain", "missing name");
      return;
    }

    String name = request->getParam("name")->value();
    if (!isSafeWavName(name)) {
      request->send(400, "text/plain", "bad name");
      return;
    }
    name = normalizePath(name);

    if (!LittleFS.exists(name)) {
      request->send(404, "text/plain", "not found");
      return;
    }

    LOG.printf("[PLAY] %s\n", name.c_str());
    logMem("before_play");

    // To odtwarza "ad-hoc" i nie ustawia slotu (LEDy zgaszone)
    stopAll("manual_play");
    relayToEsp(true);

    bool ok = audio.connecttoFS(LittleFS, name.c_str());
    audioStarted = ok;

    setActiveLed(-1);
    currentSlot = -1;

    if (!ok) {
      relayToEsp(false);
      request->send(500, "text/plain", "FAIL connecttoFS");
      return;
    }

    LOG.printf("[PLAY] connecttoFS=%d\n", (int)ok);
    logMem("after_connecttoFS");

    request->send(200, "text/plain", "OK");
  });

  // delete: /api/delete?name=/x.wav
  server.on("/api/delete", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("name")) {
      request->send(400, "text/plain", "missing name");
      return;
    }

    String name = request->getParam("name")->value();
    if (!isSafeWavName(name)) {
      request->send(400, "text/plain", "bad name");
      return;
    }
    name = normalizePath(name);

    LOG.printf("[DEL] %s\n", name.c_str());
    logMem("before_delete");

    // stop audio to free FD
    stopAll("delete");

    bool ok = false;
    for (int i = 0; i < 8; i++) {
      if (!LittleFS.exists(name)) {
        ok = true;
        break;
      }
      if (LittleFS.remove(name)) {
        ok = true;
        break;
      }
      delay(80);
      audio.loop();
    }

    logMem("after_delete");
    request->send(ok ? 200 : 423, "text/plain", ok ? "OK" : "BUSY (file open)");
  });

  // upload
  server.on(
    "/upload", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      request->redirect("/");
    },
    [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
      if (!index) {
        LOG.printf("[UPLOAD] start filename=%s\n", filename.c_str());
        logMem("before_upload");

        stopAll("upload");

        if (!filename.endsWith(".wav")) {
          request->send(400, "text/plain", "Only .wav allowed");
          return;
        }

        // tylko nazwa bez ścieżek
        if (filename.indexOf("/") >= 0 || filename.indexOf("\\") >= 0 || filename.indexOf("..") >= 0) {
          request->send(400, "text/plain", "Bad filename");
          return;
        }

        String path = "/" + filename;
        uploadFile = LittleFS.open(path, "w");
        if (!uploadFile) {
          request->send(500, "text/plain", "Cannot open file");
          return;
        }
      }

      if (uploadFile) uploadFile.write(data, len);

      if (final) {
        if (uploadFile) uploadFile.close();
        LOG.printf("[UPLOAD] done (%u bytes)\n", (unsigned)(index + len));
        logMem("after_upload");
      }
    });

  // ===== VOLUME SET =====
  server.on("/api/volume/set", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("v")) {
      request->send(400, "text/plain", "missing v");
      return;
    }

    int v = request->getParam("v")->value().toInt();
    v = constrain(v, 0, 21);

    currentVolume = v;
    audio.setVolume(currentVolume);

    LOG.printf("[VOL] set to %d\n", currentVolume);
    request->send(200, "text/plain", "OK");
  });

  // ===== VOLUME GET =====
  server.on("/api/volume/get", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", String(currentVolume));
  });

  server.begin();
  LOG.println("HTTP server started");
  logMem("server_started");
}

void loop() {
  audio.loop();

  // ===== AUTO-LOOP (polling): jeśli skończył grać, odpal od początku (tylko dla slotów) =====
  if (currentSlot >= 0 && audioStarted) {
    if (!audio.isRunning()) {
      uint32_t now = millis();
      if (now - lastRestartMs >= RESTART_GUARD_MS) {
        lastRestartMs = now;

        String path = normalizePath(slotFile[currentSlot]);
        if (LittleFS.exists(path)) {
          LOG.printf("[LOOP] restart slot %d => %s\n", currentSlot, path.c_str());

          // relay zostaje na ESP
          safeStopAudio("poll_restart");
          bool ok = audio.connecttoFS(LittleFS, path.c_str());
          audioStarted = ok;

          if (!ok) {
            LOG.println("[LOOP] restart failed, back to siren");
            stopAll("restart_failed");
          } else {
            setActiveLed(currentSlot);
          }
        } else {
          LOG.printf("[LOOP] missing on restart: %s\n", path.c_str());
          stopAll("missing_on_poll_restart");
        }
      }
    }
  }

  // ===== OBSŁUGA PRZYCISKÓW (toggle + debounce) =====
  uint32_t now = millis();

  for (int i = 0; i < SLOT_COUNT; i++) {
    uint8_t raw = digitalRead(BTN_PINS[i]);  // HIGH=puszczony, LOW=wciśnięty

    if (raw != lastRaw[i]) {
      lastRaw[i] = raw;
      lastChangeMs[i] = now;
    }

    if ((now - lastChangeMs[i]) >= DEBOUNCE_MS && raw != stableState[i]) {
      stableState[i] = raw;

      // zbocze: puszczony->wciśnięty
      if (stableState[i] == LOW) {
        LOG.printf("[BTN] slot %d pressed\n", i);
        toggleSlot(i);
      }
    }
  }
}
