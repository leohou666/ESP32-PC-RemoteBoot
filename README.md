# ESP32-PC-RemoteBoot

基于 ESP32-S3 的远程 PC 开关机 & 引导选择固件。通过 WiFi 连接局域网，提供 Web UI 和 REST API 来远程控制台式机的电源、重启，支持 Windows Boot Manager / GRUB 双模式引导。

## 功能特性

| 功能 | 说明 |
|---|---|
| **远程开机/关机** | 通过继电器模拟按压主板 JFP1 电源键和复位键 |
| **USB HID 引导选择** | 通过 USB HID 键盘模拟方向键 + 回车，兼容 GRUB 和 Windows Boot Manager |
| **BIOS 启动** | 开机或重启时自动按 DEL/F2 键进入 BIOS 设置界面 |
| **POST 检测** | USB 总线握手检测（无额外接线），或 HDD LED GPIO 检测 |
| **校园网自动认证** | 定期检测互联网连通性，断网时自动发送 ePortal 认证请求 |
| **Web UI** | 内置响应式 Web 控制面板，手机/电脑浏览器均可操作 |
| **OTA 升级** | 双分区 OTA + 回滚保护，Web UI 一键上传固件 |
| **系统日志** | PSRAM 环形缓冲捕获全部运行时日志，Web UI 实时查看/过滤 |
| **API Token 认证** | 首次启动自动生成随机 API Token，所有写操作需 Bearer 认证 |
| **NVS 配置持久化** | WiFi、校园网、引导类型、继电器极性等全部通过 NVS 存储，Web UI 热更新 |

## 硬件连接

### 所需材料

- ESP32-S3 开发板（需带 **原生 USB** 接口，GPIO19/20；**推荐带 PSRAM** 的模组如 N8R2/N16R8）
- 2 路继电器模块
- 杜邦线若干

### 接线图

```
ESP32-S3                    主板 JFP1 跳线
┌──────────┐               ┌─────────────┐
│  GPIO 4  ├──→ 继电器1 ──→│ PWR_SW+/-   │  电源键
│  GPIO 5  ├──→ 继电器2 ──→│ RST_SW+/-   │  复位键
│  GPIO 6  ├←── 1kΩ电阻 ←──│ HDD_LED+    │  硬盘灯(POST检测,可选)
│    GND   ├───────────────│ HDD_LED-    │
│          │               └─────────────┘
│ USB(原生) ├──→ USB 线 ──→  主板 USB 口    (HID 键盘 + POST 检测)
└──────────┘
```

> **注意**：ESP32-S3 有两个 USB 口，用于 HID 键盘的是 **原生 USB**（GPIO19 D−, GPIO20 D+），不是 USB-UART 口。刷固件用 USB-UART 口。POST 检测默认通过 USB 总线握手完成，无需接 HDD LED 线。

### GPIO 默认值

| 引脚 | 功能 | 默认 GPIO | 可修改方式 |
|---|---|---|---|
| 继电器1（电源键） | `RB_GPIO_RELAY1` | 4 | menuconfig / Web UI |
| 继电器2（复位键） | `RB_GPIO_RELAY2` | 5 | menuconfig / Web UI |
| HDD LED 检测 | `RB_GPIO_POST_DETECT` | 6 | menuconfig / Web UI |

### 继电器极性

大多数廉价继电器模块为**低电平触发**（GPIO LOW = 继电器吸合）。默认配置已开启内部上拉电阻，防止 ESP32 断电时 GPIO 浮空导致误触发。可在 Web UI 配置页切换极性。

> 如果断电后继电器仍然误触发，请在 GPIO4 和 GPIO5 上各加一个 **10kΩ 上拉电阻到 3.3V**。

## 编译 & 烧录

### 环境要求

- [ESP-IDF v6.0](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/)
- Python 3.8+

### 编译

```bash
# 进入 ESP-IDF 环境 (distrobox 用户)
distrobox enter esp && bash

# 激活 IDF 环境变量
. $HOME/.espressif/v6.0/esp-idf/export.sh

# 编译
idf.py build
```

### 烧录

```bash
# 首次烧录或分区表变更后，需擦除整片 Flash
idf.py -p /dev/ttyACM0 erase-flash

# 烧录
idf.py -p /dev/ttyACM0 flash

# 查看串口日志
idf.py -p /dev/ttyACM0 monitor
```

> **首次迁移说明**：本项目使用 `ota_0/ota_1 + otadata` 双分区布局，且新增了 PSRAM 配置。从旧版（factory + spiffs）迁移必须 `erase-flash` 后重新烧录，之后即可使用 OTA 升级。

> 如果遇到串口权限问题，在**宿主机**上执行：
> ```bash
> sudo usermod -aG dialout $USER
> # 然后注销重新登录
> ```

### 自定义配置

```bash
idf.py menuconfig
# → "RemoteBoot Configuration" 菜单下可调整：
#   - GPIO 引脚分配
#   - 继电器极性
#   - POST 检测方式和超时
#   - 引导器类型 (GRUB / Windows Boot Manager)
#   - GRUB 菜单索引
#   - 继电器按压时长
#   - 校园网认证服务器
```

> 大部分配置项也可以在 Web UI 的"系统配置"页面直接修改，无需重新编译。

## 使用方法

### 1. 首次配置

固件首次启动后会在串口日志中打印：
```
API Token: a1b2c3d4e5f6...  (32位十六进制)
```

**请记下这个 Token**，后续所有 API 调用都需要它。

### 2. 配置 WiFi

通过 API 设置 WiFi：

```bash
# 替换 <ESP_IP> 和 <TOKEN>
curl -X PUT http://<ESP_IP>/api/config \
  -H "Authorization: Bearer <TOKEN>" \
  -H "Content-Type: application/json" \
  -d '{
    "wifi_ssid": "你的WiFi名称",
    "wifi_pass": "WiFi密码"
  }'
```

### 3. Web UI

连接到同一局域网后，浏览器访问：
```
http://<ESP32的IP地址>
```

Web UI 提供以下功能：
- **开机** — 启动 PC（可选择 Windows / Fedora / BIOS / 默认）
- **关机 / 强制关机 / 重启** — 电源控制
- **系统配置** — WiFi、校园网、引导类型、继电器极性、GRUB 索引等
- **固件 OTA** — 上传 `remote_boot.bin` 自动升级
- **系统日志** — 查看运行时日志，支持级别过滤和关键字搜索

### 4. 校园网配置（可选）

如果你的网络需要 ePortal 认证：

```bash
curl -X PUT http://<ESP_IP>/api/config \
  -H "Authorization: Bearer <TOKEN>" \
  -H "Content-Type: application/json" \
  -d '{
    "campus_card": "你的学号",
    "campus_pass": "校园网密码"
  }'
```

## REST API

所有写操作需要 `Authorization: Bearer <token>` 请求头。

### 状态查询

| 方法 | 路径 | 认证 | 说明 |
|---|---|---|---|
| `GET` | `/api/status` | ❌ | 获取系统状态 (PC 状态、网络、当前 OS) |
| `GET` | `/api/netstat` | ❌ | 校园网连接状态 |
| `GET` | `/api/log` | ✅ | 获取系统日志（环形缓冲），支持 `?count=` `?start=` `?info=` `?clear=` |

### 电源控制

| 方法 | 路径 | 认证 | Body | 说明 |
|---|---|---|---|---|
| `POST` | `/api/boot` | ✅ | `{"os":"windows"}` | 开机（可选 OS: windows/fedora/bios/default） |
| `POST` | `/api/shutdown` | ✅ | — | 软关机 |
| `POST` | `/api/force_off` | ✅ | — | 强制断电 |
| `POST` | `/api/reboot` | ✅ | — | 按复位键 |
| `POST` | `/api/reboot_os` | ✅ | `{"os":"fedora"}` | 重启并选择 OS |

### 配置管理

| 方法 | 路径 | 认证 | 说明 |
|---|---|---|---|
| `GET` | `/api/config` | ✅ | 获取当前配置（密码脱敏） |
| `PUT` | `/api/config` | ✅ | 更新配置项 |

### 固件升级

| 方法 | 路径 | 认证 | Body | 说明 |
|---|---|---|---|---|
| `GET` | `/api/update/info` | ✅ | — | 固件版本、分区和 OTA 状态 |
| `POST` | `/api/update` | ✅ | raw `.bin` | 上传固件，成功后自动重启 |

### 校园网

| 方法 | 路径 | 认证 | 说明 |
|---|---|---|---|
| `POST` | `/api/auth_now` | ✅ | 立即触发校园网认证 |

### API 示例

```bash
TOKEN="你的API_Token"
ESP=192.168.1.42

# 查看状态
curl http://$ESP/api/status

# 开机到 Windows
curl -X POST http://$ESP/api/boot \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"os":"windows"}'

# 开机进入 BIOS
curl -X POST http://$ESP/api/boot \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"os":"bios"}'

# 关机
curl -X POST http://$ESP/api/shutdown \
  -H "Authorization: Bearer $TOKEN"

# 重启到 Fedora
curl -X POST http://$ESP/api/reboot_os \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"os":"fedora"}'

# 查看 OTA 状态
curl http://$ESP/api/update/info \
  -H "Authorization: Bearer $TOKEN"

# 上传新固件
curl -X POST http://$ESP/api/update \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/octet-stream" \
  --data-binary @build/remote_boot.bin

# 查看系统日志（最近 100 条）
curl "http://$ESP/api/log?count=100" \
  -H "Authorization: Bearer $TOKEN"
```

## 项目结构

```
ESP32-PC-RemoteBoot/
├── CMakeLists.txt              # 顶层 CMake
├── sdkconfig.defaults          # SDK 默认配置 (PSRAM, OTA, TinyUSB...)
├── partitions.csv              # Flash 分区表 (4MB, ota_0/ota_1)
├── main/
│   ├── main.c                  # 入口：依赖注入 & 启动流程
│   ├── Kconfig.projbuild       # menuconfig 配置项定义
│   ├── idf_component.yml       # 组件管理器依赖
│   ├── hal/                    # 硬件抽象层
│   │   ├── ihal.h              # 接口定义 (DIP)
│   │   ├── relay_controller.c  # 继电器驱动 (NVS 极性 + 防误触)
│   │   ├── post_detector.c     # HDD LED POST 检测
│   │   ├── usb_post_detector.c # USB 总线握手 POST 检测
│   │   └── usb_hid_keyboard.c  # USB HID 键盘 (TinyUSB)
│   ├── net/                    # 网络层
│   │   ├── wifi_manager.c      # WiFi STA 管理
│   │   ├── campus_net_auth.c   # 校园网 ePortal 认证
│   │   └── http_server.c       # REST API + 嵌入式文件服务
│   ├── app/                    # 应用层
│   │   ├── config_manager.c    # NVS 配置读写
│   │   ├── system_state.c      # 全局状态管理
│   │   ├── os_selector.c       # GRUB 键盘选择逻辑
│   │   ├── boot_manager.c      # 开机流程状态机
│   │   ├── ota_manager.c       # OTA 分区写入与状态查询
│   │   └── log_buffer.c        # PSRAM 环形日志缓冲
│   └── web/                    # Web UI（嵌入固件）
│       ├── index.html
│       ├── style.css
│       └── app.js
```

## 工作原理

```
┌──────────────────────────────────────────────────────┐
│                   Boot 流程示意                       │
│                                                      │
│  API /boot ──→ 继电器按下电源键 (200ms)              │
│            ──→ 等待 POST 完成 (USB 总线握手检测)      │
│            ──→ [GRUB 模式下] USB HID 发送方向键选 OS  │
│            ──→ 状态更新为 ONLINE (USB 枚举 = PC 在线) │
└──────────────────────────────────────────────────────┘
```

### 架构设计

采用**依赖反转原则 (DIP)**：所有应用层代码依赖 `ihal.h` 中定义的接口，不直接依赖硬件实现，方便测试和移植。

### 引导器双模式

| 模式 | NVS Key | POST 检测 | OS 选择 |
|---|---|---|---|
| Windows Boot Manager (默认) | `btldr_type=1` | USB 首次挂载 → 稳定时间 → 完成 | 无需，直接启动 |
| GRUB | `btldr_type=0` | USB 挂载→卸载→重挂载 (BIOS→GRUB 切换) | 键盘方向键 + 回车 |

### 日志系统

- 通过 `esp_log_set_vprintf()` 钩子捕获所有 ESP_LOG 输出到 PSRAM 环形缓冲
- 缓冲区大小自适应：80% 空闲 PSRAM（N8R2 ≈ 13000 条，N16R8 ≈ 52000 条）
- 校园网定时检查去噪：只在状态切换时记录（断→通 / 通→断）
- 串口输出不受影响，始终实时打印

## 配置参数一览

| 参数 | NVS Key | 默认值 | 说明 |
|---|---|---|---|
| WiFi SSID | `wifi_ssid` | — | WiFi 网络名 |
| WiFi 密码 | `wifi_pass` | — | WiFi 密码 |
| 校园网学号 | `campus_card` | — | ePortal 用户名 |
| 校园网密码 | `campus_pass` | — | ePortal 密码 |
| 引导器类型 | `btldr_type` | 1 | 0=GRUB, 1=Windows Boot Manager |
| 继电器极性 | `relay_pol` | 0 | 0=低电平触发, 1=高电平触发 |
| USB POST 稳定时间 | `post_settle` | 2000ms | 最后一次 USB 挂载后等待多久判定 POST 完成 |
| GRUB Windows 索引 | `grub_win_idx` | 0 | Windows 在 GRUB 中的位置 |
| GRUB Fedora 索引 | `grub_fed_idx` | 2 | Fedora 在 GRUB 中的位置 |
| GRUB 默认索引 | `grub_default` | 0 | GRUB 默认高亮项 |
| GRUB 等待时间 | `grub_wait_ms` | 3000ms | 发送按键前等待 GRUB 出现 |
| 连通检测间隔 | `ping_intv_s` | 60s | 互联网连通检查间隔 |
| HDD 静默阈值 | `hdd_quiet_ms` | 3000ms | HDD LED 安静多久判定 POST 完成 |

## License

MIT
