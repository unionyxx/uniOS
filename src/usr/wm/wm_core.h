#pragma once

// Umbrella header for the window manager modules. Includes every WM header in
// layering order (types -> math -> core subsystems -> pipeline/features). New
// code should include the specific module header it needs instead.
#include "wm_types.h"
#include "wm_rect.h"
#include "wm_metrics.h"
#include "wm_present.h"
#include "wm_damage.h"
#include "wm_window.h"
#include "wm_input.h"
#include "wm_render.h"
#include "wm_settings.h"
#include "wm_overlays.h"
#include "wm_main.h"
