#include "config.h"
#include "markdown.h"

static GtkTextBuffer *new_buffer(void) {
  GtkTextBuffer *buffer = gtk_text_buffer_new(NULL);
  markdown_init_tags(buffer);
  return buffer;
}

static gchar *buffer_text(GtkTextBuffer *buffer) {
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  return gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
}

static void assert_text(GtkTextBuffer *buffer, const gchar *expected) {
  gchar *text = buffer_text(buffer);
  g_assert_cmpstr(text, ==, expected);
  g_free(text);
}

static void test_reload_link_tags(void) {
  GtkTextBuffer *buffer = new_buffer();
  GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buffer);
  gint baseline = gtk_text_tag_table_get_size(table);

  for (guint i = 0; i < 50; i++) {
    markdown_apply_tags(buffer, "[One](https://one.example) [Two](#two)");
    g_assert_cmpint(gtk_text_tag_table_get_size(table), ==, baseline + 2);
    GtkTextIter at;
    gtk_text_buffer_get_start_iter(buffer, &at);
    GSList *tags = gtk_text_iter_get_tags(&at);
    gboolean found_url = FALSE;
    for (GSList *item = tags; item; item = item->next) {
      const gchar *url = g_object_get_data(G_OBJECT(item->data),
                                           VIEWMD_LINK_URL_DATA);
      if (g_strcmp0(url, "https://one.example") == 0) {
        found_url = TRUE;
      }
    }
    g_assert_true(found_url);
    g_slist_free(tags);
  }

  markdown_apply_tags(buffer, "No links");
  g_assert_cmpint(gtk_text_tag_table_get_size(table), ==, baseline);
  g_object_unref(buffer);
}

static void test_reload_heading_anchors(void) {
  GtkTextBuffer *buffer = new_buffer();
  markdown_apply_tags(buffer, "# Removed\n\n# Kept\n\n# Kept\n");
  g_assert_nonnull(gtk_text_buffer_get_mark(buffer, "viewmd-anchor-kept-1"));

  for (guint i = 0; i < 3; i++) {
    markdown_apply_tags(buffer, "Intro\n\n# Kept\n");
    g_assert_null(gtk_text_buffer_get_mark(buffer, "viewmd-anchor-removed"));
    g_assert_null(gtk_text_buffer_get_mark(buffer, "viewmd-anchor-kept-1"));
    GtkTextMark *mark = gtk_text_buffer_get_mark(buffer, "viewmd-anchor-kept");
    g_assert_nonnull(mark);
    GtkTextIter at;
    gtk_text_buffer_get_iter_at_mark(buffer, &at, mark);
    g_assert_cmpint(gtk_text_iter_get_offset(&at), ==, 7);
  }

  markdown_apply_tags(buffer, NULL);
  g_assert_null(gtk_text_buffer_get_mark(buffer, "viewmd-anchor-kept"));
  g_object_unref(buffer);
}

static void test_reload_preserves_other_state(void) {
  GtkTextBuffer *buffer = new_buffer();
  GtkTextTag *custom = gtk_text_buffer_create_tag(buffer, "custom", NULL);
  GtkTextTag *anonymous = gtk_text_buffer_create_tag(buffer, NULL, NULL);
  g_object_ref(anonymous);
  GtkTextIter at;
  gtk_text_buffer_get_start_iter(buffer, &at);
  GtkTextMark *bookmark = gtk_text_buffer_create_mark(buffer, "bookmark", &at, TRUE);

  markdown_apply_tags(buffer, "# Heading\n\n[link](https://example.com)");
  markdown_apply_tags(buffer, "# Heading\n\n[link](https://other.example)");
  GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buffer);
  g_assert_true(gtk_text_tag_table_lookup(table, "custom") == custom);
  g_assert_true(gtk_text_buffer_get_mark(buffer, "bookmark") == bookmark);
  GtkTextIter end;
  gtk_text_buffer_get_bounds(buffer, &at, &end);
  gtk_text_buffer_apply_tag(buffer, anonymous, &at, &end);
  g_assert_true(gtk_text_iter_has_tag(&at, anonymous));
  g_object_unref(anonymous);
  g_object_unref(buffer);
}

static void test_reload_buffers_independent(void) {
  GtkTextBuffer *first = new_buffer();
  GtkTextBuffer *second = new_buffer();
  markdown_apply_tags(first, "# First\n\n[link](#first)");
  markdown_apply_tags(second, "# Second\n\n[link](#second)");
  gint second_tags = gtk_text_tag_table_get_size(gtk_text_buffer_get_tag_table(second));
  markdown_apply_tags(first, "");
  g_object_unref(first);
  g_assert_nonnull(gtk_text_buffer_get_mark(second, "viewmd-anchor-second"));
  g_assert_cmpint(gtk_text_tag_table_get_size(gtk_text_buffer_get_tag_table(second)),
                  ==, second_tags);
  g_object_unref(second);
}

static void test_task_statuses(void) {
  GtkTextBuffer *buffer = new_buffer();
  markdown_apply_tags(buffer, "- [x] Done\n- [ ] Open\n- [X] Also done\n- Plain\n");
  assert_text(buffer, "☑ Done\n☐ Open\n☑ Also done\n• Plain\n");
  g_object_unref(buffer);
}

static void test_nested_tasks(void) {
  GtkTextBuffer *buffer = new_buffer();
  markdown_apply_tags(buffer, "- [ ] Parent\n  - [x] Child\n  - Plain child\n- [x] Last\n");
  assert_text(buffer, "☐ Parent\n☑ Child\n• Plain child\n☑ Last\n");
  g_object_unref(buffer);
}

static void test_ordered_tasks(void) {
  GtkTextBuffer *buffer = new_buffer();
  markdown_apply_tags(buffer, "9. [ ] Open\n10. Plain\n11. [X] Done\n12. Last\n");
  assert_text(buffer, "☐ Open\n10. Plain\n☑ Done\n12. Last\n");
  g_object_unref(buffer);
}

static void test_task_hanging_indent(void) {
  GtkTextBuffer *buffer = new_buffer();
  markdown_apply_tags(buffer, "- [ ] Task\n  continuation\n");
  assert_text(buffer, "☐ Task\ncontinuation\n");
  GtkTextIter at;
  gtk_text_buffer_get_start_iter(buffer, &at);
  GSList *tags = gtk_text_iter_get_tags(&at);
  gboolean found_indent = FALSE;
  for (GSList *item = tags; item; item = item->next) {
    gint indent = 0;
    g_object_get(item->data, "indent", &indent, NULL);
    if (indent < 0) {
      found_indent = TRUE;
      gtk_text_buffer_get_iter_at_offset(buffer, &at, 7);
      g_assert_true(gtk_text_iter_has_tag(&at, item->data));
    }
  }
  g_assert_true(found_indent);
  g_slist_free(tags);
  g_object_unref(buffer);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  gtk_init_check(&argc, &argv);
  /* GtkTextBuffer works without a display. Register the boxed color types
   * normally registered by a successful GTK display initialization. */
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  g_type_ensure(GDK_TYPE_COLOR);
  G_GNUC_END_IGNORE_DEPRECATIONS
  g_type_ensure(GDK_TYPE_RGBA);
  config = config_new();

  g_test_add_func("/markdown/reload/link-tags", test_reload_link_tags);
  g_test_add_func("/markdown/reload/heading-anchors", test_reload_heading_anchors);
  g_test_add_func("/markdown/reload/preserve-other-state", test_reload_preserves_other_state);
  g_test_add_func("/markdown/reload/independent-buffers", test_reload_buffers_independent);
  g_test_add_func("/markdown/tasks/statuses", test_task_statuses);
  g_test_add_func("/markdown/tasks/nested", test_nested_tasks);
  g_test_add_func("/markdown/tasks/ordered", test_ordered_tasks);
  g_test_add_func("/markdown/tasks/hanging-indent", test_task_hanging_indent);
  gint result = g_test_run();
  config_free(config);
  return result;
}
