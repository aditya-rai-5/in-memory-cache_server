# ===============================
# Project
# ===============================

PROJECT_NAME := redis_like
CXX          := g++
CXXFLAGS     := -std=c++17 -Wall -Wextra -Wpedantic -O2
INCLUDES     := -I./include

# ===============================
# Directories
# ===============================

SRC_DIR      := src
BUILD_DIR    := build
BIN_DIR      := bin

# ===============================
# Sources
# ===============================

SRCS := \
	main.cpp \
	$(SRC_DIR)/common/logger.cpp

# ===============================
# Objects
# ===============================

OBJS := $(SRCS:%.cpp=$(BUILD_DIR)/%.o)

# ===============================
# Target
# ===============================

TARGET := $(BIN_DIR)/$(PROJECT_NAME)

# ===============================
# Default
# ===============================

all: dirs $(TARGET)

# ===============================
# Link
# ===============================

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $@

# ===============================
# Compile
# ===============================

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# ===============================
# Directories
# ===============================

dirs:
	@mkdir -p $(BUILD_DIR) $(BIN_DIR)

# ===============================
# Clean
# ===============================

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR) *.o

# ===============================
# Rebuild
# ===============================

re: clean all

# ===============================
# Run
# ===============================

run: all
	./$(TARGET)

.PHONY: all clean re run dirs
