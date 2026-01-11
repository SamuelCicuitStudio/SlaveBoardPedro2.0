#include <I2CBusManager.hpp>
#include <string.h>

I2CBusManager& I2CBusManager::Get() {
    static I2CBusManager instance;
    return instance;
}

void I2CBusManager::registerClient(const char* name, ReinitFn cb, void* ctx) {
    if (!name || !*name || !cb) {
        return;
    }
    for (size_t i = 0; i < clientCount_; ++i) {
        if (strncmp(clients_[i].name, name, sizeof(clients_[i].name)) == 0) {
            clients_[i].cb = cb;
            clients_[i].ctx = ctx;
            return;
        }
    }
    if (clientCount_ >= kMaxClients) {
        return;
    }
    Client& c = clients_[clientCount_++];
    strlcpy(c.name, name, sizeof(c.name));
    c.cb = cb;
    c.ctx = ctx;
}

bool I2CBusManager::ensureStarted(int sda, int scl, uint32_t hz) {
    if (sda < 0 || scl < 0) {
        return false;
    }
    if (!started_) {
        if (!beginBus_(sda, scl, hz)) {
            return false;
        }
        return true;
    }
    if (sda == sda_ && scl == scl_ && hz == hz_) {
        return true;
    }
    if (!beginBus_(sda, scl, hz)) {
        return false;
    }
    notifyReinit_();
    return true;
}

bool I2CBusManager::resetBus() {
    if (!started_ || sda_ < 0 || scl_ < 0) {
        return false;
    }
    if (!beginBus_(sda_, scl_, hz_)) {
        return false;
    }
    notifyReinit_();
    return true;
}

bool I2CBusManager::beginBus_(int sda, int scl, uint32_t hz) {
#if defined(ARDUINO_ARCH_ESP32)
    Wire.end();
#endif
    Wire.begin(sda, scl);
    if (hz > 0) {
        Wire.setClock(hz);
    }
    sda_ = sda;
    scl_ = scl;
    hz_ = hz;
    started_ = true;
    return true;
}

void I2CBusManager::notifyReinit_() {
    for (size_t i = 0; i < clientCount_; ++i) {
        if (clients_[i].cb) {
            clients_[i].cb(clients_[i].ctx);
        }
    }
}
