// maze_https_minipupper.c
// Simple HTTPS POST server on the robot to receive telemetry JSON from maze simulator
// Receives same format as the move events: {"event_type":"player_move", ...}
// Compile with: gcc maze_https_minipupper.c -o maze_https_minipupper -lmicrohttpd -lssl -lcrypto
// Run: ./maze_https_minipupper
// Test: curl -k -X POST https://localhost:8447/telemetry -d '{"event_type":"player_move",...}'

#include <microhttpd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>   // for sockaddr_in (optional logging)

#define PORT 8447
#define CERT_FILE "certs/server.crt"
#define KEY_FILE "certs/server.key"
#define MAX_POST_SIZE (64 * 1024)

typedef struct {
    char  *buffer;
    size_t length;
    size_t capacity;
} PostData;

static void free_post_data(void *cls) {
    PostData *pd = (PostData *)cls;
    free(pd->buffer);
    free(pd);
}

static int handle_request(void *cls,
                          struct MHD_Connection *connection,
                          const char *url,
                          const char *method,
                          const char *version,
                          const char *upload_data,
                          size_t *upload_data_size,
                          void **con_cls)
{
    (void)cls; (void)version;

    // We only accept POST to /move
    if (0 != strcmp(method, "POST") || 0 != strcmp(url, "/move")) {
        const char *msg = "{\"error\":\"Method or path not allowed\"}";
        struct MHD_Response *resp = MHD_create_response_from_buffer(
            strlen(msg), (void*)msg, MHD_RESPMEM_PERSISTENT);
        int ret = MHD_queue_response(connection, MHD_HTTP_METHOD_NOT_ALLOWED, resp);
        MHD_destroy_response(resp);
        return ret;
    }

    // First call: allocate our post data container
    if (NULL == *con_cls) {
        PostData *pd = calloc(1, sizeof(PostData));
        if (!pd) return MHD_NO;

        pd->capacity = 4096;
        pd->buffer = malloc(pd->capacity);
        if (!pd->buffer) {
            free(pd);
            return MHD_NO;
        }
        pd->buffer[0] = '\0';

        *con_cls = pd;
        return MHD_YES;
    }

    PostData *pd = (PostData *)*con_cls;

    // Upload in progress
    if (*upload_data_size > 0) {
        // Enforce max size to prevent DoS
        if (pd->length + *upload_data_size > MAX_POST_SIZE) {
            return MHD_NO;
        }

        // Grow buffer if needed
        while (pd->length + *upload_data_size + 1 > pd->capacity) {
            pd->capacity *= 2;
            char *newbuf = realloc(pd->buffer, pd->capacity);
            if (!newbuf) return MHD_NO;
            pd->buffer = newbuf;
        }

        memcpy(pd->buffer + pd->length, upload_data, *upload_data_size);
        pd->length += *upload_data_size;
        pd->buffer[pd->length] = '\0';

        *upload_data_size = 0;
        return MHD_YES;
    }

    // Upload complete → process the JSON
    if (pd->length > 0) {
        // Here you can parse JSON if needed (e.g. with json-c or cJSON)
        // For now: just print/log the received telemetry
        printf("[TELEMETRY] Received at %s:\n%s\n\n", url, pd->buffer);
        fflush(stdout);

        // TODO: parse JSON and act on it
        // Example future steps:
        //   - json_object *obj = json_tokener_parse(pd->buffer);
        //   - if (strcmp(json_object_get_string(obj, "event_type"), "player_move") == 0)
        //         move_robot_based_on_position(...);
    }

    // Send simple success response
    const char *reply = "{\"status\":\"received\"}";
    struct MHD_Response *response = MHD_create_response_from_buffer(
        strlen(reply), (void*)reply, MHD_RESPMEM_PERSISTENT);

    MHD_add_response_header(response, "Content-Type", "application/json");

    int ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
    MHD_destroy_response(response);

    // Cleanup
    free_post_data(pd);
    *con_cls = NULL;

    return ret;
}

int main(void)
{
    // Load certificate and key (self-signed or real)
    FILE *cert_file = fopen(CERT_FILE, "r");
    FILE *key_file  = fopen(KEY_FILE,  "r");
    if (!cert_file || !key_file) {
        fprintf(stderr, "Failed to open cert/key files: %s / %s\n",
                CERT_FILE, KEY_FILE);
        if (cert_file) fclose(cert_file);
        if (key_file)  fclose(key_file);
        return 1;
    }

    fseek(cert_file, 0, SEEK_END);
    long cert_len = ftell(cert_file);
    fseek(cert_file, 0, SEEK_SET);
    char *cert = malloc(cert_len + 1);
    fread(cert, 1, cert_len, cert_file);
    cert[cert_len] = '\0';
    fclose(cert_file);

    fseek(key_file, 0, SEEK_END);
    long key_len = ftell(key_file);
    fseek(key_file, 0, SEEK_SET);
    char *key = malloc(key_len + 1);
    fread(key, 1, key_len, key_file);
    key[key_len] = '\0';
    fclose(key_file);

    struct MHD_Daemon *daemon = MHD_start_daemon(
        MHD_USE_THREAD_PER_CONNECTION | MHD_USE_TLS | MHD_USE_DUAL_STACK,
        PORT,
        NULL, NULL,
        &handle_request, NULL,
        MHD_OPTION_HTTPS_MEM_CERT, cert,
        MHD_OPTION_HTTPS_MEM_KEY,  key,
        MHD_OPTION_CONNECTION_MEMORY_LIMIT, (size_t)(128 * 1024),
        MHD_OPTION_END);

    if (NULL == daemon) {
        fprintf(stderr, "Failed to start HTTP(S) server on port %d\n", PORT);
        free(cert);
        free(key);
        return 1;
    }

    printf("Minipupper HTTPS server running on https://0.0.0.0:%d/telemetry\n", PORT);
    printf("Waiting for POST requests from maze simulator...\n");

    // Run forever (or until Ctrl+C)
    getchar();

    MHD_stop_daemon(daemon);
    free(cert);
    free(key);

    return 0;
}