#include "scheduler.h"
#include "test_framework.h"

using namespace ugreen;

void run_scheduler_tests() {
    std::cout << "Running Scheduler Tests...\n";

    test::run_test("Update coalescing produces single write with newest value", []() {
        Scheduler sched({LedId::Disk1, LedId::Disk2});

        // 20 rapid brightness updates to Disk1
        for (int b = 1; b <= 20; ++b) {
            LedPatch p;
            p.brightness = static_cast<uint8_t>(b * 10);
            sched.apply_patch(LedId::Disk1, p);
        }

        TEST_ASSERT(sched.has_dirty());
        TEST_ASSERT_EQ(sched.get_desired(LedId::Disk1).brightness, 200);

        auto batch = sched.generate_batch();
        TEST_ASSERT(batch.has_value());
        TEST_ASSERT_EQ(batch->operations.size(), 1U);
        TEST_ASSERT_EQ(batch->operations[0].id, LedId::Disk1);
        TEST_ASSERT(batch->operations[0].patch.brightness.has_value());
        TEST_ASSERT_EQ(*batch->operations[0].patch.brightness, 200);

        // While batch is in flight, generate_batch returns nullopt (bounded FIFO)
        auto second = sched.generate_batch();
        TEST_ASSERT(!second.has_value());

        // Complete the batch
        HardwareBatchResult res;
        res.batch_id = batch->batch_id;
        res.max_generation = batch->max_generation;
        res.success = true;
        res.applied_states.push_back({LedId::Disk1, batch->operations[0].target_state});
        sched.on_batch_complete(res);

        // After completion, applied state equals desired state and not dirty
        TEST_ASSERT(!sched.has_dirty());
        TEST_ASSERT_EQ(sched.get_applied(LedId::Disk1).brightness, 200);
    });

    test::run_test("No redundant hardware writes if state unchanged", []() {
        Scheduler sched({LedId::Disk1});
        TEST_ASSERT(!sched.has_dirty());

        // Applying identical patch as already applied produces no operations in batch
        LedPatch p;
        p.brightness = 0; // already 0 initially
        sched.apply_patch(LedId::Disk1, p);

        auto batch = sched.generate_batch();
        TEST_ASSERT(!batch.has_value());
        TEST_ASSERT(!sched.has_dirty());
    });

    test::run_test("Zero color/brightness drift in off/on modes is not chased", []() {
        Scheduler sched({LedId::Disk1});

        // The LED is off with retained (non-zero) color/brightness registers,
        // e.g. the chip's power-on defaults.
        LedState hw;
        hw.mode = OpMode::Off;
        hw.brightness = 200;
        hw.rgb = RgbColor{255, 0, 0};
        sched.calibrate(LedId::Disk1, hw);

        // The default fallback target: off with zero color/brightness. The
        // chip acknowledges but ignores zero values in off/on modes, so this
        // can never converge: no hardware write must be generated, and the
        // slot must not stay dirty (retrying forever would flood the log).
        LedPatch p;
        p.brightness = 0;
        p.rgb = RgbColor{0, 0, 0};
        sched.apply_patch(LedId::Disk1, p);

        auto batch = sched.generate_batch();
        TEST_ASSERT(!batch.has_value());
        TEST_ASSERT(!sched.has_dirty());
        TEST_ASSERT_EQ(sched.get_applied(LedId::Disk1).brightness, 200);

        // A non-zero target while off is applied by the chip and must still
        // converge with the zero fields left alone.
        LedPatch p2;
        p2.brightness = 100;
        sched.apply_patch(LedId::Disk1, p2);
        auto batch2 = sched.generate_batch();
        TEST_ASSERT(batch2.has_value());
        TEST_ASSERT_EQ(batch2->operations.size(), 1U);
        TEST_ASSERT(batch2->operations[0].patch.brightness.has_value());
        TEST_ASSERT_EQ(*batch2->operations[0].patch.brightness, 100);
        TEST_ASSERT(!batch2->operations[0].patch.rgb.has_value());
        TEST_ASSERT(!batch2->operations[0].patch.mode.has_value());

        HardwareBatchResult res;
        res.batch_id = batch2->batch_id;
        res.max_generation = batch2->max_generation;
        res.success = true;
        res.applied_states.push_back({LedId::Disk1, batch2->operations[0].target_state});
        sched.on_batch_complete(res);
        TEST_ASSERT(!sched.has_dirty());

        // Zero brightness in on mode is also not chased.
        LedPatch p3;
        p3.mode = OpMode::On;
        p3.brightness = 0;
        sched.apply_patch(LedId::Disk1, p3);
        auto batch3 = sched.generate_batch();
        TEST_ASSERT(batch3.has_value());
        // The mode change is issued, but the zero brightness is stripped.
        TEST_ASSERT_EQ(batch3->operations.size(), 1U);
        TEST_ASSERT(batch3->operations[0].patch.mode.has_value());
        TEST_ASSERT(!batch3->operations[0].patch.brightness.has_value());
    });

    test::run_test("Short activity flashes coalesce within interval", []() {
        Scheduler sched({LedId::Disk1});

        // Client sends ON then OFF within same interval
        LedPatch p1;
        p1.mode = OpMode::On;
        sched.apply_patch(LedId::Disk1, p1);

        LedPatch p2;
        p2.mode = OpMode::Off;
        sched.apply_patch(LedId::Disk1, p2);

        // Desired state is OFF, matching applied (OFF)
        auto batch = sched.generate_batch();
        TEST_ASSERT(!batch.has_value());
        TEST_ASSERT(!sched.has_dirty());
    });

    test::run_test("FLUSH barrier successful completion", []() {
        Scheduler sched({LedId::Disk1});

        LedPatch p;
        p.brightness = 150;
        uint64_t gen = sched.apply_patch(LedId::Disk1, p);

        bool barrier_called = false;
        bool barrier_success = false;

        sched.add_flush_barrier(1, 10, gen, [&](bool ok, const std::string& /*err*/) {
            barrier_called = true;
            barrier_success = ok;
        });

        TEST_ASSERT(!barrier_called);

        auto batch = sched.generate_batch();
        TEST_ASSERT(batch.has_value());

        HardwareBatchResult res;
        res.batch_id = batch->batch_id;
        res.max_generation = batch->max_generation;
        res.success = true;
        res.applied_states.push_back({LedId::Disk1, batch->operations[0].target_state});
        sched.on_batch_complete(res);

        TEST_ASSERT(barrier_called);
        TEST_ASSERT(barrier_success);
    });

    test::run_test("Mode transition to blink without timing uses blink default cycle", []() {
        Scheduler sched({LedId::Disk1});

        LedPatch p;
        p.mode = OpMode::Blink;
        sched.apply_patch(LedId::Disk1, p);

        auto d = sched.get_desired(LedId::Disk1);
        TEST_ASSERT_EQ(d.mode, OpMode::Blink);
        TEST_ASSERT_EQ(d.on_ms, 500);
        TEST_ASSERT_EQ(d.off_ms, 500);
    });

    test::run_test("Mode transition to breath from blink uses breath default, not inherited blink cycle", []() {
        Scheduler sched({LedId::Disk1});

        LedPatch b;
        b.mode = OpMode::Blink;
        b.on_ms = 250;
        b.off_ms = 250;
        sched.apply_patch(LedId::Disk1, b);

        LedPatch r;
        r.mode = OpMode::Breath;
        sched.apply_patch(LedId::Disk1, r);

        auto d = sched.get_desired(LedId::Disk1);
        TEST_ASSERT_EQ(d.mode, OpMode::Breath);
        TEST_ASSERT_EQ(d.on_ms, 1000);
        TEST_ASSERT_EQ(d.off_ms, 1000);
    });

    test::run_test("Mode transition to blink from breath uses blink default, not inherited breath cycle", []() {
        Scheduler sched({LedId::Disk1});

        LedPatch r;
        r.mode = OpMode::Breath;
        r.on_ms = 1000;
        r.off_ms = 1000;
        sched.apply_patch(LedId::Disk1, r);

        LedPatch b;
        b.mode = OpMode::Blink;
        sched.apply_patch(LedId::Disk1, b);

        auto d = sched.get_desired(LedId::Disk1);
        TEST_ASSERT_EQ(d.mode, OpMode::Blink);
        TEST_ASSERT_EQ(d.on_ms, 500);
        TEST_ASSERT_EQ(d.off_ms, 500);
    });

    test::run_test("Same-mode patch without timing keeps the existing cycle", []() {
        Scheduler sched({LedId::Disk1});

        LedPatch b;
        b.mode = OpMode::Blink;
        b.on_ms = 250;
        b.off_ms = 250;
        sched.apply_patch(LedId::Disk1, b);

        LedPatch again;
        again.mode = OpMode::Blink;
        sched.apply_patch(LedId::Disk1, again);

        auto d = sched.get_desired(LedId::Disk1);
        TEST_ASSERT_EQ(d.mode, OpMode::Blink);
        TEST_ASSERT_EQ(d.on_ms, 250);
        TEST_ASSERT_EQ(d.off_ms, 250);
    });

    test::run_test("Timing without mode implies blink in the desired state", []() {
        Scheduler sched({LedId::Disk1});

        LedPatch on;
        on.mode = OpMode::On;
        sched.apply_patch(LedId::Disk1, on);

        LedPatch t;
        t.on_ms = 100;
        t.off_ms = 100;
        sched.apply_patch(LedId::Disk1, t);

        auto d = sched.get_desired(LedId::Disk1);
        TEST_ASSERT_EQ(d.mode, OpMode::Blink);
        TEST_ASSERT_EQ(d.on_ms, 100);
        TEST_ASSERT_EQ(d.off_ms, 100);
    });

    test::run_test("Off mode clears timing from the desired state", []() {
        Scheduler sched({LedId::Disk1});

        LedPatch b;
        b.mode = OpMode::Blink;
        b.on_ms = 250;
        b.off_ms = 250;
        sched.apply_patch(LedId::Disk1, b);

        LedPatch off;
        off.mode = OpMode::Off;
        sched.apply_patch(LedId::Disk1, off);

        auto d = sched.get_desired(LedId::Disk1);
        TEST_ASSERT_EQ(d.mode, OpMode::Off);
        TEST_ASSERT_EQ(d.on_ms, 0);
        TEST_ASSERT_EQ(d.off_ms, 0);
    });

    test::run_test("Blink to off to blink sequence stays consistent", []() {
        Scheduler sched({LedId::Disk1});

        // blink with an explicit cycle
        LedPatch b1;
        b1.mode = OpMode::Blink;
        b1.on_ms = 250;
        b1.off_ms = 250;
        sched.apply_patch(LedId::Disk1, b1);

        auto batch1 = sched.generate_batch();
        TEST_ASSERT(batch1.has_value());
        TEST_ASSERT_EQ(batch1->operations.size(), 1U);
        HardwareBatchResult r1;
        r1.batch_id = batch1->batch_id;
        r1.max_generation = batch1->max_generation;
        r1.success = true;
        r1.applied_states.push_back({LedId::Disk1, batch1->operations[0].target_state});
        sched.on_batch_complete(r1);
        TEST_ASSERT(!sched.has_dirty());

        // off
        LedPatch off;
        off.mode = OpMode::Off;
        sched.apply_patch(LedId::Disk1, off);
        auto batch2 = sched.generate_batch();
        TEST_ASSERT(batch2.has_value());
        HardwareBatchResult r2;
        r2.batch_id = batch2->batch_id;
        r2.max_generation = batch2->max_generation;
        r2.success = true;
        r2.applied_states.push_back({LedId::Disk1, batch2->operations[0].target_state});
        sched.on_batch_complete(r2);

        // blink again without timing: must be a valid, non-zero cycle
        LedPatch b2;
        b2.mode = OpMode::Blink;
        sched.apply_patch(LedId::Disk1, b2);
        auto d = sched.get_desired(LedId::Disk1);
        TEST_ASSERT_EQ(d.mode, OpMode::Blink);
        TEST_ASSERT(d.on_ms + d.off_ms > 0);

        auto batch3 = sched.generate_batch();
        TEST_ASSERT(batch3.has_value());
        // The batch must carry a complete, legal blink state (the original
        // bug: this transition produced a zero-cycle or missing mode write).
        auto& op3 = batch3->operations[0];
        TEST_ASSERT(op3.patch.mode.has_value());
        TEST_ASSERT_EQ(*op3.patch.mode, OpMode::Blink);
        TEST_ASSERT(op3.target_state.on_ms + op3.target_state.off_ms > 0);

        HardwareBatchResult r3;
        r3.batch_id = batch3->batch_id;
        r3.max_generation = batch3->max_generation;
        r3.success = true;
        r3.applied_states.push_back({LedId::Disk1, batch3->operations[0].target_state});
        sched.on_batch_complete(r3);
        TEST_ASSERT(!sched.has_dirty());
        TEST_ASSERT(sched.get_desired(LedId::Disk1) == sched.get_applied(LedId::Disk1));
    });

    test::run_test("Fallback blink state with zero cycle is normalized", []() {
        Scheduler sched({LedId::Disk1});

        LedState fb;
        fb.mode = OpMode::Blink;  // on_ms/off_ms left at 0 (unconfigured)
        sched.set_fallback(LedId::Disk1, fb);

        sched.apply_fallback(LedId::Disk1);

        auto d = sched.get_desired(LedId::Disk1);
        TEST_ASSERT_EQ(d.mode, OpMode::Blink);
        TEST_ASSERT_EQ(d.on_ms, 500);
        TEST_ASSERT_EQ(d.off_ms, 500);
    });

    test::run_test("FLUSH barrier error propagation on hardware failure", []() {
        Scheduler sched({LedId::Disk1});

        LedPatch p;
        p.brightness = 150;
        uint64_t gen = sched.apply_patch(LedId::Disk1, p);

        bool barrier_called = false;
        bool barrier_success = true;
        std::string barrier_err;

        sched.add_flush_barrier(1, 10, gen, [&](bool ok, const std::string& err) {
            barrier_called = true;
            barrier_success = ok;
            barrier_err = err;
        });

        auto batch = sched.generate_batch();
        TEST_ASSERT(batch.has_value());

        HardwareBatchResult res;
        res.batch_id = batch->batch_id;
        res.max_generation = batch->max_generation;
        res.success = false;
        res.failed_led = LedId::Disk1;
        res.error_message = "I2C bus timeout";
        sched.on_batch_complete(res);

        TEST_ASSERT(barrier_called);
        TEST_ASSERT(!barrier_success);
        TEST_ASSERT_EQ(barrier_err, "I2C bus timeout");
    });
}
