#include "test_framework.h"

#include <iostream>

void run_protocol_tests();
void run_fake_backend_tests();
void run_config_tests();
void run_ownership_tests();
void run_scheduler_tests();
void run_integration_tests();
void run_access_tests();
void run_stress_tests();
void run_logger_tests();

int main() {
    std::cout << "=======================================\n";
    std::cout << "UGREEN LED Server Test Suite (Stages 1-3)\n";
    std::cout << "=======================================\n";

    run_protocol_tests();
    run_fake_backend_tests();
    run_config_tests();
    run_ownership_tests();
    run_scheduler_tests();
    run_integration_tests();
    run_access_tests();
    run_stress_tests();
    // Logger tests manipulate the process-wide logger; they run last and
    // leave it in the silent (uninitialized) state the other suites expect.
    run_logger_tests();

    std::cout << "=======================================\n";
    std::cout << "Summary: " << test::g_passed_tests << " passed, "
              << test::g_failed_tests << " failed.\n";
    std::cout << "=======================================\n";

    return (test::g_failed_tests == 0) ? 0 : 1;
}
