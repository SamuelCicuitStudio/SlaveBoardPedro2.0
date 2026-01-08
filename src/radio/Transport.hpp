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
 * @file Transport.h
 * @brief Transport layer (radio-agnostic) sitting above EspNowManager.
 *
 * This does not touch ESP-NOW or Device/Fingerprint directly. It defines the
 * message contract, serializer, and a transport core with retry/dedup/dispatch.
 */

#include <Arduino.h>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>
#include <FreeRTOS.h>

namespace transport {

// ------- Enums -------
enum class Module : uint8_t {
  Device      = 0x01,
  Motor       = 0x02,
  Shock       = 0x03,
  SwitchReed  = 0x04,
  Fingerprint = 0x05,
  Power       = 0x06,
  Sleep       = 0x07,
  Pairing     = 0x08, // alias for Device pairing ops if needed
};

enum class MessageType : uint8_t { Request = 0, Response = 1, Event = 2, Command = 3 };

enum class StatusCode : uint8_t {
  OK = 0,
  INVALID_PARAM = 1,
  UNSUPPORTED = 2,
  BUSY = 3,
  DENIED = 4,
  PERSIST_FAIL = 5,
  APPLY_FAIL = 6,
  TIMEOUT = 7,
  CRC_FAIL = 8,
  DUPLICATE = 9,
};

// ------- Message header -------
struct Header {
  uint8_t  version     = 1;     // Must be 1
  uint16_t msgId       = 0;     // Per-sender counter
  uint8_t  srcId       = 0;
  uint8_t  destId      = 0;
  uint8_t  module      = 0;
  uint8_t  type        = 0;
  uint8_t  opCode      = 0;
  uint8_t  flags       = 0;     // bit0=ackRequired, bit1=isResponse, bit2=isError
  uint8_t  payloadLen  = 0;     // Must satisfy header+payload <= 200
  uint8_t  crc8        = 0;     // CRC over header bytes except crc8
};

// ------- Transport message -------
struct TransportMessage {
  Header header;
  std::vector<uint8_t> payload;
};

// ------- Handler interface -------
class TransportHandler {
public:
  virtual ~TransportHandler() = default;
  virtual void onMessage(const TransportMessage& msg) = 0;
  virtual void onAckTimeout(const TransportMessage& msg) {}
  virtual void onLinkState(uint8_t /*logicalId*/, bool /*online*/) {}
};

// ------- Serializer -------
class Serializer {
public:
  static bool encode(const TransportMessage& msg, std::vector<uint8_t>& out);
  static bool decode(const uint8_t* buf, size_t len, TransportMessage& out);
};

// ------- Transport core -------
class TransportPort {
public:
  using SendFn = std::function<bool(const TransportMessage& msg,
                                    const uint8_t* data, size_t len)>;

  struct Config {
    uint8_t  maxRetries   = 3;
    uint32_t retryMs      = 200;
    uint32_t dedupEntries = 32;
  };

  explicit TransportPort(uint8_t selfId, SendFn sender, Config cfg);

  bool registerHandler(Module module, TransportHandler* handler);

  // Enqueue and transmit; sets msgId and srcId automatically. High-priority
  // queue is reserved for ackRequired/control frames.
  bool send(TransportMessage msg, bool highPriority = true);

  // Feed incoming raw bytes (from radio) into the transport.
  void onReceiveRaw(const uint8_t* data, size_t len);

  // Dispatch queued RX messages (FIFO). Call from tick() or main loop.
  void drainRxQueue();

  // Pump retries/timeouts; call periodically from a loop or timer.
  void tick();

  void setSelfId(uint8_t id) { selfId_ = id; }

private:
  struct Pending {
    TransportMessage msg;
    uint8_t  attempts = 0;
    uint32_t lastSendMs = 0;
  };

  struct DedupKey {
    uint8_t  srcId;
    uint16_t msgId;
  };

  bool sendNow_(TransportMessage& msg);
  void handleIncoming_(const TransportMessage& msg);
  void maybeAutoAck_(const TransportMessage& msg);
  void recordDedup_(const DedupKey& k);
  bool isDuplicate_(const DedupKey& k) const;
  void completePending_(uint16_t msgId);
  static bool isAckRequired_(const TransportMessage& msg);
  static bool isResponse_(const TransportMessage& msg);
public:
  static uint8_t computeCrc8(const uint8_t* data, size_t len);

private:
  uint8_t selfId_;
  uint16_t nextMsgId_ = 1;
  SendFn sendFn_;
  Config cfg_;

  portMUX_TYPE rxMux_ = portMUX_INITIALIZER_UNLOCKED;
  std::vector<TransportMessage> rxQueue_;
  std::vector<DedupKey> dedupBuf_;
  std::vector<TransportMessage> txHigh_;
  std::vector<TransportMessage> txLow_;
  std::unordered_map<uint16_t, Pending> pending_; // keyed by msgId
  std::unordered_map<uint8_t, TransportHandler*> handlers_; // module -> handler
};

} // namespace transport






