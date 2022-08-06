#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <FS.h>

Adafruit_BME280 bme;

ESP8266WebServer server(80);             // webserver a port 80-on

File fsUploadFile;                       // feltöltött fájlnak ideiglenes változója

ESP8266WiFiMulti wifiMulti;              // több wifihez tudjon kapcsolódni

const char *OTAName = "temalab";         // OTA működéshez
const char *OTAPassword = "asdf";

const char* mdnsName = "temalab";        // domain név

WiFiUDP UDP;                             // wifi UDP kapcsolat, ezen keresztül kérjük le az időt
IPAddress timeServerIP;                  // time.nist.gov NTP server IP címe
const char* ntpServerName = "time.nist.gov";

const int NTP_PACKET_SIZE = 48;          // 48 byte méretü csomagban jön az idő

byte packetBuffer[NTP_PACKET_SIZE];      // bejövö idő csomag buffere

//setup:

void setup() 
{
  Serial.begin(115200);        
  delay(10);
  Serial.println("\r\n");

  bme.begin(0x76);   
  
  startWiFi();                 
  
  startOTA();                  
  
  startSPIFFS();               

  startMDNS();                 

  startServer();               

  startUDP();                  
  
  WiFi.hostByName(ntpServerName, timeServerIP); // NTP server IP címét leklrjük
  Serial.print("Time server IP:\t");
  Serial.println(timeServerIP);

  sendNTPpacket(timeServerIP);
  delay(500);
}

//loop:

const unsigned long intervalNTP = 3600000UL; // egy óra
unsigned long prevNTP = 0;
unsigned long lastNTPResponse = millis();
float temperature;

const unsigned long intervalTemp = 60000;   // percenként kérje le az időt
unsigned long prevTemp = 0;
bool tmpRequested = false;
const unsigned long DS_delay = 750;         

uint32_t recentTime = 0;                      // legújabb időbélyeg

void loop() 
{
  unsigned long currentMillis = millis();

  if (currentMillis - prevNTP > intervalNTP)  // ha eltelt egy óra, lekéri az időt
  { 
    prevNTP = currentMillis;
    sendNTPpacket(timeServerIP);
  }

  uint32_t time = getTime();                  // az NTP serverről érkezett időt berakja, ha nem jött üzenet, akkor az előzőt hagyja
  if (time) 
  {
    recentTime = time;
    Serial.print("NTP response:\t");
    Serial.println(recentTime);
    lastNTPResponse = millis();
  } 
  else if ((millis() - lastNTPResponse) > 24UL * 3600000UL) 
  {
    Serial.println("More than 24 hours since last NTP response. Rebooting.");
    Serial.flush();
    ESP.reset();
  }

  if (recentTime != 0) 
  {
    if (currentMillis - prevTemp > intervalTemp) 
    {  
      tmpRequested = true;
      prevTemp = currentMillis;
      Serial.println("Temperature requested");
    }
   
    if (currentMillis - prevTemp > DS_delay && tmpRequested) 
    { 
      uint32_t actualTime = recentTime + (currentMillis - lastNTPResponse) / 1000;
      tmpRequested = false;
      float temp = bme.readTemperature();
      temp = round(temp * 100.0) / 100.0; 

      Serial.printf("Appending temperature to file: %lu,", actualTime);
      Serial.println(temp);
      File tempLog = SPIFFS.open("/temp.csv", "a"); // temp.csv fájlba írja a mért értéket
      tempLog.print(actualTime);
      tempLog.print(',');
      tempLog.println(temp);
      tempLog.close();
    }
  } 
  else    // ha nincs NTP válasz, küld még egy kérést
  {                                    
    sendNTPpacket(timeServerIP);
    delay(500);
  }

  server.handleClient();  
  //MDNS.update();                    
  ArduinoOTA.handle();                        
}


void startWiFi() 
{ 
  wifiMulti.addAP("Bocsa2", "Bodza1968");   
  
  Serial.println("Connecting");
  while (wifiMulti.run() != WL_CONNECTED) 
  {  
    delay(250);
    Serial.print('.');
  }
  Serial.println("\r\n");
  Serial.print("Connected to ");
  Serial.println(WiFi.SSID());             
  Serial.print("IP address:\t");
  Serial.print(WiFi.localIP());            
  Serial.println("\r\n");
            
}

void startUDP() 
{
  Serial.println("Starting UDP");
  UDP.begin(123);                          // 123-as porton indul el
  Serial.print("Local port:\t");
  Serial.println(UDP.localPort());
}

void startOTA() 
{ 
  ArduinoOTA.setHostname(OTAName);
  ArduinoOTA.setPassword(OTAPassword);

  ArduinoOTA.onStart([]() 
  {
    Serial.println("OTA started");
  });
  
  ArduinoOTA.onEnd([]() 
  {
    Serial.println("\r\nEnd");
  });
  
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) 
  {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  
  ArduinoOTA.onError([](ota_error_t error) 
  {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();
  Serial.println("OTA ready\r\n");
}

void startSPIFFS() 
{ 
  SPIFFS.begin();                             
  Serial.println("SPIFFS started. Contents:");
  {
    Dir dir = SPIFFS.openDir("/");
    while (dir.next()) {                      // az összes fájlon végigmegy a szerveren
      String fileName = dir.fileName();
      size_t fileSize = dir.fileSize();
      Serial.printf("\tFS File: %s, size: %s\r\n", fileName.c_str(), formatBytes(fileSize).c_str());
    }
    Serial.printf("\n");
  }
}

void startMDNS() { 
  if (MDNS.begin(mdnsName))
  {
    Serial.println("mDNS responder started");
    MDNS.addService("http", "tcp", 80);
    Serial.print(F("Open http://"));
    Serial.print(mdnsName);
  }
  else
  {
    Serial.println("mDNS nem akar müködni"); 
  }
  /*MDNS.begin(mdnsName);  
  MDNS.addService("http", "tcp", 80);                      
  Serial.print("mDNS responder started: http://");
  Serial.print(mdnsName);
  Serial.println(".local");*/
}

void startServer() 
{ 
  server.on("/edit.html",  HTTP_POST, []() 
  {  
    server.send(200, "text/plain", "");
  }, handleFileUpload);                       
  
  server.onNotFound(handleNotFound);          
  
  server.begin();                             
  Serial.println("HTTP server started.");
}

void handleNotFound() 
{ 
  if (!handleFileRead(server.uri())) 
  {        
    server.send(404, "text/plain", "404: File Not Found");
  }
}

bool handleFileRead(String path)                         //kért file elküldése
{                      
  Serial.println("handleFileRead: " + path);
  if (path.endsWith("/")) path += "index.html";          // mappa kérés esetén is index.html-t nyissa meg
  String contentType = getContentType(path);             // típusát lekérdezi
  String pathCompressed = path + ".gz";
  
  if (SPIFFS.exists(pathCompressed) || SPIFFS.exists(path))    // létezik-e a fájl tömörítve vagy nem tömörítve 
  {  
    if (SPIFFS.exists(pathCompressed))                         // ha van tömörített
      path += ".gz";                                           // használja a tömörítettet
       
    File file = SPIFFS.open(path, "r");                    
    size_t sending = server.streamFile(file, contentType);     // küldi a kliensnek
    file.close(); 
                                             
    Serial.println(String("\tSent file: ") + path);
    return true;
  }
  Serial.println(String("\tFile Not Found: ") + path);  
  return false;
}

void handleFileUpload()                                  //fájl feltöltése
{
  HTTPUpload& upload = server.upload();
  String path;
  if (upload.status == UPLOAD_FILE_START) 
  {
    path = upload.filename;
    
    if (!path.startsWith("/")) 
      path = "/" + path;
      
    if (!path.endsWith(".gz")) 
    {                         
      String pathGzFile = path + ".gz";                  // ha a feltöltött fájl nem tömörített, akkor az addigi tömörített változatát töröljük
      if (SPIFFS.exists(pathGzFile))                     
        SPIFFS.remove(pathGzFile);
    }
    Serial.print("handleFileUpload Name: "); 
    Serial.println(path);
    fsUploadFile = SPIFFS.open(path, "w");               // fájl megnyitása, hogy spiffsben tudjuk használni
    path = String();
  } 
  else if (upload.status == UPLOAD_FILE_WRITE) 
  {
    if (fsUploadFile)
      fsUploadFile.write(upload.buf, upload.currentSize); //méret beírása
  } 
  else if (upload.status == UPLOAD_FILE_END)
  {
    if (fsUploadFile)                                     // ha a fájl létrejött
    {                                   
      fsUploadFile.close();                               // bezárjuk
      Serial.print("handleFileUpload Size: "); 
      Serial.println(upload.totalSize);
      server.sendHeader("Location", "/success.html");     
      server.send(303);
    } 
    else 
    {
      server.send(500, "text/plain", "500: couldn't create file");
    }
  }
}


String formatBytes(size_t bytes) // Byte átváltás
{ 
  if (bytes < 1024) 
  {
    return String(bytes) + " B";
  } 
  else if (bytes < (1024 * 1024)) 
  {
    return String(bytes / 1024.0) + " KB";
  } 
  else if (bytes < (1024 * 1024 * 1024)) 
  {
    return String(bytes / 1024.0 / 1024.0) + " MB";
  }
}

String getContentType(String filename) // file típus meghatározása
{      
  if (filename.endsWith(".html")) return "text/html";
  else if (filename.endsWith(".css")) return "text/css";
  else if (filename.endsWith(".js")) return "application/javascript";
  else if (filename.endsWith(".ico")) return "image/x-icon";
  else if (filename.endsWith(".gz")) return "application/x-gzip";
  return "text/plain";
}

unsigned long getTime() 
{ 
  if (UDP.parsePacket() == 0)               // ha nincsen válasz
  { 
    return 0;
  }
  UDP.read(packetBuffer, NTP_PACKET_SIZE);  // NTP válasz beolvasása a bufferbe
  
  // számot csinálunk az időbélyegekből
  uint32_t NTPTime = (packetBuffer[40] << 24) | (packetBuffer[41] << 16) | (packetBuffer[42] << 8) | packetBuffer[43];
  // Amit kapunk, az az 1900.1.1. 0:00-tól számított idő, másodpercben 
  
  // Unix time viszont 1970.1.1-től számolódik. Elvileg 2208988800mp:
  const uint32_t seventyYears = 2208988800UL;
  
  // kivonjuk egymásból hogy a jó időt küldhessük
  uint32_t UNIXTime = NTPTime - seventyYears;
  return UNIXTime;
}


void sendNTPpacket(IPAddress& address) 
{
  Serial.println("Sending NTP request");
  memset(packetBuffer, 0, NTP_PACKET_SIZE);   
  packetBuffer[0] = 0b11100011;               // nullázza a buffert

  UDP.beginPacket(address, 123);              // NTP kérés port 123-hoz
  UDP.write(packetBuffer, NTP_PACKET_SIZE);   //elküldi a kérést
  UDP.endPacket();
}
