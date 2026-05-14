#ifndef FAN_H
#define FAN_H

#ifdef ARDUINO
#include "Somfy.h"
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <LittleFS.h>
#else
#include <cstdint>
typedef uint8_t byte;
class JsonObject;
class JsonResponse;
struct transceiver_config_t {
  bool enabled;
  uint8_t TXPin;
  float frequency;
};
class Transceiver {
  public:
    transceiver_config_t config;
    void beginTransmit();
    void endTransmit();
};
#endif

#define MAX_FANS 16
#define FAN_BIT_WINDOW_US 1000
#define FAN_BIT_ZERO_ON_US 250
#define FAN_BIT_ZERO_OFF_US 750
#define FAN_BIT_ONE_ON_US 750
#define FAN_BIT_ONE_OFF_US 250
#define FAN_PREAMBLE_ON_US 250
#define FAN_PREAMBLE_OFF_US 8000
#define FAN_REPEAT_COUNT 8

// 5-bit command: N5 (4 bits) + N6[3] (1 bit)
// N5 = upper 4 bits, N6[3] = lowest bit
enum class fan_commands : byte {
  light  = 0x12,  // N5=0x9,  N6[3]=0
  fan    = 0x0A,  // N5=0x5,  N6[3]=0
  color  = 0x1C,  // N5=0xE,  N6[3]=0
  speed1 = 0x04,  // N5=0x2,  N6[3]=0
  speed2 = 0x10,  // N5=0x8,  N6[3]=0
  speed3 = 0x0C,  // N5=0x6,  N6[3]=0
  speed4 = 0x06,  // N5=0x3,  N6[3]=0
  speed5 = 0x09,  // N5=0x4,  N6[3]=1
  speed6 = 0x15   // N5=0xA,  N6[3]=1
};

struct fan_device_t {
  uint8_t fanId = 255;
  char name[21] = "";
  uint8_t N0 = 0;
  uint8_t N1 = 0;
  uint8_t N2 = 0;
  uint8_t N3 = 0;
  uint8_t N4 = 0;
  uint8_t roomId = 0;
  int8_t sortOrder = 0;

  void clear();
  bool save();
  bool fromJSON(JsonObject &obj);
  void toJSON(JsonResponse &json);
};

class FanController {
  private:
    Transceiver &transceiver;
    uint32_t buildCode(const fan_device_t &fan, fan_commands cmd);
    void sendFanFrame(uint32_t code);
  public:
    static constexpr float FAN_FREQUENCY = 433.92f;
    fan_device_t fans[MAX_FANS];

    FanController(Transceiver &radio);
    bool begin();
    void loop();
    bool sendCommand(uint8_t fanId, fan_commands cmd);
    fan_device_t *addFan();
    bool deleteFan(uint8_t fanId);
    fan_device_t *getFanById(uint8_t fanId);
    void toJSONFans(JsonResponse &json);
    bool saveFans();
    bool loadFans();
};

#ifdef ARDUINO
inline void fan_device_t::clear() {
  this->fanId = 255;
  this->name[0] = 0x00;
  this->N0 = 0;
  this->N1 = 0;
  this->N2 = 0;
  this->N3 = 0;
  this->N4 = 0;
  this->roomId = 0;
  this->sortOrder = 0;
}
inline bool fan_device_t::save() { return true; }
inline bool fan_device_t::fromJSON(JsonObject &obj) {
  if(obj.containsKey("fanId")) this->fanId = obj["fanId"];
  if(obj.containsKey("name")) strlcpy(this->name, obj["name"], sizeof(this->name));
  if(obj.containsKey("roomId")) this->roomId = obj["roomId"];
  if(obj.containsKey("sortOrder")) this->sortOrder = obj["sortOrder"];
  if(obj.containsKey("address")) {
    uint32_t address = obj["address"].as<uint32_t>() & 0x000FFFFF;
    this->N0 = (address >> 16) & 0x0F;
    this->N1 = (address >> 12) & 0x0F;
    this->N2 = (address >> 8) & 0x0F;
    this->N3 = (address >> 4) & 0x0F;
    this->N4 = address & 0x0F;
  }
  if(obj.containsKey("N0")) this->N0 = obj["N0"].as<uint8_t>() & 0x0F;
  if(obj.containsKey("N1")) this->N1 = obj["N1"].as<uint8_t>() & 0x0F;
  if(obj.containsKey("N2")) this->N2 = obj["N2"].as<uint8_t>() & 0x0F;
  if(obj.containsKey("N3")) this->N3 = obj["N3"].as<uint8_t>() & 0x0F;
  if(obj.containsKey("N4")) this->N4 = obj["N4"].as<uint8_t>() & 0x0F;
  return true;
}
inline void fan_device_t::toJSON(JsonResponse &json) {
  uint32_t address = ((uint32_t)(this->N0 & 0x0F) << 16) |
    ((uint32_t)(this->N1 & 0x0F) << 12) |
    ((uint32_t)(this->N2 & 0x0F) << 8) |
    ((uint32_t)(this->N3 & 0x0F) << 4) |
    (uint32_t)(this->N4 & 0x0F);
  json.addElem("fanId", this->fanId);
  json.addElem("name", this->name);
  json.addElem("roomId", this->roomId);
  json.addElem("sortOrder", this->sortOrder);
  json.addElem("address", address);
  json.addElem("N0", this->N0);
  json.addElem("N1", this->N1);
  json.addElem("N2", this->N2);
  json.addElem("N3", this->N3);
  json.addElem("N4", this->N4);
}

inline uint32_t FanController::buildCode(const fan_device_t &fan, fan_commands cmd) {
  const uint8_t n0 = fan.N0 & 0x0F;
  const uint8_t n1 = fan.N1 & 0x0F;
  const uint8_t n2 = fan.N2 & 0x0F;
  const uint8_t n3 = fan.N3 & 0x0F;
  const uint8_t n4 = fan.N4 & 0x0F;
  const uint8_t n5 = (static_cast<uint8_t>(cmd) >> 1) & 0x0F;
  const uint8_t n6 = (static_cast<uint8_t>(cmd) & 0x01) << 3;
  const uint8_t n7 = (n0 ^ n1 ^ n2 ^ n3 ^ n4 ^ n5 ^ n6 ^ 0x0A) & 0x0F;
  return ((uint32_t)n0 << 28) |
    ((uint32_t)n1 << 24) |
    ((uint32_t)n2 << 20) |
    ((uint32_t)n3 << 16) |
    ((uint32_t)n4 << 12) |
    ((uint32_t)n5 << 8) |
    ((uint32_t)n6 << 4) |
    (uint32_t)n7;
}
inline void FanController::sendFanFrame(uint32_t code) {
  if(!this->transceiver.config.enabled) return;
  const float currentFrequency = this->transceiver.config.frequency;
  const uint32_t pin = 1UL << this->transceiver.config.TXPin;
  ELECHOUSE_cc1101.setMHZ(FAN_FREQUENCY);
  this->transceiver.beginTransmit();
  for(uint8_t repeat = 0; repeat < FAN_REPEAT_COUNT; repeat++) {
    REG_WRITE(GPIO_OUT_W1TS_REG, pin);
    delayMicroseconds(FAN_PREAMBLE_ON_US);
    REG_WRITE(GPIO_OUT_W1TC_REG, pin);
    delayMicroseconds(FAN_PREAMBLE_OFF_US);
    for(uint8_t i = 0; i < 32; i++) {
      if(((code >> (31 - i)) & 0x01) == 0x01) {
        REG_WRITE(GPIO_OUT_W1TS_REG, pin);
        delayMicroseconds(FAN_BIT_ONE_ON_US);
        REG_WRITE(GPIO_OUT_W1TC_REG, pin);
        delayMicroseconds(FAN_BIT_ONE_OFF_US);
      }
      else {
        REG_WRITE(GPIO_OUT_W1TS_REG, pin);
        delayMicroseconds(FAN_BIT_ZERO_ON_US);
        REG_WRITE(GPIO_OUT_W1TC_REG, pin);
        delayMicroseconds(FAN_BIT_ZERO_OFF_US);
      }
    }
  }
  this->transceiver.endTransmit();
  ELECHOUSE_cc1101.setMHZ(currentFrequency);
}
inline FanController::FanController(Transceiver &radio): transceiver(radio) {
  for(uint8_t i = 0; i < MAX_FANS; i++) this->fans[i].clear();
}
inline bool FanController::begin() { return this->loadFans(); }
inline void FanController::loop() { }
inline bool FanController::sendCommand(uint8_t fanId, fan_commands cmd) {
  fan_device_t *fan = this->getFanById(fanId);
  if(fan == nullptr || !this->transceiver.config.enabled) return false;
  this->sendFanFrame(this->buildCode(*fan, cmd));
  return true;
}
inline fan_device_t *FanController::addFan() {
  fan_device_t *fan = nullptr;
  for(uint8_t i = 0; i < MAX_FANS; i++) {
    if(this->fans[i].fanId == 255) {
      fan = &this->fans[i];
      break;
    }
  }
  if(fan == nullptr) return nullptr;
  for(uint8_t id = 1; id <= MAX_FANS; id++) {
    bool exists = false;
    for(uint8_t i = 0; i < MAX_FANS; i++) {
      if(this->fans[i].fanId == id) {
        exists = true;
        break;
      }
    }
    if(!exists) {
      fan->clear();
      fan->fanId = id;
      return fan;
    }
  }
  return nullptr;
}
inline bool FanController::deleteFan(uint8_t fanId) {
  fan_device_t *fan = this->getFanById(fanId);
  if(fan == nullptr) return false;
  fan->clear();
  return true;
}
inline fan_device_t *FanController::getFanById(uint8_t fanId) {
  for(uint8_t i = 0; i < MAX_FANS; i++) {
    if(this->fans[i].fanId == fanId) return &this->fans[i];
  }
  return nullptr;
}
inline void FanController::toJSONFans(JsonResponse &json) {
  for(uint8_t i = 0; i < MAX_FANS; i++) {
    if(this->fans[i].fanId != 255) {
      json.beginObject();
      this->fans[i].toJSON(json);
      json.endObject();
    }
  }
}
inline bool FanController::saveFans() {
  File file = LittleFS.open("/fans.json", "w");
  if(!file) return false;
  file.print("[");
  bool first = true;
  for(uint8_t i = 0; i < MAX_FANS; i++) {
    if(this->fans[i].fanId != 255) {
      if(!first) file.print(",");
      first = false;
      file.print("{\"fanId\":");
      file.print(this->fans[i].fanId);
      file.print(",\"name\":\"");
      file.print(this->fans[i].name);
      file.print("\",\"N0\":");
      file.print(this->fans[i].N0);
      file.print(",\"N1\":");
      file.print(this->fans[i].N1);
      file.print(",\"N2\":");
      file.print(this->fans[i].N2);
      file.print(",\"N3\":");
      file.print(this->fans[i].N3);
      file.print(",\"N4\":");
      file.print(this->fans[i].N4);
      file.print(",\"roomId\":");
      file.print(this->fans[i].roomId);
      file.print(",\"sortOrder\":");
      file.print(this->fans[i].sortOrder);
      file.print("}");
    }
  }
  file.print("]");
  file.close();
  return true;
}
inline bool FanController::loadFans() {
  if(!LittleFS.exists("/fans.json")) return true;
  File file = LittleFS.open("/fans.json", "r");
  if(!file) return false;
  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, file);
  file.close();
  if(err) {
    Serial.print("Error parsing fans.json: ");
    Serial.println(err.c_str());
    return false;
  }
  JsonArray arr = doc.as<JsonArray>();
  for(JsonObject obj : arr) {
    fan_device_t *fan = this->addFan();
    if(fan) fan->fromJSON(obj);
  }
  return true;
}
#endif

#endif
