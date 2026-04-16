# Maze HTTPS Redis Server - Tests

This directory contains unit tests and integration tests for the **Maze Mission Dashboard** server (`maze_https_redis.c`) using the [Unity](https://github.com/ThrowTheSwitch/Unity) test framework.

## Current Architecture

- **`maze_https_redis.c`** (port 8447): Serves the dashboard, stores missions in Redis, and proxies telemetry moves from the Mongo server.
- **`maze_https_mongo.c`** (port 8446 for local testing): Accepts telemetry data via `/move` and serves it via `/api/moves`.

For **local testing**, the Mongo HTTPS server runs on port **8446** while Redis runs on **8447**.  
In production/multi-device setups, both can run on port **8447**.

## Test Files

- `test_maze_https_redis.c` — Unit tests for Redis logic and mission handling
- `test_maze_https_mongo.c` — MongoDB-backed tests for BSON parsing, timestamp generation, and Mongo document insertion/verification
- `test_maze_sdl2.c` — Unit tests for core maze game logic (pathfinding, movement, grid handling)
- `integration_test.sh` — Full end-to-end integration test (starts both servers, sends data, verifies)

## Prerequisites

- Redis server running locally:
    ```bash
    redis-server --daemonize yes
    ```
- Valid client certificates in `../https/certs/` (`client.crt`, `client.key`)
- MongoDB database accessible (default: `mongodb://localhost:27017`)
- For MongoDB tests in **WSL**, if MongoDB is running on Windows instead of WSL, use the WSL virtual adapter IP for `MONGO_URI`  
  Example:
    ```bash
    export MONGO_URI="mongodb://192.168.80.1:27017"
    ```

## How to Run Tests

### 1. Unit Tests

```bash
cd tests
make test
```

These tests validate:

- Mission writing to Redis
- JSON generation (`get_missions_json`)
- `fetch_moves_from_mongo()` behavior
- Edge cases (empty mission_id, zero moves, etc.)

### 2. Integration Tests (Recommended)

```bash
cd tests
./integration_test.sh
```

Or using Make:

```bash
cd tests
make integration
```

**With custom MongoDB URI** (e.g. remote MongoDB):

```bash
cd tests
MONGO_URI="mongodb://<host-ip>:27017" ./integration_test.sh
```

The integration test will:

- Start Mongo telemetry server on port 8446
- Start Redis dashboard server on port 8447
- Send sample telemetry moves
- Verify data flows through Mongo → Redis proxy → Dashboard
- Test dashboard HTML loading
- Clean up both servers when finished

### 3. SDL2 Maze Unit Tests

These tests validate the **core maze game logic** implemented in `maze_sdl2.c`.

---

***What is Tested***

- Grid bounds (`in_bounds`)
- Manhattan heuristic (`heuristic`)
- Maze initialization (`maze_init`)
- Wall removal (`knock_down`)
- Movement (`can_move_to`, `try_move`)
- A* pathfinding (`compute_a_star_path`)
- JSON export (`write_maze_state_json`)

---

***Compile and Run***

```bash
cd /mnt/c/Users/bosto/Desktop/abcapsp26FriT1/tests

gcc -O2 -Wall -Wextra -std=c11 test_maze_sdl2.c unity.c -o test_maze_sdl2 \
  $(sdl2-config --cflags --libs) \
  $(pkg-config --cflags --libs libcurl)

./test_maze_sdl2

Expected Output:
- 15 Tests 0 Failures 0 Ignored
- OK

[ERROR] File not found: maze_sdl2.c
Fix:
  ls ../maze/maze_sdl2.c
  -> If not found, correct the include path in test_maze_sdl2.c

[ERROR] libcurl not found
Fix:
  sudo apt install libcurl4-openssl-dev

[ERROR] Wrong working directory
Fix:
  cd /mnt/c/Users/bosto/Desktop/abcapsp26FriT1/tests

## Environment Variables

| Variable         | Description                                    | Default                          |
| ---------------- | ---------------------------------------------- | -------------------------------- |
| `MONGO_URI`      | MongoDB database connection string             | `mongodb://localhost:27017`      |
| `MONGO_MOVE_API` | (Internal) Used by Redis to reach Mongo server | Set automatically by test script |

> Note: The Mongo HTTPS server is always accessed via `localhost` during local testing.

## Available Make Targets

```bash
make test          # Run unit tests
make integration   # Run full integration test
make maze_https_redis  # Build the Redis server
make clean         # Remove build artifacts
make distclean     # Remove everything including Unity files
```

## Manual Testing Commands (for reference)

**Send a test move:**

```bash
curl -k -X POST "https://localhost:8446/move" \
  --cert ../https/certs/client.crt \
  --key ../https/certs/client.key \
  -H "Content-Type: application/json" \
  -d '{"event_type":"player_move","input":{"device":"keyboard","move_sequence":42},"player":{"position":{"x":10,"y":15}},"goal_reached":false,"timestamp":"2026-04-13T12:00:00Z"}'
```

**Check moves via Redis proxy:**

```bash
curl -k --cert ../https/certs/client.crt --key ../https/certs/client.key \
  "https://localhost:8447/api/moves"
```

**Open Dashboard:**

```
https://localhost:8447/dashboard
```

## Important Notes

- Unit tests run against **Redis DB 15** (isolated from production DB 7).
- Integration test automatically starts and stops both servers.
- The Mongo HTTPS server is always reached via `localhost` for local testing.
- Use `MONGO_URI` when your MongoDB database is not on the default local instance.
