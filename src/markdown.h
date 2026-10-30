#ifndef MARKYD_MARKDOWN_H
#define MARKYD_MARKDOWN_H

#include <gtk/gtk.h>

/* GObject data key used to mark hrule child anchors inserted into the buffer. */
#define TRAYMD_HRULE_ANCHOR_DATA "viewmd-hr-anchor"

/* Initialize markdown tags on a text buffer */
void markdown_init_tags(GtkTextBuffer *buffer);

/* Update accent colors for existing tags (after config changes). */
void markdown_update_accent_tags(GtkTextBuffer *buffer);

/* Data key set on per-link metadata tags to store resolved URL/href. */
#define VIEWMD_LINK_URL_DATA "viewmd-link-url"

/* Prefix for named text marks used as internal heading anchors. */
#define VIEWMD_ANCHOR_MARK_PREFIX "viewmd-anchor-"

/* GObject data key used to mark table child anchors with parsed table data. */
#define VIEWMD_TABLE_ANCHOR_DATA "viewmd-table-anchor"
/* GObject data key set on table anchors for hidden searchable index metadata. */
#define VIEWMD_TABLE_SEARCH_INDEX_DATA "viewmd-table-search-index"
/* GObject data key set on table anchors for attached table widget instance. */
#define VIEWMD_TABLE_WIDGET_DATA "viewmd-table-widget"
/* GObject data keys set on each table cell widget. */
#define VIEWMD_TABLE_CELL_ROW_DATA "viewmd-table-cell-row"
#define VIEWMD_TABLE_CELL_COL_DATA "viewmd-table-cell-col"
/* CSS classes for table search highlight states. */
#define VIEWMD_TABLE_CELL_MATCH_CLASS "viewmd-table-cell-match"
#define VIEWMD_TABLE_CELL_CURRENT_CLASS "viewmd-table-cell-current"

/* GObject data keys for image anchors and metadata. */
#define VIEWMD_IMAGE_ANCHOR_DATA "viewmd-image-anchor"
#define VIEWMD_IMAGE_SRC_DATA "viewmd-image-src"
#define VIEWMD_IMAGE_ALT_DATA "viewmd-image-alt"
#define VIEWMD_IMAGE_WIDGET_DATA "viewmd-image-widget"

/* GObject data key for YAML frontmatter anchors holding key/value pairs. */
#define VIEWMD_FRONTMATTER_DATA "viewmd-frontmatter"
/* GObject data key set on frontmatter anchors once the widget has been built. */
#define VIEWMD_FRONTMATTER_WIDGET_DATA "viewmd-frontmatter-widget"

/* GObject data keys for fenced code block anchors and widgets. */
#define VIEWMD_CODE_BLOCK_DATA "viewmd-code-block"
#define VIEWMD_CODE_SRC_DATA "viewmd-code-src"
#define VIEWMD_CODE_LANG_DATA "viewmd-code-lang"
#define VIEWMD_CODE_WIDGET_DATA "viewmd-code-widget"

/* GObject data keys for heading/details fold anchors. */
#define VIEWMD_FOLD_ANCHOR_DATA "viewmd-fold-anchor"
#define VIEWMD_FOLD_KEY_DATA "viewmd-fold-key"
#define VIEWMD_FOLD_STATE_DATA "viewmd-fold-state"
#define VIEWMD_FOLD_IS_DETAILS_DATA "viewmd-fold-is-details"
#define VIEWMD_FOLD_WIDGET_DATA "viewmd-fold-widget"

/* Render markdown source into the buffer and apply markdown styling.
   'collapsed' is an optional GHashTable of heading/details keys that should
   be rendered collapsed (or NULL to render everything expanded).
   'headings_out' is an optional GPtrArray (with -with-free func) that is
   emptied then filled with ViewmdHeading* entries for the table of contents. */
typedef struct {
  gint level;
  gchar *text;
  gchar *slug;
} ViewmdHeading;

void markdown_apply_tags(GtkTextBuffer *buffer, const gchar *source,
                         GHashTable *collapsed, GPtrArray *headings_out);

typedef struct {
  gint row;
  gint col;
  gint start_offset;
  gint end_offset;
} ViewmdTableSearchCellRange;

typedef struct {
  gint start_offset;
  gint end_offset;
  GArray *cells; /* ViewmdTableSearchCellRange */
} ViewmdTableSearchIndex;

/* Normalize heading/link text into anchor slug form. Caller owns result. */
gchar *markdown_normalize_anchor_slug(const gchar *text);

/* Build full text-mark name for an anchor fragment. Caller owns result. */
gchar *markdown_anchor_mark_name(const gchar *fragment);

/* Build a GTK widget for a table anchor, or NULL if not a table anchor. */
GtkWidget *markdown_create_table_widget(GtkTextChildAnchor *anchor);

/* Build a GTK widget for a YAML frontmatter anchor, or NULL. */
GtkWidget *markdown_create_frontmatter_widget(GtkTextChildAnchor *anchor);

/* Build a GTK widget for a fenced code block anchor, or NULL. */
GtkWidget *markdown_create_code_block_widget(GtkTextChildAnchor *anchor);

/* Build a GTK widget for a fold toggle anchor (heading or details), or NULL. */
GtkWidget *markdown_create_fold_widget(GtkTextChildAnchor *anchor);

#endif /* MARKYD_MARKDOWN_H */
