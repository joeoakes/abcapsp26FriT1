import cv2
import math
import time
import platform
import threading
from pyapriltags import Detector
from flask import Flask, Response, jsonify


app = Flask(__name__)


def rotation_matrix_to_rpy_degrees(R):
    yaw = math.atan2(R[1, 0], R[0, 0])
    pitch = math.atan2(-R[2, 0], math.sqrt(R[2, 1] ** 2 + R[2, 2] ** 2))
    roll = math.atan2(R[2, 1], R[2, 2])
    return (
        math.degrees(roll),
        math.degrees(pitch),
        math.degrees(yaw),
    )


def open_camera(cam_index=0, width=640, height=480, fps=30):
    system_name = platform.system()

    if system_name == "Windows":
        backends = [cv2.CAP_DSHOW, cv2.CAP_MSMF, cv2.CAP_ANY]
    elif system_name == "Linux":
        backends = [cv2.CAP_V4L2, cv2.CAP_ANY]
    elif system_name == "Darwin":
        backends = [cv2.CAP_AVFOUNDATION, cv2.CAP_ANY]
    else:
        backends = [cv2.CAP_ANY]

    last_cap = None

    for backend in backends:
        cap = cv2.VideoCapture(cam_index, backend)
        if cap.isOpened():
            cap.set(cv2.CAP_PROP_FRAME_WIDTH, width)
            cap.set(cv2.CAP_PROP_FRAME_HEIGHT, height)
            cap.set(cv2.CAP_PROP_FPS, fps)
            cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)
            return cap
        last_cap = cap

    if last_cap is not None:
        last_cap.release()

    raise RuntimeError(
        f"Could not open camera at index {cam_index} on {system_name}. "
        f"Try cam_index=1 or cam_index=2."
    )


class ThreadedCamera:
    def __init__(self, cam_index=0, width=640, height=480, fps=30):
        self.cap = open_camera(cam_index, width, height, fps)
        self.lock = threading.Lock()
        self.frame = None
        self.running = True
        self.thread = threading.Thread(target=self._reader, daemon=True)
        self.thread.start()

    def _reader(self):
        while self.running:
            ok, frame = self.cap.read()
            if ok:
                with self.lock:
                    self.frame = frame

    def read(self):
        with self.lock:
            if self.frame is None:
                return False, None
            return True, self.frame.copy()

    def release(self):
        self.running = False
        self.thread.join(timeout=1.0)
        self.cap.release()


class AprilTagCameraServer:
    def __init__(self, cam_index=0, width=640, height=480, fps=30):
        self.frame_width = width
        self.frame_height = height

        self.camera = ThreadedCamera(
            cam_index=cam_index,
            width=width,
            height=height,
            fps=fps
        )

        self.detector = Detector(
            families="tag36h11",
            nthreads=2,
            quad_decimate=2.0,
            quad_sigma=0.0,
            refine_edges=1,
            decode_sharpening=0.25,
            debug=0
        )

        self.tag_size_m = 0.152
        self.fx = 600.0
        self.fy = 600.0
        self.cx = width / 2.0
        self.cy = height / 2.0

        self.prev_time = time.time()
        self.fps_display = 0.0

        self.latest_frame = None
        self.frame_lock = threading.Lock()
        self.running = True

        self.worker = threading.Thread(target=self._process_loop, daemon=True)
        self.worker.start()

    def _annotate_frame(self, frame):
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)

        tags = self.detector.detect(
            gray,
            estimate_tag_pose=True,
            camera_params=(self.fx, self.fy, self.cx, self.cy),
            tag_size=self.tag_size_m
        )

        best = max(tags, key=lambda t: t.decision_margin) if tags else None

        if best is not None:
            tag_id = best.tag_id
            tx, ty, tz = best.pose_t.flatten().tolist()
            roll, pitch, yaw = rotation_matrix_to_rpy_degrees(best.pose_R)

            msg1 = f"Type: tag36h11  ID: {tag_id}"
            msg2 = f"X: {tx:.3f} m  Y: {ty:.3f} m  Z: {tz:.3f} m"
            msg3 = f"Roll: {roll:.1f}  Pitch: {pitch:.1f}  Yaw: {yaw:.1f}"

            cv2.putText(
                frame, msg1, (20, 40),
                cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0), 2
            )
            cv2.putText(
                frame, msg2, (20, 72),
                cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0), 2
            )
            cv2.putText(
                frame, msg3, (20, 104),
                cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0), 2
            )

            corners = best.corners.astype(int)
            for i in range(4):
                p0 = tuple(corners[i])
                p1 = tuple(corners[(i + 1) % 4])
                cv2.line(frame, p0, p1, (0, 255, 0), 2)

            center = tuple(best.center.astype(int))
            cv2.circle(frame, center, 5, (0, 255, 0), -1)
        else:
            cv2.putText(
                frame, "No AprilTag detected", (20, 40),
                cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 200, 255), 2
            )

        current_time = time.time()
        dt = current_time - self.prev_time
        self.prev_time = current_time
        if dt > 0:
            self.fps_display = 0.9 * self.fps_display + 0.1 * (1.0 / dt)

        cv2.putText(
            frame, f"Display FPS: {self.fps_display:.1f}", (20, 140),
            cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 255), 2
        )

        return frame

    def _process_loop(self):
        while self.running:
            ok, frame = self.camera.read()
            if not ok:
                time.sleep(0.01)
                continue

            annotated = self._annotate_frame(frame)

            with self.frame_lock:
                self.latest_frame = annotated.copy()

            cv2.imshow("AprilTag Pose (Dashboard + Local Preview)", annotated)

            key = cv2.waitKey(1) & 0xFF
            if key == ord("q") or key == 27:
                self.running = False
                break

    def mjpeg_generator(self):
        while self.running:
            with self.frame_lock:
                if self.latest_frame is None:
                    frame = None
                else:
                    frame = self.latest_frame.copy()

            if frame is None:
                time.sleep(0.01)
                continue

            ok, buffer = cv2.imencode(".jpg", frame)
            if not ok:
                continue

            jpg_bytes = buffer.tobytes()

            yield (
                b"--frame\r\n"
                b"Content-Type: image/jpeg\r\n\r\n" +
                jpg_bytes +
                b"\r\n"
            )

            time.sleep(0.03)

    def shutdown(self):
        self.running = False
        self.worker.join(timeout=1.0)
        self.camera.release()
        cv2.destroyAllWindows()


camera_server = None


@app.route("/video_feed")
def video_feed():
    return Response(
        camera_server.mjpeg_generator(),
        mimetype="multipart/x-mixed-replace; boundary=frame"
    )


@app.route("/health")
def health():
    return jsonify({"status": "ok", "stream": "ready"})


@app.route("/")
def index():
    return """
    <html>
    <head><title>AprilTag Camera Feed</title></head>
    <body style="background:#111;color:white;font-family:sans-serif;">
        <h2>AprilTag Camera Stream</h2>
        <img src="/video_feed" width="640" />
    </body>
    </html>
    """


def main():
    global camera_server

    cam_index = 0
    frame_width = 640
    frame_height = 480
    target_fps = 30

    camera_server = AprilTagCameraServer(
        cam_index=cam_index,
        width=frame_width,
        height=frame_height,
        fps=target_fps
    )

    print("Local preview running.")
    print("Dashboard stream available at: https://127.0.0.1:5000/video_feed")
    print("Health check available at: https://127.0.0.1:5000/health")
    print("Press q or ESC in the OpenCV window to quit.")

    try:
        app.run(
            host="0.0.0.0",
            port=5000,
            threaded=True,
            debug=False,
            use_reloader=False,
            ssl_context=("../https/certs/server.crt", "../https/certs/server.key")
        )
    finally:
        if camera_server is not None:
            camera_server.shutdown()


if __name__ == "__main__":
    main()
