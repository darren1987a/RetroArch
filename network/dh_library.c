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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <retro_miscellaneous.h>
#include <compat/strl.h>
#include <string/stdstring.h>
#include <file/file_path.h>
#include <streams/file_stream.h>
#include <formats/rjson.h>

#if defined(_WIN32) && !defined(_XBOX)
#include <windows.h>
#include <encodings/utf.h>
#define DH_LIBRARY_HAVE_FREE_SPACE
#elif defined(__APPLE__) || defined(__linux__) || defined(ANDROID)
#include <sys/statvfs.h>
#define DH_LIBRARY_HAVE_FREE_SPACE
#endif

#include "dh_library.h"

/* Folder name -> playlist -> default core.
 * Folders that are not listed here (e.g. PCGame,
 * Switch) are hidden from the menu */
static const dh_library_system_t dh_library_systems[] = {
   { "PSGame",  "Sony - PlayStation",          "pcsx_rearmed_libretro" },
   { "PSPGame", "Sony - PlayStation Portable", "ppsspp_libretro"       }
};

#define DH_LIBRARY_SHARE_TOKEN "/public/share/"
#define DH_LIBRARY_API_LIST    "/public/api/resources?hash="
#define DH_LIBRARY_API_GET     "/public/api/resources/download?hash="

/* URL helpers */

static bool dh_library_hash_char_is_valid(char c)
{
   return (c >= 'a' && c <= 'z')
       || (c >= 'A' && c <= 'Z')
       || (c >= '0' && c <= '9')
       ||  c == '-' || c == '_' || c == '.' || c == '~';
}

static size_t dh_library_copy_hash(const char *src, char *s, size_t len)
{
   size_t _len = 0;
   while (dh_library_hash_char_is_valid(src[_len]))
   {
      if (_len + 1 >= len)
         return 0;
      s[_len] = src[_len];
      _len++;
   }
   s[_len] = '\0';
   /* Hash must be terminated by end of string, a path
    * separator, a query or a fragment */
   if (     src[_len] != '\0' && src[_len] != '/'
         && src[_len] != '?'  && src[_len] != '&'
         && src[_len] != '#')
      return 0;
   return _len;
}

bool dh_library_parse_share_url(const char *url,
      char *base, size_t base_len,
      char *hash, size_t hash_len)
{
   char tmp[1024];
   size_t _len;
   char *start      = NULL;
   char *end        = NULL;
   char *scheme_end = NULL;
   char *token      = NULL;

   if (!url || !base || !hash || base_len == 0 || hash_len == 0)
      return false;

   base[0] = '\0';
   hash[0] = '\0';

   /* Trim surrounding white space */
   while (*url == ' ' || *url == '\t')
      url++;
   if (!*url)
      return false;

   if (strstr(url, "://"))
      _len = strlcpy(tmp, url, sizeof(tmp));
   else
   {
      /* Assume plain HTTP if no scheme was given */
      _len  = strlcpy(tmp, "http://", sizeof(tmp));
      _len += strlcpy(tmp + _len, url, sizeof(tmp) - _len);
   }
   if (_len >= sizeof(tmp))
      return false;

   end = tmp + strlen(tmp);
   while (end > tmp && (end[-1] == ' ' || end[-1] == '\t'
            || end[-1] == '\r' || end[-1] == '\n'))
      *--end = '\0';

   scheme_end = strstr(tmp, "://");
   if (!scheme_end || scheme_end == tmp)
      return false;
   start = scheme_end + 3;
   if (!*start || *start == '/')
      return false; /* No host */

   if ((token = strstr(start, DH_LIBRARY_SHARE_TOKEN)))
   {
      /* Share page URL: <base>/public/share/<hash>[/...] */
      if (!dh_library_copy_hash(token + STRLEN_CONST(DH_LIBRARY_SHARE_TOKEN),
               hash, hash_len))
         return false;
      *token = '\0';
   }
   else
   {
      /* API URL: <base>/public/api/...?hash=<hash> */
      char *query = strchr(start, '?');
      char *param = NULL;

      if (query)
      {
         param = strstr(query, "?hash=");
         if (!param)
            param = strstr(query, "&hash=");
      }

      if (!param)
         return false;
      if (!dh_library_copy_hash(param + STRLEN_CONST("?hash="),
               hash, hash_len))
         return false;

      if ((token = strstr(start, "/public/")))
         *token = '\0';
      else
      {
         /* Keep scheme + host only */
         char *slash = strchr(start, '/');
         if (slash)
            *slash = '\0';
         else
            *query = '\0';
      }
   }

   /* Remove trailing slashes from base URL */
   end = tmp + strlen(tmp);
   while (end > start && end[-1] == '/')
      *--end = '\0';
   if (end <= start)
      return false;

   if (strlcpy(base, tmp, base_len) >= base_len)
   {
      base[0] = '\0';
      return false;
   }

   return !string_is_empty(hash);
}

bool dh_library_urlencode(const char *src, char *s, size_t len)
{
   static const char hex[] = "0123456789ABCDEF";
   size_t _len = 0;

   if (!s || len == 0)
      return false;

   s[0] = '\0';
   if (!src)
      return true;

   for (; *src; src++)
   {
      unsigned char c = (unsigned char)*src;

      if (     (c >= 'a' && c <= 'z')
            || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9')
            ||  c == '-' || c == '_' || c == '.' || c == '~')
      {
         if (_len + 1 >= len)
            goto truncated;
         s[_len++] = (char)c;
      }
      else
      {
         if (_len + 3 >= len)
            goto truncated;
         s[_len++] = '%';
         s[_len++] = hex[c >> 4];
         s[_len++] = hex[c & 0xF];
      }
   }

   s[_len] = '\0';
   return true;

truncated:
   s[_len] = '\0';
   return false;
}

static bool dh_library_build_url(const char *base, const char *api,
      const char *hash, const char *param, const char *path,
      char *s, size_t len)
{
   char enc_hash[256];
   char enc_path[2048];
   size_t _len;

   if (     string_is_empty(base) || string_is_empty(hash)
         || !s || len == 0)
      return false;

   if (!dh_library_urlencode(hash, enc_hash, sizeof(enc_hash)))
      return false;
   if (!dh_library_urlencode(string_is_empty(path) ? "/" : path,
            enc_path, sizeof(enc_path)))
      return false;

   _len  = strlcpy(s, base, len);
   _len += strlcpy(s + _len, api,      len > _len ? len - _len : 0);
   _len += strlcpy(s + _len, enc_hash, len > _len ? len - _len : 0);
   _len += strlcpy(s + _len, param,    len > _len ? len - _len : 0);
   _len += strlcpy(s + _len, enc_path, len > _len ? len - _len : 0);

   return _len < len;
}

bool dh_library_build_list_url(const char *base, const char *hash,
      const char *path, char *s, size_t len)
{
   return dh_library_build_url(base, DH_LIBRARY_API_LIST, hash,
         "&path=", path, s, len);
}

bool dh_library_build_download_url(const char *base, const char *hash,
      const char *path, char *s, size_t len)
{
   /* Note: the download endpoint takes the file path
    * as 'file', not 'path'. Without a 'file' parameter
    * it would send the whole share as a zip archive */
   if (string_is_empty(path) || string_is_equal(path, "/"))
      return false;
   return dh_library_build_url(base, DH_LIBRARY_API_GET, hash,
         "&file=", path, s, len);
}

/* JSON listing parser */

enum dh_library_section
{
   DH_SECTION_NONE = 0,
   DH_SECTION_FOLDERS,
   DH_SECTION_FILES,
   DH_SECTION_ITEMS     /* Legacy single 'items' array */
};

typedef struct dh_library_parse_item
{
   char *name;
   uint64_t size;
   bool is_dir_type;
   bool is_dir_flag;
   bool hidden;
} dh_library_parse_item_t;

static bool dh_library_listing_push(dh_library_listing_t *listing,
      char *name, uint64_t size, bool is_dir)
{
   if (listing->count >= listing->capacity)
   {
      size_t new_cap             = listing->capacity ? listing->capacity * 2 : 32;
      dh_library_item_t *items   = (dh_library_item_t*)realloc(
            listing->items, new_cap * sizeof(*items));
      if (!items)
         return false;
      listing->items             = items;
      listing->capacity          = new_cap;
   }

   listing->items[listing->count].name   = name;
   listing->items[listing->count].size   = size;
   listing->items[listing->count].is_dir = is_dir;
   listing->count++;
   return true;
}

static uint64_t dh_library_parse_uint64(const char *str)
{
   uint64_t val = 0;
   if (!str || *str == '-')
      return 0;
   while (*str >= '0' && *str <= '9')
   {
      val = val * 10 + (uint64_t)(*str - '0');
      str++;
   }
   return val;
}

static int dh_library_item_cmp(const void *a, const void *b)
{
   const dh_library_item_t *ia = (const dh_library_item_t*)a;
   const dh_library_item_t *ib = (const dh_library_item_t*)b;
   const unsigned char *pa     = (const unsigned char*)ia->name;
   const unsigned char *pb     = (const unsigned char*)ib->name;

   if (ia->is_dir != ib->is_dir)
      return ia->is_dir ? -1 : 1;

   while (*pa && *pb)
   {
      int ca = tolower(*pa);
      int cb = tolower(*pb);
      if (ca != cb)
         return ca - cb;
      pa++;
      pb++;
   }
   return (int)*pa - (int)*pb;
}

bool dh_library_parse_listing(const char *buf, size_t len,
      dh_library_listing_t *listing)
{
   char key_top[32];
   char key_item[32];
   dh_library_parse_item_t item;
   rjson_t *json                    = NULL;
   unsigned depth                   = 0;
   enum dh_library_section section  = DH_SECTION_NONE;
   bool in_item                     = false;
   bool is_listing                  = false;
   bool top_is_file                 = false;
   bool ok                          = false;

   if (!listing)
      return false;

   listing->items    = NULL;
   listing->count    = 0;
   listing->capacity = 0;

   if (!buf || len == 0)
      return false;

   if (!(json = rjson_open_buffer(buf, len)))
      return false;

   rjson_set_options(json,
           RJSON_OPTION_ALLOW_UTF8BOM
         | RJSON_OPTION_REPLACE_INVALID_ENCODING);

   key_top[0]  = '\0';
   key_item[0] = '\0';
   memset(&item, 0, sizeof(item));

   for (;;)
   {
      enum rjson_type type = rjson_next(json);

      switch (type)
      {
         case RJSON_DONE:
            ok = (depth == 0);
            goto end;
         case RJSON_ERROR:
            goto end;
         case RJSON_OBJECT:
            depth++;
            if (depth == 3 && section != DH_SECTION_NONE)
            {
               free(item.name);
               memset(&item, 0, sizeof(item));
               key_item[0] = '\0';
               in_item     = true;
            }
            break;
         case RJSON_OBJECT_END:
            if (depth == 3 && in_item)
            {
               bool is_dir;
               in_item = false;

               switch (section)
               {
                  case DH_SECTION_FOLDERS:
                     is_dir = true;
                     break;
                  case DH_SECTION_FILES:
                     is_dir = false;
                     break;
                  default:
                     is_dir = item.is_dir_type || item.is_dir_flag;
                     break;
               }

               if (     !string_is_empty(item.name)
                     && !item.hidden
                     && item.name[0] != '.')
               {
                  if (!dh_library_listing_push(listing, item.name,
                           is_dir ? 0 : item.size, is_dir))
                     goto end;
                  item.name = NULL; /* Ownership transferred */
               }
               free(item.name);
               item.name = NULL;
            }
            if (depth > 0)
               depth--;
            break;
         case RJSON_ARRAY:
            depth++;
            if (depth == 2)
            {
               if (string_is_equal(key_top, "folders"))
                  section = DH_SECTION_FOLDERS;
               else if (string_is_equal(key_top, "files"))
                  section = DH_SECTION_FILES;
               else if (string_is_equal(key_top, "items"))
                  section = DH_SECTION_ITEMS;
               else
                  section = DH_SECTION_NONE;

               if (section != DH_SECTION_NONE)
                  is_listing = true;
            }
            break;
         case RJSON_ARRAY_END:
            if (depth == 2)
               section = DH_SECTION_NONE;
            if (depth > 0)
               depth--;
            break;
         case RJSON_STRING:
            {
               size_t str_len  = 0;
               const char *str = rjson_get_string(json, &str_len);
               bool is_key     =
                     rjson_get_context_type(json) == RJSON_OBJECT
                  && (rjson_get_context_count(json) & 1);

               if (is_key)
               {
                  if (depth == 1)
                     strlcpy(key_top, str, sizeof(key_top));
                  else if (depth == 3 && in_item)
                     strlcpy(key_item, str, sizeof(key_item));
               }
               else if (depth == 1)
               {
                  if (string_is_equal(key_top, "type"))
                  {
                     if (string_is_equal(str, "directory"))
                        is_listing  = true;
                     else
                        top_is_file = true;
                  }
               }
               else if (depth == 3 && in_item)
               {
                  if (string_is_equal(key_item, "name"))
                  {
                     free(item.name);
                     item.name = strdup(str);
                  }
                  else if (string_is_equal(key_item, "type"))
                     item.is_dir_type = string_is_equal(str, "directory");
                  else if (string_is_equal(key_item, "size"))
                     item.size = dh_library_parse_uint64(str);
               }
            }
            break;
         case RJSON_NUMBER:
            if (     depth == 3 && in_item
                  && string_is_equal(key_item, "size"))
            {
               size_t num_len = 0;
               item.size      = dh_library_parse_uint64(
                     rjson_get_string(json, &num_len));
            }
            break;
         case RJSON_TRUE:
         case RJSON_FALSE:
            if (depth == 3 && in_item)
            {
               if (string_is_equal(key_item, "hidden"))
                  item.hidden      = (type == RJSON_TRUE);
               else if (string_is_equal(key_item, "isDir"))
                  item.is_dir_flag = (type == RJSON_TRUE);
            }
            break;
         case RJSON_NULL:
         default:
            break;
      }
   }

end:
   free(item.name);
   rjson_free(json);

   /* A file (rather than a directory) or an error object
    * such as {"status":404,"message":"..."} is not a listing */
   if (ok && (!is_listing || (top_is_file && listing->count == 0)))
      ok = false;

   if (!ok)
   {
      dh_library_listing_free(listing);
      return false;
   }

   if (listing->count > 1)
      qsort(listing->items, listing->count,
            sizeof(*listing->items), dh_library_item_cmp);

   return true;
}

void dh_library_listing_free(dh_library_listing_t *listing)
{
   size_t i;

   if (!listing)
      return;

   for (i = 0; i < listing->count; i++)
      free(listing->items[i].name);
   free(listing->items);

   listing->items    = NULL;
   listing->count    = 0;
   listing->capacity = 0;
}

/* Path helpers */

bool dh_library_name_is_safe(const char *name)
{
   if (string_is_empty(name))
      return false;
   if (string_is_equal(name, ".") || string_is_equal(name, ".."))
      return false;
   for (; *name; name++)
   {
      unsigned char c = (unsigned char)*name;
      if (c < 0x20 || c == '/' || c == '\\' || c == ':')
         return false;
   }
   return true;
}

bool dh_library_path_join(const char *parent, const char *name,
      char *s, size_t len)
{
   size_t _len;

   if (!s || len == 0 || !dh_library_name_is_safe(name))
      return false;

   if (string_is_empty(parent) || string_is_equal(parent, "/"))
      _len = strlcpy(s, "/", len);
   else
   {
      _len = strlcpy(s, parent, len);
      if (_len < len && s[_len - 1] != '/')
         _len += strlcpy(s + _len, "/", len - _len);
   }
   if (_len >= len)
      return false;
   _len += strlcpy(s + _len, name, len - _len);
   return _len < len;
}

const dh_library_system_t *dh_library_find_system_folder(const char *folder)
{
   size_t i;

   if (string_is_empty(folder))
      return NULL;

   for (i = 0; i < ARRAY_SIZE(dh_library_systems); i++)
      if (string_is_equal_case_insensitive(folder,
               dh_library_systems[i].folder))
         return &dh_library_systems[i];

   return NULL;
}

const dh_library_system_t *dh_library_find_system(const char *path)
{
   char folder[256];
   size_t _len = 0;

   if (string_is_empty(path))
      return NULL;

   while (*path == '/')
      path++;

   while (path[_len] && path[_len] != '/' && _len + 1 < sizeof(folder))
   {
      folder[_len] = path[_len];
      _len++;
   }
   folder[_len] = '\0';

   return dh_library_find_system_folder(folder);
}

bool dh_library_get_local_path(const char *download_dir,
      const char *path, char *s, size_t len)
{
   char component[256];
   size_t _len;
   size_t count = 0;

   if (string_is_empty(download_dir) || string_is_empty(path) || !s || len == 0)
      return false;

   if ((_len = fill_pathname_join_special(s, download_dir,
               DH_LIBRARY_DIR_NAME, len)) >= len)
      return false;

   while (*path)
   {
      size_t comp_len = 0;

      while (*path == '/')
         path++;
      if (!*path)
         break;

      while (path[comp_len] && path[comp_len] != '/')
      {
         if (comp_len + 1 >= sizeof(component))
            return false;
         component[comp_len] = path[comp_len];
         comp_len++;
      }
      component[comp_len] = '\0';
      path += comp_len;

      if (!dh_library_name_is_safe(component))
         return false;

      if (_len + 1 >= len)
         return false;
      s[_len++] = PATH_DEFAULT_SLASH_C();
      s[_len]   = '\0';
      _len     += strlcpy(s + _len, component, len - _len);
      if (_len >= len)
         return false;
      count++;
   }

   return count > 0;
}

static bool dh_library_is_sep(char c)
{
#ifdef _WIN32
   return c == '/' || c == '\\';
#else
   return c == '/';
#endif
}

bool dh_library_normalize_path(const char *path, char *s, size_t len)
{
   /* Offsets in 's' of the separator before each component,
    * so that '..' can drop the last one */
   size_t starts[256];
   size_t depth = 0;
   size_t _len  = 0;

   if (string_is_empty(path) || !s || len < 4)
      return false;

   /* Root: '/' or a drive ('C:\'); relative paths are refused */
   if (dh_library_is_sep(path[0]))
      path++;
#ifdef _WIN32
   else if (isalpha((unsigned char)path[0]) && path[1] == ':'
         && dh_library_is_sep(path[2]))
   {
      s[_len++] = path[0];
      s[_len++] = ':';
      path     += 3;
   }
#endif
   else
      return false;
   s[_len] = '\0';

   while (*path)
   {
      size_t comp_len = 0;

      while (dh_library_is_sep(*path))
         path++;
      while (path[comp_len] && !dh_library_is_sep(path[comp_len]))
         comp_len++;

      if (comp_len == 0 || (comp_len == 1 && path[0] == '.'))
         ;
      else if (comp_len == 2 && path[0] == '.' && path[1] == '.')
      {
         /* Can't go above the root */
         if (depth == 0)
            return false;
         _len    = starts[--depth];
         s[_len] = '\0';
      }
      else
      {
         if (depth >= ARRAY_SIZE(starts) || _len + comp_len + 2 > len)
            return false;
         starts[depth++] = _len;
         s[_len++]       = PATH_DEFAULT_SLASH_C();
         memcpy(s + _len, path, comp_len);
         _len           += comp_len;
         s[_len]         = '\0';
      }
      path += comp_len;
   }

   /* The root itself */
   if (_len == 0 || s[_len - 1] == ':')
   {
      s[_len++] = PATH_DEFAULT_SLASH_C();
      s[_len]   = '\0';
   }
   return true;
}

bool dh_library_get_root(const char *download_dir, char *s, size_t len)
{
   char tmp[PATH_MAX_LENGTH];

   if (     string_is_empty(download_dir)
         || fill_pathname_join_special(tmp, download_dir,
            DH_LIBRARY_DIR_NAME, sizeof(tmp)) >= sizeof(tmp))
      return false;
   return dh_library_normalize_path(tmp, s, len);
}

bool dh_library_path_is_inside(const char *root, const char *path,
      char *s, size_t len)
{
   size_t root_len;

   if (     string_is_empty(root)
         || !dh_library_normalize_path(path, s, len))
      return false;

   root_len = strlen(root);
   return    strncmp(s, root, root_len) == 0
          && dh_library_is_sep(s[root_len])
          && s[root_len + 1] != '\0';
}

bool dh_library_delete_downloaded(const char *root, const char *path)
{
   char root_norm[PATH_MAX_LENGTH];
   char file[PATH_MAX_LENGTH];
   char part[PATH_MAX_LENGTH];
   size_t root_len;
   size_t _len;

   if (     !dh_library_normalize_path(root, root_norm, sizeof(root_norm))
         || !dh_library_path_is_inside(root_norm, path, file, sizeof(file))
         || path_is_directory(file))
      return false;

   if (path_is_valid(file))
      filestream_delete(file);

   /* A left over partial download */
   strlcpy(part, file, sizeof(part));
   if (strlcat(part, ".part", sizeof(part)) < sizeof(part)
         && path_is_valid(part))
      filestream_delete(part);

   /* Parent directories left empty, up to the root (kept):
    * deleting a directory that isn't empty fails */
   root_len = strlen(root_norm);
   _len     = strlen(file);
   for (;;)
   {
      while (_len > 0 && !dh_library_is_sep(file[_len - 1]))
         _len--;
      if (_len == 0)
         break;
      file[--_len] = '\0';
      if (_len <= root_len || filestream_delete(file) != 0)
         break;
   }

   /* 'file' was truncated: check the game path again */
   return dh_library_path_is_inside(root_norm, path, file, sizeof(file))
       && !path_is_valid(file);
}

size_t dh_library_format_size(uint64_t size, char *s, size_t len)
{
   static const char *units[] = { "B", "KB", "MB", "GB", "TB" };
   double val    = (double)size;
   unsigned unit = 0;
   int written;

   if (!s || len == 0)
      return 0;

   while (val >= 1024.0 && unit + 1 < ARRAY_SIZE(units))
   {
      val /= 1024.0;
      unit++;
   }

   if (unit == 0)
      written = snprintf(s, len, "%u %s", (unsigned)size, units[unit]);
   else
      written = snprintf(s, len, "%.1f %s", val, units[unit]);

   if (written < 0)
   {
      s[0] = '\0';
      return 0;
   }
   return ((size_t)written < len) ? (size_t)written : len - 1;
}

int64_t dh_library_get_free_space(const char *dir)
{
#if defined(DH_LIBRARY_HAVE_FREE_SPACE)
   if (string_is_empty(dir))
      return -1;
#if defined(_WIN32)
   {
      ULARGE_INTEGER avail;
      wchar_t *dir_w = utf8_to_utf16_string_alloc(dir);
      BOOL ret;

      if (!dir_w)
         return -1;
      ret = GetDiskFreeSpaceExW(dir_w, &avail, NULL, NULL);
      free(dir_w);
      if (!ret)
         return -1;
      return (int64_t)avail.QuadPart;
   }
#else
   {
      struct statvfs st;
      if (statvfs(dir, &st) != 0)
         return -1;
      return (int64_t)st.f_bavail * (int64_t)st.f_frsize;
   }
#endif
#else
   return -1;
#endif
}
