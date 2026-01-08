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
 * @file StubHandler.h
 * @brief Simple handler returning UNSUPPORTED for modules not used on this role.
 */

#include <Transport.hpp>

class StubHandler : public transport::TransportHandler {
public:
  explicit StubHandler(transport::TransportPort* port) : port_(port) {}

  void onMessage(const transport::TransportMessage& msg) override {
    transport::TransportMessage resp;
    resp.header = msg.header;
    resp.header.srcId  = msg.header.destId;
    resp.header.destId = msg.header.srcId;
    resp.header.type   = static_cast<uint8_t>(transport::MessageType::Response);
    resp.header.flags  = 0x02;
    resp.payload = { static_cast<uint8_t>(transport::StatusCode::UNSUPPORTED) };
    resp.header.payloadLen = static_cast<uint8_t>(resp.payload.size());
    if (port_) port_->send(resp, true);
  }
private:
  transport::TransportPort* port_;
};






