/**************************************************************
 *  Central reset coordinator: route all reset requests through
 *  Device so we can shut down subsystems cleanly before restart.
 **************************************************************/
#pragma once

class Device;

namespace ResetManager {

enum class ResetKind {
  Reboot,   // plain restart
  Factory   // restart + factory reset flag
};

// Register the active device instance that will execute resets.
void Init(Device* dev);

// Queue a reset (handled inside Device::loop()).
void RequestReset(ResetKind kind, const char* reason = nullptr);

// Convenience helpers
inline void RequestFactoryReset(const char* reason = nullptr) {
  RequestReset(ResetKind::Factory, reason);
}
inline void RequestReboot(const char* reason = nullptr) {
  RequestReset(ResetKind::Reboot, reason);
}

}  // namespace ResetManager
