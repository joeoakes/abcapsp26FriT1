#include "unity.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <bson/bson.h>
#include <hiredis/hiredis.h>

// Rename original main() to avoid conflict with Unity
#define main original_main
#include "../https/maze_https_redis.c"
#undef main

static redisContext *test_redis_ctx = NULL;

void setUp(void)
{
    if (test_redis_ctx == NULL) {
        test_redis_ctx = redisConnect("127.0.0.1", 6379);
        TEST_ASSERT_NOT_NULL_MESSAGE(test_redis_ctx, "Redis must be running on 127.0.0.1:6379");
        TEST_ASSERT_FALSE_MESSAGE(test_redis_ctx->err, "Redis connection failed");

        // === IMPORTANT: Switch to test database (DB 15) ===
        redisReply *db_reply = redisCommand(test_redis_ctx, "SELECT 15");
        TEST_ASSERT_NOT_NULL(db_reply);
        TEST_ASSERT_EQUAL_INT(REDIS_REPLY_STATUS, db_reply->type);
        freeReplyObject(db_reply);
    }
    redis_ctx = test_redis_ctx;

    // Clean ALL missions in the TEST database before each test
    redisReply *reply = redisCommand(test_redis_ctx, "KEYS mission:*:summary");
    if (reply && reply->type == REDIS_REPLY_ARRAY) {
        for (size_t i = 0; i < reply->elements; ++i) {
            redisCommand(test_redis_ctx, "DEL %s", reply->element[i]->str);
        }
    }
    if (reply) freeReplyObject(reply);
}

void tearDown(void) { }

// Helper to create BSON from JSON string
static bson_t *parse_json(const char *json_str)
{
    bson_error_t err = {0};
    bson_t *doc = bson_new_from_json((const uint8_t*)json_str, -1, &err);
    TEST_ASSERT_NOT_NULL_MESSAGE(doc, "Failed to parse JSON");
    return doc;
}

// ====================== TEST DATA (Exact format from save_mission_via_curl) ======================

static const char *success_json =
"{"
"\"mission_id\":\"success-001\","
"\"robot_id\":\"maze_sim\","
"\"mission_type\":\"maze_navigation\","
"\"start_time\":\"2026-04-11T10:00:00Z\","
"\"end_time\":\"2026-04-11T10:05:30Z\","
"\"moves_left_turn\":5,"
"\"moves_right_turn\":12,"
"\"moves_straight\":25,"
"\"moves_reverse\":3,"
"\"moves_total\":45,"
"\"distance_traveled\":\"45.50\","
"\"duration_seconds\":330,"
"\"mission_result\":\"success\","
"\"abort_reason\":\"none\""
"}";

static const char *aborted_json =
"{"
"\"mission_id\":\"abort-001\","
"\"robot_id\":\"maze_sim\","
"\"mission_type\":\"maze_navigation\","
"\"start_time\":\"2026-04-11T11:00:00Z\","
"\"end_time\":\"2026-04-11T11:02:15Z\","
"\"moves_left_turn\":0,"
"\"moves_right_turn\":1,"
"\"moves_straight\":0,"
"\"moves_reverse\":2,"
"\"moves_total\":3,"
"\"distance_traveled\":\"3.00\","
"\"duration_seconds\":135,"
"\"mission_result\":\"aborted\","
"\"abort_reason\":\"user_terminated\""
"}";

static const char *zero_moves_json =
"{"
"\"mission_id\":\"zero-001\","
"\"robot_id\":\"maze_sim\","
"\"mission_type\":\"maze_navigation\","
"\"start_time\":\"2026-04-11T12:00:00Z\","
"\"end_time\":\"2026-04-11T12:00:10Z\","
"\"moves_left_turn\":0,"
"\"moves_right_turn\":0,"
"\"moves_straight\":0,"
"\"moves_reverse\":0,"
"\"moves_total\":0,"
"\"distance_traveled\":\"0.00\","
"\"duration_seconds\":10,"
"\"mission_result\":\"aborted\","
"\"abort_reason\":\"user_terminated\""
"}";

// ====================== TESTS ======================

void test_read_file_success(void)
{
    char template[] = "/tmp/gamehat_test_XXXXXX";
    int fd = mkstemp(template);
    TEST_ASSERT_NOT_EQUAL(-1, fd);

    const char *content = "GameHat test certificate data";
    TEST_ASSERT_EQUAL((ssize_t)strlen(content), write(fd, content, strlen(content)));
    close(fd);

    char *data = read_file(template);
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_EQUAL_STRING(content, data);

    free(data);
    remove(template);
}

void test_write_success_mission(void)
{
    bson_t *doc = parse_json(success_json);
    write_mission(doc);

    redisReply *reply = redisCommand(test_redis_ctx, "HGETALL mission:success-001:summary");
    TEST_ASSERT_NOT_NULL(reply);
    TEST_ASSERT_EQUAL_INT(REDIS_REPLY_ARRAY, reply->type);

    int found_total = 0, found_result = 0, found_distance = 0;

    for (size_t i = 0; i < reply->elements; i += 2) {
        const char *f = reply->element[i]->str;
        const char *v = reply->element[i + 1]->str;

        if (strcmp(f, "moves_total") == 0)      { TEST_ASSERT_EQUAL_STRING("45", v); found_total = 1; }
        if (strcmp(f, "mission_result") == 0)   { TEST_ASSERT_EQUAL_STRING("success", v); found_result = 1; }
        if (strcmp(f, "distance_traveled") == 0){ TEST_ASSERT_EQUAL_STRING("45.50", v); found_distance = 1; }
    }

    TEST_ASSERT_TRUE(found_total);
    TEST_ASSERT_TRUE(found_result);
    TEST_ASSERT_TRUE(found_distance);

    freeReplyObject(reply);
    bson_destroy(doc);
}

void test_write_aborted_mission(void)
{
    bson_t *doc = parse_json(aborted_json);
    write_mission(doc);

    redisReply *reply = redisCommand(test_redis_ctx, "HGETALL mission:abort-001:summary");
    TEST_ASSERT_NOT_NULL(reply);

    int found_abort = 0, found_reason = 0;

    for (size_t i = 0; i < reply->elements; i += 2) {
        const char *f = reply->element[i]->str;
        const char *v = reply->element[i + 1]->str;

        if (strcmp(f, "mission_result") == 0) { TEST_ASSERT_EQUAL_STRING("aborted", v); found_abort = 1; }
        if (strcmp(f, "abort_reason") == 0)   { TEST_ASSERT_EQUAL_STRING("user_terminated", v); found_reason = 1; }
    }

    TEST_ASSERT_TRUE(found_abort);
    TEST_ASSERT_TRUE(found_reason);

    freeReplyObject(reply);
    bson_destroy(doc);
}

void test_write_zero_moves_mission(void)
{
    bson_t *doc = parse_json(zero_moves_json);
    write_mission(doc);

    redisReply *reply = redisCommand(test_redis_ctx, "HGETALL mission:zero-001:summary");
    TEST_ASSERT_NOT_NULL(reply);

    for (size_t i = 0; i < reply->elements; i += 2) {
        const char *f = reply->element[i]->str;
        const char *v = reply->element[i + 1]->str;

        if (strcmp(f, "moves_total") == 0)
            TEST_ASSERT_EQUAL_STRING("0", v);
        if (strcmp(f, "distance_traveled") == 0)
            TEST_ASSERT_EQUAL_STRING("0.00", v);
    }

    freeReplyObject(reply);
    bson_destroy(doc);
}

void test_write_empty_mission_id_is_ignored(void)
{
    const char *bad_json = "{\"mission_id\":\"\",\"robot_id\":\"maze_sim\",\"mission_result\":\"success\"}";
    bson_t *doc = parse_json(bad_json);
    write_mission(doc);   // should do nothing

    redisReply *reply = redisCommand(test_redis_ctx, "KEYS mission::summary");
    TEST_ASSERT_NOT_NULL(reply);
    TEST_ASSERT_EQUAL_INT(0, reply->elements);

    freeReplyObject(reply);
    bson_destroy(doc);
}

void test_get_missions_json_contains_multiple_missions(void)
{
    bson_t *doc1 = parse_json(success_json);
    bson_t *doc2 = parse_json(aborted_json);
    write_mission(doc1);
    write_mission(doc2);
    bson_destroy(doc1);
    bson_destroy(doc2);

    char *json_str = get_missions_json();
    TEST_ASSERT_NOT_NULL(json_str);

    TEST_ASSERT_TRUE(strstr(json_str, "success-001") != NULL);
    TEST_ASSERT_TRUE(strstr(json_str, "abort-001") != NULL);
    TEST_ASSERT_TRUE(strstr(json_str, "\"mission_result\":\"success\"") != NULL);
    TEST_ASSERT_TRUE(strstr(json_str, "\"mission_result\":\"aborted\"") != NULL);

    free(json_str);
}

void test_get_missions_json_empty(void)
{
    char *json_str = get_missions_json();
    TEST_ASSERT_NOT_NULL(json_str);
    TEST_ASSERT_EQUAL_STRING("[]", json_str);
    free(json_str);
}

// ====================== RUNNER ======================
int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_read_file_success);
    RUN_TEST(test_write_success_mission);
    RUN_TEST(test_write_aborted_mission);
    RUN_TEST(test_write_zero_moves_mission);
    RUN_TEST(test_write_empty_mission_id_is_ignored);
    RUN_TEST(test_get_missions_json_contains_multiple_missions);
    RUN_TEST(test_get_missions_json_empty);

    redisFree(test_redis_ctx);

    return UNITY_END();
}