#ifndef MARKYD_WINDOW_H
#define MARKYD_WINDOW_H

#include <gtk/gtk.h>

typedef struct _MarkydApp MarkydApp;
typedef struct _MarkydEditor MarkydEditor;

typedef struct _MarkydTab {
  MarkydEditor *editor;
  GtkWidget *scroll;
  GtkWidget *tab_label;
} MarkydTab;

typedef struct _MarkydWindow {
  GtkWidget *window;
  GtkWidget *header_bar;
  GtkWidget *btn_open;
  GtkWidget *btn_refresh;
  GtkWidget *btn_search;
  GtkWidget *btn_settings;
  GtkWidget *search_revealer;
  GtkWidget *search_entry;
  GtkWidget *btn_search_prev;
  GtkWidget *btn_search_next;
  GtkWidget *lbl_search_status;
  GtkWidget *lbl_title;
  GtkWidget *notebook;
  GtkWidget *paned;
  GtkWidget *btn_toc;
  GtkWidget *toc_scroll;
  GtkWidget *toc_list;
  GPtrArray *tabs; /* MarkydTab* */
  gint active_index;
  MarkydEditor *editor; /* active tab's editor */
  MarkydApp *app;
  GArray *search_matches;
  gint search_current_index;
} MarkydWindow;

/* Lifecycle */
MarkydWindow *markyd_window_new(MarkydApp *app);
void markyd_window_free(MarkydWindow *win);

/* Visibility */
void markyd_window_show(MarkydWindow *win);
void markyd_window_hide(MarkydWindow *win);
void markyd_window_toggle(MarkydWindow *win);
gboolean markyd_window_is_visible(MarkydWindow *win);

/* Styling */
void markyd_window_apply_css(MarkydWindow *win);

/* Tabs */
MarkydTab *markyd_window_new_tab(MarkydWindow *win);
void markyd_window_close_tab(MarkydWindow *win, MarkydTab *tab);
MarkydTab *markyd_window_get_active_tab(MarkydWindow *win);
void markyd_window_switch_tab(MarkydWindow *win, gint offset);
gboolean markyd_window_has_document(MarkydWindow *win, MarkydTab *tab);

/* Open a document; reuses the empty placeholder tab if it is still unused. */
gboolean markyd_window_open_document(MarkydWindow *win, const gchar *path);

#endif /* MARKYD_WINDOW_H */
