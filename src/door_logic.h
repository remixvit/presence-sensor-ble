#pragma once
#include <Arduino.h>

// ============================================================
//  Направление движения объекта
// ============================================================
enum class MoveDir : uint8_t {
    Unknown    = 0,
    Approaching,   // объект приближается к сенсору
    Leaving,       // объект удаляется от сенсора
    Stationary,    // объект есть, но не движется
};

// ============================================================
//  Активатор двери
//
//  Независим от VL53L1X (Безопасность — отдельный сигнал).
//
//  Логика:
//    HIGH — объект приближается И в пределах openDist
//    LOW  — объект стоит или удаляется, после closeDelay мс
// ============================================================
class DoorLogic {
public:
    void begin(uint8_t activatorPin) {
        _pin = activatorPin;
    }

    // Вызывать раз в секунду.
    // movingDist    — расстояние до движущегося объекта (см), 0 если нет
    // presence      — есть ли объект (LD2410)
    // approachDelta — порог изменения дистанции для "приближения" (см/сек)
    // openDist      — макс. дистанция при которой активировать (см)
    // closeDelay    — задержка деактивации (мс) после прекращения приближения
    void update(uint16_t movingDist, bool presence,
                uint16_t approachDelta, uint16_t openDist, uint32_t closeDelay)
    {
        // ── Определяем направление ──────────────────────────
        if (!presence || movingDist == 0) {
            _dir = MoveDir::Unknown;
        } else if (_prevDist == 0) {
            _dir = MoveDir::Unknown;
        } else {
            int16_t delta = (int16_t)_prevDist - (int16_t)movingDist;
            if (delta >= (int16_t)approachDelta) {
                _dir = MoveDir::Approaching;
            } else if (delta <= -(int16_t)approachDelta) {
                _dir = MoveDir::Leaving;
            } else {
                _dir = presence ? MoveDir::Stationary : MoveDir::Unknown;
            }
        }
        _prevDist = movingDist;

        // ── Активатор: HIGH при приближении ─────────────────
        bool shouldActivate = (_dir == MoveDir::Approaching)
                              && (movingDist > 0)
                              && (movingDist <= openDist);

        if (shouldActivate) {
            if (!_activated) _activate();
            _closeTimerActive = false;  // пока приближается — таймер не идёт
        } else if (_activated) {
            // Объект стоит или удаляется — запускаем таймер
            if (!_closeTimerActive) {
                _closeTimer = millis();
                _closeTimerActive = true;
            } else if (millis() - _closeTimer >= closeDelay) {
                _deactivate();
            }
        }
    }

    MoveDir direction()    const { return _dir; }
    bool    isActivated()  const { return _activated; }

    const char* directionStr() const {
        switch (_dir) {
            case MoveDir::Approaching: return "приближается";
            case MoveDir::Leaving:     return "удаляется";
            case MoveDir::Stationary:  return "стоит";
            default:                   return "нет объекта";
        }
    }

private:
    uint8_t  _pin = 0;
    uint16_t _prevDist = 0;
    MoveDir  _dir = MoveDir::Unknown;
    bool     _activated = false;
    bool     _closeTimerActive = false;
    unsigned long _closeTimer = 0;

    void _activate() {
        _activated = true;
        digitalWrite(_pin, HIGH);
        Serial.println("[ACT] ВКЛ — объект приближается");
    }

    void _deactivate() {
        _activated = false;
        _closeTimerActive = false;
        digitalWrite(_pin, LOW);
        Serial.println("[ACT] ВЫКЛ");
    }
};
