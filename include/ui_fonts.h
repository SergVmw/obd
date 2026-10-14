#pragma once

#include "ui_font_golos_fallback.h"
#include "ui_font_golos_smooth.h"

// TFT_eSPI includes these GFXFF font objects when LOAD_GFXFF is enabled. They
// remain as a low-memory fallback if the N16R8 smooth-font layers cannot be
// allocated. Normal operation uses the embedded anti-aliased Golos fonts.
#define FSS9   (&FreeSans9pt7b)
#define FSS12  (&FreeSans12pt7b)
#define FSS18  (&FreeSans18pt7b)
#define FSS24  (&FreeSans24pt7b)
#define FSSB9  (&FreeSansBold9pt7b)
#define FSSB12 (&FreeSansBold12pt7b)
#define FSSB18 (&FreeSansBold18pt7b)
#define FSSB24 (&FreeSansBold24pt7b)

#define H2_FALLBACK_LABEL_FONT (&H2GolosFallback14)
