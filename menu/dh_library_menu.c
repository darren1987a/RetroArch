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

/* 'DH Game Library' menu:
 *
 *   Main Menu > DH Game Library            (share root, '/')
 *     [Share URL setting]
 *     PSGame/                              (only mapped system folders)
 *       Sub folder/
 *       Game.7z            512.3 MB        -> file page
 *         Download and Add to Playlist
 *
 * Directory listings are fetched asynchronously with
 * task_push_http_transfer() and kept in a small cache,
 * so that going back to a parent folder is instant.
 * A download is a single task that fetches the file in
 * Range chunks into a .part file, showing the overall
 * progress in one notification; the finished file is
 *   <Downloads dir>/DHGameCenter/<share path>
 * and is added to the system playlist. */

#include <stdlib.h>
#include <string.h>

#include <compat/strl.h>
#include <string/stdstring.h>
#include <file/file_path.h>
#include <streams/file_stream.h>
#include <retro_miscellaneous.h>
#include <retro_timers.h>
#include <lists/string_list.h>
#include <net/net_http.h>
#include <queues/task_queue.h>

#ifdef HAVE_CONFIG_H
#include "../config.h"
#endif

#if defined(HAVE_NETWORKING)

#include "dh_library_menu.h"
#include "menu_driver.h"
#include "menu_entries.h"
#include "menu_displaylist.h"
#include "menu_setting.h"

#include "../msg_hash.h"
#include "../configuration.h"
#include "../core_info.h"
#include "../playlist.h"
#include "../runloop.h"
#include "../retroarch.h"
#include "../command.h"
#include "../verbosity.h"
#include "../file_path_special.h"
#include "../defaults.h"
#include "../tasks/tasks_internal.h"
#include "../network/dh_library.h"

#define DH_MENU_CACHE_SIZE 8
#define DH_MENU_URL_LEN    2048
/* Keep some head room when checking free space */
#define DH_MENU_SPACE_MARGIN (16 * 1024 * 1024)
/* Files are downloaded with HTTP Range requests of this
 * size, since net_http keeps a whole response in memory
 * (PSP images are 1-2 GB) */
#define DH_MENU_CHUNK_SIZE (64 * 1024 * 1024)
/* Listing sizes are rounded up to the disk block size */
#define DH_MENU_SIZE_TOLERANCE (64 * 1024)

enum dh_menu_status
{
   DH_STATUS_EMPTY = 0,
   DH_STATUS_LOADING,
   DH_STATUS_OK,
   DH_STATUS_ERROR
};

typedef struct dh_menu_cache_entry
{
   dh_library_listing_t listing;
   enum dh_menu_status status;
   enum msg_hash_enums error;
   int http_status;
   unsigned lru;
   char key[DH_MENU_URL_LEN]; /* Listing URL */
} dh_menu_cache_entry_t;

typedef struct dh_menu_view_item
{
   char *name;
   uint64_t size;
   bool is_dir;
   bool downloaded;
} dh_menu_view_item_t;

enum dh_download_result
{
   DH_DOWNLOAD_INTERRUPTED = 0, /* Can be resumed */
   DH_DOWNLOAD_DONE,
   DH_DOWNLOAD_FAILED,          /* HTTP error */
   DH_DOWNLOAD_WRITE_FAILED
};

typedef struct dh_menu_download
{
   const dh_library_system_t *system;
   struct http_t *http; /* Pending chunk request */
   uint64_t size;   /* From the listing (rounded up to disk blocks) */
   uint64_t total;  /* From Content-Range, 0 until known */
   uint64_t offset; /* Bytes already written to the .part file */
   uint64_t requested; /* Length of the pending range request */
   enum dh_download_result result;
   int http_status;
   unsigned shown_percent;
   char name[NAME_MAX_LENGTH];
   char url[DH_MENU_URL_LEN];
   char local_path[PATH_MAX_LENGTH];
   char part_path[PATH_MAX_LENGTH];
} dh_menu_download_t;

typedef struct dh_menu_state
{
   dh_menu_cache_entry_t cache[DH_MENU_CACHE_SIZE];
   /* Entries of the currently displayed folder;
    * menu entries refer to these via 'entry_idx' */
   dh_menu_view_item_t *view;
   size_t view_count;
   unsigned lru_counter;
   /* File selected for the file page */
   uint64_t selected_size;
   char selected_path[PATH_MAX_LENGTH];
   char view_path[PATH_MAX_LENGTH];
   /* Game file of the playlist entry whose actions are shown */
   char playlist_game_path[PATH_MAX_LENGTH];
   /* Game file of the 'Delete Game' page */
   char delete_path[PATH_MAX_LENGTH];
   /* The 'Delete Game' page was opened from a playlist entry */
   bool delete_from_playlist;
} dh_menu_state_t;

static dh_menu_state_t dh_menu_st;

/* Helpers */

static void dh_menu_notify(enum msg_hash_enums msg, const char *detail)
{
   char buf[PATH_MAX_LENGTH];
   size_t _len = strlcpy(buf, msg_hash_to_str(msg), sizeof(buf));

   if (!string_is_empty(detail) && _len < sizeof(buf))
      _len += strlcpy(buf + _len, detail, sizeof(buf) - _len);
   if (_len >= sizeof(buf))
      _len = sizeof(buf) - 1;

   RARCH_LOG("[DH Library] %s\n", buf);
   runloop_msg_queue_push(buf, _len, 1, 180, true, NULL,
         MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
}

static bool dh_menu_get_server(char *base, size_t base_len,
      char *hash, size_t hash_len, enum msg_hash_enums *error)
{
   settings_t *settings = config_get_ptr();
   const char *url      = settings->paths.dh_library_url;

   if (string_is_empty(url))
   {
      if (error)
         *error = MSG_DH_LIBRARY_URL_NOT_SET;
      return false;
   }
   if (!dh_library_parse_share_url(url, base, base_len, hash, hash_len))
   {
      if (error)
         *error = MSG_DH_LIBRARY_URL_INVALID;
      return false;
   }
   return true;
}

static bool dh_menu_get_list_url(const char *path, char *s, size_t len,
      enum msg_hash_enums *error)
{
   char base[DH_MENU_URL_LEN];
   char hash[256];

   if (!dh_menu_get_server(base, sizeof(base), hash, sizeof(hash), error))
      return false;
   if (!dh_library_build_list_url(base, hash, path, s, len))
   {
      if (error)
         *error = MSG_DH_LIBRARY_URL_INVALID;
      return false;
   }
   return true;
}

/* Prefer the 'Downloads' directory, fall back to
 * the file browser start directory */
static const char *dh_menu_get_download_dir(void)
{
   settings_t *settings = config_get_ptr();
   if (!string_is_empty(settings->paths.directory_core_assets))
      return settings->paths.directory_core_assets;
   if (!string_is_empty(settings->paths.directory_menu_content))
      return settings->paths.directory_menu_content;
   return NULL;
}

static bool dh_menu_top_is_ours(void)
{
   const char *label = NULL;
   menu_entries_get_last_stack(NULL, &label, NULL, NULL, NULL);
   return    string_is_equal(label,
               msg_hash_to_str(MENU_ENUM_LABEL_DEFERRED_DH_LIBRARY_LIST))
          || string_is_equal(label,
               msg_hash_to_str(MENU_ENUM_LABEL_DEFERRED_DH_LIBRARY_FILE));
}

static void dh_menu_request_refresh(void)
{
   struct menu_state *menu_st = menu_state_get_ptr();
   menu_st->flags            |=  MENU_ST_FLAG_PREVENT_POPULATE
                              |  MENU_ST_FLAG_ENTRIES_NEED_REFRESH;
}

static int dh_menu_push_list(const char *path, const char *label,
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

/* Listing cache */

static dh_menu_cache_entry_t *dh_menu_cache_find(const char *key)
{
   size_t i;
   for (i = 0; i < DH_MENU_CACHE_SIZE; i++)
      if (     dh_menu_st.cache[i].status != DH_STATUS_EMPTY
            && string_is_equal(dh_menu_st.cache[i].key, key))
         return &dh_menu_st.cache[i];
   return NULL;
}

static void dh_menu_cache_reset(dh_menu_cache_entry_t *entry)
{
   dh_library_listing_free(&entry->listing);
   entry->status      = DH_STATUS_EMPTY;
   entry->error       = MSG_UNKNOWN;
   entry->http_status = 0;
   entry->key[0]      = '\0';
}

static void dh_menu_cache_clear(void)
{
   size_t i;
   /* Requests in flight are kept: a new request for the
    * same URL would be rejected as a duplicate by the
    * HTTP task queue, so wait for the pending result */
   for (i = 0; i < DH_MENU_CACHE_SIZE; i++)
      if (dh_menu_st.cache[i].status != DH_STATUS_LOADING)
         dh_menu_cache_reset(&dh_menu_st.cache[i]);
}

static void dh_menu_cache_invalidate(const char *path)
{
   char url[DH_MENU_URL_LEN];
   dh_menu_cache_entry_t *entry = NULL;

   if (!dh_menu_get_list_url(path, url, sizeof(url), NULL))
      return;
   /* Entries still loading are kept, their result
    * will arrive shortly */
   if (     (entry = dh_menu_cache_find(url))
         && entry->status != DH_STATUS_LOADING)
      dh_menu_cache_reset(entry);
}

static dh_menu_cache_entry_t *dh_menu_cache_alloc(const char *key)
{
   size_t i;
   dh_menu_cache_entry_t *slot = NULL;

   for (i = 0; i < DH_MENU_CACHE_SIZE; i++)
   {
      dh_menu_cache_entry_t *entry = &dh_menu_st.cache[i];
      if (entry->status == DH_STATUS_EMPTY)
      {
         slot = entry;
         break;
      }
      if (entry->status == DH_STATUS_LOADING)
         continue;
      if (!slot || entry->lru < slot->lru)
         slot = entry;
   }

   if (!slot)
      return NULL;

   dh_menu_cache_reset(slot);
   strlcpy(slot->key, key, sizeof(slot->key));
   slot->lru = ++dh_menu_st.lru_counter;
   return slot;
}

static bool dh_menu_data_contains(const char *data, size_t len,
      const char *needle)
{
   size_t i;
   size_t needle_len = strlen(needle);
   if (needle_len == 0 || len < needle_len)
      return false;
   for (i = 0; i + needle_len <= len; i++)
      if (!memcmp(data + i, needle, needle_len))
         return true;
   return false;
}

static void dh_menu_cb_listing(retro_task_t *task,
      void *task_data, void *user_data, const char *err)
{
   http_transfer_data_t *data   = (http_transfer_data_t*)task_data;
   char *key                    = (char*)user_data;
   dh_menu_cache_entry_t *entry = NULL;

   if (!key)
      return;

   /* Result is dropped if the cache was cleared meanwhile
    * (e.g. share URL changed) */
   if (     !(entry = dh_menu_cache_find(key))
         || entry->status != DH_STATUS_LOADING)
      goto end;

   entry->status = DH_STATUS_ERROR;

   if (!data || (err && !data->status))
      entry->error = MSG_DH_LIBRARY_CONNECT_FAILED;
   else
   {
      entry->http_status = data->status;
      switch (data->status)
      {
         case 200:
            if (     data->data
                  && dh_library_parse_listing(data->data, data->len,
                     &entry->listing))
               entry->status = DH_STATUS_OK;
            else
               entry->error  = MSG_DH_LIBRARY_INVALID_RESPONSE;
            break;
         case 0:
         case -1:
            entry->error = MSG_DH_LIBRARY_CONNECT_FAILED;
            break;
         case 404:
            /* {"status":404,"message":"share hash not found"} for an
             * invalid hash, other messages for a missing folder */
            if (     data->data
                  && dh_menu_data_contains(data->data, data->len, "hash"))
               entry->error = MSG_DH_LIBRARY_SHARE_NOT_FOUND;
            else
               entry->error = MSG_DH_LIBRARY_FOLDER_NOT_FOUND;
            break;
         case 401:
         case 403:
            entry->error = MSG_DH_LIBRARY_ACCESS_DENIED;
            break;
         default:
            entry->error = MSG_DH_LIBRARY_HTTP_ERROR;
            break;
      }
   }

   if (entry->status == DH_STATUS_ERROR)
   {
      char status[16];
      status[0] = '\0';
      if (entry->error == MSG_DH_LIBRARY_HTTP_ERROR)
         snprintf(status, sizeof(status), "%d", entry->http_status);
      RARCH_ERR("[DH Library] Listing failed (HTTP %d): %s\n",
            entry->http_status, key);
      dh_menu_notify(entry->error, status);
   }

   if (dh_menu_top_is_ours())
      dh_menu_request_refresh();

end:
   free(key);
}

static dh_menu_cache_entry_t *dh_menu_fetch(const char *url)
{
   char *key                    = NULL;
   dh_menu_cache_entry_t *entry = dh_menu_cache_alloc(url);

   if (!entry)
      return NULL;

   entry->status = DH_STATUS_LOADING;

   command_event(CMD_EVENT_NETWORK_INIT, NULL);

   if (     !(key = strdup(url))
         || !task_push_http_transfer(url, true, NULL,
            dh_menu_cb_listing, key))
   {
      free(key);
      entry->status = DH_STATUS_ERROR;
      entry->error  = MSG_DH_LIBRARY_CONNECT_FAILED;
   }

   return entry;
}

/* View (entries of the displayed folder) */

static void dh_menu_view_clear(void)
{
   size_t i;
   for (i = 0; i < dh_menu_st.view_count; i++)
      free(dh_menu_st.view[i].name);
   free(dh_menu_st.view);
   dh_menu_st.view         = NULL;
   dh_menu_st.view_count   = 0;
   dh_menu_st.view_path[0] = '\0';
}

static const dh_menu_view_item_t *dh_menu_view_get(file_list_t *list,
      size_t i)
{
   size_t entry_idx;
   if (!list || i >= list->size)
      return NULL;
   entry_idx = list->list[i].entry_idx;
   if (entry_idx >= dh_menu_st.view_count)
      return NULL;
   return &dh_menu_st.view[entry_idx];
}

/* Returns the view item for an OK action, verifying that
 * it still matches the selected menu entry */
static const dh_menu_view_item_t *dh_menu_view_get_checked(
      const char *name, size_t entry_idx)
{
   const dh_menu_view_item_t *item = NULL;
   if (entry_idx >= dh_menu_st.view_count)
      return NULL;
   item = &dh_menu_st.view[entry_idx];
   if (!string_is_equal(item->name, name))
      return NULL;
   return item;
}

/* Playlist */

/* Builds the path of the playlist of 'system' */
static bool dh_menu_playlist_path(const dh_library_system_t *system,
      char *lpl_name, size_t lpl_name_len, char *s, size_t len)
{
   settings_t *settings     = config_get_ptr();
   const char *dir_playlist = settings->paths.directory_playlist;

   if (string_is_empty(dir_playlist))
   {
      RARCH_ERR("[DH Library] Playlist directory is not set.\n");
      return false;
   }

   fill_pathname(lpl_name, system->playlist, ".lpl", lpl_name_len);
   fill_pathname_join_special(s, dir_playlist, lpl_name, len);
   return true;
}

static playlist_t *dh_menu_playlist_open(const char *lpl_path)
{
   playlist_config_t playlist_config;
   playlist_t *playlist = NULL;
   settings_t *settings = config_get_ptr();

   memset(&playlist_config, 0, sizeof(playlist_config));
   playlist_config.capacity            = COLLECTION_SIZE;
   playlist_config.old_format          = settings->bools.playlist_use_old_format;
   playlist_config.compress            = settings->bools.playlist_compression;
   playlist_config.fuzzy_archive_match = settings->bools.playlist_fuzzy_archive_match;
   playlist_config_set_base_content_directory(&playlist_config,
         settings->bools.playlist_portable_paths
         ? settings->paths.directory_menu_content : NULL);
   playlist_config_set_path(&playlist_config, lpl_path);

   if (!(playlist = playlist_init(&playlist_config)))
      RARCH_ERR("[DH Library] Failed to open playlist: %s\n", lpl_path);
   return playlist;
}

/* If the currently cached playlist was modified,
 * it must be re-cached */
static void dh_menu_playlist_recache(const char *lpl_path)
{
   playlist_t *cached_playlist = playlist_get_cached();

   if (     cached_playlist
         && string_is_equal(lpl_path, playlist_get_conf_path(cached_playlist)))
   {
      playlist_config_t cached_config;
      if (playlist_config_copy(playlist_get_config(cached_playlist),
               &cached_config))
      {
         playlist_free_cached();
         playlist_init_cached(&cached_config);
      }
   }
}

static void dh_menu_playlist_add(const char *local_path,
      const dh_library_system_t *system)
{
   char lpl_name[NAME_MAX_LENGTH];
   char lpl_path[PATH_MAX_LENGTH];
   char entry_label[NAME_MAX_LENGTH];
   size_t i;
   playlist_t *playlist          = NULL;
   core_info_list_t *core_list   = NULL;
   const core_info_t *core       = NULL;
   settings_t *settings          = config_get_ptr();
   struct menu_state *menu_st    = menu_state_get_ptr();

   if (!dh_menu_playlist_path(system, lpl_name, sizeof(lpl_name),
            lpl_path, sizeof(lpl_path)))
      return;
   path_mkdir(settings->paths.directory_playlist);

   if (!(playlist = dh_menu_playlist_open(lpl_path)))
      return;

   /* Look up the default core by its file id
    * (e.g. 'pcsx_rearmed_libretro') */
   if (core_info_get_list(&core_list) && core_list)
   {
      for (i = 0; i < core_list->count; i++)
      {
         const core_info_t *info = &core_list->list[i];
         if (     info->core_file_id.str
               && string_is_equal(info->core_file_id.str,
                  system->core_file_id))
         {
            core = info;
            break;
         }
      }
   }

   if (core && !string_is_empty(core->path))
   {
      playlist_set_default_core_path(playlist, core->path);
      playlist_set_default_core_name(playlist,
            string_is_empty(core->display_name)
            ? system->core_file_id : core->display_name);
   }
   else
      dh_menu_notify(MSG_DH_LIBRARY_CORE_NOT_FOUND, system->core_file_id);

   if (!playlist_entry_exists(playlist, local_path))
   {
      struct playlist_entry entry = {0};

      fill_pathname(entry_label, path_basename(local_path), "",
            sizeof(entry_label));

      /* The push function reads our entry as const,
       * so these casts are safe */
      entry.path       = (char*)local_path;
      entry.label      = entry_label;
      entry.core_path  = (char*)FILE_PATH_DETECT;
      entry.core_name  = (char*)FILE_PATH_DETECT;
      entry.crc32      = (char*)"00000000|crc";
      entry.db_name    = lpl_name;
      entry.entry_slot = 0;

      playlist_push(playlist, &entry);
   }

   playlist_write_file(playlist);
   playlist_free(playlist);
   dh_menu_playlist_recache(lpl_path);

   /* New playlists must show up in the menu tabs */
   if (menu_st->driver_ctx && menu_st->driver_ctx->environ_cb)
      menu_st->driver_ctx->environ_cb(MENU_ENVIRON_RESET_HORIZONTAL_LIST,
            NULL, menu_st->userdata);

   dh_menu_notify(MSG_DH_LIBRARY_ADDED_TO_PLAYLIST, system->playlist);
}

/* Delete */

/* Normalizes 'content_path' (a playlist path may start with '~'
 * or point inside an archive) into 's', the game file, and
 * returns true only if it lies inside 'root', the normalized
 * '<Downloads dir>/DHGameCenter' */
static bool dh_menu_get_owned_path(const char *content_path,
      char *root, size_t root_len, char *s, size_t len)
{
   char dir[PATH_MAX_LENGTH];
   char path[PATH_MAX_LENGTH];
   char *delim;
   const char *download_dir = dh_menu_get_download_dir();

   if (string_is_empty(download_dir) || string_is_empty(content_path))
      return false;

   fill_pathname_expand_special(dir, download_dir, sizeof(dir));
   fill_pathname_expand_special(path, content_path, sizeof(path));
   if ((delim = (char*)path_get_archive_delim(path)))
      *delim = '\0';

   if (     dh_library_get_root(dir, root, root_len)
         && dh_library_path_is_inside(root, path, s, len))
      return true;

   /* Same location through a symlink
    * (e.g. /var -> /private/var on iOS) */
   return   path_resolve_realpath(dir, sizeof(dir), true)
         && path_resolve_realpath(path, sizeof(path), true)
         && dh_library_get_root(dir, root, root_len)
         && dh_library_path_is_inside(root, path, s, len);
}

/* Removes 'path' from the playlist of 'system' and from
 * the history and favourites */
static void dh_menu_playlist_remove(const char *path,
      const dh_library_system_t *system)
{
   char lpl_name[NAME_MAX_LENGTH];
   char lpl_path[PATH_MAX_LENGTH];
   size_t i;
   playlist_t *defaults[2];

   if (     system
         && dh_menu_playlist_path(system, lpl_name, sizeof(lpl_name),
            lpl_path, sizeof(lpl_path))
         && path_is_valid(lpl_path))
   {
      playlist_t *playlist = dh_menu_playlist_open(lpl_path);
      if (playlist)
      {
         if (playlist_entry_exists(playlist, path))
         {
            playlist_delete_by_path(playlist, path);
            playlist_write_file(playlist);
         }
         playlist_free(playlist);
         dh_menu_playlist_recache(lpl_path);
      }
   }

   defaults[0] = g_defaults.content_history;
   defaults[1] = g_defaults.content_favorites;
   for (i = 0; i < ARRAY_SIZE(defaults); i++)
   {
      if (defaults[i] && playlist_entry_exists(defaults[i], path))
      {
         playlist_delete_by_path(defaults[i], path);
         playlist_write_file(defaults[i]);
         dh_menu_playlist_recache(playlist_get_conf_path(defaults[i]));
      }
   }
}

/* Deletes a game downloaded from the DH Game Library: the file,
 * a left over .part file, its playlist entries and the folders
 * left empty. Save files and BIOS files are not touched. */
static bool dh_menu_delete_game(const char *content_path)
{
   char root[PATH_MAX_LENGTH];
   char path[PATH_MAX_LENGTH];
   char share_path[PATH_MAX_LENGTH];
   char *c;
   bool ok;

   if (!dh_menu_get_owned_path(content_path, root, sizeof(root),
            path, sizeof(path)))
   {
      dh_menu_notify(MSG_DH_LIBRARY_INVALID_PATH, content_path);
      return false;
   }

   /* '/PSPGame/x.iso': the system of the game */
   strlcpy(share_path, path + strlen(root), sizeof(share_path));
   for (c = share_path; *c; c++)
      if (*c == '\\')
         *c = '/';

   /* Playlists first: the entries are matched
    * against the real path of the file */
   dh_menu_playlist_remove(path, dh_library_find_system(share_path));
   ok = dh_library_delete_downloaded(root, path);

   RARCH_LOG("[DH Library] Delete %s: %s\n", path, ok ? "done" : "failed");
   dh_menu_notify(ok ? MSG_DH_LIBRARY_DELETED : MSG_DH_LIBRARY_DELETE_FAILED,
         path_basename(path));
   return ok;
}

static uint64_t dh_menu_file_size(const char *path)
{
   int64_t size = 0;
   /* Not path_get_size(): 32 bit */
   RFILE *file  = filestream_open(path,
         RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);

   if (file)
   {
      size = filestream_get_size(file);
      filestream_close(file);
   }
   return size > 0 ? (uint64_t)size : 0;
}

/* Download */

static size_t dh_menu_u64_to_str(uint64_t val, char *s, size_t len)
{
   char tmp[24];
   size_t i = 0;
   size_t j;
   do
   {
      tmp[i++] = (char)('0' + (val % 10));
      val     /= 10;
   } while (val && i < sizeof(tmp));
   if (i >= len)
      return 0;
   for (j = 0; j < i; j++)
      s[j] = tmp[i - 1 - j];
   s[i] = '\0';
   return i;
}

/* Writes a chunk at 'offset' of the .part file */
static bool dh_menu_part_write(const char *path, uint64_t offset,
      const void *data, size_t len)
{
   bool ok;
   RFILE *file = filestream_open(path,
         offset > 0
         ? RETRO_VFS_FILE_ACCESS_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING
         : RETRO_VFS_FILE_ACCESS_WRITE,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);

   if (!file)
      return false;

   ok =     (offset == 0
            || filestream_seek(file, (int64_t)offset,
               RETRO_VFS_SEEK_POSITION_START) >= 0)
         && filestream_write(file, data, (int64_t)len) == (int64_t)len;

   if (filestream_close(file) != 0)
      ok = false;
   return ok;
}

/* Parses the total size from 'Content-Range: bytes a-b/<total>'
 * (also sent as 'bytes *\/<total>' with 416); 0 if unknown */
static uint64_t dh_menu_content_range_total(const struct string_list *headers)
{
   size_t i;

   if (!headers)
      return 0;

   for (i = 0; i < headers->size; i++)
   {
      const char *line = headers->elems[i].data;
      const char *slash;

      if (     !line
            || !string_starts_with_case_insensitive(line, "Content-Range:"))
         continue;
      if (     !(slash = strrchr(line, '/'))
            || slash[1] < '0' || slash[1] > '9')
         return 0;
      return (uint64_t)strtoull(slash + 1, NULL, 10);
   }

   return 0;
}

/* Best known total size: Content-Range once a response
 * arrived, the listing size (rounded up to disk blocks)
 * until then */
static uint64_t dh_menu_download_total(const dh_menu_download_t *dl)
{
   return dl->total ? dl->total : dl->size;
}

static unsigned dh_menu_download_percent(const dh_menu_download_t *dl,
      uint64_t done)
{
   uint64_t total = dh_menu_download_total(dl);
   uint64_t percent;

   if (total == 0)
      return 0;
   percent = done * 100 / total;
   /* 100% is only shown once the file is complete */
   return (unsigned)(percent > 99 ? 99 : percent);
}

/* Replaces the title of the running task. Only this task's
 * handler sets it; the old one is freed after the swap, under
 * the lock that the notification code reads it with */
static void dh_menu_download_set_title(retro_task_t *task, const char *title)
{
   char *old = task->title;
   task_set_title(task, strdup(title));
   if (old)
      free(old);
}

/* 'Downloading: <name> (45%, 523.1 MB / 1.16 GB)' */
static void dh_menu_download_show_progress(retro_task_t *task,
      dh_menu_download_t *dl, uint64_t done)
{
   char cur[32];
   char tot[32];
   char title[NAME_MAX_LENGTH + 96];
   unsigned percent = dh_menu_download_percent(dl, done);

   /* Every new title is redrawn: only update on change */
   if (percent == dl->shown_percent && task->title)
      return;
   dl->shown_percent = percent;

   dh_library_format_size(done, cur, sizeof(cur));
   dh_library_format_size(dh_menu_download_total(dl), tot, sizeof(tot));
   snprintf(title, sizeof(title), "%s%s (%u%%, %s / %s)",
         msg_hash_to_str(MSG_DH_LIBRARY_DOWNLOADING),
         dl->name, percent, cur, tot);

   task_set_progress(task, (int8_t)percent);
   dh_menu_download_set_title(task, title);
}

/* Sends the Range request for the next chunk */
static bool dh_menu_download_request(dh_menu_download_t *dl)
{
   char headers[96];
   size_t _len;
   struct http_connection_t *conn;

   dl->requested = DH_MENU_CHUNK_SIZE;

   _len  = strlcpy(headers, "Range: bytes=", sizeof(headers));
   _len += dh_menu_u64_to_str(dl->offset, headers + _len, sizeof(headers) - _len);
   _len += strlcpy(headers + _len, "-", sizeof(headers) - _len);
   _len += dh_menu_u64_to_str(dl->offset + dl->requested - 1,
         headers + _len, sizeof(headers) - _len);
   strlcpy(headers + _len, "\r\n", sizeof(headers) - _len);

   if (!(conn = net_http_connection_new(dl->url, "GET", NULL)))
      return false;
   net_http_connection_set_headers(conn, headers);
   while (!net_http_connection_iterate(conn)) { }
   if (net_http_connection_done(conn))
      dl->http = net_http_new(conn);
   net_http_connection_free(conn);

   return dl->http != NULL;
}

/* Handles a finished chunk response. Returns true when the
 * download goes on (next chunk), false when 'dl->result'
 * is final */
static bool dh_menu_download_chunk_done(dh_menu_download_t *dl)
{
   size_t len                  = 0;
   bool last_chunk             = false;
   int status                  = net_http_status(dl->http);
   /* The caller owns the body and the headers */
   uint8_t *data               = net_http_data(dl->http, &len, true);
   struct string_list *headers = net_http_headers(dl->http);

   if (dl->total == 0)
      dl->total = dh_menu_content_range_total(headers);
   string_list_free(headers);
   net_http_delete(dl->http);
   dl->http = NULL;

   switch (status)
   {
      case 206:
         /* Partial content: a short chunk (or reaching the
          * total size) is the last one */
         if (!data || len == 0)
            break;
         last_chunk =  ((uint64_t)len < dl->requested)
                    || (dl->total > 0 && dl->offset + len >= dl->total);
         goto write;
      case 200:
         /* Server ignored the range: this is the whole file */
         if (!data)
            break;
         dl->offset = 0;
         dl->total  = (uint64_t)len;
         last_chunk = true;
         goto write;
      case 416:
         /* Range not satisfiable: the .part file already holds
          * the whole file (its size was a multiple of the chunk
          * size, or a finished .part file was resumed) */
         free(data);
         if (     dl->offset > 0
               && (dl->total > 0
                  ? dl->offset >= dl->total
                  : (dl->size == 0
                     || dl->offset + DH_MENU_SIZE_TOLERANCE >= dl->size)))
         {
            dl->result = DH_DOWNLOAD_DONE;
            return false;
         }
         /* Stale .part file, start over */
         if (dl->offset > 0)
         {
            filestream_delete(dl->part_path);
            dl->offset = 0;
            dl->total  = 0;
            return true;
         }
         dl->http_status = status;
         dl->result      = DH_DOWNLOAD_FAILED;
         return false;
      default:
         /* -1: connection lost, worth resuming */
         free(data);
         dl->http_status = status;
         dl->result      = (status > 0)
               ? DH_DOWNLOAD_FAILED
               : DH_DOWNLOAD_INTERRUPTED;
         return false;
   }

   /* Empty 206 or 200 */
   free(data);
   dl->http_status = status;
   dl->result      = DH_DOWNLOAD_INTERRUPTED;
   return false;

write:
   if (!dh_menu_part_write(dl->part_path, dl->offset, data, len))
   {
      free(data);
      dl->result = DH_DOWNLOAD_WRITE_FAILED;
      return false;
   }
   free(data);
   dl->offset += (uint64_t)len;

   if (last_chunk)
   {
      dl->result = DH_DOWNLOAD_DONE;
      return false;
   }
   return true;
}

static void dh_menu_download_finish(retro_task_t *task,
      dh_menu_download_t *dl)
{
   char title[NAME_MAX_LENGTH + 128];

   if (dl->http)
   {
      string_list_free(net_http_headers(dl->http));
      free(net_http_data(dl->http, NULL, true));
      net_http_delete(dl->http);
      dl->http = NULL;
   }

   /* The .part file keeps an interrupted download from
    * looking like a finished one */
   if (     dl->result == DH_DOWNLOAD_DONE
         && filestream_rename(dl->part_path, dl->local_path) != 0)
      dl->result = DH_DOWNLOAD_WRITE_FAILED;

   switch (dl->result)
   {
      case DH_DOWNLOAD_DONE:
         snprintf(title, sizeof(title), "%s%s",
               msg_hash_to_str(MSG_DH_LIBRARY_DOWNLOAD_COMPLETE), dl->name);
         task_set_progress(task, 100);
         RARCH_LOG("[DH Library] Saved: %s\n", dl->local_path);
         break;
      case DH_DOWNLOAD_WRITE_FAILED:
         snprintf(title, sizeof(title), "%s%s",
               msg_hash_to_str(MSG_DH_LIBRARY_WRITE_FAILED), dl->name);
         break;
      case DH_DOWNLOAD_FAILED:
         snprintf(title, sizeof(title), "%s%s (HTTP %d)",
               msg_hash_to_str(MSG_DH_LIBRARY_DOWNLOAD_FAILED),
               dl->name, dl->http_status);
         break;
      case DH_DOWNLOAD_INTERRUPTED:
      default:
         /* The .part file is kept, a new download resumes from it */
         snprintf(title, sizeof(title),
               msg_hash_to_str(MSG_DH_LIBRARY_DOWNLOAD_INTERRUPTED),
               dl->name, dh_menu_download_percent(dl, dl->offset));
         break;
   }

   if (dl->result != DH_DOWNLOAD_DONE)
   {
      RARCH_ERR("[DH Library] Download stopped (HTTP %d) at %u MB: %s\n",
            dl->http_status, (unsigned)(dl->offset / (1024 * 1024)),
            dl->name);
      task_set_error(task, strdup(title));
   }

   dh_menu_download_set_title(task, title);
   task_set_flags(task, RETRO_TASK_FLG_FINISHED, true);
}

/* Runs on the task thread */
static void dh_menu_download_handler(retro_task_t *task)
{
   size_t pos             = 0;
   size_t tot             = 0;
   dh_menu_download_t *dl = (dh_menu_download_t*)task->state;

   if ((task_get_flags(task) & RETRO_TASK_FLG_CANCELLED) > 0)
   {
      dl->result = DH_DOWNLOAD_INTERRUPTED;
      goto finish;
   }

   if (!dl->http)
   {
      if (!dh_menu_download_request(dl))
      {
         dl->result = DH_DOWNLOAD_INTERRUPTED;
         goto finish;
      }
      return;
   }

   /* Same as the HTTP task: don't spin on the socket */
   if (task_queue_is_threaded())
      retro_sleep(1);

   if (!net_http_update(dl->http, &pos, &tot))
   {
      if (dl->total == 0 && tot > 0)
         dl->total = dh_menu_content_range_total(net_http_headers(dl->http));
      dh_menu_download_show_progress(task, dl, dl->offset + pos);
      return;
   }

   if (dh_menu_download_chunk_done(dl))
   {
      dh_menu_download_show_progress(task, dl, dl->offset);
      return;
   }

finish:
   dh_menu_download_finish(task, dl);
}

/* Runs on the main thread once the task is finished */
static void dh_menu_cb_download(retro_task_t *task,
      void *task_data, void *user_data, const char *err)
{
   dh_menu_download_t *dl = (dh_menu_download_t*)user_data;

   if (!dl)
      return;

   if (dl->result == DH_DOWNLOAD_DONE)
   {
      dh_menu_playlist_add(dl->local_path, dl->system);
      if (dh_menu_top_is_ours())
         dh_menu_request_refresh();
   }

   free(dl);
}

static bool dh_menu_download_finder(retro_task_t *task, void *user_data)
{
   if (task && task->handler == dh_menu_download_handler && user_data)
      return string_is_equal(
            ((dh_menu_download_t*)task->state)->local_path,
            (const char*)user_data);
   return false;
}

static bool dh_menu_download_push(dh_menu_download_t *dl)
{
   task_finder_data_t find_data;
   retro_task_t *task;

   /* The same file can't be downloaded twice at once */
   find_data.func     = dh_menu_download_finder;
   find_data.userdata = dl->local_path;
   if (task_queue_find(&find_data))
      return false;

   if (!(task = task_init()))
      return false;

   dl->shown_percent = (unsigned)-1;
   task->handler     = dh_menu_download_handler;
   task->state       = dl;
   task->callback    = dh_menu_cb_download;
   task->user_data   = dl;
   task->progress    = 0;
   task->flags      |= RETRO_TASK_FLG_ALTERNATIVE_LOOK;
   dh_menu_download_show_progress(task, dl, dl->offset);

   task_queue_push(task);
   return true;
}

static int dh_menu_download(const char *path, uint64_t size)
{
   char base[DH_MENU_URL_LEN];
   char hash[256];
   char local_dir[PATH_MAX_LENGTH];
   int64_t free_space;
   const char *name                   = path_basename(path);
   const char *download_dir           = dh_menu_get_download_dir();
   const dh_library_system_t *system  = dh_library_find_system(path);
   dh_menu_download_t *dl             = NULL;
   enum msg_hash_enums error          = MSG_UNKNOWN;

   if (!system)
   {
      dh_menu_notify(MSG_DH_LIBRARY_INVALID_PATH, path);
      return -1;
   }
   if (string_is_empty(download_dir))
   {
      dh_menu_notify(MSG_DH_LIBRARY_NO_DOWNLOAD_DIR, NULL);
      return -1;
   }
   if (!(dl = (dh_menu_download_t*)calloc(1, sizeof(*dl))))
      return -1;

   dl->system = system;
   dl->size   = size;
   strlcpy(dl->name, name, sizeof(dl->name));

   if (!dh_library_get_local_path(download_dir, path,
            dl->local_path, sizeof(dl->local_path)))
   {
      dh_menu_notify(MSG_DH_LIBRARY_INVALID_PATH, name);
      goto error;
   }

   /* Already downloaded: don't download again, but make
    * sure that it is in the playlist */
   if (path_is_valid(dl->local_path))
   {
      dh_menu_notify(MSG_DH_LIBRARY_ALREADY_DOWNLOADED, name);
      dh_menu_playlist_add(dl->local_path, system);
      free(dl);
      return 0;
   }

   if (!dh_menu_get_server(base, sizeof(base), hash, sizeof(hash), &error))
   {
      dh_menu_notify(error, NULL);
      goto error;
   }
   if (!dh_library_build_download_url(base, hash, path,
            dl->url, sizeof(dl->url)))
   {
      dh_menu_notify(MSG_DH_LIBRARY_INVALID_PATH, name);
      goto error;
   }

   /* Resume an interrupted download */
   /* '<name>.<ext>.part' (fill_pathname() would replace '.<ext>') */
   strlcpy(dl->part_path, dl->local_path, sizeof(dl->part_path));
   strlcat(dl->part_path, ".part", sizeof(dl->part_path));
   if (path_is_valid(dl->part_path))
   {
      /* Not path_get_size(): 32 bit */
      RFILE *part = filestream_open(dl->part_path,
            RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
      if (part)
      {
         int64_t part_size = filestream_get_size(part);
         dl->offset        = part_size > 0 ? (uint64_t)part_size : 0;
         filestream_close(part);
      }
   }

   fill_pathname_basedir(local_dir, dl->local_path, sizeof(local_dir));
   if (!path_is_directory(local_dir) && !path_mkdir(local_dir))
   {
      dh_menu_notify(MSG_DH_LIBRARY_WRITE_FAILED, local_dir);
      goto error;
   }

   free_space = dh_library_get_free_space(local_dir);
   if (     size > 0
         && free_space >= 0
         && size > dl->offset
         && (uint64_t)free_space < size - dl->offset + DH_MENU_SPACE_MARGIN)
   {
      char need[32];
      char buf[NAME_MAX_LENGTH];
      size_t _len;
      dh_library_format_size(size, need, sizeof(need));
      _len  = strlcpy(buf, name, sizeof(buf));
      _len += strlcpy(buf + _len, " (", sizeof(buf) - _len);
      _len += strlcpy(buf + _len, need, sizeof(buf) - _len);
      strlcpy(buf + _len, ")", sizeof(buf) - _len);
      dh_menu_notify(MSG_DH_LIBRARY_NOT_ENOUGH_SPACE, buf);
      goto error;
   }

   command_event(CMD_EVENT_NETWORK_INIT, NULL);

   RARCH_LOG("[DH Library] Downloading %s -> %s (from byte %u)\n",
         dl->url, dl->local_path, (unsigned)dl->offset);

   if (!dh_menu_download_push(dl))
   {
      dh_menu_notify(MSG_DH_LIBRARY_DOWNLOAD_IN_PROGRESS, name);
      goto error;
   }

   return 0;

error:
   free(dl);
   return -1;
}

/* Callbacks */

static int dh_menu_action_ok_root(const char *path,
      const char *label, unsigned type, size_t idx, size_t entry_idx)
{
   /* Always show fresh contents when entering
    * from the main menu */
   dh_menu_cache_clear();
   return dh_menu_push_list("/",
         msg_hash_to_str(MENU_ENUM_LABEL_DEFERRED_DH_LIBRARY_LIST),
         FILE_TYPE_DIRECTORY, idx);
}

static int dh_menu_action_ok_dir(const char *path,
      const char *label, unsigned type, size_t idx, size_t entry_idx)
{
   char new_path[PATH_MAX_LENGTH];
   const dh_menu_view_item_t *item = dh_menu_view_get_checked(path, entry_idx);

   if (!item || !item->is_dir)
      return -1;
   if (!dh_library_path_join(dh_menu_st.view_path, item->name,
            new_path, sizeof(new_path)))
      return -1;

   dh_menu_cache_invalidate(new_path);
   return dh_menu_push_list(new_path,
         msg_hash_to_str(MENU_ENUM_LABEL_DEFERRED_DH_LIBRARY_LIST),
         FILE_TYPE_DIRECTORY, idx);
}

static int dh_menu_action_ok_file(const char *path,
      const char *label, unsigned type, size_t idx, size_t entry_idx)
{
   const dh_menu_view_item_t *item = dh_menu_view_get_checked(path, entry_idx);

   if (!item || item->is_dir)
      return -1;
   if (!dh_library_path_join(dh_menu_st.view_path, item->name,
            dh_menu_st.selected_path, sizeof(dh_menu_st.selected_path)))
      return -1;
   dh_menu_st.selected_size = item->size;

   return dh_menu_push_list(dh_menu_st.selected_path,
         msg_hash_to_str(MENU_ENUM_LABEL_DEFERRED_DH_LIBRARY_FILE),
         FILE_TYPE_NONE, idx);
}

static int dh_menu_action_ok_download(const char *path,
      const char *label, unsigned type, size_t idx, size_t entry_idx)
{
   const char *menu_path = NULL;
   menu_entries_get_last_stack(&menu_path, NULL, NULL, NULL, NULL);

   if (string_is_empty(menu_path))
      return -1;

   dh_menu_download(menu_path,
         string_is_equal(menu_path, dh_menu_st.selected_path)
         ? dh_menu_st.selected_size : 0);
   return 0;
}

static int dh_menu_action_ok_retry(const char *path,
      const char *label, unsigned type, size_t idx, size_t entry_idx)
{
   const char *menu_path = NULL;
   menu_entries_get_last_stack(&menu_path, NULL, NULL, NULL, NULL);

   dh_menu_cache_invalidate(string_is_empty(menu_path) ? "/" : menu_path);
   dh_menu_request_refresh();
   return 0;
}

static int dh_menu_action_ok_info(const char *path,
      const char *label, unsigned type, size_t idx, size_t entry_idx)
{
   return 0;
}

/* 'Delete Game File' (playlist entry) or 'Delete Downloaded
 * Game' (file page): opens the confirmation page */
static int dh_menu_action_ok_delete(const char *path,
      const char *label, unsigned type, size_t idx, size_t entry_idx)
{
   const char *menu_path  = NULL;
   const char *menu_label = NULL;
   const char *download_dir = dh_menu_get_download_dir();

   menu_entries_get_last_stack(&menu_path, &menu_label, NULL, NULL, NULL);

   dh_menu_st.delete_from_playlist = !string_is_equal(menu_label,
         msg_hash_to_str(MENU_ENUM_LABEL_DEFERRED_DH_LIBRARY_FILE));

   if (dh_menu_st.delete_from_playlist)
      strlcpy(dh_menu_st.delete_path, dh_menu_st.playlist_game_path,
            sizeof(dh_menu_st.delete_path));
   else if (    string_is_empty(download_dir)
            || !dh_library_get_local_path(download_dir, menu_path,
               dh_menu_st.delete_path, sizeof(dh_menu_st.delete_path)))
      return -1;

   if (string_is_empty(dh_menu_st.delete_path))
      return -1;

   return dh_menu_push_list(dh_menu_st.delete_path,
         msg_hash_to_str(MENU_ENUM_LABEL_DEFERRED_DH_LIBRARY_DELETE),
         FILE_TYPE_NONE, idx);
}

static void dh_menu_pop(unsigned levels)
{
   struct menu_state *menu_st = menu_state_get_ptr();
   size_t new_selection_ptr   = menu_st->selection_ptr;

   while (levels--)
      menu_entries_pop_stack(&new_selection_ptr, 0, true);
   menu_st->selection_ptr     = new_selection_ptr;

   /* The playlist entry may be gone: thumbnails
    * must be refreshed */
   if (menu_st->driver_ctx && menu_st->driver_ctx->refresh_thumbnail_image)
      menu_st->driver_ctx->refresh_thumbnail_image(
            menu_st->userdata, (unsigned)new_selection_ptr);
}

static int dh_menu_action_ok_delete_confirm(const char *path,
      const char *label, unsigned type, size_t idx, size_t entry_idx)
{
   dh_menu_delete_game(dh_menu_st.delete_path);
   dh_menu_st.delete_path[0] = '\0';

   /* Back to the playlist (the entry's actions refer to a
    * removed entry) or to the file page (now 'Download') */
   dh_menu_pop(dh_menu_st.delete_from_playlist ? 2 : 1);
   dh_menu_request_refresh();
   return 0;
}

static int dh_menu_action_ok_delete_cancel(const char *path,
      const char *label, unsigned type, size_t idx, size_t entry_idx)
{
   dh_menu_pop(1);
   return 0;
}

static size_t dh_menu_get_value_entry(file_list_t *list,
      unsigned *w, unsigned type, unsigned i,
      const char *label, char *s, size_t len,
      const char *path, char *s2, size_t len2)
{
   size_t _len                     = 0;
   const dh_menu_view_item_t *item = dh_menu_view_get(list, i);

   *s = '\0';
   *w = 0;
   if (item && !item->is_dir)
   {
      _len = dh_library_format_size(item->size, s, len);
      *w   = (unsigned)_len;
   }
   if (!string_is_empty(path))
      strlcpy(s2, path, len2);
   return _len;
}

static size_t dh_menu_get_value_none(file_list_t *list,
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

static int dh_menu_sublabel_entry(file_list_t *list,
      unsigned type, unsigned i,
      const char *label, const char *path,
      char *s, size_t len)
{
   const dh_menu_view_item_t *item = dh_menu_view_get(list, i);

   if (!item)
      return 0;

   if (item->is_dir)
   {
      /* Show the target playlist for system folders */
      if (string_is_equal(dh_menu_st.view_path, "/"))
      {
         const dh_library_system_t *system =
            dh_library_find_system_folder(item->name);
         if (system)
            strlcpy(s, system->playlist, len);
      }
   }
   else if (item->downloaded)
      strlcpy(s, msg_hash_to_str(MSG_DH_LIBRARY_DOWNLOADED), len);

   return 0;
}

static int dh_menu_sublabel_generic(file_list_t *list,
      unsigned type, unsigned i,
      const char *label, const char *path,
      char *s, size_t len)
{
   /* Note: 'label' is the label of the parent list,
    * so the entry is identified by its enum */
   enum msg_hash_enums sublabel  = MSG_UNKNOWN;
   menu_file_list_cbs_t *cbs     = NULL;

   if (list && i < list->size)
      cbs = (menu_file_list_cbs_t*)list->list[i].actiondata;
   if (!cbs)
      return 0;

   switch (cbs->enum_idx)
   {
      case MENU_ENUM_LABEL_DH_LIBRARY:
         sublabel = MENU_ENUM_SUBLABEL_DH_LIBRARY;
         break;
      case MENU_ENUM_LABEL_DH_LIBRARY_URL:
         sublabel = MENU_ENUM_SUBLABEL_DH_LIBRARY_URL;
         break;
      case MENU_ENUM_LABEL_DH_LIBRARY_DOWNLOAD:
         /* No 'download in the background' hint once
          * the file is already there */
         if (!string_is_equal(path, msg_hash_to_str(MSG_DH_LIBRARY_READD)))
            sublabel = MENU_ENUM_SUBLABEL_DH_LIBRARY_DOWNLOAD;
         break;
      case MENU_ENUM_LABEL_DH_LIBRARY_RETRY:
         sublabel = MENU_ENUM_SUBLABEL_DH_LIBRARY_RETRY;
         break;
      case MENU_ENUM_LABEL_DH_LIBRARY_DELETE:
         sublabel = MENU_ENUM_SUBLABEL_DH_LIBRARY_DELETE;
         break;
      default:
         break;
   }

   if (sublabel != MSG_UNKNOWN)
      strlcpy(s, msg_hash_to_str(sublabel), len);
   return 0;
}

static int dh_menu_get_title(const char *path, const char *label,
      unsigned type, char *s, size_t len)
{
   size_t _len;

   if (string_is_equal(label,
            msg_hash_to_str(MENU_ENUM_LABEL_DEFERRED_DH_LIBRARY_FILE)))
   {
      strlcpy(s, path_basename(path), len);
      return 0;
   }
   if (string_is_equal(label,
            msg_hash_to_str(MENU_ENUM_LABEL_DEFERRED_DH_LIBRARY_DELETE)))
   {
      strlcpy(s, msg_hash_to_str(MSG_DH_LIBRARY_DELETE_TITLE), len);
      return 0;
   }

   _len = strlcpy(s, msg_hash_to_str(MENU_ENUM_LABEL_VALUE_DH_LIBRARY), len);
   if (     !string_is_empty(path)
         && !string_is_equal(path, "/")
         && _len < len)
   {
      _len += strlcpy(s + _len, " ", len - _len);
      if (_len < len)
         strlcpy(s + _len, path, len - _len);
   }
   return 0;
}

/* Lists */

static bool dh_menu_append_info(file_list_t *list,
      enum msg_hash_enums msg, const char *detail)
{
   char buf[PATH_MAX_LENGTH];
   size_t _len = strlcpy(buf, msg_hash_to_str(msg), sizeof(buf));
   if (!string_is_empty(detail) && _len < sizeof(buf))
      strlcpy(buf + _len, detail, sizeof(buf) - _len);
   return menu_entries_append(list, buf,
         msg_hash_to_str(MENU_ENUM_LABEL_DH_LIBRARY_INFO),
         MENU_ENUM_LABEL_DH_LIBRARY_INFO,
         FILE_TYPE_NONE, 0, 0, NULL);
}

static void dh_menu_append_retry(file_list_t *list)
{
   menu_entries_append(list,
         msg_hash_to_str(MENU_ENUM_LABEL_VALUE_DH_LIBRARY_RETRY),
         msg_hash_to_str(MENU_ENUM_LABEL_DH_LIBRARY_RETRY),
         MENU_ENUM_LABEL_DH_LIBRARY_RETRY,
         MENU_SETTING_ACTION, 0, 0, NULL);
}

static void dh_menu_build_view(file_list_t *list, const char *path,
      const dh_library_listing_t *listing)
{
   size_t i;
   bool is_root             = string_is_equal(path, "/");
   const char *download_dir = dh_menu_get_download_dir();

   strlcpy(dh_menu_st.view_path, path, sizeof(dh_menu_st.view_path));

   if (listing->count == 0)
      return;

   if (!(dh_menu_st.view = (dh_menu_view_item_t*)calloc(listing->count,
               sizeof(*dh_menu_st.view))))
      return;

   for (i = 0; i < listing->count; i++)
   {
      char item_path[PATH_MAX_LENGTH];
      char local_path[PATH_MAX_LENGTH];
      dh_menu_view_item_t *item       = NULL;
      const dh_library_item_t *source = &listing->items[i];

      /* Top level: only folders of supported systems */
      if (is_root && (!source->is_dir
               || !dh_library_find_system_folder(source->name)))
         continue;
      /* Skip names that cannot be stored locally */
      if (!dh_library_name_is_safe(source->name))
         continue;

      item         = &dh_menu_st.view[dh_menu_st.view_count];
      item->name   = strdup(source->name);
      item->size   = source->size;
      item->is_dir = source->is_dir;

      if (     !item->is_dir
            && !string_is_empty(download_dir)
            && dh_library_path_join(path, source->name,
               item_path, sizeof(item_path))
            && dh_library_get_local_path(download_dir, item_path,
               local_path, sizeof(local_path)))
         item->downloaded = path_is_valid(local_path);

      if (item->is_dir)
         menu_entries_append(list, item->name,
               msg_hash_to_str(MENU_ENUM_LABEL_DH_LIBRARY_DIR),
               MENU_ENUM_LABEL_DH_LIBRARY_DIR,
               FILE_TYPE_DIRECTORY, 0, dh_menu_st.view_count, NULL);
      else
         menu_entries_append(list, item->name,
               msg_hash_to_str(MENU_ENUM_LABEL_DH_LIBRARY_FILE),
               MENU_ENUM_LABEL_DH_LIBRARY_FILE,
               FILE_TYPE_PLAIN, 0, dh_menu_st.view_count, NULL);

      dh_menu_st.view_count++;
   }
}

static int dh_menu_deferred_push_list(menu_displaylist_info_t *info)
{
   char url[DH_MENU_URL_LEN];
   settings_t *settings         = config_get_ptr();
   const char *path             = string_is_empty(info->path) ? "/" : info->path;
   bool is_root                 = string_is_equal(path, "/");
   enum msg_hash_enums error    = MSG_UNKNOWN;
   dh_menu_cache_entry_t *entry = NULL;

   menu_entries_clear(info->list);
   dh_menu_view_clear();
   strlcpy(dh_menu_st.view_path, path, sizeof(dh_menu_st.view_path));

   /* Share URL can be edited directly from the top level */
   if (is_root)
      MENU_DISPLAYLIST_PARSE_SETTINGS_ENUM(info->list,
            MENU_ENUM_LABEL_DH_LIBRARY_URL, PARSE_ONLY_STRING, false);

   if (!dh_menu_get_list_url(path, url, sizeof(url), &error))
      dh_menu_append_info(info->list, error, NULL);
   else
   {
      if (!(entry = dh_menu_cache_find(url)))
         entry = dh_menu_fetch(url);

      if (!entry)
      {
         dh_menu_append_info(info->list, MSG_DH_LIBRARY_CONNECT_FAILED, NULL);
         dh_menu_append_retry(info->list);
      }
      else if (entry->status == DH_STATUS_LOADING)
         dh_menu_append_info(info->list, MSG_DH_LIBRARY_LOADING, NULL);
      else if (entry->status == DH_STATUS_ERROR)
      {
         char status[16];
         status[0] = '\0';
         if (entry->error == MSG_DH_LIBRARY_HTTP_ERROR)
            snprintf(status, sizeof(status), "%d", entry->http_status);
         dh_menu_append_info(info->list, entry->error, status);
         dh_menu_append_retry(info->list);
      }
      else
      {
         entry->lru = ++dh_menu_st.lru_counter;
         dh_menu_build_view(info->list, path, &entry->listing);
         if (dh_menu_st.view_count == 0)
         {
            dh_menu_append_info(info->list, is_root
                  ? MSG_DH_LIBRARY_NO_SYSTEMS
                  : MSG_DH_LIBRARY_EMPTY, NULL);
            dh_menu_append_retry(info->list);
         }
      }
   }

   info->flags |= MD_FLAG_NEED_REFRESH | MD_FLAG_NEED_PUSH;
   menu_displaylist_process(info);
   return 0;
}

static int dh_menu_deferred_push_file(menu_displaylist_info_t *info)
{
   char buf[PATH_MAX_LENGTH];
   char local_path[PATH_MAX_LENGTH];
   const char *path                  = info->path;
   const char *download_dir          = dh_menu_get_download_dir();
   const dh_library_system_t *system = dh_library_find_system(path);
   bool have_local                   = false;
   bool downloaded                   = false;

   menu_entries_clear(info->list);

   if (     !string_is_empty(download_dir)
         && dh_library_get_local_path(download_dir, path,
            local_path, sizeof(local_path)))
   {
      have_local = true;
      downloaded = path_is_valid(local_path);
   }

   menu_entries_append(info->list,
         msg_hash_to_str(downloaded
            ? MSG_DH_LIBRARY_READD
            : MENU_ENUM_LABEL_VALUE_DH_LIBRARY_DOWNLOAD),
         msg_hash_to_str(MENU_ENUM_LABEL_DH_LIBRARY_DOWNLOAD),
         MENU_ENUM_LABEL_DH_LIBRARY_DOWNLOAD,
         MENU_SETTING_ACTION, 0, 0, NULL);

   if (downloaded)
      menu_entries_append(info->list,
            msg_hash_to_str(MSG_DH_LIBRARY_DELETE_DOWNLOADED),
            msg_hash_to_str(MENU_ENUM_LABEL_DH_LIBRARY_DELETE),
            MENU_ENUM_LABEL_DH_LIBRARY_DELETE,
            MENU_SETTING_ACTION, 0, 0, NULL);

   if (     string_is_equal(path, dh_menu_st.selected_path)
         && dh_menu_st.selected_size > 0)
   {
      dh_library_format_size(dh_menu_st.selected_size, buf, sizeof(buf));
      dh_menu_append_info(info->list, MSG_DH_LIBRARY_FILE_SIZE, buf);
   }

   if (system)
      dh_menu_append_info(info->list, MSG_DH_LIBRARY_PLAYLIST,
            system->playlist);

   if (have_local)
      dh_menu_append_info(info->list, MSG_DH_LIBRARY_SAVE_PATH,
            local_path);
   else if (string_is_empty(download_dir))
      dh_menu_append_info(info->list, MSG_DH_LIBRARY_NO_DOWNLOAD_DIR, NULL);

   info->flags |= MD_FLAG_NEED_REFRESH | MD_FLAG_NEED_PUSH;
   menu_displaylist_process(info);
   return 0;
}

/* 'Delete Game' page: 'Delete <name> (1.2 GB)? Save files
 * are kept.', then 'Delete' and 'Cancel' */
static int dh_menu_deferred_push_delete(menu_displaylist_info_t *info)
{
   char size[32];
   char name[NAME_MAX_LENGTH];
   char prompt[NAME_MAX_LENGTH + 128];
   char part_path[PATH_MAX_LENGTH];
   const char *path = info->path;

   menu_entries_clear(info->list);

   strlcpy(part_path, path, sizeof(part_path));
   strlcat(part_path, ".part", sizeof(part_path));
   dh_library_format_size(dh_menu_file_size(path)
         + dh_menu_file_size(part_path), size, sizeof(size));
   fill_pathname(name, path_basename(path), "", sizeof(name));
   snprintf(prompt, sizeof(prompt),
         msg_hash_to_str(MSG_DH_LIBRARY_DELETE_PROMPT), name, size);

   menu_entries_append(info->list, prompt,
         msg_hash_to_str(MENU_ENUM_LABEL_DH_LIBRARY_INFO),
         MENU_ENUM_LABEL_DH_LIBRARY_INFO,
         FILE_TYPE_NONE, 0, 0, NULL);
   menu_entries_append(info->list,
         msg_hash_to_str(MENU_ENUM_LABEL_VALUE_DH_LIBRARY_DELETE_CONFIRM),
         msg_hash_to_str(MENU_ENUM_LABEL_DH_LIBRARY_DELETE_CONFIRM),
         MENU_ENUM_LABEL_DH_LIBRARY_DELETE_CONFIRM,
         MENU_SETTING_ACTION, 0, 0, NULL);
   menu_entries_append(info->list,
         msg_hash_to_str(MENU_ENUM_LABEL_VALUE_DH_LIBRARY_DELETE_CANCEL),
         msg_hash_to_str(MENU_ENUM_LABEL_DH_LIBRARY_DELETE_CANCEL),
         MENU_ENUM_LABEL_DH_LIBRARY_DELETE_CANCEL,
         MENU_SETTING_ACTION, 0, 0, NULL);

   /* The selection starts on the prompt, not on 'Delete' */
   info->flags |= MD_FLAG_NEED_REFRESH | MD_FLAG_NEED_PUSH;
   menu_displaylist_process(info);
   return 0;
}

/* Public */

bool dh_library_menu_append_delete_entry(file_list_t *list,
      const char *content_path)
{
   char root[PATH_MAX_LENGTH];

   if (!dh_menu_get_owned_path(content_path, root, sizeof(root),
            dh_menu_st.playlist_game_path,
            sizeof(dh_menu_st.playlist_game_path)))
   {
      dh_menu_st.playlist_game_path[0] = '\0';
      return false;
   }

   return menu_entries_append(list,
         msg_hash_to_str(MENU_ENUM_LABEL_VALUE_DH_LIBRARY_DELETE),
         msg_hash_to_str(MENU_ENUM_LABEL_DH_LIBRARY_DELETE),
         MENU_ENUM_LABEL_DH_LIBRARY_DELETE,
         MENU_SETTING_ACTION, 0, 0, NULL);
}

bool dh_library_menu_append_main_entry(file_list_t *list)
{
   return menu_entries_append(list,
         msg_hash_to_str(MENU_ENUM_LABEL_VALUE_DH_LIBRARY),
         msg_hash_to_str(MENU_ENUM_LABEL_DH_LIBRARY),
         MENU_ENUM_LABEL_DH_LIBRARY,
         MENU_SETTING_ACTION, 0, 0, NULL);
}

void dh_library_menu_cbs_init(menu_file_list_cbs_t *cbs,
      const char *path, const char *label, unsigned type)
{
   if (!cbs || !label)
      return;

   /* Lists (menu stack entries) are identified by label */
   if (string_is_equal(label,
            msg_hash_to_str(MENU_ENUM_LABEL_DEFERRED_DH_LIBRARY_LIST)))
   {
      cbs->action_deferred_push = dh_menu_deferred_push_list;
      cbs->action_get_title     = dh_menu_get_title;
      return;
   }
   if (string_is_equal(label,
            msg_hash_to_str(MENU_ENUM_LABEL_DEFERRED_DH_LIBRARY_FILE)))
   {
      cbs->action_deferred_push = dh_menu_deferred_push_file;
      cbs->action_get_title     = dh_menu_get_title;
      return;
   }
   if (string_is_equal(label,
            msg_hash_to_str(MENU_ENUM_LABEL_DEFERRED_DH_LIBRARY_DELETE)))
   {
      cbs->action_deferred_push = dh_menu_deferred_push_delete;
      cbs->action_get_title     = dh_menu_get_title;
      return;
   }

   switch (cbs->enum_idx)
   {
      case MENU_ENUM_LABEL_DH_LIBRARY:
         cbs->action_ok        = dh_menu_action_ok_root;
         cbs->action_sublabel  = dh_menu_sublabel_generic;
         break;
      case MENU_ENUM_LABEL_DH_LIBRARY_URL:
         cbs->action_sublabel  = dh_menu_sublabel_generic;
         break;
      case MENU_ENUM_LABEL_DH_LIBRARY_DIR:
         cbs->action_ok        = dh_menu_action_ok_dir;
         cbs->action_get_value = dh_menu_get_value_entry;
         cbs->action_sublabel  = dh_menu_sublabel_entry;
         cbs->action_start     = NULL;
         cbs->action_scan      = NULL;
         break;
      case MENU_ENUM_LABEL_DH_LIBRARY_FILE:
         cbs->action_ok        = dh_menu_action_ok_file;
         cbs->action_get_value = dh_menu_get_value_entry;
         cbs->action_sublabel  = dh_menu_sublabel_entry;
         cbs->action_start     = NULL;
         cbs->action_scan      = NULL;
         break;
      case MENU_ENUM_LABEL_DH_LIBRARY_DOWNLOAD:
         cbs->action_ok        = dh_menu_action_ok_download;
         cbs->action_get_value = dh_menu_get_value_none;
         cbs->action_sublabel  = dh_menu_sublabel_generic;
         break;
      case MENU_ENUM_LABEL_DH_LIBRARY_RETRY:
         cbs->action_ok        = dh_menu_action_ok_retry;
         cbs->action_get_value = dh_menu_get_value_none;
         cbs->action_sublabel  = dh_menu_sublabel_generic;
         break;
      case MENU_ENUM_LABEL_DH_LIBRARY_INFO:
         cbs->action_ok        = dh_menu_action_ok_info;
         cbs->action_get_value = dh_menu_get_value_none;
         cbs->action_sublabel  = NULL;
         break;
      case MENU_ENUM_LABEL_DH_LIBRARY_DELETE:
         cbs->action_ok        = dh_menu_action_ok_delete;
         cbs->action_get_value = dh_menu_get_value_none;
         cbs->action_sublabel  = dh_menu_sublabel_generic;
         break;
      case MENU_ENUM_LABEL_DH_LIBRARY_DELETE_CONFIRM:
         cbs->action_ok        = dh_menu_action_ok_delete_confirm;
         cbs->action_get_value = dh_menu_get_value_none;
         cbs->action_sublabel  = NULL;
         break;
      case MENU_ENUM_LABEL_DH_LIBRARY_DELETE_CANCEL:
         cbs->action_ok        = dh_menu_action_ok_delete_cancel;
         cbs->action_get_value = dh_menu_get_value_none;
         cbs->action_sublabel  = NULL;
         break;
      default:
         break;
   }
}

#endif /* HAVE_NETWORKING */
