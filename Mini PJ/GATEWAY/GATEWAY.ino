/* mosquitto_sub -h localhost -t "home/room1/sensor" -v
 * ESP32 เกตเวย์: รับข้อมูลจากโหนดเซนเซอร์ผ่าน ESP-NOW แล้วส่งต่อเข้า MQTT
 *
 * ไลบรารีที่ต้องติดตั้ง (Library Manager):
 *   - PubSubClient (Nick O'Leary)
 *   - ArduinoJson (Benoit Blanchon) เวอร์ชัน 7.x
 * บอร์ด: esp32 by Espressif Systems
 *
 * หมายเหตุ: ESP-NOW จะใช้ channel เดียวกับ Wi-Fi ที่เกตเวย์เชื่อมต่ออยู่ (ฮอตสปอตมือถือ)
 */
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ====== ตั้งค่า ======
const char* WIFI_SSID = "...";
const char* WIFI_PASS = "...";

const char* MQTT_HOST = "...";  // IP ของโน้ตบุ๊ก (เช็กด้วย ipconfig / ifconfig)
const uint16_t MQTT_PORT = 1883;
const char* MQTT_USER = "";                // เว้นว่างได้ถ้า Mosquitto ไม่ได้ตั้ง user
const char* MQTT_PASS = "";
const char* MQTT_TOPIC = "home/room1/sensor";
// =====================

// โครงสร้างข้อมูลต้องเหมือนกับฝั่งโหนดเซนเซอร์ทุกประการ
typedef struct {
  char node_id[16];
  float temperature;
  float humidity;
  uint32_t seq;
} SensorData;

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
QueueHandle_t rxQueue;
uint32_t lastMqttAttempt = 0;

// callback รับ ESP-NOW (ใช้ template เพื่อให้คอมไพล์ได้ทั้ง Arduino-ESP32 core 2.x และ 3.x)
template <typename T>
void onDataRecv(T, const uint8_t* data, int len) {
  if (len != (int)sizeof(SensorData)) return;
  SensorData d;
  memcpy(&d, data, sizeof(d));
  xQueueSend(rxQueue, &d, 0);  // ส่งเข้าคิว แล้วค่อยไปเผยแพร่ MQTT ใน loop()
}

void connectWiFi() {
  Serial.printf("เชื่อมต่อ Wi-Fi: %s ", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\nสำเร็จ IP=%s  Channel=%d\n", WiFi.localIP().toString().c_str(), WiFi.channel());
}

void connectMqtt() {
  if (mqtt.connected()) return;
  if (millis() - lastMqttAttempt < 5000) return;  // ลองใหม่ทุก 5 วินาที
  lastMqttAttempt = millis();

  Serial.print("เชื่อมต่อ MQTT... ");
  String clientId = "esp32-gateway-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  bool ok = (strlen(MQTT_USER) > 0)
              ? mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)
              : mqtt.connect(clientId.c_str());
  if (ok) {
    Serial.println("สำเร็จ");
  } else {
    Serial.printf("ล้มเหลว (state=%d)\n", mqtt.state());
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  rxQueue = xQueueCreate(10, sizeof(SensorData));

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  // แสดง MAC ของเกตเวย์ นำไปใส่ GATEWAY_MAC ในโค้ดโหนดเซนเซอร์
  Serial.print("MAC ของเกตเวย์: ");
  Serial.println(WiFi.macAddress());

  connectWiFi();

  if (esp_now_init() != ESP_OK) {
    Serial.println("เริ่ม ESP-NOW ไม่สำเร็จ");
    while (true) delay(1000);
  }
  esp_now_register_recv_cb(onDataRecv);

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  Serial.println("พร้อมรับข้อมูล");
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    connectMqtt();
  }
  mqtt.loop();

  SensorData d;
  if (mqtt.connected() && xQueueReceive(rxQueue, &d, 0) == pdTRUE) {
    JsonDocument doc;
    doc["node_id"] = d.node_id;
    doc["temperature"] = d.temperature;
    doc["humidity"] = d.humidity;
    doc["seq"] = d.seq;

    char payload[160];
    serializeJson(doc, payload, sizeof(payload));

    bool ok = mqtt.publish(MQTT_TOPIC, payload);
    Serial.printf("[%s] %s\n", ok ? "MQTT OK" : "MQTT FAIL", payload);
  }
}
