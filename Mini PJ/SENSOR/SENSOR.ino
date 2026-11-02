/*
 * ESP32 โหนดเซนเซอร์ (จำลอง): สุ่มค่าอุณหภูมิ/ความชื้น แล้วส่งไปเกตเวย์ผ่าน ESP-NOW
 *
 * - ใช้ channel ที่จำไว้ใน NVS ก่อน
 * - ถ้าส่งไม่สำเร็จติดกันหลายครั้ง จะสแกน channel 1-13 หาช่องที่เกตเวย์ตอบรับ
 * - บันทึก channel ลง NVS เฉพาะเมื่อค่าเปลี่ยน (ไม่ทำให้ flash สึก)
 *
 * บอร์ด: esp32 by Espressif Systems (ไม่ต้องติดตั้งไลบรารีเพิ่ม)
 */
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>

// ====== ตั้งค่า ======
// MAC ของเกตเวย์ (ดูจาก Serial Monitor ของเกตเวย์ แล้วแปลงเป็นรูปแบบนี้)
// เช่น "24:6F:28:AB:CD:EF" -> {0x24, 0x6F, 0x28, 0xAB, 0xCD, 0xEF} 78:1c:3c:ca:e1:84
uint8_t GATEWAY_MAC[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

const char* NODE_ID = "NODE_ESP32_1";
const uint32_t SEND_INTERVAL_MS = 10000;  // ส่งทุก 10 วินาที

const int FAIL_LIMIT = 3;            // ส่งพลาดติดกันกี่ครั้งถึงเริ่มสแกน
const int TRIES_PER_CHANNEL = 3;     // ตอนสแกน ลองกี่ครั้งต่อ channel
const uint32_t ACK_TIMEOUT_MS = 200; // รอผลการส่งสูงสุดต่อครั้ง

// จำลองค่าเกินเกณฑ์ เพื่อทดสอบแจ้งเตือน LINE
const int SPIKE_EVERY = 60;   // ทุกกี่รอบจะเกิดช่วงค่าสูง (0 = ปิด)
const int SPIKE_LENGTH = 2;   // ช่วงค่าสูงยาวกี่รอบ
// =====================

// โครงสร้างข้อมูลต้องเหมือนกับฝั่งเกตเวย์ทุกประการ
typedef struct {
  char node_id[16];
  float temperature;
  float humidity;
  uint32_t seq;
} SensorData;

Preferences prefs;
uint8_t currentChannel = 1;
volatile bool sendDone = false;
volatile bool sendOk = false;
int failCount = 0;
uint32_t seqNo = 0;
uint32_t readingCount = 0;
uint32_t lastSend = 0;

float baseTemp = 28.0;
float baseHum = 65.0;

// callback ผลการส่ง (ใช้ template เพื่อให้คอมไพล์ได้ทั้ง core 2.x และ 3.x)
template <typename T>
void onDataSent(T, esp_now_send_status_t status) {
  sendOk = (status == ESP_NOW_SEND_SUCCESS);
  sendDone = true;
}

void setChannel(uint8_t ch) {
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
  currentChannel = ch;
}

void saveChannelIfChanged(uint8_t ch) {
  if (prefs.getUChar("ch", 0) != ch) {
    prefs.putUChar("ch", ch);
    Serial.printf("บันทึก channel %d ลง NVS\n", ch);
  }
}

// ส่งหนึ่งครั้ง คืนค่า true ถ้าเกตเวย์ตอบรับ
bool sendOnce(const SensorData& d) {
  sendDone = false;
  sendOk = false;
  if (esp_now_send(GATEWAY_MAC, (const uint8_t*)&d, sizeof(d)) != ESP_OK) return false;
  uint32_t start = millis();
  while (!sendDone && millis() - start < ACK_TIMEOUT_MS) delay(1);
  return sendDone && sendOk;
}

// สแกน channel 1-13 หาช่องที่เกตเวย์ตอบรับ
bool scanChannels(const SensorData& d) {
  Serial.println("สแกนหา channel ของเกตเวย์...");
  for (uint8_t ch = 1; ch <= 13; ch++) {
    setChannel(ch);
    for (int i = 0; i < TRIES_PER_CHANNEL; i++) {
      if (sendOnce(d)) {
        Serial.printf("เจอเกตเวย์ที่ channel %d\n", ch);
        saveChannelIfChanged(ch);
        return true;
      }
      delay(20);
    }
  }
  Serial.println("ไม่พบเกตเวย์ในทุก channel (จะลองใหม่รอบถัดไป)");
  return false;
}

// สุ่มค่าแบบค่อย ๆ ขยับจากค่าก่อนหน้า ให้กราฟดูสมจริง
void generateReading(SensorData& d) {
  baseTemp += random(-30, 31) / 100.0f;  // ขยับ +-0.30 C
  baseHum += random(-100, 101) / 100.0f;  // ขยับ +-1.00 %
  baseTemp = constrain(baseTemp, 22.0f, 32.0f);
  baseHum = constrain(baseHum, 40.0f, 85.0f);

  readingCount++;
  float t = baseTemp;
  if (SPIKE_EVERY > 0 && (readingCount % SPIKE_EVERY) >= (uint32_t)(SPIKE_EVERY - SPIKE_LENGTH)) {
    t = 36.0f + random(0, 30) / 10.0f;  // 36.0 - 38.9 C (เกินเกณฑ์ตัวอย่าง 35)
  }

  memset(&d, 0, sizeof(d));
  strncpy(d.node_id, NODE_ID, sizeof(d.node_id) - 1);
  d.temperature = t;
  d.humidity = baseHum;
  d.seq = ++seqNo;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  randomSeed(esp_random());

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  prefs.begin("espnow", false);
  currentChannel = prefs.getUChar("ch", 1);
  if (currentChannel < 1 || currentChannel > 13) currentChannel = 1;
  setChannel(currentChannel);
  Serial.printf("ใช้ channel เริ่มต้น: %d\n", currentChannel);

  if (esp_now_init() != ESP_OK) {
    Serial.println("เริ่ม ESP-NOW ไม่สำเร็จ");
    while (true) delay(1000);
  }
  esp_now_register_send_cb(onDataSent);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, GATEWAY_MAC, 6);
  peer.channel = 0;  // 0 = ใช้ channel ปัจจุบันของวิทยุ
  peer.encrypt = false;
  peer.ifidx = WIFI_IF_STA;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("เพิ่ม peer ไม่สำเร็จ");
    while (true) delay(1000);
  }

  lastSend = millis() - SEND_INTERVAL_MS;  // ส่งครั้งแรกทันที
}

void loop() {
  if (millis() - lastSend < SEND_INTERVAL_MS) return;
  lastSend = millis();

  SensorData d;
  generateReading(d);

  bool ok = sendOnce(d);
  Serial.printf("[ch %d] #%lu T=%.1f H=%.1f -> %s\n", currentChannel, (unsigned long)d.seq,
                d.temperature, d.humidity, ok ? "สำเร็จ" : "ล้มเหลว");

  if (ok) {
    failCount = 0;
    saveChannelIfChanged(currentChannel);
  } else if (++failCount >= FAIL_LIMIT) {
    if (scanChannels(d)) failCount = 0;
  }
}
