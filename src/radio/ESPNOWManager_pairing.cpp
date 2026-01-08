#include <ESPNOWManager.hpp>
#include <CommandAPI.hpp>
#include <ConfigNvs.hpp>
#include <NVSManager.hpp>
#include <SecurityKeys.hpp>
#include <Utils.hpp>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

namespace {
constexpr uint8_t kDefaultChannelFallback = 1;
constexpr uint32_t kPairInitAckDelayMs = 50;
constexpr uint32_t kPairInitSecureDelayMs = 3000;
constexpr uint32_t kPairInitRetryDelayMs = 500;

bool parseCaps_(const String& msg, uint8_t& capsOut) {
  int capsIdx = msg.indexOf("caps=");
  if (capsIdx < 0) {
    return false;
  }
  String caps = msg.substring(capsIdx + 5);
  caps.trim();
  uint8_t bits = 0;
  auto readFlag = [&](char key, uint8_t bit) -> bool {
    int idx = caps.indexOf(key);
    if (idx < 0 || (idx + 1) >= caps.length()) {
      return false;
    }
    char v = caps.charAt(idx + 1);
    if (v == '1') {
      bits |= bit;
      return true;
    }
    if (v == '0') {
      return true;
    }
    return false;
  };
  if (!readFlag('O', 0x01) ||
      !readFlag('S', 0x02) ||
      !readFlag('R', 0x04) ||
      !readFlag('F', 0x08)) {
    return false;
  }
  capsOut = bits;
  return true;
}

bool parsePairInit_(const String& msg, uint8_t& channelOut, uint8_t& capsOut) {
  if (!msg.startsWith("PAIR_INIT:")) {
    return false;
  }
  int codeIdx = msg.indexOf("code=");
  int chanIdx = msg.indexOf("chan=");
  if (codeIdx < 0 || chanIdx < 0) {
    return false;
  }
  int codeEnd = msg.indexOf(':', codeIdx);
  String code = (codeEnd >= 0)
                    ? msg.substring(codeIdx + 5, codeEnd)
                    : msg.substring(codeIdx + 5);
  if (code != PAIR_INIT_CODE) {
    return false;
  }
  int chanEnd = msg.indexOf(':', chanIdx);
  String chanStr = (chanEnd >= 0)
                       ? msg.substring(chanIdx + 5, chanEnd)
                       : msg.substring(chanIdx + 5);
  int chan = chanStr.toInt();
  if (chan <= 0 || chan > 14) {
    return false;
  }
  if (!parseCaps_(msg, capsOut)) {
    return false;
  }
  channelOut = static_cast<uint8_t>(chan);
  return true;
}

void bytesToHex_(const uint8_t* data, size_t len, char* out, size_t outLen) {
  static const char kHex[] = "0123456789ABCDEF";
  if (!data || !out || outLen < (len * 2 + 1)) {
    return;
  }
  for (size_t i = 0; i < len; ++i) {
    out[i * 2]     = kHex[(data[i] >> 4) & 0x0F];
    out[i * 2 + 1] = kHex[data[i] & 0x0F];
  }
  out[len * 2] = '\0';
}

} // namespace

uint8_t EspNowManager::getDefaultChannel_() {
  if (PREER_CHANNEL > 0) {
    return static_cast<uint8_t>(PREER_CHANNEL);
  }
  return kDefaultChannelFallback;
}

// =============================================================
//  Peer Management / Send
// =============================================================
esp_err_t EspNowManager::registerPeer(const uint8_t* peer_addr, bool encrypt) {
  if (!peer_addr) {
    DBG_PRINTLN("[ESPNOW][registerPeer] Invalid arg: peer_addr=null");
    return ESP_ERR_INVALID_ARG;
  }
  if (esp_now_is_peer_exist(peer_addr)) {
    DBG_PRINTF("[ESPNOW][registerPeer] Peer already exists: %02X:%02X:%02X:%02X:%02X:%02X\n",
                 peer_addr[0], peer_addr[1], peer_addr[2], peer_addr[3], peer_addr[4], peer_addr[5]);
    return ESP_OK;
  }

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, peer_addr, ESP_NOW_ETH_ALEN);
  peerInfo.channel = channel_;
  peerInfo.encrypt = encrypt;
  if (encrypt) {
    uint8_t lmk[16] = {0};
    uint8_t slaveMac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, slaveMac) == ESP_OK &&
        deriveLmkFromMacs_(peer_addr, slaveMac, lmk)) {
      char lmkHex[33] = {0};
      bytesToHex_(lmk, sizeof(lmk), lmkHex, sizeof(lmkHex));
      DBG_PRINTF("[ESPNOW][pair] LMK=%s\n", lmkHex);
      memcpy(peerInfo.lmk, lmk, sizeof(lmk));
    }
  }
  esp_err_t r = esp_now_add_peer(&peerInfo);

  DBG_PRINTF("[ESPNOW][registerPeer] add %02X:%02X:%02X:%02X:%02X:%02X ch=%d enc=%d -> %d\n",
               peer_addr[0], peer_addr[1], peer_addr[2], peer_addr[3], peer_addr[4], peer_addr[5],
               (int)peerInfo.channel, (int)peerInfo.encrypt, (int)r);
  return r;
}

esp_err_t EspNowManager::unregisterPeer(const uint8_t* peer_addr) {
  if (!peer_addr) {
    DBG_PRINTLN("[ESPNOW][unregisterPeer] Invalid arg: peer_addr=null");
    return ESP_ERR_INVALID_ARG;
  }
  esp_err_t r = esp_now_del_peer(peer_addr);
  DBG_PRINTF("[ESPNOW][unregisterPeer] del %02X:%02X:%02X:%02X:%02X:%02X -> %d\n",
               peer_addr[0], peer_addr[1], peer_addr[2], peer_addr[3], peer_addr[4], peer_addr[5], (int)r);
  return r;
}

bool EspNowManager::setChannel_(uint8_t channel) {
  if (channel == 0) {
    channel = getDefaultChannel_();
  }
  esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  if (err != ESP_OK) {
    DBG_PRINTF("[ESPNOW][setChannel] esp_wifi_set_channel failed: %d\n", (int)err);
    return false;
  }
  channel_ = channel;
  return true;
}

bool EspNowManager::setupSecurePeer_(const uint8_t masterMac[6], uint8_t channel) {
  if (!masterMac) {
    return false;
  }
  if (!setChannel_(channel)) {
    return false;
  }
  uint8_t pmk[16] = {0};
  if (!derivePmkFromMasterMac_(masterMac, pmk)) {
    return false;
  }
  char pmkHex[33] = {0};
  bytesToHex_(pmk, sizeof(pmk), pmkHex, sizeof(pmkHex));
  DBG_PRINTF("[ESPNOW][pair] PMK=%s\n", pmkHex);
  if (esp_now_set_pmk(pmk) != ESP_OK) {
    DBG_PRINTLN("[ESPNOW][secure] esp_now_set_pmk failed");
    return false;
  }
  secure_ = true;
  return registerPeer(masterMac, true) == ESP_OK;
}

bool EspNowManager::handlePairInit_(const uint8_t masterMac[6], const String& msg) {
  uint8_t channel = 0;
  uint8_t caps = 0;
  if (!parsePairInit_(msg, channel, caps)) {
    DBG_PRINTLN("[ESPNOW][pair] INIT parse failed");
    return false;
  }

  DBG_PRINTF("[ESPNOW][pair] INIT OK: chan=%u\n", (unsigned)channel);
  DBG_PRINTF("[ESPNOW][pair] Master MAC=%02X:%02X:%02X:%02X:%02X:%02X\n",
             masterMac[0], masterMac[1], masterMac[2],
             masterMac[3], masterMac[4], masterMac[5]);
  DBG_PRINTF("[ESPNOW][pair] CAPS O=%u S=%u R=%u F=%u\n",
             (caps & 0x01) ? 1u : 0u,
             (caps & 0x02) ? 1u : 0u,
             (caps & 0x04) ? 1u : 0u,
             (caps & 0x08) ? 1u : 0u);
  storeMacAddress(masterMac);
  if (CONF) {
    CONF->PutInt(MASTER_CHANNEL_KEY, channel);
    CONF->PutBool(HAS_OPEN_SWITCH_KEY,  (caps & 0x01) != 0);
    CONF->PutBool(HAS_SHOCK_SENSOR_KEY, (caps & 0x02) != 0);
    CONF->PutBool(HAS_REED_SWITCH_KEY,  (caps & 0x04) != 0);
    CONF->PutBool(HAS_FINGERPRINT_KEY,  (caps & 0x08) != 0);
    CONF->PutBool(DEVICE_CONFIGURED, true);
    CONF->PutBool(ARMED_STATE, false);
    CONF->PutBool(MOTION_TRIG_ALARM, false);
  }
  setCapBitsShadow_(caps);

  (void)setChannel_(channel);
  secure_ = false;
  if (registerPeer(masterMac, false) != ESP_OK) {
    DBG_PRINTLN("[ESPNOW][pair] Failed to add unencrypted master peer");
    return false;
  }
  const char* ack = ACK_PAIR_INIT;
  DBG_PRINTLN("[ESPNOW][pair] Sending ACK_PAIR_INIT (unencrypted)");
  if (sendData(masterMac, reinterpret_cast<const uint8_t*>(ack), strlen(ack) + 1) != ESP_OK) {
    DBG_PRINTLN("[ESPNOW][pair] ACK_PAIR_INIT send failed");
    unregisterPeer(masterMac);
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(kPairInitAckDelayMs));
  DBG_PRINTLN("[ESPNOW][pair] Removing temporary unencrypted peer");
  (void)unregisterPeer(masterMac);
  pendingPairInit_ = true;
  pendingPairInitMs_ = millis() + kPairInitSecureDelayMs;
  memcpy(pendingPairInitMac_, masterMac, sizeof(pendingPairInitMac_));
  pendingPairInitChannel_ = channel;
  DBG_PRINTLN("[ESPNOW][pair] Scheduled secure rejoin");
  return true;
}

void EspNowManager::pollPairing_() {
  if (!pendingPairInit_) {
    return;
  }
  const uint32_t now = millis();
  if (static_cast<int32_t>(now - pendingPairInitMs_) < 0) {
    return;
  }

  DBG_PRINTLN("[ESPNOW][pair] Switching to secure mode");
  pendingPairInit_ = false;

  esp_now_deinit();
  if (esp_now_init() != ESP_OK) {
    DBG_PRINTLN("[ESPNOW][pair] esp_now_init failed (secure)");
    pendingPairInit_ = true;
    pendingPairInitMs_ = now + kPairInitRetryDelayMs;
    return;
  }

  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataReceived);
  if (!setupSecurePeer_(pendingPairInitMac_, pendingPairInitChannel_)) {
    DBG_PRINTLN("[ESPNOW][pair] secure peer setup failed");
    pendingPairInit_ = true;
    pendingPairInitMs_ = now + kPairInitRetryDelayMs;
    return;
  }
  DBG_PRINTLN("[ESPNOW][pair] Secure peer ready");

  lastHbMs_ = millis();
  online_ = true;
  sendConfiguredBundle_("PAIR_INIT");
}
