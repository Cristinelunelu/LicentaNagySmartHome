#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <ESP32Servo.h>
#include <BluetoothSerial.h>
#include <SPI.h>
#include <MFRC522.h>
#include <DHT.h>
#include <time.h>


#define WIFI_SSID "Cristian's WIFI 2.4GHz"
#define WIFI_PASSWORD "19722508"
#define API_KEY "AIzaSyD2eMaEgXFGrDWyI-vFEyS5nhxU3JbNCuM"
#define USER_EMAIL "nagymihaicristian@yahoo.ro"
#define USER_PASSWORD "123456"
#define DATABASE_URL "https://licenta-smart-home-default-rtdb.europe-west1.firebasedatabase.app"

#define LED_PIN 13
#define SERVO_PIN 12
#define LAMP_PIN 25
#define PRIZA_PIN 33     
#define DHTPIN 27
#define PIR_PIN 26
#define MQ_ANALOG_PIN 34
#define DHTTYPE DHT11
#define REED_SWITCH_PIN 14

// RFID pins
#define SS_PIN 21    // SDA
#define RST_PIN 22   // RST
#define MOSI_PIN 23  // MOSI
#define MISO_PIN 19  // MISO
#define SCK_PIN 18   // SCK

MFRC522 rfid(SS_PIN, RST_PIN);
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
Servo myServo;
BluetoothSerial SerialBT;
DHT dht(DHTPIN, DHTTYPE);

// Timing variables
unsigned long lastEnvUpdate = 0;
unsigned long lastFirebaseUpdate = 0;
unsigned long lastHistoryUpdate = 0;
unsigned long lastDoorCheck = 0;
unsigned long barrierOpenTime = 0;
unsigned long lastCardRead = 0;
unsigned long lastScheduleCheck = 0;  // ADAUGĂ ACEASTĂ LINIE
const unsigned long envUpdateInterval = 10000;
const unsigned long firebaseUpdateInterval = 1000;
const unsigned long historyUpdateInterval = 300000;
const unsigned long doorCheckInterval = 100;
const unsigned long barrierAutoCloseTime = 15000; 
const unsigned long cardReadDelay = 2000; 
const unsigned long scheduleCheckInterval = 1000; // ADAUGĂ ACEASTĂ LINIE

// Device states
bool ledOn = false;
bool lampOn = false;
bool prizaOn = false;         
bool doorOpen = false;
bool barrierOpened = false;
bool isRegistering = false;
unsigned long ledStartTime = 0;
unsigned long lampStartTime = 0;
unsigned long prizaStartTime = 0; 
float totalLedHours = 0;
float totalLampHours = 0;
float totalPrizaHours = 0;     

void setup() {
  Serial.begin(115200);
  SerialBT.begin("SmartHomeESP32");
  Serial.println(F("Pornire Smart Home ESP32..."));
  Serial.print("PIR Pin (26) initial state: ");
  Serial.println(digitalRead(PIR_PIN) ? "HIGH" : "LOW");
  Serial.println(F("=== DEVICE CODE ==="));
  Serial.println(F(" Pentru conectarea aplicației folosiți codul:"));
  Serial.println(F(" SMART2025"));
  Serial.println(F("=================="));
  SerialBT.println("=== DEVICE CODE ===");
  SerialBT.println("Cod pentru autentificare: SMART2025");
  SerialBT.println("==================");
  // Initialize pins
  pinMode(LED_PIN, OUTPUT);
  pinMode(LAMP_PIN, OUTPUT);
  pinMode(PRIZA_PIN, OUTPUT);    
  pinMode(PIR_PIN, INPUT);
  pinMode(MQ_ANALOG_PIN, INPUT);
  pinMode(REED_SWITCH_PIN, INPUT_PULLUP);  

  // Initialize servo
  myServo.setPeriodHertz(50);
  myServo.attach(SERVO_PIN, 500, 2400);
  myServo.write(90);

  // Initialize SPI with correct pins
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);
  rfid.PCD_Init();
  Serial.println("Testare Comunicare RFID...");
  byte version = rfid.PCD_ReadRegister(rfid.VersionReg);
  Serial.print("RFID Versiune: 0x");
  Serial.println(version, HEX);

  if (version == 0x00 || version == 0xFF) {
    Serial.println("Eroare: RFID nu este conectat sau are o problemă...");
    Serial.println("Verifică firele și sursa");
  } else if (version == 0x92) {
    Serial.println("SUCCES: Original RC522 detectat!");
  } else if (version == 0x18 || version == 0x12) {
    Serial.println("SUCCESS: Clonă RC522 compatibilă detectată!");
  } else {
    Serial.print("Atentie! Versiune nepotrivită: 0x");
    Serial.println(version, HEX);
  }
  rfid.PCD_Init(); // Re-init pentru siguranță
  
  // Optimizare pentru clone RC522
  rfid.PCD_SetRegisterBitMask(rfid.TxControlReg, 0x03); // Enable TX1 și TX2
  delay(100);
  
  Serial.println("RFID initializat");
  
  dht.begin();

  // Connect to WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(F("."));
  }
  Serial.println(F("\nWiFi conectat!"));

  // Configure time
  configTime(0, 0, "pool.ntp.org");
  setenv("TZ", "EET-2EEST,M3.5.0/3,M10.5.0/4", 1);
  tzset();

  // Initialize Firebase
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;
  
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  Firebase.RTDB.setString(&fbdo, "/config/accessKey", "SMART2025");
  Serial.println(F("Cheia de acces a fost setata in Firebase"));
  while (!Firebase.ready()) {
    delay(500);
    Serial.print(F("."));
  }
  Serial.println(F("Firebase connectat!"));
  Serial.println(F("Senzor usa initializat"));
  
  // Initialize RFID status
  Firebase.RTDB.setString(&fbdo, "/rfid/status", "WAITING");
  Firebase.RTDB.setBool(&fbdo, "/rfid/isRegistering", false);
  
  Serial.println(F("Sistem pregatit!"));
}

void loop() {
  if (!Firebase.ready() || WiFi.status() != WL_CONNECTED) {
    if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
    delay(1000);
    return;
  }

  if (millis() - lastFirebaseUpdate > firebaseUpdateInterval) {
    lastFirebaseUpdate = millis();
    handleFirebaseStates();
    checkRegistrationMode();
  }

   // Verifică automatizările programate la fiecare minut
  if (millis() - lastScheduleCheck > scheduleCheckInterval) {
    lastScheduleCheck = millis();
    checkScheduledAutomations();
  }
  if (millis() - lastEnvUpdate > envUpdateInterval) {
    lastEnvUpdate = millis();
    updateEnvironmentData();
  }

  if (millis() - lastHistoryUpdate > historyUpdateInterval) {
    lastHistoryUpdate = millis();
    updateHistoryData();
  }

  // Verifică senzor ușă
  if (millis() - lastDoorCheck > doorCheckInterval) {
    lastDoorCheck = millis();
    handleDoorSensor();
  }

  // Închidere barieră automat
  if (barrierOpened && millis() - barrierOpenTime > barrierAutoCloseTime) {
    Firebase.RTDB.setString(&fbdo, "/barrier/status/state", "CLOSE");
    barrierOpened = false;
    Serial.println(F("Inchidere automata bariera"));
  }

  handleBluetooth();
  handleRFID();
  handleMotionSensor();
  
  delay(50);
}
///////////
void checkScheduledAutomations() {
  time_t now;
  time(&now);
  struct tm* timeinfo = localtime(&now);
  
  int currentHour = timeinfo->tm_hour;
  int currentMinute = timeinfo->tm_min;
  
  String currentDay = "";
  switch(timeinfo->tm_wday) {
    case 0: currentDay = "Sunday"; break;
    case 1: currentDay = "Monday"; break;
    case 2: currentDay = "Tuesday"; break;
    case 3: currentDay = "Wednesday"; break;
    case 4: currentDay = "Thursday"; break;
    case 5: currentDay = "Friday"; break;
    case 6: currentDay = "Saturday"; break;
  }
  
  
  if (Firebase.RTDB.get(&fbdo, "/schedules")) {
    if (fbdo.dataType() == "json") {
      FirebaseJson schedules;
      schedules.setJsonData(fbdo.jsonString());
      
      FirebaseJsonData result;
      size_t len = schedules.iteratorBegin();
      String key, value = "";
      int type = 0;
      
      for (size_t i = 0; i < len; i++) {
        schedules.iteratorGet(i, type, key, value);
        
        if (type == FirebaseJson::JSON_OBJECT) {
          FirebaseJson schedule;
          schedule.setJsonData(value);
          
          FirebaseJsonData enabledData, hourData, minuteData, deviceData, stateData;
          schedule.get(enabledData, "enabled");
          schedule.get(hourData, "hour");
          schedule.get(minuteData, "minute");
          schedule.get(deviceData, "device");
          schedule.get(stateData, "state");
          
          if (enabledData.success && enabledData.boolValue &&
              hourData.success && minuteData.success &&
              hourData.intValue == currentHour && minuteData.intValue == currentMinute &&
              deviceData.success && stateData.success) {
            
            // Verifică dacă astăzi este în zilele programate
            FirebaseJsonData daysData;
            schedule.get(daysData, "days");
            
            if (daysData.success) {
              FirebaseJsonArray daysArray;
              daysArray.setJsonArrayData(daysData.stringValue);
              
              for (size_t j = 0; j < daysArray.size(); j++) {
                FirebaseJsonData dayData;
                daysArray.get(dayData, j);
                if (dayData.success && dayData.stringValue == currentDay) {
                  // Verifică să nu fi fost executată în ultimul minut
                  FirebaseJsonData lastExecutedData;
                  schedule.get(lastExecutedData, "lastExecuted");
                  
                  unsigned long long currentTimestamp = (unsigned long long)now * 1000ULL;
                  unsigned long long lastExecuted = 0;
                  
                  if (lastExecutedData.success) {
                    lastExecuted = strtoull(lastExecutedData.stringValue.c_str(), NULL, 10);
                  }
                  
                  // Execută doar dacă nu a fost executată în ultimul minut
                  if (currentTimestamp - lastExecuted > 60000) {
                    // EXECUTĂ AUTOMATIZAREA
                    String devicePath = "/" + deviceData.stringValue + "/status/state";
                    
                    if (Firebase.RTDB.setString(&fbdo, devicePath, stateData.stringValue)) {
                      // Actualizează timestamp-ul ultimei execuții
                      Firebase.RTDB.setString(&fbdo, "/schedules/" + key + "/lastExecuted", String(currentTimestamp));
                      
                      Serial.println(" AUTOMATIZARE EXECUTATĂ: " + deviceData.stringValue + " -> " + stateData.stringValue);
                      SerialBT.println(" Automatizare executată: " + deviceData.stringValue + " -> " + stateData.stringValue);
                    } else {
                      Serial.println(" Eroare la executarea automatizării");
                    }
                  }
                  break;
                }
              }
            }
          }
        }
      }
      schedules.iteratorEnd();
    }
  } else {
    Serial.println(" Nu s-au putut încărca automatizările din Firebase");
  }
}



void checkRegistrationMode() {
  if (Firebase.RTDB.getBool(&fbdo, "/rfid/isRegistering")) {
    isRegistering = fbdo.boolData();
  }
}

void handleFirebaseStates() {
  static String lastLedState = "";
  static String lastLampState = "";
  static String lastPrizaState = "";  
  static String lastBarrierState = "";
  
  if (Firebase.RTDB.getString(&fbdo, "/led/status/state")) {
    String ledState = fbdo.stringData();
    if (ledState != lastLedState) {
      digitalWrite(LED_PIN, ledState == "ON" ? HIGH : LOW);
      handleDeviceState("led", ledState == "ON", ledOn, ledStartTime, totalLedHours);
      lastLedState = ledState;
    }
  }

  if (Firebase.RTDB.getString(&fbdo, "/lamp/status/state")) {
    String lampState = fbdo.stringData(); 
    if (lampState != lastLampState) {
      digitalWrite(LAMP_PIN, lampState == "ON" ? HIGH : LOW);
      handleDeviceState("lamp", lampState == "ON", lampOn, lampStartTime, totalLampHours);
      lastLampState = lampState;
    }
  }

  //Control priză
  if (Firebase.RTDB.getString(&fbdo, "/priza/status/state")) {
    String prizaState = fbdo.stringData();
    if (prizaState != lastPrizaState) {
      digitalWrite(PRIZA_PIN, prizaState == "ON" ? HIGH : LOW);
      handleDeviceState("priza", prizaState == "ON", prizaOn, prizaStartTime, totalPrizaHours);
      lastPrizaState = prizaState;
    }
  }

  if (Firebase.RTDB.getString(&fbdo, "/barrier/status/state")) {
    String barrierState = fbdo.stringData();
    if (barrierState != lastBarrierState) {
      if (barrierState == "OPEN") {
        myServo.write(0);
        barrierOpened = true;
        barrierOpenTime = millis();
        Serial.println(F("Bariera Deschisa"));
      } else {
        myServo.write(90);
        barrierOpened = false;
        Serial.println(F("Bariera Inchisa"));
      }
      lastBarrierState = barrierState;
    }
  }
} 

void handleDeviceState(const char* device, bool isOn, bool& onFlag, unsigned long& startTime, float& totalHours) {
  if (isOn) {
    if (!onFlag) {
      onFlag = true;
      startTime = millis();
      Serial.println(String(device) + " turned ON");
    }
  } else {
    if (onFlag) {
      float hours = (millis() - startTime) / (1000.0 * 60.0 * 60.0);
      totalHours += hours;
      
   
      String timestamp = String((uint64_t)time(nullptr) * 1000ULL);
      Firebase.RTDB.setFloat(&fbdo, "/history/" + String(device) + "/" + timestamp, hours);
      
      Serial.println(String(device) + " was ON for: " + String(hours, 4) + " hours");
      
      onFlag = false;
    }
  }
}

void handleDoorSensor() {
  static bool lastDoorState = true; // true = închis (pull-up)
  bool currentDoorState = digitalRead(REED_SWITCH_PIN);
  
  if (currentDoorState != lastDoorState) {
    if (currentDoorState == LOW) {
      // ușă închisă
      doorOpen = false;
      Firebase.RTDB.setString(&fbdo, "/sensors/door/status", "CLOSED");
      Serial.println(F("Usa inchisa"));
    } else {
      // ușă deschisă
      doorOpen = true;
      Firebase.RTDB.setString(&fbdo, "/sensors/door/status", "OPEN");
      Serial.println(F("Usa deschisa"));
    }
    
    // Save timestamp
    Firebase.RTDB.setInt(&fbdo, "/sensors/door/timestamp", (uint64_t)time(nullptr) * 1000ULL);
    
    lastDoorState = currentDoorState;
  }
}

void handleBluetooth() {
  if (SerialBT.available()) {
    String cmd = SerialBT.readStringUntil('\n');
    cmd.trim();
    
    if (cmd == "LED ON") {
      Firebase.RTDB.setString(&fbdo, "/led/status/state", "ON");
    } else if (cmd == "LED OFF") {
      Firebase.RTDB.setString(&fbdo, "/led/status/state", "OFF");
    } else if (cmd == "LAMP ON") {
      Firebase.RTDB.setString(&fbdo, "/lamp/status/state", "ON");
    } else if (cmd == "LAMP OFF") {
      Firebase.RTDB.setString(&fbdo, "/lamp/status/state", "OFF");
    } else if (cmd == "PRIZA ON") {       // ADĂUGAT
      Firebase.RTDB.setString(&fbdo, "/priza/status/state", "ON");
    } else if (cmd == "PRIZA OFF") {      // ADĂUGAT
      Firebase.RTDB.setString(&fbdo, "/priza/status/state", "OFF");
    } else if (cmd == "BARRIER OPEN") {
      Firebase.RTDB.setString(&fbdo, "/barrier/status/state", "OPEN");
    } else if (cmd == "BARRIER CLOSE") {
      Firebase.RTDB.setString(&fbdo, "/barrier/status/state", "CLOSE");
    } else if (cmd == "DOOR") {
      SerialBT.println("Door: " + String(doorOpen ? "OPEN" : "CLOSED"));
    } else if (cmd == "RFID REG ON") {
      Firebase.RTDB.setBool(&fbdo, "/rfid/isRegistering", true);
      Firebase.RTDB.setString(&fbdo, "/rfid/status", "REGISTERING");
      SerialBT.println("RFID Registration Mode ON");
    } else if (cmd == "RFID REG OFF") {
      Firebase.RTDB.setBool(&fbdo, "/rfid/isRegistering", false);
      Firebase.RTDB.setString(&fbdo, "/rfid/status", "WAITING");
      SerialBT.println("RFID Registration Mode OFF");
    } else if (cmd == "RFID TEST") {
      Serial.println("Testing RFID...");
      byte version = rfid.PCD_ReadRegister(rfid.VersionReg);
      Serial.print("RFID Version: 0x");
      Serial.println(version, HEX);
      SerialBT.println("RFID Version: 0x" + String(version, HEX));
      if (version == 0x18 || version == 0x92 || version == 0x12) {
        SerialBT.println("RFID: OK - Ready for cards");
      } else {
        SerialBT.println("RFID: ERROR - Check connections!");
      }
    } else if (cmd == "STATUS") {
      sendStatus();
    }
  }
}

void sendStatus() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  
  SerialBT.println("=== STATUS ===");
  SerialBT.println("LED: " + String(digitalRead(LED_PIN) ? "ON" : "OFF"));
  SerialBT.println("Lamp: " + String(digitalRead(LAMP_PIN) ? "ON" : "OFF"));
  SerialBT.println("Priza: " + String(digitalRead(PRIZA_PIN) ? "ON" : "OFF"));  // ADĂUGAT
  SerialBT.println("Motion: " + String(digitalRead(PIR_PIN) ? "YES" : "NO"));
  SerialBT.println("Door: " + String(doorOpen ? "OPEN" : "CLOSED"));
  SerialBT.println("RFID Mode: " + String(isRegistering ? "REGISTERING" : "WAITING"));
  SerialBT.println("RFID Version: 0x" + String(rfid.PCD_ReadRegister(rfid.VersionReg), HEX));
  SerialBT.println("Temp: " + String(t) + "C");
  SerialBT.println("Humidity: " + String(h) + "%");
  SerialBT.println("LED Hours: " + String(totalLedHours, 2));
  SerialBT.println("Lamp Hours: " + String(totalLampHours, 2));
  SerialBT.println("Priza Hours: " + String(totalPrizaHours, 2));  // ADĂUGAT
  SerialBT.println("=============");
}

void handleRFID() {
  // Verifică des pentru carduri noi cu delay între citiri
  if (millis() - lastCardRead < cardReadDelay) {
    return; // Prea curând pentru o nouă citire
  }
  
  if (!rfid.PICC_IsNewCardPresent()) {
    return;
  }
  
  Serial.println(" Card detectat!");
  
  // Încearcă să citești cardul
  if (!rfid.PICC_ReadCardSerial()) {
    Serial.println(" Nu se poate citi cardul!");
    return;
  }
  
  Serial.println("Card citit cu succes!");
  lastCardRead = millis(); // Actualizează timpul ultimei citiri
  
  // Convert UID to string
  String cardID = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) cardID += "0";
    cardID += String(rfid.uid.uidByte[i], HEX);
  }
  cardID.toUpperCase();
  
  Serial.println("Card RFID detectat: " + cardID);
  
  // Trimite la Firebase
  Firebase.RTDB.setString(&fbdo, "/rfid/lastCard", cardID);
  Firebase.RTDB.setString(&fbdo, "/rfid/status", "DETECTED");
  
  delay(500); // Mică pauză pentru a arăta detecția
  
  if (isRegistering) {
    registerNewCard(cardID);
  } else {
    checkCardAuthorization(cardID);
  }
  
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

void registerNewCard(String cardID) {
  Serial.println("Inregistrare card nou: " + cardID);
  
  // Crează card
  FirebaseJson cardData;
  cardData.set("name", "Card " + cardID.substring(0, 4));
  cardData.set("access", true);
  cardData.set("registeredAt", (uint64_t)time(nullptr) * 1000ULL);
  
  // Salvează în Firebase
  if (Firebase.RTDB.setJSON(&fbdo, "/rfid/authorizedCards/" + cardID, &cardData)) {
    Serial.println("Card inregistrat cu succes!");
    Firebase.RTDB.setString(&fbdo, "/rfid/status", "AUTHORIZED");
    Firebase.RTDB.setString(&fbdo, "/rfid/authorizedUser", "Card " + cardID.substring(0, 4));
    
    // Închidere mod înregistrare
    Firebase.RTDB.setBool(&fbdo, "/rfid/isRegistering", false);
    isRegistering = false;
    
    // Deschide barieră pentru card nou
    //Firebase.RTDB.setString(&fbdo, "/barrier/status/state", "OPEN");
    
    SerialBT.println("Card nou inregistrat: " + cardID);
  } else {
    Serial.println("Nu s-a putut inregistra cardul");
    Firebase.RTDB.setString(&fbdo, "/rfid/status", "DENIED");
    SerialBT.println("Inregistrare esuata pentru: " + cardID);
  }
}

void checkCardAuthorization(String cardID) {
  Serial.println("Verificare autorizatie pentru: " + cardID);
  
  // Verifică dacă cardul este autorizat
  if (Firebase.RTDB.get(&fbdo, "/rfid/authorizedCards/" + cardID)) {
    if (fbdo.dataType() == "json") {
      FirebaseJson cardData;
      cardData.setJsonData(fbdo.jsonString());
      
      FirebaseJsonData result;
      cardData.get(result, "access");
      
      if (result.success && result.boolValue) {
        // Card autorizat
        Serial.println("RFID: card autorizat!");
        Firebase.RTDB.setString(&fbdo, "/rfid/status", "AUTHORIZED");
        
        // Obține numele cardului
        cardData.get(result, "name");
        if (result.success) {
          Firebase.RTDB.setString(&fbdo, "/rfid/authorizedUser", result.stringValue);
          Serial.println("Utilizator: " + result.stringValue);
        }
        
        // Deschidere barieră
        Firebase.RTDB.setString(&fbdo, "/barrier/status/state", "OPEN");
        
        SerialBT.println("Acces autorizat: " + cardID);
      } else {
        // Acces respins
        Serial.println("RFID: acces interzis");
        Firebase.RTDB.setString(&fbdo, "/rfid/status", "DENIED");
        Firebase.RTDB.setString(&fbdo, "/rfid/authorizedUser", "");
        SerialBT.println("Acces interzis: " + cardID);
      }
    } else {
      // Cardul nu există
      Serial.println("RFID: Card necunoscut");
      Firebase.RTDB.setString(&fbdo, "/rfid/status", "DENIED");
      Firebase.RTDB.setString(&fbdo, "/rfid/authorizedUser", "");
      SerialBT.println("Card necunoscut: " + cardID);
    }
  } else {
    // Eroare Firebase sau cardul nu există
    Serial.println("RFID: Card neautorizat!");
    Firebase.RTDB.setString(&fbdo, "/rfid/status", "DENIED");
    Firebase.RTDB.setString(&fbdo, "/rfid/authorizedUser", "");
    SerialBT.println("Card neautorizat: " + cardID);
  }
  
  // Resetare status după 3 secunde
  delay(3000);
  Firebase.RTDB.setString(&fbdo, "/rfid/status", "WAITING");
  Firebase.RTDB.setString(&fbdo, "/rfid/authorizedUser", "");
}

void updateEnvironmentData() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  int gas = analogRead(MQ_ANALOG_PIN);

  if (!isnan(h) && !isnan(t)) {
    FirebaseJson env;
    env.set("temperature", t);
    env.set("humidity", h);
    env.set("gas", gas);
    env.set("timestamp", (uint64_t)time(nullptr) * 1000ULL);
    
    Firebase.RTDB.updateNode(&fbdo, "/environment", &env);
  }
}

void updateHistoryData() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  
  if (!isnan(h) && !isnan(t)) {
    String ts = String((uint64_t)time(nullptr) * 1000ULL);
    
    // Salvează temperatură și umiditate
    Firebase.RTDB.setFloat(&fbdo, "/history/temperature/" + ts, t);
    Firebase.RTDB.setFloat(&fbdo, "/history/humidity/" + ts, h);
    Serial.println("Upate istoric- T: " + String(t) + "°C, H: " + String(h) + "%");
  }
}

void handleMotionSensor() {
  static String lastMotion = "";
  String motion = digitalRead(PIR_PIN) ? "YES" : "NO";
  
  if (motion != lastMotion) {
    Firebase.RTDB.setString(&fbdo, "/motion/status", motion);
    lastMotion = motion;
  }
}