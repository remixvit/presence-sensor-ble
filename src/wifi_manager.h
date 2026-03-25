#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <functional>

// ============================================================
//  WifiManager — STA only, без AP.
//  Конфигурация через BLE, AP не нужен.
// ============================================================
class WifiManager {
public:
    using Cb    = std::function<void()>;
    using ErrCb = std::function<void(const char*)>;

    enum class State : uint8_t { Idle, Connecting, Connected };

    void setTxPower(wifi_power_t power) { _txPower = power; }
    void setTimeout(uint16_t sec)       { _timeout = sec * 1000UL; }
    void setMaxRetries(uint8_t n)       { _maxRetry = n; }

    void onConnect   (Cb    cb) { _cbConnect    = cb; }
    void onDisconnect(Cb    cb) { _cbDisconnect = cb; }
    void onError     (ErrCb cb) { _cbError      = cb; }

    // Запустить подключение. Пустой ssid → ничего не делаем.
    void begin(const String& ssid, const String& pass) {
        if (ssid.isEmpty()) {
            Serial.println("[WiFi] SSID не задан, WiFi не запущен");
            return;
        }
        _ssid = ssid;
        _pass = pass;
        _retryCount = 0;

        WiFi.mode(WIFI_STA);
        WiFi.setTxPower(_txPower);

        if (_scan()) _connect();
        else if (_cbError) _cbError("сеть не найдена при скане");
    }

    void tick() {
        if (_state != State::Connecting) return;

        wl_status_t status = WiFi.status();
        if (status == WL_CONNECTED) {
            _retryCount = 0;
            _state = State::Connected;
            Serial.printf("[WiFi] Подключён: %s\n",
                WiFi.localIP().toString().c_str());
            if (_cbConnect) _cbConnect();
            return;
        }
        if (millis() - _connectStart < _timeout) return;

        const char* err = _statusStr(status);
        Serial.printf("[WiFi] Таймаут (%s), попытка %d/%d\n",
            err, _retryCount + 1, _maxRetry);

        if (++_retryCount < _maxRetry) {
            _connect();
        } else {
            Serial.println("[WiFi] Пересканирую...");
            _retryCount = 0;
            if (!_scan()) {
                if (_cbError) _cbError(err);
                _state = State::Idle;
            } else {
                _connect();
            }
        }
    }

    void notifyDisconnect() {
        if (_state != State::Connected) return;
        Serial.println("[WiFi] Соединение потеряно, переподключаюсь...");
        _state = State::Connecting;
        if (_cbDisconnect) _cbDisconnect();
        _connect();
    }

    void reconnect(const String& ssid, const String& pass) {
        WiFi.disconnect(false);
        _state = State::Idle;
        begin(ssid, pass);
    }

    void stop() {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        _state = State::Idle;
        Serial.println("[WiFi] Остановлен");
    }

    bool connected()  const { return _state == State::Connected;  }
    bool connecting() const { return _state == State::Connecting; }
    State state()     const { return _state; }

    void notifyEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
        switch (event) {
            case ARDUINO_EVENT_WIFI_STA_CONNECTED:
                Serial.printf("[WiFi] Ассоциирован с '%s'\n",
                    (char*)info.wifi_sta_connected.ssid);
                break;
            case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
                uint8_t r = info.wifi_sta_disconnected.reason;
                const char* txt =
                    r == 15  ? "неверный пароль" :
                    r == 200 ? "AP пропала"      :
                    r == 201 ? "AP не найдена"   : "другая";
                Serial.printf("[WiFi] Разрыв, причина %d: %s\n", r, txt);
                notifyDisconnect();
                break;
            }
            default: break;
        }
    }

private:
    String  _ssid, _pass;
    uint8_t _bssid[6] = {};
    int     _channel   = 0;
    bool    _hasBssid  = false;

    uint32_t     _timeout      = 20000;
    uint32_t     _connectStart = 0;
    wifi_power_t _txPower      = WIFI_POWER_11dBm;
    State        _state        = State::Idle;
    uint8_t      _retryCount   = 0;
    uint8_t      _maxRetry     = 3;

    Cb    _cbConnect, _cbDisconnect;
    ErrCb _cbError;

    bool _scan() {
        Serial.println("[WiFi] Сканирую...");
        int n = WiFi.scanNetworks(false, false);
        int bestIdx = -1, bestRssi = -999;
        for (int i = 0; i < n; i++) {
            Serial.printf("  [%d] '%s'  RSSI:%d  BSSID:%s  ch:%d\n",
                i, WiFi.SSID(i).c_str(), WiFi.RSSI(i),
                WiFi.BSSIDstr(i).c_str(), WiFi.channel(i));
            if (WiFi.SSID(i) == _ssid && WiFi.RSSI(i) > bestRssi) {
                bestRssi = WiFi.RSSI(i); bestIdx = i;
            }
        }
        if (bestIdx < 0) {
            WiFi.scanDelete();
            Serial.printf("[WiFi] '%s' не найдена\n", _ssid.c_str());
            _hasBssid = false;
            return false;
        }
        memcpy(_bssid, WiFi.BSSID(bestIdx), 6);
        _channel  = WiFi.channel(bestIdx);
        _hasBssid = true;
        Serial.printf("[WiFi] -> BSSID:%s  ch:%d  RSSI:%d\n",
            WiFi.BSSIDstr(bestIdx).c_str(), _channel, bestRssi);
        WiFi.scanDelete();
        return true;
    }

    void _connect() {
        _state        = State::Connecting;
        _connectStart = millis();
        WiFi.setTxPower(_txPower);
        if (_hasBssid)
            WiFi.begin(_ssid.c_str(), _pass.c_str(), _channel, _bssid);
        else
            WiFi.begin(_ssid.c_str(), _pass.c_str());
        Serial.printf("[WiFi] Подключаюсь к '%s'  попытка %d/%d...\n",
            _ssid.c_str(), _retryCount + 1, _maxRetry);
    }

    static const char* _statusStr(wl_status_t s) {
        switch (s) {
            case WL_NO_SSID_AVAIL:   return "сеть не найдена";
            case WL_CONNECT_FAILED:  return "неверный пароль";
            case WL_CONNECTION_LOST: return "соединение потеряно";
            case WL_DISCONNECTED:    return "отключён";
            default:                 return "неизвестная ошибка";
        }
    }
};

inline WifiManager wifiMgr;
