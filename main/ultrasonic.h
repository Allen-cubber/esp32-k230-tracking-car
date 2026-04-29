#ifndef ULTRASONIC_H
#define ULTRASONIC_H

#include <stdint.h>

// 超声波模块引脚定义 (根据实际接线修改)
#define ULTRASONIC_TRIG_PIN     12   // 触发引脚
#define ULTRASONIC_ECHO_PIN     13   // 回波引脚

// 声速定义 (cm/us)，在20°C干燥空气中约为 0.0343 cm/us，取近似值
#define SOUND_SPEED_CM_US       0.0343

// 最大测量距离 (cm) 和超时时间 (us)
#define MAX_DISTANCE_CM         400
#define TIMEOUT_US              (MAX_DISTANCE_CM * 2 * 29.1)  // 根据声速计算超时

// 初始化超声波模块
void ultrasonic_init(void);

// 测量距离，返回厘米值，如果超时或错误返回 -1
float ultrasonic_measure_cm(void);

#endif // ULTRASONIC_H