/*
Compile:
gcc -Wall -Wextra -O2 -std=c11 maze_https_minipupper.c -o maze_https_minipupper \
  $(pkg-config --cflags --libs libmicrohttpd libcjson) -lpthread

Run from a shell where ROS 2 is already sourced, for example:
source ~/.bashrc
source /opt/ros/humble/setup.bash
source ~/ros2_ws/install/setup.bash
./maze_https_minipupper

Run this program from the directory that contains:
  certs/server.crt
  certs/server.key
*/

#include <microhttpd.h>
#include <cjson/cJSON.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define PORT 8447
#define CERT_FILE "certs/server.crt"
#define KEY_FILE  "certs/server.key"

#define PUB_RATE       15
#define STOP_COUNT     2
#define FORWARD_COUNT  3
#define TURN90_COUNT   4
#define TURN180_COUNT  8

#define FORWARD_SPEED  0.05
#define TURN_SPEED     0.75

#define SETTLE_US      120000

typedef struct {
    char *data;
    size_t size;
} Buffer;

typedef enum {
    DIR_NORTH = 0,
    DIR_EAST  = 1,
    DIR_SOUTH = 2,
    DIR_WEST  = 3
} Heading;

static int cur_x = 0;
static int cur_y = 0;
static int initialized = 0;
static Heading heading = DIR_NORTH;

static pthread_mutex_t move_mutex = PTHREAD_MUTEX_INITIALIZER;

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

static int run_cmd(const char *cmd) {
    int rc = system(cmd);

    if (rc == -1) {
        perror("system");
        return 0;
    }

    if (WIFEXITED(rc) && WEXITSTATUS(rc) == 0) {
        return 1;
    }

    fprintf(stderr, "Command failed:\n%s\n", cmd);
    return 0;
}

static int publish_twist_count(int times, double linear_x, double angular_z) {
    char cmd[1024];

    snprintf(
        cmd,
        sizeof(cmd),
        "ros2 topic pub --times %d -r %d /cmd_vel geometry_msgs/msg/Twist "
        "\"{linear: {x: %.3f}, angular: {z: %.3f}}\" >/dev/null 2>&1",
        times, PUB_RATE, linear_x, angular_z
    );

    return run_cmd(cmd);
}

static void stop_robot(void) {
    publish_twist_count(STOP_COUNT, 0.0, 0.0);
}

static void move_forward_small(void) {
    publish_twist_count(FORWARD_COUNT, FORWARD_SPEED, 0.0);
    stop_robot();
    usleep(SETTLE_US);
}

static void turn_left_90(void) {
    publish_twist_count(TURN90_COUNT, 0.0, TURN_SPEED);
    stop_robot();
    usleep(SETTLE_US);
}

static void turn_right_90(void) {
    publish_twist_count(TURN90_COUNT, 0.0, -TURN_SPEED);
    stop_robot();
    usleep(SETTLE_US);
}

static void turn_around_180(void) {
    publish_twist_count(TURN180_COUNT, 0.0, TURN_SPEED);
    stop_robot();
    usleep(SETTLE_US);
}

static int extract_xy(const char *json, int *x, int *y) {
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return 0;
    }

    cJSON *player = cJSON_GetObjectItemCaseSensitive(root, "player");
    if (!cJSON_IsObject(player)) {
        cJSON_Delete(root);
        return 0;
    }

    cJSON *position = cJSON_GetObjectItemCaseSensitive(player, "position");
    if (!cJSON_IsObject(position)) {
        cJSON_Delete(root);
        return 0;
    }

    cJSON *x_item = cJSON_GetObjectItemCaseSensitive(position, "x");
    cJSON *y_item = cJSON_GetObjectItemCaseSensitive(position, "y");

    if (!cJSON_IsNumber(x_item) || !cJSON_IsNumber(y_item)) {
        cJSON_Delete(root);
        return 0;
    }

    *x = x_item->valueint;
    *y = y_item->valueint;

    cJSON_Delete(root);
    return 1;
}

static Heading heading_for_delta(int dx, int dy) {
    if (dx == 0 && dy == -1) return DIR_NORTH;
    if (dx == 1 && dy == 0)  return DIR_EAST;
    if (dx == 0 && dy == 1)  return DIR_SOUTH;
    return DIR_WEST;
}

static void rotate_to_heading(Heading target) {
    int diff = ((int)target - (int)heading + 4) % 4;

    if (diff == 1) {
        turn_right_90();
    } else if (diff == 2) {
        turn_around_180();
    } else if (diff == 3) {
        turn_left_90();
    }

    heading = target;
}

static void handle_movement(int new_x, int new_y) {
    pthread_mutex_lock(&move_mutex);

    if (!initialized) {
        cur_x = new_x;
        cur_y = new_y;
        heading = DIR_NORTH;
        initialized = 1;
        printf("[INIT] (%d,%d)\n", cur_x, cur_y);
        fflush(stdout);
        pthread_mutex_unlock(&move_mutex);
        return;
    }

    int dx = new_x - cur_x;
    int dy = new_y - cur_y;

    printf("[MOVE] (%d,%d) -> (%d,%d)\n", cur_x, cur_y, new_x, new_y);

    if (!((dx == 1 && dy == 0) ||
          (dx == -1 && dy == 0) ||
          (dx == 0 && dy == 1) ||
          (dx == 0 && dy == -1))) {
        printf("[WARN] Invalid step\n");
        fflush(stdout);
        pthread_mutex_unlock(&move_mutex);
        return;
    }

    Heading target = heading_for_delta(dx, dy);
    rotate_to_heading(target);
    move_forward_small();
    stop_robot();

    cur_x = new_x;
    cur_y = new_y;

    fflush(stdout);
    pthread_mutex_unlock(&move_mutex);
}

static enum MHD_Result send_json(struct MHD_Connection *connection,
                                 unsigned int status_code,
                                 const char *json_text)
{
    struct MHD_Response *response = MHD_create_response_from_buffer(
        strlen(json_text),
        (void *)json_text,
        MHD_RESPMEM_PERSISTENT
    );

    if (!response) {
        return MHD_NO;
    }

    MHD_add_response_header(response, "Content-Type", "application/json");

    enum MHD_Result ret = MHD_queue_response(connection, status_code, response);
    MHD_destroy_response(response);
    return ret;
}

static void request_completed(void *cls,
                              struct MHD_Connection *connection,
                              void **con_cls,
                              enum MHD_RequestTerminationCode toe)
{
    (void)cls;
    (void)connection;
    (void)toe;

    Buffer *buf = (Buffer *)*con_cls;
    if (buf) {
        free(buf->data);
        free(buf);
        *con_cls = NULL;
    }
}

static enum MHD_Result handle_request(void *cls,
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

    if (strcmp(url, "/move") != 0) {
        return send_json(connection, MHD_HTTP_NOT_FOUND, "{\"status\":\"not found\"}");
    }

    if (strcmp(method, "POST") != 0) {
        return send_json(connection, MHD_HTTP_METHOD_NOT_ALLOWED, "{\"status\":\"method not allowed\"}");
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
    if (buf->data && extract_xy(buf->data, &x, &y)) {
        handle_movement(x, y);
        return send_json(connection, MHD_HTTP_OK, "{\"status\":\"ok\"}");
    }

    return send_json(connection, MHD_HTTP_BAD_REQUEST, "{\"status\":\"bad json\"}");
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
        MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_THREAD_PER_CONNECTION | MHD_USE_TLS,
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
