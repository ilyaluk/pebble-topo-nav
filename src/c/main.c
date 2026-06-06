#include <pebble.h>

#define CHUNK_SIZE 3000
#define MAP_WIDTH 200
#define MAP_HEIGHT 150
#define MAP_BUFFER_SIZE (MAP_WIDTH * MAP_HEIGHT)

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

// State Variables
static bool s_show_dashboard = false;
static uint8_t s_zoom_level = 15;
static bool s_gps_connected = false;
static bool s_off_route = false;
static int s_nav_bearing = -1; // -1 = no instruction, 0 = straight, 90 = right, 180 = uturn, 270 = left

static GBitmap *s_map_bitmap = NULL;
static uint8_t *s_map_buffer = NULL;
static bool s_map_ready = false;
static uint32_t s_received_chunks_mask = 0;
static uint32_t s_expected_chunks = 0;

static char s_distance_text[16] = "---";
static char s_instruction_text[64] = "Warte auf GPS...";
static char s_avg_speed_text[16] = "0.0 km/h";
static char s_elevation_gain_text[24] = "---m / ---m";
static char s_elevation_loss_text[24] = "---m / ---m";
static char s_trip_distance_text[16] = "--- km";

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
  
  // Draw Zoom level text on top-right
  static char zoom_buf[8];
  snprintf(zoom_buf, sizeof(zoom_buf), "Z:%d", s_zoom_level);
  graphics_context_set_text_color(ctx, GColorBlack);
  graphics_draw_text(ctx, zoom_buf, fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(bounds.size.w - 35, 2, 30, 20),
                     GTextOverflowModeWordWrap, GTextAlignmentRight, NULL);
  
  // Draw GPS Status icon
  if (s_gps_connected) {
    graphics_context_set_fill_color(ctx, GColorIslamicGreen);
  } else {
    graphics_context_set_fill_color(ctx, GColorRed);
  }
  graphics_fill_rect(ctx, GRect(bounds.size.w - 48, 8, 6, 6), 3, GCornersAll);
  
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

// Map Layer Update Callback (Renders the assembled GColor8 map bitmap)
static void map_layer_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  
  if (s_map_ready && s_map_bitmap) {
    graphics_draw_bitmap_in_rect(ctx, s_map_bitmap, bounds);
  } else {
    // Render grey loading background
    graphics_context_set_fill_color(ctx, GColorLightGray);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
    
    // Loading Text
    graphics_context_set_text_color(ctx, GColorDarkGray);
    graphics_draw_text(ctx, 
                       s_gps_connected ? "Karte wird geladen..." : "Kein GPS-Signal", 
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
  
  // Draw separation lines for 4 rows
  graphics_context_set_stroke_color(ctx, GColorDarkGray);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_line(ctx, GPoint(10, bounds.size.h / 4), GPoint(bounds.size.w - 10, bounds.size.h / 4));
  graphics_draw_line(ctx, GPoint(10, bounds.size.h / 2), GPoint(bounds.size.w - 10, bounds.size.h / 2));
  graphics_draw_line(ctx, GPoint(10, (bounds.size.h * 3) / 4), GPoint(bounds.size.w - 10, (bounds.size.h * 3) / 4));
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

static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  // Toggle between Map and Dashboard
  s_show_dashboard = !s_show_dashboard;
  
  layer_set_hidden(s_map_layer, s_show_dashboard);
  layer_set_hidden(s_footer_layer, s_show_dashboard);
  layer_set_hidden(s_dashboard_layer, !s_show_dashboard);
  
  // In Dashboard mode, header is kept for GPS status & distance to turn
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
}

// AppMessage Callback Handlers
static void inbox_received_handler(DictionaryIterator *iter, void *context) {
  // Handle text values
  Tuple *dist_tuple = dict_find(iter, MESSAGE_KEY_NAV_DISTANCE);
  if (dist_tuple) {
    snprintf(s_distance_text, sizeof(s_distance_text), "%s", dist_tuple->value->cstring);
    text_layer_set_text(s_distance_layer, s_distance_text);
  }
  
  Tuple *inst_tuple = dict_find(iter, MESSAGE_KEY_NAV_INSTRUCTION);
  if (inst_tuple) {
    snprintf(s_instruction_text, sizeof(s_instruction_text), "%s", inst_tuple->value->cstring);
    text_layer_set_text(s_instruction_layer, s_instruction_text);
  }
  
  Tuple *bearing_tuple = dict_find(iter, MESSAGE_KEY_NAV_BEARING);
  if (bearing_tuple) {
    s_nav_bearing = bearing_tuple->value->int32;
    layer_mark_dirty(s_header_layer);
  }
  
  Tuple *gps_tuple = dict_find(iter, MESSAGE_KEY_GPS_CONNECTED);
  if (gps_tuple) {
    s_gps_connected = (gps_tuple->value->uint8 == 1);
    layer_mark_dirty(s_header_layer);
    layer_mark_dirty(s_map_layer);
  }
  
  Tuple *off_route_tuple = dict_find(iter, MESSAGE_KEY_OFF_ROUTE);
  if (off_route_tuple) {
    s_off_route = (off_route_tuple->value->uint8 == 1);
    layer_mark_dirty(s_footer_layer);
  }
  
  // Dashboard fields
  Tuple *avg_speed_tuple = dict_find(iter, MESSAGE_KEY_AVG_SPEED);
  if (avg_speed_tuple) {
    snprintf(s_avg_speed_text, sizeof(s_avg_speed_text), "%s", avg_speed_tuple->value->cstring);
    text_layer_set_text(s_dash_avg_speed_val_layer, s_avg_speed_text);
  }
  Tuple *gain_tuple = dict_find(iter, MESSAGE_KEY_ELEVATION_GAIN);
  if (gain_tuple) {
    snprintf(s_elevation_gain_text, sizeof(s_elevation_gain_text), "%s", gain_tuple->value->cstring);
    text_layer_set_text(s_dash_gain_val_layer, s_elevation_gain_text);
  }
  Tuple *loss_tuple = dict_find(iter, MESSAGE_KEY_ELEVATION_LOSS);
  if (loss_tuple) {
    snprintf(s_elevation_loss_text, sizeof(s_elevation_loss_text), "%s", loss_tuple->value->cstring);
    text_layer_set_text(s_dash_loss_val_layer, s_elevation_loss_text);
  }
  Tuple *trip_dist_tuple = dict_find(iter, MESSAGE_KEY_TRIP_DISTANCE);
  if (trip_dist_tuple) {
    snprintf(s_trip_distance_text, sizeof(s_trip_distance_text), "%s", trip_dist_tuple->value->cstring);
    text_layer_set_text(s_dash_dist_val_layer, s_trip_distance_text);
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
    if (s_map_buffer && (chunk_idx * CHUNK_SIZE + chunk_len <= MAP_BUFFER_SIZE)) {
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

// Window Loading Procedures
static void main_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer); // 200x228 for Emery
  
  // Allocate map GBitmap & pixel buffer
  s_map_bitmap = gbitmap_create_blank(GSize(MAP_WIDTH, MAP_HEIGHT), GBitmapFormat8Bit);
  s_map_buffer = gbitmap_get_data(s_map_bitmap);
  memset(s_map_buffer, 0b11101010, MAP_BUFFER_SIZE); // pre-populate with grey color (GColorLightGray)
  
  // 1. Header Layer (0 to 30px)
  s_header_layer = layer_create(GRect(0, 0, bounds.size.w, 30));
  layer_set_update_proc(s_header_layer, header_update_proc);
  layer_add_child(window_layer, s_header_layer);
  
  // Distance text in header (left indent for arrow)
  s_distance_layer = text_layer_create(GRect(35, 2, bounds.size.w - 90, 26));
  text_layer_set_background_color(s_distance_layer, GColorClear);
  text_layer_set_text_color(s_distance_layer, GColorBlack);
  text_layer_set_font(s_distance_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text(s_distance_layer, s_distance_text);
  layer_add_child(s_header_layer, text_layer_get_layer(s_distance_layer));
  
  // 2. Map Layer (30 to 180px)
  s_map_layer = layer_create(GRect(0, 30, bounds.size.w, MAP_HEIGHT));
  layer_set_update_proc(s_map_layer, map_layer_update_proc);
  layer_add_child(window_layer, s_map_layer);
  
  // 3. Footer Layer (180 to 228px)
  s_footer_layer = layer_create(GRect(0, 180, bounds.size.w, 48));
  layer_set_update_proc(s_footer_layer, footer_update_proc);
  layer_add_child(window_layer, s_footer_layer);
  
  // Instruction Text in Footer
  s_instruction_layer = text_layer_create(GRect(6, 4, bounds.size.w - 12, 40));
  text_layer_set_background_color(s_instruction_layer, GColorClear);
  text_layer_set_text_color(s_instruction_layer, GColorBlack);
  text_layer_set_font(s_instruction_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_text_alignment(s_instruction_layer, GTextAlignmentCenter);
  text_layer_set_text(s_instruction_layer, s_instruction_text);
  layer_add_child(s_footer_layer, text_layer_get_layer(s_instruction_layer));
  
  // 4. Dashboard Layer (30 to 228px, overlays map and footer, hidden initially)
  s_dashboard_layer = layer_create(GRect(0, 30, bounds.size.w, bounds.size.h - 30));
  layer_set_update_proc(s_dashboard_layer, dashboard_update_proc);
  layer_set_hidden(s_dashboard_layer, true); // hidden on launch
  layer_add_child(window_layer, s_dashboard_layer);
  
  int dash_h = bounds.size.h - 30; // 198px
  int row_h = dash_h / 4; // 49px
  
  // Row 0: Average Speed
  s_dash_avg_speed_title_layer = text_layer_create(GRect(10, 2, bounds.size.w - 20, 15));
  text_layer_set_background_color(s_dash_avg_speed_title_layer, GColorClear);
  text_layer_set_text_color(s_dash_avg_speed_title_layer, GColorDarkGray);
  text_layer_set_font(s_dash_avg_speed_title_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text(s_dash_avg_speed_title_layer, "Ø-GESCHWINDIGKEIT");
  layer_add_child(s_dashboard_layer, text_layer_get_layer(s_dash_avg_speed_title_layer));
  
  s_dash_avg_speed_val_layer = text_layer_create(GRect(10, 16, bounds.size.w - 20, 28));
  text_layer_set_background_color(s_dash_avg_speed_val_layer, GColorClear);
  text_layer_set_text_color(s_dash_avg_speed_val_layer, GColorBlack);
  text_layer_set_font(s_dash_avg_speed_val_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text(s_dash_avg_speed_val_layer, s_avg_speed_text);
  layer_add_child(s_dashboard_layer, text_layer_get_layer(s_dash_avg_speed_val_layer));
  
  // Row 1: Elevation Gain (Aufstieg gemacht / verbleibend)
  s_dash_gain_title_layer = text_layer_create(GRect(10, row_h + 2, bounds.size.w - 20, 15));
  text_layer_set_background_color(s_dash_gain_title_layer, GColorClear);
  text_layer_set_text_color(s_dash_gain_title_layer, GColorDarkGray);
  text_layer_set_font(s_dash_gain_title_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text(s_dash_gain_title_layer, "HM AUFSTIEG (GEM / VERBL)");
  layer_add_child(s_dashboard_layer, text_layer_get_layer(s_dash_gain_title_layer));
  
  s_dash_gain_val_layer = text_layer_create(GRect(10, row_h + 16, bounds.size.w - 20, 28));
  text_layer_set_background_color(s_dash_gain_val_layer, GColorClear);
  text_layer_set_text_color(s_dash_gain_val_layer, GColorBlack);
  text_layer_set_font(s_dash_gain_val_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text(s_dash_gain_val_layer, s_elevation_gain_text);
  layer_add_child(s_dashboard_layer, text_layer_get_layer(s_dash_gain_val_layer));
  
  // Row 2: Elevation Loss (Abstieg gemacht / verbleibend)
  s_dash_loss_title_layer = text_layer_create(GRect(10, (row_h * 2) + 2, bounds.size.w - 20, 15));
  text_layer_set_background_color(s_dash_loss_title_layer, GColorClear);
  text_layer_set_text_color(s_dash_loss_title_layer, GColorDarkGray);
  text_layer_set_font(s_dash_loss_title_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text(s_dash_loss_title_layer, "HM ABSTIEG (GEM / VERBL)");
  layer_add_child(s_dashboard_layer, text_layer_get_layer(s_dash_loss_title_layer));
  
  s_dash_loss_val_layer = text_layer_create(GRect(10, (row_h * 2) + 16, bounds.size.w - 20, 28));
  text_layer_set_background_color(s_dash_loss_val_layer, GColorClear);
  text_layer_set_text_color(s_dash_loss_val_layer, GColorBlack);
  text_layer_set_font(s_dash_loss_val_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text(s_dash_loss_val_layer, s_elevation_loss_text);
  layer_add_child(s_dashboard_layer, text_layer_get_layer(s_dash_loss_val_layer));
  
  // Row 3: Remaining Distance
  s_dash_dist_title_layer = text_layer_create(GRect(10, (row_h * 3) + 2, bounds.size.w - 20, 15));
  text_layer_set_background_color(s_dash_dist_title_layer, GColorClear);
  text_layer_set_text_color(s_dash_dist_title_layer, GColorDarkGray);
  text_layer_set_font(s_dash_dist_title_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text(s_dash_dist_title_layer, "RESTLICHE DISTANZ");
  layer_add_child(s_dashboard_layer, text_layer_get_layer(s_dash_dist_title_layer));
  
  s_dash_dist_val_layer = text_layer_create(GRect(10, (row_h * 3) + 16, bounds.size.w - 20, 28));
  text_layer_set_background_color(s_dash_dist_val_layer, GColorClear);
  text_layer_set_text_color(s_dash_dist_val_layer, GColorBlack);
  text_layer_set_font(s_dash_dist_val_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text(s_dash_dist_val_layer, s_trip_distance_text);
  layer_add_child(s_dashboard_layer, text_layer_get_layer(s_dash_dist_val_layer));
}

// Window Unloading Procedures
static void main_window_unload(Window *window) {
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
  layer_destroy(s_dashboard_layer);
}

// App Initialization
static void init() {
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
  
  window_stack_push(s_main_window, true);
}

// App Deinitialization
static void deinit() {
  window_destroy(s_main_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
