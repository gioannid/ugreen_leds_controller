#include "test_framework.h"

#include <iostream>

void run_protocol_tests();
void run_fake_backend_tests();
void run_ownership_tests();
void run_scheduler_tests();
void run_integration_tests();

int main() {
    std::cout << "=======================================\n";
    std::cout << "UGREEN LED Server Test Suite (Stages 1 & 2)\n";
    std::cout << "=======================================\n";

    run_protocol_tests();
    run_fake_backend_tests();
    run_ownership_tests();
    run_scheduler_tests();
    run_integration_tests();

    std::cout << "=======================================\n";
    std::cout << "Summary: " << test::g_passed_tests << " passed, "
              << test::g_failed_tests << " failed.\n";
    std::cout << "=======================================\n";

    return (test::g_failed_tests == 0) ? 0 : 1;
}
