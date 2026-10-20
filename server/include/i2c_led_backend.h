#ifndef UGREEN_SERVER_I2C_LED_BACKEND_H
#define UGREEN_SERVER_I2C_LED_BACKEND_H

#include "led_backend.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ugreen_leds.h"

namespace ugreen {

// Scans /sys/class/i2c-dev for the "SMBus I801 adapter" (the same rule the
// userspace CLI uses) and returns its /dev/i2c-N path, or nullopt when no
// such adapter is registered.
std::optional<std::string> find_ugreen_i2c_device();

// One controller command produced by plan_led_update().
struct ControllerOp {
    enum class Type { OnOff, SetRgb, SetBrightness, SetBlink, SetBreath };

    Type type = Type::OnOff;
    uint8_t on = 0;                 // OnOff: 1 = on, 0 = off
    uint8_t r = 0, g = 0, b = 0;    // SetRgb
    uint8_t brightness = 0;         // SetBrightness
    uint16_t on_ms = 0, off_ms = 0; // SetBlink / SetBreath

    bool operator==(const ControllerOp& o) const {
        return type == o.type && on == o.on && r == o.r && g == o.g && b == o.b &&
               brightness == o.brightness && on_ms == o.on_ms && off_ms == o.off_ms;
    }
    bool operator!=(const ControllerOp& o) const { return !(*this == o); }
};

std::string describe_controller_op(const ControllerOp& op);

// Maps a partial update (patch) on top of the current hardware state to the
// minimal sequence of controller commands. Pure function: no I/O, so it can
// be unit-tested without hardware. Mode/timing transition semantics come
// from resolve_target_state() (led_state.h).
std::vector<ControllerOp> plan_led_update(const LedState& current, const LedPatch& patch);

// Checks whether a post-apply readback matches the state the plan intended
// (off mode: only the mode is verifiable; off/on readbacks may retain stale
// cycle registers, so timing is only compared for animation modes). Pure
// function.
bool states_match_after_apply(const LedState& expected, const LedState& actual);

// Human-readable single-line description of a state (logs/tests).
std::string describe_led_state(const LedState& s);

// LedBackend implementation wrapping the existing userspace controller
// (cli/ugreen_leds.cpp, compiled in unmodified) for the real LED hardware.
//
// The I2C adapter is located with the controller's own discovery rule (the
// "SMBus I801 adapter") and the chip is addressed at its fixed address
// (UGREEN_LED_I2C_ADDR, 0x3a) — the same way the CLI reaches it.
//
// The constructor opens the I2C device and probes LED availability, throwing
// HardwareException when no controller is present. read() and apply() must
// only be called from the hardware worker thread; capabilities() returns the
// cached probe result and is safe to call from any thread.
class I2cLedBackend : public LedBackend {
public:
    I2cLedBackend();

    Capabilities capabilities() const override;
    LedState read(LedId id) override;
    LedState apply(LedId id, const LedPatch& patch) override;

    // Path of the /dev/i2c-N device that was opened (useful in logs/tests).
    const std::string& device_path() const { return device_path_; }

private:
    ugreen_leds_t::led_data_t read_status_robust(ugreen_leds_t::led_type_t id);
    bool execute_with_verify(ugreen_leds_t::led_type_t id, const ControllerOp& op);

    ugreen_leds_t leds_;
    std::string device_path_;
    Capabilities caps_;
};

} // namespace ugreen

#endif // UGREEN_SERVER_I2C_LED_BACKEND_H
