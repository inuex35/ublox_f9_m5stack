#include <M5Core2.h>
#include <WiFi.h>
#include <SD.h>
#include <BluetoothSerial.h>
#include "NTRIPClient.h"
#include <SparkFun_u-blox_GNSS_v3.h> //http://librarymanager/All#SparkFun_u-blox_GNSS_v3
SFE_UBLOX_GNSS_SERIAL myGNSS;
float heading = 0.0;

NTRIPClient ntrip_c;
int selectedStationIndex = 0;
File dataFile;
const char* ntpServer = "ntp.nict.jp";
const long gmtOffset_sec = 3600 * 9;
const int daylightOffset_sec = 0;
BluetoothSerial SerialBT;
uint8_t GNSSData[1024 * 2];
uint16_t dataCount = 0;
uint8_t rtcmData[1024 * 2];
uint16_t rtcmCount = 0;
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

void displayWiFiList(WiFiNetwork* wifiNetworks, int numWiFiNetworks, int selectedIndex) {
  M5.Lcd.clear();
  M5.Lcd.setCursor(0, 0);
  M5.Lcd.println("Select a WiFi network:");
  for (int i = 0; i < numWiFiNetworks; i++) {
    if (i == selectedIndex) {
      M5.Lcd.print("> ");
    } else {
      M5.Lcd.print("  ");
    }
    M5.Lcd.println(wifiNetworks[i].ssid);
  }
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

int selectWiFiFromList(WiFiNetwork* wifiNetworks, int numWiFiNetworks) {
  int selectedIndex = 0;
  displayWiFiList(wifiNetworks, numWiFiNetworks, selectedIndex);

  while (true) {
    M5.update();

    if (M5.BtnA.wasPressed()) {
      selectedIndex = (selectedIndex - 1 + numWiFiNetworks) % numWiFiNetworks;
      displayWiFiList(wifiNetworks, numWiFiNetworks, selectedIndex);
    } else if (M5.BtnC.wasPressed()) {
      selectedIndex = (selectedIndex + 1) % numWiFiNetworks;
      displayWiFiList(wifiNetworks, numWiFiNetworks, selectedIndex);
    } else if (M5.BtnB.wasPressed()) {
      return selectedIndex;
    }
    delay(100);
  }
}

int selectReferenceStation(ReferenceStation* referenceStations, int numStations) {
  int selectedIndex = 0;
  displayStationList(referenceStations, numStations, selectedIndex);

  while (true) {
    M5.update();

    if (M5.BtnA.wasPressed()) {
      selectedIndex = (selectedIndex - 1 + numStations) % numStations;
      displayStationList(referenceStations, numStations, selectedIndex);
    } else if (M5.BtnC.wasPressed()) {
      selectedIndex = (selectedIndex + 1) % numStations;
      displayStationList(referenceStations, numStations, selectedIndex);
    } else if (M5.BtnB.wasPressed()) {
      return selectedIndex;
    }
    delay(100);
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
    } else {
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
    } else {
      M5.Lcd.println("Requesting MountPoint failed");
    }
  }
}

String gnths_to_gnhdt(String gnths_message) {
  int startIndex = gnths_message.indexOf("$GNTHS");
  if (startIndex == -1) {
    return ""; // GNTHSメッセージが見つからない場合は空文字を返す
  }
  
  int endIndex = gnths_message.indexOf('*', startIndex);
  if (endIndex == -1) {
    return ""; // メッセージの終わりが見つからない場合は空文字を返す
  }

  String gnths_body = gnths_message.substring(startIndex + 7, endIndex); // $GNTHS, をスキップ
  int commaIndex = gnths_body.indexOf(',');
  if (commaIndex == -1) {
    return ""; // ヘディングとスピードの区切りが見つからない場合は空文字を返す
  }

  String heading = gnths_body.substring(0, commaIndex);
  String gnhdt_message = "$GNHDT," + heading + ",T";
  
  // チェックサムの計算
  int checksum = 0;
  for (int i = 1; i < gnhdt_message.length(); ++i) {
    checksum ^= gnhdt_message[i];
  }

  char checksumStr[3];
  sprintf(checksumStr, "%02X", checksum);
  gnhdt_message += "*" + String(checksumStr);

  return "\r\n" + gnhdt_message + "\r\n"; // 改行を追加
}

void setup() {
  M5.begin();
  SerialBT.begin("GNSS_BT"); // Bluetooth name
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

  Serial2.begin(230400, SERIAL_8N1, 33, 32);
  while (!Serial2) //Wait for user to open terminal
  if (myGNSS.begin(Serial2) == true) break;

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

  M5.Lcd.println("Setup complete.");
  delay(3000);

}

void loop() {
    uint16_t dataCount = 0;

    while (Serial2.available()) {
      GNSSData[dataCount++] = Serial2.read();
      if (dataCount == sizeof(GNSSData)) break;
    }
    dataFile = SD.open(fileName, FILE_APPEND);
    if (dataFile) {
      M5.Lcd.printf("Writing %d bytes of GNSS data to file\n", dataCount);
      dataFile.write(GNSSData, dataCount);
      dataFile.close();
    }
    M5.Lcd.clear();

    // Process NMEA Data for Bluetooth and Display
    String nmeaString = "";
  
    if (dataCount > 0) {
      String nmeaString = "";
      for (int i = 0; i < dataCount; ++i) {
        nmeaString += (char)GNSSData[i];
      }

      if (nmeaString.indexOf("$GNTHS") != -1) {
        String gnhdtString = gnths_to_gnhdt(nmeaString);
        if (gnhdtString.length() > 0) {
          nmeaString = gnhdtString; // GNHDTメッセージに置き換え

          if (SerialBT.hasClient()) { // Bluetoothクライアントが接続されているか確認
            SerialBT.print(nmeaString);
          }
        }
      }

      if (nmeaString.indexOf("$GNGGA") != -1) {
        if (SerialBT.hasClient()) { // Bluetoothクライアントが接続されているか確認
          SerialBT.print(nmeaString);
        }
        M5.Lcd.clear();
        M5.Lcd.setCursor(0, 0);
        M5.Lcd.print(nmeaString);
      }
    }

    uint16_t rtcmCount = 0;

    while (ntrip_c.available()) {
      rtcmData[rtcmCount++] = ntrip_c.read();
      if (rtcmCount == sizeof(rtcmData)) break;
    }
    if(rtcmCount>0){
      Serial2.write(rtcmData, rtcmCount);
    }
}
