# =============================================================================
# Makefile for Maze HTTPS Server Tests
# Supports both Redis and MongoDB test binaries using Unity
# =============================================================================

CC = gcc
CFLAGS = -O2 -Wall -Wextra -std=c11 -I. -D_GNU_SOURCE
THREAD_LIB = -lpthread

UNITY_FILES = unity.c unity.h unity_internals.h

REDIS_SERVER_SRC = ../https/maze_https_redis.c
MONGO_SERVER_SRC = ../https/maze_https_mongo.c

REDIS_SERVER = maze_https_redis
MONGO_SERVER = maze_https_mongo

REDIS_TEST = test_maze_https_redis
MONGO_TEST = test_maze_https_mongo

REDIS_LIBS = $(shell pkg-config --cflags --libs libmicrohttpd hiredis libbson-1.0 gnutls)
MONGO_LIBS = $(shell pkg-config --cflags --libs libmongoc-1.0 libbson-1.0 libmicrohttpd gnutls)

all: $(REDIS_SERVER) $(MONGO_SERVER)

# ---------------------------------------------------------------------
# Build servers
# ---------------------------------------------------------------------

$(REDIS_SERVER): $(REDIS_SERVER_SRC)
	$(CC) $(CFLAGS) $< -o $@ $(REDIS_LIBS) $(THREAD_LIB)
	@echo "Built Redis server: $@"

$(MONGO_SERVER): $(MONGO_SERVER_SRC)
	$(CC) $(CFLAGS) $< -o $@ $(MONGO_LIBS) $(THREAD_LIB)
	@echo "Built Mongo server: $@"

# ---------------------------------------------------------------------
# Build tests
# ---------------------------------------------------------------------

$(REDIS_TEST): test_maze_https_redis.c $(UNITY_FILES)
	$(CC) $(CFLAGS) test_maze_https_redis.c unity.c -o $@ $(REDIS_LIBS) $(THREAD_LIB)
	@echo "Built Redis test binary: $@"

$(MONGO_TEST): test_maze_https_mongo.c $(UNITY_FILES)
	$(CC) $(CFLAGS) test_maze_https_mongo.c unity.c -o $@ $(MONGO_LIBS) $(THREAD_LIB)
	@echo "Built Mongo test binary: $@"

# ---------------------------------------------------------------------
# Run tests
# ---------------------------------------------------------------------

test-redis: $(REDIS_TEST)
	@echo "Running Redis tests..."
	./$(REDIS_TEST)

test-mongo: $(MONGO_TEST)
	@echo "Running Mongo tests..."
	./$(MONGO_TEST)

test: test-redis

test-all: test-redis test-mongo

# ---------------------------------------------------------------------
# Download Unity automatically if missing
# ---------------------------------------------------------------------

unity.c unity.h unity_internals.h:
	@echo "=== Downloading Unity Test Framework ==="
	wget -q https://raw.githubusercontent.com/ThrowTheSwitch/Unity/master/src/unity.h -O unity.h
	wget -q https://raw.githubusercontent.com/ThrowTheSwitch/Unity/master/src/unity_internals.h -O unity_internals.h
	wget -q https://raw.githubusercontent.com/ThrowTheSwitch/Unity/master/src/unity.c -O unity.c
	@echo "Unity files downloaded successfully."

# ---------------------------------------------------------------------
# Integration test
# ---------------------------------------------------------------------

integration:
	@echo "=== Running integration test ==="
	@chmod +x integration_test.sh 2>/dev/null || true
	./integration_test.sh

# ---------------------------------------------------------------------
# Environment helper
# ---------------------------------------------------------------------

show-mongo-uri:
	@echo "Current MONGO_URI: $$MONGO_URI"
	@echo "Example for WSL -> Windows MongoDB:"
	@echo "export MONGO_URI=mongodb://192.168.80.1:27017"

# ---------------------------------------------------------------------
# Cleanup
# ---------------------------------------------------------------------

clean:
	rm -f $(REDIS_SERVER) $(MONGO_SERVER) $(REDIS_TEST) $(MONGO_TEST) *.o

distclean: clean
	rm -f unity.c unity.h unity_internals.h

.PHONY: all test test-all test-redis test-mongo integration clean distclean show-mongo-uri