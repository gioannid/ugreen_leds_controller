#include "config.h"
#include "i2c_led_backend.h"
#include "test_framework.h"

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace ugreen;

namespace {

LedState make_state(OpMode mode, uint8_t brightness, RgbColor rgb,
                    uint16_t on_ms = 0, uint16_t off_ms = 0) {
    LedState s;
    s.mode = mode;
    s.brightness = brightness;
    s.rgb = rgb;
    s.on_ms = on_ms;
    s.off_ms = off_ms;
    return s;
}

ControllerOp make_op(ControllerOp::Type type, uint8_t on = 0, uint8_t r = 0, uint8_t g = 0,
                     uint8_t b = 0, uint8_t brightness = 0, uint16_t on_ms = 0,
                     uint16_t off_ms = 0) {
    ControllerOp op;
    op.type = type;
    op.on = on;
    op.r = r;
    op.g = g;
    op.b = b;
    op.brightness = brightness;
    op.on_ms = on_ms;
    op.off_ms = off_ms;
    return op;
}

void assert_ops_equal(const std::vector<ControllerOp>& actual,
                      const std::vector<ControllerOp>& expected) {
    TEST_ASSERT_EQ(actual.size(), expected.size());
    for (size_t i = 0; i < expected.size() && i < actual.size(); ++i) {
        TEST_ASSERT_EQ(actual[i] == expected[i], true);
    }
}

std::string tmp_config_path(const std::string& name) {
    return "/tmp/ugreen_i2c_cfg_test_" + std::to_string(getpid()) + "_" + name + ".conf";
}

bool load_ok(const std::string& content, ServerConfig& out, std::string& err) {
    std::string path = tmp_config_path("case");
    std::ofstream out_file(path);
    out_file << content;
    out_file.close();
    bool rc = load_server_config(path, out, err);
    std::remove(path.c_str());
    return rc;
}

} // namespace

void run_i2c_backend_tests() {
    std::cout << "Running I2cLedBackend Tests (no hardware required)...\n";

    test::run_test("plan: off to on with color and brightness", []() {
        // From a different off state so the color/brightness writes are part
        // of the plan (programmed while still off, before the mode switch).
        auto current = make_state(OpMode::Off, 100, RgbColor{0, 255, 0});
        LedPatch patch;
        patch.mode = OpMode::On;
        patch.rgb = RgbColor{255, 0, 0};
        patch.brightness = 200;

        auto ops = plan_led_update(current, patch);
        assert_ops_equal(ops, {
            make_op(ControllerOp::Type::SetRgb, 0, 255, 0, 0),
            make_op(ControllerOp::Type::SetBrightness, 0, 0, 0, 0, 200),
            make_op(ControllerOp::Type::OnOff, 1),
        });
    });

    test::run_test("plan: on to blink", []() {
        auto current = make_state(OpMode::On, 200, RgbColor{255, 0, 0});
        LedPatch patch;
        patch.mode = OpMode::Blink;
        patch.on_ms = 250;
        patch.off_ms = 250;

        auto ops = plan_led_update(current, patch);
        assert_ops_equal(ops, {make_op(ControllerOp::Type::SetBlink, 0, 0, 0, 0, 0, 250, 250)});
    });

    test::run_test("plan: blink to breath with new cycle", []() {
        auto current = make_state(OpMode::Blink, 200, RgbColor{255, 0, 0}, 250, 250);
        LedPatch patch;
        patch.mode = OpMode::Breath;
        patch.on_ms = 500;
        patch.off_ms = 500;

        auto ops = plan_led_update(current, patch);
        assert_ops_equal(ops, {make_op(ControllerOp::Type::SetBreath, 0, 0, 0, 0, 0, 500, 500)});
    });

    test::run_test("plan: blink timing change re-issues blink only", []() {
        auto current = make_state(OpMode::Blink, 200, RgbColor{255, 0, 0}, 250, 250);
        LedPatch patch;
        patch.on_ms = 100;

        auto ops = plan_led_update(current, patch);
        assert_ops_equal(ops, {make_op(ControllerOp::Type::SetBlink, 0, 0, 0, 0, 0, 100, 250)});
    });

    test::run_test("plan: color-only change during blink", []() {
        auto current = make_state(OpMode::Blink, 200, RgbColor{255, 0, 0}, 250, 250);
        LedPatch patch;
        patch.rgb = RgbColor{0, 0, 255};

        auto ops = plan_led_update(current, patch);
        assert_ops_equal(ops, {make_op(ControllerOp::Type::SetRgb, 0, 0, 0, 255)});
    });

    test::run_test("plan: on to off", []() {
        auto current = make_state(OpMode::On, 200, RgbColor{255, 0, 0});
        LedPatch patch;
        patch.mode = OpMode::Off;

        auto ops = plan_led_update(current, patch);
        assert_ops_equal(ops, {make_op(ControllerOp::Type::OnOff, 0)});
    });

    test::run_test("plan: brightness-only change", []() {
        auto current = make_state(OpMode::On, 100, RgbColor{255, 0, 0});
        LedPatch patch;
        patch.brightness = 255;

        auto ops = plan_led_update(current, patch);
        assert_ops_equal(ops, {make_op(ControllerOp::Type::SetBrightness, 0, 0, 0, 0, 255)});
    });

    test::run_test("plan: identical state produces no commands", []() {
        auto current = make_state(OpMode::Blink, 200, RgbColor{255, 0, 0}, 250, 250);
        LedPatch patch;  // empty patch
        assert_ops_equal(plan_led_update(current, patch), {});

        // A patch repeating the current values is also a no-op.
        patch.mode = OpMode::Blink;
        patch.brightness = 200;
        patch.rgb = RgbColor{255, 0, 0};
        patch.on_ms = 250;
        patch.off_ms = 250;
        assert_ops_equal(plan_led_update(current, patch), {});
    });

    test::run_test("plan: off to blink carries the full cycle", []() {
        LedPatch patch;
        patch.mode = OpMode::Blink;
        patch.on_ms = 300;
        patch.off_ms = 700;

        auto ops = plan_led_update(LedState{}, patch);
        assert_ops_equal(ops, {make_op(ControllerOp::Type::SetBlink, 0, 0, 0, 0, 0, 300, 700)});
    });

    test::run_test("plan: blink to off to blink without timing re-issues a legal blink", []() {
        // The original field failure: after blink -> off, a mode-only blink
        // patch must still produce a mode command with a non-zero cycle,
        // regardless of the chip's retained (stale) cycle registers.
        LedPatch p1;
        p1.mode = OpMode::Blink;
        p1.on_ms = 250;
        p1.off_ms = 250;
        auto on = make_state(OpMode::On, 200, RgbColor{255, 0, 0});
        auto ops1 = plan_led_update(on, p1);
        assert_ops_equal(ops1, {make_op(ControllerOp::Type::SetBlink, 0, 0, 0, 0, 0, 250, 250)});

        // Off, with the chip retaining its 250/250 cycle registers.
        auto blink = make_state(OpMode::Blink, 200, RgbColor{255, 0, 0}, 250, 250);
        LedPatch p2;
        p2.mode = OpMode::Off;
        auto ops2 = plan_led_update(blink, p2);
        assert_ops_equal(ops2, {make_op(ControllerOp::Type::OnOff, 0)});

        // Blink again, no timing: default cycle, never zero, never the stale one.
        LedPatch p3;
        p3.mode = OpMode::Blink;
        auto off_retained = make_state(OpMode::Off, 200, RgbColor{255, 0, 0}, 250, 250);
        auto ops3 = plan_led_update(off_retained, p3);
        TEST_ASSERT_EQ(ops3.size(), 1U);
        TEST_ASSERT_EQ(ops3[0].type, ControllerOp::Type::SetBlink);
        TEST_ASSERT(ops3[0].on_ms + ops3[0].off_ms > 0);
        TEST_ASSERT_EQ(ops3[0].on_ms, 500);
        TEST_ASSERT_EQ(ops3[0].off_ms, 500);
    });

    test::run_test("plan: entering breath from blink without timing uses breath default", []() {
        auto current = make_state(OpMode::Blink, 200, RgbColor{255, 0, 0}, 250, 250);
        LedPatch patch;
        patch.mode = OpMode::Breath;

        auto ops = plan_led_update(current, patch);
        assert_ops_equal(ops, {make_op(ControllerOp::Type::SetBreath, 0, 0, 0, 0, 0, 1000, 1000)});
    });

    test::run_test("plan: entering blink from breath without timing uses blink default", []() {
        auto current = make_state(OpMode::Breath, 200, RgbColor{255, 0, 0}, 1000, 1000);
        LedPatch patch;
        patch.mode = OpMode::Blink;

        auto ops = plan_led_update(current, patch);
        assert_ops_equal(ops, {make_op(ControllerOp::Type::SetBlink, 0, 0, 0, 0, 0, 500, 500)});
    });

    test::run_test("plan: timing without mode from on infers blink", []() {
        auto current = make_state(OpMode::On, 200, RgbColor{255, 0, 0});
        LedPatch patch;
        patch.on_ms = 100;
        patch.off_ms = 100;

        auto ops = plan_led_update(current, patch);
        assert_ops_equal(ops, {make_op(ControllerOp::Type::SetBlink, 0, 0, 0, 0, 0, 100, 100)});
    });

    test::run_test("plan: chip-retained timing in off state is ignored by the plan", []() {
        // Readback after an off may carry the old cycle; planning a color
        // change in off mode must not touch mode or timing.
        auto current = make_state(OpMode::Off, 200, RgbColor{255, 0, 0}, 250, 250);
        LedPatch patch;
        patch.rgb = RgbColor{0, 255, 0};

        auto ops = plan_led_update(current, patch);
        assert_ops_equal(ops, {make_op(ControllerOp::Type::SetRgb, 0, 0, 255, 0)});
    });

    test::run_test("post-apply verification matches mode, color and active cycle", []() {
        LedState expected = make_state(OpMode::Blink, 128, RgbColor{1, 2, 3}, 250, 250);
        LedState ok = make_state(OpMode::Blink, 128, RgbColor{1, 2, 3}, 250, 250);
        TEST_ASSERT(states_match_after_apply(expected, ok));

        LedState wrong_mode = make_state(OpMode::Breath, 128, RgbColor{1, 2, 3}, 250, 250);
        TEST_ASSERT(!states_match_after_apply(expected, wrong_mode));

        LedState wrong_cycle = make_state(OpMode::Blink, 128, RgbColor{1, 2, 3}, 100, 250);
        TEST_ASSERT(!states_match_after_apply(expected, wrong_cycle));

        LedState wrong_color = make_state(OpMode::Blink, 128, RgbColor{1, 2, 4}, 250, 250);
        TEST_ASSERT(!states_match_after_apply(expected, wrong_color));

        // Off readback retains stale cycle registers: timing must not matter.
        LedState off_expected = make_state(OpMode::Off, 128, RgbColor{1, 2, 3});
        LedState off_retained = make_state(OpMode::Off, 128, RgbColor{1, 2, 3}, 250, 250);
        TEST_ASSERT(states_match_after_apply(off_expected, off_retained));
        LedState off_wrong_color = make_state(OpMode::Off, 128, RgbColor{9, 9, 9}, 250, 250);
        TEST_ASSERT(!states_match_after_apply(off_expected, off_wrong_color));

        // On mode: color and brightness are visible and must converge.
        LedState on_expected = make_state(OpMode::On, 128, RgbColor{1, 2, 3});
        LedState on_wrong_color = make_state(OpMode::On, 128, RgbColor{9, 9, 9});
        TEST_ASSERT(!states_match_after_apply(on_expected, on_wrong_color));
        LedState on_wrong_brightness = make_state(OpMode::On, 0, RgbColor{1, 2, 3});
        TEST_ASSERT(!states_match_after_apply(on_expected, on_wrong_brightness));
    });

    test::run_test("controller op description is stable", []() {
        TEST_ASSERT_EQ(describe_controller_op(make_op(ControllerOp::Type::OnOff, 1)),
                       std::string("mode=on"));
        TEST_ASSERT_EQ(describe_controller_op(make_op(ControllerOp::Type::OnOff, 0)),
                       std::string("mode=off"));
        TEST_ASSERT_EQ(describe_controller_op(make_op(ControllerOp::Type::SetRgb, 0, 255, 0, 0)),
                       std::string("rgb=255,0,0"));
        TEST_ASSERT_EQ(describe_controller_op(make_op(ControllerOp::Type::SetBrightness, 0, 0, 0,
                                                      0, 200)),
                       std::string("brightness=200"));
        TEST_ASSERT_EQ(describe_controller_op(make_op(ControllerOp::Type::SetBlink, 0, 0, 0, 0, 0,
                                                      250, 250)),
                       std::string("blink on_ms=250 off_ms=250"));
        TEST_ASSERT_EQ(describe_controller_op(make_op(ControllerOp::Type::SetBreath, 0, 0, 0, 0, 0,
                                                      500, 500)),
                       std::string("breath on_ms=500 off_ms=500"));
    });

    test::run_test("device discovery returns a valid i2c node or none", []() {
        auto dev = find_ugreen_i2c_device();
        // nullopt is valid on machines without the adapter.
        if (dev) {
            TEST_ASSERT(dev->rfind("/dev/i2c-", 0) == 0);
        }
    });

    // Environment-aware: on a machine without the UGREEN controller the
    // backend must fail cleanly; on the production machine it must come up
    // and report at least one LED.
    test::run_test("backend construction matches hardware presence", []() {
        bool controller_present = false;
        auto dev = find_ugreen_i2c_device();
        if (dev) {
            int fd = ::open(dev->c_str(), O_RDWR);
            if (fd >= 0) {
                // Same fixed chip address the backend uses (UGREEN_LED_I2C_ADDR).
                controller_present = (::ioctl(fd, I2C_SLAVE, UGREEN_LED_I2C_ADDR) == 0);
                ::close(fd);
            }
        }

        if (controller_present) {
            I2cLedBackend backend;
            auto caps = backend.capabilities();
            TEST_ASSERT(!caps.supported_leds.empty());
            for (auto id : caps.supported_leds) {
                auto s = backend.read(id);
                TEST_ASSERT(s.valid);
            }
            // Any LED the capability probe rejected must also be rejected by
            // read/apply (only when the panel has fewer than 10 LEDs).
            if (caps.supported_leds.size() < all_led_ids().size()) {
                LedId missing = caps.supported_leds.front();
                for (auto id : all_led_ids()) {
                    if (!caps.is_led_supported(id)) {
                        missing = id;
                        break;
                    }
                }
                TEST_ASSERT_THROWS(backend.read(missing), HardwareException);
                LedPatch p;
                p.brightness = 10;
                TEST_ASSERT_THROWS(backend.apply(missing, p), HardwareException);
            }
        } else {
            TEST_ASSERT_THROWS(I2cLedBackend(), HardwareException);
        }
    });

    test::run_test("config: parse backend=i2c", []() {
        ServerConfig cfg;
        std::string err;
        bool ok = load_ok("[server]\nbackend=i2c\n", cfg, err);
        TEST_ASSERT(ok);
        TEST_ASSERT_EQ(cfg.backend, std::string("i2c"));
    });

    test::run_test("config: defaults to fake backend", []() {
        ServerConfig cfg;
        TEST_ASSERT_EQ(cfg.backend, std::string("fake"));
    });

    test::run_test("config: reject unknown backend", []() {
        ServerConfig cfg;
        std::string err;
        bool ok = load_ok("[server]\nbackend=serial\n", cfg, err);
        TEST_ASSERT(!ok);
        TEST_ASSERT(!err.empty());
    });
}
