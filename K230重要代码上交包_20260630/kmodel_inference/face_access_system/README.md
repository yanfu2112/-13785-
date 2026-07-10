# K230 RT-Smart 三进程人脸门禁

本工程从官方 `face_recognition` 示例派生，原示例保持不变。工程拆分为：

- `capture_display.elf`：独占 VICAP/VO，采集摄像头、维护双帧缓冲、显示识别结果。
- `face_ai.elf`：人脸检测、五点对齐、特征提取、数据库查询和动态注册。
- `access_control.elf`：连续帧判定、继电器、蜂鸣器及 CSV 日志。

三个 RT-Smart 用户进程通过 `lwp_shm` 共享控制块。图像存放在采集进程分配的双 MMZ 缓冲中，AI 进程按物理地址映射并快速复制到本地推理缓冲；进程地址空间相互隔离。

## 安全默认值

`access_control.elf` 默认是 dry-run，不会操作 GPIO，只打印开门/报警信息。确认接线和 pinmux 后，使用 `start_face_access.sh --gpio` 才会启用继电器与蜂鸣器。

默认引脚：

- 继电器：GPIO27（原理图 CN4-1）
- 蜂鸣器：GPIO46（原理图 CN4-3，复用功能需确认配置为 GPIO）

GPIO 不得直接驱动电锁或继电器线圈，必须使用带隔离/驱动的继电器模块或 MOSFET，并为电锁提供独立电源。

## 编译

在 Ubuntu 的 SDK 根目录进入官方 Docker：

```bash
sudo docker run -u root -it \
  -v $(pwd):$(pwd) \
  -v $(pwd)/toolchain:/opt/toolchain \
  -w $(pwd) \
  ghcr.io/kendryte/k230_sdk /bin/bash
```

然后执行：

```bash
cd src/reference/K230_AI_Demo_Development_Process_Analysis/kmodel_related/kmodel_inference
./build_app.sh
```

成功后生成：

```text
k230_bin/face_access/
├── capture_display.elf
├── face_ai.elf
├── access_control.elf
├── face_detect_640.kmodel
├── face_recognize.kmodel
├── start_face_access.sh
├── stop_face_access.sh
├── linux_service/
└── db/
```

## 上传

```bash
adb push k230_bin/face_access /sharefs/
```

在 RT-Smart 串口 B 中，先按 `q` 退出原自启程序：

```sh
cd /sharefs/face_access
chmod +x *.sh *.elf
./start_face_access.sh
```

键盘命令：

- `i`：输入姓名，AI 在检测到下一张正脸时注册。
- `r`：清空数据库。
- `q` 或 `Esc`：停止三个进程。

确认 GPIO 接线后：

```sh
./start_face_access.sh --gpio
```

也可以覆盖默认引脚：

```sh
RELAY_PIN=27 BUZZER_PIN=46 ./start_face_access.sh --gpio
```

## Linux 小核 Web

在 Linux 串口 A 中执行：

```sh
cd /sharefs/face_access/linux_service
./start_web.sh
```

浏览器访问 `http://开发板IP:8080`，可以查看日志、发起注册和清空数据库。设置环境变量 `FACE_ACCESS_WEBHOOK` 后，陌生人报警会以 JSON POST 到指定地址：

```sh
FACE_ACCESS_WEBHOOK=http://服务器/alarm ./start_web.sh
```

若板端 Buildroot 未安装 Python 3，可在已连接 ADB 的 Ubuntu 主机运行：

```sh
cd face_access_system/linux_service
./start_web_adb.sh
```

此模式通过 ADB 读取 `/sharefs/face_access/access.csv` 并下发注册、清库命令，
浏览器访问 Ubuntu 的 `http://127.0.0.1:8080`。

## 门禁策略

- 同一个已注册人员连续识别 3 帧后开门。
- 继电器保持 1 秒后自动关闭。
- 同一人员开门后 3 秒冷却。
- 陌生人连续出现 3 帧后蜂鸣器报警。
- 注册会连续采集 5 个尺寸合格的人脸特征，平均归一化后保存。
- 识别日志写入 `/sharefs/face_access/access.csv`。
- 任一进程停止时继电器默认关闭。

## 当前阶段

该版本提供三进程、动态注册、门禁状态机、GPIO dry-run/实控、CSV 日志、Linux Web 管理和可选 Webhook 告警。下一阶段将加入多样本注册质量控制、IPCM 直连通知以及完整性能验收工具。
