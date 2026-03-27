#include <microhttpd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <cjson/cJSON.h>

#define PORT 8447
#define CERT_FILE "certs/server.crt"
#define KEY_FILE  "certs/server.key"

static int cur_x = 0;
static int cur_y = 0;
static int initialized = 0;

typedef struct {
    char *data;
    size_t size;
} Buffer;

static char *load_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }

    long len = ftell(f);
    if (len < 0) {
        fclose(f);
        return NULL;
    }

    rewind(f);

    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t n = fread(buf, 1, (size_t)len, f);
    fclose(f);

    if (n != (size_t)len) {
        free(buf);
        return NULL;
    }

    buf[len] = '\0';
    return buf;
}

static int extract_xy(const char *json, int *x, int *y) {
    cJSON *root = cJSON_Parse(json);
    if (!root) return 0;

    cJSON *player = cJSON_GetObjectItemCaseSensitive(root, "player");
    cJSON *pos = NULL;
    cJSON *x_item = NULL;
    cJSON *y_item = NULL;

    if (cJSON_IsObject(player)) {
        pos = cJSON_GetObjectItemCaseSensitive(player, "position");
    }

    if (!cJSON_IsObject(pos)) {
        cJSON_Delete(root);
        return 0;
    }

    x_item = cJSON_GetObjectItemCaseSensitive(pos, "x");
    y_item = cJSON_GetObjectItemCaseSensitive(pos, "y");

    if (!cJSON_IsNumber(x_item) || !cJSON_IsNumber(y_item)) {
        cJSON_Delete(root);
        return 0;
    }

    *x = x_item->valueint;
    *y = y_item->valueint;

    cJSON_Delete(root);
    return 1;
}

static void stop_robot(void) {
    system("timeout 1s ros2 topic pub -r 5 /cmd_vel geometry_msgs/msg/Twist "
           "\"{linear: {x: 0.0}, angular: {z: 0.0}}\" >/dev/null 2>&1");
}

static void move_forward(void) {
    system("timeout 2s ros2 topic pub -r 5 /cmd_vel geometry_msgs/msg/Twist "
           "\"{linear: {x: 0.1}, angular: {z: 0.0}}\" >/dev/null 2>&1");
    stop_robot();
}

static void turn_left(void) {
    system("timeout 1.2s ros2 topic pub -r 5 /cmd_vel geometry_msgs/msg/Twist "
           "\"{linear: {x: 0.0}, angular: {z: 0.8}}\" >/dev/null 2>&1");
    stop_robot();
}

static void turn_right(void) {
    system("timeout 1.2s ros2 topic pub -r 5 /cmd_vel geometry_msgs/msg/Twist "
           "\"{linear: {x: 0.0}, angular: {z: -0.8}}\" >/dev/null 2>&1");
    stop_robot();
}

static void turn_around(void) {
    system("timeout 2.4s ros2 topic pub -r 5 /cmd_vel geometry_msgs/msg/Twist "
           "\"{linear: {x: 0.0}, angular: {z: 0.8}}\" >/dev/null 2>&1");
    stop_robot();
}

static void handle_movement(int new_x, int new_y) {
    if (!initialized) {
        cur_x = new_x;
        cur_y = new_y;
        initialized = 1;
        printf("[INIT] (%d,%d)\n", cur_x, cur_y);
        fflush(stdout);
        return;
    }

    int dx = new_x - cur_x;
    int dy = new_y - cur_y;

    printf("[MOVE] (%d,%d) -> (%d,%d)\n", cur_x, cur_y, new_x, new_y);

    if (dx == 1 && dy == 0) {
        printf("-> RIGHT\n");
        turn_right();
        move_forward();
    } else if (dx == -1 && dy == 0) {
        printf("<- LEFT\n");
        turn_left();
        move_forward();
    } else if (dx == 0 && dy == -1) {
        printf("^ UP\n");
        move_forward();
    } else if (dx == 0 && dy == 1) {
        printf("v DOWN\n");
        turn_around();
        move_forward();
    } else {
        printf("[WARN] Invalid step\n");
    }

    fflush(stdout);

    cur_x = new_x;
    cur_y = new_y;
}

static void request_completed(
    void *cls,
    struct MHD_Connection *connection,
    void **con_cls,
    enum MHD_RequestTerminationCode toe)
{
    (void)cls;
    (void)connection;
    (void)toe;

    Buffer *buf = *con_cls;
    if (buf) {
        free(buf->data);
        free(buf);
        *con_cls = NULL;
    }
}

static enum MHD_Result handle_request(
    void *cls,
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

    if (strcmp(method, "POST") != 0 || strcmp(url, "/move") != 0) {
        const char *reply = "{\"status\":\"not found\"}";
        struct MHD_Response *response =
            MHD_create_response_from_buffer(strlen(reply), (void *)reply, MHD_RESPMEM_PERSISTENT);
        enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, response);
        MHD_destroy_response(response);
        return ret;
    }

    if (*con_cls == NULL) {
        Buffer *buf = calloc(1, sizeof(Buffer));
        if (!buf) {
            return MHD_NO;
        }
        *con_cls = buf;
        return MHD_YES;
    }

    Buffer *buf = (Buffer *)*con_cls;

    if (*upload_data_size > 0) {
        char *new_data = realloc(buf->data, buf->size + *upload_data_size + 1);
        if (!new_data) {
            return MHD_NO;
        }

        buf->data = new_data;
        memcpy(buf->data + buf->size, upload_data, *upload_data_size);
        buf->size += *upload_data_size;
        buf->data[buf->size] = '\0';

        *upload_data_size = 0;
        return MHD_YES;
    }

    int x, y;
    const char *reply_ok = "{\"status\":\"ok\"}";
    const char *reply_bad = "{\"status\":\"bad json\"}";
    struct MHD_Response *response;
    enum MHD_Result ret;

    if (buf->data && extract_xy(buf->data, &x, &y)) {
        handle_movement(x, y);
        response = MHD_create_response_from_buffer(
            strlen(reply_ok), (void *)reply_ok, MHD_RESPMEM_PERSISTENT);
        ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
    } else {
        response = MHD_create_response_from_buffer(
            strlen(reply_bad), (void *)reply_bad, MHD_RESPMEM_PERSISTENT);
        ret = MHD_queue_response(connection, MHD_HTTP_BAD_REQUEST, response);
    }

    MHD_destroy_response(response);
    return ret;
}

int main(void) {
    char *cert_pem = load_file(CERT_FILE);
    char *key_pem = load_file(KEY_FILE);

    if (!cert_pem || !key_pem) {
        fprintf(stderr, "Failed to load TLS certificate or key\n");
        free(cert_pem);
        free(key_pem);
        return 1;
    }

    struct MHD_Daemon *daemon = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_TLS,
        PORT,
        NULL, NULL,
        &handle_request, NULL,
        MHD_OPTION_HTTPS_MEM_CERT, cert_pem,
        MHD_OPTION_HTTPS_MEM_KEY, key_pem,
        MHD_OPTION_NOTIFY_COMPLETED, request_completed, NULL,
        MHD_OPTION_END
    );

    if (!daemon) {
        fprintf(stderr, "Failed to start HTTPS server\n");
        free(cert_pem);
        free(key_pem);
        return 1;
    }

    printf("Server running on https://0.0.0.0:%d/move\n", PORT);
    printf("Press Enter to stop...\n");
    getchar();

    MHD_stop_daemon(daemon);
    free(cert_pem);
    free(key_pem);
    return 0;
}
