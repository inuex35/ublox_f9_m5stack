#ifndef OBD_SPEED_H
#define OBD_SPEED_H

#include <Arduino.h>

// Vehicle speed from the OBD-II port over CAN (ISO 15765-4, 11-bit IDs, 500 kbit/s; PID 0x0D),
// forwarded to the receiver as UBX-ESF-MEAS speed (data type 11) at 10 Hz.
// Needs a CAN transceiver (e.g. M5Stack CAN Unit) between the ESP32 pins and OBD pins 6 (CAN-H) / 14 (CAN-L).
namespace obd {

// Start polling. rxPin: from the transceiver's RX/R pin, txPin: to its TX/D pin.
bool begin(int rxPin, int txPin);

// One-shot check used while the logger sleeps: does the car answer within ms? Leaves the CAN driver stopped.
bool probe(int rxPin, int txPin, uint32_t ms);

bool everReplied();          // an answer was seen since begin()
uint32_t lastReplyMs();      // millis() of the last answer
int speedKmh();              // last speed, -1 before the first answer

}  // namespace obd

#endif
