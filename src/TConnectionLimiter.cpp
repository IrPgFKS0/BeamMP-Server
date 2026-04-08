#include "TConnectionLimiter.h"
#include <mutex>
#include <optional>
#include "Common.h"

TConnectionLimiter::TConnectionLimiter(size_t maxPerIp, size_t maxGlobal)
    : mMaxPerIp(maxPerIp)
    , mMaxGlobal(maxGlobal) {
    if (maxPerIp == 0) {
        beammp_errorf("Max connections count per IP is set to zero; the server would reject ALL connections");
        throw std::runtime_error("Invalid maximum connections per IP setting");
    }
    if (maxGlobal == 0) {
        beammp_errorf("Max connection count is set to zero; the server would reject ALL connections");
        throw std::runtime_error("Invalid maximum connections setting");
    }
}

std::optional<TConnectionLimiter::TGuard> TConnectionLimiter::TryAcquire(const std::string& ip) {
    std::unique_lock Lock { mMutex };
    if (mGlobal >= mMaxGlobal) return std::nullopt;
    // `It` is the inserted element (so if insertion worked, its 0), or its the element that
    // was already there, in which case we must check the ip connection limit
    auto [It, _] = mPerIp.try_emplace(ip, 0);
    if (It->second >= mMaxPerIp) {
        return std::nullopt;
    }
    // now increment the counter finally
    ++It->second;
    ++mGlobal;
    // RAII guard will drop the count once destructed
    return TGuard(this, ip);
}

TConnectionLimiter::TGuard::TGuard(TConnectionLimiter* owner, std::string ip)
    : mOwner(owner)
    , mIp(std::move(ip)) {
    beammp_debugf("Acquired connection guard for {}", ip);
}

TConnectionLimiter::TGuard& TConnectionLimiter::TGuard::operator=(TGuard&& other) noexcept {
    // identity check
    if (this != &other) {
        Release();
        mOwner = other.mOwner;
        mIp = std::move(other.mIp);
        other.mOwner = nullptr;
    }
    return *this;
}

void TConnectionLimiter::TGuard::Release() {
    beammp_debugf("Trying to release connection guard for {} ...", mIp);
    if (mOwner) {
        mOwner->Release(mIp);
        mOwner = nullptr;
        beammp_debugf("... Released connection guard for {}", mIp);
    } else {
        beammp_debugf("... Connection guard for {} was already released (nothing happened)", mIp);
    }
}
