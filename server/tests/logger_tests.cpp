#include "logger.h"
#include "test_framework.h"

#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace ugreen;

static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void run_logger_tests() {
    std::cout << "Running Logger Tests...\n";

    test::run_test("Logging before init is a no-op", []() {
        auto& logger = Logger::instance();
        TEST_ASSERT(!logger.initialized());
        TEST_ASSERT(!logger.enabled(LogLevel::Error));
        logger.log(LogLevel::Error, "must not be written anywhere");
    });

    test::run_test("Default init logs info and above to screen", []() {
        auto& logger = Logger::instance();
        std::string err;
        TEST_ASSERT(logger.init(LogConfig{"", false}, err));
        TEST_ASSERT(logger.initialized());
        TEST_ASSERT(logger.enabled(LogLevel::Info));
        TEST_ASSERT(logger.enabled(LogLevel::Warn));
        TEST_ASSERT(logger.enabled(LogLevel::Error));
        TEST_ASSERT(!logger.enabled(LogLevel::Debug));
        logger.shutdown();
        TEST_ASSERT(!logger.initialized());
        TEST_ASSERT(!logger.enabled(LogLevel::Error));
    });

    test::run_test("Verbose init enables debug level", []() {
        auto& logger = Logger::instance();
        std::string err;
        TEST_ASSERT(logger.init(LogConfig{"", true}, err));
        TEST_ASSERT(logger.enabled(LogLevel::Debug));
        TEST_ASSERT(logger.enabled(LogLevel::Info));
        logger.shutdown();
    });

    test::run_test("File target writes formatted lines, hides debug without verbose", []() {
        std::string path = "/tmp/logger_test_" + std::to_string(getpid()) + ".log";
        std::remove(path.c_str());

        auto& logger = Logger::instance();
        std::string err;
        TEST_ASSERT(logger.init(LogConfig{path, false}, err));
        log::info("hello info");
        log::warn("hello warn");
        log::error("hello error");
        log::debug("hidden debug");
        logger.shutdown();

        std::string content = read_file(path);
        TEST_ASSERT(content.find("[INFO] hello info") != std::string::npos);
        TEST_ASSERT(content.find("[WARN] hello warn") != std::string::npos);
        TEST_ASSERT(content.find("[ERROR] hello error") != std::string::npos);
        TEST_ASSERT(content.find("hidden debug") == std::string::npos);

        // Timestamp prefix: YYYY-MM-DDTHH:MM:SS.mmmZ [LEVEL]
        TEST_ASSERT(content.size() > 26);
        TEST_ASSERT(content[4] == '-' && content[7] == '-' && content[10] == 'T');
        TEST_ASSERT(content[13] == ':' && content[16] == ':' && content[19] == '.');
        TEST_ASSERT(content[23] == 'Z' && content[25] == '[');

        std::remove(path.c_str());
    });

    test::run_test("File target appends across init/shutdown cycles", []() {
        std::string path = "/tmp/logger_test_append_" + std::to_string(getpid()) + ".log";
        std::remove(path.c_str());

        auto& logger = Logger::instance();
        std::string err;
        TEST_ASSERT(logger.init(LogConfig{path, false}, err));
        log::info("first run");
        logger.shutdown();
        TEST_ASSERT(logger.init(LogConfig{path, false}, err));
        log::info("second run");
        logger.shutdown();

        std::string content = read_file(path);
        TEST_ASSERT(content.find("[INFO] first run") != std::string::npos);
        TEST_ASSERT(content.find("[INFO] second run") != std::string::npos);

        std::remove(path.c_str());
    });

    test::run_test("Verbose file target includes debug lines", []() {
        std::string path = "/tmp/logger_test_verbose_" + std::to_string(getpid()) + ".log";
        std::remove(path.c_str());

        auto& logger = Logger::instance();
        std::string err;
        TEST_ASSERT(logger.init(LogConfig{path, true}, err));
        log::debug("visible debug");
        log::info("visible info");
        logger.shutdown();

        std::string content = read_file(path);
        TEST_ASSERT(content.find("[DEBUG] visible debug") != std::string::npos);
        TEST_ASSERT(content.find("[INFO] visible info") != std::string::npos);

        std::remove(path.c_str());
    });

    test::run_test("Unopenable file fails init with error", []() {
        auto& logger = Logger::instance();
        std::string err;
        TEST_ASSERT(!logger.init(LogConfig{"/nonexistent_dir_xyz/log.txt", false}, err));
        TEST_ASSERT(!err.empty());
        TEST_ASSERT(err.find("/nonexistent_dir_xyz/log.txt") != std::string::npos);
        TEST_ASSERT(!logger.initialized());
    });

    test::run_test("Level ordering maps to distinct tags", []() {
        TEST_ASSERT_EQ(std::string(to_string(LogLevel::Debug)), std::string("DEBUG"));
        TEST_ASSERT_EQ(std::string(to_string(LogLevel::Info)), std::string("INFO"));
        TEST_ASSERT_EQ(std::string(to_string(LogLevel::Warn)), std::string("WARN"));
        TEST_ASSERT_EQ(std::string(to_string(LogLevel::Error)), std::string("ERROR"));
        TEST_ASSERT(LogLevel::Debug < LogLevel::Info);
        TEST_ASSERT(LogLevel::Info < LogLevel::Warn);
        TEST_ASSERT(LogLevel::Warn < LogLevel::Error);
    });
}
