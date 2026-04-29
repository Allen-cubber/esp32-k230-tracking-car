from libs.PipeLine import PipeLine, ScopedTiming
from libs.AIBase import AIBase
from libs.AI2D import Ai2d
import os
from media.media import *
import nncase_runtime as nn
import ulab.numpy as np
import image
import aidemo
import gc
import math
import re

from machine import PWM, FPIOA
from ybUtils.YbUart import YbUart

uart = YbUart(baudrate=115200)


class PanTilt:
    def __init__(self):
        self.fpioa = FPIOA()

        self.fpioa.set_function(42, self.fpioa.PWM0)
        self.pwm_pan = PWM(0, 50, 7.5, enable=True)

        self.fpioa.set_function(43, self.fpioa.PWM1)
        self.pwm_tilt = PWM(1, 50, 7.5, enable=True)

        self.pan_angle = 90.0
        self.tilt_angle = 90.0

        self.kp_x = 0.025
        self.kp_y = 0.025
        self.kd_x = 0.01
        self.kd_y = 0.01
        self.deadzone = 45
        self.prev_error_x = 0
        self.prev_error_y = 0

    def set_angle(self, pwm, angle):
        angle = max(10.0, min(170.0, float(angle)))
        duty = 2.5 + (angle / 180.0) * 10.0
        pwm.duty(duty)

    def track(self, face_center_x, face_center_y, img_w, img_h):
        screen_center_x = img_w / 2
        screen_center_y = img_h / 2

        error_x = screen_center_x - face_center_x
        error_y = screen_center_y - face_center_y

        if abs(error_x) > self.deadzone:
            derivative_x = error_x - self.prev_error_x
            adjustment_x = (self.kp_x * error_x) + (self.kd_x * derivative_x)
            self.pan_angle += adjustment_x
            self.set_angle(self.pwm_pan, self.pan_angle)

        if abs(error_y) > self.deadzone:
            derivative_y = error_y - self.prev_error_y
            adjustment_y = (self.kp_y * error_y) + (self.kd_y * derivative_y)
            self.tilt_angle -= adjustment_y
            self.set_angle(self.pwm_tilt, self.tilt_angle)

        self.prev_error_x = error_x
        self.prev_error_y = error_y

    def deinit(self):
        self.pwm_pan.enable(False)
        self.pwm_tilt.enable(False)


def send_track_line(valid, pan_deg, tilt_deg, face_x, face_y, face_w, face_h):
    line = "TRACK,{},{:.2f},{:.2f},{:.1f},{:.1f},{:.1f},{:.1f}\n".format(
        1 if valid else 0,
        pan_deg,
        tilt_deg,
        face_x,
        face_y,
        face_w,
        face_h,
    )
    uart.send(line)


class FaceDetApp(AIBase):
    def __init__(self, kmodel_path, model_input_size, anchors, confidence_threshold=0.25,
                 nms_threshold=0.3, rgb888p_size=[640, 360], display_size=[640, 360], debug_mode=0):
        super().__init__(kmodel_path, model_input_size, rgb888p_size, debug_mode)
        self.kmodel_path = kmodel_path
        self.model_input_size = model_input_size
        self.confidence_threshold = confidence_threshold
        self.nms_threshold = nms_threshold
        self.anchors = anchors
        self.rgb888p_size = [ALIGN_UP(rgb888p_size[0], 16), rgb888p_size[1]]
        self.display_size = [ALIGN_UP(display_size[0], 16), display_size[1]]
        self.debug_mode = debug_mode
        self.ai2d = Ai2d(debug_mode)
        self.ai2d.set_ai2d_dtype(nn.ai2d_format.NCHW_FMT, nn.ai2d_format.NCHW_FMT, np.uint8, np.uint8)

    def config_preprocess(self, input_image_size=None):
        with ScopedTiming("set preprocess config", self.debug_mode > 0):
            ai2d_input_size = input_image_size if input_image_size else self.rgb888p_size
            self.ai2d.pad(self.get_pad_param(), 0, [104, 117, 123])
            self.ai2d.resize(nn.interp_method.tf_bilinear, nn.interp_mode.half_pixel)
            self.ai2d.build([1, 3, ai2d_input_size[1], ai2d_input_size[0]],
                            [1, 3, self.model_input_size[1], self.model_input_size[0]])

    def postprocess(self, results):
        with ScopedTiming("postprocess", self.debug_mode > 0):
            res = aidemo.face_det_post_process(
                self.confidence_threshold,
                self.nms_threshold,
                self.model_input_size[0],
                self.anchors,
                self.rgb888p_size,
                results,
            )
            if len(res) == 0:
                return res, res
            return res[0], res[1]

    def get_pad_param(self):
        dst_w = self.model_input_size[0]
        dst_h = self.model_input_size[1]
        ratio = min(dst_w / self.rgb888p_size[0], dst_h / self.rgb888p_size[1])
        new_w = int(ratio * self.rgb888p_size[0])
        new_h = int(ratio * self.rgb888p_size[1])
        dw = (dst_w - new_w) / 2
        dh = (dst_h - new_h) / 2
        top = int(round(0))
        bottom = int(round(dh * 2 + 0.1))
        left = int(round(0))
        right = int(round(dw * 2 - 0.1))
        return [0, 0, 0, 0, top, bottom, left, right]


class FaceRegistrationApp(AIBase):
    def __init__(self, kmodel_path, model_input_size, rgb888p_size=[640, 360],
                 display_size=[640, 360], debug_mode=0):
        super().__init__(kmodel_path, model_input_size, rgb888p_size, debug_mode)
        self.kmodel_path = kmodel_path
        self.model_input_size = model_input_size
        self.rgb888p_size = [ALIGN_UP(rgb888p_size[0], 16), rgb888p_size[1]]
        self.display_size = [ALIGN_UP(display_size[0], 16), display_size[1]]
        self.debug_mode = debug_mode
        self.umeyama_args_112 = [
            38.2946, 51.6963, 73.5318, 51.5014, 56.0252, 71.7366, 41.5493, 92.3655, 70.7299, 92.2041
        ]
        self.ai2d = Ai2d(debug_mode)
        self.ai2d.set_ai2d_dtype(nn.ai2d_format.NCHW_FMT, nn.ai2d_format.NCHW_FMT, np.uint8, np.uint8)

    def config_preprocess(self, landm, input_image_size=None):
        with ScopedTiming("set preprocess config", self.debug_mode > 0):
            ai2d_input_size = input_image_size if input_image_size else self.rgb888p_size
            affine_matrix = self.get_affine_matrix(landm)
            self.ai2d.affine(nn.interp_method.cv2_bilinear, 0, 0, 127, 1, affine_matrix)
            self.ai2d.build([1, 3, ai2d_input_size[1], ai2d_input_size[0]],
                            [1, 3, self.model_input_size[1], self.model_input_size[0]])

    def postprocess(self, results):
        with ScopedTiming("postprocess", self.debug_mode > 0):
            return results[0][0]

    def svd22(self, a):
        s = [0.0, 0.0]
        u = [0.0, 0.0, 0.0, 0.0]
        v = [0.0, 0.0, 0.0, 0.0]
        s[0] = (math.sqrt((a[0] - a[3]) ** 2 + (a[1] + a[2]) ** 2) +
                math.sqrt((a[0] + a[3]) ** 2 + (a[1] - a[2]) ** 2)) / 2
        s[1] = abs(s[0] - math.sqrt((a[0] - a[3]) ** 2 + (a[1] + a[2]) ** 2))
        v[2] = (math.sin((math.atan2(2 * (a[0] * a[1] + a[2] * a[3]),
                a[0] ** 2 - a[1] ** 2 + a[2] ** 2 - a[3] ** 2)) / 2) if s[0] > s[1] else 0)
        v[0] = math.sqrt(1 - v[2] ** 2)
        v[1] = -v[2]
        v[3] = v[0]
        u[0] = -(a[0] * v[0] + a[1] * v[2]) / s[0] if s[0] != 0 else 1
        u[2] = -(a[2] * v[0] + a[3] * v[2]) / s[0] if s[0] != 0 else 0
        u[1] = (a[0] * v[1] + a[1] * v[3]) / s[1] if s[1] != 0 else -u[2]
        u[3] = (a[2] * v[1] + a[3] * v[3]) / s[1] if s[1] != 0 else u[0]
        v[0] = -v[0]
        v[2] = -v[2]
        return u, s, v

    def image_umeyama_112(self, src):
        src_num = 5
        src_dim = 2
        src_mean = [0.0, 0.0]
        dst_mean = [0.0, 0.0]
        for i in range(0, src_num * 2, 2):
            src_mean[0] += src[i]
            src_mean[1] += src[i + 1]
            dst_mean[0] += self.umeyama_args_112[i]
            dst_mean[1] += self.umeyama_args_112[i + 1]
        src_mean[0] /= src_num
        src_mean[1] /= src_num
        dst_mean[0] /= src_num
        dst_mean[1] /= src_num
        src_demean = [[0.0, 0.0] for _ in range(src_num)]
        dst_demean = [[0.0, 0.0] for _ in range(src_num)]
        for i in range(src_num):
            src_demean[i][0] = src[2 * i] - src_mean[0]
            src_demean[i][1] = src[2 * i + 1] - src_mean[1]
            dst_demean[i][0] = self.umeyama_args_112[2 * i] - dst_mean[0]
            dst_demean[i][1] = self.umeyama_args_112[2 * i + 1] - dst_mean[1]
        a = [[0.0, 0.0], [0.0, 0.0]]
        for i in range(src_dim):
            for k in range(src_dim):
                for j in range(src_num):
                    a[i][k] += dst_demean[j][i] * src_demean[j][k]
                a[i][k] /= src_num
        t = [[1, 0, 0], [0, 1, 0], [0, 0, 1]]
        u, s, v = self.svd22([a[0][0], a[0][1], a[1][0], a[1][1]])
        t[0][0] = u[0] * v[0] + u[1] * v[2]
        t[0][1] = u[0] * v[1] + u[1] * v[3]
        t[1][0] = u[2] * v[0] + u[3] * v[2]
        t[1][1] = u[2] * v[1] + u[3] * v[3]
        src_demean_mean = [0.0, 0.0]
        src_demean_var = [0.0, 0.0]
        for i in range(src_num):
            src_demean_mean[0] += src_demean[i][0]
            src_demean_mean[1] += src_demean[i][1]
        src_demean_mean[0] /= src_num
        src_demean_mean[1] /= src_num
        for i in range(src_num):
            src_demean_var[0] += (src_demean_mean[0] - src_demean[i][0]) ** 2
            src_demean_var[1] += (src_demean_mean[1] - src_demean[i][1]) ** 2
        src_demean_var[0] /= src_num
        src_demean_var[1] /= src_num
        scale = 1.0 / (src_demean_var[0] + src_demean_var[1]) * (s[0] + s[1])
        t[0][2] = dst_mean[0] - scale * (t[0][0] * src_mean[0] + t[0][1] * src_mean[1])
        t[1][2] = dst_mean[1] - scale * (t[1][0] * src_mean[0] + t[1][1] * src_mean[1])
        t[0][0] *= scale
        t[0][1] *= scale
        t[1][0] *= scale
        t[1][1] *= scale
        return t

    def get_affine_matrix(self, sparse_points):
        matrix_dst = self.image_umeyama_112(sparse_points)
        return [
            matrix_dst[0][0], matrix_dst[0][1], matrix_dst[0][2],
            matrix_dst[1][0], matrix_dst[1][1], matrix_dst[1][2]
        ]


class FaceRecognition:
    def __init__(self, face_det_kmodel, face_reg_kmodel, det_input_size, reg_input_size,
                 database_dir, anchors, confidence_threshold=0.25, nms_threshold=0.3,
                 face_recognition_threshold=0.75, rgb888p_size=[1280, 720],
                 display_size=[640, 360], debug_mode=0):
        self.face_det_kmodel = face_det_kmodel
        self.face_reg_kmodel = face_reg_kmodel
        self.det_input_size = det_input_size
        self.reg_input_size = reg_input_size
        self.database_dir = database_dir
        self.anchors = anchors
        self.confidence_threshold = confidence_threshold
        self.nms_threshold = nms_threshold
        self.face_recognition_threshold = face_recognition_threshold
        self.rgb888p_size = [ALIGN_UP(rgb888p_size[0], 16), rgb888p_size[1]]
        self.display_size = [ALIGN_UP(display_size[0], 16), display_size[1]]
        self.debug_mode = debug_mode
        self.max_register_face = 100
        self.valid_register_face = 0
        self.db_name = []
        self.db_data = []
        self.face_det = FaceDetApp(
            self.face_det_kmodel,
            model_input_size=self.det_input_size,
            anchors=self.anchors,
            confidence_threshold=self.confidence_threshold,
            nms_threshold=self.nms_threshold,
            rgb888p_size=self.rgb888p_size,
            display_size=self.display_size,
            debug_mode=0,
        )
        self.face_reg = FaceRegistrationApp(
            self.face_reg_kmodel,
            model_input_size=self.reg_input_size,
            rgb888p_size=self.rgb888p_size,
            display_size=self.display_size,
        )
        self.face_det.config_preprocess()
        self.database_init()
        self.pan_tilt = PanTilt()

    def run(self, input_np):
        det_boxes, landms = self.face_det.run(input_np)
        recg_res = []
        for landm in landms:
            self.face_reg.config_preprocess(landm)
            feature = self.face_reg.run(input_np)
            recg_res.append(self.database_search(feature))
        return det_boxes, recg_res

    def database_init(self):
        try:
            db_file_list = os.listdir(self.database_dir)
            for db_file in db_file_list:
                if not db_file.endswith('.bin'):
                    continue
                if self.valid_register_face >= self.max_register_face:
                    break
                full_db_file = self.database_dir + db_file
                with open(full_db_file, 'rb') as f:
                    data = f.read()
                feature = np.frombuffer(data, dtype=np.float)
                self.db_data.append(feature)
                self.db_name.append(db_file.split('.')[0])
                self.valid_register_face += 1
        except Exception as e:
            print(e)
            print("No face database detected")

    def database_search(self, feature):
        v_id = -1
        v_score_max = 0.0
        feature /= np.linalg.norm(feature)
        for i in range(self.valid_register_face):
            db_feature = self.db_data[i]
            db_feature /= np.linalg.norm(db_feature)
            v_score = np.dot(feature, db_feature) / 2 + 0.5
            if v_score > v_score_max:
                v_score_max = v_score
                v_id = i
        if v_id == -1 or v_score_max < self.face_recognition_threshold:
            return "unknown"
        return "name: {}, score: {}".format(self.db_name[v_id], v_score_max)

    def draw_result(self, pl, dets, recg_results):
        pl.osd_img.clear()
        target_face = None

        if dets:
            for i, det in enumerate(dets):
                x1, y1, w, h = map(lambda x: int(round(x, 0)), det[:4])
                x1 = x1 * self.display_size[0] // self.rgb888p_size[0]
                y1 = y1 * self.display_size[1] // self.rgb888p_size[1]
                w = w * self.display_size[0] // self.rgb888p_size[0]
                h = h * self.display_size[1] // self.rgb888p_size[1]
                recg_text = recg_results[i]

                if recg_text == "unknown":
                    pl.osd_img.draw_rectangle(x1, y1, w, h, color=(255, 0, 0, 255), thickness=4)
                else:
                    pl.osd_img.draw_rectangle(x1, y1, w, h, color=(255, 0, 255, 0), thickness=4)
                    if target_face is None:
                        target_face = (x1 + w // 2, y1 + h // 2, w, h)

                pl.osd_img.draw_string_advanced(x1, y1, 32, recg_text, color=(255, 255, 0, 0))

        if target_face:
            self.pan_tilt.track(target_face[0], target_face[1], self.display_size[0], self.display_size[1])
            send_track_line(
                True,
                self.pan_tilt.pan_angle,
                self.pan_tilt.tilt_angle,
                float(target_face[0]),
                float(target_face[1]),
                float(target_face[2]),
                float(target_face[3]),
            )
        else:
            send_track_line(False, self.pan_tilt.pan_angle, self.pan_tilt.tilt_angle, 0.0, 0.0, 0.0, 0.0)

    def deinit(self):
        self.face_det.deinit()
        self.face_reg.deinit()
        self.pan_tilt.deinit()


fr = None


def exce_demo(pl):
    global fr
    face_det_kmodel_path = "/sdcard/kmodel/face_detection_320.kmodel"
    face_reg_kmodel_path = "/sdcard/kmodel/face_recognition.kmodel"
    anchors_path = "/sdcard/utils/prior_data_320.bin"
    database_dir = "/data/face_database/3889/"
    face_det_input_size = [320, 320]
    face_reg_input_size = [112, 112]
    confidence_threshold = 0.5
    nms_threshold = 0.2
    anchor_len = 4200
    det_dim = 4

    anchors = np.fromfile(anchors_path, dtype=np.float)
    anchors = anchors.reshape((anchor_len, det_dim))

    fr = FaceRecognition(
        face_det_kmodel_path,
        face_reg_kmodel_path,
        det_input_size=face_det_input_size,
        reg_input_size=face_reg_input_size,
        database_dir=database_dir,
        anchors=anchors,
        confidence_threshold=confidence_threshold,
        nms_threshold=nms_threshold,
        face_recognition_threshold=0.65,
        rgb888p_size=pl.rgb888p_size,
        display_size=pl.display_size,
    )

    try:
        while True:
            with ScopedTiming("total", 1):
                img = pl.get_frame()
                det_boxes, recg_res = fr.run(img)
                fr.draw_result(pl, det_boxes, recg_res)
                pl.show_image()
                gc.collect()
    except Exception as e:
        print("face recognition exit", e)
    finally:
        exit_demo()


def exit_demo():
    global fr
    if fr:
        fr.deinit()


if __name__ == "__main__":
    rgb888p_size = [640, 360]
    display_size = [640, 480]
    display_mode = "lcd"
    pl = PipeLine(rgb888p_size=rgb888p_size, display_size=display_size, display_mode=display_mode)
    pl.create()
    exce_demo(pl)
