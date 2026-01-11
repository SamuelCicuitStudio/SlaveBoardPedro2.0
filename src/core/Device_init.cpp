#include <Device.hpp>

#include <WiFi.h>
#include <ESPNOWManager.hpp>
#include <FingerprintScanner.hpp>
#include <Logger.hpp>
#include <MotorDriver.hpp>
#include <NVSManager.hpp>
#include <PowerManager.hpp>
#include <RTCManager.hpp>
#include <ShockSensor.hpp>
#include <SleepTimer.hpp>
#include <SwitchManager.hpp>
#include <TransportManager.hpp>
#include <Utils.hpp>

#include <DeviceHandler.hpp>
#include <FingerprintHandler.hpp>
#include <StubHandler.hpp>
#include <MotorHandler.hpp>
#include <ShockHandler.hpp>
#include <Transport.hpp>

// =========================
// initManagers_()
// =========================
void Device::initManagers_() {
  WiFi.mode(WIFI_AP_STA);
   delay(1000);
  // Time / RTC
  struct tm timeInfo{};
  RTCManager::Init(&timeInfo);
  RTC = RTCM;
  if (!RTC) return;

  // Logger uses RTC
  Logger::Init(RTC);
  LOGG->Begin();

  // Power / fuel gauge
  PowerManager::Init();
  PowerMgr = POWERMGR;
  if (!PowerMgr) return;
  PowerMgr->begin();

  // Motor control (skipped in alarm-only role)
  if (!isAlarmRole_) {
    motorDriver = new MotorDriver();
    if (!motorDriver) return;
    motorDriver->begin();
  }

  // Switches, shock, sleep timer
  Sw = new SwitchManager();
  if (Sw) Sw->begin();

  shockSensor = new ShockSensor();

  SleepTimer::Init(RTC, PowerMgr);
  sleepTimer = SleepTimer::Get();
  if (sleepTimer) {
    sleepTimer->reset();
  }

  // Snapshot HAS_* before bringing up FP/radio so we can gate FP begin()
  refreshCapabilities_();

  // 1) Construct Fingerprint (skipped for alarm role or if absent)
  if (!isAlarmRole_) {
    Fing = new Fingerprint(
              motorDriver,
              /*Now=*/nullptr,
              R503_RX_PIN,
              R503_TX_PIN,
              57600
           );
  }

  // 2) Construct ESP-NOW bridge and hand it the FP instance
  Now = new EspNowManager(RTC, PowerMgr, motorDriver, sleepTimer, Fing);
  if (!Now) return;

  // Optional: share Sw with Now (for status)
  Now->sw = Sw;

  // 3) Wire FP -> Now
  if (Fing) {
    Fing->attachEspNow(Now);
  }

  // 4) Init radio AFTER wiring
  Now->init();

  // 4b) Init transport manager (self logical ID = 2 by default)
  Transport = new TransportManager(/*selfId=*/2, Now, CONF);
  if (Transport) {
    Now->attachTransport(Transport);
    DevHandler = new DeviceHandler(this, &Transport->port());
    if (DevHandler) {
      Transport->port().registerHandler(transport::Module::Device, DevHandler);
    }
    // Fingerprint handler (if FP present)
    if (Fing) {
      Fing->attachTransportPort(&Transport->port());
      FpHandler = new FingerprintHandler(Fing, &Transport->port());
      if (FpHandler) {
        Transport->port().registerHandler(transport::Module::Fingerprint, FpHandler);
      }
    }
    // Motor handler (if motor present)
    if (motorDriver && !isAlarmRole_) {
      MotorH = new MotorHandler(motorDriver, CONF, &Transport->port());
      if (MotorH) Transport->port().registerHandler(transport::Module::Motor, MotorH);
    } else {
      auto stub = new StubHandler(&Transport->port());
      if (stub) Transport->port().registerHandler(transport::Module::Motor, stub);
    }
    // Shock handler
    ShockH = new ShockHandler(CONF, &Transport->port(), shockSensor);
    if (ShockH) {
      Transport->port().registerHandler(transport::Module::Shock, ShockH);
    }
  }

  // 5) Start FP only if present
  if (Fing && hasFingerprint_) {
    Fing->begin();
  } else {
    DBG_PRINTLN("[Device] Fingerprint disabled or alarm-only role");
  }
}
