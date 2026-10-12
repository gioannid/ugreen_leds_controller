#include "fake_backend.h"
#include "test_framework.h"

using namespace ugreen;

void run_fake_backend_tests() {
    std::cout << "Running FakeLedBackend Tests...\n";

    test::run_test("Initial state and capabilities", []() {
        FakeLedBackend backend({LedId::Power, LedId::Netdev, LedId::Disk1, LedId::Disk2});
        auto caps = backend.capabilities();

        TEST_ASSERT(caps.is_led_supported(LedId::Power));
        TEST_ASSERT(caps.is_led_supported(LedId::Netdev));
        TEST_ASSERT(caps.is_led_supported(LedId::Disk1));
        TEST_ASSERT(caps.is_led_supported(LedId::Disk2));
        TEST_ASSERT(!caps.is_led_supported(LedId::Disk5));

        auto s = backend.read(LedId::Disk1);
        TEST_ASSERT(s.valid);
        TEST_ASSERT_EQ(s.mode, OpMode::Off);
        TEST_ASSERT_EQ(s.brightness, 0);
    });

    test::run_test("Apply patch and read back", []() {
        FakeLedBackend backend;
        LedPatch patch;
        patch.mode = OpMode::On;
        patch.brightness = 192;
        patch.rgb = RgbColor{0, 255, 128};

        backend.apply(LedId::Disk1, patch);
        auto s = backend.read(LedId::Disk1);

        TEST_ASSERT_EQ(s.mode, OpMode::On);
        TEST_ASSERT_EQ(s.brightness, 192);
        TEST_ASSERT_EQ(s.rgb.r, 0);
        TEST_ASSERT_EQ(s.rgb.g, 255);
        TEST_ASSERT_EQ(s.rgb.b, 128);

        TEST_ASSERT_EQ(backend.apply_count(LedId::Disk1), 1U);
        TEST_ASSERT_EQ(backend.history().size(), 2U); // 1 apply, 1 read
    });

    test::run_test("Unsupported LED throws HardwareException", []() {
        FakeLedBackend backend({LedId::Disk1});
        TEST_ASSERT_THROWS(backend.read(LedId::Disk8), HardwareException);

        LedPatch p;
        p.brightness = 10;
        TEST_ASSERT_THROWS(backend.apply(LedId::Disk8, p), HardwareException);
    });

    test::run_test("Injected apply failure", []() {
        FakeLedBackend backend;
        backend.inject_apply_failure(true);

        LedPatch p;
        p.brightness = 100;
        TEST_ASSERT_THROWS(backend.apply(LedId::Disk1, p), HardwareException);

        backend.inject_apply_failure(false);
        backend.apply(LedId::Disk1, p);
        TEST_ASSERT_EQ(backend.read(LedId::Disk1).brightness, 100);
    });

    test::run_test("Injected read failure", []() {
        FakeLedBackend backend;
        backend.inject_read_failure(true);
        TEST_ASSERT_THROWS(backend.read(LedId::Disk1), HardwareException);

        backend.inject_read_failure(false);
        auto s = backend.read(LedId::Disk1);
        TEST_ASSERT(s.valid);
    });
}
