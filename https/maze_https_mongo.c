#include <errno.h>
#include <microhttpd.h>
#include <mongoc/mongoc.h>
#include <bson/bson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <hiredis/hiredis.h>

#define DEFAULT_PORT 8447
#define POSTBUFFERSIZE  4096
#define MAXNAMESIZE     64
#define MAXANSWERSIZE   512
#define DEFAULT_REDIS_HOST "10.170.8.109"
#define DEFAULT_REDIS_PORT 6379
#define DEFAULT_MONGO_URI "mongodb://localhost:27017"
#define DEFAULT_MONGO_DB  "maze"
#define DEFAULT_MONGO_COL "team1fmoves"

static const char *cert_file = "certs/server.crt";
static const char *key_file  = "certs/server.key";
static mongoc_client_t *mongo_client;

static redisContext *redis_ctx;

struct connection_info {
    char *data;
    size_t size;
};

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(n + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static void get_utc_iso8601(char *buf, size_t len) {
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static const char* bson_get_utf8_or_null(const bson_t *doc, const char *field) {
    bson_iter_t it;
    if (bson_iter_init_find(&it, doc, field) && BSON_ITER_HOLDS_UTF8(&it)) {
        return bson_iter_utf8(&it, NULL);
    }
    return NULL;
}

struct app_config {
    const char *mongo_uri;
    const char *mongo_db;
    const char *mongo_col;

    const char *redis_host;
    int redis_port;
    const char *redis_pass;
};

static struct app_config config;

static int handle_post(void *cls,
                       struct MHD_Connection *connection,
                       const char *url,
                       const char *method,
                       const char *version,
                       const char *upload_data,
                       size_t *upload_data_size,
                       void **con_cls)
{
    (void)version;
    (void)cls;

    if (strcmp(method, "POST") != 0 || strcmp(url, "/move") != 0)
        return MHD_NO;

    if (*con_cls == NULL) {
        struct connection_info *ci = calloc(1, sizeof(*ci));
        *con_cls = ci;
        return MHD_YES;
    }

    struct connection_info *ci = *con_cls;

    if (*upload_data_size != 0) {
        char *newbuf = realloc(ci->data, ci->size + *upload_data_size + 1);
        if (!newbuf) {
            fprintf(stderr, "OOM realloc\n");
            return MHD_NO;
        }
        ci->data = newbuf;
        memcpy(ci->data + ci->size, upload_data, *upload_data_size);
        ci->size += *upload_data_size;
        ci->data[ci->size] = '\0';
        *upload_data_size = 0;
        return MHD_YES;
    }

    bson_error_t error;
    bson_t *doc = bson_new_from_json((uint8_t *)ci->data, -1, &error);
    if (!doc) {
        fprintf(stderr, "JSON error: %s\n", error.message);
        return MHD_NO;
    }

    char ts[64];
    get_utc_iso8601(ts, sizeof(ts));
    BSON_APPEND_UTF8(doc, "received_at", ts);

    mongoc_collection_t *col =
        mongoc_client_get_collection(mongo_client,
                                     config.mongo_db,
                                     config.mongo_col);

    mongoc_collection_insert_one(col, doc, NULL, NULL, &error);
    mongoc_collection_destroy(col);

    if (redis_ctx) {
        const char *mission_id = bson_get_utf8_or_null(doc, "mission_id");
        const char *move = bson_get_utf8_or_null(doc, "move");
        const char *robot_id = bson_get_utf8_or_null(doc, "robot_id");
        const char *mission_type = bson_get_utf8_or_null(doc, "mission_type");
        const char *mission_result = bson_get_utf8_or_null(doc, "mission_result");
        const char *abort_reason = bson_get_utf8_or_null(doc, "abort_reason");
        const char *distance_traveled = bson_get_utf8_or_null(doc, "distance_traveled");
        const char *duration_seconds = bson_get_utf8_or_null(doc, "duration_seconds");

        if (mission_id && *mission_id) {
            char rkey[256];
            snprintf(rkey, sizeof(rkey), "mission:%s:summary", mission_id);

            redisReply *x;

            x = redisCommand(redis_ctx, "HSETNX %s start_time %s", rkey, ts);
            if (x) freeReplyObject(x);

            x = redisCommand(redis_ctx, "HSET %s end_time %s", rkey, ts);
            if (x) freeReplyObject(x);

            if (robot_id && *robot_id) {
                x = redisCommand(redis_ctx, "HSET %s robot_id %s", rkey, robot_id);
                if (x) freeReplyObject(x);
            }

            if (mission_type && *mission_type) {
                x = redisCommand(redis_ctx, "HSET %s mission_type %s", rkey, mission_type);
                if (x) freeReplyObject(x);
            }

            if (mission_result && *mission_result) {
                x = redisCommand(redis_ctx, "HSET %s mission_result %s", rkey, mission_result);
                if (x) freeReplyObject(x);
            }

            if (abort_reason && *abort_reason) {
                x = redisCommand(redis_ctx, "HSET %s abort_reason %s", rkey, abort_reason);
                if (x) freeReplyObject(x);
            }

            if (distance_traveled && *distance_traveled) {
                x = redisCommand(redis_ctx, "HSET %s distance_traveled %s", rkey, distance_traveled);
                if (x) freeReplyObject(x);
            }

            if (duration_seconds && *duration_seconds) {
                x = redisCommand(redis_ctx, "HSET %s duration_seconds %s", rkey, duration_seconds);
                if (x) freeReplyObject(x);
            }

            x = redisCommand(redis_ctx, "HINCRBY %s moves_total 1", rkey);
            if (x) freeReplyObject(x);

            if (move && *move) {
                const char *field = NULL;
                if (strcmp(move, "LEFT") == 0) field = "moves_left_turn";
                else if (strcmp(move, "RIGHT") == 0) field = "moves_right_turn";
                else if (strcmp(move, "STRAIGHT") == 0) field = "moves_straight";
                else if (strcmp(move, "REVERSE") == 0) field = "moves_reverse";

                if (field) {
                    x = redisCommand(redis_ctx, "HINCRBY %s %s 1", rkey, field);
                    if (x) freeReplyObject(x);
                }
            }
        }
    }

    bson_destroy(doc);

    const char *response = "{\"status\":\"ok\"}";
    struct MHD_Response *resp =
        MHD_create_response_from_buffer(strlen(response),
                                        (void *)response,
                                        MHD_RESPMEM_PERSISTENT);

    int ret = MHD_queue_response(connection, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);

    free(ci->data);
    free(ci);
    *con_cls = NULL;

    return ret;
}

int main(void) {
    config.mongo_uri = getenv("MONGO_URI");
    if (!config.mongo_uri || !*config.mongo_uri)
        config.mongo_uri = DEFAULT_MONGO_URI;

    config.mongo_db = getenv("MONGO_DB");
    if (!config.mongo_db || !*config.mongo_db)
        config.mongo_db = DEFAULT_MONGO_DB;

    config.mongo_col = getenv("MONGO_COL");
    if (!config.mongo_col || !*config.mongo_col)
        config.mongo_col = DEFAULT_MONGO_COL;

    config.redis_host = getenv("REDIS_HOST");
    if (!config.redis_host || !*config.redis_host)
        config.redis_host = DEFAULT_REDIS_HOST;

    const char *rp = getenv("REDIS_PORT");
    config.redis_port = (rp && *rp) ? atoi(rp) : DEFAULT_REDIS_PORT;

    config.redis_pass = getenv("REDIS_PASS");

    mongoc_init();
    mongo_client = mongoc_client_new(config.mongo_uri);
    if (!mongo_client) {
        fprintf(stderr, "Failed to create MongoDB client\n");
        return 1;
    }

    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;

    redis_ctx = redisConnectWithTimeout(config.redis_host, config.redis_port, tv);
    if (!redis_ctx || redis_ctx->err) {
        fprintf(stderr, "Redis connect failed to %s:%d\n", config.redis_host, config.redis_port);
        if (redis_ctx && redis_ctx->errstr) fprintf(stderr, "  %s\n", redis_ctx->errstr);
        if (redis_ctx) redisFree(redis_ctx);
        redis_ctx = NULL;
    }

    if (redis_ctx && config.redis_pass && *config.redis_pass) {
        redisReply *auth = redisCommand(redis_ctx, "AUTH %s", config.redis_pass);
        if (!auth || auth->type == REDIS_REPLY_ERROR) {
            fprintf(stderr, "Redis AUTH failed\n");
            if (auth && auth->str) fprintf(stderr, "  %s\n", auth->str);
            if (auth) freeReplyObject(auth);
            redisFree(redis_ctx);
            redis_ctx = NULL;
        } else {
            freeReplyObject(auth);
        }
    }

    char *cert_pem = read_file(cert_file);
    char *key_pem  = read_file(key_file);
    if (!cert_pem || !key_pem) {
        fprintf(stderr, "Failed to read cert/key files\n");
        if (redis_ctx) redisFree(redis_ctx);
        mongoc_cleanup();
        return 1;
    }

    struct MHD_Daemon *daemon = MHD_start_daemon(
        MHD_USE_THREAD_PER_CONNECTION | MHD_USE_TLS,
        DEFAULT_PORT,
        NULL, NULL,
        &handle_post, NULL,
        MHD_OPTION_HTTPS_MEM_CERT,
        cert_pem,
        MHD_OPTION_HTTPS_MEM_KEY,
        key_pem,
        MHD_OPTION_END);

    if (!daemon) {
        fprintf(stderr, "Failed to start HTTPS server\n");
        free(cert_pem);
        free(key_pem);
        if (redis_ctx) redisFree(redis_ctx);
        mongoc_cleanup();
        return 1;
    }

    printf("HTTPS server listening on https://localhost:%d/move\n", DEFAULT_PORT);
    getchar();

    MHD_stop_daemon(daemon);
    free(cert_pem);
    free(key_pem);
    if (redis_ctx) redisFree(redis_ctx);
    mongoc_cleanup();
    return 0;
}
