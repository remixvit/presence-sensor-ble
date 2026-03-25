#!/usr/bin/env python3
"""Деплой прошивки на OTA сервер"""

import os
import sys
import requests
from pathlib import Path

# Настройки
VPS_URL  = "https://ota.mpcbchat.ru/deploy/firmware"
TOKEN    = "83a9843e7489ddacab662d2fafa46e73"  # тот же что в app.py на VPS 83a9843e7489ddacab662d2fafa46e73
MODEL    = "presence-c3-v1"
BIN_PATH = Path(__file__).parent / ".pio/build/sensor/firmware.bin"


def main():
    if not BIN_PATH.exists():
        print("❌ firmware.bin не найден. Сначала скомпилируй: pio run")
        sys.exit(1)

    size = BIN_PATH.stat().st_size
    print(f"📦 Прошивка: {BIN_PATH.name}  ({size:,} байт)")

    version   = input("Версия (например 1.2.0): ").strip()
    changelog = input("Что изменилось: ").strip()

    if not version:
        print("❌ Версия не указана")
        sys.exit(1)

    print(f"\n🚀 Загрузка {MODEL} v{version} на сервер...")

    with open(BIN_PATH, "rb") as f:
        resp = requests.post(
            VPS_URL,
            headers={"X-Token": TOKEN},
            data={"model": MODEL, "version": version, "changelog": changelog},
            files={"firmware": (f"{version}.bin", f, "application/octet-stream")},
            timeout=60,
        )

    if resp.ok:
        data = resp.json()
        print(f"✅ Готово! v{data['version']}  ({data['size']:,} байт)")
        print(f"🔗 {VPS_URL.replace('/deploy/firmware', '')}/firmware/{MODEL}/manifest.json")
    else:
        print(f"❌ Ошибка {resp.status_code}: {resp.text}")


if __name__ == "__main__":
    main()
