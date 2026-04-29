# 导入所需库 / Import required libraries
from libs.PipeLine import PipeLine, ScopedTiming
from libs.AIBase import AIBase
from libs.AI2D import Ai2d
import os
import ujson
from media.media import *
from time import *
import nncase_runtime as nn
import ulab.numpy as np
import time
import image
import aidemo
import random
import gc
import sys
import math, re

from machine import PWM, FPIOA

# 全局变量定义 / Global variable definition
fr = None
from libs.YbProtocol import YbProtocol
from ybUtils.YbUart import YbUart

uart = YbUart(baudrate=115200)
pto = YbProtocol()


# 只新增这个函数：给 ESP32 发纯文本 TRACK
def send_track_line(valid, pan_deg, tilt_deg, face_x, face_y, face_w, face_h):
    line = "TRACK,{},{:.2f},{:.2f},{:.1f},{:.1f},{:.1f},{:.1f}\n".format(
        1 if valid else 0,
        pan_deg,
        tilt_deg,
        face_x,
        face_y,
        face_w,
        face_h
    )
    uart.send(line)
    print(line)


uart_rx_line = ""


def _uart_bytes_to_text(data):
    if data is None:
        return ""
    if isinstance(data, bytes):
        try:
            return data.decode("utf-8")
        except Exception:
            return data.decode("utf-8", "ignore")
    return str(data)


def _uart_try_read_text():
    try:
        if hasattr(uart, "any") and uart.any() <= 0:
            return ""
    except Exception:
        pass

    for method_name in ("read", "recv", "readline"):
        try:
            if hasattr(uart, method_name):
                data = getattr(uart, method_name)()
                return _uart_bytes_to_text(data)
        except Exception:
            pass
    return ""


def poll_esp32_gimbal_command(pan_tilt):
    global uart_rx_line

    text = _uart_try_read_text()
    if not text:
        return

    for ch in text:
        if ch == "\r":
            continue
        if ch == "\n":
            line = uart_rx_line.strip()
            uart_rx_line = ""
            if line.startswith("GIMBAL,"):
                parts = line.split(",")
                action = parts[1].strip() if len(parts) > 1 else "stop"
                try:
                    pan_delta = int(parts[2]) if len(parts) > 2 and parts[2] else 0
                    tilt_delta = int(parts[3]) if len(parts) > 3 and parts[3] else 0
                    duration_ms = int(parts[4]) if len(parts) > 4 and parts[4] else 0
                except Exception:
                    print("Bad ESP32 gimbal command:", line)
                    continue
                pan_tilt.handle_command(action, pan_delta, tilt_delta, duration_ms)
                print("ESP32 gimbal command:", line)
            continue

        if len(uart_rx_line) < 128:
            uart_rx_line += ch
        else:
            uart_rx_line = ""


# ==========================================
# 新增：二维云台追踪控制类
# ==========================================
class PanTilt:
    def __init__(self):
        self.fpioa = FPIOA()

        # 水平旋转舵机 (Pan) - 对应 GPIO 42
        self.fpioa.set_function(42, self.fpioa.PWM0)
        self.pwm_pan = PWM(0, 50, 7.5, enable=True)

        # 俯仰舵机 (Tilt) - 对应 GPIO 43
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

        self.local_step_frames = 12
        self.global_step_frames = 18
        self.search_tilt_raise = 8.0
        self.manual_lock = False
        self.manual_sequence = None
        self.manual_sequence_index = 0
        self.manual_sequence_frame = 0
        self.manual_sequence_hold_frames = 4
        self.manual_sequence_auto_unlock = False

    def clamp_angle(self, angle):
        return max(10.0, min(170.0, float(angle)))

    def set_angle(self, pwm, angle):
        """将 0-180 的角度转换为对应的 PWM 占空比并输出"""
        angle = self.clamp_angle(angle)
        duty = 2.5 + (angle / 180.0) * 10.0
        pwm.duty(duty)
        return angle

    def move_to(self, pan_angle=None, tilt_angle=None):
        if pan_angle is not None:
            self.pan_angle = self.set_angle(self.pwm_pan, pan_angle)
        if tilt_angle is not None:
            self.tilt_angle = self.set_angle(self.pwm_tilt, tilt_angle)

    def reset_pd(self):
        self.prev_error_x = 0
        self.prev_error_y = 0

    def is_manual(self):
        return self.manual_lock or self.manual_sequence is not None

    def start_sequence(self, points, duration_ms, auto_unlock=True):
        self.manual_lock = True
        self.manual_sequence = points
        self.manual_sequence_index = 0
        self.manual_sequence_frame = 0
        self.manual_sequence_auto_unlock = auto_unlock
        if duration_ms <= 0:
            duration_ms = 1000
        frames = max(2, int(duration_ms / 60))
        self.manual_sequence_hold_frames = max(1, int(frames / max(1, len(points))))
        self.reset_pd()

    def finish_sequence(self):
        self.manual_sequence = None
        if self.manual_sequence_auto_unlock:
            self.manual_lock = False
            self.reset_pd()
        self.manual_sequence_auto_unlock = False

    def update_manual_motion(self):
        if not self.manual_sequence:
            return

        if self.manual_sequence_index >= len(self.manual_sequence):
            self.finish_sequence()
            return

        pan_angle, tilt_angle = self.manual_sequence[self.manual_sequence_index]
        self.move_to(pan_angle, tilt_angle)
        self.manual_sequence_frame += 1

        if self.manual_sequence_frame >= self.manual_sequence_hold_frames:
            self.manual_sequence_frame = 0
            self.manual_sequence_index += 1
            if self.manual_sequence_index >= len(self.manual_sequence):
                self.finish_sequence()

    def handle_command(self, action, pan_delta=0, tilt_delta=0, duration_ms=0):
        action = action.lower()
        pan_delta = float(pan_delta)
        tilt_delta = float(tilt_delta)

        if action in ("unlock", "track", "resume"):
            self.manual_lock = False
            self.manual_sequence = None
            self.manual_sequence_auto_unlock = False
            self.reset_pd()
            return

        self.manual_lock = True
        self.manual_sequence = None
        self.manual_sequence_auto_unlock = False
        self.reset_pd()

        if action in ("lock", "stop"):
            return
        if action == "center":
            self.move_to(90.0, 90.0)
            return
        if action in ("left", "right", "up", "down"):
            self.move_to(self.pan_angle + pan_delta, self.tilt_angle + tilt_delta)
            return
        if action == "nod":
            amount = abs(tilt_delta) if tilt_delta != 0 else 15.0
            base_pan = self.pan_angle
            base_tilt = self.tilt_angle
            self.start_sequence([
                (base_pan, base_tilt - amount),
                (base_pan, base_tilt + amount),
                (base_pan, base_tilt - amount),
                (base_pan, base_tilt),
            ], duration_ms, auto_unlock=True)
            return
        if action == "shake":
            amount = abs(pan_delta) if pan_delta != 0 else 15.0
            base_pan = self.pan_angle
            base_tilt = self.tilt_angle
            self.start_sequence([
                (base_pan - amount, base_tilt),
                (base_pan + amount, base_tilt),
                (base_pan - amount, base_tilt),
                (base_pan, base_tilt),
            ], duration_ms, auto_unlock=True)
            return

    def track(self, face_center_x, face_center_y, img_w, img_h):
        """带有 PD 控制的追踪逻辑"""
        screen_center_x = img_w / 2
        screen_center_y = img_h / 2

        error_x = screen_center_x - face_center_x
        error_y = screen_center_y - face_center_y

        if abs(error_x) > self.deadzone:
            derivative_x = error_x - self.prev_error_x
            adjustment_x = (self.kp_x * error_x) + (self.kd_x * derivative_x)

            self.pan_angle += adjustment_x
            self.pan_angle = self.set_angle(self.pwm_pan, self.pan_angle)

        if abs(error_y) > self.deadzone:
            derivative_y = error_y - self.prev_error_y
            adjustment_y = (self.kp_y * error_y) + (self.kd_y * derivative_y)

            self.tilt_angle -= adjustment_y
            self.tilt_angle = self.set_angle(self.pwm_tilt, self.tilt_angle)

        self.prev_error_x = error_x
        self.prev_error_y = error_y

    def local_search(self, base_pan, base_tilt, lost_frames):
        pan_offsets = [0, 6, -6, 12, -12, 20, -20]
        tilt_offsets = [0, 0, 0, -4, -4, 4, 4]
        step = int(lost_frames // self.local_step_frames)
        idx = step % len(pan_offsets)
        self.move_to(base_pan + pan_offsets[idx], base_tilt + self.search_tilt_raise + tilt_offsets[idx])

    def global_search(self, lost_frames):
        pan_points = [40, 60, 80, 100, 120, 140, 120, 100, 80, 60]
        tilt_points = [88, 103, 118]
        step = int(lost_frames // self.global_step_frames)
        pan_idx = step % len(pan_points)
        tilt_idx = (step // len(pan_points)) % len(tilt_points)
        self.move_to(pan_points[pan_idx], tilt_points[tilt_idx])

    def deinit(self):
        """释放 PWM 资源"""
        self.pwm_pan.enable(False)
        self.pwm_tilt.enable(False)


class FaceDetApp(AIBase):
    """
    人脸检测应用类 / Face detection application class
    继承自AIBase基类 / Inherits from AIBase class
    """
    def __init__(self, kmodel_path, model_input_size, anchors, confidence_threshold=0.25,
                 nms_threshold=0.3, rgb888p_size=[640,360], display_size=[640,360], debug_mode=0):
        super().__init__(kmodel_path, model_input_size, rgb888p_size, debug_mode)

        self.kmodel_path = kmodel_path
        self.model_input_size = model_input_size
        self.confidence_threshold = confidence_threshold
        self.nms_threshold = nms_threshold
        self.anchors = anchors

        self.rgb888p_size = [ALIGN_UP(rgb888p_size[0],16), rgb888p_size[1]]
        self.display_size = [ALIGN_UP(display_size[0],16), display_size[1]]

        self.debug_mode = debug_mode

        self.ai2d = Ai2d(debug_mode)
        self.ai2d.set_ai2d_dtype(nn.ai2d_format.NCHW_FMT,
                                nn.ai2d_format.NCHW_FMT,
                                np.uint8, np.uint8)

    def config_preprocess(self, input_image_size=None):
        with ScopedTiming("set preprocess config", self.debug_mode > 0):
            ai2d_input_size = input_image_size if input_image_size else self.rgb888p_size
            self.ai2d.pad(self.get_pad_param(), 0, [104,117,123])
            self.ai2d.resize(nn.interp_method.tf_bilinear, nn.interp_mode.half_pixel)
            self.ai2d.build([1,3,ai2d_input_size[1],ai2d_input_size[0]],
                            [1,3,self.model_input_size[1],self.model_input_size[0]])

    def postprocess(self, results):
        with ScopedTiming("postprocess", self.debug_mode > 0):
            res = aidemo.face_det_post_process(self.confidence_threshold,
                                             self.nms_threshold,
                                             self.model_input_size[0],
                                             self.anchors,
                                             self.rgb888p_size,
                                             results)
            if len(res) == 0:
                return res, res
            else:
                return res[0], res[1]

    def get_pad_param(self):
        dst_w = self.model_input_size[0]
        dst_h = self.model_input_size[1]

        ratio_w = dst_w / self.rgb888p_size[0]
        ratio_h = dst_h / self.rgb888p_size[1]
        ratio = min(ratio_w, ratio_h)

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
    """
    人脸注册应用类 / Face registration application class
    用于人脸特征提取和注册 / For face feature extraction and registration
    """
    def __init__(self, kmodel_path, model_input_size, rgb888p_size=[640,360],
                 display_size=[640,360], debug_mode=0):
        super().__init__(kmodel_path, model_input_size, rgb888p_size, debug_mode)

        self.kmodel_path = kmodel_path
        self.model_input_size = model_input_size
        self.rgb888p_size = [ALIGN_UP(rgb888p_size[0],16), rgb888p_size[1]]
        self.display_size = [ALIGN_UP(display_size[0],16), display_size[1]]
        self.debug_mode = debug_mode

        self.umeyama_args_112 = [
            38.2946, 51.6963,
            73.5318, 51.5014,
            56.0252, 71.7366,
            41.5493, 92.3655,
            70.7299, 92.2041
        ]

        self.ai2d = Ai2d(debug_mode)
        self.ai2d.set_ai2d_dtype(nn.ai2d_format.NCHW_FMT,
                                nn.ai2d_format.NCHW_FMT,
                                np.uint8, np.uint8)

    def config_preprocess(self, landm, input_image_size=None):
        with ScopedTiming("set preprocess config", self.debug_mode > 0):
            ai2d_input_size = input_image_size if input_image_size else self.rgb888p_size
            affine_matrix = self.get_affine_matrix(landm)
            self.ai2d.affine(nn.interp_method.cv2_bilinear, 0, 0, 127, 1, affine_matrix)
            self.ai2d.build([1,3,ai2d_input_size[1],ai2d_input_size[0]],
                            [1,3,self.model_input_size[1],self.model_input_size[0]])

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
                a[0] ** 2 - a[1] ** 2 + a[2] ** 2 - a[3] ** 2)) / 2)
                if s[0] > s[1] else 0)
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
        SRC_NUM = 5
        SRC_DIM = 2

        src_mean = [0.0, 0.0]
        dst_mean = [0.0, 0.0]
        for i in range(0, SRC_NUM * 2, 2):
            src_mean[0] += src[i]
            src_mean[1] += src[i + 1]
            dst_mean[0] += self.umeyama_args_112[i]
            dst_mean[1] += self.umeyama_args_112[i + 1]

        src_mean[0] /= SRC_NUM
        src_mean[1] /= SRC_NUM
        dst_mean[0] /= SRC_NUM
        dst_mean[1] /= SRC_NUM

        src_demean = [[0.0, 0.0] for _ in range(SRC_NUM)]
        dst_demean = [[0.0, 0.0] for _ in range(SRC_NUM)]
        for i in range(SRC_NUM):
            src_demean[i][0] = src[2 * i] - src_mean[0]
            src_demean[i][1] = src[2 * i + 1] - src_mean[1]
            dst_demean[i][0] = self.umeyama_args_112[2 * i] - dst_mean[0]
            dst_demean[i][1] = self.umeyama_args_112[2 * i + 1] - dst_mean[1]

        A = [[0.0, 0.0], [0.0, 0.0]]
        for i in range(SRC_DIM):
            for k in range(SRC_DIM):
                for j in range(SRC_NUM):
                    A[i][k] += dst_demean[j][i] * src_demean[j][k]
                A[i][k] /= SRC_NUM

        T = [[1, 0, 0], [0, 1, 0], [0, 0, 1]]
        U, S, V = self.svd22([A[0][0], A[0][1], A[1][0], A[1][1]])
        T[0][0] = U[0] * V[0] + U[1] * V[2]
        T[0][1] = U[0] * V[1] + U[1] * V[3]
        T[1][0] = U[2] * V[0] + U[3] * V[2]
        T[1][1] = U[2] * V[1] + U[3] * V[3]

        scale = 1.0
        src_demean_mean = [0.0, 0.0]
        src_demean_var = [0.0, 0.0]
        for i in range(SRC_NUM):
            src_demean_mean[0] += src_demean[i][0]
            src_demean_mean[1] += src_demean[i][1]

        src_demean_mean[0] /= SRC_NUM
        src_demean_mean[1] /= SRC_NUM

        for i in range(SRC_NUM):
            src_demean_var[0] += (src_demean_mean[0] - src_demean[i][0]) ** 2
            src_demean_var[1] += (src_demean_mean[1] - src_demean[i][1]) ** 2

        src_demean_var[0] /= SRC_NUM
        src_demean_var[1] /= SRC_NUM
        scale = 1.0 / (src_demean_var[0] + src_demean_var[1]) * (S[0] + S[1])

        T[0][2] = dst_mean[0] - scale * (T[0][0] * src_mean[0] + T[0][1] * src_mean[1])
        T[1][2] = dst_mean[1] - scale * (T[1][0] * src_mean[0] + T[1][1] * src_mean[1])

        T[0][0] *= scale
        T[0][1] *= scale
        T[1][0] *= scale
        T[1][1] *= scale

        return T

    def get_affine_matrix(self, sparse_points):
        with ScopedTiming("get_affine_matrix", self.debug_mode > 1):
            matrix_dst = self.image_umeyama_112(sparse_points)
            matrix_dst = [matrix_dst[0][0], matrix_dst[0][1], matrix_dst[0][2],
                         matrix_dst[1][0], matrix_dst[1][1], matrix_dst[1][2]]
            return matrix_dst


class FaceRecognition:
    """
    人脸识别类 / Face recognition class
    集成了检测和识别功能 / Integrates detection and recognition functions
    """
    def __init__(self, face_det_kmodel, face_reg_kmodel, det_input_size, reg_input_size,
                 database_dir, anchors, confidence_threshold=0.25, nms_threshold=0.3,
                 face_recognition_threshold=0.75, rgb888p_size=[1280,720],
                 display_size=[640,360], debug_mode=0):

        self.face_det_kmodel = face_det_kmodel
        self.face_reg_kmodel = face_reg_kmodel
        self.det_input_size = det_input_size
        self.reg_input_size = reg_input_size
        self.database_dir = database_dir
        self.anchors = anchors
        self.confidence_threshold = confidence_threshold
        self.nms_threshold = nms_threshold
        self.face_recognition_threshold = face_recognition_threshold
        self.rgb888p_size = [ALIGN_UP(rgb888p_size[0],16), rgb888p_size[1]]
        self.display_size = [ALIGN_UP(display_size[0],16), display_size[1]]
        self.debug_mode = debug_mode

        self.max_register_face = 100
        self.feature_num = 128
        self.valid_register_face = 0
        self.db_name = []
        self.db_data = []

        self.face_det = FaceDetApp(self.face_det_kmodel,
                                 model_input_size=self.det_input_size,
                                 anchors=self.anchors,
                                 confidence_threshold=self.confidence_threshold,
                                 nms_threshold=self.nms_threshold,
                                 rgb888p_size=self.rgb888p_size,
                                 display_size=self.display_size,
                                 debug_mode=0)

        self.face_reg = FaceRegistrationApp(self.face_reg_kmodel,
                                          model_input_size=self.reg_input_size,
                                          rgb888p_size=self.rgb888p_size,
                                          display_size=self.display_size)

        self.face_det.config_preprocess()
        self.database_init()
        self.pan_tilt = PanTilt()

        self.track_state = 'GLOBAL_SEARCH'
        self.lost_frames = 0
        self.short_lost_frames = 8
        self.local_search_frames = 60
        self.last_target_center = None
        self.last_target_wh = None
        self.last_target_box = None
        self.last_pan_angle = self.pan_tilt.pan_angle
        self.last_tilt_angle = self.pan_tilt.tilt_angle

    def run(self, input_np):
        det_boxes, landms = self.face_det.run(input_np)
        recg_res = []

        for landm in landms:
            self.face_reg.config_preprocess(landm)
            feature = self.face_reg.run(input_np)
            res = self.database_search(feature)
            recg_res.append(res)

        return det_boxes, recg_res

    def database_init(self):
        with ScopedTiming("database_init", self.debug_mode > 1):
            try:
                db_file_list = os.listdir(self.database_dir)
                for db_file in db_file_list:
                    if not db_file.endswith('.bin'):
                        continue
                    if self.valid_register_face >= self.max_register_face:
                        break

                    valid_index = self.valid_register_face
                    full_db_file = self.database_dir + db_file

                    with open(full_db_file, 'rb') as f:
                        data = f.read()
                    feature = np.frombuffer(data, dtype=np.float)
                    self.db_data.append(feature)

                    name = db_file.split('.')[0]
                    self.db_name.append(name)
                    self.valid_register_face += 1
            except Exception as e:
                print(e)
                print("未检测到人脸数据库，请先按照教程步骤，注册人脸信息")
                print("No face database detected, please follow the tutorial steps to register the face information")

            db_file_list = os.listdir(self.database_dir)
            for db_file in db_file_list:
                if not db_file.endswith('.bin'):
                    continue
                if self.valid_register_face >= self.max_register_face:
                    break

                valid_index = self.valid_register_face
                full_db_file = self.database_dir + db_file

                with open(full_db_file, 'rb') as f:
                    data = f.read()
                feature = np.frombuffer(data, dtype=np.float)
                self.db_data.append(feature)

                name = db_file.split('.')[0]
                self.db_name.append(name)
                self.valid_register_face += 1

    def database_reset(self):
        with ScopedTiming("database_reset", self.debug_mode > 1):
            print("database clearing...")
            self.db_name = []
            self.db_data = []
            self.valid_register_face = 0
            print("database clear Done!")

    def database_search(self, feature):
        with ScopedTiming("database_search", self.debug_mode > 1):
            v_id = -1
            v_score_max = 0.0

            feature /= np.linalg.norm(feature)

            for i in range(self.valid_register_face):
                db_feature = self.db_data[i]
                db_feature /= np.linalg.norm(db_feature)
                v_score = np.dot(feature, db_feature)/2 + 0.5

                if v_score > v_score_max:
                    v_score_max = v_score
                    v_id = i

            if v_id == -1:
                return 'unknown'
            elif v_score_max < self.face_recognition_threshold:
                return 'unknown'
            else:
                result = 'name: {}, score: {}'.format(self.db_name[v_id], v_score_max)
                return result

    def update_lost_search(self):
        self.lost_frames += 1

        if not self.last_target_center:
            self.track_state = 'GLOBAL_SEARCH'
            self.pan_tilt.reset_pd()
            self.pan_tilt.global_search(self.lost_frames)
            return

        if self.lost_frames <= self.short_lost_frames:
            self.track_state = 'SHORT_LOST'
            self.pan_tilt.reset_pd()
            return

        local_lost_frames = self.lost_frames - self.short_lost_frames
        if self.last_target_center and local_lost_frames <= self.local_search_frames:
            self.track_state = 'LOCAL_SEARCH'
            self.pan_tilt.local_search(self.last_pan_angle, self.last_tilt_angle, local_lost_frames)
        else:
            self.track_state = 'GLOBAL_SEARCH'
            global_lost_frames = local_lost_frames - self.local_search_frames
            if global_lost_frames < 0:
                global_lost_frames = self.lost_frames
            self.pan_tilt.global_search(global_lost_frames)

    def draw_result(self, pl, dets, recg_results):
        """绘制识别结果并执行云台追踪 (单人专属版)"""
        pl.osd_img.clear()

        target_face_center = None
        target_face_wh = None
        target_face_box = None
        target_score = -1

        if dets:
            for i, det in enumerate(dets):
                x1, y1, w, h = map(lambda x: int(round(x, 0)), det[:4])
                x1 = x1 * self.display_size[0]//self.rgb888p_size[0]
                y1 = y1 * self.display_size[1]//self.rgb888p_size[1]
                w = w * self.display_size[0]//self.rgb888p_size[0]
                h = h * self.display_size[1]//self.rgb888p_size[1]

                if i < len(recg_results):
                    recg_text = recg_results[i]
                else:
                    recg_text = 'unknown'

                if recg_text == 'unknown':
                    pl.osd_img.draw_rectangle(x1, y1, w, h, color=(255,0,0,255), thickness=4)
                else:
                    pl.osd_img.draw_rectangle(x1, y1, w, h, color=(255,0,255,0), thickness=4)

                    center_x = x1 + w//2
                    center_y = y1 + h//2
                    score = w * h
                    if self.last_target_center:
                        dx = center_x - self.last_target_center[0]
                        dy = center_y - self.last_target_center[1]
                        score = 1000000 - (dx * dx + dy * dy)

                    if target_face_center is None or score > target_score:
                        target_face_center = (center_x, center_y)
                        target_face_wh = (w, h)
                        target_face_box = (x1, y1, w, h)
                        target_score = score

                pl.osd_img.draw_string_advanced(x1, y1, 32, recg_text, color=(255,255,0,0))

                pattern = r'name: (.*), score: (.*)'
                match = re.match(pattern, recg_text)

                if match:
                    name_value = match.group(1)
                    score_value = match.group(2)
                    pto_data = pto.get_face_recoginiton_data(x1, y1, w, h, name_value, score_value)
                    uart.send(pto_data)
                else:
                    pto_data = pto.get_face_recoginiton_data(x1, y1, w, h, recg_text, 0)
                    uart.send(pto_data)

        if self.pan_tilt.is_manual():
            self.pan_tilt.update_manual_motion()

            if target_face_center:
                send_track_line(
                    True,
                    self.pan_tilt.pan_angle,
                    self.pan_tilt.tilt_angle,
                    float(target_face_center[0]),
                    float(target_face_center[1]),
                    float(target_face_wh[0]),
                    float(target_face_wh[1])
                )
            else:
                send_track_line(
                    False,
                    self.pan_tilt.pan_angle,
                    self.pan_tilt.tilt_angle,
                    0.0, 0.0, 0.0, 0.0
                )
            return

        if target_face_center:
            self.track_state = 'TRACKING'
            self.lost_frames = 0
            self.pan_tilt.track(target_face_center[0], target_face_center[1], self.display_size[0], self.display_size[1])
            self.last_target_center = target_face_center
            self.last_target_wh = target_face_wh
            self.last_target_box = target_face_box
            self.last_pan_angle = self.pan_tilt.pan_angle
            self.last_tilt_angle = self.pan_tilt.tilt_angle

            send_track_line(
                True,
                self.pan_tilt.pan_angle,
                self.pan_tilt.tilt_angle,
                float(target_face_center[0]),
                float(target_face_center[1]),
                float(target_face_wh[0]),
                float(target_face_wh[1])
            )
        else:
            self.update_lost_search()
            # 没检测到任何脸时也发一条无目标状态
            send_track_line(
                False,
                self.pan_tilt.pan_angle,
                self.pan_tilt.tilt_angle,
                0.0, 0.0, 0.0, 0.0
            )

    def deinit(self):
        """退出程序时释放资源"""
        self.face_det.deinit()
        self.face_reg.deinit()
        self.pan_tilt.deinit()


def exce_demo(pl):
    global fr
    display_mode = pl.display_mode
    rgb888p_size = pl.rgb888p_size
    display_size = pl.display_size

    face_det_kmodel_path = "/sdcard/kmodel/face_detection_320.kmodel"
    face_reg_kmodel_path = "/sdcard/kmodel/face_recognition.kmodel"
    anchors_path = "/sdcard/utils/prior_data_320.bin"
    database_dir = "/data/face_database/3889/"
    face_det_input_size = [320,320]
    face_reg_input_size = [112,112]
    confidence_threshold = 0.5
    nms_threshold = 0.2
    anchor_len = 4200
    det_dim = 4

    anchors = np.fromfile(anchors_path, dtype=np.float)
    anchors = anchors.reshape((anchor_len, det_dim))
    face_recognition_threshold = 0.65

    fr = FaceRecognition(face_det_kmodel_path, face_reg_kmodel_path,
                        det_input_size=face_det_input_size,
                        reg_input_size=face_reg_input_size,
                        database_dir=database_dir,
                        anchors=anchors,
                        confidence_threshold=confidence_threshold,
                        nms_threshold=nms_threshold,
                        face_recognition_threshold=face_recognition_threshold,
                        rgb888p_size=rgb888p_size,
                        display_size=display_size)

    try:
        while True:
            with ScopedTiming("total", 1):
                img = pl.get_frame()
                det_boxes, recg_res = fr.run(img)
                poll_esp32_gimbal_command(fr.pan_tilt)
                fr.draw_result(pl, det_boxes, recg_res)
                pl.show_image()
                gc.collect()
    except Exception as e:
        print("人脸识别功能退出")
    finally:
        exit_demo()


def exit_demo():
    global fr
    if fr:
        fr.deinit()


if __name__ == "__main__":
    rgb888p_size = [640,360]
    display_size = [640,480]
    display_mode = "lcd"

    pl = PipeLine(rgb888p_size=rgb888p_size,
                 display_size=display_size,
                 display_mode=display_mode)
    pl.create()
    exce_demo(pl)
