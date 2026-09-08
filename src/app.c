#include "app.h"
#include "config.h"
#include "editor.h"
#include "window.h"

/* Global app instance */
MarkydApp *app = NULL;

static void on_activate(GtkApplication *gtk_app, gpointer user_data);
static void on_open(GtkApplication *gtk_app, GFile **files, gint n_files,
                    const gchar *hint, gpointer user_data);
static void markyd_app_ensure_window(MarkydApp *self);

MarkydApp *markyd_app_new(void) {
  MarkydApp *self = g_new0(MarkydApp, 1);

  config = config_new();
  config_load(config);

  GApplicationFlags flags =
#if GLIB_CHECK_VERSION(2, 74, 0)
      G_APPLICATION_DEFAULT_FLAGS;
#else
      G_APPLICATION_FLAGS_NONE;
#endif
  flags = (GApplicationFlags)(flags | G_APPLICATION_NON_UNIQUE |
                              G_APPLICATION_HANDLES_OPEN);

  self->gtk_app = gtk_application_new("org.viewmd.app", flags);

  g_signal_connect(self->gtk_app, "activate", G_CALLBACK(on_activate), self);
  g_signal_connect(self->gtk_app, "open", G_CALLBACK(on_open), self);

  app = self;
  return self;
}

void markyd_app_free(MarkydApp *self) {
  if (!self)
    return;

  if (self->window) {
    markyd_window_free(self->window);
  }

  g_object_unref(self->gtk_app);

  config_save(config);
  config_free(config);
  config = NULL;

  g_free(self);
  app = NULL;
}

int markyd_app_run(MarkydApp *self, int argc, char **argv) {
  return g_application_run(G_APPLICATION(self->gtk_app), argc, argv);
}

void markyd_app_update_window_title(MarkydApp *self) {
  MarkydTab *tab;
  const gchar *path;
  gchar *title;

  if (!self || !self->window || !self->window->window) {
    return;
  }

  tab = markyd_window_get_active_tab(self->window);
  path = tab ? markyd_editor_get_file_path(tab->editor) : NULL;

  if (path && path[0] != '\0') {
    gchar *base = g_path_get_basename(path);
    title = g_strdup_printf("ViewMD - %s", base);
    g_free(base);
  } else {
    title = g_strdup("ViewMD");
  }

  gtk_window_set_title(GTK_WINDOW(self->window->window), title);
  g_free(title);
}

static void on_activate(GtkApplication *gtk_app, gpointer user_data) {
  MarkydApp *self = (MarkydApp *)user_data;

  (void)gtk_app;
  markyd_app_ensure_window(self);
  markyd_window_show(self->window);
}

static void on_open(GtkApplication *gtk_app, GFile **files, gint n_files,
                    const gchar *hint, gpointer user_data) {
  MarkydApp *self = (MarkydApp *)user_data;
  gboolean opened = FALSE;

  (void)gtk_app;
  (void)hint;

  markyd_app_ensure_window(self);

  for (gint i = 0; i < n_files; i++) {
    gchar *path = g_file_get_path(files[i]);
    if (!path) {
      continue;
    }
    if (markyd_app_open_file(self, path)) {
      opened = TRUE;
    }
    g_free(path);
  }

  if (!opened && n_files > 0) {
    g_printerr("ViewMD: unable to open provided file(s)\n");
  }

  markyd_window_show(self->window);
}

static void markyd_app_ensure_window(MarkydApp *self) {
  if (self->window) {
    return;
  }

  self->window = markyd_window_new(self);
  self->editor = self->window->editor;

  markyd_editor_set_content(self->editor,
                            "# ViewMD\n\nUse the Open button to load a markdown document.");
  markyd_app_update_window_title(self);
}

gboolean markyd_app_open_file(MarkydApp *self, const gchar *path) {
  if (!self || !self->window || !path || path[0] == '\0') {
    return FALSE;
  }
  return markyd_window_open_document(self->window, path);
}
