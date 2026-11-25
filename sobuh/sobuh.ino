/*
  SOBUH — Fruit sorting by weight, with Serial data output
  Arduino Uno + load cell (HX711) + servo + 16x2 I2C LCD

  How it works:
    1. The load cell (through the HX711 amplifier/ADC) measures the fruit's weight.
    2. The weight is shown on the LCD and printed to the Serial Monitor.
    3. The servo turns a deflector to send the fruit to the right bin:
         weight <  3 g          -> no fruit on the platform  -> servo  90° (neutral)
         3 g <= weight < 12 g   -> LIGHT fruit               -> servo  30°
         weight >= 12 g         -> HEAVY fruit               -> servo 150°
       (thresholds and angles are the same as the original 2024 sketch)
    4. Once a fruit's weight is stable, one data line is printed to the Serial
       Monitor. The platform must be emptied before the next fruit is recorded.
       Copy the Serial Monitor output into the dashboard (../dashboard/index.html)
       to see statistics and charts.

  Data line format:  DATA,session,fruit,uptime_s,weight_g,category,servo_deg
    session   increases by 1 at every power-on (stored in EEPROM)
    fruit     fruit number within the session
    uptime_s  seconds since power-on (the Uno has no real-time clock)

  Serial commands (9600 baud):
    t = tare (zero the scale)    r = recalibrate
    c = edit calibration factor  h = help

  Requires: HX711_ADC (Olav Kallhovd), LiquidCrystal I2C (Frank de Brabander),
  Servo (bundled with the Arduino IDE).

  The calibration routine is adapted from the "Calibration" example of the
  HX711_ADC library — MIT License, Copyright (c) 2017 Olav Kallhovd.
*/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <HX711_ADC.h>
#include <Servo.h>
#include <EEPROM.h>

// ----------------------------------------------------------------- configuration
// Pins
const uint8_t HX711_DOUT_PIN = 3;   // HX711 DT  -> Arduino D3
const uint8_t HX711_SCK_PIN  = 2;   // HX711 SCK -> Arduino D2
const uint8_t SERVO_PIN      = 9;   // servo signal -> Arduino D9 (PWM)
// LCD: SDA -> A4, SCL -> A5 on the Uno
const uint8_t LCD_ADDRESS    = 0x27;  // try 0x3F if the LCD stays blank
const uint8_t LCD_COLS       = 16;
const uint8_t LCD_ROWS       = 2;

// Sorting thresholds in grams (same as the original project)
const float EMPTY_BELOW_G = 3.0;    // below this, the platform is treated as empty
const float HEAVY_FROM_G  = 12.0;   // at or above this, the fruit is HEAVY

// Servo angles in degrees (same as the original project)
const uint8_t ANGLE_NEUTRAL = 90;
const uint8_t ANGLE_LIGHT   = 30;
const uint8_t ANGLE_HEAVY   = 150;

// Timing
const unsigned long STABILIZING_TIME_MS = 2000;  // HX711 settling time at start-up
const unsigned long UPDATE_INTERVAL_MS  = 500;   // how often weight is shown and sorted

// Recording: a fruit is recorded once its weight changes by no more than
// STABLE_DELTA_G for STABLE_READINGS consecutive readings (3 x 0.5 s = 1.5 s).
const float   STABLE_DELTA_G  = 0.5;
const uint8_t STABLE_READINGS = 3;

// EEPROM layout
const int CAL_EEPROM_ADDRESS     = 0;  // float, 4 bytes: calibration factor
const int SESSION_EEPROM_ADDRESS = 4;  // uint16, 2 bytes: power-on counter

// ----------------------------------------------------------------- globals
HX711_ADC LoadCell(HX711_DOUT_PIN, HX711_SCK_PIN);
LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_COLS, LCD_ROWS);
Servo sorterServo;

enum Category { NO_FRUIT, LIGHT, HEAVY };

unsigned long lastUpdateMs = 0;
int currentAngle = -1;      // -1 = not set yet
uint16_t sessionId = 0;
uint16_t fruitCount = 0;    // fruits recorded in this session

// ----------------------------------------------------------------- helpers
// Print one LCD row, padded with spaces so old characters are cleared.
void lcdLine(uint8_t row, const char *text) {
  char buf[LCD_COLS + 1];
  snprintf(buf, sizeof(buf), "%-16s", text);
  lcd.setCursor(0, row);
  lcd.print(buf);
}

void lcdShow(const char *line1, const char *line2) {
  lcdLine(0, line1);
  lcdLine(1, line2);
}

Category categorize(float grams) {
  if (grams < EMPTY_BELOW_G) return NO_FRUIT;
  if (grams < HEAVY_FROM_G) return LIGHT;
  return HEAVY;
}

uint8_t angleFor(Category c) {
  switch (c) {
    case LIGHT: return ANGLE_LIGHT;
    case HEAVY: return ANGLE_HEAVY;
    default:    return ANGLE_NEUTRAL;
  }
}

const char *labelFor(Category c) {
  switch (c) {
    case LIGHT: return "LIGHT  -> bin A";
    case HEAVY: return "HEAVY  -> bin B";
    default:    return "Place a fruit";
  }
}

const char *csvNameFor(Category c) {
  return c == HEAVY ? "HEAVY" : (c == LIGHT ? "LIGHT" : "NONE");
}

void moveServo(uint8_t angle) {
  if (angle != currentAngle) {
    sorterServo.write(angle);
    currentAngle = angle;
  }
}

// ----------------------------------------------------------------- EEPROM
// Works on AVR (Uno) and, with begin/commit, on ESP boards.
void eepromBegin() {
#if defined(ESP8266) || defined(ESP32)
  EEPROM.begin(512);
#endif
}

void eepromCommit() {
#if defined(ESP8266) || defined(ESP32)
  EEPROM.commit();
#endif
}

void saveCalFactor(float value) {
  eepromBegin();
  EEPROM.put(CAL_EEPROM_ADDRESS, value);
  eepromCommit();
}

float loadCalFactor() {
  float value = 0;
  eepromBegin();
  EEPROM.get(CAL_EEPROM_ADDRESS, value);
  return value;
}

// An erased EEPROM reads as NaN (0xFFFFFFFF); 0 or absurd values are also invalid.
bool isValidCalFactor(float value) {
  return !isnan(value) && value != 0 && fabs(value) < 1.0e7;
}

// Increase and store the power-on counter. An erased EEPROM (0xFFFF) restarts at 1.
uint16_t nextSessionId() {
  uint16_t id = 0;
  eepromBegin();
  EEPROM.get(SESSION_EEPROM_ADDRESS, id);
  id = (id == 0xFFFF) ? 1 : id + 1;
  EEPROM.put(SESSION_EEPROM_ADDRESS, id);
  eepromCommit();
  return id;
}

// ----------------------------------------------------------------- data output
// One line per fruit, prefixed with "DATA," so the dashboard can pick these
// lines out of the rest of the Serial Monitor output.
void printDataHeader() {
  Serial.println(F("DATA,session,fruit,uptime_s,weight_g,category,servo_deg"));
}

void recordFruit(float weight, Category category) {
  fruitCount++;
  Serial.print(F("DATA,"));
  Serial.print(sessionId);          Serial.print(',');
  Serial.print(fruitCount);         Serial.print(',');
  Serial.print(millis() / 1000);    Serial.print(',');
  Serial.print(weight, 2);          Serial.print(',');
  Serial.print(csvNameFor(category)); Serial.print(',');
  Serial.println(angleFor(category));
}

// ----------------------------------------------------------------- serial helpers
// Wait for 'y' or 'n' on the Serial Monitor.
bool askYesNo() {
  while (true) {
    if (Serial.available() > 0) {
      char c = Serial.read();
      if (c == 'y') return true;
      if (c == 'n') return false;
    }
  }
}

void offerToSave(float value) {
  Serial.print(F("Save this value to EEPROM address "));
  Serial.print(CAL_EEPROM_ADDRESS);
  Serial.println(F("? y/n"));
  lcdShow("Save to EEPROM?", "Send y / n");
  if (askYesNo()) {
    saveCalFactor(value);
    Serial.print(F("Value "));
    Serial.print(loadCalFactor());
    Serial.println(F(" saved to EEPROM"));
  } else {
    Serial.println(F("Value not saved to EEPROM"));
  }
}

void printHelp() {
  Serial.println(F("Commands: t = tare, r = recalibrate, c = edit calibration factor, h = help"));
}

// ----------------------------------------------------------------- calibration
// Adapted from the HX711_ADC "Calibration" example (MIT, (c) 2017 Olav Kallhovd).
void calibrate() {
  Serial.println(F("***"));
  Serial.println(F("Start calibration:"));
  Serial.println(F("Place the load cell on a level, stable surface."));
  Serial.println(F("Remove any load applied to the load cell."));
  Serial.println(F("Send 't' from the Serial Monitor to set the tare offset."));
  lcdShow("Calibration 1/2", "Empty, send 't'");

  bool resume = false;
  while (!resume) {
    LoadCell.update();
    if (Serial.available() > 0 && Serial.read() == 't') LoadCell.tareNoDelay();
    if (LoadCell.getTareStatus()) {
      Serial.println(F("Tare complete"));
      resume = true;
    }
  }

  Serial.println(F("Now place a known mass on the load cell."));
  Serial.println(F("Then send its weight in grams (e.g. 100.0) from the Serial Monitor."));
  lcdShow("Calibration 2/2", "Known mass+grams");

  float knownMass = 0;
  while (knownMass == 0) {
    LoadCell.update();
    if (Serial.available() > 0) {
      knownMass = Serial.parseFloat();
      if (knownMass != 0) {
        Serial.print(F("Known mass is: "));
        Serial.println(knownMass);
      }
    }
  }

  LoadCell.refreshDataSet();  // make sure the known mass is measured correctly
  float newCal = LoadCell.getNewCalibration(knownMass);

  Serial.print(F("New calibration value has been set to: "));
  Serial.println(newCal);
  offerToSave(newCal);

  Serial.println(F("End calibration"));
  Serial.println(F("***"));
  printHelp();
}

void changeSavedCalFactor() {
  Serial.println(F("***"));
  Serial.print(F("Current value is: "));
  Serial.println(LoadCell.getCalFactor());
  Serial.println(F("Now send the new value from the Serial Monitor, e.g. 696.0"));
  lcdShow("Edit cal factor", "Send new value");

  float newCal = 0;
  while (newCal == 0) {
    if (Serial.available() > 0) {
      newCal = Serial.parseFloat();
      if (newCal != 0) {
        Serial.print(F("New calibration value is: "));
        Serial.println(newCal);
        LoadCell.setCalFactor(newCal);
      }
    }
  }
  offerToSave(newCal);
  Serial.println(F("End change calibration value"));
  Serial.println(F("***"));
}

// ----------------------------------------------------------------- setup / loop
void setup() {
  Serial.begin(9600);
  delay(10);
  Serial.println();
  Serial.println(F("SOBUH - fruit sorting by weight. Starting..."));

  lcd.init();
  lcd.backlight();
  lcdShow("SOBUH sorter", "Starting...");

  sorterServo.attach(SERVO_PIN);
  moveServo(ANGLE_NEUTRAL);

  sessionId = nextSessionId();
  Serial.print(F("Session "));
  Serial.println(sessionId);

  LoadCell.begin();
  LoadCell.start(STABILIZING_TIME_MS, true);  // true = tare after stabilizing
  if (LoadCell.getTareTimeoutFlag() || LoadCell.getSignalTimeoutFlag()) {
    Serial.println(F("Timeout, check MCU > HX711 wiring and pin designations"));
    lcdShow("HX711 not found", "Check wiring");
    while (true) {}
  }

  float storedCal = loadCalFactor();
  if (isValidCalFactor(storedCal)) {
    LoadCell.setCalFactor(storedCal);
    Serial.print(F("Loaded calibration factor from EEPROM: "));
    Serial.println(storedCal);
  } else {
    Serial.println(F("No valid calibration factor in EEPROM - starting calibration."));
    LoadCell.setCalFactor(1.0);
    while (!LoadCell.update()) {}
    calibrate();
  }

  printHelp();
  printDataHeader();
  lcdShow("SOBUH ready", "Place a fruit");
}

void loop() {
  static bool newDataReady = false;
  static float lastWeight = 0;
  static uint8_t stableCount = 0;
  static bool fruitRecorded = false;

  if (LoadCell.update()) newDataReady = true;

  if (newDataReady && millis() - lastUpdateMs >= UPDATE_INTERVAL_MS) {
    float weight = LoadCell.getData();
    Category category = categorize(weight);
    moveServo(angleFor(category));

    // Record each fruit once, after its weight has settled.
    if (category == NO_FRUIT) {
      fruitRecorded = false;
      stableCount = 0;
    } else if (!fruitRecorded) {
      stableCount = (fabs(weight - lastWeight) <= STABLE_DELTA_G) ? stableCount + 1 : 0;
      if (stableCount >= STABLE_READINGS) {
        recordFruit(weight, category);
        fruitRecorded = true;
      }
    }
    lastWeight = weight;

    char number[10];
    dtostrf(weight, 6, 1, number);
    char line[LCD_COLS + 1];
    snprintf(line, sizeof(line), "%sg  #%u", number, fruitCount);
    lcdLine(0, labelFor(category));
    lcdLine(1, line);

    Serial.print(F("Weight: "));
    Serial.print(weight, 2);
    Serial.print(F(" g  -> servo "));
    Serial.println(currentAngle);

    newDataReady = false;
    lastUpdateMs = millis();
  }

  if (Serial.available() > 0) {
    char command = Serial.read();
    switch (command) {
      case 't':
        LoadCell.tareNoDelay();
        lcdShow("Taring...", "Keep it empty");
        break;
      case 'r': calibrate(); break;
      case 'c': changeSavedCalFactor(); break;
      case 'h': printHelp(); break;
      default: break;  // ignore newlines and unknown characters
    }
  }

  if (LoadCell.getTareStatus()) {
    Serial.println(F("Tare complete"));
  }
}
