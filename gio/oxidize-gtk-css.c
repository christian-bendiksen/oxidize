/* oxidize-gtk-css.c
 *
 * GIO module for Oxidize giving live GTK CSS reloading.
 *
 * Loaded automatically by every GLib-based application via GIO_EXTRA_MODULES.
 * Defers GTK attachment via g_idle_add() so we never touch GTK/GDK before
 * the main loop is running.
 *
 * GTK3: attaches via gtk_style_context_add_provider_for_screen()
 * GTK4: attaches via gtk_style_context_add_provider_for_display()
 *
 * Build:
 *   cc $(pkg-config --cflags glib-2.0 gio-2.0) \
 *      -shared -fPIC -O2 -Wall -Wextra \
 *      -o liboxidize-gtk-css.so oxidize-gtk-css.c \
 *      $(pkg-config --libs glib-2.0 gio-2.0) -ldl
 *
 * Install to /usr/lib/gio/modules (for AerynOS)
 *
 * Environment variables:
 *   GTK_OXIDIZE_DEBUG=1            verbose logging
 *   GTK_DISABLE_OXIDIZE_STYLE=1    disable for this process
 *   OXIDIZE_GTK_PRIORITY=application|user  (default: user)
 *
 * D-Bus signal (org.oxidize.Appearance1.Changed):
 *   args: (t revision, s css_path, s mode)
 *
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <gio/gio.h>
#include <gio/giomodule.h>
#include <glib.h>

#define OXIDIZE_OBJECT_PATH  "/org/oxidize/Appearance1"
#define OXIDIZE_INTERFACE    "org.oxidize.Appearance1"
#define OXIDIZE_SIGNAL       "Changed"

typedef void GdkDisplay;
typedef void GdkScreen;
typedef void GtkCssProvider;

#define OX_PRIORITY_APPLICATION  600
#define OX_PRIORITY_USER         800

static guint        (*fn_gtk_get_major_version)                      (void)                  = NULL;
static void *       (*fn_gtk_css_provider_new)                       (void)                  = NULL;
static void         (*fn_gtk_css_provider_load_from_path)            (void *, const char *)  = NULL;
/* GTK4 */
static GdkDisplay * (*fn_gdk_display_get_default)                    (void)                  = NULL;
static void         (*fn_gtk_style_context_add_provider_for_display) (void *, void *, guint) = NULL;
/* GTK3 */
static GdkScreen *  (*fn_gdk_screen_get_default)                     (void)                  = NULL;
static void         (*fn_gtk_style_context_add_provider_for_screen)  (void *, void *, guint) = NULL;

static GOnce gtk_once = G_ONCE_INIT;
static guint gtk_version = 0;

static gpointer
load_gtk_syms_once (gpointer data)
{
  (void) data;

#define LOAD(name) fn_##name = dlsym (RTLD_DEFAULT, #name)
  LOAD (gtk_get_major_version);
  LOAD (gtk_css_provider_new);
  LOAD (gtk_css_provider_load_from_path);
  LOAD (gdk_display_get_default);
  LOAD (gtk_style_context_add_provider_for_display);
  LOAD (gdk_screen_get_default);
  LOAD (gtk_style_context_add_provider_for_screen);
#undef LOAD

  if (fn_gtk_get_major_version)
    gtk_version = fn_gtk_get_major_version ();

  return NULL;
}

static gboolean
load_gtk_syms (void)
{
  g_once (&gtk_once, load_gtk_syms_once, NULL);
  return gtk_version == 3 || gtk_version == 4;
}

static gboolean
debug_enabled (void)
{
  static int cached = -1;
  if (cached < 0)
    cached = g_getenv ("GTK_OXIDIZE_DEBUG") != NULL ? 1 : 0;
  return cached;
}

#define OXIDIZE_LOG(...) \
  do { if (debug_enabled ()) g_message ("Oxidize GIO: " __VA_ARGS__); } while (0)

static guint
get_priority (void)
{
  const char *p = g_getenv ("OXIDIZE_GTK_PRIORITY");
  if (g_strcmp0 (p, "application") == 0)
    return OX_PRIORITY_APPLICATION;
  return OX_PRIORITY_USER;
}

static char *
default_css_path (void)
{
  const char *subdir = (gtk_version == 3) ? "gtk-3.0" : "gtk-4.0";
  return g_build_filename (g_get_user_config_dir (), subdir, "gtk.css", NULL);
}

typedef struct {
  GtkCssProvider  *provider;
  GDBusConnection *bus;
  GWeakRef         obj_ref;   /* weak ref to GdkDisplay/GdkScreen */
  char            *css_path;
  guint            signal_id;
  guint            priority;
  gboolean         attached;
} OxidizeStyle;

static GQuark oxidize_quark = 0;

static void
oxidize_style_free (gpointer data)
{
  OxidizeStyle *s = data;

  if (s->bus && s->signal_id)
    g_dbus_connection_signal_unsubscribe (s->bus, s->signal_id);

  g_clear_object (&s->bus);
  g_weak_ref_clear (&s->obj_ref);

  if (s->provider)
    g_object_unref (s->provider);

  g_free (s->css_path);
  g_free (s);
}

static OxidizeStyle *
oxidize_style_for_object (GObject *obj)
{
  OxidizeStyle *s;

  if (G_UNLIKELY (oxidize_quark == 0))
    oxidize_quark = g_quark_from_static_string ("oxidize-gio-style");

  s = g_object_get_qdata (obj, oxidize_quark);
  if (s)
    return s;

  s           = g_new0 (OxidizeStyle, 1);
  s->provider = fn_gtk_css_provider_new ();
  s->css_path = default_css_path ();
  s->priority = get_priority ();
  g_weak_ref_init (&s->obj_ref, obj);

  g_object_set_qdata_full (obj, oxidize_quark, s, oxidize_style_free);
  return s;
}

static void
reload_css (OxidizeStyle *s)
{
  if (!s || !s->css_path || *s->css_path == '\0')
    return;

  if (!g_file_test (s->css_path, G_FILE_TEST_IS_REGULAR))
    {
      OXIDIZE_LOG ("CSS file not found: %s", s->css_path);
      return;
    }

  fn_gtk_css_provider_load_from_path (s->provider, s->css_path);
  OXIDIZE_LOG ("loaded CSS: %s", s->css_path);
}

static void
on_changed (GDBusConnection *connection,
            const gchar     *sender_name,
            const gchar     *object_path,
            const gchar     *interface_name,
            const gchar     *signal_name,
            GVariant        *parameters,
            gpointer         user_data)
{
  OxidizeStyle *s   = user_data;
  GObject      *obj;
  guint64       revision = 0;
  char         *css_path = NULL;
  char         *mode     = NULL;

  (void) connection; (void) sender_name; (void) object_path;
  (void) interface_name; (void) signal_name;

  obj = g_weak_ref_get (&s->obj_ref);
  if (!obj)
    {
      OXIDIZE_LOG ("display/screen gone, ignoring signal");
      return;
    }
  g_object_unref (obj); /* just checking, don't hold it */

  g_variant_get (parameters, "(tss)", &revision, &css_path, &mode);

  OXIDIZE_LOG ("signal revision=%" G_GUINT64_FORMAT " path=%s mode=%s",
               revision,
               css_path ? css_path : "",
               mode     ? mode     : "");
  if (gtk_version == 3)
    {
      reload_css (s);
      goto out;
    }

  if (css_path && *css_path && g_strcmp0 (s->css_path, css_path) != 0)
    {
      g_free (s->css_path);
      s->css_path = g_strdup (css_path);
    }

  reload_css (s);

out:
  g_free (css_path);
  g_free (mode);
}

static void
on_bus_ready (GObject      *source,
              GAsyncResult *result,
              gpointer      user_data)
{
  OxidizeStyle    *s = user_data;
  GObject         *obj;
  GDBusConnection *bus;
  GError          *error = NULL;

  (void) source;

  /* Verify display/screen still alive before subscribing */
  obj = g_weak_ref_get (&s->obj_ref);
  if (!obj)
    {
      OXIDIZE_LOG ("display/screen gone before bus ready, aborting");
      g_bus_get_finish (result, NULL);
      return;
    }
  g_object_unref (obj);

  bus = g_bus_get_finish (result, &error);
  if (!bus)
    {
      OXIDIZE_LOG ("could not connect to session bus: %s",
                   error ? error->message : "unknown");
      g_clear_error (&error);
      return;
    }

  s->bus       = bus;
  s->signal_id =
    g_dbus_connection_signal_subscribe (s->bus,
                                        NULL,
                                        OXIDIZE_INTERFACE,
                                        OXIDIZE_SIGNAL,
                                        OXIDIZE_OBJECT_PATH,
                                        NULL,
                                        G_DBUS_SIGNAL_FLAGS_NONE,
                                        on_changed,
                                        s,        /* pass OxidizeStyle, not obj */
                                        NULL);
  OXIDIZE_LOG ("subscribed to %s.%s signal id=%u",
               OXIDIZE_INTERFACE, OXIDIZE_SIGNAL, s->signal_id);
}

/* Attach to display/screen */

static void
attach (void)
{
  OxidizeStyle *s;
  GObject      *obj;

  if (g_getenv ("GTK_DISABLE_OXIDIZE_STYLE"))
    {
      OXIDIZE_LOG ("disabled by GTK_DISABLE_OXIDIZE_STYLE");
      return;
    }

  if (gtk_version == 4)
    {
      if (!fn_gdk_display_get_default ||
          !fn_gtk_style_context_add_provider_for_display)
        return;

      GdkDisplay *display = fn_gdk_display_get_default ();
      if (!display)
        return;

      obj = G_OBJECT (display);
      s   = oxidize_style_for_object (obj);
      if (s->attached)
        return;

      s->attached = TRUE;
      OXIDIZE_LOG ("attaching provider GTK4 priority=%u path=%s",
                   s->priority, s->css_path ? s->css_path : "(none)");
      fn_gtk_style_context_add_provider_for_display (display, s->provider, s->priority);
    }
  else
    {
      if (!fn_gdk_screen_get_default ||
          !fn_gtk_style_context_add_provider_for_screen)
        return;

      GdkScreen *screen = fn_gdk_screen_get_default ();
      if (!screen)
        return;

      obj = G_OBJECT (screen);
      s   = oxidize_style_for_object (obj);
      if (s->attached)
        return;

      s->attached = TRUE;
      OXIDIZE_LOG ("attaching provider GTK3 priority=%u path=%s",
                   s->priority, s->css_path ? s->css_path : "(none)");
      fn_gtk_style_context_add_provider_for_screen (screen, s->provider, s->priority);
    }

  reload_css (s);
  g_bus_get (G_BUS_TYPE_SESSION, NULL, on_bus_ready, s);
}

static gboolean
on_idle_attach (gpointer user_data)
{
  (void) user_data;

  if (!load_gtk_syms ())
    {
      OXIDIZE_LOG ("no GTK symbols found, skipping (non-GTK process)");
      return G_SOURCE_REMOVE;
    }

  OXIDIZE_LOG ("idle fired, GTK%u", gtk_version);
  attach ();

  return G_SOURCE_REMOVE;
}

/* GIO module entry points  */

G_MODULE_EXPORT void
g_io_module_load (GIOModule *module)
{
  if (g_getenv ("GTK_DISABLE_OXIDIZE_STYLE"))
    return;

  g_type_module_use (G_TYPE_MODULE (module));

  OXIDIZE_LOG ("module loaded (pid %d)", (int) getpid ());

  g_idle_add (on_idle_attach, NULL);
}

G_MODULE_EXPORT void
g_io_module_unload (GIOModule *module)
{
  (void) module;
  OXIDIZE_LOG ("module unloaded");
}

G_MODULE_EXPORT char **
g_io_module_query (void)
{
  static const char *features[] = { NULL };
  return (char **) features;
}
