#include "ADS1X15.h"
#include "SPIFFS.h"
#include "filehandling.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <AsyncTCP.h>
#include <ESP32Servo.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <PID_v1.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_task_wdt.h>
#include <esp_wifi.h>

// Watchdog timeout in milliseconds
const int WATCHDOG_TIMEOUT = 8000;

// Variables to save values from HTML form
String slaveString;
String firstString;
// String ip = "192.168.1.200";
// String gateway = "192.168.1.1";

// State variables for setting up device
bool slave;
bool first;

// Variables for ESP-NOW
String macString;
String macAddresses[] = {"", "", "", "", ""};
uint8_t broadcastAddress[6];

// File paths to save input values permanently
const char *jsonWifiPath = "/wifi.json";
const char *jsonConfigPath = "/config.json";
const char *jsonAddressesPath = "/addresses.json";

// Setting hostname
const char *hostname = "hydrofoil-control";

// "Watchdog" variable for the filesystem
boolean restart = false;

// Define pins for various components
const int ledPin1 = GPIO_NUM_8;
const int resetPin = GPIO_NUM_5;

// Create web server
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// Variables for ESP-NOW
typedef struct dataStruct {
  float p;
  float i;
  float d;
  int setpoint;
} dataStruct;

dataStruct pidParamsSend;
dataStruct pidParamsReceive;
esp_now_peer_info_t peerInfo;

// Variable to get the channel of the AP
const char WIFI_SSID[] = "Hydrofoil-Control";

// RC servo
Servo elevator;

// PID controller
double setpoint, input, output, kp, ki, kd;
// Target is a setpoint value, sets the nominal sinking of the vehicle
double target = 128;
PID elevatorPid(&input, &output, &setpoint, kp, ki, kd, DIRECT);

// Min 50, max 130, mid 90
int servoPin = GPIO_NUM_1;
int servoMin = 50;
int servoMax = 130;
int servoMid = 90;

// External ADC and measurement
ADS1115 waterLevelSensor(0x48);

long sensorLongValue = 0;
long sensorShortValue = 0;
float sensorRatio = 0;

volatile bool ready = false;
int readyPin = GPIO_NUM_0;
bool selector = false;

// Cycle time
unsigned long prevTime = 0;
unsigned long currTime = 0;

// I2C pin definition
int sdaPin = GPIO_NUM_7;
int sclPin = GPIO_NUM_6;

// PWM Input variables
int pwmPin = GPIO_NUM_3;
unsigned long pwmRead = 0;
int pwmValue = 0;
float control = 0;
int factor = 20;
volatile unsigned long pulsInTimeBegin;
volatile unsigned long pulsInTimeEnd;
volatile bool newPulseDurationAvailable = false;

// Callbacks for ESP-NOW send and Receive
void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.println(status == ESP_NOW_SEND_SUCCESS
                     ? "Packet delivered successfully"
                     : "Packet delivery failed");
};

void onDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
  memcpy(&pidParamsReceive, incomingData, sizeof(pidParamsReceive));
}

// Convert string to bool
bool stringToBool(String state) {
  if (state == "true" || state == "1" || state == "True") {
    return true;
  } else
    return false;
}
// Send addresses from string array through websocket
void sendAddresses() {
  ws.printfAll("{\"broadcastAddress0\":\"%s\",\"broadcastAddress1\":\"%s\","
               "\"broadcastAddress2\":\"%s\",\"broadcastAddress3\":\"%s\","
               "\"broadcastAddress4\":\"%s\"}",
               macAddresses[0].c_str(), macAddresses[1].c_str(),
               macAddresses[2].c_str(), macAddresses[3].c_str(),
               macAddresses[4].c_str());
  Serial.println("MAC addresses have been sent on websocket");
}

void updateSliders() {
  ws.printfAll("{\"slider-p\":\"%.1f\",\"slider-i\":\"%.2f\","
               "\"slider-d\":\"%.2f\",\"slider-setpoint\":\"%d\"}",
               pidParamsSend.p, pidParamsSend.i, pidParamsSend.d,
               pidParamsSend.setpoint);
  Serial.println("Slider values have been sent on websocket");
}

// Send data through websocket when page reloaded
void sendData() {
  ws.printfAll("{\"mac\":\"%s\"}", WiFi.macAddress().c_str());
  sendAddresses();
  updateSliders();
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
               AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    client->ping();
    sendData();
  } else if (type == WS_EVT_DISCONNECT) {
  } else if (type == WS_EVT_ERROR) {
  } else if (type == WS_EVT_PONG) {
  }
}

// Convert MAC string into intiger array
void stringToMac(String macString, u_int8_t *array) {
  int j = 0;
  for (int i = 0; i < macString.length(); i = i + 3) {
    String hexPairString = macString.substring(i, i + 2);
    array[j] = strtol(hexPairString.c_str(), nullptr, 16);
    Serial.print(array[j]);
    j++;
    if (j < 6)
      Serial.print(":");
    else
      Serial.println("");
  }
};

// Get the channel of the AP
int32_t getWiFiChannel(const char *ssid) {
  if (int32_t n = WiFi.scanNetworks()) {
    for (uint8_t i = 0; i < n; i++) {
      if (!strcmp(ssid, WiFi.SSID(i).c_str())) {
        return WiFi.channel(i);
      }
    }
  }
  return 0;
}

// Add new ESP-NOW peer
void addNewPeerEspNow() {
  Serial.println("Adding new peer");
  // Add new address to the peerInfo
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);

  // Specify  channel and encryption
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add peer");
    return;
  } else
    Serial.println("Peer added");
}

// Init ESP-NOW
void initEspNow() {
  esp_err_t resultOfEspNowInit = esp_now_init();
  Serial.println("Result of esp_now_init:");
  Serial.println(resultOfEspNowInit);
  if (resultOfEspNowInit != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  } else
    Serial.println("ESP-NOW initialized");
}

// Send data via ESP-NOW
void sendEspNow() {
  for (int i = 0; i <= 4; i++) {
    Serial.println("Sending data");
    esp_err_t resultOfSend =
        esp_now_send(0, (uint8_t *)&pidParamsSend, sizeof(dataStruct));
    if (resultOfSend == ESP_OK) {
      Serial.println("Sent successfully");
      break;
    } else if (i == 4) {
      Serial.println("Faild to send data");
      Serial.println("Result of esp_now_send (Master):");
      Serial.println(
          resultOfSend); // Returns 12393 (0x3069): ESP_ERR_ESPNOW_NOT_FOUND
                         // (0x3069): ESPNOW peer is not found
    } else
      Serial.println("Resending data");
  }
}

// Reset device
void resetMacAddresses() {
  for (int i = 0; i < sizeof(macAddresses) / sizeof(macAddresses[0]); i++) {
    macAddresses[i] = "";
  }
  writeArrayJson(SPIFFS, jsonAddressesPath, "addresses", macAddresses,
                 sizeof(macAddresses) / sizeof(macAddresses[0]));
  Serial.println("MAC Addresses have been deleted");
}
void resetDevice() {
  firstString = "True";
  slaveString = "False";

  pidParamsSend.p = 0;
  pidParamsSend.i = 0;
  pidParamsSend.d = 0;
  pidParamsSend.setpoint = 0;

  pidParamsReceive.p = 0;
  pidParamsReceive.i = 0;
  pidParamsReceive.d = 0;
  pidParamsReceive.setpoint = 0;

  writeFileJson(SPIFFS, jsonWifiPath, "FIRST", firstString.c_str());
  writeFileJson(SPIFFS, jsonWifiPath, "SLAVE", slaveString.c_str());

  writeFileJson(SPIFFS, jsonConfigPath, "p", String(pidParamsSend.p).c_str());
  writeFileJson(SPIFFS, jsonConfigPath, "i", String(pidParamsSend.i).c_str());
  writeFileJson(SPIFFS, jsonConfigPath, "d", String(pidParamsSend.d).c_str());
  writeFileJson(SPIFFS, jsonConfigPath, "setpoint",
                String(pidParamsSend.setpoint).c_str());

  Serial.println("Device has been reset");
}

// Function called when sensor ready triggers an interrupt
void waterLevelSensorReady() { ready = true; };

void readWaterLevel() {
  if (!selector) {
    waterLevelSensor.requestADC(0); // Request a measurement on A0
    sensorLongValue = waterLevelSensor.getValue();
    selector = true;
    Serial.print("Long: ");
    Serial.println(sensorLongValue);
  }

  else {
    waterLevelSensor.requestADC(1);
    sensorShortValue = waterLevelSensor.getValue();
    selector = false;
    Serial.print("Short: ");
    Serial.println(sensorShortValue);
  }

  ready = false;
};

// Interrupt for non-blocking PWM reading
void pwmPinInterrupt() {
  if (digitalRead(pwmPin) == HIGH) {
    pulsInTimeBegin = micros();
  } else {
    pulsInTimeEnd = micros();
    newPulseDurationAvailable = true;
  }
};

void setupWifiFirst() {

  Serial.println("Setting AP (Access Point)");
  WiFi.mode(WIFI_MODE_NULL);
  WiFi.setHostname("hydrofoil-control");
  // NULL sets an open Access Point
  WiFi.softAP("Hydrofoil-Control-WiFi-Manager", NULL);

  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(IP);

  // Web Server Root URL
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/wifimanager.html", "text/html");
  });

  server.serveStatic("/", SPIFFS, "/");

  server.on("/", HTTP_POST, [](AsyncWebServerRequest *request) {
    slaveString = "False";
    writeFileJson(SPIFFS, jsonWifiPath, "SLAVE", slaveString.c_str());
    int params = request->params();
    for (int i = 0; i < params; i++) {
      AsyncWebParameter *p = request->getParam(i);
      if (p->isPost()) {

        // HTTP POST slave value
        if (p->name() == "slave") {
          slaveString = "True";
          Serial.print("Slave device: ");
          Serial.println(slaveString);
          // Write file to save value
          writeFileJson(SPIFFS, jsonWifiPath, "SLAVE", slaveString.c_str());
        }
      }
    }
    restart = true;
    request->send(200, "text/plain",
                  "Done. ESP will restart, and create Master hotspot, or "
                  "connect as a Slave.");
    firstString = "False";
    writeFileJson(SPIFFS, jsonWifiPath, "FIRST", firstString.c_str());
  });
  server.begin();
};

void setupWifiMaster() {
  Serial.println("Setting AP (Access Point)");
  // NULL sets an open Access Point
  WiFi.softAP("Hydrofoil-Control", NULL);

  IPAddress IP = WiFi.softAPIP();
  WiFi.mode(WIFI_AP_STA);
  Serial.print("AP IP address: ");
  Serial.println(IP);

  // Web Server Root URL
  server.serveStatic("/", SPIFFS, "/");

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/index.html", "text/html");
  });

  server.on("/set-sliders", HTTP_POST, [](AsyncWebServerRequest *request) {
    // Check if all required parameters are present
    if (request->hasParam("slider-p", true) &&
        request->hasParam("slider-i", true) &&
        request->hasParam("slider-d", true) &&
        request->hasParam("slider-setpoint", true)) {

      // Extract parameters
      pidParamsSend.p = request->getParam("slider-p", true)->value().toFloat();
      pidParamsSend.i = request->getParam("slider-i", true)->value().toFloat();
      pidParamsSend.d = request->getParam("slider-d", true)->value().toFloat();
      pidParamsSend.setpoint =
          request->getParam("slider-setpoint", true)->value().toInt();

      // Send success response
      request->send(200, "text/plain", "OK");
    } else {
      // Send error response
      request->send(400, "text/plain", "Invalid parameters");
    }

    Serial.print("P value: ");
    Serial.println(pidParamsSend.p);
    Serial.print("I value: ");
    Serial.println(pidParamsSend.i);
    Serial.print("D value: ");
    Serial.println(pidParamsSend.d);
    Serial.print("Setpoint value: ");
    Serial.println(pidParamsSend.setpoint);

    writeFileJson(SPIFFS, jsonConfigPath, "p", String(pidParamsSend.p).c_str());
    writeFileJson(SPIFFS, jsonConfigPath, "i", String(pidParamsSend.i).c_str());
    writeFileJson(SPIFFS, jsonConfigPath, "d", String(pidParamsSend.d).c_str());
    writeFileJson(SPIFFS, jsonConfigPath, "setpoint",
                  String(pidParamsSend.setpoint).c_str());
    sendEspNow();
  });

  server.on("/add-mac", HTTP_POST, [](AsyncWebServerRequest *request) {
    // Check if all required parameters are present
    if (request->hasParam("mac", true)) {
      // Extract parameters
      String macString = request->getParam("mac", true)->value();

      Serial.println("MAC Address from POST request");
      Serial.println(macString);

      bool existing = false;
      // Find empty space in the array, store the value then add new peer
      for (int i = 0; i < sizeof(macAddresses) / sizeof(macAddresses[0]); i++) {
        if (macString == macAddresses[i])
          existing = true;
        if (macAddresses[i] == "" && !existing) {
          macAddresses[i] = macString;
          stringToMac(macAddresses[i], broadcastAddress);
          break;
        } else if (existing) {
          Serial.println("Address already added");
          Serial.println(macAddresses[i]);
          break;
        }
      }

      // Store value in the JSON file, add new ESP NOW peer
      if (!existing) {
        writeArrayJson(SPIFFS, jsonAddressesPath, "addresses", macAddresses,
                       sizeof(macAddresses) / sizeof(macAddresses[0]));
        Serial.println("New MAC address stored");

        addNewPeerEspNow();
      }

      // Send success response
      request->send(200, "text/plain", "OK");
    } else {
      // Send error response
      request->send(400, "text/plain", "Invalid parameters");
    }
  });

  server.begin();

  // Init ESP-NOW
  initEspNow();

  // Once ESP-NOW is successfully Init, register sender callback
  esp_err_t resultOfRegisterSend = esp_now_register_send_cb(onDataSent);
  Serial.println("Result of esp_now_register_send_cb:");
  Serial.println(resultOfRegisterSend);

  // Set up peers at boot up
  for (int i = 0; i < sizeof(macAddresses) / sizeof(macAddresses[0]); i++) {

    if (macAddresses[i] != "") {
      stringToMac(macAddresses[i], broadcastAddress);
      addNewPeerEspNow();
    }
  }

  Serial.println("Master has started");
};

void setupWifiSlave() {
  // Init ESP-NOW
  Serial.println("Start slave initialization");
  WiFi.mode(WIFI_STA);

  // Get the channel of the Master AP and set Slave's channel accordingly
  int32_t channel = getWiFiChannel(WIFI_SSID);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
  Serial.println("WiFi diagnostics");
  WiFi.printDiag(Serial);

  initEspNow();

  // Once ESP-NOW is successfully Init, register receiver callback
  esp_err_t resultOfRegisterReveived = esp_now_register_recv_cb(onDataRecv);
  Serial.println("Result of esp_now_register_recv_cb:");
  Serial.println(resultOfRegisterReveived);

  Serial.println("Slave has started");
  Serial.println("Device\'s MAC Address:");
  Serial.println(WiFi.macAddress());
}

void setup() {

  // Enable the Watchdog Timer
  esp_task_wdt_init(WATCHDOG_TIMEOUT, true);
  esp_task_wdt_add(NULL);

  // Begin serial communication
  Serial.begin(115200);

  // Configure pin modes
  pinMode(ledPin1, OUTPUT);
  pinMode(readyPin, INPUT_PULLUP);
  pinMode(pwmPin, INPUT);

  // Mount SPIFFS
  initFS();

  // Load values saved in SPIFFS
  slaveString = readFileJson(SPIFFS, jsonWifiPath, "SLAVE");
  slave = stringToBool(slaveString);
  firstString = readFileJson(SPIFFS, jsonWifiPath, "FIRST");
  first = stringToBool(firstString);

  pidParamsSend.p = readFileJson(SPIFFS, jsonConfigPath, "p").toFloat();
  pidParamsSend.i = readFileJson(SPIFFS, jsonConfigPath, "i").toFloat();
  pidParamsSend.d = readFileJson(SPIFFS, jsonConfigPath, "d").toFloat();
  pidParamsSend.setpoint =
      readFileJson(SPIFFS, jsonConfigPath, "setpoint").toInt();

  // Read MAC Addresses from JSON and store to macAddresses array
  readArrayJson(SPIFFS, jsonAddressesPath, "addresses", macAddresses);
  Serial.println("MAC addresses from SPIFFS:");
  for (int i = 0; i < sizeof(macAddresses) / sizeof(macAddresses[0]); i++) {
    if (macAddresses[i] != "")
      Serial.println(macAddresses[i]);
  }

  // Configure devices according to first and slave variables
  if (first)
    setupWifiFirst();
  else if (!slave) {
    setupWifiMaster();
  } else
    setupWifiSlave();

  if (!slave) {
    // Set up WebSocket event handler
    ws.onEvent(onWsEvent);

    // Add WebSocket handler to the server
    server.addHandler(&ws);

    // Initialize mDNS
    if (!MDNS.begin("hydrofoil-control")) {
      Serial.println("Error setting up mDNS.");
    } else {
      Serial.println("mDNS responder started");
    }
    // Add a service to mDNS
    MDNS.addService("http", "tcp", 80);
  }

  // Set up Servo
  elevator.attach(servoPin);
  elevator.write(servoMid);

  // Set up PID controller
  elevatorPid.SetMode(AUTOMATIC);

  // Set up I2C bus
  // Wire.setClock(400000); NEEDS TO BE FIXED LATER
  Wire.begin(sdaPin, sclPin);

  // Set up ADC
  waterLevelSensor.begin();
  waterLevelSensor.setGain(1);     // 2.048V
  waterLevelSensor.setDataRate(7); // 0 = slow   4 = medium   7 = fast

  // Set alert for ready interrupt
  waterLevelSensor.setComparatorThresholdHigh(0x8000);
  waterLevelSensor.setComparatorThresholdLow(0x0000);
  waterLevelSensor.setComparatorQueConvert(0);

  // Set interrupt handler to catch conversion ready
  attachInterrupt(digitalPinToInterrupt(readyPin), waterLevelSensorReady,
                  FALLING);

  waterLevelSensor.setMode(0); // Continuous mode
  // waterLevelSensor.requestADC(0);

  // Interrupt for non-blocking PWM reading
  attachInterrupt(digitalPinToInterrupt(pwmPin), pwmPinInterrupt, CHANGE);
}

void loop() {

  // Clean up and close inactive WebSocket connections
  if (!slave) {
    ws.cleanupClients();
  }

  // Reset the Watchdog Timer to prevent a system reset
  esp_task_wdt_reset();

  // Resboot ESP after SSID and PASS were set
  if (restart) {
    delay(5000);
    ESP.restart();
  }

  // Reset device
  if (digitalRead(resetPin) == LOW) {
    delay(3000);
    if (digitalRead(resetPin) == LOW) {
      // Reset MAC Addresses only after 3s
      resetMacAddresses();
      delay(3000);
      if (digitalRead(resetPin) == LOW) {
        // Reset to default after 6s
        resetDevice();
        ESP.restart();
      } else
        ESP.restart();
    }
  }

  // Master's main loop
  if (!slave && !first) {
    analogWrite(ledPin1, map(pidParamsSend.p, 0, 6, 0, 255));
    elevator.write(pidParamsSend.setpoint);

    // Slave's main loop
  } else if (!first) {
    analogWrite(ledPin1, map(pidParamsReceive.p, 0, 6, 0, 255));
    elevator.write(pidParamsReceive.setpoint);
  }
  // Read water level if sensor is ready
  if (ready) {
    readWaterLevel();
  }

  // Read PWM value, calculate control value for PID setpoint modification

  if (newPulseDurationAvailable) {
    newPulseDurationAvailable = false;
    pwmRead = pulsInTimeEnd - pulsInTimeBegin;
    if (pwmRead >= 990 && pwmRead <= 2010) {
      pwmValue = map(pwmRead, 990, 2010, 0, 255);
      control = (pwmValue - 128) / 100.0 * factor;

    } else
      control = 0;
    Serial.print("New PWM value: ");
    Serial.println(pwmValue);
  }

  // Measuring cycle time
  // currTime = millis();
  // Serial.print("Cycle time: ");
  // Serial.println(currTime - prevTime);
  // prevTime = currTime;
}
