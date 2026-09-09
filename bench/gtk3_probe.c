/* GTK3 baseline probe: window + header bar + notebook(2) + scrolled text view
   with a rendered-ish document, mirroring ViewMD's widget mix. */
#include <gtk/gtk.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static gboolean quit(gpointer u) {
  (void)u;
  gtk_main_quit();
  return G_SOURCE_REMOVE;
}

static GtkWidget *make_page(const gchar *label) {
  GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_ALWAYS,
                                 GTK_POLICY_ALWAYS);
  GtkTextBuffer *buf = gtk_text_buffer_new(NULL);
  GtkTextTag *h1 = gtk_text_buffer_create_tag(
      buf, "h1", "weight", PANGO_WEIGHT_BOLD, "scale", 2.0, "foreground",
      "#e6edf3", NULL);
  GtkTextIter at, end;
  gtk_text_buffer_get_start_iter(buf, &at);
  gtk_text_buffer_insert_with_tags(buf, &at, label, -1, h1, NULL);
  gtk_text_buffer_get_end_iter(buf, &end);
  for (int i = 0; i < 60; i++) {
    gchar *line =
        g_strdup_printf("\nParagraph %d: some words to fill lines and give "
                        "the view real content to lay out and scroll.",
                        i);
    gtk_text_buffer_insert(buf, &end, line, -1);
    g_free(line);
  }
  GtkWidget *tv = gtk_text_view_new_with_buffer(buf);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD_CHAR);
  gtk_container_add(GTK_CONTAINER(sw), tv);
  return sw;
}

static gboolean report(gpointer u) {
  (void)u;
  FILE *f = fopen("/proc/self/smaps_rollup", "r");
  char line[256];
  long pss = -1;
  if (f) {
    while (fgets(line, sizeof line, f)) {
      if (strncmp(line, "Pss:", 4) == 0) { pss = atol(line + 5); break; }
    }
    fclose(f);
  }
  f = fopen("/proc/self/status", "r");
  long rss = -1;
  if (f) {
    while (fgets(line, sizeof line, f)) {
      if (strncmp(line, "VmRSS:", 6) == 0) { rss = atol(line + 7); break; }
    }
    fclose(f);
  }
  g_print("PROBE VmRSS=%ldkB PSS=%ldkB\n", rss, pss);
  return G_SOURCE_REMOVE;
}

int main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_default_size(GTK_WINDOW(win), 900, 700);
  GtkWidget *hb = gtk_header_bar_new();
  gtk_header_bar_set_title(GTK_HEADER_BAR(hb), "ViewMD");
  gtk_window_set_titlebar(GTK_WINDOW(win), hb);
  GtkWidget *nb = gtk_notebook_new();
  gtk_notebook_append_page(GTK_NOTEBOOK(nb), make_page("# Doc One\n"),
                           gtk_label_new("doc-one.md"));
  gtk_notebook_append_page(GTK_NOTEBOOK(nb), make_page("# Doc Two\n"),
                           gtk_label_new("doc-two.md"));
  gtk_container_add(GTK_CONTAINER(win), nb);
  gtk_widget_show_all(win);
  g_timeout_add(2500, report, NULL);
  g_timeout_add(6000, quit, NULL);
  gtk_main();
  return 0;
}
