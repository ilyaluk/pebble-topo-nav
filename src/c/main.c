#include <pebble.h>

#define CHUNK_SIZE 3000
#if defined(PBL_PLATFORM_EMERY)
  #define MAP_WIDTH 200
  #define MAP_HEIGHT 150
  #define HEADER_HEIGHT 30
  #define FOOTER_HEIGHT 48
  #define DISTANCE_FONT FONT_KEY_GOTHIC_24_BOLD
  #define INSTRUCTION_FONT FONT_KEY_GOTHIC_18_BOLD
#elif defined(PBL_PLATFORM_CHALK)
  #define MAP_WIDTH 180
  #define MAP_HEIGHT 114
  #define HEADER_HEIGHT 30
  #define FOOTER_HEIGHT 36
  #define DISTANCE_FONT FONT_KEY_GOTHIC_18_BOLD
  #define INSTRUCTION_FONT FONT_KEY_GOTHIC_14_BOLD
#else // basalt, aplite
  #define MAP_WIDTH 144
  #define MAP_HEIGHT 112
  #define HEADER_HEIGHT 24
  #define FOOTER_HEIGHT 32
  #define DISTANCE_FONT FONT_KEY_GOTHIC_18_BOLD
  #define INSTRUCTION_FONT FONT_KEY_GOTHIC_14_BOLD
#endif

#define MAP_BUFFER_SIZE (MAP_WIDTH * MAP_HEIGHT)
#define PERSIST_KEY_LANGUAGE 100

// UI Elements
static Window *s_main_window;
static Layer *s_header_layer;
static Layer *s_map_layer;
static Layer *s_footer_layer;
static Layer *s_dashboard_layer;

static TextLayer *s_distance_layer;
static TextLayer *s_instruction_layer;

static TextLayer *s_dash_avg_speed_title_layer;
static TextLayer *s_dash_avg_speed_val_layer;
static TextLayer *s_dash_gain_title_layer;
static TextLayer *s_dash_gain_val_layer;
static TextLayer *s_dash_loss_title_layer;
static TextLayer *s_dash_loss_val_layer;
static TextLayer *s_dash_dist_title_layer;
static TextLayer *s_dash_dist_val_layer;
static TextLayer *s_dash_coords_title_layer;
static TextLayer *s_dash_coords_val_layer;

// State Variables
static bool s_show_dashboard = false;
static uint8_t s_zoom_level = 17;
static bool s_gps_connected = false;
static bool s_off_route = false;
static bool s_recording_active = false;
static int s_nav_bearing = -1; // -1 = no instruction, 0 = straight, 90 = right, 180 = uturn, 270 = left
static bool s_is_english = false;
static void update_ui_languages(void);

// Fullscreen setting and dynamic map dimensions
static uint16_t s_current_map_width = MAP_WIDTH;
static uint16_t s_current_map_height = MAP_HEIGHT;
static uint32_t s_current_map_buffer_size = MAP_WIDTH * MAP_HEIGHT;
static bool s_fullscreen_mode = false;

static GBitmap *s_map_bitmap = NULL;
static uint8_t *s_map_buffer = NULL;
static bool s_map_ready = false;

#define PERSIST_KEY_FULLSCREEN_MODE 101

static int s_gps_heading = -1;
static int s_gps_speed_cms = 0;

static GPath *s_arrow_outer_path = NULL;
static GPath *s_arrow_inner_path = NULL;

static const GPathInfo ARROW_OUTER_PATH_INFO = {
  .num_points = 4,
  .points = (GPoint []) {
    {0, -15},
    {10, 12},
    {0, 6},
    {-10, 12}
  }
};

static const GPathInfo ARROW_INNER_PATH_INFO = {
  .num_points = 4,
  .points = (GPoint []) {
    {0, -12},
    {8, 10},
    {0, 5},
    {-8, 10}
  }
};

#define MAX_ROUTES 15
static int s_route_count = 0;
static uint32_t s_route_ids[MAX_ROUTES];
static char s_route_names[MAX_ROUTES][32];

static void set_map_dimensions(int width, int height) {
  s_current_map_width = width;
  s_current_map_height = height;
  s_current_map_buffer_size = width * height;
  
  if (s_map_bitmap) {
    gbitmap_destroy(s_map_bitmap);
  }
  s_map_bitmap = gbitmap_create_blank(GSize(width, height), GBitmapFormat8Bit);
  s_map_buffer = gbitmap_get_data(s_map_bitmap);
  if (s_map_buffer) {
    memset(s_map_buffer, 0b11101010, s_current_map_buffer_size); // pre-populate with grey color
  }
  s_map_ready = false;
}

static void update_layout() {
  if (!s_main_window) return;
  Layer *window_layer = window_get_root_layer(s_main_window);
  GRect bounds = layer_get_bounds(window_layer);
  
  if (s_fullscreen_mode) {
    if (s_header_layer) layer_set_hidden(s_header_layer, true);
    if (s_footer_layer) layer_set_hidden(s_footer_layer, true);
    
    if (s_map_layer) {
      layer_set_frame(s_map_layer, GRect(0, 0, bounds.size.w, bounds.size.h));
    }
    set_map_dimensions(bounds.size.w, bounds.size.h);
  } else {
    if (s_header_layer) layer_set_hidden(s_header_layer, false);
    if (s_footer_layer) layer_set_hidden(s_footer_layer, false);
    
    if (s_map_layer) {
      layer_set_frame(s_map_layer, GRect(0, HEADER_HEIGHT, bounds.size.w, MAP_HEIGHT));
    }
    set_map_dimensions(MAP_WIDTH, MAP_HEIGHT);
  }
}
static uint32_t s_active_route_id = 0;

static Window *s_menu_window = NULL;
static MenuLayer *s_menu_layer = NULL;

static Window *s_confirm_window = NULL;
static TextLayer *s_confirm_text_layer = NULL;
static TextLayer *s_confirm_subtext_layer = NULL;
static uint32_t s_pending_route_id = 0;

static uint32_t s_received_chunks_mask = 0;
static uint32_t s_expected_chunks = 0;

static char s_distance_text[16] = "---";
static char s_instruction_text[64] = "Warte auf GPS...";
static char s_avg_speed_text[16] = "0.0 km/h";
static char s_elevation_gain_text[24] = "---m / ---m";
static char s_elevation_loss_text[24] = "---m / ---m";
static char s_trip_distance_text[16] = "--- / ---";
static char s_coords_text[32] = "---, ---";

// Haptic feedback levels
enum {
  VIBE_ALERT_NONE = 0,
  VIBE_ALERT_TURN = 1,
  VIBE_ALERT_OFF_ROUTE = 2
};

// Send zoom change to the phone JS companion
static void send_zoom_change() {
  DictionaryIterator *iter;
  app_message_outbox_begin(&iter);
  if (iter) {
    dict_write_uint8(iter, MESSAGE_KEY_ZOOM_LEVEL, s_zoom_level);
    dict_write_uint8(iter, MESSAGE_KEY_REQUEST_MAP_UPDATE, 1);
    app_message_outbox_send();
  }
}

// Drawing function for arrow icons
static void draw_arrow(GContext *ctx, GPoint center, int bearing) {
  graphics_context_set_stroke_width(ctx, 3);
  graphics_context_set_stroke_color(ctx, GColorOrange);
  
  // Center coordinates: x, y
  int cx = center.x;
  int cy = center.y;
  
  if (bearing >= 315 || bearing < 45) { // Straight / Forward
    graphics_draw_line(ctx, GPoint(cx, cy + 9), GPoint(cx, cy - 9));
    graphics_draw_line(ctx, GPoint(cx, cy - 9), GPoint(cx - 5, cy - 4));
    graphics_draw_line(ctx, GPoint(cx, cy - 9), GPoint(cx + 5, cy - 4));
  } else if (bearing >= 45 && bearing < 135) { // Right Turn
    graphics_draw_line(ctx, GPoint(cx - 9, cy), GPoint(cx + 9, cy));
    graphics_draw_line(ctx, GPoint(cx + 9, cy), GPoint(cx + 4, cy - 5));
    graphics_draw_line(ctx, GPoint(cx + 9, cy), GPoint(cx + 4, cy + 5));
  } else if (bearing >= 225 && bearing < 315) { // Left Turn
    graphics_draw_line(ctx, GPoint(cx + 9, cy), GPoint(cx - 9, cy));
    graphics_draw_line(ctx, GPoint(cx - 9, cy), GPoint(cx - 4, cy - 5));
    graphics_draw_line(ctx, GPoint(cx - 9, cy), GPoint(cx - 4, cy + 5));
  } else if (bearing >= 135 && bearing < 225) { // U-Turn
    // Draw U-shape
    graphics_draw_line(ctx, GPoint(cx - 5, cy + 6), GPoint(cx - 5, cy - 3));
    graphics_draw_line(ctx, GPoint(cx - 5, cy - 3), GPoint(cx + 5, cy - 3));
    graphics_draw_line(ctx, GPoint(cx + 5, cy - 3), GPoint(cx + 5, cy + 6));
    // Arrowhead pointing down on the right side
    graphics_draw_line(ctx, GPoint(cx + 5, cy + 6), GPoint(cx + 2, cy + 3));
    graphics_draw_line(ctx, GPoint(cx + 5, cy + 6), GPoint(cx + 8, cy + 3));
  }
}

// Header Update Callback (Draws turn arrow, status details, and turn distance)
static void header_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  
  // Background
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  
  // Separation line at bottom
  graphics_context_set_stroke_color(ctx, GColorLightGray);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_line(ctx, GPoint(0, bounds.size.h - 1), GPoint(bounds.size.w, bounds.size.h - 1));
  
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_CHALK)
  // Draw Zoom level text on top-right
  static char zoom_buf[8];
  snprintf(zoom_buf, sizeof(zoom_buf), "Z:%d", s_zoom_level);
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, zoom_buf, fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(bounds.size.w - 35, 2, 30, 20),
                     GTextOverflowModeWordWrap, GTextAlignmentRight, NULL);
  
  // Draw Battery percent text
  BatteryChargeState battery = battery_state_service_peek();
  static char battery_buf[8];
  snprintf(battery_buf, sizeof(battery_buf), "%d%%", battery.charge_percent);
  graphics_draw_text(ctx, battery_buf, fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(bounds.size.w - 75, 2, 35, 20),
                     GTextOverflowModeWordWrap, GTextAlignmentRight, NULL);
  
  // Draw GPS Status icon
  if (s_gps_connected) {
    graphics_context_set_fill_color(ctx, GColorIslamicGreen);
  } else {
    graphics_context_set_fill_color(ctx, GColorRed);
  }
  graphics_fill_rect(ctx, GRect(bounds.size.w - 87, 8, 6, 6), 3, GCornersAll);
  
  // Draw Recording Status dot
  if (s_recording_active) {
    graphics_context_set_fill_color(ctx, GColorRed);
    graphics_fill_rect(ctx, GRect(bounds.size.w - 102, 8, 6, 6), 3, GCornersAll);
  }
#else
  // On Basalt/Aplite (144px): Hide Zoom from header to save space
  BatteryChargeState battery = battery_state_service_peek();
  static char battery_buf[8];
  snprintf(battery_buf, sizeof(battery_buf), "%d%%", battery.charge_percent);
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, battery_buf, fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(bounds.size.w - 35, 2, 30, 20),
                     GTextOverflowModeWordWrap, GTextAlignmentRight, NULL);
                     
  // Draw GPS Status icon
  if (s_gps_connected) {
    graphics_context_set_fill_color(ctx, GColorIslamicGreen);
  } else {
    graphics_context_set_fill_color(ctx, GColorRed);
  }
  graphics_fill_rect(ctx, GRect(bounds.size.w - 47, 8, 6, 6), 3, GCornersAll);
  
  // Draw Recording Status dot
  if (s_recording_active) {
    graphics_context_set_fill_color(ctx, GColorRed);
    graphics_fill_rect(ctx, GRect(bounds.size.w - 62, 8, 6, 6), 3, GCornersAll);
  }
#endif
  
  // Draw Arrow
  if (s_nav_bearing != -1) {
    draw_arrow(ctx, GPoint(18, bounds.size.h / 2), s_nav_bearing);
  }
}

// Footer Update Callback (Draws current route street name/instruction)
static void footer_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  
  // Background
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  
  // Separation line at top
  graphics_context_set_stroke_color(ctx, GColorLightGray);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_line(ctx, GPoint(0, 0), GPoint(bounds.size.w, 0));
  
  // Off-route warning banner
  if (s_off_route) {
    graphics_context_set_fill_color(ctx, GColorRed);
    graphics_fill_rect(ctx, GRect(0, 1, bounds.size.w, 4), 0, GCornerNone);
  }
}

static int get_current_bearing() {
  // If speed is > 1.0 m/s (100 cm/s), use GPS heading/course
  if (s_gps_speed_cms > 100 && s_gps_heading >= 0) {
    return s_gps_heading;
  }
  
  // Otherwise fall back to Pebble's built-in compass
  CompassHeadingData compass;
  compass_service_peek(&compass);
  if (compass.compass_status != CompassStatusDataInvalid) {
    int heading_deg;
    if (compass.true_heading != TRIG_MAX_ANGLE) {
      heading_deg = (compass.true_heading * 360) / TRIG_MAX_ANGLE;
    } else {
      heading_deg = (compass.magnetic_heading * 360) / TRIG_MAX_ANGLE;
    }
    return heading_deg;
  }
  
  // Default fallbacks
  if (s_gps_heading >= 0) {
    return s_gps_heading;
  }
  return 0;
}

// Map Layer Update Callback (Renders the assembled GColor8 map bitmap)
static void map_layer_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  
  if (s_map_ready && s_map_bitmap) {
    graphics_draw_bitmap_in_rect(ctx, s_map_bitmap, bounds);
    
    // Draw the direction arrow at the center
    if (s_gps_connected && s_arrow_outer_path && s_arrow_inner_path) {
      int bearing = get_current_bearing();
      int32_t angle = (TRIG_MAX_ANGLE * bearing) / 360;
      GPoint center = GPoint(s_current_map_width / 2, s_current_map_height / 2);
      
      gpath_rotate_to(s_arrow_outer_path, angle);
      gpath_move_to(s_arrow_outer_path, center);
      graphics_context_set_fill_color(ctx, GColorWhite);
      gpath_draw_filled(ctx, s_arrow_outer_path);
      
      gpath_rotate_to(s_arrow_inner_path, angle);
      gpath_move_to(s_arrow_inner_path, center);
      graphics_context_set_fill_color(ctx, GColorBlue);
      gpath_draw_filled(ctx, s_arrow_inner_path);
    }
  } else {
    // Render grey loading background
    graphics_context_set_fill_color(ctx, GColorLightGray);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
    
    // Loading Text
    graphics_context_set_text_color(ctx, GColorDarkGray);
    const char *loading_text = s_gps_connected ? 
      (s_is_english ? "Loading map..." : "Karte wird geladen...") : 
      (s_is_english ? "No GPS signal" : "Kein GPS-Signal");
    graphics_draw_text(ctx, 
                       loading_text, 
                       fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                       GRect(10, bounds.size.h / 2 - 12, bounds.size.w - 20, 30),
                       GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  }
}

// Dashboard Layer Update Callback (Draws structure lines)
static void dashboard_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  
  // Background
  graphics_context_set_fill_color(ctx, GColorClear);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  
  // Draw separation lines
  graphics_context_set_stroke_color(ctx, GColorDarkGray);
  graphics_context_set_stroke_width(ctx, 2);
  
  int row_h = bounds.size.h / 3;
  
  // Horizontal lines
  graphics_draw_line(ctx, GPoint(10, row_h), GPoint(bounds.size.w - 10, row_h));
  graphics_draw_line(ctx, GPoint(10, row_h * 2), GPoint(bounds.size.w - 10, row_h * 2));
  
  // Vertical line in middle (spanning Row 0 and Row 1)
  graphics_draw_line(ctx, GPoint(bounds.size.w / 2, 5), GPoint(bounds.size.w / 2, row_h * 2 - 5));
}

// Button Clicks Handlers
static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  // Zoom In
  if (s_zoom_level < 18) {
    s_zoom_level++;
    layer_mark_dirty(s_header_layer);
    send_zoom_change();
  }
}

static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  // Zoom Out
  if (s_zoom_level > 12) {
    s_zoom_level--;
    layer_mark_dirty(s_header_layer);
    send_zoom_change();
  }
}

static void menu_window_load(Window *window);
static void menu_window_unload(Window *window);

static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (!s_show_dashboard) {
    // Toggle to Dashboard
    s_show_dashboard = true;
    layer_set_hidden(s_map_layer, s_show_dashboard);
    layer_set_hidden(s_footer_layer, s_show_dashboard);
    layer_set_hidden(s_dashboard_layer, !s_show_dashboard);
    if (s_fullscreen_mode) {
      layer_set_hidden(s_header_layer, false); // Show header on dashboard even if fullscreen map is enabled
    }
  } else {
    // Open Route Selection Menu
    if (!s_menu_window) {
      s_menu_window = window_create();
      window_set_window_handlers(s_menu_window, (WindowHandlers) {
        .load = menu_window_load,
        .unload = menu_window_unload
      });
    }
    window_stack_push(s_menu_window, true);
  }
}

static void back_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_show_dashboard) {
    // Return to map mode
    s_show_dashboard = false;
    layer_set_hidden(s_map_layer, s_show_dashboard);
    layer_set_hidden(s_footer_layer, s_show_dashboard);
    layer_set_hidden(s_dashboard_layer, !s_show_dashboard);
    if (s_fullscreen_mode) {
      layer_set_hidden(s_header_layer, true); // Re-hide header on map in fullscreen mode
    }
  } else {
    // Close the app by popping main window
    window_stack_pop(true);
  }
}

static void select_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  // Send recording toggle command to companion
  DictionaryIterator *iter;
  app_message_outbox_begin(&iter);
  if (iter) {
    dict_write_uint8(iter, MESSAGE_KEY_RECORDING_STATE, 1);
    app_message_outbox_send();
  }
  // Vibrate watch to confirm long press
  vibes_short_pulse();
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, 500, select_long_click_handler, NULL);
  window_single_click_subscribe(BUTTON_ID_BACK, back_click_handler);
}

// AppMessage Callback Handlers
static void inbox_received_handler(DictionaryIterator *iter, void *context) {
  Tuple *lang_tuple = dict_find(iter, MESSAGE_KEY_LANGUAGE);
  if (lang_tuple) {
    s_is_english = (lang_tuple->value->uint8 == 1);
    persist_write_bool(PERSIST_KEY_LANGUAGE, s_is_english);
    update_ui_languages();
    if (s_map_layer) {
      layer_mark_dirty(s_map_layer);
    }
  }

  // Handle text values
  Tuple *dist_tuple = dict_find(iter, MESSAGE_KEY_NAV_DISTANCE);
  if (dist_tuple && s_distance_layer) {
    snprintf(s_distance_text, sizeof(s_distance_text), "%s", dist_tuple->value->cstring);
    text_layer_set_text(s_distance_layer, s_distance_text);
  }
  
  Tuple *inst_tuple = dict_find(iter, MESSAGE_KEY_NAV_INSTRUCTION);
  if (inst_tuple && s_instruction_layer) {
    snprintf(s_instruction_text, sizeof(s_instruction_text), "%s", inst_tuple->value->cstring);
    text_layer_set_text(s_instruction_layer, s_instruction_text);
  }
  
  Tuple *bearing_tuple = dict_find(iter, MESSAGE_KEY_NAV_BEARING);
  if (bearing_tuple) {
    s_nav_bearing = bearing_tuple->value->int32;
    if (s_header_layer) {
      layer_mark_dirty(s_header_layer);
    }
  }
  
  Tuple *gps_tuple = dict_find(iter, MESSAGE_KEY_GPS_CONNECTED);
  if (gps_tuple) {
    s_gps_connected = (gps_tuple->value->uint8 == 1);
    if (s_header_layer) {
      layer_mark_dirty(s_header_layer);
    }
    if (s_map_layer) {
      layer_mark_dirty(s_map_layer);
    }
  }
  
  Tuple *off_route_tuple = dict_find(iter, MESSAGE_KEY_OFF_ROUTE);
  if (off_route_tuple) {
    s_off_route = (off_route_tuple->value->uint8 == 1);
    if (s_footer_layer) {
      layer_mark_dirty(s_footer_layer);
    }
  }

  Tuple *rec_tuple = dict_find(iter, MESSAGE_KEY_RECORDING_STATE);
  if (rec_tuple) {
    s_recording_active = (rec_tuple->value->uint8 == 1);
    if (s_header_layer) {
      layer_mark_dirty(s_header_layer);
    }
  }

  Tuple *gps_speed_tuple = dict_find(iter, MESSAGE_KEY_GPS_SPEED);
  if (gps_speed_tuple) {
    s_gps_speed_cms = gps_speed_tuple->value->int32;
  }
  
  Tuple *gps_heading_tuple = dict_find(iter, MESSAGE_KEY_GPS_HEADING);
  if (gps_heading_tuple) {
    s_gps_heading = gps_heading_tuple->value->int32;
  }

  Tuple *active_route_tuple = dict_find(iter, MESSAGE_KEY_ACTIVE_ROUTE_ID);
  if (active_route_tuple) {
    s_active_route_id = active_route_tuple->value->uint32;
    if (s_menu_layer) {
      menu_layer_reload_data(s_menu_layer);
    }
  }
  
  Tuple *fullscreen_tuple = dict_find(iter, MESSAGE_KEY_FULLSCREEN_MODE);
  if (fullscreen_tuple) {
    bool new_fullscreen = (fullscreen_tuple->value->uint8 == 1);
    if (new_fullscreen != s_fullscreen_mode) {
      s_fullscreen_mode = new_fullscreen;
      persist_write_bool(PERSIST_KEY_FULLSCREEN_MODE, s_fullscreen_mode);
      update_layout();
    }
  }

  Tuple *route_count_tuple = dict_find(iter, MESSAGE_KEY_ROUTE_COUNT);
  if (route_count_tuple) {
    s_route_count = 0; // Reset count for syncing new list
  }

  Tuple *route_index_tuple = dict_find(iter, MESSAGE_KEY_ROUTE_INDEX);
  Tuple *route_id_tuple = dict_find(iter, MESSAGE_KEY_ROUTE_ID);
  Tuple *route_name_tuple = dict_find(iter, MESSAGE_KEY_ROUTE_NAME);
  
  if (route_index_tuple && route_id_tuple && route_name_tuple) {
    uint16_t idx = route_index_tuple->value->uint16;
    if (idx < MAX_ROUTES) {
      s_route_ids[idx] = route_id_tuple->value->uint32;
      snprintf(s_route_names[idx], sizeof(s_route_names[idx]), "%s", route_name_tuple->value->cstring);
      
      if (idx >= s_route_count) {
        s_route_count = idx + 1;
      }
      
      if (s_menu_layer) {
        menu_layer_reload_data(s_menu_layer);
      }
    }
  }
  
  // Dashboard fields
  Tuple *avg_speed_tuple = dict_find(iter, MESSAGE_KEY_AVG_SPEED);
  if (avg_speed_tuple && s_dash_avg_speed_val_layer) {
    snprintf(s_avg_speed_text, sizeof(s_avg_speed_text), "%s", avg_speed_tuple->value->cstring);
    text_layer_set_text(s_dash_avg_speed_val_layer, s_avg_speed_text);
  }
  Tuple *gain_tuple = dict_find(iter, MESSAGE_KEY_ELEVATION_GAIN);
  if (gain_tuple && s_dash_gain_val_layer) {
    snprintf(s_elevation_gain_text, sizeof(s_elevation_gain_text), "%s", gain_tuple->value->cstring);
    text_layer_set_text(s_dash_gain_val_layer, s_elevation_gain_text);
  }
  Tuple *loss_tuple = dict_find(iter, MESSAGE_KEY_ELEVATION_LOSS);
  if (loss_tuple && s_dash_loss_val_layer) {
    snprintf(s_elevation_loss_text, sizeof(s_elevation_loss_text), "%s", loss_tuple->value->cstring);
    text_layer_set_text(s_dash_loss_val_layer, s_elevation_loss_text);
  }
  Tuple *trip_dist_tuple = dict_find(iter, MESSAGE_KEY_TRIP_DISTANCE);
  if (trip_dist_tuple && s_dash_dist_val_layer) {
    snprintf(s_trip_distance_text, sizeof(s_trip_distance_text), "%s", trip_dist_tuple->value->cstring);
    text_layer_set_text(s_dash_dist_val_layer, s_trip_distance_text);
  }
  Tuple *coords_tuple = dict_find(iter, MESSAGE_KEY_GPS_COORDS);
  if (coords_tuple && s_dash_coords_val_layer) {
    snprintf(s_coords_text, sizeof(s_coords_text), "%s", coords_tuple->value->cstring);
    text_layer_set_text(s_dash_coords_val_layer, s_coords_text);
  }
  
  // Check for Haptic/Vibe signal
  Tuple *vibe_tuple = dict_find(iter, MESSAGE_KEY_VIBRATE_ALERT);
  if (vibe_tuple) {
    uint8_t alert_type = vibe_tuple->value->uint8;
    if (alert_type == VIBE_ALERT_TURN) {
      vibes_short_pulse();
    } else if (alert_type == VIBE_ALERT_OFF_ROUTE) {
      vibes_double_pulse();
    }
  }

  // Handle Map Chunk Transmission
  Tuple *chunk_tuple = dict_find(iter, MESSAGE_KEY_MAP_DATA_CHUNK);
  Tuple *index_tuple = dict_find(iter, MESSAGE_KEY_CHUNK_INDEX);
  Tuple *total_tuple = dict_find(iter, MESSAGE_KEY_TOTAL_CHUNKS);
  
  if (chunk_tuple && index_tuple && total_tuple) {
    uint32_t chunk_idx = index_tuple->value->uint32;
    uint32_t total_chunks = total_tuple->value->uint32;
    uint8_t *chunk_data = chunk_tuple->value->data;
    uint16_t chunk_len = chunk_tuple->length;
    
    // Clear mask on starting a new image
    if (chunk_idx == 0) {
      s_received_chunks_mask = 0;
    }
    
    // Copy incoming bytes to GBitmap buffer
    if (s_map_buffer && (chunk_idx * CHUNK_SIZE + chunk_len <= s_current_map_buffer_size)) {
      memcpy(s_map_buffer + (chunk_idx * CHUNK_SIZE), chunk_data, chunk_len);
      s_received_chunks_mask |= (1 << chunk_idx);
      s_expected_chunks = total_chunks;
      
      // Check if all chunks received
      uint32_t completed_mask = (1 << s_expected_chunks) - 1;
      if (s_received_chunks_mask == completed_mask) {
        s_map_ready = true;
        layer_mark_dirty(s_map_layer);
      }
    }
  }
}

static void inbox_dropped_handler(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Inbox dropped: %d", reason);
}

static void outbox_failed_handler(DictionaryIterator *iter, AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Outbox failed: %d", reason);
}

static void update_ui_languages() {
  if (s_dash_avg_speed_title_layer) {
    text_layer_set_text(s_dash_avg_speed_title_layer, s_is_english ? "AVG SPEED" : "Ø-GESCHWIND.");
  }
  if (s_dash_dist_title_layer) {
    text_layer_set_text(s_dash_dist_title_layer, s_is_english ? "DISTANCE (W/R)" : "DISTANZ (G/R)");
  }
  if (s_dash_gain_title_layer) {
    text_layer_set_text(s_dash_gain_title_layer, s_is_english ? "ELEV GAIN" : "HM AUFSTIEG");
  }
  if (s_dash_loss_title_layer) {
    text_layer_set_text(s_dash_loss_title_layer, s_is_english ? "ELEV LOSS" : "HM ABSTIEG");
  }
  if (s_dash_coords_title_layer) {
    text_layer_set_text(s_dash_coords_title_layer, s_is_english ? "GPS COORDS (SELECT: MENU)" : "GPS KOORDINATEN (SELECT: MENÜ)");
  }
  if (s_instruction_layer) {
    const char *curr_text = text_layer_get_text(s_instruction_layer);
    if (curr_text && (strcmp(curr_text, "Warte auf GPS...") == 0 || strcmp(curr_text, "Waiting for GPS...") == 0)) {
      snprintf(s_instruction_text, sizeof(s_instruction_text), "%s", s_is_english ? "Waiting for GPS..." : "Warte auf GPS...");
      text_layer_set_text(s_instruction_layer, s_instruction_text);
    }
  }
}

// Window Loading Procedures
static void main_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer); // 200x228 for Emery
  
  // Load fullscreen mode state
  s_fullscreen_mode = persist_exists(PERSIST_KEY_FULLSCREEN_MODE) ? persist_read_bool(PERSIST_KEY_FULLSCREEN_MODE) : false;
  
  s_arrow_outer_path = gpath_create(&ARROW_OUTER_PATH_INFO);
  s_arrow_inner_path = gpath_create(&ARROW_INNER_PATH_INFO);
  
  // 1. Header Layer (0 to HEADER_HEIGHT)
  s_header_layer = layer_create(GRect(0, 0, bounds.size.w, HEADER_HEIGHT));
  layer_set_update_proc(s_header_layer, header_update_proc);
  layer_add_child(window_layer, s_header_layer);
  
  // Distance text in header (left indent for arrow)
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_CHALK)
  s_distance_layer = text_layer_create(GRect(35, 2, bounds.size.w - 137, 26));
#else
  s_distance_layer = text_layer_create(GRect(32, 0, bounds.size.w - 94, 24));
#endif
  text_layer_set_background_color(s_distance_layer, GColorClear);
  text_layer_set_text_color(s_distance_layer, GColorBlack);
  text_layer_set_font(s_distance_layer, fonts_get_system_font(DISTANCE_FONT));
  text_layer_set_text(s_distance_layer, s_distance_text);
  layer_add_child(s_header_layer, text_layer_get_layer(s_distance_layer));
  
  // 2. Map Layer
  s_map_layer = layer_create(GRect(0, HEADER_HEIGHT, bounds.size.w, MAP_HEIGHT));
  layer_set_update_proc(s_map_layer, map_layer_update_proc);
  layer_add_child(window_layer, s_map_layer);
  
  // 3. Footer Layer
  s_footer_layer = layer_create(GRect(0, HEADER_HEIGHT + MAP_HEIGHT, bounds.size.w, FOOTER_HEIGHT));
  layer_set_update_proc(s_footer_layer, footer_update_proc);
  layer_add_child(window_layer, s_footer_layer);
  
  // Instruction Text in Footer
  s_instruction_layer = text_layer_create(GRect(6, 2, bounds.size.w - 12, FOOTER_HEIGHT - 4));
  text_layer_set_background_color(s_instruction_layer, GColorClear);
  text_layer_set_text_color(s_instruction_layer, GColorBlack);
  text_layer_set_font(s_instruction_layer, fonts_get_system_font(INSTRUCTION_FONT));
  text_layer_set_text_alignment(s_instruction_layer, GTextAlignmentCenter);
  text_layer_set_text(s_instruction_layer, s_instruction_text);
  layer_add_child(s_footer_layer, text_layer_get_layer(s_instruction_layer));
  
  // 4. Dashboard Layer (overlays map and footer, hidden initially)
  s_dashboard_layer = layer_create(GRect(0, HEADER_HEIGHT, bounds.size.w, bounds.size.h - HEADER_HEIGHT));
  layer_set_update_proc(s_dashboard_layer, dashboard_update_proc);
  layer_set_hidden(s_dashboard_layer, true); // hidden on launch
  layer_add_child(window_layer, s_dashboard_layer);
  
  int dash_h = bounds.size.h - HEADER_HEIGHT;
  int row_h = dash_h / 3;
  int col_w = (bounds.size.w - 15) / 2;
  int col2_x = col_w + 10;
  
  // Row 0: Left: Average Speed, Right: Remaining Distance
  s_dash_avg_speed_title_layer = text_layer_create(GRect(5, 2, col_w, 14));
  text_layer_set_background_color(s_dash_avg_speed_title_layer, GColorClear);
  text_layer_set_text_color(s_dash_avg_speed_title_layer, GColorDarkGray);
  text_layer_set_font(s_dash_avg_speed_title_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_dash_avg_speed_title_layer, GTextAlignmentCenter);
  text_layer_set_text(s_dash_avg_speed_title_layer, "Ø-GESCHWIND.");
  layer_add_child(s_dashboard_layer, text_layer_get_layer(s_dash_avg_speed_title_layer));
  
  s_dash_avg_speed_val_layer = text_layer_create(GRect(5, 16, col_w, row_h - 18));
  text_layer_set_background_color(s_dash_avg_speed_val_layer, GColorClear);
  text_layer_set_text_color(s_dash_avg_speed_val_layer, GColorBlack);
  text_layer_set_font(s_dash_avg_speed_val_layer, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD));
  text_layer_set_text_alignment(s_dash_avg_speed_val_layer, GTextAlignmentCenter);
  text_layer_set_text(s_dash_avg_speed_val_layer, s_avg_speed_text);
  layer_add_child(s_dashboard_layer, text_layer_get_layer(s_dash_avg_speed_val_layer));
  
  s_dash_dist_title_layer = text_layer_create(GRect(col2_x, 2, col_w, 14));
  text_layer_set_background_color(s_dash_dist_title_layer, GColorClear);
  text_layer_set_text_color(s_dash_dist_title_layer, GColorDarkGray);
  text_layer_set_font(s_dash_dist_title_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_dash_dist_title_layer, GTextAlignmentCenter);
  text_layer_set_text(s_dash_dist_title_layer, "DISTANZ (G/R)");
  layer_add_child(s_dashboard_layer, text_layer_get_layer(s_dash_dist_title_layer));
  
  s_dash_dist_val_layer = text_layer_create(GRect(col2_x, 16, col_w, row_h - 18));
  text_layer_set_background_color(s_dash_dist_val_layer, GColorClear);
  text_layer_set_text_color(s_dash_dist_val_layer, GColorBlack);
  text_layer_set_font(s_dash_dist_val_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_dash_dist_val_layer, GTextAlignmentCenter);
  text_layer_set_text(s_dash_dist_val_layer, s_trip_distance_text);
  layer_add_child(s_dashboard_layer, text_layer_get_layer(s_dash_dist_val_layer));
  
  // Row 1: Left: Elevation Gain, Right: Elevation Loss
  s_dash_gain_title_layer = text_layer_create(GRect(5, row_h + 2, col_w, 14));
  text_layer_set_background_color(s_dash_gain_title_layer, GColorClear);
  text_layer_set_text_color(s_dash_gain_title_layer, GColorDarkGray);
  text_layer_set_font(s_dash_gain_title_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_dash_gain_title_layer, GTextAlignmentCenter);
  text_layer_set_text(s_dash_gain_title_layer, "HM AUFSTIEG");
  layer_add_child(s_dashboard_layer, text_layer_get_layer(s_dash_gain_title_layer));
  
  s_dash_gain_val_layer = text_layer_create(GRect(5, row_h + 16, col_w, row_h - 18));
  text_layer_set_background_color(s_dash_gain_val_layer, GColorClear);
  text_layer_set_text_color(s_dash_gain_val_layer, GColorBlack);
  text_layer_set_font(s_dash_gain_val_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_dash_gain_val_layer, GTextAlignmentCenter);
  text_layer_set_text(s_dash_gain_val_layer, s_elevation_gain_text);
  layer_add_child(s_dashboard_layer, text_layer_get_layer(s_dash_gain_val_layer));
  
  s_dash_loss_title_layer = text_layer_create(GRect(col2_x, row_h + 2, col_w, 14));
  text_layer_set_background_color(s_dash_loss_title_layer, GColorClear);
  text_layer_set_text_color(s_dash_loss_title_layer, GColorDarkGray);
  text_layer_set_font(s_dash_loss_title_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_dash_loss_title_layer, GTextAlignmentCenter);
  text_layer_set_text(s_dash_loss_title_layer, "HM ABSTIEG");
  layer_add_child(s_dashboard_layer, text_layer_get_layer(s_dash_loss_title_layer));
  
  s_dash_loss_val_layer = text_layer_create(GRect(col2_x, row_h + 16, col_w, row_h - 18));
  text_layer_set_background_color(s_dash_loss_val_layer, GColorClear);
  text_layer_set_text_color(s_dash_loss_val_layer, GColorBlack);
  text_layer_set_font(s_dash_loss_val_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_dash_loss_val_layer, GTextAlignmentCenter);
  text_layer_set_text(s_dash_loss_val_layer, s_elevation_loss_text);
  layer_add_child(s_dashboard_layer, text_layer_get_layer(s_dash_loss_val_layer));
  
  // Row 2: GPS Coordinates
  s_dash_coords_title_layer = text_layer_create(GRect(10, (row_h * 2) + 2, bounds.size.w - 20, 14));
  text_layer_set_background_color(s_dash_coords_title_layer, GColorClear);
  text_layer_set_text_color(s_dash_coords_title_layer, GColorDarkGray);
  text_layer_set_font(s_dash_coords_title_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_dash_coords_title_layer, GTextAlignmentCenter);
  text_layer_set_text(s_dash_coords_title_layer, "GPS KOORDINATEN");
  layer_add_child(s_dashboard_layer, text_layer_get_layer(s_dash_coords_title_layer));
  
  s_dash_coords_val_layer = text_layer_create(GRect(10, (row_h * 2) + 16, bounds.size.w - 20, row_h - 18));
  text_layer_set_background_color(s_dash_coords_val_layer, GColorClear);
  text_layer_set_text_color(s_dash_coords_val_layer, GColorBlack);
  text_layer_set_font(s_dash_coords_val_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(s_dash_coords_val_layer, GTextAlignmentCenter);
  text_layer_set_text(s_dash_coords_val_layer, s_coords_text);
  layer_add_child(s_dashboard_layer, text_layer_get_layer(s_dash_coords_val_layer));
  
  update_ui_languages();
  update_layout();
}

// Window Unloading Procedures
static void main_window_unload(Window *window) {
  // Free GPaths
  if (s_arrow_outer_path) {
    gpath_destroy(s_arrow_outer_path);
    s_arrow_outer_path = NULL;
  }
  if (s_arrow_inner_path) {
    gpath_destroy(s_arrow_inner_path);
    s_arrow_inner_path = NULL;
  }

  // Free buffers
  if (s_map_bitmap) {
    gbitmap_destroy(s_map_bitmap);
    s_map_bitmap = NULL;
    s_map_buffer = NULL;
  }
  
  // Free layers
  text_layer_destroy(s_distance_layer);
  text_layer_destroy(s_instruction_layer);
  layer_destroy(s_header_layer);
  layer_destroy(s_map_layer);
  layer_destroy(s_footer_layer);
  
  text_layer_destroy(s_dash_avg_speed_title_layer);
  text_layer_destroy(s_dash_avg_speed_val_layer);
  text_layer_destroy(s_dash_gain_title_layer);
  text_layer_destroy(s_dash_gain_val_layer);
  text_layer_destroy(s_dash_loss_title_layer);
  text_layer_destroy(s_dash_loss_val_layer);
  text_layer_destroy(s_dash_dist_title_layer);
  text_layer_destroy(s_dash_dist_val_layer);
  text_layer_destroy(s_dash_coords_title_layer);
  text_layer_destroy(s_dash_coords_val_layer);
  layer_destroy(s_dashboard_layer);
}

static void battery_state_handler(BatteryChargeState charge) {
  if (s_header_layer) {
    layer_mark_dirty(s_header_layer);
  }
}

static void compass_heading_handler(CompassHeadingData heading) {
  // Force redraw map layer on compass updates (if map is active/visible)
  if (s_map_layer && !s_show_dashboard) {
    layer_mark_dirty(s_map_layer);
  }
}

// Route selection Menu callbacks
static uint16_t menu_get_num_sections_callback(MenuLayer *menu_layer, void *data) {
  return 1;
}

static uint16_t menu_get_num_rows_callback(MenuLayer *menu_layer, uint16_t section_index, void *data) {
  return s_route_count > 0 ? s_route_count : 1;
}

static int16_t menu_get_header_height_callback(MenuLayer *menu_layer, uint16_t section_index, void *data) {
  return 24;
}

static void menu_draw_header_callback(GContext* ctx, const Layer *cell_layer, uint16_t section_index, void *data) {
  menu_cell_basic_header_draw(ctx, cell_layer, s_is_english ? "Select Route" : "Route auswählen");
}

static void menu_draw_row_callback(GContext* ctx, const Layer *cell_layer, MenuIndex *cell_index, void *data) {
  if (s_route_count == 0) {
    menu_cell_basic_draw(ctx, cell_layer, 
                         s_is_english ? "No routes synced" : "Keine Routen synchr.", 
                         s_is_english ? "Add in settings" : "In Einstellungen laden", 
                         NULL);
    return;
  }
  
  uint16_t row = cell_index->row;
  if (row < MAX_ROUTES) {
    bool is_active = (s_route_ids[row] == s_active_route_id && s_active_route_id != 0);
    
    static char title_buf[48];
    if (is_active) {
      snprintf(title_buf, sizeof(title_buf), "[X] %s", s_route_names[row]);
    } else {
      snprintf(title_buf, sizeof(title_buf), "    %s", s_route_names[row]);
    }
    
    menu_cell_basic_draw(ctx, cell_layer, 
                         title_buf, 
                         is_active ? (s_is_english ? "Active (Select to stop)" : "Aktiv (Klick zum Stoppen)") : "", 
                         NULL);
  }
}

static void confirm_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  // Confirm action: send s_pending_route_id to phone
  DictionaryIterator *iter;
  app_message_outbox_begin(&iter);
  if (iter) {
    dict_write_uint32(iter, MESSAGE_KEY_ROUTE_ID, s_pending_route_id);
    app_message_outbox_send();
  }
  
  // Also set s_active_route_id locally immediately for UI responsiveness
  s_active_route_id = s_pending_route_id;
  if (s_menu_layer) {
    menu_layer_reload_data(s_menu_layer);
  }
  
  // Vibration feedback: double pulse if stopping, short pulse if starting/switching
  if (s_pending_route_id == 0) {
    vibes_double_pulse();
  } else {
    vibes_short_pulse();
  }
  
  // Remove the menu window from stack so popping confirmation returns to main map screen
  if (s_menu_window) {
    window_stack_remove(s_menu_window, false);
  }
  
  // Pop confirmation window
  window_stack_pop(true);
}

static void confirm_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, confirm_select_click_handler);
}

static void confirm_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);
  
  s_confirm_text_layer = text_layer_create(GRect(5, 15, bounds.size.w - 10, 60));
  text_layer_set_background_color(s_confirm_text_layer, GColorClear);
  text_layer_set_text_color(s_confirm_text_layer, GColorBlack);
  text_layer_set_font(s_confirm_text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(s_confirm_text_layer, GTextAlignmentCenter);
  
  s_confirm_subtext_layer = text_layer_create(GRect(5, 75, bounds.size.w - 10, 70));
  text_layer_set_background_color(s_confirm_subtext_layer, GColorClear);
  text_layer_set_text_color(s_confirm_subtext_layer, GColorDarkGray);
  text_layer_set_font(s_confirm_subtext_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_text_alignment(s_confirm_subtext_layer, GTextAlignmentCenter);
  
  if (s_pending_route_id == 0) {
    // Stopping route
    text_layer_set_text(s_confirm_text_layer, s_is_english ? "Stop navigation?" : "Navi stoppen?");
    text_layer_set_text(s_confirm_subtext_layer, s_is_english ? "SELECT: Confirm\nBACK: Cancel" : "SELECT: Ja\nBACK: Nein");
  } else {
    if (s_active_route_id != 0) {
      // Overwriting existing active route navigation
      text_layer_set_text(s_confirm_text_layer, s_is_english ? "Switch route?" : "Route wechseln?");
      text_layer_set_text(s_confirm_subtext_layer, s_is_english ? "Saves current trip.\nSELECT: Start\nBACK: Cancel" : "Speichert aktuelle.\nSELECT: Start\nBACK: Nein");
    } else {
      // Starting new route navigation
      text_layer_set_text(s_confirm_text_layer, s_is_english ? "Start navigation?" : "Navi starten?");
      text_layer_set_text(s_confirm_subtext_layer, s_is_english ? "SELECT: Start\nBACK: Cancel" : "SELECT: Start\nBACK: Nein");
    }
  }
  
  layer_add_child(window_layer, text_layer_get_layer(s_confirm_text_layer));
  layer_add_child(window_layer, text_layer_get_layer(s_confirm_subtext_layer));
}

static void confirm_window_unload(Window *window) {
  if (s_confirm_text_layer) {
    text_layer_destroy(s_confirm_text_layer);
    s_confirm_text_layer = NULL;
  }
  if (s_confirm_subtext_layer) {
    text_layer_destroy(s_confirm_subtext_layer);
    s_confirm_subtext_layer = NULL;
  }
  if (s_confirm_window) {
    window_destroy(s_confirm_window);
    s_confirm_window = NULL;
  }
}

static void show_confirm_window() {
  if (!s_confirm_window) {
    s_confirm_window = window_create();
    window_set_click_config_provider(s_confirm_window, confirm_click_config_provider);
    window_set_window_handlers(s_confirm_window, (WindowHandlers) {
      .load = confirm_window_load,
      .unload = confirm_window_unload
    });
  }
  window_stack_push(s_confirm_window, true);
}

static void menu_select_callback(MenuLayer *menu_layer, MenuIndex *cell_index, void *data) {
  if (s_route_count == 0) return;
  
  uint16_t row = cell_index->row;
  if (row < MAX_ROUTES) {
    bool is_active = (s_route_ids[row] == s_active_route_id && s_active_route_id != 0);
    
    if (is_active) {
      s_pending_route_id = 0;
    } else {
      s_pending_route_id = s_route_ids[row];
    }
    show_confirm_window();
  }
}

static void menu_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_frame(window_layer);
  
  s_menu_layer = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks) {
    .get_num_sections = menu_get_num_sections_callback,
    .get_num_rows = menu_get_num_rows_callback,
    .get_header_height = menu_get_header_height_callback,
    .draw_header = menu_draw_header_callback,
    .draw_row = menu_draw_row_callback,
    .select_click = menu_select_callback,
  });
  
  menu_layer_set_click_config_onto_window(s_menu_layer, window);
  layer_add_child(window_layer, menu_layer_get_layer(s_menu_layer));
}

static void menu_window_unload(Window *window) {
  if (s_menu_layer) {
    menu_layer_destroy(s_menu_layer);
    s_menu_layer = NULL;
  }
  if (s_menu_window) {
    window_destroy(s_menu_window);
    s_menu_window = NULL;
  }
}

// App Initialization
static void init() {
  if (persist_exists(PERSIST_KEY_LANGUAGE)) {
    s_is_english = persist_read_bool(PERSIST_KEY_LANGUAGE);
  } else {
    s_is_english = false;
  }

  s_main_window = window_create();
  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load = main_window_load,
    .unload = main_window_unload
  });
  
  // Set action click handlers
  window_set_click_config_provider(s_main_window, click_config_provider);
  
  // Register AppMessage listeners
  app_message_register_inbox_received(inbox_received_handler);
  app_message_register_inbox_dropped(inbox_dropped_handler);
  app_message_register_outbox_failed(outbox_failed_handler);
  
  // Allocate buffer for AppMessages (3400 inbox, 128 outbox)
  app_message_open(3400, 128);
  
  // Register battery state service
  battery_state_service_subscribe(battery_state_handler);
  
  // Subscribe to compass service
  compass_service_subscribe(compass_heading_handler);
  compass_service_set_heading_filter(2 * (TRIG_MAX_ANGLE / 360));
  
  window_stack_push(s_main_window, true);
}

// App Deinitialization
static void deinit() {
  compass_service_unsubscribe();
  battery_state_service_unsubscribe();
  window_destroy(s_main_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
