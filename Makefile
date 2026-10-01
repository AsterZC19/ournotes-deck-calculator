CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Isrc -MMD -MP
BUILD    := build

CORE      := json model engine solver
OBJ       := $(addprefix $(BUILD)/,$(addsuffix .o,$(CORE)))
TESTS     := json model engine solver
TEST_BINS := $(addprefix $(BUILD)/test_,$(TESTS))
TEST_OBJ  := $(addsuffix .o,$(TEST_BINS))

.PHONY: all clean test

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

clean:
	rm -rf $(BUILD)

-include $(OBJ:.o=.d) $(BUILD)/main.d $(TEST_OBJ:.o=.d)
