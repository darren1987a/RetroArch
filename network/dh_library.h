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

/* DH Game Library: browse and download content from a
 * FileBrowser Quantum public share.
 *
 * This file only contains 'pure' helpers (URL handling,
 * JSON parsing, path validation) with no dependency on
 * the RetroArch runtime, so that it can be unit tested
 * on its own. Menu/task/playlist glue lives in
 * menu/dh_library_menu.c */

#ifndef __DH_LIBRARY_H
#define __DH_LIBRARY_H

#include <stddef.h>
#include <stdint.h>

#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* Name of the sub directory (inside the download
 * directory) that holds all downloaded games */
#define DH_LIBRARY_DIR_NAME "DHGameCenter"

typedef struct dh_library_system
{
   const char *folder;        /* Top level share folder, e.g. "PSGame" */
   const char *playlist;      /* Playlist name (without .lpl) */
   const char *core_file_id;  /* Default core, e.g. "pcsx_rearmed_libretro" */
} dh_library_system_t;

typedef struct dh_library_item
{
   char *name;
   uint64_t size;
   bool is_dir;
} dh_library_item_t;

typedef struct dh_library_listing
{
   dh_library_item_t *items;
   size_t count;
   size_t capacity;
} dh_library_listing_t;

/* Splits a share URL such as
 *   http://192.168.1.10:9520/public/share/<hash>
 * into the server base URL ("http://192.168.1.10:9520")
 * and the share hash. Also accepts URLs without a scheme,
 * with a FileBrowser 'baseURL' prefix, or API URLs
 * containing a 'hash=' query parameter.
 * Returns false if no valid hash could be found. */
bool dh_library_parse_share_url(const char *url,
      char *base, size_t base_len,
      char *hash, size_t hash_len);

/* Percent-encodes everything except RFC 3986
 * unreserved characters. Returns false on truncation. */
bool dh_library_urlencode(const char *src, char *s, size_t len);

/* Builds the listing API URL for 'path' (relative to
 * the share root, e.g. "/PSGame") */
bool dh_library_build_list_url(const char *base, const char *hash,
      const char *path, char *s, size_t len);

/* Builds the raw download URL for file 'path' */
bool dh_library_build_download_url(const char *base, const char *hash,
      const char *path, char *s, size_t len);

/* Parses a FileBrowser Quantum directory listing
 * (iteminfo.FileInfo JSON). Tolerant to unknown fields,
 * nested metadata objects and to the legacy 'items'
 * array format. Hidden (dot) files are skipped.
 * Items are sorted: folders first, then by name.
 * Returns false if the document is not valid JSON or
 * does not look like a directory listing. */
bool dh_library_parse_listing(const char *buf, size_t len,
      dh_library_listing_t *listing);

void dh_library_listing_free(dh_library_listing_t *listing);

/* Returns true if 'name' may safely be used as a single
 * local path component (no separators, not "." / "..") */
bool dh_library_name_is_safe(const char *name);

/* Joins share path 'parent' and child 'name' into 's',
 * producing paths like "/PSGame/RPG" */
bool dh_library_path_join(const char *parent, const char *name,
      char *s, size_t len);

/* Returns the system mapped to the first component of
 * share path 'path' (e.g. "/PSGame/x.7z"), or NULL */
const dh_library_system_t *dh_library_find_system(const char *path);

/* Returns the system mapped to top level folder 'folder' */
const dh_library_system_t *dh_library_find_system_folder(const char *folder);

/* Builds the local file path for share path 'path':
 *   <download_dir>/DHGameCenter/<share path>
 * Every path component is validated. */
bool dh_library_get_local_path(const char *download_dir,
      const char *path, char *s, size_t len);

/* Normalizes absolute path 'path' into 's': drops "." and
 * repeated separators and resolves ".." (lexically, the path
 * need not exist). Fails for relative paths and if ".." would
 * go above the root */
bool dh_library_normalize_path(const char *path, char *s, size_t len);

/* Builds the normalized '<download_dir>/DHGameCenter' */
bool dh_library_get_root(const char *download_dir, char *s, size_t len);

/* Normalizes 'path' into 's'; returns true only if it lies
 * strictly inside the (normalized) directory 'root' */
bool dh_library_path_is_inside(const char *root, const char *path,
      char *s, size_t len);

/* Deletes the downloaded game 'path' (which must lie inside
 * 'root', see dh_library_get_root()), a left over '.part'
 * file and the parent directories left empty, up to 'root'
 * (which is kept). Save files are elsewhere and untouched.
 * Returns true if the game file is gone. */
bool dh_library_delete_downloaded(const char *root, const char *path);

/* Formats a byte count as e.g. "512.3 MB" */
size_t dh_library_format_size(uint64_t size, char *s, size_t len);

/* Returns available bytes on the volume containing 'dir',
 * or -1 if this cannot be determined on this platform */
int64_t dh_library_get_free_space(const char *dir);

RETRO_END_DECLS

#endif
