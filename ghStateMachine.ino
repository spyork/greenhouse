#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_RGBLCDShield.h>
#include <utility/Adafruit_MCP23017.h>
#include <avr/wdt.h>
#include "Adafruit_SHT31.h"
#include "RTClib.h"

#define SEALEVELPRESSURE_HPA (1013.25)

// Declare global variables

// Initialize all greenhouse variables

Adafruit_RGBLCDShield lcd = Adafruit_RGBLCDShield();

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

int ghState = RECIRC;

int curr_temp = 0;
int outsideTemp = 0;

int curr_humid = 0;
int outsideHumidity = 0;
int sensor_fail = 0;
int sensor2_fail = 0;

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

void setup() {
  char buf2[] = "YYYY-MM-DDThh:mm:ss";
  
  // put your setup code here, to run once:

  //Initialize serial:
  Serial.begin(9600);
  while (!Serial) {
    ; // wait for serial port to connect. Needed for native USB port only
  }

  // Setup watchdog timer
  wdt_enable(WDTO_8S);
  
  // Setup the temp humidity sensor
  if (! sht31.begin()) {   // Set to 0x45 for alternate i2c addr
    Serial.println(F("Couldn't find SHT31"));
    // System has to run even if temp sensor fails...
    sensor_fail = 1;
    // abort();
  }
  // Setup the outside temp humidity sensor
  if (! sht31Out.begin(0x45)) {   // Set to 0x45 for alternate i2c addr
    Serial.println(F("Couldn't find Outside SHT31"));
    sensor2_fail = 1;
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

  // Start the LCD 
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
  uint8_t buttons = lcd.readButtons();

  // unixtime counts in seconds (5 minute delay)
  if (!lcdoff && ((now.unixtime() - lastPress.unixtime()) > 300)) {
    // set backlight off, no one is using
    lcd.noDisplay();
    lcdoff = true;
  }
  
  if (buttons) {
    if (lcdoff) {
       lcd.display();
       lcdoff = false;
    } else {
      if (buttons & BUTTON_UP) {
        // raise temp setpoint
        Serial.println(F("Temp Setpoint raised"));
        set_temp += 1;
      }
      if (buttons & BUTTON_DOWN) {
        // lower temp setpoint
        Serial.println(F("Temp Setpoint lowered"));
        set_temp -= 1;
      }
      if (buttons & BUTTON_LEFT) {
        // raise humidity setpoint
        Serial.println(F("Humidity Setpoint raised"));
        set_hum -= 1;
      }
      if (buttons & BUTTON_RIGHT) {
        // lower humidity setpoint
        Serial.println(F("Humidity Setpoint lowered"));
        set_hum += 1;
      }
      if (buttons & BUTTON_SELECT) {
        // Change mode
      }
    }
    lastPress = now;
  }
  
  // Now check temperature and act on fans, shutters...
  // curr_temp = bme_read_temp(0);
  // curr_temp = 26;
  float t = sht31.readTemperature();
  if (isnan(t)) {  // check if 'is not a number' 
    Serial.println(F("Failed to read temperature"));
    sensor_fail = 1;
  } else
    sensor_fail = 0;

  float tOut = sht31Out.readTemperature();
  if (isnan(tOut)) {  // check if 'is not a number'
    Serial.println(F("Failed to read outside temperature"));
    sensor2_fail = 1;
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
    updateLCD();
  }
  Serial.println(nowPhase10);
  clockPhase10 = nowPhase10;
  clockPhase30 = nowPhase30;

  delay(250);
  wdt_reset();
}

void updateLCD() {
  if (lcdoff) {
    return;
  }
  // Update LCD
  lcd.begin(16,2);
  //lcd.clear();
  lcd.setCursor(0,0);
  lcd.print(curr_temp);
  lcd.print("/"); 
  lcd.print(set_temp);
  lcd.print("/"); 
  lcd.print(outsideTemp);
  lcd.print(" C ");
  lcd.print("ST");
  lcd.setCursor(0,1);
  lcd.print(curr_humid);
  lcd.print("/");
  lcd.print(set_hum);
  lcd.print("/");
  lcd.print(outsideHumidity);
  lcd.print("RH ");
  switch (ghState) {
    case RECIRC:
      lcd.print(F("CIRC"));
      break;

    case VENT:
      lcd.print(F("VENT"));
      break;

     case COOL:
      lcd.print(F("COOL"));
      break;

    case PADDRY:
      lcd.print(F("PADS"));
      break;

    case DEHUM:
      lcd.print(F("DEHM"));
      break;
  }
  lcd.setCursor(12,0);
  // lcd.print("FSVP");
  lcd.setCursor(12,1);
  // lcd.print(fan_state);
  // lcd.print(shutter_swamp);
  // lcd.print(shutter_vent);
  // lcd.print(pump_on);
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
        } else if ((curr_humid > (set_hum + hyst_hum)) && (outsideHumidity < (curr_humid - 2*hyst_hum)) && (outsideTemp > 20)) {
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
