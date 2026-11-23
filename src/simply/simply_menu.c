#include "simply_menu.h"

#include "simply_res.h"
#include "simply_msg.h"
#include "simply_window_stack.h"

#include "simply.h"

#include "util/color.h"
#include "util/display.h"
#include "util/graphics.h"
#include "util/graphics_text.h"
#include "util/math.h"
#include "util/menu_layer.h"
#include "util/noop.h"
#include "util/platform.h"
#include "util/string.h"

#include <pebble.h>

#define MAX_CACHED_SECTIONS 10

#define MAX_CACHED_ITEMS IF_APLITE_ELSE(6, 51)

#define EMPTY_TITLE ""

#define SPINNER_MS 66

typedef Packet MenuClearPacket;

typedef struct MenuClearSectionPacket MenuClearSectionPacket;

struct __attribute__((__packed__)) MenuClearSectionPacket {
  Packet packet;
  uint16_t section;
};

typedef struct MenuPropsPacket MenuPropsPacket;

struct __attribute__((__packed__)) MenuPropsPacket {
  Packet packet;
  uint16_t num_sections;
  GColor8 background_color;
  GColor8 text_color;
  GColor8 highlight_background_color;
  GColor8 highlight_text_color;
  bool wrap;
  uint8_t title_scroll_mode;
  uint8_t title_scroll_speed;
};

typedef struct MenuSectionPacket MenuSectionPacket;

struct __attribute__((__packed__)) MenuSectionPacket {
  Packet packet;
  uint16_t section;
  uint16_t num_items;
  GColor8 background_color;
  GColor8 text_color;
  uint16_t title_length;
  char title[];
};

typedef struct MenuItemPacket MenuItemPacket;

struct __attribute__((__packed__)) MenuItemPacket {
  Packet packet;
  uint16_t section;
  uint16_t item;
  uint32_t icon;
  uint16_t title_length;
  uint16_t subtitle_length;
  char buffer[];
};

typedef struct MenuItemEventPacket MenuItemEventPacket;

struct __attribute__((__packed__)) MenuItemEventPacket {
  Packet packet;
  uint16_t section;
  uint16_t item;
};

typedef Packet MenuGetSelectionPacket;

typedef struct MenuSelectionPacket MenuSelectionPacket;

struct __attribute__((__packed__)) MenuSelectionPacket {
  Packet packet;
  uint16_t section;
  uint16_t item;
  MenuRowAlign align:8;
  bool animated;
};


static GColor8 s_normal_palette[] = { { GColorBlackARGB8 }, { GColorClearARGB8 } };
static GColor8 s_inverted_palette[] = { { GColorWhiteARGB8 }, { GColorClearARGB8 } };


static void simply_menu_clear_section_items(SimplyMenu *self, int section_index);
static void simply_menu_clear(SimplyMenu *self);

static void simply_menu_set_num_sections(SimplyMenu *self, uint16_t num_sections);
static void simply_menu_add_section(SimplyMenu *self, SimplyMenuSection *section);
static void simply_menu_add_item(SimplyMenu *self, SimplyMenuItem *item);

static MenuIndex simply_menu_get_selection(SimplyMenu *self);
static void simply_menu_set_selection(SimplyMenu *self, MenuIndex menu_index, MenuRowAlign align, bool animated);

static void refresh_spinner_timer(SimplyMenu *self);


//! Milliseconds, wrapping every ~49 days: only used for the spinner's phase. (32-bit, because
//! 64-bit division pulls in ~750 bytes of library code, which aplite can't spare.)
static uint32_t prv_get_milliseconds(void) {
  time_t now_s;
  uint16_t now_ms_part;
  time_ms(&now_s, &now_ms_part);
  return ((uint32_t) now_s) * 1000 + now_ms_part;
}

static bool prv_send_menu_item(Command type, uint16_t section, uint16_t item) {
  MenuItemEventPacket packet = {
    .packet.type = type,
    .packet.length = sizeof(packet),
    .section = section,
    .item = item,
  };
  return simply_msg_send_packet(&packet.packet);
}

static bool prv_send_menu_get_section(uint16_t index) {
  return prv_send_menu_item(CommandMenuGetSection, index, 0);
}

static bool prv_send_menu_get_item(uint16_t section, uint16_t index) {
  return prv_send_menu_item(CommandMenuGetItem, section, index);
}

static bool prv_send_menu_select_click(uint16_t section, uint16_t index) {
  return prv_send_menu_item(CommandMenuSelect, section, index);
}

static bool prv_send_menu_select_long_click(uint16_t section, uint16_t index) {
  return prv_send_menu_item(CommandMenuLongSelect, section, index);
}

static bool prv_section_filter(List1Node *node, void *data) {
  SimplyMenuCommon *section = (SimplyMenuCommon *)node;
  const uint16_t section_index = (uint16_t)(uintptr_t) data;
  return (section->section == section_index);
}

static bool prv_item_filter(List1Node *node, void *data) {
  SimplyMenuItem *item = (SimplyMenuItem *)node;
  const uint32_t cell_index = (uint32_t)(uintptr_t) data;
  const uint16_t section_index = cell_index;
  const uint16_t row = cell_index >> 16;
  return (item->section == section_index && item->item == row);
}

static bool prv_request_item_filter(List1Node *node, void *data) {
  return (((SimplyMenuItem *)node)->title == NULL);
}

static SimplyMenuSection *prv_get_menu_section(SimplyMenu *self, int index) {
  return (SimplyMenuSection*) list1_find(self->menu_layer.sections, prv_section_filter,
                                         (void*)(uintptr_t) index);
}

static void prv_free_title(char **title) {
  if (*title && *title != EMPTY_TITLE) {
    free(*title);
    *title = NULL;
  }
}

static void prv_destroy_section(SimplyMenu *self, SimplyMenuSection *section) {
  if (!section) { return; }
  list1_remove(&self->menu_layer.sections, &section->node);
  prv_free_title(&section->title);
  free(section);
}

static void prv_destroy_section_by_index(SimplyMenu *self, int section) {
  SimplyMenuSection *section_node =
      (SimplyMenuSection *)list1_find(self->menu_layer.sections, prv_section_filter,
                                      (void *)(uintptr_t)section);
  prv_destroy_section(self, section_node);
}

static SimplyMenuItem *prv_get_menu_item(SimplyMenu *self, int section, int index) {
  const uint32_t cell_index = section | (index << 16);
  return (SimplyMenuItem *) list1_find(self->menu_layer.items, prv_item_filter,
                                      (void *)(uintptr_t) cell_index);
}

static void prv_destroy_item(SimplyMenu *self, SimplyMenuItem *item) {
  if (!item) { return; }
  list1_remove(&self->menu_layer.items, &item->node);
  prv_free_title(&item->title);
  prv_free_title(&item->subtitle);
  free(item);
}

static void prv_destroy_item_by_index(SimplyMenu *self, int section, int index) {
  const uint32_t cell_index = section | (index << 16);
  SimplyMenuItem *item =
      (SimplyMenuItem *)list1_find(self->menu_layer.items, prv_item_filter,
                                   (void *)(uintptr_t) cell_index);
  prv_destroy_item(self, item);
}

static void prv_add_section(SimplyMenu *self, SimplyMenuSection *section) {
  if (list1_size(self->menu_layer.sections) >= MAX_CACHED_SECTIONS) {
    prv_destroy_section(self, (SimplyMenuSection *)list1_last(self->menu_layer.sections));
  }
  prv_destroy_section_by_index(self, section->section);
  list1_prepend(&self->menu_layer.sections, &section->node);
}

static void prv_add_item(SimplyMenu *self, SimplyMenuItem *item) {
  if (list1_size(self->menu_layer.items) >= MAX_CACHED_ITEMS) {
    prv_destroy_item(self, (SimplyMenuItem*) list1_last(self->menu_layer.items));
  }
  prv_destroy_item_by_index(self, item->section, item->item);
  list1_prepend(&self->menu_layer.items, &item->node);
}

static void prv_request_menu_section(SimplyMenu *self, uint16_t section_index) {
  SimplyMenuSection *section = prv_get_menu_section(self, section_index);
  if (section) { return; }
  section = malloc(sizeof(*section));
  *section = (SimplyMenuSection) {
    .section = section_index,
  };
  prv_add_section(self, section);
  prv_send_menu_get_section(section_index);
}

static void prv_request_menu_item(SimplyMenu *self, uint16_t section_index, uint16_t item_index) {
  SimplyMenuItem *item = prv_get_menu_item(self, section_index, item_index);
  if (item) { return; }
  item = malloc(sizeof(*item));
  *item = (SimplyMenuItem) {
    .section = section_index,
    .item = item_index,
  };
  prv_add_item(self, item);
  prv_send_menu_get_item(section_index, item_index);
}

static void prv_mark_dirty(SimplyMenu *self) {
  if (self->menu_layer.menu_layer) {
    layer_mark_dirty(menu_layer_get_layer(self->menu_layer.menu_layer));
  }
}

static void prv_reload_data(SimplyMenu *self) {
  if (self->menu_layer.menu_layer) {
    menu_layer_reload_data(self->menu_layer.menu_layer);
  }
}

// ---- Title scrolling ----
//
// When the highlighted row's title is cut off, wait a moment and then scroll it so the whole
// title can be read: sideways on rectangular displays (one line), upwards on round displays
// (two wrapped lines). At rest the row is drawn by menu_cell_basic_draw as usual; while
// scrolling, prv_draw_scrolled_title draws the title in the same place, offset by the scroll.
// The layout constants mirror the firmware's menu_layer_system_cells.c.

// Quick timings; Relaxed adds TITLE_SCROLL_RELAXED_EXTRA_MS to both. TITLE_SCROLL_STEP is the
// distance per frame at normal speed; the speed setting multiplies it.
#define TITLE_SCROLL_START_DELAY_MS 1000
#define TITLE_SCROLL_END_PAUSE_MS 1500
#define TITLE_SCROLL_RELAXED_EXTRA_MS 1000
#define TITLE_SCROLL_FRAME_MS 50
#define TITLE_SCROLL_STEP PBL_IF_RECT_ELSE(2, 1)
#define TITLE_SCROLL_CYCLES 2

#define TITLE_FONT_KEY FONT_KEY_GOTHIC_24_BOLD
#if defined(PBL_PLATFORM_EMERY)
#define TITLE_RECT_INSET 10
#else
#define TITLE_RECT_INSET 5
#endif
#define TITLE_ROUND_INSET 16
// Gothic 24 Bold's cap offset, which the firmware subtracts (measured against its rendering)
#define TITLE_ROUND_CAP_OFFSET 5
// Distance from the top of a Gothic 24 Bold line to the top of its capitals (measured)
#define TITLE_ROUND_GLYPH_TOP 10
#define TITLE_ROUND_WINDOW_MARGIN 2
#define TITLE_TEXT_MAX_EXTENT 4000

static GFont prv_title_font(void) {
  return fonts_get_system_font(TITLE_FONT_KEY);
}

static int16_t prv_title_line_height(void) {
  static int16_t s_height;
  if (!s_height) {
    s_height = graphics_text_layout_get_content_size(
        "Ag", prv_title_font(), GRect(0, 0, 200, 200), GTextOverflowModeWordWrap,
        GTextAlignmentLeft).h;
  }
  return s_height;
}

static bool prv_title_scroll_applies(const SimplyMenuItem *item) {
  return (item && item->title && item->title != EMPTY_TITLE && !item->subtitle && !item->icon);
}

ROUND_USAGE static int16_t prv_round_title_width(GSize cell_size) {
  return cell_size.w - 2 * TITLE_ROUND_INSET;
}

//! Height of the title as menu_cell_basic_draw shows it on round: at most two lines.
ROUND_USAGE static int16_t prv_round_visible_title_height(const char *title, GSize cell_size) {
  return graphics_text_layout_get_content_size(
      title, prv_title_font(), GRect(0, 0, prv_round_title_width(cell_size),
                                     2 * prv_title_line_height()),
      GTextOverflowModeFill, GTextAlignmentCenter).h;
}

static int16_t prv_title_max_offset(const char *title, GSize cell_size) {
#if defined(PBL_ROUND)
  const int16_t full_height = graphics_text_layout_get_content_size(
      title, prv_title_font(), GRect(0, 0, prv_round_title_width(cell_size), TITLE_TEXT_MAX_EXTENT),
      GTextOverflowModeWordWrap, GTextAlignmentCenter).h;
  return MAX(0, full_height - prv_round_visible_title_height(title, cell_size));
#else
  const int16_t width = graphics_text_layout_get_content_size(
      title, prv_title_font(), GRect(0, 0, TITLE_TEXT_MAX_EXTENT, cell_size.h),
      GTextOverflowModeWordWrap, GTextAlignmentLeft).w;
  return MAX(0, width - (cell_size.w - TITLE_RECT_INSET));
#endif
}

static void prv_draw_scrolled_title(SimplyMenu *self, GContext *ctx, const Layer *cell_layer,
                                    const char *title) {
  const GRect bounds = layer_get_bounds(cell_layer);
  const int16_t offset = self->title_scroll.offset;
#if defined(PBL_ROUND)
  const int16_t visible_height = prv_round_visible_title_height(title, bounds.size);
  const int16_t top = (bounds.size.h - visible_height) / 2 - TITLE_ROUND_CAP_OFFSET;
  const GRect box = GRect(TITLE_ROUND_INSET, top - offset,
                          prv_round_title_width(bounds.size), TITLE_TEXT_MAX_EXTENT);
  graphics_draw_text(ctx, title, prv_title_font(), box, GTextOverflowModeWordWrap,
                     GTextAlignmentCenter, NULL);
  // Only show two lines: cover lines scrolling in or out above and below with the highlight
  // color. The window follows the letters rather than the text box, which has space above the
  // capitals and none below the descenders, so it starts just above the first line's capitals
  // and is two lines tall.
  const int16_t window_top = top + TITLE_ROUND_GLYPH_TOP - TITLE_ROUND_WINDOW_MARGIN;
  const int16_t window_bottom = window_top + visible_height;
  graphics_context_set_fill_color(ctx, gcolor8_get_or(self->menu_layer.highlight_background,
                                                      GColorBlack));
  graphics_fill_rect(ctx, GRect(0, 0, bounds.size.w, window_top), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(0, window_bottom, bounds.size.w, bounds.size.h - window_bottom),
                     0, GCornerNone);
#else
  const int16_t line_height = prv_title_line_height();
  const GRect box = GRect(TITLE_RECT_INSET - offset, (bounds.size.h - (line_height + 10)) / 2,
                          TITLE_TEXT_MAX_EXTENT, line_height + 4);
  graphics_draw_text(ctx, title, prv_title_font(), box, GTextOverflowModeWordWrap,
                     GTextAlignmentLeft, NULL);
#endif
}

static void prv_title_scroll_timer_callback(void *data);

static uint32_t prv_title_scroll_start_delay(SimplyMenu *self) {
  return TITLE_SCROLL_START_DELAY_MS +
      (self->menu_layer.title_scroll_mode == SimplyMenuTitleScrollRelaxed ?
          TITLE_SCROLL_RELAXED_EXTRA_MS : 0);
}

static uint32_t prv_title_scroll_end_pause(SimplyMenu *self) {
  return TITLE_SCROLL_END_PAUSE_MS +
      (self->menu_layer.title_scroll_mode == SimplyMenuTitleScrollRelaxed ?
          TITLE_SCROLL_RELAXED_EXTRA_MS : 0);
}

static void prv_title_scroll_schedule(SimplyMenu *self, SimplyMenuScrollState state,
                                      uint32_t delay_ms) {
  SimplyMenuTitleScroll *scroll = &self->title_scroll;
  if (scroll->timer) {
    app_timer_cancel(scroll->timer);
  }
  scroll->state = state;
  scroll->timer = app_timer_register(delay_ms, prv_title_scroll_timer_callback, self);
}

static void prv_title_scroll_stop(SimplyMenu *self) {
  SimplyMenuTitleScroll *scroll = &self->title_scroll;
  if (scroll->timer) {
    app_timer_cancel(scroll->timer);
    scroll->timer = NULL;
  }
  scroll->state = SimplyMenuScrollIdle;
  scroll->frames = 0;
  if (scroll->offset) {
    scroll->offset = 0;
    prv_mark_dirty(self);
  }
}

//! Start over for the currently highlighted row.
static void prv_title_scroll_reset(SimplyMenu *self) {
  prv_title_scroll_stop(self);
  SimplyMenuTitleScroll *scroll = &self->title_scroll;
  scroll->max_offset = -1;
  scroll->cycles = 0;
  if (self->menu_layer.menu_layer &&
      self->menu_layer.title_scroll_mode != SimplyMenuTitleScrollOff) {
    scroll->index = menu_layer_get_selected_index(self->menu_layer.menu_layer);
    prv_title_scroll_schedule(self, SimplyMenuScrollWaiting, prv_title_scroll_start_delay(self));
  }
}

static void prv_title_scroll_timer_callback(void *data) {
  SimplyMenu *self = data;
  SimplyMenuTitleScroll *scroll = &self->title_scroll;
  scroll->timer = NULL;
  if (!self->menu_layer.menu_layer) {
    scroll->state = SimplyMenuScrollIdle;
    return;
  }

  switch (scroll->state) {
    case SimplyMenuScrollWaiting: {
      SimplyMenuItem *item = prv_get_menu_item(self, scroll->index.section, scroll->index.row);
      if (!item || !item->title || scroll->cell_size.w == 0) {
        // Not loaded or drawn yet: check again later.
        prv_title_scroll_schedule(self, SimplyMenuScrollWaiting, prv_title_scroll_start_delay(self));
        return;
      }
      if (!prv_title_scroll_applies(item)) {
        scroll->state = SimplyMenuScrollIdle;
        return;
      }
      if (scroll->max_offset < 0) {
        scroll->max_offset = prv_title_max_offset(item->title, scroll->cell_size);
      }
      if (scroll->max_offset == 0) {
        scroll->state = SimplyMenuScrollIdle; // It fits.
        return;
      }
      prv_title_scroll_schedule(self, SimplyMenuScrollScrolling, TITLE_SCROLL_FRAME_MS);
      return;
    }
    case SimplyMenuScrollScrolling: {
      // The speed is in halves, so work out the offset from the frame count: at 1.5x a 1-pixel
      // step alternates between 1 and 2 pixels instead of rounding to one of them.
      const uint8_t speed = self->menu_layer.title_scroll_speed ?: 2;
      scroll->frames++;
      scroll->offset = MIN((int32_t)scroll->frames * TITLE_SCROLL_STEP * speed / 2,
                           scroll->max_offset);
      if (scroll->offset >= scroll->max_offset) {
        prv_title_scroll_schedule(self, SimplyMenuScrollPaused, prv_title_scroll_end_pause(self));
      } else {
        prv_title_scroll_schedule(self, SimplyMenuScrollScrolling, TITLE_SCROLL_FRAME_MS);
      }
      prv_mark_dirty(self);
      return;
    }
    case SimplyMenuScrollPaused:
      scroll->offset = 0;
      scroll->frames = 0;
      prv_mark_dirty(self);
      if (++scroll->cycles < TITLE_SCROLL_CYCLES) {
        prv_title_scroll_schedule(self, SimplyMenuScrollWaiting, prv_title_scroll_start_delay(self));
      } else {
        scroll->state = SimplyMenuScrollIdle;
      }
      return;
    case SimplyMenuScrollIdle:
      return;
  }
}

static bool prv_title_scroll_is_showing(SimplyMenu *self, const MenuIndex *cell_index) {
  const SimplyMenuTitleScroll *scroll = &self->title_scroll;
  return (scroll->offset > 0 && scroll->index.section == cell_index->section &&
          scroll->index.row == cell_index->row);
}

static void simply_menu_set_num_sections(SimplyMenu *self, uint16_t num_sections) {
  if (num_sections == 0) {
    num_sections = 1;
  }
  self->menu_layer.num_sections = num_sections;
  prv_reload_data(self);
}

static void simply_menu_add_section(SimplyMenu *self, SimplyMenuSection *section) {
  if (section->title == NULL) {
    section->title = EMPTY_TITLE;
  }
  prv_add_section(self, section);
  prv_reload_data(self);
}

static void simply_menu_add_item(SimplyMenu *self, SimplyMenuItem *item) {
  if (item->title == NULL) {
    item->title = EMPTY_TITLE;
  }
  prv_add_item(self, item);
  prv_mark_dirty(self);
  if (item->section == self->title_scroll.index.section &&
      item->item == self->title_scroll.index.row) {
    prv_title_scroll_reset(self); // The highlighted row's title changed.
  }
}

static MenuIndex simply_menu_get_selection(SimplyMenu *self) {
  if (!self->menu_layer.menu_layer) {
    return (MenuIndex) {};
  }
  return menu_layer_get_selected_index(self->menu_layer.menu_layer);
}

static void simply_menu_set_selection(SimplyMenu *self, MenuIndex menu_index, MenuRowAlign align,
                                      bool animated) {
  menu_layer_set_selected_index(self->menu_layer.menu_layer, menu_index, align, animated);
}

static bool prv_send_menu_selection(SimplyMenu *self) {
  MenuIndex menu_index = simply_menu_get_selection(self);
  return prv_send_menu_item(CommandMenuSelectionEvent, menu_index.section, menu_index.row);
}

static void spinner_timer_callback(void *data) {
  SimplyMenu *self = data;
  self->spinner_timer = NULL;
  prv_mark_dirty(self);
  refresh_spinner_timer(self);
}

static SimplyMenuItem *get_first_request_item(SimplyMenu *self) {
  return (SimplyMenuItem *)list1_find(self->menu_layer.items, prv_request_item_filter, NULL);
}

static SimplyMenuItem *get_last_request_item(SimplyMenu *self) {
  return (SimplyMenuItem *)list1_find_last(self->menu_layer.items, prv_request_item_filter, NULL);
}

static void refresh_spinner_timer(SimplyMenu *self) {
  if (!self->spinner_timer && get_first_request_item(self)) {
    self->spinner_timer = app_timer_register(SPINNER_MS, spinner_timer_callback, self);
  }
}

static uint16_t prv_menu_get_num_sections_callback(MenuLayer *menu_layer, void *data) {
  SimplyMenu *self = data;
  return self->menu_layer.num_sections;
}

static uint16_t prv_menu_get_num_rows_callback(MenuLayer *menu_layer, uint16_t section_index,
                                               void *data) {
  SimplyMenu *self = data;
  SimplyMenuSection *section = prv_get_menu_section(self, section_index);
  return section ? section->num_items : 1;
}

static int16_t prv_menu_get_header_height_callback(MenuLayer *menu_layer, uint16_t section_index,
                                                   void *data) {
  SimplyMenu *self = data;
  SimplyMenuSection *section = prv_get_menu_section(self, section_index);
  return (section && section->title &&
          section->title != EMPTY_TITLE ? MENU_CELL_BASIC_HEADER_HEIGHT : 0);
}

#if defined(PBL_ROUND) || defined(PBL_PLATFORM_EMERY)
#define MENU_HAS_CELL_HEIGHT_CALLBACK 1
#endif

// Emery's firmware makes rows taller than on smaller watches but keeps the same title font, so
// it showed no more rows than they do. This height fits five rows under the section header.
#define EMERY_CELL_HEIGHT 42

#if MENU_HAS_CELL_HEIGHT_CALLBACK
static int16_t prv_menu_get_cell_height_callback(MenuLayer *menu_layer, MenuIndex *cell_index,
                                                 void *context) {
#if defined(PBL_ROUND)
  const bool is_selected = menu_layer_is_index_selected(menu_layer, cell_index);
  return is_selected ? MENU_CELL_ROUND_FOCUSED_TALL_CELL_HEIGHT :
                       MENU_CELL_ROUND_UNFOCUSED_SHORT_CELL_HEIGHT;
#else
  return EMERY_CELL_HEIGHT;
#endif
}
#endif

static void prv_menu_draw_header_callback(GContext *ctx, const Layer *cell_layer,
                                          uint16_t section_index, void *data) {
  SimplyMenu *self = data;
  SimplyMenuSection *section = prv_get_menu_section(self, section_index);
  if (!section) {
    prv_request_menu_section(self, section_index);
    return;
  }

  list1_remove(&self->menu_layer.sections, &section->node);
  list1_prepend(&self->menu_layer.sections, &section->node);

  GRect bounds = layer_get_bounds(cell_layer);

  graphics_context_set_fill_color(ctx, gcolor8_get_or(section->title_background, GColorWhite));
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  bounds.origin.x += 2;
  bounds.origin.y -= 1;

  graphics_context_set_text_color(ctx, gcolor8_get_or(section->title_foreground, GColorBlack));

  GTextAttributes *title_attributes = graphics_text_attributes_create();
  // On round, keep the header inside the circle. (Paging, which this used before, pushed the
  // header of the first section out of sight, so no feed name showed above the first article.)
  PBL_IF_ROUND_ELSE(
      graphics_text_attributes_enable_screen_text_flow(title_attributes, TEXT_FLOW_DEFAULT_INSET),
      NOOP);
  const GTextAlignment align = PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft);
  graphics_draw_text(ctx, section->title, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                     bounds, GTextOverflowModeTrailingEllipsis, align, title_attributes);
  graphics_text_attributes_destroy(title_attributes);
}

static void simply_menu_draw_row_spinner(SimplyMenu *self, GContext *ctx,
                                         const Layer *cell_layer) {
  GRect bounds = layer_get_bounds(cell_layer);
  GPoint center = grect_center_point(&bounds);

  const int16_t min_radius = 4 * bounds.size.h / 24;
  const int16_t max_radius = 9 * bounds.size.h / 24;
  const int16_t num_lines = 16;
  const int16_t num_drawn_lines = 3;

  const uint32_t now_ms = prv_get_milliseconds();
  const uint32_t start_index = (now_ms / SPINNER_MS) % num_lines;

  graphics_context_set_antialiased(ctx, true);

  GColor8 stroke_color =
      menu_cell_layer_is_highlighted(cell_layer) ? self->menu_layer.highlight_foreground :
                                                   self->menu_layer.normal_foreground;
  graphics_context_set_stroke_color(ctx, gcolor8_get_or(stroke_color, GColorBlack));

  for (int16_t i = 0; i < num_drawn_lines; i++) {
    const uint32_t angle = (i + start_index) * TRIG_MAX_ANGLE / num_lines;
    GPoint a = gpoint_add(center, gpoint_polar(angle, min_radius));
    GPoint b = gpoint_add(center, gpoint_polar(angle, max_radius));
    graphics_draw_line(ctx, a, b);
  }
}

static void prv_menu_draw_row_callback(GContext *ctx, const Layer *cell_layer,
                                       MenuIndex *cell_index, void *data) {
  SimplyMenu *self = data;
  SimplyMenuSection *section = prv_get_menu_section(self, cell_index->section);
  if (!section) {
    prv_request_menu_section(self, cell_index->section);
    return;
  }

  SimplyMenuItem *item = prv_get_menu_item(self, cell_index->section, cell_index->row);
  if (!item) {
    prv_request_menu_item(self, cell_index->section, cell_index->row);
    return;
  }

  if (item->title == NULL) {
    SimplyMenuItem *last_request = get_last_request_item(self);
    if (last_request == item) {
      simply_menu_draw_row_spinner(self, ctx, cell_layer);
      refresh_spinner_timer(self);
    }
    return;
  }

  list1_remove(&self->menu_layer.items, &item->node);
  list1_prepend(&self->menu_layer.items, &item->node);

  if (menu_cell_layer_is_highlighted(cell_layer)) {
    self->title_scroll.cell_size = layer_get_bounds(cell_layer).size;
    if (prv_title_scroll_is_showing(self, cell_index) && prv_title_scroll_applies(item)) {
      prv_draw_scrolled_title(self, ctx, cell_layer, item->title);
      return;
    }
  }

  SimplyImage *image = simply_res_get_image(self->window.simply->res, item->icon);
  GColor8 *palette = NULL;

  if (image && image->is_palette_black_and_white) {
    palette = gbitmap_get_palette(image->bitmap);
    const bool is_highlighted = menu_cell_layer_is_highlighted(cell_layer);
    gbitmap_set_palette(image->bitmap, is_highlighted ? s_inverted_palette : s_normal_palette,
                        false);
  }

  graphics_context_set_alpha_blended(ctx, true);
  menu_cell_basic_draw(ctx, cell_layer, item->title, item->subtitle, image ? image->bitmap : NULL);

  if (palette) {
    gbitmap_set_palette(image->bitmap, palette, false);
  }
}

static void prv_menu_selection_changed_callback(MenuLayer *menu_layer, MenuIndex new_index,
                                                MenuIndex old_index, void *data) {
  prv_title_scroll_reset(data);
}

static void prv_menu_select_click_callback(MenuLayer *menu_layer, MenuIndex *cell_index,
                                           void *data) {
  prv_send_menu_select_click(cell_index->section, cell_index->row);
}

static void prv_menu_select_long_click_callback(MenuLayer *menu_layer, MenuIndex *cell_index,
                                                void *data) {
  prv_send_menu_select_long_click(cell_index->section, cell_index->row);
}

static void prv_single_click_handler(ClickRecognizerRef recognizer, void *context) {
  Window *base_window = layer_get_window(context);
  SimplyWindow *window = window_get_user_data(base_window);
  simply_window_single_click_handler(recognizer, window);
}

//! The last row of the last section, or false if the menu is empty.
static bool prv_get_last_index(SimplyMenu *self, MenuIndex *index_out) {
  MenuLayer *menu_layer = self->menu_layer.menu_layer;
  const uint16_t num_sections = prv_menu_get_num_sections_callback(menu_layer, self);
  if (num_sections == 0) { return false; }
  const uint16_t section = num_sections - 1;
  const uint16_t num_rows = prv_menu_get_num_rows_callback(menu_layer, section, self);
  if (num_rows == 0) { return false; }
  *index_out = (MenuIndex) { .section = section, .row = num_rows - 1 };
  return true;
}

//! Up/down, as the firmware's MenuLayer does them, plus wrapping around at the ends when the
//! menu asks for it. Holding a button doesn't wrap, so a held button stops at the end.
static void prv_up_down_click_handler(ClickRecognizerRef recognizer, void *context) {
  MenuLayer *menu_layer = context;
  SimplyMenu *self = window_get_user_data(layer_get_window(menu_layer_get_layer(menu_layer)));
  const bool up = (click_recognizer_get_button_id(recognizer) == BUTTON_ID_UP);
  const bool animated = true;

  MenuIndex last;
  if (self && self->menu_layer.wrap && !click_recognizer_is_repeating(recognizer) &&
      prv_get_last_index(self, &last)) {
    const MenuIndex selected = menu_layer_get_selected_index(menu_layer);
    const MenuIndex first = { .section = 0, .row = 0 };
    const MenuIndex *at_end = up ? &first : &last;
    if (selected.section == at_end->section && selected.row == at_end->row) {
      menu_layer_set_selected_index(menu_layer, up ? last : first, MenuRowAlignCenter, animated);
      return;
    }
  }
  menu_layer_set_selected_next(menu_layer, up, MenuRowAlignCenter, animated);
}

static void prv_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_BACK, prv_single_click_handler);
  menu_layer_click_config(context);
  // Replace the MenuLayer's up/down handlers (same 100 ms repeat) to support wrapping around.
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 100, prv_up_down_click_handler);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 100, prv_up_down_click_handler);
  
  /*🚨🚨🚨WOWFUNHAPPY HACK ALERT!🚨🚨🚨
  * I (Wowfunhappy) added the below multi_click_subscribe line. PebbleJS doesn't actually offer a way to handle multi_click events—this code has a different purpose.
  * 
  * Normally in PebbleJS, the "up", "select", and "down" buttons activate on release, while "back" activates on press. This REALLY bothered me, so I investigated what was going on. This led me to an interesting tidbit in the Develeloper Documentation: https://developer.rebble.io/developer.pebble.com/docs/c/User_Interface/Window/index.html#window_set_click_config_provider:
  * 	"When there is no multi_click nor long_click setup, the single click handler will fire directly on button down."
  * 
  * So as best as I can tell (because I don't understand C and have no clue what I'm doing), what's going on is this:
  * 1. PebbleJS registers a longClick handler for every button—see "window_long_click_subscribe" above.
  * 2. Pebble accepts the handler for every button except "back", because Pebble doesn't allow overriding longClick behavior on the back button.
  * 
  * However, Pebble DOES allow us to register multi_click handlers for the back button. And so, I arrived at the "dummy" handler below: */
  window_multi_click_subscribe(BUTTON_ID_BACK, 2, 0, 1, true, simply_window_single_click_handler);
  /*🚨End Wowfunhappy Hack🚨*/
  
}

static void prv_menu_window_load(Window *window) {
  SimplyMenu *self = window_get_user_data(window);

  simply_window_load(&self->window);

  Layer *window_layer = window_get_root_layer(window);
  GRect frame = layer_get_frame(window_layer);
  frame.origin = GPointZero;

  MenuLayer *menu_layer = self->menu_layer.menu_layer = menu_layer_create(frame);
  Layer *menu_base_layer = menu_layer_get_layer(menu_layer);
  self->window.layer = menu_base_layer;
  layer_add_child(window_layer, menu_base_layer);

  menu_layer_set_callbacks(menu_layer, self, (MenuLayerCallbacks){
    .get_num_sections = prv_menu_get_num_sections_callback,
    .get_num_rows = prv_menu_get_num_rows_callback,
    .get_header_height = prv_menu_get_header_height_callback,
#if MENU_HAS_CELL_HEIGHT_CALLBACK
    .get_cell_height = prv_menu_get_cell_height_callback,
#endif
    .draw_header = prv_menu_draw_header_callback,
    .draw_row = prv_menu_draw_row_callback,
    .select_click = prv_menu_select_click_callback,
    .select_long_click = prv_menu_select_long_click_callback,
    .selection_changed = prv_menu_selection_changed_callback,
  });

  menu_layer_set_click_config_provider_onto_window(menu_layer, prv_click_config_provider, window);
}

static void prv_menu_window_appear(Window *window) {
  SimplyMenu *self = window_get_user_data(window);
  simply_window_appear(&self->window);
  prv_title_scroll_reset(self);
}

static void prv_menu_window_disappear(Window *window) {
  SimplyMenu *self = window_get_user_data(window);
  prv_title_scroll_stop(self);
  if (simply_window_disappear(&self->window)) {
    simply_res_clear(self->window.simply->res);
    simply_menu_clear(self);
  }
}

static void prv_menu_window_unload(Window *window) {
  SimplyMenu *self = window_get_user_data(window);

  prv_title_scroll_stop(self);
  menu_layer_destroy(self->menu_layer.menu_layer);
  // window.layer points at the menu layer's base layer; clear it too, as simply_ui and
  // simply_stage do, or the next window update calls layer_mark_dirty on freed memory.
  self->menu_layer.menu_layer = NULL;
  self->window.layer = NULL;

  simply_window_unload(&self->window);
}

static void simply_menu_clear_section_items(SimplyMenu *self, int section_index) {
  SimplyMenuItem *item = NULL;
  do {
    item = (SimplyMenuItem *)list1_find(self->menu_layer.items, prv_section_filter,
                                        (void *)(uintptr_t) section_index);
    prv_destroy_item(self, item);
  } while (item);
}

static void simply_menu_clear(SimplyMenu *self) {
  while (self->menu_layer.sections) {
    prv_destroy_section(self, (SimplyMenuSection *)self->menu_layer.sections);
  }

  while (self->menu_layer.items) {
    prv_destroy_item(self, (SimplyMenuItem *)self->menu_layer.items);
  }

  prv_title_scroll_stop(self);
  self->title_scroll.max_offset = -1;
  prv_reload_data(self);
}

static void prv_handle_menu_clear_packet(Simply *simply, Packet *data) {
  simply_menu_clear(simply->menu);
}

static void prv_handle_menu_clear_section_packet(Simply *simply, Packet *data) {
  MenuClearSectionPacket *packet = (MenuClearSectionPacket *)data;
  simply_menu_clear_section_items(simply->menu, packet->section);
}

static void prv_handle_menu_props_packet(Simply *simply, Packet *data) {
  MenuPropsPacket *packet = (MenuPropsPacket *)data;
  SimplyMenu *self = simply->menu;

  simply_menu_set_num_sections(self, packet->num_sections);
  self->menu_layer.wrap = packet->wrap;
  self->menu_layer.title_scroll_speed = packet->title_scroll_speed;
  if (self->menu_layer.title_scroll_mode != packet->title_scroll_mode) {
    self->menu_layer.title_scroll_mode = packet->title_scroll_mode;
    prv_title_scroll_reset(self);
  }

  if (!self->window.window) { return; }

  window_set_background_color(self->window.window, gcolor8_get_or(packet->background_color,
                                                                  GColorWhite));

  SimplyMenuLayer *menu_layer = &self->menu_layer;
  if (!menu_layer->menu_layer) { return; }

  menu_layer->normal_background = packet->background_color;
  menu_layer->normal_foreground = packet->text_color;
  menu_layer->highlight_background = packet->highlight_background_color;
  menu_layer->highlight_foreground = packet->highlight_text_color;

  menu_layer_set_normal_colors(menu_layer->menu_layer,
                               gcolor8_get_or(menu_layer->normal_background, GColorWhite),
                               gcolor8_get_or(menu_layer->normal_foreground, GColorBlack));
  menu_layer_set_highlight_colors(menu_layer->menu_layer,
                                  gcolor8_get_or(menu_layer->highlight_background, GColorBlack),
                                  gcolor8_get_or(menu_layer->highlight_foreground, GColorWhite));
}

static void prv_handle_menu_section_packet(Simply *simply, Packet *data) {
  MenuSectionPacket *packet = (MenuSectionPacket *)data;
  SimplyMenuSection *section = malloc(sizeof(*section));
  *section = (SimplyMenuSection) {
    .section = packet->section,
    .num_items = packet->num_items,
    .title_foreground = packet->text_color,
    .title_background = packet->background_color,
    .title = packet->title_length ? strdup2(packet->title) : NULL,
  };
  simply_menu_add_section(simply->menu, section);
}

static void prv_handle_menu_item_packet(Simply *simply, Packet *data) {
  MenuItemPacket *packet = (MenuItemPacket *)data;
  SimplyMenuItem *item = malloc(sizeof(*item));
  *item = (SimplyMenuItem) {
    .section = packet->section,
    .item = packet->item,
    .title = packet->title_length ? strdup2(packet->buffer) : NULL,
    .subtitle = packet->subtitle_length ? strdup2(packet->buffer + packet->title_length + 1) : NULL,
    .icon = packet->icon,
  };
  simply_menu_add_item(simply->menu, item);
}

static void prv_handle_menu_get_selection_packet(Simply *simply, Packet *data) {
  prv_send_menu_selection(simply->menu);
}

static void prv_handle_menu_selection_packet(Simply *simply, Packet *data) {
  MenuSelectionPacket *packet = (MenuSelectionPacket *)data;
  MenuIndex menu_index = {
    .section = packet->section,
    .row = packet->item,
  };
  simply_menu_set_selection(simply->menu, menu_index, packet->align, packet->animated);
}

bool simply_menu_handle_packet(Simply *simply, Packet *packet) {
  switch (packet->type) {
    case CommandMenuClear:
      prv_handle_menu_clear_packet(simply, packet);
      return true;
    case CommandMenuClearSection:
      prv_handle_menu_clear_section_packet(simply, packet);
      return true;
    case CommandMenuProps:
      prv_handle_menu_props_packet(simply, packet);
      return true;
    case CommandMenuSection:
      prv_handle_menu_section_packet(simply, packet);
      return true;
    case CommandMenuItem:
      prv_handle_menu_item_packet(simply, packet);
      return true;
    case CommandMenuSelection:
      prv_handle_menu_selection_packet(simply, packet);
      return true;
    case CommandMenuGetSelection:
      prv_handle_menu_get_selection_packet(simply, packet);
      return true;
  }
  return false;
}

SimplyMenu *simply_menu_create(Simply *simply) {
  SimplyMenu *self = malloc(sizeof(*self));
  *self = (SimplyMenu) {
    .window.simply = simply,
#if defined(PBL_ROUND)
    .window.status_bar_insets_bottom = true,
#endif
    .menu_layer.num_sections = 1,
  };

  static const WindowHandlers s_window_handlers = {
    .load = prv_menu_window_load,
    .appear = prv_menu_window_appear,
    .disappear = prv_menu_window_disappear,
    .unload = prv_menu_window_unload,
  };
  self->window.window_handlers = &s_window_handlers;

  simply_window_init(&self->window, simply);
  simply_window_set_background_color(&self->window, GColor8White);

  return self;
}

void simply_menu_destroy(SimplyMenu *self) {
  if (!self) {
    return;
  }

  prv_title_scroll_stop(self);
  simply_window_deinit(&self->window);

  free(self);
}
