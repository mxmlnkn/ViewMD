#ifndef MARKYD_CONFIG_H
#define MARKYD_CONFIG_H

#include <glib.h>

/* Settings structure */
typedef struct _MarkydConfig {
  /* Window geometry */
  gint window_x;
  gint window_y;
  gint window_width;
  gint window_height;
  gboolean window_maximized;

  /* Appearance */
  gchar *font_family;
  gchar *code_font_family;
  gint font_size; /* in points */
  gchar *theme;   /* "dark", "light", "system" */

  /* Document colors (only used for the "dark" and "light" themes) */
  gchar *background_color;
  gchar *text_color;

  /* Code block colors */
  gchar *code_background;
  gchar *code_foreground;

  /* Inline code colors */
  gchar *inline_code_background;
  gchar *inline_code_foreground;
  gboolean inline_code_background_enabled;

  /* Markdown accent colors */
  gchar *h1_color;
  gchar *h2_color;
  gchar *h3_color;
  gchar *list_bullet_color;

  /* Remote image loading policy: "ask", "always", "never". */
  gchar *remote_images;

  /* Table styling */
  gchar *table_border_color;
  gchar *table_row_even_background;
  gchar *table_row_odd_background;

  /* Editor */
  gboolean line_numbers;
  gboolean word_wrap;
  gboolean always_show_scrollbars;
} MarkydConfig;

/* Global config instance */
extern MarkydConfig *config;

/* Lifecycle */
MarkydConfig *config_new(void);
void config_free(MarkydConfig *cfg);

/* Apply the default color palette for the configured theme. */
void config_apply_theme_colors(MarkydConfig *cfg);

/* Load/Save */
gboolean config_load(MarkydConfig *cfg);
gboolean config_save(MarkydConfig *cfg);

/* Get config file path */
const gchar *config_get_path(void);

#endif /* MARKYD_CONFIG_H */
