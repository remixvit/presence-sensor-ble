#pragma once
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>

// ============================================================
//  BLE Configuration Interface
//
//  Service UUID:  "181A" (Environmental Sensing — подходит по смыслу)
//  Characteristics:
//    status   [NOTIFY]  — JSON сенсорных данных раз в секунду
//    settings [READ]    — JSON всех настроек из DB
//    command  [WRITE]   — JSON {"key":"...","value":"..."} для записи
// ============================================================

#define BLE_SVC_UUID      "181A"
#define BLE_STATUS_UUID   "2A6E"   // Temperature — переиспользуем для статуса
#define BLE_SETTINGS_UUID "2A6F"   // Humidity — переиспользуем для настроек
#define BLE_COMMAND_UUID  "2A70"   // кастомный

// Коллбек: приложение записало настройку — {"key":"wifi_ssid","value":"MyWifi"}
// или несколько сразу: {"wifi_ssid":"MyWifi","wifi_pass":"pass","wifi_enabled":true}
using BleCommandCb = std::function<void(const String& json)>;

class BleConfig {
public:
    void begin(const char* deviceName, BleCommandCb commandCb) {
        _commandCb = commandCb;

        NimBLEDevice::init(deviceName);
        NimBLEDevice::setPower(ESP_PWR_LVL_P3);  // +3dBm — разумный компромисс

        _server = NimBLEDevice::createServer();
        _server->setCallbacks(&_serverCb);

        NimBLEService* svc = _server->createService(BLE_SVC_UUID);

        // Status: NOTIFY
        _statusChar = svc->createCharacteristic(
            BLE_STATUS_UUID,
            NIMBLE_PROPERTY::NOTIFY
        );

        // Settings: READ
        _settingsChar = svc->createCharacteristic(
            BLE_SETTINGS_UUID,
            NIMBLE_PROPERTY::READ
        );

        // Command: WRITE (без ответа — быстрее)
        _commandChar = svc->createCharacteristic(
            BLE_COMMAND_UUID,
            NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
        );
        _commandChar->setCallbacks(&_cmdCb);

        NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
        adv->addServiceUUID(BLE_SVC_UUID);
        adv->enableScanResponse(true);
        adv->start();

        Serial.printf("[BLE] Запущен: '%s'\n", deviceName);
    }

    // Обновить статусную характеристику (вызывать раз в секунду)
    void updateStatus(const String& json) {
        if (!_server->getConnectedCount()) return;
        _statusChar->setValue(json.c_str());
        _statusChar->notify();
    }

    // Обновить характеристику настроек (после изменения DB)
    void updateSettings(const String& json) {
        _settingsChar->setValue(json.c_str());
    }

    bool connected() const {
        return _server && _server->getConnectedCount() > 0;
    }

    void setCommandCallback(BleCommandCb cb) { _commandCb = cb; }

    // Нужен для доступа из колбека
    BleCommandCb _commandCb;

private:
    NimBLEServer*         _server       = nullptr;
    NimBLECharacteristic* _statusChar   = nullptr;
    NimBLECharacteristic* _settingsChar = nullptr;
    NimBLECharacteristic* _commandChar  = nullptr;

    struct ServerCb : public NimBLEServerCallbacks {
        void onConnect(NimBLEServer* s, NimBLEConnInfo& connInfo) override {
            Serial.printf("[BLE] Клиент подключён. Всего: %d\n",
                s->getConnectedCount());
            // Перезапускаем рекламу — разрешаем несколько клиентов
            NimBLEDevice::getAdvertising()->start();
        }
        void onDisconnect(NimBLEServer* s, NimBLEConnInfo& connInfo, int reason) override {
            Serial.printf("[BLE] Клиент отключился. Всего: %d\n",
                s->getConnectedCount());
        }
    } _serverCb;

    struct CommandCb : public NimBLECharacteristicCallbacks {
        BleConfig* parent = nullptr;
        void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
            if (!parent || !parent->_commandCb) return;
            String val = c->getValue().c_str();
            if (val.length()) parent->_commandCb(val);
        }
    } _cmdCb;

    // Связываем командный коллбек с родителем после конструктора
    void _linkCb() { _cmdCb.parent = this; }

public:
    BleConfig() { _linkCb(); }
};

inline BleConfig bleCfg;
