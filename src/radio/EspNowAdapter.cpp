#include <EspNowAdapter.hpp>

EspNowAdapter::EspNowAdapter(uint8_t selfId,
                             EspNowManager* now,
                             PeerResolver resolver,
                             transport::TransportPort::Config cfg)
    : port_(selfId,
            [now, resolver](const transport::TransportMessage& msg,
                            const uint8_t* data,
                            size_t len) -> bool {
              if (!now) return false;
              // If this message is destined to master, try CommandAPI bridge first.
              if (msg.header.destId == 1 && now->handleTransportTx(msg)) {
                return true; // handled as ACK
              }
              if (!resolver) return false;
              uint8_t mac[6]{};
              if (!resolver(msg.header.destId, mac)) return false;
              return now->sendData(mac, data, len) == ESP_OK;
            },
            cfg),
      now_(now),
      resolver_(std::move(resolver)) {
}

void EspNowAdapter::onRadioReceive(const uint8_t* data, size_t len) {
  port_.onReceiveRaw(data, len);
}
