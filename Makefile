# Compiler and Flags
CC      := gcc
CFLAGS  := -Wall -Wextra -Werror -Iinclude
LIBNAME := libbingus.a

# Directories
SRC_DIR := src
OBJ_DIR := obj
INC_DIR := include

# Files
SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

# Default Target
all: $(LIBNAME)

# Create the Static Library
$(LIBNAME): $(OBJS)
	@echo "Linking $@"
	ar rcs $@ $^

# Compile Source Files to Object Files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	@echo "Compiling $<"
	$(CC) $(CFLAGS) -c $< -o $@

# Create Object Directory if it doesn't exist
$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

# Clean Build Artifacts
clean:
	@echo "Cleaning up..."
	rm -rf $(OBJ_DIR) $(LIBNAME)

.PHONY: all clean

# Tools
FORMAT_TOOL := clang-format
LINT_TOOL   := clang-tidy

# Find all source and header files for formatting/linting
ALL_FILES   := $(SRCS) $(wildcard $(INC_DIR)/*.h)
# Filter for clang-tidy to look at project headers
LINT_FILTER := -header-filter='include/.*'

# ------------------------------------------------------------------------------
# Formatting
# ------------------------------------------------------------------------------

format:
	@echo "Formatting code..."
	@$(FORMAT_TOOL) -i $(ALL_FILES)

format-check:
	@echo "Checking formatting..."
	@$(FORMAT_TOOL) --dry-run --Werror $(ALL_FILES)

# ------------------------------------------------------------------------------
# Linting
# ------------------------------------------------------------------------------

lint:
	@echo "Running linter..."
	@$(LINT_TOOL) $(SRCS) $(LINT_FILTER) -- -I$(INC_DIR)

lint-fix:
	@echo "Attempting to fix lint errors..."
	@$(LINT_TOOL) -fix $(SRCS) $(LINT_FILTER) -- -I$(INC_DIR)

.PHONY: all clean format format-check lint lint-fix
