#ifndef MQTT_H
#define MQTT_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <mqtt_client.h>

class MQTTClass {
  public:
    uint64_t lastConnect = 0;
    bool suspended = false;
    char clientId[32] = {'\0'};

    bool begin();
    bool loop();
    bool end();
    bool connect();
    bool disconnect();
    bool connected();
    void reset();
    bool unpublish(const char *topic);
    bool publish(const char *topic, const char *payload, bool retain = false);
    bool publish(const char *topic, uint8_t val, bool retain = false);
    bool publish(const char *topic, int8_t val, bool retain = false);
    bool publish(const char *topic, uint32_t val, bool retain = false);
    bool publish(const char *topic, uint16_t val, bool retain = false);
    bool publish(const char *topic, bool val, bool retain = false);
    bool publishBuffer(const char *topic, uint8_t *data, uint16_t len, bool retain = false);
    bool publishDisco(const char *topic, JsonObject &obj, bool retain = false);
    bool subscribe(const char *topic);
    bool unsubscribe(const char *topic);
    static void receive(const char *topic, byte *payload, uint32_t length);

  private:
    esp_mqtt_client_handle_t _client = nullptr;
    bool _clientStarted = false;
    bool _connected = false;
    bool _discoPublished = false;
    uint16_t _publishCount = 0;
    char _brokerUri[128] = {'\0'};
    char _lwtTopic[128] = {'\0'};
    char _messageTopic[160] = {'\0'};
    char _messagePayload[128] = {'\0'};

    bool buildRootTopic(const char *topic, char *buffer, size_t len);
    bool buildRawTopic(const char *topic, char *buffer, size_t len);
    bool publishRaw(const char *topic, const char *payload, uint16_t len, bool retain);
    void buildClientId();
    void buildBrokerUri();
    void resetMessageBuffers();
    void onConnected();
    void onDisconnected();
    void onData(esp_mqtt_event_handle_t event);
    static void mqttEventHandler(void *handlerArgs, esp_event_base_t base, int32_t eventId, void *eventData);
};

#endif
