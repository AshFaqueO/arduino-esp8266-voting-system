#include <Adafruit_Fingerprint.h>
#include <SoftwareSerial.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <string.h>
#include <stdio.h>

LiquidCrystal_I2C lcd(0x27, 16, 2);

// Setup the fingerprint sensor on pins D2 (TX) and D3 (RX)
SoftwareSerial fingerSerial(2, 3);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerSerial);

// Setup the ESP8266 WiFi module. A0 is TX, and A1 is RX (use a voltage divider on RX)
SoftwareSerial wifiSerial(A0, A1);

#define ENABLE_WIFI 1
#define ESP8266_BAUD 9600

// Update these with your network details
const char WIFI_SSID[] = "50fusion";
const char WIFI_PASS[] = "01763163386";

// ThingSpeak server details and write API key
const char THINGSPEAK_HOST[] = "api.thingspeak.com";
const int THINGSPEAK_PORT = 80;
const char THINGSPEAK_WRITE_API_KEY[] = "SUSWVP8CL16TIG8I";

bool wifiReady = false;

// We need to wait at least 16 seconds between ThingSpeak uploads on the free tier
const unsigned long THINGSPEAK_MIN_INTERVAL = 16000UL;
unsigned long lastThingSpeakUpload = 0;
bool hasThingSpeakUploaded = false;

// Connect the positive pin of the buzzer to D7
const int buzzer = 7;

// Define the layout for the 4x4 matrix keypad
const byte ROWS = 4;
const byte COLS = 4;

char keys[ROWS][COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};

// Keypad row pins are connected to D4, D5, D6, D8
byte rowPins[ROWS] = {4, 5, 6, 8};

// Keypad column pins are connected to D9, D10, D11, D12
byte colPins[COLS] = {9, 10, 11, 12};

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// Set the ID number for the admin fingerprint (used to start/stop the election)
const int ADMIN_ID = 3;

// Variables to keep track of the election status and vote counts
bool votingStarted = false;
bool votingStopped = false;
bool voted[128] = {false};
int party1_count = 0;
int party2_count = 0;
int party3_count = 0;

// List of functions used in this sketch
int getFingerprintIDez();
int takeVote(int voterID);
void showResult();
void waitForFingerRemove();
void beepSuccess();
void beepError();
void initWiFi();
void uploadVoteData(int lastParty, int voterID, int systemStatus);
void enforceThingSpeakDelay();
bool sendAT(const char *cmd, const char *expected, unsigned long timeout);
bool waitForExpected(const char *expected, unsigned long timeout);

// Initialize all the components when the Arduino starts up
void setup() {
  Serial.begin(9600);

  pinMode(buzzer, OUTPUT);
  digitalWrite(buzzer, LOW);

  lcd.init();
  lcd.backlight();

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("Smart Voting"));
  lcd.setCursor(0, 1);
  lcd.print(F("System Start"));
  delay(1500);

  // Boot up the fingerprint sensor and make sure it is responding
  finger.begin(57600);
  fingerSerial.listen();

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("Checking"));
  lcd.setCursor(0, 1);
  lcd.print(F("Fingerprint"));

  if (finger.verifyPassword()) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(F("Sensor Found"));
    beepSuccess();
    delay(1200);
  } 
  else {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(F("Sensor Failed"));
    lcd.setCursor(0, 1);
    lcd.print(F("Check Wiring"));
    beepError();
    while (1);
  }

  // Connect to the WiFi network if it is enabled in the code
#if ENABLE_WIFI
  initWiFi();
#else
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("WiFi Disabled"));
  delay(1000);
#endif

  fingerSerial.listen();

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("Admin Finger"));
  lcd.setCursor(0, 1);
  lcd.print(F("To Start Vote"));
  delay(1500);
}

// The main loop that continuously checks for fingerprints and processes votes
void loop() {
  fingerSerial.listen();

  // If the election is over, wait for the admin to scan their finger to show the final results
  if (votingStopped == true) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(F("Voting Closed"));
    lcd.setCursor(0, 1);
    lcd.print(F("Admin=Result"));
    delay(700);

    int id = getFingerprintIDez();
    if (id == ADMIN_ID) {
      showResult();
      waitForFingerRemove();
    }

    return;
  }

  // If the election hasn't started yet, wait for the admin to open the polls
  if (votingStarted == false) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(F("Admin Finger"));
    lcd.setCursor(0, 1);
    lcd.print(F("To Start Vote"));

    int id = getFingerprintIDez();
    if (id == ADMIN_ID) {
      votingStarted = true;

      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print(F("Voting Started"));
      lcd.setCursor(0, 1);
      lcd.print(F("Voters Allowed"));
      beepSuccess();
      delay(2000);

      waitForFingerRemove();
    } 
    else if (id > 0) {
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print(F("Voting Not"));
      lcd.setCursor(0, 1);
      lcd.print(F("Started Yet"));
      beepError();
      delay(2000);

      waitForFingerRemove();
    }

    delay(300);
    return;
  }

  // The polls are open, so wait for a voter to place their finger on the sensor
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("Place Finger"));
  lcd.setCursor(0, 1);
  lcd.print(F("Vote/Stop"));

  int id = getFingerprintIDez();
  if (id > 0) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(F("Detected ID:"));
    lcd.setCursor(0, 1);
    lcd.print(id);
    delay(1000);

    // If the admin scans their finger again, close the polls
    if (id == ADMIN_ID) {
      votingStopped = true;
      votingStarted = false;

      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print(F("Voting Stopped"));
      lcd.setCursor(0, 1);
      lcd.print(F("By Admin"));
      beepSuccess();
      delay(2000);

      waitForFingerRemove();
      // Upload the final status to ThingSpeak (status 2 means voting is stopped)
      uploadVoteData(0, ADMIN_ID, 2);
      showResult();

      return;
    }

    // Check if a normal voter (IDs 1 to 127) has already voted
    if (id >= 1 && id <= 127) {
      if (voted[id] == true) {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print(F("Duplicate Vote"));
        lcd.setCursor(0, 1);
        lcd.print(F("Not Allowed"));
        beepError();
        delay(2500);

        waitForFingerRemove();
        return;
      }
    } 
    else {
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print(F("Invalid ID"));
      beepError();
      delay(1500);

      waitForFingerRemove();
      return;
    }

    // If they are a valid new voter, let them cast their vote
    int selectedParty = takeVote(id);
    waitForFingerRemove();
    // Send their vote data to ThingSpeak (status 1 means voting is running)
    uploadVoteData(selectedParty, id, 1);
  }

  delay(300);
}

// Ask the voter to pick a party using the keypad and record their choice
int takeVote(int voterID) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("1:A 2:B"));
  lcd.setCursor(0, 1);
  lcd.print(F("3:C"));

  char key = 0;
  bool validVote = false;
  int selectedParty = 0;

  while (validVote == false) {
    key = keypad.getKey();
    if (key) {
      if (key == '1') {
        party1_count++;
        voted[voterID] = true;
        validVote = true;
        selectedParty = 1;

        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print(F("Vote Given To:"));
        lcd.setCursor(0, 1);
        lcd.print(F("A"));
      } 
      else if (key == '2') {
        party2_count++;
        voted[voterID] = true;
        validVote = true;
        selectedParty = 2;

        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print(F("Vote Given To:"));
        lcd.setCursor(0, 1);
        lcd.print(F("B"));
      } 
      else if (key == '3') {
        party3_count++;
        voted[voterID] = true;
        validVote = true;
        selectedParty = 3;

        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print(F("Vote Given To:"));
        lcd.setCursor(0, 1);
        lcd.print(F("C"));
      } 
      else {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print(F("Invalid Key"));
        lcd.setCursor(0, 1);
        lcd.print(F("Press 1/2/3"));
        beepError();
        delay(1500);

        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print(F("1:A 2:B"));
        lcd.setCursor(0, 1);
        lcd.print(F("3:C"));
      }
    }
  }

  beepSuccess();
  delay(1500);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("Thanks For"));
  lcd.setCursor(0, 1);
  lcd.print(F("Your Vote"));
  delay(1500);

  return selectedParty;
}

// Display the final vote counts and announce the winner on the LCD
void showResult() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("A:"));
  lcd.print(party1_count);
  lcd.print(F(" B:"));
  lcd.print(party2_count);

  lcd.setCursor(0, 1);
  lcd.print(F("C:"));
  lcd.print(party3_count);

  delay(4000);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("Winner:"));
  lcd.setCursor(0, 1);

  if (party1_count == 0 && party2_count == 0 && party3_count == 0) {
    lcd.print(F("No Votes"));
  }
  else if (party1_count > party2_count && party1_count > party3_count) {
    lcd.print(F("A"));
  }
  else if (party2_count > party1_count && party2_count > party3_count) {
    lcd.print(F("B"));
  }
  else if (party3_count > party1_count && party3_count > party2_count) {
    lcd.print(F("C"));
  }
  else {
    lcd.print(F("Tie Result"));
  }

  delay(4000);
}

// Tell the sensor to look for a fingerprint and return the ID if it finds a match
int getFingerprintIDez() {
  fingerSerial.listen();

  uint8_t p = finger.getImage();
  if (p != FINGERPRINT_OK) {
    return -1;
  }

  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) {
    return -1;
  }

  p = finger.fingerFastSearch();
  if (p != FINGERPRINT_OK) {
    return -1;
  }

  Serial.print(F("Found ID #"));
  Serial.println(finger.fingerID);

  return finger.fingerID;
}

// Pause the system until the person removes their finger so we don't accidentally scan them twice
void waitForFingerRemove() {
  fingerSerial.listen();
  uint8_t p = finger.getImage();

  while (p != FINGERPRINT_NOFINGER) {
    delay(100);
    p = finger.getImage();
  }

  delay(500);
}

// Make a quick beep to indicate success
void beepSuccess() {
  digitalWrite(buzzer, HIGH);
  delay(500);
  digitalWrite(buzzer, LOW);
}

// Make three quick beeps to indicate an error
void beepError() {
  for (int i = 0; i < 3; i++) {
    digitalWrite(buzzer, HIGH);
    delay(250);
    digitalWrite(buzzer, LOW);
    delay(250);
  }
}

// Start up the ESP8266 and connect it to the local WiFi network
void initWiFi() {
#if ENABLE_WIFI
  wifiSerial.begin(ESP8266_BAUD);
  wifiSerial.listen();

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("Checking WiFi"));
  lcd.setCursor(0, 1);
  lcd.print(F("ESP8266..."));
  delay(1000);

  if (!sendAT("AT", "OK", 3000)) {
    wifiReady = false;

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(F("WiFi Failed"));
    lcd.setCursor(0, 1);
    lcd.print(F("Check ESP"));
    beepError();
    delay(2000);

    fingerSerial.listen();
    return;
  }

  sendAT("ATE0", "OK", 2000);
  sendAT("AT+CWMODE=1", "OK", 3000);
  sendAT("AT+CIPMUX=0", "OK", 3000);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("Connecting"));
  lcd.setCursor(0, 1);
  lcd.print(F("WiFi..."));

  char cmd[120];
  snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"", WIFI_SSID, WIFI_PASS);
  if (sendAT(cmd, "OK", 25000)) {
    wifiReady = true;

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(F("WiFi Connected"));
    beepSuccess();
    delay(1500);
  } 
  else {
    wifiReady = false;

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(F("WiFi Not"));
    lcd.setCursor(0, 1);
    lcd.print(F("Connected"));
    beepError();
    delay(2000);
  }

  fingerSerial.listen();
#endif
}

// Make sure we wait long enough between ThingSpeak uploads to avoid hitting the rate limit
void enforceThingSpeakDelay() {
#if ENABLE_WIFI
  if (hasThingSpeakUploaded == false) {
    return;
  }

  unsigned long nowTime = millis();
  unsigned long elapsed = nowTime - lastThingSpeakUpload;
  if (elapsed < THINGSPEAK_MIN_INTERVAL) {
    unsigned long waitTime = THINGSPEAK_MIN_INTERVAL - elapsed;

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(F("ThingSpeak Wait"));
    lcd.setCursor(0, 1);
    lcd.print(waitTime / 1000);
    lcd.print(F(" sec"));

    delay(waitTime);
  }
#endif
}

// Build the HTTP request and send the latest vote data to the ThingSpeak cloud
void uploadVoteData(int lastParty, int voterID, int systemStatus) {
#if ENABLE_WIFI
  if (!wifiReady) {
    fingerSerial.listen();
    return;
  }

  if (strcmp(THINGSPEAK_WRITE_API_KEY, "PASTE_YOUR_THINGSPEAK_WRITE_API_KEY") == 0) {
    Serial.println(F("ThingSpeak Write API Key not set. Upload skipped."));
    fingerSerial.listen();
    return;
  }

  enforceThingSpeakDelay();

  wifiSerial.listen();

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("Uploading Data"));
  lcd.setCursor(0, 1);
  lcd.print(F("ThingSpeak"));

  sendAT("AT+CIPCLOSE", "OK", 1000);
  delay(500);
  sendAT("AT+CIPMUX=0", "OK", 2000);

  char path[220];

  snprintf(
    path,
    sizeof(path),
    "/update?api_key=%s&field1=%d&field2=%d&field3=%d&field4=%d&field5=%d&field6=%d",
    THINGSPEAK_WRITE_API_KEY,
    party1_count,
    party2_count,
    party3_count,
    lastParty,
    voterID,
    systemStatus
  );
  char startCmd[90];

  snprintf(
    startCmd,
    sizeof(startCmd),
    "AT+CIPSTART=\"TCP\",\"%s\",%d",
    THINGSPEAK_HOST,
    THINGSPEAK_PORT
  );
  if (!sendAT(startCmd, "OK", 10000)) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(F("Upload Failed"));
    lcd.setCursor(0, 1);
    lcd.print(F("TCP Error"));
    beepError();
    delay(1200);
    sendAT("AT+CIPCLOSE", "OK", 2000);
    fingerSerial.listen();
    return;
  }

  unsigned int requestLen = 0;

  requestLen += strlen("GET ");
  requestLen += strlen(path);
  requestLen += strlen(" HTTP/1.1\r\nHost: ");
  requestLen += strlen(THINGSPEAK_HOST);
  requestLen += strlen("\r\nConnection: close\r\n\r\n");

  char sendCmd[30];
  snprintf(sendCmd, sizeof(sendCmd), "AT+CIPSEND=%u", requestLen);
  if (!sendAT(sendCmd, ">", 6000)) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(F("Send Error"));
    beepError();
    delay(1000);

    sendAT("AT+CIPCLOSE", "OK", 2000);
    fingerSerial.listen();
    return;
  }

  wifiSerial.print(F("GET "));
  wifiSerial.print(path);
  wifiSerial.print(F(" HTTP/1.1\r\nHost: "));
  wifiSerial.print(THINGSPEAK_HOST);
  wifiSerial.print(F("\r\nConnection: close\r\n\r\n"));
  if (waitForExpected("SEND OK", 10000)) {
    lastThingSpeakUpload = millis();
    hasThingSpeakUploaded = true;

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(F("Upload Done"));
    lcd.setCursor(0, 1);
    lcd.print(F("ThingSpeak"));
    beepSuccess();
    delay(1000);
  } 
  else {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(F("Upload Error"));
    beepError();
    delay(1000);
  }

  sendAT("AT+CIPCLOSE", "OK", 2000);

  fingerSerial.listen();
#endif
}

// Send a command to the ESP8266 and wait to see if we get the expected response back
bool sendAT(const char *cmd, const char *expected, unsigned long timeout) {
  wifiSerial.listen();
  while (wifiSerial.available()) {
    wifiSerial.read();
  }

  Serial.print(F(">> "));
  Serial.println(cmd);

  wifiSerial.println(cmd);

  return waitForExpected(expected, timeout);
}

// Read the incoming data from the ESP8266 and look for a specific string like "OK"
bool waitForExpected(const char *expected, unsigned long timeout) {
  char buffer[80];
  byte index = 0;

  memset(buffer, 0, sizeof(buffer));

  unsigned long startTime = millis();
  while (millis() - startTime < timeout) {
    while (wifiSerial.available()) {
      char c = wifiSerial.read();
      Serial.write(c);

      if (index < sizeof(buffer) - 1) {
        buffer[index++] = c;
        buffer[index] = '\0';
      } 
      else {
        memmove(buffer, buffer + 1, sizeof(buffer) - 2);
        buffer[sizeof(buffer) - 2] = c;
        buffer[sizeof(buffer) - 1] = '\0';
      }

      if (strstr(buffer, expected) != NULL) {
        Serial.println();
        return true;
      }
    }
  }

  Serial.println();
  return false;
}