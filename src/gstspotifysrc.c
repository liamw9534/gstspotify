/*
 * Copyright (C) 2007 David Schleef <ds@schleef.org>
 *           (C) 2008 Wim Taymans <wim.taymans@gmail.com>
 *           (C) 2015 Canonical <michal.karnicki@canonical.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/audio/audio.h>
#include <gst/base/gstbasesrc.h>
#include <libspotify/api.h>

#include <string.h>
#include <unistd.h>
#include <stdio.h>

#include "gstspotifysrc.h"

typedef struct _GstSpotifySessionContext
{
  GCond          cond;
  GThread        *thread;
  GMutex         mutex;
  sp_session     *session;
  gboolean       destroy;
  gboolean       logged_in;
  gboolean       logged_out;
  gboolean       play_token_lost;
  gboolean       end_of_track;
  sp_error       connection_error;
  sp_error       streaming_error;
} GstSpotifySessionContext;

struct _GstSpotifySrcPrivate
{
  GCond        cond;
  GMutex       mutex;
  GQueue       *queue;

  GstCaps   *caps;
  gint64    size;
  guint64   max_bytes;
  gchar     *user;
  gchar     *pass;
  gchar     *uri;
  gchar     *appkey_file;

  gboolean flushing;
  gboolean started;
  gboolean is_eos;
  gboolean is_first_seek;
  guint64  queued_bytes;
  guint64  stutter;
  GstClockTime buffer_timestamp;

  GstSpotifySessionContext *spotify_context;

  GDestroyNotify notify;
};

GST_DEBUG_CATEGORY_STATIC (spotify_src_debug);
#define GST_CAT_DEFAULT spotify_src_debug

#define DEFAULT_PROP_MAX_BYTES     1000000
#define DEFAULT_PROP_USER          g_getenv("SPOTIFY_USER")
#define DEFAULT_PROP_PASS          g_getenv("SPOTIFY_PASS")
#define DEFAULT_PROP_APPKEY_FILE   g_getenv("SPOTIFY_APPKEY")
#define DEFAULT_PROP_URI           \
	"spotify://spotify:track:27jdUE1EYDSXZqhjuNxLem"

enum
{
  PROP_0,
  PROP_USER,
  PROP_PASS,
  PROP_APPKEY_FILE,
  PROP_URI,
  PROP_LAST
};

/*
 * Spotify library does not provide user data with callbacks, so we
 * have to store our context globally.
 */
static GstSpotifySrc *g_spotifysrc = NULL;

static GstStaticPadTemplate gst_spotify_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-raw, "
            // This line will most probably break source hilighting :/
            "format = (string) " GST_AUDIO_NE(S16) ", "
            "endianness = (int) { 1234 }, "
            "signed = (boolean) { TRUE }, "
            "width = (int) 16, "
            "depth = (int) 16, "
            "rate = (int) 44100, "
            "channels = (int) 2")
    );

static void gst_spotify_src_uri_handler_init (gpointer g_iface,
    gpointer iface_data);

static void gst_spotify_src_dispose (GObject * object);
static void gst_spotify_src_finalize (GObject * object);

static void gst_spotify_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_spotify_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static GstFlowReturn gst_spotify_src_create (GstBaseSrc * bsrc,
    guint64 offset, guint size, GstBuffer ** buf);
static gboolean gst_spotify_src_start (GstBaseSrc * bsrc);
static gboolean gst_spotify_src_stop (GstBaseSrc * bsrc);
static gboolean gst_spotify_src_unlock (GstBaseSrc * bsrc);
static gboolean gst_spotify_src_unlock_stop (GstBaseSrc * bsrc);
static gboolean gst_spotify_src_do_seek (GstBaseSrc * src, GstSegment * segment);
static gboolean gst_spotify_src_is_seekable (GstBaseSrc * src);
static gboolean gst_spotify_src_do_get_size (GstBaseSrc * src, guint64 * size);
static gboolean gst_spotify_src_query (GstBaseSrc * src, GstQuery * query);
static gboolean
gst_spotify_src_set_uri (GstSpotifySrc *spotifysrc, const gchar *uri);

static void
gst_spotify_src_end_of_stream (GstSpotifySrc * spotifysrc);

/* Spotify API calls */
static gboolean spotify_create(char *appkey_file);
static gboolean spotify_destroy(GstSpotifySessionContext *context);
static gboolean spotify_login(GstSpotifySessionContext *context,
                              const char *user,
                              const char *password);
static gboolean spotify_seek(GstSpotifySessionContext *context, int offset);
static gboolean spotify_play(GstSpotifySessionContext *context, const char *link);
static gboolean spotify_stop(GstSpotifySessionContext *context);

#define parent_class gst_spotify_src_parent_class
G_DEFINE_TYPE_EXTENDED (GstSpotifySrc, gst_spotify_src, GST_TYPE_BASE_SRC, 0,
    G_IMPLEMENT_INTERFACE(GST_TYPE_URI_HANDLER, gst_spotify_src_uri_handler_init));

static void
gst_spotify_src_class_init (GstSpotifySrcClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstBaseSrcClass *basesrc_class = (GstBaseSrcClass *) klass;
  GstElementClass *element_class = (GstElementClass *) klass;

  gobject_class->dispose = gst_spotify_src_dispose;
  gobject_class->finalize = gst_spotify_src_finalize;

  gobject_class->set_property = gst_spotify_src_set_property;
  gobject_class->get_property = gst_spotify_src_get_property;

  g_object_class_install_property (gobject_class, PROP_USER,
      g_param_spec_string ("user", "Username", "Username for premium Spotify account",
          DEFAULT_PROP_USER, G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_PASS,
      g_param_spec_string ("pass", "Password", "Password for premium Spotify account",
	      DEFAULT_PROP_PASS, G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_URI,
      g_param_spec_string ("uri", "URI", "A Spotify track URI",
          DEFAULT_PROP_URI, G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_APPKEY_FILE,
      g_param_spec_string ("spotifykeyfile", "Spotify app key File", "Path to spotify key file",
          DEFAULT_PROP_APPKEY_FILE, G_PARAM_READWRITE));

  basesrc_class->create = gst_spotify_src_create;
  basesrc_class->start = gst_spotify_src_start;
  basesrc_class->stop = gst_spotify_src_stop;
  basesrc_class->unlock = gst_spotify_src_unlock;
  basesrc_class->unlock_stop = gst_spotify_src_unlock_stop;
  basesrc_class->do_seek = gst_spotify_src_do_seek;
  basesrc_class->is_seekable = gst_spotify_src_is_seekable;
  basesrc_class->get_size = gst_spotify_src_do_get_size;
  basesrc_class->query = gst_spotify_src_query;

  gst_element_class_add_pad_template (element_class,
    gst_static_pad_template_get (&gst_spotify_src_template));

  gst_element_class_set_metadata (
    element_class,
    "Spotify audio source",
    "Source/Audio/Network/Protocol",
    "Feed Spotify hosted music to a pipeline",
    "Liam Wickins <liam9534@gmail.com>");

  GST_DEBUG_CATEGORY_INIT (spotify_src_debug, "spotify", 0, "spotifysrc element");

  g_type_class_add_private (klass, sizeof (GstSpotifySrcPrivate));
}

static void
gst_spotify_src_init (GstSpotifySrc * spotifysrc)
{
  GstSpotifySrcPrivate *priv;

  priv = spotifysrc->priv = G_TYPE_INSTANCE_GET_PRIVATE (
        spotifysrc, GST_TYPE_SPOTIFY_SRC, GstSpotifySrcPrivate);

  priv->queue = g_queue_new ();

  priv->max_bytes = DEFAULT_PROP_MAX_BYTES;
  priv->size = -1;
  priv->spotify_context = NULL;
  priv->user = g_strdup(DEFAULT_PROP_USER);
  priv->pass = g_strdup(DEFAULT_PROP_PASS);
  priv->uri = g_strdup(DEFAULT_PROP_URI);
  priv->appkey_file = g_strdup(DEFAULT_PROP_APPKEY_FILE);
  priv->caps = NULL; /* FIXME: Do we need to set this? */

  /* Set global context for spotify session to use */
  g_spotifysrc = spotifysrc;

  gst_base_src_set_live (GST_BASE_SRC (spotifysrc), FALSE);
}

static void
gst_spotify_src_flush_queued (GstSpotifySrc * src)
{
  GstBuffer *buf;
  GstSpotifySrcPrivate *priv = src->priv;

  while ((buf = g_queue_pop_head (priv->queue)))
    gst_buffer_unref (buf);
  priv->queued_bytes = 0;
}

static void
gst_spotify_src_dispose (GObject * obj)
{
  GstSpotifySrc *spotifysrc = GST_SPOTIFY_SRC_CAST (obj);
  GstSpotifySrcPrivate *priv = spotifysrc->priv;

  if (priv->caps) {
    gst_caps_unref (priv->caps);
    priv->caps = NULL;
  }
  gst_spotify_src_flush_queued (spotifysrc);

  G_OBJECT_CLASS (parent_class)->dispose (obj);
}

static void
gst_spotify_src_finalize (GObject * obj)
{
  GstSpotifySrc *spotifysrc = GST_SPOTIFY_SRC_CAST (obj);
  GstSpotifySrcPrivate *priv = spotifysrc->priv;

  spotify_destroy(priv->spotify_context);
  g_free (priv->user);
  g_free (priv->pass);
  g_free (priv->appkey_file);
  g_free (priv->uri);
  g_queue_free (priv->queue);

  G_OBJECT_CLASS (parent_class)->finalize (obj);
}

static void
gst_spotify_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstSpotifySrc *spotifysrc = GST_SPOTIFY_SRC_CAST (object);
  GstSpotifySrcPrivate *priv = spotifysrc->priv;

  switch (prop_id) {
    case PROP_USER:
      g_free(priv->user);
      priv->user = g_value_dup_string(value);
      break;
    case PROP_PASS:
        g_free(priv->pass);
        priv->pass = g_value_dup_string(value);
      break;
    case PROP_APPKEY_FILE:
        g_free(priv->appkey_file);
        priv->appkey_file = g_value_dup_string(value);
      break;
    case PROP_URI:
      gst_spotify_src_set_uri(spotifysrc, g_value_get_string(value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_spotify_src_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstSpotifySrc *spotifysrc = GST_SPOTIFY_SRC_CAST (object);
  GstSpotifySrcPrivate *priv = spotifysrc->priv;

  switch (prop_id) {
  case PROP_USER:
    g_value_set_string(value, priv->user);
    break;
  case PROP_PASS:
	g_value_set_string(value, priv->pass);
    break;
  case PROP_APPKEY_FILE:
	g_value_set_string(value, priv->appkey_file);
    break;
  case PROP_URI:
	g_value_set_string(value, priv->uri);
    break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_spotify_src_unlock (GstBaseSrc * bsrc)
{
  GstSpotifySrc *spotifysrc = GST_SPOTIFY_SRC_CAST (bsrc);
  GstSpotifySrcPrivate *priv = spotifysrc->priv;

  g_mutex_lock(&priv->mutex);
  GST_DEBUG_OBJECT (spotifysrc, "unlock start");
  priv->flushing = TRUE;
  g_cond_broadcast(&priv->cond);
  g_mutex_unlock(&priv->mutex);

  return TRUE;
}

static gboolean
gst_spotify_src_unlock_stop (GstBaseSrc * bsrc)
{
  GstSpotifySrc *spotifysrc = GST_SPOTIFY_SRC_CAST (bsrc);
  GstSpotifySrcPrivate *priv = spotifysrc->priv;

  g_mutex_lock(&priv->mutex);
  GST_DEBUG_OBJECT (spotifysrc, "unlock stop");
  priv->flushing = FALSE;
  g_cond_broadcast(&priv->cond);
  g_mutex_unlock(&priv->mutex);

  return TRUE;
}

static gboolean
gst_spotify_src_start (GstBaseSrc * bsrc)
{
  GstSpotifySrc *spotifysrc = GST_SPOTIFY_SRC_CAST (bsrc);
  GstSpotifySrcPrivate *priv = spotifysrc->priv;
  gboolean has_created, has_logged_in, has_started;

  g_mutex_lock(&priv->mutex);
  GST_DEBUG_OBJECT (spotifysrc, "starting");
  priv->is_first_seek = TRUE;
  priv->flushing = FALSE;
  priv->stutter = 0;
  priv->buffer_timestamp = 0;

  has_created = has_logged_in = has_started = FALSE;
  if (!(has_created = spotify_create(priv->appkey_file)) ||
      !(has_logged_in = spotify_login(priv->spotify_context, priv->user, priv->pass)) ||
      !(has_started = spotify_play(priv->spotify_context, gst_uri_get_location(priv->uri))))
  {
    if (!has_created) GST_DEBUG_OBJECT(spotifysrc, "Could not instantiate Spotify");
    if (!has_logged_in) GST_DEBUG_OBJECT(spotifysrc, "Could not log in to Spotify");
    if (!has_started) GST_DEBUG_OBJECT(spotifysrc, "Could not play track URI");

    g_mutex_unlock(&priv->mutex);
    return FALSE;
  }

  priv->started = TRUE;
  g_mutex_unlock(&priv->mutex);

  gst_base_src_set_format (bsrc, GST_FORMAT_TIME);

  return TRUE;
}

static gboolean
gst_spotify_src_stop (GstBaseSrc * bsrc)
{
  GstSpotifySrc *spotifysrc = GST_SPOTIFY_SRC_CAST (bsrc);
  GstSpotifySrcPrivate *priv = spotifysrc->priv;

  g_mutex_lock(&priv->mutex);
  GST_DEBUG_OBJECT (spotifysrc, "stopping");
  priv->is_eos = FALSE;
  priv->flushing = TRUE;

  spotify_stop(priv->spotify_context);
  gst_spotify_src_flush_queued (spotifysrc);
  spotify_destroy(priv->spotify_context);

  priv->started = FALSE;
  g_mutex_unlock(&priv->mutex);

  return TRUE;
}

static gboolean
gst_spotify_src_is_seekable (GstBaseSrc * src)
{
  return TRUE;
}

static gboolean
gst_spotify_src_do_get_size (GstBaseSrc * src, guint64 * size)
{
  GstSpotifySrc *spotifysrc = GST_SPOTIFY_SRC_CAST (src);

  /* This track size is loaded upon a play request and stored
   * in priv->size
   */
  *size = spotifysrc->priv->size;

  return TRUE;
}

static gboolean
gst_spotify_src_query (GstBaseSrc * src, GstQuery * query)
{
  gboolean res;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_LATENCY:
    {
      GstClockTime min, max;
      gboolean live;

      /* Query the parent class for the defaults */
      res = gst_base_src_query_latency (src, &live, &min, &max);

      /* FIXME: Can we get spotify latency? */
      gst_query_set_latency (query, live, min, max);
      break;
    }
    case GST_QUERY_SCHEDULING:
    {
      gst_query_add_scheduling_mode (query, GST_PAD_MODE_PUSH);
      res = TRUE;
      break;
    }
    case GST_QUERY_CAPS:
    {
      GstCaps *caps;

      caps = gst_static_pad_template_get_caps (&gst_spotify_src_template);
      gst_query_set_caps_result (query, caps);
      gst_caps_unref (caps);
      res = TRUE;
      break;
    }
    default:
      res = GST_BASE_SRC_CLASS (parent_class)->query (src, query);
      break;
  }

  return res;
}

/* will be called in push mode */
static gboolean
gst_spotify_src_do_seek (GstBaseSrc * src, GstSegment * segment)
{
  GstSpotifySrc *spotifysrc = GST_SPOTIFY_SRC_CAST (src);
  GstSpotifySrcPrivate *priv = spotifysrc->priv;
  gint64 desired_position;
  gboolean res = FALSE;

  desired_position = segment->position;

  /* This is to workaround a bug in spotify since we can't allow
   * an initial seek to position zero until the decoder has sent
   * data.  Not doing so will result in a run-time "Decode error 11"
   */
  if (priv->is_first_seek && desired_position == 0) {
    priv->is_first_seek = FALSE;
    return TRUE;
  }

  /* Avoid seeking again if we are already at the desired position */
  if (priv->buffer_timestamp == desired_position)
  {
    GST_DEBUG_OBJECT (spotifysrc, "already at position %" G_GINT64_FORMAT,
                      desired_position);
    return TRUE;
  }

  GST_DEBUG_OBJECT (spotifysrc, "seeking to %" G_GINT64_FORMAT ", format %s",
      desired_position, gst_format_get_name (segment->format));

  res = spotify_seek(priv->spotify_context, (desired_position / GST_MSECOND));

  if (res) {
    g_mutex_lock(&priv->mutex);
    GST_DEBUG_OBJECT (spotifysrc, "flushing queue");
    gst_spotify_src_flush_queued (spotifysrc);
    priv->is_eos = FALSE;
    priv->buffer_timestamp = desired_position;
    GST_DEBUG_OBJECT (spotifysrc, "Waiting for seek to arrive...");
    g_cond_wait(&priv->cond, &priv->mutex);
    GST_DEBUG_OBJECT (spotifysrc, "Seek has arrived...");
    g_mutex_unlock(&priv->mutex);
  } else {
    GST_WARNING_OBJECT (spotifysrc, "seek failed");
  }

  return res;
}

static GstFlowReturn
gst_spotify_src_create (GstBaseSrc * bsrc, guint64 offset, guint size,
    GstBuffer ** buf)
{
  GstSpotifySrc *spotifysrc = GST_SPOTIFY_SRC_CAST (bsrc);
  GstSpotifySrcPrivate *priv = spotifysrc->priv;
  GstFlowReturn ret;
  GstCaps *caps;

  GST_OBJECT_LOCK (spotifysrc);
  caps = priv->caps ? gst_caps_ref (priv->caps) : NULL;
  if (G_UNLIKELY (priv->size != bsrc->segment.duration &&
          bsrc->segment.format == GST_FORMAT_TIME)) {
    GST_DEBUG_OBJECT (spotifysrc,
        "Size changed from %" G_GINT64_FORMAT " to %" G_GINT64_FORMAT,
        bsrc->segment.duration, priv->size);
    bsrc->segment.duration = priv->size;
    GST_OBJECT_UNLOCK (spotifysrc);

    gst_element_post_message (GST_ELEMENT (spotifysrc),
        gst_message_new_duration_changed (GST_OBJECT (spotifysrc)));
  } else {
    GST_OBJECT_UNLOCK (spotifysrc);
  }

  g_mutex_lock(&priv->mutex);
  /* check flushing first */
  if (G_UNLIKELY (priv->flushing))
    goto flushing;

  while (TRUE) {
    /* return data as long as we have some */
    if (!g_queue_is_empty (priv->queue)) {
      guint buf_size;

      *buf = g_queue_pop_head (priv->queue);
      buf_size = gst_buffer_get_size (*buf);

      GST_DEBUG_OBJECT (spotifysrc, "we have buffer %p of size %u", *buf, buf_size);

      priv->queued_bytes -= buf_size;

      ret = GST_FLOW_OK;
      break;
    } else {

      /* we can be flushing now because we released the lock above */
      if (G_UNLIKELY (priv->flushing))
        goto flushing;

      /* if we have a buffer now, continue the loop and try to return it. In
       * random-access mode (where a buffer is normally pushed in the above
       * signal) we can still be empty because the pushed buffer got flushed or
       * when the application pushes the requested buffer later, we support both
       * possibilities. */
      if (!g_queue_is_empty (priv->queue))
        continue;

      /* no buffer yet, maybe we are EOS, if not, block for more data. */
    }

    /* check EOS */
    if (G_UNLIKELY (priv->is_eos))
      goto eos;

    /* nothing to return, wait a while for new data or flushing. */
    GST_DEBUG_OBJECT (spotifysrc, "Waiting for data...");
    priv->stutter++;
    g_cond_wait(&priv->cond, &priv->mutex);
  }

  g_mutex_unlock(&priv->mutex);
  if (caps)
    gst_caps_unref (caps);
  return ret;

  /* ERRORS */
flushing:
  {
    GST_DEBUG_OBJECT (spotifysrc, "we are flushing");
    g_mutex_unlock(&priv->mutex);
    if (caps)
      gst_caps_unref (caps);
    return GST_FLOW_FLUSHING;
  }
eos:
  {
    GST_DEBUG_OBJECT (spotifysrc, "we are EOS");
    g_mutex_unlock(&priv->mutex);
    if (caps)
      gst_caps_unref (caps);
    return GST_FLOW_EOS;
  }
}

static guint gst_spotify_src_alloc_and_queue(GstSpotifySrc * spotifysrc,
                                             guint num_frames,
                                             const void * data_frames,
                                             guint data_size)
{
  GstSpotifySrcPrivate *priv;

  priv = spotifysrc->priv;

  g_mutex_lock(&priv->mutex);

  /* can't accept buffers when we are flushing or EOS */
  if (priv->flushing)
    goto flushing;

  if (priv->is_eos)
    goto eos;

  if (priv->max_bytes && priv->queued_bytes >= priv->max_bytes) {
    GST_DEBUG_OBJECT (spotifysrc,
        "queue filled (%" G_GUINT64_FORMAT " >= %" G_GUINT64_FORMAT ")",
        priv->queued_bytes, priv->max_bytes);
    g_mutex_unlock(&priv->mutex);
    return 0;
  }

  /* Allocate buffer and copy spotify data into buffer */
  GstBuffer *buffer = gst_buffer_new_allocate (NULL, data_size, NULL);
  if (!buffer)
  {
      GST_DEBUG_OBJECT (spotifysrc, "gst_buffer allocation failed");
      g_mutex_unlock(&priv->mutex);
      return 0;
  }

  GstMapInfo info;
  gst_buffer_map(buffer, &info, GST_MAP_WRITE);
  memcpy (info.data, (guint8 *)data_frames, info.size);
  gst_buffer_unmap(buffer, &info);

  GstClockTime duration = gst_util_uint64_scale(num_frames, GST_SECOND, 44100);
  GST_BUFFER_DURATION(buffer) = duration;
  GST_BUFFER_TIMESTAMP(buffer) = priv->buffer_timestamp;

  GST_DEBUG_OBJECT (spotifysrc, "queueing buffer %p", buffer);
  gst_buffer_ref (buffer);
  g_queue_push_tail (priv->queue, buffer);
  priv->queued_bytes += gst_buffer_get_size (buffer);
  GST_DEBUG_OBJECT (spotifysrc,
                    "queued bytes = %" G_GUINT64_FORMAT " ts = %" G_GUINT64_FORMAT,
                    priv->queued_bytes, priv->buffer_timestamp);
  priv->buffer_timestamp += duration;
  g_cond_broadcast(&priv->cond);
  g_mutex_unlock(&priv->mutex);

  return num_frames;

  /* ERRORS */
flushing:
  {
    GST_DEBUG_OBJECT (spotifysrc, "refuse music data, we are flushing");
    g_mutex_unlock(&priv->mutex);
    return num_frames;
  }
eos:
  {
    GST_DEBUG_OBJECT (spotifysrc, "refuse music data, we are EOS");
    g_mutex_unlock(&priv->mutex);
    return num_frames;
  }
}

static void gst_spotify_src_end_of_stream (GstSpotifySrc * spotifysrc)
{
  GstSpotifySrcPrivate *priv;

  priv = spotifysrc->priv;

  g_mutex_lock(&priv->mutex);
  /* can't accept buffers when we are flushing. We can accept them when we are
   * EOS although it will not do anything. */
  if (priv->flushing)
    goto flushing;

  GST_DEBUG_OBJECT (spotifysrc, "sending EOS");
  priv->is_eos = TRUE;
  g_cond_broadcast(&priv->cond);
  g_mutex_unlock(&priv->mutex);

  return;

  /* ERRORS */
flushing:
  {
    g_mutex_unlock(&priv->mutex);
    GST_DEBUG_OBJECT (spotifysrc, "refuse EOS, we are flushing");
  }
}

/*** GSTURIHANDLER INTERFACE *************************************************/

static gboolean
gst_spotify_src_set_uri (GstSpotifySrc *spotifysrc, const gchar *uri)
{
  GstState state;
  gchar *protocol = NULL;
  gchar *location;

  /* the element must be stopped in order to do this */
  state = GST_STATE (spotifysrc);
  if (state != GST_STATE_READY && state != GST_STATE_NULL) {
    GST_WARNING_OBJECT (spotifysrc, "Setting spotify uri in wrong state");
    goto wrong_state;
  }

  if (!gst_uri_is_valid (uri)) {
    GST_WARNING_OBJECT (spotifysrc, "Invalid URI '%s'", uri);
    goto invalid_uri;
  }

  protocol = gst_uri_get_protocol (uri);
  if (strcmp (protocol, "spotify") != 0) {
     GST_WARNING_OBJECT (spotifysrc, "Setting spotify uri with wrong protocol");
     goto wrong_protocol;
  }
  g_free (protocol);

  location = gst_uri_get_location (uri);
  if (!location) {
    GST_WARNING_OBJECT (spotifysrc, "Setting spotify uri with wrong/no location");
    goto wrong_location;
  }

  g_free (spotifysrc->priv->uri );

  spotifysrc->priv->uri = g_strdup(uri);

  g_object_notify (G_OBJECT (spotifysrc), "uri");

  return TRUE;

  /* ERROR */
invalid_uri:
wrong_protocol:
  g_free (protocol);
wrong_state:
wrong_location:
  return FALSE;
}

static GstURIType
gst_spotify_src_uri_get_type (GType type)
{
  return GST_URI_SRC;
}

static const gchar * const *
gst_spotify_src_uri_get_protocols (GType type)
{
  static const gchar * const protocols[] = { (const char *) "spotify", NULL };

  return protocols;
}

static gchar *
gst_spotify_src_uri_get_uri (GstURIHandler * handler)
{
  GstSpotifySrc *spotifysrc = GST_SPOTIFY_SRC (handler);
  return spotifysrc->priv->uri;
}

static gboolean
gst_spotify_src_uri_set_uri (GstURIHandler * handler, const gchar * uri, GError ** error)
{
  gboolean res = TRUE;
  GstSpotifySrc *spotifysrc = GST_SPOTIFY_SRC (handler);

  GST_DEBUG_OBJECT (spotifysrc, "New URI for interface: '%s' for spotify src", uri);
  if (!gst_spotify_src_set_uri (spotifysrc, uri)) {
    *error = g_error_new (
      g_quark_from_string("gst-resource-error-quark"),
      GST_RESOURCE_ERROR_FAILED,
      "Failed to set source URI: '%s'", uri);
      res = FALSE;
  }
  return res;
}

static void
gst_spotify_src_uri_handler_init (gpointer g_iface, gpointer iface_data)
{
  GstURIHandlerInterface *iface = (GstURIHandlerInterface *) g_iface;

  iface->get_type = gst_spotify_src_uri_get_type;
  iface->get_protocols = gst_spotify_src_uri_get_protocols;
  iface->get_uri = gst_spotify_src_uri_get_uri;
  iface->set_uri = gst_spotify_src_uri_set_uri;
}

/**** SPOTIFY interface  **************************************************/

#define SPOTIFY_CONTEXT(src) src->priv->spotify_context

static void spotify_main_loop(GstSpotifySessionContext *context)
{
  g_mutex_lock(&context->mutex);
  while (!context->destroy) {
    gint64 end_time;
    int timeout;

    if (sp_session_process_events(context->session, &timeout) == SP_ERROR_OK) {
      GST_DEBUG_OBJECT(g_spotifysrc, "process events next timeout = %d", timeout);
    } else {
    	timeout = 1000;
    }

    end_time = g_get_monotonic_time () + 1 * G_TIME_SPAN_SECOND;
    g_cond_wait_until(&context->cond, &context->mutex, end_time);
  }
  g_mutex_unlock(&context->mutex);
}

static void spotify_loop(void)
{
  GstSpotifySessionContext *context = SPOTIFY_CONTEXT(g_spotifysrc);
  int timeout;
  sp_session_process_events(context->session, &timeout);
  GST_DEBUG_OBJECT (g_spotifysrc, "process events next timeout = %d", timeout);
}

static gboolean spotify_login(GstSpotifySessionContext *context,
                              const char *user,
                              const char *password)
{
  context->logged_in = FALSE;
  GST_DEBUG_OBJECT (g_spotifysrc, "attempting to login");
  g_mutex_lock(&context->mutex);
  sp_error ret = sp_session_login(context->session, user, password,
                                  FALSE, NULL);

  if (ret == SP_ERROR_OK) {
    guint cnt = 10;
    while (!context->logged_in && cnt-- > 0) {
      spotify_loop();
      usleep(200000); // 200000 * 10 = 2 seconds
    }

    if (!context->logged_in)
      goto loginfailed;
    g_mutex_unlock(&context->mutex);
    return TRUE;
  }

loginfailed:
  GST_DEBUG_OBJECT (g_spotifysrc, "unable to login - error = %d", ret);
  g_mutex_unlock(&context->mutex);
  return FALSE;
}

static gboolean spotify_seek(GstSpotifySessionContext *context, int offset)
{
  GST_DEBUG_OBJECT (g_spotifysrc, "attempting to seek - offset = %d", offset);
  g_mutex_lock(&context->mutex);
  sp_error ret = sp_session_player_seek(context->session, offset);
  if (ret != SP_ERROR_OK) {
    GST_DEBUG_OBJECT (g_spotifysrc, "unable to seek - error = %d", ret);
    g_mutex_unlock(&context->mutex);
    return FALSE;
  }
  g_mutex_unlock(&context->mutex);
  return TRUE;
}

static gboolean spotify_play(GstSpotifySessionContext *context, const char *link)
{
  GST_DEBUG_OBJECT (g_spotifysrc, "attempting to load link = %s", link);

  g_mutex_lock(&context->mutex);
  sp_link *spl = sp_link_create_from_string(link);
  if (!spl)
  {
    GST_DEBUG_OBJECT (g_spotifysrc, "could not create link for %s", link);
    g_mutex_unlock(&context->mutex);
    return FALSE;
  }

  sp_track *spt = sp_link_as_track(spl);
  if (!spt)
  {
    GST_DEBUG_OBJECT (g_spotifysrc, "could not find track for %s", link);
    sp_link_release(spl);
    g_mutex_unlock(&context->mutex);
    return FALSE;
  }

  /* Increment ref counts */
  sp_track_add_ref(spt);
  sp_link_add_ref(spl);

  /* Busy wait for track to load */
  GST_DEBUG_OBJECT (g_spotifysrc, "waiting for track to load...");
  guint cnt = 20;
  while (!sp_track_is_loaded(spt) && cnt-- > 0) {
    spotify_loop();
    usleep(200000); // 200000 * 20 = 4 seconds
  }

  if (!sp_track_is_loaded(spt)) {
    GST_DEBUG_OBJECT (g_spotifysrc, "track loading timed out");
    sp_track_release(spt);
    sp_link_release(spl);
    g_mutex_unlock(&context->mutex);
    return FALSE;
  }

  GST_DEBUG_OBJECT (g_spotifysrc, "track is loaded");

  sp_error ret = sp_session_player_load(context->session, spt);
  if (ret != SP_ERROR_OK) {
    GST_DEBUG_OBJECT (g_spotifysrc, "player could not load track - error = %d", ret);
    sp_track_release(spt);
    sp_link_release(spl);
    g_mutex_unlock(&context->mutex);
    return FALSE;
  }

  /* Update track duration */
  g_spotifysrc->priv->size = sp_track_duration(spt) * GST_MSECOND;

  ret = sp_session_player_play(context->session, TRUE);
  if (ret != SP_ERROR_OK) {
    GST_DEBUG_OBJECT (g_spotifysrc, "player could not play - error = %d", ret);
    sp_track_release(spt);
    sp_link_release(spl);
    g_mutex_unlock(&context->mutex);
    return FALSE;
  }

  g_mutex_unlock(&context->mutex);
  return TRUE;
}

static gboolean spotify_stop(GstSpotifySessionContext *context)
{
  GST_DEBUG_OBJECT (g_spotifysrc, "attempting to stop player");
  g_mutex_lock(&context->mutex);
  sp_error ret = sp_session_player_play(context->session, FALSE);
  if (ret != SP_ERROR_OK) {
    GST_DEBUG_OBJECT (g_spotifysrc, "unable to stop player - error = %d", ret);
    g_mutex_unlock(&context->mutex);
    return FALSE;
  }

  ret = sp_session_player_unload(context->session);
  if (ret != SP_ERROR_OK) {
    GST_DEBUG_OBJECT (g_spotifysrc, "unable to unload player - error = %d", ret);
    g_mutex_unlock(&context->mutex);
    return FALSE;
  }

  /* Reset total track size */
  g_spotifysrc->priv->size = -1;

  g_mutex_unlock(&context->mutex);
  return TRUE;
}

static void spotify_logged_in_cb(sp_session *session, sp_error error)
{
  GstSpotifySessionContext *context = SPOTIFY_CONTEXT(g_spotifysrc);
  GST_DEBUG_OBJECT (g_spotifysrc, "logged in with response = %d", error);
  if (error == SP_ERROR_OK)
    context->logged_in = TRUE;
  else
    context->logged_in = FALSE;

    // This is absolutely necessary here, see: http://goo.gl/78xPQh
    sp_session_playlistcontainer(session);
}

static void spotify_logged_out_cb(sp_session *session)
{
  GstSpotifySessionContext *context = SPOTIFY_CONTEXT(g_spotifysrc);
  GST_DEBUG_OBJECT (g_spotifysrc, "logged out");
  context->logged_in = FALSE;
}

static void spotify_connection_error_cb(sp_session *session, sp_error error)
{
  GstSpotifySessionContext *context = SPOTIFY_CONTEXT(g_spotifysrc);
  GST_DEBUG_OBJECT (g_spotifysrc, "connection error - error = %d", error);
  context->connection_error = error;
}

static void spotify_message_to_user_cb(sp_session *session, const char *msg)
{
  GST_DEBUG_OBJECT (g_spotifysrc, "user message = %s", msg);
}

static void spotify_metadata_updated_cb(sp_session *session)
{
  GST_DEBUG_OBJECT (g_spotifysrc, "metadata updated");
}

static void spotify_notify_main_thread_cb(sp_session *session)
{
  GstSpotifySessionContext *context = SPOTIFY_CONTEXT(g_spotifysrc);
  GST_DEBUG_OBJECT (g_spotifysrc, "notify main thread");
  g_cond_signal(&context->cond);
}

static int spotify_music_delivery_cb(sp_session *session,
		                             const sp_audioformat *format,
		                             const void *frames, int num_frames)
{
  guint bufsize = num_frames * sizeof(int16_t) * format->channels;

  GST_DEBUG_OBJECT (g_spotifysrc, "music delivery - bufsize = %d "
		            "rate = %d channels = %d num_frames = %d",
		            bufsize, format->sample_rate, format->channels, num_frames);
  if (num_frames == 0) {
    return 0; /* Seeking */
  }

  return gst_spotify_src_alloc_and_queue(g_spotifysrc, num_frames, frames, bufsize);
}

static void spotify_play_token_lost_cb(sp_session *session)
{
  GstSpotifySessionContext *context = SPOTIFY_CONTEXT(g_spotifysrc);
  GST_DEBUG_OBJECT (g_spotifysrc, "play token has been lost");
  context->play_token_lost = TRUE;
}

static void spotify_log_message_cb(sp_session *session, const char *msg)
{
  GST_DEBUG_OBJECT (g_spotifysrc, "log message = %s", msg);
}

static void spotify_end_of_track_cb(sp_session *session)
{
  GST_DEBUG_OBJECT (g_spotifysrc, "end of track");
  gst_spotify_src_end_of_stream(g_spotifysrc);
}

static void spotify_streaming_error_cb(sp_session *session, sp_error error)
{
  GstSpotifySessionContext *context = SPOTIFY_CONTEXT(g_spotifysrc);
  GST_DEBUG_OBJECT (g_spotifysrc, "streaming error with code = %d", error);
  context->streaming_error = error;
}

static void spotify_get_audio_buffer_stats_cb(sp_session *session, sp_audio_buffer_stats *stats)
{
  stats->stutter = g_spotifysrc->priv->stutter;
  stats->samples = g_spotifysrc->priv->queued_bytes / (2*sizeof(gint16));
  GST_DEBUG_OBJECT (g_spotifysrc, "indicating audio buffer stats - stutter = %d samples = %d",
                    stats->stutter, stats->samples);
}

static void spotify_userinfo_updated_cb(sp_session *session)
{
  GST_DEBUG_OBJECT (g_spotifysrc, "userinfo updated");
}

static gboolean spotify_create(char *appkey_file)
{
  sp_session_callbacks callbacks = {
    &spotify_logged_in_cb,
    &spotify_logged_out_cb,
    &spotify_metadata_updated_cb,
    &spotify_connection_error_cb,
    &spotify_message_to_user_cb,
    &spotify_notify_main_thread_cb,
    &spotify_music_delivery_cb,
    &spotify_play_token_lost_cb,
    &spotify_log_message_cb,
    &spotify_end_of_track_cb,
    &spotify_streaming_error_cb,
    &spotify_userinfo_updated_cb,
    NULL,
    NULL,
    &spotify_get_audio_buffer_stats_cb,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
  };
  GstSpotifySessionContext *context;
  sp_session_config config;

  if (g_spotifysrc->priv->spotify_context != NULL)
  {
    GST_DEBUG_OBJECT (g_spotifysrc, "spotify session already created");
    return TRUE;
  }

  static uint8_t appkey[321];
  static const size_t appkey_size = sizeof(appkey);
  FILE *keyfile;
  size_t sz;

  GST_DEBUG_OBJECT (g_spotifysrc, "creating spotify session");

  keyfile = fopen(appkey_file, "r");
  if (keyfile == NULL)
    return FALSE;

  /* FIXME: error check */
  sz = fread(appkey, sizeof(uint8_t), appkey_size, keyfile);
  fclose(keyfile);

  if (sz != appkey_size)
    return FALSE;

  context = g_new0(GstSpotifySessionContext, 1);
  if (context == NULL)
	  return FALSE;

  memset(context, 0, sizeof(*context));

  memset(&config, 0, sizeof(config));
  config.application_key = appkey;
  config.application_key_size = appkey_size;
  config.api_version = SPOTIFY_API_VERSION;
  /* FIXME: check if these paths are appropiate */
  config.cache_location = "/tmp";
  config.settings_location = "/tmp";
  config.user_agent = "libgstspotify";
  config.callbacks = &callbacks;
  config.compress_playlists = FALSE;
  config.dont_save_metadata_for_playlists = FALSE;

  /* Once we create the session, we may get callbacks */
  g_spotifysrc->priv->spotify_context = context;

  sp_error ret = sp_session_create(&config, &context->session);
  if (ret == SP_ERROR_OK)
  {
    context->thread = g_thread_new ("spotify-thread", (GThreadFunc)spotify_main_loop, context);
    if (context->thread == NULL) {
       GST_DEBUG_OBJECT (g_spotifysrc, "g_thread_create failed!");
       sp_session_release(context->session);
       goto fail;
    }

    return TRUE;
  }

  GST_DEBUG_OBJECT (g_spotifysrc, "unable to create session - error = %d", ret);

fail:
  g_spotifysrc->priv->spotify_context = NULL;
  g_free(context);
  return FALSE;
}

static gboolean spotify_destroy(GstSpotifySessionContext *context)
{
  if (context == NULL)
  {
    GST_DEBUG_OBJECT (g_spotifysrc, "spotify session already destroyed");
    return TRUE;
  }

  GST_DEBUG_OBJECT (g_spotifysrc, "now destroying spotify session");
  g_mutex_lock(&context->mutex);
  context->destroy = TRUE;
  g_cond_signal(&context->cond);
  g_mutex_unlock(&context->mutex);
  g_thread_join(context->thread);
  sp_error ret = sp_session_release(context->session);
  g_free(context);
  g_spotifysrc->priv->spotify_context = NULL;
  if (ret != SP_ERROR_OK) {
    GST_DEBUG_OBJECT (g_spotifysrc, "failed to release session - error = %d", ret);
    return FALSE;
  }

  return TRUE;
}
