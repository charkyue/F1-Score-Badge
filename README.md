# F1 赛事记分牌 — ESP32-S3-Touch-AMOLED-1.75C

微雪 ESP32-S3 圆屏开发板的 F1 车手积分榜查看器。数据来自 [api.jolpi.ca](https://api.jolpi.ca)（Ergast 数据源，免费无需密钥）。

![硬件](https://img.shields.io/badge/板卡-ESP32--S3--Touch--AMOLED--1.75C-orange)

## 功能

- 按最新一站比赛后的车手总积分排序，左右滑动依次查看每位车手
- 每页显示：名次、年度总积分、姓名、车队、队标水印、最近一场比赛（大奖赛 / 赛道 / 国家）、该场得分
- 点击姓名 → 查看年度积分详情，再点击任意处返回
- 圆形屏幕上下边缘显示车队主题色圆弧色带，随车手切换变色
- 大图标操作：左侧 8 点钟位置「刷新」（衔尾箭头），右侧 4 点钟位置「设置 / 配网」（扳手）
- PWR 长键锁屏 / 亮屏（经 AXP2101 电源管理芯片检测，拔 USB 用电池供电也有效）
- 亮屏使用内存缓存秒开，不重新联网；数据 30 分钟自动更新，抓取失败每 5 分钟自动重试
- 首次开机或连不上 WiFi 自动进入配网模式（手机连 `F1-SCORE-SETUP`，浏览器打开 192.168.4.1）

## 硬件

| 项目 | 说明 |
|---|---|
| 开发板 | 微雪 Waveshare ESP32-S3-Touch-AMOLED-1.75C（240×240 圆屏 + 触摸 + AXP2101 电源管理） |
| 接线 | **无需接线**，USB-C 线直连电脑即可供电、烧录、看日志 |
| 按键 | 长键 = PWR（锁屏 / 亮屏），短键 = BOOT（烧录模式） |

## 文件说明（三个文件必须在同一文件夹）

| 文件 | 作用 |
|---|---|
| `F1Score.ino` | 主程序 |
| `logos.h` | 11 支车队官方队标 + F1 标志（240×240 RGB565 位图，约 5MB） |
| `fonts_custom.h` | DejaVu Sans Bold 实体字体（26/18/15/40px 四档） |

## 环境搭建（二选一）

### 方案 A：Arduino IDE（推荐，最简单）

1. 安装 [Arduino IDE 2.x](https://www.arduino.cc/en/software)（macOS 直接拖入应用程序）
2. 首选项（Preferences）→ 附加开发板管理器地址填入：`https://espressif.github.io/arduino-esp32/package_esp32_index.json`
3. 工具 → 开发板 → 开发板管理器 → 搜索 **esp32** → 安装 Espressif ESP32（3.x）
4. 工具 → 开发板 → 选择 **ESP32S3 Dev Module**
5. 库管理器（Library Manager）依次安装三个依赖：
   - `GFX Library for Arduino`（微雪版 1.6.4，官方 1.6.9 也可，程序已兼容）
   - `SensorLib`（触摸驱动）
   - `ArduinoJson`（7.x）

### 方案 B：VS Code（PlatformIO）

1. 安装 [VS Code](https://code.visualstudio.com/)，扩展商店安装 **PlatformIO IDE**
2. 新建工程 → 开发板选 `esp32-s3-devkitc-1`，框架选 Arduino
3. 把三个工程文件拷入 `src/`（`F1Score.ino` 改名为 `main.cpp` 或保留 ino 均可）
4. `platformio.ini` 关键配置：

```ini
[env:esp32-s3-devkitc-1]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino
board_build.arduino.memory_type = qio_opi   ; 开启 OPI PSRAM
board_build.partitions = huge_app.csv       ; 3MB 应用分区
build_flags =
    -DARDUINO_USB_CDC_ON_BOOT=1
lib_deps =
    mathertel/OneButton@^2.6.0
    ; GFX / SensorLib / ArduinoJson 也加入
```

## 编译设置（Arduino IDE，缺一不可）

- 开发板：**ESP32S3 Dev Module**
- **Partition Scheme → Huge APP (3MB)**（队标位图占约 1.5MB，默认分区装不下）
- **PSRAM → OPI PSRAM**（程序用 PSRAM 做整帧缓冲；漏选会在串口报 `FATAL: framebuffer alloc failed`）
- USB CDC On Boot → Enabled（串口看日志用）

## 烧录步骤

1. USB-C 线连接电脑与开发板
2. 工具 → 端口 → 选择 `/dev/cu.usbmodemXXXX`（macOS）
3. 点击上传（→ 箭头）；若提示端口占用或连接失败：按住 BOOT（短键）再插线，松开后重试
4. 上传完成后打开串口监视器（115200），看到 `WiFi OK` 即成功

## 首次使用

1. 上电后自动进入配网模式，屏幕显示 `F1-SCORE-SETUP`
2. 手机 / 电脑连接该 WiFi（无密码），浏览器打开 `http://192.168.4.1`
3. 选择家中 WiFi 并输入密码，保存后开发板自动重启并抓取数据
4. 配网信息保存在 Flash，之后开机自动连接；如需换网络，点右侧扳手图标重新配网

## 操作方式

| 操作 | 效果 |
|---|---|
| 左右滑动 | 切换车手（左滑 = 下一位） |
| 点击姓名区域 | 进入年度积分详情 |
| 点击详情页任意处 | 返回车手卡片 |
| 点击左侧刷新图标 | 立即刷新数据 |
| 点击右侧扳手图标 | 重新进入 WiFi 配网 |
| 按 PWR 长键 | 锁屏 / 亮屏 |

## 排障速查

| 现象 | 处理 |
|---|---|
| 编译报 framebuffer / alloc failed | PSRAM 没选 OPI PSRAM |
| 编译报空间不足 | Partition Scheme 没选 Huge APP |
| 显示 "Update failed" | API 服务器过载，5 分钟后自动重试，无需操作 |
| 烧录连不上（No serial data received） | 拔掉 USB 重插；不行就按住 BOOT 键插线再松开 |
| 文字显示乱码 / 缺字 | 字体高度必须为 4 的倍数；重音字符已折叠为 ASCII |
| 改了代码显示异常 | 串口监视器看启动日志（115200），留意 Touch OK / FB OK / WiFi OK |

⚠️ **不要在库管理器里升级 GFX 库后不做验证**——1.6.9+ 与 CO5300 屏存在小事务写入兼容问题，本程序已改用整帧推送方案规避；如果再升级库版本，建议先烧录验证显示是否正常。

## 更新日志

### v1.1（2026-10-25）

- 圆形屏幕上下车队色圆弧色带 + 大图标（左刷新衔尾箭头 / 右设置扳手，8 点 / 4 点钟位置）
- PWR 键经 AXP2101 PMIC 读取，拔 USB 电池供电下也能锁屏 / 亮屏
- 触摸芯片 X/Y 双轴 180° 翻转修复，左滑 = 下一位车手
- 亮屏改 displayOn() 快速唤醒 + 内存缓存重绘，不再重新联网
- UTF-8 重音字符折叠为 ASCII（Huelkenberg / Perez 显示修复）

## 数据来源与致谢

- 赛事数据：[api.jolpi.ca / Ergast Developer API](https://api.jolpi.ca)（免费，无需注册）
- 队标图案版权归各 F1 车队所有，仅作个人学习展示
- 字体：DejaVu Sans Bold（自由授权）

## License

MIT（队标与 F1 相关商标除外，其版权归 respective owners 所有）
