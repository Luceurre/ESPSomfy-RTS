#include <WiFi.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include "mqtt_client.h"
#include "ConfigSettings.h"
#include "MQTT.h"
#include "Fan.h"
#include "Dooya.h"
#include "Network.h"
#include "Utils.h"

#define MQTT_MAX_RESPONSE 2048

static char g_content[MQTT_MAX_RESPONSE];

extern ConfigSettings settings;
extern FanController fanCtrl;
extern DooyaController dooyaCtrl;
extern Network net;
extern rebootDelay_t rebootDelay;

namespace {
  bool isTopicAbsolute(const char *topic) {
    if(topic == nullptr || topic[0] == '\0') return false;

    const size_t rootLen = strlen(settings.MQTT.rootTopic);
    if(rootLen > 0 && strncmp(topic, settings.MQTT.rootTopic, rootLen) == 0 && (topic[rootLen] == '\0' || topic[rootLen] == '/')) {
      return true;
    }

    const size_t discoLen = strlen(settings.MQTT.discoTopic);
    if(discoLen > 0 && strncmp(topic, settings.MQTT.discoTopic, discoLen) == 0 && (topic[discoLen] == '\0' || topic[discoLen] == '/')) {
      return true;
    }

    return false;
  }
}

bool MQTTClass::begin() {
  this->suspended = false;
  return true;
}

bool MQTTClass::end() {
  this->suspended = true;
  return this->disconnect();
}

void MQTTClass::reset() {
  this->disconnect();
  this->lastConnect = 0;
  this->connect();
}

bool MQTTClass::loop() {
  if(settings.MQTT.enabled && !rebootDelay.reboot && !this->suspended && net.connected() && this->_client == nullptr) {
    esp_task_wdt_reset();
    this->connect();
  }
  return true;
}

void MQTTClass::buildClientId() {
  const uint64_t mac = ESP.getEfuseMac();
  snprintf(this->clientId, sizeof(this->clientId), "client-%08x%08x", static_cast<uint32_t>((mac >> 32) & 0xFFFFFFFF), static_cast<uint32_t>(mac & 0xFFFFFFFF));
}

void MQTTClass::buildBrokerUri() {
  snprintf(this->_brokerUri, sizeof(this->_brokerUri), "%s%s:%u", settings.MQTT.protocol, settings.MQTT.hostname, settings.MQTT.port);
}

bool MQTTClass::buildRootTopic(const char *topic, char *buffer, size_t len) {
  if(buffer == nullptr || len == 0 || topic == nullptr) return false;
  if(isTopicAbsolute(topic)) {
    strlcpy(buffer, topic, len);
    return strlen(topic) < len;
  }
  if(strlen(settings.MQTT.rootTopic) > 0) {
    return snprintf(buffer, len, "%s/%s", settings.MQTT.rootTopic, topic) < static_cast<int>(len);
  }
  strlcpy(buffer, topic, len);
  return strlen(topic) < len;
}

bool MQTTClass::buildRawTopic(const char *topic, char *buffer, size_t len) {
  if(buffer == nullptr || len == 0 || topic == nullptr) return false;
  strlcpy(buffer, topic, len);
  return strlen(topic) < len;
}

void MQTTClass::resetMessageBuffers() {
  this->_messageTopic[0] = '\0';
  this->_messagePayload[0] = '\0';
}

bool MQTTClass::connect() {
  esp_task_wdt_reset();

  if(this->_client != nullptr) {
    if(!settings.MQTT.enabled || this->suspended) return this->disconnect();
    return this->_connected;
  }

  if(!settings.MQTT.enabled || this->suspended || rebootDelay.reboot || !net.connected()) return false;
  if(this->lastConnect + 10000 > millis()) return false;
  if(strlen(settings.MQTT.protocol) == 0 || strlen(settings.MQTT.hostname) == 0) return true;

  this->buildClientId();
  this->buildBrokerUri();
  if(strlen(settings.MQTT.rootTopic) > 0) snprintf(this->_lwtTopic, sizeof(this->_lwtTopic), "%s/status", settings.MQTT.rootTopic);
  else strlcpy(this->_lwtTopic, "status", sizeof(this->_lwtTopic));

  esp_mqtt_client_config_t config = {};
  config.uri = this->_brokerUri;
  config.client_id = this->clientId;
  config.username = strlen(settings.MQTT.username) > 0 ? settings.MQTT.username : nullptr;
  config.password = strlen(settings.MQTT.password) > 0 ? settings.MQTT.password : nullptr;
  config.lwt_topic = this->_lwtTopic;
  config.lwt_msg = "offline";
  config.lwt_msg_len = 7;
  config.lwt_qos = 0;
  config.lwt_retain = 1;
  config.disable_clean_session = 0;
  config.disable_auto_reconnect = false;
  config.keepalive = 60;
  config.buffer_size = 2048;
  config.out_buffer_size = 2048;
  config.network_timeout_ms = 10000;
  config.reconnect_timeout_ms = 10000;
  config.user_context = this;

  this->_client = esp_mqtt_client_init(&config);
  if(this->_client == nullptr) {
    this->lastConnect = millis();
    return false;
  }

  if(esp_mqtt_client_register_event(this->_client, MQTT_EVENT_ANY, MQTTClass::mqttEventHandler, this) != ESP_OK) {
    esp_mqtt_client_destroy(this->_client);
    this->_client = nullptr;
    this->lastConnect = millis();
    return false;
  }

  if(esp_mqtt_client_start(this->_client) != ESP_OK) {
    esp_mqtt_client_destroy(this->_client);
    this->_client = nullptr;
    this->lastConnect = millis();
    return false;
  }

  this->_clientStarted = true;
  this->_connected = false;
  this->_publishCount = 0;
  this->resetMessageBuffers();
  this->lastConnect = millis();
  return true;
}

bool MQTTClass::disconnect() {
  this->_connected = false;
  this->_publishCount = 0;
  this->resetMessageBuffers();

  if(this->_client == nullptr) {
    this->_clientStarted = false;
    return true;
  }

  if(this->_clientStarted) {
    fanCtrl.unsubscribe();
    dooyaCtrl.unsubscribe();
    esp_mqtt_client_disconnect(this->_client);
    esp_mqtt_client_stop(this->_client);
  }

  esp_mqtt_client_destroy(this->_client);
  this->_client = nullptr;
  this->_clientStarted = false;
  return true;
}

bool MQTTClass::connected() {
  return settings.MQTT.enabled && this->_connected;
}

bool MQTTClass::subscribe(const char *topic) {
  if(this->_client != nullptr && this->_connected) {
    char fullTopic[128];
    if(!this->buildRootTopic(topic, fullTopic, sizeof(fullTopic))) return false;
    esp_task_wdt_reset();
    return esp_mqtt_client_subscribe(this->_client, fullTopic, 0) >= 0;
  }
  return true;
}

bool MQTTClass::unsubscribe(const char *topic) {
  if(this->_client != nullptr && this->_connected) {
    char fullTopic[128];
    if(!this->buildRootTopic(topic, fullTopic, sizeof(fullTopic))) return false;
    return esp_mqtt_client_unsubscribe(this->_client, fullTopic) >= 0;
  }
  return true;
}

bool MQTTClass::publishRaw(const char *topic, const char *payload, uint16_t len, bool retain) {
  if(this->_client == nullptr || !this->_connected || topic == nullptr) return false;
  const int result = esp_mqtt_client_publish(this->_client, topic, payload, len, 0, retain ? 1 : 0);
  if(result < 0) return false;
  delay(20);
  return true;
}

bool MQTTClass::publish(const char *topic, const char *payload, bool retain) {
  char fullTopic[128];
  if(!this->buildRootTopic(topic, fullTopic, sizeof(fullTopic))) return false;
  return this->publishRaw(fullTopic, payload, payload == nullptr ? 0 : strlen(payload), retain);
}

bool MQTTClass::publish(const char *topic, uint32_t val, bool retain) {
  snprintf(g_content, sizeof(g_content), "%u", val);
  return this->publish(topic, g_content, retain);
}

bool MQTTClass::publish(const char *topic, int8_t val, bool retain) {
  snprintf(g_content, sizeof(g_content), "%d", val);
  return this->publish(topic, g_content, retain);
}

bool MQTTClass::publish(const char *topic, uint8_t val, bool retain) {
  snprintf(g_content, sizeof(g_content), "%u", val);
  return this->publish(topic, g_content, retain);
}

bool MQTTClass::publish(const char *topic, uint16_t val, bool retain) {
  snprintf(g_content, sizeof(g_content), "%u", val);
  return this->publish(topic, g_content, retain);
}

bool MQTTClass::publish(const char *topic, bool val, bool retain) {
  snprintf(g_content, sizeof(g_content), "%s", val ? "true" : "false");
  return this->publish(topic, g_content, retain);
}

bool MQTTClass::publishBuffer(const char *topic, uint8_t *data, uint16_t len, bool retain) {
  char rawTopic[128];
  if(!this->buildRawTopic(topic, rawTopic, sizeof(rawTopic))) return false;
  return this->publishRaw(rawTopic, reinterpret_cast<const char *>(data), len, retain);
}

bool MQTTClass::publishDisco(const char *topic, JsonObject &obj, bool retain) {
  serializeJson(obj, g_content, sizeof(g_content));
  return this->publishBuffer(topic, reinterpret_cast<uint8_t *>(g_content), strlen(g_content), retain);
}

bool MQTTClass::unpublish(const char *topic) {
  char fullTopic[128];
  if(!this->buildRootTopic(topic, fullTopic, sizeof(fullTopic))) return false;
  return this->publishRaw(fullTopic, "", 0, true);
}

void MQTTClass::receive(const char *topic, byte *payload, uint32_t length) {
  esp_task_wdt_reset();
  if(topic == nullptr || payload == nullptr || length == 0) return;

  const size_t len = strlen(topic);
  if(len == 0) return;

  uint8_t slashes = 0;
  size_t ndx = len - 1;
  while(ndx > 0) {
    if(topic[ndx] == '/') slashes++;
    if(slashes == 4) break;
    ndx--;
  }
  if(slashes < 4) return;

  char entityType[8] = {'\0'};
  char entityId[4] = {'\0'};
  char command[32] = {'\0'};
  char value[64] = {'\0'};

  uint8_t i = 0;
  while(ndx < len && topic[ndx] == '/') ndx++;
  while(ndx < len && topic[ndx] != '/') {
    if(i < sizeof(entityType) - 1) entityType[i++] = topic[ndx];
    ndx++;
  }

  i = 0;
  while(ndx < len && topic[ndx] == '/') ndx++;
  while(ndx < len && topic[ndx] != '/') {
    if(i < sizeof(entityId) - 1) entityId[i++] = topic[ndx];
    ndx++;
  }

  i = 0;
  while(ndx < len && topic[ndx] == '/') ndx++;
  while(ndx < len && topic[ndx] != '/') {
    if(i < sizeof(command) - 1) command[i++] = topic[ndx];
    ndx++;
  }

  const uint32_t copyLen = length < sizeof(value) - 1 ? length : sizeof(value) - 1;
  memcpy(value, payload, copyLen);
  value[copyLen] = '\0';

  if(strcmp(entityType, "awnings") == 0) {
    dooyaCtrl.onMqttCommand(entityId, command, value);
    return;
  }

  if(strcmp(entityType, "fans") != 0) return;

  const uint8_t fanId = atoi(entityId);
  fan_device_t *fan = fanCtrl.getFanById(fanId);
  if(fan == nullptr) return;

  if(strcmp(command, "state") == 0) {
    DynamicJsonDocument doc(512);
    DeserializationError err = deserializeJson(doc, payload, length);
    if(!err) {
      JsonObject obj = doc.as<JsonObject>();
      fanCtrl.applyState(fanId, obj);
    }
    esp_task_wdt_reset();
    return;
  }

  uint8_t cmdByte = 255;
  if(strcmp(command, "fan_command") == 0) {
    if(strcmp(value, "ON") == 0) {
      if(fan->fanOn) return;
      cmdByte = static_cast<uint8_t>(fan_commands::fan);
    }
    else if(strcmp(value, "OFF") == 0) {
      if(!fan->fanOn) return;
      cmdByte = static_cast<uint8_t>(fan_commands::fan);
    }
  }
  else if(strcmp(command, "light_command") == 0) {
    if(strcmp(value, "ON") == 0) {
      if(fan->lightOn) return;
      cmdByte = static_cast<uint8_t>(fan_commands::light);
    }
    else if(strcmp(value, "OFF") == 0) {
      if(!fan->lightOn) return;
      cmdByte = static_cast<uint8_t>(fan_commands::light);
    }
  }
  else if(strcmp(command, "preset") == 0) {
    if(strcmp(value, "speed1") == 0) cmdByte = static_cast<uint8_t>(fan_commands::speed1);
    else if(strcmp(value, "speed2") == 0) cmdByte = static_cast<uint8_t>(fan_commands::speed2);
    else if(strcmp(value, "speed3") == 0) cmdByte = static_cast<uint8_t>(fan_commands::speed3);
    else if(strcmp(value, "speed4") == 0) cmdByte = static_cast<uint8_t>(fan_commands::speed4);
    else if(strcmp(value, "speed5") == 0) cmdByte = static_cast<uint8_t>(fan_commands::speed5);
    else if(strcmp(value, "speed6") == 0) cmdByte = static_cast<uint8_t>(fan_commands::speed6);
  }
  else if(strcmp(command, "color") == 0) {
    uint8_t targetColor = 255;
    if(strcmp(value, "cold") == 0) targetColor = 0;
    else if(strcmp(value, "white") == 0) targetColor = 1;
    else if(strcmp(value, "warm") == 0) targetColor = 2;
    if(targetColor < 3) fanCtrl.sendColorCycle(fanId, targetColor);
    return;
  }
  else if(strcmp(command, "direction") == 0) {
    if(strcmp(value, "counter_clockwise") == 0) {
      if(fan->inverted) return;
      cmdByte = static_cast<uint8_t>(fan_commands::invert);
    }
    else if(strcmp(value, "clockwise") == 0) {
      if(!fan->inverted) return;
      cmdByte = static_cast<uint8_t>(fan_commands::invert);
    }
  }
  else if(strcmp(command, "mute") == 0) {
    if(strcmp(value, "ON") == 0) {
      if(fan->muted) return;
      cmdByte = static_cast<uint8_t>(fan_commands::mute);
    }
    else if(strcmp(value, "OFF") == 0) {
      if(!fan->muted) return;
      cmdByte = static_cast<uint8_t>(fan_commands::mute);
    }
  }
  else if(strncmp(command, "button_", 7) == 0 && strcmp(value, "PRESS") == 0) {
    const char *buttonCommand = command + 7;
    if(strcmp(buttonCommand, "color") == 0) cmdByte = static_cast<uint8_t>(fan_commands::color);
    else if(strcmp(buttonCommand, "mute") == 0) cmdByte = static_cast<uint8_t>(fan_commands::mute);
    else if(strcmp(buttonCommand, "invert") == 0) cmdByte = static_cast<uint8_t>(fan_commands::invert);
    else if(strcmp(buttonCommand, "cooldown1h") == 0) cmdByte = static_cast<uint8_t>(fan_commands::cooldown1h);
    else if(strcmp(buttonCommand, "cooldown2h") == 0) cmdByte = static_cast<uint8_t>(fan_commands::cooldown2h);
    else if(strcmp(buttonCommand, "cooldown4h") == 0) cmdByte = static_cast<uint8_t>(fan_commands::cooldown4h);
  }

  if(cmdByte != 255) fanCtrl.sendCommand(fanId, static_cast<fan_commands>(cmdByte));
  esp_task_wdt_reset();
}

void MQTTClass::onConnected() {
  this->_connected = true;
  this->_publishCount = 0;
  this->lastConnect = millis();

  // Always publish status and subscribe on (re)connect
  this->publish("status", "online", true);
  this->publish("ipAddress", settings.IP.ip.toString().c_str(), true);
  this->publish("host", settings.hostname, true);
  this->publish("firmware", settings.fwVersion.name, true);
  this->publish("serverId", settings.serverId, true);
  this->publish("mac", net.mac.c_str(), true);

  if(!this->_discoPublished) {
    // First connect: publish fan/awning state + HA discovery (retained, only needed once per boot)
    fanCtrl.publish();
    dooyaCtrl.publish();
    this->_discoPublished = true;
  }

  fanCtrl.subscribe();
  dooyaCtrl.subscribe();
}

void MQTTClass::onDisconnected() {
  this->_connected = false;
  this->resetMessageBuffers();
}

void MQTTClass::onData(esp_mqtt_event_handle_t event) {
  if(event == nullptr || event->topic == nullptr || event->data == nullptr || event->topic_len <= 0 || event->data_len < 0) return;

  if(event->current_data_offset == 0) {
    const size_t topicLen = event->topic_len < static_cast<int>(sizeof(this->_messageTopic) - 1) ? static_cast<size_t>(event->topic_len) : sizeof(this->_messageTopic) - 1;
    memcpy(this->_messageTopic, event->topic, topicLen);
    this->_messageTopic[topicLen] = '\0';
    this->_messagePayload[0] = '\0';
  }

  const size_t offset = event->current_data_offset < static_cast<int>(sizeof(this->_messagePayload) - 1) ? static_cast<size_t>(event->current_data_offset) : sizeof(this->_messagePayload) - 1;
  const size_t available = sizeof(this->_messagePayload) - 1 - offset;
  const size_t copyLen = event->data_len < static_cast<int>(available) ? static_cast<size_t>(event->data_len) : available;
  memcpy(this->_messagePayload + offset, event->data, copyLen);
  this->_messagePayload[offset + copyLen] = '\0';

  if(event->current_data_offset + event->data_len >= event->total_data_len) {
    MQTTClass::receive(this->_messageTopic, reinterpret_cast<byte *>(this->_messagePayload), strlen(this->_messagePayload));
    this->resetMessageBuffers();
  }
}

void MQTTClass::mqttEventHandler(void *handlerArgs, esp_event_base_t base, int32_t eventId, void *eventData) {
  (void)base;
  MQTTClass *instance = static_cast<MQTTClass *>(handlerArgs);
  if(instance == nullptr && eventData != nullptr) {
    esp_mqtt_event_handle_t event = static_cast<esp_mqtt_event_handle_t>(eventData);
    instance = static_cast<MQTTClass *>(event->user_context);
  }
  if(instance == nullptr) return;

  switch(static_cast<esp_mqtt_event_id_t>(eventId)) {
    case MQTT_EVENT_CONNECTED:
      instance->onConnected();
      break;
    case MQTT_EVENT_DISCONNECTED:
      instance->onDisconnected();
      break;
    case MQTT_EVENT_DATA:
      instance->onData(static_cast<esp_mqtt_event_handle_t>(eventData));
      break;
    default:
      break;
  }
}
