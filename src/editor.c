#include "editor.h"
#include "app.h"
#include "markdown.h"
#include "window.h"
#include "config.h"
#include <gio/gio.h>
#include <math.h>
#include <string.h>

static gboolean on_button_release(GtkWidget *widget, GdkEventButton *event,
                                  gpointer user_data);
static gboolean on_motion_notify(GtkWidget *widget, GdkEventMotion *event,
                                 gpointer user_data);
static gboolean on_leave_notify(GtkWidget *widget, GdkEventCrossing *event,
                                gpointer user_data);
static void on_view_source_activate(GtkMenuItem *item, gpointer user_data);
static void on_populate_popup(GtkTextView *text_view, GtkWidget *menu,
                              gpointer user_data);
static void on_text_view_destroyed(GtkWidget *widget, gpointer user_data);
static void apply_markdown(MarkydEditor *self);
static void schedule_markdown_apply(MarkydEditor *self);
static gboolean relayout_timeout(gpointer user_data);
static void render_image_widgets(MarkydEditor *self);
static void render_table_widgets(MarkydEditor *self);
static void render_frontmatter_widgets(MarkydEditor *self);
static void render_code_block_widgets(MarkydEditor *self);
static void render_fold_widgets(MarkydEditor *self);
static void refresh_image_widget_scales(MarkydEditor *self);
static gboolean resolve_image_source_path(MarkydEditor *self, const gchar *src,
                                           gchar **out_path);
static void update_scroll_anchor(MarkydEditor *self);
static gboolean restore_scroll_anchor(MarkydEditor *self, gboolean prefer_heading);
static void schedule_scroll_anchor_restore(MarkydEditor *self);
static gboolean restore_scroll_anchor_idle(gpointer user_data);
static void on_vadjustment_value_changed(GtkAdjustment *adjustment,
                                         gpointer user_data);
static void on_text_view_size_allocate_before(GtkWidget *widget,
                                              GtkAllocation *allocation,
                                              gpointer user_data);
static void on_text_view_size_allocate_after(GtkWidget *widget,
                                             GtkAllocation *allocation,
                                             gpointer user_data);

static gchar *get_url_from_iter_tags(GtkTextIter *iter) {
  GSList *tags;
  gchar *url = NULL;

  if (!iter) {
    return NULL;
  }

  tags = gtk_text_iter_get_tags(iter);
  for (GSList *node = tags; node != NULL; node = node->next) {
    GtkTextTag *tag = GTK_TEXT_TAG(node->data);
    const gchar *stored =
        (const gchar *)g_object_get_data(G_OBJECT(tag), VIEWMD_LINK_URL_DATA);
    if (stored && stored[0] != '\0') {
      url = g_strdup(stored);
      break;
    }
  }
  g_slist_free(tags);
  return url;
}

static gboolean get_link_url_at_iter(GtkTextBuffer *buffer, GtkTextIter *at,
                                     gchar **out_url) {
  GtkTextIter hit;

  if (!buffer || !at || !out_url) {
    return FALSE;
  }
  *out_url = NULL;

  hit = *at;
  *out_url = get_url_from_iter_tags(&hit);
  if (*out_url) {
    return TRUE;
  }

  hit = *at;
  if (gtk_text_iter_backward_char(&hit)) {
    *out_url = get_url_from_iter_tags(&hit);
    if (*out_url) {
      return TRUE;
    }
  }

  hit = *at;
  if (gtk_text_iter_forward_char(&hit)) {
    *out_url = get_url_from_iter_tags(&hit);
    if (*out_url) {
      return TRUE;
    }
  }

  return FALSE;
}

static gboolean scroll_to_markdown_anchor(MarkydEditor *self,
                                          const gchar *fragment) {
  gchar *mark_name;
  GtkTextMark *mark;

  if (!self || !self->buffer || !fragment) {
    return FALSE;
  }

  if (fragment[0] == '\0') {
    GtkTextIter start;
    gtk_text_buffer_get_start_iter(self->buffer, &start);
    gtk_text_buffer_place_cursor(self->buffer, &start);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(self->text_view), &start, 0.2,
                                 FALSE, 0.0, 0.0);
    return TRUE;
  }

  mark_name = markdown_anchor_mark_name(fragment);
  mark = gtk_text_buffer_get_mark(self->buffer, mark_name);
  g_free(mark_name);
  if (!mark) {
    return FALSE;
  }

  {
    GtkTextIter at;
    gtk_text_buffer_get_iter_at_mark(self->buffer, &at, mark);
    gtk_text_buffer_place_cursor(self->buffer, &at);
  }
  /* within_margin must be a fraction of the viewport in [0, 0.5); anything
     larger fails the assertion and silently skips the scroll. use_align
     with yalign 0 puts the heading line at the top edge. */
  gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(self->text_view), mark, 0.0, TRUE,
                               0.0, 0.0);
  return TRUE;
}

static void set_link_cursor(MarkydEditor *self, gboolean active) {
  GdkWindow *win = gtk_text_view_get_window(GTK_TEXT_VIEW(self->text_view),
                                            GTK_TEXT_WINDOW_TEXT);
  if (!win) {
    return;
  }

  if (active) {
    GdkDisplay *display = gdk_window_get_display(win);
    GdkCursor *cursor = gdk_cursor_new_from_name(display, "pointer");
    if (!cursor) {
      cursor = gdk_cursor_new_for_display(display, GDK_HAND2);
    }
    gdk_window_set_cursor(win, cursor);
    if (cursor) {
      g_object_unref(cursor);
    }
  } else {
    gdk_window_set_cursor(win, NULL);
  }
}

static void apply_markdown(MarkydEditor *self) {
  gboolean preserve_scroll = FALSE;

  if (!self || !self->buffer || !self->text_view) {
    return;
  }

  /* A source tab is not rendered: show the raw markdown verbatim. */
  if (self->source_view) {
    self->updating_tags = TRUE;
    if (self->headings) {
      g_ptr_array_set_size(self->headings, 0);
    }
    gtk_text_buffer_set_text(self->buffer,
                             self->source_content ? self->source_content : "",
                             -1);
    self->updating_tags = FALSE;
    if (self->on_rendered) {
      self->on_rendered(self);
    }
    return;
  }

  self->updating_tags = TRUE;
  /* Rebuilding the buffer scrolls it to the top, so remember the anchored
     line from the old layout first and restore it once the new document has
     been laid out. Freezing keeps the adjustment's own value-changed handler
     from overwriting the captured anchor while the buffer is cleared. */
  if (!self->in_resize_pass) {
    update_scroll_anchor(self);
    if (self->scroll_anchor_valid) {
      self->scroll_anchor_frozen = TRUE;
      preserve_scroll = TRUE;
    }
  }
  if (self->headings) {
    g_ptr_array_set_size(self->headings, 0);
  }
  markdown_apply_tags(self->buffer,
                      self->source_content ? self->source_content : "",
                      self->collapsed, self->headings);
  render_image_widgets(self);
  /* The image permission prompt runs a nested loop; the window may have been
     closed while it was open. */
  if (!self->buffer || !self->text_view) {
    self->scroll_anchor_frozen = FALSE;
    self->updating_tags = FALSE;
    return;
  }
  render_table_widgets(self);
  render_frontmatter_widgets(self);
  render_code_block_widgets(self);
  render_fold_widgets(self);
  refresh_image_widget_scales(self);
  if (preserve_scroll) {
    schedule_scroll_anchor_restore(self);
  } else {
    self->scroll_anchor_frozen = FALSE;
    update_scroll_anchor(self);
  }
  self->updating_tags = FALSE;
  if (self->on_rendered) {
    self->on_rendered(self);
  }
}

static gboolean resolve_image_source_path(MarkydEditor *self, const gchar *src,
                                          gchar **out_path) {
  gchar *path = NULL;

  if (out_path) {
    *out_path = NULL;
  }
  if (!self || !src || src[0] == '\0' || !out_path) {
    return FALSE;
  }

  if (g_uri_parse_scheme(src) != NULL) {
    if (g_str_has_prefix(src, "file://")) {
      path = g_filename_from_uri(src, NULL, NULL);
    } else {
      return FALSE;
    }
  } else if (g_path_is_absolute(src)) {
    path = g_strdup(src);
  } else {
    const gchar *current_path = self->file_path;
    if (current_path && current_path[0] != '\0') {
      gchar *dir = g_path_get_dirname(current_path);
      path = g_build_filename(dir, src, NULL);
      g_free(dir);
    } else {
      path = g_strdup(src);
    }
  }

  if (!path) {
    return FALSE;
  }
  if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
    g_free(path);
    return FALSE;
  }

  *out_path = path;
  return TRUE;
}

/* Text area width inside the text view margins, or 0 while the view is not
   laid out yet. */
static gint editor_content_width(MarkydEditor *self) {
  GdkRectangle rect;
  gint width;

  if (!self || !self->text_view) {
    return 0;
  }
  gtk_text_view_get_visible_rect(GTK_TEXT_VIEW(self->text_view), &rect);
  width = rect.width - gtk_text_view_get_left_margin(GTK_TEXT_VIEW(self->text_view)) -
          gtk_text_view_get_right_margin(GTK_TEXT_VIEW(self->text_view));
  if (width < 320 || width > 2400) {
    return 0;
  }
  return width;
}

static gint get_image_max_width(MarkydEditor *self) {
  GtkAllocation alloc;
  gint width;

  if (!self || !self->text_view) {
    return 0;
  }

  gtk_widget_get_allocation(self->text_view, &alloc);
  width = alloc.width - gtk_text_view_get_left_margin(GTK_TEXT_VIEW(self->text_view)) -
          gtk_text_view_get_right_margin(GTK_TEXT_VIEW(self->text_view)) - 24;
  return MAX(width, 64);
}

static void scale_image_widget(GtkWidget *widget, gint max_width) {
  GtkWidget *image;
  GdkPixbuf *orig;
  gint ow;
  gint oh;

  if (!widget || max_width <= 0) {
    return;
  }

  image = g_object_get_data(G_OBJECT(widget), "viewmd-image-widget-child");
  orig = g_object_get_data(G_OBJECT(widget), "viewmd-image-orig-pixbuf");
  if (!GTK_IS_IMAGE(image) || !GDK_IS_PIXBUF(orig)) {
    return;
  }

  ow = gdk_pixbuf_get_width(orig);
  oh = gdk_pixbuf_get_height(orig);
  if (ow <= 0 || oh <= 0) {
    return;
  }

  if (ow > max_width) {
    gint nh = (gint)(((gdouble)oh * (gdouble)max_width) / (gdouble)ow);
    GdkPixbuf *scaled =
        gdk_pixbuf_scale_simple(orig, max_width, MAX(nh, 1), GDK_INTERP_BILINEAR);
    gtk_image_set_from_pixbuf(GTK_IMAGE(image), scaled);
    if (scaled) {
      g_object_unref(scaled);
    }
  } else {
    gtk_image_set_from_pixbuf(GTK_IMAGE(image), orig);
  }
}

static void refresh_image_widget_scales(MarkydEditor *self) {
  GtkTextIter iter;
  GtkTextIter end;
  gint max_width;

  if (!self || !self->buffer) {
    return;
  }

  max_width = get_image_max_width(self);
  gtk_text_buffer_get_bounds(self->buffer, &iter, &end);
  while (!gtk_text_iter_equal(&iter, &end)) {
    GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(&iter);
    if (anchor &&
        g_object_get_data(G_OBJECT(anchor), VIEWMD_IMAGE_ANCHOR_DATA) != NULL) {
      GtkWidget *image_widget =
          g_object_get_data(G_OBJECT(anchor), VIEWMD_IMAGE_WIDGET_DATA);
      if (image_widget) {
        scale_image_widget(image_widget, max_width);
      }
    }
    gtk_text_iter_forward_char(&iter);
  }
}

static GtkWidget *make_image_widget_with_pixbuf(GdkPixbuf *orig,
                                                gint max_width) {
  GtkWidget *event_box;
  GtkWidget *image;

  event_box = gtk_event_box_new();
  image = gtk_image_new();
  gtk_event_box_set_visible_window(GTK_EVENT_BOX(event_box), FALSE);
  gtk_widget_set_halign(event_box, GTK_ALIGN_START);
  gtk_container_add(GTK_CONTAINER(event_box), image);
  g_object_set_data(G_OBJECT(event_box), "viewmd-image-widget-child", image);
  g_object_set_data_full(G_OBJECT(event_box), "viewmd-image-orig-pixbuf", orig,
                         g_object_unref);
  scale_image_widget(event_box, max_width);
  return event_box;
}

/* ------------------------------------------------------------------ */
/* Remote (http/https) image support: session cache, per-doc permission,
   and asynchronous fetching on a worker thread. Fetching is opt-in:
   remote_images is "ask" (default), "always", or "never", and the ask
   prompt is shown once per document, Thunderbird-style. */

static GHashTable *remote_pixbuf_cache; /* url -> GdkPixbuf* (ref held) */
static GHashTable *remote_inflight;     /* url -> gpointer */

static gboolean is_remote_src(const gchar *src) {
  return g_str_has_prefix(src, "https://");
}

static GdkPixbuf *remote_cache_lookup(const gchar *url) {
  if (!remote_pixbuf_cache) {
    return NULL;
  }
  return g_hash_table_lookup(remote_pixbuf_cache, url);
}

static void remote_cache_store(const gchar *url, GdkPixbuf *pixbuf) {
  if (!remote_pixbuf_cache) {
    remote_pixbuf_cache =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
  }
  g_hash_table_replace(remote_pixbuf_cache, g_strdup(url), pixbuf);
}

static gboolean remote_permission_allowed(MarkydEditor *self) {
  if (g_strcmp0(config->remote_images, "always") == 0) {
    return TRUE;
  }
  if (g_strcmp0(config->remote_images, "never") == 0) {
    return FALSE;
  }
  /* Do not open a modal prompt while the window is being destroyed. */
  if (!self->app || !self->app->window || !self->app->window->window) {
    return FALSE;
  }

  if (!self->remote_images_decided) {
    GtkWidget *dialog;
    gint response;

    self->remote_images_decided = TRUE;
    self->remote_images_allowed = FALSE;

    dialog = gtk_message_dialog_new(
        self->app && self->app->window ? GTK_WINDOW(self->app->window->window)
                                       : NULL,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_YES_NO,
        "This document references remote images.\nLoad them from the web?");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
                                             "%s",
                                             self->file_path
                                                 ? self->file_path
                                                 : "(untitled)");
    g_object_add_weak_pointer(G_OBJECT(dialog), (gpointer *)&dialog);
    response = gtk_dialog_run(GTK_DIALOG(dialog));
    if (dialog) {
      g_object_remove_weak_pointer(G_OBJECT(dialog), (gpointer *)&dialog);
      gtk_widget_destroy(dialog);
      dialog = NULL;
    }
    self->remote_images_allowed = (response == GTK_RESPONSE_YES);
  }
  return self->remote_images_allowed;
}

typedef struct {
  gchar *url;
  guchar *bytes;
  gsize length;
} RemoteResult;

static gboolean http_fetch(const gchar *url, guchar **out, gsize *out_len) {
  GSocketClient *client;
  GSocketConnectable *addr;
  GSocketConnection *conn = NULL;
  GOutputStream *output;
  GInputStream *input;
  GString *all;
  const gchar *hr;
  const gchar *body;
  gsize body_len;
  gchar *host = NULL;
  gchar *path = NULL;
  gint port;
  gchar *request;
  GError *error = NULL;
  gboolean ok = FALSE;
  gboolean https;

  https = g_str_has_prefix(url, "https://");
  port = https ? 443 : 80;

  {
    const gchar *p = strstr(url, "://");
    if (!p) {
      return FALSE;
    }
    p += 3;
    const gchar *path_start = strchr(p, '/');
    const gchar *colon = strchr(p, ':');
    const gchar *end = NULL;
    if (colon && (!path_start || colon < path_start)) {
      end = colon;
      port = atoi(colon + 1);
    } else if (path_start) {
      end = path_start;
    }
    host = g_strndup(p, end ? (gsize)(end - p) : strlen(p));
    path = path_start ? g_strdup(path_start) : g_strdup("/");
  }

  client = g_socket_client_new();
  g_socket_client_set_timeout(client, 15);
  if (https) {
    g_socket_client_set_tls(client, TRUE);
  }
  addr = g_network_address_new(host, port);
  conn = g_socket_client_connect(client, addr, NULL, &error);
  g_object_unref(addr);
  if (!conn) {
    g_clear_error(&error);
    goto out;
  }

  request = g_strdup_printf(
      "GET %s HTTP/1.1\r\n"
      "Host: %s\r\n"
      "User-Agent: ViewMD/1.0\r\n"
      "Accept: image/*\r\n"
      "Connection: close\r\n\r\n",
      path, host);
  output = g_io_stream_get_output_stream(G_IO_STREAM(conn));
  input = g_io_stream_get_input_stream(G_IO_STREAM(conn));
  if (!g_output_stream_write_all(output, request, strlen(request), NULL, NULL,
                                 &error)) {
    g_clear_error(&error);
    g_free(request);
    goto out;
  }
  g_free(request);

  all = g_string_new(NULL);
  {
    guchar buf[8192];
    gssize n;
    while ((n = g_input_stream_read(input, buf, sizeof buf, NULL, &error)) >
           0) {
      if (all->len + (gsize)n > 16u * 1024u * 1024u) {
        error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                                    "remote image is too large");
        break;
      }
      g_string_append_len(all, (gchar *)buf, (gsize)n);
    }
  }
  if (error) {
    g_clear_error(&error);
    g_string_free(all, TRUE);
    goto out;
  }

  hr = strstr(all->str, "\r\n\r\n");
  body = hr ? hr + 4 : all->str;
  body_len = all->len - (hr ? (gsize)(hr + 4 - all->str) : 0);
  if (body_len > 0) {
    *out = g_memdup2(body, body_len);
    *out_len = body_len;
    ok = TRUE;
  }
  g_string_free(all, TRUE);
  g_object_unref(client);
  client = NULL;

out:
  if (conn) {
    g_object_unref(conn);
  }
  if (client) {
    g_object_unref(client);
  }
  g_free(host);
  g_free(path);
  return ok;
}

static gboolean on_remote_fetched(gpointer data);

static gpointer remote_fetch_thread(gpointer data) {
  RemoteResult *result = data;
  result->bytes = NULL;
  result->length = 0;
  http_fetch(result->url, &result->bytes, &result->length);
  g_idle_add(on_remote_fetched, result);
  return NULL;
}

static gboolean on_remote_fetched(gpointer data) {
  RemoteResult *result = data;
  GdkPixbufLoader *loader;
  GdkPixbuf *pixbuf = NULL;

  if (remote_inflight) {
    g_hash_table_remove(remote_inflight, result->url);
  }

  if (result->bytes && result->length > 0) {
    loader = gdk_pixbuf_loader_new();
    if (gdk_pixbuf_loader_write(loader, result->bytes, result->length, NULL) &&
        gdk_pixbuf_loader_close(loader, NULL)) {
      pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
      if (pixbuf) {
        g_object_ref(pixbuf);
        remote_cache_store(result->url, pixbuf);
      }
    }
    if (loader) {
      g_object_unref(loader);
    }
  }

  /* Re-render any open document that references this URL. */
  if (pixbuf && app && app->window && app->window->tabs) {
    for (guint i = 0; i < app->window->tabs->len; i++) {
      MarkydTab *tab = g_ptr_array_index(app->window->tabs, i);
      if (tab && tab->editor && tab->editor->source_content &&
          g_strstr_len(tab->editor->source_content, -1, result->url)) {
        markyd_editor_refresh(tab->editor);
      }
    }
  }

  g_free(result->url);
  g_free(result->bytes);
  g_free(result);
  return G_SOURCE_REMOVE;
}

static void ensure_remote_fetch(MarkydEditor *self, const gchar *url) {
  RemoteResult *result;
  GThread *thread;

  if (!remote_inflight) {
    remote_inflight = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  }
  if (g_hash_table_contains(remote_inflight, url)) {
    return;
  }
  if (!remote_permission_allowed(self)) {
    return;
  }
  g_hash_table_add(remote_inflight, g_strdup(url));

  result = g_new0(RemoteResult, 1);
  result->url = g_strdup(url);
  thread = g_thread_new("viewmd-image", remote_fetch_thread, result);
  g_thread_unref(thread);
}

static void render_image_widgets(MarkydEditor *self) {
  GtkTextIter iter;
  GtkTextIter end;
  gint max_width;

  if (!self || !self->buffer || !self->text_view) {
    return;
  }

  max_width = get_image_max_width(self);
  gtk_text_buffer_get_bounds(self->buffer, &iter, &end);
  while (!gtk_text_iter_equal(&iter, &end)) {
    GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(&iter);
    if (anchor &&
        g_object_get_data(G_OBJECT(anchor), VIEWMD_IMAGE_ANCHOR_DATA) != NULL) {
      GtkWidget *image_widget =
          g_object_get_data(G_OBJECT(anchor), VIEWMD_IMAGE_WIDGET_DATA);
      if (!image_widget) {
        const gchar *src =
            g_object_get_data(G_OBJECT(anchor), VIEWMD_IMAGE_SRC_DATA);
        const gchar *alt =
            g_object_get_data(G_OBJECT(anchor), VIEWMD_IMAGE_ALT_DATA);
        gchar *path = NULL;

        if (is_remote_src(src)) {
          GdkPixbuf *cached = remote_cache_lookup(src);
          if (cached) {
            image_widget = make_image_widget_with_pixbuf(
                g_object_ref(cached), max_width);
          } else {
            /* Show the alt text; it will be replaced when the fetch lands. */
            GtkWidget *fallback =
                gtk_label_new(alt && alt[0] != '\0' ? alt : src);
            gtk_widget_set_halign(fallback, GTK_ALIGN_START);
            gtk_style_context_add_class(
                gtk_widget_get_style_context(fallback), "dim-label");
            image_widget = fallback;
            ensure_remote_fetch(self, src);
            /* ensure_remote_fetch() may run the permission dialog in a
               nested loop; the window can be closed while it is open. */
            if (!self->text_view || !self->buffer) {
              g_free(path);
              return;
            }
          }
        } else if (resolve_image_source_path(self, src, &path)) {
          GError *err = NULL;
          GdkPixbuf *orig = gdk_pixbuf_new_from_file(path, &err);
          if (orig) {
            image_widget = make_image_widget_with_pixbuf(orig, max_width);
          } else if (err) {
            g_error_free(err);
          }
        }

        if (!image_widget) {
          GtkWidget *fallback = gtk_label_new(alt && alt[0] != '\0' ? alt : src);
          gtk_widget_set_halign(fallback, GTK_ALIGN_START);
          gtk_style_context_add_class(gtk_widget_get_style_context(fallback),
                                      "dim-label");
          image_widget = fallback;
        }

        gtk_text_view_add_child_at_anchor(GTK_TEXT_VIEW(self->text_view),
                                          image_widget, anchor);
        gtk_widget_show_all(image_widget);
        g_object_set_data(G_OBJECT(anchor), VIEWMD_IMAGE_WIDGET_DATA, image_widget);
        g_free(path);
      } else {
        scale_image_widget(image_widget, max_width);
      }
    }
    gtk_text_iter_forward_char(&iter);
  }
}

static void render_table_widgets(MarkydEditor *self) {
  GtkTextIter iter;
  GtkTextIter end;

  if (!self || !self->buffer || !self->text_view) {
    return;
  }

  gtk_text_buffer_get_bounds(self->buffer, &iter, &end);
  while (!gtk_text_iter_equal(&iter, &end)) {
    GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(&iter);
    if (anchor &&
        g_object_get_data(G_OBJECT(anchor), VIEWMD_TABLE_ANCHOR_DATA) != NULL) {
      GtkWidget *table =
          g_object_get_data(G_OBJECT(anchor), VIEWMD_TABLE_WIDGET_DATA);
      if (!table) {
        table = markdown_create_table_widget(anchor);
        if (table) {
          gtk_text_view_add_child_at_anchor(GTK_TEXT_VIEW(self->text_view), table,
                                            anchor);
          gtk_widget_show_all(table);
          g_object_set_data(G_OBJECT(anchor), VIEWMD_TABLE_WIDGET_DATA, table);
        }
      }
    }
    gtk_text_iter_forward_char(&iter);
  }
}

static void render_frontmatter_widgets(MarkydEditor *self) {
  GtkTextIter iter;
  GtkTextIter end;

  if (!self || !self->buffer || !self->text_view) {
    return;
  }

  gtk_text_buffer_get_bounds(self->buffer, &iter, &end);
  while (!gtk_text_iter_equal(&iter, &end)) {
    GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(&iter);
    if (anchor && g_object_get_data(G_OBJECT(anchor),
                                    VIEWMD_FRONTMATTER_DATA) != NULL) {
      GtkWidget *widget = g_object_get_data(
          G_OBJECT(anchor), VIEWMD_FRONTMATTER_WIDGET_DATA);
      if (!widget) {
        widget = markdown_create_frontmatter_widget(anchor);
        if (widget) {
          gtk_text_view_add_child_at_anchor(GTK_TEXT_VIEW(self->text_view),
                                            widget, anchor);
          gtk_widget_show_all(widget);
          g_object_set_data(G_OBJECT(anchor), VIEWMD_FRONTMATTER_WIDGET_DATA,
                            widget);
        }
      }
    }
    gtk_text_iter_forward_char(&iter);
  }
}

/* Size request content width that makes a widget's total footprint (plus its
   margins and CSS padding) exactly cover the given width. */
static gint inset_total(GtkWidget *widget) {
  GtkStyleContext *style;
  GtkBorder padding = {0, 0, 0, 0};
  gint inset;

  inset = gtk_widget_get_margin_start(widget) + gtk_widget_get_margin_end(widget);
  style = gtk_widget_get_style_context(widget);
  gtk_style_context_get_padding(style, gtk_widget_get_state_flags(widget), &padding);
  inset += padding.left + padding.right;
  return inset;
}

static gint width_minus_widget_insets(GtkWidget *widget, gint width) {
  return MAX(width - inset_total(widget), 120);
}

/* Fit the flow widgets (code blocks, tables, frontmatter) to the text area.
   Child anchors are laid out at their natural size, so a size request is the
   way to control their width; re-applying on size-allocate keeps them in sync
   when the window is resized.

   - Code blocks and frontmatter always span the text area.
   - Tables are sized to their natural (unwrapped) width; only when that
     exceeds the text area do they span it and their cells wrap.

   Note this only re-pins the widgets' size requests. The space the text
   layout reserved around an anchor only updates when the layout revalidates
   the line, which after the child's first allocation happens only through a
   buffer re-layout - hence the coalesced re-render in on_text_view_size_
   allocate_before for width changes. */
static void span_child_widgets_to_width(MarkydEditor *self, gint width) {
  GtkTextIter iter;
  GtkTextIter end;

  if (!self || width <= 0) {
    return;
  }
  self->last_span_width = width;

  gtk_text_buffer_get_bounds(self->buffer, &iter, &end);
  while (!gtk_text_iter_equal(&iter, &end)) {
    GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(&iter);
    if (anchor) {
      GtkWidget *box = (GtkWidget *)g_object_get_data(
          G_OBJECT(anchor), VIEWMD_CODE_WIDGET_DATA);
      if (box) {
        GtkWidget *scroll =
            (GtkWidget *)g_object_get_data(G_OBJECT(box), "viewmd-code-scroll");
        if (scroll) {
          gtk_widget_set_size_request(scroll,
                                      width_minus_widget_insets(box, width), -1);
        }
      }
      GtkWidget *frontmatter = (GtkWidget *)g_object_get_data(
          G_OBJECT(anchor), VIEWMD_FRONTMATTER_WIDGET_DATA);
      if (frontmatter) {
        gtk_widget_set_size_request(
            frontmatter, width_minus_widget_insets(frontmatter, width), -1);
      }
      GtkWidget *table = (GtkWidget *)g_object_get_data(
          G_OBJECT(anchor), VIEWMD_TABLE_WIDGET_DATA);
      if (table) {
        gint min_width = 0;
        gint natural_width = 0;
        GtkBorder padding = {0, 0, 0, 0};
        gint outer;
        gint natural_height = 0;
        gint v_inset;
        gint new_width;
        gint new_height;

        gtk_widget_get_preferred_width(table, &min_width, &natural_width);
        (void)min_width;
        /* The anchor slot is the widget's minimum size, so an explicit
           request is needed to pin the natural (fit-to-content) width; the
           text-area width only caps it. */
        gtk_style_context_get_padding(gtk_widget_get_style_context(table),
                                      gtk_widget_get_state_flags(table), &padding);
        outer = MIN(natural_width, width);
        new_width = width_minus_widget_insets(table, outer);
        /* Pin the height for the final width explicitly: the wrapping cells
           make the natural height a function of the width, and a stale
           height from the pre-wrap measure leaves the table taller than the
           space the text layout reserved (it then floats over the text
           while scrolling). get_preferred_height_for_width takes the full
           outer width and returns it incl. margins. */
        gtk_widget_get_preferred_height_for_width(table, outer, NULL,
                                                  &natural_height);
        v_inset = gtk_widget_get_margin_top(table) +
                  gtk_widget_get_margin_bottom(table) + padding.top + padding.bottom;
        new_height = MAX(natural_height - v_inset, 1);
        gtk_widget_set_size_request(table, new_width, new_height);
      }
    }
    gtk_text_iter_forward_char(&iter);
  }
}

static void render_code_block_widgets(MarkydEditor *self) {
  GtkTextIter iter;
  GtkTextIter end;

  if (!self || !self->buffer || !self->text_view) {
    return;
  }

  gtk_text_buffer_get_bounds(self->buffer, &iter, &end);
  while (!gtk_text_iter_equal(&iter, &end)) {
    GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(&iter);
    if (anchor &&
        g_object_get_data(G_OBJECT(anchor), VIEWMD_CODE_BLOCK_DATA) != NULL) {
      GtkWidget *widget =
          g_object_get_data(G_OBJECT(anchor), VIEWMD_CODE_WIDGET_DATA);
      if (!widget) {
        widget = markdown_create_code_block_widget(anchor);
        if (widget) {
          gtk_text_view_add_child_at_anchor(GTK_TEXT_VIEW(self->text_view),
                                            widget, anchor);
          gtk_widget_show_all(widget);
          g_object_set_data(G_OBJECT(anchor), VIEWMD_CODE_WIDGET_DATA, widget);
        }
      }
    }
    gtk_text_iter_forward_char(&iter);
  }

  span_child_widgets_to_width(self, editor_content_width(self));
}

static void on_fold_toggle_clicked(GtkButton *button, gpointer user_data) {
  MarkydEditor *self = (MarkydEditor *)user_data;
  const gchar *key;
  gboolean effective_collapsed;

  if (!self) {
    return;
  }
  key = g_object_get_data(G_OBJECT(button), "viewmd-fold-key");
  if (!key || key[0] == '\0') {
    return;
  }

  /* The button stores the collapsed state as rendered (which for <details>
     includes the default). Flip the effective state and record the explicit
     override so fold_is_collapsed() sees it on the next re-render. */
  effective_collapsed =
      g_object_get_data(G_OBJECT(button), "viewmd-fold-collapsed") != NULL;
  g_hash_table_replace(self->collapsed, g_strdup(key),
                       GINT_TO_POINTER(!effective_collapsed));
  markyd_editor_refresh(self);
}

static void render_fold_widgets(MarkydEditor *self) {
  GtkTextIter iter;
  GtkTextIter end;

  if (!self || !self->buffer || !self->text_view) {
    return;
  }

  gtk_text_buffer_get_bounds(self->buffer, &iter, &end);
  while (!gtk_text_iter_equal(&iter, &end)) {
    GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(&iter);
    if (anchor &&
        g_object_get_data(G_OBJECT(anchor), VIEWMD_FOLD_ANCHOR_DATA) != NULL) {
      GtkWidget *button =
          g_object_get_data(G_OBJECT(anchor), VIEWMD_FOLD_WIDGET_DATA);
      if (!button) {
        button = markdown_create_fold_widget(anchor);
        if (button) {
          gtk_text_view_add_child_at_anchor(GTK_TEXT_VIEW(self->text_view),
                                            button, anchor);
          gtk_widget_show_all(button);
          g_object_set_data(G_OBJECT(anchor), VIEWMD_FOLD_WIDGET_DATA, button);
        }
      }
      /* Keep the fold key on the button for the click handler / no lock. */
      if (button) {
        const gchar *key =
            g_object_get_data(G_OBJECT(anchor), VIEWMD_FOLD_KEY_DATA);
        g_object_set_data_full(G_OBJECT(button), "viewmd-fold-key",
                               g_strdup(key ? key : ""), g_free);
        g_signal_connect(button, "clicked", G_CALLBACK(on_fold_toggle_clicked),
                         self);
      }
    }
    gtk_text_iter_forward_char(&iter);
  }
}

static gboolean apply_markdown_idle(gpointer user_data) {
  MarkydEditor *self = (MarkydEditor *)user_data;
  self->markdown_idle_id = 0;
  apply_markdown(self);
  return G_SOURCE_REMOVE;
}

/* Full re-render after the text area width stopped changing, so the space
   reserved around child anchors matches the re-pinned widgets again. */
static gboolean relayout_timeout(gpointer user_data) {
  MarkydEditor *self = (MarkydEditor *)user_data;
  self->relayout_timeout_id = 0;
  if (!self->updating_tags) {
    apply_markdown(self);
  }
  return G_SOURCE_REMOVE;
}

static void schedule_markdown_apply(MarkydEditor *self) {
  if (!self || self->updating_tags || self->markdown_idle_id != 0) {
    return;
  }

  self->markdown_idle_id =
      g_idle_add_full(G_PRIORITY_LOW, apply_markdown_idle, self, NULL);
}

void markyd_editor_refresh(MarkydEditor *self) { schedule_markdown_apply(self); }

static void viewmd_heading_free(gpointer data) {
  ViewmdHeading *h = (ViewmdHeading *)data;
  if (h) {
    g_free(h->text);
    g_free(h->slug);
    g_free(h);
  }
}
MarkydEditor *markyd_editor_new(MarkydApp *app) {
  MarkydEditor *self = g_new0(MarkydEditor, 1);

  self->app = app;
  self->source_content = g_strdup("");
  self->file_path = NULL;
  self->collapsed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                           NULL);
  self->headings = g_ptr_array_new_with_free_func(viewmd_heading_free);
  self->source_view = FALSE;
  self->updating_tags = FALSE;
  self->markdown_idle_id = 0;
  self->relayout_timeout_id = 0;
  self->scroll_anchor_offset = 0;
  self->scroll_anchor_remainder = 0.0;
  self->scroll_anchor_valid = FALSE;
  self->scroll_anchor_heading_slug[0] = '\0';
  self->scroll_anchor_heading_delta = 0;
  self->anchor_restore_idle_id = 0;
  self->scroll_anchor_restore_attempts = 0;
  self->scroll_anchor_frozen = FALSE;
  self->last_allocate_width = 0;
  self->last_allocate_height = 0;
  self->last_span_width = 0;
  self->in_resize_pass = FALSE;
  self->vadjustment = NULL;

  self->text_view = gtk_text_view_new();
  g_object_add_weak_pointer(G_OBJECT(self->text_view),
                            (gpointer *)&self->text_view);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(self->text_view),
                              GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(self->text_view), 16);
  gtk_text_view_set_right_margin(GTK_TEXT_VIEW(self->text_view), 16);
  gtk_text_view_set_top_margin(GTK_TEXT_VIEW(self->text_view), 16);
  gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(self->text_view), 16);
  gtk_text_view_set_editable(GTK_TEXT_VIEW(self->text_view), FALSE);
  gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(self->text_view), FALSE);

  self->buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->text_view));
  markdown_init_tags(self->buffer);
  /* Search tags live on every tab's buffer so Ctrl+F works on any tab. */
  gtk_text_buffer_create_tag(self->buffer, TAG_SEARCH_MATCH, "weight",
                             PANGO_WEIGHT_BOLD, NULL);
  gtk_text_buffer_create_tag(self->buffer, TAG_SEARCH_CURRENT, "weight",
                             PANGO_WEIGHT_BOLD, NULL);

  gtk_widget_add_events(self->text_view, GDK_POINTER_MOTION_MASK |
                                            GDK_LEAVE_NOTIFY_MASK |
                                            GDK_BUTTON_RELEASE_MASK);
  g_signal_connect(self->text_view, "button-release-event",
                   G_CALLBACK(on_button_release), self);
  g_signal_connect(self->text_view, "motion-notify-event",
                   G_CALLBACK(on_motion_notify), self);
  g_signal_connect(self->text_view, "leave-notify-event",
                   G_CALLBACK(on_leave_notify), self);
  g_signal_connect(self->text_view, "destroy",
                   G_CALLBACK(on_text_view_destroyed), self);
  g_signal_connect(self->text_view, "populate-popup",
                   G_CALLBACK(on_populate_popup), self);
  /* 'before' runs ahead of the default handler (old layout intact), 'after'
     runs after the re-layout finished. */
  g_signal_connect(self->text_view, "size-allocate",
                    G_CALLBACK(on_text_view_size_allocate_before), self);
  g_signal_connect_after(self->text_view, "size-allocate",
                         G_CALLBACK(on_text_view_size_allocate_after), self);

  return self;
}

/* Y range of a heading's anchor line, or FALSE when the heading is not
   currently rendered (e.g. inside a collapsed section). */
static gboolean heading_line_yrange(MarkydEditor *self, const gchar *slug,
                                    gint *out_y0, gint *out_y1) {
  gchar *mark_name;
  GtkTextMark *mark;
  GtkTextIter iter;

  if (!self || !self->text_view || !self->buffer || !slug || slug[0] == '\0') {
    return FALSE;
  }
  mark_name = markdown_anchor_mark_name(slug);
  mark = gtk_text_buffer_get_mark(self->buffer, mark_name);
  g_free(mark_name);
  if (!mark) {
    return FALSE;
  }
  gtk_text_buffer_get_iter_at_mark(self->buffer, &iter, mark);
  gtk_text_view_get_line_yrange(GTK_TEXT_VIEW(self->text_view), &iter, out_y0,
                                out_y1);
  return TRUE;
}

/* Buffer offset of the line currently at the top of the viewport. */
static void update_scroll_anchor(MarkydEditor *self) {
  GtkTextIter last_iter;
  GtkTextIter line_iter;
  gint last_y = 0;
  gint last_height = 0;
  gint line_top = 0;
  gdouble layout_height;
  gdouble value;
  gint query_y;

  if (!self || !self->vadjustment || !self->text_view || !self->buffer) {
    return;
  }

  gtk_text_buffer_get_end_iter(self->buffer, &last_iter);
  gtk_text_view_get_line_yrange(GTK_TEXT_VIEW(self->text_view), &last_iter, &last_y,
                                &last_height);
  layout_height = (gdouble)(last_y + last_height);
  if (layout_height <= 0.0) {
    return;
  }

  value = gtk_adjustment_get_value(self->vadjustment);
  query_y = CLAMP((gint)value, 0, (gint)layout_height - 1);
  gtk_text_view_get_line_at_y(GTK_TEXT_VIEW(self->text_view), &line_iter, query_y,
                              &line_top);
  self->scroll_anchor_offset = gtk_text_iter_get_offset(&line_iter);
  /* Keep the sub-line remainder: restoring only the line top would round the
     position to a line boundary on every resize and ratchet the view. */
  self->scroll_anchor_remainder = CLAMP(value - line_top, 0.0, 512.0);

  /* Remember which heading section the anchored line lives in, so a re-render
     that changes the document can reconstruct the position from that
     structural anchor instead of a now-meaningless buffer offset. */
  self->scroll_anchor_heading_slug[0] = '\0';
  self->scroll_anchor_heading_delta = 0;
  if (self->headings) {
    for (guint i = 0; i < self->headings->len; i++) {
      ViewmdHeading *heading =
          (ViewmdHeading *)g_ptr_array_index(self->headings, i);
      gint heading_y0 = 0;
      gint heading_y1 = 0;

      if (!heading->slug ||
          !heading_line_yrange(self, heading->slug, &heading_y0, &heading_y1)) {
        continue;
      }
      if (heading_y0 > line_top) {
        break;
      }
      g_strlcpy(self->scroll_anchor_heading_slug, heading->slug,
                sizeof(self->scroll_anchor_heading_slug));
      self->scroll_anchor_heading_delta = line_top - heading_y0;
    }
  }
  self->scroll_anchor_valid = TRUE;
}

/* Bring the anchored line back to the top after a re-layout, clamped to the
   end of the scrollable area (the documented limit of the anchor invariant).
   When 'prefer_heading' is set and the document was re-rendered (a fold
   toggle, reload, or settings change), the nearest heading is used as a
   structural anchor so content changes above the viewport do not move it.
   Returns FALSE when the requested position could not be reached yet because
   the rebuilt document's scrollable height is still stale (the caller then
   retries once the re-layout has settled). */
static gboolean restore_scroll_anchor(MarkydEditor *self,
                                      gboolean prefer_heading) {
  GtkTextIter iter;
  gint y0 = 0;
  gint y1 = 0;
  gdouble max_value;
  gdouble target = 0.0;
  gboolean have_target = FALSE;

  if (!self->scroll_anchor_valid || !self->vadjustment || !self->text_view) {
    return TRUE;
  }

  max_value = gtk_adjustment_get_upper(self->vadjustment) -
              gtk_adjustment_get_page_size(self->vadjustment);

  if (prefer_heading && self->scroll_anchor_heading_slug[0] != '\0') {
    if (heading_line_yrange(self, self->scroll_anchor_heading_slug, &y0, &y1)) {
      target = (gdouble)y0 + self->scroll_anchor_heading_delta;
      have_target = TRUE;
    }
  }
  if (!have_target) {
    gtk_text_buffer_get_iter_at_offset(self->buffer, &iter,
                                       self->scroll_anchor_offset);
    gtk_text_view_get_line_yrange(GTK_TEXT_VIEW(self->text_view), &iter, &y0, &y1);
    target = (gdouble)y0 + self->scroll_anchor_remainder;
  }

  if (fabs(target - gtk_adjustment_get_value(self->vadjustment)) < 0.5) {
    return TRUE;
  }
  /* A target beyond the (possibly stale) scrollable range would be clamped;
     report that so the caller can retry after the layout settles. */
  if (target > max_value + 0.5) {
    gtk_adjustment_set_value(self->vadjustment, MAX(max_value, 0.0));
    return FALSE;
  }
  gtk_adjustment_set_value(self->vadjustment, target);
  return fabs(target - gtk_adjustment_get_value(self->vadjustment)) < 1.0;
}

/* Restore runs from idle so the rebuilt buffer has been re-laid out; when the
   new scrollable height is not ready yet, retry briefly until it settles. */
static gboolean restore_scroll_anchor_idle(gpointer user_data) {
  MarkydEditor *self = (MarkydEditor *)user_data;
  gboolean done;

  self->anchor_restore_idle_id = 0;
  self->in_resize_pass = TRUE;
  done = restore_scroll_anchor(self, TRUE);
  if (!done && self->scroll_anchor_restore_attempts < 12) {
    self->scroll_anchor_restore_attempts++;
    self->anchor_restore_idle_id =
        g_timeout_add(20, restore_scroll_anchor_idle, self);
    self->in_resize_pass = FALSE;
    return G_SOURCE_REMOVE;
  }
  update_scroll_anchor(self);
  self->in_resize_pass = FALSE;
  self->scroll_anchor_frozen = FALSE;
  self->scroll_anchor_restore_attempts = 0;
  return G_SOURCE_REMOVE;
}

static void schedule_scroll_anchor_restore(MarkydEditor *self) {
  if (!self) {
    return;
  }
  if (self->anchor_restore_idle_id != 0) {
    g_source_remove(self->anchor_restore_idle_id);
  }
  self->scroll_anchor_restore_attempts = 0;
  self->anchor_restore_idle_id =
      g_idle_add(restore_scroll_anchor_idle, self);
}

static void on_vadjustment_value_changed(GtkAdjustment *adjustment,
                                         gpointer user_data) {
  MarkydEditor *self = (MarkydEditor *)user_data;
  (void)adjustment;

  if (!self || self->in_resize_pass || self->scroll_anchor_frozen ||
      !self->vadjustment) {
    return;
  }
  update_scroll_anchor(self);
}

static void on_text_view_size_allocate_before(GtkWidget *widget,
                                              GtkAllocation *allocation,
                                              gpointer user_data) {
  MarkydEditor *self = (MarkydEditor *)user_data;
  (void)widget;

  if (!self || !self->text_view) {
    return;
  }

  if (!self->vadjustment) {
    GtkWidget *sw =
        gtk_widget_get_ancestor(self->text_view, GTK_TYPE_SCROLLED_WINDOW);
    if (sw && GTK_IS_SCROLLED_WINDOW(sw)) {
      self->vadjustment =
          gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw));
      if (self->vadjustment) {
        g_object_add_weak_pointer(G_OBJECT(self->vadjustment),
                                  (gpointer *)&self->vadjustment);
        g_signal_connect(self->vadjustment, "value-changed",
                         G_CALLBACK(on_vadjustment_value_changed), self);
      }
    }
  }

  {
    gint prev_width = self->last_allocate_width;

    if (allocation->width != self->last_allocate_width ||
        allocation->height != self->last_allocate_height) {
      /* Runs before the default handler re-lays-out the view: capture the top
         line while the pre-resize layout and adjustment are still intact. */
      self->in_resize_pass = TRUE;
      update_scroll_anchor(self);
      self->last_allocate_width = allocation->width;
      self->last_allocate_height = allocation->height;
    }

    /* Span the flow widgets to the incoming width BEFORE the default handler
       re-lays-out the view so the widgets track the window live. The space
       the layout reserved around an anchor cannot heal from size requests
       alone (see span_child_widgets_to_width), so re-render on a debounce
       once the width stops changing; until then the previous line heights
       stand. */
    {
      gint w = allocation->width -
               gtk_text_view_get_left_margin(GTK_TEXT_VIEW(self->text_view)) -
               gtk_text_view_get_right_margin(GTK_TEXT_VIEW(self->text_view));
      if (w >= 320 && w <= 2400 && w != self->last_span_width) {
        span_child_widgets_to_width(self, w);
        if (prev_width != 0 && self->relayout_timeout_id == 0) {
          self->relayout_timeout_id = g_timeout_add(150, relayout_timeout, self);
        }
      }
    }
  }
}

static void on_text_view_size_allocate_after(GtkWidget *widget,
                                              GtkAllocation *allocation,
                                              gpointer user_data) {
  MarkydEditor *self = (MarkydEditor *)user_data;
  (void)widget;
  (void)allocation;
  if (self->in_resize_pass) {
    restore_scroll_anchor(self, FALSE);
    self->in_resize_pass = FALSE;
  }
  refresh_image_widget_scales(self);
}

static void on_text_view_destroyed(GtkWidget *widget, gpointer user_data) {
  /* GtkTextView clears its buffer on dispose and then sets text targets.
     It is no longer a drop destination after we unset it, which would warn;
     mark it as one again while the widget is still alive. */
  gtk_drag_dest_set(widget, 0, NULL, 0, 0);

  MarkydEditor *self = (MarkydEditor *)user_data;
  if (!self) {
    return;
  }
  self->text_view = NULL;
  self->buffer = NULL;
  if (self->markdown_idle_id != 0) {
    g_source_remove(self->markdown_idle_id);
    self->markdown_idle_id = 0;
  }
}

void markyd_editor_free(MarkydEditor *self) {
  if (!self) {
    return;
  }
  if (self->text_view) {
    g_object_remove_weak_pointer(G_OBJECT(self->text_view),
                                 (gpointer *)&self->text_view);
    self->text_view = NULL;
  }
  if (self->vadjustment) {
    g_signal_handlers_disconnect_by_func(
        self->vadjustment, G_CALLBACK(on_vadjustment_value_changed), self);
    g_object_remove_weak_pointer(G_OBJECT(self->vadjustment),
                                 (gpointer *)&self->vadjustment);
    self->vadjustment = NULL;
  }
  if (self->markdown_idle_id != 0) {
    g_source_remove(self->markdown_idle_id);
    self->markdown_idle_id = 0;
  }
  if (self->anchor_restore_idle_id != 0) {
    g_source_remove(self->anchor_restore_idle_id);
    self->anchor_restore_idle_id = 0;
  }
  g_free(self->source_content);
  g_free(self->file_path);
  if (self->relayout_timeout_id != 0) {
    g_source_remove(self->relayout_timeout_id);
    self->relayout_timeout_id = 0;
  }
  if (self->collapsed) {
    g_hash_table_destroy(self->collapsed);
    self->collapsed = NULL;
  }
  if (self->headings) {
    g_ptr_array_free(self->headings, TRUE);
    self->headings = NULL;
  }
  g_free(self);
}

void markyd_editor_set_content(MarkydEditor *self, const gchar *content) {
  if (!self) {
    return;
  }

  g_free(self->source_content);
  /* Guard against non-UTF-8 markdown (e.g. latin-1) crashing Pango later. */
  if (content) {
    self->source_content = g_utf8_make_valid(content, -1);
  } else {
    self->source_content = g_strdup("");
  }
  schedule_markdown_apply(self);
}

gchar *markyd_editor_get_content(MarkydEditor *self) {
  if (!self) {
    return g_strdup("");
  }
  return g_strdup(self->source_content ? self->source_content : "");
}

void markyd_editor_show_source(MarkydEditor *self, const gchar *text) {
  if (!self) {
    return;
  }
  self->source_view = TRUE;
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(self->text_view), TRUE);
  gtk_style_context_add_class(gtk_widget_get_style_context(self->text_view),
                              "viewmd-source");
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(self->text_view), GTK_WRAP_NONE);
  markyd_editor_set_content(self, text);
}

void markyd_editor_set_file_path(MarkydEditor *self, const gchar *path) {
  if (!self) {
    return;
  }

  /* Filenames may be non-UTF-8 on non-UTF-8 locales; the path also feeds the
     tab title and window title, so sanitize it. */
  gchar *sanitized = g_utf8_make_valid(path ? path : "", -1);

  /* Opening a different document resets remembered collapse state. */
  if (g_strcmp0(self->file_path, sanitized) != 0 && self->collapsed) {
    g_hash_table_remove_all(self->collapsed);
  }
  g_free(self->file_path);
  self->file_path = sanitized;
}

const gchar *markyd_editor_get_file_path(MarkydEditor *self) {
  if (!self) {
    return NULL;
  }
  return self->file_path;
}

void markyd_editor_scroll_to_anchor(MarkydEditor *self, const gchar *slug) {
  if (self) {
    scroll_to_markdown_anchor(self, slug);
  }
}

const GPtrArray *markyd_editor_get_headings(MarkydEditor *self) {
  if (!self) {
    return NULL;
  }
  return self->headings;
}

GtkWidget *markyd_editor_get_widget(MarkydEditor *self) {
  return self->text_view;
}

void markyd_editor_focus(MarkydEditor *self) {
  GtkTextMark *insert_mark;

  if (!self || !self->text_view || !self->buffer) {
    return;
  }

  gtk_widget_grab_focus(self->text_view);
  insert_mark = gtk_text_buffer_get_insert(self->buffer);
  if (insert_mark) {
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(self->text_view), insert_mark,
                                 0.0, FALSE, 0.0, 0.0);
  }
}

/* The fold toggle widget only covers the disclosure triangle, but a heading
   or details summary line starts with the fold child anchor, so a click
   anywhere on its text can be mapped back to that toggle. */
static GtkWidget *fold_button_at_iter(GtkTextBuffer *buffer,
                                      const GtkTextIter *at) {
  GtkTextIter iter;
  GtkTextIter line_end;

  if (!buffer || !at) {
    return NULL;
  }
  gtk_text_buffer_get_iter_at_line(buffer, &iter, gtk_text_iter_get_line(at));
  line_end = iter;
  gtk_text_iter_forward_to_line_end(&line_end);
  while (!gtk_text_iter_equal(&iter, &line_end)) {
    GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(&iter);
    if (anchor && g_object_get_data(G_OBJECT(anchor), VIEWMD_FOLD_ANCHOR_DATA)) {
      return (GtkWidget *)g_object_get_data(G_OBJECT(anchor),
                                            VIEWMD_FOLD_WIDGET_DATA);
    }
    if (!gtk_text_iter_forward_char(&iter)) {
      break;
    }
  }
  return NULL;
}

static gboolean on_button_release(GtkWidget *widget, GdkEventButton *event,
                                  gpointer user_data) {
  MarkydEditor *self = (MarkydEditor *)user_data;
  GtkTextIter iter;
  gint bx, by;
  gchar *url = NULL;
  GError *error = NULL;

  if (event->button != 1) {
    return FALSE;
  }

  gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(widget),
                                        GTK_TEXT_WINDOW_TEXT, (gint)event->x,
                                        (gint)event->y, &bx, &by);
  gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(widget), &iter, bx, by);

  if (!get_link_url_at_iter(self->buffer, &iter, &url)) {
    GtkWidget *fold_button = fold_button_at_iter(self->buffer, &iter);
    if (fold_button) {
      GtkTextIter sel_start;
      GtkTextIter sel_end;

      /* A release that ends a drag within the heading/summary line is a text
         selection, not a toggle click; keep the selection intact. */
      if (gtk_text_buffer_get_selection_bounds(self->buffer, &sel_start,
                                               &sel_end)) {
        return FALSE;
      }
      gtk_button_clicked(GTK_BUTTON(fold_button));
      return TRUE;
    }
    return FALSE;
  }

  if (url[0] == '#') {
    gchar *fragment = g_strdup(url + 1);
    gchar *space = strpbrk(fragment, " \t");
    if (space) {
      *space = '\0';
    }
    if (!scroll_to_markdown_anchor(self, fragment)) {
      g_printerr("Anchor not found: '%s'\n", url);
    }
    g_free(fragment);
    g_free(url);
    return TRUE;
  }

  if (g_uri_parse_scheme(url) == NULL) {
    gchar *with_scheme = g_strdup_printf("https://%s", url);
    g_free(url);
    url = with_scheme;
  }

  GtkWidget *toplevel = gtk_widget_get_toplevel(widget);
  if (!gtk_show_uri_on_window(GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel)
                                                      : NULL,
                              url, GDK_CURRENT_TIME, &error)) {
    if (error) {
      g_printerr("Failed to open link '%s': %s\n", url, error->message);
      g_clear_error(&error);
    }
  }

  g_free(url);
  return TRUE;
}

static gboolean on_motion_notify(GtkWidget *widget, GdkEventMotion *event,
                                 gpointer user_data) {
  MarkydEditor *self = (MarkydEditor *)user_data;
  GtkTextIter iter;
  gint bx, by;
  gchar *url = NULL;

  gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(widget),
                                        GTK_TEXT_WINDOW_TEXT, (gint)event->x,
                                        (gint)event->y, &bx, &by);
  gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(widget), &iter, bx, by);

  set_link_cursor(self, get_link_url_at_iter(self->buffer, &iter, &url));
  g_free(url);
  return FALSE;
}

static gboolean on_leave_notify(GtkWidget *widget, GdkEventCrossing *event,
                                gpointer user_data) {
  MarkydEditor *self = (MarkydEditor *)user_data;
  (void)widget;
  (void)event;
  set_link_cursor(self, FALSE);
  return FALSE;
}

static void on_view_source_activate(GtkMenuItem *item, gpointer user_data) {
  MarkydEditor *self = (MarkydEditor *)user_data;
  MarkydWindow *window;
  MarkydTab *tab;
  gchar *source;

  (void)item;
  if (!self || !self->app || !self->app->window) {
    return;
  }
  window = self->app->window;
  source = markyd_editor_get_content(self);

  tab = markyd_window_new_tab(window);
  if (tab && tab->editor) {
    if (self->file_path) {
      markyd_editor_set_file_path(tab->editor, self->file_path);
    }
    markyd_editor_show_source(tab->editor, source);
    markyd_window_update_tab_label(window, tab);
  }
  g_free(source);
}

static void on_populate_popup(GtkTextView *text_view, GtkWidget *menu,
                              gpointer user_data) {
  MarkydEditor *self = (MarkydEditor *)user_data;
  GtkWidget *item;

  (void)text_view;
  item = gtk_menu_item_new_with_label("View Source");
  gtk_widget_set_tooltip_text(item,
                              "Show the raw markdown of this document");
  g_signal_connect(item, "activate", G_CALLBACK(on_view_source_activate), self);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
  gtk_widget_show(item);
}
