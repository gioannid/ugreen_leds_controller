#ifndef UGREEN_SERVER_TEST_FRAMEWORK_H
#define UGREEN_SERVER_TEST_FRAMEWORK_H

#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace test {

inline int g_passed_tests = 0;
inline int g_failed_tests = 0;

inline void run_test(const std::string& name, const std::function<void()>& fn) {
    try {
        fn();
        std::cout << "  [PASS] " << name << "\n";
        ++g_passed_tests;
    } catch (const std::exception& ex) {
        std::cerr << "  [FAIL] " << name << ": " << ex.what() << "\n";
        ++g_failed_tests;
    } catch (...) {
        std::cerr << "  [FAIL] " << name << ": unknown exception\n";
        ++g_failed_tests;
    }
}

#define TEST_ASSERT(cond) \
    do { \
        if (!(cond)) { \
            throw std::runtime_error(std::string("Assertion failed: (") + #cond + ") at " + __FILE__ + ":" + std::to_string(__LINE__)); \
        } \
    } while (0)

#define TEST_ASSERT_EQ(a, b) \
    do { \
        if (!((a) == (b))) { \
            std::ostringstream ss; \
            ss << "Assertion failed: (" << #a << " == " << #b << ") at " << __FILE__ << ":" << __LINE__; \
            throw std::runtime_error(ss.str()); \
        } \
    } while (0)

#define TEST_ASSERT_THROWS(expr, ExceptionType) \
    do { \
        bool threw = false; \
        try { \
            expr; \
        } catch (const ExceptionType&) { \
            threw = true; \
        } catch (...) { \
            throw std::runtime_error(std::string("Expected ") + #ExceptionType + " but caught different exception at " + __FILE__ + ":" + std::to_string(__LINE__)); \
        } \
        if (!threw) { \
            throw std::runtime_error(std::string("Expected exception ") + #ExceptionType + " not thrown at " + __FILE__ + ":" + std::to_string(__LINE__)); \
        } \
    } while (0)

} // namespace test

#endif // UGREEN_SERVER_TEST_FRAMEWORK_H
