# Mini-Pupper / Maze Simulation Servers – Testing Guide

This document provides example JSON payloads and curl commands for testing the three backend servers:

- **Logging server** (`https://10.170.8.101:8447/move`) – receives per-move telemetry
- **AI server** (`https://10.170.8.109:8447/mission`) – receives mission summary
- **MiniPupper server** – uses the **same endpoints and payloads** as the simulation servers

All examples use `-k` to ignore self-signed certificate warnings.

## 1. Per-Move Telemetry – `/move` endpoint

(Used by both MongoDB and MiniPupper servers)

### Example JSON – Normal move (not at goal)

```json
{
    "event_type": "player_move",
    "input": {
        "device": "joystick",
        "move_sequence": 7
    },
    "player": {
        "position": {
            "x": 4,
            "y": 2
        }
    },
    "goal_reached": false,
    "timestamp": "2026-02-22T14:35:12Z"
}
```

### Example JSON – Reaching the goal

```json
{
    "event_type": "player_move",
    "input": {
        "device": "keyboard",
        "move_sequence": 43
    },
    "player": {
        "position": {
            "x": 20,
            "y": 14
        }
    },
    "goal_reached": true,
    "timestamp": "2026-02-22T14:42:19Z"
}
```

### Curl – Test `/move`

```bash
curl -k -v -X POST "https://<ip>:8447/move" \
  -H "Content-Type: application/json" \
  -d '{
    "event_type": "player_move",
    "input": {"device": "keyboard", "move_sequence": 12},
    "player": {"position": {"x": 8, "y": 5}},
    "goal_reached": false,
    "timestamp": "2026-02-22T15:10:45Z"
  }'
```

Expected response: `{"status":"ok"}` (HTTP 200)

## 2. Mission Summary – `/mission` endpoint

(Used by Redis server)

### Example JSON – Successful mission

```json
{
    "mission_id": "a1b2c3d4-e5f6-7890-abcd-1234567890ef",
    "robot_id": "maze_sim",
    "mission_type": "maze_navigation",
    "start_time": "2026-02-22T13:55:00Z",
    "end_time": "2026-02-22T14:08:22Z",
    "moves_left_turn": 11,
    "moves_right_turn": 14,
    "moves_straight": 17,
    "moves_reverse": 6,
    "moves_total": 48,
    "distance_traveled": "48.00",
    "duration_seconds": 802,
    "mission_result": "success",
    "abort_reason": "none"
}
```

### Example JSON – Aborted mission

```json
{
    "mission_id": "f9e8d7c6-b5a4-3210-fedc-ba9876543210",
    "robot_id": "maze_sim",
    "mission_type": "maze_navigation",
    "start_time": "2026-02-22T15:30:00Z",
    "end_time": "2026-02-22T15:37:45Z",
    "moves_left_turn": 9,
    "moves_right_turn": 20,
    "moves_straight": 4,
    "moves_reverse": 3,
    "moves_total": 36,
    "distance_traveled": "36.00",
    "duration_seconds": 465,
    "mission_result": "aborted",
    "abort_reason": "user_terminated"
}
```

### Curl – Test `/mission`

```bash
curl -k -v -X POST "https://10.170.8.109:8447/mission" \
  -H "Content-Type: application/json" \
  -d '{
    "mission_id": "test-mission-20260222-001",
    "robot_id": "maze_sim",
    "mission_type": "maze_navigation",
    "start_time": "2026-02-22T16:00:00Z",
    "end_time": "2026-02-22T16:05:30Z",
    "moves_left_turn": 10,
    "moves_right_turn": 10,
    "moves_straight": 15,
    "moves_reverse": 5,
    "moves_total": 40,
    "distance_traveled": "40.00",
    "duration_seconds": 330,
    "mission_result": "success",
    "abort_reason": "none"
  }'
```

Expected response: `{"status":"stored_in_redis"}` (HTTP 200)
