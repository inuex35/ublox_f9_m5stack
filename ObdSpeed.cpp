#include "ObdSpeed.h"
#include "GnssLink.h"
#include <driver/twai.h>

namespace obd {

namespace {

volatile bool replied = false;
volatile uint32_t lastReply = 0;
volatile int speed = -1;

const uint32_t OBD_REQUEST_ID = 0x7DF;          // functional request to all ECUs
const uint32_t POLL_MS = 100;                   // 10 Hz, matches CFG-SFODO-FREQUENCY

bool startDriver(int rxPin, int txPin) {
  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)txPin, (gpio_num_t)rxPin, TWAI_MODE_NORMAL);
  g.rx_queue_len = 32;
  twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  if (twai_driver_install(&g, &t, &f) != ESP_OK) return false;
  return twai_start() == ESP_OK;
}

void stopDriver() {
  twai_stop();
  twai_driver_uninstall();
}

void sendRequest() {
  twai_message_t m = {};
  m.identifier = OBD_REQUEST_ID;
  m.data_length_code = 8;
  const uint8_t req[8] = {0x02, 0x01, 0x0D, 0x55, 0x55, 0x55, 0x55, 0x55};   // mode 01, PID 0D (speed)
  memcpy(m.data, req, 8);
  twai_transmit(&m, 0);
}

// Speed answer from an ECU (0x7E8..0x7EF): 03 41 0D <km/h>
int parseSpeed(const twai_message_t& m) {
  if (m.extd || m.identifier < 0x7E8 || m.identifier > 0x7EF || m.data_length_code < 4) return -1;
  if (m.data[1] != 0x41 || m.data[2] != 0x0D) return -1;
  return m.data[3];
}

// While the engine is off nobody acknowledges our frames and the controller goes bus-off; recover it.
void keepBusAlive() {
  twai_status_info_t s;
  if (twai_get_status_info(&s) != ESP_OK) return;
  if (s.state == TWAI_STATE_BUS_OFF) twai_initiate_recovery();
  else if (s.state == TWAI_STATE_STOPPED) twai_start();
}

void sendSpeedToReceiver(int kmh) {
  int32_t mmPerS = (int32_t)(kmh * 1000.0f / 3.6f + 0.5f);
  uint32_t data = ((uint32_t)mmPerS & 0xFFFFFF) | (11u << 24);   // data type 11: speed, 1e-3 m/s
  uint32_t tag = millis();
  uint8_t m[8 + 12] = {0xB5, 0x62, 0x10, 0x02, 12, 0};
  uint8_t* p = m + 6;
  for (int i = 0; i < 4; i++) *p++ = tag >> (8 * i);        // timeTag (ms)
  uint16_t flags = 1 << 11;                                   // numMeas = 1
  *p++ = flags; *p++ = flags >> 8;
  *p++ = 0; *p++ = 0;                                         // id
  for (int i = 0; i < 4; i++) *p++ = data >> (8 * i);
  uint8_t a = 0, b = 0;
  for (uint8_t* q = m + 2; q < p; q++) { a += *q; b += a; }
  *p++ = a; *p++ = b;
  gnss::queue(m, p - m);
}

void obdTask(void*) {
  uint32_t next = millis();
  while (true) {
    keepBusAlive();
    sendRequest();
    bool got = false;
    twai_message_t m;
    while ((int32_t)(next + POLL_MS - millis()) > 0) {
      if (twai_receive(&m, pdMS_TO_TICKS(10)) != ESP_OK) continue;
      int kmh = parseSpeed(m);
      if (kmh >= 0 && !got) {                   // several ECUs may answer; take the first
        got = true;
        speed = kmh;
        lastReply = millis();
        replied = true;
        sendSpeedToReceiver(kmh);
      }
    }
    next += POLL_MS;
    if ((int32_t)(millis() - next) > 1000) next = millis();
  }
}

}  // namespace

bool begin(int rxPin, int txPin) {
  if (!startDriver(rxPin, txPin)) return false;
  xTaskCreatePinnedToCore(obdTask, "OBD", 4096, NULL, 2, NULL, 1);
  return true;
}

bool probe(int rxPin, int txPin, uint32_t ms) {
  if (!startDriver(rxPin, txPin)) return false;
  bool ok = false;
  uint32_t t = millis(), lastReq = 0;
  while (!ok && millis() - t < ms) {
    if (millis() - lastReq > 100) { keepBusAlive(); sendRequest(); lastReq = millis(); }
    twai_message_t m;
    if (twai_receive(&m, pdMS_TO_TICKS(10)) == ESP_OK && parseSpeed(m) >= 0) ok = true;
  }
  stopDriver();
  return ok;
}

bool everReplied() { return replied; }
uint32_t lastReplyMs() { return lastReply; }
int speedKmh() { return speed; }

}  // namespace obd
