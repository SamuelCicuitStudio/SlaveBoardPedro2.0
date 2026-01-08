/**************************************************************
 *  Author      : Tshibangu Samuel
 *  Role        : Freelance Embedded Systems Engineer
 *  Expertise   : Secure IoT Systems, Embedded C++, RTOS, Control Logic
 *  Contact     : tshibsamuel47@gmail.com
 *  Portfolio   : https://www.freelancer.com/u/tshibsamuel477
 *  Phone       : +216 54 429 793
 **************************************************************/
#ifndef COMMAND_API_H
#define COMMAND_API_H

/**
 * @file CommandAPI.h
 * @brief Hex-string command vocabulary for master <-> slave over ESP-NOW.
 *
 * FORMAT:
 *  - Every token is a string like "0xA0".
 *  - For payload replies/events, append ":" then the payload (e.g. "0xB1:84").
 *
 * NOTES:
 *  - Targeted to master controller -> 16 slave locks/alarms over ESP-NOW.
 *  - BLE-specific tokens removed; values preserved for remaining commands.
 *  - Gaps in the numeric space are intentional.
 *
 * SECURITY:
 *  - CMD_FP_ADOPT_SENSOR:
 *        master instructs slave to adopt/lock a new virgin sensor
 *        using a secure password.
 *  - CMD_FP_RELEASE_SENSOR:
 *        master instructs slave to release sensor to default password
 *        (0x00000000).
 */

// ============================================================================
// Error / Failure (generic error-class responses, non-ESPNOW app errors)
// ============================================================================

#define ERROR_OPEN_FILE         "0xE1"  // File open error
#define ERROR_DESERIALIZE_JSON  "0xE2"  // JSON deserialization error
#define ERROR_NO_MODIFICATIONS  "0xE3"  // No data actually changed
#define ERROR_SERIAL_PORT_BUSY  "0xE4"  // Serial / UART busy

// ============================================================================
// Pairing bootstrap (plaintext init frame)
// ============================================================================

#define PAIR_INIT_CODE          "PAIR_INIT"

// ============================================================================
// Slave ESP-NOW Commands (master -> slave)
// Foreground control: lock/unlock, motion, config, etc.
// ============================================================================

#define CMD_LOCK_SCREW          "0x01"  // (LOCK) Force lock action (drive motor lock)
#define CMD_UNLOCK_SCREW        "0x02"  // (LOCK) Force unlock action (drive motor unlock)
#define CMD_BATTERY_LEVEL       "0x03"  // (BACKGROUND) Request battery level %
#define CMD_REBOOT              "0x05"  // Reboot slave (no factory reset)
#define CMD_FACTORY_RESET       "0x06"  // Factory reset slave (clear config + unpair; same as remove)
#define CMD_ENABLE_MOTION       "0x07"  // Arm motion/impact alarm
#define CMD_DISABLE_MOTION      "0x08"  // Disarm motion/impact alarm
#define CMD_CONFIG_STATUS       "0x09"  // Query configuration/paired status
#define CMD_ARM_SYSTEM          "0x0A"  // Arm system (separate from motion trigger)
#define CMD_DISARM_SYSTEM       "0x0B"  // Disarm system (separate from motion trigger)
#define CMD_FORCE_LOCK          "0x0C"  // (LOCK) Force lock regardless of state; ignore other commands
#define CMD_FORCE_UNLOCK        "0x0D"  // (LOCK) Release force lock; resume normal command handling
#define CMD_CLEAR_ALARM         "0x0E"  // Clear alarm/breach state and exit alarm mode

// ============================================================================
// Fingerprint Control (master -> slave)  [FOREGROUND]
// ============================================================================

#define CMD_FP_VERIFY_ON        "0x40"  // Start continuous fingerprint verify loop
#define CMD_FP_VERIFY_OFF       "0x41"  // Stop continuous fingerprint verify loop
#define CMD_ENROLL_FINGERPRINT  "0x42"  // Enroll request: "0x42:<slot>"
#define CMD_FP_DELETE_ID        "0x43"  // Delete template id: "0x43:<slot>"
#define CMD_FP_CLEAR_DB         "0x44"  // Wipe all fingerprint templates
#define CMD_FP_QUERY_DB         "0x45"  // Request DB count/capacity
#define CMD_FP_NEXT_ID          "0x46"  // Request next free template slot
#define CMD_FP_ADOPT_SENSOR     "0x47"  // Adopt attached sensor (program secret PW)
#define CMD_FP_RELEASE_SENSOR   "0x48"  // Release sensor (set PW to 0x00000000)

// ============================================================================
// State / Sync / Role / Liveness Commands
//   - Most are FOREGROUND except heartbeat which is BACKGROUND.
// ============================================================================

#define CMD_STATE_QUERY         "0x10"  // Request full state snapshot
#define CMD_HEARTBEAT_REQ       "0x11"  // (BACKGROUND) Heartbeat / liveness
#define CMD_SYNC_REQ            "0x12"  // Force slave to sync state/logs
#define CMD_SET_ROLE            "0x13"  // Set logical role/zone (front/side/rear)
#define CMD_CANCEL_TIMERS       "0x14"  // Cancel pending timers
#define CMD_REMOVE_SLAVE        "0x16"  // Remove/unpair this slave (equivalent to factory reset; ACK_REMOVED)
#define CMD_ENTER_TEST_MODE     "0x17"  // Enter test mode (respond to all master commands)

// ============================================================================
// Capability Control (master -> slave)  [FOREGROUND ADMIN]
// ============================================================================

#define CMD_CAP_OPEN_ON         "0x20"  // Enable Open button capability
#define CMD_CAP_OPEN_OFF        "0x21"  // Disable Open button capability
#define CMD_CAP_SHOCK_ON        "0x22"  // Enable Shock sensor capability
#define CMD_CAP_SHOCK_OFF       "0x23"  // Disable Shock sensor capability
#define CMD_CAP_REED_ON         "0x24"  // Enable Reed switch capability
#define CMD_CAP_REED_OFF        "0x25"  // Disable Reed switch capability
#define CMD_CAP_FP_ON           "0x26"  // Enable Fingerprint capability
#define CMD_CAP_FP_OFF          "0x27"  // Disable Fingerprint capability
#define CMD_CAPS_QUERY          "0x28"  // Report current capabilities bitmap

// Lock driver mode (screw vs electromagnet) [FOREGROUND ADMIN]
// These toggle NVS key LOCK_EMAG_KEY via Device NvsWrite.
#define CMD_LOCK_EMAG_ON        "0x29"  // Set lock driver to electromagnet (pulse) mode
#define CMD_LOCK_EMAG_OFF       "0x2A"  // Set lock driver to screw motor (endstop) mode


// ============================================================================
// Acknowledgment Messages (slave -> master)
//   - Responses to master commands.
// ============================================================================

// ---- Door actions ----
#define ACK_LOCKED              "0xA0"  // Lock action complete
#define ACK_UNLOCKED            "0xA1"  // Unlock action complete
#define ACK_FORCE_LOCKED        "0xAA"  // Force lock enabled
#define ACK_FORCE_UNLOCKED      "0xAB"  // Force lock released

// ---- Config / pairing ----
#define ACK_PAIR_INIT           "0xA2"  // Pair init frame received
#define ACK_CONFIGURED          "0xA4"  // Device configured/paired
#define ACK_NOT_CONFIGURED      "0xA5"  // Device NOT configured/paired

// ---- Lock driver mode ----
#define ACK_LOCK_EMAG_ON        "0xA8"  // Lock mode set to electromagnet
#define ACK_LOCK_EMAG_OFF       "0xA9"  // Lock mode set to screw motor

// ---- Policy / power / motion responses ----
#define ACK_REBOOT              "0xB8"  // Reboot in progress
#define ACK_FACTORY_RESET       "0xBF"  // Factory reset in progress
#define ACK_ALARM_CLEARED       "0xB7"  // Alarm/breach state cleared by CMD_CLEAR_ALARM
#define ACK_LOCK_CANCELED       "0xBA"  // Lock/unlock canceled due to policy
#define ACK_ALARM_ONLY_MODE     "0xBB"  // Alarm-only mode (no motor drive)
#define ACK_DRIVER_FAR          "0xBC"  // Driver/fob too far to authorize

// ---------------------- Enrollment Lifecycle Replies ------------------------

#define ACK_FP_ENROLL_START     "0xC2"  // Enrollment started for slot
#define ACK_FP_ENROLL_CAP1      "0xC3"  // First capture done
#define ACK_FP_ENROLL_LIFT      "0xC4"  // Ask user to lift finger
#define ACK_FP_ENROLL_CAP2      "0xC5"  // Second capture done
#define ACK_FP_ENROLL_STORING   "0xC6"  // Building model + storing
#define ACK_FP_ENROLL_OK        "0xC7"  // Enrollment success
#define ACK_FP_ENROLL_FAIL      "0xC8"  // Enrollment failed
#define ACK_FP_ENROLL_TIMEOUT   "0xC9"  // Enrollment timeout

// ------------------- Busy / Presence / DB Info Replies ----------------------

#define ACK_FP_BUSY             "0xCA"  // FP busy (enroll or verify running)
#define ACK_FP_NO_SENSOR        "0xCB"  // No / bad / missing fingerprint sensor
#define ACK_FP_DB_INFO          "0xCC"  // DB info: "0xCC:<count>/<cap>"
#define ACK_FP_ID_DELETED       "0xCD"  // Delete model OK: "0xCD:<id>"
#define ACK_FP_DB_CLEARED       "0xCE"  // DB wipe complete
#define ACK_FP_NEXT_ID          "0xCF"  // Next free slot: "0xCF:<id>"

// ------------------ Adoption / Release Result Replies -----------------------

#define ACK_FP_ADOPT_OK         "0xD0"  // Sensor adopted & locked with secret PW
#define ACK_FP_ADOPT_FAIL       "0xD1"  // Adopt failed (foreign/tampered/bad PW)
#define ACK_FP_RELEASE_OK       "0xD2"  // Sensor released to default PW
#define ACK_FP_RELEASE_FAIL     "0xD3"  // Release failed (write PW error)

// ------------------------- Verify Control Replies ---------------------------

#define ACK_FP_VERIFY_ON        "0xD4"  // Verify loop started
#define ACK_FP_VERIFY_OFF       "0xD5"  // Verify loop stopped

// ---------------------- General State / Error Replies -----------------------

#define ACK_STATE               "0x90"  // Structured state dump follows
#define ACK_HEARTBEAT           "0x91"  // Heartbeat reply
#define ACK_ROLE                "0x92"  // Role/zone ACK
#define ACK_SYNCED              "0x93"  // Sync complete / journal sent
#define ACK_TMR_CANCELLED       "0x94"  // Timers cancelled
#define ACK_ARMED               "0x95"  // System armed
#define ACK_DISARMED            "0x96"  // System disarmed
#define ACK_ERR_TOKEN           "0x9A"  // Bad / foreign FP sensor token (tamper)
#define ACK_ERR_MAC             "0x9B"  // MAC check failed (not allowed sender)
#define ACK_ERR_POLICY          "0x9C"  // Policy / ruleset denies request
#define ACK_UNINTENDED          "0x9D"  // Request not allowed in this context
#define ACK_TEST_MODE           "0x9F"  // Test mode entered

// ---- Capability Replies (slave -> master) ----
#define ACK_CAP_SET             "0xAD"  // Capability updated OK
#define ACK_CAPS                "0xAE"  // Capabilities dump (bitmap/payload) "0xAE:O{0|1}S{0|1}R{0|1}F{0|1}"
#define ACK_REMOVED             "0xAF"  // Slave removed/unpaired

// ============================================================================
// Event Messages (slave -> master)
//   - Initiated by the slave (button, sensors, live status).
// ============================================================================

// ---- Door edges after MASTER unlock while DISARMED (non-breach) ----
#define EVT_UNL_OPN             "0xA6"  // Door opened after unlock (disarmed)
#define EVT_UNL_CLS             "0xA7"  // Door closed after unlock (disarmed)

// ---- Battery / power / motion / alarm ----
#define EVT_BATTERY_PREFIX      "0xB1"  // Battery report: "0xB1:<pct>"
#define EVT_LWBT                "0xB2"  // Low-battery event
#define EVT_HGBT                "0xB3"  // High/good battery event
#define EVT_MTRTTRG             "0xB4"  // Motor trigger detected
#define EVT_MTALRSET            "0xB5"  // Alarm armed / set
#define EVT_MTALRRESET          "0xB6"  // Alarm cleared / reset by sensor logic
#define EVT_REED                "0xB9"  // Reed switch state (door open/closed): "0xB9:<0|1>"
#define EVT_SLEEP_PENDING       "0xBD"  // Sleep pending (Low/Critical band, grace over but not yet safe)
#define EVT_SLEEP_PENDING_CLEAR "0xBE"  // Sleep pending cancelled (band back to Good or now safe)

// ---- Alarm clear acknowledgment event (explicit) ----
#define EVT_ALARM_CLEARED       "0xB0"  // Alarm/breach cleared after CMD_CLEAR_ALARM

// ---- Intrusion / critical ----
#define EVT_BREACH              "0x97"  // Intrusion / tamper / forced door
#define EVT_CRITICAL            "0x98"  // Critical state (battery/HW fault/etc.)

// ------------------------- Fingerprint Match Report --------------------------
// Payloads used heavily by UI / master for FP flows.

#define EVT_FP_MATCH            "0xC0"  // Match event: "0xC0:<id>,<conf>"
#define EVT_FP_FAIL             "0xC1"  // Verify fail event / bad read

// ---------------------- Generic event marker ----------------------
#define EVT_GENERIC             "0x9E"  // Generic event / log marker (e.g., open button pressed)

#endif // COMMAND_API_H
