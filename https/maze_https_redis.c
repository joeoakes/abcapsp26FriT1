// Compile with:
// gcc -O2 -Wall -Wextra -std=c11 maze_https_redis.c -o maze_https_redis $(pkg-config --cflags --libs libmicrohttpd hiredis libbson-1.0 gnutls)
#define _POSIX_C_SOURCE 200809L
#include <microhttpd.h>
#include <hiredis/hiredis.h>
#include <bson/bson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   // for strcasecmp if needed

#define DEFAULT_PORT        8447
#define DEFAULT_REDIS_PORT    6379

static const char *cert_file = "certs/server.crt";
static const char *key_file  = "certs/server.key";

static redisContext *redis_ctx;

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
   Write mission to Redis hash
   ---------------------------------------------------------------------------- */
static void write_mission(const bson_t *doc)
{
    const char *mission_id = bson_get_utf8_or_null(doc, "mission_id");
    if (!mission_id || !*mission_id) return;

    char key[256];
    snprintf(key, sizeof(key), "mission:%s:summary", mission_id);

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
   GET /api/missions
   ---------------------------------------------------------------------------- */
static char *get_missions_json(void)
{
    redisReply *keys_reply = redisCommand(redis_ctx, "KEYS mission:*:summary");
    if (!keys_reply || keys_reply->type != REDIS_REPLY_ARRAY) {
        if (keys_reply) freeReplyObject(keys_reply);
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
    return jb.buf;
}

/* ----------------------------------------------------------------------------
   Embedded Dashboard HTML (same nice UI as before)
   ---------------------------------------------------------------------------- */
static const char *dashboard_html =
"<!DOCTYPE html>\n"
"<html lang=\"en\">\n"
"<head>\n"
"    <meta charset=\"UTF-8\">\n"
"    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
"    <title>Maze Mission Dashboard</title>\n"
"    <script src=\"https://cdn.tailwindcss.com\"></script>\n"
"    <style>\n"
"        @import url('https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600&display=swap');\n"
"        body { font-family: 'Inter', system_ui, sans-serif; }\n"
"        .mission-table tr:hover { background-color: #1f2937; }\n"
"    </style>\n"
"</head>\n"
"<body class=\"bg-gray-950 text-gray-100\">\n"
"    <div class=\"max-w-screen-2xl mx-auto p-8\">\n"
"        <div class=\"flex items-center justify-between mb-10\">\n"
"            <div>\n"
"                <div class=\"flex items-center gap-x-3\">\n"
"                    <div class=\"w-10 h-10 bg-emerald-500 rounded-2xl flex items-center justify-center text-3xl\">🧭</div>\n"
"                    <h1 class=\"text-5xl font-semibold tracking-tight\">Maze Mission Dashboard</h1>\n"
"                </div>\n"
"                <p class=\"text-gray-400 mt-1\">Real-time mission data • Powered by Redis</p>\n"
"            </div>\n"
"            <div class=\"flex items-center gap-x-4\">\n"
"                <button onclick=\"loadMissions()\" class=\"flex items-center gap-x-2 bg-white text-gray-900 hover:bg-emerald-400 px-6 py-3 rounded-3xl font-medium transition-colors\">\n"
"                    <span>↻</span> Refresh Now\n"
"                </button>\n"
"                <div id=\"last-updated\" class=\"text-xs text-gray-500 font-mono\"></div>\n"
"            </div>\n"
"        </div>\n"
"        <div id=\"summary-cards\" class=\"grid grid-cols-1 md:grid-cols-3 gap-6 mb-10\"></div>\n"
"        <div class=\"bg-gray-900 rounded-3xl overflow-hidden border border-gray-800\">\n"
"            <div class=\"px-8 py-5 border-b border-gray-800 flex items-center justify-between\">\n"
"                <h2 class=\"text-xl font-semibold\">All Missions</h2>\n"
"                <span id=\"mission-count-badge\" class=\"inline-flex items-center px-4 h-8 rounded-3xl bg-gray-800 text-sm font-medium\"></span>\n"
"            </div>\n"
"            <div class=\"overflow-x-auto\">\n"
"                <table class=\"w-full mission-table\">\n"
"                    <thead>\n"
"                        <tr class=\"bg-gray-950 text-xs uppercase tracking-widest text-gray-400 border-b border-gray-800\">\n"
"                            <th class=\"px-8 py-5 text-left\">Mission ID</th>\n"
"                            <th class=\"px-8 py-5 text-left\">Robot ID</th>\n"
"                            <th class=\"px-8 py-5 text-left\">Type</th>\n"
"                            <th class=\"px-8 py-5 text-left\">Start Time</th>\n"
"                            <th class=\"px-8 py-5 text-left\">End Time</th>\n"
"                            <th class=\"px-8 py-5 text-left\">Result</th>\n"
"                            <th class=\"px-8 py-5 text-right\">Distance (m)</th>\n"
"                            <th class=\"px-8 py-5 text-right\">Duration (s)</th>\n"
"                            <th class=\"px-8 py-5 text-right\">Total Moves</th>\n"
"                        </tr>\n"
"                    </thead>\n"
"                    <tbody id=\"missions-body\" class=\"text-sm divide-y divide-gray-800\"></tbody>\n"
"                </table>\n"
"            </div>\n"
"        </div>\n"
"        <div id=\"empty-state\" class=\"hidden text-center py-20\">\n"
"            <div class=\"text-7xl mb-4\">🧭</div>\n"
"            <h3 class=\"text-2xl font-medium mb-2\">No missions yet</h3>\n"
"            <p class=\"text-gray-400 max-w-xs mx-auto\">POST mission data to <span class=\"font-mono bg-gray-900 px-2 py-1 rounded\">/mission</span> and it will appear here instantly.</p>\n"
"        </div>\n"
"    </div>\n"
"    <script>\n"
"        async function loadMissions() {\n"
"            try {\n"
"                const res = await fetch('/api/missions');\n"
"                const missions = await res.json();\n"
"                missions.sort((a, b) => (b.start_time || '').localeCompare(a.start_time || ''));\n"
"                const tbody = document.getElementById('missions-body');\n"
"                tbody.innerHTML = '';\n"
"                let successCount = 0, abortedCount = 0, totalDuration = 0, totalDistance = 0, totalMoves = 0;\n"
"                missions.forEach(m => {\n"
"                    const result = m.mission_result || 'unknown';\n"
"                    let resultHTML = '';\n"
"                    if (result === 'success') { successCount++; resultHTML = `<span class=\"inline-flex items-center px-4 h-7 rounded-3xl text-xs font-semibold bg-emerald-400 text-gray-900\">SUCCESS</span>`; }\n"
"                    else if (result === 'aborted') { abortedCount++; resultHTML = `<span class=\"inline-flex items-center px-4 h-7 rounded-3xl text-xs font-semibold bg-amber-400 text-gray-900\">ABORTED</span>`; }\n"
"                    else resultHTML = `<span class=\"inline-flex items-center px-4 h-7 rounded-3xl text-xs font-semibold bg-red-400 text-gray-900\">${result.toUpperCase()}</span>`;\n"
"                    const row = document.createElement('tr');\n"
"                    row.innerHTML = `\n"
"                        <td class=\"px-8 py-5 font-mono text-emerald-300\">${m.mission_id || '-'}</td>\n"
"                        <td class=\"px-8 py-5\">${m.robot_id || '-'}</td>\n"
"                        <td class=\"px-8 py-5 font-medium\">${m.mission_type || '-'}</td>\n"
"                        <td class=\"px-8 py-5 text-gray-400 font-mono text-xs\">${m.start_time || '-'}</td>\n"
"                        <td class=\"px-8 py-5 text-gray-400 font-mono text-xs\">${m.end_time || '-'}</td>\n"
"                        <td class=\"px-8 py-5\">${resultHTML}</td>\n"
"                        <td class=\"px-8 py-5 text-right font-medium\">${m.distance_traveled || '0'}</td>\n"
"                        <td class=\"px-8 py-5 text-right font-medium\">${m.duration_seconds || '0'}</td>\n"
"                        <td class=\"px-8 py-5 text-right font-medium\">${m.moves_total || '0'}</td>\n"
"                    `;\n"
"                    tbody.appendChild(row);\n"
"                    if (m.duration_seconds) totalDuration += parseFloat(m.duration_seconds);\n"
"                    if (m.distance_traveled) totalDistance += parseFloat(m.distance_traveled);\n"
"                    if (m.moves_total) totalMoves += parseInt(m.moves_total);\n"
"                });\n"
"                const total = missions.length;\n"
"                const successRate = total ? Math.round((successCount / total) * 100) : 0;\n"
"                const avgDuration = total ? (totalDuration / total).toFixed(1) : '0';\n"
"                document.getElementById('summary-cards').innerHTML = `\n"
"                    <div class=\"bg-gray-900 rounded-3xl p-8 flex flex-col\"><div class=\"text-emerald-400 text-sm font-medium\">TOTAL MISSIONS</div><div class=\"text-7xl font-semibold mt-2\">${total}</div></div>\n"
"                    <div class=\"bg-gray-900 rounded-3xl p-8 flex flex-col\"><div class=\"text-emerald-400 text-sm font-medium\">SUCCESS RATE</div><div class=\"text-7xl font-semibold mt-2\">${successRate}<span class=\"text-3xl\">%</span></div><div class=\"text-xs text-gray-400 mt-1\">${successCount} successful</div></div>\n"
"                    <div class=\"bg-gray-900 rounded-3xl p-8 flex flex-col\"><div class=\"text-emerald-400 text-sm font-medium\">AVG DURATION</div><div class=\"text-7xl font-semibold mt-2\">${avgDuration}<span class=\"text-3xl\">s</span></div><div class=\"text-xs text-gray-400 mt-1\">${totalMoves} total moves</div></div>\n"
"                `;\n"
"                document.getElementById('mission-count-badge').textContent = `${total} missions`;\n"
"                document.getElementById('last-updated').textContent = `Last updated: ${new Date().toLocaleTimeString()}`;\n"
"                document.getElementById('empty-state').classList.toggle('hidden', total > 0);\n"
"            } catch (err) { console.error(err); }\n"
"        }\n"
"        window.onload = () => { loadMissions(); setInterval(loadMissions, 3000); };\n"
"    </script>\n"
"</body>\n"
"</html>\n";

/* ----------------------------------------------------------------------------
   Connection info for POST
   ---------------------------------------------------------------------------- */
typedef struct {
    char  *data;
    size_t size;
} ConnectionInfo;

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

        if (strcmp(url, "/favicon.ico") == 0) {
            struct MHD_Response *response = MHD_create_response_from_buffer(0, "", MHD_RESPMEM_PERSISTENT);
            enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_NO_CONTENT, response);
            MHD_destroy_response(response);
            return ret;
        }
    }

    /* POST /mission */
    if (strcmp(method, "POST") == 0 && strcmp(url, "/mission") == 0) {
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

    redisReply *reply = redisCommand(redis_ctx, "SELECT 7");
    if (!reply) {
        fprintf(stderr, "Failed to select Redis DB 7\n");
        redisFree(redis_ctx);
        return 1;
    }
    freeReplyObject(reply);
    printf("Connected to Redis at %s:%d (DB 7)\n", redis_host, redis_port);

    char *cert = read_file(cert_file);
    char *key  = read_file(key_file);
    if (!cert || !key) {
        fprintf(stderr, "Failed to load certificate or key file\n");
        free(cert); free(key);
        redisFree(redis_ctx);
        return 1;
    }

    struct MHD_Daemon *daemon = MHD_start_daemon(
        MHD_USE_THREAD_PER_CONNECTION | MHD_USE_TLS,
        DEFAULT_PORT,
        NULL, NULL,
        &http_handler, NULL,
        MHD_OPTION_HTTPS_MEM_CERT, cert,
        MHD_OPTION_HTTPS_MEM_KEY,  key,
        MHD_OPTION_END);

    if (!daemon) {
        fprintf(stderr, "Failed to start MHD daemon (TLS). Check cert/key and that port %d is free.\n", DEFAULT_PORT);
        free(cert); free(key);
        redisFree(redis_ctx);
        return 1;
    }

    printf("Redis HTTPS server running on https://0.0.0.0:%d\n", DEFAULT_PORT);
    printf("   → Dashboard: https://127.0.0.1:%d/dashboard\n", DEFAULT_PORT);
    printf("   → API: https://127.0.0.1:%d/api/missions\n", DEFAULT_PORT);
    printf("Press Enter to stop...\n");

    getchar();

    MHD_stop_daemon(daemon);
    free(cert);
    free(key);
    redisFree(redis_ctx);
    return 0;
}
