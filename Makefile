CXX      ?= clang++
CXXFLAGS ?= -std=c++20 -O3 -Wall -Wextra -Wpedantic -Iinclude
LDFLAGS  ?= -pthread

BUILD := build
LIB_SRC := src/order_book.cpp

.PHONY: all test bench demo sanitize clean

all: $(BUILD)/tests $(BUILD)/bench $(BUILD)/demo

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/tests: tests/test_main.cpp $(LIB_SRC) include/lob/*.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) tests/test_main.cpp $(LIB_SRC) -o $@ $(LDFLAGS)

$(BUILD)/bench: bench/bench_main.cpp $(LIB_SRC) include/lob/*.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) bench/bench_main.cpp $(LIB_SRC) -o $@ $(LDFLAGS)

$(BUILD)/demo: examples/demo.cpp $(LIB_SRC) include/lob/*.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) examples/demo.cpp $(LIB_SRC) -o $@ $(LDFLAGS)

demo: $(BUILD)/demo
	./$(BUILD)/demo

test: $(BUILD)/tests
	./$(BUILD)/tests

bench: $(BUILD)/bench
	./$(BUILD)/bench

# Tests under AddressSanitizer + UndefinedBehaviorSanitizer (memory bugs, UB),
# and separately under ThreadSanitizer (data races in the SPSC queue).
sanitize: | $(BUILD)
	$(CXX) -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Iinclude \
		tests/test_main.cpp $(LIB_SRC) -o $(BUILD)/tests_asan $(LDFLAGS)
	./$(BUILD)/tests_asan
	$(CXX) -std=c++20 -O1 -g -fsanitize=thread -Iinclude \
		tests/test_main.cpp $(LIB_SRC) -o $(BUILD)/tests_tsan $(LDFLAGS)
	./$(BUILD)/tests_tsan

clean:
	rm -rf $(BUILD)
