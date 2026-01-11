/**************************************************************
 *  Author      : Tshibangu Samuel
 *  Role        : Freelance Embedded Systems Engineer
 *  Expertise   : Secure IoT Systems, Embedded C++, RTOS, Control Logic
 *  Contact     : tshibsamuel47@gmail.com
 *  Portfolio   : https://www.freelancer.com/u/tshibsamuel477
 *  Phone       : +216 54 429 793
 **************************************************************/
#ifndef MOTOR_DRIVER_H
#define MOTOR_DRIVER_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <Utils.hpp>

#define MOTOR_SETTLE_MS 800   // ignore shock while motor runs + this many ms after stop

// ----------------------------------------------------------------------
// New NVS (ConfigManager) keys
// NOTE: keep key strings <=15 chars for ESP32 NVS
//  - LOCK_TIMEOUT_KEY : uint32 (ms) max runtime / pulse width
//  - LOCK_EMAG_KEY    : bool   true = electromagnet lock, false = screw motor
// ----------------------------------------------------------------------

class MotorDriver {
public:
    MotorDriver();
    void begin();

    // High-level API (auto-selects lock style based on LOCK_EMAG_KEY in NVS)
    bool lockDoor();    // synchronous lock (routes to screw or electromagnet mode)
    bool unlockDoor();  // synchronous unlock (routes to screw or electromagnet mode)

    // Explicit screw-drive versions (existing behavior, serialized)
    bool lockScrew();                    // synchronous lock using end-of-road switches
    bool unlockScrew();                  // synchronous unlock using end-of-road switches

    // Electromagnet / "no endstop" versions (pulse drive only)
    bool lockElectroMag();               // synchronous lock pulse, no EOR check
    bool unlockElectroMag();             // synchronous unlock pulse, no EOR check

    void stop();                         // serialized brake/stop
    void setDirection(bool direction);   // serialized direction update
    void shutdown();                     // stop pins + kill any running tasks

    // Existing async API preserved
    bool startLockTask();
    bool startUnlockTask();
    static void lockTask(void* param);
    static void unlockTask(void* param);

    // Status (safe snapshots)
    bool isMoving() const;                                // true while motor turns
    bool isBusy() const;                                  // moving OR async task active
    bool isMovingOrSettling(uint32_t settleMs = 600) const;

    // Task handles + result kept for backward compat
    TaskHandle_t getLockTaskHandle()   { return lockTaskHandle; }
    TaskHandle_t getUnlockTaskHandle() { return unlockTaskHandle; }
    bool         getLockResult() const { return lockResult; }

private:
    // ---- concurrency primitives ----
    SemaphoreHandle_t mtx_;   // recursive mutex for class-wide serialization
    inline void lock_()   { if (mtx_) xSemaphoreTakeRecursive(mtx_, portMAX_DELAY); }
    inline void unlock_() { if (mtx_) xSemaphoreGiveRecursive(mtx_); }
    static inline void sleepMs_(uint32_t ms) {
        if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) vTaskDelay(pdMS_TO_TICKS(ms));
        else delay(ms);
    }

    // Motion lifetime flags
    volatile bool     motorRunning;     // true while motor is turning
    volatile uint32_t motionStartMs{0};
    volatile uint32_t motionStopMs{0};

    // Dependencies + state
    bool            Dir;
    class Logger*   Log;

    // Internal helpers
    uint32_t getTimeoutMs_();   // fetch LOCK_TIMEOUT_KEY from NVS / ConfigManager
    bool     isElectroMag_();   // fetch LOCK_EMAG_KEY from NVS / ConfigManager

    // Motion bookkeeping (safe while holding mtx_)
    inline void motionStart() { motorRunning = true ; DBG_PRINTLN("[Motor] Motion Started! ▶️");  motionStartMs = millis(); }
    inline void motionStop()  { motorRunning = false; DBG_PRINTLN("[Motor] Motion Stopped! ⏹️"); motionStopMs  = millis(); }

    // Keep your public task handles & result (used elsewhere)
public:
    TaskHandle_t lockTaskHandle   = nullptr;
    TaskHandle_t unlockTaskHandle = nullptr;
    bool         lockResult       = true;
};

#endif // MOTOR_DRIVER_H
