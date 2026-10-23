#include <pebble.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PERSIST_RAW 1
#define RAW_MAX PERSIST_DATA_MAX_LENGTH

static const char *HF_NAMES[] = {"80m-40m", "30m-20m", "17m-15m", "12m-10m"};
static const char *VHF_NAMES[] = {"Aurora", "Es Europe", "Es N.Am", "Es EU 6m", "Es EU 4m"};
static const char *PAGE_TITLES[] = {"HF Conditions", "Solar Data", "VHF Conditions"};

typedef struct {
  char updated[40];
  char sfi[8];
  char sn[8];
  char a[8];
  char k[8];
  char xray[16];
  char helium[16];
  char proton[16];
  char electron[16];
  char aurora[16];
  char norm[16];
  char lat[16];
  char wind[16];
  char bz[16];
  char geomag[20];
  char noise[16];
  char muf[16];
  char day[4][12];
  char night[4][12];
  char vhf[5][20];
  char source[8];
  bool have;
} Solar;

static Window *s_window;
static Layer *s_canvas;
static AppTimer *s_bar_timer;
static AppTimer *s_retry_timer;
static AppTimer *s_hour_timer;
static Solar s_solar;
static char s_raw[RAW_MAX];
static const char *s_status = "Loading solar data";
static int s_page;
static int s_bar_frame;
static time_t s_asked;

static GFont s_font14;
static GFont s_font14b;
static GFont s_font18b;

static void request_refresh(bool force);
static void schedule_retry(void);
static void schedule_hour(void);
static void draw_canvas(Layer *layer, GContext *ctx);

static void scopy(char *dst, size_t n, const char *src) {
  size_t i = 0;
  if (!src) {
    src = "";
  }
  while (src[i] && i + 1 < n) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';
}

static bool has_ci(const char *hay, const char *needle) {
  if (!hay || !needle || !needle[0]) {
    return false;
  }
  for (size_t i = 0; hay[i]; i++) {
    size_t j = 0;
    while (needle[j] && hay[i + j] &&
           (hay[i + j] | 32) == (needle[j] | 32)) {
      j++;
    }
    if (!needle[j]) {
      return true;
    }
  }
  return false;
}

static bool is_num(const char *s) {
  if (!s || !s[0]) {
    return false;
  }
  if (*s == '-') {
    s++;
  }
  bool digit = false;
  for (; *s; s++) {
    if (*s == '.') {
      continue;
    }
    if (*s < '0' || *s > '9') {
      return false;
    }
    digit = true;
  }
  return digit;
}

static int split_char(char *s, char sep, char **out, int max) {
  int n = 0;
  if (!s || max < 1) {
    return 0;
  }
  out[n++] = s;
  for (char *p = s; *p && n < max; p++) {
    if (*p == sep) {
      *p = '\0';
      out[n++] = p + 1;
    }
  }
  return n;
}

static void draw_text(GContext *ctx, const char *text, GFont font, GColor color,
                      int x, int y, int w, int h, GTextAlignment align) {
  if (!text) {
    text = "";
  }
  graphics_context_set_text_color(ctx, color);
  graphics_draw_text(ctx, text, font, GRect(x, y, w, h),
                     GTextOverflowModeTrailingEllipsis, align, NULL);
}

static GColor num_color(const char *s, int green_at, int yellow_at, bool higher_better) {
  if (!is_num(s)) {
    return GColorWhite;
  }
  int n = atoi(s);
  if (higher_better) {
    if (n >= green_at) {
      return GColorGreen;
    }
    if (n >= yellow_at) {
      return GColorYellow;
    }
    return GColorRed;
  }
  if (n < green_at) {
    return GColorGreen;
  }
  if (n < yellow_at) {
    return GColorYellow;
  }
  return GColorRed;
}

static GColor k_color(const char *s) {
  if (!is_num(s)) {
    return GColorWhite;
  }
  int n = atoi(s);
  if (n <= 2) {
    return GColorGreen;
  }
  if (n <= 4) {
    return GColorYellow;
  }
  return GColorRed;
}

static GColor xray_color(const char *s) {
  char c = s && s[0] ? s[0] : 0;
  if (c >= 'a' && c <= 'z') {
    c = (char)(c - 32);
  }
  if (c == 'A') {
    return GColorGreen;
  }
  if (c == 'B') {
    return GColorYellow;
  }
  if (c == 'C') {
    return GColorOrange;
  }
  if (c == 'M' || c == 'X') {
    return GColorRed;
  }
  return GColorWhite;
}

static GColor n_color(const char *s) {
  if (!is_num(s)) {
    return GColorWhite;
  }
  return atoi(s) <= 2 ? GColorGreen : GColorYellow;
}

static GColor noise_color(const char *s) {
  if (!s) {
    return GColorWhite;
  }
  for (const char *p = s; *p; p++) {
    if ((*p == 'S' || *p == 's') && p[1] >= '0' && p[1] <= '9') {
      if (p[1] <= '2') {
        return GColorGreen;
      }
      if (p[1] <= '4') {
        return GColorYellow;
      }
      return GColorRed;
    }
  }
  return GColorWhite;
}

static GColor geomag_color(const char *s) {
  if (has_ci(s, "storm") || has_ci(s, "active")) {
    return GColorRed;
  }
  if (has_ci(s, "unsett")) {
    return GColorYellow;
  }
  if (has_ci(s, "quiet")) {
    return GColorGreen;
  }
  return GColorWhite;
}

static void cond_style(const char *s, GColor *fill, GColor *ink, const char **label) {
  if (has_ci(s, "closed")) {
    *fill = GColorRed;
    *ink = GColorBlack;
    *label = "Closed";
  } else if (has_ci(s, "poor")) {
    *fill = GColorRed;
    *ink = GColorBlack;
    *label = "Poor";
  } else if (has_ci(s, "fair") || has_ci(s, "moderate")) {
    *fill = GColorYellow;
    *ink = GColorBlack;
    *label = "Fair";
  } else if (has_ci(s, "good") || has_ci(s, "open")) {
    *fill = GColorGreen;
    *ink = GColorBlack;
    *label = has_ci(s, "open") && !has_ci(s, "good") ? "Open" : "Good";
  } else {
    *fill = GColorDarkGray;
    *ink = GColorWhite;
    *label = (s && s[0]) ? s : "--";
  }
}

static void draw_cell(GContext *ctx, const char *text, int x, int y, int w, int h) {
  GColor fill;
  GColor ink;
  const char *label;
  cond_style(text, &fill, &ink, &label);
  graphics_context_set_fill_color(ctx, fill);
  graphics_fill_rect(ctx, GRect(x, y, w, h), 0, GCornerNone);
  draw_text(ctx, label, s_font14b, ink, x, y + (h - 16) / 2, w, 18, GTextAlignmentCenter);
}

static void draw_field(GContext *ctx, const char *label, const char *value, GColor color,
                       int x, int y, int w) {
  const char *shown = (value && value[0]) ? value : "--";
  GSize label_size = graphics_text_layout_get_content_size(
      label, s_font14, GRect(0, 0, w, 20), GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft);
  int label_w = label_size.w;
  if (label_w > w - 28) {
    label_w = w - 28;
  }
  int value_x = x + label_w + 3;
  int value_w = x + w - value_x;
  draw_text(ctx, label, s_font14, GColorWhite, x, y, label_w, 18, GTextAlignmentLeft);
  draw_text(ctx, shown, s_font14b, color, value_x, y, value_w, 18, GTextAlignmentRight);
}

static void draw_pair(GContext *ctx, const char *label, const char *value, GColor color,
                      int x, int y) {
  draw_text(ctx, label, s_font14b, GColorWhite, x, y + 3, 36, 18, GTextAlignmentLeft);
  draw_text(ctx, (value && value[0]) ? value : "--", s_font18b, color, x + 34, y, 62, 24,
            GTextAlignmentLeft);
}

static void draw_footer(GContext *ctx, GRect bounds) {
  bool noaa = strcmp(s_solar.source, "NOAA") == 0;
  const char *credit = noaa ? "NOAA" : "AC1RV";
  const char *brand = noaa ? "AC1RV" : "NOAA";
  int y = bounds.size.h - 18;
  draw_text(ctx, credit, s_font14, GColorLightGray, 4, y, 70, 18, GTextAlignmentLeft);
  draw_text(ctx, brand, s_font14, GColorLightGray, bounds.size.w - 78, y, 74, 18,
            GTextAlignmentRight);
  int dots_w = 5 * 3 + 4 * 2;
  int x = (bounds.size.w - dots_w) / 2;
  int dy = y + 6;
  for (int i = 0; i < 3; i++) {
    graphics_context_set_fill_color(ctx, i == s_page ? GColorYellow : GColorLightGray);
    graphics_fill_rect(ctx, GRect(x, dy, 5, 5), 0, GCornerNone);
    x += 9;
  }
}

static void draw_loading(GContext *ctx, GRect bounds) {
  draw_text(ctx, PAGE_TITLES[s_page], s_font14b, GColorYellow, 0, 48, bounds.size.w, 20,
            GTextAlignmentCenter);
  draw_text(ctx, s_status, s_font14, GColorWhite, 8, 74, bounds.size.w - 16, 18,
            GTextAlignmentCenter);
  int x = 20;
  int y = 104;
  int w = bounds.size.w - 40;
  int h = 14;
  graphics_context_set_fill_color(ctx, GColorDarkGray);
  graphics_fill_rect(ctx, GRect(x, y, w, h), 0, GCornerNone);
  int fill = (w * (s_bar_frame + 1)) / 16;
  if (fill < 8) {
    fill = 8;
  }
  graphics_context_set_fill_color(ctx, GColorYellow);
  graphics_fill_rect(ctx, GRect(x, y, fill, h), 0, GCornerNone);
  draw_text(ctx, "UP / DOWN  pages", s_font14, GColorLightGray, 0, 132, bounds.size.w, 18,
            GTextAlignmentCenter);
  draw_text(ctx, "SELECT  refresh", s_font14, GColorLightGray, 0, 154, bounds.size.w, 18,
            GTextAlignmentCenter);
}

static void draw_hf(GContext *ctx, GRect bounds) {
  draw_text(ctx, s_solar.updated, s_font14, GColorYellow, 0, 2, bounds.size.w, 18,
            GTextAlignmentCenter);
  int half = bounds.size.w / 2;
  draw_pair(ctx, "SFI", s_solar.sfi, num_color(s_solar.sfi, 100, 70, true), 8, 22);
  draw_pair(ctx, "SN", s_solar.sn, num_color(s_solar.sn, 100, 30, true), half, 22);
  draw_pair(ctx, "A", s_solar.a, num_color(s_solar.a, 10, 20, false), 8, 46);
  draw_pair(ctx, "K", s_solar.k, k_color(s_solar.k), half, 46);

  draw_text(ctx, "HF Conditions", s_font14b, GColorYellow, 4, 74, 140, 18, GTextAlignmentLeft);
  draw_text(ctx, "Band", s_font14b, GColorYellow, 4, 92, 60, 18, GTextAlignmentLeft);

  int table_x = 74;
  int gap = 4;
  int col_w = (bounds.size.w - table_x - 4 - gap) / 2;
  int day_x = table_x;
  int night_x = table_x + col_w + gap;
  draw_text(ctx, "Day", s_font14b, GColorYellow, day_x, 92, col_w, 18, GTextAlignmentCenter);
  draw_text(ctx, "Night", s_font14b, GColorYellow, night_x, 92, col_w, 18, GTextAlignmentCenter);

  int top = 112;
  int bottom = bounds.size.h - 22;
  int row_h = (bottom - top) / 4;
  int cell_h = row_h - 3;
  if (cell_h > 22) {
    cell_h = 22;
  }
  for (int i = 0; i < 4; i++) {
    int y = top + i * row_h;
    draw_text(ctx, HF_NAMES[i], s_font14, GColorWhite, 4, y + (cell_h - 16) / 2, 68, 18,
              GTextAlignmentLeft);
    draw_cell(ctx, s_solar.day[i], day_x, y, col_w, cell_h);
    draw_cell(ctx, s_solar.night[i], night_x, y, col_w, cell_h);
  }
}

static void draw_solar(GContext *ctx, GRect bounds) {
  draw_text(ctx, s_solar.updated, s_font14, GColorYellow, 0, 2, bounds.size.w, 18,
            GTextAlignmentCenter);
  draw_text(ctx, "Solar Data", s_font14b, GColorYellow, 4, 20, 160, 18, GTextAlignmentLeft);

  int top = 40;
  int bottom = bounds.size.h - 22;
  int row_h = (bottom - top) / 9;
  int half = bounds.size.w / 2;
  int left = 4;
  int right = half + 2;
  int col_w = half - 8;

  struct {
    const char *l;
    const char *lv;
    GColor lc;
    const char *r;
    const char *rv;
    GColor rc;
  } rows[7] = {
    {"SFI", s_solar.sfi, num_color(s_solar.sfi, 100, 70, true),
     "SN", s_solar.sn, num_color(s_solar.sn, 100, 30, true)},
    {"A", s_solar.a, num_color(s_solar.a, 10, 20, false),
     "K", s_solar.k, k_color(s_solar.k)},
    {"X-Ray", s_solar.xray, xray_color(s_solar.xray),
     "304A", s_solar.helium, num_color(s_solar.helium, 100, 80, true)},
    {"Ptn Flx", s_solar.proton, num_color(s_solar.proton, 100, 1000, false),
     "Elc Flx", s_solar.electron, num_color(s_solar.electron, 100, 1000, false)},
    {"Aurora", s_solar.aurora, num_color(s_solar.aurora, 6, 8, false),
     "n", s_solar.norm, n_color(s_solar.norm)},
    {"Wind", s_solar.wind, num_color(s_solar.wind, 500, 700, false),
     "Bz", s_solar.bz, num_color(s_solar.bz, 0, -5, true)},
    {"Aur Lat", s_solar.lat, GColorWhite, "MUF", s_solar.muf, GColorWhite},
  };

  for (int i = 0; i < 7; i++) {
    int y = top + i * row_h;
    draw_field(ctx, rows[i].l, rows[i].lv, rows[i].lc, left, y, col_w);
    draw_field(ctx, rows[i].r, rows[i].rv, rows[i].rc, right, y, col_w);
  }
  draw_field(ctx, "GeoMag", s_solar.geomag, geomag_color(s_solar.geomag), left,
             top + 7 * row_h, bounds.size.w - 8);
  draw_field(ctx, "Sig Noise", s_solar.noise, noise_color(s_solar.noise), left, top + 8 * row_h,
             bounds.size.w - 8);
}

static void draw_vhf(GContext *ctx, GRect bounds) {
  draw_text(ctx, s_solar.updated, s_font14, GColorYellow, 0, 2, bounds.size.w, 18,
            GTextAlignmentCenter);
  draw_text(ctx, "VHF Conditions", s_font14b, GColorYellow, 4, 20, 180, 18, GTextAlignmentLeft);

  int top = 42;
  int bottom = bounds.size.h - 22;
  int row_h = (bottom - top) / 5;
  int cell_h = row_h - 6;
  if (cell_h > 24) {
    cell_h = 24;
  }
  int pill_x = 108;
  int pill_w = bounds.size.w - pill_x - 4;
  for (int i = 0; i < 5; i++) {
    int y = top + i * row_h;
    draw_text(ctx, VHF_NAMES[i], s_font14, GColorWhite, 4, y + (cell_h - 16) / 2, 100, 18,
              GTextAlignmentLeft);
    draw_cell(ctx, s_solar.vhf[i], pill_x, y, pill_w, cell_h);
  }
}

static void draw_canvas(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  if (!s_solar.have) {
    draw_loading(ctx, bounds);
  } else if (s_page == 0) {
    draw_hf(ctx, bounds);
  } else if (s_page == 1) {
    draw_solar(ctx, bounds);
  } else {
    draw_vhf(ctx, bounds);
  }
  draw_footer(ctx, bounds);
}

static bool parse_buf(char *buf) {
  char *fields[20];
  int n = split_char(buf, '|', fields, 20);
  if (n < 18) {
    return false;
  }
  scopy(s_solar.updated, sizeof(s_solar.updated), fields[0]);
  scopy(s_solar.sfi, sizeof(s_solar.sfi), fields[1]);
  scopy(s_solar.sn, sizeof(s_solar.sn), fields[2]);
  scopy(s_solar.a, sizeof(s_solar.a), fields[3]);
  scopy(s_solar.k, sizeof(s_solar.k), fields[4]);
  scopy(s_solar.xray, sizeof(s_solar.xray), fields[5]);
  scopy(s_solar.helium, sizeof(s_solar.helium), fields[6]);
  scopy(s_solar.proton, sizeof(s_solar.proton), fields[7]);
  scopy(s_solar.electron, sizeof(s_solar.electron), fields[8]);
  scopy(s_solar.aurora, sizeof(s_solar.aurora), fields[9]);
  scopy(s_solar.norm, sizeof(s_solar.norm), fields[10]);
  scopy(s_solar.lat, sizeof(s_solar.lat), fields[11]);
  scopy(s_solar.wind, sizeof(s_solar.wind), fields[12]);
  scopy(s_solar.bz, sizeof(s_solar.bz), fields[13]);
  scopy(s_solar.geomag, sizeof(s_solar.geomag), fields[14]);
  scopy(s_solar.noise, sizeof(s_solar.noise), fields[15]);
  scopy(s_solar.muf, sizeof(s_solar.muf), n > 16 ? fields[16] : "NoRpt");

  char *bands[8];
  int bn = split_char(fields[17], ',', bands, 8);
  for (int i = 0; i < 4; i++) {
    scopy(s_solar.day[i], sizeof(s_solar.day[i]), i * 2 < bn ? bands[i * 2] : "");
    scopy(s_solar.night[i], sizeof(s_solar.night[i]), i * 2 + 1 < bn ? bands[i * 2 + 1] : "");
  }
  if (n > 18) {
    char *vhf[5];
    int vn = split_char(fields[18], ',', vhf, 5);
    for (int i = 0; i < 5; i++) {
      scopy(s_solar.vhf[i], sizeof(s_solar.vhf[i]), i < vn ? vhf[i] : "");
    }
  }
  scopy(s_solar.source, sizeof(s_solar.source), n > 19 ? fields[19] : "N0NBH");
  return s_solar.sfi[0] || s_solar.day[0][0];
}

static void apply_payload(const char *text) {
  if (!text || !text[0] || strcmp(text, "0") == 0) {
    return;
  }
  if (strcmp(text, "ERR") == 0) {
    if (!s_solar.have) {
      s_status = "No link. SELECT retries";
      if (s_canvas) {
        layer_mark_dirty(s_canvas);
      }
    }
    return;
  }
  char buf[RAW_MAX];
  scopy(buf, sizeof(buf), text);
  if (!parse_buf(buf)) {
    if (!s_solar.have) {
      s_status = "Bad feed. SELECT retries";
      if (s_canvas) {
        layer_mark_dirty(s_canvas);
      }
    }
    return;
  }
  scopy(s_raw, sizeof(s_raw), text);
  persist_write_string(PERSIST_RAW, s_raw);
  s_solar.have = true;
  schedule_hour();
  if (s_bar_timer) {
    app_timer_cancel(s_bar_timer);
    s_bar_timer = NULL;
  }
  if (s_canvas) {
    layer_mark_dirty(s_canvas);
  }
}

static void inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *solar = dict_find(iter, MESSAGE_KEY_SOLAR);
  if (solar && solar->type == TUPLE_CSTRING) {
    apply_payload(solar->value->cstring);
  }
}

static void inbox_dropped(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "inbox dropped %d", reason);
}

static void bar_timer_cb(void *context) {
  s_bar_frame = (s_bar_frame + 1) % 16;
  if (s_canvas) {
    layer_mark_dirty(s_canvas);
  }
  if (!s_solar.have) {
    s_bar_timer = app_timer_register(100, bar_timer_cb, NULL);
  } else {
    s_bar_timer = NULL;
  }
}

static void retry_cb(void *context) {
  s_retry_timer = NULL;
  if (!s_solar.have) {
    request_refresh(false);
    schedule_retry();
  }
}

static void schedule_retry(void) {
  if (s_retry_timer || s_solar.have) {
    return;
  }
  s_retry_timer = app_timer_register(3000, retry_cb, NULL);
}

static void hour_cb(void *context) {
  s_hour_timer = NULL;
  request_refresh(true);
  schedule_hour();
}

static void schedule_hour(void) {
  if (s_hour_timer) {
    return;
  }
  s_hour_timer = app_timer_register(60 * 60 * 1000, hour_cb, NULL);
}

static void request_refresh(bool force) {
  time_t now = time(NULL);
  if (!connection_service_peek_pebblekit_connection()) {
    if (!s_solar.have) {
      s_status = "Waiting for phone...";
      if (s_canvas) {
        layer_mark_dirty(s_canvas);
      }
    }
    return;
  }
  if (!force && s_asked && now - s_asked < 15) {
    return;
  }
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
    return;
  }
  dict_write_uint8(iter, MESSAGE_KEY_REFRESH, 1);
  if (app_message_outbox_send() != APP_MSG_OK) {
    return;
  }
  s_asked = now;
  if (!s_solar.have) {
    s_status = "Loading solar data";
    if (s_canvas) {
      layer_mark_dirty(s_canvas);
    }
  }
}

static void on_pebblekit(bool connected) {
  if (connected) {
    request_refresh(false);
    schedule_retry();
  } else if (!s_solar.have) {
    s_status = "Waiting for phone...";
    if (s_canvas) {
      layer_mark_dirty(s_canvas);
    }
  }
}

static void up_click(ClickRecognizerRef recognizer, void *context) {
  s_page = (s_page + 2) % 3;
  layer_mark_dirty(s_canvas);
}

static void down_click(ClickRecognizerRef recognizer, void *context) {
  s_page = (s_page + 1) % 3;
  layer_mark_dirty(s_canvas);
}

static void select_click(ClickRecognizerRef recognizer, void *context) {
  s_asked = 0;
  request_refresh(true);
}

static void click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_canvas = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_canvas, draw_canvas);
  layer_add_child(root, s_canvas);
}

static void window_unload(Window *window) {
  layer_destroy(s_canvas);
  s_canvas = NULL;
}

static void init(void) {
  s_font14 = fonts_get_system_font(FONT_KEY_GOTHIC_14);
  s_font14b = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  s_font18b = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);

  if (persist_exists(PERSIST_RAW)) {
    persist_read_string(PERSIST_RAW, s_raw, sizeof(s_raw));
    char buf[RAW_MAX];
    scopy(buf, sizeof(buf), s_raw);
    if (parse_buf(buf)) {
      s_solar.have = true;
    }
  }

  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = window_load,
    .unload = window_unload,
  });
  window_set_click_config_provider(s_window, click_config);
  window_stack_push(s_window, true);

  app_message_register_inbox_received(inbox_received);
  app_message_register_inbox_dropped(inbox_dropped);
  app_message_open(app_message_inbox_size_maximum(), app_message_outbox_size_maximum());

  connection_service_subscribe((ConnectionHandlers) {
    .pebblekit_connection_handler = on_pebblekit,
  });

  if (!s_solar.have) {
    s_bar_timer = app_timer_register(100, bar_timer_cb, NULL);
    schedule_retry();
  } else {
    schedule_hour();
  }
  request_refresh(true);
}

static void deinit(void) {
  if (s_bar_timer) {
    app_timer_cancel(s_bar_timer);
  }
  if (s_retry_timer) {
    app_timer_cancel(s_retry_timer);
  }
  if (s_hour_timer) {
    app_timer_cancel(s_hour_timer);
  }
  connection_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
