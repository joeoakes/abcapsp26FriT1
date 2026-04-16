// Compile with: gcc -O2 -Wall -Wextra -std=c11 test_maze_sdl2.c unity.c -o test_maze_sdl2 \ $(sdl2-config --cflags --libs) \ $(pkg-config --cflags --libs libcurl)

#include "unity.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define main original_main
#include "../maze/maze_sdl2.c"
#undef main

static void reset_open_grid(void) {
    maze_init();

    for (int y = 0; y < MAZE_H; y++) {
        for (int x = 0; x < MAZE_W; x++) {
            g[y][x].walls = 0;
            g[y][x].visited = false;
        }
    }
}

void setUp(void) {
    memset(&mission, 0, sizeof(mission));
    network_enabled = false;
    path_len = 0;
    maze_init();
}

void tearDown(void) {
}

/* -------------------------------------------------------
 * in_bounds
 * -----------------------------------------------------*/
void test_in_bounds_valid_cells(void) {
    TEST_ASSERT_TRUE(in_bounds(0, 0));
    TEST_ASSERT_TRUE(in_bounds(MAZE_W - 1, MAZE_H - 1));
    TEST_ASSERT_TRUE(in_bounds(10, 7));
}

void test_in_bounds_invalid_cells(void) {
    TEST_ASSERT_FALSE(in_bounds(-1, 0));
    TEST_ASSERT_FALSE(in_bounds(0, -1));
    TEST_ASSERT_FALSE(in_bounds(MAZE_W, 0));
    TEST_ASSERT_FALSE(in_bounds(0, MAZE_H));
}

/* -------------------------------------------------------
 * heuristic
 * -----------------------------------------------------*/
void test_heuristic_at_goal_is_zero(void) {
    TEST_ASSERT_EQUAL_INT(0, heuristic(MAZE_W - 1, MAZE_H - 1));
}

void test_heuristic_from_origin_matches_manhattan_distance(void) {
    int expected = (MAZE_W - 1) + (MAZE_H - 1);
    TEST_ASSERT_EQUAL_INT(expected, heuristic(0, 0));
}

/* -------------------------------------------------------
 * maze_init
 * -----------------------------------------------------*/
void test_maze_init_sets_all_walls_and_clears_visited(void) {
    maze_init();

    for (int y = 0; y < MAZE_H; y++) {
        for (int x = 0; x < MAZE_W; x++) {
            TEST_ASSERT_EQUAL_UINT8(WALL_N | WALL_E | WALL_S | WALL_W, g[y][x].walls);
            TEST_ASSERT_FALSE(g[y][x].visited);
        }
    }
}

/* -------------------------------------------------------
 * knock_down
 * -----------------------------------------------------*/
void test_knock_down_east_removes_matching_walls(void) {
    maze_init();

    knock_down(0, 0, 1, 0);

    TEST_ASSERT_FALSE(g[0][0].walls & WALL_E);
    TEST_ASSERT_FALSE(g[0][1].walls & WALL_W);
}

void test_knock_down_south_removes_matching_walls(void) {
    maze_init();

    knock_down(0, 0, 0, 1);

    TEST_ASSERT_FALSE(g[0][0].walls & WALL_S);
    TEST_ASSERT_FALSE(g[1][0].walls & WALL_N);
}

/* -------------------------------------------------------
 * can_move_to / try_move
 * -----------------------------------------------------*/
void test_can_move_to_false_when_wall_blocks(void) {
    maze_init();

    TEST_ASSERT_FALSE(can_move_to(0, 0, 1, 0));
    TEST_ASSERT_FALSE(can_move_to(0, 0, 0, 1));
}

void test_can_move_to_true_when_open(void) {
    maze_init();
    knock_down(0, 0, 1, 0);

    TEST_ASSERT_TRUE(can_move_to(0, 0, 1, 0));
}

void test_try_move_updates_position_when_open(void) {
    int px = 0, py = 0;

    maze_init();
    knock_down(0, 0, 1, 0);

    TEST_ASSERT_TRUE(try_move(&px, &py, 1, 0));
    TEST_ASSERT_EQUAL_INT(1, px);
    TEST_ASSERT_EQUAL_INT(0, py);
}

void test_try_move_fails_when_wall_blocks(void) {
    int px = 0, py = 0;

    maze_init();

    TEST_ASSERT_FALSE(try_move(&px, &py, 1, 0));
    TEST_ASSERT_EQUAL_INT(0, px);
    TEST_ASSERT_EQUAL_INT(0, py);
}

/* -------------------------------------------------------
 * compute_a_star_path
 * -----------------------------------------------------*/
void test_compute_a_star_path_on_open_grid(void) {
    Pos path[MAZE_W * MAZE_H];
    int len = 0;

    reset_open_grid();

    TEST_ASSERT_TRUE(compute_a_star_path(0, 0, path, &len));
    TEST_ASSERT_TRUE(len > 0);

    TEST_ASSERT_EQUAL_INT(0, path[0].x);
    TEST_ASSERT_EQUAL_INT(0, path[0].y);

    TEST_ASSERT_EQUAL_INT(MAZE_W - 1, path[len - 1].x);
    TEST_ASSERT_EQUAL_INT(MAZE_H - 1, path[len - 1].y);
}

void test_compute_a_star_path_at_goal_returns_single_node(void) {
    Pos path[MAZE_W * MAZE_H];
    int len = 0;

    reset_open_grid();

    TEST_ASSERT_TRUE(compute_a_star_path(MAZE_W - 1, MAZE_H - 1, path, &len));
    TEST_ASSERT_EQUAL_INT(1, len);
    TEST_ASSERT_EQUAL_INT(MAZE_W - 1, path[0].x);
    TEST_ASSERT_EQUAL_INT(MAZE_H - 1, path[0].y);
}

void test_compute_a_star_path_fails_when_start_is_trapped(void) {
    Pos path[MAZE_W * MAZE_H];
    int len = 0;

    maze_init(); /* all walls remain closed */

    TEST_ASSERT_FALSE(compute_a_star_path(0, 0, path, &len));
    TEST_ASSERT_EQUAL_INT(0, len);
}

/* -------------------------------------------------------
 * write_maze_state_json
 * -----------------------------------------------------*/
void test_write_maze_state_json_creates_file(void) {
    const char *test_file = "maze_state_test.json";
    MAZE_STATE_FILE = test_file;

    reset_open_grid();
    write_maze_state_json(2, 3, false);

    FILE *f = fopen(test_file, "r");
    TEST_ASSERT_NOT_NULL(f);

    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);

    TEST_ASSERT_NOT_NULL(strstr(buf, "\"maze_width\": 21"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"maze_height\": 15"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"player\": {\"x\": 2, \"y\": 3}"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"won\": false"));

    remove(test_file);
}

/* -------------------------------------------------------
 * Runner
 * -----------------------------------------------------*/
int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_in_bounds_valid_cells);
    RUN_TEST(test_in_bounds_invalid_cells);

    RUN_TEST(test_heuristic_at_goal_is_zero);
    RUN_TEST(test_heuristic_from_origin_matches_manhattan_distance);

    RUN_TEST(test_maze_init_sets_all_walls_and_clears_visited);

    RUN_TEST(test_knock_down_east_removes_matching_walls);
    RUN_TEST(test_knock_down_south_removes_matching_walls);

    RUN_TEST(test_can_move_to_false_when_wall_blocks);
    RUN_TEST(test_can_move_to_true_when_open);
    RUN_TEST(test_try_move_updates_position_when_open);
    RUN_TEST(test_try_move_fails_when_wall_blocks);

    RUN_TEST(test_compute_a_star_path_on_open_grid);
    RUN_TEST(test_compute_a_star_path_at_goal_returns_single_node);
    RUN_TEST(test_compute_a_star_path_fails_when_start_is_trapped);

    RUN_TEST(test_write_maze_state_json_creates_file);

    return UNITY_END();
}