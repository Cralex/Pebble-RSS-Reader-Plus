#pragma once

#include "simply_window.h"

#include "simply_msg.h"

#include "simply.h"

#include "util/list1.h"

#include <pebble.h>

//! Default cell height in pixels
#define MENU_CELL_BASIC_CELL_HEIGHT ((const int16_t) 44)

typedef enum SimplyMenuType SimplyMenuType;

enum SimplyMenuType {
  SimplyMenuTypeNone = 0,
  SimplyMenuTypeSection,
  SimplyMenuTypeItem,
};

typedef struct SimplyMenuLayer SimplyMenuLayer;

struct SimplyMenuLayer {
  MenuLayer *menu_layer;
  List1Node *sections;
  List1Node *items;
  uint16_t num_sections;
  GColor8 normal_foreground;
  GColor8 normal_background;
  GColor8 highlight_foreground;
  GColor8 highlight_background;
  bool wrap; //!< Whether up/down wrap around at the ends of the menu
  uint8_t title_scroll_mode; //!< SimplyMenuTitleScrollMode for long titles in this menu
};

typedef enum SimplyMenuTitleScrollMode SimplyMenuTitleScrollMode;

//! How the highlighted row's title scrolls when it's too long to show.
enum SimplyMenuTitleScrollMode {
  SimplyMenuTitleScrollOff = 0,
  SimplyMenuTitleScrollQuick,   //!< Start after 1 s; pause 1.5 s at the end
  SimplyMenuTitleScrollRelaxed, //!< Start after 2 s; pause 2.5 s at the end
};

typedef struct SimplyMenu SimplyMenu;

typedef enum SimplyMenuScrollState SimplyMenuScrollState;

enum SimplyMenuScrollState {
  SimplyMenuScrollIdle = 0,
  SimplyMenuScrollWaiting,
  SimplyMenuScrollScrolling,
  SimplyMenuScrollPaused,
};

//! Scrolls the highlighted row's title when it's too long to show in full.
typedef struct SimplyMenuTitleScroll SimplyMenuTitleScroll;

struct SimplyMenuTitleScroll {
  AppTimer *timer;
  SimplyMenuScrollState state;
  MenuIndex index;
  GSize cell_size;      //!< Size of the highlighted cell when it was last drawn
  int16_t offset;       //!< How far the title is scrolled, in pixels
  int16_t max_offset;   //!< Offset that shows the end of the title; 0 if it fits, -1 if unknown
  uint8_t cycles;
};

struct SimplyMenu {
  SimplyWindow window;
  SimplyMenuLayer menu_layer;
  AppTimer *spinner_timer;
  SimplyMenuTitleScroll title_scroll;
};

typedef struct SimplyMenuCommon SimplyMenuCommon;

struct SimplyMenuCommon {
  List1Node node;
  uint16_t section;
  char *title;
};

typedef struct SimplyMenuCommonMember SimplyMenuCommonMember;

struct SimplyMenuCommonMember {
  union {
    SimplyMenuCommon common;
    SimplyMenuCommon;
  };
};

typedef struct SimplyMenuSection SimplyMenuSection;

struct SimplyMenuSection {
  SimplyMenuCommonMember;
  uint16_t num_items;
  GColor8 title_foreground;
  GColor8 title_background;
};

typedef struct SimplyMenuItem SimplyMenuItem;

struct SimplyMenuItem {
  SimplyMenuCommonMember;
  char *subtitle;
  uint32_t icon;
  uint16_t item;
};

SimplyMenu *simply_menu_create(Simply *simply);
void simply_menu_destroy(SimplyMenu *self);

bool simply_menu_handle_packet(Simply *simply, Packet *packet);
