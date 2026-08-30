# Defaults to clang++ (the dev machine); the Linux container overrides to g++.
CXX      ?= clang++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra
CPPFLAGS += -Iinclude -Isrc
# sw_demo stays at -O0 with frame pointers so captured backtraces reliably
# contain stall_here for the symbolizer test.
DEMOFLAGS = -std=c++20 -O0 -g -fno-omit-frame-pointer -Wall -Wextra

UNAME_S := $(shell uname -s)
LDLIBS  :=
# SW_REMOTE=1 builds the non-cooperative ptrace + libunwind capture path into
# stallwatchd (Linux with libunwind-dev installed; on by default there).
ifeq ($(UNAME_S),Linux)
LDLIBS += -lrt -pthread
SW_REMOTE ?= 1
else
SW_REMOTE ?= 0
endif
LDLIBS_REMOTE :=
ifeq ($(SW_REMOTE),1)
CPPFLAGS += -DSW_REMOTE=1
LDLIBS_REMOTE += -lunwind-ptrace -lunwind-generic
endif

BIN  := bin
HDRS := include/stallwatch/stallwatch.hpp src/shm.hpp src/detect.hpp src/report.hpp src/remote_unwind.hpp
BINS := $(BIN)/stallwatchd $(BIN)/sw_demo $(BIN)/test_unit $(BIN)/bench_beat

all: $(BINS)

$(BIN):
	mkdir -p $(BIN)

$(BIN)/stallwatchd: src/monitor.cpp src/shm.cpp src/remote_unwind.cpp $(HDRS) | $(BIN)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) src/monitor.cpp src/shm.cpp src/remote_unwind.cpp -o $@ $(LDLIBS) $(LDLIBS_REMOTE)

$(BIN)/sw_demo: src/demo.cpp include/stallwatch/stallwatch.hpp | $(BIN)
	$(CXX) $(DEMOFLAGS) $(CPPFLAGS) src/demo.cpp -o $@ $(LDLIBS)

$(BIN)/test_unit: tests/test_unit.cpp src/shm.cpp $(HDRS) | $(BIN)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) tests/test_unit.cpp src/shm.cpp -o $@ $(LDLIBS)

$(BIN)/bench_beat: bench/bench_beat.cpp include/stallwatch/stallwatch.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) bench/bench_beat.cpp -o $@ $(LDLIBS)

test: all
	./$(BIN)/test_unit
	tests/test_integration.sh
	tests/test_integration_remote.sh

bench: all
	./$(BIN)/bench_beat | tee bench/results.txt
	bench/bench_detect.sh | tee -a bench/results.txt

clean:
	rm -rf $(BIN) stallwatch_report.txt bench/detect_report.txt

.PHONY: all test bench clean
