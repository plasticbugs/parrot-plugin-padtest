// TinyUSB configuration for paddiag: sdk/cvpad_tusb's (what the emulator
// plugins' pads get), and TinyUSB's own complaints into the screen's log.
#pragma once
#include "../../sdk/cvpad_tusb/tusb_config.h"

int paddiag_printf(const char *fmt, ...);
#define CFG_TUSB_DEBUG_PRINTF paddiag_printf
