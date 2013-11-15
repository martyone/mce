/**
 * @file filter-brightness-simple.c
 * Simple level adjusting brightness filter module
 * for display backlight brightness
 * This file implements a filter module for MCE
 * <p>
 * Copyright © 2007-2011 Nokia Corporation and/or its subsidiary(-ies).
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 *
 * mce is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * mce is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mce.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <glib.h>
#include <gmodule.h>

#include "mce.h"
#include "filter-brightness-simple.h"

#include "datapipe.h"			/* append_filter_to_datapipe(),
					 * remove_filter_from_datapipe(),
					 * execute_datapipe()
					 */

/** Module name */
#define MODULE_NAME		"filter-brightness-simple"

/** Functionality provided by this module */
static const gchar *const provides[] = {
	"display-brightness-filter",
	NULL
};

/** Functionality that this module enhances */
static const gchar *const enhances[] = {
	"display-brightness",
	NULL
};
/** Module information */
G_MODULE_EXPORT module_info_struct module_info = {
	/** Name of the module */
	.name = MODULE_NAME,
	/** Module enhances */
	.enhances = enhances,
	/** Module provides */
	.provides = provides,
	/** Module priority */
	.priority = 250
};

/** Display state */
static display_state_t display_state = MCE_DISPLAY_UNDEF;

/**
 * Simple level adjustment filter for display brightness
 *
 * @param data The un-processed brightness setting (1-5) stored in a pointer
 * @return The processed brightness value (percentage)
 */
static gpointer display_brightness_filter(gpointer data) G_GNUC_PURE;
static gpointer display_brightness_filter(gpointer data)
{
	gint raw = GPOINTER_TO_INT(data);
	gpointer retval;

	/* If the display is off or in low power mode,
	 * don't update its brightness
	 */
	if ((display_state == MCE_DISPLAY_OFF) ||
	    (display_state == MCE_DISPLAY_LPM_OFF) ||
	    (display_state == MCE_DISPLAY_LPM_ON)) {
		raw = 0;
		goto EXIT;
	}

	/* Safety net */
	if (raw < DISPLAY_BRIGHTNESS_MINIMUM)
		raw = DISPLAY_BRIGHTNESS_MINIMUM;
	else if (raw > DISPLAY_BRIGHTNESS_MAXIMUM)
		raw = DISPLAY_BRIGHTNESS_MAXIMUM;

	/* Convert to percentage (1% minimum) */
	raw = (raw - 1) * 25 ?: 1;

EXIT:
	retval = GINT_TO_POINTER(raw);

	return retval;
}

/**
 * Init function for the simple level-adjusting brightness filter
 *
 * @todo XXX status needs to be set on error!
 *
 * @param module Unused
 * @return NULL on success, a string with an error message on failure
 */
G_MODULE_EXPORT const gchar *g_module_check_init(GModule *module);
const gchar *g_module_check_init(GModule *module)
{
	(void)module;

	/* Append triggers/filters to datapipes */
	append_filter_to_datapipe(&display_brightness_pipe,
				  display_brightness_filter);

	/* Re-filter the brightness */
	(void)execute_datapipe(&display_brightness_pipe, NULL,
			       USE_CACHE, DONT_CACHE_INDATA);

	return NULL;
}

/**
 * Exit function for the simple level-adjusting brightness filter
 *
 * @param module Unused
 */
G_MODULE_EXPORT void g_module_unload(GModule *module);
void g_module_unload(GModule *module)
{
	(void)module;

	/* Remove triggers/filters from datapipes */
	remove_filter_from_datapipe(&display_brightness_pipe,
				    display_brightness_filter);

	return;
}
