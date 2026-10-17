#include "i2c_led_backend.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

namespace ugreen {

namespace {

// Timing constants mirroring the proven retry behavior of ugreen_leds_cli.
constexpr int kReadSettleMs = 8;          // settle time before a status read
constexpr int kReadRetryDelayMs = 3;      // delay between status read retries
constexpr int kMaxReadAttempts = 5;       // status read attempts per read
constexpr int kPreWriteDelayUs = 500;     // settle time before a write
constexpr int kPostWriteStatusDelayMs = 2; // wait before querying the result register
constexpr int kMaxWriteAttempts = 5;      // write + verify attempts per command

// The server LedId/OpMode enumerations and the CLI led_type_t/op_mode_t
// enumerations use identical values and ordering.
ugreen_leds_t::led_type_t to_cli_led(LedId id) {
    return static_cast<ugreen_leds_t::led_type_t>(static_cast<uint8_t>(id));
}

OpMode to_op_mode(ugreen_leds_t::op_mode_t m) {
    return static_cast<OpMode>(static_cast<uint8_t>(m));
}

LedState to_led_state(const ugreen_leds_t::led_data_t& d) {
    LedState s;
    s.mode = to_op_mode(d.op_mode);
    s.brightness = d.brightness;
    s.rgb = {d.color_r, d.color_g, d.color_b};
    // The controller chip retains its programmed cycle timing in registers
    // 5-8 across on/off changes. Preserve it whenever non-zero so returning
    // to blink/breath mode retains the hardware cycle.
    if (d.t_on + d.t_off > 0) {
        s.on_ms = d.t_on;
        s.off_ms = d.t_off;
    }
    s.valid = true;
    return s;
}

int issue_op(ugreen_leds_t& leds, ugreen_leds_t::led_type_t id, const ControllerOp& op) {
    switch (op.type) {
        case ControllerOp::Type::OnOff:
            return leds.set_onoff(id, op.on);
        case ControllerOp::Type::SetRgb:
            return leds.set_rgb(id, op.r, op.g, op.b);
        case ControllerOp::Type::SetBrightness:
            return leds.set_brightness(id, op.brightness);
        case ControllerOp::Type::SetBlink:
            return leds.set_blink(id, op.on_ms, op.off_ms);
        case ControllerOp::Type::SetBreath:
            return leds.set_breath(id, op.on_ms, op.off_ms);
    }
    return -1;
}

std::string format_address(uint16_t addr) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "0x%02x", addr);
    return buf;
}

} // namespace

std::optional<std::string> find_ugreen_i2c_device() {
    namespace fs = std::filesystem;

    // Same discovery rule as ugreen_leds_t::start(): the first I2C adapter
    // whose device/name begins with "SMBus I801 adapter".
    const std::string i2c_dev_path = "/sys/class/i2c-dev/";
    if (!fs::exists(i2c_dev_path)) {
        return std::nullopt;
    }

    for (const auto& entry : fs::directory_iterator(i2c_dev_path)) {
        if (!entry.is_directory()) {
            continue;
        }
        std::ifstream ifs(entry.path() / "device/name");
        std::string line;
        std::getline(ifs, line);
        if (line.rfind("SMBus I801 adapter", 0) == 0) {
            return "/dev/" + entry.path().filename().string();
        }
    }
    return std::nullopt;
}

std::string describe_controller_op(const ControllerOp& op) {
    switch (op.type) {
        case ControllerOp::Type::OnOff:
            return op.on ? "mode=on" : "mode=off";
        case ControllerOp::Type::SetRgb:
            return "rgb=" + std::to_string(op.r) + "," + std::to_string(op.g) + "," +
                   std::to_string(op.b);
        case ControllerOp::Type::SetBrightness:
            return "brightness=" + std::to_string(op.brightness);
        case ControllerOp::Type::SetBlink:
            return "blink on_ms=" + std::to_string(op.on_ms) + " off_ms=" +
                   std::to_string(op.off_ms);
        case ControllerOp::Type::SetBreath:
            return "breath on_ms=" + std::to_string(op.on_ms) + " off_ms=" +
                   std::to_string(op.off_ms);
    }
    return "unknown";
}

std::vector<ControllerOp> plan_led_update(const LedState& current, const LedPatch& patch) {
    LedState target = current;
    patch.apply_to(target);

    // If timing is provided without an explicit mode for an LED currently
    // in off/on mode, infer that the client intends to enter blink mode.
    if ((patch.on_ms || patch.off_ms) && !patch.mode &&
        target.mode != OpMode::Blink && target.mode != OpMode::Breath) {
        target.mode = OpMode::Blink;
    }

    // If switching mode to Breath without explicit timing from a non-breath mode,
    // do not inherit fast blink timing.
    if (patch.mode && *patch.mode == OpMode::Breath && !patch.on_ms && !patch.off_ms &&
        current.mode != OpMode::Breath) {
        target.on_ms = 1000;
        target.off_ms = 1000;
    }
    // If switching mode to Blink without explicit timing from a non-blink mode,
    // do not inherit slow breath timing.
    else if (patch.mode && *patch.mode == OpMode::Blink && !patch.on_ms && !patch.off_ms &&
             current.mode != OpMode::Blink) {
        target.on_ms = 500;
        target.off_ms = 500;
    }

    std::vector<ControllerOp> ops;

    if (target.rgb != current.rgb) {
        ControllerOp op;
        op.type = ControllerOp::Type::SetRgb;
        op.r = target.rgb.r;
        op.g = target.rgb.g;
        op.b = target.rgb.b;
        ops.push_back(op);
    }

    if (target.brightness != current.brightness) {
        ControllerOp op;
        op.type = ControllerOp::Type::SetBrightness;
        op.brightness = target.brightness;
        ops.push_back(op);
    }

    const bool timing_changed = target.on_ms != current.on_ms || target.off_ms != current.off_ms;
    if (target.mode != current.mode) {
        ControllerOp op;
        switch (target.mode) {
            case OpMode::Off:
                op.type = ControllerOp::Type::OnOff;
                op.on = 0;
                break;
            case OpMode::On:
                op.type = ControllerOp::Type::OnOff;
                op.on = 1;
                break;
            case OpMode::Blink:
                op.type = ControllerOp::Type::SetBlink;
                op.on_ms = target.on_ms;
                op.off_ms = target.off_ms;
                if (op.on_ms + op.off_ms == 0) {
                    op.on_ms = 500;
                    op.off_ms = 500;
                }
                break;
            case OpMode::Breath:
                op.type = ControllerOp::Type::SetBreath;
                op.on_ms = target.on_ms;
                op.off_ms = target.off_ms;
                if (op.on_ms + op.off_ms == 0) {
                    op.on_ms = 1000;
                    op.off_ms = 1000;
                }
                break;
        }
        ops.push_back(op);
    } else if (timing_changed &&
               (target.mode == OpMode::Blink || target.mode == OpMode::Breath)) {
        // Same blink/breath mode with a changed cycle: re-issue the mode
        // command carrying the updated timing.
        ControllerOp op;
        op.type = (target.mode == OpMode::Blink)
                      ? ControllerOp::Type::SetBlink
                      : ControllerOp::Type::SetBreath;
        op.on_ms = target.on_ms;
        op.off_ms = target.off_ms;
        if (op.on_ms + op.off_ms == 0) {
            op.on_ms = (target.mode == OpMode::Blink) ? 500 : 1000;
            op.off_ms = (target.mode == OpMode::Blink) ? 500 : 1000;
        }
        ops.push_back(op);
    }

    return ops;
}

I2cLedBackend::I2cLedBackend() {
    // Resolve the adapter first so error messages can name the exact device.
    auto dev = find_ugreen_i2c_device();
    if (!dev) {
        throw HardwareException("no 'SMBus I801 adapter' found in /sys/class/i2c-dev "
                                "(is the i2c-dev module loaded?)");
    }
    device_path_ = *dev;

    // ugreen_leds_t::start() repeats the same discovery and opens the
    // adapter at the chip's fixed address; the CLI-side code is used as-is.
    int rc = leds_.start();
    if (rc != 0) {
        throw HardwareException("failed to open I2C controller " + device_path_ +
                                " at address " + format_address(UGREEN_LED_I2C_ADDR) + ": " +
                                std::strerror(errno));
    }

    // Probe which LEDs exist; reads of non-existent LEDs never verify.
    for (auto id : all_led_ids()) {
        auto data = read_status_robust(to_cli_led(id));
        if (data.is_available) {
            caps_.supported_leds.push_back(id);
        }
    }
}

Capabilities I2cLedBackend::capabilities() const {
    return caps_;
}

LedState I2cLedBackend::read(LedId id) {
    if (!caps_.is_led_supported(id)) {
        throw HardwareException("unsupported LED: " + to_string(id));
    }

    auto data = read_status_robust(to_cli_led(id));
    if (!data.is_available) {
        throw HardwareException("read of LED " + to_string(id) +
                                " failed (no valid controller response)");
    }
    return to_led_state(data);
}

void I2cLedBackend::apply(LedId id, const LedPatch& patch) {
    if (!caps_.is_led_supported(id)) {
        throw HardwareException("unsupported LED: " + to_string(id));
    }

    auto current_raw = read_status_robust(to_cli_led(id));
    if (!current_raw.is_available) {
        throw HardwareException("read of LED " + to_string(id) +
                                " failed before update (no valid controller response)");
    }
    LedState current = to_led_state(current_raw);

    auto ops = plan_led_update(current, patch);
    for (const auto& op : ops) {
        if (!execute_with_verify(to_cli_led(id), op)) {
            throw HardwareException("controller rejected " + describe_controller_op(op) +
                                    " for LED " + to_string(id));
        }
    }
}

ugreen_leds_t::led_data_t I2cLedBackend::read_status_robust(ugreen_leds_t::led_type_t id) {
    ::usleep(kReadSettleMs * 1000);
    auto data = leds_.get_status(id);
    for (int attempt = 1; !data.is_available && attempt < kMaxReadAttempts; ++attempt) {
        ::usleep(kReadRetryDelayMs * 1000);
        data = leds_.get_status(id);
    }
    return data;
}

bool I2cLedBackend::execute_with_verify(ugreen_leds_t::led_type_t id, const ControllerOp& op) {
    for (int attempt = 0; attempt < kMaxWriteAttempts; ++attempt) {
        if (attempt == 0) {
            ::usleep(kPreWriteDelayUs);
        } else {
            ::usleep(kReadRetryDelayMs * 1000);
        }

        if (issue_op(leds_, id, op) != 0) {
            continue;
        }

        // The chip reports the outcome of the last command in register 0x80;
        // a write that was not acknowledged by the chip is a failure even
        // though the I2C transaction itself succeeded.
        ::usleep(kPostWriteStatusDelayMs * 1000);
        if (leds_.is_last_modification_successful()) {
            return true;
        }
    }
    return false;
}

} // namespace ugreen
