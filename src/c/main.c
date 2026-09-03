#include <pebble.h>

static Window *s_main_window;
static TextLayer *s_time_layer, *s_station_layer, *s_type_layer, *s_dest_layer, *s_countdown_layer, *s_depart_layer;
static TextLayer *s_toast_layer = NULL;
static AppTimer *s_toast_timer = NULL;
static GRect s_countdown_frame;
static int s_target_hour = 0;
static int s_target_min = 0;
static bool s_data_received = false;

static bool s_vibrated_3min = false;
static bool s_vibrated_1min = false;
static bool s_vibrated_0min = false;

static bool s_alarm_set = false;
static bool s_alarm_view = false;
static time_t s_alarm_time = 0;
static int s_alarm_minutes_before = 30;

/*
 * 「終電30分前」到達時に出すアラーム画面。
 *
 * up  = 5分スヌーズ
 * down = 停止
 */
static Window *s_alarm_window = NULL;
static Layer *s_alarm_icon_layer = NULL;
static TextLayer *s_alarm_title_layer = NULL;
static TextLayer *s_alarm_hint_layer = NULL;
static AppTimer *s_alarm_vibe_timer = NULL;

/*
 * PDCS(アニメーション付きベクター画像)。
 * timer_main.c で使われていた Pebble_80x80_Alarm_clock.pdc と同じもの。
 */
static GDrawCommandSequence *s_alarm_sequence = NULL;
static AppTimer *s_alarm_anim_timer = NULL;
static uint32_t s_alarm_anim_elapsed_ms = 0;

#define ALARM_WAKEUP_COOKIE 20260901
#define ALARM_SNOOZE_SECONDS (5 * 60)
#define ALARM_VIBE_INTERVAL_MS 2000
#define ALARM_ANIM_TICK_MS 50

static void request_train(int key) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
    dict_write_uint8(iter, key, 1);
    app_message_outbox_send();
  }
}

static void request_timeline_action(uint8_t action, time_t alarm_time) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
    dict_write_uint8(iter, MESSAGE_KEY_KEY_REQUEST_ADD_TIMELINE, action);
    if (action == 2) {
      dict_write_int32(iter, MESSAGE_KEY_KEY_HOUR, (int32_t)alarm_time);
    }
    app_message_outbox_send();
  }
}

static void toast_timer_callback(void *data) {
  s_toast_timer = NULL;
  if (s_toast_layer) {
    layer_set_hidden(text_layer_get_layer(s_toast_layer), true);
  }
}

static void show_toast(const char *text) {
  if (!s_toast_layer) return;

  text_layer_set_text(s_toast_layer, text);
  layer_set_hidden(text_layer_get_layer(s_toast_layer), false);

  if (s_toast_timer) {
    app_timer_cancel(s_toast_timer);
  }
  s_toast_timer = app_timer_register(1000, toast_timer_callback, NULL);
}

/*
 * ============================================================
 * Alarm ring window
 * ============================================================
 * 「終電30分前」到達時に表示する専用画面。
 * 目覚まし時計の絵は Pebble_80x80_Alarm_clock.pdc
 * (RESOURCE_ID_ALARM_CLOCK, PDCS=アニメーション付き)を再生する。
 */

static void alarm_icon_update_proc(Layer *layer, GContext *ctx) {
  if (!s_alarm_sequence) return;

  GDrawCommandFrame *frame = gdraw_command_sequence_get_frame_by_elapsed(s_alarm_sequence, s_alarm_anim_elapsed_ms);
  if (!frame) return;

  GRect bounds = layer_get_bounds(layer);
  GSize img_size = gdraw_command_sequence_get_bounds_size(s_alarm_sequence);
  GPoint origin = GPoint((bounds.size.w - img_size.w) / 2, (bounds.size.h - img_size.h) / 2);

  gdraw_command_frame_draw(ctx, s_alarm_sequence, frame, origin);
}

static void alarm_anim_timer_callback(void *data) {
  s_alarm_anim_timer = NULL;
  if (!s_alarm_sequence) return;

  uint32_t total = gdraw_command_sequence_get_total_duration(s_alarm_sequence);
  s_alarm_anim_elapsed_ms += ALARM_ANIM_TICK_MS;

  if (total > 0 && s_alarm_anim_elapsed_ms >= total) {
    /* ループ再生 */
    s_alarm_anim_elapsed_ms = 0;
  }

  if (s_alarm_icon_layer) {
    layer_mark_dirty(s_alarm_icon_layer);
  }

  s_alarm_anim_timer = app_timer_register(ALARM_ANIM_TICK_MS, alarm_anim_timer_callback, NULL);
}

static void alarm_vibe_timer_callback(void *data) {
  vibes_long_pulse();
  s_alarm_vibe_timer = app_timer_register(ALARM_VIBE_INTERVAL_MS, alarm_vibe_timer_callback, NULL);
}

static void alarm_stop_vibrating(void) {
  if (s_alarm_vibe_timer) {
    app_timer_cancel(s_alarm_vibe_timer);
    s_alarm_vibe_timer = NULL;
  }
}

static void alarm_stop(void) {
  alarm_stop_vibrating();

  s_alarm_set = false;
  s_alarm_time = 0;

  persist_write_bool(1, false);
  persist_write_int(2, 0);

  request_timeline_action(3, 0);

  if (s_alarm_window && window_stack_get_top_window() == s_alarm_window) {
    window_stack_pop(true);
  }
  show_toast("Alarm Deleted!");
}

static void alarm_snooze(void) {
  alarm_stop_vibrating();

  time_t new_alarm_time = time(NULL) + ALARM_SNOOZE_SECONDS;
  wakeup_cancel(ALARM_WAKEUP_COOKIE);

  if (wakeup_schedule(new_alarm_time, ALARM_WAKEUP_COOKIE, true)) {
    s_alarm_set = true;
    s_alarm_time = new_alarm_time;
    persist_write_bool(1, true);
    persist_write_int(2, (int32_t)new_alarm_time);
    request_timeline_action(2, new_alarm_time);
  }

  if (s_alarm_window && window_stack_get_top_window() == s_alarm_window) {
    window_stack_pop(true);
  }
}

static void alarm_up_click_handler(ClickRecognizerRef recognizer, void *context) {
  alarm_snooze();
}

static void alarm_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  alarm_stop();
}

static void alarm_back_click_handler(ClickRecognizerRef recognizer, void *context) {
  /*
   * 誤操作防止: 戻るボタンで無音のまま閉じてしまわないよう、
   * 停止扱いにする。
   */
  alarm_stop();
}

static void alarm_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, alarm_up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, alarm_down_click_handler);
  window_single_click_subscribe(BUTTON_ID_BACK, alarm_back_click_handler);
}

static void alarm_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  window_set_background_color(window, GColorWhite);

  int icon_h = (int)(bounds.size.h * 0.55);

  s_alarm_icon_layer = layer_create(GRect(0, 4, bounds.size.w, icon_h));
  layer_set_update_proc(s_alarm_icon_layer, alarm_icon_update_proc);
  layer_add_child(root, s_alarm_icon_layer);

  s_alarm_sequence = gdraw_command_sequence_create_with_resource(RESOURCE_ID_ALARM_CLOCK);
  s_alarm_anim_elapsed_ms = 0;
  s_alarm_anim_timer = app_timer_register(ALARM_ANIM_TICK_MS, alarm_anim_timer_callback, NULL);

  s_alarm_title_layer = text_layer_create(GRect(0, icon_h + 4, bounds.size.w, 32));
  text_layer_set_text(s_alarm_title_layer, "Time's Up!");
  text_layer_set_text_alignment(s_alarm_title_layer, GTextAlignmentCenter);
  text_layer_set_background_color(s_alarm_title_layer, GColorClear);
  text_layer_set_font(s_alarm_title_layer, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD));
  layer_add_child(root, text_layer_get_layer(s_alarm_title_layer));

  s_alarm_hint_layer = text_layer_create(GRect(0, bounds.size.h - 40, bounds.size.w, 40));
  text_layer_set_text(s_alarm_hint_layer, "UP: 5分スヌーズ\nDOWN: 停止");
  text_layer_set_text_alignment(s_alarm_hint_layer, GTextAlignmentCenter);
  text_layer_set_background_color(s_alarm_hint_layer, GColorClear);
  text_layer_set_font(s_alarm_hint_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  layer_add_child(root, text_layer_get_layer(s_alarm_hint_layer));
}

static void alarm_window_unload(Window *window) {
  alarm_stop_vibrating();

  if (s_alarm_anim_timer) {
    app_timer_cancel(s_alarm_anim_timer);
    s_alarm_anim_timer = NULL;
  }
  if (s_alarm_sequence) {
    gdraw_command_sequence_destroy(s_alarm_sequence);
    s_alarm_sequence = NULL;
  }
  if (s_alarm_icon_layer) {
    layer_destroy(s_alarm_icon_layer);
    s_alarm_icon_layer = NULL;
  }
  if (s_alarm_title_layer) {
    text_layer_destroy(s_alarm_title_layer);
    s_alarm_title_layer = NULL;
  }
  if (s_alarm_hint_layer) {
    text_layer_destroy(s_alarm_hint_layer);
    s_alarm_hint_layer = NULL;
  }
}

static void show_alarm_window(void) {
  if (!s_alarm_window) {
    s_alarm_window = window_create();
    window_set_click_config_provider(s_alarm_window, alarm_click_config_provider);
    window_set_window_handlers(s_alarm_window, (WindowHandlers) {
      .load = alarm_window_load,
      .unload = alarm_window_unload
    });
  }

  if (window_stack_get_top_window() != s_alarm_window) {
    window_stack_push(s_alarm_window, true);
  }

  alarm_stop_vibrating();
  vibes_long_pulse();
  s_alarm_vibe_timer = app_timer_register(ALARM_VIBE_INTERVAL_MS, alarm_vibe_timer_callback, NULL);
}

static void wakeup_handler(WakeupId id, int32_t reason) {
  if (id != ALARM_WAKEUP_COOKIE) return;

  s_alarm_set = false;
  s_alarm_time = 0;
  persist_write_bool(1, false);
  persist_write_int(2, 0);

  request_timeline_action(3, 0);
  show_alarm_window();
}

static void toggle_alarm(void) {
  if (!s_data_received || s_target_hour < 0) {
    show_toast("No train");
    return;
  }

  if (s_alarm_set) {
    wakeup_cancel(ALARM_WAKEUP_COOKIE);
    s_alarm_set = false;
    s_alarm_time = 0;
    persist_write_bool(1, false);
    persist_write_int(2, 0);
    request_timeline_action(3, 0);
    show_toast("Alarm OFF");
    return;
  }

  time_t now = time(NULL);
  struct tm departure_tm = *localtime(&now);
  /*
   * s_target_hour は午前0～3時の終電の場合、
   * 24～27時として保持されている。
   */
  int departure_hour = s_target_hour % 24;

  departure_tm.tm_hour = departure_hour;
  departure_tm.tm_min = s_target_min;
  departure_tm.tm_sec = 0;

  time_t departure_time = mktime(&departure_tm);

  /*
   * すでに今日の時刻を過ぎている場合は翌日。
   */
  if (departure_time <= now) {
    departure_time += 24 * 60 * 60;
  }

  time_t alarm_time = departure_time - (time_t)s_alarm_minutes_before * 60;
  //ここで乗車x分前のアラームを指示している

  /*
   * 30分前がすでに過ぎている場合。
   *
   * アラームは設定せず、
   * 現在の終電をTimelineへ登録する。
   */
  if (alarm_time <= now) {
    request_train(MESSAGE_KEY_KEY_REQUEST_ADD_TIMELINE);
    show_toast("Added pin!\n終電が近いです！");
    return;
  }

  wakeup_cancel(ALARM_WAKEUP_COOKIE);

  if (wakeup_schedule(alarm_time, ALARM_WAKEUP_COOKIE, true)) {
    s_alarm_set = true;
    s_alarm_time = alarm_time;
    persist_write_bool(1, true);
    persist_write_int(2, (int32_t)alarm_time);
    request_timeline_action(2, alarm_time);
    show_toast("Alarm ON");
  } else {
    show_toast("Alarm Error");
  }
}

static void update_time(void) {
  time_t temp = time(NULL);
  struct tm *tick_time = localtime(&temp);
  static char s_buffer[8];

  strftime(s_buffer, sizeof(s_buffer), "%H:%M", tick_time);
  text_layer_set_text(s_time_layer, s_buffer);
}

static void update_countdown(void) {
  if (!s_data_received) {
    text_layer_set_text(s_countdown_layer, "Loading");
    return;
  }

  time_t now = time(NULL);
  struct tm *t = localtime(&now);

  int hour = t->tm_hour;
  if (hour < 4) {
    hour += 24;
  }

  int now_sec = hour * 3600 + t->tm_min * 60 + t->tm_sec;
  int train_sec = s_target_hour * 3600 + s_target_min * 60;
  int diff = train_sec - now_sec;

  static char s_count_buf[16];

  text_layer_set_text_color(s_countdown_layer, GColorBlack);
  text_layer_set_background_color(s_countdown_layer, GColorClear);

  if (diff > 0) {
    snprintf(s_count_buf, sizeof(s_count_buf), "%02d:%02d", diff / 60, diff % 60);

    if (diff <= 180 && !s_vibrated_3min) {
      vibes_short_pulse();
      s_vibrated_3min = true;
    }
    if (diff <= 60 && !s_vibrated_1min) {
      vibes_short_pulse();
      s_vibrated_1min = true;
    }
  } else if (diff >= -180) {
    int passed = -diff;

    if (passed <= 2 && !s_vibrated_0min) {
      vibes_double_pulse();
      s_vibrated_0min = true;
    }

    snprintf(s_count_buf, sizeof(s_count_buf), "-%02d:%02d", passed / 60, passed % 60);

    if (t->tm_sec % 2 == 0) {
      text_layer_set_text_color(s_countdown_layer, GColorWhite);
      text_layer_set_background_color(s_countdown_layer, GColorBlack);
    }
  } else {
    snprintf(s_count_buf, sizeof(s_count_buf), "Departed");
  }

  text_layer_set_text(s_countdown_layer, s_count_buf);
}

static void tick_handler(struct tm *t, TimeUnits u) {
  update_time();
  update_countdown();
}

#if defined(PBL_TOUCH)
static void tap_handler(AccelAxisType axis, int32_t direction) {
  light_enable_interaction();
}
#endif

static void animate_layer(Layer *layer, GRect start, GRect end, int duration, int delay) {
  PropertyAnimation *prop_anim = property_animation_create_layer_frame(layer, &start, &end);
  Animation *anim = property_animation_get_animation(prop_anim);
  animation_set_duration(anim, duration);
  animation_set_delay(anim, delay);
  animation_set_curve(anim, AnimationCurveEaseIn);
  animation_schedule(anim);
}

static void inbox_received_callback(DictionaryIterator *iterator, void *context) {
  Tuple *st_t = dict_find(iterator, MESSAGE_KEY_KEY_STATION);
  Tuple *de_t = dict_find(iterator, MESSAGE_KEY_KEY_DEST);
  Tuple *hr_t = dict_find(iterator, MESSAGE_KEY_KEY_HOUR);
  Tuple *mn_t = dict_find(iterator, MESSAGE_KEY_KEY_MIN);
  Tuple *ty_t = dict_find(iterator, MESSAGE_KEY_KEY_TYPE_TEXT);
  Tuple *tl_res_t = dict_find(iterator, MESSAGE_KEY_KEY_TIMELINE_RESULT);
  Tuple *alarm_min_t = dict_find(iterator, MESSAGE_KEY_KEY_ALARM_MINUTES_BEFORE);

  if (alarm_min_t) {
    int minutes = (int)alarm_min_t->value->int32;
    if (minutes >= 0) {
      s_alarm_minutes_before = minutes;
    }
  }
  
  if (tl_res_t) {
    if (tl_res_t->value->int32 == 1) {
      show_toast("Added Pin!");
    } else {
      show_toast("Failed Pin");
    }
    return;
  }

  static char s_st_buf[64], s_ty_buf[64], s_de_buf[128];

  if (st_t) {
    strncpy(s_st_buf, st_t->value->cstring, sizeof(s_st_buf) - 1);
    s_st_buf[sizeof(s_st_buf) - 1] = '\0';
    text_layer_set_text(s_station_layer, s_st_buf);
  }

  if (ty_t) {
    strncpy(s_ty_buf, ty_t->value->cstring, sizeof(s_ty_buf) - 1);
    s_ty_buf[sizeof(s_ty_buf) - 1] = '\0';
  } else {
    s_ty_buf[0] = '\0';
  }
  text_layer_set_text(s_type_layer, s_ty_buf);

  if (de_t) {
    strncpy(s_de_buf, de_t->value->cstring, sizeof(s_de_buf) - 1);
    s_de_buf[sizeof(s_de_buf) - 1] = '\0';
    text_layer_set_text(s_dest_layer, s_de_buf);
  }

  bool has_train = true;

  if (hr_t) {
    s_target_hour = (int)hr_t->value->int32;
    if (s_target_hour == -1) {
      has_train = false;
    } else if (s_target_hour >= 0 && s_target_hour < 4) {
      s_target_hour += 24;
    }
  }

  if (mn_t) {
    s_target_min = (int)mn_t->value->int32;
  }

  s_vibrated_3min = false;
  s_vibrated_1min = false;
  s_vibrated_0min = false;
  s_data_received = true;

  if (has_train) {
    static char s_dep_buf[16];
    snprintf(s_dep_buf, sizeof(s_dep_buf), "Dep: %02d:%02d", s_target_hour % 24, s_target_min);
    text_layer_set_text(s_depart_layer, s_dep_buf);
  } else {
    text_layer_set_text(s_depart_layer, "");
    text_layer_set_text(s_countdown_layer, "Departed");
  }

  Layer *root = window_get_root_layer(s_main_window);
  int w = layer_get_bounds(root).size.w;

#if defined(PBL_PLATFORM_GABBRO)
  if (has_train) {
    animate_layer(text_layer_get_layer(s_type_layer), GRect(0, 67, w, 24), GRect(0, 65, w, 24), 300, 100);
    animate_layer(text_layer_get_layer(s_dest_layer), GRect(0, 91, w, 32), GRect(0, 89, w, 32), 300, 150);
    animate_layer(text_layer_get_layer(s_depart_layer), GRect(0, 155, w, 28), GRect(0, 153, w, 28), 300, 180);
  }
  animate_layer(text_layer_get_layer(s_countdown_layer), GRect(0, 123, w, 32), GRect(0, 121, w, 32), 300, 0);

#elif defined(PBL_PLATFORM_EMERY)
  if (has_train) {
    animate_layer(text_layer_get_layer(s_type_layer), GRect(0, 52, w, 24), GRect(0, 49, w, 24), 300, 100);
    animate_layer(text_layer_get_layer(s_dest_layer), GRect(0, 76, w, 32), GRect(0, 73, w, 32), 300, 150);
    animate_layer(text_layer_get_layer(s_depart_layer), GRect(0, 143, w, 28), GRect(0, 141, w, 28), 300, 180);
  }
  animate_layer(text_layer_get_layer(s_countdown_layer), GRect(0, 108, w, 32), GRect(0, 105, w, 32), 300, 0);

#else
  int h = layer_get_bounds(root).size.h;
  animate_layer(text_layer_get_layer(s_countdown_layer), GRect(0, (int)(h * 0.55), w, (int)(h * 0.20)), s_countdown_frame, 300, 0);

  if (has_train) {
    animate_layer(text_layer_get_layer(s_dest_layer), GRect(0, (int)(h * 0.38), w, (int)(h * 0.15)), GRect(0, (int)(h * 0.35), w, (int)(h * 0.15)), 300, 100);
  }
#endif

  update_countdown();
}

static void center_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  alarm_stop();
}

static void up_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  toggle_alarm();
}

static void down_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  request_train(MESSAGE_KEY_KEY_REQUEST_ADD_TIMELINE);
}

static void up_click_handler(ClickRecognizerRef r, void *c) {
  request_train(MESSAGE_KEY_KEY_REQUEST_PREV);
}

static void down_click_handler(ClickRecognizerRef r, void *c) {
  request_train(MESSAGE_KEY_KEY_REQUEST_NEXT);
}

static void center_click_handler(ClickRecognizerRef r, void *c) {
  request_train(MESSAGE_KEY_KEY_REQUEST_SWITCH);
}

static void click_config_provider(void *c) {
  window_single_click_subscribe(BUTTON_ID_UP, up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, center_click_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, 500, center_long_click_handler, NULL);
  window_long_click_subscribe(BUTTON_ID_UP, 500, up_long_click_handler, NULL);
  window_long_click_subscribe(BUTTON_ID_DOWN, 500, down_long_click_handler, NULL);
}

static void main_window_load(Window *window) {
  Layer *w_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(w_layer);
  int w = bounds.size.w;
  int h = bounds.size.h;

#if defined(PBL_PLATFORM_GABBRO)
  GRect rect_time = GRect(0, 16, w, 24);
  GRect rect_station = GRect(0, 40, w, 24);
  GRect rect_type = GRect(0, 64, w, 24);
  GRect rect_dest = GRect(0, 88, w, 32);
  GRect rect_countdown = GRect(0, 120, w, 32);
  GRect rect_depart = GRect(0, 152, w, 28);
  s_countdown_frame = rect_countdown;
#elif defined(PBL_PLATFORM_EMERY)
  GRect rect_time = GRect(0, 0, w, 24);
  GRect rect_station = GRect(0, 24, w, 24);
  GRect rect_type = GRect(0, 48, w, 24);
  GRect rect_dest = GRect(0, 72, w, 32);
  GRect rect_countdown = GRect(0, 104, w, 32);
  GRect rect_depart = GRect(0, 140, w, 28);
  s_countdown_frame = rect_countdown;
#else
  GRect rect_time = GRect(0, 4, w, 22);
  GRect rect_station = GRect(0, (int)(h * 0.20), w, 28);
  GRect rect_type = GRect(0, (int)(h * 0.36), w, 20);
  GRect rect_dest = GRect(0, (int)(h * 0.48), w, 20);
  s_countdown_frame = GRect(0, (int)(h * 0.60), w, 32);
  GRect rect_countdown = s_countdown_frame;
  GRect rect_depart = GRect(0, (int)(h * 0.80), w, 24);
#endif

  s_time_layer = text_layer_create(rect_time);
  s_station_layer = text_layer_create(rect_station);
  s_type_layer = text_layer_create(rect_type);
  s_dest_layer = text_layer_create(rect_dest);
  s_countdown_layer = text_layer_create(rect_countdown);
  s_depart_layer = text_layer_create(rect_depart);

  text_layer_set_background_color(s_time_layer, GColorBlack);
  text_layer_set_text_color(s_time_layer, GColorWhite);

  TextLayer *layers[] = {
    s_time_layer, s_station_layer, s_type_layer, s_dest_layer, s_countdown_layer, s_depart_layer
  };

  for (int i = 0; i < 6; i++) {
    text_layer_set_text_alignment(layers[i], GTextAlignmentCenter);

#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_GABBRO)
    text_layer_set_font(layers[i], fonts_get_system_font((i == 4) ? FONT_KEY_GOTHIC_28_BOLD : (i == 1) ? FONT_KEY_GOTHIC_24_BOLD : FONT_KEY_GOTHIC_18_BOLD));
#else
    text_layer_set_font(layers[i], fonts_get_system_font((i == 4) ? FONT_KEY_GOTHIC_28_BOLD : (i == 1) ? FONT_KEY_GOTHIC_24_BOLD : FONT_KEY_GOTHIC_18_BOLD));
#endif

    text_layer_set_overflow_mode(layers[i], GTextOverflowModeTrailingEllipsis);
    layer_add_child(w_layer, text_layer_get_layer(layers[i]));
  }

  s_toast_layer = text_layer_create(GRect((w - 110) / 2, (h - 32) / 2, 110, 32));
  text_layer_set_background_color(s_toast_layer, GColorBlack);
  text_layer_set_text_color(s_toast_layer, GColorWhite);
  text_layer_set_text_alignment(s_toast_layer, GTextAlignmentCenter);
  text_layer_set_font(s_toast_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  layer_set_hidden(text_layer_get_layer(s_toast_layer), true);
  layer_add_child(w_layer, text_layer_get_layer(s_toast_layer));
}

static void main_window_unload(Window *window) {
  if (s_toast_timer) {
    app_timer_cancel(s_toast_timer);
    s_toast_timer = NULL;
  }
  text_layer_destroy(s_toast_layer);
  text_layer_destroy(s_time_layer);
  text_layer_destroy(s_station_layer);
  text_layer_destroy(s_dest_layer);
  text_layer_destroy(s_countdown_layer);
  text_layer_destroy(s_depart_layer);
  text_layer_destroy(s_type_layer);
}

static void init() {
  s_main_window = window_create();
  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load = main_window_load,
    .unload = main_window_unload
  });

  if (persist_exists(1) && persist_exists(2)) {
    s_alarm_set = persist_read_bool(1);
    s_alarm_time = (time_t)persist_read_int(2);

    if (s_alarm_set && s_alarm_time > time(NULL)) {
      wakeup_schedule(s_alarm_time, ALARM_WAKEUP_COOKIE, true);
    } else {
      s_alarm_set = false;
      s_alarm_time = 0;
      persist_write_bool(1, false);
      persist_write_int(2, 0);
    }
  }

  window_stack_push(s_main_window, true);
  window_set_click_config_provider(s_main_window, click_config_provider);
  app_message_register_inbox_received(inbox_received_callback);
  app_message_open(512, 128);
  tick_timer_service_subscribe(SECOND_UNIT, tick_handler);
  wakeup_service_subscribe(wakeup_handler);

  /*
   * アプリが終了している状態でアラーム時刻を迎えた場合、
   * OSがアプリを新規起動する。この場合 wakeup_handler は
   * 呼ばれないため、launch_reason() で判定する必要がある。
   */
  if (launch_reason() == APP_LAUNCH_WAKEUP) {
    WakeupId woken_id = 0;
    int32_t woken_cookie = 0;

    if (wakeup_get_launch_event(&woken_id, &woken_cookie) && woken_cookie == ALARM_WAKEUP_COOKIE) {
      s_alarm_set = false;
      s_alarm_time = 0;
      persist_write_bool(1, false);
      persist_write_int(2, 0);
      request_timeline_action(3, 0);
      show_alarm_window();
    }
  }

#if defined(PBL_TOUCH)
  accel_tap_service_subscribe(tap_handler);
#endif
}

static void deinit() {
#if defined(PBL_TOUCH)
  accel_tap_service_unsubscribe();
#endif
  tick_timer_service_unsubscribe();

  if (s_alarm_window) {
    window_destroy(s_alarm_window);
    s_alarm_window = NULL;
  }
  window_destroy(s_main_window);
}

int main() {
  init();
  app_event_loop();
  deinit();
}