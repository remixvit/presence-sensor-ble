#pragma once
#include <GyverDB.h>

// Все ключи персистентного хранилища
DB_KEYS(kk,
    // Устройство
    device_id,

    // WiFi
    wifi_enabled,
    wifi_ssid,
    wifi_pass,

    // MQTT / Home Assistant
    mqtt_enabled,
    mqtt_broker,
    mqtt_port,
    mqtt_user,
    mqtt_pass,
    pub_interval,

    // Сенсор LD2410C
    sensor_maxdist,

    // VL53L1X
    vl53_threshold,

    // Логика двери
    door_approach_delta,
    door_open_dist,
    door_close_delay
);
