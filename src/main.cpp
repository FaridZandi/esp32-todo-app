// Todo Checker: a touch-first, offline daily checklist for ESP32-S3-Touch-LCD-3.49.
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <Wire.h>
#include <WiFi.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_sleep.h>

#include <algorithm>

#if !defined(TODO_REVISION)
#error "Build with pio -e rev1 or pio -e rev2."
#endif

#if __has_include("Secrets.h")
#include "Secrets.h"
#endif
#ifndef TODO_WIFI_SSID
#define TODO_WIFI_SSID ""
#define TODO_WIFI_PASSWORD ""
#define TODO_TIMEZONE "EST5EDT,M3.2.0,M11.1.0"
#endif

namespace {
constexpr int kPanelW = 172, kPanelH = 640;
constexpr int kWidth = kPanelW, kHeight = kPanelH;
constexpr int kStatsWidth = kPanelH, kStatsHeight = kPanelW;
constexpr int kTouchAddress = 0x3B, kExpanderAddress = 0x20;
constexpr int kBacklightRail = 1, kLcdReset = 5, kSysEn = 6, kTouchIrq = 0;
constexpr int kBacklightPin = TODO_REVISION == 1 ? 8 : 42;
constexpr int kTouchIrqPin = TODO_REVISION == 1 ? 42 : 8;
constexpr int kResetPin = TODO_REVISION == 1 ? 21 : -1;
constexpr int kHeaderHeight = 96, kListTop = 104, kListBottom = 548;
constexpr int kListHeight = kListBottom - kListTop;
constexpr int kCardMinHeight = 78, kCardGap = 6;
constexpr int kTaskTextX = 20, kTaskTextWidth = 136, kTaskTextSize = 2, kTaskLineHeight = 16;
constexpr int kFilterTop = 576, kFilterHeight = 48, kFilterHitTop = 552;
constexpr int kAllButtonWidth = 105, kFilterButtonWidth = 55;
constexpr int kPowerHoldMs = 300;
constexpr int kBatteryAdcPin = 4;
constexpr uint32_t kBatteryIntervalMs = 30000;
constexpr int kTouchDragThreshold = 10;
constexpr uint16_t kBackground = 0x1082, kCard = 0x2104, kText = 0xFFFF;
constexpr uint16_t kMuted = 0x9CD3, kAccent = 0x5E6A, kDoneCard = 0x05C7, kNotDoneCard = 0xC965;

class PsramCanvas final : public Arduino_Canvas {
 public:
  using Arduino_Canvas::Arduino_Canvas;
  bool begin(int32_t speed = GFX_NOT_DEFINED) override {
    if (!_framebuffer) {
      _framebuffer = static_cast<uint16_t *>(heap_caps_aligned_alloc(
          16, static_cast<size_t>(kPanelW) * kPanelH * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (!_framebuffer) return false;
    }
    return Arduino_Canvas::begin(speed);
  }
};

Arduino_ESP32QSPI *bus;
Arduino_AXS15231B *panel;
PsramCanvas *portrait_screen;
PsramCanvas *stats_screen;
PsramCanvas *screen;

#if __has_include("Tasks.h")
#include "Tasks.h"
#else
#error "Copy src/Tasks.example.h to src/Tasks.h and add your private tasks."
#endif
constexpr int kTaskCount = sizeof(kTasks) / sizeof(kTasks[0]);
static_assert(kTaskCount <= 16, "Two-bit task states fit up to 16 tasks in NVS.");
Preferences preferences;
uint32_t task_mask = 0;
int selected_day_offset = 0;
time_t today_anchor = 0;
bool clock_is_synced = false;
bool ntp_requested = false;
int scroll_offset = 0;
bool filter_active = false;
uint32_t filtered_out_mask = 0;  // resolved tasks captured the last time FILTER was pressed
bool stats_page = false;
enum class StatsMode : uint8_t { Days, Weeks };
StatsMode stats_mode = StatsMode::Days;
struct Battery {
  bool present = false;
  float volts = 0.0f;
  uint8_t percent = 0;
};
Battery battery;
bool touch_held = false;
int16_t touch_x = 0, touch_y = 0;
int16_t press_x = 0, press_y = 0;
int16_t last_drag_y = 0;
bool press_started_in_list = false;
bool touch_dragging = false;
uint32_t last_ready_poll = 0, last_touch_read = 0, last_contact_ms = 0;
uint32_t last_clock_poll = 0;
uint32_t last_battery_ms = 0;
bool boot_was_down = false;
uint32_t boot_changed_ms = 0;
uint32_t power_pressed_ms = 0;

void render();
void renderStats();
int displayedTaskCount();
int taskAtDisplayIndex(int index);
Battery readBattery();

time_t buildDate() {
  char month_text[4] = {}; int day = 1, year = 2026;
  sscanf(__DATE__, "%3s %d %d", month_text, &day, &year);
  constexpr const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  int month = 0;
  while (month < 11 && strcmp(month_text, months[month]) != 0) ++month;
  tm date = {}; date.tm_year = year - 1900; date.tm_mon = month; date.tm_mday = day; date.tm_hour = 12;
  return mktime(&date);
}
time_t dateAtOffset(int offset) {
  tm date = {}; localtime_r(&today_anchor, &date);
  date.tm_mday += offset;
  date.tm_hour = 12;  // avoids a DST boundary changing the selected calendar day
  return mktime(&date);
}
time_t selectedDate() { return dateAtOffset(selected_day_offset); }
void dayKeyFor(time_t date, char *out, size_t size) {
  tm local = {}; localtime_r(&date, &local);
  char date_key[10];
  strftime(date_key, sizeof(date_key), "%Y%m%d", &local);
  // A private task-list version prevents old task positions from leaking into
  // a replacement list. NVS keys must be 15 characters or shorter.
  snprintf(out, size, "v%ud%s", kTaskListVersion, date_key);
}
void dayKey(char *out, size_t size) {
  dayKeyFor(selectedDate(), out, size);
}
enum class TaskState : uint8_t { Default = 0, Done = 1, NotDone = 2 };
TaskState taskStateInMask(uint32_t mask, int task) {
  return static_cast<TaskState>((mask >> (task * 2)) & 0x3u);
}
TaskState taskState(int task) { return taskStateInMask(task_mask, task); }
bool taskDoneInMask(uint32_t mask, int task) { return taskStateInMask(mask, task) == TaskState::Done; }
bool taskNotDoneInMask(uint32_t mask, int task) { return taskStateInMask(mask, task) == TaskState::NotDone; }
bool taskMarkedInMask(uint32_t mask, int task) { return taskStateInMask(mask, task) != TaskState::Default; }
bool taskDone(int task) { return taskState(task) == TaskState::Done; }
bool taskNotDone(int task) { return taskState(task) == TaskState::NotDone; }
int completedInMask(uint32_t mask) {
  int complete = 0;
  for (int task = 0; task < kTaskCount; ++task) if (taskDoneInMask(mask, task)) ++complete;
  return complete;
}
int notDoneInMask(uint32_t mask) {
  int missed = 0;
  for (int task = 0; task < kTaskCount; ++task) if (taskNotDoneInMask(mask, task)) ++missed;
  return missed;
}
void cycleTaskState(int task) {
  const uint32_t shift = task * 2;
  const TaskState next = static_cast<TaskState>((static_cast<uint8_t>(taskState(task)) + 1) % 3);
  task_mask = (task_mask & ~(0x3u << shift)) | (static_cast<uint32_t>(next) << shift);
}
uint32_t maskAtOffset(int offset) {
  char key[12]; dayKeyFor(dateAtOffset(offset), key, sizeof(key));
  return preferences.getUInt(key, 0);
}
void loadDay() {
  char key[12]; dayKey(key, sizeof(key));
  task_mask = preferences.getUInt(key, 0);
}
void saveDay() {
  char key[12]; dayKey(key, sizeof(key));
  preferences.putUInt(key, task_mask);
}
void beginClock() {
  setenv("TZ", TODO_TIMEZONE, 1); tzset();
  today_anchor = buildDate();
  if (strlen(TODO_WIFI_SSID) == 0) return;
  WiFi.mode(WIFI_STA); WiFi.begin(TODO_WIFI_SSID, TODO_WIFI_PASSWORD);
}
void serviceClock() {
  if (millis() - last_clock_poll < 1000) return;
  last_clock_poll = millis();
  if (clock_is_synced) {
    const time_t now = time(nullptr);
    tm current = {}, previous = {};
    localtime_r(&now, &current); localtime_r(&today_anchor, &previous);
    if (now > 1700000000 && (current.tm_year != previous.tm_year || current.tm_yday != previous.tm_yday)) {
      saveDay(); today_anchor = now; scroll_offset = 0; loadDay(); render();
    }
    return;
  }
  if (strlen(TODO_WIFI_SSID) == 0 || WiFi.status() != WL_CONNECTED) return;
  if (!ntp_requested) {
    configTzTime(TODO_TIMEZONE, "time.cloudflare.com", "pool.ntp.org");
    ntp_requested = true;
  }
  time_t now = time(nullptr);
  if (now < 1700000000) return;  // NTP has not answered yet
  today_anchor = now;
  clock_is_synced = true;
  loadDay();
  render();
}
void serviceBattery() {
  const uint32_t now = millis();
  if (now - last_battery_ms < kBatteryIntervalMs) return;
  last_battery_ms = now;
  const Battery latest = readBattery();
  const bool changed = latest.present != battery.present || latest.percent != battery.percent;
  battery = latest;
  if (changed || stats_page) render();
}

bool readReg(uint8_t reg, uint8_t &value) {
  Wire1.beginTransmission(kExpanderAddress); Wire1.write(reg);
  if (Wire1.endTransmission(true) != 0 || Wire1.requestFrom(kExpanderAddress, 1) != 1) return false;
  value = Wire1.read(); return true;
}
bool writeReg(uint8_t reg, uint8_t value) {
  Wire1.beginTransmission(kExpanderAddress); Wire1.write(reg); Wire1.write(value);
  return Wire1.endTransmission(true) == 0;
}
bool expanderOutput(uint8_t pin, bool high) {
  uint8_t value; const uint8_t mask = 1u << pin;
  if (!readReg(0x01, value)) return false;
  high ? value |= mask : value &= ~mask;
  if (!writeReg(0x01, value) || !readReg(0x03, value)) return false;
  return writeReg(0x03, value & ~mask);  // set level before driving the pin
}
void setBrightness(uint8_t percent) {
  if (!percent) { digitalWrite(kBacklightPin, HIGH); return; }
  const uint8_t duty = 102 + (std::min<uint8_t>(percent, 100) - 1) * 153 / 99;
  analogWriteResolution(kBacklightPin, 8); analogWriteFrequency(kBacklightPin, 25000);
  analogWrite(kBacklightPin, 255 - duty);  // board PWM is inverted
}
Battery readBattery() {
  // The battery is connected through a 3:1 divider. Several ADC samples avoid
  // presenting the normal ESP32-S3 voltage noise as a jumping percentage.
  constexpr int kSamples = 24, kDiscardFirst = 2, kTrimEachEnd = 2;
  (void)analogRead(kBatteryAdcPin);
  analogSetPinAttenuation(kBatteryAdcPin, ADC_11db);
  delay(12);  // let the divider settle after the ADC is enabled

  uint32_t readings[kSamples] = {};
  int count = 0;
  bool calibrated = false;
  for (int sample = 0; sample < kSamples + kDiscardFirst; ++sample) {
    const uint32_t millivolts = analogReadMilliVolts(kBatteryAdcPin);
    if (millivolts > 0) calibrated = true;
    if (sample >= kDiscardFirst) readings[count++] = millivolts;
  }
  if (!calibrated) {
    for (int sample = 0; sample < count; ++sample) {
      readings[sample] = static_cast<uint32_t>(analogRead(kBatteryAdcPin)) * 3300u / 4095u;
    }
  }
  std::sort(readings, readings + count);
  uint32_t sum = 0;
  for (int sample = kTrimEachEnd; sample < count - kTrimEachEnd; ++sample) sum += readings[sample];

  Battery reading;
  const int kept = count - 2 * kTrimEachEnd;
  reading.volts = (static_cast<float>(sum) / kept) * 3.0f / 1000.0f;
  reading.present = reading.volts >= 2.5f && reading.volts <= 4.6f;
  if (!reading.present) return reading;

  // A lithium cell spends much of its usable life near 3.7 V, so percentage
  // follows a discharge curve rather than a misleading linear conversion.
  static constexpr struct { float volts; uint8_t percent; } kCurve[] = {
      {3.30f, 0},  {3.50f, 5},  {3.60f, 10}, {3.65f, 20}, {3.70f, 30}, {3.75f, 40},
      {3.79f, 50}, {3.85f, 60}, {3.92f, 70}, {4.00f, 80}, {4.10f, 90}, {4.15f, 100},
  };
  constexpr int kPoints = sizeof(kCurve) / sizeof(kCurve[0]);
  if (reading.volts <= kCurve[0].volts) {
    reading.percent = 0;
  } else if (reading.volts >= kCurve[kPoints - 1].volts) {
    reading.percent = 100;
  } else {
    for (int point = 1; point < kPoints; ++point) {
      if (reading.volts > kCurve[point].volts) continue;
      const float range = kCurve[point].volts - kCurve[point - 1].volts;
      const float ratio = (reading.volts - kCurve[point - 1].volts) / range;
      reading.percent = kCurve[point - 1].percent +
          static_cast<uint8_t>(ratio * (kCurve[point].percent - kCurve[point - 1].percent));
      break;
    }
  }
  return reading;
}

void flush() { panel->draw16bitRGBBitmap(0, 0, screen->getFramebuffer(), kPanelW, kPanelH); }
void label(const char *text, int x, int y, uint16_t color, uint8_t size = 1) {
  screen->setFont(nullptr); screen->setTextColor(color); screen->setTextSize(size); screen->setCursor(x, y); screen->print(text);
}
// Arduino_GFX wraps at the physical edge of the screen.  Task labels have a
// narrower usable area, so wrap them ourselves and can centre the result.
const char *nextTaskLine(const char *text, char *line, size_t line_size) {
  constexpr int kMaxChars = kTaskTextWidth / (6 * kTaskTextSize);
  while (*text == ' ') ++text;
  int used = 0;
  while (*text) {
    const char *word_end = text;
    while (*word_end && *word_end != ' ') ++word_end;
    const int word_length = word_end - text;
    const int needed = used ? word_length + 1 : word_length;
    if (used && used + needed > kMaxChars) break;
    if (!used && word_length > kMaxChars) {  // avoid an unbounded line for a future long word
      const int count = std::min(word_length, kMaxChars);
      memcpy(line, text, count); line[count] = '\0'; return text + count;
    }
    if (used) line[used++] = ' ';
    memcpy(line + used, text, word_length); used += word_length;
    text = word_end;
    while (*text == ' ') ++text;
  }
  line[used] = '\0';
  return text;
}
int taskLineCount(const char *text) {
  char line[20]; int lines = 0;
  while (*text) { text = nextTaskLine(text, line, sizeof(line)); ++lines; }
  return lines;
}
int cardHeightForTask(int task) {
  return std::max(kCardMinHeight, taskLineCount(kTasks[task]) * kTaskLineHeight + 24);
}
void drawTaskLabel(const char *text, int y, int card_height, uint16_t color) {
  const int lines = taskLineCount(text);
  // The default Arduino_GFX font cursor is its top-left corner, not a baseline.
  // Centre the complete text block in the card.
  int top = y + (card_height - lines * kTaskLineHeight) / 2;
  char line[20];
  screen->setFont(nullptr); screen->setTextColor(color); screen->setTextSize(kTaskTextSize);
  screen->setTextWrap(false);
  while (*text) {
    text = nextTaskLine(text, line, sizeof(line));
    screen->setCursor(kTaskTextX, top); screen->print(line);
    top += kTaskLineHeight;
  }
}
void dayLabel(char *out, size_t size) {
  tm date = {}; const time_t selected = selectedDate(); localtime_r(&selected, &date);
  strftime(out, size, "%a %b %e", &date);
}
int displayedContentHeight() {
  int height = 0;
  for (int index = 0; index < displayedTaskCount(); ++index) {
    const int task = taskAtDisplayIndex(index);
    if (task < 0) break;
    height += cardHeightForTask(task) + kCardGap;
  }
  return height ? height - kCardGap : 0;
}
int maximumScrollOffset() { return std::max(0, displayedContentHeight() - kListHeight); }
void clampScrollOffset() { scroll_offset = std::clamp(scroll_offset, 0, maximumScrollOffset()); }
int taskAtListY(int y) {
  const int content_y = y - kListTop + scroll_offset;
  int top = 0;
  for (int index = 0; index < displayedTaskCount(); ++index) {
    const int task = taskAtDisplayIndex(index);
    if (task < 0) break;
    const int height = cardHeightForTask(task);
    if (content_y >= top && content_y < top + height) return task;
    top += height + kCardGap;
  }
  return -1;
}
uint16_t batteryColor() {
  if (!battery.present) return kMuted;
  if (battery.percent <= 15) return kNotDoneCard;
  if (battery.percent <= 35) return 0xFD20;
  return kText;
}
void drawBattery(int x, int y) {
  const uint16_t color = batteryColor();
  char percent[5];
  if (battery.present) snprintf(percent, sizeof(percent), "%u%%", battery.percent);
  else strcpy(percent, "--");
  label(percent, x - strlen(percent) * 6 - 3, y + 1, color, 1);
  screen->drawRoundRect(x, y, 17, 10, 2, color);
  screen->fillRect(x + 17, y + 3, 2, 4, color);
  if (battery.present) {
    const int fill = std::max(1, battery.percent * 13 / 100);
    screen->fillRect(x + 2, y + 2, fill, 6, color);
  } else {
    screen->drawLine(x + 3, y + 7, x + 13, y + 2, color);
  }
}
void headerButton(int x, const char *text, bool highlighted = false) {
  constexpr int kButtonY = 45, kButtonW = 52, kButtonH = 43;
  screen->fillRoundRect(x, kButtonY, kButtonW, kButtonH, 7, highlighted ? kAccent : kCard);
  screen->drawRoundRect(x, kButtonY, kButtonW, kButtonH, 7, highlighted ? kText : kMuted);
  label(text, x + (kButtonW - strlen(text) * 6) / 2, kButtonY + 17, kText, 1);
}
int completedCount() {
  return completedInMask(task_mask);
}
void statsTab(int y, const char *text, bool selected) {
  constexpr int kTabX = 12, kTabW = 108, kTabH = 42;
  screen->fillRoundRect(kTabX, y, kTabW, kTabH, 7, selected ? kAccent : kCard);
  screen->drawRoundRect(kTabX, y, kTabW, kTabH, 7, selected ? kText : kMuted);
  label(text, kTabX + (kTabW - strlen(text) * 6) / 2, y + 17, kText, 1);
}
void statsBar(int x, int width, int done, int not_done, int maximum, const char *caption) {
  constexpr int kPlotTop = 20, kPlotBottom = 140;
  const int done_height = maximum ? done * (kPlotBottom - kPlotTop) / maximum : 0;
  const int not_done_height = maximum ? not_done * (kPlotBottom - kPlotTop) / maximum : 0;
  screen->fillRoundRect(x, kPlotTop, width, kPlotBottom - kPlotTop, 4, kCard);
  if (done_height) screen->fillRoundRect(x, kPlotBottom - done_height, width, done_height, 4, kDoneCard);
  if (not_done_height) screen->fillRoundRect(x, kPlotBottom - done_height - not_done_height, width, not_done_height, 4, kNotDoneCard);
  char number[12]; snprintf(number, sizeof(number), "%d/%d", done, not_done);
  const int used_height = done_height + not_done_height;
  const int number_y = std::max(kPlotTop + 3, kPlotBottom - used_height - 10);
  label(number, x + (width - strlen(number) * 6) / 2, number_y, kText, 1);
  label(caption, x + (width - strlen(caption) * 6) / 2, 151, kMuted, 1);
}
void renderStats() {
  screen->fillScreen(kBackground);
  screen->fillRect(0, 0, 132, kStatsHeight, 0x0821);
  char date[16]; dayLabel(date, sizeof(date));
  label("STATS", 12, 9, kText, 2);
  char battery_text[20];
  if (battery.present) snprintf(battery_text, sizeof(battery_text), "BAT %u%% %.2fV", battery.percent, battery.volts);
  else snprintf(battery_text, sizeof(battery_text), "BATTERY --");
  label(battery_text, 12, 31, batteryColor(), 1);
  statsTab(44, "DAY", stats_mode == StatsMode::Days);
  statsTab(96, "WEEK", stats_mode == StatsMode::Weeks);
  screen->fillRoundRect(12, 151, 7, 7, 2, kDoneCard);
  label("DONE", 23, 151, kMuted, 1);
  screen->fillRoundRect(70, 151, 7, 7, 2, kNotDoneCard);
  label("NOT", 81, 151, kMuted, 1);
  label(date, 145, 5, kMuted, 1);

  if (stats_mode == StatsMode::Days) {
    for (int bar = 0; bar < 7; ++bar) {
      const int offset = selected_day_offset + bar - 6;
      const uint32_t mask = maskAtOffset(offset);
      const int complete = completedInMask(mask);
      const int missed = notDoneInMask(mask);
      tm local = {}; const time_t day = dateAtOffset(offset); localtime_r(&day, &local);
      char caption[4]; strftime(caption, sizeof(caption), "%a", &local);
      statsBar(146 + bar * 68, 52, complete, missed, kTaskCount, caption);
    }
  } else {
    tm selected = {}; const time_t anchor = selectedDate(); localtime_r(&anchor, &selected);
    const int monday_offset = (selected.tm_wday + 6) % 7;
    for (int bar = 0; bar < 6; ++bar) {
      const int weeks_ago = 5 - bar;
      const int start = selected_day_offset - monday_offset - weeks_ago * 7;
      const int days_in_week = weeks_ago == 0 ? monday_offset + 1 : 7;
      int complete = 0, missed = 0;
      for (int day = 0; day < days_in_week; ++day) {
        const uint32_t mask = maskAtOffset(start + day);
        complete += completedInMask(mask);
        missed += notDoneInMask(mask);
      }
      const int possible = days_in_week * kTaskCount;
      char caption[8];
      if (weeks_ago == 0) snprintf(caption, sizeof(caption), "THIS");
      else snprintf(caption, sizeof(caption), "-%dw", weeks_ago);
      statsBar(150 + bar * 76, 60, complete, missed, possible, caption);
    }
  }
  flush();
}
int displayedTaskCount() {
  if (!filter_active) return kTaskCount;
  int displayed = 0;
  for (int task = 0; task < kTaskCount; ++task) if (!taskMarkedInMask(filtered_out_mask, task)) ++displayed;
  return displayed;
}
int taskAtDisplayIndex(int index) {
  for (int task = 0; task < kTaskCount; ++task) {
    if (filter_active && taskMarkedInMask(filtered_out_mask, task)) continue;
    if (index-- == 0) return task;
  }
  return -1;
}
bool hasMarkedDisplayedTask() {
  const int displayed = displayedTaskCount();
  for (int index = 0; index < displayed; ++index) {
    const int task = taskAtDisplayIndex(index);
    if (task >= 0 && taskMarkedInMask(task_mask, task)) return true;
  }
  return false;
}
void filterButton(int x, int width, const char *text, bool selected, bool enabled = true) {
  const uint16_t fill = selected ? kAccent : (enabled ? kCard : 0x18C3);
  const uint16_t ink = enabled ? kText : 0x5AEB;
  screen->fillRoundRect(x, kFilterTop, width, kFilterHeight, 7, fill);
  screen->drawRoundRect(x, kFilterTop, width, kFilterHeight, 7, selected ? kText : (enabled ? kMuted : 0x39E7));
  label(text, x + (width - strlen(text) * 6) / 2, kFilterTop + (kFilterHeight - 8) / 2, ink, 1);
}
void render() {
  if (stats_page) { renderStats(); return; }
  clampScrollOffset();
  screen->fillScreen(kBackground);
  const int displayed = displayedTaskCount();
  int card_y = kListTop - scroll_offset;
  for (int index = 0; index < displayed; ++index) {
    const int task = taskAtDisplayIndex(index);
    if (task < 0) break;
    const int card_height = cardHeightForTask(task);
    if (card_y < kListBottom && card_y + card_height > kListTop) {
      const uint16_t card_color = taskDone(task) ? kDoneCard : (taskNotDone(task) ? kNotDoneCard : kCard);
      screen->fillRoundRect(8, card_y, 156, card_height, 8, card_color);
      drawTaskLabel(kTasks[task], card_y, card_height, kText);
    }
    card_y += card_height + kCardGap;
  }

  // Cards may be partly above or below the viewport while dragging. Redrawing
  // these fixed regions afterwards clips them without a costly extra canvas.
  screen->fillRect(0, 0, kWidth, kHeaderHeight, 0x0821);
  screen->fillRect(0, kHeaderHeight, kWidth, kListTop - kHeaderHeight, kBackground);
  char date[16]; dayLabel(date, sizeof(date));
  label(date, std::max(2, (122 - static_cast<int>(strlen(date)) * 12) / 2), 13, kText, 2);
  drawBattery(149, 17);
  headerButton(3, "PREV");
  headerButton(60, "TODAY", selected_day_offset == 0);
  headerButton(117, "NEXT");
  screen->fillRect(0, kListBottom, kWidth, kHeight - kListBottom, kBackground);

  const int content_height = displayedContentHeight();
  const int max_scroll = maximumScrollOffset();
  const int bar_height = content_height > kListHeight
      ? std::max(24, kListHeight * kListHeight / content_height) : kListHeight;
  const int bar_y = kListTop + (max_scroll ? scroll_offset * (kListHeight - bar_height) / max_scroll : 0);
  screen->fillRoundRect(166, kListTop, 3, kListHeight, 1, 0x2945);
  screen->fillRoundRect(166, bar_y, 3, bar_height, 1, kAccent);
  if (filter_active && displayed == 0) label("FILTERED LIST IS EMPTY", 22, 330, kText, 1);
  const bool can_filter = hasMarkedDisplayedTask();
  filterButton(4, kAllButtonWidth, "ALL", !filter_active);
  // FILTER is an action that refreshes the hidden-task snapshot, not a mode.
  filterButton(113, kFilterButtonWidth, "FILTER", false, can_filter);
  flush();
}

bool touchReady() { uint8_t input; return readReg(0x00, input) && !(input & (1u << kTouchIrq)); }
bool readTouch(bool &down) {
  constexpr uint8_t command[] = {0xB5, 0xAB, 0xA5, 0x5A, 0, 0, 0, 8, 0, 0, 0};
  uint8_t data[8]; Wire.beginTransmission(kTouchAddress); Wire.write(command, sizeof(command));
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(kTouchAddress, 8) != 8) return false;
  for (uint8_t &byte : data) byte = Wire.read();
  if (data[0] != 0 || data[1] == 0 || data[1] > 4) { down = false; return true; }
  const uint16_t raw_long = ((data[2] & 0x0F) << 8) | data[3];
  const uint16_t raw_short = ((data[4] & 0x0F) << 8) | data[5];
  const int native_x = std::min<int>(raw_short, kPanelW - 1);
  const int native_y = raw_long >= kPanelH ? 0 : kPanelH - 1 - raw_long;
  if (stats_page) {
    touch_x = native_y; touch_y = kPanelW - 1 - native_x;  // landscape rotation 1
  } else {
    touch_x = native_x; touch_y = native_y;                 // portrait rotation 0
  }
  down = true; return true;
}
void finishTouch(int start_x, int start_y, int end_x, int end_y, bool was_drag) {
  if (stats_page) {
    if (!was_drag && std::abs(end_y - start_y) <= 18 && start_x <= 132) {
      if (start_y >= 40 && start_y <= 90) stats_mode = StatsMode::Days;
      else if (start_y >= 92 && start_y <= 144) stats_mode = StatsMode::Weeks;
      else return;
      render();
    }
    return;
  }
  if (was_drag) return;  // the list already followed this gesture live
  if (std::abs(end_y - start_y) <= 18 && start_y >= 43 && start_y < kHeaderHeight) {
    if (start_x < 57) { saveDay(); --selected_day_offset; scroll_offset = 0; loadDay(); }
    else if (start_x < 115) { saveDay(); selected_day_offset = 0; scroll_offset = 0; loadDay(); }
    else { saveDay(); ++selected_day_offset; scroll_offset = 0; loadDay(); }
    filter_active = false;
    filtered_out_mask = 0;
  } else if (std::abs(end_y - start_y) <= 18 && start_y >= kFilterHitTop) {
    if (start_x < 113) {
      filter_active = false;
      filtered_out_mask = 0;
    } else if (hasMarkedDisplayedTask()) {
      filter_active = true;
      filtered_out_mask = task_mask;
    } else {
      return;
    }
    scroll_offset = 0;
  } else if (std::abs(end_y - start_y) <= 18 && start_y >= kListTop && start_y < kListBottom) {
    const int task = taskAtListY(end_y);
    if (task < 0) return;  // a gap between cards is deliberately inert
    cycleTaskState(task);
    saveDay();
  } else return;
  render();
}

// On battery SYS_EN dropping ends execution. On USB it cannot remove power, so
// use light sleep and let the same PWR button wake the already-rendered app.
void powerDown() {
  screen->fillScreen(kBackground);
  label("POWERING OFF", stats_page ? 210 : 20, stats_page ? 74 : 300, kText, 2);
  flush();
  setBrightness(0);
  expanderOutput(kBacklightRail, false);
  expanderOutput(kSysEn, false);
  delay(150);

  // Still running means USB is supplying the board.
  while (digitalRead(16) == LOW) delay(10);
  delay(60);
  uint8_t discard = 0;
  readReg(0x00, discard);  // clear the latched touch interrupt before sleeping

  const gpio_num_t wake_pin = GPIO_NUM_16;
  gpio_wakeup_enable(wake_pin, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  const esp_err_t result = esp_light_sleep_start();
  gpio_wakeup_disable(wake_pin);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);

  if (result != ESP_OK) {  // preserve the apparent-off state if sleep is refused
    while (digitalRead(16) == HIGH) delay(50);
  }
  while (digitalRead(16) == LOW) delay(10);  // drain the wake-button release
  expanderOutput(kSysEn, true);
  expanderOutput(kBacklightRail, true);
  setBrightness(85);
  render();
}
void toggleStats() {
  stats_page = !stats_page;
  screen = stats_page ? stats_screen : portrait_screen;
  touch_held = false; press_started_in_list = false; touch_dragging = false;
  render();
}
void handleButtons() {
  const uint32_t now = millis();
  if (digitalRead(16) == LOW) {
    if (power_pressed_ms == 0) power_pressed_ms = now;
    if (now - power_pressed_ms >= kPowerHoldMs) {
      power_pressed_ms = 0;
      powerDown();
    }
    return;
  }
  power_pressed_ms = 0;

  const bool boot_down = digitalRead(0) == LOW;
  if (boot_down == boot_was_down || now - boot_changed_ms < 40) return;
  boot_was_down = boot_down;
  boot_changed_ms = now;
  if (boot_down) toggleStats();
}
}  // namespace

void setup() {
  Serial.begin(115200);
  pinMode(0, INPUT_PULLUP); pinMode(16, INPUT_PULLUP); pinMode(kTouchIrqPin, INPUT_PULLUP);
  pinMode(kBacklightPin, OUTPUT); digitalWrite(kBacklightPin, HIGH);
  Wire.begin(17, 18, 300000); Wire.setTimeOut(10);
  Wire1.begin(47, 48, 300000); Wire1.setTimeOut(10);
  if (!expanderOutput(kSysEn, true)) log_e("could not latch battery power");
  preferences.begin("todo", false);
  beginClock();
  loadDay();
  if (kResetPin < 0) expanderOutput(kLcdReset, true); else { pinMode(kResetPin, OUTPUT); digitalWrite(kResetPin, HIGH); }
  delay(20);
  bus = new Arduino_ESP32QSPI(9, 10, 11, 12, 13, 14, false);
  panel = new Arduino_AXS15231B(bus, kResetPin, 0, false, kPanelW, kPanelH, 0, 0, 0, 0);
  portrait_screen = new PsramCanvas(kPanelW, kPanelH, panel, 0, 0, 0);
  stats_screen = new PsramCanvas(kPanelW, kPanelH, panel, 0, 0, 1);
  if (!portrait_screen->begin(40000000) || !stats_screen->begin(GFX_SKIP_OUTPUT_BEGIN)) {
    log_e("display or PSRAM initialization failed"); return;
  }
  screen = portrait_screen;
  battery = readBattery();
  last_battery_ms = millis();
  expanderOutput(kBacklightRail, true); setBrightness(85); render();
}

void loop() {
  handleButtons();
  serviceClock();
  serviceBattery();
  const uint32_t now = millis();
  if (now - last_ready_poll < 5) return;
  last_ready_poll = now;
  const bool ready = touchReady();
  if (!ready && !touch_held) return;
  if (now - last_touch_read < 30) return;
  last_touch_read = now;
  bool down = false;
  const bool read_ok = readTouch(down);
  if (read_ok && down) {
    last_contact_ms = now;
    if (!touch_held) {
      touch_held = true;
      press_x = touch_x; press_y = touch_y; last_drag_y = touch_y;
      press_started_in_list = !stats_page && touch_y >= kListTop && touch_y < kListBottom;
      touch_dragging = false;
    } else if (press_started_in_list) {
      if (!touch_dragging && std::abs(touch_y - press_y) >= kTouchDragThreshold) {
        touch_dragging = true;
        last_drag_y = press_y;
      }
      if (touch_dragging) {
        const int before = scroll_offset;
        scroll_offset -= touch_y - last_drag_y;
        clampScrollOffset();
        last_drag_y = touch_y;
        if (scroll_offset != before) render();
      }
    }
    return;
  }
  // The controller occasionally skips a report while a finger is down.  Wait
  // briefly before treating it as a release so ordinary taps remain reliable.
  if (touch_held && now - last_contact_ms > 60) {
    touch_held = false;
    finishTouch(press_x, press_y, touch_x, touch_y, touch_dragging);
    press_started_in_list = false;
    touch_dragging = false;
  }
}
