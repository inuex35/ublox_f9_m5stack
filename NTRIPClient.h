#ifndef NTRIP_CLIENT
#define NTRIP_CLIENT

#include <WiFiClient.h>
#include <Arduino.h>
#include<base64.h>

class NTRIPClient : public WiFiClient{
  public :
  bool reqSrcTbl(const char* host,int port);   //request MountPoints List serviced the NTRIP Caster
  bool reqRaw(const char* host,int port,const char* mntpnt,const char* user,const char* psw);      //request RAW data from Caster
  bool reqRaw(const char* host,int port,const char* mntpnt); //non user
  int readLine(char* buffer,int size);
  // Send the receiver's position (one NMEA GGA sentence, without line end) to the caster.
  // VRS / nearest-base mountpoints only start or keep sending corrections after this.
  void sendGGA(const char* gga);
};

#endif
