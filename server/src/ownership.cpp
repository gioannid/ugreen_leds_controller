#include "ownership.h"

namespace ugreen {

bool OwnershipManager::can_mutate(LedId id, uint64_t client_id, int priority) const {
    auto it = owners_.find(id);
    if (it == owners_.end()) {
        // Unowned: any valid client can mutate
        return true;
    }

    if (it->second.client_id == client_id) {
        // Already owned by this client
        return true;
    }

    // Competing client: strictly closer to 0 is higher priority (0 is highest)
    return priority < it->second.priority;
}

bool OwnershipManager::can_mutate_all(const std::vector<LedId>& ids, uint64_t client_id, int priority) const {
    for (auto id : ids) {
        if (!can_mutate(id, client_id, priority)) {
            return false;
        }
    }
    return true;
}

bool OwnershipManager::acquire_or_preempt(LedId id, uint64_t client_id, int priority) {
    if (!can_mutate(id, client_id, priority)) {
        return false;
    }
    owners_[id] = OwnerInfo{client_id, priority};
    return true;
}

bool OwnershipManager::acquire_or_preempt_all(const std::vector<LedId>& ids, uint64_t client_id, int priority) {
    if (!can_mutate_all(ids, client_id, priority)) {
        return false;
    }
    for (auto id : ids) {
        owners_[id] = OwnerInfo{client_id, priority};
    }
    return true;
}

bool OwnershipManager::can_reset(LedId id, uint64_t client_id, int priority) const {
    auto it = owners_.find(id);
    if (it == owners_.end()) {
        return true; // Already unowned
    }
    if (it->second.client_id == client_id) {
        return true;
    }
    // Strictly higher priority client can clear lower priority owner
    return priority < it->second.priority;
}

bool OwnershipManager::reset(LedId id, uint64_t client_id, int priority) {
    if (!can_reset(id, client_id, priority)) {
        return false;
    }
    owners_.erase(id);
    return true;
}

std::vector<LedId> OwnershipManager::release_client(uint64_t client_id) {
    std::vector<LedId> released;
    for (auto it = owners_.begin(); it != owners_.end(); ) {
        if (it->second.client_id == client_id) {
            released.push_back(it->first);
            it = owners_.erase(it);
        } else {
            ++it;
        }
    }
    return released;
}

std::optional<OwnerInfo> OwnershipManager::get_owner(LedId id) const {
    auto it = owners_.find(id);
    if (it != owners_.end()) {
        return it->second;
    }
    return std::nullopt;
}

void OwnershipManager::clear() {
    owners_.clear();
}

} // namespace ugreen
