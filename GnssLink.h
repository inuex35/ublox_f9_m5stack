#ifndef GNSS_LINK_H
#define GNSS_LINK_H

#include <Arduino.h>

// Link to the u-blox receiver: I2C (Qwiic / Grove Port A) or UART (UART1 of the receiver).
enum class LinkType { I2C, UART };

namespace gnss {

// I2C: pinA = SDA, pinB = SCL. UART: pinA = RX, pinB = TX (the baud rate is detected and set to 921600).
bool begin(LinkType type, int pinA, int pinB);
LinkType type();
uint32_t uartBaud();

// Receiver -> host. Call only from the GNSS task.
int read(uint8_t* buf, int maxLen);

// Host -> receiver: one complete message (RTCM frame, UBX frame). Safe from any task; sent by serviceWrites().
bool queue(const uint8_t* msg, size_t len);
// Send queued messages. Call only from the GNSS task.
void serviceWrites();

// Queue the logger's receiver configuration (RAM + battery-backed RAM, not flash):
// measurement rate, the logged messages on the link port only, speed input from the host when useSpeed.
void applyConfig(int rateHz, bool useSpeed);

// UBX frames with a wrong checksum seen in the receiver stream (data damaged or lost on the link).
void checkFrames(const uint8_t* d, int len);
extern volatile uint32_t goodFrames, badFrames;

}  // namespace gnss

#endif
