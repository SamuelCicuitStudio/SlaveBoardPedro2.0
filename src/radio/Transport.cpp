#include <Transport.hpp>
#include <Utils.hpp>

namespace transport {

namespace {
constexpr size_t kHeaderSize = 11;     // fixed header bytes
constexpr size_t kMaxFrameBytes = 200; // header + payload

bool checkHeaderFields(const Header& h) {
  if (h.version != 1) return false;
  if (h.payloadLen > kMaxFrameBytes) return false;
  if ((kHeaderSize + h.payloadLen) > kMaxFrameBytes) return false;
  return true;
}
} // namespace

// ---------------- CRC8 (poly 0x07) ----------------
uint8_t TransportPort::computeCrc8(const uint8_t* data, size_t len) {
  uint8_t crc = 0x00;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b) {
      if (crc & 0x80) crc = (crc << 1) ^ 0x07;
      else            crc <<= 1;
    }
  }
  return crc;
}

// ---------------- Serializer ----------------
bool Serializer::encode(const TransportMessage& msg, std::vector<uint8_t>& out) {
  const Header& h = msg.header;
  if (!checkHeaderFields(h)) return false;
  if (msg.payload.size() != h.payloadLen) return false;

  out.clear();
  out.reserve(kHeaderSize + msg.payload.size());

  // Write header except crc8
  out.push_back(h.version);
  out.push_back(uint8_t(h.msgId & 0xFF));
  out.push_back(uint8_t((h.msgId >> 8) & 0xFF));
  out.push_back(h.srcId);
  out.push_back(h.destId);
  out.push_back(h.module);
  out.push_back(h.type);
  out.push_back(h.opCode);
  out.push_back(h.flags);
  out.push_back(h.payloadLen);

  // Compute crc8 over header bytes so far
  uint8_t crc = TransportPort::computeCrc8(out.data(), out.size());
  out.push_back(crc);

  // Payload
  out.insert(out.end(), msg.payload.begin(), msg.payload.end());
  return true;
}

bool Serializer::decode(const uint8_t* buf, size_t len, TransportMessage& out) {
  if (!buf || len < kHeaderSize) return false;

  Header h{};
  h.version    = buf[0];
  h.msgId      = static_cast<uint16_t>(buf[1]) | (static_cast<uint16_t>(buf[2]) << 8);
  h.srcId      = buf[3];
  h.destId     = buf[4];
  h.module     = buf[5];
  h.type       = buf[6];
  h.opCode     = buf[7];
  h.flags      = buf[8];
  h.payloadLen = buf[9];
  h.crc8       = buf[10];

  if (!checkHeaderFields(h)) return false;
  if (len != (kHeaderSize + h.payloadLen)) return false;

  // Verify CRC
  const uint8_t computed = TransportPort::computeCrc8(buf, kHeaderSize - 1); // crc over header except crc8
  if (computed != h.crc8) return false;

  out.header = h;
  out.payload.assign(buf + kHeaderSize, buf + kHeaderSize + h.payloadLen);
  return true;
}

// ---------------- TransportPort ----------------
TransportPort::TransportPort(uint8_t selfId, SendFn sender, Config cfg)
    : selfId_(selfId), sendFn_(std::move(sender)), cfg_(cfg) {
  dedupBuf_.reserve(cfg_.dedupEntries ? cfg_.dedupEntries : 32);
}

bool TransportPort::registerHandler(Module module, TransportHandler* handler) {
  handlers_[static_cast<uint8_t>(module)] = handler;
  return true;
}

bool TransportPort::send(TransportMessage msg, bool highPriority) {
  msg.header.srcId = selfId_;
  if (!isResponse_(msg)) {
    msg.header.msgId = nextMsgId_++;
  }
  msg.header.payloadLen = static_cast<uint8_t>(msg.payload.size());

  if (highPriority) txHigh_.push_back(msg);
  else              txLow_.push_back(msg);
  return true;
}

bool TransportPort::sendNow_(TransportMessage& msg) {
  std::vector<uint8_t> buf;
  if (!Serializer::encode(msg, buf)) return false;
  const bool ok = sendFn_ ? sendFn_(msg, buf.data(), buf.size()) : false;
  if (ok && isAckRequired_(msg) && !isResponse_(msg)) {
    Pending p;
    p.msg = msg;
    p.attempts = 1;
    p.lastSendMs = millis();
    pending_[msg.header.msgId] = p;
  }
  return ok;
}

void TransportPort::tick() {
  // Drain RX before transmitting so responses can be issued even while TX queue is busy.
  drainRxQueue();

  // Transmit queued (high priority first)
  if (!txHigh_.empty()) {
    TransportMessage msg = txHigh_.front();
    txHigh_.erase(txHigh_.begin());
    sendNow_(msg);
  } else if (!txLow_.empty()) {
    TransportMessage msg = txLow_.front();
    txLow_.erase(txLow_.begin());
    sendNow_(msg);
  }

  // Handle retries
  const uint32_t now = millis();
  for (auto it = pending_.begin(); it != pending_.end(); ) {
    Pending& p = it->second;
    if ((now - p.lastSendMs) >= cfg_.retryMs) {
      if (p.attempts >= cfg_.maxRetries) {
        auto handlerIt = handlers_.find(p.msg.header.module);
        if (handlerIt != handlers_.end() && handlerIt->second) {
          handlerIt->second->onAckTimeout(p.msg);
        }
        it = pending_.erase(it);
        continue;
      }
      p.attempts++;
      p.lastSendMs = now;
      sendNow_(p.msg);
    }
    ++it;
  }
}

void TransportPort::onReceiveRaw(const uint8_t* data, size_t len) {
  TransportMessage msg;
  if (!Serializer::decode(data, len, msg)) return;

  DBG_PRINTF("[ESPNOW][RX] TRSPRT src=%u dst=%u mod=0x%02X op=0x%02X type=0x%02X flags=0x%02X len=%u\n",
               msg.header.srcId,
               msg.header.destId,
               (unsigned)msg.header.module,
               (unsigned)msg.header.opCode,
               (unsigned)msg.header.type,
               (unsigned)msg.header.flags,
               (unsigned)msg.header.payloadLen);

  DedupKey key{msg.header.srcId, msg.header.msgId};
  portENTER_CRITICAL(&rxMux_);
  if (isDuplicate_(key)) {
    portEXIT_CRITICAL(&rxMux_);
    return;
  }
  recordDedup_(key);

  // Queue for deferred processing to keep RX independent from TX path.
  rxQueue_.push_back(msg);
  portEXIT_CRITICAL(&rxMux_);
}

void TransportPort::drainRxQueue() {
  for (;;) {
    TransportMessage msg;
    portENTER_CRITICAL(&rxMux_);
    if (rxQueue_.empty()) {
      portEXIT_CRITICAL(&rxMux_);
      break;
    }
    msg = rxQueue_.front();
    rxQueue_.erase(rxQueue_.begin());
    portEXIT_CRITICAL(&rxMux_);
    handleIncoming_(msg);
  }
}

void TransportPort::handleIncoming_(const TransportMessage& msg) {
  if (isResponse_(msg)) {
    completePending_(msg.header.msgId);
  }
  const uint8_t module = msg.header.module;
  auto it = handlers_.find(module);
  if (it != handlers_.end() && it->second) {
    it->second->onMessage(msg);
  }
  maybeAutoAck_(msg);
}

void TransportPort::maybeAutoAck_(const TransportMessage& msg) {
  if (!isAckRequired_(msg)) return;
  if (isResponse_(msg)) return;

  // Build minimal OK response
  TransportMessage resp;
  resp.header.version = 1;
  resp.header.msgId   = msg.header.msgId;
  resp.header.srcId   = selfId_;
  resp.header.destId  = msg.header.srcId;
  resp.header.module  = msg.header.module;
  resp.header.type    = static_cast<uint8_t>(MessageType::Response);
  resp.header.opCode  = msg.header.opCode;
  resp.header.flags   = 0x02; // isResponse
  resp.payload = { static_cast<uint8_t>(StatusCode::OK) };
  resp.header.payloadLen = static_cast<uint8_t>(resp.payload.size());

  sendNow_(resp);
}

void TransportPort::recordDedup_(const DedupKey& k) {
  if (dedupBuf_.size() >= cfg_.dedupEntries && !dedupBuf_.empty()) {
    dedupBuf_.erase(dedupBuf_.begin()); // drop oldest
  }
  dedupBuf_.push_back(k);
}

bool TransportPort::isDuplicate_(const DedupKey& k) const {
  for (const auto& e : dedupBuf_) {
    if (e.srcId == k.srcId && e.msgId == k.msgId) return true;
  }
  return false;
}

void TransportPort::completePending_(uint16_t msgId) {
  pending_.erase(msgId);
}

bool TransportPort::isAckRequired_(const TransportMessage& msg) {
  return (msg.header.flags & 0x01) != 0;
}

bool TransportPort::isResponse_(const TransportMessage& msg) {
  return (msg.header.flags & 0x02) != 0 ||
         msg.header.type == static_cast<uint8_t>(MessageType::Response);
}

} // namespace transport
