# 智能追踪小车当前引脚分布

更新时间：2026-05-11

本文档按当前代码整理，覆盖 ESP32-S3、K230 和树莓派语音端。当前版本中，ESP32 与 K230 已改为通过 Wi-Fi/MQTT 通信，不再使用 ESP32-K230 UART 线。

## ESP32-S3

### 电机驱动

定义位置：`main/motor_control.h`

| 功能 | ESP32-S3 GPIO | 方向 | 代码定义 | 说明 |
|---|---:|---|---|---|
| 左电机 PWM | GPIO4 | 输出 | `MOTOR_L_PWM_PIN` | LEDC channel 0，5 kHz，13-bit |
| 左电机 DIR1 | GPIO5 | 输出 | `MOTOR_L_DIR1_PIN` | 左轮方向控制 |
| 左电机 DIR2 | GPIO6 | 输出 | `MOTOR_L_DIR2_PIN` | 左轮方向控制 |
| 右电机 PWM | GPIO7 | 输出 | `MOTOR_R_PWM_PIN` | LEDC channel 1，5 kHz，13-bit |
| 右电机 DIR1 | GPIO15 | 输出 | `MOTOR_R_DIR1_PIN` | 右轮方向控制 |
| 右电机 DIR2 | GPIO16 | 输出 | `MOTOR_R_DIR2_PIN` | 右轮方向控制 |

注意：左右电机方向是否正确取决于电机线序。如果出现左右轮反向，可以优先在接线或方向控制逻辑里调整。

### 轮速编码器

定义位置：`main/encoder.h`

| 编码器信号 | ESP32-S3 GPIO | 方向 | 代码定义 | 说明 |
|---|---:|---|---|---|
| 左轮 E1A | GPIO39 | 输入 | `ENCODER_L_A_PIN` | PCNT 左轮 A 相 |
| 左轮 E1B | GPIO40 | 输入 | `ENCODER_L_B_PIN` | PCNT 左轮 B 相 |
| 右轮 E2A | GPIO41 | 输入 | `ENCODER_R_A_PIN` | PCNT 右轮 A 相 |
| 右轮 E2B | GPIO42 | 输入 | `ENCODER_R_B_PIN` | PCNT 右轮 B 相 |

当前底盘速度闭环参数在 `main/motor_control.c` 顶部修改：

| 参数 | 当前值 | 说明 |
|---|---:|---|
| `MOTOR_CLOSED_LOOP_PERIOD_MS` | 50 ms | 速度环采样周期 |
| `MOTOR_ENCODER_TARGET_MAX_PPS` | 3000 | 满速对应的编码器目标脉冲/秒估计值 |
| `MOTOR_SPEED_PID_KP` | 0.65 | 速度环比例项 |
| `MOTOR_SPEED_PID_KI` | 0.18 | 速度环积分项 |
| `MOTOR_SPEED_PID_KD` | 0.0 | 速度环微分项，默认不用 |

注意：编码器 A/B 输出电平必须不高于 3.3V。若编码器板按 5V 供电且 A/B 被上拉到 5V，需要加分压或电平转换。闭环控制使用编码器脉冲绝对值计算轮速，因此 E1A/E1B 或 E2A/E2B 相序反了不会立刻导致方向失控；方向仍由 TB6612 的 AIN/BIN 控制。如果后续要做精确里程计，再校正编码器正负方向。

### 超声波避障

定义位置：`main/ultrasonic.h`

| 功能 | ESP32-S3 GPIO | 方向 | 代码定义 | 说明 |
|---|---:|---|---|---|
| Trig | GPIO12 | 输出 | `ULTRASONIC_TRIG_PIN` | 触发测距 |
| Echo | GPIO13 | 输入 | `ULTRASONIC_ECHO_PIN` | 回波输入 |

注意：如果使用 5V 供电的 HC-SR04，Echo 可能是 5V 电平，ESP32-S3 GPIO 不耐 5V，建议加分压或电平转换。

### MPU6050 姿态传感器

定义位置：`main/Kconfig.projbuild`，当前值来自 `sdkconfig`

| 功能 | ESP32-S3 GPIO | 总线 | 当前配置 | 说明 |
|---|---:|---|---|---|
| SDA | GPIO17 | I2C0 | `CONFIG_MPU6050_I2C_SDA_GPIO=17` | MPU6050 I2C 数据线 |
| SCL | GPIO18 | I2C0 | `CONFIG_MPU6050_I2C_SCL_GPIO=18` | MPU6050 I2C 时钟线 |

其他参数：

| 参数 | 当前值 |
|---|---:|
| I2C 频率 | 400 kHz |
| MPU6050 地址 | `0x68` |
| 采样周期 | 20 ms |
| 上电陀螺仪校准样本 | 500 |

### ST7735 情绪显示屏

定义位置：`main/Kconfig.projbuild`，驱动使用位置：`main/robot_lcd.c`

| 屏幕引脚 | ESP32-S3 GPIO | 方向 | 当前配置 | 说明 |
|---|---:|---|---|---|
| SCLK / SCK | GPIO14 | 输出 | `CONFIG_ROBOT_LCD_SCLK_GPIO=14` | SPI 时钟 |
| MOSI / SDA | GPIO11 | 输出 | `CONFIG_ROBOT_LCD_MOSI_GPIO=11` | SPI 数据输出 |
| MISO | 未使用 | 输入 | `GPIO_NUM_NC` | ST7735 当前不读屏 |
| CS | GPIO10 | 输出 | `CONFIG_ROBOT_LCD_CS_GPIO=10` | SPI 片选 |
| DC / A0 | GPIO9 | 输出 | `CONFIG_ROBOT_LCD_DC_GPIO=9` | 数据/命令选择 |
| RST | GPIO21 | 输出 | `CONFIG_ROBOT_LCD_RST_GPIO=21` | 屏幕复位 |
| BL / LED | GPIO8 | 输出 | `CONFIG_ROBOT_LCD_BL_GPIO=8` | 背光控制，高电平点亮 |
| VCC | 3.3V | 电源 | - | 建议接 ESP32 3V3 |
| GND | GND | 电源 | - | 必须共地 |

当前显示参数：

| 参数 | 当前值 |
|---|---:|
| SPI Host | SPI2 |
| SPI 频率 | 20 MHz |
| 分辨率 | 128 x 160 |
| X/Y offset | 0 / 0 |
| 颜色反相 | 开启 |

### Wi-Fi / MQTT

Wi-Fi 和 MQTT 不占用 GPIO。

当前 ESP32 配置来自 `sdkconfig`：

| 项 | 当前值 |
|---|---|
| Wi-Fi SSID | `OnePlus13` |
| MQTT broker | `mqtt://10.179.231.220:1883` |
| MQTT device id | `robot01` |
| MQTT client id | `robot01-esp32` |
| MQTT topic root | `qrs` |

### ESP32 当前占用 GPIO 汇总

| GPIO | 当前用途 |
|---:|---|
| 4 | 左电机 PWM |
| 5 | 左电机 DIR1 |
| 6 | 左电机 DIR2 |
| 7 | 右电机 PWM |
| 8 | ST7735 背光 |
| 9 | ST7735 DC |
| 10 | ST7735 CS |
| 11 | ST7735 MOSI |
| 12 | 超声波 Trig |
| 13 | 超声波 Echo |
| 14 | ST7735 SCLK |
| 15 | 右电机 DIR1 |
| 16 | 右电机 DIR2 |
| 17 | MPU6050 SDA |
| 18 | MPU6050 SCL |
| 21 | ST7735 RST |
| 39 | 左轮编码器 E1A |
| 40 | 左轮编码器 E1B |
| 41 | 右轮编码器 E2A |
| 42 | 右轮编码器 E2B |

## K230 / CanMV

主脚本：`main1.py`

### 二维云台舵机

定义位置：`main1.py` 的 `PanTilt` 类

| 功能 | K230 GPIO | FPIOA 功能 | PWM 通道 | 频率 | 初始 duty | 说明 |
|---|---:|---|---:|---:|---:|---|
| Pan 水平舵机 | GPIO42 | `PWM0` | 0 | 50 Hz | 7.5 | 控制左右转头 |
| Tilt 俯仰舵机 | GPIO43 | `PWM1` | 1 | 50 Hz | 7.5 | 控制上下点头 |

舵机角度范围：

| 项 | 当前值 |
|---|---:|
| 软件限制最小角度 | 10° |
| 软件限制最大角度 | 170° |
| 中心角 | 90° |
| duty 映射 | `2.5 + angle / 180 * 10.0` |

注意：舵机建议单独稳定供电，K230 GPIO 只接信号线，舵机电源 GND 要和 K230 GND 共地。

### K230 摄像头 / LCD

摄像头和 K230 LCD 走 CanMV/K230 板载接口，当前 Python 脚本没有显式配置普通 GPIO 引脚。相关配置主要是分辨率和显示模式：

| 项 | 当前值 |
|---|---|
| 摄像头输入尺寸 | 640 x 360 |
| LCD 显示尺寸 | 640 x 480 |
| display mode | `lcd` |

### K230 Wi-Fi / MQTT

Wi-Fi 和 MQTT 不占用普通 GPIO。

当前 `main1.py` 配置：

| 项 | 当前值 |
|---|---|
| Wi-Fi SSID | `OnePlus13` |
| MQTT broker | `10.179.231.220:1883` |
| MQTT client id | `robot01-k230` |
| MQTT topic root | `qrs` |
| MQTT device id | `robot01` |

当前 K230 通过 MQTT 发布追踪数据，并订阅云台命令，不再依赖 ESP32-K230 UART。

## 树莓派语音端

目录：`raspberry_pi_voice/`

树莓派语音端当前主要使用 USB 声卡/语音模块和网络，不占用 GPIO。

| 外设 | 连接方式 | 说明 |
|---|---|---|
| 语音模块 / USB 声卡 | 树莓派 USB | ASR 音频采集，建议 `.env` 使用稳定设备名 |
| MQTT broker | 本机网络服务 | 默认 `127.0.0.1:1883` |
| ESP32/K230 通信 | Wi-Fi / MQTT | 与 ESP32、K230 连同一个热点 |

推荐语音端 `.env` 中使用稳定录音设备名：

```env
MIC_DEVICE=plughw:CARD=Device,DEV=0
```

## 当前未使用 / 已废弃连接

| 连接 | 当前状态 | 说明 |
|---|---|---|
| ESP32-K230 UART | 已废弃 | 当前追踪数据和云台命令通过 MQTT 传输 |
| ESP32 SPI MISO for LCD | 未使用 | ST7735 当前只写屏，不读屏 |
| 树莓派 GPIO | 未使用 | 语音端使用 USB 音频和 MQTT |

## 修改位置速查

| 要改的内容 | 文件 |
|---|---|
| 电机 GPIO | `main/motor_control.h` |
| 超声波 GPIO | `main/ultrasonic.h` |
| LCD SPI GPIO / 分辨率 / 颜色反相 | `main/Kconfig.projbuild` 或 `idf.py menuconfig` |
| MPU6050 SDA/SCL | `main/Kconfig.projbuild` 或 `idf.py menuconfig` |
| ESP32 Wi-Fi / MQTT | `idf.py menuconfig` 生成 `sdkconfig` |
| K230 舵机 GPIO / PWM 通道 | `main1.py` 的 `PanTilt` 类 |
| K230 Wi-Fi / MQTT | `main1.py` 顶部配置 |
| 树莓派录音设备 / ASR 参数 | `raspberry_pi_voice/.env` |

## 接线注意事项

- 所有通信相关模块必须共地：ESP32、电机驱动、超声波、MPU6050、LCD、K230 舵机供电、树莓派相关外设都要根据实际电源拓扑保证公共 GND。
- ESP32-S3 GPIO 不耐 5V。超声波 Echo、部分外设信号如果是 5V，需要电平转换。
- 电机和舵机不要直接从 ESP32/K230 的 3V3 取大电流；推荐独立电源供电，并共地。
- ST7735 小屏建议接 ESP32 3V3，背光如果电流较大，需要确认开发板 3V3 余量。
- 当前 ESP32 GPIO 8~14、17、18、21 已被 LCD/超声波/MPU 占用，新增外设前先检查冲突。
