/*
 * Copyright (C) 2026 ClutchReframe
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <obs-module.h>

#include "plugin_version.h"

void riftreframe_register_roi_filter_source(void);

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("clutchreframe-obs-plugin", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
    return "ClutchReframe ROI execution filter plugin (P3-0 skeleton).";
}

bool obs_module_load(void)
{
    obs_module_t *module = obs_current_module();
    const char *module_path = module ? obs_get_module_binary_path(module) : NULL;
    blog(LOG_INFO,
         "[ClutchReframe] obs_module_load productVersion=%s modulePath=%s",
         CLUTCHREFRAME_PRODUCT_VERSION,
         module_path ? module_path : "<unknown>");
    riftreframe_register_roi_filter_source();
    blog(LOG_INFO, "[ClutchReframe] obs_module_load completed.");
    return true;
}

void obs_module_unload(void)
{
    blog(LOG_INFO, "[ClutchReframe] obs_module_unload completed.");
}
