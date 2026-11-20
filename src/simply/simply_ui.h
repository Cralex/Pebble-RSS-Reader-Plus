#pragma once

#include "simply_window.h"

#include "simply.h"

#include <pebble.h>

typedef struct SimplyStyle SimplyStyle;

typedef enum SimplyUiTextfieldId SimplyUiTextfieldId;

enum SimplyUiTextfieldId {
  UiTitle = 0,
  UiSubtitle,
  UiBody,
  NumUiTextfields,
};

typedef enum SimplyUiImagefieldId SimplyUiImagefieldId;

enum SimplyUiImagefieldId {
  UiTitleIcon,
  UiSubtitleIcon,
  UiBodyImage,
  NumUiImagefields,
};

typedef struct SimplyUiTextfield SimplyUiTextfield;

struct SimplyUiTextfield {
  char *text;
  GColor8 color;
};

typedef enum SimplyUiFitMode SimplyUiFitMode;

//! How the body of a non-scrolling card is fitted to the screen.
enum SimplyUiFitMode {
  SimplyUiFitNone = 0,
  //! Show as many whole words of the body as fit, followed by a "›" if any are left out.
  SimplyUiFitPage,
  //! The body is the end of a paragraph: as SimplyUiFitPage, but if splitting it would leave
  //! only a little for the next page (a third of a page or less), show it all in the smaller
  //! font instead, if that fits.
  SimplyUiFitEnd,
};

typedef struct SimplyUiFit SimplyUiFit;

//! The fitted body, and how much of it was shown (reported to JS with the id it was asked with).
struct SimplyUiFit {
  uint8_t mode;
  uint16_t id;
  bool dirty;          //!< The body or mode changed since the text was last fitted
  bool use_small_font;
  char *text;          //!< What's drawn: the start of the body, plus "›" if it was cut
};

typedef enum SimplyUiProgressMode SimplyUiProgressMode;

//! How reading progress is shown in the card's status bar.
enum SimplyUiProgressMode {
  SimplyUiProgressNone = 0,
  SimplyUiProgressBar,     //!< A thin bar along the bottom of the status bar, under the clock
  SimplyUiProgressPercent, //!< "42%" in place of the clock
};

typedef struct SimplyUiLayer SimplyUiLayer;

struct SimplyUiLayer {
  Layer *layer;
  const SimplyStyle *style;
  SimplyUiTextfield textfields[3];
  uint32_t imagefields[3];
  GFont custom_body_font;
  SimplyUiFit fit;
  Layer *progress_layer; //!< Drawn over the status bar; see SimplyUiProgressMode
  uint8_t progress_mode;
  uint8_t progress_percent;
};

typedef struct SimplyUi SimplyUi;

struct SimplyUi {
  SimplyWindow window;
  SimplyUiLayer ui_layer;
};

SimplyUi *simply_ui_create(Simply *simply);
void simply_ui_destroy(SimplyUi *self);

void simply_ui_clear(SimplyUi *self, uint32_t clear_mask);

void simply_ui_set_style(SimplyUi *self, int style_index);
void simply_ui_set_text(SimplyUi *self, SimplyUiTextfieldId textfield_id, const char *str);
void simply_ui_set_text_color(SimplyUi *self, SimplyUiTextfieldId textfield_id, GColor8 color);

bool simply_ui_handle_packet(Simply *simply, Packet *packet);
