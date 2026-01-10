#include <SwitchManager.hpp>
#include <ResetManager.hpp>
#include <RGBLed.hpp>
#include <Utils.hpp>
#include <esp_system.h>
#include <stdio.h>

// Small helper
static inline uint32_t nowMs() { return millis(); }

// Static instance pointer for ISR thunks
SwitchManager* SwitchManager::s_instance_ = nullptr;

SwitchManager::SwitchManager() {
    s_instance_ = this;
    // Your original behavior: set BOOT input; leave other pins as-is.
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
    pinMode(USER_BUTTON_PIN, INPUT_PULLUP);

    DBGSTR();
    DBG_PRINTLN("###########################################################");
    DBG_PRINTLN("#           Starting Switch Manager (RTOS-free)           #");
    DBG_PRINTLN("###########################################################");
    DBG_PRINT  ("BOOT_BUTTON_PIN: "); DBG_PRINTLN((int)BOOT_BUTTON_PIN);
    DBG_PRINT  ("TAP_WINDOW_MS  : "); DBG_PRINTLN((int)TAP_WINDOW_MS);
    DBG_PRINT  ("HOLD_THRESHOLD : "); DBG_PRINTLN((int)HOLD_THRESHOLD_MS);
    DBGSTP();
}

void SwitchManager::begin() {
    // Keep minimal (same as your original ctor setup); safe to re-call.
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
    pinMode(USER_BUTTON_PIN, INPUT_PULLUP);
    pinMode(REED_SWITCH_PIN, INPUT_PULLUP);
    pinMode(OPEN_SWITCH_PIN, INPUT_PULLUP);

#ifdef ARDUINO_ARCH_ESP32
    // Fast edge detection on door reed and open button.
    // ISR only sets flags; actual logic stays in isDoorOpen()/isOpenButtonPressed().
    attachInterrupt(digitalPinToInterrupt(REED_SWITCH_PIN),  SwitchManager::doorIsrThunk_, CHANGE);
    attachInterrupt(digitalPinToInterrupt(OPEN_SWITCH_PIN),  SwitchManager::openIsrThunk_, CHANGE);
#endif
}

// ------------------------------------------------------------
// Polling entrypoint — call this from your main loop
// ------------------------------------------------------------
void SwitchManager::service() {
    // Maintain door & open-button overlays on edges
    (void)isDoorOpen();
    (void)isOpenButtonPressed();

    // Handle BOOT button tap/hold state machine
    handleBootTapHold_();

    // Handle USER button tap state machine
    handleUserTap_();
}

// ------------------------------------------------------------
// Door reed (active-low: HIGH=open) — overlay on edge only
// ------------------------------------------------------------
bool SwitchManager::isDoorOpen() {
    const bool open = (digitalRead(REED_SWITCH_PIN) == HIGH);

    if (firstDoorSample_) {
        firstDoorSample_ = false;
        lastDoorOpen_ = open;               // silent baseline (no overlay)
    } else if (open != lastDoorOpen_) {
        DBGSTR();
        DBG_PRINTLN("[SW] Door state change");
        DBG_PRINT  ("      prev="); DBG_PRINTLN(lastDoorOpen_ ? "OPEN" : "CLOSED");
        DBG_PRINT  ("      curr="); DBG_PRINTLN(open          ? "OPEN" : "CLOSED");
        DBGSTP();

        if (RGB) {
            RGB->postOverlay(open ? OverlayEvent::DOOR_OPEN
                                  : OverlayEvent::DOOR_CLOSED);
        }
        lastDoorOpen_ = open;
    }

    return open;
}

// ------------------------------------------------------------
// Open button (active-low: LOW=pressed) — overlay on rising edge
// ------------------------------------------------------------
bool SwitchManager::isOpenButtonPressed() {
    const bool pressed = (digitalRead(OPEN_SWITCH_PIN) == LOW);

    if (pressed && !lastOpenBtn_) {
        DBGSTR();
        DBG_PRINTLN("[SW] Open button pressed (rising edge)");
        DBGSTP();
    }
    lastOpenBtn_ = pressed;
    return pressed;
}

// ------------------------------------------------------------
// BOOT button tap/hold — same thresholds & prints as your task loop
// ------------------------------------------------------------
void SwitchManager::handleBootTapHold_() {
    const bool pressed = (digitalRead(BUTTON_PIN) == LOW);
    const uint32_t t   = nowMs();

    // Edge: press down -> start timing
    if (pressed && !bootPrev_) {
        pressStartMs_ = t;
    }

    // Edge: release -> evaluate press duration
    if (!pressed && bootPrev_) {
        const uint32_t pressDur = t - pressStartMs_;

        if (pressDur >= HOLD_THRESHOLD_MS) {
            // === Long-press group ===
            DBGSTR();
            DBG_PRINTLN("[SW] Long press detected 🕒");
            DBG_PRINTLN("###########################################################");
            DBG_PRINTLN("#                   Resetting device 🔄                   #");
            DBG_PRINTLN("###########################################################");
            DBGSTP();

            ResetManager::RequestFactoryReset("BOOT long press");

            tapCount_  = 0;   // cancel any tap sequence
            lastTapMs_ = 0;
        } else {
            // Short press → tap
            tapCount_++;
            lastTapMs_ = t;

            DBGSTR();
            DBG_PRINT  ("[SW] Tap detected (count="); DBG_PRINT((int)tapCount_);
            DBG_PRINT  (", dur="); DBG_PRINT((int)pressDur);
            DBG_PRINTLN(" ms)");
            DBGSTP();

            // Triple tap detection (kept exactly as your logic)
            if (tapCount_ == 3) {
                if ((t - lastTapMs_) <= TAP_WINDOW_MS) {
                    DBGSTR();
                    DBG_PRINTLN("[SW] Triple tap detected 🖱️🖱️🖱️");
                    DBGSTP();
                    tapCount_ = 0;
                } else {
                    DBGSTR();
                    DBG_PRINTLN("[SW] Triple tap window elapsed; reset count");
                    DBGSTP();
                    tapCount_ = 0;
                }
            }
        }
    }

    // Tap timeout (same threshold as your code path)
    if (tapCount_ > 0 && (t - lastTapMs_) > 1500U) {
        DBGSTR();
        DBG_PRINTLN("[SW] Tap timeout ⏱️ → reset tapCount");
        DBGSTP();
        tapCount_ = 0;
    }
    bootPrev_ = pressed;
}

// ------------------------------------------------------------
// USER button tap: single tap prints MAC, triple tap toggles RGB
// ------------------------------------------------------------
void SwitchManager::handleUserTap_() {
    const bool pressed = (digitalRead(USER_BUTTON_PIN) == LOW);
    const uint32_t t   = nowMs();

    if (pressed && !userPrev_) {
        userPressMs_ = t;
    }

    if (!pressed && userPrev_) {
        const uint32_t pressDur = t - userPressMs_;
        userTapCount_++;
        userLastTapMs_ = t;

        DBGSTR();
        DBG_PRINT  ("[SW] User tap detected (count="); DBG_PRINT((int)userTapCount_);
        DBG_PRINT  (", dur="); DBG_PRINT((int)pressDur);
        DBG_PRINTLN(" ms)");
        DBGSTP();

        DBG_PRINTLN("[SW] User tap -> print MAC");
        printMac_();

        if (userTapCount_ == 3) {
            if ((t - userLastTapMs_) <= TAP_WINDOW_MS) {
                DBG_PRINTLN("[SW] User triple tap -> toggle RGB feedback");
                toggleRgbFeedback_();
                userTapCount_ = 0;
            } else {
                userTapCount_ = 0;
            }
        }
    }

    if (userTapCount_ > 0 && (t - userLastTapMs_) > 1500U) {
        userTapCount_ = 0;
    }

    userPrev_ = pressed;
}

void SwitchManager::printMac_() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    DBGSTR();
    DBG_PRINTLN("###########################################################");
    DBG_PRINTLN(String("#       Slave MAC Address:     ") + macStr + "          #");
    DBG_PRINTLN("###########################################################");
    DBGSTP();
}

void SwitchManager::toggleRgbFeedback_() {
    RGBLed* rgb = RGBLed::TryGet();
    if (!rgb) {
        DBG_PRINTLN("[SW] RGB feedback toggle ignored (RGB not ready)");
        return;
    }
    const bool next = !rgb->isEnabled();
    rgb->setEnabled(next);
    DBG_PRINTLN(next ? "[SW] RGB feedback enabled" : "[SW] RGB feedback disabled");
}

// ------------------------------------------------------------
// IRQ support: door/open fast edge flags
// ------------------------------------------------------------
void IRAM_ATTR SwitchManager::doorIsrThunk_() {
    if (s_instance_) {
        s_instance_->onDoorEdge_();
    }
}

void IRAM_ATTR SwitchManager::openIsrThunk_() {
    if (s_instance_) {
        s_instance_->onOpenEdge_();
    }
}

void IRAM_ATTR SwitchManager::onDoorEdge_() {
    doorEdgeFlag_ = true;
}

void IRAM_ATTR SwitchManager::onOpenEdge_() {
    openEdgeFlag_ = true;
}

