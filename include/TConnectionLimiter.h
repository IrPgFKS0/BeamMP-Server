#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

class TConnectionLimiter {
public:
    class TGuard {
    public:
        TGuard() = default;
        TGuard(TConnectionLimiter* owner, std::string ip);

        TGuard(const TGuard&) = delete;
        TGuard& operator=(const TGuard&) = delete;

        ~TGuard() { Release(); }

        // not threadsafe
        TGuard(TGuard&& other) noexcept { *this = std::move(other); }
        // not threadsafe
        TGuard& operator=(TGuard&& other) noexcept;

    private:
        friend class TConnectionLimiter;

        // not threadsafe
        void Release();

        TConnectionLimiter* mOwner { nullptr };
        std::string mIp;
    };

    TConnectionLimiter(size_t maxPerIp, size_t maxGlobal);

    [[nodiscard]] std::optional<TGuard> TryAcquire(const std::string& ip);

private:
    void Release(const std::string& ip) {
        std::unique_lock Lock { mMutex };
        auto It = mPerIp.find(ip);
        if (It != mPerIp.end()) {
            // this guard exists to avoid underflow in case something goes wrong and this gets called too many times
            if (It->second > 0)
                --It->second;
            if (It->second == 0)
                mPerIp.erase(It);
        }
        // this guard exists to avoid underflow in case something goes wrong and this gets called too many times
        if (mGlobal > 0)
            --mGlobal;
    }

    const size_t mMaxPerIp;
    const size_t mMaxGlobal;

    std::mutex mMutex { };
    std::unordered_map<std::string, size_t> mPerIp { };
    size_t mGlobal = 0;
};
