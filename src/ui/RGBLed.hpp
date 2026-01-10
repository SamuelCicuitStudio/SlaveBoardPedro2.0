/**************************************************************
 *  Author      : Tshibangu Samuel
 *  Role        : Freelance Embedded Systems Engineer
 *  Expertise   : Secure IoT Systems, Embedded C++, RTOS, Control Logic
 *  Contact     : tshibsamuel47@gmail.com
 *  Portfolio   : https://www.freelancer.com/u/tshibsamuel477
 *  Phone       : +216 54 429 793
 **************************************************************/
#ifndef RGBLED_H
#define RGBLED_H

#include <Arduino.h>
#include <RGBConfig.hpp>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

// ---------- Priorities (bigger = stronger) ----------
enum : uint8_t {
  PRIO_BACKGROUND = 0,
  PRIO_ACTION     = 1,
  PRIO_ALERT      = 2,
  PRIO_CRITICAL   = 3
};

// ---------- Patterns ----------
enum class Pattern : uint8_t {
  OFF, SOLID, BLINK, BREATHE, RAINBOW, HEARTBEAT2, FLASH_ONCE,
};

// ---------- Device-facing states (background) ----------
enum class DeviceState : uint8_t { BOOT, INIT, PAIRING, READY_ONLINE, READY_OFFLINE, SLEEP };

// ---------- Overlay events ----------
enum class OverlayEvent : uint8_t {
  LOCKING, BREACH, LOW_BATT, CRITICAL_BATT,
  DOOR_OPEN, DOOR_CLOSED, SHOCK_DETECTED,
  FP_ENROLL_START, FP_ENROLL_LIFT, FP_ENROLL_CAPTURE1, FP_ENROLL_CAPTURE2,
  FP_ENROLL_STORING, FP_ENROLL_OK, FP_ENROLL_FAIL, FP_ENROLL_TIMEOUT
};

// ---------- Pattern options payload ----------
struct PatternOpts {
  uint32_t color      = RGB_HEX(255,255,255);
  uint16_t periodMs   = 300;
  uint16_t onMs       = 100;
  uint32_t durationMs = 0;
  uint8_t  priority   = PRIO_ACTION;
  bool     preempt    = true;
};

class RGBLed {
public:
  // ---------------- Singleton access ----------------
  // Call once at boot to define pins; safe to call again to re-attach pins.
  static void     Init(int pinR, int pinG, int pinB, bool activeLow = true);
  // Always returns a valid pointer (auto-creates with unattached pins if no Init yet).
  static RGBLed*  Get();
  // Returns nullptr if never created (optional early-boot checks).
  static RGBLed*  TryGet();

  // ---------------- Lifecycle ----------------
  // After Init(), call begin() once to start the worker.
  bool begin();
  void end();

  // ---------------- Background state ----------------
  void setDeviceState(DeviceState s);

  // ---------------- Overlay events ----------------
  void postOverlay(OverlayEvent e);

  // ---------------- Direct helpers ----------------
  void off(uint8_t priority = PRIO_ACTION, bool preempt = true);
  void solid(uint32_t color, uint8_t priority = PRIO_ACTION, bool preempt = true, uint32_t durationMs = 0);
  void blink(uint32_t color, uint16_t periodMs, uint8_t priority = PRIO_ACTION, bool preempt = true, uint32_t durationMs = 0);
  void breathe(uint32_t color, uint16_t periodMs, uint8_t priority = PRIO_ACTION, bool preempt = true, uint32_t durationMs = 0);
  void rainbow(uint16_t stepMs = 20, uint8_t priority = PRIO_BACKGROUND, bool preempt = true, uint32_t durationMs = 0);
  void heartbeat(uint32_t color, uint16_t periodMs = 1500, uint8_t priority = PRIO_ACTION, bool preempt = true, uint32_t durationMs = 0);
  void flash(uint32_t color, uint16_t onMs = 120, uint8_t priority = PRIO_ACTION, bool preempt = true);
  void setEnabled(bool enabled);
  bool isEnabled() const;

  // If you want to (re)define pins later:
  void attachPins(int pinR, int pinG, int pinB, bool activeLow = true);
  void playPattern(Pattern pat, const PatternOpts& opts);
private:
  // ----- Internal command wire -----
  enum class CmdType : uint8_t { SET_BACKGROUND, PLAY, STOP, SHUTDOWN };
  struct Cmd {
    CmdType     type;
    DeviceState bgState;
    Pattern     pattern;
    PatternOpts opts;
  };

  static void taskThunk(void* arg);
  void        taskLoop();

  // Low-level I/O
  void writeColor(uint8_t r, uint8_t g, uint8_t b);

  // Helpers
  bool sendCmd(const Cmd& c, TickType_t to = 0);
  void stepRainbow(uint16_t stepMs);
  void stepBlink(uint32_t color, uint16_t periodMs);
  void stepBreathe(uint32_t color, uint16_t periodMs);
  void doHeartbeat2(uint32_t color, uint16_t periodMs);
  void doFlashOnce(uint32_t color, uint16_t onMs);
  void applyBackground(DeviceState s);

private:
  // -------- Singleton storage --------
  static RGBLed* s_instance;
  RGBLed() = default;                                // private default (singleton)
  RGBLed(int pinR, int pinG, int pinB, bool activeLow) { attachPins(pinR,pinG,pinB,activeLow); }
  RGBLed(const RGBLed&) = delete;
  RGBLed& operator=(const RGBLed&) = delete;

  // pins
  int  _pinR = -1, _pinG = -1, _pinB = -1;
  bool _activeLow = true;

  // RTOS
  TaskHandle_t      _task  = nullptr;
  QueueHandle_t     _queue = nullptr;
  SemaphoreHandle_t _mtx   = nullptr;

  // live state (owned by worker)
  uint8_t     _currentPrio = PRIO_BACKGROUND;
  Pattern     _currentPat  = Pattern::OFF;
  PatternOpts _currentOpts {};
  bool        _haveCurrent = false;

  // background (worker owns)
  DeviceState _bgState = DeviceState::INIT;
  bool        _enabled = true;
};

// Pointer-style convenience macro (like CONF/LOG):
//   RGB->postOverlay(...);  RGB->setDeviceState(...);
#define RGB RGBLed::Get()

#endif // RGBLED_H






