// Compile with:
// gcc -O2 -Wall -Wextra -std=c11 maze_https_redis.c -o maze_https_redis $(pkg-config --cflags --libs libmicrohttpd hiredis libbson-1.0 gnutls) -lpthread
#define _POSIX_C_SOURCE 200809L
#include <microhttpd.h>
#include <hiredis/hiredis.h>
#include <bson/bson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <gnutls/gnutls.h>
#include <gnutls/x509.h>
#include <pthread.h>

#define DEFAULT_PORT        8447
#define DEFAULT_REDIS_PORT  6379

static const char *cert_file = "certs/server.crt";
static const char *key_file  = "certs/server.key";
static const char *ca_file   = "certs/ca.crt";

static redisContext *redis_ctx;
static const char *dashboard_html = NULL;
static pthread_mutex_t redis_mutex = PTHREAD_MUTEX_INITIALIZER;
static char *cached_cert = NULL;
static char *cached_key = NULL;
static char *cached_ca = NULL;

/* ----------------------------------------------------------------------------
   Utility: Read entire file into memory
   ---------------------------------------------------------------------------- */
static char *read_file(const char *path)
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

    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

/* ----------------------------------------------------------------------------
   Helper: Get UTF-8 string from BSON
   ---------------------------------------------------------------------------- */
static const char *bson_get_utf8_or_null(const bson_t *doc, const char *field)
{
    bson_iter_t iter;
    if (bson_iter_init_find(&iter, doc, field) &&
        BSON_ITER_HOLDS_UTF8(&iter)) {
        return bson_iter_utf8(&iter, NULL);
    }
    return NULL;
}

/* ----------------------------------------------------------------------------
   Write mission to Redis hash (Thread-safe with mutex)
   ---------------------------------------------------------------------------- */
static void write_mission(const bson_t *doc)
{
    const char *mission_id = bson_get_utf8_or_null(doc, "mission_id");
    if (!mission_id || !*mission_id) return;

    char key[256];
    snprintf(key, sizeof(key), "mission:%s:summary", mission_id);

    pthread_mutex_lock(&redis_mutex);
    
    redisReply *reply;

    #define HSET_STR(field) do { \
        const char *val = bson_get_utf8_or_null(doc, field); \
        if (val) { \
            reply = redisCommand(redis_ctx, "HSET %s %s %s", key, field, val); \
            if (reply) freeReplyObject(reply); \
        } \
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
    const char *int_fields[] = {"moves_left_turn", "moves_right_turn",
                                "moves_straight", "moves_reverse", "moves_total"};

    for (size_t i = 0; i < sizeof(int_fields)/sizeof(int_fields[0]); i++) {
        const char *field = int_fields[i];
        bson_iter_t iter;
        if (bson_iter_init_find(&iter, doc, field) &&
            BSON_ITER_HOLDS_INT32(&iter)) {
            int32_t val = bson_iter_int32(&iter);
            reply = redisCommand(redis_ctx, "HSET %s %s %d", key, field, (int)val);
            if (reply) freeReplyObject(reply);
        }
    }
    
    pthread_mutex_unlock(&redis_mutex);
}

/* ----------------------------------------------------------------------------
   JSON helpers
   ---------------------------------------------------------------------------- */
typedef struct {
    char   *buf;
    size_t  len;
    size_t  cap;
} JsonBuffer;

static void json_init(JsonBuffer *jb)
{
    jb->cap = 4096;
    jb->buf = malloc(jb->cap);
    jb->len = 0;
    jb->buf[0] = '\0';
}

static void json_append(JsonBuffer *jb, const char *str)
{
    size_t add_len = strlen(str);
    if (jb->len + add_len + 1 > jb->cap) {
        jb->cap = (jb->cap + add_len + 4096) * 2;
        jb->buf = realloc(jb->buf, jb->cap);
    }
    memcpy(jb->buf + jb->len, str, add_len);
    jb->len += add_len;
    jb->buf[jb->len] = '\0';
}

static char *json_escape(const char *input)
{
    if (!input) return strdup("");
    size_t len = strlen(input);
    char *out = malloc(len * 2 + 1);
    char *p = out;
    for (const char *q = input; *q; ++q) {
        if (*q == '"') { *p++ = '\\'; *p++ = '"'; }
        else if (*q == '\\') { *p++ = '\\'; *p++ = '\\'; }
        else *p++ = *q;
    }
    *p = '\0';
    return out;
}

/* ----------------------------------------------------------------------------
   GET /api/missions (Thread-safe with mutex)
   ---------------------------------------------------------------------------- */
static char *get_missions_json(void)
{
    pthread_mutex_lock(&redis_mutex);
    
    redisReply *keys_reply = redisCommand(redis_ctx, "KEYS mission:*:summary");
    if (!keys_reply || keys_reply->type != REDIS_REPLY_ARRAY) {
        if (keys_reply) freeReplyObject(keys_reply);
        pthread_mutex_unlock(&redis_mutex);
        return strdup("[]");
    }

    JsonBuffer jb;
    json_init(&jb);
    json_append(&jb, "[");

    int first = 1;
    for (size_t i = 0; i < keys_reply->elements; ++i) {
        const char *key = keys_reply->element[i]->str;

        char mission_id[256] = {0};
        sscanf(key, "mission:%255[^:]:summary", mission_id);

        redisReply *hreply = redisCommand(redis_ctx, "HGETALL %s", key);
        if (!hreply || hreply->type != REDIS_REPLY_ARRAY || hreply->elements == 0) {
            if (hreply) freeReplyObject(hreply);
            continue;
        }

        if (!first) json_append(&jb, ",");
        first = 0;

        json_append(&jb, "{");
        json_append(&jb, "\"mission_id\":\"");
        char *esc = json_escape(mission_id);
        json_append(&jb, esc);
        free(esc);
        json_append(&jb, "\"");

        for (size_t j = 0; j < hreply->elements; j += 2) {
            const char *field = hreply->element[j]->str;
            const char *val   = hreply->element[j + 1]->str;

            json_append(&jb, ",\"");
            esc = json_escape(field);
            json_append(&jb, esc);
            free(esc);
            json_append(&jb, "\":\"");
            esc = json_escape(val);
            json_append(&jb, esc);
            free(esc);
            json_append(&jb, "\"");
        }

        json_append(&jb, "}");
        freeReplyObject(hreply);
    }

    json_append(&jb, "]");
    freeReplyObject(keys_reply);
    
    pthread_mutex_unlock(&redis_mutex);
    return jb.buf;
}

/* ----------------------------------------------------------------------------
   Fetch telemetry moves from the MongoDB server via curl
   ---------------------------------------------------------------------------- */
static char *fetch_moves_from_mongo(void)
{
    const char *mongo_api = getenv("MONGO_MOVE_API");
    if (!mongo_api || !*mongo_api) {
        mongo_api = "https://localhost:8447/api/moves";
    }

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "curl -k -s --connect-timeout 2 --max-time 5 \"%s\" 2>/dev/null",
             mongo_api);

    FILE *pipe = popen(cmd, "r");
    if (!pipe) {
        return strdup("[]");
    }

    size_t cap = 16384;
    char *buf = malloc(cap);
    if (!buf) {
        pclose(pipe);
        return strdup("[]");
    }

    size_t pos = 0;
    char line[4096];

    while (fgets(line, sizeof(line), pipe)) {
        size_t len = strlen(line);
        if (pos + len >= cap) {
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) break;
            buf = tmp;
        }
        memcpy(buf + pos, line, len);
        pos += len;
    }

    buf[pos] = '\0';
    pclose(pipe);

    // If empty or failed, return empty array
    if (pos == 0) {
        free(buf);
        return strdup("[]");
    }

    return buf;
}

/* ----------------------------------------------------------------------------
   Load HTML file into memory
   ---------------------------------------------------------------------------- */
static char *load_dashboard_html(const char *path)
{
    char *content = read_file(path);
    if (!content) {
        fprintf(stderr, "ERROR: Could not load dashboard from %s\n", path);
        const char *fallback =
            "<!DOCTYPE html><html><head><title>Error</title></head>"
            "<body><h1>Failed to load dashboard.html</h1></body></html>";
        return strdup(fallback);
    }
    return content;
}

/* ----------------------------------------------------------------------------
   Connection info for POST
   ---------------------------------------------------------------------------- */
typedef struct {
    char  *data;
    size_t size;
} ConnectionInfo;

/* ----------------------------------------------------------------------------
   Check if client certificate is valid
   ---------------------------------------------------------------------------- */
static int is_client_cert_valid(struct MHD_Connection *connection)
{
    const union MHD_ConnectionInfo *ci =
        MHD_get_connection_info(connection, MHD_CONNECTION_INFO_GNUTLS_SESSION);
    
    if (!ci) {
        fprintf(stderr, "No TLS session info\n");
        return 0;
    }
    
    gnutls_session_t session = ci->tls_session;
    if (!session) {
        fprintf(stderr, "No TLS session\n");
        return 0;
    }
    
    // Check if we have a client certificate
    if (gnutls_certificate_type_get(session) != GNUTLS_CRT_X509) {
        fprintf(stderr, "Not an X.509 certificate\n");
        return 0;
    }
    
    // Get client certificate
    unsigned int cert_list_size = 0;
    const gnutls_datum_t *cert_list = gnutls_certificate_get_peers(session, &cert_list_size);
    if (cert_list_size == 0) {
        fprintf(stderr, "No client certificate provided\n");
        return 0;
    }
    
    // Verify the certificate
    unsigned int status = 0;
    int ret = gnutls_certificate_verify_peers2(session, &status);
    if (ret < 0) {
        fprintf(stderr, "Certificate verification error: %s\n", gnutls_strerror(ret));
        return 0;
    }
    
    if (status != 0) {
        gnutls_datum_t out;
        gnutls_certificate_verification_status_print(status, GNUTLS_CRT_X509, &out, 0);
        fprintf(stderr, "Certificate verification failed: %s\n", out.data);
        gnutls_free(out.data);
        return 0;
    }
    
    return 1;
}

/* ----------------------------------------------------------------------------
   HTTP handler
   ---------------------------------------------------------------------------- */
static enum MHD_Result
http_handler(void *cls,
             struct MHD_Connection *connection,
             const char *url,
             const char *method,
             const char *version,
             const char *upload_data,
             size_t *upload_data_size,
             void **con_cls)
{
    (void)cls; (void)version;

    /* GET dashboard */
    if (strcmp(method, "GET") == 0) {
        if (strcmp(url, "/dashboard") == 0 || strcmp(url, "/") == 0) {
            struct MHD_Response *response = MHD_create_response_from_buffer(
                strlen(dashboard_html), (void *)dashboard_html, MHD_RESPMEM_PERSISTENT);
            MHD_add_response_header(response, "Content-Type", "text/html; charset=utf-8");
            MHD_add_response_header(response, "Cache-Control", "no-cache");
            enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
            MHD_destroy_response(response);
            return ret;
        }

        if (strcmp(url, "/api/missions") == 0) {
            char *json = get_missions_json();
            struct MHD_Response *response = MHD_create_response_from_buffer(
                strlen(json), json, MHD_RESPMEM_MUST_FREE);
            MHD_add_response_header(response, "Content-Type", "application/json");
            MHD_add_response_header(response, "Cache-Control", "no-cache");
            enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
            MHD_destroy_response(response);
            return ret;
        }

        if (strcmp(url, "/api/moves") == 0) {
            char *json = fetch_moves_from_mongo();
            struct MHD_Response *response = MHD_create_response_from_buffer(
                strlen(json), json, MHD_RESPMEM_MUST_FREE);
            MHD_add_response_header(response, "Content-Type", "application/json");
            MHD_add_response_header(response, "Cache-Control", "no-cache");
            enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
            MHD_destroy_response(response);
            return ret;
        }

        if (strcmp(url, "/favicon.ico") == 0) {
            struct MHD_Response *response = MHD_create_response_from_buffer(0, "", MHD_RESPMEM_PERSISTENT);
            enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_NO_CONTENT, response);
            MHD_destroy_response(response);
            return ret;
        }
    }

    /* POST /mission */
    if (strcmp(method, "POST") == 0) {
        // Require client certificate for all requests
        if (!is_client_cert_valid(connection)) {
            const char *error = "{\"error\":\"Valid client certificate required\"}";
            struct MHD_Response *response = MHD_create_response_from_buffer(
                strlen(error), (void*)error, MHD_RESPMEM_PERSISTENT);
            enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_UNAUTHORIZED, response);
            MHD_destroy_response(response);
            return ret;
        }
        if (strcmp(url, "/mission") == 0) {
            if (!*con_cls) {
                *con_cls = calloc(1, sizeof(ConnectionInfo));
                if (!*con_cls) return MHD_NO;
                return MHD_YES;
            }

            ConnectionInfo *info = *con_cls;

            if (*upload_data_size > 0) {
                info->data = realloc(info->data, info->size + *upload_data_size + 1);
                if (!info->data) return MHD_NO;
                memcpy(info->data + info->size, upload_data, *upload_data_size);
                info->size += *upload_data_size;
                info->data[info->size] = '\0';
                *upload_data_size = 0;
                return MHD_YES;
            }

            /* Process JSON */
            bson_error_t err = {0};
            bson_t *doc = bson_new_from_json((const uint8_t *)info->data, -1, &err);
            if (doc) {
                write_mission(doc);
                bson_destroy(doc);
            } else {
                fprintf(stderr, "JSON parse error: %s\n", err.message);
            }

            const char *reply = "{\"status\":\"stored_in_redis\"}";
            struct MHD_Response *response = MHD_create_response_from_buffer(
                strlen(reply), (void *)reply, MHD_RESPMEM_PERSISTENT);
            enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
            MHD_destroy_response(response);

            free(info->data);
            free(info);
            *con_cls = NULL;
            return ret;
        }
    }

    /* 404 for everything else */
    const char *notfound = "{\"error\":\"not found\"}";
    struct MHD_Response *response = MHD_create_response_from_buffer(
        strlen(notfound), (void*)notfound, MHD_RESPMEM_PERSISTENT);
    enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, response);
    MHD_destroy_response(response);
    return ret;
}

/* ----------------------------------------------------------------------------
   Main
   ---------------------------------------------------------------------------- */
int main(void)
{
    /* Redis connection with env var support */
    const char *redis_host = getenv("REDIS_HOST");
    if (!redis_host || !*redis_host) redis_host = "127.0.0.1";

    int redis_port = DEFAULT_REDIS_PORT;
    const char *port_str = getenv("REDIS_PORT");
    if (port_str && *port_str) redis_port = atoi(port_str);

    struct timeval timeout = { .tv_sec = 2, .tv_usec = 0 };
    redis_ctx = redisConnectWithTimeout(redis_host, redis_port, timeout);
    if (redis_ctx == NULL || redis_ctx->err) {
        fprintf(stderr, "Redis connection failed (%s:%d): %s\n",
                redis_host, redis_port,
                redis_ctx ? redis_ctx->errstr : "allocation error");
        return 1;
    }

    pthread_mutex_lock(&redis_mutex);
    redisReply *reply = redisCommand(redis_ctx, "SELECT 7");
    if (!reply) {
        fprintf(stderr, "Failed to select Redis DB 7\n");
        pthread_mutex_unlock(&redis_mutex);
        redisFree(redis_ctx);
        return 1;
    }
    freeReplyObject(reply);
    pthread_mutex_unlock(&redis_mutex);
    
    printf("Connected to Redis at %s:%d (DB 7)\n", redis_host, redis_port);

    // Read certificate files
    cached_cert = read_file(cert_file);
    cached_key = read_file(key_file);
    cached_ca = read_file(ca_file);
    
    if (!cached_cert || !cached_key || !cached_ca) {
        fprintf(stderr, "Failed to load certificate files\n");
        free(cached_cert); free(cached_key); free(cached_ca);
        redisFree(redis_ctx);
        return 1;
    }

    dashboard_html = load_dashboard_html("../dashboard/dashboard.html");

    // Start HTTPS server with mTLS
    struct MHD_Daemon *daemon = MHD_start_daemon(
        MHD_USE_THREAD_PER_CONNECTION | MHD_USE_TLS,
        DEFAULT_PORT,
        NULL, NULL,
        &http_handler, NULL,
        MHD_OPTION_HTTPS_MEM_CERT, cached_cert,
        MHD_OPTION_HTTPS_MEM_KEY, cached_key,
        MHD_OPTION_HTTPS_MEM_TRUST, cached_ca,
        MHD_OPTION_HTTPS_PRIORITIES, "NORMAL:+VERS-TLS1.2:+CTYPE-X509:+SIGN-ALL:+COMP-NULL",
        MHD_OPTION_END);

    if (!daemon) {
        fprintf(stderr, "Failed to start MHD daemon (TLS). Check cert/key and that port %d is free.\n", DEFAULT_PORT);
        free(cached_cert); free(cached_key); free(cached_ca);
        redisFree(redis_ctx);
        return 1;
    }

    printf("\n=== Redis HTTPS Server with mTLS (Thread-Safe) ===\n");
    printf("Server running on https://0.0.0.0:%d\n", DEFAULT_PORT);
    printf("   → Dashboard: https://127.0.0.1:%d/dashboard\n", DEFAULT_PORT);
    printf("   → API: https://127.0.0.1:%d/api/missions\n", DEFAULT_PORT);
    printf("\nPress Enter to stop...\n");

    getchar();

    MHD_stop_daemon(daemon);
    free(cached_cert);
    free(cached_key);
    free(cached_ca);
    redisFree(redis_ctx);
    pthread_mutex_destroy(&redis_mutex);
    return 0;
}