#include <FingerprintScanner.hpp>
#include <ConfigNvs.hpp>
#include <NVSManager.hpp>
#include <RGBLed.hpp>
#include <Utils.hpp>

namespace {
void logFpPayload_(const char* tag,
                   uint8_t op,
                   const std::vector<uint8_t>& payload) {
    DBG_PRINTF("[FP] %s op=0x%02X len=%u", tag, op, (unsigned)payload.size());
    if (!payload.empty()) {
        DBG_PRINT(" data=");
        for (size_t i = 0; i < payload.size(); ++i) {
            DBG_PRINTF("%02X", payload[i]);
            if (i + 1 < payload.size()) {
                DBG_PRINT(" ");
            }
        }
    }
    DBG_PRINTLN();
}

#if FINGERPRINT_TEST_MODE
const char* enrollStageDescription_(uint8_t stage) {
    switch (stage) {
        case 1: return "Place finger on the sensor";
        case 2: return "First capture OK";
        case 3: return "Remove finger now";
        case 4: return "Second capture OK";
        case 5: return "Saving template";
        case 6: return "Enrollment complete";
        case 7: return "Enrollment failed";
        case 8: return "Enrollment timed out";
        default: return "Unknown enrollment stage";
    }
}

void enrollPrint_(const char* msg) {
    DBG_PRINT("[FP] ");
    DBG_PRINTLN(msg);
}

const char* enrollGetImageError_(uint8_t code) {
    switch (code) {
        case FINGERPRINT_PACKETRECIEVEERR: return "Communication error";
        case FINGERPRINT_IMAGEFAIL: return "Imaging error";
        default: return "Unknown image error";
    }
}

const char* enrollImage2TzError_(uint8_t code) {
    switch (code) {
        case FINGERPRINT_PACKETRECIEVEERR: return "Communication error";
        case FINGERPRINT_IMAGEMESS: return "Image too messy";
        case FINGERPRINT_FEATUREFAIL: return "Could not find fingerprint features";
        case FINGERPRINT_INVALIDIMAGE: return "Could not find fingerprint features";
        default: return "Unknown image conversion error";
    }
}

const char* enrollModelError_(uint8_t code) {
    switch (code) {
        case FINGERPRINT_PACKETRECIEVEERR: return "Communication error";
        case FINGERPRINT_ENROLLMISMATCH: return "Fingerprints did not match";
        default: return "Unknown model error";
    }
}

const char* enrollStoreError_(uint8_t code) {
    switch (code) {
        case FINGERPRINT_PACKETRECIEVEERR: return "Communication error";
        case FINGERPRINT_BADLOCATION: return "Could not store in that location";
        case FINGERPRINT_FLASHERR: return "Error writing to flash";
        default: return "Unknown store error";
    }
}
#endif
}

// -----------------------------------------------------------
// Constructor
// -----------------------------------------------------------
Fingerprint::Fingerprint(MotorDriver* motor,

                         EspNowManager* Now,
                         int rxPin,
                         int txPin,
                         uint32_t baud)
: motor(motor),
  Now(Now),
  uart(nullptr),
  finger(nullptr),
  rxPin_(rxPin),
  txPin_(txPin),
  baud_(baud),
  enrollmentTaskHandle(nullptr),
  fingerMonitorHandle(nullptr),
  targetEnrollID_(0),
  enrollmentState(FP_ENROLL_IDLE),
  verifyLoopStopFlag(false),
  resumeVerifyAfterEnroll_(false),
  tamperDetected_(false),
  sensorPresent_(false),
  lastTamperReportMs_(0)
{
    mtx_ = xSemaphoreCreateRecursiveMutex();
    // NOTE: we don't touch the UART / sensor here.
    // We'll probe in begin().
}

// -----------------------------------------------------------
// begin()
// -----------------------------------------------------------
//
// We probe the sensor WITHOUT adopting (we won't change passwords).
// We ONLY start background verify if the sensor is present AND trusted.
void Fingerprint::begin() {
    DBG_PRINTLN("[FP] begin()");
    bool ok = initSensor_(false);
    DBG_PRINTF("[FP] begin: sensor_ok=%d present=%d tamper=%d\n",
               ok ? 1 : 0,
               sensorPresent_ ? 1 : 0,
               tamperDetected_ ? 1 : 0);
    if (ok) {
        startVerifyMode();
    }
}

// -----------------------------------------------------------
// attachEspNow()
// -----------------------------------------------------------
void Fingerprint::attachEspNow(EspNowManager* now) {
    lock_();
    Now = now;
    unlock_();
    DBG_PRINTLN("[FP] attachEspNow");
}

void Fingerprint::sendFpEvent_(uint8_t op, const std::vector<uint8_t>& payload) {
#if !FINGERPRINT_TEST_MODE
    logFpPayload_("TX_EVT", op, payload);
#endif
    if (!transport_) {
        return;
    }
    transport::TransportMessage msg;
    msg.header.destId = 1; // master
    msg.header.module = static_cast<uint8_t>(transport::Module::Fingerprint);
    msg.header.type   = static_cast<uint8_t>(transport::MessageType::Event);
    msg.header.opCode = op;
    msg.header.flags  = 0;
    msg.payload = payload;
    msg.header.payloadLen = static_cast<uint8_t>(payload.size());
    transport_->send(msg, true);
}

void Fingerprint::sendFpStatusEvent_(uint8_t op, transport::StatusCode status,
                                     const std::vector<uint8_t>& extra) {
    std::vector<uint8_t> pl;
    if (op == 0x0B) {
        // For fail/busy/no-sensor/tamper events, payload is just the reason code.
        const uint8_t reason = extra.empty() ? 0 : extra[0];
        #if !FINGERPRINT_TEST_MODE
        DBG_PRINTF("[FP] TX_STATUS op=0x%02X status=%u reason=%u\n",
                   op,
                   static_cast<unsigned>(status),
                   static_cast<unsigned>(reason));
        #endif
        pl.push_back(reason);
        sendFpEvent_(op, pl);
        return;
    }

    pl.reserve(1 + extra.size());
    pl.push_back(static_cast<uint8_t>(status));
    pl.insert(pl.end(), extra.begin(), extra.end());
    #if !FINGERPRINT_TEST_MODE
    DBG_PRINTF("[FP] TX_STATUS op=0x%02X status=%u extra_len=%u\n",
               op,
               static_cast<unsigned>(status),
               (unsigned)extra.size());
    #endif
    sendFpEvent_(op, pl);
}

void Fingerprint::sendEnrollStage_(uint8_t stage, uint8_t status, uint16_t slot) {
#if !FINGERPRINT_TEST_MODE
    DBG_PRINTF("[FP] ENROLL stage=%u status=%u slot=%u\n",
               (unsigned)stage,
               (unsigned)status,
               (unsigned)slot);
#endif
#if FINGERPRINT_TEST_MODE
    const char* description = enrollStageDescription_(stage);
    if (description) {
        DBG_PRINT("[FP] enroll info: ");
        DBG_PRINTLN(description);
    }
    if (stage == 6 && status == 0) {
        DBG_PRINTLN("[FP] enrollment complete; run 'verify start' to resume verification");
    }
    if (stage == 7 || stage == 8) {
        DBG_PRINTLN("[FP] enrollment failed or timed out; try again or type 'help'");
    }
#endif
    std::vector<uint8_t> pl;
    pl.reserve(4);
    pl.push_back(stage);
    pl.push_back(uint8_t(slot & 0xFF));
    pl.push_back(uint8_t((slot >> 8) & 0xFF));
    pl.push_back(status);
    sendFpEvent_(0x0C, pl); // EnrollProgress
}

// -----------------------------------------------------------
// stopAllFpTasks_()
// -----------------------------------------------------------
//
// We call this before doing sensitive operations like adopt/release
// so nothing else is poking the UART / sensor.
void Fingerprint::stopAllFpTasks_() {
    // ask verify task to stop
    stopVerifyMode();

    lock_();
    // kill any enrollment task immediately
    if (enrollmentTaskHandle != nullptr) {
        vTaskDelete(enrollmentTaskHandle);
        enrollmentTaskHandle = nullptr;
    }
    enrollmentState = FP_ENROLL_IDLE;
    resumeVerifyAfterEnroll_ = false;
    unlock_();

    // give verify task time to exit + clear handle
    uint32_t waitStart = millis();
    while (true) {
        lock_();
        bool running = (fingerMonitorHandle != nullptr);
        unlock_();
        if (!running) break;
        if (millis() - waitStart > 2000UL) {
            DBG_PRINTLN("[FP] verify task still running after stop");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    vTaskDelay(pdMS_TO_TICKS(50));
}

// -----------------------------------------------------------
// initSensor_(allowAdopt)
// -----------------------------------------------------------
//
// RETURNS bool success = (sensorPresent_ && !tamperDetected_)
//
// It also:
//   - drives LED overlays
//   - sends ONE snapshot ACK to the master (DB info if trusted,
//     ERR_TOKEN if tampered + present). That snapshot is allowed,
//     it's not background spam.
//
bool Fingerprint::initSensor_(bool allowAdopt) {
    lock_();
    DBG_PRINTF("[FP] initSensor allowAdopt=%d\n", allowAdopt ? 1 : 0);

    if (!uart) {
        uart = new HardwareSerial(1);
    }
    uart->begin(baud_, SERIAL_8N1, rxPin_, txPin_);

    sensorPresent_  = false;
    tamperDetected_ = true;

    if (finger) {
        delete finger;
        finger = nullptr;
    }

#if FINGERPRINT_TEST_MODE
    // Test mode: basic default-password sensor only, no tamper/security logic.
    finger = new Adafruit_Fingerprint(uart, 0x00000000UL);
    finger->begin(baud_);
    if (finger->verifyPassword()) {
        sensorPresent_ = true;
        tamperDetected_ = false;
    } else {
        sensorPresent_ = false;
        tamperDetected_ = false;
    }
#else
    // 1) Try with our secret password (trusted path)
    finger = new Adafruit_Fingerprint(uart, FP_SECRET_PASSWORD);
    finger->begin(baud_);

    if (finger->verifyPassword()) {
        sensorPresent_  = true;
        tamperDetected_ = false;
    } else {
        // 2) Try factory default (0x00000000) so we can detect virgin sensors
        Adafruit_Fingerprint* tmp = new Adafruit_Fingerprint(uart, 0x00000000UL);
        tmp->begin(baud_);

        if (tmp->verifyPassword()) {
            sensorPresent_  = true;
            tamperDetected_ = true; // not trusted yet

            if (allowAdopt) {
                // allowed to claim the virgin sensor
                uint8_t pwResult = tmp->setPassword(FP_SECRET_PASSWORD);
                if (pwResult == FINGERPRINT_OK) {
                    delete finger;
                    finger = new Adafruit_Fingerprint(uart, FP_SECRET_PASSWORD);
                    finger->begin(baud_);
                    if (finger->verifyPassword()) {
                        tamperDetected_ = false;
                    } else {
                        tamperDetected_ = true;
                    }
                } else {
                    tamperDetected_ = true;
                }
            }
        } else {
            // no response with secret or default -> not present or attacker-locked
            sensorPresent_  = false;
            tamperDetected_ = true;
        }

        delete tmp;
    }
#endif

    DBG_PRINTF("[FP] initSensor result present=%d tamper=%d\n",
               sensorPresent_ ? 1 : 0,
               tamperDetected_ ? 1 : 0);

    // SINGLE snapshot via transport
    if (isReadyForVerify_()) {
        finger->getTemplateCount();
        if (finger->templateCount > 0) {
            setDeviceConfigured(true);
        }
        std::vector<uint8_t> pl;
        pl.push_back(uint8_t(transport::StatusCode::OK));
        pl.push_back(uint8_t(finger->templateCount & 0xFF));
        pl.push_back(uint8_t((finger->templateCount >> 8) & 0xFF));
        pl.push_back(uint8_t(finger->capacity & 0xFF));
        pl.push_back(uint8_t((finger->capacity >> 8) & 0xFF));
        DBG_PRINTF("[FP] DB snapshot count=%u cap=%u\n",
                   (unsigned)finger->templateCount,
                   (unsigned)finger->capacity);
        sendFpEvent_(0x06, pl); // reuse QueryDb opcode as event
    } else if (sensorPresent_) {
        // sensor answered, but it's not ours (tampered / wrong password)
        DBG_PRINTLN("[FP] sensor present but tampered");
        sendFpStatusEvent_(0x0B, transport::StatusCode::DENIED, {3}); // reason 3=tamper
        lastTamperReportMs_ = millis();
    }

    bool ok = isReadyForVerify_();
    unlock_();
    return ok;
}

// -----------------------------------------------------------
// adoptNewSensor()  [CMD_FP_ADOPT_SENSOR]
// -----------------------------------------------------------
//
// Master says: take the sensor that's plugged in, and make it ours.
// We:
//   - stop verify/enroll tasks
//   - initSensor_(true)  -> allowed to switch default password to secret
//   - ONLY if that succeeds we startVerifyMode()
//   - SendAck FP_ADOPT_OK / FP_ADOPT_FAIL once.
transport::StatusCode Fingerprint::adoptNewSensor() {
    DBG_PRINTLN("[FP] adoptNewSensor");
    stopAllFpTasks_();

    bool success = initSensor_(true); // try to claim the sensor

    if (success) {
        startVerifyMode();
    }

    return success ? transport::StatusCode::OK : transport::StatusCode::APPLY_FAIL;
}

// -----------------------------------------------------------
// releaseSensorToDefault()  [CMD_FP_RELEASE_SENSOR]
// -----------------------------------------------------------
//
// Master says: reset this sensor so I can reuse it elsewhere.
// Steps:
//   1. stopAllFpTasks_()
//   2. set password back to 0x00000000
//   3. re-probe with initSensor_(false) (no adopt)
//   4. ONLY restart verify loop if still trusted (usually no)
//   5. SendAck FP_RELEASE_OK / FP_RELEASE_FAIL once.
transport::StatusCode Fingerprint::releaseSensorToDefault() {
    DBG_PRINTLN("[FP] releaseSensorToDefault");
    stopAllFpTasks_();

    bool success = false;

    lock_();
    if (finger) {
        uint8_t pwResult = finger->setPassword(0x00000000UL);
        success = (pwResult == FINGERPRINT_OK);
    }
    unlock_();

    bool okAfter = initSensor_(false);   // re-check, do NOT adopt automatically

    if (okAfter) {
        startVerifyMode();
    }

    return success ? transport::StatusCode::OK : transport::StatusCode::APPLY_FAIL;
}

// -----------------------------------------------------------
// startVerifyMode() / stopVerifyMode()
// -----------------------------------------------------------
//
// We ONLY spawn the background task if:
//   - it's not already running
//   - sensorPresent_ == true
//   - tamperDetected_ == false
void Fingerprint::startVerifyMode() {
    lock_();

    // already running?
    if (fingerMonitorHandle != nullptr) {
        DBG_PRINTLN("[FP] verify already running");
        unlock_();
        return;
    }

    if (enrollmentState == FP_ENROLL_IN_PROGRESS || enrollmentTaskHandle != nullptr) {
        DBG_PRINTLN("[FP] verify not started (enroll active)");
        unlock_();
        return;
    }

    // sensor not ready/trusted? don't start
    if (!isReadyForVerify_()) {
        DBG_PRINTLN("[FP] verify not started (sensor not ready)");
        unlock_();
        return;
    }

    verifyLoopStopFlag = false;

    xTaskCreate(
        Fingerprint::FingerMonitorTask,
        "FPVerifyTask",
        4096,
        this,
        1,
        &fingerMonitorHandle
    );
    DBG_PRINTLN("[FP] verify started");
    unlock_();
}

void Fingerprint::stopVerifyMode() {
    lock_();
    verifyLoopStopFlag = true;
    unlock_();
    DBG_PRINTLN("[FP] verify stop requested");
    // FingerMonitorTask will self-delete and clear handle
}

bool Fingerprint::isVerifyRunning() {
    lock_();
    bool running = (fingerMonitorHandle != nullptr);
    unlock_();
    return running;
}

void Fingerprint::shutdown() {
    stopAllFpTasks_();
    lock_();
    verifyLoopStopFlag = true;
    unlock_();
}


// -----------------------------------------------------------
// verifyFingerprint()
// -----------------------------------------------------------
//
// Background loop does this ~5Hz.
// Policy:
//   - If tampered: send ERR_TOKEN (not more than once every 20 seconds).
//   - If finger matches: SendAck(FPMATCH..)
//   - If finger fails / no match: NO SendAck
//   - No more FPFAIL spam every 200ms.
//   - No nonstop ERR_TOKEN spam either (20s throttle).
uint8_t Fingerprint::verifyFingerprint() {
    // Sensor untrusted / tampered / foreign password
    if (tamperDetected_) {
        // Only treat this as "tamper alarm" if a sensor is actually connected.
        // We resend ERR_TOKEN at most once every 20 seconds.
    if (sensorPresent_) {
        uint32_t nowMs = millis();
        if ((nowMs - lastTamperReportMs_) >= 20000UL) {
            sendFpStatusEvent_(0x0B, transport::StatusCode::DENIED, {3}); // tamper
            lastTamperReportMs_ = nowMs;
        }
    }

        return FINGERPRINT_PACKETRECIEVEERR;
    }

    if (!finger) {
        // shouldn't happen if we gated startVerifyMode(),
        // but if it does, stay quiet.
        return FINGERPRINT_PACKETRECIEVEERR;
    }

#if FINGERPRINT_TEST_MODE
    static uint32_t lastNoFingerMs = 0;
    static uint32_t lastNotFoundMs = 0;

    uint8_t p = finger->getImage();
    switch (p) {
        case FINGERPRINT_OK:
            DBG_PRINTLN("[FP] Image taken");
            break;
        case FINGERPRINT_NOFINGER:
            if (millis() - lastNoFingerMs >= 1000UL) {
                DBG_PRINTLN("[FP] No finger detected");
                lastNoFingerMs = millis();
            }
            return p;
        case FINGERPRINT_PACKETRECIEVEERR:
            DBG_PRINTLN("[FP] Communication error");
            return p;
        case FINGERPRINT_IMAGEFAIL:
            DBG_PRINTLN("[FP] Imaging error");
            return p;
        default:
            DBG_PRINTLN("[FP] Unknown error");
            return p;
    }

    p = finger->image2Tz();
    switch (p) {
        case FINGERPRINT_OK:
            DBG_PRINTLN("[FP] Image converted");
            break;
        case FINGERPRINT_IMAGEMESS:
            DBG_PRINTLN("[FP] Image too messy");
            return p;
        case FINGERPRINT_PACKETRECIEVEERR:
            DBG_PRINTLN("[FP] Communication error");
            return p;
        case FINGERPRINT_FEATUREFAIL:
        case FINGERPRINT_INVALIDIMAGE:
            DBG_PRINTLN("[FP] Could not find fingerprint features");
            return p;
        default:
            DBG_PRINTLN("[FP] Unknown error");
            return p;
    }

    p = finger->fingerSearch();
    if (p == FINGERPRINT_OK) {
        DBG_PRINTLN("[FP] Found a print match!");
        DBG_PRINTF("[FP] Found ID #%u with confidence %u\n",
                   static_cast<unsigned>(finger->fingerID),
                   static_cast<unsigned>(finger->confidence));
        return p;
    }
    if (p == FINGERPRINT_NOTFOUND) {
        if (millis() - lastNotFoundMs >= 1000UL) {
            DBG_PRINTLN("[FP] Did not find a match");
            lastNotFoundMs = millis();
        }
        return p;
    }
    if (p == FINGERPRINT_PACKETRECIEVEERR) {
        DBG_PRINTLN("[FP] Communication error");
        return p;
    }
    DBG_PRINTLN("[FP] Unknown error");
    return p;
#else

    // Try to get an image
    uint8_t p = finger->getImage();
    if (p != FINGERPRINT_OK) {
        return p;
    }

    // Finger touched sensor -> short feedback flash
    if (RGB) {
    }

    // Convert image to template buffer
    p = finger->image2Tz();
    if (p != FINGERPRINT_OK) {
        if (RGB) {
        }
        // No SendAck() here. We don't spam master for bad reads.
        return p;
    }

    // Search database
    static uint32_t lastNoMatchMs = 0;
    p = finger->fingerSearch();
    if (p == FINGERPRINT_OK) {
        // MATCH = legit event -> report immediately once
        if (RGB) {
        }
        lastNoMatchMs = 0;
        DBG_PRINTF("[FP] match id=%u confidence=%u\n",
                   static_cast<unsigned>(finger->fingerID),
                   static_cast<unsigned>(finger->confidence));
        // Transport event MatchEvent (op 0x0A)
        std::vector<uint8_t> pl;
        pl.push_back(uint8_t(finger->fingerID & 0xFF));
        pl.push_back(uint8_t((finger->fingerID >> 8) & 0xFF));
        pl.push_back(uint8_t(finger->confidence));
        sendFpEvent_(0x0A, pl);
    } else if (p == FINGERPRINT_NOTFOUND) {
        // Throttle fail events to avoid spam.
        if (millis() - lastNoMatchMs >= 1500UL) {
            sendFpStatusEvent_(0x0B, transport::StatusCode::DENIED, {0}); // match_fail
            lastNoMatchMs = millis();
        }
    }

#endif
    return p;
}

// -----------------------------------------------------------
// FingerMonitorTask()
// -----------------------------------------------------------
void Fingerprint::FingerMonitorTask(void* parameter) {
    Fingerprint* self = static_cast<Fingerprint*>(parameter);

    for (;;) {
        self->lock_();
        bool stopReq = self->verifyLoopStopFlag;
        self->unlock_();
        if (stopReq) break;

        self->verifyFingerprint();
        vTaskDelay(pdMS_TO_TICKS(200)); // ~5Hz
    }

    self->lock_();
    self->fingerMonitorHandle = nullptr;
    self->unlock_();

    vTaskDelete(nullptr);
}

// -----------------------------------------------------------
// requestEnrollment()  (ENFP_xx)
// -----------------------------------------------------------
//
transport::StatusCode Fingerprint::requestEnrollment(uint16_t slotId) {
    lock_();

    if (!finger || !sensorPresent_) {
        DBG_PRINTLN("[FP] enroll denied (no sensor)");
        sendFpStatusEvent_(0x0B, transport::StatusCode::DENIED, {1}); // no sensor
        unlock_();
        return transport::StatusCode::DENIED;
    }

    if (tamperDetected_) {
        DBG_PRINTLN("[FP] enroll denied (tamper)");
        sendFpStatusEvent_(0x0B, transport::StatusCode::DENIED, {3}); // tamper
        unlock_();
        return transport::StatusCode::DENIED;
    }

    if (enrollmentTaskHandle != nullptr ||
        enrollmentState == FP_ENROLL_IN_PROGRESS) {
        DBG_PRINTLN("[FP] enroll busy");
        sendFpStatusEvent_(0x0B, transport::StatusCode::BUSY, {2}); // busy
        unlock_();
        return transport::StatusCode::BUSY;
    }

    bool wasVerifyRunning = (fingerMonitorHandle != nullptr);
    resumeVerifyAfterEnroll_ = wasVerifyRunning;
    targetEnrollID_ = slotId;
    enrollmentState = FP_ENROLL_IN_PROGRESS; // block verify while enrollment is pending
    verifyLoopStopFlag = true;
    unlock_();

    stopVerifyMode();

    if (wasVerifyRunning) {
        uint32_t waitStart = millis();
        while (true) {
            lock_();
            bool running = (fingerMonitorHandle != nullptr);
            unlock_();
            if (!running) break;
            if (millis() - waitStart > 2000UL) {
                DBG_PRINTLN("[FP] verify loop still stopping; aborting enroll");
                lock_();
                resumeVerifyAfterEnroll_ = false;
                enrollmentState = FP_ENROLL_IDLE;
                targetEnrollID_ = 0;
                unlock_();
                sendFpStatusEvent_(0x0B, transport::StatusCode::BUSY, {2}); // busy
                return transport::StatusCode::BUSY;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    lock_();
    if (fingerMonitorHandle != nullptr) {
        DBG_PRINTLN("[FP] enroll denied (verify still running)");
        resumeVerifyAfterEnroll_ = false;
        enrollmentState = FP_ENROLL_IDLE;
        targetEnrollID_ = 0;
        unlock_();
        sendFpStatusEvent_(0x0B, transport::StatusCode::BUSY, {2}); // busy
        return transport::StatusCode::BUSY;
    }

    DBG_PRINTF("[FP] enroll start slot=%u\n", (unsigned)slotId);

    // LED cue: "place finger"
    if (RGB) {
        RGB->postOverlay(OverlayEvent::FP_ENROLL_START);
    }
    unlock_();

    sendEnrollStage_(1, /*status*/0, slotId); // START

    lock_();
    BaseType_t created = xTaskCreate(
        Fingerprint::enrollFingerprint,
        "FPEnrollTask",
        4096,
        this,
        1,
        &enrollmentTaskHandle
    );
    unlock_();

    if (created != pdPASS) {
        DBG_PRINTLN("[FP] enroll task create failed");
        bool resumeVerify = false;
        lock_();
        enrollmentState = FP_ENROLL_IDLE;
        resumeVerify = resumeVerifyAfterEnroll_;
        resumeVerifyAfterEnroll_ = false;
        targetEnrollID_ = 0;
        unlock_();
        sendEnrollStage_(7, /*status*/1, slotId); // FAIL
        if (resumeVerify && isReadyForVerify_()) {
            startVerifyMode();
        }
        return transport::StatusCode::APPLY_FAIL;
    }

    return transport::StatusCode::OK;
}

// Wrapper for default slot
void Fingerprint::enrollFingerprintTask() {
    requestEnrollment(fingerprintID);
}

// -----------------------------------------------------------
// enrollFingerprint() task
// -----------------------------------------------------------
void Fingerprint::enrollFingerprint(void* parameter) {
    Fingerprint* self = static_cast<Fingerprint*>(parameter);

    DBG_PRINTLN("[FP] enroll task running");
    uint8_t res = self->doEnrollment_(self->targetEnrollID_);

    bool resumeVerify = false;
    self->lock_();
    self->enrollmentTaskHandle = nullptr;
    self->enrollmentState = (res == FINGERPRINT_OK)
                                ? FP_ENROLL_OK
                                : FP_ENROLL_FAIL;  // final state only; ACKs already sent
    resumeVerify = self->resumeVerifyAfterEnroll_;
    self->resumeVerifyAfterEnroll_ = false;
    self->unlock_();

    if (res == FINGERPRINT_OK) {
        DBG_PRINTLN("[FP] enroll OK");
        self->setDeviceConfigured(true);
        self->tamperDetected_ = false; // successful enroll implies trusted
        self->sendEnrollStage_(6, /*status*/0, self->targetEnrollID_); // OK
    }
    // NOTE: For timeout/failure, doEnrollment_ already sent progress/fail events.

#if FINGERPRINT_TEST_MODE
    if (res == FINGERPRINT_OK) {
        DBG_PRINTLN("[FP] enrollment flow done; type 'verify start' to resume matching");
    } else {
        DBG_PRINTLN("[FP] enrollment did not complete; try again with 'enroll <slot>'");
    }
#endif

    if (resumeVerify && self->isReadyForVerify_()) {
        self->startVerifyMode();
    }

    vTaskDelete(nullptr);
}

// -----------------------------------------------------------
// doEnrollment_(slotId)
// -----------------------------------------------------------
uint8_t Fingerprint::doEnrollment_(uint16_t slotId) {
    if (!finger) {
        DBG_PRINTLN("[FP] enroll fail (no sensor)");
        if (RGB) RGB->postOverlay(OverlayEvent::FP_ENROLL_FAIL);
        sendEnrollStage_(7, /*status*/1, slotId);
        return FINGERPRINT_PACKETRECIEVEERR;
    }

    const uint32_t SCAN_TIMEOUT_MS = 30000;
    const uint32_t LIFT_TIMEOUT_MS = 30000;
#if FINGERPRINT_TEST_MODE
    enrollPrint_("Enrollment started. Follow the prompts.");
#endif

    // ---- First scan ----
    uint32_t t0 = millis();
    uint8_t p = 0xFF;
#if FINGERPRINT_TEST_MODE
    enrollPrint_("Waiting for valid finger to enroll (first capture)...");
    uint32_t lastNoFingerMs = 0;
#endif
    while (p != FINGERPRINT_OK) {
        p = finger->getImage();
        switch (p) {
            case FINGERPRINT_OK:
#if FINGERPRINT_TEST_MODE
                enrollPrint_("Image taken.");
#endif
                break;
            case FINGERPRINT_NOFINGER:
#if FINGERPRINT_TEST_MODE
                if (millis() - lastNoFingerMs >= 1000UL) {
                    DBG_PRINT(".");
                    lastNoFingerMs = millis();
                }
#endif
                break;
            case FINGERPRINT_PACKETRECIEVEERR:
            case FINGERPRINT_IMAGEFAIL:
#if FINGERPRINT_TEST_MODE
                enrollPrint_(enrollGetImageError_(p));
#endif
                break;
            default:
#if FINGERPRINT_TEST_MODE
                enrollPrint_(enrollGetImageError_(p));
#endif
                break;
        }
        if (p == FINGERPRINT_OK) break;
        if (millis() - t0 > SCAN_TIMEOUT_MS) {
            DBG_PRINTLN("[FP] enroll timeout (capture1)");
            if (RGB) RGB->postOverlay(OverlayEvent::FP_ENROLL_TIMEOUT);
            sendEnrollStage_(8, /*status*/1, slotId); // TIMEOUT
            return FINGERPRINT_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
#if FINGERPRINT_TEST_MODE
    DBG_PRINTLN();
#endif

    p = finger->image2Tz(1);
    if (p != FINGERPRINT_OK) {
#if FINGERPRINT_TEST_MODE
        enrollPrint_(enrollImage2TzError_(p));
#endif
        if (RGB) RGB->postOverlay(OverlayEvent::FP_ENROLL_FAIL);
        sendEnrollStage_(7, /*status*/1, slotId); // FAIL
        return p;
    }
#if FINGERPRINT_TEST_MODE
    enrollPrint_("Image converted.");
#endif
    if (RGB) RGB->postOverlay(OverlayEvent::FP_ENROLL_CAPTURE1);
    sendEnrollStage_(2, /*status*/0, slotId); // CAP1

    // ---- Ask user to lift finger ----
    vTaskDelay(pdMS_TO_TICKS(250)); // allow master/UI to show CAP1
    if (RGB) RGB->postOverlay(OverlayEvent::FP_ENROLL_LIFT);
    sendEnrollStage_(3, /*status*/0, slotId); // LIFT
#if FINGERPRINT_TEST_MODE
    enrollPrint_("Remove finger now.");
#endif
    vTaskDelay(pdMS_TO_TICKS(2000)); // match reference flow before checking removal

    t0 = millis();
    while (true) {
        uint8_t p = finger->getImage();
        if (p == FINGERPRINT_NOFINGER) {
#if FINGERPRINT_TEST_MODE
            enrollPrint_("Finger removed.");
#endif
            break;
        }
        if (millis() - t0 > LIFT_TIMEOUT_MS) {
            DBG_PRINTLN("[FP] enroll timeout (lift)");
            if (RGB) RGB->postOverlay(OverlayEvent::FP_ENROLL_TIMEOUT);
            sendEnrollStage_(8, /*status*/1, slotId); // TIMEOUT
            return FINGERPRINT_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    // ---- Second scan ----
    p = 0xFF;
    t0 = millis();
#if FINGERPRINT_TEST_MODE
    enrollPrint_("Place same finger again for second capture.");
    uint32_t lastNoFingerMs2 = 0;
#endif
    while (p != FINGERPRINT_OK) {
        p = finger->getImage();
        switch (p) {
            case FINGERPRINT_OK:
#if FINGERPRINT_TEST_MODE
                enrollPrint_("Image taken.");
#endif
                break;
            case FINGERPRINT_NOFINGER:
#if FINGERPRINT_TEST_MODE
                if (millis() - lastNoFingerMs2 >= 1000UL) {
                    DBG_PRINT(".");
                    lastNoFingerMs2 = millis();
                }
#endif
                break;
            case FINGERPRINT_PACKETRECIEVEERR:
            case FINGERPRINT_IMAGEFAIL:
#if FINGERPRINT_TEST_MODE
                enrollPrint_(enrollGetImageError_(p));
#endif
                break;
            default:
#if FINGERPRINT_TEST_MODE
                enrollPrint_(enrollGetImageError_(p));
#endif
                break;
        }
        if (p == FINGERPRINT_OK) break;
        if (millis() - t0 > SCAN_TIMEOUT_MS) {
            DBG_PRINTLN("[FP] enroll timeout (capture2)");
            if (RGB) RGB->postOverlay(OverlayEvent::FP_ENROLL_TIMEOUT);
            sendEnrollStage_(8, /*status*/1, slotId); // TIMEOUT
            return FINGERPRINT_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
#if FINGERPRINT_TEST_MODE
    DBG_PRINTLN();
#endif

    p = finger->image2Tz(2);
    if (p != FINGERPRINT_OK) {
#if FINGERPRINT_TEST_MODE
        enrollPrint_(enrollImage2TzError_(p));
#endif
        if (RGB) RGB->postOverlay(OverlayEvent::FP_ENROLL_FAIL);
        sendEnrollStage_(7, /*status*/1, slotId); // FAIL
        return p;
    }
#if FINGERPRINT_TEST_MODE
    enrollPrint_("Image converted.");
#endif
    if (RGB) RGB->postOverlay(OverlayEvent::FP_ENROLL_CAPTURE2);
    sendEnrollStage_(4, /*status*/0, slotId); // CAP2

    // ---- Build model + store ----
    vTaskDelay(pdMS_TO_TICKS(250)); // allow master/UI to show CAP2
    if (RGB) RGB->postOverlay(OverlayEvent::FP_ENROLL_STORING);
    sendEnrollStage_(5, /*status*/0, slotId); // STORING

#if FINGERPRINT_TEST_MODE
    enrollPrint_("Creating model...");
#endif
    p = finger->createModel();
    if (p != FINGERPRINT_OK) {
        DBG_PRINTF("[FP] enroll createModel fail=%u\n", (unsigned)p);
#if FINGERPRINT_TEST_MODE
        enrollPrint_(enrollModelError_(p));
#endif
        if (RGB) RGB->postOverlay(OverlayEvent::FP_ENROLL_FAIL);
        sendEnrollStage_(7, /*status*/1, slotId);
        return p;
    }
#if FINGERPRINT_TEST_MODE
    enrollPrint_("Prints matched.");
#endif

#if FINGERPRINT_TEST_MODE
    enrollPrint_("Storing template...");
#endif
    p = finger->storeModel(slotId);
    if (p != FINGERPRINT_OK) {
        DBG_PRINTF("[FP] enroll storeModel fail=%u\n", (unsigned)p);
#if FINGERPRINT_TEST_MODE
        enrollPrint_(enrollStoreError_(p));
#endif
        if (RGB) RGB->postOverlay(OverlayEvent::FP_ENROLL_FAIL);
        sendEnrollStage_(7, /*status*/1, slotId);
        return p;
    }
#if FINGERPRINT_TEST_MODE
    enrollPrint_("Stored.");
#endif

    // success (overlay only; final ENOK is sent by enrollFingerprint task)
    if (RGB) RGB->postOverlay(OverlayEvent::FP_ENROLL_OK);
    return FINGERPRINT_OK;
}

// -----------------------------------------------------------
// Enrollment state helpers / DB helpers / prefs
// (unchanged logic except they're allowed to SendAck() because
// they're direct command responses, not background spam.)
// -----------------------------------------------------------
uint8_t Fingerprint::getEnrollmentState() {
    lock_();
    uint8_t st = enrollmentState;
    unlock_();
    return st;
}

void Fingerprint::resetEnrollmentState() {
    lock_();
    enrollmentState = FP_ENROLL_IDLE;
    unlock_();
}

transport::StatusCode Fingerprint::deleteFingerprint(uint16_t id) {
    if (!finger) {
        return transport::StatusCode::DENIED;
    }

    lock_();
    uint8_t p = finger->deleteModel(id);
    unlock_();

    return (p == FINGERPRINT_OK) ? transport::StatusCode::OK
                                 : transport::StatusCode::APPLY_FAIL;
}

transport::StatusCode Fingerprint::deleteFingerprint() {
    return deleteFingerprint(fingerprintID);
}

transport::StatusCode Fingerprint::deleteAllFingerprints() {
    if (!finger) {
        return transport::StatusCode::DENIED;
    }

    lock_();
    uint8_t p = finger->emptyDatabase();
    unlock_();

    return (p == FINGERPRINT_OK) ? transport::StatusCode::OK
                                 : transport::StatusCode::APPLY_FAIL;
}

bool Fingerprint::getDbInfo(uint16_t& count, uint16_t& cap) {
    if (!finger) return false;

    lock_();
    finger->getTemplateCount();
    count = finger->templateCount;
    cap   = finger->capacity;
    unlock_();
    return true;
}

int16_t Fingerprint::findNextFreeID() {
    if (!finger) {
        return -1;
    }

    lock_();
    finger->getTemplateCount();
    uint16_t cap = finger->capacity;

    for (uint16_t id = 1; id <= cap; ++id) {
        uint8_t p = finger->loadModel(id);
        if (p != FINGERPRINT_OK) {
            unlock_();
            return id; // first empty slot
        }
    }
    unlock_();
    return -1;
}

bool Fingerprint::getNextFreeId(uint16_t& id) {
    int16_t tmp = findNextFreeID();
    if (tmp <= 0) return false;
    id = static_cast<uint16_t>(tmp);
    return true;
}

// -----------------------------------------------------------
// Preferences flag
// -----------------------------------------------------------
bool Fingerprint::isDeviceConfigured() {
    if (!CONF) return false;
    return CONF->GetBool(FP_DEVICE_CONFIGURED_KEY, false);
}

void Fingerprint::setDeviceConfigured(bool value) {
    if (!CONF) return;
    CONF->PutBool(FP_DEVICE_CONFIGURED_KEY, value);
}
