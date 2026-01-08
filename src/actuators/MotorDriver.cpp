#include <MotorDriver.hpp>
#include <Config.hpp>
#include <ConfigNvs.hpp>
#include <Logger.hpp>
#include <NVSManager.hpp>
#include <Utils.hpp>

// Constructor
MotorDriver::MotorDriver()
: mtx_(xSemaphoreCreateRecursiveMutex()),
  motorRunning(false),
  Dir(false)
{
    DBG_PRINTLN("###########################################################");
    DBG_PRINTLN("#               Starting Motor Manager                   #");
    DBG_PRINTLN("###########################################################");
    Dir = CONF->GetBool(DIR_STATE, DIR_STATE_DEFAULT); // Default direction
}

// ==================================================
// Internal helpers for config
// ==================================================
uint32_t MotorDriver::getTimeoutMs_() {
    // Max runtime / pulse window in ms for motor / electromagnet drive
    return CONF->GetULong64(LOCK_TIMEOUT_KEY, LOCK_TIMEOUT_DEFAULT);
}

bool MotorDriver::isElectroMag_() {
    // true  -> electromagnet lock mode (no end-of-road switches)
    // false -> screw drive lock mode (uses end-of-road switches)
    return CONF->GetBool(LOCK_EMAG_KEY, LOCK_EMAG_DEFAULT);
}

// ==================================================
// Initialize motor pins and set initial motor state
// ==================================================
void MotorDriver::begin() {
    // HW setup (serialize just in case others try to touch pins during boot)
    lock_();
    pinMode(MOTOR_IN01_PIN, OUTPUT);
    pinMode(MOTOR_IN02_PIN, OUTPUT);

    // Even if electromagnet mode won't *use* them, keeping pullups is harmless
    pinMode(END01_OF_ROAD_PIN, INPUT_PULLUP);  // Unlock position sensor
    pinMode(END02_OF_ROAD_PIN, INPUT_PULLUP);  // Lock position sensor
    unlock_();

    // Honor stored LOCK_STATE at boot using your async tasks
    if (CONF->GetBool(LOCK_STATE, LOCK_STATE_DEFAULT)) {
        if (startLockTask())   DBG_PRINTLN("[MOTOR] Initial state: LOCKED. Lock task started. 🔒");
        else                   DBG_PRINTLN("[MOTOR] Failed to start initial lock task. ❌");
    } else {
        if (startUnlockTask()) DBG_PRINTLN("[MOTOR] Initial state: UNLOCKED. Unlock task started. 🔓");
        else                   DBG_PRINTLN("[MOTOR] Failed to start initial unlock task. ❌");
    }

    DBG_PRINTLN("[MOTOR] Motor initialization completed. ✅");
}

// Serialized direction change
void MotorDriver::setDirection(bool direction) {
    lock_();
    Dir = direction;
    CONF->PutBool(DIR_STATE, direction);
    DBG_PRINT("[MOTOR] Motor direction set to: ");
    DBG_PRINTLN(Dir ? "[MOTOR] Clockwise ⏩" : "[MOTOR] Counter-clockwise ⏪");
    unlock_();
}

// ==================================================
// High-level public API: chooses lock type from NVS
// ==================================================
bool MotorDriver::lockDoor() {
    bool emag = isElectroMag_();
    DBG_PRINTF("[MOTOR] lockDoor(): mode=%s\n", emag ? "EMAG" : "SCREW");
    if (emag) {
        return lockElectroMag();
    }
    return lockScrew();
}

bool MotorDriver::unlockDoor() {
    bool emag = isElectroMag_();
    DBG_PRINTF("[MOTOR] unlockDoor(): mode=%s\n", emag ? "EMAG" : "SCREW");
    if (emag) {
        return unlockElectroMag();
    }
    return unlockScrew();
}

// ---------------- Synchronous SCREW operations (serialized) ----------------
bool MotorDriver::lockScrew() {
    lock_();
    // If another motion or async task is already active, reject
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    bool otherLockTask   = (lockTaskHandle   && lockTaskHandle   != self);
    bool otherUnlockTask = (unlockTaskHandle && unlockTaskHandle != self);
    if (motorRunning || otherLockTask || otherUnlockTask) {
        DBG_PRINTLN("[MOTOR] lockScrew(): busy, rejecting request");
        unlock_();
        return false;
    }

    motionStart();
    LOGG->logLockAction("Motor Locking Motion (Screw).");
    DBG_PRINTLN("[MOTOR] Starting motor to lock screw. 🔒");

    // drive motor in "lock" direction
    if (Dir) { digitalWrite(MOTOR_IN01_PIN, HIGH); digitalWrite(MOTOR_IN02_PIN, LOW); }
    else     { digitalWrite(MOTOR_IN01_PIN, LOW);  digitalWrite(MOTOR_IN02_PIN, HIGH); }

    const uint32_t timeoutMs = getTimeoutMs_();
    unsigned long startTime = millis();
    bool reachedEor = false;
    while ((millis() - startTime) < timeoutMs) {
        // Optional early-stop on end-of-road if present (active-low)
        if (digitalRead(END02_OF_ROAD_PIN) == LOW) {
            reachedEor = true;
            break;
        }
        sleepMs_(10);
    }
    if (!reachedEor) {
        LOGG->logLockAction("EOR switch (lock) not detected before timeout; stopping on timeout.");
    }

    CONF->PutBool(LOCK_STATE, true);
    stop();
    motionStop();
    DBG_PRINTLN("[MOTOR] Screw locked successfully. ✅");
    unlock_();
    return true;
}

bool MotorDriver::unlockScrew() {
    lock_();
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    bool otherLockTask   = (lockTaskHandle   && lockTaskHandle   != self);
    bool otherUnlockTask = (unlockTaskHandle && unlockTaskHandle != self);
    if (motorRunning || otherLockTask || otherUnlockTask) {
        DBG_PRINTLN("[MOTOR] unlockScrew(): busy, rejecting request");
        unlock_();
        return false;
    }

    motionStart();
    LOGG->logLockAction("Motor Unlocking Motion (Screw).");
    DBG_PRINTLN("[MOTOR] Starting motor to unlock screw. 🔓");

    // drive motor in "unlock" direction
    if (Dir) { digitalWrite(MOTOR_IN01_PIN, LOW);  digitalWrite(MOTOR_IN02_PIN, HIGH); }
    else     { digitalWrite(MOTOR_IN01_PIN, HIGH); digitalWrite(MOTOR_IN02_PIN, LOW);  }

    const uint32_t timeoutMs = getTimeoutMs_();
    unsigned long startTime = millis();
    bool reachedEor = false;
    while ((millis() - startTime) < timeoutMs) {
        // Optional early-stop on end-of-road if present (active-low)
        if (digitalRead(END01_OF_ROAD_PIN) == LOW) {
            reachedEor = true;
            break;
        }
        sleepMs_(10);
    }
    if (!reachedEor) {
        LOGG->logLockAction("EOR switch (unlock) not detected before timeout; stopping on timeout.");
    }

    CONF->PutBool(LOCK_STATE, false);
    stop();
    motionStop();
    DBG_PRINTLN("[MOTOR] Screw unlocked successfully. ✅");
    unlock_();
    return true;
}

// ---------------- Electromagnet / no end-stop mode ----------------
// No end-of-road switches. We just energize for timeoutMs and trust.
// timeoutMs still comes from NVS (LOCK_TIMEOUT_KEY); caller can tune.
bool MotorDriver::lockElectroMag() {
    lock_();
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    bool otherLockTask   = (lockTaskHandle   && lockTaskHandle   != self);
    bool otherUnlockTask = (unlockTaskHandle && unlockTaskHandle != self);
    if (motorRunning || otherLockTask || otherUnlockTask) {
        DBG_PRINTLN("[MOTOR] lockElectroMag(): busy, rejecting request");
        unlock_();
        return false;
    }

    motionStart();
    LOGG->logLockAction("ElectroMag Locking Pulse.");
    DBG_PRINTLN("[MOTOR] EMAG LOCK: driving output. 🔒⚡");

    // "lock" polarity same as lockScrew()
    if (Dir) { digitalWrite(MOTOR_IN01_PIN, HIGH); digitalWrite(MOTOR_IN02_PIN, LOW); }
    else     { digitalWrite(MOTOR_IN01_PIN, LOW);  digitalWrite(MOTOR_IN02_PIN, HIGH); }

    const uint32_t pulseMs = getTimeoutMs_();
    unsigned long startTime = millis();
    while (millis() - startTime < pulseMs) {
        sleepMs_(10);
    }

    CONF->PutBool(LOCK_STATE, true);
    stop();
    motionStop();
    DBG_PRINTLN("[MOTOR] EMAG lock pulse complete. ✅");
    unlock_();
    return true;
}

bool MotorDriver::unlockElectroMag() {
    lock_();
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    bool otherLockTask   = (lockTaskHandle   && lockTaskHandle   != self);
    bool otherUnlockTask = (unlockTaskHandle && unlockTaskHandle != self);
    if (motorRunning || otherLockTask || otherUnlockTask) {
        DBG_PRINTLN("[MOTOR] unlockElectroMag(): busy, rejecting request");
        unlock_();
        return false;
    }

    motionStart();
    LOGG->logLockAction("ElectroMag Unlocking Pulse.");
    DBG_PRINTLN("[MOTOR] EMAG UNLOCK: driving output. 🔓⚡");

    // "unlock" polarity same as unlockScrew()
    if (Dir) { digitalWrite(MOTOR_IN01_PIN, LOW);  digitalWrite(MOTOR_IN02_PIN, HIGH); }
    else     { digitalWrite(MOTOR_IN01_PIN, HIGH); digitalWrite(MOTOR_IN02_PIN, LOW);  }

    const uint32_t pulseMs = getTimeoutMs_();
    unsigned long startTime = millis();
    while (millis() - startTime < pulseMs) {
        sleepMs_(10);
    }

    CONF->PutBool(LOCK_STATE, false);
    stop();
    motionStop();
    DBG_PRINTLN("[MOTOR] EMAG unlock pulse complete. ✅");
    unlock_();
    return true;
}

// Stop() stays as your GPIO brake; now serialized
void MotorDriver::stop() {
    lock_();
    LOGG->logLockAction("Motor Stop Motion.");
    digitalWrite(MOTOR_IN01_PIN, LOW);
    digitalWrite(MOTOR_IN02_PIN, LOW);
    DBG_PRINTLN("[MOTOR] Motor stopped. 🛑");
    unlock_();
}


// Stop any active motor tasks and release outputs for an orderly reset.
void MotorDriver::shutdown() {
    lock_();
    if (lockTaskHandle) {
        vTaskDelete(lockTaskHandle);
        lockTaskHandle = nullptr;
    }
    if (unlockTaskHandle) {
        vTaskDelete(unlockTaskHandle);
        unlockTaskHandle = nullptr;
    }
    motorRunning = false;
    motionStopMs = millis();
    digitalWrite(MOTOR_IN01_PIN, LOW);
    digitalWrite(MOTOR_IN02_PIN, LOW);
    DBG_PRINTLN("[MOTOR] Shutdown: tasks canceled and outputs off.");
    unlock_();
}

// ---------------- Async tasks (preserved, now route to lockDoor/unlockDoor) ----------------
bool MotorDriver::startLockTask() {
    if (lockTaskHandle == nullptr) {
        DBG_PRINTLN("[MOTOR] startLockTask(): creating lock task");
        BaseType_t result = xTaskCreate(
            lockTask,
            "LockTask",
            LOCK_TASK_STACK_SIZE,
            this,
            LOCK_ACK_TASK_PRIORITY,
            &lockTaskHandle
        );
        if (result != pdPASS) {
            DBG_PRINTF("[MOTOR] startLockTask(): xTaskCreate failed (code=%d)\n", (int)result);
            lockTaskHandle = nullptr;
            return false;
        }
        DBG_PRINTLN("[MOTOR] startLockTask(): lock task created");
        return true;
    }
    DBG_PRINTLN("[MOTOR] startLockTask(): lock task already running");
    return false;
}

bool MotorDriver::startUnlockTask() {
    if (unlockTaskHandle == nullptr) {
        DBG_PRINTLN("[MOTOR] startUnlockTask(): creating unlock task");
        BaseType_t result = xTaskCreate(
            unlockTask,
            "UnlockTask",
            LOCK_TASK_STACK_SIZE,
            this,
            LOCK_ACK_TASK_PRIORITY,
            &unlockTaskHandle
        );
        if (result != pdPASS) {
            DBG_PRINTF("[MOTOR] startUnlockTask(): xTaskCreate failed (code=%d)\n", (int)result);
            unlockTaskHandle = nullptr;
            return false;
        }
        DBG_PRINTLN("[MOTOR] startUnlockTask(): unlock task created");
        return true;
    }
    DBG_PRINTLN("[MOTOR] startUnlockTask(): unlock task already running");
    return false;
}

void MotorDriver::lockTask(void* param) {
    MotorDriver* motor = static_cast<MotorDriver*>(param);
    DBG_PRINTLN("[MOTOR] lockTask(): started");
    bool ok = motor->lockDoor();     // will choose screw vs electromagnet

    motor->lock_();
    motor->lockResult = ok;
    motor->lockTaskHandle = nullptr;
    motor->unlock_();

    DBG_PRINTF("[MOTOR] lockTask(): lockDoor() returned %s, deleting task\n", ok ? "OK" : "FAIL");
    vTaskDelete(nullptr);
}

void MotorDriver::unlockTask(void* param) {
    MotorDriver* motor = static_cast<MotorDriver*>(param);
    DBG_PRINTLN("[MOTOR] unlockTask(): started");
    bool ok = motor->unlockDoor();   // will choose screw vs electromagnet

    motor->lock_();
    motor->lockResult = ok;
    motor->unlockTaskHandle = nullptr;
    motor->unlock_();

    DBG_PRINTF("[MOTOR] unlockTask(): unlockDoor() returned %s, deleting task\n", ok ? "OK" : "FAIL");
    vTaskDelete(nullptr);
}

// ---------------- Status helpers ----------------
bool MotorDriver::isMoving() const {
    return motorRunning;
}

bool MotorDriver::isBusy() const {
    return motorRunning || lockTaskHandle != nullptr || unlockTaskHandle != nullptr;
}

bool MotorDriver::isMovingOrSettling(uint32_t settleMs) const {
    if (motorRunning) return true;
    return (millis() - motionStopMs) < settleMs;
}
