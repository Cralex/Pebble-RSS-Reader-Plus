#include "simply_ui.h"

#include "simply_msg.h"
#include "simply_res.h"
#include "simply_window_stack.h"

#include "simply.h"

#include "util/compat.h"
#include "util/color.h"
#include "util/graphics.h"
#include "util/graphics_text.h"
#include "util/math.h"
#include "util/noop.h"
#include "util/string.h"
#include "util/window.h"

#include <pebble.h>

struct __attribute__((__packed__)) SimplyStyle {
  const char *title_font;
  const char *subtitle_font;
  const char *body_font;
  int custom_body_font_id;
  int8_t title_icon_padding;
  int8_t title_padding;
  int8_t subtitle_padding;
};

enum ClearIndex {
  ClearIndex_Action = 0,
  ClearIndex_Text,
  ClearIndex_Image,
  ClearIndex_Style,
};

enum StyleIndex {
  StyleIndex_ClassicSmall = 0,
  StyleIndex_ClassicLarge,
  StyleIndex_Mono,
  StyleIndex_Small,
  StyleIndex_Large,
  StyleIndexCount,
};

#define StyleIndex_Default StyleIndex_ClassicLarge

static const SimplyStyle STYLES[StyleIndexCount] = {
  [StyleIndex_ClassicSmall] = {
    .title_font = FONT_KEY_GOTHIC_18_BOLD,
    .subtitle_font = FONT_KEY_GOTHIC_18_BOLD,
    .body_font = FONT_KEY_GOTHIC_18,
  },
  [StyleIndex_ClassicLarge] = {
    .title_font = FONT_KEY_GOTHIC_28_BOLD,
    .subtitle_font = FONT_KEY_GOTHIC_28,
    .body_font = FONT_KEY_GOTHIC_24_BOLD,
  },
  [StyleIndex_Mono] = {
    .title_font = FONT_KEY_GOTHIC_24_BOLD,
    .subtitle_font = FONT_KEY_GOTHIC_18_BOLD,
    .custom_body_font_id = RESOURCE_ID_MONO_FONT_14,
  },
  [StyleIndex_Small] = {
    .title_icon_padding = 4,
    .title_font = FONT_KEY_GOTHIC_18_BOLD,
    .title_padding = 2,
    .subtitle_font = FONT_KEY_GOTHIC_18_BOLD,
    .subtitle_padding = 3,
    .body_font = FONT_KEY_GOTHIC_18,
  },
  [StyleIndex_Large] = {
    .title_icon_padding = 4,
    .title_font = FONT_KEY_GOTHIC_18_BOLD,
    .title_padding = 3,
    .subtitle_font = FONT_KEY_GOTHIC_24_BOLD,
    .subtitle_padding = 4,
    .body_font = FONT_KEY_GOTHIC_24_BOLD,
  },
};

typedef struct CardClearPacket CardClearPacket;

struct __attribute__((__packed__)) CardClearPacket {
  Packet packet;
  uint8_t flags;
};

typedef struct CardTextPacket CardTextPacket;

struct __attribute__((__packed__)) CardTextPacket {
  Packet packet;
  uint8_t index;
  GColor8 color;
  char text[];
};

typedef struct CardImagePacket CardImagePacket;

struct __attribute__((__packed__)) CardImagePacket {
  Packet packet;
  uint32_t image;
  uint8_t index;
};

typedef struct CardStylePacket CardStylePacket;

struct __attribute__((__packed__)) CardStylePacket {
  Packet packet;
  uint8_t style;
};

typedef struct CardFitPacket CardFitPacket;

struct __attribute__((__packed__)) CardFitPacket {
  Packet packet;
  uint8_t mode;
  uint16_t id;
};

typedef struct CardFitEventPacket CardFitEventPacket;

struct __attribute__((__packed__)) CardFitEventPacket {
  Packet packet;
  uint16_t id;
  uint16_t length; //!< Bytes of the body that were shown
};

static void mark_dirty(SimplyUi *self) {
  if (self->ui_layer.layer) {
    layer_mark_dirty(self->ui_layer.layer);
  }
}

void simply_ui_clear(SimplyUi *self, uint32_t clear_mask) {
  if (clear_mask & (1 << ClearIndex_Action)) {
    simply_window_action_bar_clear(&self->window);
  }
  if (clear_mask & (1 << ClearIndex_Text)) {
    for (int textfield_id = 0; textfield_id < NumUiTextfields; ++textfield_id) {
      simply_ui_set_text(self, textfield_id, NULL);
      simply_ui_set_text_color(self, textfield_id, GColor8Black);
    }
  }
  if (clear_mask & (1 << ClearIndex_Image)) {
    memset(self->ui_layer.imagefields, 0, sizeof(self->ui_layer.imagefields));
  }
  if (clear_mask & (1 << ClearIndex_Style)) {
    simply_ui_set_style(self, StyleIndex_Default);
  }
}

void simply_ui_set_style(SimplyUi *self, int style_index) {
  self->ui_layer.fit.dirty = true;
  if (self->ui_layer.custom_body_font) {
    fonts_unload_custom_font(self->ui_layer.custom_body_font);
    self->ui_layer.custom_body_font = NULL;
  }
  self->ui_layer.style = &STYLES[style_index];
  if (self->ui_layer.style->custom_body_font_id) {
    self->ui_layer.custom_body_font = fonts_load_custom_font(
        resource_get_handle(self->ui_layer.style->custom_body_font_id));
  }
  mark_dirty(self);
}

void simply_ui_set_text(SimplyUi *self, SimplyUiTextfieldId textfield_id, const char *str) {
  SimplyUiTextfield *textfield = &self->ui_layer.textfields[textfield_id];
  char **str_field = &textfield->text;
  strset_truncated(str_field, str);
  if (textfield_id == UiBody) {
    self->ui_layer.fit.dirty = true;
  }
  mark_dirty(self);
}

// ---- Fitting the body to the screen ----
//
// Article pages are cut to what the watch can actually show, since only the watch knows its
// font metrics, screen size and (on round) the screen's curve. JS sends more text than fits;
// this shows as many whole words as fit, followed by "›", and tells JS how many bytes it showed
// so the next page can start there.

static const char FIT_MORE_MARKER[] = "\xc2\xa0\xe2\x80\xba"; // no-break space, "›"
#define FIT_SMALL_FONT_KEY FONT_KEY_GOTHIC_18_BOLD
//! Extra height to measure with, so text that doesn't fit shows up as a taller layout.
#define FIT_OVERFLOW_PROBE 64
//! On round, stop pages short of the bottom of the circle: lines there are so narrow that the
//! firmware splits words across them with a hyphen.
#define FIT_ROUND_BOTTOM_INSET 8

#if !defined(PBL_ROUND)
// On rectangular screens the firmware's word wrap is used: binary-search the longest run of whole
// words that fits.

static bool prv_fit_text_fits(const char *text, GFont font, GRect box, GTextAlignment align,
                              GTextAttributes *attributes) {
  GRect probe = box;
  probe.size.h += FIT_OVERFLOW_PROBE;
  const GSize size = graphics_text_layout_get_content_size_with_attributes(
      text, font, probe, GTextOverflowModeWordWrap, align, attributes);
  return size.h <= box.size.h;
}

//! Whether the first `length` bytes of body, followed by the "more" marker, fit.
static bool prv_fit_prefix_fits(char *buffer, const char *body, size_t length, GFont font,
                                GRect box, GTextAlignment align, GTextAttributes *attributes) {
  memcpy(buffer, body, length);
  memcpy(buffer + length, FIT_MORE_MARKER, sizeof(FIT_MORE_MARKER));
  return prv_fit_text_fits(buffer, font, box, align, attributes);
}

//! The most bytes of body (ending before a space, or failing that at a character boundary)
//! that fit with the "more" marker after them.
static size_t prv_fit_find_prefix(char *buffer, const char *body, size_t body_length, GFont font,
                                  GRect box, GTextAlignment align, GTextAttributes *attributes) {
  // Binary search over the spaces: the text before space i fits for every i up to some point.
  size_t num_spaces = 0;
  for (size_t i = 0; i < body_length; i++) {
    if (body[i] == ' ') { num_spaces++; }
  }
  size_t best = 0;
  if (num_spaces) {
    uint16_t *spaces = malloc(num_spaces * sizeof(*spaces));
    if (spaces) {
      size_t n = 0;
      for (size_t i = 0; i < body_length; i++) {
        if (body[i] == ' ') { spaces[n++] = i; }
      }
      int lo = 0, hi = (int)num_spaces - 1;
      while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (prv_fit_prefix_fits(buffer, body, spaces[mid], font, box, align, attributes)) {
          best = spaces[mid];
          lo = mid + 1;
        } else {
          hi = mid - 1;
        }
      }
      free(spaces);
    }
  }
  if (best > 0) {
    return best;
  }
  // Not even the first word fits: split it at a character boundary instead.
  size_t lo = 1, hi = body_length;
  while (lo <= hi) {
    size_t mid = (lo + hi) / 2;
    while (mid > lo && (body[mid] & 0xC0) == 0x80) { mid--; } // don't split a UTF-8 sequence
    if ((body[mid] & 0xC0) == 0x80) { break; }
    if (prv_fit_prefix_fits(buffer, body, mid, font, box, align, attributes)) {
      best = mid;
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return best ? best : body_length; // Nothing fits at all: show it all rather than nothing.
}
#endif

#if defined(PBL_ROUND)
// On round screens, fitted pages are laid out line by line here rather than with the firmware's
// screen text flow: near the top and bottom of the circle, the firmware splits words that don't
// fit a line with a hyphen, and what it measures and what it draws there can differ. Here each
// line's width comes from the circle at that height, lines break only at spaces, and the lines
// are drawn centered, joined with newlines, so what's measured is exactly what's drawn.

//! Lines narrower than this are left empty rather than squeezing a word into them.
#define FIT_ROUND_MIN_LINE_WIDTH 60
#define FIT_ABS(x) ((x) < 0 ? -(x) : (x))

static int32_t prv_isqrt(int32_t n) {
  if (n <= 0) { return 0; }
  int32_t x = n, y = (x + 1) / 2;
  while (y < x) {
    x = y;
    y = (x + n / x) / 2;
  }
  return x;
}

static int16_t prv_text_width(const char *text, GFont font) {
  return graphics_text_layout_get_content_size(
      text, font, GRect(0, 0, 1000, 1000), GTextOverflowModeWordWrap, GTextAlignmentCenter).w;
}

//! Distance between lines of the font, as the firmware spaces lines separated by newlines.
static int16_t prv_line_pitch(GFont font) {
  const GRect box = GRect(0, 0, 1000, 1000);
  return graphics_text_layout_get_content_size("A\nA", font, box, GTextOverflowModeWordWrap,
                                               GTextAlignmentCenter).h -
         graphics_text_layout_get_content_size("A", font, box, GTextOverflowModeWordWrap,
                                               GTextAlignmentCenter).h;
}

//! Width available to a line whose top is at screen_y, inside the circle (with the usual inset).
static int16_t prv_round_line_width(int16_t screen_y, int16_t pitch, GSize screen) {
  const int32_t radius = screen.w / 2;
  const int32_t center_y = screen.h / 2;
  // The letters sit in roughly the lower three quarters of the line.
  const int32_t top = screen_y + pitch / 4;
  const int32_t bottom = screen_y + pitch;
  const int32_t d = MAX(FIT_ABS(top - center_y), FIT_ABS(bottom - center_y));
  if (d >= radius) { return 0; }
  return 2 * prv_isqrt(radius * radius - d * d) - 2 * TEXT_FLOW_DEFAULT_INSET;
}

//! Copies body[start, end) into scratch (plus `suffix`) and returns its width.
static int16_t prv_span_width(char *scratch, const char *body, size_t start, size_t end,
                              const char *suffix, GFont font) {
  memcpy(scratch, body + start, end - start);
  strcpy(scratch + end - start, suffix);
  return prv_text_width(scratch, font);
}

//! Whether the word starting at pos is wider than `width`.
static bool prv_word_wider_than(char *scratch, const char *body, size_t pos, size_t body_length,
                                int16_t width, GFont font) {
  size_t end = pos + 1;
  while (end < body_length && body[end] != ' ') { end++; }
  return prv_span_width(scratch, body, pos, end, "", font) > width;
}

/*!
 * Fills the lines of a page in box with body, breaking at spaces. Writes the page's text (lines
 * joined by '\n', with the "more" marker after the last word if body doesn't all fit) to out,
 * and returns how many bytes of body it holds. out and scratch need room for body plus the
 * marker plus one byte per line.
 */
static size_t prv_fit_round_lines(const char *body, size_t body_length, GFont font, GRect box,
                                  GPoint screen_origin, GSize screen, char *out, char *scratch) {
  const int16_t pitch = prv_line_pitch(font);
  size_t pos = 0;
  char *cursor = out;
  // Where the last non-empty line starts and ends, in body and in out (to fit the marker).
  size_t last_start = 0, last_end = 0;
  char *last_out = NULL;
  int16_t last_width = 0;
  // The widest a line can be (across the middle of the screen).
  const int16_t widest = screen.w - 2 * TEXT_FLOW_DEFAULT_INSET;

  for (int16_t y = box.origin.y; y + pitch <= box.origin.y + box.size.h && pos < body_length;
       y += pitch) {
    const int16_t width = prv_round_line_width(screen_origin.y + y, pitch, screen);
    while (pos < body_length && body[pos] == ' ') { pos++; }
    size_t end = pos;
    if (width >= FIT_ROUND_MIN_LINE_WIDTH) {
      // Add words while they fit.
      while (end < body_length) {
        size_t next = end + 1;
        while (next < body_length && body[next] != ' ') { next++; }
        if (prv_span_width(scratch, body, pos, next, "", font) > width) { break; }
        end = next;
      }
      if (end == pos && prv_span_width(scratch, body, pos, MIN(body_length, pos + 1), "", font) <= width &&
          prv_word_wider_than(scratch, body, pos, body_length, widest, font)) {
        // A word wider than even the widest line: split it at a character boundary.
        // (A word that's only too wide for this line waits for a wider one below.)
        size_t next = pos + 1;
        while (next < body_length) {
          size_t after = next + 1;
          while (after < body_length && (body[after] & 0xC0) == 0x80) { after++; }
          if (prv_span_width(scratch, body, pos, after, "", font) > width) { break; }
          next = after;
        }
        end = next;
      }
    }
    if (cursor != out) { *cursor++ = '\n'; }
    if (end > pos) {
      last_start = pos;
      last_end = end;
      last_out = cursor;
      last_width = width;
      memcpy(cursor, body + pos, end - pos);
      cursor += end - pos;
      pos = end;
    }
  }
  *cursor = '\0';
  while (pos < body_length && body[pos] == ' ') { pos++; }
  if (pos >= body_length || !last_out) {
    return pos >= body_length ? body_length : pos;
  }
  // Text is left over: the last line needs the marker, so drop words from it until it fits.
  size_t end = last_end;
  while (end > last_start &&
         prv_span_width(scratch, body, last_start, end, FIT_MORE_MARKER, font) > last_width) {
    size_t prev = end;
    while (prev > last_start && body[prev - 1] != ' ') { prev--; }
    while (prev > last_start && body[prev - 1] == ' ') { prev--; }
    if (prev == last_start) { break; } // Keep at least one word.
    end = prev;
  }
  memcpy(last_out, body + last_start, end - last_start);
  strcpy(last_out + (end - last_start), FIT_MORE_MARKER);
  return end;
}
#endif

static void prv_fit_send_event(SimplyUiFit *fit, size_t length) {
  CardFitEventPacket packet = {
    .packet.type = CommandCardFitEvent,
    .packet.length = sizeof(packet),
    .id = fit->id,
    .length = length,
  };
  simply_msg_send_packet(&packet.packet);
}

static void prv_fit_body(SimplyUi *self, const char *body, GFont font, GFont small_font,
                         GRect box, GTextAlignment align, GTextAttributes *attributes) {
  SimplyUiFit *fit = &self->ui_layer.fit;
  fit->dirty = false;
  fit->use_small_font = false;
  free(fit->text);
  fit->text = NULL;

  const size_t body_length = strlen(body);
#if defined(PBL_ROUND)
  // Room for the body, the marker and a newline per line (at most one per byte, plus a few).
  const size_t buffer_size = 2 * body_length + sizeof(FIT_MORE_MARKER) + 16;
  fit->text = malloc(buffer_size);
  char *scratch = malloc(body_length + sizeof(FIT_MORE_MARKER));
  size_t shown = body_length;
  if (fit->text && scratch) {
    const GPoint screen_origin = layer_convert_point_to_screen(self->ui_layer.layer, GPointZero);
    const GSize screen = layer_get_bounds(window_get_root_layer(self->window.window)).size;
    shown = prv_fit_round_lines(body, body_length, font, box, screen_origin, screen, fit->text,
                                scratch);
    const size_t left_over = body_length - shown;
    if (shown < body_length && fit->mode == SimplyUiFitEnd && small_font &&
        left_over * 3 <= shown) {
      char *small_text = malloc(buffer_size);
      if (small_text &&
          prv_fit_round_lines(body, body_length, small_font, box, screen_origin, screen,
                              small_text, scratch) == body_length) {
        free(fit->text);
        fit->text = small_text;
        fit->use_small_font = true;
        shown = body_length;
      } else {
        free(small_text);
      }
    }
  } else {
    free(fit->text);
    fit->text = NULL;
  }
  free(scratch);
#else
  size_t shown = body_length;
  if (!prv_fit_text_fits(body, font, box, align, attributes)) {
    fit->text = malloc(body_length + sizeof(FIT_MORE_MARKER));
    if (fit->text) {
      shown = prv_fit_find_prefix(fit->text, body, body_length, font, box, align, attributes);
      const size_t left_over = body_length - shown;
      if (fit->mode == SimplyUiFitEnd && small_font && left_over * 3 <= shown &&
          prv_fit_text_fits(body, small_font, box, align, attributes)) {
        // The end of a paragraph that's slightly too long: rather than leave a few words for
        // a page of their own, show it all in the smaller font.
        fit->use_small_font = true;
        shown = body_length;
      }
      if (shown < body_length) {
        memcpy(fit->text, body, shown);
        memcpy(fit->text + shown, FIT_MORE_MARKER, sizeof(FIT_MORE_MARKER));
      } else {
        strcpy(fit->text, body);
      }
    }
  }
#endif
  prv_fit_send_event(fit, shown);
}

void simply_ui_set_text_color(SimplyUi *self, SimplyUiTextfieldId textfield_id, GColor8 color) {
  SimplyUiTextfield *textfield = &self->ui_layer.textfields[textfield_id];
  textfield->color = color;
  mark_dirty(self);
}

static void enable_text_flow_and_paging(SimplyUi *self, GTextAttributes *text_attributes,
                                        const GRect *box) {
  graphics_text_attributes_enable_paging_on_layer(
      text_attributes, (Layer *)self->window.scroll_layer, box, TEXT_FLOW_DEFAULT_INSET);
}

static void layer_update_callback(Layer *layer, GContext *ctx) {
  SimplyUi *self = *(void **)layer_get_data(layer);

  const GTextAlignment text_align =
      PBL_IF_ROUND_ELSE((self->window.use_action_bar ? GTextAlignmentRight : GTextAlignmentCenter),
                        GTextAlignmentLeft);

  GRect window_frame = {
    .size = layer_get_frame(scroll_layer_get_layer(self->window.scroll_layer)).size,
  };
  GRect frame = window_frame;

  const SimplyStyle *style = self->ui_layer.style;
  GFont title_font = fonts_get_system_font(style->title_font);
  GFont subtitle_font = fonts_get_system_font(style->subtitle_font);
  GFont body_font = self->ui_layer.custom_body_font ?
      self->ui_layer.custom_body_font : fonts_get_system_font(style->body_font);

  const int16_t margin_x = 5;
  const int16_t margin_top = 2;
  const int16_t margin_bottom = self->window.is_scrollable ? 10 : margin_top;
  const int16_t image_offset_y = 3;

  GRect text_frame = frame;
  text_frame.size.w -= 2 * margin_x;
  text_frame.size.h += INT16_MAX / 2;
  GPoint cursor = { margin_x, margin_top };

  if (self->window.use_action_bar) {
    text_frame.size.w -= ACTION_BAR_WIDTH + PBL_IF_ROUND_ELSE(TEXT_FLOW_DEFAULT_INSET, 0);
    window_frame.size.w -= ACTION_BAR_WIDTH;
  }

  graphics_context_set_text_color(ctx, GColorBlack);

  const SimplyUiTextfield *title = &self->ui_layer.textfields[UiTitle];
  const SimplyUiTextfield *subtitle = &self->ui_layer.textfields[UiSubtitle];
  const SimplyUiTextfield *body = &self->ui_layer.textfields[UiBody];

  GTextAttributes *title_attributes = graphics_text_attributes_create();
  GTextAttributes *subtitle_attributes = graphics_text_attributes_create();
  GTextAttributes *body_attributes = graphics_text_attributes_create();

  bool has_title = is_string(title->text);
  bool has_subtitle = is_string(subtitle->text);
  bool has_body = is_string(body->text);

  GSize title_size, subtitle_size;
  GPoint title_pos, subtitle_pos, image_pos = GPointZero;
  GRect body_rect;

  SimplyImage *title_icon = simply_res_get_image(
      self->window.simply->res, self->ui_layer.imagefields[UiTitleIcon]);
  SimplyImage *subtitle_icon = simply_res_get_image(
      self->window.simply->res, self->ui_layer.imagefields[UiSubtitleIcon]);
  SimplyImage *body_image = simply_res_get_image(
      self->window.simply->res, self->ui_layer.imagefields[UiBodyImage]);

  GRect title_icon_bounds =
      title_icon ? gbitmap_get_bounds(title_icon->bitmap) : GRectZero;
  GRect subtitle_icon_bounds =
      subtitle_icon ? gbitmap_get_bounds(subtitle_icon->bitmap) : GRectZero;
  GRect body_image_bounds;

  if (has_title) {
    GRect title_frame = { cursor, text_frame.size };
    if (title_icon) {
      title_icon_bounds.origin = title_frame.origin;
      title_icon_bounds.origin.y += image_offset_y;
      PBL_IF_RECT_ELSE({
        title_frame.origin.x += title_icon_bounds.size.w;
        title_frame.size.w -= title_icon_bounds.size.w;
      }, {
        title_frame.origin.y += title_icon_bounds.size.h + style->title_icon_padding;
      });
    }
    PBL_IF_ROUND_ELSE(
        enable_text_flow_and_paging(self, title_attributes, &title_frame), NOOP);
    title_size = graphics_text_layout_get_content_size_with_attributes(
        title->text, title_font, title_frame, GTextOverflowModeWordWrap, text_align,
        title_attributes);
    title_size.w = title_frame.size.w;
    title_pos = title_frame.origin;
    cursor.y = title_frame.origin.y + title_size.h + style->title_padding;
  }

  if (has_subtitle) {
    GRect subtitle_frame = { cursor, text_frame.size };
    if (subtitle_icon) {
      subtitle_icon_bounds.origin = subtitle_frame.origin;
      subtitle_icon_bounds.origin.y += image_offset_y;
      PBL_IF_RECT_ELSE({
        subtitle_frame.origin.x += subtitle_icon_bounds.size.w;
        subtitle_frame.size.w -= subtitle_icon_bounds.size.w;
      }, {
        subtitle_frame.origin.y += subtitle_icon_bounds.size.h;
      });
    }
    PBL_IF_ROUND_ELSE(
        enable_text_flow_and_paging(self, subtitle_attributes, &subtitle_frame), NOOP);
    subtitle_size = graphics_text_layout_get_content_size_with_attributes(
        subtitle->text, subtitle_font, subtitle_frame, GTextOverflowModeWordWrap, text_align,
        subtitle_attributes);
    subtitle_size.w = subtitle_frame.size.w;
    subtitle_pos = subtitle_frame.origin;
    if (subtitle_icon) {
      subtitle_pos.x += subtitle_icon_bounds.size.w;
    }
    cursor.y = subtitle_frame.origin.y + subtitle_size.h + style->subtitle_padding;
  }

  if (body_image) {
    body_image_bounds = gbitmap_get_bounds(body_image->bitmap);
    image_pos = cursor;
    cursor.y += body_image_bounds.size.h;
  }

  const char *body_text = body->text;
  GTextOverflowMode body_overflow = GTextOverflowModeTrailingEllipsis;
  const bool fit_body = (self->ui_layer.fit.mode != SimplyUiFitNone && !self->window.is_scrollable);
  if (has_body && fit_body) {
    body_rect = (GRect) { cursor, frame.size };
    body_rect.size.w = text_frame.size.w;
    body_rect.size.h -= cursor.y + margin_bottom + PBL_IF_ROUND_ELSE(FIT_ROUND_BOTTOM_INSET, 0);
    // (On round, prv_fit_body lays the lines out to follow the screen's edge itself.)
    GFont small_font =
        self->ui_layer.custom_body_font ? NULL : fonts_get_system_font(FIT_SMALL_FONT_KEY);
    if (self->ui_layer.fit.dirty) {
      prv_fit_body(self, body->text, body_font, small_font, body_rect, text_align,
                   body_attributes);
    }
    if (self->ui_layer.fit.use_small_font) {
      body_font = small_font;
    }
    if (self->ui_layer.fit.text) {
      body_text = self->ui_layer.fit.text;
    }
    body_overflow = GTextOverflowModeWordWrap;
    // The text was measured to fit: draw it in a taller box so the firmware doesn't drop the
    // last line when its descenders reach past the bottom.
    body_rect.size.h += FIT_OVERFLOW_PROBE;
  } else if (has_body) {
    body_rect = (GRect) { cursor, (self->window.is_scrollable ? text_frame.size : frame.size) };
    body_rect.origin = cursor;
    body_rect.size.w = text_frame.size.w;
    body_rect.size.h -= cursor.y + margin_bottom;
    PBL_IF_ROUND_ELSE(
        enable_text_flow_and_paging(self, body_attributes, &body_rect), NOOP);
    GSize body_size = graphics_text_layout_get_content_size_with_attributes(
        body->text, body_font, body_rect, GTextOverflowModeWordWrap, text_align, body_attributes);
    body_size.w = body_rect.size.w;
    cursor.y = body_rect.origin.y + body_size.h;
    if (self->window.is_scrollable) {
      body_rect.size = body_size;
      const int new_height = cursor.y + margin_bottom;
      frame.size.h = MAX(window_frame.size.h, new_height);
      const GSize content_size = scroll_layer_get_content_size(self->window.scroll_layer);
      if (!gsize_equal(&frame.size, &content_size)) {
        layer_set_frame(layer, frame);
        scroll_layer_set_content_size(self->window.scroll_layer, frame.size);
      }
    } else if (!self->ui_layer.custom_body_font && body_size.h > body_rect.size.h) {
      body_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);

    }
    // For rendering text descenders
    body_rect.size.h += margin_bottom;
  }

  IF_SDK_2_ELSE(({
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, frame, 0, GCornerNone);
  }), NONE);

  graphics_context_set_fill_color(ctx, gcolor8_get_or(self->window.background_color, GColorWhite));
  const int radius = IF_SDK_2_ELSE(4, 0);
  graphics_fill_rect(ctx, frame, radius, GCornersAll);

  if (title_icon) {
    GRect icon_frame = title_icon_bounds;
    icon_frame.origin.x =
        PBL_IF_ROUND_ELSE((frame.size.w - title_icon_bounds.size.w) / 2, margin_x);
    PBL_IF_RECT_ELSE(icon_frame.size.h = title_size.h, NOOP);
    graphics_context_set_alpha_blended(ctx, true);
    graphics_draw_bitmap_centered(ctx, title_icon->bitmap, icon_frame);
  }
  if (has_title) {
    graphics_context_set_text_color(ctx, gcolor8_get_or(title->color, GColorBlack));
    graphics_draw_text(ctx, title->text, title_font, (GRect) { title_pos, title_size },
                       GTextOverflowModeWordWrap, text_align, title_attributes);
  }

  if (subtitle_icon) {
    GRect subicon_frame = subtitle_icon_bounds;
    subicon_frame.origin.x =
        PBL_IF_ROUND_ELSE((frame.size.w - subtitle_icon_bounds.size.w) / 2, margin_x);
    PBL_IF_RECT_ELSE(subicon_frame.size.h = subtitle_size.h, NOOP);
    graphics_context_set_alpha_blended(ctx, true);
    graphics_draw_bitmap_centered(ctx, subtitle_icon->bitmap, subicon_frame);
  }
  if (has_subtitle) {
    graphics_context_set_text_color(ctx, gcolor8_get_or(subtitle->color, GColorBlack));
    graphics_draw_text(ctx, subtitle->text, subtitle_font, (GRect) { subtitle_pos, subtitle_size },
                       GTextOverflowModeWordWrap, text_align, subtitle_attributes);
  }

  if (body_image) {
    GRect image_frame = (GRect) {
      .origin = {
        PBL_IF_ROUND_ELSE(((frame.size.w - body_rect.size.w) / 2), 0),
        image_pos.y + image_offset_y,
      },
      .size = { window_frame.size.w, body_image_bounds.size.h }
    };
    graphics_context_set_alpha_blended(ctx, true);
    graphics_draw_bitmap_centered(ctx, body_image->bitmap, image_frame);
  }
  if (has_body) {
    graphics_context_set_text_color(ctx, gcolor8_get_or(body->color, GColorBlack));
    graphics_draw_text(ctx, body_text, body_font, body_rect, body_overflow, text_align,
                       body_attributes);
  }

  graphics_text_attributes_destroy(title_attributes);
  graphics_text_attributes_destroy(subtitle_attributes);
  graphics_text_attributes_destroy(body_attributes);
}

static void show_welcome_text(SimplyUi *self) {
  if (simply_msg_has_communicated()) {
    return;
  }

  simply_msg_show_disconnected(self->window.simply->msg);
}

static void window_load(Window *window) {
  SimplyUi * const self = window_get_user_data(window);

  simply_window_load(&self->window);

  Layer * const window_layer = window_get_root_layer(window);
  const GRect frame = { .size = layer_get_frame(window_layer).size };

  Layer * const layer = layer_create_with_data(frame, sizeof(void *));
  self->ui_layer.layer = layer;
  *(void**) layer_get_data(layer) = self;
  layer_set_update_proc(layer, layer_update_callback);
  scroll_layer_add_child(self->window.scroll_layer, layer);
  self->window.use_scroll_layer = true;

  simply_ui_set_style(self, StyleIndex_Default);
}

static void window_appear(Window *window) {
  SimplyUi *self = window_get_user_data(window);
  simply_window_appear(&self->window);
}

static void window_disappear(Window *window) {
  SimplyUi *self = window_get_user_data(window);
  if (simply_window_disappear(&self->window)) {
    simply_res_clear(self->window.simply->res);
  }
}

static void window_unload(Window *window) {
  SimplyUi *self = window_get_user_data(window);

  layer_destroy(self->ui_layer.layer);
  self->window.layer = self->ui_layer.layer = NULL;

  simply_window_unload(&self->window);
}

static void handle_card_clear_packet(Simply *simply, Packet *data) {
  CardClearPacket *packet = (CardClearPacket*) data;
  simply_ui_clear(simply->ui, packet->flags);
}

static void handle_card_text_packet(Simply *simply, Packet *data) {
  CardTextPacket *packet = (CardTextPacket*) data;
  SimplyUiTextfieldId textfield_id = packet->index;
  if (textfield_id >= NumUiTextfields) {
    return;
  }
  simply_ui_set_text(simply->ui, textfield_id, packet->text);
  if (!gcolor8_equal(packet->color, GColor8ClearWhite)) {
    simply_ui_set_text_color(simply->ui, textfield_id, packet->color);
  }
}

static void handle_card_image_packet(Simply *simply, Packet *data) {
  CardImagePacket *packet = (CardImagePacket*) data;
  SimplyUiImagefieldId imagefield_id = packet->index;
  if (imagefield_id >= NumUiImagefields) {
    return;
  }
  simply->ui->ui_layer.imagefields[imagefield_id] = packet->image;
  window_stack_schedule_top_window_render();
}

static void handle_card_style_packet(Simply *simply, Packet *data) {
  CardStylePacket *packet = (CardStylePacket*) data;
  simply_ui_set_style(simply->ui, packet->style);
}

static void handle_card_fit_packet(Simply *simply, Packet *data) {
  CardFitPacket *packet = (CardFitPacket*) data;
  SimplyUiFit *fit = &simply->ui->ui_layer.fit;
  fit->mode = packet->mode;
  fit->id = packet->id;
  fit->dirty = true;
  mark_dirty(simply->ui);
}

bool simply_ui_handle_packet(Simply *simply, Packet *packet) {
  switch (packet->type) {
    case CommandCardClear:
      handle_card_clear_packet(simply, packet);
      return true;
    case CommandCardText:
      handle_card_text_packet(simply, packet);
      return true;
    case CommandCardImage:
      handle_card_image_packet(simply, packet);
      return true;
    case CommandCardStyle:
      handle_card_style_packet(simply, packet);
      return true;
    case CommandCardFit:
      handle_card_fit_packet(simply, packet);
      return true;
  }
  return false;
}

SimplyUi *simply_ui_create(Simply *simply) {
  SimplyUi *self = malloc(sizeof(*self));
  *self = (SimplyUi) { .window.layer = NULL };

  static const WindowHandlers s_window_handlers = {
    .load = window_load,
    .appear = window_appear,
    .disappear = window_disappear,
    .unload = window_unload,
  };
  self->window.window_handlers = &s_window_handlers;

  simply_window_init(&self->window, simply);
  simply_window_set_background_color(&self->window, GColor8White);

  app_timer_register(10000, (AppTimerCallback) show_welcome_text, self);

  return self;
}

void simply_ui_destroy(SimplyUi *self) {
  if (!self) {
    return;
  }

  simply_ui_clear(self, ~0);

  fonts_unload_custom_font(self->ui_layer.custom_body_font);
  self->ui_layer.custom_body_font = NULL;

  free(self->ui_layer.fit.text);
  self->ui_layer.fit.text = NULL;

  simply_window_deinit(&self->window);

  free(self);
}
