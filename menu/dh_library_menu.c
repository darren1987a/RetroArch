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
 * Downloads use task_push_http_transfer_file() (with
 * progress display); the callback writes the file to
 *   <Downloads dir>/DHGameCenter/<share path>
 * and adds it to the system playlist. */

#include <stdlib.h>
#include <string.h>

#include <compat/strl.h>
#include <string/stdstring.h>
#include <file/file_path.h>
#include <streams/file_stream.h>
#include <retro_miscellaneous.h>

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
#include "../tasks/task_file_transfer.h"
#include "../tasks/tasks_internal.h"
#include "../network/dh_library.h"

#define DH_MENU_CACHE_SIZE 8
#define DH_MENU_URL_LEN    2048
/* Keep some head room when checking free space */
#define DH_MENU_SPACE_MARGIN (16 * 1024 * 1024)
/* Files are downloaded with HTTP Range requests of this
 * size, since the HTTP task keeps a whole response in
 * memory (PSP images are 1-2 GB) */
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

typedef struct dh_menu_download
{
   const dh_library_system_t *system;
   uint64_t size;   /* From the listing (rounded up to disk blocks) */
   uint64_t offset; /* Bytes already written to the .part file */
   uint64_t requested; /* Length of the pending range request */
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

static void dh_menu_playlist_add(const char *local_path,
      const dh_library_system_t *system)
{
   char lpl_name[NAME_MAX_LENGTH];
   char lpl_path[PATH_MAX_LENGTH];
   char entry_label[NAME_MAX_LENGTH];
   playlist_config_t playlist_config;
   size_t i;
   playlist_t *playlist          = NULL;
   playlist_t *cached_playlist   = NULL;
   core_info_list_t *core_list   = NULL;
   const core_info_t *core       = NULL;
   settings_t *settings          = config_get_ptr();
   struct menu_state *menu_st    = menu_state_get_ptr();
   const char *dir_playlist      = settings->paths.directory_playlist;

   if (string_is_empty(dir_playlist))
   {
      RARCH_ERR("[DH Library] Playlist directory is not set.\n");
      return;
   }

   fill_pathname(lpl_name, system->playlist, ".lpl", sizeof(lpl_name));
   fill_pathname_join_special(lpl_path, dir_playlist, lpl_name,
         sizeof(lpl_path));
   path_mkdir(dir_playlist);

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
   {
      RARCH_ERR("[DH Library] Failed to open playlist: %s\n", lpl_path);
      return;
   }

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

   /* If the currently cached playlist was modified,
    * it must be re-cached */
   if (     (cached_playlist = playlist_get_cached())
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

   /* New playlists must show up in the menu tabs */
   if (menu_st->driver_ctx && menu_st->driver_ctx->environ_cb)
      menu_st->driver_ctx->environ_cb(MENU_ENVIRON_RESET_HORIZONTAL_LIST,
            NULL, menu_st->userdata);

   dh_menu_notify(MSG_DH_LIBRARY_ADDED_TO_PLAYLIST, system->playlist);
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

static void dh_menu_cb_download(retro_task_t *task,
      void *task_data, void *user_data, const char *err);

/* Requests the next chunk. The task title shows the
 * overall progress, the progress bar the chunk's.
 * Note: net_http only exposes response headers on
 * errors, so Content-Range can't be used; a chunk
 * shorter than requested marks the end of the file */
static bool dh_menu_push_chunk(file_transfer_t *transf)
{
   char headers[96];
   char title[NAME_MAX_LENGTH + 64];
   size_t _len;
   retro_task_t *task     = NULL;
   dh_menu_download_t *dl = (dh_menu_download_t*)transf->user_data;

   dl->requested = DH_MENU_CHUNK_SIZE;

   _len  = strlcpy(headers, "Range: bytes=", sizeof(headers));
   _len += dh_menu_u64_to_str(dl->offset, headers + _len, sizeof(headers) - _len);
   _len += strlcpy(headers + _len, "-", sizeof(headers) - _len);
   _len += dh_menu_u64_to_str(dl->offset + dl->requested - 1,
         headers + _len, sizeof(headers) - _len);
   strlcpy(headers + _len, "\r\n", sizeof(headers) - _len);

   _len  = strlcpy(title, msg_hash_to_str(MSG_DOWNLOADING), sizeof(title));
   _len += strlcpy(title + _len, ": ", sizeof(title) - _len);
   _len += strlcpy(title + _len, dl->name, sizeof(title) - _len);
   /* Listing sizes are approximate (rounded up to disk blocks) */
   if (dl->size > 0 && _len < sizeof(title))
   {
      uint64_t percent = dl->offset * 100 / dl->size;
      snprintf(title + _len, sizeof(title) - _len, " (%u%%)",
            (unsigned)(percent > 99 ? 99 : percent));
   }

   /* Fails if the same URL is already being downloaded */
   if (!(task = (retro_task_t*)task_push_http_transfer_with_headers(
               dl->url, false, NULL, headers, dh_menu_cb_download, transf)))
      return false;

   task_set_title(task, strdup(title));
   return true;
}

static void dh_menu_cb_download(retro_task_t *task,
      void *task_data, void *user_data, const char *err)
{
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;
   file_transfer_t *transf    = (file_transfer_t*)user_data;
   dh_menu_download_t *dl     = transf ? (dh_menu_download_t*)transf->user_data : NULL;
   bool last_chunk            = false;

   if (!dl)
      goto end;

   if (err || !data)
      goto failed;

   switch (data->status)
   {
      case 206:
         /* Partial content: a short chunk is the last one */
         if (!data->data || data->len == 0)
            goto failed;
         last_chunk = ((uint64_t)data->len < dl->requested);
         break;
      case 200:
         /* Server ignored the range: this is the whole file */
         if (!data->data)
            goto failed;
         dl->offset = 0;
         last_chunk = true;
         break;
      case 416:
         /* Range not satisfiable: the .part file already
          * holds the whole file (its size was a multiple of
          * the chunk size, or a finished .part was resumed) */
         if (     dl->offset > 0
               && (dl->size == 0 || dl->offset + DH_MENU_SIZE_TOLERANCE >= dl->size))
            goto finished;
         /* Stale .part file, start over */
         if (dl->offset > 0)
         {
            filestream_delete(dl->part_path);
            dl->offset = 0;
            if (dh_menu_push_chunk(transf))
               return;
         }
         goto failed;
      default:
         goto failed;
   }

   if (!dh_menu_part_write(dl->part_path, dl->offset,
            data->data, data->len))
   {
      dh_menu_notify(MSG_DH_LIBRARY_WRITE_FAILED, dl->name);
      goto end;
   }

   dl->offset += (uint64_t)data->len;

   if (!last_chunk)
   {
      if (dh_menu_push_chunk(transf))
         return;
      goto failed;
   }

finished:
   /* The .part file keeps an interrupted download from
    * looking like a finished one */
   if (filestream_rename(dl->part_path, dl->local_path) != 0)
   {
      dh_menu_notify(MSG_DH_LIBRARY_WRITE_FAILED, dl->name);
      goto end;
   }

   RARCH_LOG("[DH Library] Saved: %s\n", dl->local_path);

   dh_menu_playlist_add(dl->local_path, dl->system);

   if (dh_menu_top_is_ours())
      dh_menu_request_refresh();
   goto end;

failed:
   /* The .part file is kept, a new download resumes from it */
   RARCH_ERR("[DH Library] Download failed (HTTP %d) at %u MB: %s\n",
         data ? data->status : 0,
         (unsigned)(dl->offset / (1024 * 1024)),
         err ? err : "");
   dh_menu_notify(MSG_DH_LIBRARY_DOWNLOAD_FAILED, dl->name);

end:
   free(dl);
   free(transf);
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
   file_transfer_t *transf            = NULL;
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

   if (!(transf = (file_transfer_t*)calloc(1, sizeof(*transf))))
      goto error;

   transf->enum_idx  = MSG_UNKNOWN;
   transf->user_data = dl;
   strlcpy(transf->path, name, sizeof(transf->path));

   command_event(CMD_EVENT_NETWORK_INIT, NULL);

   RARCH_LOG("[DH Library] Downloading %s -> %s (from byte %u)\n",
         dl->url, dl->local_path, (unsigned)dl->offset);

   if (!dh_menu_push_chunk(transf))
   {
      dh_menu_notify(MSG_DH_LIBRARY_DOWNLOAD_IN_PROGRESS, name);
      free(transf);
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

/* Public */

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
      default:
         break;
   }
}

#endif /* HAVE_NETWORKING */
