/**************************************************************
 *  Author      : Tshibangu Samuel
 *  Role        : Freelance Embedded Systems Engineer
 *  Expertise   : Secure IoT Systems, Embedded C++, RTOS, Control Logic
 *  Contact     : tshibsamuel47@gmail.com
 *  Portfolio   : https://www.freelancer.com/u/tshibsamuel477
 *  Phone       : +216 54 429 793
 **************************************************************/
#ifndef FINGERPRINT_H
#define FINGERPRINT_H

// Enable this build for a local fingerprint test harness (serial-only, no transport/ESP-NOW).
#ifndef FINGERPRINT_TEST_MODE
#define FINGERPRINT_TEST_MODE 0
#endif

#include <Arduino.h>
#include <HardwareSerial.h>
#include <Adafruit_Fingerprint.h>
#include <Config.hpp>
#include <Transport.hpp>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

/**
 * SECURITY / RADIO POLICY
 * -----------------------
 *
 * We only trust a sensor if:
 *   - it physically replied on UART  (sensorPresent_ == true)
 *   - AND it accepts the derived per-device password (tamperDetected_ == false)
 *
 * We ONLY run the background verify task if it's trusted.
 *
 * We NEVER unlock locally. A valid match just notifies master.
 *
 * We DO NOT spam ESP-NOW:
 *   - No "FPFAIL" spam on every scan.
 *   - No nonstop "ERR_TOKEN".
 *   - If the sensor is tampered (swapped/wrong pwd), we send "ERR_TOKEN"
 *     at most once every 20 seconds from the verify loop.
 *
 * Master-triggered commands (enroll, adopt, etc.) are allowed to reply
 * immediately via SendAck(), because that's not background spam.
 */

class EspNowManager;
class MotorDriver;

enum FP_EnrollState : uint8_t {
    FP_ENROLL_IDLE        = 0,
    FP_ENROLL_IN_PROGRESS = 1,
    FP_ENROLL_OK          = 2,
    FP_ENROLL_FAIL        = 3
};

class Fingerprint {
public:
    Fingerprint(MotorDriver* motor,
                EspNowManager* Now,
                int rxPin = R503_RX_PIN,
                int txPin = R503_TX_PIN,
                uint32_t baud = R503_BAUD_RATE);

    void begin();
    void attachEspNow(EspNowManager* now);
    void attachTransportPort(transport::TransportPort* port) { transport_ = port; }

    // --- status / info ---
    void setEnabled(bool enabled);
    void setSupported(bool supported);
    bool isEnabled() const       { return enabled_ && supported_; }
    bool isSupported() const     { return supported_; }
    bool isTampered() const      { return tamperDetected_; }
    bool isSensorPresent() const { return sensorPresent_; }

    // --- background verification task ---
    //   startVerifyMode() will REFUSE to start unless the sensor is trusted.
    void   startVerifyMode();
    void   stopVerifyMode();
    bool   isVerifyRunning();
    uint8_t verifyFingerprint();
    void   shutdown();  // stop tasks during safe reset

    // --- enrollment (master-driven) ---
    transport::StatusCode requestEnrollment(uint16_t slotId);
    void    enrollFingerprintTask();     // wrapper for default slot
    uint8_t getEnrollmentState();
    void    resetEnrollmentState();

    // --- DB mgmt / queries ---
    transport::StatusCode deleteFingerprint(uint16_t id);
    transport::StatusCode deleteFingerprint();         // default ID=1
    transport::StatusCode deleteAllFingerprints();
    bool    getDbInfo(uint16_t& count, uint16_t& cap);
    int16_t findNextFreeID();
    bool    getNextFreeId(uint16_t& id);

    // --- configured flag in preferences ---
    bool isDeviceConfigured();
    void setDeviceConfigured(bool val);

    // --- security actions (master commands) ---
    // Claim a virgin sensor and lock it with the derived per-device password.
    // Will ONLY restart verify task if adoption actually succeeded.
    transport::StatusCode adoptNewSensor();               // CMD_FP_ADOPT_SENSOR

    // Reset current sensor back to default password (0x00000000)
    // so it can be reused on another slave.
    // After release we will *not* restart verify unless sensor is still trusted.
    transport::StatusCode releaseSensorToDefault();       // CMD_FP_RELEASE_SENSOR

private:
    // RTOS tasks
    static void FingerMonitorTask(void* parameter);
    static void enrollFingerprint(void* parameter);

    // worker for enrollment
    uint8_t doEnrollment_(uint16_t slotId);

    // Bring up / re-bring up the sensor.
    //
    // allowAdopt=false:
    //   - probe only, NEVER change password
    // allowAdopt=true:
    //   - allowed to take a default-password sensor,
    //     set the derived per-device password, and trust it
    //
    // Updates:
    //   sensorPresent_
    //   tamperDetected_
    //
    // RETURNS:
    //   true  = sensor present AND trusted (safe to start verify loop)
    //   false = missing OR untrusted
    bool initSensor_(bool allowAdopt);

    // Stop verify + enrollment tasks cleanly before sensitive ops
    void stopAllFpTasks_();

    // Helper: ready for background verify?
    inline bool isReadyForVerify_() const {
        return (isEnabled() && sensorPresent_ && !tamperDetected_);
    }

    // Dependencies
    MotorDriver*          motor;
    EspNowManager*        Now;

    // HW interface
    HardwareSerial*       uart;
    Adafruit_Fingerprint* finger;

    int                   rxPin_;
    int                   txPin_;
    uint32_t              baud_;

    const uint16_t        fingerprintID = 1;

    // task state
    TaskHandle_t          enrollmentTaskHandle;
    TaskHandle_t          fingerMonitorHandle;

    uint16_t              targetEnrollID_;
    volatile uint8_t      enrollmentState;
    volatile bool         verifyLoopStopFlag;
    bool                  resumeVerifyAfterEnroll_;

    // security state
    uint32_t              secretPassword_;
    bool                  tamperDetected_;
    bool                  sensorPresent_;
    bool                  enabled_;
    bool                  supported_;

    // rate limiting for tamper reports
    uint32_t              lastTamperReportMs_;

    // mutex
    SemaphoreHandle_t     mtx_;
    inline void lock_()   { if (mtx_) xSemaphoreTakeRecursive(mtx_, portMAX_DELAY); }
    inline void unlock_() { if (mtx_) xSemaphoreGiveRecursive(mtx_); }

    // Transport port for events (optional)
    transport::TransportPort* transport_ = nullptr;
    void sendFpEvent_(uint8_t op, const std::vector<uint8_t>& payload);
    void sendFpStatusEvent_(uint8_t op, transport::StatusCode status,
                            const std::vector<uint8_t>& extra = {});
    void sendEnrollStage_(uint8_t stage, uint8_t status, uint16_t slot);
};

#endif // FINGERPRINT_H






