ifeq ($(OS),Windows_NT)
    PLATFORM := windows
    RM       := rmdir /S /Q
    DEL      := del /Q
    EXE      := .exe
    MKDIR    := if not exist "build" mkdir build
else
    UNAME_S  := $(shell uname -s)
    ifeq ($(UNAME_S),Darwin)
        PLATFORM := macos
    else
        PLATFORM := linux
    endif
    RM       := rm -rf
    DEL      := rm -f
    EXE      :=
    MKDIR    := mkdir -p build
endif

CXX      ?= g++
IS_MSVC  := $(findstring cl,$(notdir $(CXX)))

STD      := c++17
WARN     := -Wall -Wextra -Wpedantic
OPT_REL  := -O2 -DNDEBUG
OPT_DBG  := -O1 -g -DDEBUG

ifeq ($(IS_MSVC),cl)
    CXXFLAGS_COMMON := /std:$(STD) /EHsc /W4 /permissive-
    CXXFLAGS_REL    := $(CXXFLAGS_COMMON) /O2 /DNDEBUG
    CXXFLAGS_DBG    := $(CXXFLAGS_COMMON) /Od /Zi /DDEBUG
    LDFLAGS         :=
else
    CXXFLAGS_COMMON := -std=$(STD) $(WARN) -pthread
    CXXFLAGS_REL    := $(CXXFLAGS_COMMON) $(OPT_REL)
    CXXFLAGS_DBG    := $(CXXFLAGS_COMMON) $(OPT_DBG)
    LDFLAGS         := -pthread
endif

ifeq ($(PLATFORM),windows)
    SAN_FLAGS :=
    ASAN_FLAGS:=
else
    SAN_FLAGS := -fsanitize=thread -fno-omit-frame-pointer -g
    ASAN_FLAGS:= -fsanitize=address,undefined -fno-omit-frame-pointer -g
endif

BUILD_DIR := build
BIN_REL   := $(BUILD_DIR)/stress_rel$(EXE)
BIN_DBG   := $(BUILD_DIR)/stress_dbg$(EXE)
BIN_TSAN  := $(BUILD_DIR)/stress_tsan$(EXE)
BIN_ASAN  := $(BUILD_DIR)/stress_asan$(EXE)
BIN_SANITY:= $(BUILD_DIR)/sanity$(EXE)

SRC      := stress.cpp
SANITY   := sanity.cpp
HDR      := fastalloc.hpp

.PHONY: all run run-tsan run-asan sanity clean

all: $(BIN_REL) $(BIN_DBG) $(BIN_SANITY)

$(BIN_REL): $(SRC) $(HDR)
	@$(MKDIR)
	$(CXX) $(CXXFLAGS_REL) $< -o $@ $(LDFLAGS)

$(BIN_DBG): $(SRC) $(HDR)
	@$(MKDIR)
	$(CXX) $(CXXFLAGS_DBG) $< -o $@ $(LDFLAGS)

$(BIN_TSAN): $(SRC) $(HDR)
	@$(MKDIR)
	$(CXX) $(CXXFLAGS_REL) $(SAN_FLAGS) $< -o $@ $(LDFLAGS)

$(BIN_ASAN): $(SRC) $(HDR)
	@$(MKDIR)
	$(CXX) $(CXXFLAGS_REL) $(ASAN_FLAGS) $< -o $@ $(LDFLAGS)

$(BIN_SANITY): $(SANITY) $(HDR)
	@$(MKDIR)
	$(CXX) $(CXXFLAGS_REL) $< -o $@ $(LDFLAGS)

run: $(BIN_REL)
	./$(BIN_REL)

run-tsan: $(BIN_TSAN)
	./$(BIN_TSAN)

run-asan: $(BIN_ASAN)
	./$(BIN_ASAN)

sanity: $(BIN_SANITY)
	./$(BIN_SANITY)

clean:
ifeq ($(OS),Windows_NT)
	@if exist $(BUILD_DIR) $(RM) $(BUILD_DIR)
else
	$(RM) $(BUILD_DIR)
endif