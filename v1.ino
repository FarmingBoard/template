#include <WiFi.h>
#include <EEPROM.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <ArduinoJson.h>

// DHT sensor configuration
#define DHTPIN 4 // GPIO pin where the DHT11 is connected
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE); // Initialize DHT sensor

// EEPROM size for storing credentials
#define EEPROM_SIZE 512

// BLE service UUIDs and characteristics
#define SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

// MQTT configuration
WiFiClient espClient;
PubSubClient client(espClient);

// Wi-Fi and MQTT credentials stored in EEPROM
char wifiSSID[32];
char wifiPass[64];
char *mqttServer = "192.168.217.213";
char mqttUser[32];
char *mqttPass = "";
char *mqttTopic = "v1/devices/me/telemetry";

// Connection status flags
bool wifiConnected = false;
bool mqttConnected = false;

// BLE characteristic
BLECharacteristic *pCharacteristic;
BLEServer *pServer;  // Store BLEServer instance globally
String rxValue = ""; // Use String instead of std::string to match getValue()

// Function prototypes
void setupBLE();
void connectToWiFi(const char *ssid, const char *password);
void connectToMQTT();
void sendDHT11Data();
void onBLEReceive(String jsonData); // Change to String
void saveCredentialsToEEPROM();
void loadCredentialsFromEEPROM();

// BLE callback class to handle received credentials
class MyBLECallbacks : public BLECharacteristicCallbacks
{
  void onWrite(BLECharacteristic *pCharacteristic) override
  {
    String rxData = pCharacteristic->getValue().c_str(); // Lấy dữ liệu nhận qua BLE và chuyển thành chuỗi String
    if (rxData.length() > 0)                             // Kiểm tra dữ liệu có độ dài lớn hơn 0
    {
      Serial.println("Received credentials over BLE");
      rxValue += rxData;                           // Thêm dữ liệu nhận vào chuỗi rxValue
      Serial.println("Received Data: " + rxValue); // In dữ liệu nhận được

      // Kiểm tra nếu chuỗi chứa dấu phân cách ';' (nếu bạn đang sử dụng để xác định khi nào chuỗi kết thúc)
      if (rxValue.indexOf(';') != -1) // Sử dụng dấu phân cách (nếu có) để xác định kết thúc tin nhắn
      {
        onBLEReceive(rxValue); // Gọi hàm xử lý dữ liệu JSON
        Serial.println("BLECALLBACK.LOG -> " + rxValue);
        rxValue = ""; // Xóa rxValue sau khi xử lý xong
      }
    }
    else
    {
      Serial.println("Received empty data");
    }
  }
};

// Setup function: initializes sensor, EEPROM, and attempts connections
void setup()
{
  Serial.begin(115200);
  dht.begin();               // Start DHT11 sensor
  EEPROM.begin(EEPROM_SIZE); // Initialize EEPROM

  // Load Wi-Fi and MQTT credentials from EEPROM
  loadCredentialsFromEEPROM();
  Serial.printf("SETUP.LOG -> SSI: %s\n", wifiSSID);
  Serial.printf("SETUP.LOG -> PASSWORD: %s\n", wifiPass);
  Serial.printf("SETUP.LOG -> MQTTUSER: %s\n", mqttUser);

  // Attempt to connect to Wi-Fi
  connectToWiFi(wifiSSID, wifiPass);

  setupBLE();
  connectToMQTT();
}

bool countConnectMqtt = 0;

// Main loop: handles connection and sensor data transmission
void loop()
{
  if (WiFi.status() == WL_CONNECTED && mqttConnected)
  {
    sendDHT11Data();
    delay(10000); // Send data every 10 seconds
  }
  else
  {
    Serial.print("[-]");
    delay(1000);
  }
}

// Connect to Wi-Fi using provided SSID and password
void connectToWiFi(const char *ssid, const char *password)
{
  Serial.println("Connecting to WiFi...");
  WiFi.begin(ssid, password); // Start Wi-Fi connection
  unsigned long startTime = millis();

  // Attempt connection for up to 10 seconds
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
    if (millis() - startTime > 30000)
    { // Timeout after 10 seconds
      Serial.println("\nFailed to connect to WiFi");
      wifiConnected = false;
      return;
    }
  }

  Serial.println("\nWiFi connected");
  wifiConnected = true;
}

// Connect to MQTT broker using stored credentials
void connectToMQTT()
{
  client.setServer(mqttServer, 1883); // Set MQTT server
  Serial.print("Connecting to MQTT...");

  // Attempt MQTT connection
  if (client.connect("ESP32Client", mqttUser, mqttPass))
  {
    Serial.println("MQTT connected");
    mqttConnected = true;
  }
  else
  {
    Serial.print("Failed to connect to MQTT, rc=");
    Serial.println(client.state());
    mqttConnected = false;
  }
}

// Send DHT11 sensor data (humidity) to MQTT
void sendDHT11Data()
{
  float humidity = dht.readHumidity(); // Read humidity from DHT11 sensor
  float temperature = dht.readTemperature();
  if (isnan(humidity))
  {
    Serial.println("Failed to read from DHT11 sensor!");
    return;
  }

  // Create JSON payload with humidity data
  String payload = "{\"humidity\": " + String(humidity) + ", \"temperature\":" + String(temperature) + "}";
  client.publish(mqttTopic, payload.c_str()); // Publish data to MQTT
  Serial.println("Data sent to MQTT: " + payload);
}

// Initialize BLE service for receiving credentials
void setupBLE()
{
  BLEDevice::init("ESP32_BLE");                                // Initialize BLE with device name
  pServer = BLEDevice::createServer();                         // Create BLE server and store in global variable
  BLEService *pService = pServer->createService(SERVICE_UUID); // Create BLE service

  pCharacteristic = pService->createCharacteristic(
      CHARACTERISTIC_UUID,
          BLECharacteristic::PROPERTY_WRITE |
          BLECharacteristic::PROPERTY_NOTIFY);

  pCharacteristic->setCallbacks(new MyBLECallbacks()); // Set BLE callbacks
  pService->start();                                   // Start BLE service
  pServer->getAdvertising()->start();                  // Start advertising BLE service
  Serial.println("BLE setup complete, waiting for credentials...");
}

// Parse received JSON data and try to connect to WiFi and MQTT
void onBLEReceive(String jsonData)
{
  DynamicJsonDocument doc(512);                                // Create JSON document for parsing
  DeserializationError error = deserializeJson(doc, jsonData); // Parse JSON data
  if (error)
  {
    Serial.print("Failed to parse JSON: ");
    Serial.println(error.c_str());
    return;
  }

  // Extract Wi-Fi and MQTT credentials from parsed JSON
  strcpy(wifiSSID, doc["ssid"]);
  strcpy(wifiPass, doc["password"]);
  strcpy(mqttUser, doc["mqttUser"]);

  // Try connecting to WiFi and MQTT with the new credentials
  connectToWiFi(wifiSSID, wifiPass);
  if (wifiConnected)
  {
    connectToMQTT();
  }

  if (mqttConnected)
  {
    String message = "SUCCESS";
    pCharacteristic->setValue(message.c_str()); // Thiết lập giá trị cho đặc tính BLE
    pCharacteristic->notify();                  // Gửi thông báo qua BLE
    Serial.println("BLE notification sent: " + message);
  }

  // Save the received credentials to EEPROM
  saveCredentialsToEEPROM();
}

// Save Wi-Fi and MQTT credentials to EEPROM for persistent storage
void saveCredentialsToEEPROM()
{
  EEPROM.writeString(0, wifiSSID);
  EEPROM.writeString(32, wifiPass);
  EEPROM.writeString(96, mqttUser);
  EEPROM.commit(); // Save data to EEPROM
  Serial.printf("SAVE_EROM.LOG -> SSI: %s\n", wifiSSID);
  Serial.printf("SAVE_EROM.LOG -> PASSWORD: %s\n", wifiPass);
  Serial.printf("SAVE_EROM.LOG -> MQTTUSER: %s\n", mqttUser);
  Serial.println("Credentials saved to EEPROM.");
}

// Load Wi-Fi and MQTT credentials from EEPROM
void loadCredentialsFromEEPROM()
{
  EEPROM.readString(0, wifiSSID, 32);
  EEPROM.readString(32, wifiPass, 64);
  EEPROM.readString(96, mqttUser, 32);
}
