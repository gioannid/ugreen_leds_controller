// Hardware smoke test for the real UGREEN LED controller (Stage 4).
//
// This binary is built separately (statically linked, single file) so it can
// be copied to the production machine that owns the I2C controller and run
// there. It drives one physical LED through a sequence of visible states,
// verifies every step by reading the controller back, and can optionally
// ask the user to confirm what the LED does by eye (--confirm).
//
// Exit codes:
//   0  all checks passed
//   1  one or more checks failed
//   2  usage or environment error (no controller, no suitable LED)
//   3  cancelled by the user

#include "i2c_led_backend.h"
#include "led_state.h"

#include <unistd.h>

#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace ugreen;

namespace {

int g_checks_passed = 0;
int g_checks_failed = 0;

struct Options {
    std::string led;         // empty: auto-select (first available disk LED)
    bool include_power = false;
    bool confirm = false;
    bool skip_restore = false;
    bool help = false;
};

void show_help() {
    std::cout
        << "Usage: hw_smoke_test [options]\n"
        << "\n"
        << "Drives one physical NAS front-panel LED through a sequence of\n"
        << "states (solid red, blink, breath, white, off), verifies each step\n"
        << "by reading the controller back, and restores the LED's original\n"
        << "state at the end.\n"
        << "\n"
        << "The I2C adapter is located automatically (SMBus I801 adapter) and the\n"
        << "controller is addressed at its fixed chip address (0x3a).\n"
        << "\n"
        << "Options:\n"
        << "  --led <name>        LED to test (default: first available disk LED;\n"
        << "                      the power LED requires --include-power)\n"
        << "  --include-power     allow testing the power LED\n"
        << "  --confirm           ask the user to confirm each visible state by eye\n"
        << "  --skip-restore      do not restore the LED's original state at the end\n"
        << "  --help              display this help\n"
        << "\n"
        << "Exit codes: 0 = all checks passed, 1 = checks failed,\n"
        << "            2 = usage/environment error, 3 = cancelled by user\n";
}

void check(bool ok, const std::string& name, const std::string& detail = "") {
    if (ok) {
        ++g_checks_passed;
        std::cout << "  [PASS] " << name;
    } else {
        ++g_checks_failed;
        std::cout << "  [FAIL] " << name;
    }
    if (!detail.empty()) {
        std::cout << " (" << detail << ")";
    }
    std::cout << "\n";
}

std::string format_state(const LedState& s) {
    std::string out = "mode=" + to_string(s.mode) +
                      " rgb=" + to_string(s.rgb) +
                      " brightness=" + std::to_string(s.brightness);
    if (s.mode == OpMode::Blink || s.mode == OpMode::Breath) {
        out += " on_ms=" + std::to_string(s.on_ms) + " off_ms=" + std::to_string(s.off_ms);
    }
    return out;
}

bool ask_yes(const std::string& prompt) {
    std::cout << prompt << " [y/N] ";
    std::cout.flush();
    std::string line;
    if (!std::getline(std::cin, line)) {
        return false;
    }
    return line == "y" || line == "Y";
}

struct Step {
    std::string name;
    LedPatch patch;
    LedState expected;      // full state expected from the read-back
    bool check_color = false;   // verify rgb and brightness
    bool check_timing = false;  // verify on_ms/off_ms (blink/breath)
    std::string visual;      // human description for --confirm
};

bool run_step(std::unique_ptr<I2cLedBackend>& backend, LedId led, const Step& step,
              bool confirm) {
    std::cout << "\nStep: " << step.name << "\n";

    try {
        backend->apply(led, step.patch);
    } catch (const std::exception& ex) {
        check(false, "apply " + step.name, ex.what());
        return false;
    }

    // Give the chip (and the user, in confirm mode) time to settle.
    ::usleep(400 * 1000);

    LedState actual;
    try {
        actual = backend->read(led);
    } catch (const std::exception& ex) {
        check(false, "read back after " + step.name, ex.what());
        return false;
    }

    bool ok = actual.valid && actual.mode == step.expected.mode;
    if (ok && step.check_color) {
        ok = actual.rgb == step.expected.rgb &&
             actual.brightness == step.expected.brightness;
    }
    if (ok && step.check_timing) {
        ok = actual.on_ms == step.expected.on_ms && actual.off_ms == step.expected.off_ms;
    }
    check(ok, "read back matches " + step.name,
          ok ? "" : "expected " + format_state(step.expected) + ", got " + format_state(actual));

    if (confirm) {
        bool seen = ask_yes("  Do you see: " + step.visual + "?");
        check(seen, "user confirms " + step.name, seen ? "" : "user did not see the expected state");
    }
    return ok;
}

LedState make_state(OpMode mode, uint8_t brightness = 0, RgbColor rgb = RgbColor{0, 0, 0},
                    uint16_t on_ms = 0, uint16_t off_ms = 0) {
    LedState s;
    s.mode = mode;
    s.brightness = brightness;
    s.rgb = rgb;
    s.on_ms = on_ms;
    s.off_ms = off_ms;
    return s;
}

std::vector<Step> build_steps() {
    std::vector<Step> steps;

    Step s1;
    s1.name = "solid red";
    s1.patch.mode = OpMode::On;
    s1.patch.rgb = RgbColor{255, 0, 0};
    s1.patch.brightness = 200;
    s1.expected = make_state(OpMode::On, 200, RgbColor{255, 0, 0});
    s1.check_color = true;
    s1.visual = "the LED is solid red";
    steps.push_back(s1);

    Step s2;
    s2.name = "blink green";
    s2.patch.mode = OpMode::Blink;
    s2.patch.rgb = RgbColor{0, 255, 0};
    s2.patch.on_ms = 250;
    s2.patch.off_ms = 250;
    s2.expected = make_state(OpMode::Blink, 200, RgbColor{0, 255, 0}, 250, 250);
    s2.check_color = true;
    s2.check_timing = true;
    s2.visual = "the LED blinks green twice per second";
    steps.push_back(s2);

    Step s3;
    s3.name = "breath blue";
    s3.patch.mode = OpMode::Breath;
    s3.patch.rgb = RgbColor{0, 0, 255};
    s3.patch.on_ms = 500;
    s3.patch.off_ms = 500;
    s3.expected = make_state(OpMode::Breath, 200, RgbColor{0, 0, 255}, 500, 500);
    s3.check_color = true;
    s3.check_timing = true;
    s3.visual = "the LED breathes blue with a 1 second cycle";
    steps.push_back(s3);

    Step s4;
    s4.name = "solid white";
    s4.patch.mode = OpMode::On;
    s4.patch.rgb = RgbColor{255, 255, 255};
    s4.patch.brightness = 255;
    s4.expected = make_state(OpMode::On, 255, RgbColor{255, 255, 255});
    s4.check_color = true;
    s4.visual = "the LED is solid white at full brightness";
    steps.push_back(s4);

    Step s5;
    s5.name = "off";
    s5.patch.mode = OpMode::Off;
    s5.expected = make_state(OpMode::Off);
    s5.visual = "the LED is off";
    steps.push_back(s5);

    return steps;
}

} // namespace

int main(int argc, char* argv[]) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--led" && i + 1 < argc) {
            opts.led = argv[++i];
        } else if (arg == "--include-power") {
            opts.include_power = true;
        } else if (arg == "--confirm") {
            opts.confirm = true;
        } else if (arg == "--skip-restore") {
            opts.skip_restore = true;
        } else if (arg == "--help" || arg == "-h") {
            opts.help = true;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            show_help();
            return 2;
        }
    }

    if (opts.help) {
        show_help();
        return 0;
    }

    std::cout << "UGREEN LED hardware smoke test\n";
    std::cout << "=================================\n";
    std::cout << "This test changes the state of the physical LED(s) on the NAS\n";
    std::cout << "front panel and restores the original state at the end.\n";

    std::unique_ptr<I2cLedBackend> backend;
    try {
        backend = std::make_unique<I2cLedBackend>();
    } catch (const std::exception& ex) {
        std::cerr << "ERROR: cannot open the LED controller: " << ex.what() << "\n";
        std::cerr << "Check that the i2c-dev module is loaded and the device node is\n";
        std::cerr << "accessible (root or i2c group).\n";
        return 2;
    }

    auto caps = backend->capabilities();
    if (caps.supported_leds.empty()) {
        std::cerr << "ERROR: no LEDs detected at " << backend->device_path() << "\n";
        return 2;
    }

    std::string leds_str;
    for (auto l : caps.supported_leds) {
        if (!leds_str.empty()) leds_str += ",";
        leds_str += to_string(l);
    }
    std::cout << "Controller: " << backend->device_path() << " (address 0x" << std::hex
              << UGREEN_LED_I2C_ADDR << std::dec << ")\n";
    std::cout << "Supported LEDs: " << leds_str << "\n";

    // Select the LED to test.
    std::optional<LedId> led;
    if (!opts.led.empty()) {
        auto parsed = parse_led_id(opts.led);
        if (!parsed) {
            std::cerr << "ERROR: unknown LED name '" << opts.led << "'\n";
            return 2;
        }
        if (*parsed == LedId::Power && !opts.include_power) {
            std::cerr << "ERROR: refusing to test the power LED without --include-power\n";
            return 2;
        }
        if (!caps.is_led_supported(*parsed)) {
            std::cerr << "ERROR: LED " << to_string(*parsed) << " is not present on this unit\n";
            return 2;
        }
        led = *parsed;
    } else {
        for (auto id : all_led_ids()) {
            if (id == LedId::Power || id == LedId::Netdev) continue;
            if (caps.is_led_supported(id)) {
                led = id;
                break;
            }
        }
        if (!led && caps.is_led_supported(LedId::Netdev)) {
            led = LedId::Netdev;
        }
        if (!led) {
            std::cerr << "ERROR: no suitable LED found; pass --led <name> explicitly\n";
            return 2;
        }
    }
    std::cout << "Test LED: " << to_string(*led) << "\n";
    std::cout << "Confirm mode: " << (opts.confirm ? "on" : "off") << "\n";

    if (opts.confirm && !ask_yes("\nProceed with the hardware test?")) {
        std::cout << "Cancelled by user.\n";
        return 3;
    }

    // Remember the original state for the final restore.
    LedState original;
    try {
        original = backend->read(*led);
    } catch (const std::exception& ex) {
        std::cerr << "ERROR: cannot read the current state of " << to_string(*led)
                  << ": " << ex.what() << "\n";
        return 2;
    }
    std::cout << "Original state: " << format_state(original) << "\n";

    for (const auto& step : build_steps()) {
        run_step(backend, *led, step, opts.confirm);
    }

    // A non-existent LED must be rejected by the backend.
    if (caps.supported_leds.size() < all_led_ids().size()) {
        LedId missing = *led;
        for (auto id : all_led_ids()) {
            if (!caps.is_led_supported(id)) {
                missing = id;
                break;
            }
        }
        bool threw = false;
        try {
            backend->read(missing);
        } catch (const HardwareException&) {
            threw = true;
        }
        check(threw, "unsupported LED " + to_string(missing) + " is rejected");
    }

    if (!opts.skip_restore) {
        std::cout << "\nStep: restore original state\n";
        LedPatch restore;
        restore.mode = original.mode;
        restore.brightness = original.brightness;
        restore.rgb = original.rgb;
        if (original.mode == OpMode::Blink || original.mode == OpMode::Breath) {
            restore.on_ms = original.on_ms;
            restore.off_ms = original.off_ms;
        }
        if (restore.empty()) {
            check(true, "restore original state (LED already at original state)");
        } else {
            try {
                backend->apply(*led, restore);
                ::usleep(400 * 1000);
                LedState actual = backend->read(*led);
                bool ok = actual.valid && actual.mode == original.mode &&
                          actual.rgb == original.rgb &&
                          actual.brightness == original.brightness;
                if (ok && (original.mode == OpMode::Blink || original.mode == OpMode::Breath)) {
                    ok = actual.on_ms == original.on_ms && actual.off_ms == original.off_ms;
                }
                check(ok, "restore original state",
                      ok ? "" : "expected " + format_state(original) +
                                   ", got " + format_state(actual));
                if (!ok) {
                    std::cerr << "WARNING: the LED may not match its original state.\n";
                }
            } catch (const std::exception& ex) {
                check(false, "restore original state", ex.what());
                std::cerr << "WARNING: the LED may not match its original state.\n";
            }
        }
    }

    std::cout << "\nSummary: " << g_checks_passed << " passed, " << g_checks_failed << " failed\n";
    if (g_checks_failed == 0) {
        std::cout << "Hardware smoke test PASSED\n";
        return 0;
    }
    std::cout << "Hardware smoke test FAILED\n";
    return 1;
}
