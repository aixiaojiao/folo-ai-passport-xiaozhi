#ifndef NOTIFY_DELIVERY_H_
#define NOTIFY_DELIVERY_H_

#include <array>
#include <cstdint>
#include <string>
#include <utility>

// No delivery queue: one active notification and four recent terminal receipts.
// This bounded, RAM-only cache makes a retry observable without replaying audio.
struct NotifyReceipt {
    std::string id;
    const char* state = "received";
    const char* reason = "";
};

class NotifyReceiptCache {
public:
    const NotifyReceipt* Find(const std::string& id) const {
        if (id.empty())
            return nullptr;
        for (const auto& receipt : receipts_) {
            if (receipt.id == id)
                return &receipt;
        }
        return nullptr;
    }

    void Save(NotifyReceipt receipt) {
        if (receipt.id.empty())
            return;
        receipts_[next_] = std::move(receipt);
        next_ = (next_ + 1) % receipts_.size();
    }

private:
    std::array<NotifyReceipt, 4> receipts_;
    size_t next_ = 0;
};

inline bool IsValidNotifyId(const std::string& id) {
    if (id.empty() || id.size() > 64)
        return false;
    for (char c : id) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.' || c == ':')) {
            return false;
        }
    }
    return true;
}

// Return a reason on rejection. Caller converts remaining lifetime to a monotonic
// deadline, so later wall-clock adjustments cannot extend an accepted message.
inline const char* ValidateNotifyWindow(int64_t now_ms, int64_t sent_at_ms, int64_t ttl_ms,
                                        bool clock_synced) {
    constexpr int64_t kMinUnixMs = 1704067200000LL;  // 2024-01-01
    constexpr int64_t kMaxUnixMs = 4102444800000LL;  // 2100-01-01
    if (sent_at_ms < kMinUnixMs || sent_at_ms > kMaxUnixMs || ttl_ms < 1 || ttl_ms > 600000) {
        return "invalid_expiry";
    }
    if (!clock_synced || now_ms < kMinUnixMs)
        return "clock_unsynced";
    if (sent_at_ms > now_ms + 5000)
        return "future_timestamp";
    if (now_ms >= sent_at_ms + ttl_ms)
        return "expired";
    return nullptr;
}

#endif  // NOTIFY_DELIVERY_H_
