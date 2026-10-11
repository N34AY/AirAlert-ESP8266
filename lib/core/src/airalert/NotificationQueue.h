#pragma once
#include <cstdint>
#include <cstddef>
#include "Types.h"

namespace airalert {

// SPEC 34: siren-relevant events entering the queue.
enum class Signal : uint8_t { Start, Escalation, Reminder, End, ManualTest };

struct Notification {
    Signal signal = Signal::Start;
    AlertType type = AlertType::Unknown;
    uint8_t priority = 50; // per-threat profile priority (SPEC 45, 49)
};

// Fixed-depth priority queue with coalescing (SPEC 47-48).
// Ordering: Start/Escalation > End > Reminder, then profile priority, then FIFO.
// END never interrupts a playing START (SPEC 47): preemption is allowed only
// over a playing Reminder, and only by Start/Escalation.
class NotificationQueue {
public:
    static constexpr size_t kDepth = 12;

    bool push(const Notification& n) {
        for (size_t i = 0; i < count_; ++i) { // coalesce identical pending
            const Notification& q = at(i);
            if (q.signal == n.signal && q.type == n.type) return true;
        }
        if (count_ >= kDepth) return false; // overflow: drop, caller may log
        slots_[(head_ + count_) % kDepth] = n;
        seq_[(head_ + count_) % kDepth] = nextSeq_++;
        ++count_;
        return true;
    }

    // Remove and return the best pending notification.
    bool pop(Notification& out) {
        if (count_ == 0) return false;
        size_t best = 0;
        for (size_t i = 1; i < count_; ++i)
            if (better(i, best)) best = i;
        const size_t idx = (head_ + best) % kDepth;
        out = slots_[idx];
        // compact: shift the tail down over the removed slot
        for (size_t i = best; i + 1 < count_; ++i) {
            slots_[(head_ + i) % kDepth] = slots_[(head_ + i + 1) % kDepth];
            seq_[(head_ + i) % kDepth] = seq_[(head_ + i + 1) % kDepth];
        }
        --count_;
        return true;
    }

    void clear() { count_ = 0; }
    size_t size() const { return count_; }

    static uint8_t rank(Signal s) {
        switch (s) {
            case Signal::ManualTest: return 4;
            case Signal::Start:
            case Signal::Escalation: return 3;
            case Signal::End: return 2;
            case Signal::Reminder: return 1;
        }
        return 0;
    }

    static bool canPreempt(const Notification& incoming, const Notification& playing) {
        return rank(incoming.signal) == 3 && playing.signal == Signal::Reminder;
    }

private:
    const Notification& at(size_t i) const { return slots_[(head_ + i) % kDepth]; }

    bool better(size_t a, size_t b) const {
        const Notification &na = at(a), &nb = at(b);
        if (rank(na.signal) != rank(nb.signal)) return rank(na.signal) > rank(nb.signal);
        if (na.priority != nb.priority) return na.priority > nb.priority;
        return seq_[(head_ + a) % kDepth] < seq_[(head_ + b) % kDepth]; // FIFO
    }

    Notification slots_[kDepth];
    uint32_t seq_[kDepth] = {};
    size_t head_ = 0;
    size_t count_ = 0;
    uint32_t nextSeq_ = 0;
};

} // namespace airalert
