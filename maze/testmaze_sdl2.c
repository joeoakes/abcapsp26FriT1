// maze_sdl2.c
// Simple SDL2 maze: generate (DFS backtracker), draw, move player to goal.
// Controls: Arrow keys or WASD. R = regenerate. P = advance ONE step along A* optimal path.
//           L = save mission + launch dashboard. Esc = quit.
// Compile using: gcc testmaze_sdl2.c -o testmaze_sdl2 `sdl2-config --cflags --libs`
// Run WITH network: ./testmaze_sdl2
// Run WITHOUT network: DISABLE_NETWORK=1 ./testmaze_sdl2

#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <unistd.h>
#include <limits.h>     // INT_MAX for A*

#define MAZE_W 21   // number of cells horizontally
#define MAZE_H 15   // number of cells vertically
#define CELL   32   // pixels per cell
#define PAD    16   // window padding around maze

// Wall bitmask for each cell
enum { WALL_N = 1, WALL_E = 2, WALL_S = 4, WALL_W = 8 };

typedef struct {
  uint8_t walls;
  bool visited;
} Cell;

static Cell g[MAZE_H][MAZE_W];

// Position helper for A* and path storage
typedef struct {
  int x, y;
} Pos;

static Pos current_path[MAZE_W * MAZE_H];
static int path_len = 0;

// For local testing
static bool network_enabled = true;

static inline bool in_bounds(int x, int y) {
  return (x >= 0 && x < MAZE_W && y >= 0 && y < MAZE_H);
}

// Remove wall between (x,y) and (nx,ny)
static void knock_down(int x, int y, int nx, int ny) {
  if (nx == x && ny == y - 1) { // N
    g[y][x].walls &= ~WALL_N;
    g[ny][nx].walls &= ~WALL_S;
  } else if (nx == x + 1 && ny == y) { // E
    g[y][x].walls &= ~WALL_E;
    g[ny][nx].walls &= ~WALL_W;
  } else if (nx == x && ny == y + 1) { // S
    g[y][x].walls &= ~WALL_S;
    g[ny][nx].walls &= ~WALL_N;
  } else if (nx == x - 1 && ny == y) { // W
    g[y][x].walls &= ~WALL_W;
    g[ny][nx].walls &= ~WALL_E;
  }
}

static void maze_init(void) {
  for (int y = 0; y < MAZE_H; y++) {
    for (int x = 0; x < MAZE_W; x++) {
      g[y][x].walls = WALL_N | WALL_E | WALL_S | WALL_W;
      g[y][x].visited = false;
    }
  }
}

// Iterative DFS "recursive backtracker"
static void maze_generate(int sx, int sy) {
  typedef struct { int x, y; } P;
  P stack[MAZE_W * MAZE_H];
  int top = 0;

  g[sy][sx].visited = true;
  stack[top++] = (P){sx, sy};

  while (top > 0) {
    P cur = stack[top - 1];
    int x = cur.x, y = cur.y;

    // Collect unvisited neighbors
    P neigh[4];
    int ncount = 0;

    const int dx[4] = { 0, 1, 0, -1 };
    const int dy[4] = { -1, 0, 1, 0 };

    for (int i = 0; i < 4; i++) {
      int nx = x + dx[i], ny = y + dy[i];
      if (in_bounds(nx, ny) && !g[ny][nx].visited) {
        neigh[ncount++] = (P){nx, ny};
      }
    }

    if (ncount == 0) {
      // backtrack
      top--;
      continue;
    }

    // choose random neighbor
    int pick = rand() % ncount;
    int nx = neigh[pick].x, ny = neigh[pick].y;

    // carve passage
    knock_down(x, y, nx, ny);
    g[ny][nx].visited = true;
    stack[top++] = (P){nx, ny};
  }

  // Clear visited flags so we can reuse for other logic later if needed
  for (int y = 0; y < MAZE_H; y++)
    for (int x = 0; x < MAZE_W; x++)
      g[y][x].visited = false;
}

// Can we move from (x,y) to (nx,ny)? Used by A* (mirrors try_move logic)
static bool can_move_to(int x, int y, int nx, int ny) {
  if (!in_bounds(nx, ny)) return false;

  uint8_t w = g[y][x].walls;

  if (nx == x && ny == y - 1 && (w & WALL_N)) return false;
  if (nx == x + 1 && ny == y && (w & WALL_E)) return false;
  if (nx == x && ny == y + 1 && (w & WALL_S)) return false;
  if (nx == x - 1 && ny == y && (w & WALL_W)) return false;

  return true;
}

// Manhattan distance heuristic (perfect for 4-way grid)
static int heuristic(int x, int y) {
  return abs(x - (MAZE_W - 1)) + abs(y - (MAZE_H - 1));
}

// A* pathfinding from (sx,sy) to goal. Fills current_path[0..path_len-1] (start -> goal)
// Returns true if a path was found (always true in a perfect maze).
static bool compute_a_star_path(int sx, int sy, Pos* out_path, int* out_len) {
  int goalx = MAZE_W - 1;
  int goaly = MAZE_H - 1;

  if (sx == goalx && sy == goaly) {
    out_path[0].x = sx;
    out_path[0].y = sy;
    *out_len = 1;
    return true;
  }

  // A* working arrays (stack-allocated, tiny maze)
  int g_score[MAZE_H][MAZE_W];
  int f_score[MAZE_H][MAZE_W];
  int parent_x[MAZE_H][MAZE_W];
  int parent_y[MAZE_H][MAZE_W];
  bool closed[MAZE_H][MAZE_W];

  for (int y = 0; y < MAZE_H; y++) {
    for (int x = 0; x < MAZE_W; x++) {
      g_score[y][x] = INT_MAX;
      f_score[y][x] = INT_MAX;
      parent_x[y][x] = -1;
      parent_y[y][x] = -1;
      closed[y][x] = false;
    }
  }

  g_score[sy][sx] = 0;
  f_score[sy][sx] = heuristic(sx, sy);

  // Simple open set (no heap - maze is tiny, O(n²) is fine)
  Pos open[MAZE_W * MAZE_H];
  int open_count = 0;
  open[open_count++] = (Pos){sx, sy};

  while (open_count > 0) {
    // Find node with lowest f_score
    int best_idx = 0;
    int best_f = f_score[open[0].y][open[0].x];
    for (int i = 1; i < open_count; i++) {
      int ff = f_score[open[i].y][open[i].x];
      if (ff < best_f) {
        best_f = ff;
        best_idx = i;
      }
    }

    Pos current = open[best_idx];
    int cx = current.x, cy = current.y;

    // Remove from open
    open[best_idx] = open[open_count - 1];
    open_count--;

    if (cx == goalx && cy == goaly) {
      // Reconstruct path (goal -> start, then reverse)
      int len = 0;
      int tx = goalx, ty = goaly;
      while (true) {
        out_path[len].x = tx;
        out_path[len].y = ty;
        len++;
        if (tx == sx && ty == sy) break;

        int px = parent_x[ty][tx];
        int py = parent_y[ty][tx];
        if (px == -1) break; // safety
        tx = px;
        ty = py;
      }
      // Reverse in place
      for (int i = 0; i < len / 2; i++) {
        Pos temp = out_path[i];
        out_path[i] = out_path[len - 1 - i];
        out_path[len - 1 - i] = temp;
      }
      *out_len = len;
      return true;
    }

    closed[cy][cx] = true;

    // 4 neighbors
    const int dirs[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
    for (int d = 0; d < 4; d++) {
      int nx = cx + dirs[d][0];
      int ny = cy + dirs[d][1];
      if (!in_bounds(nx, ny)) continue;
      if (closed[ny][nx]) continue;
      if (!can_move_to(cx, cy, nx, ny)) continue;

      int tentative_g = g_score[cy][cx] + 1;
      if (tentative_g < g_score[ny][nx]) {
        parent_x[ny][nx] = cx;
        parent_y[ny][nx] = cy;
        g_score[ny][nx] = tentative_g;
        f_score[ny][nx] = tentative_g + heuristic(nx, ny);

        // Add to open if not already present
        bool already_in_open = false;
        for (int i = 0; i < open_count; i++) {
          if (open[i].x == nx && open[i].y == ny) {
            already_in_open = true;
            break;
          }
        }
        if (!already_in_open) {
          open[open_count++] = (Pos){nx, ny};
        }
      }
    }
  }

  *out_len = 0; // no path (should never happen)
  return false;
}

// Draw maze walls as lines
static void draw_maze(SDL_Renderer* r) {
  // Background
  SDL_SetRenderDrawColor(r, 15, 15, 18, 255);
  SDL_RenderClear(r);

  // Maze lines
  SDL_SetRenderDrawColor(r, 230, 230, 230, 255);

  int ox = PAD;
  int oy = PAD;

  for (int y = 0; y < MAZE_H; y++) {
    for (int x = 0; x < MAZE_W; x++) {
      int x0 = ox + x * CELL;
      int y0 = oy + y * CELL;
      int x1 = x0 + CELL;
      int y1 = y0 + CELL;

      uint8_t w = g[y][x].walls;

      if (w & WALL_N) SDL_RenderDrawLine(r, x0, y0, x1, y0);
      if (w & WALL_E) SDL_RenderDrawLine(r, x1, y0, x1, y1);
      if (w & WALL_S) SDL_RenderDrawLine(r, x0, y1, x1, y1);
      if (w & WALL_W) SDL_RenderDrawLine(r, x0, y0, x0, y1);
    }
  }
}

// Player / goal rendering
static void draw_player_goal(SDL_Renderer* r, int px, int py) {
  int ox = PAD;
  int oy = PAD;

  // Goal cell highlight
  SDL_Rect goal = {
    ox + (MAZE_W - 1) * CELL + 6,
    oy + (MAZE_H - 1) * CELL + 6,
    CELL - 12,
    CELL - 12
  };
  SDL_SetRenderDrawColor(r, 40, 160, 70, 255);
  SDL_RenderFillRect(r, &goal);

  // Player
  SDL_Rect p = {
    ox + px * CELL + 8,
    oy + py * CELL + 8,
    CELL - 16,
    CELL - 16
  };
  SDL_SetRenderDrawColor(r, 255, 255, 0, 255);
  SDL_RenderFillRect(r, &p);
}

// Attempt to move player; returns true if moved
static bool try_move(int* px, int* py, int dx, int dy) {
  int x = *px, y = *py;
  int nx = x + dx, ny = y + dy;
  if (!in_bounds(nx, ny)) return false;

  uint8_t w = g[y][x].walls;

  // Blocked by wall?
  if (dx == 0 && dy == -1 && (w & WALL_N)) return false;
  if (dx == 1 && dy == 0  && (w & WALL_E)) return false;
  if (dx == 0 && dy == 1  && (w & WALL_S)) return false;
  if (dx == -1 && dy == 0 && (w & WALL_W)) return false;

  *px = nx;
  *py = ny;
  return true;
}

// POSTs player movement data to the HTTPS server at MOVE_ENDPOINT / MOVE_ENDPOINT_2 (HTTPS)
// using curl. Sends event_type "player_move" with position, move sequence,
// goal status, and UTC ISO 8601 timestamp.
static int post_json_via_curl(const char *endpoint, const char *json)
{
    if (!endpoint || !*endpoint) return 0;

    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "curl -k -sS -X POST \"%s\" "
        "-H \"Content-Type: application/json\" "
        "-d '%s'",
        endpoint, json);

    int ret = system(cmd);
    if (ret != 0) {
        fprintf(stderr, "curl POST failed (endpoint=%s) rc=%d\n", endpoint, ret);
    }
    return ret;
}

static void send_move_via_curl(uint32_t move_seq, int cell_x, int cell_y, bool goal_reached) {
    if (!network_enabled) return;

    time_t now = time(NULL);
    struct tm* utc = gmtime(&now);
    char timestamp[21];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", utc);

    char json[512];
    snprintf(json, sizeof(json),
        "{"
        "\"event_type\":\"player_move\","
        "\"input\":{\"device\":\"joystick\",\"move_sequence\":%u},"
        "\"player\":{\"position\":{\"x\":%d,\"y\":%d}},"
        "\"goal_reached\":%s,"
        "\"timestamp\":\"%s\""
        "}",
        move_seq,
        cell_x, cell_y,
        goal_reached ? "true" : "false",
        timestamp);

    const char *ep_robot = getenv("MOVE_ENDPOINT");
    if (!ep_robot || !*ep_robot) ep_robot = "https://10.170.8.135:8447/move";

    const char *ep_log = getenv("MOVE_ENDPOINT_2");
    if (!ep_log || !*ep_log) ep_log = "https://10.170.8.130:8447/move";

    post_json_via_curl(ep_robot, json);
    if (strcmp(ep_log, ep_robot) != 0) {
        post_json_via_curl(ep_log, json);
    }
}

typedef struct {
    char mission_id[64];
    time_t start_time;

    int moves_left_turn;
    int moves_right_turn;
    int moves_straight;
    int moves_reverse;
    int moves_total;

    float distance_traveled;
    bool finished;
} MissionState;

typedef enum {
    STATE_MAZE,
    STATE_SUMMARY
} AppState;

static AppState app_state = STATE_MAZE;
static MissionState mission;


static void generate_mission_id(char* out, size_t n) {
    snprintf(out, n, "%08x-%04x-%04x-%04x-%08x",
        rand(), rand() & 0xffff, rand() & 0xffff,
        rand() & 0xffff, rand());
}

static void save_mission_via_curl()
{
    if (!network_enabled) return;

    time_t end = time(NULL);

    char start_buf[32], end_buf[32];
    strftime(start_buf, sizeof(start_buf), "%Y-%m-%dT%H:%M:%SZ", gmtime(&mission.start_time));
    strftime(end_buf, sizeof(end_buf), "%Y-%m-%dT%H:%M:%SZ", gmtime(&end));

    int duration = (int)difftime(end, mission.start_time);

    const char* result = mission.finished ? "success" : "aborted";
    const char* abort_reason = mission.finished ? "none" : "user_terminated";

    /* Build JSON (escaped properly for shell) */
    char json[2048];
    snprintf(json, sizeof(json),
             "{"
             "\"mission_id\":\"%s\","
             "\"robot_id\":\"maze_sim\","
             "\"mission_type\":\"maze_navigation\","
             "\"start_time\":\"%s\","
             "\"end_time\":\"%s\","
             "\"moves_left_turn\":%d,"
             "\"moves_right_turn\":%d,"
             "\"moves_straight\":%d,"
             "\"moves_reverse\":%d,"
             "\"moves_total\":%d,"
             "\"distance_traveled\":\"%.2f\","
             "\"duration_seconds\":%d,"
             "\"mission_result\":\"%s\","
             "\"abort_reason\":\"%s\""
             "}",
             mission.mission_id,
             start_buf,
             end_buf,
             mission.moves_left_turn,
             mission.moves_right_turn,
             mission.moves_straight,
             mission.moves_reverse,
             mission.moves_total,
             mission.distance_traveled,
             duration,
             result,
             abort_reason);

    const char *endpoint = getenv("MISSION_ENDPOINT");
    if (!endpoint || !*endpoint) endpoint = "https://10.170.8.130:8447/mission";

    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
             "curl -k -sS -X POST \"%s\" "
             "-H \"Content-Type: application/json\" "
             "-d '%s'",
             endpoint,
             json);

    int ret = system(cmd);
    if (ret != 0) {
        fprintf(stderr, "curl mission POST failed with return code %d\n", ret);
    }
}

static void launch_mission_dashboard(const char *mission_id) {
    const char *redis_host = getenv("REDIS_HOST");
    if (!redis_host || !*redis_host) redis_host = "127.0.0.1";

    const char *redis_port = getenv("REDIS_PORT");
    if (!redis_port || !*redis_port) redis_port = "6379";

    pid_t pid = fork();
    if (pid == -1) {
        perror("fork failed for mission_dashboard");
        return;
    }

    if (pid == 0) {
        close(STDIN_FILENO);

        execl("./missions/mission_dashboard",
              "mission_dashboard",
              mission_id,
              redis_host,
              redis_port,
              (char *) NULL);

        perror("execl ./missions/mission_dashboard failed");
        _exit(1);
    }
    printf("Mission report launched in background (mission %s)\n", mission_id);
}

static void regenerate(int* px, int* py, SDL_Window* win) {
  maze_init();
  maze_generate(0, 0);
  *px = 0; *py = 0;

  generate_mission_id(mission.mission_id, sizeof(mission.mission_id));
  mission.start_time = time(NULL);

  mission.moves_left_turn = 0;
  mission.moves_right_turn = 0;
  mission.moves_straight = 0;
  mission.moves_reverse = 0;
  mission.moves_total = 0;
  mission.distance_traveled = 0.0f;
  mission.finished = false;

  // Compute initial A* path from start
  compute_a_star_path(*px, *py, current_path, &path_len);

  SDL_SetWindowTitle(win, "SDL2 Maze - Reach the green goal (R regenerate, P = A* step, L = dashboard, Esc quit)");
}

int main(int argc, char** argv) {
    srand((unsigned)time(NULL));

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    const char* dn = getenv("DISABLE_NETWORK");
    if (dn && strcmp(dn, "1") == 0) {
        network_enabled = false;
        printf("=== Network posting DISABLED (DISABLE_NETWORK=1). "
               "All moves are local and instant. ===\n");
    }

    int win_w = PAD * 2 + MAZE_W * CELL;
    int win_h = PAD * 2 + MAZE_H * CELL;

    SDL_Window* win = SDL_CreateWindow(
        "SDL2 Maze - Reach the green goal (R regenerate, P = A* step, L = dashboard, Esc quit)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h,
        SDL_WINDOW_SHOWN
    );
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* r = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!r) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

    int px = 0, py = 0;
    regenerate(&px, &py, win);

    bool running = true;
    bool won = false;
    uint32_t move_sequence = 0;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;

            if (e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;

                if (k == SDLK_ESCAPE) running = false;

                if (k == SDLK_l) {
                    mission.finished = won;
                    save_mission_via_curl();
                    SDL_SetWindowTitle(win, "Mission report launched in terminal...");
                    launch_mission_dashboard(mission.mission_id);

                    regenerate(&px, &py, win);
                    won = false;
                    move_sequence = 0;
                }

                if (k == SDLK_r) {
                    regenerate(&px, &py, win);
                    won = false;
                    move_sequence = 0;
                }

                // === A* AUTO-STEP (one cell per press) ===
                if (k == SDLK_p && !won) {
                    if (path_len > 1) {
                        int old_px = px;
                        int old_py = py;
                        int nx = current_path[1].x;
                        int ny = current_path[1].y;

                        // Guaranteed valid move (A* only returns legal neighbors)
                        px = nx;
                        py = ny;

                        move_sequence++;
                        mission.moves_total++;
                        mission.distance_traveled += 1.0f;

                        // Classify move direction (same logic as manual moves)
                        int dx = nx - old_px;
                        int dy = ny - old_py;
                        if (dx == -1 && dy == 0) mission.moves_left_turn++;
                        else if (dx == 1 && dy == 0) mission.moves_right_turn++;
                        else if (dx == 0 && dy == -1) mission.moves_straight++;
                        else if (dx == 0 && dy == 1) mission.moves_reverse++;

                        bool goal_reached = (px == MAZE_W - 1 && py == MAZE_H - 1);

                        send_move_via_curl(move_sequence, px, py, goal_reached);

                        if (goal_reached) {
                            won = true;
                            mission.finished = true;
                            SDL_SetWindowTitle(win, "You win! Press R to regenerate, Esc to quit");
                        }

                        // Recompute A* from new position
                        compute_a_star_path(px, py, current_path, &path_len);
                    }
                }

                if (!won) {
                    bool moved = false;

                    if (k == SDLK_UP || k == SDLK_w) {
                        moved = try_move(&px, &py, 0, -1);
                    } else if (k == SDLK_RIGHT || k == SDLK_d) {
                        moved = try_move(&px, &py, 1, 0);
                    } else if (k == SDLK_DOWN || k == SDLK_s) {
                        moved = try_move(&px, &py, 0, 1);
                    } else if (k == SDLK_LEFT || k == SDLK_a) {
                        moved = try_move(&px, &py, -1, 0);
                    }

                    if (moved) {
                        move_sequence++;
                        mission.moves_total++;
                        mission.distance_traveled += 1.0f;

                        // classify movement (original logic)
                        if (k == SDLK_LEFT || k == SDLK_a)
                            mission.moves_left_turn++;
                        else if (k == SDLK_RIGHT || k == SDLK_d)
                            mission.moves_right_turn++;
                        else if (k == SDLK_UP || k == SDLK_w)
                            mission.moves_straight++;
                        else if (k == SDLK_DOWN || k == SDLK_s)
                            mission.moves_reverse++;

                        bool goal_reached = (px == MAZE_W - 1 && py == MAZE_H - 1);

                        // Send to HTTPS server instead of writing file
                        send_move_via_curl(move_sequence, px, py, goal_reached);

                        if (goal_reached) {
                            won = true;
                            mission.finished = true;
                            SDL_SetWindowTitle(win, "You win! Press R to regenerate, Esc to quit");
                        }

                        // Recompute A* after every manual user move
                        compute_a_star_path(px, py, current_path, &path_len);
                    }
                }
            }
        }

        draw_maze(r);
        draw_player_goal(r, px, py);
        SDL_RenderPresent(r);
    }

    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}