/*
 * Water AI — ESP32 Cheap Yellow Display (ESP32-2432S028R)
 *
 * Water reminder with cloud sync:
 *   - "Next water in" countdown (4 h interval), DRINK NOW button, animated glass
 *   - Multi-network Wi-Fi (home / office / phone hotspot) + NTP real clock
 *   - Every DRINK NOW press is queued in flash and uploaded to Cloud Firestore,
 *     so presses are never lost even when offline
 *   - Daily count and next-due time survive power loss
 *
 * UI runs on core 1 (Arduino loop); networking runs on core 0.
 * LVGL is only ever touched from the UI core; the net task communicates
 * through a FreeRTOS queue and volatile status flags.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>
#include <vector>

#include <lvgl.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

#include "secrets.h"

/* ---------------- Display & touch hardware ---------------- */

static const uint16_t SCREEN_W = 240;
static const uint16_t SCREEN_H = 320;

#define XPT2046_IRQ 36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33

TFT_eSPI tft = TFT_eSPI();
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf1[SCREEN_W * 40];

static void disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)&color_p->full, w * h, true);
  tft.endWrite();
  lv_disp_flush_ready(disp);
}

static const int TOUCH_X_MIN = 200, TOUCH_X_MAX = 3700;
static const int TOUCH_Y_MIN = 240, TOUCH_Y_MAX = 3800;

static void touch_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  if (ts.tirqTouched() && ts.touched()) {
    TS_Point p = ts.getPoint();
    int x = map(p.x, TOUCH_X_MIN, TOUCH_X_MAX, 0, SCREEN_W);
    int y = map(p.y, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, SCREEN_H);
    data->point.x = constrain(x, 0, SCREEN_W - 1);
    data->point.y = constrain(y, 0, SCREEN_H - 1);
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

/* ---------------- Shared state (UI core <-> net core) ---------------- */

enum NetState : uint8_t {
  NET_OFFLINE = 0,   // no Wi-Fi
  NET_CONNECTING,    // trying networks
  NET_ONLINE,        // Wi-Fi up, uploads pending
  NET_SYNCED,        // Wi-Fi up, everything uploaded
};

static volatile NetState g_net_state = NET_OFFLINE;
static volatile bool g_time_synced = false;
static volatile int g_pending_count = 0;

static QueueHandle_t drink_queue;  // carries time_t of each DRINK NOW press (0 = time unknown)

/* ---------------- UI state ---------------- */

static const int32_t DOSE_INTERVAL_S = 4 * 3600;  // drink reminder every 4 hours
static int32_t remaining_s = 4 * 3600;
static int32_t water_count = 0;  // glasses today

static Preferences ui_prefs;  // namespace "ui": daily count, date, next-due epoch
static bool due_restored = false;

static lv_obj_t *countdown_label;
static lv_obj_t *clock_label;
static lv_obj_t *take_btn;
static lv_obj_t *water_label;
static lv_obj_t *wifi_icon;

static const lv_color_t COLOR_BG = lv_color_hex(0xF3F6FB);
static const lv_color_t COLOR_TEXT_DARK = lv_color_hex(0x1A1F2E);
static const lv_color_t COLOR_TEXT_MUTED = lv_color_hex(0x7C8DB0);
static const lv_color_t COLOR_BTN_IDLE = lv_color_hex(0xADC6EA);
static const lv_color_t COLOR_BTN_READY = lv_color_hex(0x2F6BE5);

static void update_countdown_label() {
  int h = remaining_s / 3600;
  int m = (remaining_s % 3600) / 60;
  int s = remaining_s % 60;
  lv_label_set_text_fmt(countdown_label, "%02d:%02d:%02d", h, m, s);
}

/* Current local date as YYYY-MM-DD ("" when clock not yet synced) */
static String today_str() {
  if (!g_time_synced) return String("");
  time_t now = time(nullptr);
  struct tm tm_local;
  localtime_r(&now, &tm_local);
  char buf[12];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm_local.tm_year + 1900, tm_local.tm_mon + 1,
           tm_local.tm_mday);
  return String(buf);
}

/* Reset the on-screen counter when the day rolls over */
static void check_day_rollover() {
  String today = today_str();
  if (today.length() == 0) return;
  String stored = ui_prefs.getString("date", "");
  if (stored != today) {
    ui_prefs.putString("date", today);
    ui_prefs.putInt("cnt", 0);
    water_count = 0;
    lv_label_set_text_fmt(water_label, "%d Water", (int)water_count);
  }
}

static void countdown_tick(lv_timer_t *timer) {
  (void)timer;

  /* Once real time arrives, restore the schedule saved before power-off */
  if (g_time_synced && !due_restored) {
    due_restored = true;
    time_t due = (time_t)ui_prefs.getLong64("due", 0);
    time_t now = time(nullptr);
    if (due > now) {
      remaining_s = (int32_t)(due - now);
    } else if (due != 0) {
      remaining_s = 0;
    }
    /* Restore today's count if the stored date is still today */
    if (ui_prefs.getString("date", "") == today_str()) {
      water_count = ui_prefs.getInt("cnt", 0);
      lv_label_set_text_fmt(water_label, "%d Water", (int)water_count);
    }
    check_day_rollover();
  }

  if (remaining_s > 0) {
    remaining_s--;
    if (remaining_s == 0) {
      lv_obj_set_style_bg_color(take_btn, COLOR_BTN_READY, 0);
    }
  }
  update_countdown_label();

  /* Top bar clock: real time when synced, boot-relative otherwise */
  if (g_time_synced) {
    time_t now = time(nullptr);
    struct tm tm_local;
    localtime_r(&now, &tm_local);
    int h12 = tm_local.tm_hour % 12;
    if (h12 == 0) h12 = 12;
    lv_label_set_text_fmt(clock_label, "%d:%02d %s", h12, tm_local.tm_min,
                          tm_local.tm_hour >= 12 ? "PM" : "AM");
    if (tm_local.tm_hour == 0 && tm_local.tm_min == 0) check_day_rollover();
  } else {
    uint32_t mins = 11 * 60 + 37 + millis() / 60000;
    int ch = (mins / 60) % 24, cm = mins % 60;
    const char *ampm = (ch >= 12) ? "PM" : "AM";
    int ch12 = ch % 12;
    if (ch12 == 0) ch12 = 12;
    lv_label_set_text_fmt(clock_label, "%d:%02d %s", ch12, cm, ampm);
  }

  /* Wi-Fi / sync status icon */
  switch (g_net_state) {
    case NET_SYNCED:
      lv_obj_set_style_text_color(wifi_icon, lv_color_hex(0x2FA45B), 0);  // green: all uploaded
      break;
    case NET_ONLINE:
      lv_obj_set_style_text_color(wifi_icon, lv_color_hex(0xE8A13C), 0);  // amber: uploading
      break;
    case NET_CONNECTING:
      lv_obj_set_style_text_color(wifi_icon, lv_color_hex(0x9AA8C0), 0);  // gray-blue
      break;
    default:
      lv_obj_set_style_text_color(wifi_icon, lv_color_hex(0xC7CFDC), 0);  // pale: offline
  }
}

static void take_now_cb(lv_event_t *e) {
  (void)e;
  static uint32_t last_press_ms = 0;
  if (millis() - last_press_ms < 3000) return;  // ignore accidental double taps
  last_press_ms = millis();

  check_day_rollover();
  water_count++;
  lv_label_set_text_fmt(water_label, "%d Water", (int)water_count);
  ui_prefs.putInt("cnt", water_count);
  String today = today_str();
  if (today.length()) ui_prefs.putString("date", today);

  remaining_s = DOSE_INTERVAL_S;
  update_countdown_label();
  lv_obj_set_style_bg_color(take_btn, COLOR_BTN_IDLE, 0);

  /* Persist next-due and queue the event for cloud upload */
  time_t now = g_time_synced ? time(nullptr) : 0;
  if (now) ui_prefs.putLong64("due", (int64_t)(now + DOSE_INTERVAL_S));
  xQueueSend(drink_queue, &now, 0);
}

/* Menu icon: restart the countdown at 1 minute (testing helper) */
static void menu_cb(lv_event_t *e) {
  (void)e;
  remaining_s = 60;
  update_countdown_label();
  lv_obj_set_style_bg_color(take_btn, COLOR_BTN_IDLE, 0);
  if (g_time_synced) ui_prefs.putLong64("due", (int64_t)(time(nullptr) + 60));
}

/* Water level animation: grow/shrink the water rect, anchored to the bottom */
static void water_level_anim(void *var, int32_t v) {
  lv_obj_t *water = (lv_obj_t *)var;
  lv_obj_set_height(water, v);
  lv_obj_align(water, LV_ALIGN_BOTTOM_MID, 0, -4);
}

static void bubble_anim(void *var, int32_t v) {
  lv_obj_set_y((lv_obj_t *)var, v);
}

static lv_obj_t *make_rect(lv_obj_t *parent, int w, int h, lv_color_t color, int radius) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_color(o, color, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_radius(o, radius, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  return o;
}

static void build_ui() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, COLOR_BG, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  /* ---- Top bar ---- */
  lv_obj_t *menu = lv_label_create(scr);
  lv_label_set_text(menu, LV_SYMBOL_LIST);
  lv_obj_set_style_text_color(menu, COLOR_TEXT_DARK, 0);
  lv_obj_align(menu, LV_ALIGN_TOP_LEFT, 12, 10);
  lv_obj_add_flag(menu, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(menu, 14);
  lv_obj_add_event_cb(menu, menu_cb, LV_EVENT_CLICKED, NULL);

  clock_label = lv_label_create(scr);
  lv_label_set_text(clock_label, "--:--");
  lv_obj_set_style_text_font(clock_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(clock_label, COLOR_TEXT_DARK, 0);
  lv_obj_align(clock_label, LV_ALIGN_TOP_MID, 0, 10);

  wifi_icon = lv_label_create(scr);
  lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI);
  lv_obj_set_style_text_color(wifi_icon, lv_color_hex(0xC7CFDC), 0);
  lv_obj_align(wifi_icon, LV_ALIGN_TOP_RIGHT, -36, 10);

  lv_obj_t *bell = lv_label_create(scr);
  lv_label_set_text(bell, LV_SYMBOL_BELL);
  lv_obj_set_style_text_color(bell, COLOR_TEXT_DARK, 0);
  lv_obj_align(bell, LV_ALIGN_TOP_RIGHT, -12, 10);

  /* ---- "Next water in" + countdown ---- */
  lv_obj_t *subtitle = lv_label_create(scr);
  lv_label_set_text(subtitle, "Next water in");
  lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(subtitle, COLOR_TEXT_MUTED, 0);
  lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 34);

  countdown_label = lv_label_create(scr);
  lv_label_set_text(countdown_label, "04:00:00");
  lv_obj_set_style_text_font(countdown_label, &lv_font_montserrat_44, 0);
  lv_obj_set_style_text_color(countdown_label, COLOR_TEXT_DARK, 0);
  lv_obj_align(countdown_label, LV_ALIGN_TOP_MID, 0, 54);

  /* ---- Animated water glass card ---- */
  lv_obj_t *card = make_rect(scr, 200, 116, lv_color_hex(0xE9EEF5), 16);
  lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 106);

  lv_obj_t *glass = make_rect(card, 64, 94, lv_color_white(), 8);
  lv_obj_align(glass, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_border_width(glass, 3, 0);
  lv_obj_set_style_border_color(glass, lv_color_hex(0x9FB3CC), 0);

  lv_obj_t *water = make_rect(glass, 52, 40, lv_color_hex(0x3D9BE9), 5);
  lv_obj_align(water, LV_ALIGN_BOTTOM_MID, 0, -4);

  lv_anim_t wa;
  lv_anim_init(&wa);
  lv_anim_set_var(&wa, water);
  lv_anim_set_exec_cb(&wa, water_level_anim);
  lv_anim_set_values(&wa, 34, 64);
  lv_anim_set_time(&wa, 1800);
  lv_anim_set_playback_time(&wa, 1800);
  lv_anim_set_repeat_count(&wa, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&wa, lv_anim_path_ease_in_out);
  lv_anim_start(&wa);

  static const int bubble_x[3] = {12, 28, 42};
  static const int bubble_delay[3] = {0, 700, 1400};
  for (int i = 0; i < 3; i++) {
    lv_obj_t *bubble = make_rect(glass, 7, 7, lv_color_hex(0xBFE2F8), LV_RADIUS_CIRCLE);
    lv_obj_set_pos(bubble, bubble_x[i], 72);
    lv_anim_t ba;
    lv_anim_init(&ba);
    lv_anim_set_var(&ba, bubble);
    lv_anim_set_exec_cb(&ba, bubble_anim);
    lv_anim_set_values(&ba, 72, 14);
    lv_anim_set_time(&ba, 2200);
    lv_anim_set_delay(&ba, bubble_delay[i]);
    lv_anim_set_repeat_count(&ba, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&ba, lv_anim_path_ease_out);
    lv_anim_start(&ba);
  }

  /* ---- Water counter ---- */
  water_label = lv_label_create(scr);
  lv_label_set_text(water_label, "0 Water");
  lv_obj_set_style_text_font(water_label, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(water_label, COLOR_TEXT_DARK, 0);
  lv_obj_align(water_label, LV_ALIGN_TOP_LEFT, 14, 240);

  /* ---- DRINK NOW button ---- */
  take_btn = lv_btn_create(scr);
  lv_obj_set_size(take_btn, 216, 40);
  lv_obj_align(take_btn, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_obj_set_style_bg_color(take_btn, COLOR_BTN_IDLE, 0);
  lv_obj_set_style_radius(take_btn, 8, 0);
  lv_obj_set_style_shadow_width(take_btn, 0, 0);
  lv_obj_add_event_cb(take_btn, take_now_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *btn_label = lv_label_create(take_btn);
  lv_label_set_text(btn_label, "DRINK NOW");
  lv_obj_set_style_text_font(btn_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(btn_label, lv_color_white(), 0);
  lv_obj_center(btn_label);

  lv_timer_create(countdown_tick, 1000, NULL);
}

/* ================= Networking (runs on core 0) ================= */

static Preferences net_prefs;               // namespace "net": pending queue, refresh token
static std::vector<int64_t> pending_events; // epoch seconds (0 = stamp at upload time)
static String id_token;
static uint32_t token_expiry_ms = 0;

static void save_pending() {
  String s;
  for (size_t i = 0; i < pending_events.size(); i++) {
    if (i) s += ",";
    s += String((long long)pending_events[i]);
  }
  net_prefs.putString("pending", s);
  g_pending_count = pending_events.size();
}

static void load_pending() {
  String s = net_prefs.getString("pending", "");
  pending_events.clear();
  int start = 0;
  while (start < (int)s.length()) {
    int comma = s.indexOf(',', start);
    if (comma < 0) comma = s.length();
    pending_events.push_back(atoll(s.substring(start, comma).c_str()));
    start = comma + 1;
  }
  g_pending_count = pending_events.size();
}

static bool wifi_try_connect() {
  /* Show what the board can actually see (2.4 GHz only) */
  int n = WiFi.scanNetworks();
  Serial.printf("[net] scan found %d networks:\n", n);
  for (int i = 0; i < n && i < 10; i++) {
    Serial.printf("[net]   '%s' (ch %d, %d dBm)\n", WiFi.SSID(i).c_str(), WiFi.channel(i),
                  WiFi.RSSI(i));
  }
  WiFi.scanDelete();

  for (int i = 0; i < WIFI_NETWORK_COUNT; i++) {
    if (strncmp(WIFI_NETWORKS[i].ssid, "YOUR_", 5) == 0) continue;  // unfilled placeholder
    Serial.printf("[net] trying '%s'...\n", WIFI_NETWORKS[i].ssid);
    WiFi.begin(WIFI_NETWORKS[i].ssid, WIFI_NETWORKS[i].pass);
    // Reduce TX power: full-power Wi-Fi bursts brown out the CYD's USB supply
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
    uint32_t t0 = millis();
    while (millis() - t0 < 10000) {
      if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[net] connected to %s, IP %s\n", WIFI_NETWORKS[i].ssid,
                      WiFi.localIP().toString().c_str());
        return true;
      }
      vTaskDelay(pdMS_TO_TICKS(250));
    }
    Serial.printf("[net] failed, status=%d\n", (int)WiFi.status());
    WiFi.disconnect(true);
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  return false;
}

static void sync_time() {
  configTzTime(TZ_INFO, "pool.ntp.org", "time.nist.gov");
  for (int i = 0; i < 40; i++) {  // up to 10 s
    time_t now = time(nullptr);
    if (now > 1600000000) {  // sanity: after Sep 2020
      g_time_synced = true;
      Serial.printf("[net] time synced: %ld\n", (long)now);
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

/* Get a Firebase ID token via anonymous auth (reuses the same anonymous
 * user across reboots through the saved refresh token). */
static bool ensure_token() {
  if (id_token.length() && millis() < token_expiry_ms) return true;

  WiFiClientSecure client;
  client.setInsecure();  // skip cert pinning; fine for this personal project
  HTTPClient http;
  String refresh = net_prefs.getString("rtok", "");
  int code = 0;
  String body;

  if (refresh.length()) {
    http.begin(client, "https://securetoken.googleapis.com/v1/token?key=" FIREBASE_API_KEY);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    code = http.POST("grant_type=refresh_token&refresh_token=" + refresh);
    body = http.getString();
    http.end();
    if (code == 200) {
      JsonDocument doc;
      if (deserializeJson(doc, body) == DeserializationError::Ok) {
        id_token = doc["id_token"].as<String>();
        net_prefs.putString("rtok", doc["refresh_token"].as<String>());
        token_expiry_ms = millis() + 50UL * 60UL * 1000UL;
        return true;
      }
    }
    Serial.printf("[net] token refresh failed (%d), signing up fresh\n", code);
  }

  http.begin(client,
             "https://identitytoolkit.googleapis.com/v1/accounts:signUp?key=" FIREBASE_API_KEY);
  http.addHeader("Content-Type", "application/json");
  code = http.POST("{\"returnSecureToken\":true}");
  body = http.getString();
  http.end();
  if (code != 200) {
    Serial.printf("[net] anonymous sign-up failed: %d %s\n", code, body.c_str());
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) return false;
  id_token = doc["idToken"].as<String>();
  net_prefs.putString("rtok", doc["refreshToken"].as<String>());
  token_expiry_ms = millis() + 50UL * 60UL * 1000UL;
  Serial.println("[net] signed in anonymously");
  return true;
}

static bool upload_event(int64_t epoch) {
  if (epoch == 0) epoch = (int64_t)time(nullptr);  // press happened before clock sync
  struct tm tm_utc;
  time_t t = (time_t)epoch;
  gmtime_r(&t, &tm_utc);
  char iso[24];
  snprintf(iso, sizeof(iso), "%04d-%02d-%02dT%02d:%02d:%02dZ", tm_utc.tm_year + 1900,
           tm_utc.tm_mon + 1, tm_utc.tm_mday, tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client,
             "https://firestore.googleapis.com/v1/projects/" FIREBASE_PROJECT_ID
             "/databases/(default)/documents/water_events");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + id_token);

  String payload = String("{\"fields\":{"
                          "\"ts\":{\"timestampValue\":\"") + iso + "\"}," +
                   "\"device\":{\"stringValue\":\"" DEVICE_ID "\"}}}";
  int code = http.POST(payload);
  if (code != 200) Serial.printf("[net] upload failed: %d %s\n", code, http.getString().c_str());
  http.end();
  return code == 200;
}

static void net_task(void *param) {
  (void)param;
  net_prefs.begin("net", false);
  load_pending();
  WiFi.mode(WIFI_STA);
  uint32_t last_wifi_attempt = 0;

  for (;;) {
    /* Absorb new button presses into the persistent queue */
    int64_t ev;
    while (xQueueReceive(drink_queue, &ev, 0) == pdTRUE) {
      pending_events.push_back(ev);
      save_pending();
    }

    if (WiFi.status() != WL_CONNECTED) {
      g_net_state = NET_CONNECTING;
      if (millis() - last_wifi_attempt > 20000 || last_wifi_attempt == 0) {
        last_wifi_attempt = millis();
        if (!wifi_try_connect()) g_net_state = NET_OFFLINE;
      } else {
        g_net_state = NET_OFFLINE;
      }
    }

    if (WiFi.status() == WL_CONNECTED) {
      if (!g_time_synced) sync_time();

      if (!pending_events.empty() && g_time_synced) {
        g_net_state = NET_ONLINE;
        if (ensure_token()) {
          while (!pending_events.empty() && upload_event(pending_events.front())) {
            pending_events.erase(pending_events.begin());
            save_pending();
            Serial.printf("[net] uploaded, %d left\n", (int)pending_events.size());
          }
        }
      }
      g_net_state = pending_events.empty() ? NET_SYNCED : NET_ONLINE;
    }

    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

/* ---------------- Arduino entry points ---------------- */

void setup() {
  Serial.begin(115200);

  ui_prefs.begin("ui", false);
  drink_queue = xQueueCreate(32, sizeof(int64_t));

  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSPI);
  ts.setRotation(0);

  tft.init();
  tft.setRotation(0);

  lv_init();
  lv_disp_draw_buf_init(&draw_buf, buf1, NULL, SCREEN_W * 40);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = SCREEN_W;
  disp_drv.ver_res = SCREEN_H;
  disp_drv.flush_cb = disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touch_read;
  lv_indev_drv_register(&indev_drv);

  build_ui();

  /* Networking on core 0; UI stays on core 1 */
  xTaskCreatePinnedToCore(net_task, "net", 16384, NULL, 1, NULL, 0);
}

void loop() {
  lv_timer_handler();
  delay(5);
}
