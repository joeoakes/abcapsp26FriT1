// maze_https_minipupper.c
// Simple HTTPS POST server on the robot to receive telemetry JSON from maze simulator
// Receives same format as the move events: {"event_type":"player_move", ...}
//
// Build:
// source /opt/ros/jazzy/setup.bash

// gcc maze_https_minipupper.c -o maze_https_minipupper \
//   -I/opt/ros/jazzy/include \
//   -I/opt/ros/jazzy/include/rcl \
//   -I/opt/ros/jazzy/include/rclc \
//   -I/opt/ros/jazzy/include/rcutils \
//   -I/opt/ros/jazzy/include/rmw \
//   -I/opt/ros/jazzy/include/rcl_action \
//   -I/opt/ros/jazzy/include/rcl_yaml_param_parser \
//   -I/opt/ros/jazzy/include/type_description_interfaces \
//   -I/opt/ros/jazzy/include/rosidl_runtime_c \
//   -I/opt/ros/jazzy/include/rosidl_typesupport_interface \
//   -I/opt/ros/jazzy/include/rosidl_dynamic_typesupport \
//   -I/opt/ros/jazzy/include/service_msgs \
//   -I/opt/ros/jazzy/include/builtin_interfaces \
//   -I/opt/ros/jazzy/include/unique_identifier_msgs \
//   -I/opt/ros/jazzy/include/action_msgs \
//   -I/opt/ros/jazzy/include/geometry_msgs \
//   -L/opt/ros/jazzy/lib \
//   $(pkg-config --cflags --libs libmicrohttpd gnutls libcjson) \
//   -lrclc -lrcl -lrcutils -lrmw -lrcl_action \
//   -lrosidl_typesupport_c \
//   -lgeometry_msgs__rosidl_generator_c \
//   -lgeometry_msgs__rosidl_typesupport_c \
//   -lm

//
// Run after compiling:
//   ./maze_https_minipupper
//
// Notes:
// - bringup must be running so /cmd_vel is consumed
// - adjust CELL_METERS / speeds for your maze

#include <microhttpd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <math.h>

// ---- JSON ----
#include <cjson/cJSON.h>

// ---- ROS 2 (rclc) ----
#include <rcl/rcl.h>
#include <rclc/rclc.h>
#include <geometry_msgs/msg/twist.h>

#define PORT 8447
#define CERT_FILE "certs/server.crt"
#define KEY_FILE "certs/server.key"
#define MAX_POST_SIZE (64 * 1024)

// ---- motion tuning ----
#define CELL_METERS  0.25    // set to your cell size (meters)
#define LIN_SPEED    0.10    // m/s
#define ANG_SPEED    0.80    // rad/s (not used unless you add turning)
#define PUB_HZ       10      // publish rate while moving

typedef struct {
    char  *buffer;
    size_t length;
    size_t capacity;
} PostData;

// ---- robot state (grid) ----
typedef struct {
    int cur_x;
    int cur_y;
    int initialized; // 0 until first JSON sets it
} RobotState;

static RobotState g_state = {0, 0, 0};

// ---- ROS globals ----
static rclc_support_t g_support;
static rcl_allocator_t g_alloc;
static rcl_node_t g_node;
static rcl_publisher_t g_cmdvel_pub;
static int g_ros_ready = 0;

static void free_post_data(void *cls) {
    PostData *pd = (PostData *)cls;
    free(pd->buffer);
    free(pd);
}

// ---------------- ROS helpers ----------------

static void publish_twist(double lin_x, double ang_z) {
    if (!g_ros_ready) return;

    geometry_msgs__msg__Twist msg;
    geometry_msgs__msg__Twist__init(&msg);
    msg.linear.x  = lin_x;
    msg.angular.z = ang_z;

    rcl_ret_t rc = rcl_publish(&g_cmdvel_pub, &msg, NULL);
    (void)rc; // optional: if (rc != RCL_RET_OK) fprintf(stderr, "publish failed\n");
    geometry_msgs__msg__Twist__fini(&msg);
}

static void stop_robot(void) {
    for (int i = 0; i < 5; i++) {
        publish_twist(0.0, 0.0);
        usleep(100000); // 100ms
    }
}

static void run_for_seconds(double lin_x, double ang_z, double seconds) {
    int ticks = (int)ceil(seconds * PUB_HZ);
    int sleep_us = (int)(1e6 / PUB_HZ);

    for (int i = 0; i < ticks; i++) {
        publish_twist(lin_x, ang_z);
        usleep(sleep_us);
    }
    stop_robot();
}

static void forward_one_cell(void) {
    double seconds = CELL_METERS / LIN_SPEED;
    run_for_seconds(LIN_SPEED, 0.0, seconds);
}

// Very simple: move 1 step closer to target (x first, then y)
static void step_toward_target(RobotState *st, int tx, int ty) {
    int dx = tx - st->cur_x;
    int dy = ty - st->cur_y;

    if (dx == 0 && dy == 0) return;

    // x priority
    if (dx != 0) {
        // NOTE: this assumes robot is already oriented to move +x as "forward".
        // If you need real turning based on heading, we can add heading logic.
        forward_one_cell();
        st->cur_x += (dx > 0) ? 1 : -1;
        return;
    }

    if (dy != 0) {
        forward_one_cell();
        st->cur_y += (dy > 0) ? 1 : -1;
        return;
    }
}

// Optional: if telemetry can jump multiple cells, loop steps
static void go_to_cell(RobotState *st, int tx, int ty) {
    // Safety cap to prevent infinite loops
    for (int i = 0; i < 200; i++) {
        if (st->cur_x == tx && st->cur_y == ty) break;
        step_toward_target(st, tx, ty);
    }
}

static int ros_init_cmdvel(void) {
    g_alloc = rcl_get_default_allocator();

    rcl_ret_t rc = rclc_support_init(&g_support, 0, NULL, &g_alloc);
    if (rc != RCL_RET_OK) {
        fprintf(stderr, "[ROS] rclc_support_init failed\n");
        return 0;
    }

    rc = rclc_node_init_default(&g_node, "maze_minipupper_bridge", "", &g_support);
    if (rc != RCL_RET_OK) {
        fprintf(stderr, "[ROS] node init failed\n");
        return 0;
    }

    rc = rclc_publisher_init_default(
        &g_cmdvel_pub,
        &g_node,
        ROSIDL_GET_MSG_TYPE_SUPPORT(geometry_msgs, msg, Twist),
        "/cmd_vel"
    );
    if (rc != RCL_RET_OK) {
        fprintf(stderr, "[ROS] publisher init failed\n");
        return 0;
    }

    g_ros_ready = 1;
    fprintf(stdout, "[ROS] /cmd_vel publisher ready\n");
    return 1;
}

// ---------------- JSON parsing ----------------

static int extract_xy(const char *json_str, int *x, int *y) {
    cJSON *root = cJSON_Parse(json_str);
    if (!root) return 0;

    cJSON *player = cJSON_GetObjectItem(root, "player");
    cJSON *pos = player ? cJSON_GetObjectItem(player, "position") : NULL;
    cJSON *jx  = pos ? cJSON_GetObjectItem(pos, "x") : NULL;
    cJSON *jy  = pos ? cJSON_GetObjectItem(pos, "y") : NULL;

    if (!cJSON_IsNumber(jx) || !cJSON_IsNumber(jy)) {
        cJSON_Delete(root);
        return 0;
    }

    *x = jx->valueint;
    *y = jy->valueint;

    cJSON_Delete(root);
    return 1;
}

// ---------------- HTTP handler ----------------

static enum MHD_Result handle_request(void *cls,
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
        enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_METHOD_NOT_ALLOWED, resp);
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
        if (pd->length + *upload_data_size > MAX_POST_SIZE) {
            return MHD_NO;
        }

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

    // Upload complete → process JSON
    if (pd->length > 0) {
        printf("[TELEMETRY] %s\n%s\n\n", url, pd->buffer);
        fflush(stdout);

        int tx, ty;
        if (extract_xy(pd->buffer, &tx, &ty)) {
            if (!g_state.initialized) {
                g_state.cur_x = tx;
                g_state.cur_y = ty;
                g_state.initialized = 1;
                printf("[STATE] Initialized position = (%d,%d)\n", tx, ty);
            } else {
                printf("[MOVE] (%d,%d) -> (%d,%d)\n",
                       g_state.cur_x, g_state.cur_y, tx, ty);

                // choose ONE:
                // step_toward_target(&g_state, tx, ty);  // one-cell step per message
                go_to_cell(&g_state, tx, ty);             // multi-step until target

                printf("[STATE] Now at (%d,%d)\n", g_state.cur_x, g_state.cur_y);
            }
        } else {
            printf("[WARN] Could not parse player.position.x/y\n");
        }
    }

    const char *reply = "{\"status\":\"received\"}";
    struct MHD_Response *response = MHD_create_response_from_buffer(
        strlen(reply), (void*)reply, MHD_RESPMEM_PERSISTENT);

    MHD_add_response_header(response, "Content-Type", "application/json");

    enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
    MHD_destroy_response(response);

    free_post_data(pd);
    *con_cls = NULL;

    return ret;
}

int main(void)
{
    // ---- init ROS publisher for /cmd_vel ----
    if (!ros_init_cmdvel()) {
        fprintf(stderr, "ROS init failed (did you source humble + your ws?)\n");
        // continue anyway so server can run for debug, but robot won't move
    }

    // Load certificate and key
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

    printf("Minipupper HTTPS server running on https://0.0.0.0:%d/move\n", PORT);
    printf("Waiting for POST requests from maze simulator...\n");
    printf("Press Enter to exit...\n");

    getchar();

    MHD_stop_daemon(daemon);
    free(cert);
    free(key);

    // stop on exit (best effort)
    stop_robot();

    return 0;
}