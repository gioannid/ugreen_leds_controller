#ifndef UGREEN_SERVER_SCHEDULER_H
#define UGREEN_SERVER_SCHEDULER_H

#include "led_state.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ugreen {

struct HardwareOperation {
    LedId id;
    LedPatch patch;
    LedState target_state;
};

struct HardwareBatch {
    uint64_t batch_id = 0;
    uint64_t max_generation = 0;
    std::vector<HardwareOperation> operations;

    bool empty() const { return operations.empty(); }
};

struct HardwareBatchResult {
    uint64_t batch_id = 0;
    uint64_t max_generation = 0;
    bool success = true;
    std::optional<LedId> failed_led;
    std::string error_message;
    std::vector<std::pair<LedId, LedState>> applied_states;
};

struct FlushBarrier {
    uint64_t client_id = 0;
    uint64_t request_id = 0;
    uint64_t target_generation = 0;
    std::function<void(bool success, const std::string& err)> callback;
};

class Scheduler {
public:
    struct Slot {
        LedState desired;
        LedState applied;
        LedState fallback;
        uint64_t desired_generation = 0;
        uint64_t applied_generation = 0;
        bool dirty = false;
        std::string last_hardware_error;
        // Generation of the failed batch that produced last_hardware_error.
        // An error is stale (does not fail FLUSH barriers) once the desired
        // state has advanced past it, since a retry is pending.
        uint64_t error_generation = 0;
    };

    explicit Scheduler(const std::vector<LedId>& supported_leds);

    // Applies a validated patch to the desired state. Returns the new generation number.
    uint64_t apply_patch(LedId id, const LedPatch& patch);

    // Resets desired state to fallback. Returns the new generation number.
    uint64_t apply_fallback(LedId id);

    // Configures fallback state for a specific LED.
    void set_fallback(LedId id, const LedState& fallback);

    // State queries
    LedState get_desired(LedId id) const;
    LedState get_applied(LedId id) const;
    std::string get_last_error(LedId id) const;
    uint64_t current_generation() const { return current_generation_; }
    uint64_t applied_generation(LedId id) const;

    // Checks if any LED has desired state differing from applied state.
    bool has_dirty() const;

    // Generates a coalesced batch of hardware writes for dirty LEDs.
    std::optional<HardwareBatch> generate_batch();

    // Called when the hardware worker completes a batch.
    void on_batch_complete(const HardwareBatchResult& result);

    // Barrier management for FLUSH command.
    // If target_generation has already been applied, callback is invoked immediately.
    void add_flush_barrier(uint64_t client_id, uint64_t req_id, uint64_t target_gen,
                           std::function<void(bool, const std::string&)> cb);

    // Cancels pending barriers for a client (e.g. on disconnect).
    void cancel_barriers_for_client(uint64_t client_id);

private:
    void evaluate_barriers();

    std::map<LedId, Slot> slots_;
    uint64_t current_generation_ = 0;
    uint64_t next_batch_id_ = 1;
    bool in_flight_ = false;
    uint64_t in_flight_batch_id_ = 0;
    std::vector<FlushBarrier> pending_barriers_;
};

} // namespace ugreen

#endif // UGREEN_SERVER_SCHEDULER_H
