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

/* 'Resolve Sync Conflicts' menu:
 *
 *   Settings > Cloud Sync > Resolve Sync Conflicts
 *     saves/<core>/<file>.srm              (one per file in conflict)
 *       Use This Device's Version          -> confirmation
 *       Use the Server's Version           -> confirmation
 *
 * The files are the ones the last sync found in conflict (see
 * task_cloud_sync_get_conflicts()). The confirmation says which
 * side is replaced; Confirm syncs again, applying the chosen side
 * to that one file. The replaced version is backed up first. */

#include <stdlib.h>
#include <string.h>

#include <compat/strl.h>
#include <string/stdstring.h>
#include <file/file_path.h>
#include <retro_miscellaneous.h>
#include <lists/string_list.h>

#ifdef HAVE_CONFIG_H
#include "../config.h"
#endif

#if defined(HAVE_NETWORKING) && defined(HAVE_CLOUDSYNC)

#include "cloud_sync_conflict_menu.h"
#include "menu_driver.h"
#include "menu_entries.h"
#include "menu_displaylist.h"

#include "../msg_hash.h"
#include "../configuration.h"
#include "../runloop.h"
#include "../verbosity.h"
#include "../tasks/tasks_internal.h"

static struct
{
   char key[PATH_MAX_LENGTH];
   char kind;
   bool prefer_local;
} cscm_st;

static void cscm_notify(enum msg_hash_enums msg)
{
   const char *str = msg_hash_to_str(msg);
   runloop_msg_queue_push(str, strlen(str), 1, 180, true, NULL,
         MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
}

static int cscm_push_list(const char *path, const char *label,
      unsigned type, size_t directory_ptr)
{
   menu_displaylist_info_t info;
   int ret                    = -1;
   struct menu_state *menu_st = menu_state_get_ptr();
   settings_t *settings       = config_get_ptr();

   if (!menu_st->entries.list)
      return -1;

   menu_displaylist_info_init(&info);
   info.list          = MENU_LIST_GET(menu_st->entries.list, 0);
   info.path          = strdup(path);
   info.label         = strdup(label);
   info.type          = type;
   info.directory_ptr = directory_ptr;

   if (menu_displaylist_ctl(DISPLAYLIST_GENERIC, &info, settings))
      if (menu_displaylist_process(&info))
         ret = 0;

   menu_displaylist_info_free(&info);
   return ret;
}

static void cscm_pop(unsigned levels)
{
   struct menu_state *menu_st = menu_state_get_ptr();
   size_t new_selection_ptr   = menu_st->selection_ptr;

   while (levels--)
      menu_entries_pop_stack(&new_selection_ptr, 0, true);
   menu_st->selection_ptr     = new_selection_ptr;
   menu_st->flags            |=  MENU_ST_FLAG_PREVENT_POPULATE
                              |  MENU_ST_FLAG_ENTRIES_NEED_REFRESH;
}

static bool cscm_append_info(file_list_t *list,
      enum msg_hash_enums msg, const char *detail)
{
   char buf[PATH_MAX_LENGTH];
   size_t _len = strlcpy(buf, msg_hash_to_str(msg), sizeof(buf));
   if (!string_is_empty(detail) && _len < sizeof(buf))
      strlcpy(buf + _len, detail, sizeof(buf) - _len);
   return menu_entries_append(list, buf,
         msg_hash_to_str(MENU_ENUM_LABEL_CLOUD_SYNC_CONFLICT_INFO),
         MENU_ENUM_LABEL_CLOUD_SYNC_CONFLICT_INFO,
         FILE_TYPE_NONE, 0, 0, NULL);
}

static enum msg_hash_enums cscm_kind_msg(char kind)
{
   switch (kind)
   {
      case CLOUD_SYNC_CONFLICT_LOCAL_DELETED:
         return MSG_CLOUD_SYNC_CONFLICT_KIND_LOCAL_DELETED;
      case CLOUD_SYNC_CONFLICT_SERVER_DELETED:
         return MSG_CLOUD_SYNC_CONFLICT_KIND_SERVER_DELETED;
      default:
         break;
   }
   return MSG_CLOUD_SYNC_CONFLICT_KIND_BOTH;
}

/* The kind of conflict of a file, looked up again in the last sync's list */
static bool cscm_find_kind(const char *key, char *kind)
{
   size_t i;
   bool   found = false;
   struct string_list *list = task_cloud_sync_get_conflicts();

   if (!list)
      return false;
   for (i = 0; i < list->size; i++)
   {
      if (string_is_equal(list->elems[i].data, key))
      {
         *kind = (char)list->elems[i].attr.i;
         found = true;
         break;
      }
   }
   string_list_free(list);
   return found;
}

/* Actions */

static int cscm_action_ok_root(const char *path,
      const char *label, unsigned type, size_t idx, size_t entry_idx)
{
   return cscm_push_list("conflicts",
         msg_hash_to_str(MENU_ENUM_LABEL_DEFERRED_CLOUD_SYNC_CONFLICTS),
         FILE_TYPE_NONE, idx);
}

static int cscm_action_ok_file(const char *path,
      const char *label, unsigned type, size_t idx, size_t entry_idx)
{
   char kind = CLOUD_SYNC_CONFLICT_BOTH;

   if (string_is_empty(path))
      return -1;
   strlcpy(cscm_st.key, path, sizeof(cscm_st.key));
   if (cscm_find_kind(path, &kind))
      cscm_st.kind = kind;
   else
      cscm_st.kind = CLOUD_SYNC_CONFLICT_BOTH;

   return cscm_push_list(cscm_st.key,
         msg_hash_to_str(MENU_ENUM_LABEL_DEFERRED_CLOUD_SYNC_CONFLICT_FILE),
         FILE_TYPE_NONE, idx);
}

static int cscm_choose(bool prefer_local, size_t idx)
{
   if (string_is_empty(cscm_st.key))
      return -1;
   cscm_st.prefer_local = prefer_local;
   return cscm_push_list(cscm_st.key,
         msg_hash_to_str(MENU_ENUM_LABEL_DEFERRED_CLOUD_SYNC_CONFLICT_CONFIRM),
         FILE_TYPE_NONE, idx);
}

static int cscm_action_ok_use_local(const char *path,
      const char *label, unsigned type, size_t idx, size_t entry_idx)
{
   return cscm_choose(true, idx);
}

static int cscm_action_ok_use_server(const char *path,
      const char *label, unsigned type, size_t idx, size_t entry_idx)
{
   return cscm_choose(false, idx);
}

static int cscm_action_ok_confirm(const char *path,
      const char *label, unsigned type, size_t idx, size_t entry_idx)
{
   if (!task_push_cloud_sync_resolve(cscm_st.key, cscm_st.prefer_local))
      cscm_notify(MSG_CLOUD_SYNC_RESOLVE_BUSY);
   cscm_st.key[0] = '\0';

   /* The file page and the list are stale now: back to the Cloud Sync
    * settings (confirmation, file page, list). The result shows up as a
    * notification when the sync ends */
   cscm_pop(3);
   return 0;
}

static int cscm_action_ok_cancel(const char *path,
      const char *label, unsigned type, size_t idx, size_t entry_idx)
{
   cscm_pop(1);
   return 0;
}

static int cscm_action_ok_info(const char *path,
      const char *label, unsigned type, size_t idx, size_t entry_idx)
{
   return 0;
}

static size_t cscm_get_value_none(file_list_t *list,
      unsigned *w, unsigned type, unsigned i,
      const char *label, char *s, size_t len,
      const char *path, char *s2, size_t len2)
{
   *s = '\0';
   *w = 0;
   if (!string_is_empty(path))
      strlcpy(s2, path, len2);
   return 0;
}

static int cscm_sublabel(file_list_t *list,
      unsigned type, unsigned i,
      const char *label, const char *path,
      char *s, size_t len)
{
   enum msg_hash_enums sublabel = MSG_UNKNOWN;
   menu_file_list_cbs_t *cbs    = NULL;

   if (list && i < list->size)
      cbs = (menu_file_list_cbs_t*)list->list[i].actiondata;
   if (!cbs)
      return 0;

   switch (cbs->enum_idx)
   {
      case MENU_ENUM_LABEL_CLOUD_SYNC_CONFLICTS:
         sublabel = MENU_ENUM_SUBLABEL_CLOUD_SYNC_CONFLICTS;
         break;
      case MENU_ENUM_LABEL_CLOUD_SYNC_CONFLICT_USE_LOCAL:
         sublabel = MENU_ENUM_SUBLABEL_CLOUD_SYNC_CONFLICT_USE_LOCAL;
         break;
      case MENU_ENUM_LABEL_CLOUD_SYNC_CONFLICT_USE_SERVER:
         sublabel = MENU_ENUM_SUBLABEL_CLOUD_SYNC_CONFLICT_USE_SERVER;
         break;
      default:
         break;
   }
   if (sublabel != MSG_UNKNOWN)
      strlcpy(s, msg_hash_to_str(sublabel), len);
   return 0;
}

/* The kind of conflict as the sublabel of a file in the list */
static int cscm_sublabel_file(file_list_t *list,
      unsigned type, unsigned i,
      const char *label, const char *path,
      char *s, size_t len)
{
   char kind = CLOUD_SYNC_CONFLICT_BOTH;
   if (cscm_find_kind(path, &kind))
      strlcpy(s, msg_hash_to_str(cscm_kind_msg(kind)), len);
   return 0;
}

static int cscm_get_title(const char *path, const char *label,
      unsigned type, char *s, size_t len)
{
   if (string_is_equal(label,
            msg_hash_to_str(MENU_ENUM_LABEL_DEFERRED_CLOUD_SYNC_CONFLICT_FILE)))
      strlcpy(s, path_basename(path), len);
   else if (string_is_equal(label,
            msg_hash_to_str(MENU_ENUM_LABEL_DEFERRED_CLOUD_SYNC_CONFLICT_CONFIRM)))
      strlcpy(s, msg_hash_to_str(MSG_CLOUD_SYNC_CONFLICT_CONFIRM_TITLE), len);
   else
      strlcpy(s, msg_hash_to_str(MSG_CLOUD_SYNC_CONFLICTS_TITLE), len);
   return 0;
}

/* Lists */

static int cscm_deferred_push_conflicts(menu_displaylist_info_t *info)
{
   size_t i;
   struct string_list *list = task_cloud_sync_get_conflicts();

   menu_entries_clear(info->list);

   if (!list)
      cscm_append_info(info->list, MSG_CLOUD_SYNC_CONFLICTS_NONE, NULL);
   else
   {
      cscm_append_info(info->list, MSG_CLOUD_SYNC_CONFLICTS_HINT, NULL);
      for (i = 0; i < list->size; i++)
         menu_entries_append(info->list, list->elems[i].data,
               msg_hash_to_str(MENU_ENUM_LABEL_CLOUD_SYNC_CONFLICT_FILE),
               MENU_ENUM_LABEL_CLOUD_SYNC_CONFLICT_FILE,
               MENU_SETTING_ACTION, 0, 0, NULL);
      string_list_free(list);
   }

   info->flags |= MD_FLAG_NEED_REFRESH | MD_FLAG_NEED_PUSH;
   menu_displaylist_process(info);
   return 0;
}

static int cscm_deferred_push_file(menu_displaylist_info_t *info)
{
   char dir[DIR_MAX_LENGTH];

   menu_entries_clear(info->list);

   cscm_append_info(info->list, cscm_kind_msg(cscm_st.kind), NULL);

   menu_entries_append(info->list,
         msg_hash_to_str(MENU_ENUM_LABEL_VALUE_CLOUD_SYNC_CONFLICT_USE_LOCAL),
         msg_hash_to_str(MENU_ENUM_LABEL_CLOUD_SYNC_CONFLICT_USE_LOCAL),
         MENU_ENUM_LABEL_CLOUD_SYNC_CONFLICT_USE_LOCAL,
         MENU_SETTING_ACTION, 0, 0, NULL);
   menu_entries_append(info->list,
         msg_hash_to_str(MENU_ENUM_LABEL_VALUE_CLOUD_SYNC_CONFLICT_USE_SERVER),
         msg_hash_to_str(MENU_ENUM_LABEL_CLOUD_SYNC_CONFLICT_USE_SERVER),
         MENU_ENUM_LABEL_CLOUD_SYNC_CONFLICT_USE_SERVER,
         MENU_SETTING_ACTION, 0, 0, NULL);

   task_cloud_sync_get_conflict_dir(dir, sizeof(dir));
   cscm_append_info(info->list, MSG_CLOUD_SYNC_CONFLICT_BACKED_UP, NULL);
   menu_entries_append(info->list, dir,
         msg_hash_to_str(MENU_ENUM_LABEL_CLOUD_SYNC_CONFLICT_INFO),
         MENU_ENUM_LABEL_CLOUD_SYNC_CONFLICT_INFO,
         FILE_TYPE_NONE, 0, 0, NULL);

   info->flags |= MD_FLAG_NEED_REFRESH | MD_FLAG_NEED_PUSH;
   menu_displaylist_process(info);
   return 0;
}

/* 'Which side is replaced', then 'Replace' and 'Cancel' */
static int cscm_deferred_push_confirm(menu_displaylist_info_t *info)
{
   char dir[DIR_MAX_LENGTH];
   enum msg_hash_enums replaces;

   menu_entries_clear(info->list);

   if (cscm_st.prefer_local)
      replaces = cscm_st.kind == CLOUD_SYNC_CONFLICT_LOCAL_DELETED
         ? MSG_CLOUD_SYNC_CONFLICT_WILL_DELETE_SERVER
         : MSG_CLOUD_SYNC_CONFLICT_WILL_REPLACE_SERVER;
   else
      replaces = cscm_st.kind == CLOUD_SYNC_CONFLICT_SERVER_DELETED
         ? MSG_CLOUD_SYNC_CONFLICT_WILL_DELETE_LOCAL
         : MSG_CLOUD_SYNC_CONFLICT_WILL_REPLACE_LOCAL;

   menu_entries_append(info->list, path_basename(cscm_st.key),
         msg_hash_to_str(MENU_ENUM_LABEL_CLOUD_SYNC_CONFLICT_INFO),
         MENU_ENUM_LABEL_CLOUD_SYNC_CONFLICT_INFO,
         FILE_TYPE_NONE, 0, 0, NULL);
   cscm_append_info(info->list, replaces, NULL);
   if (     !cscm_st.prefer_local
         && cscm_st.kind != CLOUD_SYNC_CONFLICT_SERVER_DELETED)
      cscm_append_info(info->list, MSG_CLOUD_SYNC_CONFLICT_NO_SERVER_FILE, NULL);
   task_cloud_sync_get_conflict_dir(dir, sizeof(dir));
   cscm_append_info(info->list, MSG_CLOUD_SYNC_CONFLICT_BACKED_UP, NULL);
   menu_entries_append(info->list, dir,
         msg_hash_to_str(MENU_ENUM_LABEL_CLOUD_SYNC_CONFLICT_INFO),
         MENU_ENUM_LABEL_CLOUD_SYNC_CONFLICT_INFO,
         FILE_TYPE_NONE, 0, 0, NULL);

   menu_entries_append(info->list,
         msg_hash_to_str(MENU_ENUM_LABEL_VALUE_CLOUD_SYNC_CONFLICT_CONFIRM),
         msg_hash_to_str(MENU_ENUM_LABEL_CLOUD_SYNC_CONFLICT_CONFIRM),
         MENU_ENUM_LABEL_CLOUD_SYNC_CONFLICT_CONFIRM,
         MENU_SETTING_ACTION, 0, 0, NULL);
   menu_entries_append(info->list,
         msg_hash_to_str(MENU_ENUM_LABEL_VALUE_CLOUD_SYNC_CONFLICT_CANCEL),
         msg_hash_to_str(MENU_ENUM_LABEL_CLOUD_SYNC_CONFLICT_CANCEL),
         MENU_ENUM_LABEL_CLOUD_SYNC_CONFLICT_CANCEL,
         MENU_SETTING_ACTION, 0, 0, NULL);

   info->flags |= MD_FLAG_NEED_REFRESH | MD_FLAG_NEED_PUSH;
   menu_displaylist_process(info);
   return 0;
}

/* Public */

bool cloud_sync_conflict_menu_append_entry(file_list_t *list)
{
   return menu_entries_append(list,
         msg_hash_to_str(MENU_ENUM_LABEL_VALUE_CLOUD_SYNC_CONFLICTS),
         msg_hash_to_str(MENU_ENUM_LABEL_CLOUD_SYNC_CONFLICTS),
         MENU_ENUM_LABEL_CLOUD_SYNC_CONFLICTS,
         MENU_SETTING_ACTION, 0, 0, NULL);
}

void cloud_sync_conflict_menu_cbs_init(menu_file_list_cbs_t *cbs,
      const char *path, const char *label, unsigned type)
{
   if (!cbs || !label)
      return;

   /* Lists (menu stack entries) are identified by label */
   if (string_is_equal(label,
            msg_hash_to_str(MENU_ENUM_LABEL_DEFERRED_CLOUD_SYNC_CONFLICTS)))
   {
      cbs->action_deferred_push = cscm_deferred_push_conflicts;
      cbs->action_get_title     = cscm_get_title;
      return;
   }
   if (string_is_equal(label,
            msg_hash_to_str(MENU_ENUM_LABEL_DEFERRED_CLOUD_SYNC_CONFLICT_FILE)))
   {
      cbs->action_deferred_push = cscm_deferred_push_file;
      cbs->action_get_title     = cscm_get_title;
      return;
   }
   if (string_is_equal(label,
            msg_hash_to_str(MENU_ENUM_LABEL_DEFERRED_CLOUD_SYNC_CONFLICT_CONFIRM)))
   {
      cbs->action_deferred_push = cscm_deferred_push_confirm;
      cbs->action_get_title     = cscm_get_title;
      return;
   }

   switch (cbs->enum_idx)
   {
      case MENU_ENUM_LABEL_CLOUD_SYNC_CONFLICTS:
         cbs->action_ok        = cscm_action_ok_root;
         cbs->action_sublabel  = cscm_sublabel;
         break;
      case MENU_ENUM_LABEL_CLOUD_SYNC_CONFLICT_FILE:
         cbs->action_ok        = cscm_action_ok_file;
         cbs->action_get_value = cscm_get_value_none;
         cbs->action_sublabel  = cscm_sublabel_file;
         cbs->action_start     = NULL;
         cbs->action_scan      = NULL;
         break;
      case MENU_ENUM_LABEL_CLOUD_SYNC_CONFLICT_USE_LOCAL:
         cbs->action_ok        = cscm_action_ok_use_local;
         cbs->action_get_value = cscm_get_value_none;
         cbs->action_sublabel  = cscm_sublabel;
         break;
      case MENU_ENUM_LABEL_CLOUD_SYNC_CONFLICT_USE_SERVER:
         cbs->action_ok        = cscm_action_ok_use_server;
         cbs->action_get_value = cscm_get_value_none;
         cbs->action_sublabel  = cscm_sublabel;
         break;
      case MENU_ENUM_LABEL_CLOUD_SYNC_CONFLICT_CONFIRM:
         cbs->action_ok        = cscm_action_ok_confirm;
         cbs->action_get_value = cscm_get_value_none;
         cbs->action_sublabel  = NULL;
         break;
      case MENU_ENUM_LABEL_CLOUD_SYNC_CONFLICT_CANCEL:
         cbs->action_ok        = cscm_action_ok_cancel;
         cbs->action_get_value = cscm_get_value_none;
         cbs->action_sublabel  = NULL;
         break;
      case MENU_ENUM_LABEL_CLOUD_SYNC_CONFLICT_INFO:
         cbs->action_ok        = cscm_action_ok_info;
         cbs->action_get_value = cscm_get_value_none;
         cbs->action_sublabel  = NULL;
         break;
      default:
         break;
   }
}

#endif /* HAVE_NETWORKING && HAVE_CLOUDSYNC */
