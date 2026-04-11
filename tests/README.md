# Maze HTTPS Redis Server - Tests

This directory contains unit tests for the Maze Mission Dashboard server using the [Unity](https://github.com/ThrowTheSwitch/Unity) test framework.

## Test Files

- `test_maze_https_redis.c` — Main test suite

## Key Features of the Test Suite

- Tests the exact JSON format produced by `save_mission_via_curl()` in the GameHat console
- Validates mission storage in Redis (success, aborted, zero moves, etc.)
- Uses **Redis Database 15** to ensure complete isolation from production data (DB 0)
- Automatically cleans up test data before each test and after all tests finish
- Includes tests for edge cases like empty `mission_id`

## Prerequisites

Make sure Redis is running locally:

```bash
redis-server --daemonize yes
```

## How to Run the Tests

From the project root (`~/abcapsp26FriT1`):

```bash
cd tests
make test
```

The first time you run the tests, the Makefile will automatically download the Unity framework files (`unity.c`, `unity.h`, `unity_internals.h`).

## Project Structure (Tests)

```
tests/
├── test_maze_https_redis.c
├── Makefile
└── README.md
```

**Note**: The test file includes the server code using:

```c
#include "../https/maze_https_redis.c"
```

## Important Notes

- All tests run on **Redis DB 15** — they will **never** affect your production data.
- Tests are strict and expect the exact JSON output from `save_mission_via_curl()` (numbers as unquoted values).

## Available Make Targets

```bash
make test          # Build and run all tests
make clean         # Remove test binary
```

## Troubleshooting

- If Redis is not running → tests will fail with a clear message.
- If you see compilation issues with Unity → run `make` again (it will re-download the files).
