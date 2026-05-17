/* oxidize-gtk-css.c
 *
 * A small GIO module for Oxidize GTK CSS live reloading.
 *
 * Warning: This code has been made using the help of AI.
 * However, it is not a cruical part for oxidize to work.
 * It is only for extending some of the limitations of GTK.
 *
 * Stage-one refactor:
 *   - GTK3: simple CSS provider reload.
 *   - GTK4: persistent per-window overlay wrapper + old-texture fade.
 *   - No temporary replacement/restoration of application content during fade.
 *
 * Build:
 *   cc $(pkg-config --cflags glib-2.0 gio-2.0) \
 *      -shared -fPIC -O2 -Wall -Wextra \
 *      -o liboxidize-gtk-css.so oxidize-gtk-css.c \
 *      $(pkg-config --libs glib-2.0 gio-2.0) -ldl
 *
 * Environment:
 *   GTK_OXIDIZE_DEBUG=1
 *   GTK_DISABLE_OXIDIZE_STYLE=1
 *   OXIDIZE_GTK_PRIORITY=application|user
 *   OXIDIZE_TRANSITION_MS=150
 *
 * D-Bus signal:
 *   /org/oxidize/Appearance1 org.oxidize.Appearance1.Changed
 *   args: (t revision, s css_path, s mode)
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define _GNU_SOURCE

#include <dlfcn.h>
#include <gio/gio.h>
#include <gio/giomodule.h>
#include <glib.h>
#include <unistd.h>

#define OX_VERSION "2026-05-17-stage1-refactor"

#define OX_DBUS_OBJECT_PATH "/org/oxidize/Appearance1"
#define OX_DBUS_INTERFACE   "org.oxidize.Appearance1"
#define OX_DBUS_SIGNAL      "Changed"

#define OX_PRIORITY_APPLICATION 600
#define OX_PRIORITY_USER        800

#define OX_DEFAULT_TRANSITION_MS 500
#define OX_MAX_TRANSITION_MS     5000
#define OX_FRAME_MS              16
#define OX_PREPARE_DELAY_MS      48

/* GtkAlign enum value for GTK_ALIGN_FILL. Avoid GTK headers. */
#define OX_GTK_ALIGN_FILL 0

/* Opaque toolkit types. This module is linked only against GLib/GIO. */
typedef void GdkDisplay;
typedef void GdkScreen;
typedef void GtkCssProvider;

typedef struct {
  float x;
  float y;
  float width;
  float height;
} OxGrapheneRect;

/* ──────────────────────────────────────────────────────────────────────────
 * Configuration and logging
 * ────────────────────────────────────────────────────────────────────────── */

static gboolean
ox_debug_enabled (void)
{
  static int cached = -1;

  if (cached < 0)
    cached = g_getenv ("GTK_OXIDIZE_DEBUG") ? 1 : 0;

  return cached != 0;
}

#define OX_LOG(...) \
  do { if (ox_debug_enabled ()) g_message ("Oxidize GIO: " __VA_ARGS__); } while (0)

static guint
ox_css_priority (void)
{
  const char *priority = g_getenv ("OXIDIZE_GTK_PRIORITY");

  if (g_strcmp0 (priority, "application") == 0)
    return OX_PRIORITY_APPLICATION;

  return OX_PRIORITY_USER;
}

static guint
ox_transition_ms (void)
{
  const char *value = g_getenv ("OXIDIZE_TRANSITION_MS");

  if (value && *value)
    {
      guint64 parsed = g_ascii_strtoull (value, NULL, 10);
      if (parsed <= OX_MAX_TRANSITION_MS)
        return (guint) parsed;
    }

  return OX_DEFAULT_TRANSITION_MS;
}

static gboolean
ox_css_path_is_loadable (const char *path)
{
  return path && *path && g_file_test (path, G_FILE_TEST_IS_REGULAR);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Lazily resolved GTK symbols
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct {
  guint        (*gtk_get_major_version)                      (void);
  void *       (*gtk_css_provider_new)                       (void);
  void         (*gtk_css_provider_load_from_path)            (void *, const char *);

  GdkDisplay * (*gdk_display_get_default)                    (void);
  void         (*gtk_style_context_add_provider_for_display) (void *, void *, guint);

  GdkScreen *  (*gdk_screen_get_default)                     (void);
  void         (*gtk_style_context_add_provider_for_screen)  (void *, void *, guint);

  /* Window/content APIs. */
  GList *      (*gtk_window_list_toplevels)                  (void);
  void *       (*gtk_window_get_child)                       (void *);
  void         (*gtk_window_set_child)                       (void *, void *);

  void *       (*adw_application_window_get_content)         (void *);
  void         (*adw_application_window_set_content)         (void *, void *);
  void *       (*adw_window_get_content)                     (void *);
  void         (*adw_window_set_content)                     (void *, void *);
  GType        (*adw_application_window_get_type)            (void);
  GType        (*adw_window_get_type)                        (void);

  /* GTK4 overlay/fade APIs. */
  void *       (*gtk_overlay_new)                            (void);
  void         (*gtk_overlay_set_child)                      (void *, void *);
  void         (*gtk_overlay_add_overlay)                    (void *, void *);
  void         (*gtk_overlay_remove_overlay)                 (void *, void *);
  void *       (*gtk_picture_new_for_paintable)              (void *);

  void         (*gtk_widget_set_opacity)                     (void *, double);
  void         (*gtk_widget_queue_draw)                      (void *);
  void         (*gtk_widget_set_visible)                     (void *, gboolean);
  void         (*gtk_widget_set_hexpand)                     (void *, gboolean);
  void         (*gtk_widget_set_vexpand)                     (void *, gboolean);
  void         (*gtk_widget_set_halign)                      (void *, int);
  void         (*gtk_widget_set_valign)                      (void *, int);
  void         (*gtk_widget_set_size_request)                (void *, int, int);
  void         (*gtk_widget_set_can_target)                  (void *, gboolean);
  int          (*gtk_widget_get_width)                       (void *);
  int          (*gtk_widget_get_height)                      (void *);
  void *       (*gtk_widget_get_native)                      (void *);
  void *       (*gtk_native_get_renderer)                    (void *);
  void *       (*gtk_widget_paintable_new)                   (void *);

  /* Snapshot/texture APIs. */
  void *       (*gtk_snapshot_new)                           (void);
  void *       (*gtk_snapshot_to_node)                       (void *);
  void         (*gdk_paintable_snapshot)                     (void *, void *, double, double);
  void *       (*gsk_renderer_render_texture)                (void *, void *, const void *);
  void         (*gsk_render_node_unref)                      (void *);
  void *       (*graphene_rect_init)                         (void *, float, float, float, float);
} OxGtk;

static OxGtk ox;
static guint ox_gtk_version = 0;
static GOnce ox_gtk_once = G_ONCE_INIT;
static gboolean ox_gtk4_syms_loaded = FALSE;

#define OX_LOAD(sym) ox.sym = dlsym (RTLD_DEFAULT, #sym)

static gpointer
ox_load_core_symbols_once (gpointer data)
{
  (void) data;

  OX_LOAD (gtk_get_major_version);
  OX_LOAD (gtk_css_provider_new);
  OX_LOAD (gtk_css_provider_load_from_path);
  OX_LOAD (gdk_display_get_default);
  OX_LOAD (gtk_style_context_add_provider_for_display);
  OX_LOAD (gdk_screen_get_default);
  OX_LOAD (gtk_style_context_add_provider_for_screen);

  if (ox.gtk_get_major_version)
    ox_gtk_version = ox.gtk_get_major_version ();

  return NULL;
}

static gboolean
ox_load_core_symbols (void)
{
  g_once (&ox_gtk_once, ox_load_core_symbols_once, NULL);

  return (ox_gtk_version == 3 || ox_gtk_version == 4) &&
         ox.gtk_css_provider_new &&
         ox.gtk_css_provider_load_from_path;
}

static void
ox_load_gtk4_symbols (void)
{
  if (ox_gtk4_syms_loaded)
    return;

  ox_gtk4_syms_loaded = TRUE;

  OX_LOAD (gtk_window_list_toplevels);
  OX_LOAD (gtk_window_get_child);
  OX_LOAD (gtk_window_set_child);

  OX_LOAD (adw_application_window_get_content);
  OX_LOAD (adw_application_window_set_content);
  OX_LOAD (adw_window_get_content);
  OX_LOAD (adw_window_set_content);
  OX_LOAD (adw_application_window_get_type);
  OX_LOAD (adw_window_get_type);

  OX_LOAD (gtk_overlay_new);
  OX_LOAD (gtk_overlay_set_child);
  OX_LOAD (gtk_overlay_add_overlay);
  OX_LOAD (gtk_overlay_remove_overlay);
  OX_LOAD (gtk_picture_new_for_paintable);

  OX_LOAD (gtk_widget_set_opacity);
  OX_LOAD (gtk_widget_queue_draw);
  OX_LOAD (gtk_widget_set_visible);
  OX_LOAD (gtk_widget_set_hexpand);
  OX_LOAD (gtk_widget_set_vexpand);
  OX_LOAD (gtk_widget_set_halign);
  OX_LOAD (gtk_widget_set_valign);
  OX_LOAD (gtk_widget_set_size_request);
  OX_LOAD (gtk_widget_set_can_target);
  OX_LOAD (gtk_widget_get_width);
  OX_LOAD (gtk_widget_get_height);
  OX_LOAD (gtk_widget_get_native);
  OX_LOAD (gtk_native_get_renderer);
  OX_LOAD (gtk_widget_paintable_new);

  OX_LOAD (gtk_snapshot_new);
  OX_LOAD (gtk_snapshot_to_node);
  OX_LOAD (gdk_paintable_snapshot);
  OX_LOAD (gsk_renderer_render_texture);
  OX_LOAD (gsk_render_node_unref);
  OX_LOAD (graphene_rect_init);
}

static gboolean
ox_gtk4_transition_supported (void)
{
  ox_load_gtk4_symbols ();

  return ox.gtk_window_list_toplevels &&
         ox.gtk_overlay_new &&
         ox.gtk_overlay_set_child &&
         ox.gtk_overlay_add_overlay &&
         ox.gtk_overlay_remove_overlay &&
         ox.gtk_picture_new_for_paintable &&
         ox.gtk_widget_set_opacity &&
         ox.gtk_widget_queue_draw &&
         ox.gtk_widget_get_width &&
         ox.gtk_widget_get_height &&
         ox.gtk_widget_get_native &&
         ox.gtk_native_get_renderer &&
         ox.gtk_widget_paintable_new &&
         ox.gtk_snapshot_new &&
         ox.gtk_snapshot_to_node &&
         ox.gdk_paintable_snapshot &&
         ox.gsk_renderer_render_texture &&
         ox.gsk_render_node_unref &&
         ox.graphene_rect_init;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Style state and CSS reload
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct {
  GtkCssProvider  *provider;
  GDBusConnection *bus;
  GWeakRef         owner_ref;
  char            *css_path;
  guint            signal_id;
  guint            priority;
  gboolean         attached;
} OxStyle;

static GQuark ox_style_quark = 0;

static char *
ox_default_css_path (void)
{
  const char *subdir = ox_gtk_version == 3 ? "gtk-3.0" : "gtk-4.0";

  if (g_file_test ("/.flatpak-info", G_FILE_TEST_EXISTS))
    return g_build_filename (g_get_home_dir (), ".config", subdir, "gtk.css", NULL);

  return g_build_filename (g_get_user_config_dir (), subdir, "gtk.css", NULL);
}

static void
ox_style_free (gpointer data)
{
  OxStyle *style = data;

  if (style->bus && style->signal_id)
    g_dbus_connection_signal_unsubscribe (style->bus, style->signal_id);

  g_clear_object (&style->bus);
  g_weak_ref_clear (&style->owner_ref);

  if (style->provider)
    g_object_unref (style->provider);

  g_free (style->css_path);
  g_free (style);
}

static OxStyle *
ox_style_for_owner (GObject *owner)
{
  OxStyle *style;

  if (G_UNLIKELY (ox_style_quark == 0))
    ox_style_quark = g_quark_from_static_string ("oxidize-style");

  style = g_object_get_qdata (owner, ox_style_quark);
  if (style)
    return style;

  style = g_new0 (OxStyle, 1);
  style->provider = ox.gtk_css_provider_new ();
  style->css_path = ox_default_css_path ();
  style->priority = ox_css_priority ();
  g_weak_ref_init (&style->owner_ref, owner);

  g_object_set_qdata_full (owner, ox_style_quark, style, ox_style_free);
  return style;
}

static void
ox_style_set_path (OxStyle *style, const char *css_path)
{
  if (!style || !css_path || !*css_path)
    return;

  if (g_strcmp0 (style->css_path, css_path) == 0)
    return;

  g_free (style->css_path);
  style->css_path = g_strdup (css_path);
}

static void
ox_style_reload (OxStyle *style)
{
  if (!style || !ox_css_path_is_loadable (style->css_path))
    {
      OX_LOG ("CSS file not loadable: %s", style && style->css_path ? style->css_path : "");
      return;
    }

  ox.gtk_css_provider_load_from_path (style->provider, style->css_path);
  OX_LOG ("loaded CSS: %s", style->css_path);
}

/* ──────────────────────────────────────────────────────────────────────────
 * GTK4 window wrapper
 *
 * Invariant:
 *   Animated GTK4 windows keep their real application child inside a persistent
 *   GtkOverlay. A theme transition only adds/removes a fading old screenshot on
 *   top of this overlay. It never temporarily removes and restores the app's
 *   real widget tree during the fade.
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct {
  void *overlay;
} OxWindow;

static GQuark ox_window_quark = 0;

static GQuark
ox_window_state_quark (void)
{
  if (G_UNLIKELY (ox_window_quark == 0))
    ox_window_quark = g_quark_from_static_string ("oxidize-window-wrapper");

  return ox_window_quark;
}

static void
ox_window_free (gpointer data)
{
  OxWindow *window = data;

  if (window->overlay)
    g_object_unref (window->overlay);

  g_free (window);
}

static void
ox_widget_fill (void *widget)
{
  if (!widget)
    return;

  if (ox.gtk_widget_set_visible)
    ox.gtk_widget_set_visible (widget, TRUE);
  if (ox.gtk_widget_set_hexpand)
    ox.gtk_widget_set_hexpand (widget, TRUE);
  if (ox.gtk_widget_set_vexpand)
    ox.gtk_widget_set_vexpand (widget, TRUE);
  if (ox.gtk_widget_set_halign)
    ox.gtk_widget_set_halign (widget, OX_GTK_ALIGN_FILL);
  if (ox.gtk_widget_set_valign)
    ox.gtk_widget_set_valign (widget, OX_GTK_ALIGN_FILL);
}

static void *
ox_window_get_content (void *window)
{
  if (!window)
    return NULL;

  if (ox.adw_application_window_get_type &&
      ox.adw_application_window_get_content &&
      G_TYPE_CHECK_INSTANCE_TYPE (window, ox.adw_application_window_get_type ()))
    return ox.adw_application_window_get_content (window);

  if (ox.adw_window_get_type &&
      ox.adw_window_get_content &&
      G_TYPE_CHECK_INSTANCE_TYPE (window, ox.adw_window_get_type ()))
    return ox.adw_window_get_content (window);

  return ox.gtk_window_get_child ? ox.gtk_window_get_child (window) : NULL;
}

static void
ox_window_set_content (void *window, void *child)
{
  if (!window)
    return;

  if (ox.adw_application_window_get_type &&
      ox.adw_application_window_set_content &&
      G_TYPE_CHECK_INSTANCE_TYPE (window, ox.adw_application_window_get_type ()))
    {
      ox.adw_application_window_set_content (window, child);
      return;
    }

  if (ox.adw_window_get_type &&
      ox.adw_window_set_content &&
      G_TYPE_CHECK_INSTANCE_TYPE (window, ox.adw_window_get_type ()))
    {
      ox.adw_window_set_content (window, child);
      return;
    }

  if (ox.gtk_window_set_child)
    ox.gtk_window_set_child (window, child);
}

static OxWindow *
ox_window_get_existing (void *gtk_window)
{
  OxWindow *state = g_object_get_qdata (G_OBJECT (gtk_window), ox_window_state_quark ());

  if (!state)
    return NULL;

  if (ox_window_get_content (gtk_window) == state->overlay)
    return state;

  OX_LOG ("transition: stale wrapper on window %p; dropping state", gtk_window);
  g_object_set_qdata_full (G_OBJECT (gtk_window), ox_window_state_quark (), NULL, NULL);
  return NULL;
}

static OxWindow *
ox_window_ensure_wrapped (void *gtk_window)
{
  OxWindow *state = ox_window_get_existing (gtk_window);
  void *child;
  void *overlay;

  if (state)
    return state;

  child = ox_window_get_content (gtk_window);
  if (!child)
    return NULL;

  overlay = ox.gtk_overlay_new ();
  if (!overlay)
    return NULL;

  ox_widget_fill (overlay);
  ox_widget_fill (child);

  g_object_ref (child);
  ox_window_set_content (gtk_window, NULL);
  ox.gtk_overlay_set_child (overlay, child);
  ox_window_set_content (gtk_window, overlay);
  g_object_unref (child);

  ox.gtk_widget_queue_draw (child);
  ox.gtk_widget_queue_draw (overlay);

  state = g_new0 (OxWindow, 1);
  state->overlay = g_object_ref_sink (overlay);

  g_object_set_qdata_full (G_OBJECT (gtk_window),
                           ox_window_state_quark (),
                           state,
                           ox_window_free);

  OX_LOG ("transition: installed persistent wrapper on window %p", gtk_window);
  return state;
}

static guint
ox_window_prepare_all (void)
{
  guint count = 0;
  GList *windows = ox.gtk_window_list_toplevels ();

  for (GList *l = windows; l; l = l->next)
    if (ox_window_ensure_wrapped (l->data))
      count++;

  g_list_free (windows);
  return count;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Snapshot capture and fade animation
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct {
  OxWindow *window;
  void     *texture;
  int       width;
  int       height;
} OxCapture;

typedef struct {
  void *overlay;
  void *picture;
  void *texture;
} OxFadeItem;

typedef struct {
  GSList *items;
  guint   timeout_id;
  guint   duration_ms;
  gint64  start_us;
} OxTransition;

typedef struct {
  OxStyle *style;
  guint    source_id;
} OxPendingTransition;

static OxTransition *ox_active_transition = NULL;
static OxPendingTransition *ox_pending_transition = NULL;

static void *
ox_capture_texture (void *gtk_window)
{
  int width = ox.gtk_widget_get_width (gtk_window);
  int height = ox.gtk_widget_get_height (gtk_window);
  void *native;
  void *renderer;
  void *paintable;
  void *snapshot;
  void *node;
  void *texture = NULL;
  OxGrapheneRect bounds;

  if (width <= 0 || height <= 0)
    {
      OX_LOG ("transition: cannot capture window %p with size %dx%d", gtk_window, width, height);
      return NULL;
    }

  native = ox.gtk_widget_get_native (gtk_window);
  renderer = native ? ox.gtk_native_get_renderer (native) : NULL;
  if (!renderer)
    return NULL;

  paintable = ox.gtk_widget_paintable_new (gtk_window);
  snapshot = paintable ? ox.gtk_snapshot_new () : NULL;
  if (!snapshot)
    {
      if (paintable)
        g_object_unref (paintable);
      return NULL;
    }

  ox.gdk_paintable_snapshot (paintable, snapshot, (double) width, (double) height);
  node = ox.gtk_snapshot_to_node (snapshot);

  if (node)
    {
      ox.graphene_rect_init (&bounds, 0.0f, 0.0f, (float) width, (float) height);
      texture = ox.gsk_renderer_render_texture (renderer, node, &bounds);
      ox.gsk_render_node_unref (node);
    }

  g_object_unref (snapshot);
  g_object_unref (paintable);

  return texture;
}

static void
ox_capture_free (OxCapture *capture)
{
  if (!capture)
    return;

  if (capture->texture)
    g_object_unref (capture->texture);

  g_free (capture);
}

static GSList *
ox_capture_all (void)
{
  GSList *captures = NULL;
  GList *windows = ox.gtk_window_list_toplevels ();

  for (GList *l = windows; l; l = l->next)
    {
      void *gtk_window = l->data;
      OxWindow *state = ox_window_get_existing (gtk_window);
      void *texture;
      OxCapture *capture;

      if (!state)
        continue;

      texture = ox_capture_texture (gtk_window);
      if (!texture)
        continue;

      capture = g_new0 (OxCapture, 1);
      capture->window = state;
      capture->texture = texture;
      capture->width = ox.gtk_widget_get_width (gtk_window);
      capture->height = ox.gtk_widget_get_height (gtk_window);

      captures = g_slist_prepend (captures, capture);
    }

  g_list_free (windows);
  return captures;
}

static void
ox_capture_list_free (GSList *captures)
{
  for (GSList *l = captures; l; l = l->next)
    ox_capture_free (l->data);

  g_slist_free (captures);
}

static OxFadeItem *
ox_fade_item_new (OxCapture *capture)
{
  OxFadeItem *item;
  void *picture;

  if (!capture || !capture->window || !capture->window->overlay || !capture->texture)
    return NULL;

  picture = ox.gtk_picture_new_for_paintable (capture->texture);
  if (!picture)
    return NULL;

  ox_widget_fill (picture);

  if (ox.gtk_widget_set_size_request)
    ox.gtk_widget_set_size_request (picture, capture->width, capture->height);
  if (ox.gtk_widget_set_can_target)
    ox.gtk_widget_set_can_target (picture, FALSE);

  ox.gtk_widget_set_opacity (picture, 1.0);
  ox.gtk_overlay_add_overlay (capture->window->overlay, picture);
  ox.gtk_widget_queue_draw (capture->window->overlay);

  item = g_new0 (OxFadeItem, 1);
  item->overlay = g_object_ref (capture->window->overlay);
  item->picture = g_object_ref_sink (picture);
  item->texture = g_object_ref (capture->texture);

  return item;
}

static void
ox_fade_item_free (OxFadeItem *item)
{
  if (!item)
    return;

  if (item->overlay && item->picture)
    ox.gtk_overlay_remove_overlay (item->overlay, item->picture);
  if (item->picture)
    g_object_unref (item->picture);
  if (item->texture)
    g_object_unref (item->texture);
  if (item->overlay)
    g_object_unref (item->overlay);

  g_free (item);
}

static void
ox_transition_free (OxTransition *transition)
{
  if (!transition)
    return;

  if (transition->timeout_id)
    g_source_remove (transition->timeout_id);

  for (GSList *l = transition->items; l; l = l->next)
    ox_fade_item_free (l->data);

  g_slist_free (transition->items);

  if (ox_active_transition == transition)
    ox_active_transition = NULL;

  g_free (transition);
}

static void
ox_transition_cancel (void)
{
  if (!ox_active_transition)
    return;

  OX_LOG ("transition: cancelling active fade");
  ox_transition_free (ox_active_transition);
}

static void
ox_transition_set_opacity (OxTransition *transition, double opacity)
{
  if (opacity < 0.0)
    opacity = 0.0;
  else if (opacity > 1.0)
    opacity = 1.0;

  for (GSList *l = transition->items; l; l = l->next)
    {
      OxFadeItem *item = l->data;
      ox.gtk_widget_set_opacity (item->picture, opacity);
    }
}

static gboolean
ox_transition_tick (gpointer user_data)
{
  OxTransition *transition = user_data;
  gint64 elapsed_us;
  double progress;

  if (transition != ox_active_transition)
    return G_SOURCE_REMOVE;

  elapsed_us = g_get_monotonic_time () - transition->start_us;
  if (elapsed_us < 0)
    elapsed_us = 0;

  progress = (double) elapsed_us / ((double) transition->duration_ms * 1000.0);

  if (progress >= 1.0)
    {
      transition->timeout_id = 0;
      ox_transition_set_opacity (transition, 0.0);
      OX_LOG ("transition: fade complete");
      ox_transition_free (transition);
      return G_SOURCE_REMOVE;
    }

  ox_transition_set_opacity (transition, 1.0 - progress);
  return G_SOURCE_CONTINUE;
}

static void
ox_transition_start (GSList *items, guint duration_ms)
{
  OxTransition *transition;

  if (!items)
    return;

  transition = g_new0 (OxTransition, 1);
  transition->items = items;
  transition->duration_ms = MAX (1, duration_ms);
  transition->start_us = g_get_monotonic_time () + (OX_FRAME_MS * 1000);

  ox_active_transition = transition;
  ox_transition_set_opacity (transition, 1.0);
  transition->timeout_id = g_timeout_add (OX_FRAME_MS, ox_transition_tick, transition);

  OX_LOG ("transition: fading %u window(s) over %ums",
          g_slist_length (items), transition->duration_ms);
}

static void
ox_pending_free (OxPendingTransition *pending)
{
  if (!pending)
    return;

  if (pending->source_id)
    g_source_remove (pending->source_id);

  if (ox_pending_transition == pending)
    ox_pending_transition = NULL;

  g_free (pending);
}

static void
ox_pending_cancel (void)
{
  if (ox_pending_transition)
    ox_pending_free (ox_pending_transition);
}

static gboolean
ox_transition_prepare_cb (gpointer user_data)
{
  OxPendingTransition *pending = user_data;
  OxStyle *style = pending->style;
  GSList *captures;
  GSList *items = NULL;

  pending->source_id = 0;
  if (ox_pending_transition == pending)
    ox_pending_transition = NULL;

  if (!style || !ox_css_path_is_loadable (style->css_path))
    {
      ox_pending_free (pending);
      return G_SOURCE_REMOVE;
    }

  captures = ox_capture_all ();
  if (!captures)
    {
      OX_LOG ("transition: no captures; falling back to direct reload");
      ox_style_reload (style);
      ox_pending_free (pending);
      return G_SOURCE_REMOVE;
    }

  /* Add old-theme pictures before CSS reload to avoid a one-frame flash. */
  for (GSList *l = captures; l; l = l->next)
    {
      OxFadeItem *item = ox_fade_item_new (l->data);
      if (item)
        items = g_slist_prepend (items, item);
    }

  ox_style_reload (style);

  if (items)
    ox_transition_start (items, ox_transition_ms ());
  else
    OX_LOG ("transition: no fade items after capture");

  ox_capture_list_free (captures);
  ox_pending_free (pending);
  return G_SOURCE_REMOVE;
}

static void
ox_style_reload_animated (OxStyle *style)
{
  guint prepared;
  OxPendingTransition *pending;

  if (!style || !ox_css_path_is_loadable (style->css_path))
    {
      ox_style_reload (style);
      return;
    }

  if (ox_transition_ms () == 0 || !ox_gtk4_transition_supported ())
    {
      ox_style_reload (style);
      return;
    }

  ox_pending_cancel ();
  ox_transition_cancel ();

  prepared = ox_window_prepare_all ();
  if (prepared == 0)
    {
      OX_LOG ("transition: no windows prepared; falling back to direct reload");
      ox_style_reload (style);
      return;
    }

  pending = g_new0 (OxPendingTransition, 1);
  pending->style = style;
  pending->source_id = g_timeout_add (OX_PREPARE_DELAY_MS,
                                      ox_transition_prepare_cb,
                                      pending);
  ox_pending_transition = pending;

  OX_LOG ("transition: prepared %u window(s); capturing in %ums",
          prepared, OX_PREPARE_DELAY_MS);
}

/* ──────────────────────────────────────────────────────────────────────────
 * D-Bus signal handling
 * ────────────────────────────────────────────────────────────────────────── */

static void
ox_on_changed (GDBusConnection *connection,
               const gchar     *sender_name,
               const gchar     *object_path,
               const gchar     *interface_name,
               const gchar     *signal_name,
               GVariant        *parameters,
               gpointer         user_data)
{
  OxStyle *style = user_data;
  GObject *owner;
  guint64 revision = 0;
  char *css_path = NULL;
  char *mode = NULL;

  (void) connection;
  (void) sender_name;
  (void) object_path;
  (void) interface_name;
  (void) signal_name;

  owner = g_weak_ref_get (&style->owner_ref);
  if (!owner)
    {
      OX_LOG ("owner gone; ignoring signal");
      return;
    }
  g_object_unref (owner);

  g_variant_get (parameters, "(tss)", &revision, &css_path, &mode);

  OX_LOG ("signal revision=%" G_GUINT64_FORMAT " path=%s mode=%s",
          revision, css_path ? css_path : "", mode ? mode : "");

  ox_style_set_path (style, css_path);

  if (ox_gtk_version == 4)
    ox_style_reload_animated (style);
  else
    ox_style_reload (style);

  g_free (css_path);
  g_free (mode);
}

static void
ox_on_bus_ready (GObject *source, GAsyncResult *result, gpointer user_data)
{
  OxStyle *style = user_data;
  GDBusConnection *bus;
  GObject *owner;
  GError *error = NULL;

  (void) source;

  owner = g_weak_ref_get (&style->owner_ref);
  if (!owner)
    {
      g_bus_get_finish (result, NULL);
      OX_LOG ("owner gone before D-Bus ready");
      return;
    }
  g_object_unref (owner);

  bus = g_bus_get_finish (result, &error);
  if (!bus)
    {
      OX_LOG ("could not connect to session bus: %s", error ? error->message : "unknown");
      g_clear_error (&error);
      return;
    }

  style->bus = bus;
  style->signal_id = g_dbus_connection_signal_subscribe (
      style->bus,
      NULL,
      OX_DBUS_INTERFACE,
      OX_DBUS_SIGNAL,
      OX_DBUS_OBJECT_PATH,
      NULL,
      G_DBUS_SIGNAL_FLAGS_NONE,
      ox_on_changed,
      style,
      NULL);

  OX_LOG ("subscribed to %s.%s signal id=%u",
          OX_DBUS_INTERFACE, OX_DBUS_SIGNAL, style->signal_id);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Attach to GTK
 * ────────────────────────────────────────────────────────────────────────── */

static void ox_attach (void);

static void
ox_attach (void)
{
  OxStyle *style;
  GObject *owner;

  if (g_getenv ("GTK_DISABLE_OXIDIZE_STYLE"))
    {
      OX_LOG ("disabled by GTK_DISABLE_OXIDIZE_STYLE");
      return;
    }

  if (ox_gtk_version == 4)
    {
      GdkDisplay *display;

      if (!ox.gdk_display_get_default || !ox.gtk_style_context_add_provider_for_display)
        return;

      display = ox.gdk_display_get_default ();
      if (!display)
        return;

      owner = G_OBJECT (display);
      style = ox_style_for_owner (owner);

      if (style->attached)
        return;

      style->attached = TRUE;
      OX_LOG ("attaching GTK4 priority=%u path=%s", style->priority, style->css_path);

      ox.gtk_style_context_add_provider_for_display (display, style->provider, style->priority);
    }
  else if (ox_gtk_version == 3)
    {
      GdkScreen *screen;

      if (!ox.gdk_screen_get_default || !ox.gtk_style_context_add_provider_for_screen)
        return;

      screen = ox.gdk_screen_get_default ();
      if (!screen)
        return;

      owner = G_OBJECT (screen);
      style = ox_style_for_owner (owner);

      if (style->attached)
        return;

      style->attached = TRUE;
      OX_LOG ("attaching GTK3 priority=%u path=%s", style->priority, style->css_path);

      ox.gtk_style_context_add_provider_for_screen (screen, style->provider, style->priority);
    }
  else
    {
      return;
    }

  ox_style_reload (style);
  g_bus_get (G_BUS_TYPE_SESSION, NULL, ox_on_bus_ready, style);
}

static void
ox_on_display_opened (void *manager, void *display, gpointer user_data)
{
  (void) manager;
  (void) display;
  (void) user_data;

  ox_attach ();
}

static gboolean
ox_idle_attach (gpointer user_data)
{
  GdkDisplay *display = NULL;

  (void) user_data;

  if (!ox_load_core_symbols ())
    {
      OX_LOG ("no GTK symbols; skipping");
      return G_SOURCE_REMOVE;
    }

  if (ox.gdk_display_get_default)
    display = ox.gdk_display_get_default ();

  OX_LOG ("idle fired version=%s GTK%u display=%p", OX_VERSION, ox_gtk_version, display);

  if (display)
    {
      ox_attach ();
      return G_SOURCE_REMOVE;
    }

  void *(*gdk_display_manager_get) (void) = dlsym (RTLD_DEFAULT, "gdk_display_manager_get");
  if (gdk_display_manager_get)
    {
      void *manager = gdk_display_manager_get ();
      if (manager)
        {
          g_signal_connect (manager,
                            "display-opened",
                            G_CALLBACK (ox_on_display_opened),
                            NULL);
          OX_LOG ("waiting for display-opened signal");
        }
    }

  return G_SOURCE_REMOVE;
}

/* ──────────────────────────────────────────────────────────────────────────
 * GIO module entry points
 * ────────────────────────────────────────────────────────────────────────── */

G_MODULE_EXPORT void
g_io_module_load (GIOModule *module)
{
  if (g_getenv ("GTK_DISABLE_OXIDIZE_STYLE"))
    return;

  g_type_module_use (G_TYPE_MODULE (module));

  OX_LOG ("module loaded pid=%d version=%s", (int) getpid (), OX_VERSION);
  g_idle_add (ox_idle_attach, NULL);
}

G_MODULE_EXPORT void
g_io_module_unload (GIOModule *module)
{
  (void) module;

  ox_pending_cancel ();
  ox_transition_cancel ();

  OX_LOG ("module unloaded version=%s", OX_VERSION);
}

G_MODULE_EXPORT char **
g_io_module_query (void)
{
  static const char *features[] = { NULL };
  return (char **) features;
}
