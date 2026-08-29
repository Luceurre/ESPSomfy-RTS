#ifndef DOOYA_H
#define DOOYA_H

#ifdef ARDUINO
#include "Somfy.h"
#include "MQTT.h"
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

#define MAX_AWNINGS 8
// Timing constants captured from a DOOYA DC90 remote (see dooya_captures/).
// Frame: 14 TE sync high + 5 TE low + 40 PWM symbols (bit 1 = 2 TE on/1 TE off,
// bit 0 = 1 TE on/2 TE off), frames repeated back-to-back per key press.
#define DOOYA_TE_US 335
#define DOOYA_SYNC_ON_US 4690    // 14 TE
#define DOOYA_SYNC_OFF_US 1675   // 5 TE
#define DOOYA_REPEAT_COUNT 3
#define DOOYA_FREQUENCY 433.92f

// Final byte of the 40-bit frame: button nibble (high) | check nibble (low).
// Values captured from the remote; the check nibble is a fixed per-button
// mapping (this remote generation uses check nibbles 14/12/5 for up/down/stop,
// not the button-echo variant seen on other Dooya remotes).
enum class dooya_commands : byte {
  up   = 0x1E,  // button 1, check E
  stop = 0x55,  // button 5, check 5
  down = 0x3C   // button 3, check C
};

// Cover travel state (no RF feedback; position is estimated from travel time).
enum class dooya_state : uint8_t {
  stopped = 0,
  opening = 1,
  closing = 2
};

struct dooya_device_t {
  uint8_t awningId = 255;
  char name[21] = "";
  uint8_t remoteId[3] = {0, 0, 0};  // 24-bit remote ID (e.g. 62 13 0C)
  uint8_t channel = 0x61;           // channel byte from the remote
  uint8_t roomId = 0;
  int8_t sortOrder = 0;
  uint16_t travelTime = 30;         // full-travel duration in seconds
  // Runtime state
  uint8_t position = 0;             // 0-100 estimated position
  uint8_t target = 100;             // 0-100 commanded target
  dooya_state state = dooya_state::stopped;
  uint32_t travelStart = 0;         // millis() when the current travel began
  uint8_t travelFrom = 0;           // position when the current travel began
  uint32_t lastPublish = 0;

  void clear();
  bool fromJSON(JsonObject &obj);
  void toJSON(JsonResponse &json);
};

class DooyaController {
  private:
    Transceiver &transceiver;
    void sendDooyaFrame(const dooya_device_t &awning, dooya_commands cmd);
    uint8_t currentPosition(dooya_device_t &awning);
    void stopTravel(dooya_device_t &awning, bool sendFrame);
  public:
    dooya_device_t awnings[MAX_AWNINGS];

    DooyaController(Transceiver &radio);
    bool begin();
    void loop();
    bool sendCommand(uint8_t awningId, dooya_commands cmd);
    bool sendPosition(uint8_t awningId, uint8_t target);
    dooya_device_t *addAwning();
    bool deleteAwning(uint8_t awningId);
    dooya_device_t *getAwningById(uint8_t awningId);
    void toJSONAwnings(JsonResponse &json);
    bool saveAwnings();
    bool loadAwnings();
#ifdef ARDUINO
    void publishDisco();
    void unpublishDisco(uint8_t awningId);
    void unpublishAllDisco();
    void publish();
    void publishState(uint8_t awningId);
    void subscribe();
    void unsubscribe();
    void onMqttCommand(const char *entityId, const char *command, const char *value);
#endif
};

#ifdef ARDUINO
extern ConfigSettings settings;
extern MQTTClass mqtt;

inline void dooya_device_t::clear() {
  this->awningId = 255;
  this->name[0] = 0x00;
  this->remoteId[0] = 0;
  this->remoteId[1] = 0;
  this->remoteId[2] = 0;
  this->channel = 0x61;
  this->roomId = 0;
  this->sortOrder = 0;
  this->travelTime = 30;
  this->position = 0;
  this->target = 100;
  this->state = dooya_state::stopped;
  this->travelStart = 0;
  this->travelFrom = 0;
  this->lastPublish = 0;
}
inline bool dooya_device_t::fromJSON(JsonObject &obj) {
  if(obj.containsKey("awningId")) this->awningId = obj["awningId"];
  if(obj.containsKey("name")) strlcpy(this->name, obj["name"], sizeof(this->name));
  if(obj.containsKey("roomId")) this->roomId = obj["roomId"];
  if(obj.containsKey("sortOrder")) this->sortOrder = obj["sortOrder"];
  if(obj.containsKey("remoteId")) {
    uint32_t id = obj["remoteId"].as<uint32_t>() & 0x00FFFFFF;
    this->remoteId[0] = (id >> 16) & 0xFF;
    this->remoteId[1] = (id >> 8) & 0xFF;
    this->remoteId[2] = id & 0xFF;
  }
  if(obj.containsKey("channel")) this->channel = obj["channel"].as<uint8_t>();
  if(obj.containsKey("travelTime")) this->travelTime = obj["travelTime"].as<uint16_t>();
  if(obj.containsKey("position")) {
    uint8_t pos = obj["position"].as<uint8_t>();
    this->position = pos > 100 ? 100 : pos;
  }
  return true;
}
inline void dooya_device_t::toJSON(JsonResponse &json) {
  uint32_t id = ((uint32_t)this->remoteId[0] << 16) |
    ((uint32_t)this->remoteId[1] << 8) |
    (uint32_t)this->remoteId[2];
  json.addElem("awningId", this->awningId);
  json.addElem("name", this->name);
  json.addElem("roomId", this->roomId);
  json.addElem("sortOrder", this->sortOrder);
  json.addElem("remoteId", id);
  json.addElem("channel", this->channel);
  json.addElem("travelTime", this->travelTime);
  json.addElem("position", this->position);
  json.addElem("target", this->target);
  json.addElem("state", static_cast<uint8_t>(this->state));
}

inline void DooyaController::sendDooyaFrame(const dooya_device_t &awning, dooya_commands cmd) {
  if(!this->transceiver.config.enabled) return;
  const float currentFrequency = this->transceiver.config.frequency;
  const uint32_t pin = 1UL << this->transceiver.config.TXPin;
  const uint8_t frame[5] = {
    awning.remoteId[0],
    awning.remoteId[1],
    awning.remoteId[2],
    awning.channel,
    static_cast<uint8_t>(cmd)
  };
  ELECHOUSE_cc1101.setMHZ(DOOYA_FREQUENCY);
  this->transceiver.beginTransmit();
  for(uint8_t repeat = 0; repeat < DOOYA_REPEAT_COUNT; repeat++) {
    // Sync: long high followed by the inter-symbol low.
    REG_WRITE(GPIO_OUT_W1TS_REG, pin);
    delayMicroseconds(DOOYA_SYNC_ON_US);
    REG_WRITE(GPIO_OUT_W1TC_REG, pin);
    delayMicroseconds(DOOYA_SYNC_OFF_US);
    // 40 bits, MSB first. Bit 1 = 2 TE on / 1 TE off; bit 0 = 1 TE on / 2 TE off.
    for(uint8_t i = 0; i < 40; i++) {
      const bool bit = (frame[i >> 3] >> (7 - (i & 0x07))) & 0x01;
      if(bit) {
        REG_WRITE(GPIO_OUT_W1TS_REG, pin);
        delayMicroseconds(DOOYA_TE_US * 2);
        REG_WRITE(GPIO_OUT_W1TC_REG, pin);
        delayMicroseconds(DOOYA_TE_US);
      }
      else {
        REG_WRITE(GPIO_OUT_W1TS_REG, pin);
        delayMicroseconds(DOOYA_TE_US);
        REG_WRITE(GPIO_OUT_W1TC_REG, pin);
        delayMicroseconds(DOOYA_TE_US * 2);
      }
    }
  }
  this->transceiver.endTransmit();
  ELECHOUSE_cc1101.setMHZ(currentFrequency);
}
inline DooyaController::DooyaController(Transceiver &radio): transceiver(radio) {
  for(uint8_t i = 0; i < MAX_AWNINGS; i++) this->awnings[i].clear();
}
inline bool DooyaController::begin() { return this->loadAwnings(); }
inline uint8_t DooyaController::currentPosition(dooya_device_t &awning) {
  if(awning.state == dooya_state::stopped) return awning.position;
  const uint32_t travelMs = (uint32_t)awning.travelTime * 1000UL;
  if(travelMs == 0) return awning.position;
  uint32_t elapsed = millis() - awning.travelStart;
  if(elapsed > travelMs) elapsed = travelMs;
  const uint8_t delta = (uint8_t)((elapsed * 100UL) / travelMs);
  if(awning.state == dooya_state::opening) {
    return (uint8_t)((uint32_t)awning.travelFrom + delta > 100) ? 100 : awning.travelFrom + delta;
  }
  return (awning.travelFrom > delta) ? (uint8_t)(awning.travelFrom - delta) : 0;
}
inline void DooyaController::stopTravel(dooya_device_t &awning, bool sendFrame) {
  awning.position = this->currentPosition(awning);
  awning.state = dooya_state::stopped;
  awning.target = awning.position;
  if(sendFrame) this->sendDooyaFrame(awning, dooya_commands::stop);
  this->saveAwnings();
}
inline void DooyaController::loop() {
  const uint32_t now = millis();
  for(uint8_t i = 0; i < MAX_AWNINGS; i++) {
    dooya_device_t &awning = this->awnings[i];
    if(awning.awningId == 255) continue;
    if(awning.state == dooya_state::stopped) continue;
    const uint8_t pos = this->currentPosition(awning);
    bool done = false;
    if(awning.state == dooya_state::opening && (pos >= awning.target || pos >= 100)) done = true;
    if(awning.state == dooya_state::closing && (pos <= awning.target || pos <= 0)) done = true;
    if(done) {
      this->stopTravel(awning, true);
      this->publishState(awning.awningId);
    }
    else if(now - awning.lastPublish > 1000) {
      awning.position = pos;
      this->publishState(awning.awningId);
    }
  }
}
inline bool DooyaController::sendCommand(uint8_t awningId, dooya_commands cmd) {
  dooya_device_t *awning = this->getAwningById(awningId);
  if(awning == nullptr || !this->transceiver.config.enabled) return false;
  if(cmd == dooya_commands::stop) {
    if(awning->state != dooya_state::stopped) {
      // Computes the current position, sends the stop frame and persists it.
      this->stopTravel(*awning, true);
    }
    else {
      this->sendDooyaFrame(*awning, dooya_commands::stop);
    }
    this->publishState(awningId);
    return true;
  }
  // Finalize any in-flight travel so position bookkeeping stays honest.
  if(awning->state != dooya_state::stopped) this->stopTravel(*awning, false);
  this->sendDooyaFrame(*awning, cmd);
  if(cmd == dooya_commands::up) {
    awning->state = dooya_state::opening;
    awning->target = 100;
  }
  else if(cmd == dooya_commands::down) {
    awning->state = dooya_state::closing;
    awning->target = 0;
  }
  awning->travelStart = millis();
  awning->travelFrom = awning->position;
  awning->lastPublish = millis();
  this->publishState(awningId);
  return true;
}
inline bool DooyaController::sendPosition(uint8_t awningId, uint8_t target) {
  dooya_device_t *awning = this->getAwningById(awningId);
  if(awning == nullptr || !this->transceiver.config.enabled) return false;
  if(target > 100) target = 100;
  const uint8_t pos = this->currentPosition(*awning);
  if(awning->state != dooya_state::stopped) this->stopTravel(*awning, false);
  if(target == pos || awning->travelTime == 0) {
    awning->target = target;
    awning->position = target;
    this->publishState(awningId);
    return true;
  }
  this->sendDooyaFrame(*awning, target > pos ? dooya_commands::up : dooya_commands::down);
  awning->state = target > pos ? dooya_state::opening : dooya_state::closing;
  awning->target = target;
  awning->travelStart = millis();
  awning->travelFrom = pos;
  awning->lastPublish = millis();
  this->publishState(awningId);
  return true;
}
inline dooya_device_t *DooyaController::addAwning() {
  dooya_device_t *awning = nullptr;
  for(uint8_t i = 0; i < MAX_AWNINGS; i++) {
    if(this->awnings[i].awningId == 255) {
      this->awnings[i].clear();
      this->awnings[i].awningId = i + 1;
      awning = &this->awnings[i];
      break;
    }
  }
  return awning;
}
inline bool DooyaController::deleteAwning(uint8_t awningId) {
  for(uint8_t i = 0; i < MAX_AWNINGS; i++) {
    if(this->awnings[i].awningId == awningId) {
      this->awnings[i].clear();
      this->saveAwnings();
      return true;
    }
  }
  return false;
}
inline dooya_device_t *DooyaController::getAwningById(uint8_t awningId) {
  for(uint8_t i = 0; i < MAX_AWNINGS; i++) {
    if(this->awnings[i].awningId == awningId) return &this->awnings[i];
  }
  return nullptr;
}
inline void DooyaController::toJSONAwnings(JsonResponse &json) {
  for(uint8_t i = 0; i < MAX_AWNINGS; i++) {
    if(this->awnings[i].awningId != 255) {
      json.beginObject();
      this->awnings[i].toJSON(json);
      json.endObject();
    }
  }
}
inline bool DooyaController::saveAwnings() {
  File file = LittleFS.open("/awnings.json", "w");
  if(!file) return false;
  file.print("[");
  bool first = true;
  for(uint8_t i = 0; i < MAX_AWNINGS; i++) {
    if(this->awnings[i].awningId != 255) {
      if(!first) file.print(",");
      first = false;
      file.print("{\"awningId\":");
      file.print(this->awnings[i].awningId);
      file.print(",\"name\":\"");
      file.print(this->awnings[i].name);
      file.print("\",\"remoteId\":");
      file.print(((uint32_t)this->awnings[i].remoteId[0] << 16) | ((uint32_t)this->awnings[i].remoteId[1] << 8) | this->awnings[i].remoteId[2]);
      file.print(",\"channel\":");
      file.print(this->awnings[i].channel);
      file.print(",\"roomId\":");
      file.print(this->awnings[i].roomId);
      file.print(",\"sortOrder\":");
      file.print(this->awnings[i].sortOrder);
      file.print(",\"travelTime\":");
      file.print(this->awnings[i].travelTime);
      file.print(",\"position\":");
      file.print(this->awnings[i].position);
      file.print("}");
    }
  }
  file.print("]");
  file.close();
  return true;
}
inline bool DooyaController::loadAwnings() {
  if(!LittleFS.exists("/awnings.json")) return true;
  File file = LittleFS.open("/awnings.json", "r");
  if(!file) return false;
  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, file);
  file.close();
  if(err) {
    Serial.print("Error parsing awnings.json: ");
    Serial.println(err.c_str());
    return false;
  }
  JsonArray arr = doc.as<JsonArray>();
  for(JsonObject obj : arr) {
    dooya_device_t *awning = this->addAwning();
    if(awning) {
      awning->fromJSON(obj);
      // Restore the runtime awningId if it was persisted.
      if(obj.containsKey("awningId")) {
        uint8_t persistedId = obj["awningId"];
        if(persistedId >= 1 && persistedId <= MAX_AWNINGS) awning->awningId = persistedId;
      }
      awning->state = dooya_state::stopped;
    }
  }
  return true;
}
inline void DooyaController::publishDisco() {
  if(!mqtt.connected() || !settings.MQTT.pubDisco) return;
  char topic[128] = "";
  for(uint8_t i = 0; i < MAX_AWNINGS; i++) {
    dooya_device_t &awning = this->awnings[i];
    if(awning.awningId == 255) continue;

    DynamicJsonDocument doc(1536);
    JsonObject obj = doc.to<JsonObject>();
    snprintf(topic, sizeof(topic), "%s/awnings/%d", settings.MQTT.rootTopic, awning.awningId);
    obj["~"] = topic;
    JsonObject dobj = obj.createNestedObject("device");
    dobj["hw_version"] = settings.fwVersion.name;
    dobj["name"] = settings.hostname;
    dobj["mf"] = "rstrouse";
    JsonArray arrids = dobj.createNestedArray("identifiers");
    snprintf(topic, sizeof(topic), "mqtt_espsomfyrts_%s", settings.serverId);
    arrids.add(topic);
    dobj["via_device"] = topic;
    dobj["model"] = "ESPSomfy-RTS MQTT";
    snprintf(topic, sizeof(topic), "%s/status", settings.MQTT.rootTopic);
    obj["availability_topic"] = topic;
    obj["payload_available"] = "online";
    obj["payload_not_available"] = "offline";
    obj["name"] = awning.name;
    snprintf(topic, sizeof(topic), "mqtt_%s_awning%d", settings.serverId, awning.awningId);
    obj["unique_id"] = topic;
    obj["device_class"] = "awning";
    obj["command_topic"] = "~/cover_command/set";
    obj["payload_open"] = "OPEN";
    obj["payload_close"] = "CLOSE";
    obj["payload_stop"] = "STOP";
    obj["state_topic"] = "~/cover_state";
    obj["state_open"] = "open";
    obj["state_opening"] = "opening";
    obj["state_closed"] = "closed";
    obj["state_closing"] = "closing";
    obj["state_stopped"] = "stopped";
    obj["position_topic"] = "~/position_state";
    obj["set_position_topic"] = "~/position_command/set";
    obj["position_open"] = 100;
    obj["position_closed"] = 0;
    obj["optimistic"] = false;
    obj["enabled_by_default"] = true;
    snprintf(topic, sizeof(topic), "%s/cover/%d/config", settings.MQTT.discoTopic, awning.awningId);
    mqtt.publishDisco(topic, obj, true);
  }
}
inline void DooyaController::unpublishDisco(uint8_t awningId) {
  if(!mqtt.connected() || !settings.MQTT.pubDisco) return;
  char topic[128] = "";
  snprintf(topic, sizeof(topic), "%s/cover/%d/config", settings.MQTT.discoTopic, awningId);
  mqtt.unpublish(topic);
}
inline void DooyaController::unpublishAllDisco() {
  if(!mqtt.connected() || !settings.MQTT.pubDisco) return;
  char topic[128] = "";
  for(uint8_t i = 0; i < MAX_AWNINGS; i++) {
    if(this->awnings[i].awningId == 255) continue;
    snprintf(topic, sizeof(topic), "%s/cover/%d/config", settings.MQTT.discoTopic, this->awnings[i].awningId);
    mqtt.unpublish(topic);
  }
}
inline void DooyaController::publish() {
  if(!mqtt.connected()) return;
  char topic[128] = "";
  for(uint8_t i = 0; i < MAX_AWNINGS; i++) {
    dooya_device_t &awning = this->awnings[i];
    if(awning.awningId == 255) continue;
    snprintf(topic, sizeof(topic), "awnings/%d/awningId", awning.awningId);
    mqtt.publish(topic, awning.awningId, true);
    snprintf(topic, sizeof(topic), "awnings/%d/name", awning.awningId);
    mqtt.publish(topic, awning.name, true);
    this->publishState(awning.awningId);
  }
  this->publishDisco();
}
inline void DooyaController::publishState(uint8_t awningId) {
  if(!mqtt.connected()) return;
  dooya_device_t *awning = this->getAwningById(awningId);
  if(awning == nullptr) return;
  const uint8_t pos = this->currentPosition(*awning);
  awning->position = pos;
  awning->lastPublish = millis();
  char topic[128] = "";
  const char *stateStr = "stopped";
  if(pos >= 100) stateStr = "open";
  else if(pos <= 0) stateStr = "closed";
  else if(awning->state == dooya_state::opening) stateStr = "opening";
  else if(awning->state == dooya_state::closing) stateStr = "closing";
  snprintf(topic, sizeof(topic), "awnings/%d/cover_state", awningId);
  mqtt.publish(topic, stateStr, true);
  snprintf(topic, sizeof(topic), "awnings/%d/position_state", awningId);
  mqtt.publish(topic, (uint16_t)pos, true);
}
inline void DooyaController::subscribe() {
  mqtt.subscribe("awnings/+/+/set");
}
inline void DooyaController::unsubscribe() {
  mqtt.unsubscribe("awnings/+/+/set");
}
inline void DooyaController::onMqttCommand(const char *entityId, const char *command, const char *value) {
  const uint8_t awningId = atoi(entityId);
  dooya_device_t *awning = this->getAwningById(awningId);
  if(awning == nullptr) return;
  if(strcmp(command, "cover_command") == 0) {
    if(strcmp(value, "OPEN") == 0) this->sendCommand(awningId, dooya_commands::up);
    else if(strcmp(value, "CLOSE") == 0) this->sendCommand(awningId, dooya_commands::down);
    else if(strcmp(value, "STOP") == 0) this->sendCommand(awningId, dooya_commands::stop);
  }
  else if(strcmp(command, "position_command") == 0) {
    int target = atoi(value);
    if(target >= 0 && target <= 100) this->sendPosition(awningId, (uint8_t)target);
  }
}
#endif

#endif
