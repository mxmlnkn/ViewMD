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

  /* Headings of the current document (ViewmdHeading*), newest render last. */
  GPtrArray *headings;

  /* TRUE when this tab shows the raw markdown source instead of a render;
     source_content then holds the text shown verbatim. */
  gboolean source_view;

  /* Remote image loading decision for this document ("ask" mode). */
  gboolean remote_images_decided;
  gboolean remote_images_allowed;

  /* Optional hook invoked after a full re-render (used for the ToC). */
  void (*on_rendered)(struct _MarkydEditor *editor);

  /* Prevent recursive tag application. */
  gboolean updating_tags;

  /* Coalesce markdown re-rendering to idle to avoid invalidating GTK iterators. */
  guint markdown_idle_id;

  /* Debounced full re-render after the text area width changed, so the space
     reserved around child anchors matches the re-pinned widgets again. */
  guint relayout_timeout_id;

  /* Scroll anchoring across resizes: offset of the line that sits at the top
     of the viewport plus the sub-line remainder, so a re-wrap can restore the
     exact same top-left position instead of ratcheting the view upwards. */
  gint scroll_anchor_offset;
  gdouble scroll_anchor_remainder;
  gboolean scroll_anchor_valid;

  /* Structural anchor used when a re-render changes the document: the slug of
     the heading section containing the anchored line and the pixel distance
     from that heading's top to the viewport top. The slug is stored in a fixed
     buffer (empty when no heading precedes the line) so no ownership is added. */
  gchar scroll_anchor_heading_slug[128];
  gint scroll_anchor_heading_delta;

  /* Coalesce the post-re-render scroll restore to idle so the text view has
     re-laid-out (and resized its adjustment) before the value is restored. */
  guint anchor_restore_idle_id;
  gint scroll_anchor_restore_attempts;

  /* True between capturing the anchor and restoring it across a re-render,
     while the buffer rebuild temporarily resets the adjustment; the
     adjustment's own value-changed handler must not overwrite the anchor. */
  gboolean scroll_anchor_frozen;

  /* Text view size of the last "size-allocate" pass. */
  gint last_allocate_width;
  gint last_allocate_height;

  /* Content width the flow widgets were last spanned to. */
  gint last_span_width;

  /* True while the view is being re-laid out (guards anchor updates from the
     adjustments' own clamping during that pass). */
  gboolean in_resize_pass;

  /* Borrowed from the enclosing GtkScrolledWindow once discovered. */
  GtkAdjustment *vadjustment;
} MarkydEditor;

/* Lifecycle */
MarkydEditor *markyd_editor_new(MarkydApp *app);
void markyd_editor_free(MarkydEditor *editor);

/* Content management */
void markyd_editor_set_content(MarkydEditor *editor, const gchar *content);
gchar *markyd_editor_get_content(MarkydEditor *editor);

/* Show 'text' verbatim (raw markdown) instead of rendering it. */
void markyd_editor_show_source(MarkydEditor *editor, const gchar *text);

/* Document path handling (used for relative image/link resolution). */
void markyd_editor_set_file_path(MarkydEditor *editor, const gchar *path);
const gchar *markyd_editor_get_file_path(MarkydEditor *editor);

/* Scroll the document to a heading anchor referenced by its slug. */
void markyd_editor_scroll_to_anchor(MarkydEditor *editor, const gchar *slug);

/* Headings of the current document; 2-tuple of level + text, for the ToC. */
const GPtrArray *markyd_editor_get_headings(MarkydEditor *editor);

/* Widget access */
GtkWidget *markyd_editor_get_widget(MarkydEditor *editor);
void markyd_editor_focus(MarkydEditor *editor);

/* Force a refresh of markdown styling/rendering (e.g., after settings change). */
void markyd_editor_refresh(MarkydEditor *editor);

#endif /* MARKYD_EDITOR_H */
