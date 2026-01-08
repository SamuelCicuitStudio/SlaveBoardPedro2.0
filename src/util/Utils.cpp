#include <Utils.hpp>
#include <stdio.h>


// ===================== Internal state (no RTOS) =====================
namespace {
  bool     s_serialInit   = false;
  bool     s_groupActive  = false;
  char     s_groupBuf[DBG_GROUP_MAX];
  size_t   s_groupLen     = 0;

  inline void ensureSerial_(unsigned long baud = SERIAL_BAUD_RATE) {
    if (!s_serialInit) {
      Serial.begin(baud);
      // Give USB CDC a moment on some boards (safe no-op elsewhere)
      s_serialInit = true;
    }
  }

  inline void groupReset_() {
    s_groupLen = 0;
    if (DBG_GROUP_MAX) s_groupBuf[0] = '\0';
  }

  inline void groupAppend_(const char* data, size_t n) {
    if (!DBG_GROUP_MAX || !data || n == 0) return;
    size_t space = (DBG_GROUP_MAX > 0) ? (DBG_GROUP_MAX - 1 - s_groupLen) : 0;
    if (space == 0) return; // drop overflow (non-fatal)
    if (n > space) n = space;
    memcpy(s_groupBuf + s_groupLen, data, n);
    s_groupLen += n;
    s_groupBuf[s_groupLen] = '\0';
  }

  inline void emit_(const char* s, bool nl) {
    ensureSerial_();
    if (!s_groupActive) {
      if (s) Serial.print(s);
      if (nl) Serial.print('\n');
    } else {
      if (s) groupAppend_(s, strnlen(s, DBG_LINE_MAX - 1));
      if (nl) groupAppend_("\n", 1);
    }
  }

  // bounded printf helper
  inline void vprintff_(const char* fmt, va_list ap, bool with_nl = false) {
    char buf[DBG_LINE_MAX];
    vsnprintf(buf, sizeof(buf), fmt ? fmt : "", ap);
    emit_(buf, with_nl);
  }
} // namespace

// ===================== Debug API (sync) =====================
namespace Debug {
  void begin(unsigned long baud) { ensureSerial_(baud); }

  // strings
  void print(const char* s)                   { emit_(s ? s : "", false); }
  void print(const String& s)                 { emit_(s.c_str(), false); }
  void print(const __FlashStringHelper* fs)   { ensureSerial_(); Serial.print(fs); }
  void println(const char* s)                 { emit_(s ? s : "", true); }
  void println(const String& s)               { emit_(s.c_str(), true); }
  void println(const __FlashStringHelper* fs) { ensureSerial_(); Serial.println(fs); }
  void println()                              { emit_("", true); }

  // numbers (print) — rely on Arduino's overloaded Serial.print
  void print(int32_t v)        { ensureSerial_(); Serial.print(v); }
  void print(uint32_t v)       { ensureSerial_(); Serial.print(v); }
  void print(int64_t v)        { ensureSerial_(); Serial.print((long long)v); }
  void print(uint64_t v)       { ensureSerial_(); Serial.print((unsigned long long)v); }
  void print(long v)           { ensureSerial_(); Serial.print(v); }
  void print(unsigned long v)  { ensureSerial_(); Serial.print(v); }
  void print(float v)          { ensureSerial_(); Serial.print(v); }
  void print(double v)         { ensureSerial_(); Serial.print(v); }

  // with precision
  void print(float v, int d)   { ensureSerial_(); Serial.print(v, d); }
  void print(double v, int d)  { ensureSerial_(); Serial.print(v, d); }
  void println(float v, int d) { ensureSerial_(); Serial.println(v, d); }
  void println(double v, int d){ ensureSerial_(); Serial.println(v, d); }

  // numbers (println)
  void println(int32_t v)        { ensureSerial_(); Serial.println(v); }
  void println(uint32_t v)       { ensureSerial_(); Serial.println(v); }
  void println(int64_t v)        { ensureSerial_(); Serial.println((long long)v); }
  void println(uint64_t v)       { ensureSerial_(); Serial.println((unsigned long long)v); }
  void println(long v)           { ensureSerial_(); Serial.println(v); }
  void println(unsigned long v)  { ensureSerial_(); Serial.println(v); }
  void println(float v)          { ensureSerial_(); Serial.println(v); }
  void println(double v)         { ensureSerial_(); Serial.println(v); }

  // printf
  void printf(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vprintff_(fmt, ap, false);
    va_end(ap);
  }

  // grouping
  void groupStart() {
    ensureSerial_();
    s_groupActive = true;
    groupReset_();
  }

  void groupStop(bool addTrailingNewline) {
    ensureSerial_();
    if (s_groupActive && s_groupLen > 0) {
      Serial.write((const uint8_t*)s_groupBuf, s_groupLen);
      if (addTrailingNewline) Serial.print('\n');
    } else if (addTrailingNewline) {
      Serial.print('\n');
    }
    s_groupActive = false;
    groupReset_();
  }

  void groupCancel() {
    s_groupActive = false;
    groupReset_();
  }
} // namespace Debug
