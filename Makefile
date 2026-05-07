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
