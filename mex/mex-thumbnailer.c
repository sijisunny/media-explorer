/*
 * Mex - a media explorer
 *
 * Copyright © 2010, 2011 Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU Lesser General Public License,
 * version 2.1, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses>
 */

#include <config.h>
#include <gio/gio.h>
#include <dbus/dbus-glib.h>
#include "mex-thumbnailer.h"
#include "mex-marshal.h"

static DBusGProxy *thumb_proxy = NULL;
/* Hash of integer handles to callback data */
static GHashTable *pending = NULL;

typedef struct {
  MexThumbnailCallback callback;
  gpointer user_data;
} ClosureData;

static void
on_ready (DBusGProxy *proxy, guint handle, char **uris)
{
  ClosureData *data;

  data = g_hash_table_lookup (pending, GUINT_TO_POINTER (handle));
  if (data == NULL)
    return;

  /* Assumption is that its one URI per handle */
  if (data->callback)
    data->callback (uris[0], data->user_data);
  g_free (data);
}

static void
on_error (DBusGProxy *proxy, guint handle, char **failed_uris, int error_code, const char *error_message)
{
  ClosureData *data;

  g_message ("Cannot generate thumbnail for %s: %s",
             failed_uris[0], error_message);

  data = g_hash_table_lookup (pending, GUINT_TO_POINTER (handle));
  g_free (data);
}

static void
mex_thumbnailer_init (void)
{
  DBusGConnection *connection;

  if (thumb_proxy)
    return;

  pending = g_hash_table_new (NULL, NULL);

  connection = dbus_g_bus_get (DBUS_BUS_SESSION, NULL);
  g_assert (connection); /* TODO: error */

  thumb_proxy = dbus_g_proxy_new_for_name (connection,
                                     "org.freedesktop.thumbnails.Thumbnailer1",
                                     "/org/freedesktop/thumbnails/Thumbnailer1",
                                     "org.freedesktop.thumbnails.Thumbnailer1");

  dbus_g_object_register_marshaller (mex_marshal_VOID__UINT_STRING,
                                     G_TYPE_NONE,
                                     G_TYPE_UINT,
                                     G_TYPE_STRV,
                                     G_TYPE_INVALID);
  dbus_g_proxy_add_signal (thumb_proxy, "Ready",
                           G_TYPE_UINT,
                           G_TYPE_STRV,
                           G_TYPE_INVALID);

  dbus_g_object_register_marshaller (mex_marshal_VOID__UINT_POINTER_INT_STRING,
                                     G_TYPE_NONE,
                                     G_TYPE_UINT,
                                     G_TYPE_STRV,
                                     G_TYPE_INT,
                                     G_TYPE_STRING,
                                     G_TYPE_INVALID);
  dbus_g_proxy_add_signal (thumb_proxy, "Error",
                           G_TYPE_UINT,
                           G_TYPE_STRV,
                           G_TYPE_INT,
                           G_TYPE_STRING,
                           G_TYPE_INVALID);

  dbus_g_proxy_connect_signal (thumb_proxy, "Ready", G_CALLBACK (on_ready), NULL, NULL);
  dbus_g_proxy_connect_signal (thumb_proxy, "Error", G_CALLBACK (on_error), NULL, NULL);
}

static char *
get_mime_type (const char *uri)
{
  GFile *file;
  GFileInfo *info;
  GError *error = NULL;
  char *mime;

  g_assert (uri);

  file = g_file_new_for_uri (uri);
  info = g_file_query_info (file,
                            G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE,
                            G_FILE_QUERY_INFO_NONE,
                            NULL, &error);
  if (error) {
    g_message ("Cannot query MIME type for %s: %s", uri, error->message);
    g_object_unref (file);
    return NULL;
  }

  mime = g_strdup (g_file_info_get_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE));
  g_object_unref (info);

  return mime;
}

static void
on_queue (DBusGProxy *proxy, DBusGProxyCall *call, void *user_data)
{
  ClosureData *data = user_data;
  GError *error = NULL;
  guint handle = 0;

  if (dbus_g_proxy_end_call (proxy, call, &error, G_TYPE_UINT, &handle, G_TYPE_INVALID)) {
    g_hash_table_insert (pending, GUINT_TO_POINTER (handle), data);
  } else {
    g_message ("Cannot queue thumbnails: %s", error->message);
    g_error_free (error);
    g_free (data);
  }
}

/**
 * mex_thumbnailer_generate:
 * @url: the URL to thumbnail
 * @callback: function to callback when thumbnailing is successfull
 * @user_data: data to pass to @callback
 *
 * Attempt to generate a thumbnail for @url asynchronously.  When thumbnailing
 * is complete, @callback is called.  If thumbnailing isn't possible or fails no
 * callback will be made, so it is recommended to set a fallback image before
 * calling this function.
 */
void
mex_thumbnailer_generate (const char *url, MexThumbnailCallback callback, gpointer user_data)
{
  char *uris[2], *mimes[2];
  ClosureData *data;

  /* TODO: internal queue? */

  mex_thumbnailer_init ();

  /* We can assign @url because DBusGProxy will copy, so just free mimes[0]
     later */
  uris[0] = (char *)url;
  mimes[0] = get_mime_type (url);
  uris[1] = mimes[1] = NULL;

  data = g_new0 (ClosureData, 1);
  data->callback = callback;
  data->user_data = user_data;

  dbus_g_proxy_begin_call (thumb_proxy, "Queue",
                           on_queue, data, NULL,
                           G_TYPE_STRV, uris,
                           G_TYPE_STRV, mimes,
                           G_TYPE_STRING, "x-huge",
                           G_TYPE_STRING, "default",
                           G_TYPE_UINT, 0,
                           G_TYPE_INVALID,
                           G_TYPE_INVALID);

  g_free (mimes[0]);
}