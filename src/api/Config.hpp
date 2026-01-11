/**************************************************************
 *  Author      : Tshibangu Samuel
 *  Role        : Freelance Embedded Systems Engineer
 *  Expertise   : Secure IoT Systems, Embedded C++, RTOS, Control Logic
 *  Contact     : tshibsamuel47@gmail.com
 *  Portfolio   : https://www.freelancer.com/u/tshibsamuel477
 *  Phone       : +216 54 429 793
 **************************************************************/
#ifndef CONFIG_H
#define CONFIG_H

/**
 * @file Config.h
 * @brief Central configuration for the ESP32 Slave (lock/alarm board).
 *
 * @details
 * MASTER ⇄ SLAVE: What the Master must know about this Slave (7 points)
 * 1) Identity & Pairing
 *    - Keys: DEVICE_NAME, DEVICE_ID, MASTER_ESPNOW_ID, DEVICE_CONFIGURED, RESET_FLAG
 *    - Purpose: identify this unit, verify pairing, and know if it’s been fully configured.
 *
 * 2) Capabilities Map (Hardware Presence)
 *    - Keys: HAS_OPEN_SWITCH_KEY, HAS_SHOCK_SENSOR_KEY, HAS_REED_SWITCH_KEY, HAS_FINGERPRINT_KEY
 *    - Purpose: tells the Master which peripherals exist on this board so it can enable/disable
 *      features (e.g., expect door events only if HAS_REED_SWITCH_KEY = true).
 *
 * 3) Lock Mode & Motor Behavior
 *    - Keys: LOCK_EMAG_KEY (electromagnet vs screw motor), LOCK_TIMEOUT_KEY
 *    - Purpose: tells the Master how to command this Slave (pulse vs. run-until-endstops) and
 *      what safety timeout is imposed at the Slave.
 *
 * 4) Current State & Security Flags
 *    - Keys: LOCK_STATE, DIR_STATE, ARMED_STATE, FINGERPRINT_ENABLED, MOTION_TRIG_ALARM
 *    - Purpose: live state for orchestration and UI: lock/unlock, motor direction sense, FP auth
 *      enabled, motion-triggered alarm latched.
 *
 * 5) Time Base & Sync
 *    - Keys: CURRENT_TIME_SAVED, LAST_TIME_SAVED; Build constants: TIMEOFFSET, NTP_SERVER,
 *      NTP_UPDATE_INTERVAL
 *    - Purpose: Master can decide whether to push time, and how to interpret timestamps coming
 *      from this Slave (offset, sync cadence).
 *
 * 6) Wake Sources & GPIO Map (for sleep/wake policy)
 *    - Pins: WAKE_UP_GPIO_REED_SWITCH, WAKE_UP_GPIO_OPEN_SWITCH, WAKE_UP_GPIO_SHOCK_SENSOR1
 *    - Purpose: Master knows which events can wake the board (door open HIGH via EXT0, button/shock
 *      ALL_LOW via EXT1) and maps them to system behavior.
 *
 * 7) Power/Sleep Policy & Battery Signals
 *    - Build constants: SLEEP_TIMER, TIMER_WAKUP, POWER_MODE_UPDATE, LOW_BATT_TRHESHOLD
 *    - Purpose: Master aligns its timing and expectations (e.g., when the Slave may go to sleep,
 *      how low-battery is flagged).
 *
 * Notes:
 * - All NVS keys are ≤ 15 chars (ESP32 NVS limit).
 * - Everything below is grouped by concern to make audits and diffs painless.
 */

// ============================================================================
//  Includes
// ============================================================================
#include <Arduino.h>
#include "driver/gpio.h"

// ============================================================================
//  Build Mode (Board Role)
//  - IS_SLAVE_ALARM=false  => regular lock slave (motor / lock actuator)
//  - IS_SLAVE_ALARM=true   => alarm-only slave (shock sensor / siren only)
// ============================================================================
#define IS_SLAVE_ALARM false    // 🔒 false = Lock mode | 🚨 true = Alarm mode

// ---------------------------
// Storage helpers
// ---------------------------
#define CONFIG_PARTITION        "config"
#define LOGFILE_PATH            "/Log/log.json"

// ============================================================================
//  Time / Power Behaviour
// ============================================================================
#define TIMEOFFSET              3600          // seconds offset from UTC
#define NTP_SERVER              "pool.ntp.org"
#define NTP_UPDATE_INTERVAL     60000         // ms between NTP sync checks

#define SLEEP_TIMER             240000        // ms before app puts device to sleep
#define TIMER_WAKUP             60000000      // us for esp_sleep timer wakeup (if used)
#ifndef POWER_MODE_UPDATE
#define POWER_MODE_UPDATE       30000       // ms between power mode eval (30)
#endif

// ============================================================================
//  Hardware Pin Map (grouped by function for quick audits)
// ============================================================================

// ---------------------------
// Power / Fuel Gauge / Charger
// ---------------------------
// MAX17055 fuel gauge (I2C)
#define MAX17055_SDA_PIN        4
#define MAX17055_SCL_PIN        5
#define MAX17055_ALRT_PIN       6
#define LOW_BATT_TRHESHOLD      20   // % battery considered "low"
// Charger status (MCP73831-2ACI/MC)
#define CHARGE_STATUS_PIN       11

// ---------------------------
// User / Boot buttons
// ---------------------------
#define BOOT_BUTTON_PIN         0
#define USER_BUTTON_PIN         40

// ---------------------------
// Shock sensor / motion
// ---------------------------
#define SHOCK_SENSOR1_PIN               12
#define WAKE_UP_GPIO_SHOCK_SENSOR1      GPIO_NUM_12

// Internal shock sensor (LIS2DHTR) pin map
// - SDA/SCL share the same I2C bus as the MAX17055 fuel gauge
// - INT uses the same GPIO as the external shock sensor input
#define LIS2DHTR_SDA_PIN                MAX17055_SDA_PIN
#define LIS2DHTR_SCL_PIN                MAX17055_SCL_PIN
#define LIS2DHTR_INT_PIN                SHOCK_SENSOR1_PIN

// ---------------------------
// Mechanical end-of-travel (screw lock only)
//   END01_OF_ROAD_PIN -> "unlock" end
//   END02_OF_ROAD_PIN -> "lock" end
// ---------------------------
#define END01_OF_ROAD_PIN       13   // unlock position detect
#define END02_OF_ROAD_PIN       14   // lock position detect

// ---------------------------
// Door / access sensors
// ---------------------------
// Reed switch = magnetic door sensor
#define REED_SWITCH_PIN                 15
#define WAKE_UP_GPIO_REED_SWITCH        GPIO_NUM_15
// OPEN switch = physical "open door" / "request exit" button
#define OPEN_SWITCH_PIN                 16
#define WAKE_UP_GPIO_OPEN_SWITCH        GPIO_NUM_16

// ---------------------------
// Status / indicator LEDs (RGB PWM)
// ---------------------------
#define DATA_FLAG_LED_PIN       17   // e.g. blue
#define LOWBAT_LED_PIN          2    // red
#define BLE_FLAG_LED_PIN        1    // green

#define PWM_FREQ                2000 // 2 kHz
#define PWM_RES                 8    // 8-bit (0..255)
// LEDC channels for RGB
#define RED_CH                  0
#define GREEN_CH                1
#define BLUE_CH                 2

// ---------------------------
// Fingerprint Sensor (R503) UART
// ---------------------------
// NOTE: R503_PW is sensor power/enable pin.
#define R503_RX_PIN             10
#define R503_TX_PIN             9
#define R503_PW                 21
#define R503_BAUD_RATE          57600

// ---------------------------
// Motor Driver (TMI8340 / H-bridge / etc.)
// We drive these pins HIGH/LOW to lock/unlock the screw,
// OR energize the electromagnet for a pulse.
// ---------------------------
#define MOTOR_IN01_PIN          8
#define MOTOR_IN02_PIN          18
#define ROT_DURATION            10000    // legacy rotation duration (ms)

// ============================================================================
//  RTOS Task Configuration (stacks, priorities, core assignment)
// ============================================================================
#define CORE_0                  0
#define CORE_1                  1


// Motor lock/unlock tasks
#define LOCK_TASK_STACK_SIZE            4096
#define LOCK_TASK_CORE                  CORE_1
#define LOCK_TASK_PRIORITY              2   // higher priority for motor action

// Ack / radio notify after lock/unlock
#define LOCK_ACK_TASK_STACK_SIZE        4096
#define LOCK_ACK_TASK_CORE              CORE_0
#define LOCK_ACK_TASK_PRIORITY          1

// Door status monitor (reed/open/etc.)
#define DOOR_STATUS_TASK_STACK_SIZE     4096
#define DOOR_STATUS_TASK_CORE           CORE_0
#define DOOR_STATUS_TASK_PRIORITY       1

// Input monitor (buttons, shock, etc.)
#define INPUT_MONITOR_TASK_STACK_SIZE   4096
#define INPUT_MONITOR_TASK_CORE         CORE_0
#define INPUT_MONITOR_TASK_PRIORITY     1

// ============================================================================
//  Feature Toggles / Debug
// ============================================================================
// If false, PowerManager reports a fixed SOC and treats the gauge as online.
#define USE_MAX17048 false
#define FAKE_SOC_PERCENT 80
#define FAKE_BATTERY_VOLTAGE_V 3.9f


// ============================================================================
//  ESP-NOW / Radio Config
// ============================================================================
#define PREER_CHANNEL           0

#endif // CONFIG_H
