#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Adafruit_Sensor.h>
// #include <avr/wdt.h>
#include "Adafruit_SHT31.h"
#include "RTClib.h"
#include "WiFiS3.h"
#include <WDT.h>

// #include "arduino_secrets.h"

// SECRET_SSID
// SECRET_PASS
#include "arduino_secrets.h"
#define SEALEVELPRESSURE_HPA (1013.25)

// Declare global variables

// Initialize all greenhouse variables

// These #defines make it easy to set the backlight color
#define RED 0x1
#define YELLOW 0x3
#define GREEN 0x2
#define TEAL 0x6
#define BLUE 0x4
#define VIOLET 0x5
#define WHITE 0x7

#if 0
# define DBG_SERIAL_PRINTLN(x) Serial.println(x)
# define DBG_SERIAL_PRINT(x) Serial.print(x)
#else
# define DBG_SERIAL_PRINTLN(x) (void)(x)
# define DBG_SERIAL_PRINT(x) (void)(x)
#endif

// States for Greenhouse state machine
#define RECIRC 1
#define VENT   2
#define COOL   3
#define PADDRY 4
#define DEHUM  5

RTC_PCF8523 rtc;

const int chipSelect = 10;

DateTime lastPress;    // The time of the last button press  
uint32_t  padTime;    // The time we started drying the pads
int shtInside = 0x44;
int shtOutside = 0x45;
const long wdtInterval = 5592;
int ghState = RECIRC;

int curr_temp = 0;
int outsideTemp = 0;

int curr_humid = 0;
int outsideHumidity = 0;
int sensor_fail = 0;
int sensor2_fail = 0;
int sns_fail_cnt = 0;
int sns2_fail_cnt = 0;

int pump_on = 0;         // Pump is off
int hyst_temp = 2;   // 2 degrees C of hysterysis
int hyst_hum = 3;    // 3 % RH
int set_temp = 27;  // in degrees C; about 80 F
int set_hum = 70;       // Initial Humidity setpoint
int shutter_vent = 0;   // Single shutter starts out closed
int shutter_swamp = 0;  // Double shutter starts out closed
int fan_state = 0;      // Fan starts out off
bool lcdoff = false;
Adafruit_SHT31 sht31 = Adafruit_SHT31();
Adafruit_SHT31 sht31Out = Adafruit_SHT31();
int status = WL_IDLE_STATUS;
WiFiServer server(80);

struct netmessage {
  int16_t temp_inside;
  int16_t temp_outside;
  int16_t temp_setpoint;
  int16_t humid_inside;
  int16_t humid_outside;
  int16_t humid_setpoint;
  // 0: Pump on
  // 1: Single shutter
  // 2: Double shutter
  // 3: Fan
  int8_t  mechanism_state;
  // RECIRC, VENT, etc.
  int8_t  operating_state;
  int32_t paddry_time_left;
};

void setup() {
  char buf2[] = "YYYY-MM-DDThh:mm:ss";
  
  // put your setup code here, to run once:

  //Initialize serial:
  Serial.begin(9600);
  while (!Serial) {
    ; // wait for serial port to connect. Needed for native USB port only
  }

  // if(WDT.begin(wdtInterval)) {
  //   Serial.print("WDT interval: ");
  //  WDT.refresh();
  //  Serial.print(WDT.getTimeout());
  //  WDT.refresh();
  //  Serial.println(" ms");
  //  WDT.refresh();
 // } else {
  //  Serial.println("Error initializing watchdog");
  //  while(1){}
  //}

  if (WiFi.status() == WL_NO_MODULE) {
    Serial.println("Communication with WiFi module failed!");
    // have to  continue
    // while (true);
  } else {
    Serial.println("Communication with WiFi module succeeded");
  }
  String fv = WiFi.firmwareVersion();
  if (fv < WIFI_FIRMWARE_LATEST_VERSION) {
    Serial.println("Please upgrade the firmware");
  } else {
    Serial.println("Wifi firmware is current");
  }

  // attempt to connect to WiFi network:
  while (status != WL_CONNECTED) {
    Serial.print("Attempting to connect to WPA SSID: ");
    Serial.println(SECRET_SSID);
    // Connect to WPA/WPA2 network:
    WiFi.config(IPAddress(192,168,0,156));
    status = WiFi.begin(SECRET_SSID, SECRET_PASS);

    // wait 3 seconds for connection:
    delay(3000);
  }
  server.begin();                           // start the web server on port 80
  printWifiStatus();                        // you're connected now, so print out the status

  // Setup watchdog timer
  // wdt_enable(WDTO_8S);
  
  // Setup the temp humidity sensor
  if (! sht31.begin()) {   // Set to 0x45 for alternate i2c addr
    Serial.println(F("Couldn't find SHT31"));
    // System has to run even if temp sensor fails...
    sensor_fail = 1;
    sns_fail_cnt++;
    // abort();
  }
  // Setup the outside temp humidity sensor
  if (! sht31Out.begin(0x45)) {   // Set to 0x45 for alternate i2c addr
    Serial.println(F("Couldn't find Outside SHT31"));
    sensor2_fail = 1;
    sns2_fail_cnt++;
  }
  if (! rtc.begin()) {
    Serial.println(F("Couldn't find RTC"));
    Serial.flush();
    // abort();
  }
  DateTime now = rtc.now();
  lastPress = now;
  padTime = 0;
  
  // see if the card is present and can be initialized:
  if (!SD.begin(chipSelect)) {
    Serial.println("Card failed, or not present");
    // don't do anything more:
    // abort();
  }
  // Serial.println("card initialized.");

   
  File dataFile = SD.open("statechg.txt", FILE_WRITE);
  now.toString(buf2);
  if (dataFile) {
    dataFile.print(buf2);
    dataFile.println(F(" Setup statechange file"));
    dataFile.close();
  }
    
  // Setup the Analog ports
  pinMode(5, OUTPUT);           // D0 is fan
  pinMode(6, OUTPUT);            // D1 is pump
  pinMode(2, OUTPUT);            // D2 is swamp cooler
  pinMode(3, OUTPUT);            // D3 is vent
  pinMode(4, OUTPUT);           // D4 is dehumidify
  
}

void loop() {
  // put your main code here, to run repeatedly:
  // Grab temperature and humidity.
  DateTime now = rtc.now();

  // Now check temperature and act on fans, shutters...
  // curr_temp = bme_read_temp(0);
  // curr_temp = 26;
  float t = sht31.readTemperature();
  if (isnan(t)) {  // check if 'is not a number' 
    Serial.println(F("Failed to read temperature"));
    sensor_fail = 1;
    sns_fail_cnt++;

    // Try to setup again!
    sht31.begin();
  } else
    sensor_fail = 0;

  float tOut = sht31Out.readTemperature();
  if (isnan(tOut)) {  // check if 'is not a number'
    Serial.println(F("Failed to read outside temperature"));
    sensor2_fail = 1;
    sns2_fail_cnt++;
  } else
    sensor2_fail = 0;

  curr_temp = t;
  outsideTemp = tOut;
  
  float h = sht31.readHumidity();
  // curr_humid = bme_read_humidity(0);
  if (isnan(h)) {  // check if 'is not a number'
    Serial.println(F("Failed to read humidity"));
  }
  float hOut = sht31Out.readHumidity();
  if (isnan(hOut)) {  // check if 'is not a number'
    Serial.println(F("Failed to read outside humidity"));
  }
  curr_humid = h;
  outsideHumidity = hOut;
  
  // Take Web server requests
  processWebRequests();

  static int clockPhase30 = 0;
  static int clockPhase10 = 0;
  int nowPhase30 = now.unixtime() % 30 /* 30 seconds in millis */;
  if (nowPhase30 < clockPhase30) {
    // perform 30 second actions
    writeToLog(&now);
    updateControl(&now);
  }
  int nowPhase10 = now.unixtime() % 10;
  // if (nowPhase10 < clockPhase10) {
    // perform 10 second actions
    
  // }
  if (nowPhase10 != clockPhase10) {
    // perform 1 second actions
    
  }
  Serial.println(nowPhase10);
  clockPhase10 = nowPhase10;
  clockPhase30 = nowPhase30;

  delay(250);
  // wdt_reset();
  // WDT.refresh();
}

void updateControl(DateTime* now) {
  bool change = false;
  char buf2[] = "YYYY-MM-DDThh:mm:ss";
  
  Serial.println(F("updateControl called"));

  if (sensor_fail) {
    if (sensor2_fail) {
      // Both sensors have failed, fans on, cooling on
      digitalWrite(5, HIGH);   // Turn fans on
      digitalWrite(2, HIGH);   // Open swamp shutter
      digitalWrite(3, LOW);    // Close vent
      digitalWrite(6, HIGH);    // Turn on pump
      digitalWrite(4, LOW);     // Turn off Dehumidifier
      ghState = COOL;
      logState(now, "Double Sensor fail, cooling");
    } else if (outsideTemp > 21) {
      // One sensor failed,  and it is warm outside
      digitalWrite(5, HIGH);   // Turn fans on
      digitalWrite(2, HIGH);   // Open swamp shutter
      digitalWrite(3, LOW);    // Close vent
      digitalWrite(6, HIGH);    // Turn on pump
      digitalWrite(4, LOW);     // Turn off Dehumidifier
      ghState = COOL;
      logState(now, "Main Sensor fail, still warm outside, cooling");
    } else {
      // Turn on front vent for safety
      digitalWrite(5, HIGH);   // Turn fan on
      digitalWrite(2, LOW);   // Close swamp shutter
      digitalWrite(3, HIGH);    // Open vent
      digitalWrite(6, LOW);    // Turn off pump
      digitalWrite(4, LOW);     // Turn off Dehumidifier
      ghState = VENT;
      logState(now, "Main Sensor fail, still cool outside, venting");
    }  
  } else {
    switch (ghState) {

      case RECIRC:
      
        if (curr_temp >= (set_temp + hyst_temp)) {
          digitalWrite(5, HIGH);   // Turn fans on
          digitalWrite(2, HIGH);   // Open swamp shutter
          digitalWrite(3, LOW);    // Close vent
          digitalWrite(6, HIGH);    // Turn on pump
          digitalWrite(4, LOW);     // Turn off Dehumidifier
          // Change state to cool!
          ghState = COOL;
          logState(now, "RECIRC to COOL");
          Serial.println(F("RECIRC to COOL"));
        } else if ((curr_humid > (set_hum + hyst_hum)) && (outsideHumidity < (curr_humid - 2*hyst_hum)) && (outsideTemp >= 20)) {
          digitalWrite(5, HIGH);   // Turn fans on
          digitalWrite(3, HIGH);   // Open vent
          digitalWrite(6, LOW);    // Turn off pump
          digitalWrite(2, LOW);    // Close swamp shutter
          digitalWrite(4, LOW);  // Turn off Dehumidifier
          // Change state to vent
          ghState = VENT;
          logState(now, "RECIRC to VENT");
          Serial.println(F("RECIRC to VENT"));
        } else if ((curr_humid > (set_hum + hyst_hum)) && ((outsideHumidity >= (curr_humid - 2*hyst_hum)) || (outsideTemp < 20))) {
          // Its too moist inside and outside, need to switch to dehumidify
          digitalWrite(5, LOW);   // Turn fans off
          digitalWrite(2, LOW);   // Close swamp shutter
          digitalWrite(3, LOW);    // Close vent
          digitalWrite(6, LOW);    // Turn off pump
          digitalWrite(4, HIGH);  // Turn on Dehumidifier
          // Change state to dehumidify
          ghState = DEHUM;
          logState(now, "RECIRC to DEHUM");
          Serial.println(F("RECIRC to DEHUM"));
        }
      break;
      case VENT:
        if (curr_temp >= (set_temp + hyst_temp)) {
          digitalWrite(5, HIGH);   // Turn fans on
          digitalWrite(2, HIGH);   // Open swamp shutter
          digitalWrite(3, LOW);    // Close vent
          digitalWrite(6, HIGH);    // Turn on pump
          digitalWrite(4, LOW);  // Turn off Dehumidifier
          // Change state to cool!
          ghState = COOL;
          logState(now, "VENT to COOL");
          Serial.println(F("VENT to COOL"));

        } else if (curr_humid <= set_hum) {
          digitalWrite(5, LOW);   // Turn fans off
          digitalWrite(2, LOW);   // Close swamp shutter
          digitalWrite(3, LOW);    // Close vent
          digitalWrite(6, LOW);    // Turn off pump
          digitalWrite(4, LOW);    // Turn off Dehumidifier
          // Change state to recirculate
          ghState = RECIRC;
          logState(now, "VENT to RECIRC");
          Serial.println(F("VENT to RECIRC"));
        } else if ((outsideHumidity > (curr_humid - 3*hyst_hum)) || (outsideTemp < 20)) {    // Is it too humid or cold outside to dry greenhouse?
          // Its too moist inside and outside, need to switch to dehumidify
          digitalWrite(5, LOW);   // Turn fans off
          digitalWrite(2, LOW);   // Close swamp shutter
          digitalWrite(3, LOW);    // Close vent
          digitalWrite(6, LOW);    // Turn off pump
          digitalWrite(4, HIGH);  // Turn on Dehumidifier
        
          // Change state to recirculate
          ghState = DEHUM;
          logState(now, "COOL to DEHUM");
          Serial.println(F("COOL to DEHUM"));
        }
        break;
      case COOL: 
        if (curr_temp < (set_temp - hyst_temp)) {
          if (now->hour() >= 17) {
            digitalWrite(5, HIGH);   // Turn fans on
            digitalWrite(2, HIGH);   // Open swamp shutter
            digitalWrite(3, LOW);    // Close vent
            digitalWrite(6, LOW);    // Turn off pump
            digitalWrite(4, LOW);    // Turn off Dehumidifier
            // Change state to pad dry
            ghState = PADDRY;
            padTime = now->unixtime();  
          logState(now, "COOL to PADDRY");
          Serial.println(F("COOL to PADDRY"));
        } else {
          digitalWrite(5, LOW);   // Turn fans off
          digitalWrite(2, LOW);   // Close swamp shutter
          digitalWrite(3, LOW);    // Close vent
          digitalWrite(6, LOW);    // Turn off pump
          digitalWrite(4, LOW);    // Turn off Dehumidifier
          // Change state to recirculate
          ghState = RECIRC;
          logState(now, "COOL to RECIRC");
          Serial.println(F("COOL to RECIRC"));
        }
      }
      break;
    case PADDRY:
      if (curr_temp >= (set_temp + hyst_temp)) {
        // Go back to cooling mode
        digitalWrite(5, HIGH);   // Turn fans on
        digitalWrite(2, HIGH);   // Open swamp shutter
        digitalWrite(3, LOW);    // Close vent
        digitalWrite(6, HIGH);    // Turn on pump
        digitalWrite(4, LOW);    // Turn off Dehumidifier
        // Change state to cooling
        ghState = COOL;
        logState(now, "PADDRY to COOL");
        Serial.println(F("PADDRY to COOL"));
      } else { 
        // update the pad drying timer. 
        int duration = now->unixtime() - padTime;
        
        // Did we time out? (10 min = 600 seconds )
        if (duration > 600) {
          
          digitalWrite(5, HIGH);   // Turn fans on
          digitalWrite(2, LOW);   // Close swamp shutter
          digitalWrite(3, HIGH);   // Open vent
          digitalWrite(6, LOW);    // Turn off pump
          digitalWrite(4, LOW);    // Turn off Dehumidifier
          // Change state to venting
          ghState = VENT;
          logState(now, "PADDRY to VENT");
          Serial.println(F("PADDRY to VENT"));
        }
      }
      break;
    case DEHUM:
      // Is it getting hot, sun's up and heating greenhouse
      if (curr_temp >= (set_temp + hyst_temp)) {
        // It is heating up outside, dont need to dehumidify any more
        digitalWrite(5, HIGH);   // Turn fans on
        digitalWrite(2, HIGH);   // Open swamp shutter
        digitalWrite(3, LOW);    // Close vent
        digitalWrite(6, HIGH);   // Turn on pump
        digitalWrite(4, LOW);    // Turn off Dehumidifier
        // Change state to cool!
        ghState = COOL;
        logState(now, "DEHUM to COOL");
        Serial.println(F("DEHUM to COOL"));
      } else if ((curr_humid > set_hum) && (outsideHumidity < (curr_humid - 3*hyst_hum)) && (outsideTemp > 20)) {
        // Outside is drying off, turn off dehumidifier and vent
        digitalWrite(5, HIGH);   // Turn fans on
        digitalWrite(3, HIGH);   // Open vent
        digitalWrite(6, LOW);    // Turn off pump
        digitalWrite(2, LOW);    // Close swamp shutter
        digitalWrite(4, LOW);  // Turn off Dehumidifier
        // Change state to vent
        ghState = VENT;
        logState(now, "DEHUM to VENT");
        Serial.println(F("DEHUM to VENT"));
      } else if (curr_humid <= set_hum) {
        digitalWrite(5, LOW);   // Turn fans off
        digitalWrite(2, LOW);   // Close swamp shutter
        digitalWrite(3, LOW);    // Close vent
        digitalWrite(6, LOW);    // Turn off pump
        digitalWrite(4, LOW);    // Turn off Dehumidifier
        // Change state to recirculate
        ghState = RECIRC;
        logState(now, "DEHUM to RECIRC");
        Serial.println(F("DEHUM to RECIRC"));
      }
      break;
    }
  }
}

void writeToLog(DateTime* now) {
  File dataFile = SD.open("datalog.txt", FILE_WRITE);

  char buf2[] = "YYYY-MM-DDThh:mm:ss";
  now->toString(buf2);
  
  Serial.print(buf2);
  Serial.print(curr_temp);
  Serial.print(" *C");
  Serial.print((curr_temp * 1.8) + 32);
  Serial.print(" *F");
  Serial.print(curr_humid);
  Serial.println(" %RH");

  Serial.print(buf2);
  Serial.print(outsideTemp);
  Serial.println(" *C");
  Serial.print((outsideTemp * 1.8) + 32);
  Serial.println(" *F");
  Serial.print(outsideHumidity);
  Serial.println(" %RH");
  
 
  Serial.println(buf2);
  // if the file is available, write to it:
  if (dataFile) {
    dataFile.print(buf2);
    dataFile.print(" Inside ");
    dataFile.print(curr_temp);
    dataFile.print(" C ");
    dataFile.print(curr_humid);
    dataFile.print(" H");
   //  dataFile.print(buf2);
    dataFile.print(" Outside ");
    dataFile.print(outsideTemp);
    dataFile.print(" C ");
    dataFile.print(outsideHumidity);
    dataFile.println(" H");
    dataFile.close();
    // print to the serial port too:
    
  }
}

void logState(DateTime* now, const char *action) {
  char buf2[] = "YYYY-MM-DDThh:mm:ss";
  
  File dataFile = SD.open("statechg.txt", FILE_WRITE);
  if (dataFile) {
    now->toString(buf2);
    dataFile.println(buf2);
    dataFile.println(action);
    dataFile.close();
  } else {
    Serial.println(F("Statechange failed to open"));
  }
}

void printWifiStatus() {
  // print the SSID of the network you're attached to:
  Serial.print("SSID: ");
  Serial.println(WiFi.SSID());

  // print your board's IP address:
  IPAddress ip = WiFi.localIP();
  Serial.print("IP Address: ");
  Serial.println(ip);

  // print the received signal strength:
  long rssi = WiFi.RSSI();
  Serial.print("signal strength (RSSI):");
  Serial.print(rssi);
  Serial.println(" dBm");
  // print where to go in a browser:
  Serial.print("To see this page in action, open a browser to http://");
  Serial.println(ip);
}

void processWebRequests() {

  int httpResult = 200;

  // Take Web server requests
  WiFiClient client = server.available();   // listen for incoming clients
  if (client) {                             // if you get a client,
    Serial.println("new client");           // print a message out the serial port
    String currentLine = "";                // make a String to hold incoming data from the client
    while (client.connected()) {            // loop while the client's connected
      if (client.available()) {             // if there's bytes to read from the client,
        char c = client.read();             // read a byte, then
        // Serial.write(c);                    // print it out to the serial monitor
        if (c == '\n') {                    // if the byte is a newline character
          // if the current line is blank, you got two newline characters in a row.
          // that's the end of the client HTTP request, so send a response:
          if (currentLine.length() == 0) {
            if (httpResult == 404) {
              client.println("HTTP/1.1 404 File not found");
              client.println("Content-type:text/html");
              client.println("Connection: close");
              client.stop();
            } else {
              // HTTP headers always start with a response code (e.g. HTTP/1.1 200 OK)
              // and a content-type so the client knows what's coming, then a blank line:
              client.println("HTTP/1.1 200 OK");
              client.println("Content-type:text/html");
              client.println("Connection: close");
              client.println();
              client.println("<!DOCTYPE html>");
              client.println("<HEAD>");
              client.println("<BODY>");
            
              client.print("<BR>Greenhouse temp: ");
              client.println(curr_temp);
              client.print("<BR>Outside temp: ");
              client.println(outsideTemp);
              client.print("<BR>Temperature Setpoint: ");
              client.println(set_temp);
              client.print("<BR>Greenhouse humidity: ");
              client.println(curr_humid);
              client.print("<BR>Outside humidity: ");
              client.println(outsideHumidity);
              client.print("<BR>Humidity Setpoint: ");
              client.println(set_hum);
              client.print("<BR>Indoor Sensor fail count: ");
              client.println(sns_fail_cnt);
              client.print("<BR>Outdoor Sensor fail count: ");
              client.println(sns2_fail_cnt);

              char *modestr = "";
              switch (ghState) {
                case RECIRC:
                  modestr = "Recirculate";
                  break;

                case COOL:
                  modestr = "Cooling";
                  break;

                case VENT:
                  modestr = "Venting";
                  break;

                case PADDRY:
                  modestr = "Pads Drying";
                  break;

                case DEHUM:
                  modestr  ="Dehumidify";
                  break;
              }

              client.print("<BR>Mode: ");
              client.println(modestr);

              // The HTTP response ends with another blank line:
              client.println("</BODY></html>");
              client.println();
              client.stop();

              // break out of the while loop:
              break;
            }
          } else {    // if you got a newline, then clear currentLine:
            String lastLine = currentLine;
            if (lastLine.endsWith("GET /favicon.ico HTTP/1.1")) {
              // Serial.println("favicon found"); 
              httpResult = 404;
            } else {
              // Serial.print("No match for : ");
              // Serial.println(lastLine);
            }
            currentLine = "";
          }
        } else if (c != '\r') {  // if you got anything else but a carriage return character,
          currentLine += c;      // add it to the end of the currentLine
        }
      }
    }
  }
}
