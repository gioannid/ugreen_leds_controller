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
              src/logger.cpp

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
           src/logger.o

TEST_SRCS = tests/test_main.cpp \
            tests/protocol_tests.cpp \
            tests/fake_backend_tests.cpp \
            tests/config_tests.cpp \
            tests/logger_tests.cpp \
            tests/ownership_tests.cpp \
            tests/scheduler_tests.cpp \
            tests/integration_tests.cpp \
            tests/access_tests.cpp \
            tests/stress_tests.cpp

TEST_OBJS = $(TEST_SRCS:.cpp=.o)

all: ugreen_leds_server test_runner example_client

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

cli_i2c.o: ../cli/i2c.cpp ../cli/i2c.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

cli_ugreen_leds.o: ../cli/ugreen_leds.cpp ../cli/ugreen_leds.h ../cli/i2c.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

ugreen_leds_server: $(SERVER_OBJS) $(COMMON_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

test_runner: $(LIB_OBJS) $(TEST_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

example_client: examples/client.o
	$(CXX) $(CXXFLAGS) -o $@ $^

test: test_runner
	./test_runner

clean:
	rm -f src/*.o tests/*.o examples/*.o $(COMMON_OBJS) ugreen_leds_server test_runner example_client

.PHONY: all test clean
