/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2026 - DHGameCenter
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

/* Menu glue for the 'DH Game Library' (see network/dh_library.h) */

#ifndef __DH_LIBRARY_MENU_H
#define __DH_LIBRARY_MENU_H

#include <boolean.h>
#include <retro_common_api.h>
#include <lists/file_list.h>

#include "menu_entries.h"

RETRO_BEGIN_DECLS

#if defined(HAVE_NETWORKING)
/* Appends the 'DH Game Library' entry to a main menu list */
bool dh_library_menu_append_main_entry(file_list_t *list);

/* Binds/overrides the menu callbacks of all
 * 'DH Game Library' entries and lists.
 * Called at the end of menu_cbs_init() */
void dh_library_menu_cbs_init(menu_file_list_cbs_t *cbs,
      const char *path, const char *label, unsigned type);
#endif

RETRO_END_DECLS

#endif
