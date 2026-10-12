#include "ownership.h"
#include "test_framework.h"

using namespace ugreen;

void run_ownership_tests() {
    std::cout << "Running Ownership Tests...\n";

    test::run_test("Implicit ownership acquisition", []() {
        OwnershipManager mgr;
        TEST_ASSERT(!mgr.get_owner(LedId::Disk1).has_value());

        // Client 1 with priority 50 acquires disk1
        TEST_ASSERT(mgr.can_mutate(LedId::Disk1, 1, 50));
        TEST_ASSERT(mgr.acquire_or_preempt(LedId::Disk1, 1, 50));

        auto owner = mgr.get_owner(LedId::Disk1);
        TEST_ASSERT(owner.has_value());
        TEST_ASSERT_EQ(owner->client_id, 1ULL);
        TEST_ASSERT_EQ(owner->priority, 50);

        // Same client can mutate again
        TEST_ASSERT(mgr.can_mutate(LedId::Disk1, 1, 50));
    });

    test::run_test("Priority preemption and rejection", []() {
        OwnershipManager mgr;
        mgr.acquire_or_preempt(LedId::Disk1, 1, 50);

        // Client 2 with lower priority (100 > 50) is rejected
        TEST_ASSERT(!mgr.can_mutate(LedId::Disk1, 2, 100));
        TEST_ASSERT(!mgr.acquire_or_preempt(LedId::Disk1, 2, 100));

        // Client 2 with equal priority (50) is rejected
        TEST_ASSERT(!mgr.can_mutate(LedId::Disk1, 2, 50));
        TEST_ASSERT(!mgr.acquire_or_preempt(LedId::Disk1, 2, 50));

        // Client 3 with strictly higher priority (10 < 50) preempts!
        TEST_ASSERT(mgr.can_mutate(LedId::Disk1, 3, 10));
        TEST_ASSERT(mgr.acquire_or_preempt(LedId::Disk1, 3, 10));

        auto owner = mgr.get_owner(LedId::Disk1);
        TEST_ASSERT_EQ(owner->client_id, 3ULL);
        TEST_ASSERT_EQ(owner->priority, 10);
    });

    test::run_test("Atomic multi-LED acquisition", []() {
        OwnershipManager mgr;
        std::vector<LedId> leds = {LedId::Disk1, LedId::Disk2, LedId::Disk3};

        // Client 1 owns disk2 with high priority 5
        mgr.acquire_or_preempt(LedId::Disk2, 1, 5);

        // Client 2 with priority 20 tries to acquire all 3 LEDs
        // Disk1 is free, Disk3 is free, but Disk2 is held with priority 5 (< 20)
        TEST_ASSERT(!mgr.can_mutate_all(leds, 2, 20));
        TEST_ASSERT(!mgr.acquire_or_preempt_all(leds, 2, 20));

        // None of the other LEDs should have been acquired by client 2
        TEST_ASSERT(!mgr.get_owner(LedId::Disk1).has_value());
        TEST_ASSERT(!mgr.get_owner(LedId::Disk3).has_value());
        TEST_ASSERT_EQ(mgr.get_owner(LedId::Disk2)->client_id, 1ULL);

        // Client 3 with priority 0 (highest) acquires all
        TEST_ASSERT(mgr.can_mutate_all(leds, 3, 0));
        TEST_ASSERT(mgr.acquire_or_preempt_all(leds, 3, 0));
        TEST_ASSERT_EQ(mgr.get_owner(LedId::Disk1)->client_id, 3ULL);
        TEST_ASSERT_EQ(mgr.get_owner(LedId::Disk2)->client_id, 3ULL);
        TEST_ASSERT_EQ(mgr.get_owner(LedId::Disk3)->client_id, 3ULL);
    });

    test::run_test("RESET command clearing ownership", []() {
        OwnershipManager mgr;
        mgr.acquire_or_preempt(LedId::Disk1, 1, 50);

        // Client 2 with lower priority cannot reset Client 1's LED
        TEST_ASSERT(!mgr.reset(LedId::Disk1, 2, 80));
        TEST_ASSERT(mgr.get_owner(LedId::Disk1).has_value());

        // Client 1 resets own LED
        TEST_ASSERT(mgr.reset(LedId::Disk1, 1, 50));
        TEST_ASSERT(!mgr.get_owner(LedId::Disk1).has_value());
    });

    test::run_test("Disconnect releases all owned LEDs", []() {
        OwnershipManager mgr;
        mgr.acquire_or_preempt(LedId::Disk1, 10, 50);
        mgr.acquire_or_preempt(LedId::Disk2, 10, 50);
        mgr.acquire_or_preempt(LedId::Power, 20, 50);

        auto released = mgr.release_client(10);
        TEST_ASSERT_EQ(released.size(), 2U);
        TEST_ASSERT(!mgr.get_owner(LedId::Disk1).has_value());
        TEST_ASSERT(!mgr.get_owner(LedId::Disk2).has_value());
        TEST_ASSERT(mgr.get_owner(LedId::Power).has_value());
    });
}
