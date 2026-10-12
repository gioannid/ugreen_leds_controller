#ifndef UGREEN_SERVER_LED_BACKEND_H
#define UGREEN_SERVER_LED_BACKEND_H

#include "led_state.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace ugreen {

// Exception thrown on controller read/write errors.
class HardwareException : public std::runtime_error {
public:
    explicit HardwareException(const std::string& msg) : std::runtime_error(msg) {}
};

// Capabilities reported by the backend hardware or simulator.
struct Capabilities {
    std::vector<LedId> supported_leds;
    std::vector<OpMode> supported_modes = {OpMode::Off, OpMode::On, OpMode::Blink, OpMode::Breath};
    bool supports_rgb = true;
    bool supports_brightness = true;
    bool supports_blink = true;
    bool supports_breath = true;

    bool is_led_supported(LedId id) const {
        for (auto l : supported_leds) {
            if (l == id) return true;
        }
        return false;
    }
};

// Abstract LED Backend interface as defined in ARCHITECTURE.md.
class LedBackend {
public:
    virtual ~LedBackend() = default;

    virtual Capabilities capabilities() const = 0;
    virtual LedState read(LedId id) = 0;
    virtual void apply(LedId id, const LedPatch& patch) = 0;
};

} // namespace ugreen

#endif // UGREEN_SERVER_LED_BACKEND_H
