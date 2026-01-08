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
 * @file EspNowAdapter.h
 * @brief Thin adapter to bridge TransportPort to EspNowManager send/receive.
 *
 * Notes:
 *  - Does not modify EspNowManager; caller must hook onDataReceived to onRadioReceive().
 *  - Peer resolution (logical destId -> MAC) is provided by the user via resolver.
 */

#include <Transport.hpp>
#include <ESPNOWManager.hpp>
#include <functional>

class EspNowAdapter {
public:
  using PeerResolver = std::function<bool(uint8_t destId, uint8_t outMac[6])>;

  EspNowAdapter(uint8_t selfId,
                EspNowManager* now,
                PeerResolver resolver,
                transport::TransportPort::Config cfg = {});

  // Access to the transport port (register handlers, send messages, tick, etc.)
  transport::TransportPort& port() { return port_; }

  // Feed raw ESP-NOW RX frames here (from EspNowManager::onDataReceived).
  void onRadioReceive(const uint8_t* data, size_t len);

private:
  transport::TransportPort port_;
  EspNowManager* now_;
  PeerResolver resolver_;
};






