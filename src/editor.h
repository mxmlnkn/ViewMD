#ifndef MARKYD_EDITOR_H
#define MARKYD_EDITOR_H

#include <gtk/gtk.h>

/* Search-highlight tags; created on every tab's buffer so Ctrl+F is not
   limited to the first tab. */
#define TAG_SEARCH_MATCH "viewmd_search_match"
#define TAG_SEARCH_CURRENT "viewmd_search_current"

typedef struct _MarkydApp MarkydApp;

typedef struct _MarkydEditor {
  GtkWidget *text_view;
  GtkTextBuffer *buffer;
  MarkydApp *app;

  /* Original markdown content loaded into the viewer. */
  gchar *source_content;

  /* Absolute path of the document shown in this editor, or NULL. */
  gchar *file_path;

  /* Collapse state for heading/details sections (key -> TRUE when folded). */
  GHashTable *collapsed;

  /* Prevent recursive tag application. */
  gboolean updating_tags;

  /* Coalesce markdown re-rendering to idle to avoid invalidating GTK iterators. */
  guint markdown_idle_id;
} MarkydEditor;

/* Lifecycle */
MarkydEditor *markyd_editor_new(MarkydApp *app);
void markyd_editor_free(MarkydEditor *editor);

/* Content management */
void markyd_editor_set_content(MarkydEditor *editor, const gchar *content);
gchar *markyd_editor_get_content(MarkydEditor *editor);

/* Document path handling (used for relative image/link resolution). */
void markyd_editor_set_file_path(MarkydEditor *editor, const gchar *path);
const gchar *markyd_editor_get_file_path(MarkydEditor *editor);

/* Widget access */
GtkWidget *markyd_editor_get_widget(MarkydEditor *editor);
void markyd_editor_focus(MarkydEditor *editor);

/* Force a refresh of markdown styling/rendering (e.g., after settings change). */
void markyd_editor_refresh(MarkydEditor *editor);

#endif /* MARKYD_EDITOR_H */
