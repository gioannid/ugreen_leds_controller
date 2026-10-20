#include "led_state.h"

#include <charconv>
#include <sstream>

namespace ugreen {

static const std::vector<LedId> kAllLeds = {
    LedId::Power,
    LedId::Netdev,
    LedId::Disk1,
    LedId::Disk2,
    LedId::Disk3,
    LedId::Disk4,
    LedId::Disk5,
    LedId::Disk6,
    LedId::Disk7,
    LedId::Disk8,
};

const std::vector<LedId>& all_led_ids() {
    return kAllLeds;
}

std::optional<LedId> parse_led_id(std::string_view name) {
    if (name == "power") return LedId::Power;
    if (name == "netdev") return LedId::Netdev;
    if (name == "disk1") return LedId::Disk1;
    if (name == "disk2") return LedId::Disk2;
    if (name == "disk3") return LedId::Disk3;
    if (name == "disk4") return LedId::Disk4;
    if (name == "disk5") return LedId::Disk5;
    if (name == "disk6") return LedId::Disk6;
    if (name == "disk7") return LedId::Disk7;
    if (name == "disk8") return LedId::Disk8;
    return std::nullopt;
}

std::string to_string(LedId id) {
    switch (id) {
        case LedId::Power: return "power";
        case LedId::Netdev: return "netdev";
        case LedId::Disk1: return "disk1";
        case LedId::Disk2: return "disk2";
        case LedId::Disk3: return "disk3";
        case LedId::Disk4: return "disk4";
        case LedId::Disk5: return "disk5";
        case LedId::Disk6: return "disk6";
        case LedId::Disk7: return "disk7";
        case LedId::Disk8: return "disk8";
    }
    return "unknown";
}

std::optional<OpMode> parse_op_mode(std::string_view name) {
    if (name == "off") return OpMode::Off;
    if (name == "on") return OpMode::On;
    if (name == "blink") return OpMode::Blink;
    if (name == "breath") return OpMode::Breath;
    return std::nullopt;
}

std::string to_string(OpMode mode) {
    switch (mode) {
        case OpMode::Off: return "off";
        case OpMode::On: return "on";
        case OpMode::Blink: return "blink";
        case OpMode::Breath: return "breath";
    }
    return "unknown";
}

std::optional<RgbColor> parse_rgb(std::string_view str) {
    // Expected format: R,G,B (3 comma-separated decimal integers in range 0-255)
    size_t c1 = str.find(',');
    if (c1 == std::string_view::npos || c1 == 0) return std::nullopt;
    size_t c2 = str.find(',', c1 + 1);
    if (c2 == std::string_view::npos || c2 == c1 + 1 || c2 == str.size() - 1) return std::nullopt;

    // Check no additional commas
    if (str.find(',', c2 + 1) != std::string_view::npos) return std::nullopt;

    auto parse_u8 = [](std::string_view part) -> std::optional<uint8_t> {
        if (part.empty()) return std::nullopt;
        for (char c : part) {
            if (c < '0' || c > '9') return std::nullopt;
        }
        unsigned int val = 0;
        auto res = std::from_chars(part.data(), part.data() + part.size(), val);
        if (res.ec != std::errc() || res.ptr != part.data() + part.size() || val > 255) {
            return std::nullopt;
        }
        return static_cast<uint8_t>(val);
    };

    auto r = parse_u8(str.substr(0, c1));
    auto g = parse_u8(str.substr(c1 + 1, c2 - c1 - 1));
    auto b = parse_u8(str.substr(c2 + 1));

    if (!r || !g || !b) return std::nullopt;
    return RgbColor{*r, *g, *b};
}

std::string to_string(const RgbColor& color) {
    std::string s;
    s += std::to_string(color.r);
    s += ',';
    s += std::to_string(color.g);
    s += ',';
    s += std::to_string(color.b);
    return s;
}

bool validate_cycle_timing(uint16_t on_ms, uint16_t off_ms, OpMode mode, std::string* error_field) {
    uint32_t total_cycle = static_cast<uint32_t>(on_ms) + static_cast<uint32_t>(off_ms);
    if (total_cycle > 65535) {
        if (error_field) *error_field = "on_ms";
        return false;
    }
    // The chip rejects blink/breath commands with a zero half-cycle, so in
    // animation modes both on_ms and off_ms must be non-zero (which also
    // covers the zero-total case).
    if ((mode == OpMode::Blink || mode == OpMode::Breath) &&
        (total_cycle == 0 || on_ms == 0 || off_ms == 0)) {
        if (error_field) *error_field = "on_ms";
        return false;
    }
    return true;
}

namespace {

bool is_animation_mode(OpMode mode) {
    return mode == OpMode::Blink || mode == OpMode::Breath;
}

// Default cycle for an animation mode: a non-zero cycle the chip accepts.
void default_cycle_for_mode(OpMode mode, uint16_t& on_ms, uint16_t& off_ms) {
    if (mode == OpMode::Breath) {
        on_ms = 1000;
        off_ms = 1000;
    } else {
        on_ms = 500;
        off_ms = 500;
    }
}

} // namespace

LedState normalize_led_state(const LedState& s) {
    LedState out = s;
    if (is_animation_mode(out.mode)) {
        if (static_cast<uint32_t>(out.on_ms) + out.off_ms == 0) {
            default_cycle_for_mode(out.mode, out.on_ms, out.off_ms);
        }
    } else {
        // Off/on states carry no active cycle.
        out.on_ms = 0;
        out.off_ms = 0;
    }
    return out;
}

LedState resolve_target_state(const LedState& current, const LedPatch& patch) {
    LedState target = current;
    patch.apply_to(target);

    // Rule 1: timing without an explicit mode implies entering blink.
    if (!patch.mode && (patch.on_ms || patch.off_ms) && !is_animation_mode(target.mode)) {
        target.mode = OpMode::Blink;
    }

    // Rule 2: entering an animation mode from a different mode without
    // explicit timing uses the mode's default cycle rather than inheriting
    // a stale cycle from the previous mode.
    if (patch.mode && is_animation_mode(target.mode) &&
        !patch.on_ms && !patch.off_ms && current.mode != target.mode) {
        default_cycle_for_mode(target.mode, target.on_ms, target.off_ms);
    }

    return normalize_led_state(target);
}

bool validate_patch_cycle(const LedState& current, const LedPatch& patch, std::string* error_field) {
    LedState target = resolve_target_state(current, patch);
    if (is_animation_mode(target.mode)) {
        uint32_t total = static_cast<uint32_t>(target.on_ms) + target.off_ms;
        // The chip rejects a zero half-cycle, so both halves of the resolved
        // cycle must be non-zero (see validate_cycle_timing). This catches
        // single-field updates that resolve to a zero half against the
        // retained field.
        if (total > 65535 || target.on_ms == 0 || target.off_ms == 0) {
            if (error_field) *error_field = "on_ms";
            return false;
        }
    }
    return true;
}

} // namespace ugreen
