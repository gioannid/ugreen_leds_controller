#ifndef UGREEN_SERVER_OWNERSHIP_H
#define UGREEN_SERVER_OWNERSHIP_H

#include "led_state.h"

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace ugreen {

struct OwnerInfo {
    uint64_t client_id = 0;
    int priority = 100; // Strictly closer to 0 is higher priority
};

// Manages LED ownership and priority-based preemption per Section 6 of ARCHITECTURE.md.
class OwnershipManager {
public:
    // Checks if client can mutate the LED without modifying state.
    bool can_mutate(LedId id, uint64_t client_id, int priority) const;

    // Checks if client can mutate all given LEDs atomically.
    bool can_mutate_all(const std::vector<LedId>& ids, uint64_t client_id, int priority) const;

    // Acquires or preempts ownership for an LED.
    // Returns true on success, false if rejected due to priority conflict.
    bool acquire_or_preempt(LedId id, uint64_t client_id, int priority);

    // Atomically acquires or preempts ownership for a list of LEDs.
    // If any LED fails priority check, none are modified and returns false.
    bool acquire_or_preempt_all(const std::vector<LedId>& ids, uint64_t client_id, int priority);

    // Checks if client can reset the LED (must be owner or strictly higher priority).
    bool can_reset(LedId id, uint64_t client_id, int priority) const;

    // Releases ownership of an LED if allowed.
    // Returns true if cleared, false if rejected due to priority conflict.
    bool reset(LedId id, uint64_t client_id, int priority);

    // Releases all LEDs owned by the client (called on RESET all or disconnect).
    // Returns the list of LEDs that were released.
    std::vector<LedId> release_client(uint64_t client_id);

    // Queries current owner of an LED.
    std::optional<OwnerInfo> get_owner(LedId id) const;

    // Clears all ownership records.
    void clear();

private:
    std::map<LedId, OwnerInfo> owners_;
};

} // namespace ugreen

#endif // UGREEN_SERVER_OWNERSHIP_H
