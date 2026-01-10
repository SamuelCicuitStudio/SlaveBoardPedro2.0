/**************************************************************
 *  Author      : Tshibangu Samuel
 *  Role        : Freelance Embedded Systems Engineer
 *  Expertise   : Secure IoT Systems, Embedded C++, RTOS, Control Logic
 *  Contact     : tshibsamuel47@gmail.com
 *  Portfolio   : https://www.freelancer.com/u/tshibsamuel477
 *  Phone       : +216 54 429 793
 **************************************************************/
#ifndef SWITCH_MANAGER_H
#define SWITCH_MANAGER_H

/**
 * RTOS-free SwitchManager
 * - Call service() regularly from your main loop (e.g., every ~300 ms).
 * - Door/open edges still post overlays.
 * - BOOT long-press triggers reset flow; triple-tap preserved.
 */

#include <Arduino.h>
#include <Config.hpp>

// -----------------------------
// Tap / Hold configuration
// -----------------------------
#ifndef BUTTON_PIN
#define BUTTON_PIN            BOOT_BUTTON_PIN   // Active-low input (BOOT)
#endif
#ifndef TAP_WINDOW_MS
#define TAP_WINDOW_MS         1200              // triple-tap window (same as before)
#endif
#ifndef HOLD_THRESHOLD_MS
#define HOLD_THRESHOLD_MS     3000              // long-press threshold (ms)
#endif

class SwitchManager {
public:
    SwitchManager();

    // Optional init (pins). Safe to call multiple times.
    void begin();

    // Polling entrypoint — call from your main loop
    void service();

    // Door reed status (active-low: HIGH=open). Posts overlay only on edge.
    bool isDoorOpen();

    // Open button status (active-low: LOW=pressed). Posts overlay on rising edge.
    bool isOpenButtonPressed();


private:
    // Global instance pointer used by static ISRs
    static SwitchManager* s_instance_;

    // -------- fast edge flags (set from IRQ) --------
    volatile bool doorEdgeFlag_  = false;
    volatile bool openEdgeFlag_  = false;

    // -------- door / open edges --------
    bool firstDoorSample_   = true;
    bool lastDoorOpen_      = false;
    bool lastOpenBtn_       = false;

    // -------- BOOT button tap/hold state machine --------
    bool     bootPrev_      = false;
    uint8_t  tapCount_      = 0;
    uint32_t pressStartMs_  = 0;
    uint32_t lastTapMs_     = 0;

    // helpers
    void handleBootTapHold_();

    // -------- USER button tap state machine --------
    bool     userPrev_      = false;
    uint8_t  userTapCount_  = 0;
    uint32_t userPressMs_   = 0;
    uint32_t userLastTapMs_ = 0;

    void handleUserTap_();
    void printMac_();
    void toggleRgbFeedback_();

    // Fast ISR hooks for door/open edges (ESP32 / Arduino interrupt)
    static void IRAM_ATTR doorIsrThunk_();
    static void IRAM_ATTR openIsrThunk_();
    void IRAM_ATTR onDoorEdge_();
    void IRAM_ATTR onOpenEdge_();
};

#endif // SWITCH_MANAGER_H






