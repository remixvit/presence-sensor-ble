#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>

#include <GyverDBFile.h>
#include <PubSubClient.h>
#include <VL53L1X.h>

#include "db_keys.h"
#include "pins.h"
#include "ld2410.h"
#include "door_logic.h"
#include "wifi_manager.h"
#include "ble_config.h"
#include "ble_ota.h"

#define FW_VERSION    "1.1.0"
#define DEVICE_MODEL  "presence-c3-v1"

// ============================================================
//  Глобальные объекты
// ============================================================
GyverDBFile db(&LittleFS, "/config.db");

WiFiClient   wifiClient;
PubSubClient mqttClient(wifiClient);

LD2410   sensor;
VL53L1X  vl53;
DoorLogic door;

// ============================================================
//  Данные сенсора — shared между sensorTask и loop()
// ============================================================
struct SensorSnap {
    volatile bool     presence    = false;
    volatile bool     isMoving   = false;
    volatile bool     isStatic   = false;
    volatile uint16_t movingDist = 0;
    volatile uint16_t staticDist = 0;
    volatile uint8_t  movingEnergy = 0;
    volatile uint16_t vl53dist   = 9999;
    volatile bool     zoneBlocked = false;
    volatile bool     doorOpen    = false;
    volatile uint8_t  moveDir    = 0;
};
SensorSnap snap;

// Статус сенсоров
bool vl53ok     = false;
bool vl53Failed = false;
bool ld2410Failed = false;


// Кэш настроек (loop пишет, sensorTask читает)
volatile uint16_t cfg_vl53Threshold  = 500;
volatile uint16_t cfg_approachDelta  = 15;
volatile uint16_t cfg_openDist       = 200;
volatile uint32_t cfg_closeDelay     = 3000;

// ============================================================
//  MQTT
// ============================================================
char deviceId[32];
char topicState[80];
char topicAvail[80];

bool mqttNeedsConnect = false;
bool firstPublish     = true;
unsigned long lastPublish    = 0;
unsigned long lastMqttCheck  = 0;

// ============================================================
//  Device ID
// ============================================================
void setupIds() {
    String id = (String)db[kk::device_id];
    id.trim();
    if (id.isEmpty()) {
        uint64_t chipId = ESP.getEfuseMac();
        id = String((uint32_t)(chipId & 0xFFFFFF), HEX);
    }
    id.toCharArray(deviceId, sizeof(deviceId));
    snprintf(topicState, sizeof(topicState), "presence/%s/state", deviceId);
    snprintf(topicAvail, sizeof(topicAvail), "presence/%s/avail", deviceId);
}

// ============================================================
//  MQTT
// ============================================================
void publishDiscovery() {
    char cfgTopic[120];
    JsonDocument doc;

    // Имя устройства: кастомное или дефолтное
    String devCustomName = (String)db[kk::device_name];
    devCustomName.trim();
    String devDisplayName = devCustomName.isEmpty()
        ? String("Sensor ") + deviceId
        : devCustomName;

    auto makeDevice = [&]() {
        doc["device"]["ids"][0]  = deviceId;
        doc["device"]["name"]    = devDisplayName;
        doc["device"]["model"]   = "ESP32-C3 + LD2410C";
        doc["device"]["mf"]      = "DIY";
        doc["avty_t"]            = topicAvail;
        doc["avty_tpl"]          = "{{ value_json.state }}";
    };

    auto publish = [&](const char* topic) {
        String payload;
        serializeJson(doc, payload);
        mqttClient.publish(topic, payload.c_str(), true);
        doc.clear();
        delay(20); // небольшая пауза между discovery-пакетами
    };

    // Присутствие
    snprintf(cfgTopic, sizeof(cfgTopic), "homeassistant/binary_sensor/%s/presence/config", deviceId);
    makeDevice();
    doc["name"]        = "Presence";
    doc["uniq_id"]     = String(deviceId) + "_presence";
    doc["stat_t"]      = topicState;
    doc["val_tpl"]     = "{{ 'on' if value_json.presence else 'off' }}";
    doc["payload_on"]  = "on";
    doc["payload_off"] = "off";
    doc["dev_cla"]     = "occupancy";
    publish(cfgTopic);

    // Активатор (сигнал открытия)
    snprintf(cfgTopic, sizeof(cfgTopic), "homeassistant/binary_sensor/%s/activator/config", deviceId);
    makeDevice();
    doc["name"]        = "Activator";
    doc["uniq_id"]     = String(deviceId) + "_activator";
    doc["stat_t"]      = topicState;
    doc["val_tpl"]     = "{{ value_json.activator }}";
    doc["payload_on"]  = "on";
    doc["payload_off"] = "off";
    doc["dev_cla"]     = "opening";
    publish(cfgTopic);

    // Безопасность (VL53L1X зона)
    snprintf(cfgTopic, sizeof(cfgTopic), "homeassistant/binary_sensor/%s/safety/config", deviceId);
    makeDevice();
    doc["name"]        = "Safety Zone";
    doc["uniq_id"]     = String(deviceId) + "_safety";
    doc["stat_t"]      = topicState;
    doc["val_tpl"]     = "{{ 'on' if value_json.safety else 'off' }}";
    doc["payload_on"]  = "on";
    doc["payload_off"] = "off";
    doc["dev_cla"]     = "occupancy";
    publish(cfgTopic);

    // Расстояние
    snprintf(cfgTopic, sizeof(cfgTopic), "homeassistant/sensor/%s/distance/config", deviceId);
    makeDevice();
    doc["name"]                = "Distance";
    doc["uniq_id"]             = String(deviceId) + "_dist";
    doc["stat_t"]              = topicState;
    doc["val_tpl"]             = "{{ value_json.dist }}";
    doc["unit_of_measurement"] = "cm";
    doc["dev_cla"]             = "distance";
    publish(cfgTopic);

    // Направление
    snprintf(cfgTopic, sizeof(cfgTopic), "homeassistant/sensor/%s/direction/config", deviceId);
    makeDevice();
    doc["name"]     = "Direction";
    doc["uniq_id"]  = String(deviceId) + "_dir";
    doc["stat_t"]   = topicState;
    doc["val_tpl"]  = "{{ value_json.dir }}";
    publish(cfgTopic);
}

void publishState() {
    JsonDocument doc;
    doc["state"]     = "online";
    doc["presence"]  = snap.presence;
    doc["dist"]      = snap.movingDist ? snap.movingDist : snap.staticDist;
    doc["activator"] = snap.doorOpen ? "on" : "off";
    doc["safety"]    = snap.zoneBlocked;
    doc["dir"]       = door.directionStr();
    String payload;
    serializeJson(doc, payload);
    mqttClient.publish(topicState, payload.c_str());
}


bool connectMQTT() {
    String broker = (String)db[kk::mqtt_broker];
    if (broker.isEmpty()) return false;

    mqttClient.setServer(broker.c_str(), (int)db[kk::mqtt_port]);
    mqttClient.setBufferSize(1024);
    mqttClient.setSocketTimeout(3);
    mqttClient.setKeepAlive(10);

    String clientId = String("sensor-") + deviceId;
    String user = (String)db[kk::mqtt_user];
    String pass = (String)db[kk::mqtt_pass];

    bool ok = user.isEmpty()
        ? mqttClient.connect(clientId.c_str(), topicAvail, 1, true, "{\"state\":\"offline\"}")
        : mqttClient.connect(clientId.c_str(),
                             user.c_str(), pass.c_str(),
                             topicAvail, 1, true, "{\"state\":\"offline\"}");
    if (ok) {
        Serial.printf("[MQTT] Подключён к %s\n", broker.c_str());
        publishDiscovery();
        mqttClient.publish(topicAvail, "{\"state\":\"online\"}", true);
        firstPublish = true;
    } else {
        Serial.printf("[MQTT] Ошибка: %d\n", mqttClient.state());
    }
    return ok;
}

// ============================================================
//  BLE: сборка JSON настроек для чтения
// ============================================================
String buildSettingsJson() {
    JsonDocument doc;
    doc["device_id"]          = deviceId;
    doc["device_name"]        = (String)db[kk::device_name];
    doc["wifi_enabled"]       = (bool)db[kk::wifi_enabled];
    doc["wifi_ssid"]          = (String)db[kk::wifi_ssid];
    doc["wifi_pass"]          = "";   // не раскрываем, но поле должно быть в UI
    doc["mqtt_enabled"]       = (bool)db[kk::mqtt_enabled];
    doc["mqtt_broker"]        = (String)db[kk::mqtt_broker];
    doc["mqtt_port"]          = (int)db[kk::mqtt_port];
    doc["mqtt_user"]          = (String)db[kk::mqtt_user];
    doc["mqtt_pass"]          = "";   // не раскрываем
    doc["pub_interval"]       = (int)db[kk::pub_interval];
    doc["sensor_maxdist"]     = (int)db[kk::sensor_maxdist];
    doc["vl53_threshold"]     = (int)db[kk::vl53_threshold];
    doc["door_approach_delta"]= (int)db[kk::door_approach_delta];
    doc["door_open_dist"]     = (int)db[kk::door_open_dist];
    doc["door_close_delay"]   = (int)db[kk::door_close_delay];
    // Статус (readonly)
    doc["vl53_ok"]   = vl53ok;
    doc["ld2410_ok"] = !ld2410Failed;
    doc["ip"]        = WiFi.localIP().toString();
    String out;
    serializeJson(doc, out);
    return out;
}

// ============================================================
//  BLE: сборка JSON статуса для NOTIFY
// ============================================================
String buildStatusJson() {
    JsonDocument doc;
    // Идентификация устройства
    doc["model"]  = DEVICE_MODEL;
    doc["fw"]     = FW_VERSION;
    // Capabilities — приложение адаптирует UI под конкретную модель
    JsonArray caps = doc["caps"].to<JsonArray>();
    caps.add("ld2410");
    if (vl53ok) caps.add("vl53");
    caps.add("activator");
    if ((bool)db[kk::wifi_enabled] || true) caps.add("wifi");   // всегда поддерживается
    caps.add("mqtt");
    caps.add("ota_ble");

    doc["presence"] = snap.presence;
    doc["dist"]     = snap.movingDist ? snap.movingDist : snap.staticDist;
    doc["moving"]   = snap.isMoving;
    doc["energy"]   = snap.movingEnergy;
    doc["dir"]      = door.directionStr();
    doc["activator"] = snap.doorOpen ? "on" : "off";
    doc["safety"]    = snap.zoneBlocked;
    doc["vl53"]     = snap.vl53dist;
    doc["ld_ok"]    = !ld2410Failed;
    doc["vl53_ok"]  = vl53ok && !vl53Failed;
    doc["heap"]     = esp_get_free_heap_size() / 1024;
    doc["heap_min"] = esp_get_minimum_free_heap_size() / 1024;
    doc["uptime"]   = millis() / 1000;
    // WiFi: IP / статус
    if (wifiMgr.connected())
        doc["wifi"] = WiFi.localIP().toString();
    else if (wifiMgr.connecting())
        doc["wifi"] = "подключение...";
    else if ((bool)db[kk::wifi_enabled])
        doc["wifi"] = "ошибка";
    else
        doc["wifi"] = "выключен";

    // MQTT: статус + код ошибки
    if (mqttClient.connected())
        doc["mqtt"] = "подключён";
    else if (!wifiMgr.connected() || !(bool)db[kk::mqtt_enabled])
        doc["mqtt"] = "выключен";
    else
        doc["mqtt"] = String("ошибка: ") + mqttClient.state();
    String out;
    serializeJson(doc, out);
    return out;
}

// ============================================================
//  BLE: обработка команды от приложения
// ============================================================
void onBleCommand(const String& json) {
    Serial.printf("[BLE] Команда: %s\n", json.c_str());

    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) {
        Serial.println("[BLE] JSON parse error");
        return;
    }

    bool changed = false;
    bool wifiChanged = false;
    bool mqttChanged = false;

    for (JsonPair kv : doc.as<JsonObject>()) {
        String key = kv.key().c_str();

        if      (key == "device_id")            { db[kk::device_id]            = kv.value().as<String>(); changed = true; }
        else if (key == "device_name")          { db[kk::device_name]          = kv.value().as<String>(); changed = true; }
        else if (key == "wifi_enabled")         { db[kk::wifi_enabled]         = kv.value().as<bool>();   wifiChanged = true; changed = true; }
        else if (key == "wifi_ssid")            { db[kk::wifi_ssid]            = kv.value().as<String>(); wifiChanged = true; changed = true; }
        else if (key == "wifi_pass")            { db[kk::wifi_pass]            = kv.value().as<String>(); wifiChanged = true; changed = true; }
        else if (key == "mqtt_enabled")         { db[kk::mqtt_enabled]         = kv.value().as<bool>();   mqttChanged = true; changed = true; }
        else if (key == "mqtt_broker")          { db[kk::mqtt_broker]          = kv.value().as<String>(); mqttChanged = true; changed = true; }
        else if (key == "mqtt_port")            { db[kk::mqtt_port]            = (uint16_t)kv.value().as<int>(); mqttChanged = true; changed = true; }
        else if (key == "mqtt_user")            { db[kk::mqtt_user]            = kv.value().as<String>(); mqttChanged = true; changed = true; }
        else if (key == "mqtt_pass")            { db[kk::mqtt_pass]            = kv.value().as<String>(); mqttChanged = true; changed = true; }
        else if (key == "pub_interval")         { db[kk::pub_interval]         = (uint16_t)kv.value().as<int>(); changed = true; }
        else if (key == "sensor_maxdist")       { db[kk::sensor_maxdist]       = (uint16_t)kv.value().as<int>(); changed = true; }
        else if (key == "vl53_threshold")       { db[kk::vl53_threshold]       = (uint16_t)kv.value().as<int>(); changed = true; }
        else if (key == "door_approach_delta")  { db[kk::door_approach_delta]  = (uint16_t)kv.value().as<int>(); changed = true; }
        else if (key == "door_open_dist")       { db[kk::door_open_dist]       = (uint16_t)kv.value().as<int>(); changed = true; }
        else if (key == "door_close_delay")     { db[kk::door_close_delay]     = (uint16_t)kv.value().as<int>(); changed = true; }
    }

    if (changed) {
        db.update();
        setupIds();
        // Если поменялось имя — применяем сразу в BLE-рекламе
        if (doc.containsKey("device_name")) {
            String n = (String)db[kk::device_name]; n.trim();
            String newName = n.isEmpty() ? String("Sensor-") + deviceId : n;
            bleCfg.setName(newName.c_str());
        }
        bleCfg.updateSettings(buildSettingsJson());
    }

    if (wifiChanged) {
        bool en = (bool)db[kk::wifi_enabled];
        if (en) wifiMgr.reconnect((String)db[kk::wifi_ssid], (String)db[kk::wifi_pass]);
        else    wifiMgr.stop();
    }

    if (mqttChanged && !wifiChanged) {
        if (mqttClient.connected()) mqttClient.disconnect();
        mqttNeedsConnect = (bool)db[kk::mqtt_enabled] && wifiMgr.connected();
    }
}

// ============================================================
//  Sensor Task — Core 0, priority 5
// ============================================================
void sensorTask(void*) {
    esp_task_wdt_add(NULL);

    TickType_t lastWake = xTaskGetTickCount();

    uint16_t heartbeatCount = 0;
    uint8_t  errorBlinkTick = 0;

    // VL53: время последнего успешного чтения
    unsigned long lastVl53Read = millis();

    // LD2410: время последнего валидного кадра
    unsigned long lastLd2410Frame = millis();

    while (true) {
        esp_task_wdt_reset();

        // ── LD2410 ──────────────────────────────────────────
        if (sensor.update()) {
            lastLd2410Frame = millis();  // любой фрейм — датчик жив
        }

        if (!ld2410Failed && millis() - lastLd2410Frame > 3000) {
            ld2410Failed = true;
            snap.presence   = false;
            snap.isMoving   = false;
            snap.isStatic   = false;
            snap.movingDist = 0;
            snap.staticDist = 0;
            digitalWrite(DOOR_OPEN_PIN, LOW);
            Serial.println("[LD2410] Датчик не отвечает — аварийный режим");
        }

        if (!ld2410Failed) {
            const LD2410Data& d = sensor.data();
            snap.presence    = d.presence();
            snap.isMoving    = d.isMoving();
            snap.isStatic    = d.isStatic();
            snap.movingDist  = d.movingDist;
            snap.staticDist  = d.staticDist;
            snap.movingEnergy= d.movingEnergy;
        }

        // ── VL53L1X ─────────────────────────────────────────
        if (vl53ok && !vl53Failed) {
            if (vl53.dataReady()) {
                vl53.read(false);
                if (!vl53.timeoutOccurred()) {
                    lastVl53Read     = millis();
                    snap.vl53dist    = vl53.ranging_data.range_mm;
                    snap.zoneBlocked = snap.vl53dist < cfg_vl53Threshold;
                    digitalWrite(DOOR_ZONE_PIN, snap.zoneBlocked ? HIGH : LOW);
                }
            }
            if (millis() - lastVl53Read > 3000) {
                vl53Failed       = true;
                snap.zoneBlocked = false;
                digitalWrite(DOOR_ZONE_PIN, LOW);
                Serial.println("[VL53] Датчик не отвечает — аварийный режим");
            }
        }

        // ── Логика двери — раз в секунду ────────────────────
        static unsigned long lastDoorUpdate = 0;
        if (millis() - lastDoorUpdate >= 1000) {
            lastDoorUpdate = millis();
            const LD2410Data& d = sensor.data();
            door.update(
                ld2410Failed ? 0     : d.movingDist,
                ld2410Failed ? false : d.presence(),
                cfg_approachDelta,
                cfg_openDist,
                cfg_closeDelay
            );
            snap.doorOpen = door.isActivated();
            snap.moveDir  = (uint8_t)door.direction();
        }

        // ── Heartbeat LED ────────────────────────────────────
        if (++heartbeatCount >= 10) {
            heartbeatCount = 0;
            digitalWrite(LED_HEARTBEAT_PIN, !digitalRead(LED_HEARTBEAT_PIN));
        }

        // ── Error LED ────────────────────────────────────────
        {
            bool vl53Err = vl53ok && vl53Failed;
            if (ld2410Failed && vl53Err) {
                errorBlinkTick = 0;
                digitalWrite(LED_ERROR_PIN, HIGH);
            } else if (ld2410Failed) {
                digitalWrite(LED_ERROR_PIN, !digitalRead(LED_ERROR_PIN));
            } else if (vl53Err) {
                if (++errorBlinkTick >= 5) {
                    errorBlinkTick = 0;
                    digitalWrite(LED_ERROR_PIN, !digitalRead(LED_ERROR_PIN));
                }
            } else {
                errorBlinkTick = 0;
                digitalWrite(LED_ERROR_PIN, LOW);
            }
        }

        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(50));
    }
}

// ============================================================
//  Setup
// ============================================================
void setup() {
    Serial.begin(115200);
    { uint32_t t = millis(); while (!Serial && millis() - t < 5000) delay(10); }
    Serial.println("\n=== Presence Sensor BLE ===");

    LittleFS.begin(true);
    db.begin();

    // Дефолтные значения
    db.init(kk::device_id,           (String)"");
    db.init(kk::device_name,         (String)"");
    db.init(kk::wifi_enabled,        (bool)false);
    db.init(kk::wifi_ssid,           (String)"");
    db.init(kk::wifi_pass,           (String)"");
    db.init(kk::mqtt_enabled,        (bool)false);
    db.init(kk::mqtt_broker,         (String)"");
    db.init(kk::mqtt_port,           (uint16_t)1883);
    db.init(kk::mqtt_user,           (String)"");
    db.init(kk::mqtt_pass,           (String)"");
    db.init(kk::pub_interval,        (uint16_t)500);
    db.init(kk::sensor_maxdist,      (uint16_t)300);
    db.init(kk::vl53_threshold,      (uint16_t)500);
    db.init(kk::door_approach_delta, (uint16_t)15);
    db.init(kk::door_open_dist,      (uint16_t)200);
    db.init(kk::door_close_delay,    (uint16_t)3000);

    setupIds();

    // Кэш настроек
    cfg_vl53Threshold = (int)db[kk::vl53_threshold];
    cfg_approachDelta = (int)db[kk::door_approach_delta];
    cfg_openDist      = (int)db[kk::door_open_dist];
    cfg_closeDelay    = (uint32_t)(int)db[kk::door_close_delay];

    // GPIO
    pinMode(DOOR_OPEN_PIN,     OUTPUT); digitalWrite(DOOR_OPEN_PIN, LOW);
    pinMode(DOOR_ZONE_PIN,     OUTPUT); digitalWrite(DOOR_ZONE_PIN, LOW);
    pinMode(LED_HEARTBEAT_PIN, OUTPUT); digitalWrite(LED_HEARTBEAT_PIN, LOW);
    pinMode(LED_ERROR_PIN,     OUTPUT); digitalWrite(LED_ERROR_PIN, LOW);
    door.begin(DOOR_OPEN_PIN);

    // VL53L1X
    Wire.begin(VL53_SDA_PIN, VL53_SCL_PIN);
    Wire.setClock(100000);
    Wire.setTimeOut(10);
    delay(200);
    vl53.setTimeout(500);
    vl53ok = vl53.init();
    Serial.printf("[VL53L1X] init() = %s\n", vl53ok ? "OK" : "не найден");
    if (vl53ok) {
        vl53.setDistanceMode(VL53L1X::Short);
        vl53.setMeasurementTimingBudget(50000);
        vl53.startContinuous(100);
    }

    // LD2410C
    Serial1.begin(256000, SERIAL_8N1, LD2410_RX_PIN, LD2410_TX_PIN);
    sensor.begin(Serial1, LD2410_RX_PIN, LD2410_TX_PIN, LD2410_OUT_PIN);
    Serial.printf("[LD2410] RX=%d TX=%d OUT=%d\n",
        LD2410_RX_PIN, LD2410_TX_PIN, LD2410_OUT_PIN);

    // BLE
    String customName = (String)db[kk::device_name];
    customName.trim();
    String bleName = customName.isEmpty() ? String("Sensor-") + deviceId : customName;
    bleCfg.begin(bleName.c_str(), onBleCommand, [](NimBLEServer* srv) {
        NimBLEService* otaSvc = bleOta.createService(srv);
        otaSvc->start();
        Serial.println("[OTA] BLE OTA сервис запущен");
    });
    bleCfg.updateSettings(buildSettingsJson());

    // WiFi — колбэки регистрируем всегда, запускаем только если включён
    WiFi.onEvent([](WiFiEvent_t ev, WiFiEventInfo_t info) {
        wifiMgr.notifyEvent(ev, info);
    });

    wifiMgr.setTxPower(WIFI_POWER_11dBm);
    wifiMgr.setTimeout(20);
    wifiMgr.setMaxRetries(3);
    wifiMgr.onConnect([]() {
        setupIds();
        if ((bool)db[kk::mqtt_enabled]) mqttNeedsConnect = true;
        bleCfg.updateSettings(buildSettingsJson());
    });
    wifiMgr.onDisconnect([]() {
        if (mqttClient.connected()) mqttClient.disconnect();
    });
    wifiMgr.onError([](const char* reason) {
        Serial.printf("[WiFi] Ошибка: %s\n", reason);
    });

    if ((bool)db[kk::wifi_enabled]) {
        wifiMgr.begin((String)db[kk::wifi_ssid], (String)db[kk::wifi_pass]);
    } else {
        Serial.println("[WiFi] Отключён (wifi_enabled=false)");
    }

    // Sensor task — приоритет 5
    xTaskCreatePinnedToCore(sensorTask, "sensor", 6144, nullptr, 5, nullptr, 0);

    Serial.printf("[INFO] Device: %s  BLE: %s\n", deviceId, bleName.c_str());
}

// ============================================================
//  Loop — приоритет 1 (ниже sensorTask)
// ============================================================
static unsigned long timerStatus = 0;

void loop() {
    delay(1);

    bleOta.loop();   // отложенная перезагрузка после OTA
    wifiMgr.tick();

    unsigned long now = millis();

    // ── BLE статус раз в секунду ─────────────────────────────
    if (now - timerStatus >= 1000) {
        timerStatus = now;

        // Кэш настроек
        cfg_vl53Threshold = (int)db[kk::vl53_threshold];
        cfg_approachDelta = (int)db[kk::door_approach_delta];
        cfg_openDist      = (int)db[kk::door_open_dist];
        cfg_closeDelay    = (uint32_t)(int)db[kk::door_close_delay];

        // BLE notify
        if (bleCfg.connected()) {
            bleCfg.updateStatus(buildStatusJson());
        }

        // Лог в Serial
        if (!wifiMgr.connected() && (bool)db[kk::wifi_enabled]) {
            if (wifiMgr.connecting())
                Serial.printf("[WiFi] Подключаюсь... (%lus)\n", now / 1000);
        }
    }

    // ── MQTT ────────────────────────────────────────────────
    if (!wifiMgr.connected()) return;
    if (!(bool)db[kk::mqtt_enabled]) return;

    if (mqttNeedsConnect) {
        mqttNeedsConnect = false;
        lastMqttCheck = now;
        connectMQTT();
    }

    if (now - lastMqttCheck >= 15000) {
        lastMqttCheck = now;
        if (!mqttClient.connected()) connectMQTT();
    }

    if (mqttClient.connected()) {
        mqttClient.loop();

        uint16_t interval = (int)db[kk::pub_interval];
        if (!interval) interval = 500;

        static bool prevPresence = false;
        bool changed = (snap.presence != prevPresence);
        if (changed || (now - lastPublish >= interval) || firstPublish) {
            publishState();
            prevPresence = snap.presence;
            lastPublish  = now;
            firstPublish = false;
        }
    }
}
