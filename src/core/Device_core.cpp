#include <Device.hpp>
#include <ConfigNvs.hpp>
#include <ESPNOWManager.hpp>
#include <NVSManager.hpp>
#include <PowerManager.hpp>
#include <RGBLed.hpp>
#include <ResetManager.hpp>
#include <SleepTimer.hpp>
#include <SwitchManager.hpp>
#include <TransportManager.hpp>
#include <Utils.hpp>

// =========================
// Construction / teardown
// =========================
Device::Device() {}
Device::~Device() {}

// =========================
// begin()
// =========================
void Device::begin() {
  initManagers_();
  ResetManager::Init(this);

  // Early battery policy before entering main loop.
  guardLowPowerEarly_();
  if (RGB) RGB->setDeviceState(DeviceState::INIT);

  // Snapshot HAS_* (loop() will use these gates)
  refreshCapabilities_();
  DBG_PRINTF("[Caps] role=%s O%d S%d R%d F%d\n",
               isAlarmRole_ ? "ALARM" : "LOCK",
               hasOpenSwitch_ ? 1 : 0,
               hasShock_ ? 1 : 0,
               hasReed_ ? 1 : 0,
               hasFingerprint_ ? 1 : 0);

  // Pairing banner (local log)
  if (CONF) {
    const bool configured = CONF->GetBool(DEVICE_CONFIGURED, false);
    DBGSTR();
    DBG_PRINTLN("###########################################################");
    if (configured) {
      String master = CONF->GetString(MASTER_ESPNOW_ID, MASTER_ESPNOW_ID_DEFAULT);
      DBG_PRINTLN(
        (master.isEmpty() || master == MASTER_ESPNOW_ID_DEFAULT)
        ? "#        [Pairing]  Configured but master ID missing     #"
        : "#               [Pairing]  Paired Successfully          #"
      );
      DBG_PRINTLN("#               Master ID: " + master + "              #");
    } else {
      DBG_PRINTLN("#         [Pairing]  Not Configured (Unpaired)          #");
      DBG_PRINTLN("#      Waiting for INIT from master to start pairing...    #");
    }
    DBG_PRINTLN("###########################################################");
    DBGSTP();
    // Initial LED state by pairing
    if (RGB && !configured) {
      RGB->setDeviceState( DeviceState::PAIRING);
    }else if(RGB && configured){
      RGB->setDeviceState( DeviceState::READY_ONLINE);
    }
  }

  // Initialize cached states for transitions
  prevConfigured       = isConfigured_();
  prevArmed            = isArmed_();
  prevLocked           = isLocked_();
  prevDoorOpen         = hasReed_ ? isDoorOpen_() : false;
  prevMotorMoving      = isMotorMoving_();
  prevCriticalOverlay  = false;

  DBG_PRINTLN("[Device] begin() complete");
}

// =========================
// loop()
// =========================
void Device::loop() {
  processResetIfNeeded_();
  if (resetInProgress_) return;

  updateConfigMode_();

  // 1) Handle power policy (critical/low -> may sleep immediately).
  enforcePowerPolicy_();

  // 2) Poll inputs and forward events to master.
  const bool securityEnabled = !configModeActive_;
  pollInputsAndEdges_(securityEnabled);

  // 3) Debug MAC print (button-held).
  printMACIfUserButton_();
  if (PowerMgr)   PowerMgr->service();
  if (sleepTimer) {
    if (Now && isConfigured_() && Now->isMasterOnline()) {
      sleepTimer->resetQuiet();
    }
    sleepTimer->service();
  }
  if (Sw)         Sw->service();
  if (Transport)  Transport->tick();

  if (resetRequested_) {
    processResetIfNeeded_();
  }
}
