#include "GnssLink.h"
#include <Wire.h>
#include <freertos/message_buffer.h>
#include <freertos/semphr.h>

namespace gnss {

volatile uint32_t goodFrames = 0, badFrames = 0;

namespace {

LinkType linkType = LinkType::I2C;
HardwareSerial& uart = Serial2;
uint32_t baud = 0;

const uint8_t I2C_ADDR = 0x42;
// The receiver stretches the I2C clock; above 100 kHz the ESP32 misreads the first bit after a stretch
// (byte counts of 0x7FFF, damaged frames). 100 kHz carries about 10 KB/s.
const uint32_t I2C_HZ = 100000;
const size_t I2C_CHUNK = 128;          // Wire buffer size
const uint32_t UART_BAUD = 921600;
const size_t UART_RX_BUFFER = 32768;

MessageBufferHandle_t txQueue;
SemaphoreHandle_t txMutex;

// ---- I2C (u-blox DDC) ----

// Bytes waiting in the receiver (registers 0xFD/0xFE). The register address must be followed by a repeated
// start: after a stop the receiver returns stream data instead.
int i2cAvailable() {
  Wire.beginTransmission(I2C_ADDR);
  Wire.write(0xFD);
  if (Wire.endTransmission(false) != 0) return 0;
  if (Wire.requestFrom(I2C_ADDR, (uint8_t)2) != 2) return 0;
  uint16_t n = (uint16_t)Wire.read() << 8;
  n |= Wire.read();
  return n > 0x7000 ? 0 : n;           // 0xFFFF: nothing, 0x7FFF: misread
}

// Reading 0xFE moved the address to 0xFF (the stream), which stays there.
int i2cRead(uint8_t* buf, int len) {
  int got = 0;
  while (got < len) {
    int want = min(len - got, (int)I2C_CHUNK);
    int n = Wire.requestFrom(I2C_ADDR, (uint8_t)want);
    for (int k = 0; k < n; k++) buf[got++] = Wire.read();
    if (n < want) break;
  }
  return got;
}

// A 1-byte write sets the register address, so every write carries at least 2 bytes.
void i2cWrite(const uint8_t* buf, int len) {
  int off = 0;
  while (off < len) {
    int n = min(len - off, (int)I2C_CHUNK);
    if (len - off - n == 1) n--;
    Wire.beginTransmission(I2C_ADDR);
    Wire.write(buf + off, n);
    Wire.endTransmission();
    off += n;
  }
}

void linkWrite(const uint8_t* buf, int len) {
  if (linkType == LinkType::I2C) i2cWrite(buf, len);
  else uart.write(buf, len);
}

// ---- UART baud rate detection ----

// UBX or NMEA seen within ms at the current baud rate?
bool uartTalking(uint32_t ms) {
  uint32_t t = millis();
  uint8_t prev = 0;
  int hits = 0;
  while (millis() - t < ms) {
    while (uart.available()) {
      uint8_t c = uart.read();
      if ((prev == 0xB5 && c == 0x62) || (prev == '$' && c == 'G')) hits++;
      prev = c;
    }
    if (hits >= 3) return true;
    delay(5);
  }
  return false;
}

// ---- CFG-VALSET ----

uint8_t cfgMsg[8 + 4 + 64 * 9];
int cfgLen = 0, cfgKeys = 0;
uint8_t cfgLayers = 0;

void cfgFlush() {
  if (cfgKeys == 0) return;
  int payload = cfgLen - 6;
  cfgMsg[4] = payload & 0xFF;
  cfgMsg[5] = payload >> 8;
  uint8_t a = 0, b = 0;
  for (int i = 2; i < cfgLen; i++) { a += cfgMsg[i]; b += a; }
  cfgMsg[cfgLen++] = a;
  cfgMsg[cfgLen++] = b;
  queue(cfgMsg, cfgLen);
  cfgKeys = 0;
}

void cfgBegin(uint8_t layers) {
  cfgLayers = layers;
  cfgKeys = 0;
}

// The value size is part of the key (bits 28..30): 1 = bit, 2 = 1 byte, 3 = 2, 4 = 4, 5 = 8 bytes.
void cfgAdd(uint32_t key, uint32_t value) {
  if (cfgKeys == 0) {
    const uint8_t head[] = {0xB5, 0x62, 0x06, 0x8A, 0, 0, 0, cfgLayers, 0, 0};
    memcpy(cfgMsg, head, sizeof(head));
    cfgLen = sizeof(head);
  }
  int size = (key >> 28) & 7;
  int bytes = size <= 2 ? 1 : size == 3 ? 2 : size == 4 ? 4 : 8;
  for (int i = 0; i < 4; i++) cfgMsg[cfgLen++] = key >> (8 * i);
  for (int i = 0; i < bytes; i++) cfgMsg[cfgLen++] = i < 4 ? value >> (8 * i) : 0;
  if (++cfgKeys == 32) cfgFlush();
}

// Message output keys (CFG-MSGOUT), generated from the u-blox configuration database.
// Logged: RXM-RAWX, RXM-SFRBX, ESF-MEAS, ESF-STATUS, ESF-INS, NAV-PVT, NAV-PVAT, NMEA GGA.
const uint32_t MSG_LOG_I2C[] = {0x209102A4, 0x20910231, 0x20910277, 0x20910105, 0x20910114, 0x20910006, 0x2091062A, 0x209100BA};
const uint32_t MSG_LOG_UART1[] = {0x209102A5, 0x20910232, 0x20910278, 0x20910106, 0x20910115, 0x20910007, 0x2091062B, 0x209100BB};
const uint32_t MSG_LOG_USB[] = {0x209102A7, 0x20910234, 0x2091027A, 0x20910108, 0x20910117, 0x20910009, 0x2091062D, 0x209100BD};
// Always off: ESF-RAW (ESF-MEAS has the same IMU data), the other NMEA sentences, the extra USB messages.
const uint32_t MSG_OFF_I2C[] = {0x2091029F, 0x209100C9, 0x209100BF, 0x209100C4, 0x209100AB, 0x209100B0, 0x209100E2, 0x209100D8, 0x20910119, 0x2091034F, 0x209101A5, 0x20910196, 0x20910065, 0x20910010, 0x2091008D, 0x20910015, 0x20910345, 0x2091001F};
const uint32_t MSG_OFF_UART1[] = {0x209102A0, 0x209100CA, 0x209100C0, 0x209100C5, 0x209100AC, 0x209100B1, 0x209100E3, 0x209100D9, 0x2091011A, 0x20910350, 0x209101A6, 0x20910197, 0x20910066, 0x20910011, 0x2091008E, 0x20910016, 0x20910346, 0x20910020};
const uint32_t MSG_OFF_USB[] = {0x209102A2, 0x209100CC, 0x209100C2, 0x209100C7, 0x209100AE, 0x209100B3, 0x209100E5, 0x209100DB, 0x2091011C, 0x20910352, 0x209101A8, 0x20910199, 0x20910068, 0x20910013, 0x20910090, 0x20910018, 0x20910348, 0x20910022};

const uint32_t CFG_RATE_MEAS = 0x30210001;
const uint32_t CFG_RATE_NAV = 0x30210002;
const uint32_t CFG_UART1_BAUDRATE = 0x40520001;
const uint32_t CFG_I2CINPROT_UBX = 0x10710001;
const uint32_t CFG_I2CINPROT_RTCM3X = 0x10710004;
const uint32_t CFG_UART1INPROT_UBX = 0x10730001;
const uint32_t CFG_UART1INPROT_RTCM3X = 0x10730004;
const uint32_t CFG_SFODO_USE_SPEED = 0x10070003;
const uint32_t CFG_SFODO_USE_WT_PIN = 0x1007000F;
const uint32_t CFG_SFODO_FREQUENCY = 0x2007000B;
const uint32_t CFG_SFODO_LATENCY = 0x3007000A;
const uint32_t CFG_SFODO_QUANT_ERROR = 0x40070008;

}  // namespace

bool begin(LinkType t, int pinA, int pinB) {
  linkType = t;
  txQueue = xMessageBufferCreate(8192);
  txMutex = xSemaphoreCreateMutex();

  if (t == LinkType::I2C) {
    Wire.begin(pinA, pinB, I2C_HZ);
    Wire.beginTransmission(I2C_ADDR);
    return Wire.endTransmission() == 0;
  }

  uart.setRxBufferSize(UART_RX_BUFFER);        // before begin()
  const uint32_t rates[] = {UART_BAUD, 230400, 115200, 38400, 9600};
  for (uint32_t r : rates) {
    uart.begin(r, SERIAL_8N1, pinA, pinB);
    if (!uartTalking(1500)) { uart.end(); continue; }
    if (r != UART_BAUD) {                       // switch the receiver (and us) to 921600
      cfgBegin(0x03);
      cfgAdd(CFG_UART1_BAUDRATE, UART_BAUD);
      cfgFlush();
      serviceWrites();
      uart.flush();
      delay(100);
      uart.end();
      uart.begin(UART_BAUD, SERIAL_8N1, pinA, pinB);
      if (!uartTalking(1500)) return false;
    }
    baud = UART_BAUD;
    return true;
  }
  return false;
}

LinkType type() { return linkType; }
uint32_t uartBaud() { return baud; }

int read(uint8_t* buf, int maxLen) {
  if (linkType == LinkType::I2C) {
    int n = i2cAvailable();
    return n > 0 ? i2cRead(buf, min(n, maxLen)) : 0;
  }
  int n = uart.available();
  return n > 0 ? uart.readBytes(buf, min(n, maxLen)) : 0;
}

bool queue(const uint8_t* msg, size_t len) {
  xSemaphoreTake(txMutex, portMAX_DELAY);       // a message buffer takes one writer at a time
  bool ok = xMessageBufferSend(txQueue, msg, len, 0) == len;
  xSemaphoreGive(txMutex);
  return ok;
}

void serviceWrites() {
  static uint8_t m[1100];
  size_t n;
  while ((n = xMessageBufferReceive(txQueue, m, sizeof(m), 0)) > 0) linkWrite(m, n);
}

void applyConfig(int rateHz, bool useSpeed) {
  cfgBegin(0x03);                               // RAM + BBR: no flash wear on every start
  cfgAdd(CFG_RATE_MEAS, 1000 / rateHz);
  cfgAdd(CFG_RATE_NAV, 1);
  bool i2c = linkType == LinkType::I2C;
  for (int i = 0; i < 8; i++) {                 // logged messages on the link port only
    cfgAdd(MSG_LOG_I2C[i], i2c ? 1 : 0);
    cfgAdd(MSG_LOG_UART1[i], i2c ? 0 : 1);
    cfgAdd(MSG_LOG_USB[i], 0);                  // an undrained USB port fills the shared TX buffer
  }
  for (int i = 0; i < 18; i++) {
    cfgAdd(MSG_OFF_I2C[i], 0);
    cfgAdd(MSG_OFF_UART1[i], 0);
    cfgAdd(MSG_OFF_USB[i], 0);
  }
  cfgAdd(i2c ? CFG_I2CINPROT_UBX : CFG_UART1INPROT_UBX, 1);
  cfgAdd(i2c ? CFG_I2CINPROT_RTCM3X : CFG_UART1INPROT_RTCM3X, 1);
  if (useSpeed) {                               // vehicle speed from OBD-II as UBX-ESF-MEAS (type 11)
    cfgAdd(CFG_SFODO_USE_SPEED, 1);
    cfgAdd(CFG_SFODO_USE_WT_PIN, 0);
    cfgAdd(CFG_SFODO_FREQUENCY, 10);
    cfgAdd(CFG_SFODO_LATENCY, 50);              // ms, OBD request/response
    cfgAdd(CFG_SFODO_QUANT_ERROR, 277800);      // 1e-6 m/s: OBD speed has 1 km/h steps
  }
  cfgFlush();
}

void checkFrames(const uint8_t* d, int len) {
  static uint8_t f[1100];
  static int pos = 0, need = 0;
  for (int i = 0; i < len; i++) {
    uint8_t c = d[i];
    if (pos == 0) { if (c == 0xB5) f[pos++] = c; continue; }
    if (pos == 1) { if (c == 0x62) f[pos++] = c; else pos = 0; continue; }
    f[pos++] = c;
    if (pos == 6) {
      need = (f[4] | (f[5] << 8)) + 8;
      if (need > (int)sizeof(f)) { badFrames++; pos = 0; }
      continue;
    }
    if (pos > 6 && pos == need) {
      uint8_t a = 0, b = 0;
      for (int k = 2; k < need - 2; k++) { a += f[k]; b += a; }
      if (a == f[need - 2] && b == f[need - 1]) goodFrames++; else badFrames++;
      pos = 0;
    }
  }
}

}  // namespace gnss
