/**************************************************************
 *  Author      : Tshibangu Samuel
 *  Role        : Freelance Embedded Systems Engineer
 *  Expertise   : Secure IoT Systems, Embedded C++, RTOS, Control Logic
 *  Contact     : tshibsamuel47@gmail.com
 *  Portfolio   : https://www.freelancer.com/u/tshibsamuel477
 *  Phone       : +216 54 429 793
 **************************************************************/
#ifndef DEVICE_H
#define DEVICE_H

#include <Arduino.h>
#include <vector>
#include <Config.hpp>
#include <Transport.hpp>

class EspNowManager;
class RTCManager;
class PowerManager;
class SleepTimer;
class MotorDriver;
class ShockSensor;
class SwitchManager;
class Fingerprint;
class TransportManager;

class DeviceHandler;
class FingerprintHandler;
class MotorHandler;
class ShockHandler;
class StubHandler;

#ifndef MAIN_LOOP_DELAY_MS
#define MAIN_LOOP_DELAY_MS            300
#endif
#ifndef OPEN_DEBOUNCE_MS
#define OPEN_DEBOUNCE_MS              100
#endif
#ifndef DRIVER_FAR_ACK_MS
#define DRIVER_FAR_ACK_MS             5000
#endif
#ifndef LOW_BATTERY_PCT
#define LOW_BATTERY_PCT               20
#endif
// Battery band debounce (anti-flicker) and Low/Critical grace period before sleep (ms)
#ifndef BATTERY_BAND_CONFIRM_MS
#define BATTERY_BAND_CONFIRM_MS       15000UL   // e.g. 15s stable before changing band
#endif
#ifndef LOW_CRIT_GRACE_MS
#define LOW_CRIT_GRACE_MS             60000UL   // ~60s grace before enforcing sleep
#endif

class Device {
public:
  Device();
  ~Device();

  void begin();
  void loop();
  // Central entrypoint for reset requests to ensure orderly shutdown.
  void requestReset(bool factoryReset, const char* reason = nullptr);

private:
  // ==== Managers (owned) ====
  RTCManager*      RTC         = nullptr;
  PowerManager*    PowerMgr    = nullptr;
  SleepTimer*      sleepTimer  = nullptr;
  MotorDriver*     motorDriver = nullptr;
  ShockSensor*     shockSensor = nullptr;
  EspNowManager*   Now         = nullptr;
  SwitchManager*   Sw          = nullptr;
  Fingerprint*     Fing        = nullptr;
  TransportManager* Transport  = nullptr;
  DeviceHandler*   DevHandler  = nullptr;
  FingerprintHandler* FpHandler = nullptr;
  MotorHandler*    MotorH      = nullptr;
  ShockHandler*    ShockH      = nullptr;
  const bool       isAlarmRole_ = IS_SLAVE_ALARM;

  // ==== Cached states / edges ====
  bool prevConfigured   = false;
  bool prevArmed        = false;  // ARMED_STATE
  bool prevLocked       = true;
  bool prevDoorOpen     = false;
  bool prevMotorMoving  = false;

  bool prevCriticalOverlay = false;
  bool lowPowerCancelLatched_ = false;
  bool configModeActive_      = false;
  bool sleepPending_          = false;
  // Battery band / grace tracking: 0=Good, 1=Low, 2=Critical
  uint8_t  effectiveBand_       = 0;
  uint8_t  pendingBand_         = 0;
  uint32_t bandChangeStartMs_   = 0;
  uint32_t lowCritGraceStartMs_ = 0;

  // ==== Capability snapshot (from Preferences) ====
  void refreshCapabilities_();
  void updateShockSensor_();
  bool hasOpenSwitch_    = true;
  bool hasShock_         = true;
  bool hasReed_          = true;
  bool hasFingerprint_   = true;

  // ==== Post-master-unlock door-cycle tracking (paired+DISARMED) ====
  bool     awaitingDoorCycle_ = false;
  uint32_t unlockEventMs_     = 0;

  // ==== Debounce / rate-limit ====
  uint32_t lastOpenBtnEdgeMs = 0;
  uint32_t lastDriverFarMs   = 0;

  // ==== Reset handling ====
  bool     resetRequested_        = false;
  bool     resetInProgress_       = false;
  bool     factoryResetRequested_ = false;
  uint32_t resetRequestMs_        = 0;
  String   resetReason_;

  // ==== Internal helpers ====
  void initManagers_();
  void updateConfigMode_();

  void guardLowPowerEarly_();
  void enforcePowerPolicy_();
  void pollInputsAndEdges_(bool securityEnabled);
  void handleStateTransitions_(bool configured, bool armed, bool locked,
                               bool doorOpen, bool motorMoving);
  void cmd_LockIfSafeAndAck_(const char* src);
  void cmd_RequestUnlockIfAllowed_(const char* src);
  void raiseBreachIfNeeded_();
  void processResetIfNeeded_();
  void performSafeReset_();
  bool canSleepNow_() const;
  void stopRadio_();
  void stopMotor_();
  void stopFingerprint_();
  void stopTransport_();

  static uint32_t ms_();
  bool isConfigured_() const;
  bool isArmed_() const;
  bool isMotionEnabled_() const;
  bool isLocked_() const;
  bool isDoorOpen_() const;
  bool isMotorMoving_() const;
  void printMACIfUserButton_() const;

  void sendAck_(uint16_t opcode, bool ok=false);
  void sendAck_(uint16_t opcode, const uint8_t* payload, size_t payloadLen, bool ok=false);
  void sendMotionTrig_();
  void requestUnlock_();
  void requestAlarm_();
  void sendTransportEvent_(transport::Module mod, uint8_t op, const std::vector<uint8_t>& payload,
                           transport::MessageType type = transport::MessageType::Event);
  std::vector<uint8_t> buildStatePayload_() const;
  void enterCriticalSleepUnpaired_();

  friend class DeviceHandler;
};

#endif // DEVICE_H
