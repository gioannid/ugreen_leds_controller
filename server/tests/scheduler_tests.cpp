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
