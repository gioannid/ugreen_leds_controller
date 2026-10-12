#ifndef UGREEN_SERVER_LED_STATE_H
#define UGREEN_SERVER_LED_STATE_H

#include <cstdint>
#include <string>
#include <string_view>
#include <optional>
#include <vector>

namespace ugreen {

// Enumerated identifiers for NAS panel LEDs.
enum class LedId : uint8_t {
    Power = 0,
    Netdev,
    Disk1,
    Disk2,
    Disk3,
    Disk4,
    Disk5,
    Disk6,
    Disk7,
    Disk8,
};

// Operating modes supported by the controller.
enum class OpMode : uint8_t {
    Off = 0,
    On,
    Blink,
    Breath,
};

// 24-bit RGB color.
struct RgbColor {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    bool operator==(const RgbColor& o) const {
        return r == o.r && g == o.g && b == o.b;
    }
    bool operator!=(const RgbColor& o) const {
        return !(*this == o);
    }
};

// Current logical or physical state of an individual LED.
struct LedState {
    OpMode mode = OpMode::Off;
    uint8_t brightness = 0;
    RgbColor rgb = {0, 0, 0};
    uint16_t on_ms = 0;
    uint16_t off_ms = 0;
    bool valid = true;

    bool operator==(const LedState& o) const {
        return mode == o.mode &&
               brightness == o.brightness &&
               rgb == o.rgb &&
               on_ms == o.on_ms &&
               off_ms == o.off_ms &&
               valid == o.valid;
    }
    bool operator!=(const LedState& o) const {
        return !(*this == o);
    }
};

// Partial update applied to a LED's desired state.
struct LedPatch {
    std::optional<OpMode> mode;
    std::optional<uint8_t> brightness;
    std::optional<RgbColor> rgb;
    std::optional<uint16_t> on_ms;
    std::optional<uint16_t> off_ms;
    std::optional<int> priority;

    bool empty() const {
        return !mode && !brightness && !rgb && !on_ms && !off_ms && !priority;
    }

    void apply_to(LedState& target) const {
        if (mode) target.mode = *mode;
        if (brightness) target.brightness = *brightness;
        if (rgb) target.rgb = *rgb;
        if (on_ms) target.on_ms = *on_ms;
        if (off_ms) target.off_ms = *off_ms;
    }

    // Creates a patch representing only the fields where desired differs from applied.
    static LedPatch diff(const LedState& desired, const LedState& applied) {
        LedPatch p;
        if (desired.mode != applied.mode) p.mode = desired.mode;
        if (desired.brightness != applied.brightness) p.brightness = desired.brightness;
        if (desired.rgb != applied.rgb) p.rgb = desired.rgb;
        if (desired.on_ms != applied.on_ms) p.on_ms = desired.on_ms;
        if (desired.off_ms != applied.off_ms) p.off_ms = desired.off_ms;
        return p;
    }
};

// Conversion and parsing helpers
std::optional<LedId> parse_led_id(std::string_view name);
std::string to_string(LedId id);
const std::vector<LedId>& all_led_ids();

std::optional<OpMode> parse_op_mode(std::string_view name);
std::string to_string(OpMode mode);

std::optional<RgbColor> parse_rgb(std::string_view str);
std::string to_string(const RgbColor& color);

// Timing constraint validation:
// 16-bit cycle limit: on_ms + off_ms must not exceed 65535 to prevent 16-bit wraparound.
// When mode is Blink or Breath, total cycle must be > 0.
bool validate_cycle_timing(uint16_t on_ms, uint16_t off_ms, OpMode mode = OpMode::Blink, std::string* error_field = nullptr);

} // namespace ugreen

#endif // UGREEN_SERVER_LED_STATE_H
