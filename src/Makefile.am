# Note: plugindir is set in configure

plugin_LTLIBRARIES = libgstspotify.la

# sources used to compile this plug-in
libgstspotify_la_SOURCES = gstspotify.c gstspotifysrc.c gstspotifysrc.h

# compiler and linker flags used to compile this plugin, set in configure.ac
libgstspotify_la_CFLAGS = $(GST_CFLAGS)
libgstspotify_la_LIBADD = $(GST_LIBS)
libgstspotify_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS) -lspotify -lspotifywrapper
libgstspotify_la_LIBTOOLFLAGS = --tag=disable-static

# headers we need but don't want installed
noinst_HEADERS = gstspotifysrc.h
