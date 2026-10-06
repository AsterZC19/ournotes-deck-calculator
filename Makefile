CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Isrc -MMD -MP
BUILD    := build

CORE      := json model engine solver event
OBJ       := $(addprefix $(BUILD)/,$(addsuffix .o,$(CORE)))
TESTS     := json model engine solver
TEST_BINS := $(addprefix $(BUILD)/test_,$(TESTS))
TEST_OBJ  := $(addsuffix .o,$(TEST_BINS))

.PHONY: all clean test format format-check

all: $(BUILD)/deckcalc

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/test_%.o: tests/test_%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/deckcalc: $(OBJ) $(BUILD)/main.o
	$(CXX) $(CXXFLAGS) $^ -o $@

$(TEST_BINS): $(BUILD)/test_%: $(OBJ) $(BUILD)/test_%.o
	$(CXX) $(CXXFLAGS) $^ -o $@

test: $(BUILD)/deckcalc $(TEST_BINS)
	@for t in $(TESTS); do ./$(BUILD)/test_$$t || exit 1; done
	python3 -m unittest discover -s tests -p test_account.py
	python3 -m unittest discover -s tests -p test_normal_context.py
	python3 tests/test_event.py
	python3 tests/test_event_bounds.py
	python3 tests/test_search.py
	python3 tests/test_theoretical.py
	python3 tests/test_topk.py
	python3 tests/test_warm_start.py

format:
	clang-format -i src/*.cpp src/*.hpp tests/*.cpp
	python3 -m black tools tests

format-check:
	clang-format --dry-run --Werror src/*.cpp src/*.hpp tests/*.cpp
	python3 -m black --check tools tests

clean:
	rm -rf $(BUILD)

-include $(OBJ:.o=.d) $(BUILD)/main.d $(TEST_OBJ:.o=.d)
