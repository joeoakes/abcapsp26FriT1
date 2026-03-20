#include <microhttpd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <cjson/cJSON.h>
 
#define PORT 8447
#define CERT_FILE "certs/server.crt"
#define KEY_FILE "certs/server.key"
 
// Track last position
static int cur_x = 0;
static int cur_y = 0;
static int initialized = 0;
 
// ---------------- JSON ----------------
 
static int extract_xy(const char *json, int *x, int *y) {
    cJSON *root = cJSON_Parse(json);
    if (!root) return 0;
 
    cJSON *pos = cJSON_GetObjectItem(
        cJSON_GetObjectItem(root, "player"), "position");
 
    if (!pos) { cJSON_Delete(root); return 0; }
 
    *x = cJSON_GetObjectItem(pos, "x")->valueint;
    *y = cJSON_GetObjectItem(pos, "y")->valueint;
 
    cJSON_Delete(root);
    return 1;
}
 
// ---------------- ROS2 COMMANDS ----------------
 
void stop_robot() {
    system("ros2 topic pub /cmd_vel geometry_msgs/msg/Twist "
           "'{linear: {x: 0.0}, angular: {z: 0.0}}' -r 5 & sleep 1; pkill -f \"ros2 topic pub /cmd_vel\"");
}
 
void move_forward() {
    system("ros2 topic pub /cmd_vel geometry_msgs/msg/Twist "
           "'{linear: {x: 0.1}, angular: {z: 0.0}}' -r 5 & sleep 2; pkill -f \"ros2 topic pub /cmd_vel\"");
    stop_robot();
}
 
void turn_left() {
    system("ros2 topic pub /cmd_vel geometry_msgs/msg/Twist "
           "'{linear: {x: 0.0}, angular: {z: 0.8}}' -r 5 & sleep 1.2; pkill -f \"ros2 topic pub /cmd_vel\"");
    stop_robot();
}
 
void turn_right() {
    system("ros2 topic pub /cmd_vel geometry_msgs/msg/Twist "
           "'{linear: {x: 0.0}, angular: {z: -0.8}}' -r 5 & sleep 1.2; pkill -f \"ros2 topic pub /cmd_vel\"");
    stop_robot();
}
 
void turn_around() {
    system("ros2 topic pub /cmd_vel geometry_msgs/msg/Twist "
           "'{linear: {x: 0.0}, angular: {z: 0.8}}' -r 5 & sleep 2.4; pkill -f \"ros2 topic pub /cmd_vel\"");
    stop_robot();
}
 
// ---------------- MOVE LOGIC ----------------
 
void handle_movement(int new_x, int new_y) {
    if (!initialized) {
        cur_x = new_x;
        cur_y = new_y;
        initialized = 1;
        printf("[INIT] (%d,%d)\n", cur_x, cur_y);
        return;
    }
 
    int dx = new_x - cur_x;
    int dy = new_y - cur_y;
 
    printf("[MOVE] (%d,%d) -> (%d,%d)\n", cur_x, cur_y, new_x, new_y);
 
    if (dx == 1 && dy == 0) {
        printf("→ RIGHT\n");
        turn_right();
        move_forward();
    }
    else if (dx == -1 && dy == 0) {
        printf("← LEFT\n");
        turn_left();
        move_forward();
    }
    else if (dx == 0 && dy == -1) {
        printf("↑ UP\n");
        move_forward();
    }
    else if (dx == 0 && dy == 1) {
        printf("↓ DOWN\n");
        turn_around();
        move_forward();
    }
    else {
        printf("[WARN] Invalid step\n");
    }
 
    cur_x = new_x;
    cur_y = new_y;
}
 
// ---------------- HTTP ----------------
 
typedef struct {
    char *data;
    size_t size;
} Buffer;
 
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
    if (strcmp(method, "POST") != 0 || strcmp(url, "/move") != 0)
        return MHD_NO;
 
    if (*con_cls == NULL) {
        Buffer *buf = calloc(1, sizeof(Buffer));
        *con_cls = buf;
        return MHD_YES;
    }
 
    Buffer *buf = *con_cls;
 
    if (*upload_data_size > 0) {
        buf->data = realloc(buf->data, buf->size + *upload_data_size + 1);
        memcpy(buf->data + buf->size, upload_data, *upload_data_size);
        buf->size += *upload_data_size;
        buf->data[buf->size] = '\0';
 
        *upload_data_size = 0;
        return MHD_YES;
    }
 
    if (buf->data) {
        int x, y;
        if (extract_xy(buf->data, &x, &y)) {
            handle_movement(x, y);
        }
    }
 
    const char *reply = "{\"status\":\"ok\"}";
    struct MHD_Response *response =
        MHD_create_response_from_buffer(strlen(reply), (void*)reply, MHD_RESPMEM_PERSISTENT);
 
    MHD_queue_response(connection, MHD_HTTP_OK, response);
    MHD_destroy_response(response);
 
    free(buf->data);
    free(buf);
    *con_cls = NULL;
 
    return MHD_YES;
}
 
// ---------------- MAIN ----------------
 
int main() {
    struct MHD_Daemon *daemon = MHD_start_daemon(
        MHD_USE_THREAD_PER_CONNECTION | MHD_USE_TLS,
        PORT,
        NULL, NULL,
        &handle_request, NULL,
        MHD_OPTION_HTTPS_MEM_CERT, CERT_FILE,
        MHD_OPTION_HTTPS_MEM_KEY, KEY_FILE,
        MHD_OPTION_END);
 
    printf("Server running on https://0.0.0.0:%d/move\n", PORT);
    getchar();
 
    MHD_stop_daemon(daemon);
    return 0;
}