/*
  MTG Life Counter — Waveshare ESP32-S3 Knob Touch LCD 1.8" (360x360 round)
  ------------------------------------------------------------------------------
  A Magic: The Gathering life/counter tracker for the Waveshare knob display.
    Display : ST77916 over QSPI  (needs the PATCHED Arduino_GFX in ./libraries)
    Touch   : CST816S over I2C (read directly over Wire)
    Knob    : rotary encoder A=GPIO8 B=GPIO7 — NO push button on this board,
              so Settings is opened with an on-screen gear, selection is by tap,
              and seat-move is confirmed with an on-screen "Done" button.
  Board-independent game logic lives in game_engine.h.

  Pin map (Waveshare ESP32-S3-Knob-Touch-LCD-1.8 / JC3636W518):
    LCD  QSPI  CS=14 CLK=13 D0=15 D1=16 D2=17 D3=18  RST=21  BL=47(PWM)
    Touch I2C  SDA=11 SCL=12  INT=9  RST=10  addr 0x15
    Encoder    A=8  B=7   (no press)   |   Battery ADC = GPIO1 (2:1 divider)

  Requires: ESP32 Arduino core 2.0.14, lvgl 8.3.6 (+ the provided lv_conf.h),
  and the PATCHED Arduino_GFX bundled in ./libraries (stock Arduino_GFX shows a
  striped/garbled screen — the ST77916 V2 init table is required). See README.md.
  Board settings: ESP32S3 Dev Module, Flash 16MB, Partition "huge_app",
  PSRAM "OPI", USB CDC On Boot "Enabled".
*/

#include <lvgl.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <Preferences.h>
#include <math.h>

#include "game_engine.h"
LV_FONT_DECLARE(life_font_88);

// ====================== Display (ST77916 QSPI) ======================
#define LCD_CS 14
#define LCD_SCK 13
#define LCD_D0 15
#define LCD_D1 16
#define LCD_D2 17
#define LCD_D3 18
#define LCD_RST 21
#define LCD_BL 47
static const uint16_t screenWidth = 360, screenHeight = 360;

Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCK, LCD_D0, LCD_D1, LCD_D2, LCD_D3);
Arduino_GFX *gfx = new Arduino_ST77916(bus, LCD_RST, 0 /* rotation */, true /* IPS */, 360, 360);

const int blFreq = 50000, blChannel = 0, blRes = 8;   // 50kHz (matches Waveshare demo; avoids flicker/banding)

// ====================== Touch (CST816S over I2C) ======================
#define TP_SDA 11
#define TP_SCL 12
#define TP_INT 9
#define TP_RST 10
#define TP_ADDR 0x15

// ====================== Encoder (no push button) ======================
#define ENCODER_A_PIN 8
#define ENCODER_B_PIN 7

// ====================== Battery sense ======================
// Onboard LiPo voltage is on ADC1_CH0 = GPIO1 through a 2:1 divider
// (confirmed by Waveshare's 01_ADC_Test demo: reads CH0, multiplies by 2).
#define BAT_ADC_PIN 1
// Charge-time estimate: 500 mAh cell, ~500 mA onboard charger, x1.2 CV-taper overhead.
// NOTE: while plugged in, the rail voltage is pinned high (~4.7V), so charge PROGRESS
// can't be read from voltage. We estimate by counting down from the last on-battery %.
// Battery capacity is a per-device SETTING (units ship with different LiPos). Charge-time model:
// full(min) ~= capacity(mAh) / 500mA charger * 60 * 1.2 (CV taper)  ==  capacity * 0.144.
// The runtime "time left" estimate is capacity-independent (measured drain), so it needs no config.
const int BAT_CAPS[]   = { 500, 700, 1000, 1500, 2000 };   // 802035=500, 102035=700, ...
const int NUM_BAT_CAPS = sizeof(BAT_CAPS) / sizeof(BAT_CAPS[0]);
int  batCapIdx = 1;                    // default 700 mAh (102035, this board's stock cell);
                                       // change it in Settings -> Battery for a different LiPo. Persisted.
static inline int fullChargeMin()     { return (int)(BAT_CAPS[batCapIdx] * 0.144f + 0.5f); }
static inline int fullChargeSafeMin() { return fullChargeMin() * 5 / 4; }   // +25% margin for unknown-start
const float VBUS_THRESH_V = 4.35f;     // above this = plugged into USB / charging
bool     g_charging       = false;     // latched USB-present state (for the main-screen bolt)
int      g_lastUnpluggedPct = -1;      // last true % measured while on battery
bool     g_wasCharging      = false;
uint32_t g_chargeStartMs    = 0;
int      g_chargeStartPct   = -1;      // % when this charge session began (-1 = unknown)
// Discharge tracking -> "time left" estimate. Uses wall-clock elapsed + % dropped since the
// battery was last unplugged, so it self-calibrates to real usage (brightness + sleep included).
uint32_t g_dischargeStartMs = 0;
int      g_dischargeStartPct = -1;     // % at start of this on-battery session (-1 = none yet)

static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf1 = NULL, *buf2 = NULL;

// ====================== Color themes ======================
struct Theme { const char* name; uint32_t bg, panel, text, accent, danger; };
// Dark backgrounds are lifted off pure-black: this panel shows visible
// horizontal banding on very dark flat fields, which is far less obvious at
// slightly higher grey levels.
static const Theme THEMES[] = {
  { "Midnight", 0x20222C, 0x33374A, 0xFFFFFF, 0x00E0FF, 0xFF4040 },
  { "Daylight", 0xF0F0F0, 0xFFFFFF, 0x101018, 0x2060FF, 0xD00000 },
  { "Forest",    0x16301A, 0x224A2C, 0xE8FFE8, 0x50E070, 0xFF5040 },
  { "Crimson",  0x301818, 0x4A2424, 0xFFE8E8, 0xFF6050, 0xFFC000 },
  { "Ocean",    0x152838, 0x244A60, 0xEAF6FF, 0x40C0FF, 0xFF6060 },
};
static const int NUM_THEMES = sizeof(THEMES) / sizeof(THEMES[0]);
static int themeIndex = 0;
static inline lv_color_t C(uint32_t hex) { return lv_color_hex(hex); }
#define TH THEMES[themeIndex]

void applyTheme();
void applyRotation();
void updateBattery();
void updateUI();
void buildPickerScreen();
void startSpin();
void updateSpin();
void positionCounters();

// ---- Backlight brightness (battery-friendly default; adjustable in Settings) ----
const uint8_t BRIGHT_VALUES[] = { 25, 70, 130, 210 };
const char*   BRIGHT_NAMES[]  = { "Dim", "Low", "Med", "High" };
const int NUM_BRIGHT = 4;
int brightIdx = 1;   // default "Low"

// Battery-saver backlight state:
//  - g_lowBat: below threshold, the backlight is capped to "Dim" no matter the setting.
//  - predimmed: halfway to auto-sleep, the backlight drops to PREDIM_VAL as a warning.
const uint8_t LOW_BAT_PCT = 20;        // auto-dim at/below this %
const uint8_t PREDIM_VAL  = 10;        // dim-before-sleep backlight level (very low)
bool g_lowBat   = false;
bool predimmed  = false;
void applyBacklight() {
  uint8_t v = BRIGHT_VALUES[brightIdx];
  if (g_lowBat && v > BRIGHT_VALUES[0]) v = BRIGHT_VALUES[0];   // cap to Dim when low
  ledcWrite(blChannel, v);
}

// ---- Auto-sleep: backlight off after inactivity; any knob/touch wakes it ----
const uint32_t SLEEP_MS[] = { 60000UL, 180000UL, 300000UL, 0 };
const char*    SLEEP_NAMES[] = { "1m", "3m", "5m", "Off" };
const int NUM_SLEEP = 4;
int sleepIdx = 1;              // default 3 min
unsigned long lastActivity = 0;
bool asleep = false;
volatile bool g_wakeReq = false;
void goSleep() { asleep = true; predimmed = false; ledcWrite(blChannel, 0); }
void wakeUp()  { asleep = false; predimmed = false; applyBacklight(); lastActivity = millis(); }

// ---- Screen rotation (square panel: 4 orientations so the cable can exit any edge) ----
// Hardware rotation via the ST77916 MADCTL register (free; no effect on full_refresh).
// Touch is remapped to match in my_touchpad_read(); the LVGL layout is rotation-agnostic.
const char* ROT_NAMES[] = { "0", "90", "180", "270" };
const int NUM_ROT = 4;
int rotIdx = 0;
void applyRotation() {
  gfx->setRotation(rotIdx);
  gfx->fillScreen(BLACK);
  lv_obj_t *s = lv_scr_act();
  if (s) lv_obj_invalidate(s);   // force a full redraw in the new orientation
}

// ---- Opponent corner placement (rotated together as a group; see seat-move) ----
enum { COR_TL = 0, COR_TR = 1, COR_BL = 2, COR_BR = 3, NUM_CORNERS = 4 };
static const int RING[NUM_CORNERS] = { COR_TL, COR_TR, COR_BR, COR_BL };   // clockwise
int seatOffset = 0;
int oppCorner[NUM_OPPONENTS];
void computeSeats() { for (int i = 0; i < NUM_OPPONENTS; i++) oppCorner[i] = RING[(seatOffset + i) % NUM_CORNERS]; }

// ====================== Counters UI registry (0=Poison,1=Energy,2=Storm) ======================
struct CtrUI { const char* label; const char* abbr; uint32_t color; };
const CtrUI CTR_UI[NUM_COUNTERS] = {
  { "POISON", "",  0x40C040 },
  { "ENERGY", "E", 0x30B0FF },
  { "STORM",  "S", 0xFFB030 },
};

// ====================== Persistence ======================
Preferences prefs;
void saveSettings() {
  prefs.begin("mtggc", false);
  prefs.putInt("theme", themeIndex);
  prefs.putInt("fmt", formatIndex);
  prefs.putInt("seats", seatOffset);
  int m = 0; for (int k = 0; k < NUM_COUNTERS; k++) if (counterEnabled[k]) m |= (1 << k);
  prefs.putInt("counters", m);
  prefs.putInt("bright", brightIdx);
  prefs.putInt("sleep", sleepIdx);
  prefs.putInt("rot", rotIdx);
  prefs.putInt("batcap", batCapIdx);
  prefs.end();
}
void loadSettings() {
  prefs.begin("mtggc", true);
  themeIndex = clampInt(prefs.getInt("theme", 0), 0, NUM_THEMES - 1);
  int fmt    = clampInt(prefs.getInt("fmt", 1), 0, NUM_FORMATS - 1);
  seatOffset = clampInt(prefs.getInt("seats", 0), 0, NUM_CORNERS - 1);
  int m      = prefs.getInt("counters", 1);
  for (int k = 0; k < NUM_COUNTERS; k++) counterEnabled[k] = (m >> k) & 1;
  brightIdx  = clampInt(prefs.getInt("bright", 1), 0, NUM_BRIGHT - 1);
  sleepIdx   = clampInt(prefs.getInt("sleep", 1), 0, NUM_SLEEP - 1);
  rotIdx     = clampInt(prefs.getInt("rot", 0), 0, NUM_ROT - 1);
  batCapIdx  = clampInt(prefs.getInt("batcap", 1), 0, NUM_BAT_CAPS - 1);  // default 700 mAh
  prefs.end();
  computeSeats();
  applyFormat(fmt);
}


// ====================== Shared state (task <-> loop) ======================
volatile int  g_encDelta = 0;
volatile bool g_ready    = false;
bool uiDirty = true;

// ====================== Geometry (360x360) ======================
static const int LIFE_W = 200, LIFE_H = 156;
static const int OPP_W  = 74,  OPP_H  = 52;
#define DOT_SIZE 30
#define DOT_LIGHT_RED  0xFF8080
#define DOT_LETHAL_RED 0xFF2020
#define DOT_LETHAL_GRN 0x20E020
#define TAX_COLOR 0xB060FF

// ====================== LVGL objects ======================
lv_obj_t *scrGame, *scrSettings, *scrPicker;
lv_obj_t *lblFormat, *panelLife, *lblLife, *lblLifeCap, *lblDead, *btnGear, *lblBattery;
lv_obj_t *oppPanel[NUM_OPPONENTS], *oppNum[NUM_OPPONENTS], *oppCap[NUM_OPPONENTS], *oppDot[NUM_OPPONENTS];
bool oppExpanded[NUM_OPPONENTS] = { false, false, false };
lv_obj_t *ctrPanel[NUM_COUNTERS], *ctrNum[NUM_COUNTERS], *ctrCap[NUM_COUNTERS], *ctrPip[NUM_COUNTERS], *ctrPipLbl[NUM_COUNTERS];
bool ctrExpanded[NUM_COUNTERS] = { false, false, false };
lv_obj_t *poisonRing, *poisonBar;
lv_obj_t *taxPanel, *taxNum, *taxCap, *taxPip, *taxPipLbl;
bool taxExpanded = false;
lv_obj_t *lblSeatHint, *btnSeatDone;
// settings
lv_obj_t *lblSetTitle, *btnFormat, *lblBtnFormat, *btnTheme, *lblBtnTheme, *lblBtnCtr[NUM_COUNTERS];
lv_obj_t *btnBright, *lblBtnBright, *btnSleep, *lblBtnSleep, *btnRot, *lblBtnRot;
lv_obj_t *setCol;   // scrollable settings column (knob scrolls it)
// Battery detail + Counters sub-screens
lv_obj_t *scrBattery, *scrCounters;
lv_obj_t *btnBattery, *lblBtnBattery;               // Settings menu button (shows live %)
lv_obj_t *lblBatTitle, *lblBatBig, *lblBatVolt, *lblBatStatus; // battery detail screen
lv_obj_t *btnBatCap, *lblBtnBatCap;                 // battery-capacity selector (per device)
lv_obj_t *lblCtrTitle;
// picker
lv_obj_t *pickArrow, *pickHub, *lblPickTitle, *lblPickResult, *lblPickHint;

bool seatMode = false;
bool spinning = false;
float spinAngle = 315.0f, spinStart = 0, spinTotal = 0;
const float spinDurationMs = 3000.0f;
unsigned long spinT0 = 0;
int spinTargetCorner = 0;
const float CORNER_ANGLE[NUM_CORNERS] = { 225.0f, 315.0f, 135.0f, 45.0f };
const char* CORNER_LABEL_FULL[NUM_CORNERS] = { "Top-Left", "Top-Right", "Bottom-Left", "Bottom-Right" };

// ====================== Display flush ======================
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = area->x2 - area->x1 + 1, h = area->y2 - area->y1 + 1;
#if (LV_COLOR_16_SWAP != 0)
  gfx->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
#else
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
#endif
  lv_disp_flush_ready(disp);
}

// ====================== Touch (CST816S) ======================
bool readTouch(int16_t *x, int16_t *y) {
  uint8_t buf[6];
  Wire.beginTransmission(TP_ADDR);
  Wire.write(0x01);                 // start at gesture/status reg
  if (Wire.endTransmission(true) != 0) return false;
  if (Wire.requestFrom(TP_ADDR, 6) != 6) return false;
  for (int i = 0; i < 6; i++) buf[i] = Wire.read();
  uint8_t points = buf[1] & 0x0F;   // 0x02: finger count
  if (points == 0) return false;
  *x = ((buf[2] & 0x0F) << 8) | buf[3];   // 0x03/0x04
  *y = ((buf[4] & 0x0F) << 8) | buf[5];   // 0x05/0x06
  return true;
}
void my_touchpad_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  if (asleep) {                       // while asleep, a touch only wakes (no tap-through)
    int16_t tx, ty;
    if (readTouch(&tx, &ty)) g_wakeReq = true;
    data->state = LV_INDEV_STATE_REL;
    return;
  }
  int16_t x, y;
  if (readTouch(&x, &y)) {
    // Touch panel reports in the native (rotation-0) frame; map it to the
    // current display orientation. Square panel, so S is the same on both axes.
    const int16_t S = screenWidth - 1;   // 359
    int16_t lx = x, ly = y;
    switch (rotIdx) {
      case 1: lx = y;     ly = S - x; break;   // 90
      case 2: lx = S - x; ly = S - y; break;   // 180
      case 3: lx = S - y; ly = x;     break;   // 270
      default: break;                          // 0
    }
    data->point.x = lx;
    data->point.y = ly;
    data->state = LV_INDEV_STATE_PR;
    lastActivity = millis();
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

// ====================== Event callbacks ======================
static void selectCounterCb(lv_event_t *e) { selected = (int)(intptr_t)lv_event_get_user_data(e); uiDirty = true; }
static void toggleOppCb(lv_event_t *e) {
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  oppExpanded[i] = !oppExpanded[i];
  if (oppExpanded[i]) selected = i + 1; else if (selected == i + 1) selected = 0;
  uiDirty = true;
}
static void toggleCounterCb(lv_event_t *e) {
  int k = (int)(intptr_t)lv_event_get_user_data(e);
  ctrExpanded[k] = !ctrExpanded[k];
  if (ctrExpanded[k]) selected = COUNTER_SEL_BASE + k; else if (selected == COUNTER_SEL_BASE + k) selected = 0;
  uiDirty = true;
}
static void toggleTaxCb(lv_event_t *e) {
  taxExpanded = !taxExpanded;
  if (taxExpanded) selected = TAX_SEL; else if (selected == TAX_SEL) selected = 0;
  uiDirty = true;
}
static void counterEnableCb(lv_event_t *e) {
  int k = (int)(intptr_t)lv_event_get_user_data(e);
  counterEnabled[k] = !counterEnabled[k];
  if (!counterEnabled[k]) { ctrExpanded[k] = false; if (selected == COUNTER_SEL_BASE + k) selected = 0; }
  positionCounters(); saveSettings(); uiDirty = true;
}
static void openSettingsCb(lv_event_t *e) { updateBattery(); lv_scr_load_anim(scrSettings, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false); uiDirty = true; }
static void backToGameCb(lv_event_t *e) { lv_scr_load_anim(scrGame, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false); uiDirty = true; }
static void backToSettingsCb(lv_event_t *e) { lv_scr_load_anim(scrSettings, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false); uiDirty = true; }
static void enterBatteryCb(lv_event_t *e) { updateBattery(); lv_scr_load_anim(scrBattery, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false); uiDirty = true; }
static void enterCountersCb(lv_event_t *e) { lv_scr_load_anim(scrCounters, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false); uiDirty = true; }
static void cycleBatCapCb(lv_event_t *e) { batCapIdx = (batCapIdx + 1) % NUM_BAT_CAPS; g_chargeStartMs = millis(); saveSettings(); updateBattery(); }
static void cycleFormatCb(lv_event_t *e) { applyFormat(formatIndex + 1); positionCounters(); saveSettings(); uiDirty = true; }
static void cycleThemeCb(lv_event_t *e) { themeIndex = (themeIndex + 1) % NUM_THEMES; saveSettings(); applyTheme(); uiDirty = true; }
static void cycleBrightCb(lv_event_t *e) { brightIdx = (brightIdx + 1) % NUM_BRIGHT; applyBacklight(); saveSettings(); uiDirty = true; }
static void cycleSleepCb(lv_event_t *e) { sleepIdx = (sleepIdx + 1) % NUM_SLEEP; lastActivity = millis(); saveSettings(); uiDirty = true; }
static void cycleRotCb(lv_event_t *e) { rotIdx = (rotIdx + 1) % NUM_ROT; applyRotation(); saveSettings(); uiDirty = true; }
static void newGameCb(lv_event_t *e) { newGame(); uiDirty = true; }
static void enterPickerCb(lv_event_t *e) { lv_scr_load_anim(scrPicker, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false); uiDirty = true; }
static void pickerSpinCb(lv_event_t *e) { startSpin(); }
static void enterSeatModeCb(lv_event_t *e) { seatMode = true; lv_scr_load_anim(scrGame, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false); uiDirty = true; }
static void seatDoneCb(lv_event_t *e) { seatMode = false; saveSettings(); uiDirty = true; }

// ====================== UI builders ======================
static lv_obj_t* makePanel(lv_obj_t *parent, int w, int h) {
  lv_obj_t *p = lv_obj_create(parent);
  lv_obj_set_size(p, w, h);
  lv_obj_set_style_radius(p, 12, 0);
  lv_obj_set_style_border_width(p, 3, 0);
  lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(p, 2, 0);
  return p;
}
static lv_obj_t* makeButton(lv_obj_t *parent, const char *txt, lv_event_cb_t cb, void *ud, lv_obj_t **outLabel) {
  lv_obj_t *btn = lv_btn_create(parent);
  lv_obj_set_height(btn, 38);
  lv_obj_set_style_radius(btn, 9, 0);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ud);
  lv_obj_t *l = lv_label_create(btn);
  lv_label_set_text(l, txt);
  lv_obj_center(l);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
  if (outLabel) *outLabel = l;
  return btn;
}

void buildGameScreen() {
  scrGame = lv_obj_create(NULL);
  lv_obj_clear_flag(scrGame, LV_OBJ_FLAG_SCROLLABLE);

  // battery readout (top-left, mirrors the gear at top-right)
  lblBattery = lv_label_create(scrGame);
  lv_obj_set_style_text_font(lblBattery, &lv_font_montserrat_14, 0);
  lv_label_set_text(lblBattery, LV_SYMBOL_BATTERY_FULL);
  lv_obj_align(lblBattery, LV_ALIGN_TOP_LEFT, 12, 16);

  lblFormat = lv_label_create(scrGame);
  lv_obj_set_style_text_font(lblFormat, &lv_font_montserrat_16, 0);
  lv_obj_align(lblFormat, LV_ALIGN_TOP_MID, 0, 24);
  lv_obj_add_flag(lblFormat, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(lblFormat, openSettingsCb, LV_EVENT_CLICKED, NULL);

  // gear button (no knob press, so Settings opens by tap)
  btnGear = lv_btn_create(scrGame);
  lv_obj_set_size(btnGear, 38, 38);
  lv_obj_set_style_radius(btnGear, LV_RADIUS_CIRCLE, 0);
  lv_obj_align(btnGear, LV_ALIGN_TOP_RIGHT, -8, 8);
  lv_obj_add_event_cb(btnGear, openSettingsCb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *gl = lv_label_create(btnGear);
  lv_label_set_text(gl, LV_SYMBOL_SETTINGS);
  lv_obj_center(gl);

  panelLife = makePanel(scrGame, LIFE_W, LIFE_H);
  lv_obj_align(panelLife, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_flag(panelLife, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(panelLife, selectCounterCb, LV_EVENT_CLICKED, (void*)(intptr_t)0);

  lblLifeCap = lv_label_create(panelLife);
  lv_label_set_text(lblLifeCap, "LIFE");
  lv_obj_set_style_text_font(lblLifeCap, &lv_font_montserrat_12, 0);
  lv_obj_align(lblLifeCap, LV_ALIGN_TOP_MID, 0, 8);

  lblLife = lv_label_create(scrGame);
  lv_obj_set_style_text_font(lblLife, &life_font_88, 0);
  lv_obj_align(lblLife, LV_ALIGN_CENTER, 0, 4);

  lblDead = lv_label_create(panelLife);
  lv_label_set_text(lblDead, "DEAD");
  lv_obj_set_style_text_font(lblDead, &lv_font_montserrat_12, 0);
  lv_obj_align(lblDead, LV_ALIGN_BOTTOM_MID, 0, -6);
  lv_obj_add_flag(lblDead, LV_OBJ_FLAG_HIDDEN);

  for (int i = 0; i < NUM_OPPONENTS; i++) {
    oppPanel[i] = makePanel(scrGame, OPP_W, OPP_H);
    lv_obj_add_flag(oppPanel[i], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(oppPanel[i], toggleOppCb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    oppCap[i] = lv_label_create(oppPanel[i]);
    lv_label_set_text(oppCap[i], oppName[i]);
    lv_obj_set_style_text_font(oppCap[i], &lv_font_montserrat_10, 0);
    lv_obj_align(oppCap[i], LV_ALIGN_TOP_MID, 0, 2);
    oppNum[i] = lv_label_create(oppPanel[i]);
    lv_obj_set_style_text_font(oppNum[i], &lv_font_montserrat_16, 0);
    lv_obj_align(oppNum[i], LV_ALIGN_BOTTOM_MID, 0, -3);

    oppDot[i] = lv_obj_create(scrGame);
    lv_obj_set_size(oppDot[i], DOT_SIZE, DOT_SIZE);
    lv_obj_set_style_radius(oppDot[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(oppDot[i], 0, 0);
    lv_obj_clear_flag(oppDot[i], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(oppDot[i], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(oppDot[i], toggleOppCb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
  }

  // optional counters
  for (int k = 0; k < NUM_COUNTERS; k++) {
    ctrPanel[k] = makePanel(scrGame, OPP_W, OPP_H);
    lv_obj_add_flag(ctrPanel[k], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ctrPanel[k], toggleCounterCb, LV_EVENT_CLICKED, (void*)(intptr_t)k);
    ctrCap[k] = lv_label_create(ctrPanel[k]);
    lv_label_set_text(ctrCap[k], CTR_UI[k].label);
    lv_obj_set_style_text_font(ctrCap[k], &lv_font_montserrat_10, 0);
    lv_obj_align(ctrCap[k], LV_ALIGN_TOP_MID, 0, 2);
    ctrNum[k] = lv_label_create(ctrPanel[k]);
    lv_obj_set_style_text_font(ctrNum[k], &lv_font_montserrat_16, 0);
    lv_obj_align(ctrNum[k], LV_ALIGN_BOTTOM_MID, 0, -3);

    if (k == 0) {  // poison Φ glyph pip
      ctrPip[k] = lv_obj_create(scrGame);
      lv_obj_set_size(ctrPip[k], 34, 40);
      lv_obj_set_style_bg_opa(ctrPip[k], LV_OPA_TRANSP, 0);
      lv_obj_set_style_border_width(ctrPip[k], 0, 0);
      lv_obj_set_style_pad_all(ctrPip[k], 0, 0);
      lv_obj_clear_flag(ctrPip[k], LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_add_flag(ctrPip[k], LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(ctrPip[k], toggleCounterCb, LV_EVENT_CLICKED, (void*)(intptr_t)k);
      poisonRing = lv_obj_create(ctrPip[k]);
      lv_obj_set_size(poisonRing, 26, 26);
      lv_obj_set_style_radius(poisonRing, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_opa(poisonRing, LV_OPA_TRANSP, 0);
      lv_obj_set_style_border_width(poisonRing, 3, 0);
      lv_obj_clear_flag(poisonRing, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
      lv_obj_align(poisonRing, LV_ALIGN_CENTER, 0, 0);
      poisonBar = lv_obj_create(ctrPip[k]);
      lv_obj_set_size(poisonBar, 4, 40);
      lv_obj_set_style_radius(poisonBar, 2, 0);
      lv_obj_set_style_border_width(poisonBar, 0, 0);
      lv_obj_clear_flag(poisonBar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
      lv_obj_align(poisonBar, LV_ALIGN_CENTER, 0, 0);
    } else {       // energy/storm dot+letter pip
      ctrPip[k] = lv_obj_create(scrGame);
      lv_obj_set_size(ctrPip[k], DOT_SIZE, DOT_SIZE);
      lv_obj_set_style_radius(ctrPip[k], LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_border_width(ctrPip[k], 0, 0);
      lv_obj_clear_flag(ctrPip[k], LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_add_flag(ctrPip[k], LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(ctrPip[k], toggleCounterCb, LV_EVENT_CLICKED, (void*)(intptr_t)k);
      ctrPipLbl[k] = lv_label_create(ctrPip[k]);
      lv_label_set_text(ctrPipLbl[k], CTR_UI[k].abbr);
      lv_obj_set_style_text_font(ctrPipLbl[k], &lv_font_montserrat_14, 0);
      lv_obj_set_style_text_color(ctrPipLbl[k], lv_color_hex(0x101018), 0);
      lv_obj_center(ctrPipLbl[k]);
    }
  }

  // commander tax (permanent, Commander only, leftmost of bottom row)
  taxPanel = makePanel(scrGame, OPP_W, OPP_H);
  lv_obj_add_flag(taxPanel, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(taxPanel, toggleTaxCb, LV_EVENT_CLICKED, NULL);
  taxCap = lv_label_create(taxPanel);
  lv_label_set_text(taxCap, "C.TAX");
  lv_obj_set_style_text_font(taxCap, &lv_font_montserrat_10, 0);
  lv_obj_align(taxCap, LV_ALIGN_TOP_MID, 0, 2);
  taxNum = lv_label_create(taxPanel);
  lv_obj_set_style_text_font(taxNum, &lv_font_montserrat_16, 0);
  lv_obj_align(taxNum, LV_ALIGN_BOTTOM_MID, 0, -3);
  taxPip = lv_obj_create(scrGame);
  lv_obj_set_size(taxPip, DOT_SIZE, DOT_SIZE);
  lv_obj_set_style_radius(taxPip, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(taxPip, 0, 0);
  lv_obj_clear_flag(taxPip, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(taxPip, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(taxPip, toggleTaxCb, LV_EVENT_CLICKED, NULL);
  taxPipLbl = lv_label_create(taxPip);
  lv_label_set_text(taxPipLbl, "T");
  lv_obj_set_style_text_font(taxPipLbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(taxPipLbl, lv_color_hex(0x101018), 0);
  lv_obj_center(taxPipLbl);

  // seat-move hint + Done button (no knob press to confirm)
  lblSeatHint = lv_label_create(scrGame);
  lv_label_set_text(lblSeatHint, "Turn knob to move seats");
  lv_obj_set_style_text_font(lblSeatHint, &lv_font_montserrat_14, 0);
  lv_obj_align(lblSeatHint, LV_ALIGN_CENTER, 0, 44);
  lv_obj_add_flag(lblSeatHint, LV_OBJ_FLAG_HIDDEN);
  btnSeatDone = makeButton(scrGame, "Done", seatDoneCb, NULL, NULL);
  lv_obj_set_width(btnSeatDone, 110);
  lv_obj_align(btnSeatDone, LV_ALIGN_BOTTOM_MID, 0, -18);
  lv_obj_add_flag(btnSeatDone, LV_OBJ_FLAG_HIDDEN);

  positionOpponents();
  positionCounters();
}

void positionOpponents() {
  const int cx = LIFE_W / 2, cy = LIFE_H / 2;
  const int cornerX[NUM_CORNERS] = { -cx, +cx, -cx, +cx };
  const int cornerY[NUM_CORNERS] = { -cy, -cy, +cy, +cy };
  for (int i = 0; i < NUM_OPPONENTS; i++) {
    int c = oppCorner[i];
    lv_obj_align(oppPanel[i], LV_ALIGN_CENTER, cornerX[c], cornerY[c]);
    lv_obj_align(oppDot[i],   LV_ALIGN_CENTER, cornerX[c], cornerY[c]);
  }
}

void positionCounters() {
  bool showTax = trackCommanderDamage;
  int n = showTax ? 1 : 0;
  for (int k = 0; k < NUM_COUNTERS; k++) if (counterEnabled[k]) n++;
  const int spacing = (n >= 4) ? 50 : 60;
  int idx = 0;
  if (showTax) {
    int x = (int)((idx - (n - 1) / 2.0f) * spacing);
    lv_obj_align(taxPanel, LV_ALIGN_BOTTOM_MID, x, -20);
    lv_obj_align(taxPip,   LV_ALIGN_BOTTOM_MID, x, -20);
    idx++;
  }
  for (int k = 0; k < NUM_COUNTERS; k++) {
    if (!counterEnabled[k]) { lv_obj_add_flag(ctrPanel[k], LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(ctrPip[k], LV_OBJ_FLAG_HIDDEN); continue; }
    int x = (int)((idx - (n - 1) / 2.0f) * spacing);
    lv_obj_align(ctrPanel[k], LV_ALIGN_BOTTOM_MID, x, -20);
    lv_obj_align(ctrPip[k],   LV_ALIGN_BOTTOM_MID, x, -20);
    idx++;
  }
}

void buildSettingsScreen() {
  scrSettings = lv_obj_create(NULL);
  lv_obj_clear_flag(scrSettings, LV_OBJ_FLAG_SCROLLABLE);
  lblSetTitle = lv_label_create(scrSettings);
  lv_label_set_text(lblSetTitle, "Settings");
  lv_obj_set_style_text_font(lblSetTitle, &lv_font_montserrat_18, 0);
  lv_obj_align(lblSetTitle, LV_ALIGN_TOP_MID, 0, 26);

  lv_obj_t *col = lv_obj_create(scrSettings);
  setCol = col;
  lv_obj_set_size(col, 250, 280);
  lv_obj_align(col, LV_ALIGN_CENTER, 0, 24);
  lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(col, 0, 0);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(col, 7, 0);

  btnBattery = makeButton(col, "Battery --%", enterBatteryCb, NULL, &lblBtnBattery); lv_obj_set_width(btnBattery, 220);
  btnFormat = makeButton(col, "", cycleFormatCb, NULL, &lblBtnFormat); lv_obj_set_width(btnFormat, 220);
  btnTheme  = makeButton(col, "", cycleThemeCb, NULL, &lblBtnTheme);   lv_obj_set_width(btnTheme, 220);
  btnBright = makeButton(col, "", cycleBrightCb, NULL, &lblBtnBright); lv_obj_set_width(btnBright, 220);
  btnSleep  = makeButton(col, "", cycleSleepCb, NULL, &lblBtnSleep);   lv_obj_set_width(btnSleep, 220);
  btnRot    = makeButton(col, "", cycleRotCb, NULL, &lblBtnRot);       lv_obj_set_width(btnRot, 220);
  lv_obj_t *bCtr   = makeButton(col, "Counters", enterCountersCb, NULL, NULL);       lv_obj_set_width(bCtr, 220);
  lv_obj_t *bSeats = makeButton(col, "Move Opponents", enterSeatModeCb, NULL, NULL); lv_obj_set_width(bSeats, 220);
  lv_obj_t *bPick  = makeButton(col, "First Player", enterPickerCb, NULL, NULL);     lv_obj_set_width(bPick, 220);
  lv_obj_t *bNew  = makeButton(col, "New Game", newGameCb, NULL, NULL); lv_obj_set_width(bNew, 220);
  lv_obj_t *bBack = makeButton(col, "Back", backToGameCb, NULL, NULL);  lv_obj_set_width(bBack, 220);
}

// ---- Battery detail screen: big %, voltage, charge status/ETA, Back + Back to game ----
void buildBatteryScreen() {
  scrBattery = lv_obj_create(NULL);
  lv_obj_clear_flag(scrBattery, LV_OBJ_FLAG_SCROLLABLE);

  lblBatTitle = lv_label_create(scrBattery);
  lv_label_set_text(lblBatTitle, "Battery");
  lv_obj_set_style_text_font(lblBatTitle, &lv_font_montserrat_18, 0);
  lv_obj_align(lblBatTitle, LV_ALIGN_TOP_MID, 0, 40);

  lblBatBig = lv_label_create(scrBattery);
  lv_label_set_text(lblBatBig, "--%");
  lv_obj_set_style_text_font(lblBatBig, &lv_font_montserrat_18, 0);
  lv_obj_align(lblBatBig, LV_ALIGN_CENTER, 0, -34);

  lblBatVolt = lv_label_create(scrBattery);
  lv_label_set_text(lblBatVolt, "-- V");
  lv_obj_set_style_text_font(lblBatVolt, &lv_font_montserrat_16, 0);
  lv_obj_align(lblBatVolt, LV_ALIGN_CENTER, 0, -8);

  lblBatStatus = lv_label_create(scrBattery);
  lv_label_set_text(lblBatStatus, "");
  lv_obj_set_style_text_font(lblBatStatus, &lv_font_montserrat_16, 0);
  lv_obj_align(lblBatStatus, LV_ALIGN_CENTER, 0, 16);

  // per-device battery-capacity selector (drives the charge-time estimate)
  btnBatCap = makeButton(scrBattery, "Battery: -- mAh", cycleBatCapCb, NULL, &lblBtnBatCap);
  lv_obj_set_width(btnBatCap, 180); lv_obj_align(btnBatCap, LV_ALIGN_CENTER, 0, 50);

  lv_obj_t *b1 = makeButton(scrBattery, "Back", backToSettingsCb, NULL, NULL);
  lv_obj_set_width(b1, 150); lv_obj_align(b1, LV_ALIGN_BOTTOM_MID, 0, -58);
  lv_obj_t *b2 = makeButton(scrBattery, "Back to game", backToGameCb, NULL, NULL);
  lv_obj_set_width(b2, 150); lv_obj_align(b2, LV_ALIGN_BOTTOM_MID, 0, -14);
}

// ---- Counters screen: the per-counter On/Off toggles, Back + Back to game ----
void buildCountersScreen() {
  scrCounters = lv_obj_create(NULL);
  lv_obj_clear_flag(scrCounters, LV_OBJ_FLAG_SCROLLABLE);

  lblCtrTitle = lv_label_create(scrCounters);
  lv_label_set_text(lblCtrTitle, "Counters");
  lv_obj_set_style_text_font(lblCtrTitle, &lv_font_montserrat_18, 0);
  lv_obj_align(lblCtrTitle, LV_ALIGN_TOP_MID, 0, 30);

  lv_obj_t *col = lv_obj_create(scrCounters);
  lv_obj_set_size(col, 250, 260);
  lv_obj_align(col, LV_ALIGN_CENTER, 0, 22);
  lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(col, 0, 0);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(col, 7, 0);

  for (int k = 0; k < NUM_COUNTERS; k++) {
    lv_obj_t *b = makeButton(col, "", counterEnableCb, (void*)(intptr_t)k, &lblBtnCtr[k]);
    lv_obj_set_width(b, 220);
  }
  lv_obj_t *b1 = makeButton(col, "Back", backToSettingsCb, NULL, NULL);   lv_obj_set_width(b1, 220);
  lv_obj_t *b2 = makeButton(col, "Back to game", backToGameCb, NULL, NULL); lv_obj_set_width(b2, 220);
}

// ---- First-player picker ----
static lv_point_t g_arrowPts[5];
static void setArrowAngle(float deg) {
  float rad = deg * 3.14159265f / 180.0f;
  float dx = cosf(rad), dy = sinf(rad), px = -dy, py = dx;
  const int cx = 180, cy = 180, R = 100;
  const float barbLen = 28, barbW = 19;
  int tipx = cx + (int)(R * dx), tipy = cy + (int)(R * dy);
  int blx = tipx - (int)(barbLen * dx) + (int)(barbW * px), bly = tipy - (int)(barbLen * dy) + (int)(barbW * py);
  int brx = tipx - (int)(barbLen * dx) - (int)(barbW * px), bry = tipy - (int)(barbLen * dy) - (int)(barbW * py);
  g_arrowPts[0].x = cx; g_arrowPts[0].y = cy;
  g_arrowPts[1].x = tipx; g_arrowPts[1].y = tipy;
  g_arrowPts[2].x = blx; g_arrowPts[2].y = bly;
  g_arrowPts[3].x = tipx; g_arrowPts[3].y = tipy;
  g_arrowPts[4].x = brx; g_arrowPts[4].y = bry;
  lv_line_set_points(pickArrow, g_arrowPts, 5);
}
void buildPickerScreen() {
  scrPicker = lv_obj_create(NULL);
  lv_obj_clear_flag(scrPicker, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(scrPicker, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(scrPicker, pickerSpinCb, LV_EVENT_CLICKED, NULL);
  lblPickTitle = lv_label_create(scrPicker);
  lv_label_set_text(lblPickTitle, "First Player");
  lv_obj_set_style_text_font(lblPickTitle, &lv_font_montserrat_18, 0);
  lv_obj_align(lblPickTitle, LV_ALIGN_TOP_MID, 0, 22);
  lblPickResult = lv_label_create(scrPicker);
  lv_label_set_text(lblPickResult, "");
  lv_obj_set_style_text_font(lblPickResult, &lv_font_montserrat_16, 0);
  lv_obj_align(lblPickResult, LV_ALIGN_TOP_MID, 0, 50);
  pickArrow = lv_line_create(scrPicker);
  lv_obj_set_style_line_width(pickArrow, 11, 0);
  lv_obj_set_style_line_rounded(pickArrow, true, 0);
  setArrowAngle(spinAngle);
  pickHub = lv_obj_create(scrPicker);
  lv_obj_set_size(pickHub, 24, 24);
  lv_obj_set_style_radius(pickHub, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(pickHub, 0, 0);
  lv_obj_clear_flag(pickHub, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(pickHub, LV_ALIGN_CENTER, 0, 0);
  lblPickHint = lv_label_create(scrPicker);
  lv_label_set_text(lblPickHint, "tap to spin");
  lv_obj_set_style_text_font(lblPickHint, &lv_font_montserrat_14, 0);
  lv_obj_align(lblPickHint, LV_ALIGN_BOTTOM_MID, 0, -100);
  lv_obj_t *bBack = makeButton(scrPicker, "Back", backToGameCb, NULL, NULL);
  lv_obj_set_width(bBack, 110);
  lv_obj_align(bBack, LV_ALIGN_BOTTOM_MID, 0, -14);
}
void startSpin() {
  spinTargetCorner = (int)(esp_random() % NUM_CORNERS);
  float target = CORNER_ANGLE[spinTargetCorner];
  float startMod = fmodf(spinAngle, 360.0f); if (startMod < 0) startMod += 360.0f;
  float delta = fmodf(target - startMod, 360.0f); if (delta < 0) delta += 360.0f;
  spinStart = spinAngle; spinTotal = 360.0f * 4 + delta; spinT0 = millis(); spinning = true;
  lv_label_set_text(lblPickResult, ""); lv_label_set_text(lblPickHint, "");
}
void updateSpin() {
  if (!spinning) return;
  float t = (millis() - spinT0) / spinDurationMs;
  if (t >= 1.0f) { t = 1.0f; spinning = false; }
  float eased = 1.0f - powf(1.0f - t, 3.0f);
  spinAngle = spinStart + spinTotal * eased;
  setArrowAngle(spinAngle);
  if (!spinning) { lv_label_set_text(lblPickResult, CORNER_LABEL_FULL[spinTargetCorner]); lv_label_set_text(lblPickHint, "tap to spin again"); }
}

// ====================== Theme ======================
void applyTheme() {
  lv_obj_set_style_bg_color(scrGame, C(TH.bg), 0);
  lv_obj_set_style_bg_color(scrSettings, C(TH.bg), 0);
  lv_obj_set_style_bg_color(scrPicker, C(TH.bg), 0);
  lv_obj_set_style_text_color(lblFormat, C(TH.text), 0);
  lv_obj_set_style_text_color(lblLifeCap, C(TH.text), 0);
  lv_obj_set_style_text_color(lblSetTitle, C(TH.text), 0);
  lv_obj_set_style_text_color(lblDead, C(TH.danger), 0);
  lv_obj_set_style_text_color(lblSeatHint, C(TH.accent), 0);
  lv_obj_set_style_bg_color(panelLife, C(TH.panel), 0);
  for (int i = 0; i < NUM_OPPONENTS; i++) {
    lv_obj_set_style_bg_color(oppPanel[i], C(TH.panel), 0);
    lv_obj_set_style_text_color(oppCap[i], C(TH.text), 0);
    lv_obj_set_style_text_color(oppNum[i], C(TH.text), 0);
  }
  for (int k = 0; k < NUM_COUNTERS; k++) {
    lv_obj_set_style_bg_color(ctrPanel[k], C(TH.panel), 0);
    lv_obj_set_style_text_color(ctrCap[k], C(TH.text), 0);
    lv_obj_set_style_text_color(ctrNum[k], C(TH.text), 0);
  }
  lv_obj_set_style_bg_color(taxPanel, C(TH.panel), 0);
  lv_obj_set_style_text_color(taxCap, C(TH.text), 0);
  lv_obj_set_style_text_color(taxNum, C(TH.text), 0);
  lv_obj_set_style_text_color(lblPickTitle, C(TH.text), 0);
  lv_obj_set_style_text_color(lblPickResult, C(TH.accent), 0);
  lv_obj_set_style_text_color(lblPickHint, C(TH.text), 0);
  lv_obj_set_style_line_color(pickArrow, C(TH.accent), 0);
  lv_obj_set_style_bg_color(pickHub, C(TH.accent), 0);
  lv_obj_set_style_bg_color(scrBattery, C(TH.bg), 0);
  lv_obj_set_style_bg_color(scrCounters, C(TH.bg), 0);
  lv_obj_set_style_text_color(lblBatTitle, C(TH.text), 0);
  lv_obj_set_style_text_color(lblBatVolt, C(TH.text), 0);
  lv_obj_set_style_text_color(lblBatStatus, C(TH.accent), 0);
  lv_obj_set_style_text_color(lblCtrTitle, C(TH.text), 0);
  updateBattery();   // re-tint the readout to the new theme
}

// life color: green (full) -> yellow -> red (near 0)
static lv_color_t lifeColor(int life, int start) {
  if (start < 1) start = 1;
  int t = (life * 255) / start; if (t < 0) t = 0; if (t > 255) t = 255;
  lv_color_t red = C(0xFF3030), yel = C(0xFFC000), grn = C(0x30D050);
  if (t < 128) return lv_color_mix(yel, red, t * 2);
  else         return lv_color_mix(grn, yel, (t - 128) * 2);
}

// ====================== Battery ======================
// Read VBAT (8-sample average) via the calibrated ADC, undo the 2:1 divider.
static float readBatteryVolts() {
  uint32_t mv = 0;
  for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(BAT_ADC_PIN);
  return (mv / 8.0f) * 2.0f / 1000.0f;
}
// Rough single-cell LiPo voltage->% curve (light load), linearly interpolated.
static int batVoltageToPct(float v) {
  static const float vt[] = { 3.30f, 3.50f, 3.60f, 3.70f, 3.80f, 3.90f, 4.00f, 4.10f, 4.20f };
  static const int   pt[] = { 0,     8,     20,    35,    50,    65,    78,    90,    100  };
  const int n = 9;
  if (v <= vt[0]) return 0;
  if (v >= vt[n - 1]) return 100;
  for (int i = 1; i < n; i++)
    if (v <= vt[i]) {
      float f = (v - vt[i - 1]) / (vt[i] - vt[i - 1]);
      return (int)(pt[i - 1] + f * (pt[i] - pt[i - 1]) + 0.5f);
    }
  return 100;
}
void updateBattery() {
  float v = readBatteryVolts();
  int pct = batVoltageToPct(v);

  // ---- charge/discharge session tracking (voltage is pinned high while plugged, so track by time) ----
  bool charging = (v > VBUS_THRESH_V);
  if (charging != g_charging) { g_charging = charging; uiDirty = true; }  // refresh main-screen bolt
  if (!charging) {
    g_lastUnpluggedPct = pct;      // true reading only when on battery
    if (g_wasCharging || g_dischargeStartPct < 0) {   // fresh on-battery session (just unplugged / first boot)
      g_dischargeStartMs = millis();
      g_dischargeStartPct = pct;
    }
    g_wasCharging = false;
  } else {
    if (!g_wasCharging) {
      g_wasCharging = true;
      g_chargeStartMs = millis();
      g_chargeStartPct = g_lastUnpluggedPct;   // -1 if we never saw an on-battery level
    }
    g_dischargeStartPct = -1;      // not discharging while plugged in
  }

  // Display %: while plugged the rail is pinned (~4.7V => fake 100%), so show the last REAL
  // on-battery level instead. -1 means "charging with no prior reading" -> show "--".
  int  shownPct = charging ? g_lastUnpluggedPct : pct;
  bool havePct  = (shownPct >= 0);
  const char* sym = (!havePct || shownPct >= 80) ? LV_SYMBOL_BATTERY_FULL
                  : (shownPct >= 55) ? LV_SYMBOL_BATTERY_3
                  : (shownPct >= 30) ? LV_SYMBOL_BATTERY_2
                  : (shownPct >= 12) ? LV_SYMBOL_BATTERY_1
                                     : LV_SYMBOL_BATTERY_EMPTY;
  const char* icon = charging ? LV_SYMBOL_CHARGE : sym;
  uint32_t pctColor = (havePct && shownPct <= 15) ? TH.danger : TH.text;

  // game-screen corner label (clipped on the round panel, but kept in sync)
  char buf[24];
  if (havePct) snprintf(buf, sizeof(buf), "%s %d%%", icon, shownPct);
  else         snprintf(buf, sizeof(buf), "%s --%%", icon);
  lv_label_set_text(lblBattery, buf);
  lv_obj_set_style_text_color(lblBattery, C(pctColor), 0);

  // Settings menu button: "Battery XX%"
  if (lblBtnBattery) {
    char sbuf[24];
    if (havePct) snprintf(sbuf, sizeof(sbuf), "%s %d%%", icon, shownPct);
    else         snprintf(sbuf, sizeof(sbuf), "%s --%%", icon);
    lv_label_set_text(lblBtnBattery, sbuf);
  }

  // Battery detail screen big number
  if (lblBatBig) {
    char b1[16];
    if (havePct) snprintf(b1, sizeof(b1), "%d%%", shownPct);
    else         snprintf(b1, sizeof(b1), "--%%");
    lv_label_set_text(lblBatBig, b1);
    lv_obj_set_style_text_color(lblBatBig, C(pctColor), 0);
  }
  if (lblBatVolt) { char b2[16]; snprintf(b2, sizeof(b2), "%.2f V", v); lv_label_set_text(lblBatVolt, b2); }
  if (lblBtnBatCap) { char bc[24]; snprintf(bc, sizeof(bc), "Battery: %d mAh", BAT_CAPS[batCapIdx]); lv_label_set_text(lblBtnBatCap, bc); }
  if (lblBatStatus) {
    char st[56];
    if (charging) {
      float elapsedMin = (millis() - g_chargeStartMs) / 60000.0f;
      if (g_chargeStartPct < 0) {
        // booted already plugged in: no starting level, so estimate full by elapsed time
        if (elapsedMin >= fullChargeSafeMin()) snprintf(st, sizeof(st), LV_SYMBOL_CHARGE " Fully charged (est.)");
        else                                   snprintf(st, sizeof(st), LV_SYMBOL_CHARGE " Charging (on USB)");
      } else {
        float totalMin = (100 - g_chargeStartPct) / 100.0f * fullChargeMin();
        int   remain   = (int)ceilf(totalMin - elapsedMin);
        if (remain <= 0) snprintf(st, sizeof(st), LV_SYMBOL_CHARGE " Fully charged");
        else             snprintf(st, sizeof(st), LV_SYMBOL_CHARGE " ~%d min to full", remain);
      }
    } else {
      // time-left: % dropped over real elapsed time since unplugged (accounts for sleep)
      int   dp       = (g_dischargeStartPct >= 0) ? (g_dischargeStartPct - pct) : 0;
      float elapsedH = (millis() - g_dischargeStartMs) / 3600000.0f;
      if (g_dischargeStartPct >= 0 && dp >= 3 && elapsedH > 0.05f) {
        float rate     = dp / elapsedH;                 // %/hour
        int   totalMin = (int)(pct / rate * 60.0f);
        if (totalMin < 0) totalMin = 0;
        int h = totalMin / 60, m = totalMin % 60;
        if (h > 0) snprintf(st, sizeof(st), "~%dh %dm left", h, m);
        else       snprintf(st, sizeof(st), "~%dm left", m);
      } else {
        snprintf(st, sizeof(st), "On battery - estimating...");
      }
    }
    lv_label_set_text(lblBatStatus, st);
  }

  // auto-dim: cap backlight to Dim once low (hysteresis to avoid flicker at the edge)
  bool low = g_lowBat ? (pct <= LOW_BAT_PCT + 3) : (pct <= LOW_BAT_PCT);
  if (low != g_lowBat) {
    g_lowBat = low;
    if (!asleep && !predimmed) applyBacklight();
  }
}

void updateUI() {
  char buf[40];
  // Charging bolt lives on the format label (always-visible spot; the round screen clips the corner one)
  snprintf(buf, sizeof(buf), "%s%s  (%d)", g_charging ? LV_SYMBOL_CHARGE " " : "", formatLabel(), startingLife);
  lv_label_set_text(lblFormat, buf);

  snprintf(buf, sizeof(buf), "%d", myLife);
  lv_label_set_text(lblLife, buf);
  lv_obj_set_style_text_color(lblLife, lifeColor(myLife, startingLife), 0);

  if (isDead()) lv_obj_clear_flag(lblDead, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(lblDead, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_border_color(panelLife, C((selected == 0 && !seatMode) ? TH.accent : TH.panel), 0);

  if (seatMode) { lv_obj_clear_flag(lblSeatHint, LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(btnSeatDone, LV_OBJ_FLAG_HIDDEN); }
  else          { lv_obj_add_flag(lblSeatHint, LV_OBJ_FLAG_HIDDEN);   lv_obj_add_flag(btnSeatDone, LV_OBJ_FLAG_HIDDEN); }

  for (int i = 0; i < NUM_OPPONENTS; i++) {
    bool track = trackCommanderDamage;
    bool lethal = cmdrDamage[i] >= LETHAL_CMDR_DAMAGE;
    bool expanded = track && !seatMode && (oppExpanded[i] || selected == i + 1);
    bool dot = track && !expanded;
    if (expanded) lv_obj_clear_flag(oppPanel[i], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(oppPanel[i], LV_OBJ_FLAG_HIDDEN);
    if (dot) lv_obj_clear_flag(oppDot[i], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(oppDot[i], LV_OBJ_FLAG_HIDDEN);
    if (expanded) {
      snprintf(buf, sizeof(buf), "%d/%d", cmdrDamage[i], LETHAL_CMDR_DAMAGE);
      lv_label_set_text(oppNum[i], buf);
      lv_obj_set_style_text_color(oppNum[i], lethal ? C(TH.danger) : C(TH.text), 0);
      lv_obj_set_style_border_color(oppPanel[i], C(selected == i + 1 ? TH.accent : TH.panel), 0);
    }
    if (dot) {
      uint32_t dc = seatMode ? TH.accent : (lethal ? DOT_LETHAL_RED : DOT_LIGHT_RED);
      lv_obj_set_style_bg_color(oppDot[i], C(dc), 0);
    }
  }

  for (int k = 0; k < NUM_COUNTERS; k++) {
    if (!counterEnabled[k]) continue;
    bool lethal = counterLethal[k] > 0 && counterVal[k] >= counterLethal[k];
    bool exp = ctrExpanded[k] || selected == COUNTER_SEL_BASE + k;
    if (exp) lv_obj_clear_flag(ctrPanel[k], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(ctrPanel[k], LV_OBJ_FLAG_HIDDEN);
    if (!exp) lv_obj_clear_flag(ctrPip[k], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(ctrPip[k], LV_OBJ_FLAG_HIDDEN);
    if (exp) {
      if (counterLethal[k] > 0) snprintf(buf, sizeof(buf), "%d/%d", counterVal[k], counterLethal[k]);
      else                      snprintf(buf, sizeof(buf), "%d", counterVal[k]);
      lv_label_set_text(ctrNum[k], buf);
      lv_obj_set_style_text_color(ctrNum[k], lethal ? C(TH.danger) : C(TH.text), 0);
      lv_obj_set_style_border_color(ctrPanel[k], C(selected == COUNTER_SEL_BASE + k ? TH.accent : TH.panel), 0);
    } else if (k == 0) {
      uint32_t pc = lethal ? DOT_LETHAL_GRN : CTR_UI[k].color;
      lv_obj_set_style_border_color(poisonRing, C(pc), 0);
      lv_obj_set_style_bg_color(poisonBar, C(pc), 0);
    } else {
      lv_obj_set_style_bg_color(ctrPip[k], C(CTR_UI[k].color), 0);
    }
  }

  if (trackCommanderDamage) {
    bool exp = taxExpanded || selected == TAX_SEL;
    if (exp) lv_obj_clear_flag(taxPanel, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(taxPanel, LV_OBJ_FLAG_HIDDEN);
    if (!exp) lv_obj_clear_flag(taxPip, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(taxPip, LV_OBJ_FLAG_HIDDEN);
    if (exp) {
      snprintf(buf, sizeof(buf), "+%d", cmdrTax);
      lv_label_set_text(taxNum, buf);
      lv_obj_set_style_border_color(taxPanel, C(selected == TAX_SEL ? TH.accent : TH.panel), 0);
    } else {
      lv_obj_set_style_bg_color(taxPip, C(TAX_COLOR), 0);
    }
  } else {
    lv_obj_add_flag(taxPanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(taxPip, LV_OBJ_FLAG_HIDDEN);
  }

  snprintf(buf, sizeof(buf), "Format: %s", FORMATS[formatIndex].name); lv_label_set_text(lblBtnFormat, buf);
  snprintf(buf, sizeof(buf), "Theme: %s", TH.name); lv_label_set_text(lblBtnTheme, buf);
  snprintf(buf, sizeof(buf), "Brightness: %s", BRIGHT_NAMES[brightIdx]); lv_label_set_text(lblBtnBright, buf);
  snprintf(buf, sizeof(buf), "Sleep: %s", SLEEP_NAMES[sleepIdx]); lv_label_set_text(lblBtnSleep, buf);
  snprintf(buf, sizeof(buf), "Screen: %s deg", ROT_NAMES[rotIdx]); lv_label_set_text(lblBtnRot, buf);
  for (int k = 0; k < NUM_COUNTERS; k++) {
    snprintf(buf, sizeof(buf), "%s: %s", CTR_UI[k].label, counterEnabled[k] ? "On" : "Off");
    lv_label_set_text(lblBtnCtr[k], buf);
  }
}

// ====================== Encoder task (no button) ======================
// This is a *bidirectional pulse* knob (not standard quadrature): turning one
// way pulses line A, the other way pulses line B. So we count a rising edge on
// A as +1 and a rising edge on B as -1 (matches Waveshare's iot_knob driver).
TaskHandle_t encTaskHandle = NULL;
void encTask(void *pv) {
  uint8_t a = digitalRead(ENCODER_A_PIN);
  uint8_t b = digitalRead(ENCODER_B_PIN);
  while (1) {
    uint8_t a1 = digitalRead(ENCODER_A_PIN), a2 = digitalRead(ENCODER_A_PIN);
    uint8_t b1 = digitalRead(ENCODER_B_PIN), b2 = digitalRead(ENCODER_B_PIN);
    if (a1 == a2 && a1 != a) { a = a1; if (a == 1 && g_ready) g_encDelta++; }  // A rising
    if (b1 == b2 && b1 != b) { b = b1; if (b == 1 && g_ready) g_encDelta--; }  // B rising
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void rotateSeats(int steps) {
  seatOffset = ((seatOffset + steps) % NUM_CORNERS + NUM_CORNERS) % NUM_CORNERS;
  computeSeats();
  positionOpponents();
}

// ====================== Setup / loop ======================
void setup() {
  Serial.begin(115200);
  delay(200);
  loadSettings();

  // I2C + touch reset
  Wire.begin(TP_SDA, TP_SCL);
  pinMode(TP_RST, OUTPUT);
  digitalWrite(TP_RST, HIGH); delay(10);
  digitalWrite(TP_RST, LOW);  delay(20);
  digitalWrite(TP_RST, HIGH); delay(50);
  pinMode(TP_INT, INPUT);

  pinMode(ENCODER_A_PIN, INPUT_PULLUP);
  pinMode(ENCODER_B_PIN, INPUT_PULLUP);

  gfx->begin(40000000);          // 40MHz QSPI (80MHz default can corrupt init on this wiring)
  gfx->setRotation(rotIdx);      // restore saved screen orientation
  gfx->fillScreen(BLACK);

  lv_init();
  // Full-screen buffers + full_refresh: the whole frame is written in one pass
  // (avoids the per-strip horizontal seams of partial buffers on this panel).
  size_t pix = (size_t)screenWidth * screenHeight;
  buf1 = (lv_color_t *)heap_caps_malloc(sizeof(lv_color_t) * pix, MALLOC_CAP_SPIRAM);
  buf2 = (lv_color_t *)heap_caps_malloc(sizeof(lv_color_t) * pix, MALLOC_CAP_SPIRAM);
  lv_disp_draw_buf_init(&draw_buf, buf1, buf2, pix);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = screenWidth;
  disp_drv.ver_res = screenHeight;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  disp_drv.full_refresh = 1;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touchpad_read;
  lv_indev_drv_register(&indev_drv);

  buildGameScreen();
  buildSettingsScreen();
  buildBatteryScreen();
  buildCountersScreen();
  buildPickerScreen();
  applyTheme();
  updateUI();
  lv_scr_load(scrGame);

  // backlight on
  ledcSetup(blChannel, blFreq, blRes);
  ledcAttachPin(LCD_BL, blChannel);
  applyBacklight();   // battery-friendly default, persisted

  xTaskCreatePinnedToCore(encTask, "ENC", 2048, NULL, 1, &encTaskHandle, 0);
  lastActivity = millis();
  g_ready = true;
  Serial.println("MTG Game Companion (Waveshare) ready.");
}

void loop() {
  if (g_wakeReq) { g_wakeReq = false; wakeUp(); }   // touch woke us

  if (g_encDelta != 0) {
    int d = g_encDelta; g_encDelta = 0;
    bool was = asleep;
    wakeUp();                                        // any knob turn counts as activity
    if (!was) {                                      // if it wasn't a wake-up, act on it
      lv_obj_t *cur = lv_scr_act();
      if (cur == scrGame) {
        if (seatMode) rotateSeats(d);
        else          adjustSelected(d > 0 ? +abs(d) : -abs(d));
        uiDirty = true;
      } else if (cur == scrSettings) {
        lv_obj_scroll_by(setCol, 0, -d * 28, LV_ANIM_ON);
      }
    }
  }

  if (uiDirty) { updateUI(); uiDirty = false; }
  if (lv_scr_act() == scrPicker) updateSpin();

  // refresh battery readout every 15s (cheap ADC read; not while asleep)
  static unsigned long lastBatMs = 0;
  if (!asleep && millis() - lastBatMs > 15000) { lastBatMs = millis(); updateBattery(); }

  // Idle backlight staging: dim at half the timeout, off (sleep) at the timeout.
  uint32_t t = SLEEP_MS[sleepIdx];
  if (!asleep && t != 0) {
    unsigned long idle = millis() - lastActivity;
    if (idle >= t) {
      goSleep();
    } else if (idle >= t / 2) {
      if (!predimmed) { predimmed = true; ledcWrite(blChannel, PREDIM_VAL); }
    } else if (predimmed) {
      predimmed = false; applyBacklight();        // activity resumed before sleep
    }
  }

  lv_timer_handler();
  delay(5);
}
