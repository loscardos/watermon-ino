#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <NTPClient.h>
#include <Adafruit_NeoPixel.h>
#include <HTTPClient.h>

#define SERVICE_UUID        "12345678-1234-1234-1234-123456789012"
#define CHARACTERISTIC_UUID "87654321-4321-4321-4321-210987654321"

#ifdef RGB_MATRIX_64LEDS
#define PIN 47
#define LED_COUNT 64
#else
#define PIN 48
#define LED_COUNT 1
#endif

NimBLECharacteristic *pCharacteristic;
Preferences preferences;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org");

bool waitingForNewCredentials = false;

const char *serverUrl = "http://192.168.3.23/api/sensor_data";
const char *serverUrlMetadata = "http://192.168.3.23/api/metadata";

String lastDescription = "";
String description = "";

String ssid = "";
String password = "";

String device_name_prefix = "ESP32_";
String device_name;

typedef struct struct_peerInfo {
    uint8_t mac[6];
    int deviceNumber;
} peerInfo_t;

int deviceNumber;

Adafruit_NeoPixel strip = Adafruit_NeoPixel(LED_COUNT, PIN, NEO_GRB + NEO_KHZ800);

void setPixelColor(uint8_t r, uint8_t g, uint8_t b)
{
    for (uint16_t i = 0; i < strip.numPixels(); i++) {
        strip.setPixelColor(i, Adafruit_NeoPixel::Color(r, g, b));
    }

    strip.show();
}

String getIsoTimeString() {
    timeClient.update();
    unsigned long epochTime = timeClient.getEpochTime();
    time_t now = epochTime;
    struct tm timeinfo;
    char iso_time[21];

    gmtime_r(&now, &timeinfo);
    strftime(iso_time, sizeof(iso_time), "%Y-%m-%dT%H:%M:%S", &timeinfo);

    return {iso_time};
}

String getDeviceName() {
    uint8_t baseMac[6];
    esp_read_mac(baseMac, ESP_MAC_WIFI_STA);
    char baseMacChr[18] = {0};
    sprintf(baseMacChr, "ESP32_%02X%02X%02X", baseMac[3], baseMac[4], baseMac[5]);
    return {baseMacChr};
}

void connectToWiFi(const char *ssid, const char *password) {
    unsigned long startTime;
    unsigned long timeout = 3000;
    int maxRetries = 2;
    int retryCount = 0;
    bool connected = false;

    while (!connected && retryCount < maxRetries) {
        WiFi.disconnect();
        WiFi.begin(ssid, password);
        Serial.print("Connecting to Wi-Fi");
        startTime = millis();
        while (WiFiClass::status() != WL_CONNECTED) {
            delay(500);
            Serial.print(".");

            if (millis() - startTime > timeout) {
                Serial.println("");
                Serial.print("Connection attempt timed out. Retry ");
                Serial.print(retryCount + 1);
                Serial.print(" of ");
                Serial.print(maxRetries);
                Serial.println(".");
                break;
            }
        }

        if (WiFiClass::status() == WL_CONNECTED) {
            connected = true;
        } else {
            retryCount++;
        }
    }

    if (connected) {
        Serial.println("");
        Serial.print("Connected. IP address: ");
        Serial.println(WiFi.localIP());
        timeClient.begin();
        timeClient.update();
        preferences.putString("ssid", ssid);
        preferences.putString("password", password);
    } else {
        Serial.print("Failed to connect after ");
        Serial.print(maxRetries);
        Serial.println(" attempts. Giving up.");
        WiFi.disconnect();
        preferences.putString("ssid", "");
        preferences.putString("password", "");
    }
}

void sendDataToServer(float raw) {
    if (WiFiClass::status() == WL_CONNECTED) {
        HTTPClient http;
        http.begin(serverUrl);

        String timestamp = getIsoTimeString();

        String payload = "{";
        payload += "\"device_name\":\"" + String(device_name) + "\",";
        payload += "\"timestamp\":\"" + timestamp + "\",";
        payload += "\"data\":[";
        payload += "{\"key\":\"key\",\"value\":" + String(raw) + "}";
        payload += "]";
        payload += "}";

        Serial.print("Payload: ");
        Serial.println(payload);

        http.addHeader("Content-Type", "application/json");
        int httpResponseCode = http.POST(payload);

        if (httpResponseCode > 0) {
            String response = http.getString();
            Serial.print("HTTP Response code: ");
            Serial.println(httpResponseCode);
            Serial.print("Response: ");
            Serial.println(response);
        } else {
            Serial.print("Error sending data. HTTP error code: ");
            Serial.println(httpResponseCode);
        }
        http.end();
    } else {
        Serial.println("Error: Not connected to Wi-Fi");
    }
}


void sendMetaDataToServer(const String &description) {
    if (WiFiClass::status() == WL_CONNECTED) {
        HTTPClient http;
        http.begin(serverUrlMetadata);

        String timestamp = getIsoTimeString();

        String payload = "{";
        payload += "\"device_name\":\"" + String(device_name) + "\",";
        payload += "\"timestamp\":\"" + timestamp + "\",";
        payload += "\"metadata\":[";
        payload += "{\"key\":\"description\",\"value\":\"" + description + "\"}";
        payload += "]";
        payload += "}";

        Serial.print("Metadata Payload: ");
        Serial.println(payload);

        http.addHeader("Content-Type", "application/json");
        int httpResponseCode = http.POST(payload);

        if (httpResponseCode > 0) {
            String response = http.getString();
            Serial.print("HTTP Response code: ");
            Serial.println(httpResponseCode);
            Serial.print("Response: ");
            Serial.println(response);
        } else {
            Serial.print("Error sending metadata. HTTP error code: ");
            Serial.println(httpResponseCode);
        }
        http.end();
    }  else {
        Serial.println("Error: Not connected to Wi-Fi");
    }
}


class JSONCallback : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *pCharacteristic) override {
        std::string receivedData = pCharacteristic->getValue();

        receivedData.erase(remove(receivedData.begin(), receivedData.end(), '\\'), receivedData.end());

        DynamicJsonDocument doc(1024);
        DeserializationError error = deserializeJson(doc, receivedData);

        if (!error) {
            if (doc.containsKey("ssid") && doc.containsKey("passwd")) {
                ssid = doc["ssid"].as<String>();
                password = doc["passwd"].as<String>();

                waitingForNewCredentials = true;
            }

            if (doc.containsKey("description")) {
                description = doc["description"].as<String>();

                if (description == "forgot") {
                    preferences.putString("ssid", "");
                    preferences.putString("password", "");
                    ssid = ""; password = "";

                    WiFi.disconnect();
                    waitingForNewCredentials = true;
                }
            }
        } else {
            Serial.print("Failed to parse JSON: ");
            Serial.println(error.c_str());
        }
    }
};

void setup() {
    Serial.begin(115200);

    device_name = getDeviceName();

    strip.begin();
    strip.setBrightness(15);

    NimBLEDevice::init("ESP32-S3");

    NimBLEServer *pServer = NimBLEDevice::createServer();
    NimBLEService *pService = pServer->createService(SERVICE_UUID);

    pCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::READ
    );

    pCharacteristic->setCallbacks(new JSONCallback());

    pService->start();
    NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->start();
}

void loop() {
    if (waitingForNewCredentials) {
        if (WiFiClass::status() != WL_CONNECTED) {
            connectToWiFi(ssid.c_str(), password.c_str());
        }

        waitingForNewCredentials = false;

        Serial.print("Check condition: ");
        Serial.print("WaitingForNewCredentials: ");
        Serial.print(waitingForNewCredentials);
        Serial.print(", Description.length(): ");
        Serial.print(description.length());
        Serial.print(", Description: ");
        Serial.print(description);
        Serial.print(", lastDescription: ");
        Serial.println(lastDescription);
    }

    if (!waitingForNewCredentials && description.length() > 0 && description !=
        lastDescription) {
        lastDescription = description;
        description = "";
    }

    if (WiFiClass::status() == WL_CONNECTED) {
        setPixelColor(0, 255, 0);

        sendDataToServer(10.0);
        delay(5000);
    } else {
        ssid = preferences.getString("ssid", "");
        password = preferences.getString("password", "");

        if (!waitingForNewCredentials && !ssid.isEmpty() && !password.isEmpty()) {
            if (ssid.length() > 0 && password.length() > 0) {
                connectToWiFi(ssid.c_str(), password.c_str());
            }

            if (WiFiClass::status() != WL_CONNECTED) {
                waitingForNewCredentials = true;
            }
        }
    }

    if (waitingForNewCredentials || ssid.isEmpty()) {
        setPixelColor(255, 0, 0);
        delay(1000);
        setPixelColor(0, 0, 0);
        delay(1000);
    }
}
