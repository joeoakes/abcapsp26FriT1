// Compile using: 
// gcc -O2 -Wall -Wextra -std=c11 maze_https_redis.c -o maze_https_redis $(pkg-config --cflags --libs libmicrohttpd hiredis libbson-1.0 gnutls)
#include <microhttpd.h>
#include <hiredis/hiredis.h>
#include <bson/bson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_PORT        8447
#define DEFAULT_REDIS_HOST  "127.0.0.1"
#define DEFAULT_REDIS_PORT  6379

static const char *cert_file = "certs/server.crt";
static const char *key_file  = "certs/server.key";

static redisContext *redis_ctx;

/* Structure to accumulate POST body data */
typedef struct {
    char  *data;
    size_t size;
} ConnectionInfo;

/* ----------------------------------------------------------------------------
   Utility: Read entire file into memory (caller must free)
   ---------------------------------------------------------------------------- */
static char *
read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    fread(buf, 1, (size_t)len, f);
    buf[len] = '\0';
    fclose(f);

    return buf;
}

/* ----------------------------------------------------------------------------
   Helper: Get UTF-8 string from BSON document or return NULL
   ---------------------------------------------------------------------------- */
static const char *
bson_get_utf8_or_null(const bson_t *doc, const char *field)
{
    bson_iter_t iter;
    if (bson_iter_init_find(&iter, doc, field) &&
        BSON_ITER_HOLDS_UTF8(&iter)) {
        return bson_iter_utf8(&iter, NULL);
    }
    return NULL;
}

/* ----------------------------------------------------------------------------
   Write mission data from BSON document → Redis hash
   ---------------------------------------------------------------------------- */
static void
write_mission(const bson_t *doc)
{
    const char *mission_id = bson_get_utf8_or_null(doc, "mission_id");
    if (!mission_id || !*mission_id) {
        return;
    }

    char key[256];
    snprintf(key, sizeof(key), "mission:%s:summary", mission_id);

    redisReply *reply;

    /* String fields */
    #define HSET_STR(field) do {                                      \
        const char *val = bson_get_utf8_or_null(doc, field);          \
        if (val) {                                                    \
            reply = redisCommand(redis_ctx, "HSET %s %s %s",          \
                                 key, field, val);                    \
            if (reply) freeReplyObject(reply);                        \
        }                                                             \
    } while (0)

    HSET_STR("robot_id");
    HSET_STR("mission_type");
    HSET_STR("start_time");
    HSET_STR("end_time");
    HSET_STR("mission_result");
    HSET_STR("abort_reason");
    HSET_STR("distance_traveled");
    HSET_STR("duration_seconds");

    #undef HSET_STR

    /* Integer fields */
    const char *int_fields[] = {
        "moves_left_turn",
        "moves_right_turn",
        "moves_straight",
        "moves_reverse",
        "moves_total"
    };

    for (size_t i = 0; i < sizeof(int_fields)/sizeof(int_fields[0]); i++) {
        const char *field = int_fields[i];
        bson_iter_t iter;

        if (bson_iter_init_find(&iter, doc, field) &&
            BSON_ITER_HOLDS_INT32(&iter)) {
            int32_t val = bson_iter_int32(&iter);

            reply = redisCommand(redis_ctx, "HSET %s %s %d",
                                 key, field, (int)val);
            if (reply) freeReplyObject(reply);
        }
    }
}

/* ----------------------------------------------------------------------------
   libmicrohttpd request handler
   ---------------------------------------------------------------------------- */
static int
http_handler(void *cls,
             struct MHD_Connection *connection,
             const char *url,
             const char *method,
             const char *version,
             const char *upload_data,
             size_t *upload_data_size,
             void **con_cls)
{
    (void)cls; (void)version;  /* unused */

    /* Only accept POST /mission */
    if (strcmp(method, "POST") != 0 || strcmp(url, "/mission") != 0) {
        return MHD_NO;
    }

    /* First call: allocate connection state */
    if (!*con_cls) {
        *con_cls = calloc(1, sizeof(ConnectionInfo));
        if (!*con_cls) return MHD_NO;
        return MHD_YES;
    }

    ConnectionInfo *info = *con_cls;

    /* Accumulate POST body */
    if (*upload_data_size > 0) {
        info->data = realloc(info->data, info->size + *upload_data_size + 1);
        if (!info->data) return MHD_NO;

        memcpy(info->data + info->size, upload_data, *upload_data_size);
        info->size += *upload_data_size;
        info->data[info->size] = '\0';

        *upload_data_size = 0;
        return MHD_YES;
    }

    /* End of upload → process JSON */
    bson_error_t err = {0};
    bson_t *doc = bson_new_from_json((const uint8_t *)info->data, -1, &err);

    if (doc) {
        write_mission(doc);
        bson_destroy(doc);
    } else {
        fprintf(stderr, "JSON parse error: %s\n", err.message);
    }

    /* Response */
    const char *reply = "{\"status\":\"stored_in_redis\"}";
    struct MHD_Response *response =
        MHD_create_response_from_buffer(strlen(reply),
                                        (void *)reply,
                                        MHD_RESPMEM_PERSISTENT);

    int ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
    MHD_destroy_response(response);

    /* Cleanup */
    free(info->data);
    free(info);
    *con_cls = NULL;

    return ret;
}

/* ----------------------------------------------------------------------------
   Main entry point
   ---------------------------------------------------------------------------- */
int
main(void)
{
    struct timeval timeout = { .tv_sec = 2, .tv_usec = 0 };

    redis_ctx = redisConnectWithTimeout(DEFAULT_REDIS_HOST,
                                        DEFAULT_REDIS_PORT,
                                        timeout);
    if (redis_ctx == NULL || redis_ctx->err) {
        fprintf(stderr, "Redis connection failed: %s\n",
                redis_ctx ? redis_ctx->errstr : "allocation error");
        return 1;
    }

    char *cert = read_file(cert_file);
    char *key  = read_file(key_file);

    if (!cert || !key) {
        fprintf(stderr, "Failed to load certificate or key file\n");
        free(cert);
        free(key);
        redisFree(redis_ctx);
        return 1;
    }

    struct MHD_Daemon *daemon = MHD_start_daemon(
        MHD_USE_THREAD_PER_CONNECTION | MHD_USE_TLS,
        DEFAULT_PORT,
        NULL, NULL,                       /* accept / accept arg */
        &http_handler, NULL,              /* handler / handler arg */
        MHD_OPTION_HTTPS_MEM_CERT, cert,
        MHD_OPTION_HTTPS_MEM_KEY,  key,
        MHD_OPTION_END);

    if (!daemon) {
        fprintf(stderr, "Failed to start MHD daemon\n");
        free(cert);
        free(key);
        redisFree(redis_ctx);
        return 1;
    }

    printf("Redis HTTPS server running on https://0.0.0.0:%d/mission\n",
           DEFAULT_PORT);

    getchar();   // wait for user input to stop

    MHD_stop_daemon(daemon);
    free(cert);
    free(key);
    redisFree(redis_ctx);

    return 0;
}