/*
Compile:
gcc -Wall -Wextra -O2 -std=c11 maze_sdl2.c -o maze_sdl2 $(sdl2-config --cflags --libs)

Run with networ===-
./maze_sdl2

Run without network:
DISABLE_NETWORK=1 ./maze_sdl2

Optional environment overrides:
export MOVE_ENDPOINT="https://10.170.8.133:8447/move"
export MOVE_ENDPOINT_2="https://10.170.8.130:8447/move"
export MISSION_ENDPOINT="https://10.170.8.130:8447/mission"
*/

#include <curl/curl.h>
#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>

#define MAZE_W 21
#define MAZE_H 15
#define CELL   32
#define PAD    16
#define MOVE_COOLDOWN_MS 325


enum { WALL_N = 1, WALL_E = 2, WALL_S = 4, WALL_W = 8 };

typedef struct {
    uint8_t walls;
    bool visited;
} Cell;

typedef struct {
    int x, y;
} Pos;

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

static Cell g[MAZE_H][MAZE_W];
static Pos current_path[MAZE_W * MAZE_H];
static int path_len = 0;
static bool network_enabled = true;
static MissionState mission;

static inline bool in_bounds(int x, int y) {
    return (x >= 0 && x < MAZE_W && y >= 0 && y < MAZE_H);
}

static int heuristic(int x, int y) {
    return abs(x - (MAZE_W - 1)) + abs(y - (MAZE_H - 1));
}

static void knock_down(int x, int y, int nx, int ny) {
    if (nx == x && ny == y - 1) {
        g[y][x].walls &= ~WALL_N;
        g[ny][nx].walls &= ~WALL_S;
    } else if (nx == x + 1 && ny == y) {
        g[y][x].walls &= ~WALL_E;
        g[ny][nx].walls &= ~WALL_W;
    } else if (nx == x && ny == y + 1) {
        g[y][x].walls &= ~WALL_S;
        g[ny][nx].walls &= ~WALL_N;
    } else if (nx == x - 1 && ny == y) {
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

static void maze_generate(int sx, int sy) {
    typedef struct { int x, y; } P;
    P stack[MAZE_W * MAZE_H];
    int top = 0;

    g[sy][sx].visited = true;
    stack[top++] = (P){sx, sy};

    while (top > 0) {
        P cur = stack[top - 1];
        int x = cur.x;
        int y = cur.y;

        P neigh[4];
        int ncount = 0;

        const int dx[4] = { 0, 1, 0, -1 };
        const int dy[4] = { -1, 0, 1, 0 };

        for (int i = 0; i < 4; i++) {
            int nx = x + dx[i];
            int ny = y + dy[i];
            if (in_bounds(nx, ny) && !g[ny][nx].visited) {
                neigh[ncount++] = (P){nx, ny};
            }
        }

        if (ncount == 0) {
            top--;
            continue;
        }

        int pick = rand() % ncount;
        int nx = neigh[pick].x;
        int ny = neigh[pick].y;

        knock_down(x, y, nx, ny);
        g[ny][nx].visited = true;
        stack[top++] = (P){nx, ny};
    }

    for (int y = 0; y < MAZE_H; y++) {
        for (int x = 0; x < MAZE_W; x++) {
            g[y][x].visited = false;
        }
    }
}

static bool can_move_to(int x, int y, int nx, int ny) {
    if (!in_bounds(nx, ny)) return false;

    uint8_t w = g[y][x].walls;

    if (nx == x && ny == y - 1 && (w & WALL_N)) return false;
    if (nx == x + 1 && ny == y && (w & WALL_E)) return false;
    if (nx == x && ny == y + 1 && (w & WALL_S)) return false;
    if (nx == x - 1 && ny == y && (w & WALL_W)) return false;

    return true;
}

static bool try_move(int *px, int *py, int dx, int dy) {
    int x = *px;
    int y = *py;
    int nx = x + dx;
    int ny = y + dy;

    if (!in_bounds(nx, ny)) return false;

    uint8_t w = g[y][x].walls;

    if (dx == 0 && dy == -1 && (w & WALL_N)) return false;
    if (dx == 1 && dy == 0  && (w & WALL_E)) return false;
    if (dx == 0 && dy == 1  && (w & WALL_S)) return false;
    if (dx == -1 && dy == 0 && (w & WALL_W)) return false;

    *px = nx;
    *py = ny;
    return true;
}

static bool compute_a_star_path(int sx, int sy, Pos *out_path, int *out_len) {
    int goalx = MAZE_W - 1;
    int goaly = MAZE_H - 1;

    if (sx == goalx && sy == goaly) {
        out_path[0].x = sx;
        out_path[0].y = sy;
        *out_len = 1;
        return true;
    }

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

    Pos open[MAZE_W * MAZE_H];
    int open_count = 0;
    open[open_count++] = (Pos){sx, sy};

    while (open_count > 0) {
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
        int cx = current.x;
        int cy = current.y;

        open[best_idx] = open[open_count - 1];
        open_count--;

        if (cx == goalx && cy == goaly) {
            int len = 0;
            int tx = goalx;
            int ty = goaly;

            while (true) {
                out_path[len].x = tx;
                out_path[len].y = ty;
                len++;

                if (tx == sx && ty == sy) break;

                int px = parent_x[ty][tx];
                int py = parent_y[ty][tx];
                if (px == -1) break;

                tx = px;
                ty = py;
            }

            for (int i = 0; i < len / 2; i++) {
                Pos temp = out_path[i];
                out_path[i] = out_path[len - 1 - i];
                out_path[len - 1 - i] = temp;
            }

            *out_len = len;
            return true;
        }

        closed[cy][cx] = true;

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

    *out_len = 0;
    return false;
}

static void draw_maze(SDL_Renderer *r) {
    SDL_SetRenderDrawColor(r, 15, 15, 18, 255);
    SDL_RenderClear(r);

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

static void draw_player_goal(SDL_Renderer *r, int px, int py) {
    int ox = PAD;
    int oy = PAD;

    SDL_Rect goal = {
        ox + (MAZE_W - 1) * CELL + 6,
        oy + (MAZE_H - 1) * CELL + 6,
        CELL - 12,
        CELL - 12
    };
    SDL_SetRenderDrawColor(r, 40, 160, 70, 255);
    SDL_RenderFillRect(r, &goal);

    SDL_Rect p = {
        ox + px * CELL + 8,
        oy + py * CELL + 8,
        CELL - 16,
        CELL - 16
    };
    SDL_SetRenderDrawColor(r, 255, 255, 0, 255);
    SDL_RenderFillRect(r, &p);
}

static void reap_children(void) {
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }
}

static void post_json_via_curl_async(const char *endpoint,
                                     const char *json,
                                     const char *connect_timeout,
                                     const char *max_time)
{
    if (!endpoint || !*endpoint) return;

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return;
    }

    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        execlp("curl",
               "curl",
               "-k",
               "-sS",
               "--connect-timeout", connect_timeout,
               "--max-time", max_time,
               "-X", "POST",
               endpoint,
               "-H", "Content-Type: application/json",
               "-d", json,
               (char *)NULL);

        _exit(127);
    }
}

static void send_move_via_curl(uint32_t move_seq, int cell_x, int cell_y, bool goal_reached) {
    if (!network_enabled) return;

    time_t now = time(NULL);
    struct tm *utc = gmtime(&now);
    char timestamp[21];

    if (!utc) return;
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
        timestamp
    );

    const char *ep_robot = getenv("MOVE_ENDPOINT");
    if (!ep_robot || !*ep_robot) ep_robot = "https://10.170.8.133:8447/move";

    const char *ep_log = getenv("MOVE_ENDPOINT_2");
    if (!ep_log || !*ep_log) ep_log = "https://10.170.8.130:8447/move";

    post_json_via_curl_async(ep_robot, json, "0.15", "0.35");

    if (strcmp(ep_log, ep_robot) != 0) {
        post_json_via_curl_async(ep_log, json, "0.15", "0.35");
    }
}

static void save_mission_via_curl(void) {
    if (!network_enabled) return;

    time_t end = time(NULL);

    char start_buf[32];
    char end_buf[32];

    strftime(start_buf, sizeof(start_buf), "%Y-%m-%dT%H:%M:%SZ", gmtime(&mission.start_time));
    strftime(end_buf, sizeof(end_buf), "%Y-%m-%dT%H:%M:%SZ", gmtime(&end));

    int duration = (int)difftime(end, mission.start_time);

    const char *result = mission.finished ? "success" : "aborted";
    const char *abort_reason = mission.finished ? "none" : "user_terminated";

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
        abort_reason
    );

    const char *endpoint = getenv("MISSION_ENDPOINT");
    if (!endpoint || !*endpoint) endpoint = "https://10.170.8.130:8447/mission";

    post_json_via_curl_async(endpoint, json, "0.30", "1.50");
}

static void launch_mission_dashboard(const char *mission_id) {
    const char *redis_host = getenv("REDIS_HOST");
    if (!redis_host || !*redis_host) redis_host = "127.0.0.1";

    const char *redis_port = getenv("REDIS_PORT");
    if (!redis_port || !*redis_port) redis_port = "6379";

    pid_t pid = fork();
    if (pid < 0) {
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
              (char *)NULL);

        perror("execl ./missions/mission_dashboard failed");
        _exit(1);
    }

    printf("Mission report launched in background (mission %s)\n", mission_id);
}

static void generate_mission_id(char *out, size_t n) {
    snprintf(out, n, "%08x-%04x-%04x-%04x-%08x",
             rand(), rand() & 0xffff, rand() & 0xffff,
             rand() & 0xffff, rand());
}

static void regenerate(int *px, int *py, SDL_Window *win) {
    maze_init();
    maze_generate(0, 0);
    *px = 0;
    *py = 0;

    generate_mission_id(mission.mission_id, sizeof(mission.mission_id));
    mission.start_time = time(NULL);

    mission.moves_left_turn = 0;
    mission.moves_right_turn = 0;
    mission.moves_straight = 0;
    mission.moves_reverse = 0;
    mission.moves_total = 0;
    mission.distance_traveled = 0.0f;
    mission.finished = false;

    compute_a_star_path(*px, *py, current_path, &path_len);

    SDL_SetWindowTitle(win, "SDL2 Maze - Reach the green goal (R regenerate, P = A* step, L = dashboard, Esc quit)");
}

int main(void) {
    srand((unsigned)time(NULL));

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    const char *dn = getenv("DISABLE_NETWORK");
    if (dn && strcmp(dn, "1") == 0) {
        network_enabled = false;
        printf("=== Network posting DISABLED (DISABLE_NETWORK=1). All moves are local and instant. ===\n");
    }

    int win_w = PAD * 2 + MAZE_W * CELL;
    int win_h = PAD * 2 + MAZE_H * CELL;

    SDL_Window *win = SDL_CreateWindow(
        "SDL2 Maze - Reach the green goal (R regenerate, P = A* step, L = dashboard, Esc quit)",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        win_w,
        win_h,
        SDL_WINDOW_SHOWN
    );
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *r = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!r) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

    int px = 0;
    int py = 0;
    regenerate(&px, &py, win);

    bool running = true;
    bool won = false;
    uint32_t move_sequence = 0;
    uint32_t next_move_tick = 0;

    while (running) {
        reap_children();

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                running = false;
                continue;
            }

            if (e.type != SDL_KEYDOWN) {
                continue;
            }

            if (e.key.repeat) {
                continue;
            }

            SDL_Keycode k = e.key.keysym.sym;
            uint32_t now = SDL_GetTicks();

            if (k == SDLK_ESCAPE) {
                running = false;
                continue;
            }

            if (k == SDLK_l) {
                mission.finished = won;
                save_mission_via_curl();
                SDL_SetWindowTitle(win, "Mission report launched in terminal...");
                launch_mission_dashboard(mission.mission_id);

                regenerate(&px, &py, win);
                won = false;
                move_sequence = 0;
                next_move_tick = 0;
                continue;
            }

            if (k == SDLK_r) {
                regenerate(&px, &py, win);
                won = false;
                move_sequence = 0;
                next_move_tick = 0;
                continue;
            }

            if (won) {
                continue;
            }

            if (now < next_move_tick) {
                continue;
            }

            if (k == SDLK_p) {
                if (path_len > 1) {
                    int old_px = px;
                    int old_py = py;
                    int nx = current_path[1].x;
                    int ny = current_path[1].y;

                    px = nx;
                    py = ny;

                    move_sequence++;
                    mission.moves_total++;
                    mission.distance_traveled += 1.0f;

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

                    compute_a_star_path(px, py, current_path, &path_len);
                    next_move_tick = now + MOVE_COOLDOWN_MS;
                }
                continue;
            }

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

                if (k == SDLK_LEFT || k == SDLK_a) {
                    mission.moves_left_turn++;
                } else if (k == SDLK_RIGHT || k == SDLK_d) {
                    mission.moves_right_turn++;
                } else if (k == SDLK_UP || k == SDLK_w) {
                    mission.moves_straight++;
                } else if (k == SDLK_DOWN || k == SDLK_s) {
                    mission.moves_reverse++;
                }

                bool goal_reached = (px == MAZE_W - 1 && py == MAZE_H - 1);
                send_move_via_curl(move_sequence, px, py, goal_reached);

                if (goal_reached) {
                    won = true;
                    mission.finished = true;
                    SDL_SetWindowTitle(win, "You win! Press R to regenerate, Esc to quit");
                }

                compute_a_star_path(px, py, current_path, &path_len);
                next_move_tick = now + MOVE_COOLDOWN_MS;
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
