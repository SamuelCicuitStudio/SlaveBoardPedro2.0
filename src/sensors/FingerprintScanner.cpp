#include <FingerprintScanner.hpp>
#include <ConfigNvs.hpp>
#include <NVSManager.hpp>
#include <RGBLed.hpp>

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
    bool ok = initSensor_(false);
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
}

void Fingerprint::sendFpEvent_(uint8_t op, const std::vector<uint8_t>& payload) {
    if (!transport_) return;
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
    pl.reserve(1 + extra.size());
    pl.push_back(static_cast<uint8_t>(status));
    pl.insert(pl.end(), extra.begin(), extra.end());
    sendFpEvent_(op, pl);
}

void Fingerprint::sendEnrollStage_(uint8_t stage, uint8_t status, uint16_t slot) {
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
    unlock_();

    // give tasks time to exit + clear handles
    vTaskDelay(pdMS_TO_TICKS(150));
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
        sendFpEvent_(0x06, pl); // reuse QueryDb opcode as event
    } else if (sensorPresent_) {
        // sensor answered, but it's not ours (tampered / wrong password)
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
    stopAllFpTasks_();

    bool success = initSensor_(true); // try to claim the sensor

    if (success) {
        startVerifyMode();
    }

    sendFpStatusEvent_(0x08, success ? transport::StatusCode::OK
                                     : transport::StatusCode::APPLY_FAIL);
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

    sendFpStatusEvent_(0x09, success ? transport::StatusCode::OK
                                     : transport::StatusCode::APPLY_FAIL);

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
        unlock_();
        return;
    }

    // sensor not ready/trusted? don't start
    if (!isReadyForVerify_()) {
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
    unlock_();
}

void Fingerprint::stopVerifyMode() {
    lock_();
    verifyLoopStopFlag = true;
    unlock_();
    // FingerMonitorTask will self-delete and clear handle
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
    p = finger->fingerFastSearch();
    if (p == FINGERPRINT_OK) {
        // MATCH = legit event -> report immediately once
        if (RGB) {
        }
        // Transport event MatchEvent (op 0x0A)
        std::vector<uint8_t> pl;
        pl.push_back(uint8_t(finger->fingerID & 0xFF));
        pl.push_back(uint8_t((finger->fingerID >> 8) & 0xFF));
        pl.push_back(uint8_t(finger->confidence));
        sendFpEvent_(0x0A, pl);
    } else {
        // NO MATCH -> local feedback only, absolutely no spam
        if (RGB) {
        }
        sendFpStatusEvent_(0x0B, transport::StatusCode::DENIED, {0}); // match_fail
    }

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

    if (!finger) {
        sendFpStatusEvent_(0x0B, transport::StatusCode::DENIED, {1}); // no sensor
        unlock_();
        return transport::StatusCode::DENIED;
    }

    if (enrollmentTaskHandle != nullptr ||
        enrollmentState == FP_ENROLL_IN_PROGRESS) {
        sendFpStatusEvent_(0x0B, transport::StatusCode::BUSY, {2}); // busy
        unlock_();
        return transport::StatusCode::BUSY;
    }

    targetEnrollID_     = slotId;
    enrollmentState     = FP_ENROLL_IN_PROGRESS;
    verifyLoopStopFlag  = true;   // ask verify task to stop

    // LED cue: "place finger"
    if (RGB) {
        RGB->postOverlay(OverlayEvent::FP_ENROLL_START);
    }

    sendEnrollStage_(1, /*status*/0, slotId); // START

    unlock_();

    // let verify task wind down
    vTaskDelay(pdMS_TO_TICKS(150));

    lock_();
    xTaskCreate(
        Fingerprint::enrollFingerprint,
        "FPEnrollTask",
        4096,
        this,
        1,
        &enrollmentTaskHandle
    );
    unlock_();

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

    uint8_t res = self->doEnrollment_(self->targetEnrollID_);

    self->lock_();
    self->enrollmentTaskHandle = nullptr;
    self->enrollmentState = (res == FINGERPRINT_OK)
                                ? FP_ENROLL_OK
                                : FP_ENROLL_FAIL;  // final state only; ACKs already sent
    self->unlock_();

    if (res == FINGERPRINT_OK) {
        self->setDeviceConfigured(true);
        self->tamperDetected_ = false; // successful enroll implies trusted
        self->sendEnrollStage_(6, /*status*/0, self->targetEnrollID_); // OK
    }
    // NOTE: For timeout/failure, doEnrollment_ already sent progress/fail events.

    // only restart verify task if sensor is still trusted
    if (self->isReadyForVerify_()) {
        self->startVerifyMode();
    }

    vTaskDelete(nullptr);
}

// -----------------------------------------------------------
// doEnrollment_(slotId)
// -----------------------------------------------------------
uint8_t Fingerprint::doEnrollment_(uint16_t slotId) {
    if (!finger) {
        if (RGB) RGB->postOverlay(OverlayEvent::FP_ENROLL_FAIL);
        sendEnrollStage_(7, /*status*/1, slotId);
        return FINGERPRINT_PACKETRECIEVEERR;
    }

    const uint32_t SCAN_TIMEOUT_MS = 15000;

    // ---- First scan ----
    uint32_t t0 = millis();
    while (true) {
        uint8_t p = finger->getImage();
        if (p == FINGERPRINT_OK) {
            p = finger->image2Tz(1);
            if (p == FINGERPRINT_OK) {
                // first capture good
                if (RGB) RGB->postOverlay(OverlayEvent::FP_ENROLL_CAPTURE1);
                sendEnrollStage_(2, /*status*/0, slotId); // CAP1
                break;
            }
        }
        if (millis() - t0 > SCAN_TIMEOUT_MS) {
            if (RGB) RGB->postOverlay(OverlayEvent::FP_ENROLL_TIMEOUT);
            sendEnrollStage_(8, /*status*/1, slotId); // TIMEOUT
            return FINGERPRINT_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    // ---- Ask user to lift finger ----
    if (RGB) RGB->postOverlay(OverlayEvent::FP_ENROLL_LIFT);
    sendEnrollStage_(3, /*status*/0, slotId); // LIFT

    t0 = millis();
    while (finger->getImage() != FINGERPRINT_NOFINGER) {
        if (millis() - t0 > 3000) break;
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    // ---- Second scan ----
    t0 = millis();
    while (true) {
        uint8_t p = finger->getImage();
        if (p == FINGERPRINT_OK) {
            p = finger->image2Tz(2);
            if (p == FINGERPRINT_OK) {
                // second capture good
                if (RGB) RGB->postOverlay(OverlayEvent::FP_ENROLL_CAPTURE2);
                sendEnrollStage_(4, /*status*/0, slotId); // CAP2
                break;
            }
        }
        if (millis() - t0 > SCAN_TIMEOUT_MS) {
            if (RGB) RGB->postOverlay(OverlayEvent::FP_ENROLL_TIMEOUT);
            sendEnrollStage_(8, /*status*/1, slotId); // TIMEOUT
            return FINGERPRINT_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    // ---- Build model + store ----
    if (RGB) RGB->postOverlay(OverlayEvent::FP_ENROLL_STORING);
    sendEnrollStage_(5, /*status*/0, slotId); // STORING

    uint8_t p = finger->createModel();
    if (p != FINGERPRINT_OK) {
        if (RGB) RGB->postOverlay(OverlayEvent::FP_ENROLL_FAIL);
        sendEnrollStage_(7, /*status*/1, slotId);
        return p;
    }

    p = finger->storeModel(slotId);
    if (p != FINGERPRINT_OK) {
        if (RGB) RGB->postOverlay(OverlayEvent::FP_ENROLL_FAIL);
        sendEnrollStage_(7, /*status*/1, slotId);
        return p;
    }

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
