#include "scheduler.h"

#include <algorithm>

namespace ugreen {

Scheduler::Scheduler(const std::vector<LedId>& supported_leds) {
    for (auto id : supported_leds) {
        Slot s;
        s.desired = LedState{};
        s.applied = LedState{};
        s.fallback = LedState{};
        slots_[id] = s;
    }
}

void Scheduler::set_fallback(LedId id, const LedState& fallback) {
    auto it = slots_.find(id);
    if (it != slots_.end()) {
        it->second.fallback = fallback;
    }
}

void Scheduler::calibrate(LedId id, const LedState& state) {
    auto it = slots_.find(id);
    if (it == slots_.end()) return;

    // Once any update has been accepted, the desired state is client-owned;
    // the verified readbacks of the first batch already establish the truth.
    if (it->second.desired_generation != 0) return;

    it->second.desired = normalize_led_state(state);
    it->second.applied = it->second.desired;
    it->second.dirty = false;
}

void Scheduler::init_to_fallback(const std::map<LedId, LedState>& explicit_fallbacks) {
    for (auto& [id, slot] : slots_) {
        auto it = explicit_fallbacks.find(id);
        if (it == explicit_fallbacks.end()) {
            // No explicit [fallback] entry: the panel may legitimately show
            // anything (e.g. firmware activity indicators), so this LED must
            // not be rewritten — it is calibrated from hardware instead.
            continue;
        }
        slot.desired = normalize_led_state(it->second);
        slot.init_write = true;
        slot.dirty = true;
    }
}

uint64_t Scheduler::apply_patch(LedId id, const LedPatch& patch) {
    auto it = slots_.find(id);
    if (it == slots_.end()) return current_generation_;

    // The shared resolver keeps the desired state canonical: mode inference
    // (timing without mode implies blink), mode-switch default cycles and
    // zeroed timing for off/on states all happen here, exactly as the
    // hardware planner will resolve them.
    it->second.desired = resolve_target_state(it->second.desired, patch);
    ++current_generation_;
    it->second.desired_generation = current_generation_;
    it->second.dirty = true;
    return current_generation_;
}

uint64_t Scheduler::apply_fallback(LedId id) {
    auto it = slots_.find(id);
    if (it == slots_.end()) return current_generation_;

    // A fallback may legitimately be uncanonical (e.g. a blink state with no
    // cycle in the config); normalize it the same way patches are resolved.
    it->second.desired = normalize_led_state(it->second.fallback);
    ++current_generation_;
    it->second.desired_generation = current_generation_;
    it->second.dirty = true;
    return current_generation_;
}

LedState Scheduler::get_desired(LedId id) const {
    auto it = slots_.find(id);
    if (it != slots_.end()) return it->second.desired;
    LedState st;
    st.valid = false;
    return st;
}

LedState Scheduler::get_applied(LedId id) const {
    auto it = slots_.find(id);
    if (it != slots_.end()) return it->second.applied;
    LedState st;
    st.valid = false;
    return st;
}

std::string Scheduler::get_last_error(LedId id) const {
    auto it = slots_.find(id);
    if (it != slots_.end()) return it->second.last_hardware_error;
    return "";
}

uint64_t Scheduler::applied_generation(LedId id) const {
    auto it = slots_.find(id);
    if (it != slots_.end()) return it->second.applied_generation;
    return 0;
}

bool Scheduler::has_dirty() const {
    for (const auto& [id, slot] : slots_) {
        if (slot.dirty && (slot.desired != slot.applied || slot.init_write)) {
            return true;
        }
    }
    return false;
}

std::optional<HardwareBatch> Scheduler::generate_batch() {
    if (in_flight_) {
        return std::nullopt;
    }

    HardwareBatch batch;
    batch.batch_id = next_batch_id_++;
    uint64_t max_gen = 0;

    for (auto& [id, slot] : slots_) {
        if (!slot.dirty) continue;

        if (slot.desired == slot.applied && !slot.init_write) {
            // Desired state equals applied state (e.g. temporary flash reverted)
            slot.dirty = false;
            slot.applied_generation = slot.desired_generation;
            continue;
        }

        LedPatch diff;
        if (slot.init_write) {
            // Startup init-to-fallback: force a full-state write (every field)
            // so the fallback reaches the hardware even when it matches the
            // assumed initial state the scheduler started from.
            diff.mode = slot.desired.mode;
            diff.brightness = slot.desired.brightness;
            diff.rgb = slot.desired.rgb;
            diff.on_ms = slot.desired.on_ms;
            diff.off_ms = slot.desired.off_ms;
        } else {
            diff = LedPatch::diff(slot.desired, slot.applied);
        }
        if (!diff.empty()) {
            batch.operations.push_back(HardwareOperation{id, diff, slot.desired});
            max_gen = std::max(max_gen, slot.desired_generation);
        } else {
            slot.dirty = false;
            slot.applied_generation = slot.desired_generation;
        }
    }

    if (batch.empty()) {
        evaluate_barriers();
        return std::nullopt;
    }

    batch.max_generation = max_gen;
    in_flight_ = true;
    in_flight_batch_id_ = batch.batch_id;
    return batch;
}

void Scheduler::on_batch_complete(const HardwareBatchResult& result) {
    in_flight_ = false;

    for (const auto& [id, state] : result.applied_states) {
        auto it = slots_.find(id);
        if (it != slots_.end()) {
            it->second.applied = state;
            it->second.last_hardware_error.clear();
            // The forced startup write reached the hardware; a retry (after a
            // failure on a *later* LED of this batch) is plain diff-based.
            it->second.init_write = false;
            it->second.applied_generation = std::max(it->second.applied_generation, result.max_generation);
            if (it->second.desired == it->second.applied) {
                it->second.dirty = false;
            }
        }
    }

    if (!result.success) {
        if (result.failed_led) {
            auto it = slots_.find(*result.failed_led);
            if (it != slots_.end()) {
                it->second.last_hardware_error = result.error_message;
                it->second.error_generation = result.max_generation;
                it->second.dirty = true;
            }
        }
    }

    evaluate_barriers();
}

void Scheduler::evaluate_barriers() {
    auto it = pending_barriers_.begin();
    while (it != pending_barriers_.end()) {
        uint64_t target = it->target_generation;
        bool all_satisfied = true;
        bool hardware_failed = false;
        std::string err_msg;

        for (const auto& [id, slot] : slots_) {
            // Only a current (non-stale) hardware error fails the barrier:
            // an error from a batch whose generation the desired state has
            // since surpassed is being retried, so the barrier must wait.
            // A slot whose startup init write is still pending is a
            // server-internal task, not a client update: its desired
            // generation never advances on its own, so its staleness is
            // measured against the global generation (client activity).
            // Otherwise a startup write that failed once would keep failing
            // every later FLUSH, even after the hardware has recovered and
            // the client's own updates are what is actually pending.
            uint64_t reference = slot.init_write ? current_generation_ : slot.desired_generation;
            if (!slot.last_hardware_error.empty() && reference <= slot.error_generation) {
                hardware_failed = true;
                err_msg = slot.last_hardware_error;
            }
            if (slot.dirty && slot.desired_generation <= target && slot.desired != slot.applied) {
                all_satisfied = false;
            }
        }

        if (hardware_failed) {
            it->callback(false, err_msg.empty() ? "HARDWARE_ERROR" : err_msg);
            it = pending_barriers_.erase(it);
        } else if (all_satisfied && !in_flight_) {
            it->callback(true, "");
            it = pending_barriers_.erase(it);
        } else {
            ++it;
        }
    }
}

void Scheduler::add_flush_barrier(uint64_t client_id, uint64_t req_id, uint64_t target_gen,
                                  std::function<void(bool, const std::string&)> cb) {
    pending_barriers_.push_back(FlushBarrier{client_id, req_id, target_gen, std::move(cb)});
    evaluate_barriers();
}

void Scheduler::cancel_barriers_for_client(uint64_t client_id) {
    auto it = pending_barriers_.begin();
    while (it != pending_barriers_.end()) {
        if (it->client_id == client_id) {
            it = pending_barriers_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace ugreen
