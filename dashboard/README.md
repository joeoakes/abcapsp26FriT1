# Redis Mission Data Dashboard UI Snapshot

![mission_dashboard](mission_dashboard.jpg 'Redis Mission Data Dashboard UI Snapshot')

## Mission Dashboard

The Redis HTTPS server provides a clean, real-time web dashboard to visualize all mission data stored in Redis.

### Access the Dashboard

Open your browser and navigate to:

**https://HOST_IP:8447/dashboard**

### Dashboard Features

- **Modern dark-themed UI** built with Tailwind CSS
- **Live auto-refresh** every 3 seconds (no manual refresh needed)
- **Summary Cards** at the top showing key statistics:
    - Total number of missions
    - Success rate (percentage + count of successful missions)
    - Average mission duration
    - Total moves across all missions
- **Missions Table** with the following columns:

| Column       | Description                                                                 | Format         |
| ------------ | --------------------------------------------------------------------------- | -------------- |
| Mission ID   | Unique identifier of the mission                                            | Monospace font |
| Robot ID     | ID of the robot that performed the mission                                  | Text           |
| Type         | Mission type (e.g. `maze_navigation`)                                       | Text           |
| Start Time   | UTC timestamp when the mission started                                      | ISO 8601       |
| Result       | Color-coded badge: **SUCCESS** (green), **ABORTED** (amber), or other (red) | Badge          |
| Distance (m) | Distance traveled during the mission                                        | Right-aligned  |
| Duration (s) | How long the mission took                                                   | Right-aligned  |
| Total Moves  | Total number of moves (left + right + straight + reverse)                   | Right-aligned  |

- **Sorting**: Missions are automatically sorted with the newest on top (based on `start_time`)
- **Empty State**: Friendly message shown when no missions exist yet
- **Manual Refresh Button**: "↻ Refresh Now" for immediate update

### Endpoints

| Method | Path            | Description                             |
| ------ | --------------- | --------------------------------------- |
| GET    | `/dashboard`    | Full HTML dashboard (recommended)       |
| GET    | `/`             | Same as `/dashboard`                    |
| GET    | `/api/missions` | Raw JSON API – returns all missions     |
| POST   | `/mission`      | Endpoint used by your game to send data |

### Example JSON Response from `/api/missions`

```json
[
    {
        "mission_id": "4104393b-dadc-20c9-87a5-45cd9a55",
        "robot_id": "maze_sim",
        "mission_type": "maze_navigation",
        "start_time": "2026-03-25T19:36:21Z",
        "end_time": "2026-03-25T19:36:24Z",
        "moves_left_turn": "0",
        "moves_right_turn": "2",
        "moves_straight": "0",
        "moves_reverse": "0",
        "moves_total": "2",
        "distance_traveled": "2.00",
        "duration_seconds": "3",
        "mission_result": "aborted",
        "abort_reason": "user_terminated"
    }
]
```
