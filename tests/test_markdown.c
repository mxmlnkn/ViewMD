#include <gtk/gtk.h>
#include <glib.h>
#include <string.h>

#include "markdown.h"
#include "code_highlight.h"
#include "config.h"

/* Headless tests for the renderer logic. They drive markdown_apply_tags
   against a plain GtkTextBuffer, which needs no window or display. */


static GtkTextBuffer *render_buffer(const gchar *source, GHashTable *collapsed,
                                    GPtrArray *headings_out) {
  GtkTextBuffer *buffer = gtk_text_buffer_new(NULL);
  markdown_init_tags(buffer);
  markdown_apply_tags(buffer, source, collapsed, headings_out);
  return buffer;
}

static gchar *buffer_text(GtkTextBuffer *buffer, gboolean include_hidden) {
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  return gtk_text_buffer_get_text(buffer, &start, &end, include_hidden);
}

static gboolean has_anchor_with_data(GtkTextBuffer *buffer,
                                     const gchar *data_key) {
  GtkTextIter iter, end;
  gtk_text_buffer_get_bounds(buffer, &iter, &end);
  while (!gtk_text_iter_equal(&iter, &end)) {
    GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(&iter);
    if (anchor && g_object_get_data(G_OBJECT(anchor), data_key) != NULL) {
      return TRUE;
    }
    gtk_text_iter_forward_char(&iter);
  }
  return FALSE;
}

static gboolean mark_exists(GtkTextBuffer *buffer, const gchar *name) {
  return gtk_text_buffer_get_mark(buffer, name) != NULL;
}

static void test_slug_normalization(void) {
  g_assert_cmpstr(markdown_normalize_anchor_slug("Hello World"), ==,
                  "hello-world");
  g_assert_cmpstr(markdown_normalize_anchor_slug("C++ & C"), ==, "c-c");
  g_assert_cmpstr(markdown_normalize_anchor_slug("UPPER_case"), ==,
                  "upper-case");
  g_assert_cmpstr(markdown_normalize_anchor_slug(" a b "), ==, "a-b");
}

static void test_frontmatter_table(void) {
  const gchar *source =
      "---\ntitle: My Doc\nauthor: Kilo\ntags: markdown, test\n---\n\n"
      "# Heading\n\nBody paragraph.\n";
  GtkTextBuffer *buffer;
  gchar *visible;
  gchar *all;

  buffer = render_buffer(source, NULL, NULL);
  visible = buffer_text(buffer, FALSE);
  all = buffer_text(buffer, TRUE);

  g_assert_nonnull(strstr(visible, "Heading"));
  g_assert_nonnull(strstr(visible, "Body paragraph."));
  /* The frontmatter itself must not render as visible body text. */
  g_assert_true(strstr(visible, "My Doc") == NULL);
  /* ...but the values stay searchable through the hidden text. */
  g_assert_nonnull(strstr(all, "My Doc"));
  g_assert_nonnull(strstr(all, "Kilo"));

  g_assert_true(has_anchor_with_data(buffer, VIEWMD_FRONTMATTER_DATA));

  g_free(visible);
  g_free(all);
  g_object_unref(buffer);
}

static void test_setext_heading(void) {
  /* Setext H1/H2: `Title\n===` / `Title\n---` must render as a heading,
     not as paragraph + thematic break. */
  GtkTextBuffer *buffer;
  gchar *visible;

  buffer = render_buffer("Title\n---\n\nBody.\n", NULL, NULL);
  visible = buffer_text(buffer, TRUE);

  g_assert_nonnull(strstr(visible, "Title"));
  g_assert_nonnull(strstr(visible, "Body."));
  /* The --- underline must be consumed by Setext, not appear as text. */
  g_assert_true(strstr(visible, "---") == NULL);
  /* The heading mark was created, proving it parsed as a heading. */
  g_assert_true(mark_exists(buffer, "viewmd-anchor-title"));

  g_free(visible);
  g_object_unref(buffer);
}

static void test_details_override(void) {
  /* <details> has no id in the source; the transform numbers them by line, so
     the first one is details-0. An explicit map entries overrides the
     collapsed-by-default / <open> defaults. */
  const gchar *source = "<details>\n<summary>One</summary>\n\nHidden.\n\n"
                        "</details>\n";
  GHashTable *collapsed;
  GtkTextBuffer *buffer;
  gchar *text;

  collapsed = g_hash_table_new(g_str_hash, g_str_equal);

  /* No entry -> default collapsed. */
  buffer = render_buffer(source, collapsed, NULL);
  text = buffer_text(buffer, TRUE);
  g_assert_true(strstr(text, "Hidden.") == NULL);
  g_free(text);
  g_object_unref(buffer);

  /* Explicit FALSE -> expanded. */
  g_hash_table_insert(collapsed, g_strdup("details-0"), GINT_TO_POINTER(FALSE));
  buffer = render_buffer(source, collapsed, NULL);
  text = buffer_text(buffer, TRUE);
  g_assert_nonnull(strstr(text, "Hidden."));
  g_free(text);
  g_object_unref(buffer);

  /* Explicit TRUE -> stays collapsed. */
  g_hash_table_replace(collapsed, g_strdup("details-0"), GINT_TO_POINTER(TRUE));
  buffer = render_buffer(source, collapsed, NULL);
  text = buffer_text(buffer, TRUE);
  g_assert_true(strstr(text, "Hidden.") == NULL);
  g_free(text);
  g_object_unref(buffer);

  g_hash_table_destroy(collapsed);
}

static void test_details_collapsed_by_default(void) {
  const gchar *source =
      "# Top\n\n<details>\n<summary>Click to expand</summary>\n\n"
      "Hidden paragraph.\n\n</details>\n\n<details open>\n"
      "<summary>Open details</summary>\n\nVisible content.\n</details>\n";
  GtkTextBuffer *buffer;
  gchar *text;

  buffer = render_buffer(source, NULL, NULL);
  text = buffer_text(buffer, TRUE);

  g_assert_nonnull(strstr(text, "Click to expand"));
  g_assert_true(strstr(text, "Hidden paragraph") == NULL);
  g_assert_nonnull(strstr(text, "Open details"));
  g_assert_nonnull(strstr(text, "Visible content."));

  g_free(text);
  g_object_unref(buffer);
}

static void test_heading_collapse(void) {
  const gchar *source =
      "# Top\n\n## Section A\n\nContent of section A.\n\n"
      "### Sub of A\n\nDeeper.\n\n## Section B\n\nContent of section B.\n";
  GHashTable *collapsed;
  GtkTextBuffer *buffer;
  gchar *text;

  collapsed = g_hash_table_new(g_str_hash, g_str_equal);
  g_hash_table_insert(collapsed, g_strdup("section-a"), GINT_TO_POINTER(TRUE));

  buffer = render_buffer(source, collapsed, NULL);
  text = buffer_text(buffer, TRUE);

  g_assert_nonnull(strstr(text, "Section A"));
  g_assert_true(strstr(text, "Content of section A") == NULL);
  g_assert_true(strstr(text, "Deeper") == NULL);
  g_assert_nonnull(strstr(text, "Section B"));
  g_assert_nonnull(strstr(text, "Content of section B"));

  g_free(text);
  g_object_unref(buffer);
  g_hash_table_destroy(collapsed);
}

static void test_heading_collapse_with_details(void) {
  /* A <details> block inside a collapsed section must not leak its pending
     summary into the next section (which would swallow it). */
  const gchar *source =
      "# Alpha\n\nAlpha intro.\n\n<details>\n<summary>Summary</summary>\n\n"
      "Hidden detail.\n\n</details>\n\n## Alpha Sub\n\nSub content.\n\n"
      "# Beta\n\nBeta content that must survive.\n";
  GHashTable *collapsed;
  GtkTextBuffer *buffer;
  gchar *text;

  collapsed = g_hash_table_new(g_str_hash, g_str_equal);
  g_hash_table_insert(collapsed, g_strdup("alpha"), GINT_TO_POINTER(TRUE));

  buffer = render_buffer(source, collapsed, NULL);
  text = buffer_text(buffer, TRUE);

  g_assert_nonnull(strstr(text, "Alpha"));
  g_assert_true(strstr(text, "Hidden detail") == NULL);
  g_assert_nonnull(strstr(text, "Beta"));
  g_assert_nonnull(strstr(text, "Beta content that must survive"));

  g_free(text);
  g_object_unref(buffer);
  g_hash_table_destroy(collapsed);
}

static void test_heading_toc(void) {
  const gchar *source = "# Title\n\n## Section Two\n\n### Sub Three\n";
  GPtrArray *headings;
  GtkTextBuffer *buffer;
  ViewmdHeading *h;

  headings = g_ptr_array_new();
  buffer = render_buffer(source, NULL, headings);

  g_assert_cmpuint(headings->len, ==, 3);
  h = g_ptr_array_index(headings, 0);
  g_assert_cmpint(h->level, ==, 1);
  g_assert_cmpstr(h->text, ==, "Title");
  g_assert_cmpstr(h->slug, ==, "title");

  h = g_ptr_array_index(headings, 1);
  g_assert_cmpint(h->level, ==, 2);
  g_assert_cmpstr(h->text, ==, "Section Two");
  g_assert_cmpstr(h->slug, ==, "section-two");

  h = g_ptr_array_index(headings, 2);
  g_assert_cmpint(h->level, ==, 3);
  g_assert_cmpstr(h->slug, ==, "sub-three");

  g_assert_true(mark_exists(buffer, "viewmd-anchor-title"));
  for (guint i = 0; i < headings->len; i++) {
    ViewmdHeading *he = g_ptr_array_index(headings, i);
    g_free(he->text);
    g_free(he->slug);
    g_free(he);
  }
  g_ptr_array_free(headings, TRUE);
  g_object_unref(buffer);
}

static void test_soft_break_folding(void) {
  /* A single newline inside a paragraph is a soft break and must render as a
     space, not a line break. New paragraphs and explicit hard breaks keep
     their real newlines. */
  const gchar *source = "line one\nline two\nline three\n\nnext para  \n"
                        "after hard break\n";
  GtkTextBuffer *buffer;
  gchar *text;

  buffer = render_buffer(source, NULL, NULL);
  text = buffer_text(buffer, FALSE);

  g_assert_nonnull(strstr(text, "line one line two line three"));
  g_assert_true(strstr(text, "line three\n") != NULL);
  /* The soft breaks must not leave a stray newline inside the paragraph. */
  g_assert_true(strstr(text, "line one\n") == NULL);
  g_assert_true(strstr(text, "line two\n") == NULL);
  /* The hard break (two trailing spaces) survives. */
  g_assert_nonnull(strstr(text, "next para\nafter hard break"));

  g_free(text);
  g_object_unref(buffer);
}

static void test_details_in_fence(void) {
  /* <details> inside a fenced code block is literal code, not a fold. */
  const gchar *source = "```html\n<details>\n<summary>X</summary>\n"
                        "</details>\n```\n\nAfter.\n";
  GtkTextBuffer *buffer;
  gchar *text;

  buffer = render_buffer(source, NULL, NULL);
  text = buffer_text(buffer, TRUE);

  g_assert_nonnull(strstr(text, "<details>"));
  g_assert_null(strstr(text, "<!--vmd-details"));
  g_assert_null(strstr(text, "###### X"));
  g_assert_nonnull(strstr(text, "After."));

  g_free(text);
  g_object_unref(buffer);
}

static void test_details_after_fence_info(void) {
  /* A line like ```js inside a ```text block is content, not a close. */
  const gchar *source = "```text\n```js\nHELLO\n```\n\n<details>\n"
                        "<summary>Fold</summary>\n\nHidden.\n\n</details>\n";
  GtkTextBuffer *buffer;
  gchar *text;

  buffer = render_buffer(source, NULL, NULL);
  text = buffer_text(buffer, TRUE);

  g_assert_nonnull(strstr(text, "```js"));
  g_assert_nonnull(strstr(text, "Fold"));
  g_assert_null(strstr(text, "Hidden."));

  g_free(text);
  g_object_unref(buffer);
}

static void test_frontmatter_empty_values(void) {
  /* A recognized frontmatter block stays hidden even if all values are empty. */
  const gchar *source = "---\ntitle:\n---\n\n# Heading\n";
  GtkTextBuffer *buffer;
  gchar *text;

  buffer = render_buffer(source, NULL, NULL);
  text = buffer_text(buffer, TRUE);

  g_assert_null(strstr(text, "title:"));
  g_assert_nonnull(strstr(text, "Heading"));

  g_free(text);
  g_object_unref(buffer);
}

static void test_codeblock_searchable_text(void) {
  const gchar *source = "# Doc\n\n```c\nint main(void) { return 0; }\n```\n";
  GtkTextBuffer *buffer;
  gchar *text;

  buffer = render_buffer(source, NULL, NULL);
  text = buffer_text(buffer, TRUE);

  /* Code is kept findable: its text lives (invisibly) in the buffer. */
  g_assert_nonnull(strstr(text, "int main"));
  g_assert_true(has_anchor_with_data(buffer, VIEWMD_CODE_BLOCK_DATA));

  g_free(text);
  g_object_unref(buffer);
}

/* Tokenizer checks for the syntax highlighter. */
typedef struct {
  gboolean seen_return;
  gboolean seen_int;
  gboolean seen_literal;
} TokenProbe;

static void on_token(gint start, gint end, const gchar *tag,
                     gpointer user_data) {
  (void)start;
  (void)end;
  TokenProbe *probe = (TokenProbe *)user_data;
  if (g_strcmp0(tag, MARKYD_TAG_CODE_KW_A) == 0) {
    probe->seen_return = TRUE;
  } else if (g_strcmp0(tag, MARKYD_TAG_CODE_KW_C) == 0) {
    probe->seen_int = TRUE;
  } else if (g_strcmp0(tag, MARKYD_TAG_CODE_LITERAL) == 0) {
    probe->seen_literal = TRUE;
  }
}

static void test_c_tokenizer(void) {
  const MarkydLanguageHighlight *lang = markyd_code_lookup_language("c");
  MarkydCodeScanState state = {0};
  TokenProbe probe = {FALSE, FALSE, FALSE};

  g_assert_nonnull(lang);
  markyd_code_scan_state_reset(&state);
  markyd_code_scan_line(lang, "int main(void) { return 0; }", &state,
                        on_token, &probe);
  g_assert_true(probe.seen_return);
  g_assert_true(probe.seen_int);
  g_assert_true(probe.seen_literal);

  g_assert_null(markyd_code_lookup_language("nosuchlanguage"));
}

static GtkTextBuffer *new_buffer(void) {
  GtkTextBuffer *buffer = gtk_text_buffer_new(NULL);
  markdown_init_tags(buffer);
  return buffer;
}

static void assert_text(GtkTextBuffer *buffer, const gchar *expected) {
  gchar *text = buffer_text(buffer, FALSE);
  g_assert_cmpstr(text, ==, expected);
  g_free(text);
}

static void upstream_apply(GtkTextBuffer *buffer, const gchar *source) {
  markdown_apply_tags(buffer, source, NULL, NULL);
}

#define markdown_apply_tags(buffer, source) upstream_apply((buffer), (source))

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
  assert_text(buffer, "☐ Task continuation\n");
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

static void test_entity_names(void) {
  GtkTextBuffer *buffer = new_buffer();
  markdown_apply_tags(buffer,
      "&copy; &mdash; caf&eacute; A&nbsp;B &amp; &lt; &gt; &quot; &apos; "
      "&NotEqualTilde; &Afr; &CounterClockwiseContourIntegral;");
  assert_text(buffer, "© — café A\302\240B & < > \" ' ≂̸ 𝔄 ∳");
  g_object_unref(buffer);
}

static void test_entity_numbers(void) {
  GtkTextBuffer *buffer = new_buffer();
  markdown_apply_tags(buffer, "&#169; &#x2014; &#X1F600; &#0; &#xD800; "
                               "&#xDFFF; &#1114112; &#xFFFFFF;");
  assert_text(buffer, "© — 😀 � � � � �");
  g_object_unref(buffer);
}

static void test_entity_literals(void) {
  GtkTextBuffer *buffer = new_buffer();
  const gchar *text = "&MadeUpEntity; &COPYwrong; &copy &#; &#x; &#xGG; "
                       "&#12345678; &#x1234567;";
  markdown_apply_tags(buffer, text);
  assert_text(buffer, text);
  markdown_apply_tags(buffer, "&amp;copy; &#38;copy; \\&copy; &#42;literal&#42;");
  assert_text(buffer, "&copy; &copy; &copy; *literal*");
  GtkTextIter at;
  gtk_text_buffer_get_iter_at_offset(buffer, &at, 23);
  g_assert_false(gtk_text_iter_has_tag(&at, gtk_text_tag_table_lookup(
      gtk_text_buffer_get_tag_table(buffer), "italic")));
  g_object_unref(buffer);
}

static void assert_hidden_contains(GtkTextBuffer *buffer, const gchar *needle) {
  gchar *text = buffer_text(buffer, TRUE);
  g_assert_nonnull(strstr(text, needle));
  g_free(text);
}

static void test_entities_in_code(void) {
  GtkTextBuffer *buffer = new_buffer();
  /* Code blocks are child widgets; their source lives in hidden text. */
  markdown_apply_tags(buffer, "`&copy; &#169;`\n\n```\n&amp; &#x2014;\n```\n");
  assert_hidden_contains(buffer, "&copy; &#169;");
  assert_hidden_contains(buffer, "&amp; &#x2014;");
  markdown_apply_tags(buffer, "    &copy; &#169;\n");
  assert_hidden_contains(buffer, "&copy; &#169;");
  g_object_unref(buffer);
}

static void test_entity_attributes(void) {
  GtkTextBuffer *buffer = new_buffer();
  markdown_apply_tags(buffer, "[A &amp; B](https://example.com/?a=1&amp;b=2&#35;part)");
  assert_text(buffer, "A & B");
  GtkTextIter at;
  gtk_text_buffer_get_start_iter(buffer, &at);
  GSList *tags = gtk_text_iter_get_tags(&at);
  gboolean found_url = FALSE;
  for (GSList *item = tags; item; item = item->next) {
    const gchar *url = g_object_get_data(G_OBJECT(item->data), VIEWMD_LINK_URL_DATA);
    if (url) {
      g_assert_cmpstr(url, ==, "https://example.com/?a=1&b=2#part");
      found_url = TRUE;
    }
  }
  g_assert_true(found_url);
  g_slist_free(tags);

  markdown_apply_tags(buffer, "![A &amp; B](images/a&amp;b.png)");
  gtk_text_buffer_get_start_iter(buffer, &at);
  GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(&at);
  g_assert_nonnull(anchor);
  g_assert_cmpstr(g_object_get_data(G_OBJECT(anchor), VIEWMD_IMAGE_SRC_DATA),
                  ==, "images/a&b.png");
  g_assert_cmpstr(g_object_get_data(G_OBJECT(anchor), VIEWMD_IMAGE_ALT_DATA),
                  ==, "A & B");

  markdown_apply_tags(buffer, "![A](images/a\\&amp;b.png)");
  gtk_text_buffer_get_start_iter(buffer, &at);
  anchor = gtk_text_iter_get_child_anchor(&at);
  g_assert_nonnull(anchor);
  g_assert_cmpstr(g_object_get_data(G_OBJECT(anchor), VIEWMD_IMAGE_SRC_DATA),
                  ==, "images/a&amp;b.png");
  g_object_unref(buffer);
}

static void test_entities_in_headings_and_tables(void) {
  GtkTextBuffer *buffer = new_buffer();
  markdown_apply_tags(buffer, "# Tom &amp; Jerry\n\n| Header |\n| --- |\n"
                               "| &copy; &amp; &lt;b&gt; &NotEqualTilde; |\n");
  g_assert_nonnull(gtk_text_buffer_get_mark(buffer, "viewmd-anchor-tom-jerry"));
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  gchar *text = gtk_text_buffer_get_text(buffer, &start, &end, TRUE);
  g_assert_nonnull(strstr(text, "© & <b> ≂̸"));
  g_free(text);
  g_object_unref(buffer);
}


#undef markdown_apply_tags

int main(int argc, char **argv) {
  gtk_init_check(NULL, NULL);

  config = config_new();
  config_apply_theme_colors(config);

  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/markdown/reload/link-tags", test_reload_link_tags);
  g_test_add_func("/markdown/reload/heading-anchors", test_reload_heading_anchors);
  g_test_add_func("/markdown/reload/preserve-other-state", test_reload_preserves_other_state);
  g_test_add_func("/markdown/reload/independent-buffers", test_reload_buffers_independent);
  g_test_add_func("/markdown/tasks/statuses", test_task_statuses);
  g_test_add_func("/markdown/tasks/nested", test_nested_tasks);
  g_test_add_func("/markdown/tasks/ordered", test_ordered_tasks);
  g_test_add_func("/markdown/tasks/hanging-indent", test_task_hanging_indent);
  g_test_add_func("/markdown/entities/names", test_entity_names);
  g_test_add_func("/markdown/entities/numbers", test_entity_numbers);
  g_test_add_func("/markdown/entities/literals", test_entity_literals);
  g_test_add_func("/markdown/entities/code", test_entities_in_code);
  g_test_add_func("/markdown/entities/attributes", test_entity_attributes);
  g_test_add_func("/markdown/entities/headings-and-tables", test_entities_in_headings_and_tables);
  g_test_add_func("/markdown/slug-normalization", test_slug_normalization);
  g_test_add_func("/markdown/frontmatter-table", test_frontmatter_table);
  g_test_add_func("/markdown/details-collapsed-by-default",
                  test_details_collapsed_by_default);
  g_test_add_func("/markdown/details-override", test_details_override);
  g_test_add_func("/markdown/setext-heading", test_setext_heading);
  g_test_add_func("/markdown/heading-collapse", test_heading_collapse);
  g_test_add_func("/markdown/heading-collapse-with-details",
                  test_heading_collapse_with_details);
  g_test_add_func("/markdown/heading-toc", test_heading_toc);
  g_test_add_func("/markdown/soft-break-folding", test_soft_break_folding);
  g_test_add_func("/markdown/details-in-fence", test_details_in_fence);
  g_test_add_func("/markdown/details-after-fence-info",
                  test_details_after_fence_info);
  g_test_add_func("/markdown/frontmatter-empty-values",
                  test_frontmatter_empty_values);
  g_test_add_func("/markdown/codeblock-searchable-text",
                  test_codeblock_searchable_text);
  g_test_add_func("/code/c-tokenizer", test_c_tokenizer);

  {
    gint result = g_test_run();
    config_free(config);
    config = NULL;
    return result;
  }
}
