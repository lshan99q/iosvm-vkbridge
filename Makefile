# SPDX-License-Identifier: MIT
#
# IOSVM host-side Vulkan backend (vkbridge).
# Everything is headless: no window system is required or used.

CXX ?= g++
AR ?= ar
PKG_CONFIG ?= pkg-config

VULKAN_CFLAGS := $(shell $(PKG_CONFIG) --cflags vulkan 2>/dev/null)
VULKAN_LIBS := $(shell $(PKG_CONFIG) --libs vulkan 2>/dev/null)

CXXFLAGS ?= -std=c++20 -Wall -Wextra -Werror -O2 -g
SANITIZE_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer
CPPFLAGS += -Iinclude

BUILD := build
OBJ := $(BUILD)/obj

LIB_SOURCES := $(wildcard src/*.cpp)
LIB_OBJECTS := $(patsubst src/%.cpp,$(OBJ)/%.o,$(LIB_SOURCES))
LIB := $(BUILD)/libvkbridge.a

PROBE := $(BUILD)/vkbridge-probe
PROBE_SOURCE := tools/vkbridge-probe.cpp
PROBE_OBJECT := $(OBJ)/vkbridge-probe.o

TEST_SOURCES := $(wildcard tests/*.cpp)
TEST_BINARY := $(BUILD)/vkbridge-tests
TEST_ASAN_BINARY := $(BUILD)/vkbridge-tests-asan
PROBE_ASAN := $(BUILD)/vkbridge-probe-asan
SCREENSHOT := $(BUILD)/vkbridge-probe.png

.PHONY: all probe test test-asan probe-asan clean help format-check

all: $(LIB) $(PROBE)

probe: $(PROBE)

$(OBJ)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(VULKAN_CFLAGS) -MMD -MP -c $< -o $@

$(PROBE_OBJECT): $(PROBE_SOURCE)
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(VULKAN_CFLAGS) -MMD -MP -c $< -o $@

$(LIB): $(LIB_OBJECTS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(PROBE): $(PROBE_OBJECT) $(LIB)
	$(CXX) $(CXXFLAGS) $(PROBE_OBJECT) $(LIB) $(VULKAN_LIBS) -o $@

$(TEST_BINARY): $(TEST_SOURCES) $(LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(VULKAN_CFLAGS) $(TEST_SOURCES) $(LIB) $(VULKAN_LIBS) -o $@

$(PROBE_ASAN): $(PROBE_SOURCE) $(LIB_SOURCES)
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SANITIZE_FLAGS) $(VULKAN_CFLAGS) $(PROBE_SOURCE) \
		$(LIB_SOURCES) $(VULKAN_LIBS) -o $@

$(TEST_ASAN_BINARY): $(TEST_SOURCES) $(LIB_SOURCES)
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SANITIZE_FLAGS) $(VULKAN_CFLAGS) $(TEST_SOURCES) \
		$(LIB_SOURCES) $(VULKAN_LIBS) -o $@

# Unit tests need no GPU. The probe is the GPU integration test; when no physical
# device can be opened the Vulkan half is skipped instead of failing.
test: all $(TEST_BINARY)
	./$(TEST_BINARY)
	@if ./$(PROBE) --device-check >/dev/null 2>&1; then \
		./$(PROBE) --screenshot $(SCREENSHOT); \
	else \
		echo "[skip] no Vulkan device"; \
	fi

# ASan/UBSan is kept to the pure unit tests: Vulkan drivers commonly leak and
# trip LeakSanitizer inside the ICD, which is not our memory.
test-asan: $(TEST_ASAN_BINARY)
	./$(TEST_ASAN_BINARY)

# Optional: run the GPU probe under ASan/UBSan. Leak detection is disabled because
# some ICDs (the NVIDIA proprietary driver here) leak bookkeeping allocations that
# are outside our control; memory-error checking stays on.
probe-asan: $(PROBE_ASAN)
	ASAN_OPTIONS=detect_leaks=0 ./$(PROBE_ASAN) --screenshot $(BUILD)/vkbridge-probe-asan.png

format-check:
	clang-format --dry-run --Werror $(LIB_SOURCES) $(wildcard include/vkbridge/*.hpp) \
		tools/*.cpp tests/*.cpp tests/*.hpp

clean:
	rm -rf $(BUILD)

help:
	@echo "make            build build/libvkbridge.a and build/vkbridge-probe"
	@echo "make probe      build only the probe"
	@echo "make test       run unit tests, then the GPU probe on the selected device"
	@echo "make test-asan  build and run the pure unit tests with ASan + UBSan"
	@echo "make probe-asan run the GPU probe under ASan + UBSan (leak detection off)"
	@echo "make format-check  verify clang-format cleanliness"
	@echo "make clean      remove build/"
	@echo "make help       this text"
	@echo ""
	@echo "Lavapipe: VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json make test"

-include $(LIB_OBJECTS:.o=.d) $(PROBE_OBJECT:.o=.d)
