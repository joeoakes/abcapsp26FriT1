# =============================================================================
# Makefile for Maze HTTPS Redis Server Tests
# =============================================================================

CC = gcc
CFLAGS = -O2 -Wall -Wextra -std=c11 -I. -D_GNU_SOURCE

# Full set of libraries needed because we #include the full server file
LIBS = $(shell pkg-config --cflags --libs libmicrohttpd hiredis libbson-1.0 gnutls)

# Main server (for convenience)
SERVER_SRC = ../https/maze_https_redis.c
SERVER = maze_https_redis

# Test program
TEST = test_maze_https_redis

all: $(SERVER)

# Build the main Redis server
$(SERVER): $(SERVER_SRC)
	$(CC) $(CFLAGS) $< -o $@ $(LIBS) -lpthread
	@echo "Built server: $@"

# Build and run unit tests
test: $(TEST)
	@echo "Running unit tests..."
	./$(TEST)

$(TEST): test_maze_https_redis.c unity.c
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS) -lpthread
	@echo "Built test binary: $@"

# Download Unity test framework if missing
unity.c unity.h unity_internals.h:
	@echo "=== Downloading Unity Test Framework ==="
	wget -q https://raw.githubusercontent.com/ThrowTheSwitch/Unity/master/src/unity.h -O unity.h
	wget -q https://raw.githubusercontent.com/ThrowTheSwitch/Unity/master/src/unity_internals.h -O unity_internals.h
	wget -q https://raw.githubusercontent.com/ThrowTheSwitch/Unity/master/src/unity.c -O unity.c
	@echo "Unity files downloaded successfully."

# Clean build artifacts
clean:
	rm -f $(SERVER) $(TEST) *.o

# Full cleanup (including Unity files)
distclean: clean
	rm -f unity.h unity_internals.h unity.c

# Integration test
integration:
	@echo "=== Running Integration Test ==="
	@chmod +x integration_test.sh 2>/dev/null || true
	./integration_test.sh

.PHONY: all test clean distclean integration