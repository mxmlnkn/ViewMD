#include "window.h"
#include "app.h"
#include "config.h"
#include "editor.h"
#include "markdown.h"
#include <pango/pangocairo.h>

typedef struct {
  gint start_offset;
  gint end_offset;
  GtkTextChildAnchor *table_anchor;
  gint table_row;
  gint table_col;
} SearchMatch;

static void on_open_clicked(GtkButton *button, gpointer user_data);
static void on_refresh_clicked(GtkButton *button, gpointer user_data);
static void on_settings_clicked(GtkButton *button, gpointer user_data);
static void on_search_changed(GtkEditable *editable, gpointer user_data);
static void on_search_prev_clicked(GtkButton *button, gpointer user_data);
static void on_search_next_clicked(GtkButton *button, gpointer user_data);
static gboolean on_search_entry_key_press(GtkWidget *widget, GdkEventKey *event,
                                          gpointer user_data);
static void on_editor_buffer_changed(GtkTextBuffer *buffer, gpointer user_data);
static void ensure_search_tags(MarkydWindow *self);
static void update_search_tag_styles(MarkydWindow *self, const gchar *match_bg,
                                     const gchar *match_fg,
                                     const gchar *current_bg,
                                     const gchar *current_fg);
static void clear_search_matches(MarkydWindow *self);
static void update_search_matches(MarkydWindow *self);
static void jump_to_search_match(MarkydWindow *self, gint index,
                                 gboolean scroll_to_match);
static void clear_table_search_highlight(MarkydWindow *self, gboolean clear_match,
                                         gboolean clear_current);
static void apply_table_search_match_highlight(MarkydWindow *self);
static gboolean resolve_table_match_location(MarkydWindow *self, gint start_offset,
                                             gint end_offset,
                                             GtkTextChildAnchor **out_anchor,
                                             gint *out_row, gint *out_col);
static GtkWidget *lookup_table_cell_widget(GtkWidget *table_widget, gint row,
                                           gint col);
static void set_table_cell_highlight(GtkWidget *cell, gboolean match,
                                     gboolean current);
static gboolean scroll_to_table_cell(MarkydWindow *self, GtkWidget *cell);
static void show_search_ui(MarkydWindow *self);
static void hide_search_ui(MarkydWindow *self);
static void on_search_clicked(GtkButton *button, gpointer user_data);
static gboolean on_key_press_event(GtkWidget *widget, GdkEventKey *event,
                                   gpointer user_data);
static void on_drag_data_received(GtkWidget *widget, GdkDragContext *context,
                                  gint x, gint y, GtkSelectionData *data,
                                  guint info, guint time, gpointer user_data);
static gboolean on_text_view_scroll(GtkWidget *widget, GdkEventScroll *event,
                                    gpointer user_data);
static gboolean on_configure_event(GtkWidget *widget, GdkEventConfigure *event,
                                   gpointer user_data);
static gboolean on_window_state_event(GtkWidget *widget,
                                      GdkEventWindowState *event,
                                      gpointer user_data);
static void on_window_destroyed(GtkWidget *widget, gpointer user_data);
static void on_notebook_switch_page(GtkNotebook *notebook, GtkWidget *page,
                                    guint page_num, gpointer user_data);
static void on_notebook_page_reordered(GtkNotebook *notebook, GtkWidget *child,
                                       guint page_num, gpointer user_data);
static gboolean on_notebook_reassert_page(gpointer user_data);
static gboolean on_tab_label_button_press(GtkWidget *widget,
                                          GdkEventButton *event,
                                          gpointer user_data);
static void on_tab_close_clicked(GtkButton *button, gpointer user_data);
static gboolean update_tab_label(MarkydWindow *self, MarkydTab *tab);
static gboolean load_document_into_tab(MarkydWindow *self, MarkydTab *tab,
                                       const gchar *path);
static void update_toc(MarkydWindow *self);
static void on_editor_rendered(MarkydEditor *editor);
static void on_toc_toggle_clicked(GtkButton *button, gpointer user_data);
static void on_toc_row_activated(GtkListBox *list, GtkListBoxRow *row,
                                 gpointer user_data);

static void on_font_size_changed(GtkSpinButton *spin, gpointer user_data);
static void on_font_family_changed(GtkComboBoxText *combo, gpointer user_data);
static void on_theme_changed(GtkComboBoxText *combo, gpointer user_data);
static gchar *rgba_to_hex(const GdkRGBA *rgba);
static void init_color_button(GtkColorButton *btn, const gchar *color_str);
static void on_color_set(GtkColorButton *btn, gpointer user_data);
static GtkWidget *create_settings_dialog(MarkydWindow *win);
static void apply_settings_to_all_tabs(MarkydWindow *self);
static void on_settings_response(GtkDialog *dialog, gint response,
                                 gpointer user_data);

static gboolean geometry_debug_enabled(void) {
  const gchar *v = g_getenv("VIEWMD_DEBUG_GEOMETRY");
  if (!v) {
    v = g_getenv("TRAYMD_DEBUG_GEOMETRY");
  }
  return v && v[0] != '\0' && g_strcmp0(v, "0") != 0;
}

static void ensure_search_tags(MarkydWindow *self) {
  GtkTextTagTable *table;

  if (!self || !self->editor || !self->editor->buffer) {
    return;
  }

  table = gtk_text_buffer_get_tag_table(self->editor->buffer);
  if (!table) {
    return;
  }

  if (!gtk_text_tag_table_lookup(table, TAG_SEARCH_MATCH)) {
    gtk_text_buffer_create_tag(self->editor->buffer, TAG_SEARCH_MATCH, "weight",
                               PANGO_WEIGHT_BOLD, NULL);
  }
  if (!gtk_text_tag_table_lookup(table, TAG_SEARCH_CURRENT)) {
    gtk_text_buffer_create_tag(self->editor->buffer, TAG_SEARCH_CURRENT, "weight",
                               PANGO_WEIGHT_BOLD, NULL);
  }
}

static void update_search_tag_styles(MarkydWindow *self, const gchar *match_bg,
                                     const gchar *match_fg,
                                     const gchar *current_bg,
                                     const gchar *current_fg) {
  if (!self || !self->tabs) {
    return;
  }

  for (guint i = 0; i < self->tabs->len; i++) {
    MarkydTab *tab = g_ptr_array_index(self->tabs, i);
    GtkTextTagTable *table;
    GtkTextTag *tag;

    if (!tab || !tab->editor || !tab->editor->buffer) {
      continue;
    }
    table = gtk_text_buffer_get_tag_table(tab->editor->buffer);
    if (!table) {
      continue;
    }

    tag = gtk_text_tag_table_lookup(table, TAG_SEARCH_MATCH);
    if (tag) {
      g_object_set(tag, "background", match_bg, "foreground", match_fg, NULL);
    }

    tag = gtk_text_tag_table_lookup(table, TAG_SEARCH_CURRENT);
    if (tag) {
      g_object_set(tag, "background", current_bg, "foreground", current_fg, NULL);
    }
  }
}

static void clear_search_matches(MarkydWindow *self) {
  GtkTextIter start;
  GtkTextIter end;

  if (!self || !self->editor || !self->editor->buffer) {
    return;
  }

  gtk_text_buffer_get_bounds(self->editor->buffer, &start, &end);
  gtk_text_buffer_remove_tag_by_name(self->editor->buffer, TAG_SEARCH_MATCH,
                                     &start, &end);
  gtk_text_buffer_remove_tag_by_name(self->editor->buffer, TAG_SEARCH_CURRENT,
                                     &start, &end);
  clear_table_search_highlight(self, TRUE, TRUE);

  if (self->search_matches) {
    g_array_set_size(self->search_matches, 0);
  }
  self->search_current_index = -1;

  if (self->lbl_search_status) {
    gtk_label_set_text(GTK_LABEL(self->lbl_search_status), "");
  }
  if (self->btn_search_prev) {
    gtk_widget_set_sensitive(self->btn_search_prev, FALSE);
  }
  if (self->btn_search_next) {
    gtk_widget_set_sensitive(self->btn_search_next, FALSE);
  }
}

static void set_table_cell_highlight(GtkWidget *cell, gboolean match,
                                     gboolean current) {
  GtkStyleContext *style;

  if (!cell) {
    return;
  }

  style = gtk_widget_get_style_context(cell);
  if (match) {
    gtk_style_context_add_class(style, VIEWMD_TABLE_CELL_MATCH_CLASS);
  } else {
    gtk_style_context_remove_class(style, VIEWMD_TABLE_CELL_MATCH_CLASS);
  }

  if (current) {
    gtk_style_context_add_class(style, VIEWMD_TABLE_CELL_CURRENT_CLASS);
  } else {
    gtk_style_context_remove_class(style, VIEWMD_TABLE_CELL_CURRENT_CLASS);
  }
}

static gboolean scroll_to_table_cell(MarkydWindow *self, GtkWidget *cell) {
  gint x = 0;
  gint y = 0;
  gdouble doc_x;
  gdouble doc_y;
  GtkAllocation alloc;
  GtkAdjustment *hadj;
  GtkAdjustment *vadj;
  GtkWidget *scroll = markyd_window_get_active_tab(self) ? markyd_window_get_active_tab(self)->scroll : NULL;

  if (!self || !cell || !self->editor || !self->editor->text_view || !scroll) {
    return FALSE;
  }

  if (!gtk_widget_translate_coordinates(cell, self->editor->text_view, 0, 0, &x,
                                        &y)) {
    return FALSE;
  }

  gtk_widget_get_allocation(cell, &alloc);
  hadj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scroll));
  vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
  doc_x = x + (hadj ? gtk_adjustment_get_value(hadj) : 0.0);
  doc_y = y + (vadj ? gtk_adjustment_get_value(vadj) : 0.0);

  if (hadj) {
    gdouble target = MAX(0.0, doc_x - 16.0);
    gdouble max_val =
        gtk_adjustment_get_upper(hadj) - gtk_adjustment_get_page_size(hadj);
    gtk_adjustment_set_value(hadj, CLAMP(target, 0.0, MAX(0.0, max_val)));
  }
  if (vadj) {
    gdouble page = gtk_adjustment_get_page_size(vadj);
    gdouble target = MAX(0.0, doc_y - (page * 0.25));
    gdouble max_val =
        gtk_adjustment_get_upper(vadj) - gtk_adjustment_get_page_size(vadj);
    gtk_adjustment_set_value(vadj, CLAMP(target, 0.0, MAX(0.0, max_val)));
  }

  return TRUE;
}

static GtkWidget *lookup_table_cell_widget(GtkWidget *table_widget, gint row,
                                           gint col) {
  GList *wrapper_children;
  GtkWidget *found = NULL;

  if (!table_widget || !GTK_IS_CONTAINER(table_widget)) {
    return NULL;
  }

  wrapper_children = gtk_container_get_children(GTK_CONTAINER(table_widget));
  for (GList *w = wrapper_children; w != NULL && !found; w = w->next) {
    GtkWidget *child = GTK_WIDGET(w->data);
    if (!GTK_IS_GRID(child)) {
      continue;
    }

    GList *grid_children = gtk_container_get_children(GTK_CONTAINER(child));
    for (GList *g = grid_children; g != NULL; g = g->next) {
      GtkWidget *cell = GTK_WIDGET(g->data);
      gint cell_row = GPOINTER_TO_INT(
          g_object_get_data(G_OBJECT(cell), VIEWMD_TABLE_CELL_ROW_DATA));
      gint cell_col = GPOINTER_TO_INT(
          g_object_get_data(G_OBJECT(cell), VIEWMD_TABLE_CELL_COL_DATA));
      if (cell_row == row && cell_col == col) {
        found = cell;
        break;
      }
    }
    g_list_free(grid_children);
  }
  g_list_free(wrapper_children);

  return found;
}

static void clear_table_search_highlight(MarkydWindow *self, gboolean clear_match,
                                         gboolean clear_current) {
  GtkTextIter iter;
  GtkTextIter end;

  if (!self || !self->editor || !self->editor->buffer) {
    return;
  }

  gtk_text_buffer_get_bounds(self->editor->buffer, &iter, &end);
  while (!gtk_text_iter_equal(&iter, &end)) {
    GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(&iter);
    if (anchor &&
        g_object_get_data(G_OBJECT(anchor), VIEWMD_TABLE_ANCHOR_DATA) != NULL) {
      GtkWidget *table_widget =
          g_object_get_data(G_OBJECT(anchor), VIEWMD_TABLE_WIDGET_DATA);
      if (table_widget && GTK_IS_CONTAINER(table_widget)) {
        GList *wrapper_children =
            gtk_container_get_children(GTK_CONTAINER(table_widget));
        for (GList *w = wrapper_children; w != NULL; w = w->next) {
          GtkWidget *child = GTK_WIDGET(w->data);
          if (!GTK_IS_GRID(child)) {
            continue;
          }
          GList *grid_children = gtk_container_get_children(GTK_CONTAINER(child));
          for (GList *g = grid_children; g != NULL; g = g->next) {
            GtkWidget *cell = GTK_WIDGET(g->data);
            if (clear_match) {
              gtk_style_context_remove_class(gtk_widget_get_style_context(cell),
                                             VIEWMD_TABLE_CELL_MATCH_CLASS);
            }
            if (clear_current) {
              gtk_style_context_remove_class(gtk_widget_get_style_context(cell),
                                             VIEWMD_TABLE_CELL_CURRENT_CLASS);
            }
          }
          g_list_free(grid_children);
        }
        g_list_free(wrapper_children);
      }
    }
    gtk_text_iter_forward_char(&iter);
  }
}

static gboolean resolve_table_match_location(MarkydWindow *self, gint start_offset,
                                             gint end_offset,
                                             GtkTextChildAnchor **out_anchor,
                                             gint *out_row, gint *out_col) {
  GtkTextIter iter;
  GtkTextIter end;

  if (out_anchor) {
    *out_anchor = NULL;
  }
  if (out_row) {
    *out_row = -1;
  }
  if (out_col) {
    *out_col = -1;
  }

  if (!self || !self->editor || !self->editor->buffer ||
      end_offset <= start_offset) {
    return FALSE;
  }

  gtk_text_buffer_get_bounds(self->editor->buffer, &iter, &end);
  while (!gtk_text_iter_equal(&iter, &end)) {
    GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(&iter);
    if (anchor) {
      ViewmdTableSearchIndex *index = g_object_get_data(
          G_OBJECT(anchor), VIEWMD_TABLE_SEARCH_INDEX_DATA);
      if (index && start_offset < index->end_offset &&
          end_offset > index->start_offset) {
        if (out_anchor) {
          *out_anchor = anchor;
        }

        if (index->cells) {
          ViewmdTableSearchCellRange *overlap_cell = NULL;
          for (guint i = 0; i < index->cells->len; i++) {
            ViewmdTableSearchCellRange *cell =
                &g_array_index(index->cells, ViewmdTableSearchCellRange, i);
            if (start_offset >= cell->start_offset && start_offset < cell->end_offset) {
              if (out_row) {
                *out_row = cell->row;
              }
              if (out_col) {
                *out_col = cell->col;
              }
              return TRUE;
            }
            if (start_offset < cell->end_offset && end_offset > cell->start_offset) {
              overlap_cell = cell;
            }
          }
          if (overlap_cell) {
            if (out_row) {
              *out_row = overlap_cell->row;
            }
            if (out_col) {
              *out_col = overlap_cell->col;
            }
            return TRUE;
          }
        }

        return TRUE;
      }
    }
    gtk_text_iter_forward_char(&iter);
  }

  return FALSE;
}

static void apply_table_search_match_highlight(MarkydWindow *self) {
  if (!self || !self->search_matches) {
    return;
  }

  clear_table_search_highlight(self, TRUE, FALSE);

  for (guint i = 0; i < self->search_matches->len; i++) {
    SearchMatch *match = &g_array_index(self->search_matches, SearchMatch, i);
    GtkWidget *table_widget;
    GtkWidget *cell;

    if (!match->table_anchor || match->table_row < 0 || match->table_col < 0) {
      continue;
    }

    table_widget =
        g_object_get_data(G_OBJECT(match->table_anchor), VIEWMD_TABLE_WIDGET_DATA);
    cell = lookup_table_cell_widget(table_widget, match->table_row, match->table_col);
    if (cell) {
      set_table_cell_highlight(cell, TRUE, FALSE);
    }
  }
}

static void jump_to_search_match(MarkydWindow *self, gint index,
                                 gboolean scroll_to_match) {
  GtkTextIter start;
  GtkTextIter end;
  SearchMatch *match;
  gchar *status;

  if (!self || !self->editor || !self->editor->buffer || !self->search_matches ||
      self->search_matches->len == 0) {
    return;
  }

  if (index < 0 || index >= (gint)self->search_matches->len) {
    return;
  }

  gtk_text_buffer_get_bounds(self->editor->buffer, &start, &end);
  gtk_text_buffer_remove_tag_by_name(self->editor->buffer, TAG_SEARCH_CURRENT,
                                     &start, &end);
  clear_table_search_highlight(self, FALSE, TRUE);

  match = &g_array_index(self->search_matches, SearchMatch, index);
  if (match->table_anchor && match->table_row >= 0 && match->table_col >= 0) {
    GtkWidget *table_widget =
        g_object_get_data(G_OBJECT(match->table_anchor), VIEWMD_TABLE_WIDGET_DATA);
    GtkWidget *cell =
        lookup_table_cell_widget(table_widget, match->table_row, match->table_col);
    if (cell) {
      set_table_cell_highlight(cell, TRUE, TRUE);
    }
  } else {
    gtk_text_buffer_get_iter_at_offset(self->editor->buffer, &start,
                                       match->start_offset);
    gtk_text_buffer_get_iter_at_offset(self->editor->buffer, &end,
                                       match->end_offset);
    gtk_text_buffer_apply_tag_by_name(self->editor->buffer, TAG_SEARCH_CURRENT,
                                      &start, &end);
    gtk_text_buffer_place_cursor(self->editor->buffer, &start);
  }

  if (scroll_to_match && self->editor->text_view) {
    if (match->table_anchor) {
      gboolean scrolled_to_cell = FALSE;
      if (match->table_row >= 0 && match->table_col >= 0) {
        GtkWidget *table_widget = g_object_get_data(G_OBJECT(match->table_anchor),
                                                    VIEWMD_TABLE_WIDGET_DATA);
        GtkWidget *cell = lookup_table_cell_widget(table_widget, match->table_row,
                                                   match->table_col);
        if (cell) {
          scrolled_to_cell = scroll_to_table_cell(self, cell);
        }
      }
      if (!scrolled_to_cell) {
        GtkTextIter anchor_iter;
        gtk_text_buffer_get_iter_at_child_anchor(self->editor->buffer, &anchor_iter,
                                                 match->table_anchor);
        gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(self->editor->text_view),
                                     &anchor_iter, 0.2, FALSE, 0.0, 0.0);
      }
    } else {
      gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(self->editor->text_view), &start,
                                   0.2, FALSE, 0.0, 0.0);
    }
  }

  self->search_current_index = index;
  status = g_strdup_printf("%d/%u", index + 1, self->search_matches->len);
  gtk_label_set_text(GTK_LABEL(self->lbl_search_status), status);
  g_free(status);
}

static void update_search_matches(MarkydWindow *self) {
  const gchar *query;
  GtkTextIter iter;
  GtkTextIter match_start;
  GtkTextIter match_end;
  GtkTextIter end;

  if (!self || !self->editor || !self->editor->buffer || !self->search_entry) {
    return;
  }

  query = gtk_entry_get_text(GTK_ENTRY(self->search_entry));
  clear_search_matches(self);

  if (!query || query[0] == '\0') {
    return;
  }

  ensure_search_tags(self);
  gtk_text_buffer_get_start_iter(self->editor->buffer, &iter);
  gtk_text_buffer_get_end_iter(self->editor->buffer, &end);

  while (gtk_text_iter_forward_search(&iter, query,
                                      GTK_TEXT_SEARCH_CASE_INSENSITIVE |
                                          GTK_TEXT_SEARCH_TEXT_ONLY,
                                      &match_start, &match_end, &end)) {
    SearchMatch match = {gtk_text_iter_get_offset(&match_start),
                         gtk_text_iter_get_offset(&match_end), NULL, -1, -1};
    gtk_text_buffer_apply_tag_by_name(self->editor->buffer, TAG_SEARCH_MATCH,
                                      &match_start, &match_end);
    resolve_table_match_location(self, match.start_offset, match.end_offset,
                                 &match.table_anchor, &match.table_row,
                                 &match.table_col);
    g_array_append_val(self->search_matches, match);
    iter = match_end;
  }

  if (self->search_matches->len == 0) {
    gtk_label_set_text(GTK_LABEL(self->lbl_search_status), "0 matches");
    return;
  }

  gtk_widget_set_sensitive(self->btn_search_prev, TRUE);
  gtk_widget_set_sensitive(self->btn_search_next, TRUE);
  apply_table_search_match_highlight(self);
  jump_to_search_match(self, 0, TRUE);
}

static void show_search_ui(MarkydWindow *self) {
  if (!self || !self->search_revealer || !self->search_entry) {
    return;
  }

  gtk_revealer_set_reveal_child(GTK_REVEALER(self->search_revealer), TRUE);
  gtk_widget_grab_focus(self->search_entry);
  gtk_editable_select_region(GTK_EDITABLE(self->search_entry), 0, -1);

  if (gtk_entry_get_text_length(GTK_ENTRY(self->search_entry)) > 0) {
    update_search_matches(self);
  }
}

static void hide_search_ui(MarkydWindow *self) {
  if (!self || !self->search_revealer || !self->search_entry) {
    return;
  }

  gtk_revealer_set_reveal_child(GTK_REVEALER(self->search_revealer), FALSE);
  gtk_entry_set_text(GTK_ENTRY(self->search_entry), "");
  clear_search_matches(self);
  markyd_editor_focus(self->editor);
}

static void on_search_clicked(GtkButton *button, gpointer user_data) {
  MarkydWindow *self = (MarkydWindow *)user_data;

  (void)button;
  if (!self || !self->search_revealer) {
    return;
  }
  if (gtk_revealer_get_reveal_child(GTK_REVEALER(self->search_revealer))) {
    hide_search_ui(self);
  } else {
    show_search_ui(self);
  }
}

static void on_search_changed(GtkEditable *editable, gpointer user_data) {
  MarkydWindow *self = (MarkydWindow *)user_data;
  (void)editable;
  update_search_matches(self);
}

static void on_search_prev_clicked(GtkButton *button, gpointer user_data) {
  MarkydWindow *self = (MarkydWindow *)user_data;
  gint index;

  (void)button;

  if (!self || !self->search_matches || self->search_matches->len == 0) {
    return;
  }

  index = self->search_current_index - 1;
  if (index < 0) {
    index = (gint)self->search_matches->len - 1;
  }
  jump_to_search_match(self, index, TRUE);
}

static void on_search_next_clicked(GtkButton *button, gpointer user_data) {
  MarkydWindow *self = (MarkydWindow *)user_data;
  gint index;

  (void)button;

  if (!self || !self->search_matches || self->search_matches->len == 0) {
    return;
  }

  index = self->search_current_index + 1;
  if (index >= (gint)self->search_matches->len) {
    index = 0;
  }
  jump_to_search_match(self, index, TRUE);
}

static gboolean on_search_entry_key_press(GtkWidget *widget, GdkEventKey *event,
                                          gpointer user_data) {
  MarkydWindow *self = (MarkydWindow *)user_data;
  gboolean shift_pressed;

  (void)widget;

  if (!event) {
    return FALSE;
  }

  shift_pressed = (event->state & GDK_SHIFT_MASK) != 0;
  if (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter) {
    if (shift_pressed) {
      on_search_prev_clicked(NULL, self);
    } else {
      on_search_next_clicked(NULL, self);
    }
    return TRUE;
  }

  if (event->keyval == GDK_KEY_Escape) {
    hide_search_ui(self);
    return TRUE;
  }

  return FALSE;
}

static void on_editor_buffer_changed(GtkTextBuffer *buffer, gpointer user_data) {
  MarkydWindow *self = (MarkydWindow *)user_data;

  if (!self || !self->search_revealer || !self->search_entry) {
    return;
  }

  /* Only the active tab's buffer drives the search UI. */
  if (self->editor && self->editor->buffer != buffer) {
    return;
  }

  if (!gtk_revealer_get_reveal_child(GTK_REVEALER(self->search_revealer))) {
    return;
  }

  if (gtk_entry_get_text_length(GTK_ENTRY(self->search_entry)) == 0) {
    clear_search_matches(self);
    return;
  }

  update_search_matches(self);
}

static void setup_file_drop_target(GtkWidget *widget, MarkydWindow *self) {
  GtkTargetEntry targets[] = {{"text/uri-list", 0, 0}};

  gtk_drag_dest_set(widget, GTK_DEST_DEFAULT_ALL, targets, 1, GDK_ACTION_COPY);
  g_signal_connect(widget, "drag-data-received",
                   G_CALLBACK(on_drag_data_received), self);
}

MarkydWindow *markyd_window_new(MarkydApp *app) {
  MarkydWindow *self = g_new0(MarkydWindow, 1);
  GtkWidget *left_buttons;
  GtkWidget *main_box;
  GtkWidget *search_box;

  self->app = app;

  self->window = gtk_application_window_new(app->gtk_app);
  g_object_add_weak_pointer(G_OBJECT(self->window), (gpointer *)&self->window);
  g_signal_connect(self->window, "destroy", G_CALLBACK(on_window_destroyed),
                   self);
  gtk_window_set_title(GTK_WINDOW(self->window), "ViewMD");

  gtk_window_set_default_size(GTK_WINDOW(self->window), config->window_width,
                              config->window_height);

  if (config->window_x >= 0 && config->window_y >= 0) {
    gtk_window_move(GTK_WINDOW(self->window), config->window_x,
                    config->window_y);
  }

  if (geometry_debug_enabled()) {
    g_printerr("ViewMD geometry init: x=%d y=%d w=%d h=%d maximized=%d\n",
               config->window_x, config->window_y, config->window_width,
               config->window_height, config->window_maximized);
  }

  if (config->window_maximized) {
    gtk_window_maximize(GTK_WINDOW(self->window));
  }

  g_signal_connect(self->window, "key-press-event",
                   G_CALLBACK(on_key_press_event), self);
  g_signal_connect(self->window, "configure-event",
                   G_CALLBACK(on_configure_event), self);
  g_signal_connect(self->window, "window-state-event",
                   G_CALLBACK(on_window_state_event), self);

  setup_file_drop_target(self->window, self);

  self->header_bar = gtk_header_bar_new();
  gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(self->header_bar), TRUE);
  gtk_window_set_titlebar(GTK_WINDOW(self->window), self->header_bar);

  self->lbl_title = gtk_label_new("ViewMD");
  gtk_widget_set_halign(self->lbl_title, GTK_ALIGN_CENTER);
  gtk_header_bar_set_custom_title(GTK_HEADER_BAR(self->header_bar),
                                  self->lbl_title);

  self->btn_open =
      gtk_button_new_from_icon_name("document-open-symbolic", GTK_ICON_SIZE_BUTTON);
  gtk_widget_set_tooltip_text(self->btn_open, "Open a markdown document (Ctrl+O)");
  g_signal_connect(self->btn_open, "clicked", G_CALLBACK(on_open_clicked), self);

  self->btn_refresh =
      gtk_button_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
  gtk_widget_set_tooltip_text(self->btn_refresh, "Reload document (F5)");
  g_signal_connect(self->btn_refresh, "clicked", G_CALLBACK(on_refresh_clicked),
                   self);

  self->btn_search =
      gtk_button_new_from_icon_name("edit-find-symbolic", GTK_ICON_SIZE_BUTTON);
  gtk_widget_set_tooltip_text(self->btn_search, "Find in document (Ctrl+F)");
  g_signal_connect(self->btn_search, "clicked", G_CALLBACK(on_search_clicked),
                   self);

  self->btn_settings =
      gtk_button_new_from_icon_name("emblem-system-symbolic", GTK_ICON_SIZE_BUTTON);
  gtk_widget_set_tooltip_text(self->btn_settings, "Settings");
  g_signal_connect(self->btn_settings, "clicked", G_CALLBACK(on_settings_clicked),
                   self);

  self->btn_toc = gtk_button_new_from_icon_name("view-list-ordered-symbolic",
                                                  GTK_ICON_SIZE_BUTTON);
  gtk_widget_set_tooltip_text(self->btn_toc, "Toggle table of contents");
  g_signal_connect(self->btn_toc, "clicked", G_CALLBACK(on_toc_toggle_clicked),
                   self);

  left_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(left_buttons), self->btn_open, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(left_buttons), self->btn_refresh, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(left_buttons), self->btn_search, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(left_buttons), self->btn_toc, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(left_buttons), self->btn_settings, FALSE, FALSE, 0);
  gtk_header_bar_pack_start(GTK_HEADER_BAR(self->header_bar), left_buttons);

  main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(self->window), main_box);

  self->search_revealer = gtk_revealer_new();
  gtk_revealer_set_transition_type(GTK_REVEALER(self->search_revealer),
                                   GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
  gtk_revealer_set_reveal_child(GTK_REVEALER(self->search_revealer), FALSE);
  gtk_box_pack_start(GTK_BOX(main_box), self->search_revealer, FALSE, FALSE, 0);

  search_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_top(search_box, 6);
  gtk_widget_set_margin_bottom(search_box, 6);
  gtk_widget_set_margin_start(search_box, 8);
  gtk_widget_set_margin_end(search_box, 8);
  gtk_container_add(GTK_CONTAINER(self->search_revealer), search_box);

  self->search_entry = gtk_search_entry_new();
  gtk_widget_set_hexpand(self->search_entry, TRUE);
  gtk_widget_set_tooltip_text(self->search_entry, "Find in document");
  g_signal_connect(self->search_entry, "changed", G_CALLBACK(on_search_changed),
                   self);
  g_signal_connect(self->search_entry, "key-press-event",
                   G_CALLBACK(on_search_entry_key_press), self);
  gtk_box_pack_start(GTK_BOX(search_box), self->search_entry, TRUE, TRUE, 0);

  self->btn_search_prev =
      gtk_button_new_from_icon_name("go-up-symbolic", GTK_ICON_SIZE_BUTTON);
  gtk_widget_set_tooltip_text(self->btn_search_prev, "Previous Match");
  g_signal_connect(self->btn_search_prev, "clicked",
                   G_CALLBACK(on_search_prev_clicked), self);
  gtk_widget_set_sensitive(self->btn_search_prev, FALSE);
  gtk_box_pack_start(GTK_BOX(search_box), self->btn_search_prev, FALSE, FALSE, 0);

  self->btn_search_next =
      gtk_button_new_from_icon_name("go-down-symbolic", GTK_ICON_SIZE_BUTTON);
  gtk_widget_set_tooltip_text(self->btn_search_next, "Next Match");
  g_signal_connect(self->btn_search_next, "clicked",
                   G_CALLBACK(on_search_next_clicked), self);
  gtk_widget_set_sensitive(self->btn_search_next, FALSE);
  gtk_box_pack_start(GTK_BOX(search_box), self->btn_search_next, FALSE, FALSE, 0);

  self->lbl_search_status = gtk_label_new("");
  gtk_widget_set_halign(self->lbl_search_status, GTK_ALIGN_END);
  gtk_box_pack_start(GTK_BOX(search_box), self->lbl_search_status, FALSE, FALSE, 0);

  self->notebook = gtk_notebook_new();
  gtk_notebook_set_scrollable(GTK_NOTEBOOK(self->notebook), TRUE);
  gtk_notebook_set_show_border(GTK_NOTEBOOK(self->notebook), FALSE);
  g_signal_connect(self->notebook, "switch-page",
                   G_CALLBACK(on_notebook_switch_page), self);
  g_signal_connect(self->notebook, "page-reordered",
                   G_CALLBACK(on_notebook_page_reordered), self);
  /* GTK marks the notebook back to page 0 when it is first realized/painted,
     which demotes a document opened at startup (CLI) to the first tab.
     Re-assert the desired page once the window has been shown. */
  g_idle_add(on_notebook_reassert_page, self);

  self->paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_box_pack_start(GTK_BOX(main_box), self->paned, TRUE, TRUE, 0);

  self->toc_scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->toc_scroll),
                                 GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_size_request(self->toc_scroll, 210, -1);
  self->toc_list = gtk_list_box_new();
  gtk_widget_set_hexpand(self->toc_list, TRUE);
  gtk_style_context_add_class(gtk_widget_get_style_context(self->toc_list),
                              "viewmd-toc");
  g_signal_connect(self->toc_list, "row-activated",
                   G_CALLBACK(on_toc_row_activated), self);
  gtk_container_add(GTK_CONTAINER(self->toc_scroll), self->toc_list);
  gtk_paned_pack1(GTK_PANED(self->paned), self->toc_scroll, FALSE, FALSE);

  gtk_paned_pack2(GTK_PANED(self->paned), self->notebook, TRUE, FALSE);

  self->tabs = g_ptr_array_new_with_free_func(NULL);
  self->active_index = -1;
  self->search_matches = g_array_new(FALSE, FALSE, sizeof(SearchMatch));
  self->search_current_index = -1;

  markyd_window_new_tab(self);

  ensure_search_tags(self);

  markyd_window_apply_css(self);

  gtk_widget_show_all(self->window);
  /* show_all above would reveal the sidebar again; keep the ToC closed and
     resize the paned back to content. */
  gtk_widget_hide(self->toc_scroll);
  gtk_paned_set_position(GTK_PANED(self->paned), 210);

  return self;
}

static gboolean on_notebook_reassert_page(gpointer user_data) {
  MarkydWindow *self = (MarkydWindow *)user_data;

  if (self && self->notebook && self->active_index >= 0 &&
      self->active_index < (gint)self->tabs->len) {
    gtk_notebook_set_current_page(GTK_NOTEBOOK(self->notebook),
                                  self->active_index);
  }
  return G_SOURCE_REMOVE;
}

static void on_toc_toggle_clicked(GtkButton *button, gpointer user_data) {
  MarkydWindow *self = (MarkydWindow *)user_data;
  gboolean visible;

  (void)button;
  if (!self || !self->toc_scroll) {
    return;
  }

  visible = gtk_widget_get_visible(self->toc_scroll);
  if (visible) {
    gtk_widget_hide(self->toc_scroll);
  } else {
    update_toc(self);
    gtk_widget_show(self->toc_scroll);
  }
}

static void on_toc_row_activated(GtkListBox *list, GtkListBoxRow *row,
                                 gpointer user_data) {
  MarkydWindow *self = (MarkydWindow *)user_data;
  MarkydTab *tab;
  const gchar *slug;

  (void)list;
  if (!self) {
    return;
  }
  slug = g_object_get_data(G_OBJECT(row), "viewmd-toc-slug");
  if (!slug) {
    return;
  }

  tab = markyd_window_get_active_tab(self);
  if (tab && tab->editor) {
    markyd_editor_scroll_to_anchor(tab->editor, slug[0] != '\0' ? slug : NULL);
    markyd_editor_focus(tab->editor);
  }
}

static void update_toc(MarkydWindow *self) {
  MarkydTab *tab;
  const GPtrArray *headings;

  if (!self || !self->toc_list) {
    return;
  }

  tab = markyd_window_get_active_tab(self);
  headings = tab ? markyd_editor_get_headings(tab->editor) : NULL;

  /* Clear existing rows. */
  {
    GtkListBoxRow *row;
    while ((row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(self->toc_list),
                                                0))) {
      gtk_widget_destroy(GTK_WIDGET(row));
    }
  }

  if (!headings) {
    return;
  }

  for (guint i = 0; i < headings->len; i++) {
    ViewmdHeading *h = g_ptr_array_index(headings, i);
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *label = gtk_label_new(h->text);

    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_margin_start(label, 8 + 14 * (h->level - 1));
    gtk_widget_set_margin_top(label, 2);
    gtk_widget_set_margin_bottom(label, 2);
    g_object_set_data_full(G_OBJECT(row), "viewmd-toc-slug", g_strdup(h->slug),
                           g_free);
    gtk_container_add(GTK_CONTAINER(row), label);
    gtk_list_box_insert(GTK_LIST_BOX(self->toc_list), row, -1);
    gtk_widget_show_all(row);
  }
}

static void on_editor_rendered(MarkydEditor *editor) {
  MarkydWindow *self;
  MarkydTab *tab;

  if (!editor || !editor->app || !editor->app->window) {
    return;
  }
  self = editor->app->window;
  tab = markyd_window_get_active_tab(self);
  if (tab && tab->editor == editor && self->toc_scroll &&
      gtk_widget_get_visible(self->toc_scroll)) {
    update_toc(self);
  }
}

static gboolean on_tab_label_button_press(GtkWidget *widget,
                                          GdkEventButton *event,
                                          gpointer user_data) {
  MarkydWindow *self = (MarkydWindow *)user_data;
  MarkydTab *tab;

  if (event->button != GDK_BUTTON_MIDDLE) {
    return FALSE;
  }

  tab = g_object_get_data(G_OBJECT(widget), "viewmd-tab");
  if (tab) {
    markyd_window_close_tab(self, tab);
    return TRUE;
  }
  return FALSE;
}

static void on_notebook_switch_page(GtkNotebook *notebook, GtkWidget *page,
                                    guint page_num, gpointer user_data) {
  MarkydWindow *self = (MarkydWindow *)user_data;
  MarkydTab *tab;

  (void)notebook;
  (void)page;

  if (!self || page_num >= self->tabs->len) {
    return;
  }

  tab = g_ptr_array_index(self->tabs, page_num);
  if (self->editor != tab->editor) {
    clear_search_matches(self);
    if (self->search_revealer &&
        gtk_revealer_get_reveal_child(GTK_REVEALER(self->search_revealer))) {
      hide_search_ui(self);
    }
  }
  self->active_index = (gint)page_num;
  self->editor = tab->editor;
  if (self->toc_scroll && gtk_widget_get_visible(self->toc_scroll)) {
    update_toc(self);
  }
  markyd_app_update_window_title(self->app);
}

static void on_notebook_page_reordered(GtkNotebook *notebook, GtkWidget *child,
                                       guint page_num, gpointer user_data) {
  MarkydWindow *self = (MarkydWindow *)user_data;
  MarkydTab *tab = NULL;
  guint old_index = 0;

  if (!self || !self->tabs) {
    return;
  }
  for (guint i = 0; i < self->tabs->len; i++) {
    MarkydTab *candidate = g_ptr_array_index(self->tabs, i);
    if (candidate && candidate->scroll == child) {
      tab = candidate;
      old_index = i;
      break;
    }
  }
  if (!tab) {
    return;
  }
  g_ptr_array_remove_index(self->tabs, old_index);
  g_ptr_array_insert(self->tabs, (gint)page_num, tab);
  self->active_index = gtk_notebook_get_current_page(notebook);
  if (self->active_index >= 0 && self->active_index < (gint)self->tabs->len) {
    self->editor =
        ((MarkydTab *)g_ptr_array_index(self->tabs, (guint)self->active_index))
            ->editor;
  }
}

static void on_tab_close_clicked(GtkButton *button, gpointer user_data) {
  MarkydWindow *self = (MarkydWindow *)user_data;
  MarkydTab *tab;

  tab = g_object_get_data(G_OBJECT(button), "viewmd-tab");
  if (tab) {
    markyd_window_close_tab(self, tab);
  }
}

MarkydTab *markyd_window_new_tab(MarkydWindow *self) {
  MarkydTab *tab;
  GtkWidget *label_box;
  GtkWidget *label;
  GtkWidget *close_button;
  gint page;

  tab = g_new0(MarkydTab, 1);
  tab->editor = markyd_editor_new(self->app);
  tab->scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tab->scroll),
                                 GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_container_add(GTK_CONTAINER(tab->scroll),
                    markyd_editor_get_widget(tab->editor));

  label_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  label = gtk_label_new("Untitled");
  gtk_box_pack_start(GTK_BOX(label_box), label, FALSE, FALSE, 0);
  close_button = gtk_button_new_from_icon_name("window-close-symbolic",
                                               GTK_ICON_SIZE_MENU);
  gtk_button_set_relief(GTK_BUTTON(close_button), GTK_RELIEF_NONE);
  gtk_widget_set_focus_on_click(close_button, FALSE);
  gtk_widget_set_tooltip_text(close_button, "Close tab (middle-click or ✕)");
  g_object_set_data(G_OBJECT(close_button), "viewmd-tab", tab);
  g_signal_connect(close_button, "clicked", G_CALLBACK(on_tab_close_clicked),
                   self);
  gtk_box_pack_start(GTK_BOX(label_box), close_button, FALSE, FALSE, 0);

  /* A plain GtkBox has no GdkWindow, so the notebook delivers tab button
     events to itself and a middle-click on the label never reaches a
     handler. An event box owns the tab's input window. */
  tab->tab_label = gtk_event_box_new();
  gtk_event_box_set_visible_window(GTK_EVENT_BOX(tab->tab_label), FALSE);
  gtk_container_add(GTK_CONTAINER(tab->tab_label), label_box);
  g_object_set_data(G_OBJECT(tab->tab_label), "viewmd-tab-label-child", label);

  /* Middle-click anywhere on the tab label closes the tab. */
  gtk_widget_add_events(tab->tab_label, GDK_BUTTON_PRESS_MASK);
  g_object_set_data(G_OBJECT(tab->tab_label), "viewmd-tab", tab);
  g_signal_connect(tab->tab_label, "button-press-event",
                   G_CALLBACK(on_tab_label_button_press), self);

  /* The text view registers its own drop target (text/plain), stealing file
     drops in the document area from the window; let the window handle them.
     gtk_drag_dest_unset warns unless the widget is currently a destination,
     and a plain scrolled window never is, so only unset what was set. */
  if (gtk_drag_dest_get_target_list(tab->editor->text_view) != NULL) {
    gtk_drag_dest_unset(tab->editor->text_view);
  }
  if (gtk_drag_dest_get_target_list(tab->scroll) != NULL) {
    gtk_drag_dest_unset(tab->scroll);
  }

  page = gtk_notebook_append_page(GTK_NOTEBOOK(self->notebook), tab->scroll,
                                  tab->tab_label);
  g_ptr_array_add(self->tabs, tab);
  gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(self->notebook), tab->scroll,
                                   TRUE);
  /* Realizing a widget (show_all) after set_current_page silently resets the
     notebook back to page 0, so realize first, then select the new page. */
  gtk_widget_show_all(tab->scroll);
  gtk_widget_show_all(tab->tab_label);
  gtk_notebook_set_current_page(GTK_NOTEBOOK(self->notebook), page);
  self->active_index = page;
  self->editor = tab->editor;
  ensure_search_tags(self);
  g_signal_connect(tab->editor->buffer, "changed",
                   G_CALLBACK(on_editor_buffer_changed), self);
  g_signal_connect(tab->editor->text_view, "scroll-event",
                   G_CALLBACK(on_text_view_scroll), self);
  tab->editor->on_rendered = on_editor_rendered;

  return tab;
}

static gboolean update_tab_label(MarkydWindow *self, MarkydTab *tab) {
  GList *children;
  gchar *base;
  GtkWidget *label;

  (void)self;
  if (!tab || !tab->tab_label) {
    return FALSE;
  }

  base = NULL;
  if (tab->editor->file_path && tab->editor->file_path[0] != '\0') {
    base = g_path_get_basename(tab->editor->file_path);
  }
  label = g_object_get_data(G_OBJECT(tab->tab_label), "viewmd-tab-label-child");
  if (!label) {
    GList *node;
    children = gtk_container_get_children(GTK_CONTAINER(tab->tab_label));
    for (node = children; node != NULL; node = node->next) {
      if (GTK_IS_LABEL(node->data)) {
        label = GTK_WIDGET(node->data);
        break;
      }
    }
    g_list_free(children);
  }
  if (label) {
    gtk_label_set_text(GTK_LABEL(label), base ? base : "Untitled");
  }
  g_free(base);
  return TRUE;
}

void markyd_window_close_tab(MarkydWindow *self, MarkydTab *tab) {
  gint index;
  gboolean was_active;

  if (!self || !tab) {
    return;
  }

  if (self->tabs->len <= 1) {
    gtk_window_close(GTK_WINDOW(self->window));
    return;
  }

  index = -1;
  for (guint i = 0; i < self->tabs->len; i++) {
    if (g_ptr_array_index(self->tabs, i) == tab) {
      index = (gint)i;
      break;
    }
  }
  if (index < 0) {
    return;
  }
  was_active = (index == self->active_index);

  clear_search_matches(self);
  gtk_notebook_remove_page(GTK_NOTEBOOK(self->notebook), (gint)index);
  g_ptr_array_remove_index(self->tabs, (guint)index);
  markyd_editor_free(tab->editor);
  g_free(tab);

  if (was_active) {
    /* Drop the now-freed editor before switching: on_notebook_switch_page reads
       self->editor->buffer in clear_search_matches and would dereference freed
       memory otherwise. GTK usually selects the adjacent page itself, so read
       the resulting page back instead of assuming set_current_page fired; a
       stale active_index would silently break the next Ctrl+W. */
    gint new_index;

    self->editor = NULL;
    self->active_index = -1;
    new_index = CLAMP(index, 0, (gint)self->tabs->len - 1);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(self->notebook), new_index);
    new_index = gtk_notebook_get_current_page(GTK_NOTEBOOK(self->notebook));
    if (new_index >= 0 && new_index < (gint)self->tabs->len) {
      MarkydTab *now = g_ptr_array_index(self->tabs, (guint)new_index);
      self->active_index = new_index;
      self->editor = now->editor;
    }
  } else {
    /* Removing a page before the active one shifts its index without a
       switch-page signal. */
    gint new_index = gtk_notebook_get_current_page(GTK_NOTEBOOK(self->notebook));
    if (new_index >= 0 && new_index < (gint)self->tabs->len) {
      MarkydTab *now = g_ptr_array_index(self->tabs, (guint)new_index);
      self->active_index = new_index;
      self->editor = now->editor;
    }
  }
}

MarkydTab *markyd_window_get_active_tab(MarkydWindow *self) {
  if (!self || self->active_index < 0 ||
      self->active_index >= (gint)self->tabs->len) {
    return NULL;
  }
  return g_ptr_array_index(self->tabs, (guint)self->active_index);
}

void markyd_window_switch_tab(MarkydWindow *self, gint offset) {
  gint page;
  gint count;

  if (!self || !self->notebook || self->tabs->len == 0) {
    return;
  }
  count = gtk_notebook_get_n_pages(GTK_NOTEBOOK(self->notebook));
  if (count == 0) {
    return;
  }
  page = gtk_notebook_get_current_page(GTK_NOTEBOOK(self->notebook)) + offset;
  page = ((page % count) + count) % count;
  gtk_notebook_set_current_page(GTK_NOTEBOOK(self->notebook), page);
}

gboolean markyd_window_has_document(MarkydWindow *self, MarkydTab *tab) {
  (void)self;
  return tab && tab->editor->file_path && tab->editor->file_path[0] != '\0';
}

static gboolean load_document_into_tab(MarkydWindow *self, MarkydTab *tab,
                                       const gchar *path) {
  gchar *content = NULL;
  GError *error = NULL;

  if (!g_file_get_contents(path, &content, NULL, &error)) {
    if (error) {
      g_printerr("Failed to load markdown file '%s': %s\n", path,
                 error->message);
      g_error_free(error);
    }
    return FALSE;
  }

  markyd_editor_set_file_path(tab->editor, path);
  markyd_editor_set_content(tab->editor, content);
  g_free(content);
  update_tab_label(self, tab);
  markyd_app_update_window_title(self->app);
  return TRUE;
}

gboolean markyd_window_open_document(MarkydWindow *self, const gchar *path) {
  MarkydTab *tab;

  if (!self || !path || path[0] == '\0') {
    return FALSE;
  }

  tab = markyd_window_get_active_tab(self);
  if (tab && !markyd_window_has_document(self, tab)) {
    return load_document_into_tab(self, tab, path);
  }

  tab = markyd_window_new_tab(self);
  if (!load_document_into_tab(self, tab, path)) {
    markyd_window_close_tab(self, tab);
    return FALSE;
  }
  return TRUE;
}

void markyd_window_apply_css(MarkydWindow *self) {
  static GtkCssProvider *css = NULL;
  gchar *css_str;
  const gchar *bg;
  const gchar *fg;
  const gchar *sel_bg;
  const gchar *table_bg;
  const gchar *table_fg;
  const gchar *table_border;
  const gchar *search_match_bg;
  const gchar *search_match_fg;
  const gchar *search_current_bg;
  const gchar *search_current_fg;
  const gchar *code_bg;
  const gchar *code_fg;

  if (css) {
    gtk_style_context_remove_provider_for_screen(gdk_screen_get_default(),
                                                 GTK_STYLE_PROVIDER(css));
    g_object_unref(css);
  }

  /* Let GTK draw decorations (titlebar, borders) in the matching color scheme. */
  if (g_strcmp0(config->theme, "dark") == 0 ||
      g_strcmp0(config->theme, "light") == 0) {
    GtkSettings *gtk_settings = gtk_settings_get_default();
    g_object_set(gtk_settings, "gtk-application-prefer-dark-theme",
                 g_strcmp0(config->theme, "dark") == 0, NULL);
  }

  css = gtk_css_provider_new();

  if (g_strcmp0(config->theme, "light") == 0) {
    bg = config->background_color;
    fg = config->text_color;
    sel_bg = "#cfe3ff";
    search_match_bg = "#fff3b0";
    search_match_fg = "#1f2328";
    search_current_bg = "#ffd166";
    search_current_fg = "#1f2328";
  } else if (g_strcmp0(config->theme, "dark") == 0) {
    bg = config->background_color;
    fg = config->text_color;
    sel_bg = "#264f78";
    search_match_bg = "#3e3a12";
    search_match_fg = "#f4f4e8";
    search_current_bg = "#66551f";
    search_current_fg = "#f4f4e8";
  } else {
    bg = "@theme_base_color";
    fg = "@theme_text_color";
    sel_bg = "@theme_selected_bg_color";
    search_match_bg = "@theme_selected_bg_color";
    search_match_fg = "@theme_selected_fg_color";
    search_current_bg = "@theme_selected_bg_color";
    search_current_fg = "@theme_selected_fg_color";
  }

  code_bg = config->code_background;
  code_fg = config->code_foreground;

  table_bg = bg;
  table_fg = fg;
  table_border = sel_bg;

  css_str = g_strdup_printf(
      "textview {"
      "  font-family: '%s', sans-serif;"
      "  font-size: %dpt;"
      "  padding: 0px;"
      "  background-color: %s;"
      "  color: %s;"
      "  caret-color: %s;"
      "}"
      "textview text {"
      "  background-color: %s;"
      "  color: %s;"
      "  caret-color: %s;"
      "}"
      "textview text selection {"
      "  background-color: %s;"
      "}"
      "scrolledwindow {"
      "  background-color: %s;"
      "  border: none;"
      "}"
      "window {"
      "  background-color: %s;"
      "}"
      "headerbar {"
      "  background-color: %s;"
      "  background-image: none;"
      "  color: %s;"
      "  border-bottom: 1px solid alpha(%s, 0.2);"
      "}"
      "headerbar label {"
      "  color: %s;"
      "}"
      "headerbar button {"
      "  background-image: none;"
      "  color: %s;"
      "  border-color: alpha(%s, 0.3);"
      "}"
      "notebook {"
      "  background-color: %s;"
      "  background-image: none;"
      "}"
      "notebook header {"
      "  background-color: %s;"
      "  background-image: none;"
      "  border-bottom: 1px solid alpha(%s, 0.2);"
      "}"
      "notebook header tab {"
      "  background-color: %s;"
      "  background-image: none;"
      "  color: %s;"
      "  padding: 4px 12px 4px 12px;"
      "  border: 1px solid alpha(%s, 0.15);"
      "  border-radius: 4px 4px 0 0;"
      "  margin-right: 1px;"
      "}"
      "notebook header tab:checked {"
      "  background-color: %s;"
      "  color: %s;"
      "  border-color: alpha(%s, 0.3);"
      "}"
      "notebook header tab button {"
      "  background-image: none;"
      "  color: %s;"
      "  border: none;"
      "}"
      ".viewmd-table-cell {"
      "  background-color: %s;"
      "  border-style: solid;"
      "  border-width: 1px;"
      "  border-color: %s;"
      "  color: %s;"
      "}"
      ".viewmd-table-header-cell {"
      "  background-color: %s;"
      "  font-weight: bold;"
      "}"
      ".viewmd-table-cell label {"
      "  color: %s;"
      "}"
      ".viewmd-table-cell." VIEWMD_TABLE_CELL_MATCH_CLASS " {"
      "  background-color: %s;"
      "}"
      ".viewmd-table-cell." VIEWMD_TABLE_CELL_MATCH_CLASS " label {"
      "  color: %s;"
      "}"
      ".viewmd-table-cell." VIEWMD_TABLE_CELL_CURRENT_CLASS " {"
      "  background-color: %s;"
      "}"
      ".viewmd-table-cell." VIEWMD_TABLE_CELL_CURRENT_CLASS " label {"
      "  color: %s;"
      "}"
      ".viewmd-code-block {"
      "  background-color: %s;"
      "  border-radius: 6px;"
      "  padding: 6px 4px 6px 4px;"
      "  margin: 8px 4px 8px 4px;"
      "}"
      ".viewmd-code-header {"
      "  background-color: %s;"
      "  color: %s;"
      "  border-top-left-radius: 6px;"
      "  border-top-right-radius: 6px;"
      "  padding: 2px 8px 2px 8px;"
      "}"
      ".viewmd-code-header label {"
      "  color: %s;"
      "}"
      ".viewmd-code-block textview {"
      "  background-color: %s;"
      "  color: %s;"
      "  font-family: '%s', monospace;"
      "  font-size: %dpt;"
      "}"
      ".viewmd-code-block textview text {"
      "  background-color: %s;"
      "}"
      ".viewmd-fold-toggle {"
      "  background-color: transparent;"
      "  border: none;"
      "  padding: 0 4px 0 0;"
      "  font-family: monospace;"
      "  color: %s;"
      "}"
      ".viewmd-copy-button {"
      "  background-color: transparent;"
      "  border-radius: 4px;"
      "  padding: 0 6px 0 6px;"
      "  color: %s;"
      "}"
      ".viewmd-details-summary {"
      "  font-weight: bold;"
      "}",
      config->font_family, config->font_size, bg, fg, fg, bg, fg, fg, sel_bg, bg,
      bg, bg, fg, fg, fg, fg, fg, bg, bg, fg, bg, fg, fg, code_bg, fg, fg, fg,
      table_bg, table_border, table_fg, table_bg, table_fg, search_match_bg,
      search_match_fg, search_current_bg, search_current_fg, code_bg, code_bg,
      code_fg, code_fg, code_bg, code_fg, config->code_font_family,
      config->font_size, code_bg, fg, fg);

  gtk_css_provider_load_from_data(css, css_str, -1, NULL);
  g_free(css_str);

  gtk_style_context_add_provider_for_screen(
      gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  static GtkCssProvider *css_extra = NULL;
  {
    gchar *css_extra_str;

    if (css_extra) {
      gtk_style_context_remove_provider_for_screen(
          gdk_screen_get_default(), GTK_STYLE_PROVIDER(css_extra));
      g_object_unref(css_extra);
    }
    css_extra = gtk_css_provider_new();

    css_extra_str = g_strdup_printf(
        ".viewmd-toc { background-color: %s; }"
        ".viewmd-toc label { color: %s; }"
        ".viewmd-toc row { background-color: %s; border: none; }"
        ".viewmd-toc row:hover { background-color: %s; }"
        ".viewmd-toc row:selected { background-color: %s; color: %s; }"
        ".viewmd-code-block scrolledwindow { background-color: %s; }"
        /* Tab close button: Adwaita's hover pill is nearly invisible on the
           tab background, so paint a contrasting one. */
        "notebook header tab button { background-image: none; "
        "  background-color: transparent; border: none; border-radius: 4px; "
        "  padding: 1px; min-width: 20px; min-height: 20px; "
        "  color: alpha(%s, 0.75); }"
        "notebook header tab button:hover { background-color: alpha(%s, 0.28); "
        "  color: %s; }"
        "notebook header tab button:active { background-color: alpha(%s, 0.5); }",
        bg, fg, bg, code_bg, sel_bg, fg, code_bg, fg, fg, fg, fg);
    gtk_css_provider_load_from_data(css_extra, css_extra_str, -1, NULL);
    g_free(css_extra_str);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(), GTK_STYLE_PROVIDER(css_extra),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  }

  update_search_tag_styles(self, search_match_bg, search_match_fg,
                           search_current_bg, search_current_fg);

}

void markyd_window_free(MarkydWindow *self) {
  if (!self)
    return;

  if (self->search_matches) {
    g_array_free(self->search_matches, TRUE);
    self->search_matches = NULL;
  }

  if (self->tabs) {
    for (guint i = 0; i < self->tabs->len; i++) {
      MarkydTab *tab = g_ptr_array_index(self->tabs, i);
      if (tab) {
        markyd_editor_free(tab->editor);
        g_free(tab);
      }
    }
    g_ptr_array_free(self->tabs, TRUE);
    self->tabs = NULL;
  }

  if (self->window) {
    gtk_widget_destroy(self->window);
    self->window = NULL;
  }

  g_free(self);
}

void markyd_window_show(MarkydWindow *self) {
  gtk_widget_show(self->window);
  if (!config->window_maximized && config->window_x >= 0 && config->window_y >= 0) {
    gtk_window_move(GTK_WINDOW(self->window), config->window_x, config->window_y);
  }
  gtk_window_present(GTK_WINDOW(self->window));
}

void markyd_window_hide(MarkydWindow *self) { gtk_widget_hide(self->window); }

void markyd_window_toggle(MarkydWindow *self) {
  if (markyd_window_is_visible(self)) {
    markyd_window_hide(self);
  } else {
    markyd_window_show(self);
  }
}

gboolean markyd_window_is_visible(MarkydWindow *self) {
  return gtk_widget_get_visible(self->window);
}

static void on_open_clicked(GtkButton *button, gpointer user_data) {
  MarkydWindow *self = (MarkydWindow *)user_data;
  MarkydTab *active;
  GtkWidget *dialog;
  GtkFileFilter *md_filter;
  gint response;

  (void)button;

  dialog = gtk_file_chooser_dialog_new(
      "Open Markdown Document", GTK_WINDOW(self->window),
      GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel", GTK_RESPONSE_CANCEL, "_Open",
      GTK_RESPONSE_ACCEPT, NULL);
  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dialog), TRUE);

  /* Seed the dialog with the active document's folder, if it has one. */
  active = markyd_window_get_active_tab(self);
  if (active && active->editor && active->editor->file_path &&
      active->editor->file_path[0] != '\0') {
    gchar *dir = g_path_get_dirname(active->editor->file_path);
    if (dir) {
      gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), dir);
      g_free(dir);
    }
  }

  md_filter = gtk_file_filter_new();
  gtk_file_filter_set_name(md_filter, "Markdown files (*.md, *.markdown)");
  gtk_file_filter_add_pattern(md_filter, "*.md");
  gtk_file_filter_add_pattern(md_filter, "*.markdown");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), md_filter);

  md_filter = gtk_file_filter_new();
  gtk_file_filter_set_name(md_filter, "All files");
  gtk_file_filter_add_pattern(md_filter, "*");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), md_filter);

  response = gtk_dialog_run(GTK_DIALOG(dialog));
  if (response == GTK_RESPONSE_ACCEPT) {
    GSList *paths = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dialog));
    GSList *node;
    gboolean any_failed = FALSE;
    gchar *first_failed = NULL;

    /* Open in the order shown, and leave the last opened file active. */
    for (node = paths; node != NULL; node = node->next) {
      gchar *path = (gchar *)node->data;
      if (!markyd_window_open_document(self, path)) {
        any_failed = TRUE;
        if (!first_failed) {
          first_failed = g_strdup(path);
        }
      }
    }
    if (any_failed) {
      GtkWidget *error_dialog = gtk_message_dialog_new(
          GTK_WINDOW(self->window),
          GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR,
          GTK_BUTTONS_CLOSE, "Failed to open document");
      gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(error_dialog),
                                               "%s", first_failed);
      gtk_dialog_run(GTK_DIALOG(error_dialog));
      gtk_widget_destroy(error_dialog);
    }
    g_free(first_failed);
    g_slist_free_full(paths, g_free);
  }

  gtk_widget_destroy(dialog);
}

static void on_refresh_clicked(GtkButton *button, gpointer user_data) {
  MarkydWindow *self = (MarkydWindow *)user_data;
  MarkydTab *tab;
  const gchar *current;

  (void)button;

  tab = markyd_window_get_active_tab(self);
  current = tab ? markyd_editor_get_file_path(tab->editor) : NULL;
  if (!current || current[0] == '\0') {
    return;
  }

  if (!load_document_into_tab(self, tab, current)) {
    GtkWidget *error_dialog = gtk_message_dialog_new(
        GTK_WINDOW(self->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR,
        GTK_BUTTONS_CLOSE, "Failed to reload document");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(error_dialog),
                                             "%s", current);
    gtk_dialog_run(GTK_DIALOG(error_dialog));
    gtk_widget_destroy(error_dialog);
  }
}

static void on_settings_clicked(GtkButton *button, gpointer user_data) {
  MarkydWindow *self = (MarkydWindow *)user_data;
  GtkWidget *dialog;

  (void)button;

  dialog = create_settings_dialog(self);
  gtk_widget_show(dialog);
}

static void apply_settings_to_all_tabs(MarkydWindow *self) {
  if (!self || !self->editor) {
    return;
  }
  markdown_update_accent_tags(self->editor->buffer);
  markyd_editor_refresh(self->editor);
  markyd_window_apply_css(self);
}

static void on_settings_response(GtkDialog *dialog, gint response,
                                 gpointer user_data) {
  MarkydWindow *self = (MarkydWindow *)user_data;

  if (response == GTK_RESPONSE_APPLY) {
    config_save(config);
    apply_settings_to_all_tabs(self);
    return; /* apply without closing */
  }

  if (response == GTK_RESPONSE_OK || response == GTK_RESPONSE_ACCEPT) {
    config_save(config);
    apply_settings_to_all_tabs(self);
  }

  gtk_widget_destroy(GTK_WIDGET(dialog));
}

static gboolean on_text_view_scroll(GtkWidget *widget, GdkEventScroll *event,
                                    gpointer user_data) {
  MarkydWindow *self = (MarkydWindow *)user_data;
  gboolean zoom_in;
  gint new_size;

  (void)widget;
  if (!self || !event || (event->state & GDK_CONTROL_MASK) == 0) {
    return FALSE;
  }

  if (event->direction == GDK_SCROLL_UP ||
      (event->direction == GDK_SCROLL_SMOOTH && event->delta_y < 0)) {
    zoom_in = TRUE;
  } else if (event->direction == GDK_SCROLL_DOWN ||
             (event->direction == GDK_SCROLL_SMOOTH && event->delta_y > 0)) {
    zoom_in = FALSE;
  } else {
    return FALSE;
  }

  new_size = config->font_size + (zoom_in ? 1 : -1);
  if (new_size < 8) {
    new_size = 8;
  }
  if (new_size > 48) {
    new_size = 48;
  }

  if (new_size != config->font_size) {
    config->font_size = new_size;
    markyd_window_apply_css(self);
  }
  return TRUE;
}

static void on_drag_data_received(GtkWidget *widget, GdkDragContext *context,
                                  gint x, gint y, GtkSelectionData *data,
                                  guint info, guint time,
                                  gpointer user_data) {
  MarkydWindow *self = (MarkydWindow *)user_data;
  gchar **uris;
  gchar **uri;

  (void)widget;
  (void)x;
  (void)y;
  (void)info;

  uris = gtk_selection_data_get_uris(data);
  if (!uris) {
    gtk_drag_finish(context, FALSE, FALSE, time);
    return;
  }
  for (uri = uris; *uri != NULL; uri++) {
    gchar *path = g_filename_from_uri(*uri, NULL, NULL);
    gboolean opened = FALSE;

    if (!path) {
      continue;
    }
    for (guint i = 0; self->tabs && i < self->tabs->len; i++) {
      MarkydTab *tab = g_ptr_array_index(self->tabs, i);
      if (tab && tab->editor &&
          g_strcmp0(tab->editor->file_path, path) == 0) {
        load_document_into_tab(self, tab, path);
        gtk_notebook_set_current_page(GTK_NOTEBOOK(self->notebook), (gint)i);
        self->active_index = (gint)i;
        self->editor = tab->editor;
        opened = TRUE;
        break;
      }
    }
    if (!opened) {
      markyd_window_open_document(self, path);
    }
    g_free(path);
  }
  g_strfreev(uris);
  gtk_drag_finish(context, TRUE, FALSE, time);
}

static gboolean on_key_press_event(GtkWidget *widget, GdkEventKey *event,
                                   gpointer user_data) {
  MarkydWindow *self = (MarkydWindow *)user_data;

  if (!event || !self) {
    return FALSE;
  }

  if ((event->state & GDK_CONTROL_MASK) != 0 &&
      (event->keyval == GDK_KEY_Tab ||
       event->keyval == GDK_KEY_ISO_Left_Tab)) {
    gboolean shift = (event->state & GDK_SHIFT_MASK) != 0 ||
                     event->keyval == GDK_KEY_ISO_Left_Tab;
    markyd_window_switch_tab(self, shift ? -1 : 1);
    return TRUE;
  }

  if ((event->state & GDK_CONTROL_MASK) != 0 &&
      (event->keyval == GDK_KEY_Page_Up ||
       event->keyval == GDK_KEY_KP_Page_Up)) {
    markyd_window_switch_tab(self, -1);
    return TRUE;
  }

  if ((event->state & GDK_CONTROL_MASK) != 0 &&
      (event->keyval == GDK_KEY_Page_Down ||
       event->keyval == GDK_KEY_KP_Page_Down)) {
    markyd_window_switch_tab(self, 1);
    return TRUE;
  }

  if ((event->state & GDK_CONTROL_MASK) != 0 &&
      (event->keyval == GDK_KEY_w || event->keyval == GDK_KEY_W)) {
    MarkydTab *tab = markyd_window_get_active_tab(self);
    if (tab) {
      markyd_window_close_tab(self, tab);
    }
    return TRUE;
  }

  if ((event->state & GDK_CONTROL_MASK) != 0 &&
      (event->keyval == GDK_KEY_q || event->keyval == GDK_KEY_Q)) {
    gtk_window_close(GTK_WINDOW(widget));
    return TRUE;
  }

  if ((event->state & GDK_CONTROL_MASK) != 0 &&
      (event->keyval == GDK_KEY_o || event->keyval == GDK_KEY_O)) {
    on_open_clicked(NULL, self);
    return TRUE;
  }

  if ((event->state & GDK_CONTROL_MASK) != 0 &&
      (event->keyval == GDK_KEY_f || event->keyval == GDK_KEY_F)) {
    show_search_ui(self);
    return TRUE;
  }

  if (event->keyval == GDK_KEY_F5) {
    on_refresh_clicked(NULL, self);
    return TRUE;
  }

  if (event->keyval == GDK_KEY_Escape &&
      self->search_revealer &&
      gtk_revealer_get_reveal_child(GTK_REVEALER(self->search_revealer))) {
    hide_search_ui(self);
    return TRUE;
  }

  if (event->keyval == GDK_KEY_Escape) {
    gtk_window_close(GTK_WINDOW(widget));
    return TRUE;
  }

  if (event->keyval == GDK_KEY_Home || event->keyval == GDK_KEY_KP_Home) {
    GtkWidget *scroll;

    if (self->editor && self->editor->buffer && self->editor->text_view) {
      GtkTextIter start;
      GtkAdjustment *hadj = NULL;
      GtkAdjustment *vadj = NULL;

      gtk_text_buffer_get_start_iter(self->editor->buffer, &start);
      gtk_text_buffer_place_cursor(self->editor->buffer, &start);
      gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(self->editor->text_view), &start,
                                   0.0, FALSE, 0.0, 0.0);

      scroll = markyd_window_get_active_tab(self)
                   ? markyd_window_get_active_tab(self)->scroll
                   : NULL;
      if (scroll) {
        hadj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scroll));
        vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
      }
      if (hadj) {
        gtk_adjustment_set_value(hadj, gtk_adjustment_get_lower(hadj));
      }
      if (vadj) {
        gtk_adjustment_set_value(vadj, gtk_adjustment_get_lower(vadj));
      }
    }
    return TRUE;
  }

  return FALSE;
}

static gboolean on_configure_event(GtkWidget *widget, GdkEventConfigure *event,
                                   gpointer user_data) {
  MarkydWindow *self = (MarkydWindow *)user_data;
  (void)self;

  GdkWindow *gdk_window = gtk_widget_get_window(widget);
  if (gdk_window) {
    GdkWindowState state = gdk_window_get_state(gdk_window);
    if (state & GDK_WINDOW_STATE_MAXIMIZED) {
      return FALSE;
    }
  } else if (config->window_maximized) {
    return FALSE;
  }

  {
    gint x, y;
    gint width, height;
    gint gtk_w = 0, gtk_h = 0;
    GdkRectangle frame_extents;
    gboolean have_extents = FALSE;

    gtk_window_get_position(GTK_WINDOW(widget), &x, &y);
    config->window_x = x;
    config->window_y = y;

    gtk_window_get_size(GTK_WINDOW(widget), &gtk_w, &gtk_h);

    width = gtk_w > 0 ? gtk_w : event->width;
    height = gtk_h > 0 ? gtk_h : event->height;

    if (gdk_window) {
      gdk_window_get_frame_extents(gdk_window, &frame_extents);
      have_extents = TRUE;
    }

    config->window_width = width;
    config->window_height = height;

    if (geometry_debug_enabled()) {
      if (have_extents) {
        g_printerr(
            "ViewMD configure: event=%dx%d gtk=%dx%d saved=%dx%d frame=%dx%d at (%d,%d)\n",
            event->width, event->height, gtk_w, gtk_h, width, height,
            frame_extents.width, frame_extents.height, x, y);
      } else {
        g_printerr("ViewMD configure: event=%dx%d gtk=%dx%d saved=%dx%d at (%d,%d)\n",
                   event->width, event->height, gtk_w, gtk_h, width, height, x,
                   y);
      }
      g_printerr("ViewMD saved: x=%d y=%d w=%d h=%d\n", config->window_x,
                 config->window_y, config->window_width, config->window_height);
    }
  }

  return FALSE;
}

static void on_window_destroyed(GtkWidget *widget, gpointer user_data) {
  MarkydWindow *self = (MarkydWindow *)user_data;
  (void)widget;
  if (self) {
    self->window = NULL;
  }
}

static gboolean on_window_state_event(GtkWidget *widget,
                                      GdkEventWindowState *event,
                                      gpointer user_data) {
  (void)widget;
  (void)user_data;

  config->window_maximized =
      (event->new_window_state & GDK_WINDOW_STATE_MAXIMIZED) != 0;

  return FALSE;
}

typedef struct {
  MarkydWindow *win;
  GtkWidget *theme_combo;
  GtkWidget *font_combo;
  GtkWidget *code_font_combo;
  GArray *color_rows; /* SettingsColorRow */
} SettingsUi;

typedef struct {
  GtkWidget *button;
  gchar **target;
} SettingsColorRow;

static void settings_ui_free(gpointer data) {
  SettingsUi *ui = (SettingsUi *)data;
  if (ui) {
    if (ui->color_rows) {
      g_array_free(ui->color_rows, TRUE);
    }
    g_free(ui);
  }
}

static void populate_font_combo(GtkComboBoxText *combo, gboolean mono_only,
                                const gchar *current) {
  PangoFontMap *map;
  PangoFontFamily **families = NULL;
  gint n_families = 0;
  gint select_index = 0;
  gint index = 0;
  gboolean found = FALSE;

  map = pango_cairo_font_map_get_default();
  if (!map) {
    return;
  }

  pango_font_map_list_families(map, &families, &n_families);

  for (gint i = 0; i < n_families; i++) {
    const gchar *family_name = pango_font_family_get_name(families[i]);
    if (mono_only && !pango_font_family_is_monospace(families[i])) {
      continue;
    }
    gtk_combo_box_text_append_text(combo, family_name);
    if (!found && g_strcmp0(family_name, current) == 0) {
      select_index = index;
      found = TRUE;
    }
    index++;
  }

  if (!found && current && current[0] != '\0') {
    select_index = index;
    gtk_combo_box_text_append_text(combo, current);
  }
  g_free(families);

  gtk_combo_box_set_active(GTK_COMBO_BOX(combo), select_index);
}

static void on_font_size_changed(GtkSpinButton *spin, gpointer user_data) {
  (void)user_data;
  config->font_size = gtk_spin_button_get_value_as_int(spin);
}

static void on_font_family_changed(GtkComboBoxText *combo, gpointer user_data) {
  (void)user_data;
  g_free(config->font_family);
  config->font_family = gtk_combo_box_text_get_active_text(combo);
}

static void on_code_font_family_changed(GtkComboBoxText *combo,
                                        gpointer user_data) {
  (void)user_data;
  g_free(config->code_font_family);
  config->code_font_family = gtk_combo_box_text_get_active_text(combo);
}

static void on_theme_changed(GtkComboBoxText *combo, gpointer user_data) {
  SettingsUi *ui = (SettingsUi *)user_data;

  g_free(config->theme);
  config->theme = gtk_combo_box_text_get_active_text(combo);

  /* Switch the color palette to the newly selected theme. */
  config_apply_theme_colors(config);
  if (ui && ui->color_rows) {
    for (guint i = 0; i < ui->color_rows->len; i++) {
      SettingsColorRow *row =
          &g_array_index(ui->color_rows, SettingsColorRow, i);
      init_color_button(GTK_COLOR_BUTTON(row->button), *row->target);
    }
  }
}

static SettingsColorRow add_color_row(GtkWidget *grid, gint *row,
                                      const gchar *label_text, gchar **target,
                                      SettingsUi *ui) {
  GtkWidget *label;
  GtkWidget *btn;
  SettingsColorRow color_row;

  label = gtk_label_new(label_text);
  gtk_widget_set_halign(label, GTK_ALIGN_END);
  gtk_grid_attach(GTK_GRID(grid), label, 0, *row, 1, 1);

  btn = gtk_color_button_new();
  init_color_button(GTK_COLOR_BUTTON(btn), *target);
  gtk_widget_set_halign(btn, GTK_ALIGN_START);
  g_signal_connect(btn, "color-set", G_CALLBACK(on_color_set), target);
  gtk_grid_attach(GTK_GRID(grid), btn, 1, (*row)++, 1, 1);

  color_row.button = btn;
  color_row.target = target;
  if (ui->color_rows) {
    g_array_append_val(ui->color_rows, color_row);
  }
  return color_row;
}

static void add_section_label(GtkWidget *grid, gint *row, const gchar *text) {
  GtkWidget *label = gtk_label_new(text);
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(label), "dim-label");
  gtk_grid_attach(GTK_GRID(grid), label, 0, (*row)++, 2, 1);
}

static GtkWidget *create_settings_dialog(MarkydWindow *win) {
  GtkWidget *dialog;
  GtkWidget *content;
  GtkWidget *grid;
  GtkWidget *label;
  GtkWidget *font_size_spin;
  SettingsUi *ui;
  gint row = 0;

  ui = g_new0(SettingsUi, 1);
  ui->win = win;
  ui->color_rows = g_array_new(FALSE, FALSE, sizeof(SettingsColorRow));

  dialog = gtk_dialog_new_with_buttons(
      "ViewMD Settings", GTK_WINDOW(win->window),
      GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "_Cancel",
      GTK_RESPONSE_CANCEL, "_Apply", GTK_RESPONSE_APPLY, "_OK",
      GTK_RESPONSE_OK, NULL);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
  g_signal_connect(dialog, "response", G_CALLBACK(on_settings_response), win);
  g_object_set_data_full(G_OBJECT(dialog), "viewmd-settings-ui", ui,
                         settings_ui_free);

  gtk_window_set_default_size(GTK_WINDOW(dialog), 460, -1);

  content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_container_set_border_width(GTK_CONTAINER(content), 16);
  gtk_widget_set_margin_bottom(content, 12);

  grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
  gtk_container_add(GTK_CONTAINER(content), grid);

  label = gtk_label_new("Theme:");
  gtk_widget_set_halign(label, GTK_ALIGN_END);
  gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);

  ui->theme_combo = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui->theme_combo), "dark");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui->theme_combo), "light");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui->theme_combo), "system");

  if (g_strcmp0(config->theme, "light") == 0)
    gtk_combo_box_set_active(GTK_COMBO_BOX(ui->theme_combo), 1);
  else if (g_strcmp0(config->theme, "system") == 0)
    gtk_combo_box_set_active(GTK_COMBO_BOX(ui->theme_combo), 2);
  else
    gtk_combo_box_set_active(GTK_COMBO_BOX(ui->theme_combo), 0);

  g_signal_connect(ui->theme_combo, "changed", G_CALLBACK(on_theme_changed),
                   ui);
  gtk_widget_set_hexpand(ui->theme_combo, TRUE);
  gtk_grid_attach(GTK_GRID(grid), ui->theme_combo, 1, row++, 1, 1);

  label = gtk_label_new("Font:");
  gtk_widget_set_halign(label, GTK_ALIGN_END);
  gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);

  ui->font_combo = gtk_combo_box_text_new();
  populate_font_combo(GTK_COMBO_BOX_TEXT(ui->font_combo), FALSE,
                      config->font_family);
  g_signal_connect(ui->font_combo, "changed", G_CALLBACK(on_font_family_changed),
                   NULL);
  gtk_widget_set_hexpand(ui->font_combo, TRUE);
  gtk_grid_attach(GTK_GRID(grid), ui->font_combo, 1, row++, 1, 1);

  label = gtk_label_new("Code font:");
  gtk_widget_set_halign(label, GTK_ALIGN_END);
  gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);

  ui->code_font_combo = gtk_combo_box_text_new();
  populate_font_combo(GTK_COMBO_BOX_TEXT(ui->code_font_combo), TRUE,
                      config->code_font_family);
  g_signal_connect(ui->code_font_combo, "changed",
                   G_CALLBACK(on_code_font_family_changed), NULL);
  gtk_widget_set_hexpand(ui->code_font_combo, TRUE);
  gtk_grid_attach(GTK_GRID(grid), ui->code_font_combo, 1, row++, 1, 1);

  label = gtk_label_new("Font size:");
  gtk_widget_set_halign(label, GTK_ALIGN_END);
  gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);

  font_size_spin = gtk_spin_button_new_with_range(8, 48, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(font_size_spin), config->font_size);
  g_signal_connect(font_size_spin, "value-changed",
                   G_CALLBACK(on_font_size_changed), NULL);
  gtk_grid_attach(GTK_GRID(grid), font_size_spin, 1, row++, 1, 1);

  add_section_label(grid, &row, "Document");
  add_color_row(grid, &row, "Background:", &config->background_color, ui);
  add_color_row(grid, &row, "Text:", &config->text_color, ui);

  add_section_label(grid, &row, "Code");
  add_color_row(grid, &row, "Code block:", &config->code_background, ui);
  add_color_row(grid, &row, "Code text:", &config->code_foreground, ui);
  add_color_row(grid, &row, "Inline code:", &config->inline_code_background,
                ui);
  add_color_row(grid, &row, "Inline text:", &config->inline_code_foreground,
                ui);

  add_section_label(grid, &row, "Accents");
  add_color_row(grid, &row, "Heading 1:", &config->h1_color, ui);
  add_color_row(grid, &row, "Heading 2:", &config->h2_color, ui);
  add_color_row(grid, &row, "Heading 3:", &config->h3_color, ui);
  add_color_row(grid, &row, "List bullet:", &config->list_bullet_color, ui);

  gtk_widget_show_all(dialog);

  return dialog;
}

static gchar *rgba_to_hex(const GdkRGBA *rgba) {
  guint r = (guint)(CLAMP(rgba->red, 0.0, 1.0) * 255.0 + 0.5);
  guint g = (guint)(CLAMP(rgba->green, 0.0, 1.0) * 255.0 + 0.5);
  guint b = (guint)(CLAMP(rgba->blue, 0.0, 1.0) * 255.0 + 0.5);
  return g_strdup_printf("#%02X%02X%02X", r, g, b);
}

static void init_color_button(GtkColorButton *btn, const gchar *color_str) {
  GdkRGBA rgba;
  if (color_str && gdk_rgba_parse(&rgba, color_str)) {
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(btn), &rgba);
  }
}

static void on_color_set(GtkColorButton *btn, gpointer user_data) {
  gchar **target = (gchar **)user_data;
  GdkRGBA rgba;

  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(btn), &rgba);
  g_free(*target);
  *target = rgba_to_hex(&rgba);
}

