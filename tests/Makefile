# =============================================================================
# Makefile for Maze HTTPS Redis Server + Tests
# =============================================================================

CC = gcc
CFLAGS = -O2 -Wall -Wextra -std=c11 -I.
LIBS = $(shell pkg-config --cflags --libs libmicrohttpd hiredis libbson-1.0)

# Main program
SERVER = maze_https_redis

# Test program
TEST = test_maze_https_redis

all: $(SERVER)

# Build the main server
$(SERVER): maze_https_redis.c
	$(CC) $(CFLAGS) $< -o $@ $(LIBS)

# Build and run tests (automatically downloads Unity if missing)
test: $(TEST)
	./$(TEST)

$(TEST): test_maze_https_redis.c unity.c
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

# Download Unity test framework (only if files are missing)
unity.c unity.h unity_internals.h:
	@echo "=== Downloading Unity Test Framework ==="
	wget -q https://raw.githubusercontent.com/ThrowTheSwitch/Unity/master/src/unity.h -O unity.h
	wget -q https://raw.githubusercontent.com/ThrowTheSwitch/Unity/master/src/unity_internals.h -O unity_internals.h
	wget -q https://raw.githubusercontent.com/ThrowTheSwitch/Unity/master/src/unity.c -O unity.c
	@echo "Unity files downloaded."

# Clean build artifacts
clean:
	rm -f $(SERVER) $(TEST)

# Full cleanup (including downloaded Unity files)
distclean: clean
	rm -f unity.h unity_internals.h unity.c

.PHONY: all test clean distclean