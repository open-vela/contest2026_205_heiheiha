# 智慧看护终端 ESP32-S3-EYE 本地原型

本目录为 ESP-IDF 固件，不是 OpenVela 固件。已接入屏幕、按键、摄像头，并新增手机控制台、主动视频共享、参数保存、报警历史和屏幕关怀提醒，不需要额外硬件或云服务。

## 第一次使用

1. 烧录后设备停在 SETTINGS / Network，显示热点名称、12 位 Key 和地址。
2. 手机连接 AI-Care-xxxx，密码就是屏幕上的 Key。提示无互联网时，选择保留 Wi-Fi 连接。
3. 浏览器输入 http://192.168.4.1，再输入相同 Key 登录。
4. 先点“测试屏幕报警”，验证倒计时、取消和记录，不需要真的跌倒。
5. 点“开始 / 重新校准”，保持摄像头固定、场景正常，等待 Base 达到 12/12。不要用固定几秒代替校准完成状态。
6. 如需视频，监测开始后明确点击“开启共享并查看”。共享最长 5 分钟，默认关闭；停止监测、报警触发或重启都会关闭共享。

Key 保存在 NVS，断电不变。退出配网页后，按 MENU 到 SETTINGS，选择 Network 后按 PLAY 查看。源码没有统一密码；网页不会返回家庭 Wi-Fi 密码。

## 已实现

| 功能 | 行为与边界 |
| --- | --- |
| 局域网控制台 | 中文移动端页面、实时状态、开始/停止/重新校准、取消报警、手动测试 |
| 配网 | 独立 WPA2 热点；保存 2.4 GHz WPA2 家庭网络，失败仍保留热点，后台限速重连 |
| 视频 | 端口 81 的 HTTP MJPEG，QVGA JPEG，最多一个观看端，主动授权，无图像落盘 |
| 接口控制 | API 需要设备 Key，不开放 CORS，网页没有第三方在线依赖 |
| 记录 | 最近 32 条保存在 NVS，可查看、导出 JSON、确认清空；手动测试独立标记 |
| 参数 | 候选/恢复阈值、Motion 上限、连续采样数、报警延迟，断电保留，下次开始时应用 |
| 定时关怀 | 0 关闭或每 5–1440 分钟一次屏幕/网页提醒，可确认，无语音 |
| 麦克风 | 板载 I2S 数字麦克风采音自检和音量峰值；未接入语音识别或扬声器 |
| 自检 | 摄像头、网络/IP、剩余及最低堆内存、存储错误和丢弃记录数 |
| 时间 | 网页登录时同步手机时间；未同步的事件显示启动编号和运行时间 |

连接家庭路由器后，在网页网络状态中查看实际局域网 IP。保存配置不等于连上路由器，以 wifi_connected 为准。同一无线网卡同时负责热点与家庭网络，切换网络时视频可能需重新打开。

## 检测与报警的含义

- 没有训练好的模型和推理适配器。当前为画面差分演示，不是人体关键点识别。
- Motion 是相邻采样的灰度变化分数；Pose 是相对前 12 个采样建立的基线的差异，均不是概率。
- 默认候选：Pose ≥60、Motion ≤35，连续 4 次满足。中断会清零连续计数；Pose <45 时清除候选。
- 保留现有报警流程：候选触发后暂停采集/预览；5 秒是人工取消窗口，不是在暂停期间重新识别人体，更不代表医学确认。
- 手动测试只验证报警 UI 和日志，不验证识别准确率，不会写成真实跌倒事件。
- 摄像头预览关闭时不进行后台检测。取消报警后需主动重新开始。
- 本原型不能用于无人值守的人身安全保障。请用已有视频或安全摆姿测试，不要真实摔倒。

## 默认参数

| 参数 | 默认 | 允许值 |
| --- | --- | --- |
| Pose 候选 | 60 | 10–100 |
| 恢复阈值 | 45 | 1 至 Pose 阈值减 1 |
| Motion 上限 | 35 | 0–100 |
| 连续采样数 | 4 | 2–20 |
| 确认窗口 | 5 秒 | 3–60 秒 |
| 关怀间隔 | 0 | 0 关闭，或 5–1440 分钟 |

提醒修改间隔或重启后从头计时。没有接入环境传感器、扬声器、蜂鸣器、天气服务或云通知；麦克风只用于本地采音自检和峰值显示，不输出伪造读数或已推送状态。

## 编译烧录

环境：ESP-IDF 6.1、ESP32-S3、8 MB PSRAM。保守沿用 2 MB Flash 配置，不假设板上额外 Flash。程序区扩大到 0x1f0000；NVS 仍为 0x9000–0xefff，PHY 仍在 0xf000，与旧固件相同。不需要 erase-flash。

Windows PowerShell：

    & 'D:\ai_care_terminal\firmware\eye_display\tools\build.ps1' -Action build
    & 'D:\ai_care_terminal\firmware\eye_display\tools\build.ps1' -Action flash -Port COM7
    & 'D:\ai_care_terminal\firmware\eye_display\tools\build.ps1' -Action monitor -Port COM7

脚本只配置当前进程环境，不修改系统 PATH。其他安装路径可通过 -IdfPath、-ToolsPath 传入。ESP-IDF 终端中也可在本目录运行 idf.py -p COM7 build flash。

使用既有 ESP-IDF、LVGL、camera、jpeg 组件，无新增在线依赖。存储初始化失败不会自动擦除 NVS；摄像头可继续使用，网络报告不可用，请检查串口日志。

## 接口

除首页外需要 X-Care-Key 请求头；视频 img 使用查询参数 Key。POST 为 application/x-www-form-urlencoded。

| 方法 | 路径 | 用途 |
| --- | --- | --- |
| GET | /api/status | 实时状态与未接入能力 |
| GET/POST | /api/config | pose、recovery、motion、hold、delay、reminder |
| POST | /api/wifi | ssid、password；两者留空可清除家庭网络 |
| POST | /api/command | action：start、stop、cancel、test、share_on、share_off、reminder_ack |
| GET | /api/history | 最新事件优先 |
| POST | /api/history/clear | confirm=clear |
| POST | /api/time | epoch：手机当前 Unix 秒数 |
| GET（81 端口） | /stream?key=... | 授权有效时的 MJPEG |

202 表示命令已排队，不代表硬件执行成功，需观察实时状态。视频和控制运行在不同 HTTP 服务任务。

## 隐私与剩余工作

- HTTP 仅限可信局域网。禁止公网映射 80/81，不分享 Key 或带 Key 的视频地址。
- 无 HTTPS、Flash 加密和后台手机系统推送。Wi-Fi 凭据在未加密 NVS 中，物理读取 Flash 的人仍可能访问。
- 关闭网页时尽力关闭共享；断网可能使关闭命令丢失，但设备端 5 分钟到期会关闭。停止时不通过接口提供缓存旧图像。
- 日志异步写入；失败和队列满会增加错误数，事件刚生成即断电可能丢失尚未提交的记录。
- 尚需真实模型与独立验证样本、传感器及音频硬件、带认证/TLS 的云推送，以及 OpenVela 环境中的真正移植和整机验收。

## 文件

- main/main.c：屏幕、按键、摄像头及功能接入。
- main/fall_detector.c：可调画面差分演示。
- main/care_store.c：NVS 参数、异步事件队列和历史。
- main/care_portal.c：热点/家庭网络、API、受控视频。
- main/dashboard.html：离线中文手机控制台。
- MODEL_INTEGRATION.md：真实模型接入边界。
