CC      := gcc
STD     := -std=c11
WARN    := -Wall -Wextra -Wpedantic

SRC_DIR   := src
INC_DIR   := inc
TEST_DIR  := tests
BUILD_DIR := build

CFLAGS   := $(STD) $(WARN) -I$(INC_DIR) -O2
SANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

LIB       := $(BUILD_DIR)/libpoolalloc.a
TEST_SRCS := $(wildcard $(TEST_DIR)/*.c)
TEST_BIN  := $(BUILD_DIR)/test_poolalloc
SAN_BIN   := $(BUILD_DIR)/test_poolalloc_san

.PHONY: all lib test check san clean

all: lib

lib: $(LIB)

$(LIB): $(OBJS)
	ar rcs $@ $^

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

test: $(TEST_BIN)

$(TEST_BIN): $(TEST_SRCS) $(LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_SRCS) $(LIB) -o $@

check: $(TEST_BIN)
	./$(TEST_BIN)

# Same tests, rebuilt from source under ASan + UBSan.
san: $(SAN_BIN)
	./$(SAN_BIN)

$(SAN_BIN): $(SRCS) $(TEST_SRCS) | $(BUILD_DIR)
	$(CC) $(STD) $(WARN) -I$(INC_DIR) $(SANFLAGS) \
		$(SRCS) $(TEST_SRCS) -o $@

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)
