#include <M5Stack.h>
#include <WiFi.h>
#include <SD.h>
#include "NTRIPClient.h"

NTRIPClient ntrip_c;
int selectedStationIndex = 0;
File dataFile;
const char* ntpServer = "ntp.nict.jp";
const long gmtOffset_sec = 3600 * 9;
const int daylightOffset_sec = 0;

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

bool loadConfigFromSD(WiFiNetwork* wifiNetworks, int& numWiFiNetworks, ReferenceStation* referenceStations, int& numStations) {
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

    if (line.startsWith("WIFI:")) {
      int delimiterIndex = line.indexOf(',');
      if (delimiterIndex != -1) {
        wifiNetworks[wifiIndex].ssid = line.substring(5, delimiterIndex);
        wifiNetworks[wifiIndex].password = line.substring(delimiterIndex + 1);
        wifiIndex++;
      }
    } else if (line.startsWith("STATION:")) {
      int delimiterIndex1 = line.indexOf(',');
      int delimiterIndex2 = line.indexOf(',', delimiterIndex1 + 1);
      int delimiterIndex3 = line.indexOf(',', delimiterIndex2 + 1);
      int delimiterIndex4 = line.indexOf(',', delimiterIndex3 + 1);

      if (delimiterIndex1 != -1 && delimiterIndex2 != -1 && delimiterIndex3 != -1 && delimiterIndex4 != -1) {
        referenceStations[stationIndex].name = line.substring(8, delimiterIndex1);
        referenceStations[stationIndex].serverAddress = line.substring(delimiterIndex1 + 1, delimiterIndex2);
        referenceStations[stationIndex].portNumber = line.substring(delimiterIndex2 + 1, delimiterIndex3).toInt();
        referenceStations[stationIndex].mountPoint = line.substring(delimiterIndex3 + 1, delimiterIndex4);
        referenceStations[stationIndex].username = line.substring(delimiterIndex4 + 1);
        stationIndex++;
      }
    }
  }

  configFile.close();

  numWiFiNetworks = wifiIndex;
  numStations = stationIndex;

  return true;
}

int selectReferenceStation(ReferenceStation* referenceStations, int numStations) {
  int selectedIndex = 0;
  bool stationSelected = false;

  displayStationList(referenceStations, numStations, selectedIndex);

  while (!stationSelected) {
    M5.update();

    if (M5.BtnA.wasPressed()) {
      selectedIndex = (selectedIndex - 1 + numStations) % numStations;
      displayStationList(referenceStations, numStations, selectedIndex);
    } else if (M5.BtnC.wasPressed()) {
      selectedIndex = (selectedIndex + 1) % numStations;
      displayStationList(referenceStations, numStations, selectedIndex);
    } else if (M5.BtnB.wasPressed()) {
      stationSelected = true;
    }
    delay(100);
  }
  return selectedIndex;
}

void displayStationList(ReferenceStation* referenceStations, int numStations, int selectedIndex) {
  M5.Lcd.clear();
  M5.Lcd.setCursor(0, 0);
  M5.Lcd.println("Select a reference station:");

  for (int i = 0; i < numStations; i++) {
      if (i == selectedIndex) {
          M5.Lcd.print("> ");
      } else {
          M5.Lcd.print("  ");
      }
      M5.Lcd.println(referenceStations[i].name);
  }
}

void connectToStation(int index, ReferenceStation* referenceStations) {
  const ReferenceStation& station = referenceStations[index];

  M5.Lcd.clear();
  M5.Lcd.setCursor(0, 0);
  M5.Lcd.println("Connecting to:");
  M5.Lcd.println(station.name);
  char* host = const_cast<char*>(station.serverAddress.c_str());
  int port = station.portNumber;
  if (ntrip_c.connect(host, port)) {
    M5.Lcd.println("Connected to NTRIP server");

    M5.Lcd.println("Requesting SourceTable");
    if (ntrip_c.reqSrcTbl(host, port)) {
      char buffer[512];
      delay(5);
      while(ntrip_c.available()){
        ntrip_c.readLine(buffer,sizeof(buffer));
        M5.Lcd.print(buffer);
      }
    }
    else{
      M5.Lcd.println("SourceTable request error");
    }
    M5.Lcd.print("Requesting SourceTable is OK\n");
    ntrip_c.stop();

    M5.Lcd.println("Requesting MountPoint's Raw data");
    char* mntpnt = const_cast<char*>(station.mountPoint.c_str());
    char* user = const_cast<char*>(station.username.c_str());
    char* psw = const_cast<char*>(station.password.c_str());
    if (!ntrip_c.reqRaw(host, port, mntpnt, user, psw)) {
      delay(15000);
      ESP.restart();
    }
    else {
      M5.Lcd.println("Requesting MountPoint failed");
    }
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

void ntripClientTask(void *pvParameters) {
  while (true) {
    uint8_t rtcmData[512 * 2];
    uint16_t rtcmCount = 0;

    while (ntrip_c.available()) {
      rtcmData[rtcmCount++] = ntrip_c.read();
      if (rtcmCount == sizeof(rtcmData)) break;
    }

    Serial2.write(rtcmData, rtcmCount);

    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void gnssDataTask(void *pvParameters) {
  char* fileName = (char*)pvParameters;

  while (true) {
    uint8_t GNSSData[1024 * 2];
    uint16_t dataCount = 0;

    while (Serial2.available()) {
      GNSSData[dataCount++] = Serial2.read();
      if (dataCount == sizeof(GNSSData)) break;
    }

    if (dataCount > 0) {
      M5.Lcd.printf("Trying to write %d bytes of GNSS data to file\n", dataCount);
      M5.Lcd.clear();
      M5.Lcd.setCursor(0, 0);
      dataFile = SD.open(fileName, FILE_APPEND);
      if (dataFile) {
        M5.Lcd.printf("Writing %d bytes of GNSS data to file\n", dataCount);
        dataFile.write(GNSSData, dataCount);
        dataFile.close();
        M5.Lcd.clear();
        M5.Lcd.setCursor(0, 0);
      } else {
        M5.Lcd.println("Failed to open data file for writing");
        M5.Lcd.clear();
        M5.Lcd.setCursor(0, 0);
      }
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void setup() {
  M5.begin();
  WiFiNetwork wifiNetworks[10];
  int numWiFiNetworks = 0;
  ReferenceStation referenceStations[10];
  int numStations = 0;

  if (!loadConfigFromSD(wifiNetworks, numWiFiNetworks, referenceStations, numStations)) {
    M5.Lcd.println("Failed to load config from SD card");
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

  M5.Lcd.println("Requesting MountPoint is OK");
  selectedStationIndex = selectReferenceStation(referenceStations, numStations);
  connectToStation(selectedStationIndex, referenceStations);

  Serial2.begin(230400, SERIAL_8N1, 16, 17);

  M5.Lcd.println("Serial begin.");
  delay(3000);

  M5.Lcd.clear();
  M5.Lcd.setCursor(0, 0);

  if (!SD.begin()) {
    M5.Lcd.println("SD card initialization failed!");
    return;
  }
  M5.Lcd.println("SD card initialized.");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  M5.Lcd.println("Time Adjusted.");

  struct tm timeInfo;
  getLocalTime(&timeInfo);
  sprintf(fileName, "/data_%04d%02d%02d_%02d%02d%02d.ubx",
          timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday,
          timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);

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
  xTaskCreate(ntripClientTask, "NTRIPClient", 4096, NULL, 1, NULL);
  xTaskCreate(gnssDataTask, "GNSSData", 8192, (void*)fileName, 1, NULL);
}

void loop()
{

}