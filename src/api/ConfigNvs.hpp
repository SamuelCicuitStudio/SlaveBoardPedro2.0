/**************************************************************
 *  Author      : Tshibangu Samuel
 *  Role        : Freelance Embedded Systems Engineer
 *  Expertise   : Secure IoT Systems, Embedded C++, RTOS, Control Logic
 *  Contact     : tshibsamuel47@gmail.com
 *  Portfolio   : https://www.freelancer.com/u/tshibsamuel477
 *  Phone       : +216 54 429 793
 **************************************************************/
#ifndef CONFIG_NVS_H
#define CONFIG_NVS_H

// ============================================================================
//  NVS KEYS (Preferences) + Defaults
//  Groups: Identity/Pairing, Runtime State, Lock Driver Config, HW Presence
//  NOTE: keep key strings <=15 chars (ESP32 NVS limitation).
// ============================================================================

// ---------------------------
// Identity / pairing / auth
// ---------------------------
#define DEVICE_NAME             "DEVNM"   // string : Human-readable name
#define DEVICE_ID               "DEVID"   // string : Short ID / logical slot
#define MASTER_ESPNOW_ID        "MSTNW"   // string : MAC of paired master
#define DEVICE_CONFIGURED       "CFGED"   // bool   : true once paired / fully set
#define RESET_FLAG              "RSTFL"   // bool   : request factory reset
#define MASTER_LMK_KEY          "MLMK"   // string : master link key (hex)

#define DEVICE_NAME_DEFAULT         "LOCK"
#define DEVICE_ID_DEFAULT           "LCK_"
#define MASTER_ESPNOW_ID_DEFAULT    "00:00:00:00:00:00"
#define DEVICE_CONFIGURED_DEFAULT   false
#define RESET_FLAG_DEFAULT          true
#define MASTER_LMK_DEFAULT          ""

// Pairing bootstrap channel (stored in NVS)
#define MASTER_CHANNEL_KEY          "MCH"
#define MASTER_CHANNEL_DEFAULT      0

// ---------------------------
// Runtime lock / security state
// ---------------------------
#define LOCK_STATE              "LCKST"   // bool   : true=locked, false=unlocked
#define DIR_STATE               "DIRST"   // bool   : motor direction sense
#define ARMED_STATE             "ARMED"   // bool   : system armed/disarmed
#define FINGERPRINT_ENABLED     "FPENA"   // bool   : enable / disable FP auth
#define MOTION_TRIG_ALARM       "MOTAL"   // bool   : motion/shock trigger enabled
#define CURRENT_TIME_SAVED      "CAVTT"   // uint32 : last synced unix time
#define LAST_TIME_SAVED         "LSTPD"   // uint32 : previous synced unix time

#define LOCK_STATE_DEFAULT          true
#define DIR_STATE_DEFAULT           true
#define ARMED_STATE_DEFAULT         false
#define FINGERPRINT_ENABLED_DEFAULT false
#define MOTION_TRIG_ALARM_DEFAULT   false
#define DEFAULT_CURRENT_TIME_SAVED  1736121600
#define DEFAULT_LAST_TIME_SAVED     1736121600

// ---------------------------
// Shock sensor configuration
// ---------------------------
// Which shock sensor implementation is used on this board.
// - EXTERNAL: current GPIO-based shock sensor input
// - INTERNAL: LIS2DHTR over I2C (shares SDA/SCL with MAX17055), INT on SHOCK_SENSOR1_PIN
#define SHOCK_SENSOR_TYPE_KEY         "SHTYP"  // int : 0=external, 1=internal
#define SHOCK_SENSOR_TYPE_EXTERNAL    0
#define SHOCK_SENSOR_TYPE_INTERNAL    1
#define SHOCK_SENSOR_TYPE_DEFAULT     SHOCK_SENSOR_TYPE_EXTERNAL

// Sensitivity threshold used by the selected shock sensor implementation
// (units depend on the sensor/driver; interpreted later when integration is added).
#define SHOCK_SENS_THRESHOLD_KEY      "SHTHR"  // int : sensitivity threshold
#define SHOCK_SENS_THRESHOLD_DEFAULT  16

// LIS2DHTR (internal shock) configuration defaults.
// Values map to l2d_* enums and register fields (see src/sensors/l2d_types.h).
#define SHOCK_L2D_ODR_KEY             "SHODR"  // int : l2d_odr_t (0..9)
#define SHOCK_L2D_ODR_DEFAULT         5        // L2D_ODR_100
#define SHOCK_L2D_SCALE_KEY           "SHSCL"  // int : l2d_scale_t (0..3)
#define SHOCK_L2D_SCALE_DEFAULT       0        // L2D_SCALE_2G
#define SHOCK_L2D_RES_KEY             "SHRES"  // int : l2d_res_t (0..2)
#define SHOCK_L2D_RES_DEFAULT         2        // L2D_RES_H
#define SHOCK_L2D_EVT_MODE_KEY        "SHEVT"  // int : l2d_evt_mode_t (0..5)
#define SHOCK_L2D_EVT_MODE_DEFAULT    0        // L2D_EVT_WAKE
#define SHOCK_L2D_DUR_KEY             "SHDUR"  // int : INT1 duration (0..127)
#define SHOCK_L2D_DUR_DEFAULT         2
#define SHOCK_L2D_AXIS_KEY            "SHAX"   // int : axis mask (bit0=XL,1=XH,2=YL,3=YH,4=ZL,5=ZH)
#define SHOCK_L2D_AXIS_DEFAULT        0x2A     // XH|YH|ZH
#define SHOCK_L2D_HPF_MODE_KEY        "SHHPM"  // int : l2d_hpf_t (0..3)
#define SHOCK_L2D_HPF_MODE_DEFAULT    0        // L2D_HPF_NORM
#define SHOCK_L2D_HPF_CUT_KEY         "SHHPC"  // int : HPF cutoff (0..3)
#define SHOCK_L2D_HPF_CUT_DEFAULT     0
#define SHOCK_L2D_HPF_EN_KEY          "SHHPE"  // bool: enable HPF on INT1
#define SHOCK_L2D_HPF_EN_DEFAULT      true
#define SHOCK_L2D_LATCH_KEY           "SHLAT"  // bool: latch INT1
#define SHOCK_L2D_LATCH_DEFAULT       true
#define SHOCK_L2D_INT_LVL_KEY         "SHILV"  // int : 0=active high, 1=active low
#define SHOCK_L2D_INT_LVL_DEFAULT     1

// ---------------------------
// Lock driver configuration
// ---------------------------
// Defines how the physical actuator is driven on this Slave.
#define LOCK_TIMEOUT_KEY        "LCKTMT"  // uint32 : ms runtime / pulse width
#define LOCK_TIMEOUT_DEFAULT    8000      // default 8s safety cut-off / EM pulse

#define LOCK_EMAG_KEY           "LKEMAG"  // bool   : true = electromagnet lock mode
#define LOCK_EMAG_DEFAULT       false     // false = screw motor with endstops,
                                          // true  = electromagnet / no endstop

// ---------------------------
// Hardware presence map
// ---------------------------
// Tells the firmware which peripherals are actually populated on THIS board.
#define HAS_OPEN_SWITCH_KEY     "HWOPSW" // bool: board has OPEN switch input
#define HAS_SHOCK_SENSOR_KEY    "HWSHCK" // bool: board has shock sensor
#define HAS_REED_SWITCH_KEY     "HWREED" // bool: board has reed/door sensor
#define HAS_FINGERPRINT_KEY     "HWFP"   // bool: board has fingerprint sensor

#define HAS_OPEN_SWITCH_DEFAULT     true
#define HAS_SHOCK_SENSOR_DEFAULT    true
#define HAS_REED_SWITCH_DEFAULT     true
#define HAS_FINGERPRINT_DEFAULT     false

// ---------------------------
// Fingerprint device setup flag
// ---------------------------
#define FP_DEVICE_CONFIGURED_KEY     "FPDEV"
#define FP_DEVICE_CONFIGURED_DEFAULT false

// ============================================================================
//  (Optional) Compile-time sanity checks for NVS key lengths
// ============================================================================
#ifdef __cplusplus
  #define NVS_KEYLEN_OK(KEYSYM) static_assert(sizeof(KEYSYM) - 1 <= 6, "NVS key too long: " #KEYSYM)
  NVS_KEYLEN_OK(DEVICE_NAME);
  NVS_KEYLEN_OK(DEVICE_ID);
  NVS_KEYLEN_OK(MASTER_ESPNOW_ID);
  NVS_KEYLEN_OK(DEVICE_CONFIGURED);
  NVS_KEYLEN_OK(RESET_FLAG);
  NVS_KEYLEN_OK(MASTER_LMK_KEY);
  NVS_KEYLEN_OK(MASTER_CHANNEL_KEY);

  NVS_KEYLEN_OK(LOCK_STATE);
  NVS_KEYLEN_OK(DIR_STATE);
  NVS_KEYLEN_OK(ARMED_STATE);
  NVS_KEYLEN_OK(FINGERPRINT_ENABLED);
  NVS_KEYLEN_OK(MOTION_TRIG_ALARM);
  NVS_KEYLEN_OK(CURRENT_TIME_SAVED);
  NVS_KEYLEN_OK(LAST_TIME_SAVED);

  NVS_KEYLEN_OK(SHOCK_SENSOR_TYPE_KEY);
  NVS_KEYLEN_OK(SHOCK_SENS_THRESHOLD_KEY);
  NVS_KEYLEN_OK(SHOCK_L2D_ODR_KEY);
  NVS_KEYLEN_OK(SHOCK_L2D_SCALE_KEY);
  NVS_KEYLEN_OK(SHOCK_L2D_RES_KEY);
  NVS_KEYLEN_OK(SHOCK_L2D_EVT_MODE_KEY);
  NVS_KEYLEN_OK(SHOCK_L2D_DUR_KEY);
  NVS_KEYLEN_OK(SHOCK_L2D_AXIS_KEY);
  NVS_KEYLEN_OK(SHOCK_L2D_HPF_MODE_KEY);
  NVS_KEYLEN_OK(SHOCK_L2D_HPF_CUT_KEY);
  NVS_KEYLEN_OK(SHOCK_L2D_HPF_EN_KEY);
  NVS_KEYLEN_OK(SHOCK_L2D_LATCH_KEY);
  NVS_KEYLEN_OK(SHOCK_L2D_INT_LVL_KEY);

  NVS_KEYLEN_OK(LOCK_TIMEOUT_KEY);
  NVS_KEYLEN_OK(LOCK_EMAG_KEY);

  NVS_KEYLEN_OK(HAS_OPEN_SWITCH_KEY);
  NVS_KEYLEN_OK(HAS_SHOCK_SENSOR_KEY);
  NVS_KEYLEN_OK(HAS_REED_SWITCH_KEY);
  NVS_KEYLEN_OK(HAS_FINGERPRINT_KEY);
  NVS_KEYLEN_OK(FP_DEVICE_CONFIGURED_KEY);
  #undef NVS_KEYLEN_OK
#endif

#endif // CONFIG_NVS_H
