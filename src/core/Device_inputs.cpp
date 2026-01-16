#include <Device.hpp>
#include <CommandAPI.hpp>
#include <ESPNOWManager.hpp>
#include <MotorDriver.hpp>
#include <PowerManager.hpp>
#include <RGBLed.hpp>
#include <ShockSensor.hpp>
#include <SleepTimer.hpp>
#include <SwitchManager.hpp>
#include <Utils.hpp>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// =========================
// pollInputsAndEdges_()
// =========================
void Device::pollInputsAndEdges_(bool securityEnabled) {
  const bool configured  = isConfigured_();
  const bool armed       = securityEnabled ? isArmed_() : false;  // effective armed (forced false in Config Mode)
  const bool locked      = isLocked_();

  // If no reed, treat as always closed for edge logic (no false triggers)
  const bool doorOpenHw  = isDoorOpen_();
  const bool doorOpen    = hasReed_ ? doorOpenHw : false;

  const bool motorMoving = isMotorMoving_();

  // Handle edges -> LEDs + master ACKs + flow tracking
  handleStateTransitions_(configured, armed, locked, doorOpen, motorMoving);

  // Shock / motion tamper (only if sensor exists and motion is enabled)
  // - Unpaired: still detect and log locally for debugging, but NO transport events.
  // - Paired: always emit Shock Trigger; AlarmRequest(reason=shock) only when
  //           "effective armed" is true (armed==true here).
  const bool motionEnabled = isMotionEnabled_();
  if (hasShock_ && shockSensor && motionEnabled && !motorMoving && shockSensor->isTriggered()) {
    if (!configured) {
      DBG_PRINTLN("[Device] Shock/motion detected (UNPAIRED bench mode)");
      if (RGB) RGB->postOverlay(OverlayEvent::SHOCK_DETECTED);
      if (sleepTimer) sleepTimer->reset();
    } else {
      if (RGB) RGB->postOverlay(OverlayEvent::SHOCK_DETECTED);
      sendMotionTrig_();               // Now->SendMotionTrigg()
      sendTransportEvent_(transport::Module::Shock, /*op*/0x03, {uint8_t(transport::StatusCode::OK)});
      if (armed) {
        // Only when effectively armed (Config Mode forces armed=false).
        sendTransportEvent_(transport::Module::Device, /*op*/0x0F, {1}); // AlarmRequest reason=shock
      }
      if (sleepTimer) sleepTimer->reset();
      DBG_PRINTLN("[Device] trigger -> motion alert sent to master");
    }
  }

  // "Driver far" nag (paired+armed, door still open while unlocked)
  if (!isAlarmRole_ && configured && armed && doorOpen && !locked) {
    const uint32_t now = ms_();
    if ((now - lastDriverFarMs) >= DRIVER_FAR_ACK_MS) {
      sendAck_(ACK_DRIVER_FAR, true);
      lastDriverFarMs = now;
      sendTransportEvent_(transport::Module::Device, /*op*/0x10, {});
    }
  }

  // Physical OPEN button (only if present)
  if (hasOpenSwitch_) {
    const bool openBtnNow = (Sw ? Sw->isOpenButtonPressed() : false);
    static bool openBtnPrev = false;
    if (openBtnNow && !openBtnPrev) {
      if ((ms_() - lastOpenBtnEdgeMs) >= OPEN_DEBOUNCE_MS) {
        if (configured && armed) {
          DBG_PRINTLN("[OpenButton] pressed while armed -> report to master only");
          lastOpenBtnEdgeMs = ms_();

          // Still report the press and unlock attempt so the master can log/deny.
          requestUnlock_();
          sendTransportEvent_(transport::Module::SwitchReed, /*op*/0x02, {});
          sendTransportEvent_(transport::Module::Device,     /*op*/0x0E, {});

          const bool criticalNow = (PowerMgr && (PowerMgr->getPowerMode() == CRITICAL_POWER_MODE));
          if (criticalNow) {
            vTaskDelay(pdMS_TO_TICKS(200));   // allow TX window
            if (sleepTimer) sleepTimer->goToSleep(); // does not return
          } else {
            if (sleepTimer) sleepTimer->reset();
          }
        } else {
          const bool criticalNow = (PowerMgr && (PowerMgr->getPowerMode() == CRITICAL_POWER_MODE));
          const int  battPct     = PowerMgr ? PowerMgr->getBatteryPercentage() : 100;
          const bool lowNow      = (!criticalNow && battPct < LOW_BATTERY_PCT);

          if (!configured && !isAlarmRole_ && motorDriver && !lowNow && !criticalNow) {
            // Unpaired bench mode, Lock role only: local motor control, no transport.
            // Treat button as "open/unlock" request.
            DBG_PRINTLN("[OpenButton] Unpaired bench mode -> local unlock task");
            motorDriver->startUnlockTask();
            if (sleepTimer) sleepTimer->reset();
          } else {
            // Paired path: request unlock from master, never local motor.
            cmd_RequestUnlockIfAllowed_("OpenButton");
            // Transport: OpenRequest event
            sendTransportEvent_(transport::Module::SwitchReed, /*op*/0x02, {});
            sendTransportEvent_(transport::Module::Device, /*op*/0x0E, {});

            if (criticalNow && configured) {
              vTaskDelay(pdMS_TO_TICKS(200));   // allow TX window
              if (sleepTimer) sleepTimer->goToSleep(); // does not return
            } else {
              if (sleepTimer) sleepTimer->reset();
            }
          }

          lastOpenBtnEdgeMs = ms_();
        }
      }
    }
    openBtnPrev = openBtnNow;
  }

  // Breach handling (paired+armed)
  if (configured && armed) {
    raiseBreachIfNeeded_();
  }
  clearBreachIfClosed_();
}

// =========================
// handleStateTransitions_()
// =========================
void Device::handleStateTransitions_(bool configured, bool armed,
                                     bool locked, bool doorOpen,
                                     bool motorMoving)
{
  // Pairing flip -> LED + refresh HAS_* (config likely changed)
  if (configured != prevConfigured) {

    refreshCapabilities_();
  }

  // Lock state changed -> start DISARMED post-unlock flow if applicable
  if (locked != prevLocked) {
    if (configured && !locked && !armed) {
      awaitingDoorCycle_ = true;
      unlockEventMs_     = ms_();
      DBG_PRINTLN("[Flow] Master unlock (DISARMED) -> awaiting door open/close edges");
    }
  }

  // Door edge (OPEN <-> CLOSED)
  if (doorOpen != prevDoorOpen) {
    if (RGB) {
      RGB->postOverlay(doorOpen ? OverlayEvent::DOOR_OPEN
                                : OverlayEvent::DOOR_CLOSED);
    }

    // Report to master (paired only)
    if (configured) {
      // Transport: DoorEdge event
      std::vector<uint8_t> plEdge{ static_cast<uint8_t>(doorOpen) };
      sendTransportEvent_(transport::Module::SwitchReed, /*op*/0x01, plEdge);

      // Transport: StateReport event
      sendTransportEvent_(transport::Module::Device, /*op*/0x09, buildStatePayload_());

      if (doorOpen) {
        // Generic state update
        sendAck_(ACK_UNLOCKED, true);
        // Extra signal in the DISARMED post-unlock flow
        if (awaitingDoorCycle_ && !armed && !locked) {
          sendAck_(EVT_UNL_OPN, true);
          DBG_PRINTLN("[Flow] UNOPN sent (after master unlock, disarmed)");
        }
      } else {
        sendAck_(ACK_LOCKED, true);
        // Complete the DISARMED post-unlock flow
        if (awaitingDoorCycle_ && !armed) {
          sendAck_(EVT_UNL_CLS, true);
          awaitingDoorCycle_ = false;
          DBG_PRINTLN("[Flow] UNCLS sent (cycle complete)");
        }
      }
    }
  }

  // Motor completion -> emit MotorDone event (Lock role only, paired)
  if (configured && !isAlarmRole_ && motorDriver &&
      prevMotorMoving && !motorMoving) {
    uint8_t status = static_cast<uint8_t>(transport::StatusCode::OK);
    uint8_t lockedByte = locked ? 1 : 0;
    sendTransportEvent_(transport::Module::Motor, /*op*/0x05,
                        {status, lockedByte});
  }

  prevConfigured  = configured;
  prevArmed       = armed;
  prevLocked      = locked;
  prevDoorOpen    = doorOpen;
  prevMotorMoving = motorMoving;
}

// =========================
// Actions / reporting
// =========================
void Device::cmd_LockIfSafeAndAck_(const char* src) {
  if (isAlarmRole_) {
    DBG_PRINTLN(String("[Action-IGNORED] ") + src + " (alarm-only role; no motor)");
    return;
  }
  if (!motorDriver) return;

  if (!isConfigured_()) {
    if (RGB) RGB->postOverlay(OverlayEvent::LOCKING);
    DBG_PRINTLN(String("[Action-LOCAL] ") + src + " -> startLockTask()");
    motorDriver->startLockTask();
    sendTransportEvent_(transport::Module::Device, /*op*/0x05, {uint8_t(transport::StatusCode::OK), 1});
    return;
  }

  DBG_PRINTLN(String("[Action-BLOCKED] ") + src + " (paired) -> master lock request only");

  const bool criticalNow = (PowerMgr && (PowerMgr->getPowerMode() == CRITICAL_POWER_MODE));
  sendTransportEvent_(transport::Module::Device, /*op*/0x11, {static_cast<uint8_t>(criticalNow)});
  sendTransportEvent_(transport::Module::Device, /*op*/0x12, {static_cast<uint8_t>(criticalNow)});
}

void Device::cmd_RequestUnlockIfAllowed_(const char* src) {
  if (!isConfigured_()) {
    DBG_PRINTLN(String("[Action-LOCAL] ") + src + " ignored (unpaired; no master)");
    return;
  }
  DBG_PRINTLN(String("[Action] ") + src + " -> RequestUnlock() (auth request to master)");
  requestUnlock_();
}

// =========================
// Breach handling
// =========================
void Device::raiseBreachIfNeeded_() {
  if (configModeActive_) return;
  if (!isConfigured_() || !Now) return;
  if (!isArmed_())   return;

  // Effective door state (respect hasReed_ gating used elsewhere)
  const bool doorOpenHw = isDoorOpen_();
  const bool doorOpen   = hasReed_ ? doorOpenHw : false;

  // Breach rule (both roles): Armed & LOCK_STATE=locked & reed->open.
  const bool locked = isLocked_();
  const bool breachCondition = locked && doorOpen;

  if (breachCondition && !Now->breach) {
    DBG_PRINTLN("[Breach] Door opened while supposed to be locked -> report to master");
    if (RGB) RGB->postOverlay(OverlayEvent::BREACH);
    requestAlarm_();            // Now->RequesAlarm()
    sendTransportEvent_(transport::Module::Device, /*op*/0x0F, {0}); // reason=breach
    sendTransportEvent_(transport::Module::Device, /*op*/0x13, {1}); // breach set
    Now->breach = true;
    if (sleepTimer) sleepTimer->reset();
  }
}
void Device::clearBreachIfClosed_() {
  if (!Now || !Now->breach) return;
  // Use the same effective door state as elsewhere (hasReed_ gating)
  const bool doorOpenHw = isDoorOpen_();
  const bool doorOpen   = hasReed_ ? doorOpenHw : false;
  if (!doorOpen) {
    Now->breach = false;
    sendTransportEvent_(transport::Module::Device, /*op*/0x13, {0}); // breach clear
    DBG_PRINTLN("[Breach] cleared on door close");
  }
}
