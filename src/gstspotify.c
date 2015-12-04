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
/**
 * SECTION:element-spotifysrc
 *
 * The spotifysrc element can be used by applications to insert spotify hosted
 * music into a GStreamer pipeline.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>

#include "gstspotifysrc.h"

static gboolean
plugin_init (GstPlugin * plugin)
{
  gst_element_register (plugin, "spotifysrc", GST_RANK_NONE, GST_TYPE_SPOTIFY_SRC);

  return TRUE;
}

GST_PLUGIN_DEFINE (
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    spotify,
    "Element used to provide audio music hosted by spotify",
    plugin_init, VERSION, "LGPL", "GStreamer", "http://gstreamer.net/")
