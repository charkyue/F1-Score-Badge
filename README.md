# F1 赛事记分牌 — ESP32-S3-Touch-AMOLED-1.75C

微雪 ESP32-S3 圆屏开发板的 F1 车手积分榜查看器。数据来自 api.jolpi.ca（Ergast 数据源）。

## 功能

- 按最新一站比赛后的车手总积分排序，左右滑动依次查看每位车手
- 每页显示：名次、年度总积分、姓名、车队、队标水印、最近一场比赛（大奖赛 / 赛道 / 国家）、该场得分
- 点击姓名 → 查看年度积分详情，再点击任意处返回
- 首次开机或连不上 WiFi 自动进入配网模式（手机连 `F1-SCORE-SETUP`，浏览器打开 192.168.4.1）
- 数据 30 分钟自动更新；抓取失败每 5 分钟自动重试（"Update failed" 属正常现象，稍后会自动恢复）

## 文件说明（三个文件必须在同一文件夹）

| 文件 | 作用 |
|---|---|
| `F1Score.ino` | 主程序 |
| `logos.h` | 11 支车队官方队标 + F1 标志（240×240 RGB565 位图，约 5MB） |
| `fonts_custom.h` | DejaVu Sans Bold 实体字体（26/18/15/40px 四档） |

## Arduino IDE 编译设置（缺一不可）

- 开发板：**ESP32S3 Dev Module**
- **Partition Scheme → Huge APP (3MB)**（队标位图占约 1.5MB，默认分区装不下）
- **PSRAM → OPI PSRAM**（程序用 PSRAM 做整帧缓冲；漏选会在串口报 `FATAL: framebuffer alloc failed`）
- USB CDC On Boot → Enabled（串口看日志用）

## 依赖库

- GFX Library for Arduino（微雪版 1.6.4，或官方 1.6.9 也可，程序已兼容）
- SensorLib（触摸）
- ArduinoJson 7.x

⚠️ **不要在库管理器里升级 GFX 库后不做验证**——1.6.9+ 与 CO5300 屏存在小事务写入兼容问题，本程序已改用整帧推送方案规避，但如果再升级库版本，建议先烧录验证显示是否正常。

## 操作方式

| 操作 | 效果 |
|---|---|
| 左右滑动 | 切换车手 |
| 点击姓名区域 | 进入年度积分详情 |
| 点击详情页任意处 | 返回车手卡片 |
| 点击左下图标 | 立即刷新数据 |
| 点击右下齿轮 | 重新进入 WiFi 配网 |

## 排障速查

| 现象 | 处理 |
|---|---|
| 编译报Framebuffer / alloc failed | PSRAM 没选 OPI PSRAM |
| 编译报空间不足 | Partition Scheme 没选 Huge APP |
| 显示 "Update failed" | API 服务器过载，5 分钟后自动重试，无需操作 |
| 烧录连不上（No serial data received） | 拔掉 USB 重插；不行就按住 BOOT 键插线再松开 |
| 改了代码显示异常 | 串口监视器看启动日志（115200），留意 Touch OK / FB OK / WiFi OK |
