#ifndef UGREEN_SERVER_FAKE_BACKEND_H
#define UGREEN_SERVER_FAKE_BACKEND_H

#include "led_backend.h"

#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace ugreen {

class FakeLedBackend : public LedBackend {
public:
    struct Operation {
        enum class Type { Read, Apply };
        Type type;
        LedId id;
        LedPatch patch;
        LedState result_state;
        std::chrono::steady_clock::time_point timestamp;
    };

    FakeLedBackend() {
        // Default supported LEDs: Power, Netdev, Disk1 through Disk4
        supported_leds_ = {
            LedId::Power,
            LedId::Netdev,
            LedId::Disk1,
            LedId::Disk2,
            LedId::Disk3,
            LedId::Disk4,
        };
        for (auto id : supported_leds_) {
            states_[id] = LedState{};
        }
    }

    explicit FakeLedBackend(std::vector<LedId> leds) : supported_leds_(std::move(leds)) {
        for (auto id : supported_leds_) {
            states_[id] = LedState{};
        }
    }

    void set_supported_leds(const std::vector<LedId>& leds) {
        std::lock_guard<std::mutex> lock(mutex_);
        supported_leds_ = leds;
        states_.clear();
        for (auto id : supported_leds_) {
            states_[id] = LedState{};
        }
    }

    Capabilities capabilities() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        Capabilities caps;
        caps.supported_leds = supported_leds_;
        return caps;
    }

    LedState read(LedId id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!is_supported_unlocked(id)) {
            throw HardwareException("unsupported LED: " + to_string(id));
        }

        ++read_attempts_[id];

        if (fail_all_reads_ || fail_reads_leds_.count(id)) {
            throw HardwareException("injected hardware read failure for " + to_string(id));
        }

        Operation op;
        op.type = Operation::Type::Read;
        op.id = id;
        op.result_state = states_[id];
        op.timestamp = std::chrono::steady_clock::now();
        history_.push_back(op);

        return states_[id];
    }

    LedState apply(LedId id, const LedPatch& patch) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!is_supported_unlocked(id)) {
            throw HardwareException("unsupported LED: " + to_string(id));
        }

        if (fail_all_applies_ || fail_applies_leds_.count(id)) {
            throw HardwareException("injected hardware apply failure for " + to_string(id));
        }

        if (fail_apply_countdown_ > 0) {
            --fail_apply_countdown_;
            throw HardwareException("injected countdown hardware apply failure for " + to_string(id));
        }

        // Same mode/timing resolution semantics as the real backend's planner.
        states_[id] = resolve_target_state(states_[id], patch);

        Operation op;
        op.type = Operation::Type::Apply;
        op.id = id;
        op.patch = patch;
        op.result_state = states_[id];
        op.timestamp = std::chrono::steady_clock::now();
        history_.push_back(op);

        return states_[id];
    }

    // Failure injection helpers
    void inject_apply_failure(bool fail) {
        std::lock_guard<std::mutex> lock(mutex_);
        fail_all_applies_ = fail;
    }

    void inject_read_failure(bool fail) {
        std::lock_guard<std::mutex> lock(mutex_);
        fail_all_reads_ = fail;
    }

    void inject_apply_failure_for_led(LedId id, bool fail) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fail) {
            fail_applies_leds_.insert(id);
        } else {
            fail_applies_leds_.erase(id);
        }
    }

    void inject_apply_failure_count(int count) {
        std::lock_guard<std::mutex> lock(mutex_);
        fail_apply_countdown_ = count;
    }

    // State & history inspection
    void set_led_state(LedId id, const LedState& state) {
        std::lock_guard<std::mutex> lock(mutex_);
        states_[id] = state;
    }

    LedState get_internal_state(LedId id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = states_.find(id);
        if (it != states_.end()) {
            return it->second;
        }
        LedState st;
        st.valid = false;
        return st;
    }

    std::vector<Operation> history() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return history_;
    }

    void clear_history() {
        std::lock_guard<std::mutex> lock(mutex_);
        history_.clear();
    }

    size_t apply_count(LedId id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t count = 0;
        for (const auto& op : history_) {
            if (op.type == Operation::Type::Apply && op.id == id) {
                ++count;
            }
        }
        return count;
    }

    // Total read() invocations for the LED, including injected failures —
    // unlike history(), which only records successful operations.
    size_t read_attempts(LedId id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = read_attempts_.find(id);
        return it == read_attempts_.end() ? 0 : it->second;
    }

    size_t total_apply_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t count = 0;
        for (const auto& op : history_) {
            if (op.type == Operation::Type::Apply) {
                ++count;
            }
        }
        return count;
    }

private:
    bool is_supported_unlocked(LedId id) const {
        for (auto l : supported_leds_) {
            if (l == id) return true;
        }
        return false;
    }

    mutable std::mutex mutex_;
    std::vector<LedId> supported_leds_;
    std::map<LedId, LedState> states_;
    std::vector<Operation> history_;

    bool fail_all_applies_ = false;
    bool fail_all_reads_ = false;
    std::set<LedId> fail_applies_leds_;
    std::set<LedId> fail_reads_leds_;
    int fail_apply_countdown_ = 0;
    std::map<LedId, size_t> read_attempts_;
};

} // namespace ugreen

#endif // UGREEN_SERVER_FAKE_BACKEND_H
