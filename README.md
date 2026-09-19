# 智慧看护终端 AI-Care

## 一、作品简介

智慧看护终端是一套运行在 `ESP32-S3-EYE` 上的本地看护原型。它使用摄像头画面差分、板载数字麦克风、加速度计和屏幕构成端侧闭环：设备在本地采集状态，给出 Pose / Motion 变化指标，在屏幕上执行演示报警，并通过设备热点提供手机控制台。

本作品重点验证“家庭看护设备不依赖云端也能工作”的端侧方案：控制、状态查看、事件记录和视频共享都在可信局域网内完成，默认不上传图像，也不开放公网服务。

当前版本是可运行的 ESP-IDF 原型，已完成硬件端到端联调；它不是医疗器械，也没有宣称真实跌倒识别或语音识别能力。

演示视频：[`docs/demo/ai-care-demo.mp4`](docs/demo/ai-care-demo.mp4)。视频用于展示设备启动、手机控制台和本地看护流程，不代表真实医疗或跌倒识别准确率。

项目规划书：[`docs/project-plan/基于 OpenVela 的端侧 AI 智慧看护终端.docx`](docs/project-plan/基于%20OpenVela%20的%20端侧%20AI%20智慧看护终端.docx)。

## 二、选题方向

**AI 硬件产品创新**。

作品将摄像头、屏幕、按键、麦克风、加速度计和本地 Web 控制台组合成一个可复现的端侧看护终端。当前用可解释的画面差分演示检测链路，为后续接入真实 AI 模型保留明确接口和验证边界。

## 三、主要功能

- 摄像头实时采集与画面差分演示检测。
- `Pose` 表示相对基线的画面变化分数，`Motion` 表示相邻采样的变化分数；二者都不是概率。
- 屏幕报警倒计时、取消、手动测试和最近事件记录。
- 板载 I2S 数字麦克风采音自检与峰值显示；不伪造语音识别结果。
- 加速度计状态、冲击值和 Bluetooth 5 LE 状态显示。
- 设备热点 `AI-Care-xxxx`、手机本地控制台、家庭 2.4 GHz Wi-Fi 配置。
- 受控 MJPEG 视频共享，默认关闭、最多一个观看端、最长 5 分钟、不保存图像。
- NVS 保存检测参数、设备访问 Key 和最近事件；网页登录时同步设备时间。

## 四、目录结构

- `firmware/eye_display/` — 实际 ESP32-S3-EYE 的 ESP-IDF 固件。
  - `main/main.c` — 屏幕、按键、摄像头和任务接入。
  - `main/fall_detector.c` — 可调的画面差分演示检测器。
  - `main/care_portal.c` — 热点、家庭 Wi-Fi、HTTP API 和视频服务。
  - `main/dashboard.html` — 内嵌式中文手机控制台。
  - `main/care_audio.c` — I2S 麦克风采样与峰值统计。
  - `main/care_imu.c` — 加速度计状态与冲击统计。
  - `main/care_store.c` — NVS 参数和事件历史。
  - `README.md` — 固件使用说明、接口和边界。
  - `VALIDATION.md` — 已执行验证和仍需实机验收的项目。
- `logs/` — AI Coding 日志归集目录。
- `docs/demo/` — 作品演示视频。
- `docs/project-plan/` — 作品规划书。
- `firmware/eye_display/MODEL_INTEGRATION.md` — 后续真实模型接入边界。

根目录的 `app/`、`board/`、`quickapp/` 是组委会仓库模板目录，本作品实际运行代码位于 `firmware/eye_display/`。

## 五、硬件与运行环境

- 开发板：`ESP32-S3-EYE`，8 MB PSRAM。
- 工具链：ESP-IDF `6.1`、CMake、Ninja。
- 当前固件按 2 MB Flash 分区配置构建。
- 板载 I2S 麦克风引脚：BCLK `GPIO41`、WS `GPIO42`、DIN `GPIO2`。
- 串口示例：`COM7`，波特率 `115200`。

## 六、编译、烧录与运行

在 Windows PowerShell 中，先准备 ESP-IDF 6.1 环境，然后执行：

```powershell
& 'D:\ai_care_terminal\firmware\eye_display\tools\build.ps1' -Action build
& 'D:\ai_care_terminal\firmware\eye_display\tools\build.ps1' -Action flash -Port COM7
& 'D:\ai_care_terminal\firmware\eye_display\tools\build.ps1' -Action monitor -Port COM7
```

首次运行：

1. 设备启动后进入 `SETTINGS / Network`，记录屏幕显示的热点名称和 12 位访问 Key。
2. 手机连接 `AI-Care-xxxx`，提示“无互联网”时选择保留连接。
3. 浏览器打开 `http://192.168.4.1`，输入同一个 Key 登录控制台。
4. 先点击“测试屏幕报警”，确认屏幕倒计时、取消和事件记录。
5. 点击“开始 / 重新校准”，等待基线达到 `12/12` 后再观察 Pose / Motion。
6. 需要视频时，先开始监测，再点击“开启共享并查看”；停止监测、报警或重启会自动关闭共享。
7. 如需家庭网络，在“网络与自检”中填写 2.4 GHz Wi-Fi；热点不会因家庭网络失败而关闭。

更完整的接口、隐私边界和故障排查见 `firmware/eye_display/README.md`，验证结果见 `firmware/eye_display/VALIDATION.md`。

## 七、接口与数据边界

除首页外，控制台 API 需要 `X-Care-Key` 请求头。主要接口如下：

| 接口 | 用途 |
| --- | --- |
| `GET /api/status` | 实时状态、Pose、Motion、麦克风、IMU、网络和内存信息 |
| `GET/POST /api/config` | 检测阈值、确认时间和关怀提醒参数 |
| `POST /api/command` | 开始、停止、取消、测试、共享控制 |
| `GET /api/history` | 查看最近事件 |
| `POST /api/wifi` | 保存或清除家庭 Wi-Fi 配置 |
| `GET /stream?key=...` | 端口 81 上的受控 MJPEG 视频 |

当前没有真实 AI 模型、语音识别、扬声器/蜂鸣器、云推送或公网服务。麦克风只用于本地采音自检和峰值显示；本项目不能用于无人值守的人身安全保障。

## 八、验证情况

- ESP-IDF 6.1 编译、链接和分区容量校验通过。
- ESP32-S3 固件已在 `COM7` 烧录，写入数据哈希校验通过。
- 已验证屏幕、按键、摄像头、热点、手机控制台、事件记录、参数保存、受控视频和页面状态刷新。
- 已验证麦克风采样状态接口；实机验收时可对着板载麦克风拍手或说话，观察网页峰值变化。
- 尚未声称真实模型准确率、真实跌倒识别、云端推送或 OpenVela 整机移植完成。

详细记录见 `firmware/eye_display/VALIDATION.md`。

## 九、AI Coding 使用说明

AI 主要参与了需求拆解、端侧架构设计、ESP-IDF API 编码、网页控制台编写、串口/烧录问题定位、断网和实时视频问题排查，以及 README 和验证记录整理。

协作方式是先让 AI 分析日志和代码路径，再由开发者在真实开发板上编译、烧录和验证；对涉及网络、摄像头、麦克风和报警的行为，以实机结果为准，不把模拟页面测试当作硬件验收。完整对话日志位于 `logs/`。
