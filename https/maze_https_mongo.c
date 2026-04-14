// Compile with:
// gcc -O2 -Wall -Wextra -std=c11 maze_https_mongo.c -o maze_https_mongo $(pkg-config --cflags --libs libmicrohttpd libmongoc-1.0 gnutls)

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <microhttpd.h>
#include <mongoc/mongoc.h>
#include <bson/bson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_MONGO_HTTPS_PORT 8447

static const char *cert_file = "certs/server.crt";
static const char *key_file  = "certs/server.key";

static mongoc_client_t *mongo_client;

struct connection_info {
    char *data;
    size_t size;
};

struct app_config {
    const char *mongo_uri;
    const char *mongo_db;
    const char *mongo_col;
};

static struct app_config config;

/* ----------------------------------------------------------------------------
   Read entire file into memory
   ---------------------------------------------------------------------------- */
static char *read_file(const char *path)
{
    if (!path) return NULL;

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
   Get current UTC time in ISO8601 format
   ---------------------------------------------------------------------------- */
static void get_utc_iso8601(char *buf, size_t len)
{
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* ----------------------------------------------------------------------------
   Return last 200 moves as JSON
   ---------------------------------------------------------------------------- */
static char *get_moves_json(void)
{
    mongoc_collection_t *col = mongoc_client_get_collection(mongo_client,
                                                           config.mongo_db,
                                                           config.mongo_col);
    if (!col) {
        fprintf(stderr, "Failed to get collection\n");
        return strdup("[]");
    }

    bson_t *filter = bson_new();
    bson_t *opts   = bson_new();

    BSON_APPEND_INT32(opts, "limit", 200);

    // Sort by received_at descending (newest first)
    bson_t *sort_doc = bson_new();
    BSON_APPEND_INT32(sort_doc, "received_at", -1);
    BSON_APPEND_DOCUMENT(opts, "sort", sort_doc);
    bson_destroy(sort_doc);

    mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(col, filter, opts, NULL);

    bson_destroy(filter);
    bson_destroy(opts);

    size_t cap = 16384;
    char *buf = malloc(cap);
    if (!buf) {
        mongoc_cursor_destroy(cursor);
        mongoc_collection_destroy(col);
        return strdup("[]");
    }

    size_t pos = 0;
    buf[pos++] = '[';
    bool first = true;

    const bson_t *doc;
    while (mongoc_cursor_next(cursor, &doc)) {
        char *doc_json = bson_as_canonical_extended_json(doc, NULL);
        if (!doc_json) continue;

        size_t doclen = strlen(doc_json);

        if (!first) {
            if (pos + 1 >= cap) {
                cap *= 2;
                buf = realloc(buf, cap);
            }
            buf[pos++] = ',';
        }
        first = false;

        if (pos + doclen + 2 >= cap) {
            cap = cap * 2 + doclen + 1024;
            buf = realloc(buf, cap);
        }

        memcpy(buf + pos, doc_json, doclen);
        pos += doclen;
        bson_free(doc_json);
    }

    if (pos + 1 >= cap) buf = realloc(buf, cap + 1);
    buf[pos++] = ']';
    buf[pos] = '\0';

    mongoc_cursor_destroy(cursor);
    mongoc_collection_destroy(col);

    return buf;
}

/* ----------------------------------------------------------------------------
   HTTP handler - handles small and large POST bodies
   ---------------------------------------------------------------------------- */
static enum MHD_Result http_handler(void *cls,
                                    struct MHD_Connection *connection,
                                    const char *url,
                                    const char *method,
                                    const char *version,
                                    const char *upload_data,
                                    size_t *upload_data_size,
                                    void **con_cls)
{
    (void)cls;
    (void)version;

    /* GET /api/moves */
    if (strcmp(method, "GET") == 0 && strcmp(url, "/api/moves") == 0) {
        char *json = get_moves_json();
        struct MHD_Response *response = MHD_create_response_from_buffer(
            strlen(json), json, MHD_RESPMEM_MUST_FREE);
        MHD_add_response_header(response, "Content-Type", "application/json");
        MHD_add_response_header(response, "Cache-Control", "no-cache");
        MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
        enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
        MHD_destroy_response(response);
        return ret;
    }

    /* POST /move */
    if (strcmp(method, "POST") == 0 && strcmp(url, "/move") == 0) {
        struct connection_info *ci = *con_cls;

        /* First call for this request */
        if (ci == NULL) {
            ci = calloc(1, sizeof(*ci));
            if (!ci) return MHD_NO;
            *con_cls = ci;
            return MHD_YES;  // Return YES to continue receiving data
        }

        /* Append any data provided in this callback */
        if (*upload_data_size > 0) {
            char *new_data = realloc(ci->data, ci->size + *upload_data_size + 1);
            if (!new_data) {
                free(ci->data);
                free(ci);
                *con_cls = NULL;
                return MHD_NO;
            }
            ci->data = new_data;
            
            memcpy(ci->data + ci->size, upload_data, *upload_data_size);
            ci->size += *upload_data_size;
            ci->data[ci->size] = '\0';
            
            *upload_data_size = 0;  // Mark as consumed
            return MHD_YES;
        }

        /* No more data → process the full body now */
        if (ci->size == 0 || !ci->data) {
            // Empty POST body
            const char *reply = "{\"error\":\"empty body\"}";
            struct MHD_Response *response = MHD_create_response_from_buffer(
                strlen(reply), (void *)reply, MHD_RESPMEM_PERSISTENT);
            enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_BAD_REQUEST, response);
            MHD_destroy_response(response);
            
            free(ci->data);
            free(ci);
            *con_cls = NULL;
            return ret;
        }
        
        printf("Received POST data (%zu bytes): %s\n", ci->size, ci->data);
        
        /* Parse JSON and store */
        bson_error_t error;
        bson_t *doc = bson_new_from_json((uint8_t *)ci->data, ci->size, &error);
        if (!doc) {
            fprintf(stderr, "JSON parse error: %s\n", error.message);
            const char *reply = "{\"error\":\"invalid json\"}";
            struct MHD_Response *response = MHD_create_response_from_buffer(
                strlen(reply), (void *)reply, MHD_RESPMEM_PERSISTENT);
            MHD_add_response_header(response, "Content-Type", "application/json");
            MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
            
            enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_BAD_REQUEST, response);
            MHD_destroy_response(response);
            
            free(ci->data);
            free(ci);
            *con_cls = NULL;
            return ret;
        }

        char ts[64];
        get_utc_iso8601(ts, sizeof(ts));
        BSON_APPEND_UTF8(doc, "received_at", ts);

        mongoc_collection_t *col = mongoc_client_get_collection(mongo_client,
                                                            config.mongo_db,
                                                            config.mongo_col);
        if (!col) {
            const char *reply = "{\"error\":\"database connection failed\"}";
            struct MHD_Response *response = MHD_create_response_from_buffer(
                strlen(reply), (void *)reply, MHD_RESPMEM_PERSISTENT);
            MHD_add_response_header(response, "Content-Type", "application/json");
            MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
            
            enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, response);
            MHD_destroy_response(response);
            
            bson_destroy(doc);
            free(ci->data);
            free(ci);
            *con_cls = NULL;
            return ret;
        }
        
        bool ok = mongoc_collection_insert_one(col, doc, NULL, NULL, &error);
        if (!ok) {
            fprintf(stderr, "Mongo insert failed: %s\n", error.message);
        }
        mongoc_collection_destroy(col);
        
        if (!ok) {
            fprintf(stderr, "Mongo insert failed: %s\n", error.message);
            const char *reply = "{\"error\":\"mongo insert failed\"}";
            struct MHD_Response *response = MHD_create_response_from_buffer(
                strlen(reply), (void *)reply, MHD_RESPMEM_PERSISTENT);
            MHD_add_response_header(response, "Content-Type", "application/json");
            MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
            
            enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, response);
            MHD_destroy_response(response);
            
            bson_destroy(doc);
            free(ci->data);
            free(ci);
            *con_cls = NULL;
            return ret;
        }

        printf("Insert successful\n");
        bson_destroy(doc);

        /* Success response */
        const char *reply = "{\"status\":\"ok\"}";
        struct MHD_Response *response = MHD_create_response_from_buffer(
            strlen(reply), (void *)reply, MHD_RESPMEM_PERSISTENT);
        MHD_add_response_header(response, "Content-Type", "application/json");
        MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
        
        enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
        MHD_destroy_response(response);
        
        /* Cleanup */
        free(ci->data);
        free(ci);
        *con_cls = NULL;
        
        return ret;
    }

    /* 404 */
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
    config.mongo_uri = getenv("MONGO_URI");
    if (!config.mongo_uri || !*config.mongo_uri)
        config.mongo_uri = "mongodb://localhost:27017";

    config.mongo_db = getenv("MONGO_DB");
    if (!config.mongo_db || !*config.mongo_db)
        config.mongo_db = "team1fmoves";

    config.mongo_col = getenv("MONGO_COL");
    if (!config.mongo_col || !*config.mongo_col)
        config.mongo_col = "moves";

    int port = DEFAULT_MONGO_HTTPS_PORT;
    const char *env_port = getenv("MONGO_PORT");
    if (env_port && *env_port) {
        port = atoi(env_port);
    }

    mongoc_init();
    mongo_client = mongoc_client_new(config.mongo_uri);
    if (!mongo_client) {
        fprintf(stderr, "Failed to create MongoDB client\n");
        return 1;
    }

    char *cert_pem = read_file(cert_file);
    char *key_pem  = read_file(key_file);
    if (!cert_pem || !key_pem) {
        fprintf(stderr, "Failed to read certificate files\n");
        mongoc_cleanup();
        return 1;
    }

    struct MHD_Daemon *daemon = MHD_start_daemon(
        MHD_USE_THREAD_PER_CONNECTION | MHD_USE_TLS,
        port,
        NULL, NULL,
        &http_handler, NULL,
        MHD_OPTION_HTTPS_MEM_CERT, cert_pem,
        MHD_OPTION_HTTPS_MEM_KEY,  key_pem,
        MHD_OPTION_END);

    if (!daemon) {
        fprintf(stderr, "Failed to start HTTPS server on port %d\n", port);
        free(cert_pem);
        free(key_pem);
        mongoc_cleanup();
        return 1;
    }

    printf("MongoDB Telemetry Server running on https://0.0.0.0:%d\n", port);
    printf("   → POST moves   : https://...:%d/move\n", port);
    printf("   → GET telemetry: https://...:%d/api/moves\n", port);
    printf("\nPress Enter to stop...\n");

    // Allow non-interactive mode for integration tests
    if (getenv("NON_INTERACTIVE") == NULL) {
        getchar();
    } else {
        printf("Running in non-interactive mode (for tests)\n");
        // Keep running until killed
        while (1) {
            sleep(1);
        }
    }

    MHD_stop_daemon(daemon);
    free(cert_pem);
    free(key_pem);
    mongoc_cleanup();
    return 0;
}