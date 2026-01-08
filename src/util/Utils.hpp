#ifndef UTILS_H
#define UTILS_H
/**
 * @file Utils.h (RTOS-free)
 * @brief Synchronous Serial debug printing with optional simple grouping.
 *80:65:99:CC:C0:9C 
 * - No FreeRTOS usage: no tasks, no queues, no mutexes.
 * - DBG_PRINT/DBG_PRINTLN/DBG_PRINTF print directly to Serial.
 * - DBGSTR/DBGSTP capture a burst into a static buffer and flush on STOP.
 * - Blink helpers are no-ops to avoid blocking the main loop.
 */

#include <Arduino.h>
#include <pgmspace.h>
#include <stdarg.h>
// ===================== Global debug switch =====================

#define DBGMD true

#define SERIAL_BAUD_RATE 250000

// Capacity for grouped-burst buffer (bytes). Safe to lower if RAM is tight.

#define DBG_GROUP_MAX 4096
#define DBG_LINE_MAX  256


namespace Debug {
  // Initialize Serial on first use (idempotent)
  void begin(unsigned long baud = SERIAL_BAUD_RATE);

  // Strings
  void print(const char* s);
  void print(const String& s);
  void print(const __FlashStringHelper* fs);  // F("...")
  void println(const char* s);
  void println(const String& s);
  void println(const __FlashStringHelper* fs);
  void println(); // blank line

  // Numbers
  void print(int32_t v);
  void print(uint32_t v);
  void print(int64_t v);
  void print(uint64_t v);
  void print(long v);
  void print(unsigned long v);
  void print(float v);
  void print(double v);

  // With precision (Arduino-style)
  void print(float v, int digits);
  void print(double v, int digits);
  void println(float v, int digits);
  void println(double v, int digits);

  void println(int32_t v);
  void println(uint32_t v);
  void println(int64_t v);
  void println(uint64_t v);
  void println(long v);
  void println(unsigned long v);
  void println(float v);
  void println(double v);

  // printf-style (bounded)
  void printf(const char* fmt, ...);

  // ===== GROUPED PRINTING (simple, no locks) =====
  // While a group is active, prints/println/printf are buffered into
  // a static buffer and flushed on groupStop().
  void groupStart();
  void groupStop(bool addTrailingNewline = false);
  void groupCancel();
}

// ===================== Debug macros =====================
#if DBGMD
  #define DBG_PRINT(...)     Debug::print(__VA_ARGS__)
  #define DBG_PRINTLN(...)   Debug::println(__VA_ARGS__)
  #define DBG_PRINTF(...)    Debug::printf(__VA_ARGS__)
  #ifndef DBGSTR
    #define DBGSTR()      Debug::groupStart()
  #endif
  #ifndef DBGSTP
    #define DBGSTP()       Debug::groupStop(false)
  #endif
#else
  #define DBG_PRINT(...)     do{}while(0)
  #define DBG_PRINTLN(...)   do{}while(0)
  #define DBG_PRINTF(...)    do{}while(0)
  #define DBGSTR()        do{}while(0)
  #define DBGSTP()         do{}while(0)
#endif
#endif