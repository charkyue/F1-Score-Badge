// ============================================================================
// F1 Score Board  ——  Waveshare ESP32-S3-Touch-AMOLED-1.75C（圆屏美化版）
//
// 功能：
//   1. 按最新一站后的车手总积分排序，左右滑动查看：姓名/车队色/队标水印/
//      最近一场（大奖赛、赛道、国家）/该场得分
//   2. 点击姓名 -> 年度积分详情；再点击任意处返回
//   3. WiFi 配网：首次开机或连不上 WiFi 自动进入配网模式；数据 30 分钟自动更新
//
// 圆屏适配：全部文字按 466 圆盘可视区域排布；队标水印 200x200 RGB565 预压暗
// 依赖：GFX_Library_for_Arduino（微雪 1.6.4）、SensorLib、ArduinoJson 7.x
//       + 同目录 logos.h（队标位图）
// 烧录：Partition Scheme 必须选 "Huge APP (3MB)"（队标位图占用约 1MB）
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include "Arduino_GFX_Library.h"
#include "TouchDrvCSTXXX.hpp"
#include <XPowersLib.h>
#include "logos.h"
#include "fonts_custom.h"

// ---------------- 板级引脚（与微雪 Mylibrary/pin_config.h 一致） ----------------
#ifndef LCD_SDIO0
#define LCD_SDIO0   4
#define LCD_SDIO1   5
#define LCD_SDIO2   6
#define LCD_SDIO3   7
#define LCD_SCLK    38
#define LCD_RESET   1
#define LCD_CS      12
#define LCD_WIDTH   466
#define LCD_HEIGHT  466
#define IIC_SDA     15
#define IIC_SCL     14
#define TP_INT      11
#define TP_RST      2
#endif

#define LOGO_SIZE         240
#define MAX_DRIVERS       24
#define REFRESH_INTERVAL  (30UL * 60UL * 1000UL)   // 有数据后：30 分钟自动更新
#define RETRY_INTERVAL    (5UL  * 60UL * 1000UL)   // 无数据/抓取失败：5 分钟重试
#define AP_SSID           "F1-SCORE-SETUP"

// 手势编码（用宏而非枚举：Arduino 自动生成的函数原型在文件顶部，枚举未声明）
#define G_NONE        0
#define G_TAP         1
#define G_SWIPE_LEFT  2
#define G_SWIPE_RIGHT 3

// ---------------- 数据源 ----------------
static const char *URL_STANDINGS = "https://api.jolpi.ca/ergast/f1/current/driverStandings.json";
static const char *URL_LAST_RACE = "https://api.jolpi.ca/ergast/f1/current/last/results.json";

// ---------------- 运行模式 ----------------
enum RunMode { MODE_CARD, MODE_DETAIL, MODE_SETUP };
RunMode mode = MODE_CARD;

// ---------------- 数据结构 ----------------
struct DriverInfo {
  char     driverId[24];
  char     name[32];
  char     team[24];
  uint16_t color;
  float    points;
  int      wins;
  int      position;
  float    lastPoints;    // -1 = 该站无成绩
  int      lastPos;
};
DriverInfo drivers[MAX_DRIVERS];
int  driverCount = 0;
int  currentIdx  = 0;

char raceName[48]    = "";
char circuitName[48] = "";
char countryName[32] = "";

// ---------------- 全局对象 ----------------
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, LCD_RESET, 0 /* rotation */, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);

TouchDrvCST92xx touch;
int16_t touchX[5], touchY[5];

Preferences prefs;
WebServer   server(80);

bool     touchDown   = false;
int16_t  startX = 0, startY = 0, lastX = 0, lastY = 0;
uint32_t touchStartMs = 0;

bool     hasData      = false;
bool     isFetching   = false;
uint32_t lastRefresh  = 0;

// ============================================================================
// 工具函数
// ============================================================================
static void fmtPoints(char *buf, size_t n, float v) {
  if (fabsf(v - roundf(v)) < 0.001f) snprintf(buf, n, "%ld", lroundf(v));
  else                               snprintf(buf, n, "%.1f", v);
}

// 车队主题色（按 constructorId 匹配）
static uint16_t teamColor(const char *id) {
  if (strstr(id, "mclaren"))              return 0xFC00;
  if (strstr(id, "ferrari"))              return 0xE805;
  if (strstr(id, "red_bull"))             return 0x3398;
  if (strstr(id, "mercedes"))             return 0x27BA;
  if (strstr(id, "aston"))                return 0x24CE;
  if (strstr(id, "alpine"))               return 0x0499;
  if (strstr(id, "williams"))             return 0x663F;
  if (strstr(id, "racing_bulls") ||
      strstr(id, "toro_rosso") ||
      strstr(id, "rb"))                   return 0x649F;
  if (strstr(id, "sauber") ||
      strstr(id, "audi"))                 return 0x570A;
  if (strstr(id, "haas"))                 return 0xB5D7;
  if (strstr(id, "cadillac"))             return 0xD6B2;
  return 0x7BEF;
}

// 车队显示名 -> 队标索引（与 logos.h 的 LOGO_TABLE 顺序一致）
static int logoForTeamName(const char *t) {
  if (strstr(t, "McLaren"))                                   return 0;
  if (strstr(t, "Ferrari"))                                   return 1;
  if (strstr(t, "Red Bull"))                                  return 2;
  if (strstr(t, "Mercedes"))                                  return 3;
  if (strstr(t, "Aston"))                                     return 4;
  if (strstr(t, "Alpine"))                                    return 5;
  if (strstr(t, "Williams"))                                  return 6;
  if (strstr(t, "Racing Bull") || strncmp(t, "RB", 2) == 0 ||
      strstr(t, "VCARB"))                                     return 7;
  if (strstr(t, "Audi") || strstr(t, "Sauber") ||
      strstr(t, "Kick"))                                      return 8;
  if (strstr(t, "Haas"))                                      return 9;
  if (strstr(t, "Cadillac"))                                  return 10;
  return 11;   // 兜底 F1
}

// ---------------- 字体绘制助手（默认内置点阵字体，最稳定路径） ----------------
static void drawTextD(const char *s, int16_t x, int16_t y, uint16_t color, uint8_t size) {
  gfx->setFont(nullptr);
  gfx->setTextSize(size);
  gfx->setTextColor(color);
  gfx->setCursor(x, y);
  gfx->print(s);
}
static uint16_t textWidthD(const char *s, uint8_t size) {
  return strlen(s) * 6 * size;
}
static void drawCenteredD(const char *s, int16_t y, uint16_t color, uint8_t size) {
  drawTextD(s, (LCD_WIDTH - (int)textWidthD(s, size)) / 2, y, color, size);
}
static void drawRightD(const char *s, int16_t rightX, int16_t y, uint16_t color, uint8_t size) {
  drawTextD(s, rightX - (int)textWidthD(s, size), y, color, size);
}

// ---------------- 实体字体助手（DejaVu Sans Bold，y = 基线，写入帧缓冲） ----------------
// UTF-8 → ASCII 折叠（字体仅含 ASCII）：ü→u、é→e、Ö→O 等；返回 0 表示跳过该字符
static char foldUtf8(const char *&p) {
  static const char lutHi[] = "AAAAAAACEEEEIIIIDNOOOOO?OUUUUY??";  // 0xC3 0x80-0x9F
  static const char lutLo[] = "aaaaaaaceeeeiiiidnooooo?ouuuuy?y";  // 0xC3 0xA0-0xBF
  uint8_t c = (uint8_t)*p;
  if (c < 0x80) { p++; return (char)c; }
  p++;
  if (c == 0xC3 && (uint8_t)*p >= 0x80 && (uint8_t)*p <= 0xBF) {
    uint8_t c2 = (uint8_t)*p++;
    return c2 < 0xA0 ? lutHi[c2 - 0x80] : lutLo[c2 - 0xA0];
  }
  if ((c & 0xE0) == 0xC0) { if (*p) p++; }          // 其他 2 字节序列
  else if ((c & 0xF0) == 0xE0) { p += 2; }          // 3 字节序列
  else if ((c & 0xF8) == 0xF0) { p += 3; }          // 4 字节序列
  return 0;
}
static uint16_t textWidthF(const GFXfont *f, const char *s) {
  uint16_t w = 0;
  char c;
  while ((c = foldUtf8(s)) != 0) {
    uint8_t uc = (uint8_t)c;
    if (uc < f->first || uc > f->last) continue;
    GFXglyph *g = &((GFXglyph *)f->glyph)[uc - f->first];
    w += pgm_read_byte(&g->xAdvance);
  }
  return w;
}
static void drawTextF(const GFXfont *f, const char *s, int16_t x, int16_t y, uint16_t color) {
  const uint8_t *bitmap = (const uint8_t *)f->bitmap;
  char c;
  while ((c = foldUtf8(s)) != 0) {
    uint8_t uc = (uint8_t)c;
    if (uc < f->first || uc > f->last) continue;
    GFXglyph *g = &((GFXglyph *)f->glyph)[uc - f->first];
    uint16_t bo = pgm_read_word(&g->bitmapOffset);
    uint8_t  w  = pgm_read_byte(&g->width);
    uint8_t  h  = pgm_read_byte(&g->height);
    int8_t   xo = pgm_read_sbyte(&g->xOffset);
    int8_t   yo = pgm_read_sbyte(&g->yOffset);
    uint8_t  xa = pgm_read_byte(&g->xAdvance);
    for (uint8_t yy = 0; yy < h; yy++) {
      for (uint8_t xx = 0; xx < w; xx++) {
        uint32_t bitpos = (uint32_t)bo * 8 + (uint32_t)yy * w + xx;
        if (pgm_read_byte(&bitmap[bitpos >> 3]) & (0x80 >> (bitpos & 7))) {
          fbPixel(x + xo + xx, y + yo + yy, color);
        }
      }
    }
    x += xa;
  }
}
static void drawCenteredF(const GFXfont *f, const char *s, int16_t baselineY, uint16_t color) {
  drawTextF(f, s, (LCD_WIDTH - (int)textWidthF(f, s)) / 2, baselineY, color);
}
static void drawRightF(const GFXfont *f, const char *s, int16_t rightX, int16_t baselineY, uint16_t color) {
  drawTextF(f, s, rightX - (int)textWidthF(f, s), baselineY, color);
}

// ============================================================================
// 帧缓冲绘制（PSRAM）：先全部画进内存，再一次 DMA 推送整帧
// 规避 CO5300 + GFX 1.6.9 小事务/混合事务的兼容问题
// ============================================================================
static uint16_t *fb = nullptr;
static bool fbAlloc() {
  fb = (uint16_t *)ps_malloc((size_t)LCD_WIDTH * LCD_HEIGHT * 2);
  if (!fb) fb = (uint16_t *)malloc((size_t)LCD_WIDTH * LCD_HEIGHT * 2);
  return fb != nullptr;
}
static inline void fbClear() { memset(fb, 0, (size_t)LCD_WIDTH * LCD_HEIGHT * 2); }
static inline void fbPixel(int16_t x, int16_t y, uint16_t color) {
  if (x < 0 || y < 0 || x >= LCD_WIDTH || y >= LCD_HEIGHT) return;
  fb[(uint32_t)y * LCD_WIDTH + x] = color;
}
static void fbFillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > LCD_WIDTH)  w = LCD_WIDTH - x;
  if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;
  if (w <= 0 || h <= 0) return;
  for (int16_t j = 0; j < h; j++) {
    uint16_t *row = &fb[(uint32_t)(y + j) * LCD_WIDTH + x];
    for (int16_t i = 0; i < w; i++) row[i] = color;
  }
}
static void fbBitmap(int16_t x, int16_t y, const uint16_t *bmp, int16_t w, int16_t h) {
  for (int16_t j = 0; j < h; j++) {
    if (y + j < 0 || y + j >= LCD_HEIGHT) continue;
    memcpy(&fb[(uint32_t)(y + j) * LCD_WIDTH + x], &bmp[(uint32_t)j * w], (size_t)w * 2);
  }
}
static void fbDisc(int16_t cx, int16_t cy, int16_t r, uint16_t color) {
  for (int16_t dy = -r; dy <= r; dy++) {
    int16_t dx = (int16_t)sqrtf((float)(r * r - dy * dy));
    fbFillRect(cx - dx, cy + dy, dx * 2 + 1, 1, color);
  }
}
static void fbCircle(int16_t cx, int16_t cy, int16_t r, uint16_t color) {
  int16_t x = r, y = 0, err = 0;
  while (x >= y) {
    fbPixel(cx + x, cy + y, color); fbPixel(cx + y, cy + x, color);
    fbPixel(cx - y, cy + x, color); fbPixel(cx - x, cy + y, color);
    fbPixel(cx - x, cy - y, color); fbPixel(cx - y, cy - x, color);
    fbPixel(cx + y, cy - x, color); fbPixel(cx + x, cy - y, color);
    y++;
    if (err <= 0) err += 2 * y + 1;
    if (err > 0) { x--; err -= 2 * x + 1; }
  }
}
static void fbTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                       int16_t x2, int16_t y2, uint16_t color) {
  // 扫描线填充：水平线 y 与三条边求交，取最左/最右交点画横线
  int16_t ymin = min(y0, min(y1, y2)), ymax = max(y0, max(y1, y2));
  for (int16_t y = ymin; y <= ymax; y++) {
    int16_t xs[3]; int n = 0;
    int16_t vx[3] = {x0, x1, x2}, vy[3] = {y0, y1, y2};
    for (int e = 0; e < 3; e++) {
      int16_t ya = vy[e], yb = vy[(e + 1) % 3];
      if (ya == yb) continue;
      if ((y >= min(ya, yb)) && (y <= max(ya, yb))) {
        xs[n++] = vx[e] + (int32_t)(y - ya) * (vx[(e + 1) % 3] - vx[e]) / (yb - ya);
      }
    }
    if (n >= 2) {
      int16_t xa = min(xs[0], xs[1]), xb = max(xs[0], xs[1]);
      if (n == 3) { xa = min(xa, xs[2]); xb = max(xb, xs[2]); }
      fbFillRect(xa, y, xb - xa + 1, 1, color);
    }
  }
}
// ---------------- PWR 键锁屏/亮屏（经 AXP2101 电源芯片读取按键，USB/电池供电均有效） ----------------
static XPowersAXP2101 PMU(Wire, IIC_SDA, IIC_SCL, AXP2101_SLAVE_ADDRESS);
static bool     screenLocked = false;
static bool     btnPressHandled = false;

static void flushFB() {
  if (screenLocked) return;   // 锁屏时只更新内存帧，不上屏
  gfx->draw16bitRGBBitmap(0, 0, fb, LCD_WIDTH, LCD_HEIGHT);
}

// 圆形屏幕边缘的圆弧色带（替代原来的上下直条）：环带 [222,233] ∩ ±45° 锥
static void fbEdgeArc(bool top, uint16_t color) {
  const int16_t cx = LCD_WIDTH / 2, cy = LCD_HEIGHT / 2;
  const int32_t ro2 = 233L * 233, ri2 = 222L * 222;
  int16_t y0 = top ? 0 : cy + 1;
  int16_t y1 = top ? cy : LCD_HEIGHT;
  for (int16_t y = y0; y < y1; y++) {
    int32_t dy = y - cy;
    for (int16_t x = 0; x < LCD_WIDTH; x++) {
      int32_t dx = x - cx;
      int32_t r2 = dx * dx + dy * dy;
      if (r2 < ri2 || r2 > ro2) continue;
      if ((dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy)) continue;  // 限制 ±45°
      fbPixel(x, y, color);
    }
  }
}

// ---------------- PWR 键锁屏/亮屏 ----------------
static void toggleLock() {
  screenLocked = !screenLocked;
  if (screenLocked) {
    gfx->displayOff();
    Serial.println("screen locked");
  } else {
    gfx->displayOn();            // 快速唤醒，不走完整初始化
    gfx->setBrightness(128);
    Serial.println("screen unlocked");
    if (mode == MODE_SETUP)       drawSetupScreen();
    else if (mode == MODE_DETAIL) drawDetail();
    else if (hasData)             drawCard();
    else                          drawMessage("F1 SCORE BOARD", "Updating...", nullptr);
  }
}
// PWR 键经 PMIC 读取：按下=下降沿中断，松开=上升沿中断，无需消抖
static void pollPowerButton() {
  uint64_t irq = PMU.getIrqStatus();
  if (!irq) return;
  PMU.clearIrqStatus();
  if (irq & XPOWERS_AXP2101_PKEY_NEGATIVE_IRQ) {   // 按下
    if (!btnPressHandled) { btnPressHandled = true; toggleLock(); }
  }
  if (irq & XPOWERS_AXP2101_PKEY_POSITIVE_IRQ) {   // 松开
    btnPressHandled = false;
  }
}

// ============================================================================
// 屏幕绘制（全部坐标已按 466 圆盘可视区域校准）
// ============================================================================
static void drawWatermark(const char *team) {
  const uint16_t *logo = LOGO_TABLE[logoForTeamName(team)];
  fbBitmap((LCD_WIDTH - LOGO_SIZE) / 2, (LCD_HEIGHT - LOGO_SIZE) / 2 + 12,
           logo, LOGO_SIZE, LOGO_SIZE);
}

// 左下 8 点钟：刷新 = 圆形衔尾箭头；右下 4 点钟：设置 = 扳手
static void drawRefreshIcon(int16_t cx, int16_t cy, uint16_t color) {
  const int16_t r = 13;
  // 顺时针圆弧 30°~330°（右侧留缺口），2px 粗
  for (int16_t a = 30; a <= 330; a += 2) {
    float rad = a * PI / 180.0f;
    fbPixel(cx + (int16_t)(cosf(rad) * r),         cy + (int16_t)(sinf(rad) * r),         color);
    fbPixel(cx + (int16_t)(cosf(rad) * (r - 1)),   cy + (int16_t)(sinf(rad) * (r - 1)),   color);
  }
  // 箭头：终点在 330°，尖端沿顺时针切向伸出
  float ea = 330.0f * PI / 180.0f;
  int16_t ex = cx + (int16_t)(cosf(ea) * r);
  int16_t ey = cy + (int16_t)(sinf(ea) * r);
  float tx = -sinf(ea), ty = cosf(ea);    // 顺时针切向
  float rx =  cosf(ea), ry = sinf(ea);    // 径向
  fbTriangle(ex + (int16_t)(tx * 9), ey + (int16_t)(ty * 9),
              ex + (int16_t)(rx * 5), ey + (int16_t)(ry * 5),
              ex - (int16_t)(rx * 5), ey - (int16_t)(ry * 5), color);
}
static void drawWrenchIcon(int16_t cx, int16_t cy, uint16_t color) {
  // 手柄：45° 斜向圆点串（左下 → 右上）
  for (int i = 0; i <= 8; i++) {
    float t = i / 8.0f;
    fbDisc(cx - 11 + (int16_t)(t * 22), cy + 11 - (int16_t)(t * 22), 4, color);
  }
  // 大钳口：右上端圆环，切出朝右上的 C 形缺口
  int16_t jx = cx + 13, jy = cy - 13;
  fbCircle(jx, jy, 6, color);
  fbFillRect(jx + 2, jy - 9, 8, 8, RGB565_BLACK);
  // 尾端：左下小圆环（镂空）
  fbDisc(cx - 13, cy + 13, 4, color);
  fbDisc(cx - 13, cy + 13, 2, RGB565_BLACK);
}

static void drawMessage(const char *line1, const char *line2, const char *line3) {
  fbClear();
  drawCenteredF(&FONT_MID,  line1, 216, RGB565_WHITE);
  if (line2) drawCenteredF(&FONT_TEAM, line2, 254, RGB565_LIGHTGREY);
  if (line3) drawCenteredF(&FONT_TEAM, line3, 278, RGB565_DARKGREY);
  flushFB();
}

static void drawCard() {
  if (driverCount == 0 || !fb) return;
  const DriverInfo &d = drivers[currentIdx];
  char buf[32];

  fbClear();

  // 车队色圆弧（贴圆屏边缘，替代原来的直条）
  fbEdgeArc(true, d.color);
  fbEdgeArc(false, d.color);

  // 队标水印（衬在文字下层）
  drawWatermark(d.team);

  // 名次（左上）+ 年度总积分（右上）——内移避开圆弧两端
  snprintf(buf, sizeof(buf), "P%d", d.position);
  drawTextF(&FONT_MID, buf, 108, 78, RGB565_WHITE);
  fmtPoints(buf, sizeof(buf), d.points);
  char pts[32]; snprintf(pts, sizeof(pts), "%s PTS", buf);
  drawRightF(&FONT_MID, pts, 360, 78, d.color);

  // 车手姓名（基线 140，字形 123..147，完全在圆盘内；点击热区 y80..155 不变）
  drawCenteredF(&FONT_NAME, d.name, 140, RGB565_WHITE);

  // 车队名
  drawCenteredF(&FONT_TEAM, d.team, 174, RGB565_LIGHTGREY);

  // 分隔线
  fbFillRect(150, 196, LCD_WIDTH - 300, 2, RGB565_DARKGREY);

  // 最近一场比赛
  drawCenteredF(&FONT_TEAM, "LAST RACE", 224, d.color);
  drawCenteredF(&FONT_MID, raceName, 250, RGB565_WHITE);
  drawCenteredF(&FONT_TEAM, circuitName, 276, RGB565_LIGHTGREY);
  drawCenteredF(&FONT_TEAM, countryName, 298, RGB565_DARKGREY);

  // 该场得分
  if (d.lastPoints >= 0) {
    fmtPoints(buf, sizeof(buf), d.lastPoints);
    char plus[20]; snprintf(plus, sizeof(plus), "+%s", buf);
    drawCenteredF(&FONT_NUM, plus, 340, d.color);
    snprintf(buf, sizeof(buf), "Finished P%d", d.lastPos);
    drawCenteredF(&FONT_TEAM, buf, 372, RGB565_DARKGREY);
  } else {
    drawCenteredF(&FONT_NUM, "--", 340, RGB565_DARKGREY);
    drawCenteredF(&FONT_TEAM, "No result in this race", 372, RGB565_DARKGREY);
  }

  // 页面指示点（23 点，y=396，与图标错开）
  for (int i = 0; i < driverCount; i++) {
    int16_t px = LCD_WIDTH / 2 - driverCount * 5 + i * 10;
    fbDisc(px, 396, 3, i == currentIdx ? d.color : RGB565_DARKGREY);
  }

  // 两个操作图标：画面左=刷新（用户视角左），画面右=设置（用户视角右）
  drawRefreshIcon(72, 326, RGB565_LIGHTGREY);
  drawWrenchIcon(394, 326, RGB565_LIGHTGREY);

  flushFB();
}

static void drawDetail() {
  if (driverCount == 0 || !fb) return;
  const DriverInfo &d = drivers[currentIdx];
  char buf[32];

  fbClear();
  fbEdgeArc(true, d.color);
  fbEdgeArc(false, d.color);

  drawWatermark(d.team);

  drawCenteredF(&FONT_TEAM, "DRIVER DETAIL", 84, RGB565_DARKGREY);
  drawCenteredF(&FONT_NAME, d.name, 138, RGB565_WHITE);
  drawCenteredF(&FONT_TEAM, d.team, 174, d.color);

  fmtPoints(buf, sizeof(buf), d.points);
  drawCenteredF(&FONT_NUM, buf, 250, RGB565_WHITE);
  drawCenteredF(&FONT_TEAM, "CHAMPIONSHIP POINTS", 282, RGB565_LIGHTGREY);

  snprintf(buf, sizeof(buf), "Position  P%d", d.position);
  drawCenteredF(&FONT_MID, buf, 326, RGB565_WHITE);
  snprintf(buf, sizeof(buf), "Wins  %d", d.wins);
  drawCenteredF(&FONT_MID, buf, 354, RGB565_WHITE);
  if (d.lastPoints >= 0) {
    char tmp[16]; fmtPoints(tmp, sizeof(tmp), d.lastPoints);
    snprintf(buf, sizeof(buf), "Last race  +%s", tmp);
    drawCenteredF(&FONT_MID, buf, 382, RGB565_WHITE);
  }

  drawCenteredF(&FONT_TEAM, "tap anywhere to go back", 412, RGB565_DARKGREY);

  flushFB();
}

// ============================================================================
// 网络：WiFi 连接 / 数据抓取
// ============================================================================
static bool connectWiFi(uint32_t timeoutSec) {
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  if (ssid.length() == 0) return false;

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.printf("Connecting to %s", ssid.c_str());

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutSec * 1000UL) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi OK, IP=%s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  Serial.println("WiFi connect failed");
  return false;
}

static bool httpGet(const char *url, String &payload) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(25);
  HTTPClient http;
  http.setTimeout(25000);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  bool ok = (code == 200);
  if (ok) payload = http.getString();
  else    Serial.printf("HTTP %d for %s\n", code, url);
  http.end();
  return ok;
}

static bool fetchStandings() {
  String payload;
  if (!httpGet(URL_STANDINGS, payload)) return false;

  JsonDocument filter;
  filter["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"][0]["position"] = true;
  filter["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"][0]["points"] = true;
  filter["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"][0]["wins"] = true;
  filter["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"][0]["Driver"]["driverId"] = true;
  filter["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"][0]["Driver"]["givenName"] = true;
  filter["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"][0]["Driver"]["familyName"] = true;
  filter["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"][0]["Constructors"][0]["name"] = true;
  filter["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"][0]["Constructors"][0]["constructorId"] = true;

  JsonDocument doc;
  if (deserializeJson(doc, payload, DeserializationOption::Filter(filter))) return false;

  JsonArray arr = doc["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"];
  int n = 0;
  for (JsonObject s : arr) {
    if (n >= MAX_DRIVERS) break;
    DriverInfo &d = drivers[n];
    memset(&d, 0, sizeof(d));

    const char *given  = s["Driver"]["givenName"]  | "";
    const char *family = s["Driver"]["familyName"] | "";
    snprintf(d.name, sizeof(d.name), "%s %s", given, family);
    strlcpy(d.driverId, s["Driver"]["driverId"] | "", sizeof(d.driverId));

    // 一辆车手赛季内可能换队，取最后一个即当前车队
    JsonArray cons = s["Constructors"];
    JsonObject cur = cons[cons.size() ? cons.size() - 1 : 0];
    strlcpy(d.team, cur["name"] | "?", sizeof(d.team));
    const char *cid = cur["constructorId"] | "";
    d.color = teamColor(cid);
    Serial.printf("driver %d: %-22s team=%-28s id=%s\n", n, d.name, d.team, cid);

    d.points   = s["points"].as<float>();
    d.wins     = s["wins"] | 0;
    d.position = s["position"] | (n + 1);
    d.lastPoints = -1;   // 待与上一场成绩匹配
    d.lastPos    = 0;
    n++;
  }
  driverCount = n;
  return driverCount > 0;
}

static bool fetchLastRace() {
  String payload;
  if (!httpGet(URL_LAST_RACE, payload)) return false;

  JsonDocument filter;
  filter["MRData"]["RaceTable"]["Races"][0]["raceName"] = true;
  filter["MRData"]["RaceTable"]["Races"][0]["Circuit"]["circuitName"] = true;
  filter["MRData"]["RaceTable"]["Races"][0]["Circuit"]["Location"]["country"] = true;
  filter["MRData"]["RaceTable"]["Races"][0]["Results"][0]["position"] = true;
  filter["MRData"]["RaceTable"]["Races"][0]["Results"][0]["points"] = true;
  filter["MRData"]["RaceTable"]["Races"][0]["Results"][0]["Driver"]["driverId"] = true;

  JsonDocument doc;
  if (deserializeJson(doc, payload, DeserializationOption::Filter(filter))) return false;

  JsonObject race = doc["MRData"]["RaceTable"]["Races"][0];
  strlcpy(raceName,    race["raceName"] | "Unknown GP", sizeof(raceName));
  strlcpy(circuitName, race["Circuit"]["circuitName"] | "", sizeof(circuitName));
  strlcpy(countryName, race["Circuit"]["Location"]["country"] | "", sizeof(countryName));

  for (JsonObject r : race["Results"].as<JsonArray>()) {
    const char *id = r["Driver"]["driverId"] | "";
    for (int i = 0; i < driverCount; i++) {
      if (strcmp(drivers[i].driverId, id) == 0) {
        drivers[i].lastPoints = r["points"].as<float>();
        drivers[i].lastPos    = r["position"] | 0;
        break;
      }
    }
  }
  return true;
}

static void fetchData() {
  if (isFetching) return;
  isFetching = true;
  drawMessage("Updating data...", WiFi.status() == WL_CONNECTED ? "" : "Connecting WiFi...", nullptr);

  if (WiFi.status() != WL_CONNECTED && !connectWiFi(15)) {
    drawMessage("WiFi disconnected", "Check router or tap gear", "to reconfigure network");
    lastRefresh = millis();   // 失败退避，避免主循环里无限重试
    isFetching = false;
    return;
  }

  if (!fetchStandings()) {
    drawMessage("Update failed", "API busy or unreachable", "Auto retry in 5 min");
    lastRefresh = millis();
    isFetching = false;
    return;
  }
  fetchLastRace();

  hasData     = true;
  lastRefresh = millis();
  currentIdx  = 0;
  mode        = MODE_CARD;
  drawCard();
  isFetching = false;
}

// ============================================================================
// 配网模式
// ============================================================================
static const char PORTAL_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>F1 Score Setup</title></head>
<body style="font-family:sans-serif;max-width:420px;margin:40px auto;">
<h2>F1 Score Board - WiFi Setup</h2>
<form method="POST" action="/save">
  <p>WiFi Name (SSID)<br><input name="ssid" style="width:100%;padding:8px;" required></p>
  <p>Password<br><input name="pass" type="password" style="width:100%;padding:8px;"></p>
  <button style="padding:10px 24px;">Save & Restart</button>
</form>
</body></html>)rawliteral";

static void enterSetupMode() {
  mode = MODE_SETUP;
  WiFi.disconnect(true);
  delay(200);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID);
  IPAddress ip = WiFi.softAPIP();

  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", PORTAL_HTML); });
  server.on("/save", HTTP_POST, []() {
    String ssid = server.arg("ssid");
    String pass = server.arg("pass");
    if (ssid.length() > 0) {
      prefs.putString("ssid", ssid);
      prefs.putString("pass", pass);
      server.send(200, "text/html", "<h3>Saved. Restarting...</h3>");
      delay(800);
      ESP.restart();
    } else {
      server.send(200, "text/html", "<h3>SSID empty. <a href='/'>Back</a></h3>");
    }
  });
  server.begin();

  drawSetupScreen();
}

static void drawSetupScreen() {
  fbClear();
  fbEdgeArc(true, 0x07E0);
  fbEdgeArc(false, 0x07E0);
  drawCenteredF(&FONT_MID,  "WIFI SETUP", 120, RGB565_WHITE);
  drawCenteredF(&FONT_TEAM, "1. Connect your phone to:", 176, RGB565_LIGHTGREY);
  drawCenteredF(&FONT_MID,  AP_SSID, 210, 0x07E0);
  drawCenteredF(&FONT_TEAM, "2. Open in browser:", 258, RGB565_LIGHTGREY);
  drawCenteredF(&FONT_MID,  "192.168.4.1", 292, 0x07E0);
  drawCenteredF(&FONT_TEAM, "3. Enter WiFi & save", 340, RGB565_DARKGREY);
  flushFB();
}

// ============================================================================
// 触摸手势
// ============================================================================
static int readGesture() {
  uint8_t n = touch.getPoint(touchX, touchY, 1);
  // 触摸芯片相对屏幕旋转 180°：X、Y 两轴都要翻转
  if (n > 0) {
    touchX[0] = LCD_WIDTH - 1 - touchX[0];
    touchY[0] = LCD_HEIGHT - 1 - touchY[0];
  }
  if (n > 0) {
    if (!touchDown) {
      touchDown = true;
      startX = lastX = touchX[0];
      startY = lastY = touchY[0];
      touchStartMs = millis();
    } else {
      lastX = touchX[0];
      lastY = touchY[0];
    }
    return G_NONE;
  }
  if (!touchDown) return G_NONE;
  touchDown = false;

  int16_t dx = lastX - startX, dy = lastY - startY;
  uint32_t dt = millis() - touchStartMs;
  if (abs(dx) > 70 && abs(dx) > 2 * abs(dy)) return dx > 0 ? G_SWIPE_RIGHT : G_SWIPE_LEFT;
  if (dt < 400 && abs(dx) < 25 && abs(dy) < 25) return G_TAP;
  return G_NONE;
}

static void handleTap(int16_t x, int16_t y) {
  if (mode == MODE_DETAIL) {
    mode = MODE_CARD;
    drawCard();
    return;
  }
  if (mode != MODE_CARD) return;

  if (x < 136 && y > 285 && y < 380) { fetchData(); return; }        // 刷新（画面左侧）
  if (x > 330 && y > 285 && y < 380) { enterSetupMode(); return; }   // 设置（画面右侧）
  if (y > 80 && y < 155)  { mode = MODE_DETAIL; drawDetail(); }  // 姓名热区
}

// ============================================================================
// setup / loop
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(200);

  Wire.begin(IIC_SDA, IIC_SCL);

  pinMode(TP_RST, OUTPUT);
  digitalWrite(TP_RST, LOW);
  delay(30);
  digitalWrite(TP_RST, HIGH);
  delay(50);
  touch.setPins(TP_RST, TP_INT);
  while (!touch.begin(Wire, 0x5A, IIC_SDA, IIC_SCL)) {
    Serial.println("Touch init failed, retrying...");
    delay(500);
  }
  Serial.println("Touch OK");

  // PWR 键：接在 AXP2101 电源芯片的 PWRON 引脚上，经 I2C 读取按键中断（USB/电池均有效）
  if (PMU.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
    PMU.clearIrqStatus();
    PMU.enableIRQ(XPOWERS_AXP2101_PKEY_NEGATIVE_IRQ | XPOWERS_AXP2101_PKEY_POSITIVE_IRQ);
    Serial.println("PMU OK");
  } else {
    Serial.println("PMU init failed");
  }

  if (!gfx->begin()) Serial.println("gfx->begin() failed!");
  gfx->fillScreen(RGB565_BLACK);
  gfx->setBrightness(128);

  if (!fbAlloc()) {
    Serial.println("FATAL: framebuffer alloc failed (need PSRAM=OPI)");
    while (1) delay(1000);
  }
  Serial.printf("FB OK (%u bytes)\n", (unsigned)((size_t)LCD_WIDTH * LCD_HEIGHT * 2));

  prefs.begin("f1cfg", false);
  String ssid = prefs.getString("ssid", "");

  if (ssid.length() == 0) {
    enterSetupMode();
  } else {
    drawMessage("F1 SCORE BOARD", "Connecting WiFi...", nullptr);
    if (connectWiFi(15)) {
      fetchData();
    } else {
      enterSetupMode();
    }
  }
}

void loop() {
  pollPowerButton();                 // 锁屏/亮屏随时响应（含配网模式）

  if (mode == MODE_SETUP) {
    server.handleClient();
    delay(10);
    return;
  }

  if (screenLocked) {                // 锁屏：忽略触摸与定时刷新显示
    delay(10);
    return;
  }

  switch (readGesture()) {
    case G_SWIPE_LEFT:
      if (mode == MODE_CARD && hasData) {
        currentIdx = (currentIdx + 1) % driverCount;
        drawCard();
      }
      break;
    case G_SWIPE_RIGHT:
      if (mode == MODE_CARD && hasData) {
        currentIdx = (currentIdx - 1 + driverCount) % driverCount;
        drawCard();
      }
      break;
    case G_TAP:
      handleTap(lastX, lastY);
      break;
    default:
      break;
  }

  uint32_t interval = hasData ? REFRESH_INTERVAL : RETRY_INTERVAL;
  if (!isFetching && millis() - lastRefresh > interval) fetchData();

  delay(10);
}
