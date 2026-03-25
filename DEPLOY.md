# Деплой прошивки на OTA сервер

## Требования

- Python 3.x
- Библиотека `requests`: `pip install requests`
- PlatformIO

---

## Процесс выпуска новой версии

### 1. Внести изменения в прошивку

Редактируй файлы в `src/`. Не забудь обновить версию в `src/main.cpp`:

```cpp
#define FW_VERSION "1.2.0"
```

### 2. Скомпилировать

```
pio run
```

Или через кнопку **Build** в VS Code (PlatformIO).

Убедись что компиляция прошла без ошибок и прошивка влезает в партицию:
```
Flash: [========  ]  81.4% (used 1 279 896 bytes from 1 572 864 bytes)
```

### 3. Залить на плату и протестировать

```
pio run -t upload
```

Проверь что всё работает через Flutter приложение.

### 4. Задеплоить на сервер

```
python deploy.py
```

Скрипт спросит:
- **Версия** — например `1.2.0` (должна совпадать с `FW_VERSION` в коде)
- **Что изменилось** — краткое описание для пользователей

Пример:
```
📦 Прошивка: firmware.bin  (1 321 872 байт)
Версия: 1.2.0
Что изменилось: Добавлен BLE OTA, исправлен активатор

🚀 Загрузка presence-c3-v1 v1.2.0 на сервер...
✅ Готово! v1.2.0  (1 321 872 байт)
🔗 https://ota.mpcbchat.ru/firmware/presence-c3-v1/manifest.json
```

### 5. Проверить манифест

```
https://ota.mpcbchat.ru/firmware/presence-c3-v1/manifest.json
```

---

## Структура OTA сервера

```
https://ota.mpcbchat.ru/
├── firmware/
│   └── presence-c3-v1/
│       ├── manifest.json   ← текущая версия
│       ├── 1.1.0.bin
│       └── 1.2.0.bin
└── app/
    ├── manifest.json
    └── presence-sensor-1.0.0.apk
```

Старые `.bin` файлы остаются на сервере — можно откатиться вручную если нужно.

---

## Модели устройств

| Модель | Процессор | Папка на сервере |
|--------|-----------|-----------------|
| presence-c3-v1 | ESP32-C3 Super Mini | `firmware/presence-c3-v1/` |
| presence-s3-v1 | ESP32-S3 (будущее) | `firmware/presence-s3-v1/` |

Для новой модели: изменить `MODEL` в `deploy.py` и создать папку на сервере.

---

## Сервис деплоя на VPS

Flask приложение: `/opt/ota-deploy/app.py`

```bash
# Статус
sudo systemctl status ota-deploy

# Перезапуск (после изменений в app.py)
sudo systemctl restart ota-deploy

# Логи
sudo journalctl -u ota-deploy -f
```
