/**************************************************************
 *  Author      : Tshibangu Samuel
 *  Role        : Freelance Embedded Systems Engineer
 *  Expertise   : Secure IoT Systems, Embedded C++, RTOS, Control Logic
 *  Contact     : tshibsamuel47@gmail.com
 *  Portfolio   : https://www.freelancer.com/u/tshibsamuel477
 *  Phone       : +216 54 429 793
 **************************************************************/
#ifndef RGB_CONFIG_H
#define RGB_CONFIG_H

/**
 * @file RGBConfig.h
 * @brief Distinct color palette for backgrounds (DeviceState)
 *        and overlays (OverlayEvent).
 *
 * Priorities:
 *  - PRIO_BACKGROUND = 0, PRIO_ACTION = 1, PRIO_ALERT = 2, PRIO_CRITICAL = 3.
 * Overlays preempt when priority >= current.
 */

// =============================================================
//  Task sizing
// =============================================================
#define RGB_TASK_STACK     4096
#define RGB_TASK_PRIORITY  2
#define RGB_CMD_QUEUE_LEN  24

// =============================================================
//  Color helpers
// =============================================================
#define RGB_HEX(r,g,b)   ( ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b) )
#define RGB_R(c)         ( (uint8_t)(((c) >> 16) & 0xFF) )
#define RGB_G(c)         ( (uint8_t)(((c) >> 8) & 0xFF) )
#define RGB_B(c)         ( (uint8_t)((c) & 0xFF) )

// Rainbow speed (PAIRING)
#define RGB_RAINBOW_STEP_DEG 8.0f

// =============================================================
//  Background (DeviceState) colors  — all distinct from overlays
// =============================================================
// INIT/BOOT: very dim neutral gray (steady)
#define RGB_BG_INIT            RGB_HEX(10,10,10)
// READY_ONLINE: firm green heartbeat (double pulse)
#define RGB_BG_READY_ONLINE    RGB_HEX(0,180,60)     // #00B43C
// READY_OFFLINE: **indigo blink** (not amber anymore)
#define RGB_BG_READY_OFFLINE   RGB_HEX(93,0,255)     // #5D00FF
// SLEEP: faint deep-blue heartbeat every ~10s
#define RGB_BG_SLEEP_BEAT      RGB_HEX(26,46,128)    // #1A2E80
// PAIRING: rainbow animation (timing stays in code)
#define RGB_BG_PAIRING_STEP_MS 30

// =============================================================
//  Overlay palette — unique hues per meaning
// =============================================================

// --- Radio & Motor (all unique) ---
#define RGB_OVR_ESPNOW_RX          RGB_HEX(0,255,255)   // CYAN
#define RGB_OVR_ESPNOW_TX          RGB_HEX(255,0,255)   // MAGENTA
#define RGB_OVR_UNLOCKING          RGB_HEX(0,109,255)   // ROYAL BLUE
#define RGB_OVR_LOCKING            RGB_HEX(0,255,157)   // MINT

// --- Security & Network ---
#define RGB_OVR_BREACH             RGB_HEX(255,59,0)    // ORANGE-RED
#define RGB_OVR_NET_LOST           RGB_HEX(125,0,255)   // VIOLET (distinct from offline indigo)
#define RGB_OVR_NET_RECOVER        RGB_HEX(0,255,127)   // SPRING GREEN
#define RGB_OVR_WAKE_FLASH         RGB_HEX(255,255,255) // WHITE
#define RGB_OVR_RESET_TRIGGER      RGB_HEX(0,225,255)   // ICE BLUE

// --- Power ---
#define RGB_OVR_LOW_BATT           RGB_HEX(255,149,0)   // ORANGE (no amber)
#define RGB_OVR_CRITICAL_BATT      RGB_HEX(255,0,0)     // RED

// --- Inputs & Sensors ---
#define RGB_OVR_FP_DETECTED        RGB_HEX(0,200,255)   // SKY
#define RGB_OVR_FP_MATCH           RGB_HEX(92,255,0)    // NEON LIME (bright, distinct from online)
#define RGB_OVR_FP_FAIL            RGB_HEX(220,20,60)   // CRIMSON
#define RGB_OVR_BUTTON_PRESSED     RGB_HEX(255,0,160)   // FUCHSIA
#define RGB_OVR_DOOR_OPEN          RGB_HEX(255,122,0)   // TANGERINE
#define RGB_OVR_DOOR_CLOSED        RGB_HEX(0,167,167)   // TEAL
#define RGB_OVR_SHOCK_DETECTED     RGB_HEX(255,0,127)   // HOT PINK

// --- Fingerprint Enrollment (each step a different hue) ---
#define RGB_OVR_FP_ENROLL_START    RGB_HEX(0,77,255)    // COBALT
#define RGB_OVR_FP_ENROLL_LIFT     RGB_HEX(255,234,0)   // BRIGHT YELLOW
#define RGB_OVR_FP_ENROLL_CAPTURE1 RGB_HEX(0,229,255)   // ARCTIC
#define RGB_OVR_FP_ENROLL_CAPTURE2 RGB_HEX(0,255,200)   // SEAFOAM
#define RGB_OVR_FP_ENROLL_STORING  RGB_HEX(106,106,255) // PERIWINKLE
#define RGB_OVR_FP_ENROLL_OK       RGB_HEX(38,255,218)  // TURQUOISE
#define RGB_OVR_FP_ENROLL_FAIL     RGB_HEX(255,23,68)   // VIVID RED
#define RGB_OVR_FP_ENROLL_TIMEOUT  RGB_HEX(255,90,0)    // ORANGE-RED (different from BREACH)

// --- Arming overlays ---
#define RGB_OVR_ARMED_ON           RGB_HEX(255,40,0)    // BRIGHT ORANGE-RED (armed)
#define RGB_OVR_ARMED_OFF          RGB_HEX(0,200,80)    // BRIGHT GREEN (disarmed)

// (FP_TAMPER uses FP_FAIL in code; if you'd like its own hue, I can wire that too)

#endif  // RGB_CONFIG_H






