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
constexpr uint8_t kDefaultChannelFallback = 0;
constexpr uint32_t kPairInitAckDelayMs = 300;
constexpr size_t kPairInitPayloadLen = sizeof(PairInit);

bool parsePairInit_(const uint8_t* data, size_t len, uint8_t& capsOut, uint32_t& seedOut) {
  if (!data || len < kPairInitPayloadLen) {
    return false;
  }
  if (data[0] != NOW_FRAME_PAIR_INIT) {
    return false;
  }
  const uint8_t caps = data[1];
  if ((caps & 0xF0) != 0) {
    return false;
  }
  const uint32_t seed = (static_cast<uint32_t>(data[2]) << 24) |
                        (static_cast<uint32_t>(data[3]) << 16) |
                        (static_cast<uint32_t>(data[4]) << 8)  |
                        (static_cast<uint32_t>(data[5]) << 0);
  capsOut = caps;
  seedOut = seed;
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

bool hexToBytes_(const char* hex, uint8_t* out, size_t outLen) {
  if (!hex || !out || outLen == 0) {
    return false;
  }
  const size_t hexLen = strlen(hex);
  if (hexLen != outLen * 2) {
    return false;
  }
  for (size_t i = 0; i < outLen; ++i) {
    char hi = hex[i * 2];
    char lo = hex[i * 2 + 1];
    int hiVal = (hi >= '0' && hi <= '9') ? (hi - '0') :
                (hi >= 'A' && hi <= 'F') ? (hi - 'A' + 10) :
                (hi >= 'a' && hi <= 'f') ? (hi - 'a' + 10) : -1;
    int loVal = (lo >= '0' && lo <= '9') ? (lo - '0') :
                (lo >= 'A' && lo <= 'F') ? (lo - 'A' + 10) :
                (lo >= 'a' && lo <= 'f') ? (lo - 'a' + 10) : -1;
    if (hiVal < 0 || loVal < 0) {
      return false;
    }
    out[i] = static_cast<uint8_t>((hiVal << 4) | loVal);
  }
  return true;
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
    if (!CONF) {
      DBG_PRINTLN("[ESPNOW][registerPeer] Conf missing for LMK");
      return ESP_ERR_INVALID_STATE;
    }
    String lmkHex = CONF->GetString(MASTER_LMK_KEY, MASTER_LMK_DEFAULT);
    if (lmkHex.length() != 32) {
      DBG_PRINTLN("[ESPNOW][registerPeer] Missing or invalid LMK");
      return ESP_ERR_INVALID_ARG;
    }
    uint8_t lmk[16] = {0};
    if (!hexToBytes_(lmkHex.c_str(), lmk, sizeof(lmk))) {
      DBG_PRINTLN("[ESPNOW][registerPeer] LMK hex parse failed");
      return ESP_ERR_INVALID_ARG;
    }
    DBG_PRINTF("[ESPNOW][pair] LMK=%s\n", lmkHex.c_str());
    memcpy(peerInfo.lmk, lmk, sizeof(lmk));
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
  const uint8_t requested = channel;
  uint8_t primary = 0;
  wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
  esp_err_t getErr = esp_wifi_get_channel(&primary, &second);
  if (getErr == ESP_OK && primary != 0) {
    channel_ = primary;
    DBG_PRINTF("[ESPNOW][setChannel] skip (wifi current=%u)\n", (unsigned)channel_);
  } else {
    DBG_PRINTLN("[ESPNOW][setChannel] skip (wifi current unknown)");
  }
  if (requested != 0 && channel_ != 0 && requested != channel_) {
    DBG_PRINTF("[ESPNOW][setChannel] ignore requested=%u (wifi=%u)\n",
               (unsigned)requested, (unsigned)channel_);
  }
  return true;
}

bool EspNowManager::setupSecurePeer_(const uint8_t masterMac[6], uint8_t channel) {
  if (!masterMac) {
    return false;
  }
  DBG_PRINTF("[ESPNOW][secure] Setup secure peer ch=%u\n", (unsigned)channel);
  (void)channel;
  DBG_PRINTLN("[ESPNOW][secure] Skip channel change (keep current)");
  if (!setPmk_()) {
    return false;
  }
  if (registerPeer(masterMac, true) != ESP_OK) {
    secure_ = false;
    return false;
  }
  secure_ = true;
  return true;
}

bool EspNowManager::setPmk_() {
  uint8_t pmk[16] = {0};
  if (!hexToBytes_(ESPNOW_PMK_HEX, pmk, sizeof(pmk))) {
    return false;
  }
  char pmkHex[33] = {0};
  bytesToHex_(pmk, sizeof(pmk), pmkHex, sizeof(pmkHex));
  DBG_PRINTF("[ESPNOW][pmk] PMK=%s\n", pmkHex);
  if (esp_now_set_pmk(pmk) != ESP_OK) {
    DBG_PRINTLN("[ESPNOW][pmk] esp_now_set_pmk failed");
    return false;
  }
  DBG_PRINTLN("[ESPNOW][pmk] PMK applied");
  return true;
}

bool EspNowManager::handlePairInit_(const uint8_t masterMac[6], const uint8_t* data, size_t len) {
  if (!masterMac || !data) {
    return false;
  }
  if (pendingPairInit_) {
    DBG_PRINTLN("[ESPNOW][pair] INIT ignored: pairing already pending");
    return true;
  }

  uint8_t caps = 0;
  uint32_t seed = 0;
  if (!parsePairInit_(data, len, caps, seed)) {
    return false;
  }

  uint8_t channel = channel_;

  DBG_PRINTF("[ESPNOW][pair] INIT OK: chan=%u\n", (unsigned)channel);
  DBG_PRINTF("[ESPNOW][pair] SEED=%lu\n", static_cast<unsigned long>(seed));
  DBG_PRINTF("[ESPNOW][pair] Master MAC=%02X:%02X:%02X:%02X:%02X:%02X\n",
             masterMac[0], masterMac[1], masterMac[2],
             masterMac[3], masterMac[4], masterMac[5]);
  DBG_PRINTF("[ESPNOW][pair] CAPS O=%u S=%u R=%u F=%u\n",
             (caps & 0x01) ? 1u : 0u,
             (caps & 0x02) ? 1u : 0u,
             (caps & 0x04) ? 1u : 0u,
             (caps & 0x08) ? 1u : 0u);
  DBG_PRINTLN("[ESPNOW][pair] Step 1: add temporary unencrypted peer");

  uint8_t lmk[16] = {0};
  if (!deriveLmkFromSeed_(masterMac, seed, lmk)) {
    DBG_PRINTLN("[ESPNOW][pair] LMK derivation failed");
    return false;
  }

  pendingPairInit_ = true;
  pendingPairInitMs_ = millis();
  pendingPairInitChannel_ = channel;
  pendingPairInitCaps_ = caps;
  pendingPairInitSeed_ = seed;
  pendingPairInitAckInFlight_ = false;
  pendingPairInitAckDone_ = false;
  pendingPairInitAckOk_ = false;
  pendingPairInitAckDoneMs_ = 0;
  memcpy(pendingPairInitMac_, masterMac, 6);
  DBG_PRINTLN("[ESPNOW][pair] Skip channel change (use current)");
  secure_ = false;
  if (registerPeer(masterMac, false) != ESP_OK) {
    DBG_PRINTLN("[ESPNOW][pair] Failed to add unencrypted master peer");
    clearPendingPairInit_("peer add failed", false);
    return false;
  }

  uint8_t resp[ESPNOW_MAX_DATA_LEN];
  size_t respLen = 0;
  if (!buildResponse_(ACK_PAIR_INIT, nullptr, 0, resp, &respLen)) {
    DBG_PRINTLN("[ESPNOW][pair] ACK_PAIR_INIT build failed");
    clearPendingPairInit_("ack build failed", true);
    return false;
  }
  pendingPairInitAckInFlight_ = true;
  DBG_PRINTLN("[ESPNOW][pair] Step 2: send ACK_PAIR_INIT (unencrypted)");
  if (sendData(masterMac, resp, respLen) != ESP_OK) {
    DBG_PRINTLN("[ESPNOW][pair] ACK_PAIR_INIT send failed");
    pendingPairInitAckInFlight_ = false;
    clearPendingPairInit_("ack send failed", true);
    return false;
  }
  DBG_PRINTLN("[ESPNOW][pair] Waiting for ACK delivery result...");

  return true;
}

void EspNowManager::finalizePairInit_() {
  if (!pendingPairInit_) {
    return;
  }
  if (!CONF) {
    clearPendingPairInit_("conf missing", true);
    return;
  }

  DBG_PRINTLN("[ESPNOW][pair] Step 3: apply caps + store pairing data (after ACK OK)");
  uint8_t lmk[16] = {0};
  if (!deriveLmkFromSeed_(pendingPairInitMac_, pendingPairInitSeed_, lmk)) {
    DBG_PRINTLN("[ESPNOW][pair] LMK derivation failed (finalize)");
    clearPendingPairInit_("lmk derive failed", true);
    return;
  }

  char lmkHex[33] = {0};
  bytesToHex_(lmk, sizeof(lmk), lmkHex, sizeof(lmkHex));

  storeMacAddress(pendingPairInitMac_);
  CONF->PutInt(MASTER_CHANNEL_KEY, pendingPairInitChannel_);
  CONF->PutBool(HAS_OPEN_SWITCH_KEY,  (pendingPairInitCaps_ & 0x01) != 0);
  CONF->PutBool(HAS_SHOCK_SENSOR_KEY, (pendingPairInitCaps_ & 0x02) != 0);
  CONF->PutBool(HAS_REED_SWITCH_KEY,  (pendingPairInitCaps_ & 0x04) != 0);
  CONF->PutBool(HAS_FINGERPRINT_KEY,  (pendingPairInitCaps_ & 0x08) != 0);
  CONF->PutBool(DEVICE_CONFIGURED, true);
  CONF->PutBool(ARMED_STATE, false);
  CONF->PutBool(MOTION_TRIG_ALARM, false);
  CONF->PutString(MASTER_LMK_KEY, String(lmkHex));
  setCapBitsShadow_(pendingPairInitCaps_);

  DBG_PRINTLN("[ESPNOW][pair] Step 4: remove temporary unencrypted peer");
  (void)unregisterPeer(pendingPairInitMac_);
  if (!setupSecurePeer_(pendingPairInitMac_, pendingPairInitChannel_)) {
    DBG_PRINTLN("[ESPNOW][pair] secure peer setup failed");
    CONF->PutBool(DEVICE_CONFIGURED, false);
    CONF->PutString(MASTER_LMK_KEY, MASTER_LMK_DEFAULT);
    clearPendingPairInit_("secure setup failed", false);
    return;
  }

  DBG_PRINTLN("[ESPNOW][pair] Step 5: secure peer ready, send configured bundle");
  lastHbMs_ = millis();
  online_ = true;
  sendConfiguredBundle_("PAIR_INIT");
  clearPendingPairInit_("paired", false);
}

void EspNowManager::clearPendingPairInit_(const char* reason, bool removePeer) {
  if (reason) {
    DBG_PRINTLN(String("[ESPNOW][pair] Clear pending: ") + reason);
  }
  if (removePeer && pendingPairInit_) {
    (void)unregisterPeer(pendingPairInitMac_);
  }
  pendingPairInit_ = false;
  pendingPairInitMs_ = 0;
  pendingPairInitChannel_ = MASTER_CHANNEL_DEFAULT;
  pendingPairInitCaps_ = 0;
  pendingPairInitSeed_ = 0;
  pendingPairInitAckInFlight_ = false;
  pendingPairInitAckDone_ = false;
  pendingPairInitAckOk_ = false;
  pendingPairInitAckDoneMs_ = 0;
  memset(pendingPairInitMac_, 0, sizeof(pendingPairInitMac_));
}

void EspNowManager::pollPairing_() {
  if (!pendingPairInit_) {
    return;
  }
  if (!pendingPairInitAckDone_) {
    return;
  }
  if (!pendingPairInitAckOk_) {
    clearPendingPairInit_("ack failed", true);
    return;
  }
  const uint32_t now = millis();
  if (now - pendingPairInitAckDoneMs_ < kPairInitAckDelayMs) {
    return;
  }
  DBG_PRINTLN("[ESPNOW][pair] ACK delivered OK + delay elapsed -> finalize pairing");
  finalizePairInit_();
}
