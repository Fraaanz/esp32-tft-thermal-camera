#include <Wire.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <Adafruit_MLX90640.h>

#include "button_bottom_left__default.h"
#include "button_bottom_left__pressed.h"
#include "button_bottom_right__default.h"
#include "button_bottom_right__pressed.h"
#include "button_color__default.h"
#include "button_color__pressed.h"
#include "button_post_processing__default.h"
#include "button_post_processing__pressed.h"

// ===== TFT =====
TFT_eSPI tft = TFT_eSPI();

// ===== Palette Stops Helper =====
struct Stop { float p; uint8_t r, g, b; };

static inline uint16_t colorFromStops(float x, const Stop* s, int n) {
  if (x <= s[0].p) return tft.color565(s[0].r, s[0].g, s[0].b);
  if (x >= s[n - 1].p) return tft.color565(s[n - 1].r, s[n - 1].g, s[n - 1].b);

  int i = 0;
  while (i < n - 1 && x > s[i + 1].p) i++;

  float tt = (x - s[i].p) / (s[i + 1].p - s[i].p);
  uint8_t r = (uint8_t)(s[i].r + tt * (s[i + 1].r - s[i].r));
  uint8_t g = (uint8_t)(s[i].g + tt * (s[i + 1].g - s[i].g));
  uint8_t b = (uint8_t)(s[i].b + tt * (s[i + 1].b - s[i].b));
  return tft.color565(r, g, b);
}

// ===== MLX90640 =====
Adafruit_MLX90640 mlx;

static float frame[32 * 24];
static float filtered[32 * 24];
static bool  filteredInit = false;

// I2C Pins
#define I2C_SDA 22
#define I2C_SCL 21

// ===== Layout =====
static const int MARGIN    = 16;
static const int CONTENT_W = 288;

static const int IMG_W = 288;
static const int IMG_H = 192;

static const int TEMP_BAR_Y = 276;
static const int TEMP_BAR_H = 12;

static const int HIST_Y = 304;
static const int HIST_H = 64;
static const int HIST_W = 286;
static const int HIST_X = MARGIN + 1;

static const int SCALE_Y = 376;
static const int SCALE_H = 32;

static const int TOP_BTN_Y = 0;
static const int BOT_BTN_Y = 480 - 48;

int screenW, screenH;
int contentX;
int imgX, imgY;
int scaleX, scaleY;

int btnCS_X, btnCS_Y;
int btnPP_X, btnPP_Y;

int btnBL_X, btnBL_Y;
int btnBR_X, btnBR_Y;

// ===== UI Colors =====
uint16_t HIST_COLOR;
uint16_t UI_DARK;
uint16_t UI_DIM;
uint16_t UI_WHITE;
uint16_t UI_BLACK;

// ===== Touch Calibration (Rotation 0) =====
const uint16_t RAW_X_MIN = 0;
const uint16_t RAW_X_MAX = 319;
const uint16_t RAW_Y_MIN = 0;
const uint16_t RAW_Y_MAX = 471;

int32_t map_clamped(int32_t x, int32_t in_min, int32_t in_max,
                    int32_t out_min, int32_t out_max) {
  if (x < in_min) x = in_min;
  if (x > in_max) x = in_max;
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

static inline bool hitRect(int x, int y, int rx, int ry, int rw, int rh) {
  return (x >= rx && x < rx + rw && y >= ry && y < ry + rh);
}

// ===== Color Modes =====
enum ColorMode {
  COLOR_INFERNO   = 0,
  COLOR_VIRIDIS   = 1,
  COLOR_WHITE_HOT = 2
};
ColorMode colorMode = COLOR_INFERNO;

// ===== Focus Modes =====
static const uint8_t FOCUS_ENVIRONMENT = 0;
static const uint8_t FOCUS_CENTER      = 1;
static const uint8_t FOCUS_HOTCOLD     = 2;

uint8_t focusMode = FOCUS_ENVIRONMENT;

// Hot/Cold Markers (Sensor Coordinates)
int hotSX = 0, hotSY = 0;
int coldSX = 0, coldSY = 0;

static const int FOCUS_W = 16;
static const int FOCUS_H = 16;
int focusX, focusY;

// ===== Filter Options =====
bool optTemporalSmooth = true;
bool optRangeSmooth    = false;
bool optBilinear       = false;

// tMin/tMax smoothing
bool  rangeInit  = false;
float tMinFilt   = 0.0f;
float tMaxFilt   = 0.0f;

// Hold stats
bool  holdInit = false;
float holdMin  = 0.0f;
float holdMax  = 0.0f;

// Hot/Cold live values
float liveMinHC   = 0.0f;
float liveMaxHC   = 0.0f;
bool  liveHCValid = false;

// Live average
float liveAvgRaw = 0.0f;

// Scale redraw cache
static uint8_t   scaleFrameDiv  = 0;
static bool      scaleInit      = false;
static float     lastScaleMin   = 0.0f;
static float     lastScaleMax   = 0.0f;
static ColorMode lastScaleMode  = COLOR_INFERNO;
static const float SCALE_REDRAW_EPS = 0.3f;

static inline void resetHoldStats() {
  holdInit = false;
  holdMin = holdMax = 0.0f;
}

// ===== Histogram Live Cache =====
static const int HIST_BINS = 72;
static uint8_t histPrevH[HIST_BINS] = {0};
static bool    histPrevInit = false;

// Sensor ROI (Ignore Edge Pixels)
static const int ROI_BORDER = 1;
static const int SX_MIN = ROI_BORDER;
static const int SX_MAX = 31 - ROI_BORDER;
static const int SY_MIN = ROI_BORDER;
static const int SY_MAX = 23 - ROI_BORDER;

static inline void resetHistogramCache() {
  memset(histPrevH, 0, sizeof(histPrevH));
  histPrevInit = false;
}

// ===== UI State =====
enum UiScreen { SCREEN_MAIN, SCREEN_FILTER_MENU, SCREEN_COLOR_MENU };
UiScreen uiScreen = SCREEN_MAIN;

// ===== Menu States =====
bool menuTS = false;
bool menutS = false;
bool menuBU = false;
ColorMode menuColor = COLOR_INFERNO;

static inline int clampi(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static inline int getScaleY() {
  return (focusMode == FOCUS_ENVIRONMENT) ? SCALE_Y : 340;
}

static inline void clearScaleAreaAt(int y) {
  tft.fillRect(scaleX, y, CONTENT_W, SCALE_H, TFT_BLACK);
}

static inline void clearScaleAreasBoth() {
  clearScaleAreaAt(340);
  clearScaleAreaAt(SCALE_Y);
}

// =====================================================
//  Rounded Mask Helper
// =====================================================
static inline void maskRoundedRectCorners(int x, int y, int w, int h, int r, uint16_t bg) {
  if (r <= 0) return;
  if (r * 2 > w) r = w / 2;
  if (r * 2 > h) r = h / 2;

  const int rr = r * r;

  for (int dy = 0; dy < r; dy++) {
    for (int dx = 0; dx < r; dx++) {
      int px = dx - (r - 1);
      int py = dy - (r - 1);
      if (px * px + py * py > rr) tft.drawPixel(x + dx, y + dy, bg);
    }
  }
  for (int dy = 0; dy < r; dy++) {
    for (int dx = 0; dx < r; dx++) {
      int px = dx;
      int py = dy - (r - 1);
      if (px * px + py * py > rr) tft.drawPixel(x + (w - r) + dx, y + dy, bg);
    }
  }
  for (int dy = 0; dy < r; dy++) {
    for (int dx = 0; dx < r; dx++) {
      int px = dx - (r - 1);
      int py = dy;
      if (px * px + py * py > rr) tft.drawPixel(x + dx, y + (h - r) + dy, bg);
    }
  }
  for (int dy = 0; dy < r; dy++) {
    for (int dx = 0; dx < r; dx++) {
      int px = dx;
      int py = dy;
      if (px * px + py * py > rr) tft.drawPixel(x + (w - r) + dx, y + (h - r) + dy, bg);
    }
  }
}

// =====================================================
//  Button Draw (Assets)
// =====================================================
static inline void pushAsset(int x, int y, const uint16_t* data, int w, int h) {
  tft.pushImage(x, y, w, h, (uint16_t*)data);
}

void drawTopButtons(bool csPressed = false, bool ppPressed = false) {
  pushAsset(
    btnCS_X, btnCS_Y,
    csPressed ? button_color__pressed : button_color__default,
    button_color__default_width, button_color__default_height
  );

  pushAsset(
    btnPP_X, btnPP_Y,
    ppPressed ? button_post_processing__pressed : button_post_processing__default,
    button_post_processing__default_width, button_post_processing__default_height
  );
}

void drawBottomButtons(bool leftPressed = false, bool rightPressed = false) {
  pushAsset(
    btnBL_X, btnBL_Y,
    leftPressed ? button_bottom_left__pressed : button_bottom_left__default,
    button_bottom_left__default_width, button_bottom_left__default_height
  );

  pushAsset(
    btnBR_X, btnBR_Y,
    rightPressed ? button_bottom_right__pressed : button_bottom_right__default,
    button_bottom_right__default_width, button_bottom_right__default_height
  );

  tft.setTextFont(2);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(UI_BLACK, UI_DIM);
}

// =====================================================
//  Focus Label
// =====================================================
void drawFocusLabel() {
  const char* modeStr =
    (focusMode == FOCUS_ENVIRONMENT) ? "Environment" :
    (focusMode == FOCUS_CENTER)      ? "Center" :
                                       "Hot/Cold";

  const int gap = 6;

  tft.setTextFont(2);

  int y = 32;
  int h = 18;

  int clearLeft  = btnPP_X + button_post_processing__default_width + 8;
  int clearRight = screenW - 16;
  if (clearRight > clearLeft) {
    tft.fillRect(clearLeft, y - 3, clearRight - clearLeft, h + 6, TFT_BLACK);
  }

  int wMode  = tft.textWidth(modeStr, 2);
  int wFocus = tft.textWidth("Focus", 2);

  int rightX = screenW - 16;
  int xMode  = rightX - wMode;
  int xFocus = xMode - gap - wFocus;

  tft.setTextDatum(TL_DATUM);

  tft.setTextColor(UI_DIM, TFT_BLACK);
  tft.drawString("Focus", xFocus, y);

  tft.setTextColor(UI_WHITE, TFT_BLACK);
  tft.drawString(modeStr, xMode, y);
}

void updateHeader() {
  drawTopButtons(false, false);
  drawFocusLabel();
}

// =====================================================
//  Palettes
// =====================================================
uint16_t colorWhiteHot(float x) {
  uint8_t v = (uint8_t)(x * 255);
  return tft.color565(v, v, v);
}

uint16_t colorInferno(float x) {
  static const Stop S[] = {
    {0.00f, 0x00, 0x00, 0x04},
    {0.10f, 0x1B, 0x0C, 0x41},
    {0.25f, 0x4A, 0x0C, 0x6B},
    {0.45f, 0x78, 0x1C, 0x6D},
    {0.65f, 0xB4, 0x3C, 0x4E},
    {0.80f, 0xE1, 0x64, 0x2A},
    {0.92f, 0xF6, 0xA4, 0x1A},
    {1.00f, 0xFC, 0xFF, 0xD6},
  };
  return colorFromStops(x, S, sizeof(S) / sizeof(S[0]));
}

uint16_t colorViridis(float x) {
  static const Stop S[] = {
    {0.00f, 0x44, 0x01, 0x54},
    {0.12f, 0x48, 0x1A, 0x6C},
    {0.28f, 0x3B, 0x52, 0x8B},
    {0.45f, 0x2C, 0x7A, 0x8E},
    {0.62f, 0x21, 0xA5, 0x85},
    {0.78f, 0x5E, 0xC9, 0x62},
    {0.90f, 0xB5, 0xDE, 0x2B},
    {1.00f, 0xFD, 0xE7, 0x25},
  };
  return colorFromStops(x, S, sizeof(S) / sizeof(S[0]));
}

uint16_t tempToColor(float t, float tMin, float tMax) {
  if (tMax <= tMin) tMax = tMin + 1.0f;

  float x = (t - tMin) / (tMax - tMin);
  if (x < 0.0f) x = 0.0f;
  if (x > 1.0f) x = 1.0f;

  switch (colorMode) {
    case COLOR_WHITE_HOT: return colorWhiteHot(x);
    case COLOR_VIRIDIS:   return colorViridis(x);
    case COLOR_INFERNO:
    default:              return colorInferno(x);
  }
}

// =====================================================
//  Temperature Bar
// =====================================================
void drawTempHoldBar(float vLeft, float vMid, float vRight,
                     const char* lLeft, const char* lMid, const char* lRight) {
  const int clearY = TEMP_BAR_Y - 6;
  const int clearH = 24;
  tft.fillRect(contentX, clearY, CONTENT_W, clearH, TFT_BLACK);

  tft.setTextFont(2);

  char bL[12], bM[12], bR[12];
  snprintf(bL, sizeof(bL), "%.0fC", vLeft);
  snprintf(bM, sizeof(bM), "%.0fC", vMid);
  snprintf(bR, sizeof(bR), "%.0fC", vRight);

  const int y = TEMP_BAR_Y + (TEMP_BAR_H / 2) + 1;

  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(UI_DIM, TFT_BLACK);
  tft.drawString(lLeft, contentX, y);

  int wL = tft.textWidth(lLeft, 2);
  tft.setTextColor(UI_WHITE, TFT_BLACK);
  tft.drawString(bL, contentX + wL, y);

  int wLM = tft.textWidth(lMid, 2);
  int wVM = tft.textWidth(bM, 2);
  int totalM = wLM + wVM;
  int xM = contentX + (CONTENT_W / 2) - (totalM / 2);

  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(UI_DIM, TFT_BLACK);
  tft.drawString(lMid, xM, y);
  tft.setTextColor(UI_WHITE, TFT_BLACK);
  tft.drawString(bM, xM + wLM, y);

  int wLR = tft.textWidth(lRight, 2);
  int wVR = tft.textWidth(bR, 2);
  int totalR = wLR + wVR;
  int xR = contentX + CONTENT_W - totalR;

  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(UI_DIM, TFT_BLACK);
  tft.drawString(lRight, xR, y);
  tft.setTextColor(UI_WHITE, TFT_BLACK);
  tft.drawString(bR, xR + wLR, y);
}

// =====================================================
//  Histogram
// =====================================================
void drawHistogramLive_Optimized(float* img, float tMin, float tMax) {
  uint16_t bins[HIST_BINS] = {0};

  float range = (tMax - tMin);
  if (range < 0.0001f) range = 0.0001f;
  const float inv = (float)(HIST_BINS - 1) / range;

  for (int sy = SY_MIN; sy <= SY_MAX; sy++) {
    for (int sx = SX_MIN; sx <= SX_MAX; sx++) {
      float v = img[sy * 32 + sx];
      int b = (int)((v - tMin) * inv);
      if (b < 0) b = 0;
      else if (b >= HIST_BINS) b = HIST_BINS - 1;
      bins[b]++;
    }
  }

  uint16_t maxCount = 1;
  for (int b = 0; b < HIST_BINS; b++) if (bins[b] > maxCount) maxCount = bins[b];

  if (!histPrevInit) {
    tft.fillRect(HIST_X, HIST_Y, HIST_W, HIST_H, TFT_BLACK);
    histPrevInit = true;
  }

  for (int b = 0; b < HIST_BINS; b++) {
    int newH = (int)((uint32_t)bins[b] * HIST_H / maxCount);
    if (bins[b] > 0 && newH < 1) newH = 1;
    if (newH > HIST_H) newH = HIST_H;

    int oldH = histPrevH[b];
    if (newH == oldH) continue;

    int x = HIST_X + b * 4;

    if (newH > oldH) {
      int addH = newH - oldH;
      int y = HIST_Y + (HIST_H - newH);
      tft.fillRect(x, y, 2, addH, HIST_COLOR);
    } else {
      int cutH = oldH - newH;
      int y = HIST_Y + (HIST_H - oldH);
      tft.fillRect(x, y, 2, cutH, TFT_BLACK);
    }

    histPrevH[b] = (uint8_t)newH;
  }
}

void clearHistogramArea() {
  tft.fillRect(HIST_X, HIST_Y, HIST_W, HIST_H, TFT_BLACK);
  resetHistogramCache();
}

// =====================================================
//  Color Scale
// =====================================================
void drawTempScaleHorizontal(float tMin, float tMax) {
  int scaleYdyn = getScaleY();
  const int x0 = scaleX;
  const int y0 = scaleYdyn + 2;
  const int w  = CONTENT_W;
  const int h  = 8;
  const int r  = 6;

  if (tMax <= tMin) tMax = tMin + 1.0f;

  clearScaleAreaAt(scaleYdyn);

  for (int x = 0; x < w; x++) {
    float ratio = (float)x / (float)(w - 1);
    float tt = tMin + ratio * (tMax - tMin);
    uint16_t c = tempToColor(tt, tMin, tMax);

    int yStart = 0;
    int yEnd   = h - 1;

    if (x < r) {
      int dx = (r - 1) - x;
      int inside = (r - 1) * (r - 1) - dx * dx;
      if (inside < 0) continue;
      int dy = (int)(sqrtf((float)inside) + 0.5f);
      yStart = (r - 1) - dy;
      yEnd   = (h - 1) - ((r - 1) - dy);
    } else if (x >= w - r) {
      int dx = x - (w - r);
      int inside = (r - 1) * (r - 1) - dx * dx;
      if (inside < 0) continue;
      int dy = (int)(sqrtf((float)inside) + 0.5f);
      yStart = (r - 1) - dy;
      yEnd   = (h - 1) - ((r - 1) - dy);
    }

    tft.drawFastVLine(x0 + x, y0 + yStart, (yEnd - yStart + 1), c);
  }

  float tMid = (tMin + tMax) * 0.5f;

  char bMin[16], bMid[16], bMax[16];
  snprintf(bMin, sizeof(bMin), "%.0fC", tMin);
  snprintf(bMid, sizeof(bMid), "%.0fC", tMid);
  snprintf(bMax, sizeof(bMax), "%.0fC", tMax);

  tft.setTextFont(2);
  tft.setTextColor(UI_WHITE, TFT_BLACK);

  const int textY = scaleYdyn + 22;
  tft.fillRect(x0, scaleYdyn + 14, w, 18, TFT_BLACK);

  tft.setTextDatum(ML_DATUM);
  tft.drawString(bMin, x0, textY);

  tft.setTextDatum(MC_DATUM);
  tft.drawString(bMid, x0 + w / 2, textY);

  tft.setTextDatum(MR_DATUM);
  tft.drawString(bMax, x0 + w, textY);
}

// =====================================================
//  Focus Overlays
// =====================================================
void drawFocusOverlayIfNeeded() {
  if (focusMode != FOCUS_CENTER) return;

  tft.drawRect(focusX, focusY, FOCUS_W, FOCUS_H, TFT_WHITE);
  if (FOCUS_W > 2 && FOCUS_H > 2) {
    tft.drawRect(focusX + 1, focusY + 1, FOCUS_W - 2, FOCUS_H - 2, TFT_BLACK);
  }
}

static inline void drawCrosshairDouble(int cx, int cy, int halfLen, uint16_t outer, uint16_t inner) {
  tft.drawLine(cx - halfLen, cy - 1, cx + halfLen, cy - 1, outer);
  tft.drawLine(cx - halfLen, cy,     cx + halfLen, cy,     outer);
  tft.drawLine(cx - halfLen, cy + 1, cx + halfLen, cy + 1, outer);

  tft.drawLine(cx - 1, cy - halfLen, cx - 1, cy + halfLen, outer);
  tft.drawLine(cx,     cy - halfLen, cx,     cy + halfLen, outer);
  tft.drawLine(cx + 1, cy - halfLen, cx + 1, cy + halfLen, outer);

  tft.drawLine(cx - halfLen, cy, cx + halfLen, cy, inner);
  tft.drawLine(cx, cy - halfLen, cx, cy + halfLen, inner);
}

void drawHotColdOverlayIfNeeded() {
  if (focusMode != FOCUS_HOTCOLD) return;

  const int scaleXpx = IMG_W / 32;
  const int scaleYpx = IMG_H / 24;

  int hotX  = imgX + (31 - hotSX)  * scaleXpx + scaleXpx / 2;
  int hotY  = imgY + hotSY         * scaleYpx + scaleYpx / 2;

  int coldX = imgX + (31 - coldSX) * scaleXpx + scaleXpx / 2;
  int coldY = imgY + coldSY        * scaleYpx + scaleYpx / 2;

  const int HALF = 12;

  drawCrosshairDouble(hotX,  hotY,  HALF, TFT_BLACK, TFT_WHITE);
  drawCrosshairDouble(coldX, coldY, HALF, TFT_BLACK, TFT_WHITE);
}

// =====================================================
//  Draw Thermal Image
// =====================================================
void drawImageNearest(float* img, float tMin, float tMax) {
  const int scaleXpx = IMG_W / 32;
  const int scaleYpx = IMG_H / 24;

  int gy1 = (focusY + FOCUS_H - 1 - imgY) / scaleYpx;
  if (gy1 > 23) gy1 = 23;

  for (int y = 0; y < 24; y++) {
    int py = imgY + y * scaleYpx;

    for (int x = 0; x < 32; x++) {
      float tt = img[y * 32 + (31 - x)];
      uint16_t color = tempToColor(tt, tMin, tMax);
      int px = imgX + x * scaleXpx;
      tft.fillRect(px, py, scaleXpx, scaleYpx, color);
    }

    if (focusMode == FOCUS_CENTER && y == gy1) {
      drawFocusOverlayIfNeeded();
      drawHotColdOverlayIfNeeded();
    }

    if (y == 0) {
      maskRoundedRectCorners(imgX, imgY, IMG_W, IMG_H, 8, TFT_BLACK);
      drawFocusOverlayIfNeeded();
      drawHotColdOverlayIfNeeded();
    }
  }

  drawFocusOverlayIfNeeded();
  drawHotColdOverlayIfNeeded();
  maskRoundedRectCorners(imgX, imgY, IMG_W, IMG_H, 8, TFT_BLACK);

  tft.drawFastHLine(imgX, imgY + IMG_H,     IMG_W, TFT_BLACK);
  tft.drawFastHLine(imgX, imgY + IMG_H + 1, IMG_W, TFT_BLACK);
}

void drawImageBilinear(float* img, float tMin, float tMax) {
  int focusYEnd = focusY + FOCUS_H - 1;

  for (int yy = 0; yy < IMG_H; yy++) {
    float sy = (float)yy * (24.0f - 1.0f) / (IMG_H - 1);
    int y0 = (int)sy;
    int y1 = y0 + 1;
    if (y1 > 23) y1 = 23;
    float fy = sy - y0;

    for (int xx = 0; xx < IMG_W; xx++) {
      float sxNormal = (float)xx * (32.0f - 1.0f) / (IMG_W - 1);
      float sx = 31.0f - sxNormal;
      int x0 = (int)sx;
      int x1 = x0 + 1;
      if (x1 > 31) x1 = 31;
      float fx = sx - x0;

      float f00 = img[y0 * 32 + x0];
      float f10 = img[y0 * 32 + x1];
      float f01 = img[y1 * 32 + x0];
      float f11 = img[y1 * 32 + x1];

      float tt = f00 * (1 - fx) * (1 - fy)
               + f10 * fx       * (1 - fy)
               + f01 * (1 - fx) * fy
               + f11 * fx       * fy;

      uint16_t color = tempToColor(tt, tMin, tMax);
      tft.drawPixel(imgX + xx, imgY + yy, color);
    }

    if (focusMode == FOCUS_CENTER && (imgY + yy) == focusYEnd) {
      drawFocusOverlayIfNeeded();
      drawHotColdOverlayIfNeeded();
    }

    if (yy == 7) {
      maskRoundedRectCorners(imgX, imgY, IMG_W, IMG_H, 8, TFT_BLACK);
      drawFocusOverlayIfNeeded();
      drawHotColdOverlayIfNeeded();
    }
  }

  drawFocusOverlayIfNeeded();
  drawHotColdOverlayIfNeeded();
  maskRoundedRectCorners(imgX, imgY, IMG_W, IMG_H, 8, TFT_BLACK);

  tft.drawFastHLine(imgX, imgY + IMG_H,     IMG_W, TFT_BLACK);
  tft.drawFastHLine(imgX, imgY + IMG_H + 1, IMG_W, TFT_BLACK);
}

// =====================================================
//  Menus
// =====================================================
void drawHeadline(const char* title) {
  tft.setTextFont(2);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(UI_WHITE, TFT_BLACK);
  tft.drawString(title, 16, 20);
}

void drawPillStyled(int x, int y, int w, int h, const char* label, bool active) {
  uint16_t fill = active ? UI_DIM : UI_DARK;
  uint16_t txt  = active ? UI_BLACK : UI_DIM;

  tft.fillRoundRect(x, y, w, h, 16, fill);
  tft.setTextFont(2);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(txt, fill);
  tft.drawString(label, x + 16, y + h / 2);
}

void drawBottomButton(int x, int y, int w, int h, const char* label) {
  tft.fillRoundRect(x, y, w, h, 16, UI_DIM);
  tft.setTextFont(2);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(UI_BLACK, UI_DIM);
  tft.drawString(label, x + w / 2, y + h / 2);
}

const int FP_TS_X = 16,  FP_TS_Y = 48,  FP_TS_W = 288, FP_TS_H = 48;
const int FP_tS_X = 16,  FP_tS_Y = 112, FP_tS_W = 288, FP_tS_H = 48;
const int FP_BU_X = 16,  FP_BU_Y = 176, FP_BU_W = 288, FP_BU_H = 48;
const int FB_RST_X = 16, FB_RST_Y = 416, FB_RST_W = 136, FB_RST_H = 48;
const int FB_OK_X  = 168, FB_OK_Y = 416, FB_OK_W  = 136, FB_OK_H  = 48;

void drawFilterMenu() {
  tft.fillScreen(TFT_BLACK);
  drawHeadline("Post-Processing");
  drawPillStyled(FP_TS_X, FP_TS_Y, FP_TS_W, FP_TS_H, "Temporal Smoothing", menuTS);
  drawPillStyled(FP_tS_X, FP_tS_Y, FP_tS_W, FP_tS_H, "temperature-Scale Smoothing", menutS);
  drawPillStyled(FP_BU_X, FP_BU_Y, FP_BU_W, FP_BU_H, "bilinear Upscaling", menuBU);
  drawBottomButton(FB_RST_X, FB_RST_Y, FB_RST_W, FB_RST_H, "Reset");
  drawBottomButton(FB_OK_X,  FB_OK_Y,  FB_OK_W,  FB_OK_H,  "Confirm");
}

void openFilterMenu() {
  menuTS = optTemporalSmooth;
  menutS = optRangeSmooth;
  menuBU = optBilinear;
  uiScreen = SCREEN_FILTER_MENU;
  drawFilterMenu();
}

void closeFilterMenu_apply(bool apply) {
  if (apply) {
    bool prevTS = optTemporalSmooth;
    bool prevtS = optRangeSmooth;

    optTemporalSmooth = menuTS;
    optRangeSmooth    = menutS;
    optBilinear       = menuBU;

    if (prevTS != optTemporalSmooth) filteredInit = false;
    if (prevtS != optRangeSmooth)    rangeInit = false;
  }

  uiScreen = SCREEN_MAIN;
  tft.fillScreen(TFT_BLACK);

  updateHeader();
  drawBottomButtons(false, false);

  drawTempHoldBar(holdInit ? holdMin : 0, holdInit ? holdMax : 0, liveAvgRaw,
                  "Min-Hold ", "Max-Hold ", "Average ");
  clearHistogramArea();
  scaleInit = false;
  clearScaleAreasBoth();
}

const int CP_WH_X = 16,  CP_WH_Y = 48,  CP_WH_W = 288, CP_WH_H = 48;
const int CP_IH_X = 16,  CP_IH_Y = 112, CP_IH_W = 288, CP_IH_H = 48;
const int CB_OK_X = 168, CB_OK_Y = 416, CB_OK_W = 136, CB_OK_H = 48;

void drawColorMenu() {
  tft.fillScreen(TFT_BLACK);
  drawHeadline("Color Scale");
  
  drawPillStyled(16,  48, 288, 48, "Inferno",           (menuColor == COLOR_INFERNO));
  drawPillStyled(16, 112, 288, 48, "Viridis",           (menuColor == COLOR_VIRIDIS));
  drawPillStyled(16, 176, 288, 48, "Grayscale (white)", (menuColor == COLOR_WHITE_HOT));

  drawBottomButton(168, 416, 136, 48, "Confirm");
}

void openColorMenu() {
  menuColor = colorMode;
  uiScreen = SCREEN_COLOR_MENU;
  drawColorMenu();
}

void closeColorMenu_apply(bool apply) {
  if (apply) colorMode = menuColor;

  uiScreen = SCREEN_MAIN;
  tft.fillScreen(TFT_BLACK);

  updateHeader();
  drawBottomButtons(false, false);

  drawTempHoldBar(holdInit ? holdMin : 0, holdInit ? holdMax : 0, liveAvgRaw,
                  "Min-Hold ", "Max-Hold ", "Average ");
  clearHistogramArea();
  scaleInit = false;
  clearScaleAreasBoth();
}

// =====================================================
//  Main Static
// =====================================================
void drawMainStatic() {
  tft.fillScreen(TFT_BLACK);

  updateHeader();
  drawBottomButtons(false, false);

  drawTempHoldBar(holdInit ? holdMin : 0, holdInit ? holdMax : 0, liveAvgRaw,
                  "Min-Hold ", "Max-Hold ", "Average ");
  clearHistogramArea();
  scaleInit = false;
  clearScaleAreasBoth();
}

// =====================================================
//  Focus Switch
// =====================================================
void setFocusMode(uint8_t m) {
  if (focusMode == m) return;

  clearHistogramArea();
  clearScaleAreasBoth();
  scaleInit = false;

  focusMode = m;

  resetHoldStats();

  if (focusMode == FOCUS_HOTCOLD && liveHCValid) {
    drawTempHoldBar(liveMinHC, liveMaxHC, liveAvgRaw, "Coldest ", "Hottest ", "Average ");
  } else {
    drawTempHoldBar(0, 0, liveAvgRaw, "Min-Hold ", "Max-Hold ", "Average ");
  }

  updateHeader();
  drawBottomButtons(false, false);
}

// =====================================================
//  Touch
// =====================================================
void handleTouchMain(int x, int y) {
  if (hitRect(x, y, btnCS_X, btnCS_Y, button_color__default_width, button_color__default_height)) {
    drawTopButtons(true, false);
    delay(60);
    openColorMenu();
    return;
  }

  if (hitRect(x, y, btnPP_X, btnPP_Y, button_post_processing__default_width, button_post_processing__default_height)) {
    drawTopButtons(false, true);
    delay(60);
    openFilterMenu();
    return;
  }

  if (hitRect(x, y, btnBL_X, btnBL_Y, button_bottom_left__default_width, button_bottom_left__default_height)) {
    drawBottomButtons(true, false);
    delay(60);
    resetHoldStats();
    drawTempHoldBar(0, 0, liveAvgRaw, "Min-Hold ", "Max-Hold ", "Average ");
    drawBottomButtons(false, false);
    return;
  }

  if (hitRect(x, y, btnBR_X, btnBR_Y, button_bottom_right__default_width, button_bottom_right__default_height)) {
    drawBottomButtons(false, true);
    delay(60);

    uint8_t next =
      (focusMode == FOCUS_ENVIRONMENT) ? FOCUS_CENTER :
      (focusMode == FOCUS_CENTER)      ? FOCUS_HOTCOLD :
                                         FOCUS_ENVIRONMENT;

    setFocusMode(next);
    drawBottomButtons(false, false);
    return;
  }
}

void handleTouchFilterMenu(int x, int y) {
  if (hitRect(x, y, FP_TS_X, FP_TS_Y, FP_TS_W, FP_TS_H)) {
    menuTS = !menuTS;
    drawPillStyled(FP_TS_X, FP_TS_Y, FP_TS_W, FP_TS_H, "Temporal Smoothing", menuTS);
    return;
  }
  if (hitRect(x, y, FP_tS_X, FP_tS_Y, FP_tS_W, FP_tS_H)) {
    menutS = !menutS;
    drawPillStyled(FP_tS_X, FP_tS_Y, FP_tS_W, FP_tS_H, "temperature-Scale Smoothing", menutS);
    return;
  }
  if (hitRect(x, y, FP_BU_X, FP_BU_Y, FP_BU_W, FP_BU_H)) {
    menuBU = !menuBU;
    drawPillStyled(FP_BU_X, FP_BU_Y, FP_BU_W, FP_BU_H, "bilinear Upscaling", menuBU);
    return;
  }
  if (hitRect(x, y, FB_RST_X, FB_RST_Y, FB_RST_W, FB_RST_H)) {
    menuTS = false; menutS = false; menuBU = false;
    drawFilterMenu();
    return;
  }
  if (hitRect(x, y, FB_OK_X, FB_OK_Y, FB_OK_W, FB_OK_H)) {
    closeFilterMenu_apply(true);
    return;
  }
}

void handleTouchColorMenu(int x, int y) {
  if (hitRect(x, y, 16,  48, 288, 48)) { menuColor = COLOR_INFERNO;   drawColorMenu(); return; }
  if (hitRect(x, y, 16, 112, 288, 48)) { menuColor = COLOR_VIRIDIS;   drawColorMenu(); return; }
  if (hitRect(x, y, 16, 176, 288, 48)) { menuColor = COLOR_WHITE_HOT; drawColorMenu(); return; }

  if (hitRect(x, y, CB_OK_X, CB_OK_Y, CB_OK_W, CB_OK_H)) { closeColorMenu_apply(true); return; }
}

void handleTouch() {
  uint16_t rawX, rawY;
  bool pressed = tft.getTouch(&rawX, &rawY);
  if (!pressed) return;

  uint16_t x = map_clamped(rawY, RAW_Y_MIN, RAW_Y_MAX, 0, screenW - 1);
  uint16_t y = map_clamped(rawX, RAW_X_MIN, RAW_X_MAX, 0, screenH - 1);

  if (uiScreen == SCREEN_MAIN) handleTouchMain(x, y);
  else if (uiScreen == SCREEN_FILTER_MENU) handleTouchFilterMenu(x, y);
  else handleTouchColorMenu(x, y);
}

// =====================================================
//  Setup
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  tft.init();
  tft.setRotation(0);

#ifdef TFT_BL
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
#endif

  screenW = tft.width();
  screenH = tft.height();

  contentX = MARGIN;

  imgX = contentX;
  imgY = 72;

  scaleX = contentX;
  scaleY = SCALE_Y;

  focusX = imgX + (IMG_W - FOCUS_W) / 2;
  focusY = imgY + (IMG_H - FOCUS_H) / 2;

  UI_DARK  = tft.color565(0x30, 0x2E, 0x38);
  UI_DIM   = tft.color565(0x92, 0x8F, 0xA3);
  UI_WHITE = tft.color565(0xFF, 0xFF, 0xFF);
  UI_BLACK = tft.color565(0x00, 0x00, 0x00);

  HIST_COLOR = tft.color565(0x5F, 0x5C, 0x70);

  btnCS_X = 16;
  btnCS_Y = TOP_BTN_Y;

  btnPP_X = btnCS_X + button_color__default_width + 8;
  btnPP_Y = TOP_BTN_Y;

  btnBL_X = 16;
  btnBL_Y = BOT_BTN_Y;

  btnBR_X = screenW - 16 - button_bottom_right__default_width;
  btnBR_Y = BOT_BTN_Y;

  resetHoldStats();
  liveAvgRaw = 0.0f;
  drawMainStatic();

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);

  if (!mlx.begin(0x33, &Wire)) {
    tft.fillScreen(TFT_RED);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_RED);
    tft.drawString("MLX MISSING!", screenW / 2, screenH / 2);
    while (1) delay(1000);
  }

  mlx.setMode(MLX90640_CHESS);
  mlx.setResolution(MLX90640_ADC_18BIT);
  mlx.setRefreshRate(MLX90640_16_HZ);
}

// =====================================================
//  Loop
// =====================================================
void loop() {
  handleTouch();

  if (uiScreen != SCREEN_MAIN) {
    delay(10);
    return;
  }

  static uint8_t histFrameDiv = 0;
  static uint8_t tempBarDiv   = 0;

  int status = mlx.getFrame(frame);
  if (status != 0) {
    delay(10);
    return;
  }

  float* img = frame;
  if (optTemporalSmooth) {
    const float alpha = 0.4f;
    if (!filteredInit) {
      for (int i = 0; i < 32 * 24; i++) filtered[i] = frame[i];
      filteredInit = true;
    } else {
      for (int i = 0; i < 32 * 24; i++) {
        filtered[i] = filtered[i] * (1.0f - alpha) + frame[i] * alpha;
      }
    }
    img = filtered;
  }

  float envMinRaw = 0, envMaxRaw = 0, envSum = 0;
  bool firstEnv = true;
  int envN = 0;

  for (int sy = SY_MIN; sy <= SY_MAX; sy++) {
    for (int sx = SX_MIN; sx <= SX_MAX; sx++) {
      float v = img[sy * 32 + sx];
      if (firstEnv) { envMinRaw = envMaxRaw = v; firstEnv = false; }
      else {
        if (v < envMinRaw) envMinRaw = v;
        if (v > envMaxRaw) envMaxRaw = v;
      }
      envSum += v;
      envN++;
    }
  }
  float envAvgRaw = (envN > 0) ? (envSum / envN) : 0.0f;

  if (focusMode == FOCUS_HOTCOLD) {
    float vMin = 0, vMax = 0;
    bool first = true;
    int iMin = 0, iMax = 0;

    for (int sy = SY_MIN; sy <= SY_MAX; sy++) {
      for (int sx = SX_MIN; sx <= SX_MAX; sx++) {
        int i = sy * 32 + sx;
        float v = img[i];
        if (first) { vMin = vMax = v; iMin = iMax = i; first = false; }
        else {
          if (v < vMin) { vMin = v; iMin = i; }
          if (v > vMax) { vMax = v; iMax = i; }
        }
      }
    }

    coldSX = iMin % 32; coldSY = iMin / 32;
    hotSX  = iMax % 32; hotSY  = iMax / 32;

    liveMinHC = vMin;
    liveMaxHC = vMax;
    liveHCValid = true;
  } else {
    liveHCValid = false;
  }

  float holdMinRaw = envMinRaw;
  float holdMaxRaw = envMaxRaw;

  if (focusMode == FOCUS_CENTER) {
    const int scaleXpx = IMG_W / 32;
    const int scaleYpx = IMG_H / 24;

    int gx0 = (focusX - imgX) / scaleXpx;
    int gx1 = (focusX + FOCUS_W - 1 - imgX) / scaleXpx;
    int gy0 = (focusY - imgY) / scaleYpx;
    int gy1 = (focusY + FOCUS_H - 1 - imgY) / scaleYpx;

    gx0 = clampi(gx0, SX_MIN, SX_MAX);
    gx1 = clampi(gx1, SX_MIN, SX_MAX);
    gy0 = clampi(gy0, SY_MIN, SY_MAX);
    gy1 = clampi(gy1, SY_MIN, SY_MAX);

    if (gx1 < gx0 || gy1 < gy0) {
      holdMinRaw = envMinRaw;
      holdMaxRaw = envMaxRaw;
      liveAvgRaw = envAvgRaw;
    } else {
      int sx0 = 31 - gx1;
      int sx1 = 31 - gx0;

      float sum = 0.0f;
      bool first = true;
      int n = 0;

      for (int sy = gy0; sy <= gy1; sy++) {
        for (int sx = sx0; sx <= sx1; sx++) {
          float v = img[sy * 32 + sx];
          if (first) { holdMinRaw = holdMaxRaw = v; first = false; }
          else {
            if (v < holdMinRaw) holdMinRaw = v;
            if (v > holdMaxRaw) holdMaxRaw = v;
          }
          sum += v;
          n++;
        }
      }
      liveAvgRaw = (n > 0) ? (sum / n) : envAvgRaw;
    }
  } else {
    liveAvgRaw = envAvgRaw;
  }

  if (!holdInit) {
    holdInit = true;
    holdMin = holdMinRaw;
    holdMax = holdMaxRaw;
  } else {
    if (holdMinRaw < holdMin) holdMin = holdMinRaw;
    if (holdMaxRaw > holdMax) holdMax = holdMaxRaw;
  }

  float tMin = envMinRaw;
  float tMax = envMaxRaw;

  if (optRangeSmooth) {
    const float beta = 0.2f;
    if (!rangeInit) {
      tMinFilt = envMinRaw;
      tMaxFilt = envMaxRaw;
      rangeInit = true;
    } else {
      tMinFilt = tMinFilt * (1.0f - beta) + envMinRaw * beta;
      tMaxFilt = tMaxFilt * (1.0f - beta) + envMaxRaw * beta;
    }
    tMin = tMinFilt;
    tMax = tMaxFilt;
  } else {
    rangeInit = false;
  }

  float padding = (tMax - tMin) * 0.05f;
  tMin -= padding;
  tMax += padding;

  if (optBilinear) drawImageBilinear(img, tMin, tMax);
  else            drawImageNearest(img, tMin, tMax);

  scaleFrameDiv++;
  if (scaleFrameDiv >= 3) {
    scaleFrameDiv = 0;

    bool modeChanged  = (!scaleInit) || (colorMode != lastScaleMode);
    bool rangeChanged = (!scaleInit) ||
                        (fabsf(tMin - lastScaleMin) > SCALE_REDRAW_EPS) ||
                        (fabsf(tMax - lastScaleMax) > SCALE_REDRAW_EPS);

    if (modeChanged || rangeChanged) {
      drawTempScaleHorizontal(tMin, tMax);
      lastScaleMin  = tMin;
      lastScaleMax  = tMax;
      lastScaleMode = colorMode;
      scaleInit     = true;
    }
  }

  tempBarDiv++;
  if (tempBarDiv >= 6) {
    tempBarDiv = 0;

    if (focusMode == FOCUS_HOTCOLD && liveHCValid) {
      drawTempHoldBar(liveMinHC, liveMaxHC, liveAvgRaw, "Coldest ", "Hottest ", "Average ");
    } else {
      drawTempHoldBar(holdMin, holdMax, liveAvgRaw, "Min-Hold ", "Max-Hold ", "Average ");
    }
  }

  if (focusMode == FOCUS_ENVIRONMENT) {
    histFrameDiv++;
    if (histFrameDiv >= 3) {
      histFrameDiv = 0;
      drawHistogramLive_Optimized(img, tMin, tMax);
    }
  } else {
    histFrameDiv = 0;
  }
}
