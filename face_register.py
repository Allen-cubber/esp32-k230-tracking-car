try:
    from main1 import FaceDetApp, FaceRegistrationApp, PipeLine, np, gc
except ImportError:
    from main import FaceDetApp, FaceRegistrationApp, PipeLine, np, gc

import os
import time


REGISTER_NAME = "szl"
DATABASE_DIR = "/data/face_database/3889/"

FACE_DET_KMODEL = "/sdcard/kmodel/face_detection_320.kmodel"
FACE_REG_KMODEL = "/sdcard/kmodel/face_recognition.kmodel"
ANCHORS_PATH = "/sdcard/utils/prior_data_320.bin"

RGB888P_SIZE = [640, 360]
DISPLAY_SIZE = [640, 480]
DISPLAY_MODE = "lcd"
FACE_DET_INPUT_SIZE = [320, 320]
FACE_REG_INPUT_SIZE = [112, 112]
CONFIDENCE_THRESHOLD = 0.5
NMS_THRESHOLD = 0.2
STABLE_FRAMES = 12
MIN_FACE_SIZE = 70


def ensure_dir(path):
    current = ""
    for part in path.strip("/").split("/"):
        if not part:
            continue
        current += "/" + part
        try:
            os.mkdir(current)
        except Exception:
            pass


def draw_text(pl, text, y, color=(255, 255, 255, 255)):
    try:
        pl.osd_img.draw_string_advanced(20, y, 28, text, color=color)
    except Exception:
        print(text)


def draw_box(pl, box, color):
    x, y, w, h = map(lambda v: int(round(v, 0)), box[:4])
    x = x * DISPLAY_SIZE[0] // RGB888P_SIZE[0]
    y = y * DISPLAY_SIZE[1] // RGB888P_SIZE[1]
    w = w * DISPLAY_SIZE[0] // RGB888P_SIZE[0]
    h = h * DISPLAY_SIZE[1] // RGB888P_SIZE[1]
    pl.osd_img.draw_rectangle(x, y, w, h, color=color, thickness=4)


def save_feature(feature, name):
    ensure_dir(DATABASE_DIR)
    path = DATABASE_DIR + name + ".bin"
    with open(path, "wb") as f:
        f.write(feature.tobytes())
    return path


def main():
    anchors = np.fromfile(ANCHORS_PATH, dtype=np.float)
    anchors = anchors.reshape((4200, 4))

    face_det = FaceDetApp(
        FACE_DET_KMODEL,
        model_input_size=FACE_DET_INPUT_SIZE,
        anchors=anchors,
        confidence_threshold=CONFIDENCE_THRESHOLD,
        nms_threshold=NMS_THRESHOLD,
        rgb888p_size=RGB888P_SIZE,
        display_size=DISPLAY_SIZE,
        debug_mode=0,
    )
    face_reg = FaceRegistrationApp(
        FACE_REG_KMODEL,
        model_input_size=FACE_REG_INPUT_SIZE,
        rgb888p_size=RGB888P_SIZE,
        display_size=DISPLAY_SIZE,
        debug_mode=0,
    )
    face_det.config_preprocess()

    pl = PipeLine(rgb888p_size=RGB888P_SIZE,
                  display_size=DISPLAY_SIZE,
                  display_mode=DISPLAY_MODE)
    pl.create()

    stable_count = 0
    saved_path = ""

    try:
        while True:
            img = pl.get_frame()
            det_boxes, landms = face_det.run(img)

            pl.osd_img.clear()
            draw_text(pl, "Face register: {}".format(REGISTER_NAME), 20)

            face_count = len(det_boxes) if det_boxes else 0
            if face_count == 0:
                stable_count = 0
                draw_text(pl, "No face. Look at camera.", 60, color=(255, 255, 0, 0))
            elif face_count > 1:
                stable_count = 0
                for box in det_boxes:
                    draw_box(pl, box, color=(255, 255, 0, 0))
                draw_text(pl, "Keep only your face in view.", 60, color=(255, 255, 0, 0))
            else:
                box = det_boxes[0]
                _, _, w, h = box[:4]
                if w < MIN_FACE_SIZE or h < MIN_FACE_SIZE:
                    stable_count = 0
                    draw_box(pl, box, color=(255, 255, 0, 0))
                    draw_text(pl, "Move closer.", 60, color=(255, 255, 0, 0))
                else:
                    stable_count += 1
                    draw_box(pl, box, color=(255, 0, 255, 0))
                    draw_text(pl,
                              "Hold still: {}/{}".format(stable_count, STABLE_FRAMES),
                              60,
                              color=(255, 0, 255, 0))

                    if stable_count >= STABLE_FRAMES:
                        face_reg.config_preprocess(landms[0])
                        feature = face_reg.run(img)
                        saved_path = save_feature(feature, REGISTER_NAME)
                        print("saved:", saved_path)
                        draw_text(pl, "Saved: {}".format(saved_path), 100, color=(255, 0, 255, 0))
                        pl.show_image()
                        time.sleep(2)
                        break

            pl.show_image()
            gc.collect()
    finally:
        face_det.deinit()
        face_reg.deinit()
        if saved_path:
            print("Register done. Restart main1.py to load the new face.")


if __name__ == "__main__":
    main()
