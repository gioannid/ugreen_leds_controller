CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -pedantic -pthread -Iinclude -Isrc -I../cli

SERVER_SRCS = src/main.cpp \
              src/led_state.cpp \
              src/protocol.cpp \
              src/config.cpp \
              src/ownership.cpp \
              src/scheduler.cpp \
              src/hardware_worker.cpp \
              src/reactor.cpp \
              src/logger.cpp \
              src/i2c_led_backend.cpp

SERVER_OBJS = $(SERVER_SRCS:.cpp=.o)

# Shared userspace controller sources, compiled from cli/ (single copy).
# Objects are written to this directory; cli/ is not modified.
COMMON_OBJS = cli_i2c.o cli_ugreen_leds.o

LIB_OBJS = src/led_state.o \
           src/protocol.o \
           src/config.o \
           src/ownership.o \
           src/scheduler.o \
           src/hardware_worker.o \
           src/reactor.o \
           src/logger.o \
           src/i2c_led_backend.o

TEST_SRCS = tests/test_main.cpp \
            tests/protocol_tests.cpp \
            tests/fake_backend_tests.cpp \
            tests/config_tests.cpp \
            tests/logger_tests.cpp \
            tests/ownership_tests.cpp \
            tests/scheduler_tests.cpp \
            tests/integration_tests.cpp \
            tests/access_tests.cpp \
            tests/stress_tests.cpp \
            tests/i2c_backend_tests.cpp

TEST_OBJS = $(TEST_SRCS:.cpp=.o)

# Real-hardware smoke test: standalone binary meant to be copied to the
# machine that owns the LED controller (needs the real HW, may ask the user
# to confirm what the LEDs do). Statically linked so it can be copied as a
# single file.
HW_TEST_SRCS = tests/hw/hardware_smoke_test.cpp
HW_TEST_OBJS = $(HW_TEST_SRCS:.cpp=.o)

all: ugreen_leds_server test_runner example_client hw_smoke_test

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

cli_i2c.o: ../cli/i2c.cpp ../cli/i2c.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

cli_ugreen_leds.o: ../cli/ugreen_leds.cpp ../cli/ugreen_leds.h ../cli/i2c.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

ugreen_leds_server: $(SERVER_OBJS) $(COMMON_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

test_runner: $(LIB_OBJS) $(TEST_OBJS) $(COMMON_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

example_client: examples/client.o
	$(CXX) $(CXXFLAGS) -o $@ $^

hw_smoke_test: $(HW_TEST_OBJS) $(LIB_OBJS) $(COMMON_OBJS)
	$(CXX) $(CXXFLAGS) -static -o $@ $^

test: test_runner
	./test_runner

clean:
	rm -f src/*.o tests/*.o tests/hw/*.o examples/*.o $(COMMON_OBJS) ugreen_leds_server test_runner example_client hw_smoke_test

.PHONY: all test clean
