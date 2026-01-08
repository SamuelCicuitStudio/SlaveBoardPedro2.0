#include <Device.hpp>
#include <CommandAPI.hpp>
#include <ConfigNvs.hpp>
#include <NVSManager.hpp>
#include <PowerManager.hpp>
#include <SleepTimer.hpp>
#include <Utils.hpp>
#include <esp_sleep.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// =========================
// guardLowPowerEarly_()
// =========================
void Device::guardLowPowerEarly_() {
  enforcePowerPolicy_();
}

// =========================
// enforcePowerPolicy_()
// =========================
void Device::enforcePowerPolicy_() {
  if (!PowerMgr) return;

  const int  battPct      = PowerMgr->getBatteryPercentage();
  const bool configured   = isConfigured_();
  const bool criticalRaw  = (PowerMgr->getPowerMode() == CRITICAL_POWER_MODE);
  const uint32_t nowMs    = ms_();

  // ------------------------------
  // 1) Determine raw band
  // ------------------------------
  uint8_t rawBand = 0; // 0=Good, 1=Low, 2=Critical
  if (criticalRaw) {
    rawBand = 2;
  } else if (battPct < LOW_BATTERY_PCT) {
    rawBand = 1;
  } else {
    rawBand = 0;
  }

  // ------------------------------
  // 2) Band confirmation (anti-flicker)
  // ------------------------------
  if (rawBand != effectiveBand_) {
    // New candidate band
    if (pendingBand_ != rawBand) {
      pendingBand_       = rawBand;
      bandChangeStartMs_ = nowMs;
    } else if (bandChangeStartMs_ != 0 &&
               (nowMs - bandChangeStartMs_) >= BATTERY_BAND_CONFIRM_MS) {
      // Commit the band change after it has been stable long enough
      effectiveBand_     = rawBand;
      bandChangeStartMs_ = 0;

      // Track grace + reset latches appropriately
      if (effectiveBand_ == 0) {
        // Back to Good: clear grace + latches
        lowCritGraceStartMs_   = 0;
        lowPowerCancelLatched_ = false;
        prevCriticalOverlay    = false;
      } else {
        // Entering Low or Critical
        lowCritGraceStartMs_ = nowMs;
        // Leaving Low or Critical resets latches for the next entry
        if (effectiveBand_ != 1) {
          lowPowerCancelLatched_ = false;
        }
        if (effectiveBand_ != 2) {
          prevCriticalOverlay = false;
        }
      }
    }
  } else {
    // Band stable; clear pending change tracking
    pendingBand_       = rawBand;
    bandChangeStartMs_ = 0;
  }

  // If effective band is Good, nothing else to do here.
  if (effectiveBand_ == 0) {
    if (sleepPending_) {
      // Cancel any pending sleep when band returns to Good.
      sleepPending_ = false;
      sendAck_(EVT_SLEEP_PENDING_CLEAR, true);
    }
    return;
  }

  // Grace window for Low/Critical before enforcing sleep.
  const bool inGrace = (lowCritGraceStartMs_ != 0) &&
                       ((nowMs - lowCritGraceStartMs_) < LOW_CRIT_GRACE_MS);

  // ------------------------------
  // 3) Critical band handling
  // ------------------------------
  if (effectiveBand_ == 2) {
    if (!configured) {
      // Unpaired: wait grace period and ensure it's safe, then deep sleep
      if (inGrace) {
        // Grace active: no sleep pending yet
        if (sleepPending_) {
          sleepPending_ = false;
          sendAck_(EVT_SLEEP_PENDING_CLEAR, true);
        }
        return;
      }
      if (!canSleepNow_()) {
        if (!sleepPending_) {
          sleepPending_ = true;
          sendAck_(EVT_SLEEP_PENDING, true);
        }
        return;
      }
      DBG_PRINTLN("[Power] Critical battery (UNPAIRED) -> deep sleep now");
      enterCriticalSleepUnpaired_(); // does not return
      return;
    }

    // Paired critical path: announce only once per entry into Critical
    if (!prevCriticalOverlay) {
      DBG_PRINTLN("[Power] Critical battery (PAIRED) -> announce + sleep mode");
      if (CONF) CONF->PutBool(MOTION_TRIG_ALARM, false);
      sendAck_(ACK_LOCK_CANCELED,     /*criticalNow=*/true);
      sendAck_(ACK_ALARM_ONLY_MODE,   /*criticalNow=*/true);
      uint8_t pct = (uint8_t)battPct;
      sendTransportEvent_(transport::Module::Device, /*op*/0x11, {1}); // LockCanceled critical
      sendTransportEvent_(transport::Module::Device, /*op*/0x12, {1}); // AlarmOnlyMode critical
      sendTransportEvent_(transport::Module::Device, /*op*/0x14, {pct});
      sendTransportEvent_(transport::Module::Power,  /*op*/0x03, {pct});
    }

    prevCriticalOverlay    = true;
    lowPowerCancelLatched_ = true;

    // During grace window or while not safe, stay awake for diagnostics.
    if (inGrace) {
      if (sleepPending_) {
        sleepPending_ = false;
        sendAck_(EVT_SLEEP_PENDING_CLEAR, true);
      }
      return;
    }
    if (!canSleepNow_()) {
      if (!sleepPending_) {
        sleepPending_ = true;
        sendAck_(EVT_SLEEP_PENDING, true);
      }
      return;
    }

    // We are about to sleep: clear pending and (optionally) signal clear first.
    if (sleepPending_) {
      sleepPending_ = false;
      sendAck_(EVT_SLEEP_PENDING_CLEAR, true);
    }

    if (sleepTimer) {
      sleepTimer->goToSleep();
    } else {
      enterCriticalSleepUnpaired_();
    }
    return;
  }

  // ------------------------------
  // 4) Low band handling (effectiveBand_ == 1)
  // ------------------------------
  if (effectiveBand_ == 1) {
    if (!lowPowerCancelLatched_) {
      DBG_PRINTLN("[Device] Low battery detected (<LOW_BATTERY_PCT%)");
      if (!configured) {
        // Local power save in bench mode
        if (!isAlarmRole_ && CONF) {
          CONF->PutBool(FINGERPRINT_ENABLED, false);
          DBG_PRINTLN("[Device] (UNPAIRED) low battery -> FP disabled (local only)");
        }
      } else {
        const bool criticalFlag = false;
        sendAck_(ACK_LOCK_CANCELED,   criticalFlag);
        sendAck_(ACK_ALARM_ONLY_MODE, criticalFlag);
        sendTransportEvent_(transport::Module::Device, /*op*/0x11, {0});
        sendTransportEvent_(transport::Module::Device, /*op*/0x12, {0});
        sendTransportEvent_(transport::Module::Power,  /*op*/0x02, {uint8_t(battPct)});
      }
    }

    lowPowerCancelLatched_ = true;

    // During grace window or while not safe, stay awake for diagnostics.
    if (inGrace) {
      if (sleepPending_) {
        sleepPending_ = false;
        sendAck_(EVT_SLEEP_PENDING_CLEAR, true);
      }
      return;
    }
    if (!canSleepNow_()) {
      if (!sleepPending_) {
        sleepPending_ = true;
        sendAck_(EVT_SLEEP_PENDING, true);
      }
      return;
    }

    // We are about to sleep: clear pending and (optionally) signal clear first.
    if (sleepPending_) {
      sleepPending_ = false;
      sendAck_(EVT_SLEEP_PENDING_CLEAR, true);
    }

    if (sleepTimer) {
      sleepTimer->goToSleep();
    } else {
      enterCriticalSleepUnpaired_();
    }
    return;
  }
}

bool Device::canSleepNow_() const {
  // Do not enter sleep/deep-sleep while the motor is moving or settling.
  if (isMotorMoving_()) {
    return false;
  }
  return true;
}


// =========================
// enterCriticalSleepUnpaired_()
// =========================
void Device::enterCriticalSleepUnpaired_() {
 DBG_PRINTLN("[Power] entering deep sleep (unpaired, critical battery)");
  delay(50);
  esp_deep_sleep_start();
  while (true) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}
