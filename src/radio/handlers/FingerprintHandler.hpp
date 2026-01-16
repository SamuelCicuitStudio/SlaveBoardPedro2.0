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
 * @file FingerprintHandler.h
 * @brief Transport handler for Fingerprint module opCodes.
 */

#include <Transport.hpp>
#include <FingerprintScanner.hpp>

class FingerprintHandler : public transport::TransportHandler {
public:
  FingerprintHandler(Fingerprint* fp, transport::TransportPort* port)
      : fp_(fp), port_(port) {}

  void onMessage(const transport::TransportMessage& msg) override;

private:
  void sendStatus_(const transport::TransportMessage& req,
                   transport::StatusCode status,
                   const std::vector<uint8_t>& extra = {});
  void sendReasonEvent_(uint8_t reason);

  Fingerprint* fp_;
  transport::TransportPort* port_;
};






