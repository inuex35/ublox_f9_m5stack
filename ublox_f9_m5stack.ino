#include <M5Stack.h>
#include <WiFi.h>
#include <SD.h>
#include <Wire.h>
#include <WebServer.h>
#include <freertos/stream_buffer.h>
#include "NTRIPClient.h"

NTRIPClient ntrip_c;
int selectedStationIndex = 0;
File dataFile;
const char* ntpServer = "ntp.nict.jp";
const long gmtOffset_sec = 3600 * 9;
const int daylightOffset_sec = 0;

const int MAX_WIFI = 10;
const int MAX_STATIONS = 10;
// Receiver on Qwiic / Grove Port A (I2C, u-blox DDC). The receiver buffers its output and we poll it.
const uint8_t GNSS_I2C_ADDR = 0x42;
const int GNSS_SDA = 21, GNSS_SCL = 22;
const uint32_t GNSS_I2C_HZ = 400000;            // u-blox DDC maximum
const size_t I2C_CHUNK = 128;                   // Wire buffer size in arduino-esp32
const uint32_t SD_FLUSH_INTERVAL_MS = 1000;
const uint32_t GGA_INTERVAL_MS = 10000;
const uint32_t RECONNECT_INTERVAL_MS = 10000;

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

char fileName[40];
ReferenceStation referenceStations[MAX_STATIONS];

// Latest GGA sentence seen in the receiver's output (written by the GNSS task, read by the NTRIP task).
portMUX_TYPE ggaMux = portMUX_INITIALIZER_UNLOCKED;
char latestGGA[100] = "";
volatile uint32_t loggedBytes = 0;
// RTCM from the NTRIP task to the GNSS task, which owns the I2C bus.
StreamBufferHandle_t rtcmBuffer;
WebServer server(80);

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
    } else if (line.startsWith("STATION:") && stationIndex < MAX_STATIONS) {
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
    }
  }

  configFile.close();

  numWiFiNetworks = wifiIndex;
  numStations = stationIndex;

  return true;
}

int selectReferenceStation(ReferenceStation* stations, int numStations) {
  int selectedIndex = 0;
  bool stationSelected = false;

  displayStationList(stations, numStations, selectedIndex);

  while (!stationSelected) {
    M5.update();

    if (M5.BtnA.wasPressed()) {
      selectedIndex = (selectedIndex - 1 + numStations) % numStations;
      displayStationList(stations, numStations, selectedIndex);
    } else if (M5.BtnC.wasPressed()) {
      selectedIndex = (selectedIndex + 1) % numStations;
      displayStationList(stations, numStations, selectedIndex);
    } else if (M5.BtnB.wasPressed()) {
      stationSelected = true;
    }
    delay(100);
  }
  return selectedIndex;
}

void displayStationList(ReferenceStation* stations, int numStations, int selectedIndex) {
  M5.Lcd.clear();
  M5.Lcd.setCursor(0, 0);
  M5.Lcd.println("Select a reference station:");

  for (int i = 0; i < numStations; i++) {
      if (i == selectedIndex) {
          M5.Lcd.print("> ");
      } else {
          M5.Lcd.print("  ");
      }
      M5.Lcd.println(stations[i].name);
  }
}

// Open the correction stream of the selected mountpoint. Returns false on any failure.
bool requestStream(const ReferenceStation& station) {
  return ntrip_c.reqRaw(station.serverAddress.c_str(), station.portNumber, station.mountPoint.c_str(),
                        station.username.c_str(), station.password.c_str());
}

void connectToStation(int index, ReferenceStation* stations) {
  const ReferenceStation& station = stations[index];

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
  if (requestStream(station)) {
    M5.Lcd.println("Requesting MountPoint is OK");
  }
  else {
    M5.Lcd.println("Requesting MountPoint failed, restarting");
    delay(15000);
    ESP.restart();
  }
}

int selectWiFiFromList(WiFiNetwork* wifiNetworks, int numWiFiNetworks) {
  M5.Lcd.clear();
  M5.Lcd.setCursor(0, 0);
  M5.Lcd.println("Scanning WiFi networks...");

  int selectedIndex = 0;
  int scrollOffset = 0;

  displayWiFiList(wifiNetworks, numWiFiNetworks, selectedIndex, scrollOffset);

  while (true) {
    M5.update();
    if (M5.BtnA.wasPressed()) {
      if (selectedIndex > 0) {
        selectedIndex--;
        if (selectedIndex < scrollOffset) {
          scrollOffset--;
        }
        displayWiFiList(wifiNetworks, numWiFiNetworks, selectedIndex, scrollOffset);
      }
    } else if (M5.BtnC.wasPressed()) {
      if (selectedIndex < numWiFiNetworks - 1) {
        selectedIndex++;
        if (selectedIndex >= scrollOffset + 5) {
          scrollOffset++;
        }
        displayWiFiList(wifiNetworks, numWiFiNetworks, selectedIndex, scrollOffset);
      }
    } else if (M5.BtnB.wasPressed()) {
      return selectedIndex;
    }
    delay(100);
  }
}

void displayWiFiList(WiFiNetwork* wifiNetworks, int numWiFiNetworks, int selectedIndex, int scrollOffset) {
  M5.Lcd.clear();
  M5.Lcd.setCursor(0, 0);
  M5.Lcd.println("Select a WiFi network:");
  for (int i = scrollOffset; i < min(numWiFiNetworks, scrollOffset + 5); ++i) {
    if (i == selectedIndex) {
      M5.Lcd.print("> ");
    } else {
      M5.Lcd.print("  ");
    }
    M5.Lcd.println(wifiNetworks[i].ssid);
  }
}

// Corrections: caster -> receiver. Also uploads GGA periodically and reconnects when the caster drops us.
void ntripClientTask(void *pvParameters) {
  uint8_t rtcmData[1024];
  uint32_t lastGGA = 0, lastReconnect = 0;
  while (true) {
    if (!ntrip_c.connected()) {
      if (millis() - lastReconnect > RECONNECT_INTERVAL_MS) {
        lastReconnect = millis();
        if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
        else if (requestStream(referenceStations[selectedStationIndex])) lastGGA = 0;   // send GGA right away
      }
      vTaskDelay(100 / portTICK_PERIOD_MS);
      continue;
    }

    int n = ntrip_c.available();
    if (n > 0) {
      n = ntrip_c.read(rtcmData, min(n, (int)sizeof(rtcmData)));
      if (n > 0) xStreamBufferSend(rtcmBuffer, rtcmData, n, 0);   // dropped if the GNSS task falls behind
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

// Remember the latest complete NMEA GGA sentence from the (mixed UBX/NMEA) receiver stream.
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

// Bytes waiting in the receiver's output buffer (registers 0xFD/0xFE), 0 on error.
uint16_t gnssAvailable() {
  Wire.beginTransmission(GNSS_I2C_ADDR);
  Wire.write(0xFD);
  if (Wire.endTransmission(false) != 0) return 0;
  if (Wire.requestFrom(GNSS_I2C_ADDR, (uint8_t)2) != 2) return 0;
  uint16_t n = (uint16_t)Wire.read() << 8;
  n |= Wire.read();
  return n == 0xFFFF ? 0 : n;
}

// Read up to len bytes of the receiver's data stream (register 0xFF).
int gnssRead(uint8_t* buf, int len) {
  int got = 0;
  while (got < len) {
    int want = min(len - got, (int)I2C_CHUNK);
    Wire.beginTransmission(GNSS_I2C_ADDR);
    Wire.write(0xFF);
    if (Wire.endTransmission(false) != 0) break;
    int n = Wire.requestFrom(GNSS_I2C_ADDR, (uint8_t)want);
    for (int k = 0; k < n; k++) buf[got++] = Wire.read();
    if (n < want) break;
  }
  return got;
}

// Write RTCM to the receiver. u-blox treats a 1-byte write as a register address, so writes are >= 2 bytes.
void gnssWrite(const uint8_t* buf, int len) {
  for (int off = 0; off < len; off += I2C_CHUNK) {
    Wire.beginTransmission(GNSS_I2C_ADDR);
    Wire.write(buf + off, min(len - off, (int)I2C_CHUNK));
    Wire.endTransmission();
  }
}

// Receiver <-> I2C: forward RTCM, read its output, log it to the SD card (file kept open, flushed once a second).
void gnssDataTask(void *pvParameters) {
  static uint8_t GNSSData[4096];
  static uint8_t rtcm[512];
  size_t rtcmCarry = 0;             // a lone byte kept back until more RTCM arrives
  uint32_t lastFlush = millis();
  while (true) {
    size_t r = xStreamBufferReceive(rtcmBuffer, rtcm + rtcmCarry, sizeof(rtcm) - rtcmCarry, 0) + rtcmCarry;
    if (r >= 2) { gnssWrite(rtcm, r); rtcmCarry = 0; } else rtcmCarry = r;

    int n = gnssAvailable();
    if (n > 0) {
      n = gnssRead(GNSSData, min(n, (int)sizeof(GNSSData)));
      scanForGGA(GNSSData, n);
      if (dataFile) {
        dataFile.write(GNSSData, n);
        loggedBytes += n;
      }
    }
    if (dataFile && millis() - lastFlush > SD_FLUSH_INTERVAL_MS) {
      dataFile.flush();
      lastFlush = millis();
    }
    if (n <= 0) vTaskDelay(5 / portTICK_PERIOD_MS);
  }
}

// ---- Download over WiFi: http://<M5Stack IP>/ lists the .ubx files on the SD card ----
void handleIndex() {
  String html = "<html><head><meta name=viewport content='width=device-width'></head><body><h3>GNSS logs</h3><ul>";
  File root = SD.open("/");
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    String name = f.name();
    if (!f.isDirectory() && name.endsWith(".ubx")) {
      if (!name.startsWith("/")) name = "/" + name;
      html += "<li><a href='/dl?f=" + name + "'>" + name.substring(1) + "</a> (" + String((unsigned long)f.size() / 1024) + " KB)";
      if (name == String(fileName)) html += " <b>recording</b>";
      html += "</li>";
    }
    f.close();
  }
  root.close();
  html += "</ul></body></html>";
  server.send(200, "text/html", html);
}

void handleDownload() {
  String name = server.arg("f");
  if (!name.startsWith("/") || name.indexOf("..") >= 0 || !name.endsWith(".ubx")) { server.send(400, "text/plain", "bad name"); return; }
  if (name == String(fileName) && dataFile) dataFile.flush();     // include everything logged so far
  File f = SD.open(name, FILE_READ);
  if (!f) { server.send(404, "text/plain", "not found"); return; }
  server.sendHeader("Content-Disposition", "attachment; filename=" + name.substring(1));
  server.streamFile(f, "application/octet-stream");
  f.close();
}

void setup() {
  M5.begin();
  WiFiNetwork wifiNetworks[MAX_WIFI];
  int numWiFiNetworks = 0;
  int numStations = 0;

  if (!loadConfigFromSD(wifiNetworks, numWiFiNetworks, referenceStations, numStations)) {
    M5.Lcd.println("Failed to load config from SD card");
    return;
  }
  if (numWiFiNetworks == 0 || numStations == 0) {
    M5.Lcd.println("config.txt needs at least one WIFI: and one STATION: line");
    return;
  }

  bool connected = false;
  while (!connected) {
    int selectedWiFiIndex = selectWiFiFromList(wifiNetworks, numWiFiNetworks);
    if (selectedWiFiIndex != -1) {
      M5.Lcd.print("Connecting to ");
      M5.Lcd.println(wifiNetworks[selectedWiFiIndex].ssid);
      WiFi.begin(wifiNetworks[selectedWiFiIndex].ssid.c_str(), wifiNetworks[selectedWiFiIndex].password.c_str());
      unsigned long startTime = millis();
      while (WiFi.status() != WL_CONNECTED) {
        if (millis() - startTime > 10000) {
          M5.Lcd.println("\nFailed to connect. Rescanning...");
          delay(3000);
          M5.Lcd.clear();
          M5.Lcd.setCursor(0, 0);
          break;
        }
        delay(500);
        M5.Lcd.print(".");
      }
      if (WiFi.status() == WL_CONNECTED) {
        connected = true;
        M5.Lcd.println("\nConnected!");
        delay(3000);
        M5.Lcd.clear();
        M5.Lcd.setCursor(0, 0);
      } else {
        M5.Lcd.println("Not connected...");
        delay(5000);
        M5.Lcd.clear();
        M5.Lcd.setCursor(0, 0);
      }
    } else {
      M5.Lcd.println("No WiFi network selected. Rescanning...");
      delay(3000);
      M5.Lcd.clear();
      M5.Lcd.setCursor(0, 0);
    }
  }

  selectedStationIndex = selectReferenceStation(referenceStations, numStations);
  connectToStation(selectedStationIndex, referenceStations);

  Wire.begin(GNSS_SDA, GNSS_SCL, GNSS_I2C_HZ);
  rtcmBuffer = xStreamBufferCreate(8192, 1);

  M5.Lcd.println("I2C begin.");
  delay(3000);

  M5.Lcd.clear();
  M5.Lcd.setCursor(0, 0);

  if (!SD.begin()) {
    M5.Lcd.println("SD card initialization failed!");
    return;
  }
  M5.Lcd.println("SD card initialized.");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  struct tm timeInfo;
  if (getLocalTime(&timeInfo, 10000)) {        // wait up to 10 s for NTP, otherwise the name says 1970
    M5.Lcd.println("Time Adjusted.");
    sprintf(fileName, "/data_%04d%02d%02d_%02d%02d%02d.ubx",
            timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday,
            timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);
  } else {
    M5.Lcd.println("NTP failed, using a random file name");
    sprintf(fileName, "/data_nontp_%lu.ubx", (unsigned long)esp_random());
  }

  dataFile = SD.open(fileName, FILE_APPEND);
  if (dataFile) {
    M5.Lcd.printf("Opened %s for writing\n", fileName);
  } else {
    M5.Lcd.println("Failed to open data file for writing");
  }
  delay(3000);
  M5.Speaker.tone(100, 100);
  delay(100);
  M5.Speaker.mute();
  xTaskCreate(ntripClientTask, "NTRIPClient", 6144, NULL, 1, NULL);
  xTaskCreate(gnssDataTask, "GNSSData", 6144, NULL, 2, NULL);
  server.on("/", handleIndex);
  server.on("/dl", handleDownload);
  server.begin();
}

// Status screen once a second instead of clearing the LCD on every write.
void loop()
{
  server.handleClient();
  static uint32_t last = 0;
  if (millis() - last < 1000) return;
  last = millis();
  M5.Lcd.fillRect(0, 0, 320, 100, BLACK);
  M5.Lcd.setCursor(0, 0);
  M5.Lcd.printf("File: %s\n", fileName);
  M5.Lcd.printf("Logged: %lu bytes\n", (unsigned long)loggedBytes);
  M5.Lcd.printf("NTRIP: %s\n", ntrip_c.connected() ? "connected" : "reconnecting");
  M5.Lcd.printf("GGA: %s\n", latestGGA[0] ? "yes" : "none yet");
  M5.Lcd.printf("Download: http://%s/\n", WiFi.localIP().toString().c_str());
}
