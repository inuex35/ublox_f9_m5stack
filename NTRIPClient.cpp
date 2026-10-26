#include"NTRIPClient.h"
bool NTRIPClient::reqSrcTbl(const char* host,int port)
{
  if(!connect(host,port)){
      Serial.print("Cannot connect to ");
      Serial.println(host);
      return false;
  }
  print(
      "GET / HTTP/1.0\r\n"
      "User-Agent: NTRIPClient for Arduino v1.0\r\n"
      "\r\n"
      );
  unsigned long timeout = millis();
  while (available() == 0) {
     if (millis() - timeout > 5000) {
        Serial.println("Client Timeout !");
        stop();
        return false;
     }
     delay(10);
  }
  char buffer[50];
  readLine(buffer,sizeof(buffer));
  if(strncmp(buffer,"SOURCETABLE 200 OK",18))
  {
    Serial.print(buffer);
    return false;
  }
  return true;

}
bool NTRIPClient::reqRaw(const char* host,int port,const char* mntpnt,const char* user,const char* psw)
{
    if(!connect(host,port))return false;
    String p="GET /";
    Serial.println("Request NTRIP");

    p = p + mntpnt + String(" HTTP/1.0\r\n"
        "User-Agent: NTRIPClient for Arduino v1.0\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n"
    );
    if (strlen(user) > 0) {
        String auth = base64::encode(String(user) + String(":") + psw);
        p = p + String("Authorization: Basic ") + auth + String("\r\n");
    }
    p = p + String("\r\n");
    print(p);
    unsigned long timeout = millis();
    while (available() == 0) {
        if (millis() - timeout > 20000) {
            Serial.println("Client Timeout !");
            stop();
            return false;
        }
        delay(10);
    }
    char buffer[50];
    readLine(buffer,sizeof(buffer));
    // NTRIP v1 casters answer "ICY 200 OK", v2-capable ones may answer "HTTP/1.x 200 OK".
    if(strncmp(buffer,"ICY 200 OK",10) && !(strncmp(buffer,"HTTP/1.",7) == 0 && strstr(buffer," 200 ")))
    {
      Serial.print(buffer);
      stop();
      return false;
    }
    return true;
}
bool NTRIPClient::reqRaw(const char* host,int port,const char* mntpnt)
{
    return reqRaw(host,port,mntpnt,"","");
}
int NTRIPClient::readLine(char* _buffer,int size)
{
  int len = 0;
  while(available() && len < size - 1) {   // keep room for the terminating '\0'
    _buffer[len] = read();
    len++;
    if(_buffer[len-1] == '\n') break;
  }
  _buffer[len]='\0';

  return len;
}
void NTRIPClient::sendGGA(const char* gga)
{
  print(gga);
  print("\r\n");
}
