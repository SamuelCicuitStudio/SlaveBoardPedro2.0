#include <TransportManager.hpp>
#include <ConfigNvs.hpp>
#include <stdio.h>

TransportManager::TransportManager(uint8_t selfId, EspNowManager* now, NVS* nvs)
    : adapter_(selfId,
               now,
               [this](uint8_t destId, uint8_t outMac[6]) { return resolvePeer_(destId, outMac); },
               transport::TransportPort::Config()),
      nvs_(nvs) {}

void TransportManager::onRadioReceive(const uint8_t* data, size_t len) {
  adapter_.onRadioReceive(data, len);
}

void TransportManager::tick() {
  adapter_.port().tick();
}

bool TransportManager::resolvePeer_(uint8_t destId, uint8_t outMac[6]) {
  // For now, only master is supported as destId=1 (example).
  if (destId != 1) return false;
  if (!nvs_) return false;
  String macStr = nvs_->GetString(MASTER_ESPNOW_ID, MASTER_ESPNOW_ID_DEFAULT);
  if (macStr.isEmpty() || macStr == MASTER_ESPNOW_ID_DEFAULT) return false;
  return parseMac_(macStr, outMac);
}

bool TransportManager::parseMac_(const String& macStr, uint8_t outMac[6]) {
  if (macStr.length() != 17) return false;
  int values[6];
  if (sscanf(macStr.c_str(), "%x:%x:%x:%x:%x:%x",
             &values[0], &values[1], &values[2], &values[3], &values[4], &values[5]) != 6) {
    return false;
  }
  for (int i = 0; i < 6; ++i) outMac[i] = static_cast<uint8_t>(values[i]);
  return true;
}
