/**************************************************************
 *  Author      : Tshibangu Samuel
 *  Role        : Freelance Embedded Systems Engineer
 *  Expertise   : Secure IoT Systems, Embedded C++, RTOS, Control Logic
 *  Contact     : tshibsamuel47@gmail.com
 *  Portfolio   : https://www.freelancer.com/u/tshibsamuel477
 *  Phone       : +216 54 429 793
 **************************************************************/
#pragma once
/**
 * @file TransportManager.h
 * @brief Wrapper that owns EspNowAdapter + TransportPort and exposes a simple API.
 *
 * Integrates:
 *  - Resolves logical destId -> MAC (master MAC from NVS).
 *  - Provides entrypoints to feed RX and tick retries.
 *  - Lets callers register module handlers and send messages.
 *
 * NOTE: This does NOT modify EspNowManager; caller must:
 *   - Call onRadioReceive() from EspNowManager::onDataReceived.
 *   - Call tick() regularly (e.g., in Device::loop()).
 */

#include <EspNowAdapter.hpp>
#include <NVSManager.hpp>

class TransportManager {
public:
  TransportManager(uint8_t selfId, EspNowManager* now, NVS* nvs);

  // Feed raw ESP-NOW frames into transport (call from EspNowManager::onDataReceived).
  void onRadioReceive(const uint8_t* data, size_t len);

  // Pump retries/timeouts (call in loop()).
  void tick();

  // Expose the transport port to register handlers and send messages.
  transport::TransportPort& port() { return adapter_.port(); }

private:
  bool resolvePeer_(uint8_t destId, uint8_t outMac[6]);
  bool parseMac_(const String& macStr, uint8_t outMac[6]);

  EspNowAdapter adapter_;
  NVS* nvs_;
};






