#include <RGBLed.hpp>
#include <Utils.hpp>
#include <stdio.h>

// -------- Singleton backing pointer --------
RGBLed* RGBLed::s_instance = nullptr;

// Create (or reconfigure) the singleton with hardware pins.
void RGBLed::Init(int pinR, int pinG, int pinB, bool activeLow) {
  if (!s_instance) {
    s_instance = new RGBLed(pinR, pinG, pinB, activeLow);
  } else {
    s_instance->attachPins(pinR, pinG, pinB, activeLow);
  }
}

// Always return a valid pointer. If not initialized yet, create
// an unattached instance; user must call attachPins() before begin().
RGBLed* RGBLed::Get() {
  if (!s_instance) s_instance = new RGBLed();
  return s_instance;
}

// Optional early boot probe
RGBLed* RGBLed::TryGet() {
  return s_instance;
}

// ---------- Debug helpers (names + color pretty print) ----------
static const char* patternName(Pattern p) {
  switch (p) {
    case Pattern::OFF:         return "OFF";
    case Pattern::SOLID:       return "SOLID";
    case Pattern::BLINK:       return "BLINK";
    case Pattern::BREATHE:     return "BREATHE";
    case Pattern::RAINBOW:     return "RAINBOW";
    case Pattern::HEARTBEAT2:  return "HEARTBEAT2";
    case Pattern::FLASH_ONCE:  return "FLASH_ONCE";
    default:                   return "?";
  }
}

static const char* stateName(DeviceState s) {
  switch (s) {
    case DeviceState::BOOT:          return "BOOT";
    case DeviceState::INIT:          return "INIT";
    case DeviceState::PAIRING:       return "PAIRING";
    case DeviceState::READY_ONLINE:  return "READY_ONLINE";
    case DeviceState::READY_OFFLINE: return "READY_OFFLINE";
    case DeviceState::SLEEP:         return "SLEEP";
    default:                         return "?";
  }
}

static void debugPrintColor(uint32_t c) {
  DBG_PRINT(F(" color=#"));
  char buf[7];
  snprintf(buf, sizeof(buf), "%02X%02X%02X", RGB_R(c), RGB_G(c), RGB_B(c));
  DBG_PRINT(buf);
}

void RGBLed::attachPins(int pinR, int pinG, int pinB, bool activeLow) {
  _pinR = pinR; _pinG = pinG; _pinB = pinB; _activeLow = activeLow;
}

// ---------------- Lifecycle ----------------
bool RGBLed::begin() {
  if (_pinR < 0 || _pinG < 0 || _pinB < 0) return false;
  pinMode(_pinR, OUTPUT);
  pinMode(_pinG, OUTPUT);
  pinMode(_pinB, OUTPUT);
  writeColor(0,0,0);

  _mtx = xSemaphoreCreateMutex();
  _queue = xQueueCreate(RGB_CMD_QUEUE_LEN, sizeof(Cmd));
  if (!_mtx || !_queue) return false;

  if (xTaskCreate(&RGBLed::taskThunk, "RGBLed", RGB_TASK_STACK, this,
                  RGB_TASK_PRIORITY, &_task) != pdPASS) return false;

  // default background at startup
  setDeviceState(DeviceState::INIT);
  return true;
}

void RGBLed::end() {
  if (!_queue) return;
  Cmd c{}; c.type = CmdType::SHUTDOWN;
  sendCmd(c, portMAX_DELAY);
  // task deletes itself
}

// ---------------- Public API: background state ----------------
void RGBLed::setDeviceState(DeviceState s) {
  Cmd c{}; c.type = CmdType::SET_BACKGROUND; c.bgState = s;
  sendCmd(c, 0);
}

// ---------------- Public API: overlay events ----------------
void RGBLed::postOverlay(OverlayEvent e) {
  PatternOpts o{};
  Cmd c{}; 
  c.type = CmdType::PLAY;

  switch (e) {
    case OverlayEvent::LOCKING:
      c.pattern = Pattern::BREATHE;
      o.color = RGB_OVR_LOCKING;
      o.periodMs = 900;
      o.priority = PRIO_ACTION; o.preempt = true;
      o.durationMs = 0;
      break;

    case OverlayEvent::BREACH:
      c.pattern = Pattern::BLINK;
      o.color = RGB_OVR_BREACH;
      o.periodMs = 180;
      o.priority = PRIO_ALERT; o.preempt = true;
      o.durationMs = 1800;
      break;

    case OverlayEvent::LOW_BATT:
      c.pattern = Pattern::BLINK;
      o.color = RGB_OVR_LOW_BATT;
      o.periodMs = 1000;
      o.priority = PRIO_ACTION; o.preempt = true;
      o.durationMs = 5000;
      break;

    case OverlayEvent::CRITICAL_BATT:
      c.pattern = Pattern::HEARTBEAT2;
      o.color = RGB_OVR_CRITICAL_BATT;
      o.periodMs = 1400;
      o.priority = PRIO_CRITICAL; o.preempt = true;
      o.durationMs = 10000;
      break;

    case OverlayEvent::DOOR_OPEN:
      c.pattern = Pattern::BLINK;
      o.color = RGB_OVR_DOOR_OPEN;
      o.periodMs = 220;
      o.priority = PRIO_ACTION; o.preempt = true;
      o.durationMs = 500;
      break;

    case OverlayEvent::DOOR_CLOSED:
      c.pattern = Pattern::FLASH_ONCE;
      o.color = RGB_OVR_DOOR_CLOSED;
      o.onMs = 160;
      o.priority = PRIO_ACTION; o.preempt = true;
      o.durationMs = 200;
      break;

    case OverlayEvent::SHOCK_DETECTED:
      c.pattern = Pattern::BLINK;
      o.color = RGB_OVR_SHOCK_DETECTED;
      o.periodMs = 180;
      o.priority = PRIO_ALERT; o.preempt = true;
      o.durationMs = 600;
      break;

    // Enrollment guidance overlays
    case OverlayEvent::FP_ENROLL_START:
      c.pattern = Pattern::BLINK;
      o.color = RGB_OVR_FP_ENROLL_START;
      o.periodMs = 220;
      o.priority = PRIO_ACTION; o.preempt = true;
      o.durationMs = 600;
      break;

    case OverlayEvent::FP_ENROLL_LIFT:
      c.pattern = Pattern::FLASH_ONCE;
      o.color = RGB_OVR_FP_ENROLL_LIFT;
      o.onMs = 160;
      o.priority = PRIO_ACTION; o.preempt = true;
      o.durationMs = 200;
      break;

    case OverlayEvent::FP_ENROLL_CAPTURE1:
      c.pattern = Pattern::FLASH_ONCE;
      o.color = RGB_OVR_FP_ENROLL_CAPTURE1;
      o.onMs = 140;
      o.priority = PRIO_ACTION; o.preempt = true;
      o.durationMs = 180;
      break;

    case OverlayEvent::FP_ENROLL_CAPTURE2:
      c.pattern = Pattern::FLASH_ONCE;
      o.color = RGB_OVR_FP_ENROLL_CAPTURE2;
      o.onMs = 140;
      o.priority = PRIO_ACTION; o.preempt = true;
      o.durationMs = 180;
      break;

    case OverlayEvent::FP_ENROLL_STORING:
      c.pattern = Pattern::BLINK;
      o.color = RGB_OVR_FP_ENROLL_STORING;
      o.periodMs = 240;
      o.priority = PRIO_ACTION; o.preempt = true;
      o.durationMs = 600;
      break;

    case OverlayEvent::FP_ENROLL_OK:
      c.pattern = Pattern::FLASH_ONCE;
      o.color = RGB_OVR_FP_ENROLL_OK;
      o.onMs = 220;
      o.priority = PRIO_ACTION; o.preempt = true;
      o.durationMs = 260;
      break;

    case OverlayEvent::FP_ENROLL_FAIL:
      c.pattern = Pattern::BLINK;
      o.color = RGB_OVR_FP_ENROLL_FAIL;
      o.periodMs = 180;
      o.priority = PRIO_ALERT;  o.preempt = true;
      o.durationMs = 600;
      break;

    case OverlayEvent::FP_ENROLL_TIMEOUT:
      c.pattern = Pattern::BLINK;
      o.color = RGB_OVR_FP_ENROLL_TIMEOUT;
      o.periodMs = 220;
      o.priority = PRIO_ALERT;  o.preempt = true;
      o.durationMs = 600;
      break;

  }

  c.opts = o;
  sendCmd(c, 0);
}

// ---------------- Public API: helpers ----------------
void RGBLed::off(uint8_t priority, bool preempt) {
  PatternOpts o{}; o.priority = priority; o.preempt = preempt;
  playPattern(Pattern::OFF, o);
}

void RGBLed::solid(uint32_t color, uint8_t priority, bool preempt, uint32_t durationMs) {
  PatternOpts o{}; o.color = color; o.priority = priority; o.preempt = preempt; o.durationMs = durationMs;
  playPattern(Pattern::SOLID, o);
}

void RGBLed::blink(uint32_t color, uint16_t periodMs, uint8_t priority, bool preempt, uint32_t durationMs) {
  PatternOpts o{}; o.color = color; o.periodMs = periodMs; o.priority = priority; o.preempt = preempt; o.durationMs = durationMs;
  playPattern(Pattern::BLINK, o);
}

void RGBLed::breathe(uint32_t color, uint16_t periodMs, uint8_t priority, bool preempt, uint32_t durationMs) {
  PatternOpts o{}; o.color = color; o.periodMs = periodMs; o.priority = priority; o.preempt = preempt; o.durationMs = durationMs;
  playPattern(Pattern::BREATHE, o);
}

void RGBLed::rainbow(uint16_t stepMs, uint8_t priority, bool preempt, uint32_t durationMs) {
  PatternOpts o{}; o.periodMs = stepMs; o.priority = priority; o.preempt = preempt; o.durationMs = durationMs;
  playPattern(Pattern::RAINBOW, o);
}

void RGBLed::heartbeat(uint32_t color, uint16_t periodMs, uint8_t priority, bool preempt, uint32_t durationMs) {
  PatternOpts o{}; o.color = color; o.periodMs = periodMs; o.priority = priority; o.preempt = preempt; o.durationMs = durationMs;
  playPattern(Pattern::HEARTBEAT2, o);
}

void RGBLed::flash(uint32_t color, uint16_t onMs, uint8_t priority, bool preempt) {
  PatternOpts o{}; o.color = color; o.onMs = onMs; o.priority = priority; o.preempt = preempt; o.durationMs = onMs + 20;
  playPattern(Pattern::FLASH_ONCE, o);
}

void RGBLed::setEnabled(bool enabled) {
  _enabled = enabled;
  if (!enabled) {
    Cmd c{}; c.type = CmdType::STOP;
    sendCmd(c, 0);
  } else {
    Cmd c{}; c.type = CmdType::SET_BACKGROUND; c.bgState = _bgState;
    sendCmd(c, 0);
  }
}

bool RGBLed::isEnabled() const {
  return _enabled;
}

void RGBLed::playPattern(Pattern pat, const PatternOpts& opts) {
  Cmd c{}; c.type = CmdType::PLAY; c.pattern = pat; c.opts = opts;
  sendCmd(c, 0);
}

// ---------------- Worker task ----------------
void RGBLed::taskThunk(void* arg) {
  static_cast<RGBLed*>(arg)->taskLoop();
}

bool RGBLed::sendCmd(const Cmd& c, TickType_t to) {
  if (!_queue) return false; // add this guard
  if (!_enabled && c.type == CmdType::PLAY) return false;
  if (xQueueSend(_queue, &c, to) == pdTRUE) return true;
  if (c.type == CmdType::PLAY && c.opts.priority >= PRIO_ALERT) {
    Cmd dump{}; xQueueReceive(_queue, &dump, 0);
    return xQueueSend(_queue, &c, to) == pdTRUE;
  }
  return false;
}

void RGBLed::taskLoop() {
  TickType_t last = xTaskGetTickCount();
  bool running = true;

  while (running) {
    if (!_enabled) {
      _haveCurrent = false;
      writeColor(0,0,0);
      Cmd cmd{};
      while (xQueueReceive(_queue, &cmd, 0) == pdTRUE) {
        if (cmd.type == CmdType::SHUTDOWN) { running = false; break; }
        if (cmd.type == CmdType::SET_BACKGROUND) {
          _bgState = cmd.bgState;
        }
      }
      if (!running) break;
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    // If we have an active pattern, step it with small waits and poll commands frequently.
    Cmd cmd{};
    if (_haveCurrent) {
      // look for immediate preemption / updates
      if (xQueueReceive(_queue, &cmd, 0) == pdTRUE) {
        if (cmd.type == CmdType::SHUTDOWN) { running = false; break; }
        else if (cmd.type == CmdType::SET_BACKGROUND) {
          if (_bgState != cmd.bgState) {
            DBG_PRINT(F("[RGB] Background -> ")); DBG_PRINT(stateName(cmd.bgState)); DBG_PRINTLN();
          }
          _bgState = cmd.bgState;
          // background change may be ignored if a stronger pattern is active
        } else if (cmd.type == CmdType::PLAY) {
          // preempt if requested and priority >= current
          if (cmd.opts.preempt && cmd.opts.priority >= _currentPrio) {
            _currentPat   = cmd.pattern;
            _currentOpts  = cmd.opts;
            _currentPrio  = cmd.opts.priority;
            // restart duration window
            _haveCurrent  = true;
            last = xTaskGetTickCount();
          } else {
            // queue not prioritized beyond this; ignore lower-priority noise while busy
          }
        } else if (cmd.type == CmdType::STOP) {
          _haveCurrent = false;
          writeColor(0,0,0);
        }
      }

      // step current pattern (each step delays modestly and returns)
      switch (_currentPat) {
        case Pattern::OFF:
          writeColor(0,0,0);
          vTaskDelay(pdMS_TO_TICKS(15));
          break;

        case Pattern::SOLID:
          writeColor(RGB_R(_currentOpts.color), RGB_G(_currentOpts.color), RGB_B(_currentOpts.color));
          vTaskDelay(pdMS_TO_TICKS(25));
          break;

        case Pattern::BLINK:
          stepBlink(_currentOpts.color, _currentOpts.periodMs);
          break;

        case Pattern::BREATHE:
          stepBreathe(_currentOpts.color, _currentOpts.periodMs);
          break;

        case Pattern::RAINBOW:
          stepRainbow(_currentOpts.periodMs ? _currentOpts.periodMs : 20);
          break;

        case Pattern::HEARTBEAT2:
          doHeartbeat2(_currentOpts.color, _currentOpts.periodMs ? _currentOpts.periodMs : 1400);
          break;

        case Pattern::FLASH_ONCE:
          doFlashOnce(_currentOpts.color, _currentOpts.onMs ? _currentOpts.onMs : 120);
          // auto-finish immediately after the flash
          _haveCurrent = false;
          break;
      }

      // duration expiry?
      if (_haveCurrent && _currentOpts.durationMs > 0) {
        uint32_t elapsedMs = (xTaskGetTickCount() - last) * portTICK_PERIOD_MS;
        if (elapsedMs >= _currentOpts.durationMs) {
          _haveCurrent = false;
        }
      }

      // if nothing active anymore, fall through to background apply
      if (!_haveCurrent) {
        applyBackground(_bgState);
      }
    } else {
      // idle -> apply/keep background; block a little but remain interruptible
      applyBackground(_bgState);
      if (xQueueReceive(_queue, &cmd, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (cmd.type == CmdType::SHUTDOWN) { running = false; break; }
        else if (cmd.type == CmdType::SET_BACKGROUND) {
          _bgState = cmd.bgState;
        } else if (cmd.type == CmdType::PLAY) {
          _currentPat   = cmd.pattern;
          _currentOpts  = cmd.opts;
          _currentPrio  = cmd.opts.priority;
          _haveCurrent  = true;
          last = xTaskGetTickCount();
        } else if (cmd.type == CmdType::STOP) {
          // noop (already idle)
        }
      }
    }
  }

  // shutdown
  writeColor(0,0,0);
  vTaskDelete(nullptr);
}

// ---------------- Background mapping ----------------
void RGBLed::applyBackground(DeviceState s) {
  switch (s) {
    case DeviceState::BOOT:
    case DeviceState::INIT:
      writeColor(RGB_R(RGB_BG_INIT), RGB_G(RGB_BG_INIT), RGB_B(RGB_BG_INIT));
      vTaskDelay(pdMS_TO_TICKS(20));
      break;

    case DeviceState::PAIRING:
      stepRainbow(RGB_BG_PAIRING_STEP_MS);
      break;

    case DeviceState::READY_ONLINE:
      doHeartbeat2(RGB_BG_READY_ONLINE, 1500);
      break;

    case DeviceState::READY_OFFLINE:
      stepBlink(RGB_BG_READY_OFFLINE, 1000);
      break;

    case DeviceState::SLEEP: {
      static uint32_t t0 = millis();
      uint32_t now = millis();
      if (now - t0 > 10000) {
        doHeartbeat2(RGB_BG_SLEEP_BEAT, 1200);
        t0 = now;
      } else {
        writeColor(0,0,0);
        vTaskDelay(pdMS_TO_TICKS(60));
      }
      break;
    }
  }
}
// ---------------- Pattern primitives ----------------
void RGBLed::writeColor(uint8_t r, uint8_t g, uint8_t b) {
  if (_activeLow) { r = 255 - r; g = 255 - g; b = 255 - b; }
  analogWrite(_pinR, r);
  analogWrite(_pinG, g);
  analogWrite(_pinB, b);
}

void RGBLed::stepRainbow(uint16_t stepMs) {
  static float hue = 0.0f;

  // Speed control: degrees advanced per tick (from RGBConfig.h)
  const float stepDeg = RGB_RAINBOW_STEP_DEG;
  if (hue >= 360.0f) hue -= 360.0f;  // keep bounded (fast, no fmod)

  // HSV -> RGB (s=1, v=1) fast path
  float h = hue;
  int   i = int(h / 60.0f) % 6;
  float f = (h / 60.0f) - i;

  const float v = 1.0f, s = 1.0f;
  const float p = v * (1 - s);           // 0
  const float q = v * (1 - f * s);
  const float t = v * (1 - (1 - f) * s);

  float rf, gf, bf;
  switch (i) {
    case 0: rf=v; gf=t; bf=p; break;
    case 1: rf=q; gf=v; bf=p; break;
    case 2: rf=p; gf=v; bf=t; break;
    case 3: rf=p; gf=q; bf=v; break;
    case 4: rf=t; gf=p; bf=v; break;
    default: rf=v; gf=p; bf=q; break;  // i==5
  }

  writeColor((uint8_t)(rf * 255.0f),
             (uint8_t)(gf * 255.0f),
             (uint8_t)(bf * 255.0f));

  // Advance hue faster (configurable)
  hue += stepDeg;
  if (hue >= 360.0f) hue -= 360.0f;

  // Keep caller’s pacing
  vTaskDelay(pdMS_TO_TICKS(stepMs));
}


void RGBLed::stepBlink(uint32_t color, uint16_t periodMs) {
  uint16_t half = periodMs / 2;
  writeColor(RGB_R(color), RGB_G(color), RGB_B(color));
  vTaskDelay(pdMS_TO_TICKS(half));
  writeColor(0,0,0);
  vTaskDelay(pdMS_TO_TICKS(half));
}

void RGBLed::stepBreathe(uint32_t color, uint16_t periodMs) {
  // triangle wave 0..255..0
  static int16_t a = 0, dir = 1;
  uint8_t r = RGB_R(color), g = RGB_G(color), b = RGB_B(color);
  uint8_t rr = (uint8_t)((r * a) / 255);
  uint8_t gg = (uint8_t)((g * a) / 255);
  uint8_t bb = (uint8_t)((b * a) / 255);
  writeColor(rr, gg, bb);

  // step size to span the period in ~50 steps
  int step = 255 / 25; // 25 up + 25 down ~50
  a += dir * step;
  if (a >= 255) { a = 255; dir = -1; }
  if (a <= 0)   { a = 0;   dir =  1;  }
  vTaskDelay(pdMS_TO_TICKS(periodMs / 50));
}

void RGBLed::doHeartbeat2(uint32_t color, uint16_t periodMs) {
  // two short pulses then pause
  uint16_t beat = periodMs / 8;         // short ON
  uint16_t gap  = periodMs / 8;         // short OFF between pulses
  uint16_t rest = periodMs - (beat*2 + gap);

  writeColor(RGB_R(color), RGB_G(color), RGB_B(color));
  vTaskDelay(pdMS_TO_TICKS(beat));
  writeColor(0,0,0);
  vTaskDelay(pdMS_TO_TICKS(gap));
  writeColor(RGB_R(color), RGB_G(color), RGB_B(color));
  vTaskDelay(pdMS_TO_TICKS(beat));
  writeColor(0,0,0);
  vTaskDelay(pdMS_TO_TICKS(rest));
}

void RGBLed::doFlashOnce(uint32_t color, uint16_t onMs) {
  writeColor(RGB_R(color), RGB_G(color), RGB_B(color));
  vTaskDelay(pdMS_TO_TICKS(onMs));
  writeColor(0,0,0);
}

