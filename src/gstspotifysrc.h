/* GStreamer
 * Copyright (C) 2007 David Schleef <ds@schleef.org>
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

#ifndef _GST_SPOTIFY_SRC_H_
#define _GST_SPOTIFY_SRC_H_

#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>

G_BEGIN_DECLS

#define GST_TYPE_SPOTIFY_SRC \
  gst_spotify_src_get_type()
#define GST_SPOTIFY_SRC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj),GST_TYPE_SPOTIFY_SRC,GstSpotifySrc))

/* Since 0.10.23 */
#define GST_SPOTIFY_SRC_CAST(obj) \
  ((GstSpotifySrc*)(obj))

typedef struct _GstSpotifySrc GstSpotifySrc;
typedef struct _GstSpotifySrcClass GstSpotifySrcClass;
typedef struct _GstSpotifySrcPrivate GstSpotifySrcPrivate;

struct _GstSpotifySrc
{
  GstBaseSrc basesrc;

  /*< private >*/
  GstSpotifySrcPrivate *priv;

  /*< private >*/
  gpointer     _gst_reserved[GST_PADDING];
};

struct _GstSpotifySrcClass
{
  GstBaseSrcClass basesrc_class;

  /*< private >*/
  gpointer     _gst_reserved[GST_PADDING];
};

GType gst_spotify_src_get_type(void);

G_END_DECLS

#endif
