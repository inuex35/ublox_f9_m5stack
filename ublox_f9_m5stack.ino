#include <M5Unified.h>
#include <WiFi.h>
#include <SD.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_sleep.h>
#include <freertos/stream_buffer.h>
#include <freertos/semphr.h>
#include "NTRIPClient.h"
#include "GnssLink.h"
#include "ObdSpeed.h"

NTRIPClient ntrip_c;
WebServer server(80);
Preferences prefs;
const char* ntpServer = "ntp.nict.jp";
const long gmtOffset_sec = 3600 * 9;
const int daylightOffset_sec = 0;

const int MAX_WIFI = 10;
const int MAX_STATIONS = 10;
const uint32_t SD_FLUSH_INTERVAL_MS = 1000;
const uint32_t GGA_INTERVAL_MS = 10000;
const uint32_t RECONNECT_INTERVAL_MS = 10000;
const uint32_t MENU_AUTO_SELECT_MS = 10000;     // the saved choice is taken if no button is touched
const uint32_t SLEEP_CHECK_S = 30;              // while the engine is off: wake up and ask the car this often
const uint32_t POWER_LOST_MS = 3000;            // external power below 4 V this long: close the file, power off

struct ReferenceStation {
  String name;
  String serverAddress;
  int portNumber;
  String mountPoint;
  String username;
  String password;
};

struct WiFiNetwork {
  String ssid;
  String password;
};

// Settings from config.txt (all optional except WIFI)
struct LoggerConfig {
  LinkType link = LinkType::I2C;
  int rateHz = 0;                 // 0: 5 Hz on UART, 2 Hz on I2C (I2C carries about 10 KB/s)
  int canRx = -1, canTx = -1;     // OBD-II CAN, -1: not used
  uint32_t engineOffS = 60;
} cfg;

ReferenceStation referenceStations[MAX_STATIONS];
int selectedStationIndex = 0;
bool useNtrip = true;
char fileName[40] = "";
bool sdOk = false;

// Remembered across deep sleep (engine off)
RTC_DATA_ATTR bool rtcEngineSleep = false;
RTC_DATA_ATTR int rtcCanRx = -1, rtcCanTx = -1;

// Latest GGA sentence seen in the receiver's output (written by the GNSS task, read by the NTRIP task).
portMUX_TYPE ggaMux = portMUX_INITIALIZER_UNLOCKED;
char latestGGA[100] = "";

// Receiver data: GNSS task -> ring buffer -> SD task. The ring absorbs SD card stalls and web downloads.
StreamBufferHandle_t logRing;
StaticStreamBuffer_t logRingStruct;
volatile uint32_t receivedBytes = 0, droppedBytes = 0, loggedBytes = 0;
// SD card and display share the SPI bus; every access from any task holds this.
SemaphoreHandle_t spiMutex;
File dataFile;
volatile bool fileOpen = false;
volatile int logCommand = 0;                    // 1: close the file, 2: open a new one (handled by the SD task)

bool loadConfigFromSD(WiFiNetwork* wifiNetworks, int& numWiFiNetworks, ReferenceStation* stations, int& numStations) {
  File configFile = SD.open("/config.txt");
  if (!configFile) {
    M5.Lcd.println("Failed to open config file");
    return false;
  }

  String line;
  int wifiIndex = 0;
  int stationIndex = 0;

  while (configFile.available()) {
    line = configFile.readStringUntil('\n');
    line.trim();

    if (line.startsWith("WIFI:") && wifiIndex < MAX_WIFI) {
      int delimiterIndex = line.indexOf(',');
      if (delimiterIndex != -1) {
        wifiNetworks[wifiIndex].ssid = line.substring(5, delimiterIndex);
        wifiNetworks[wifiIndex].password = line.substring(delimiterIndex + 1);
        wifiIndex++;
      }
    } else if (line.startsWith("STATION:") && stationIndex < MAX_STATIONS - 1) {
      // STATION:NAME,SERVER_ADDRESS,PORT,MOUNTPOINT,USERNAME,PASSWORD (username/password may be empty)
      int d1 = line.indexOf(',');
      int d2 = line.indexOf(',', d1 + 1);
      int d3 = line.indexOf(',', d2 + 1);
      int d4 = line.indexOf(',', d3 + 1);
      int d5 = line.indexOf(',', d4 + 1);

      if (d1 != -1 && d2 != -1 && d3 != -1) {
        ReferenceStation& st = stations[stationIndex];
        st.name = line.substring(8, d1);
        st.serverAddress = line.substring(d1 + 1, d2);
        st.portNumber = line.substring(d2 + 1, d3).toInt();
        st.mountPoint = d4 == -1 ? line.substring(d3 + 1) : line.substring(d3 + 1, d4);
        st.username = d4 == -1 ? "" : (d5 == -1 ? line.substring(d4 + 1) : line.substring(d4 + 1, d5));
        st.password = d5 == -1 ? "" : line.substring(d5 + 1);
        stationIndex++;
      }
    } else if (line.startsWith("GNSS:")) {          // GNSS:I2C (Port A) or GNSS:UART (Port C)
      cfg.link = line.substring(5).equalsIgnoreCase("UART") ? LinkType::UART : LinkType::I2C;
    } else if (line.startsWith("RATE_HZ:")) {       // RAWX / navigation rate
      cfg.rateHz = constrain(line.substring(8).toInt(), 1, 10);
    } else if (line.startsWith("CAN:")) {           // CAN:ON (Port B) or CAN:<rx gpio>,<tx gpio>
      String v = line.substring(4);
      int c = v.indexOf(',');
      if (c != -1) { cfg.canRx = v.substring(0, c).toInt(); cfg.canTx = v.substring(c + 1).toInt(); }
      else if (v.equalsIgnoreCase("ON")) { cfg.canRx = M5.getPin(m5::pin_name_t::port_b_in); cfg.canTx = M5.getPin(m5::pin_name_t::port_b_out); }
    } else if (line.startsWith("ENGINE_OFF_S:")) {  // seconds without an OBD answer before sleeping
      cfg.engineOffS = max(10L, line.substring(13).toInt());
    }
  }

  configFile.close();

  // Last entry: log without corrections (post-processing / PPK does not need NTRIP).
  stations[stationIndex++].name = "(no NTRIP, log only)";

  numWiFiNetworks = wifiIndex;
  numStations = stationIndex;
  if (cfg.rateHz == 0) cfg.rateHz = cfg.link == LinkType::UART ? 5 : 2;
  return true;
}

// Button menu: A up, C down, B select. With autoMs > 0 the preselected entry is taken after that time
// unless a button is touched (unattended start in the car).
int selectFromList(const char* title, const String* items, int n, int selected, uint32_t autoMs) {
  int scrollOffset = max(0, selected - 4);
  uint32_t start = millis();
  bool counting = autoMs > 0;
  int shownSecs = -1;
  bool redraw = true;
  while (true) {
    M5.update();
    if (M5.BtnA.wasPressed()) { counting = false; if (selected > 0) selected--; redraw = true; }
    if (M5.BtnC.wasPressed()) { counting = false; if (selected < n - 1) selected++; redraw = true; }
    if (M5.BtnB.wasPressed()) return selected;
    if (counting && millis() - start >= autoMs) return selected;
    int secs = counting ? (autoMs - (millis() - start) + 999) / 1000 : -1;
    if (secs != shownSecs) { shownSecs = secs; redraw = true; }
    if (selected < scrollOffset) scrollOffset = selected;
    if (selected >= scrollOffset + 5) scrollOffset = selected - 4;
    if (redraw) {
      redraw = false;
      M5.Lcd.clear();
      M5.Lcd.setCursor(0, 0);
      M5.Lcd.println(title);
      for (int i = scrollOffset; i < min(n, scrollOffset + 5); ++i) {
        M5.Lcd.print(i == selected ? "> " : "  ");
        M5.Lcd.println(items[i]);
      }
      M5.Lcd.println();
      M5.Lcd.println("A: up  B: select  C: down");
      if (counting) M5.Lcd.printf("auto select in %d s\n", secs);
    }
    delay(20);
  }
}

// Open the correction stream of the selected mountpoint. Returns false on any failure.
bool requestStream(const ReferenceStation& station) {
  return ntrip_c.reqRaw(station.serverAddress.c_str(), station.portNumber, station.mountPoint.c_str(),
                        station.username.c_str(), station.password.c_str());
}

void connectToStation(const ReferenceStation& station) {
  M5.Lcd.clear();
  M5.Lcd.setCursor(0, 0);
  M5.Lcd.println("Connecting to:");
  M5.Lcd.println(station.name);

  M5.Lcd.println("Requesting SourceTable");
  if (ntrip_c.reqSrcTbl(station.serverAddress.c_str(), station.portNumber)) {
    char buffer[512];
    delay(5);
    while(ntrip_c.available()){
      ntrip_c.readLine(buffer,sizeof(buffer));
      M5.Lcd.print(buffer);
    }
    M5.Lcd.println("Requesting SourceTable is OK");
  }
  else{
    M5.Lcd.println("SourceTable request error");
  }
  ntrip_c.stop();

  M5.Lcd.println("Requesting MountPoint's Raw data");
  if (requestStream(station)) M5.Lcd.println("Requesting MountPoint is OK");
  else M5.Lcd.println("Requesting MountPoint failed, will retry");   // the NTRIP task keeps trying
  delay(2000);
}

// Corrections: caster -> receiver, one complete RTCM3 frame at a time (other messages to the receiver,
// such as the vehicle speed, go in between frames). Also uploads GGA and reconnects.
void ntripClientTask(void *pvParameters) {
  static uint8_t frame[1030];
  int pos = 0, need = 0;
  uint32_t lastGGA = 0, lastReconnect = 0;
  while (true) {
    if (!ntrip_c.connected()) {
      pos = 0;
      if (millis() - lastReconnect > RECONNECT_INTERVAL_MS) {
        lastReconnect = millis();
        if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
        else if (requestStream(referenceStations[selectedStationIndex])) lastGGA = 0;   // send GGA right away
      }
      vTaskDelay(100 / portTICK_PERIOD_MS);
      continue;
    }

    while (ntrip_c.available()) {
      uint8_t c = ntrip_c.read();
      if (pos == 0 && c != 0xD3) continue;    // RTCM3 preamble
      frame[pos++] = c;
      if (pos == 3) {
        need = (((frame[1] & 0x03) << 8) | frame[2]) + 6;   // header 3 + payload + CRC 3
        if (need > (int)sizeof(frame)) pos = 0;
      } else if (pos > 3 && pos == need) {
        gnss::queue(frame, need);
        pos = 0;
      }
    }

    if (lastGGA == 0 || millis() - lastGGA > GGA_INTERVAL_MS) {
      char gga[sizeof(latestGGA)];
      portENTER_CRITICAL(&ggaMux);
      strcpy(gga, latestGGA);
      portEXIT_CRITICAL(&ggaMux);
      if (gga[0]) {
        ntrip_c.sendGGA(gga);
        lastGGA = millis();
      }
    }

    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

// Remember the latest complete NMEA GGA sentence with a fix from the (mixed UBX/NMEA) receiver stream.
void scanForGGA(const uint8_t* data, int len) {
  static char line[sizeof(latestGGA)];
  static int pos = -1;              // -1: not inside a '$' sentence
  for (int i = 0; i < len; i++) {
    char c = (char)data[i];
    if (c == '$') { pos = 0; line[pos++] = c; continue; }
    if (pos < 0) continue;
    if (c == '\r' || c == '\n') {
      line[pos] = '\0';
      // $GPGGA / $GNGGA with a fix: the quality (7th) field is not empty and not '0'
      const char* q = line;
      for (int f = 0; f < 6 && q; f++) { q = strchr(q, ','); if (q) q++; }
      if (pos > 6 && strncmp(line + 3, "GGA,", 4) == 0 && q && *q && *q != ',' && *q != '0') {
        portENTER_CRITICAL(&ggaMux);
        strcpy(latestGGA, line);
        portEXIT_CRITICAL(&ggaMux);
      }
      pos = -1;
    } else if (pos < (int)sizeof(line) - 1 && c >= 0x20 && c < 0x7f) {
      line[pos++] = c;
    } else {
      pos = -1;                     // binary (UBX) byte or too long: not NMEA
    }
  }
}

// Receiver <-> link: send queued messages, read the receiver's output into the ring buffer.
void gnssDataTask(void *pvParameters) {
  static uint8_t data[4096];
  while (true) {
    gnss::serviceWrites();
    int n = gnss::read(data, sizeof(data));
    if (n > 0) {
      receivedBytes += n;
      scanForGGA(data, n);
      gnss::checkFrames(data, n);
      if (fileOpen) {
        size_t sent = xStreamBufferSend(logRing, data, n, 0);
        if (sent < (size_t)n) droppedBytes += n - sent;
      }
    }
    vTaskDelay((n > 0 ? 2 : 10) / portTICK_PERIOD_MS);
  }
}

// Name of a new log file: date and time when NTP has synced, otherwise a running number.
void makeFileName() {
  struct tm t;
  if (WiFi.status() == WL_CONNECTED && getLocalTime(&t, 5000)) {
    sprintf(fileName, "/data_%04d%02d%02d_%02d%02d%02d.ubx",
            t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
  } else {
    uint32_t no = prefs.getUInt("fileNo", 0) + 1;
    prefs.putUInt("fileNo", no);
    sprintf(fileName, "/log_%05lu.ubx", (unsigned long)no);
  }
}

// Ring buffer -> SD card. Owns the log file; opens and closes it on request.
void sdTask(void *pvParameters) {
  static uint8_t chunk[8192];
  uint32_t lastFlush = millis();
  while (true) {
    if (logCommand == 1 || logCommand == 2) {
      int cmd = logCommand;
      xSemaphoreTake(spiMutex, portMAX_DELAY);
      if (fileOpen) {
        fileOpen = false;
        size_t n;
        while ((n = xStreamBufferReceive(logRing, chunk, sizeof(chunk), 0)) > 0) { dataFile.write(chunk, n); loggedBytes += n; }
        dataFile.close();
      }
      xSemaphoreGive(spiMutex);
      if (cmd == 2) {
        makeFileName();
        xSemaphoreTake(spiMutex, portMAX_DELAY);
        dataFile = SD.open(fileName, FILE_APPEND);
        xSemaphoreGive(spiMutex);
        fileOpen = (bool)dataFile;
      }
      logCommand = 0;
    }

    size_t n = xStreamBufferReceive(logRing, chunk, sizeof(chunk), 100 / portTICK_PERIOD_MS);
    if (n > 0 && fileOpen) {
      xSemaphoreTake(spiMutex, portMAX_DELAY);
      dataFile.write(chunk, n);
      xSemaphoreGive(spiMutex);
      loggedBytes += n;
    }
    if (fileOpen && millis() - lastFlush > SD_FLUSH_INTERVAL_MS) {
      xSemaphoreTake(spiMutex, portMAX_DELAY);
      dataFile.flush();
      xSemaphoreGive(spiMutex);
      lastFlush = millis();
    }
  }
}

// Ask the SD task to close the log file and wait until it has.
void closeLog() {
  if (!fileOpen) return;
  logCommand = 1;
  for (int i = 0; i < 300 && logCommand != 0; i++) delay(10);
}

// ---- Download over WiFi: http://<M5Stack IP>/ lists the .ubx files on the SD card ----
void handleIndex() {
  String html = "<html><head><meta name=viewport content='width=device-width'></head><body><h3>GNSS logs</h3><ul>";
  xSemaphoreTake(spiMutex, portMAX_DELAY);
  File root = SD.open("/");
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    String name = f.name();
    if (!f.isDirectory() && name.endsWith(".ubx")) {
      if (!name.startsWith("/")) name = "/" + name;
      html += "<li><a href='/dl?f=" + name + "'>" + name.substring(1) + "</a> (" + String((unsigned long)f.size() / 1024) + " KB)";
      if (fileOpen && name == String(fileName)) html += " <b>recording</b>";
      html += "</li>";
    }
    f.close();
  }
  root.close();
  xSemaphoreGive(spiMutex);
  html += "</ul></body></html>";
  server.send(200, "text/html", html);
}

// Sent in chunks so that the SD task can keep writing in between.
void handleDownload() {
  String name = server.arg("f");
  if (!name.startsWith("/") || name.indexOf("..") >= 0 || !name.endsWith(".ubx")) { server.send(400, "text/plain", "bad name"); return; }
  xSemaphoreTake(spiMutex, portMAX_DELAY);
  if (fileOpen && name == String(fileName)) dataFile.flush();     // include everything logged so far
  File f = SD.open(name, FILE_READ);
  size_t size = f ? f.size() : 0;
  xSemaphoreGive(spiMutex);
  if (!f) { server.send(404, "text/plain", "not found"); return; }
  server.setContentLength(size);
  server.sendHeader("Content-Disposition", "attachment; filename=" + name.substring(1));
  server.send(200, "application/octet-stream", "");
  static uint8_t buf[4096];
  WiFiClient client = server.client();
  size_t left = size;
  while (left > 0 && client.connected()) {
    xSemaphoreTake(spiMutex, portMAX_DELAY);
    int n = f.read(buf, min(left, sizeof(buf)));
    xSemaphoreGive(spiMutex);
    if (n <= 0) break;
    client.write(buf, n);
    left -= n;
  }
  xSemaphoreTake(spiMutex, portMAX_DELAY);
  f.close();
  xSemaphoreGive(spiMutex);
}

// Engine off: close the log, turn the screen off and sleep; wake every SLEEP_CHECK_S to ask the car.
void sleepUntilEngineRuns() {
  closeLog();
  M5.Lcd.clear();
  M5.Lcd.setCursor(0, 0);
  M5.Lcd.println("Engine off: log closed, sleeping");
  delay(1500);
  rtcEngineSleep = true;
  rtcCanRx = cfg.canRx;
  rtcCanTx = cfg.canTx;
  WiFi.disconnect(true);
  M5.Display.sleep();
  M5.Display.setBrightness(0);
  M5.Power.deepSleep((uint64_t)SLEEP_CHECK_S * 1000000ULL, true);   // a touch also wakes it
}

void setup() {
  // Woken up from engine-off sleep by the timer: ask the car quietly (screen stays off) before starting.
  if (rtcEngineSleep && esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER && rtcCanRx >= 0) {
    if (!obd::probe(rtcCanRx, rtcCanTx, 600)) {
      esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_CHECK_S * 1000000ULL);
      esp_deep_sleep_start();
    }
  }
  bool unattended = rtcEngineSleep;             // started by the car: take the saved choices
  rtcEngineSleep = false;

  auto m5cfg = M5.config();
  M5.begin(m5cfg);
  Serial.begin(115200);
  prefs.begin("logger");
  spiMutex = xSemaphoreCreateMutex();
  M5.Lcd.setTextSize(1);

  WiFiNetwork wifiNetworks[MAX_WIFI];
  int numWiFiNetworks = 0;
  int numStations = 0;

  sdOk = SD.begin(GPIO_NUM_4, SPI, 25000000) &&
         loadConfigFromSD(wifiNetworks, numWiFiNetworks, referenceStations, numStations);
  if (!sdOk) {                                  // still check the receiver link, just without logging
    M5.Lcd.println("No SD card / config.txt: receiver check only, nothing is logged");
    referenceStations[0].name = "(no NTRIP, log only)";
    numStations = 1;
    cfg.rateHz = cfg.link == LinkType::UART ? 5 : 2;
    delay(2000);
  }

  // WiFi (for NTRIP, NTP and downloads). Logging also works without it.
  if (numWiFiNetworks > 0) {
    String names[MAX_WIFI];
    int saved = 0;
    for (int i = 0; i < numWiFiNetworks; i++) {
      names[i] = wifiNetworks[i].ssid;
      if (names[i] == prefs.getString("ssid", "")) saved = i;
    }
    int w = selectFromList("Select a WiFi network:", names, numWiFiNetworks, saved, unattended ? 1 : MENU_AUTO_SELECT_MS);
    prefs.putString("ssid", names[w]);
    M5.Lcd.clear();
    M5.Lcd.setCursor(0, 0);
    M5.Lcd.print("Connecting to ");
    M5.Lcd.println(names[w]);
    WiFi.begin(wifiNetworks[w].ssid.c_str(), wifiNetworks[w].password.c_str());
    uint32_t t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 10000) { delay(500); M5.Lcd.print("."); }
    if (WiFi.status() == WL_CONNECTED) M5.Lcd.println("\nConnected!");
    else M5.Lcd.println("\nNot connected, logging without WiFi (keeps retrying)");
    delay(1500);
  }

  String stationNames[MAX_STATIONS];
  int savedStation = numStations - 1;
  for (int i = 0; i < numStations; i++) {
    stationNames[i] = referenceStations[i].name;
    if (stationNames[i] == prefs.getString("station", "")) savedStation = i;
  }
  selectedStationIndex = selectFromList("Select a reference station:", stationNames, numStations, savedStation,
                                        unattended ? 1 : MENU_AUTO_SELECT_MS);
  prefs.putString("station", stationNames[selectedStationIndex]);
  useNtrip = referenceStations[selectedStationIndex].serverAddress.length() > 0;
  if (useNtrip && WiFi.status() == WL_CONNECTED) connectToStation(referenceStations[selectedStationIndex]);
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  // Receiver
  M5.Lcd.clear();
  M5.Lcd.setCursor(0, 0);
  bool uartLink = cfg.link == LinkType::UART;
  int pinA = M5.getPin(uartLink ? m5::pin_name_t::port_c_rxd : m5::pin_name_t::port_a_sda);
  int pinB = M5.getPin(uartLink ? m5::pin_name_t::port_c_txd : m5::pin_name_t::port_a_scl);
  M5.Lcd.printf("Receiver on %s (GPIO %d/%d)...\n", uartLink ? "UART, Port C" : "I2C, Port A", pinA, pinB);
  if (gnss::begin(cfg.link, pinA, pinB)) {
    if (uartLink) M5.Lcd.printf("Receiver found, %lu bps\n", (unsigned long)gnss::uartBaud());
    else M5.Lcd.println("Receiver found");
  } else {
    M5.Lcd.println("Receiver NOT found, check the cable");
  }
  bool useCan = cfg.canRx >= 0;
  gnss::applyConfig(cfg.rateHz, useCan);
  M5.Lcd.printf("Rate %d Hz, OBD speed %s\n", cfg.rateHz, useCan ? "on" : "off");
  if (useCan && !obd::begin(cfg.canRx, cfg.canTx)) M5.Lcd.println("CAN driver failed");

  // Log ring buffer: large in PSRAM (Core2), smaller in internal RAM otherwise.
  size_t ringSize = psramFound() ? 512 * 1024 : 48 * 1024;
  uint8_t* ringMem = (uint8_t*)(psramFound() ? ps_malloc(ringSize + 1) : malloc(ringSize + 1));
  logRing = xStreamBufferCreateStatic(ringSize, 1, ringMem, &logRingStruct);

  xTaskCreatePinnedToCore(gnssDataTask, "GNSSData", 6144, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(sdTask, "SDWrite", 6144, NULL, 2, NULL, 1);
  if (useNtrip) xTaskCreatePinnedToCore(ntripClientTask, "NTRIPClient", 6144, NULL, 1, NULL, 0);

  if (sdOk) {
    logCommand = 2;                             // open the first log file
    for (int i = 0; i < 800 && logCommand != 0; i++) delay(10);
    if (fileOpen) M5.Lcd.printf("Logging to %s\n", fileName);
    else M5.Lcd.println("Failed to open data file for writing");
  }

  server.on("/", handleIndex);
  server.on("/dl", handleDownload);
  server.begin();

  M5.Speaker.tone(2000, 100);
  delay(1500);
  M5.Lcd.clear();
}

// Status once a second; B held 2 s closes the log so the SD card can be removed (held again: new file).
void loop()
{
  M5.update();
  server.handleClient();

  static bool holdHandled = false;
  if (M5.BtnB.pressedFor(2000)) {
    if (!holdHandled) {
      holdHandled = true;
      if (fileOpen) closeLog(); else if (sdOk) logCommand = 2;
    }
  } else if (!M5.BtnB.isPressed()) {
    holdHandled = false;
  }

  // External power lost (USB / car 5 V): close the file while the internal battery still runs, then power off.
  static uint32_t powerLowSince = 0;
  int vbus = M5.Power.getVBUSVoltage();
  if (vbus >= 0 && vbus < 4000) {
    if (powerLowSince == 0) powerLowSince = millis();
    if (millis() - powerLowSince > POWER_LOST_MS) {
      closeLog();
      M5.Power.powerOff();
    }
  } else {
    powerLowSince = 0;
  }

  // Engine off: the car stopped answering OBD requests.
  if (cfg.canRx >= 0 && obd::everReplied() && millis() - obd::lastReplyMs() > cfg.engineOffS * 1000UL) {
    sleepUntilEngineRuns();
  }

  static uint32_t last = 0, lastReceived = 0;
  if (millis() - last < 1000) return;
  last = millis();
  uint32_t rate = receivedBytes - lastReceived;
  lastReceived = receivedBytes;
  int kmh = obd::speedKmh();

  xSemaphoreTake(spiMutex, portMAX_DELAY);
  M5.Lcd.fillRect(0, 0, 320, 140, BLACK);
  M5.Lcd.setCursor(0, 0);
  M5.Lcd.printf("File: %s %s\n", fileName, fileOpen ? "" : "(CLOSED - SD can be removed)");
  M5.Lcd.printf("Received: %lu KB (%lu B/s)\n", (unsigned long)(receivedBytes / 1024), (unsigned long)rate);
  M5.Lcd.printf("Logged: %lu KB  dropped: %lu B\n", (unsigned long)(loggedBytes / 1024), (unsigned long)droppedBytes);
  M5.Lcd.printf("Frames ok: %lu  damaged: %lu\n", (unsigned long)gnss::goodFrames, (unsigned long)gnss::badFrames);
  M5.Lcd.printf("NTRIP: %s\n", !useNtrip ? "off (log only)" : ntrip_c.connected() ? "connected" : "reconnecting");
  M5.Lcd.printf("GGA: %s\n", latestGGA[0] ? "fix" : "no fix yet");
  if (cfg.canRx >= 0) {
    if (kmh < 0) M5.Lcd.println("OBD speed: no answer");
    else M5.Lcd.printf("OBD speed: %d km/h\n", kmh);
  }
  M5.Lcd.printf("Download: http://%s/\n", WiFi.localIP().toString().c_str());
  M5.Lcd.println("Hold B 2 s: close / reopen log");
  xSemaphoreGive(spiMutex);

  // Same status on USB serial (115200) for checking from a PC.
  Serial.printf("rx %lu B/s total %lu logged %lu dropped %lu frames ok %lu bad %lu file %s %s ntrip %s gga %s speed %d ip %s\n",
                (unsigned long)rate, (unsigned long)receivedBytes, (unsigned long)loggedBytes, (unsigned long)droppedBytes,
                (unsigned long)gnss::goodFrames, (unsigned long)gnss::badFrames, fileName, fileOpen ? "open" : "closed",
                !useNtrip ? "off" : ntrip_c.connected() ? "on" : "down", latestGGA[0] ? "fix" : "none", kmh,
                WiFi.localIP().toString().c_str());
}
