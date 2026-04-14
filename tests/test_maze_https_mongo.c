// Compile with: gcc -O2 -Wall -Wextra -std=c11 test_maze_https_mongo.c unity.c -o test_mongo \
  $(pkg-config --cflags --libs libmongoc-1.0 libbson-1.0 libmicrohttpd gnutls) \
  -lpthread

// test_maze_https_mongo.c
#include "unity.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <bson/bson.h>
#include <mongoc/mongoc.h>

// Include the real server file, but rename main
#define main original_main
#include "../https/maze_https_mongo.c"
#undef main

static mongoc_collection_t *test_collection = NULL;

static void log_test(const char *msg)
{
    printf("[TEST] %s\n", msg);
}

// =====================================================
// Setup / Teardown
// =====================================================
void setUp(void)
{
    const char *uri = getenv("MONGO_URI");
    config.mongo_uri = (uri && *uri) ? uri : "mongodb://localhost:27017";
    config.mongo_db  = "team1fmoves_test";
    config.mongo_col = "moves_test";

    if (!mongo_client) {
        mongoc_init();
        mongo_client = mongoc_client_new(config.mongo_uri);
        TEST_ASSERT_NOT_NULL_MESSAGE(mongo_client, "MongoDB must be running on localhost:27017");
    }

    test_collection = mongoc_client_get_collection(
        mongo_client,
        config.mongo_db,
        config.mongo_col
    );
    TEST_ASSERT_NOT_NULL(test_collection);

    // Clean test collection before each test
    bson_t *filter = bson_new();
    bson_error_t error;
    bool ok = mongoc_collection_delete_many(test_collection, filter, NULL, NULL, &error);
    TEST_ASSERT_TRUE_MESSAGE(ok, error.message);
    bson_destroy(filter);
}

void tearDown(void)
{
    if (test_collection) {
        mongoc_collection_destroy(test_collection);
        test_collection = NULL;
    }
}

// =====================================================
// Helpers
// =====================================================
static bson_t *parse_json(const char *json_str)
{
    bson_error_t err = {0};
    bson_t *doc = bson_new_from_json((const uint8_t *)json_str, -1, &err);
    TEST_ASSERT_NOT_NULL_MESSAGE(doc, err.message);
    return doc;
}

static int count_documents(void)
{
    bson_t *filter = bson_new();
    bson_error_t error;
    int64_t count = mongoc_collection_count_documents(
        test_collection, filter, NULL, NULL, NULL, &error
    );
    bson_destroy(filter);

    TEST_ASSERT_TRUE_MESSAGE(count >= 0, error.message);
    return (int)count;
}

static bson_t *find_one_by_field(const char *field, const char *value)
{
    bson_t *filter = BCON_NEW(field, BCON_UTF8(value));
    mongoc_cursor_t *cursor =
        mongoc_collection_find_with_opts(test_collection, filter, NULL, NULL);

    const bson_t *doc = NULL;
    bson_t *copy = NULL;

    if (mongoc_cursor_next(cursor, &doc)) {
        copy = bson_copy(doc);
    }

    mongoc_cursor_destroy(cursor);
    bson_destroy(filter);
    return copy;
}

// ====================== TEST DATA ======================

static const char *move_json =
"{"
"\"mission_id\":\"mongo-001\","
"\"robot_id\":\"maze_sim\","
"\"mission_type\":\"maze_navigation\","
"\"moves_total\":45,"
"\"mission_result\":\"success\""
"}";

static const char *bad_json =
"{"
"\"mission_id\":\"broken-001\","
"\"robot_id\":\"maze_sim\","
"\"moves_total\":"
"}";

// =====================================================
// Tests
// =====================================================

void test_read_file_success(void)
{
    char template[] = "/tmp/mongo_test_XXXXXX";
    int fd = mkstemp(template);
    TEST_ASSERT_NOT_EQUAL(-1, fd);

    const char *content = "Mongo test certificate data";
    TEST_ASSERT_EQUAL((ssize_t)strlen(content), write(fd, content, strlen(content)));
    close(fd);

    char *data = read_file(template);
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_EQUAL_STRING(content, data);

    free(data);
    remove(template);
}

void test_get_utc_iso8601_format(void)
{
    char ts[64] = {0};
    get_utc_iso8601(ts, sizeof(ts));

    TEST_ASSERT_TRUE(strlen(ts) > 0);
    TEST_ASSERT_NOT_NULL(strchr(ts, 'T'));
    TEST_ASSERT_EQUAL_CHAR('Z', ts[strlen(ts) - 1]);
}

void test_valid_json_parses_to_bson(void)
{
    bson_error_t error = {0};
    bson_t *doc = bson_new_from_json((const uint8_t *)move_json, -1, &error);

    TEST_ASSERT_NOT_NULL_MESSAGE(doc, error.message);
    bson_destroy(doc);
}

void test_invalid_json_fails_to_parse(void)
{
    bson_error_t error = {0};
    bson_t *doc = bson_new_from_json((const uint8_t *)bad_json, -1, &error);

    TEST_ASSERT_NULL(doc);
}

void test_insert_document_into_mongo(void)
{
    bson_error_t error = {0};
    bson_t *doc = parse_json(move_json);

    char ts[64];
    get_utc_iso8601(ts, sizeof(ts));
    BSON_APPEND_UTF8(doc, "received_at", ts);

    bool ok = mongoc_collection_insert_one(test_collection, doc, NULL, NULL, &error);
    TEST_ASSERT_TRUE_MESSAGE(ok, error.message);

    TEST_ASSERT_EQUAL_INT(1, count_documents());

    bson_t *saved = find_one_by_field("mission_id", "mongo-001");
    TEST_ASSERT_NOT_NULL(saved);

    bson_iter_t iter;
    TEST_ASSERT_TRUE(bson_iter_init_find(&iter, saved, "received_at"));
    TEST_ASSERT_TRUE(BSON_ITER_HOLDS_UTF8(&iter));

    bson_destroy(saved);
    bson_destroy(doc);
}

void test_inserted_document_contains_original_fields(void)
{
    bson_error_t error = {0};
    bson_t *doc = parse_json(move_json);

    char ts[64];
    get_utc_iso8601(ts, sizeof(ts));
    BSON_APPEND_UTF8(doc, "received_at", ts);

    bool ok = mongoc_collection_insert_one(test_collection, doc, NULL, NULL, &error);
    TEST_ASSERT_TRUE_MESSAGE(ok, error.message);

    bson_t *saved = find_one_by_field("mission_id", "mongo-001");
    TEST_ASSERT_NOT_NULL(saved);

    bson_iter_t iter;
    TEST_ASSERT_TRUE(bson_iter_init_find(&iter, saved, "robot_id"));
    TEST_ASSERT_EQUAL_STRING("maze_sim", bson_iter_utf8(&iter, NULL));

    TEST_ASSERT_TRUE(bson_iter_init_find(&iter, saved, "mission_result"));
    TEST_ASSERT_EQUAL_STRING("success", bson_iter_utf8(&iter, NULL));

    bson_destroy(saved);
    bson_destroy(doc);
}

// =====================================================
// Runner
// =====================================================
int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_read_file_success);
    RUN_TEST(test_get_utc_iso8601_format);
    RUN_TEST(test_valid_json_parses_to_bson);
    RUN_TEST(test_invalid_json_fails_to_parse);
    RUN_TEST(test_insert_document_into_mongo);
    RUN_TEST(test_inserted_document_contains_original_fields);

    if (mongo_client) {
        mongoc_client_destroy(mongo_client);
        mongo_client = NULL;
        mongoc_cleanup();
    }

    return UNITY_END();
}