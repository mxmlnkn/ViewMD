#include "markdown.h"
#include "code_highlight.h"
#include "config.h"
#include "md4c/md4c.h"
#include "html_entities.h"
#include <stdlib.h>
#include <string.h>

/* Tag names */
#define TAG_H1 "h1"
#define TAG_H2 "h2"
#define TAG_H3 "h3"
#define TAG_BOLD "bold"
#define TAG_ITALIC "italic"
#define TAG_STRIKE "strike"
#define TAG_CODE "code"
#define TAG_QUOTE "quote"
#define TAG_LIST "list"
#define TAG_LIST_BULLET "list_bullet"
#define TAG_LINK "link"
#define TAG_HRULE "hrule"
#define TAG_TABLE "table"
#define TAG_TABLE_HEADER "table_header"
#define TAG_TABLE_SEPARATOR "table_separator"
#define TAG_INVISIBLE "invisible"
#define TABLE_MODEL_DATA_KEY "viewmd-table-model"
#define RENDER_STATE_DATA_KEY "viewmd-render-state"

typedef struct {
  GPtrArray *link_tags;    /* Owned GtkTextTag references. */
  GPtrArray *anchor_marks; /* Owned GtkTextMark references. */
} RenderState;

typedef struct {
  gboolean ordered;
  guint next_index;
} ListState;

typedef struct {
  gint left_margin;
  gint indent;
} ListLayoutSpec;

typedef struct {
  MD_BLOCKTYPE type;
  guint pushed_tags;
  gboolean skipped; /* block inside a collapsed section, emitted nothing */
} BlockState;

typedef struct {
  MD_SPANTYPE type;
  guint pushed_tags;
} SpanState;

typedef struct {
  gboolean is_header;
  GPtrArray *cells; /* gchar* */
} ViewmdTableRow;

typedef struct {
  guint col_count;
  GArray *aligns;   /* MD_ALIGN */
  GPtrArray *rows;  /* ViewmdTableRow* */
} ViewmdTable;

typedef struct {
  GtkTextBuffer *buffer;
  RenderState *render_state;
  GtkTextIter iter;
  GPtrArray *active_tags; /* GtkTextTag* */
  GArray *block_stack;    /* BlockState */
  GArray *span_stack;     /* SpanState */
  GArray *list_stack;     /* ListState */
  GHashTable *anchor_counts;
  GString *heading_text;
  gint heading_start_offset;
  gboolean in_heading;
  gboolean list_item_prefix_pending;
  guint quote_depth;
  gboolean in_table_head;
  ViewmdTable *table_model;
  ViewmdTableRow *table_current_row;
  GString *table_cell_text;
  guint table_current_col;
  gboolean in_image;
  gchar *image_src;
  GString *image_alt;
  gboolean in_code_block;
  GString *code_source;
  gchar *code_language; /* fenced code block language, or "" */
  /* Collapse state */
  GHashTable *collapsed; /* heading/details key -> TRUE; borrowed */
  gint heading_level;
  gboolean in_details_summary;
  GtkTextChildAnchor *current_fold_anchor;
  gchar *pending_details_id; /* details fold key, or NULL */
  gboolean pending_details_open;
  gboolean pending_details_closed; /* currently inside collapsed details */
  gint details_seq;
  gint skip_until_level; /* skip until heading of this level or shallower */
  gboolean details_skip; /* skip until the matching end-details marker */
  gint details_skip_depth; /* nesting depth of the skipped details block */
  gboolean has_output;
  guint trailing_newlines;
} RenderCtx;

static void render_state_free(gpointer data) {
  RenderState *state = data;
  g_ptr_array_free(state->link_tags, TRUE);
  g_ptr_array_free(state->anchor_marks, TRUE);
  g_free(state);
}

static RenderState *reset_render_state(GtkTextBuffer *buffer) {
  RenderState *state = g_object_get_data(G_OBJECT(buffer), RENDER_STATE_DATA_KEY);

  if (!state) {
    state = g_new0(RenderState, 1);
    state->link_tags = g_ptr_array_new_with_free_func(g_object_unref);
    state->anchor_marks = g_ptr_array_new_with_free_func(g_object_unref);
    g_object_set_data_full(G_OBJECT(buffer), RENDER_STATE_DATA_KEY, state,
                          render_state_free);
    return state;
  }

  /* Replacing text removes tag applications, but leaves tags and marks alive. */
  GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buffer);
  for (guint i = 0; i < state->link_tags->len; i++) {
    gtk_text_tag_table_remove(table, g_ptr_array_index(state->link_tags, i));
  }
  g_ptr_array_set_size(state->link_tags, 0);

  for (guint i = 0; i < state->anchor_marks->len; i++) {
    GtkTextMark *mark = g_ptr_array_index(state->anchor_marks, i);
    if (!gtk_text_mark_get_deleted(mark)) {
      gtk_text_buffer_delete_mark(buffer, mark);
    }
  }
  g_ptr_array_set_size(state->anchor_marks, 0);
  return state;
}

static void viewmd_table_row_free(gpointer data) {
  ViewmdTableRow *row = (ViewmdTableRow *)data;
  if (!row) {
    return;
  }
  if (row->cells) {
    g_ptr_array_free(row->cells, TRUE);
  }
  g_free(row);
}

static void viewmd_table_free(gpointer data) {
  ViewmdTable *table = (ViewmdTable *)data;
  if (!table) {
    return;
  }
  if (table->aligns) {
    g_array_free(table->aligns, TRUE);
  }
  if (table->rows) {
    g_ptr_array_free(table->rows, TRUE);
  }
  g_free(table);
}

static ViewmdTable *viewmd_table_new(guint col_count) {
  ViewmdTable *table = g_new0(ViewmdTable, 1);
  table->col_count = col_count;
  table->aligns = g_array_sized_new(FALSE, FALSE, sizeof(MD_ALIGN), col_count);
  table->rows = g_ptr_array_new_with_free_func(viewmd_table_row_free);
  for (guint i = 0; i < col_count; i++) {
    MD_ALIGN align = MD_ALIGN_DEFAULT;
    g_array_append_val(table->aligns, align);
  }
  return table;
}

static GtkTextTag *lookup_tag(GtkTextBuffer *buffer, const gchar *name) {
  GtkTextTagTable *table;
  if (!buffer || !name) {
    return NULL;
  }
  table = gtk_text_buffer_get_tag_table(buffer);
  return table ? gtk_text_tag_table_lookup(table, name) : NULL;
}

static void update_newline_state(RenderCtx *ctx, const gchar *text, gsize len) {
  if (!ctx || !text || len == 0) {
    return;
  }
  ctx->has_output = TRUE;
  for (gsize i = 0; i < len; i++) {
    if (text[i] == '\n') {
      ctx->trailing_newlines++;
    } else {
      ctx->trailing_newlines = 0;
    }
  }
}

static void note_non_newline_output(RenderCtx *ctx) {
  if (!ctx) {
    return;
  }
  ctx->has_output = TRUE;
  ctx->trailing_newlines = 0;
}

static void apply_tag_by_offsets(GtkTextBuffer *buffer, GtkTextTag *tag,
                                 gint start_offset, gint end_offset) {
  GtkTextIter start;
  GtkTextIter end;

  if (!buffer || !tag || end_offset <= start_offset) {
    return;
  }

  gtk_text_buffer_get_iter_at_offset(buffer, &start, start_offset);
  gtk_text_buffer_get_iter_at_offset(buffer, &end, end_offset);
  gtk_text_buffer_apply_tag(buffer, tag, &start, &end);
}

static void apply_tag_by_name_offsets(GtkTextBuffer *buffer, const gchar *name,
                                      gint start_offset, gint end_offset) {
  GtkTextTag *tag = lookup_tag(buffer, name);
  apply_tag_by_offsets(buffer, tag, start_offset, end_offset);
}

static void apply_active_tags(RenderCtx *ctx, gint start_offset,
                              gint end_offset) {
  if (!ctx || end_offset <= start_offset) {
    return;
  }
  for (guint i = 0; i < ctx->active_tags->len; i++) {
    GtkTextTag *tag = g_ptr_array_index(ctx->active_tags, i);
    apply_tag_by_offsets(ctx->buffer, tag, start_offset, end_offset);
  }
}

static void insert_text(RenderCtx *ctx, const gchar *text, gsize len) {
  gint start_offset;
  gint end_offset;

  if (!ctx || !text || len == 0) {
    return;
  }

  start_offset = gtk_text_iter_get_offset(&ctx->iter);
  gtk_text_buffer_insert(ctx->buffer, &ctx->iter, text, (gint)len);
  end_offset = gtk_text_iter_get_offset(&ctx->iter);

  apply_active_tags(ctx, start_offset, end_offset);
  update_newline_state(ctx, text, len);
}

static void insert_cstr(RenderCtx *ctx, const gchar *text) {
  if (!text) {
    return;
  }
  insert_text(ctx, text, strlen(text));
}

static void ensure_newlines(RenderCtx *ctx, guint min_newlines) {
  if (!ctx || min_newlines == 0 || !ctx->has_output) {
    return;
  }
  while (ctx->trailing_newlines < min_newlines) {
    insert_cstr(ctx, "\n");
  }
}

static guint decimal_digits(guint value) {
  guint digits = 1;
  while (value >= 10) {
    value /= 10;
    digits++;
  }
  return digits;
}

static guint list_marker_columns(RenderCtx *ctx, gboolean is_task) {
  ListState *list;
  if (is_task || !ctx || !ctx->list_stack || ctx->list_stack->len == 0) {
    return 2; /* A bullet or checkbox followed by a space. */
  }
  list = &g_array_index(ctx->list_stack, ListState, ctx->list_stack->len - 1);
  if (list->ordered) {
    return decimal_digits(MAX(1, list->next_index)) + 2; /* "12. " */
  }
  return 2; /* "• " */
}

static ListLayoutSpec compute_list_layout(guint depth, guint quote_depth,
                                          guint marker_cols) {
  ListLayoutSpec spec = {0, 0};
  guint depth_index = (depth > 0) ? (depth - 1) : 0;
  gint marker_px = (gint)CLAMP((gint)marker_cols * 9, 16, 64);
  gint base_indent = 20 + ((gint)depth_index * 22) + ((gint)quote_depth * 24);

  spec.left_margin = base_indent + marker_px;
  spec.indent = -marker_px;
  return spec;
}

static GtkTextTag *ensure_list_layout_tag(RenderCtx *ctx, guint depth,
                                          guint quote_depth,
                                          guint marker_cols) {
  gchar *name;
  GtkTextTag *tag;
  ListLayoutSpec spec;

  if (!ctx || !ctx->buffer) {
    return NULL;
  }

  name = g_strdup_printf("list_layout_%u_%u_%u", depth, quote_depth, marker_cols);
  tag = lookup_tag(ctx->buffer, name);
  if (!tag) {
    spec = compute_list_layout(depth, quote_depth, marker_cols);
    tag = gtk_text_buffer_create_tag(ctx->buffer, name, "left-margin",
                                     spec.left_margin, "indent", spec.indent, NULL);
  }
  g_free(name);
  return tag;
}

static void push_active_tag(RenderCtx *ctx, GtkTextTag *tag, guint *counter) {
  if (!ctx || !tag) {
    return;
  }
  g_ptr_array_add(ctx->active_tags, tag);
  if (counter) {
    (*counter)++;
  }
}

static void push_active_tag_by_name(RenderCtx *ctx, const gchar *name,
                                    guint *counter) {
  push_active_tag(ctx, lookup_tag(ctx->buffer, name), counter);
}

static void pop_active_tags(RenderCtx *ctx, guint count) {
  if (!ctx) {
    return;
  }
  while (count > 0 && ctx->active_tags->len > 0) {
    g_ptr_array_remove_index(ctx->active_tags, ctx->active_tags->len - 1);
    count--;
  }
}

static gint compare_entity_names(const void *key, const void *entry) {
  return strcmp(key, ((const HtmlEntity *)entry)->name);
}

static gchar *decode_entity(const MD_CHAR *text, MD_SIZE size) {
  gchar *entity = g_strndup(text, size);

  /* md4c has already validated the reference syntax, including the semicolon
   * and the number of digits. Decode only these parser-identified references. */
  if (size > 2 && entity[1] == '#') {
    const gchar *digits = entity + 2;
    guint base = 10;
    if (*digits == 'x' || *digits == 'X') {
      digits++;
      base = 16;
    }
    guint64 value = g_ascii_strtoull(digits, NULL, base);
    if (value == 0 || value > 0x10FFFF ||
        (value >= 0xD800 && value <= 0xDFFF)) {
      value = 0xFFFD;
    }
    gchar utf8[7] = {0};
    g_unichar_to_utf8((gunichar)value, utf8);
    g_free(entity);
    return g_strdup(utf8);
  }

  const HtmlEntity *match = bsearch(entity, html_entities,
      G_N_ELEMENTS(html_entities), sizeof(html_entities[0]), compare_entity_names);
  if (match) {
    g_free(entity);
    return g_strdup(match->utf8);
  }
  return entity; /* Unknown names remain literal, as required by CommonMark. */
}

static gchar *md_text_to_utf8(MD_TEXTTYPE type, const MD_CHAR *text,
                              MD_SIZE size) {
  if (type == MD_TEXT_NULLCHAR) {
    GString *s = g_string_new(NULL);
    for (MD_SIZE i = 0; i < size; i++) {
      g_string_append(s, "\xEF\xBF\xBD");
    }
    return g_string_free(s, FALSE);
  }

  if (!text || size == 0) {
    return g_strdup("");
  }
  if (type == MD_TEXT_ENTITY) {
    return decode_entity(text, size);
  }
  return g_strndup(text, size);
}

static gchar *attr_to_string(const MD_ATTRIBUTE *attr) {
  if (!attr || !attr->text || attr->size == 0) {
    return g_strdup("");
  }
  GString *result = g_string_new(NULL);
  for (guint i = 0; attr->substr_offsets[i] < attr->size; i++) {
    MD_OFFSET start = attr->substr_offsets[i];
    MD_SIZE size = attr->substr_offsets[i + 1] - start;
    gchar *part = md_text_to_utf8(attr->substr_types[i], attr->text + start, size);
    g_string_append(result, part);
    g_free(part);
  }
  return g_string_free(result, FALSE);
}

static const gchar *line_end_pos(const gchar *s) {
  const gchar *nl = strchr(s, '\n');
  if (!nl) {
    return s + strlen(s);
  }
  return nl; /* excluding '\n' */
}

static gboolean line_trimmed_equals(const gchar *line, const gchar *end,
                                    const gchar *word) {
  const gchar *p = line;
  const gchar *q = end;
  gsize len;

  while (p < end && (*p == ' ' || *p == '\t' || *p == '\r')) {
    p++;
  }
  while (q > p && (q[-1] == ' ' || q[-1] == '\t' || q[-1] == '\r')) {
    q--;
  }
  len = (gsize)(q - p);
  return len == strlen(word) && strncmp(p, word, len) == 0;
}

static gchar *strip_quotes(const gchar *text) {
  gsize len;
  gchar *out;

  if (!text) {
    return g_strdup("");
  }
  out = g_strdup(text);
  g_strstrip(out);
  len = strlen(out);
  if (len >= 2 && ((out[0] == '"' && out[len - 1] == '"') ||
                   (out[0] == '\'' && out[len - 1] == '\''))) {
    out[len - 1] = '\0';
    memmove(out, out + 1, strlen(out + 1) + 1);
  }
  return out;
}

static void frontmatter_pairs_free(gpointer data) {
  GPtrArray *pairs = (GPtrArray *)data;
  if (!pairs) {
    return;
  }
  g_ptr_array_free(pairs, TRUE);
}

static gboolean fold_is_collapsed(RenderCtx *ctx, const gchar *key,
                                  gboolean default_collapsed) {
  if (!ctx || !ctx->collapsed || !key || key[0] == '\0') {
    return default_collapsed;
  }
  /* Distinguish 'absent' from an explicit FALSE, which is stored as a NULL
     value. */
  if (g_hash_table_contains(ctx->collapsed, key)) {
    return GPOINTER_TO_INT(g_hash_table_lookup(ctx->collapsed, key)) != 0;
  }
  return default_collapsed;
}

/* Pre-pass: rewrite <details>/<summary>/</details> blocks into marker HTML
   comments plus a synthetic H6 summary line, so the renderer can recognize
   them via md4c events. The pass is fence-aware so fenced code blocks are
   left untouched. */
static gchar *transform_details(const gchar *source) {
  GPtrArray *lines;
  GString *out;
  guint i;
  guint n;

  if (!source) {
    return g_strdup("");
  }

  lines = g_ptr_array_new_with_free_func(g_free);
  {
    const gchar *p = source;
    while (p && *p) {
      const gchar *nl = strchr(p, '\n');
      gsize len = nl ? (gsize)(nl - p) : strlen(p);
      g_ptr_array_add(lines, g_strndup(p, len));
      if (!nl) {
        break;
      }
      p = nl + 1;
    }
  }

  out = g_string_new(NULL);
  n = lines->len;
  gboolean in_fence = FALSE;
  gchar fence_char = 0;
  guint fence_len = 0;
  for (i = 0; i < n; i++) {
    const gchar *line = g_ptr_array_index(lines, i);
    const gchar *p = line;
    const gchar *trimmed_start;
    gchar *trimmed;
    gboolean fence_line = FALSE;
    gboolean fence_close = FALSE;
    gchar line_fence_char = 0;
    guint line_fence_len = 0;

    while (*p == ' ' || *p == '\t') {
      p++;
    }
    trimmed_start = p;

    if ((trimmed_start[0] == '`' && strncmp(trimmed_start, "```", 3) == 0) ||
        (trimmed_start[0] == '~' && strncmp(trimmed_start, "~~~", 3) == 0)) {
      gint run = 0;
      while (trimmed_start[run] == trimmed_start[0]) {
        run++;
      }
      if (run >= 3) {
        const gchar *rest = trimmed_start + run;
        while (*rest == ' ' || *rest == '\t') {
          rest++;
        }
        fence_line = TRUE;
        fence_close = (*rest == '\0');
        line_fence_char = trimmed_start[0];
        line_fence_len = (guint)run;
      }
    }

    if (in_fence) {
      if (fence_close && line_fence_char == fence_char &&
          line_fence_len >= fence_len) {
        in_fence = FALSE;
      }
      g_string_append(out, line);
      g_string_append_c(out, '\n');
      continue;
    }
    if (fence_line) {
      in_fence = TRUE;
      fence_char = line_fence_char;
      fence_len = line_fence_len;
      g_string_append(out, line);
      g_string_append_c(out, '\n');
      continue;
    }

    trimmed = g_strdup(line);
    g_strstrip(trimmed);

    if (g_str_has_prefix(trimmed, "<details") &&
        (trimmed[8] == '>' || trimmed[8] == ' ' || trimmed[8] == '\t' ||
         trimmed[8] == '\0') &&
        g_strstr_len(trimmed, -1, "</details>") == NULL) {
      gboolean open =
          g_strstr_len(trimmed, -1, " open") != NULL ||
          g_strstr_len(trimmed, -1, "open>") != NULL;
      guint id = i; /* unique-enough within the document */
      g_string_append_printf(out, "<!--vmd-details %u %d-->\n", id,
                             open ? 1 : 0);
      /* Inject a default summary heading when no <summary> follows. */
      if (i + 1 >= n || !g_str_has_prefix(g_ptr_array_index(lines, i + 1),
                                          "<summary")) {
        g_string_append_printf(out, "###### Details\n");
      }
    } else if (g_str_has_prefix(trimmed, "</details>")) {
      g_string_append_printf(out, "<!--vmd-details-end %u-->\n", i);
    } else if (g_str_has_prefix(trimmed, "<summary") &&
               (trimmed[8] == '>' || trimmed[8] == ' ' ||
                trimmed[8] == '\t')) {
      const gchar *gt = strchr(trimmed, '>');
      const gchar *close;
      gchar *inner = NULL;
      if (gt) {
        close = strstr(gt + 1, "</summary>");
        if (close) {
          inner = g_strndup(gt + 1, (gsize)(close - (gt + 1)));
        }
      }
      g_string_append(out, "###### ");
      g_string_append(out, inner ? inner : "Details");
      g_string_append_c(out, '\n');
      g_free(inner);
    } else {
      g_string_append(out, line);
      g_string_append_c(out, '\n');
    }

    g_free(trimmed);
  }

  g_ptr_array_free(lines, TRUE);
  return g_string_free(out, FALSE);
}

/* Returns a GPtrArray of "key\tvalue" strings for a leading YAML frontmatter
   block, or NULL. *out_body points past the closing delimiter. */
static GPtrArray *parse_frontmatter(const gchar *source,
                                    const gchar **out_body) {
  const gchar *cursor;
  const gchar *block_start;
  const gchar *block_end;
  const gchar *line;
  GPtrArray *pairs;
  gboolean has_key = FALSE;

  *out_body = source;
  if (!source || strncmp(source, "---", 3) != 0 ||
      !line_trimmed_equals(source, line_end_pos(source), "---")) {
    return NULL;
  }

  cursor = line_end_pos(source);
  if (*cursor == '\n') {
    cursor++;
  }
  block_start = cursor;
  block_end = NULL;
  while (*cursor) {
    const gchar *le = line_end_pos(cursor);
    if (line_trimmed_equals(cursor, le, "---") ||
        line_trimmed_equals(cursor, le, "...")) {
      block_end = cursor;
      break;
    }
    if (!*le) {
      break;
    }
    cursor = le;
    if (*cursor == '\n') {
      cursor++;
    }
  }
  if (!block_end) {
    return NULL;
  }

  cursor = line_end_pos(block_end);
  if (*cursor == '\n') {
    cursor++;
  }

  pairs = g_ptr_array_new_with_free_func(g_free);
  line = block_start;
  while (line < block_end) {
    const gchar *le = line_end_pos(line);
    const gchar *sep = memchr(line, ':', (gsize)(le - line));
    if (sep && !line_trimmed_equals(line, le, "")) {
      gchar *key = g_strndup(line, (gsize)(sep - line));
      g_strstrip(key);
      if (key[0] != '\0' && key[0] != '#') {
        gchar *raw_value;
        gchar *value;
        has_key = TRUE;
        raw_value = g_strndup(sep + 1, (gsize)(le - (sep + 1)));
        value = strip_quotes(raw_value);
        if (value[0] != '\0') {
          g_ptr_array_add(pairs, g_strdup_printf("%s\t%s", key, value));
        }
        g_free(value);
        g_free(raw_value);
      }
      g_free(key);
    }
    if (!*le) {
      break;
    }
    line = le;
    if (*line == '\n') {
      line++;
    }
  }

  if (!has_key) {
    g_ptr_array_free(pairs, TRUE);
    return NULL;
  }
  *out_body = cursor;
  if (pairs->len == 0) {
    g_ptr_array_free(pairs, TRUE);
    return NULL;
  }
  return pairs;
}

static gchar *extract_code_language_from_detail(MD_BLOCK_CODE_DETAIL *code) {
  gchar *lang;
  gchar *info;
  gchar *trimmed;
  const gchar *end;

  if (!code) {
    return NULL;
  }

  lang = attr_to_string(&code->lang);
  g_strstrip(lang);
  if (lang[0] != '\0') {
    return lang;
  }
  g_free(lang);

  info = attr_to_string(&code->info);
  trimmed = g_strstrip(info);
  if (trimmed[0] == '\0') {
    g_free(info);
    return NULL;
  }

  end = trimmed;
  while (*end && !g_ascii_isspace(*end)) {
    end++;
  }

  lang = g_strndup(trimmed, (gsize)(end - trimmed));
  g_free(info);
  return lang;
}

typedef struct {
  GtkTextBuffer *buffer;
  gint line_offset;
} CodeTagApplyContext;

static void on_code_scan_token(gint start_char_offset, gint end_char_offset,
                               const gchar *tag_name, gpointer user_data) {
  CodeTagApplyContext *ctx = (CodeTagApplyContext *)user_data;

  if (!ctx || !ctx->buffer || !tag_name || end_char_offset <= start_char_offset) {
    return;
  }

  apply_tag_by_name_offsets(ctx->buffer, tag_name, ctx->line_offset + start_char_offset,
                            ctx->line_offset + end_char_offset);
}

static void ensure_code_tags(GtkTextBuffer *buffer) {
  const gchar *kw_a;
  const gchar *kw_b;
  const gchar *kw_c;
  const gchar *literal;
  gboolean light = g_strcmp0(config->theme, "light") == 0;

  if (!buffer || lookup_tag(buffer, MARKYD_TAG_CODE_KW_A)) {
    return;
  }

  if (light) {
    kw_a = "#cf222e";
    kw_b = "#953800";
    kw_c = "#0550ae";
    literal = "#0a3069";
  } else {
    kw_a = "#ff7b72";
    kw_b = "#ffa657";
    kw_c = "#79c0ff";
    literal = "#a5d6ff";
  }

  gtk_text_buffer_create_tag(buffer, MARKYD_TAG_CODE_KW_A, "family",
                             config->code_font_family, "foreground", kw_a,
                             "weight", PANGO_WEIGHT_BOLD, NULL);
  gtk_text_buffer_create_tag(buffer, MARKYD_TAG_CODE_KW_B, "family",
                             config->code_font_family, "foreground", kw_b,
                             "weight", PANGO_WEIGHT_BOLD, NULL);
  gtk_text_buffer_create_tag(buffer, MARKYD_TAG_CODE_KW_C, "family",
                             config->code_font_family, "foreground", kw_c,
                             "weight", PANGO_WEIGHT_BOLD, NULL);
  gtk_text_buffer_create_tag(buffer, MARKYD_TAG_CODE_LITERAL, "family",
                             config->code_font_family, "foreground", literal,
                             NULL);
}

/* Create a new buffer with the code text and (if a known language) highlighted
   syntax tags applied. The caller owns the buffer and must drop its ref. */
static GtkTextBuffer *
create_highlighted_code_buffer(const gchar *code,
                               const MarkydLanguageHighlight *language) {
  GtkTextBuffer *buffer = gtk_text_buffer_new(NULL);
  const gchar *line_start = code ? code : "";
  MarkydCodeScanState state = {0};
  CodeTagApplyContext scan_ctx = {0};
  GtkTextTag *base_tag;
  GtkTextIter start;
  GtkTextIter end;
  gint line_offset = 0;

  gtk_text_buffer_set_text(buffer, line_start, -1);

  /* Base color/mono font for untagged text; syntax tags layer on top. */
  base_tag = gtk_text_buffer_create_tag(buffer, "viewmd-code-plain", "family",
                                        config->code_font_family, "foreground",
                                        config->code_foreground, NULL);
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  gtk_text_buffer_apply_tag(buffer, base_tag, &start, &end);

  if (!language || !language->scan_line) {
    return buffer;
  }

  ensure_code_tags(buffer);
  markyd_code_scan_state_reset(&state);
  scan_ctx.buffer = buffer;

  while (TRUE) {
    const gchar *nl = strchr(line_start, '\n');
    gsize len = nl ? (gsize)(nl - line_start) : strlen(line_start);
    gchar *line = g_strndup(line_start, len);

    scan_ctx.line_offset = line_offset;
    markyd_code_scan_line(language, line, &state, on_code_scan_token, &scan_ctx);
    line_offset += (gint)g_utf8_strlen(line, -1);
    g_free(line);

    if (!nl) {
      break;
    }
    line_offset += 1;
    line_start = nl + 1;
  }

  return buffer;
}

static void on_code_copy_clicked(GtkButton *button, gpointer user_data) {
  const gchar *code = (const gchar *)user_data;
  GtkClipboard *clip = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
  (void)button;
  gtk_clipboard_set_text(clip, code ? code : "", -1);
}

static void code_widget_text_view_setup(GtkTextView *text_view) {
  gtk_text_view_set_editable(text_view, FALSE);
  gtk_text_view_set_cursor_visible(text_view, FALSE);
  /* Long lines wrap instead of blowing the block up to thousands of pixels
     (that also produced invalid 'width: 2599, height: -1' allocations while
     scrolling). */
  gtk_text_view_set_wrap_mode(text_view, GTK_WRAP_CHAR);
  gtk_widget_set_size_request(GTK_WIDGET(text_view), 480, -1);
  gtk_text_view_set_top_margin(text_view, 8);
  gtk_text_view_set_bottom_margin(text_view, 8);
  gtk_text_view_set_left_margin(text_view, 12);
  gtk_text_view_set_right_margin(text_view, 12);
  gtk_widget_set_hexpand(GTK_WIDGET(text_view), TRUE);
  gtk_text_view_set_accepts_tab(text_view, FALSE);
}

GtkWidget *markdown_create_code_block_widget(GtkTextChildAnchor *anchor) {
  gchar *code;
  gchar *lang;
  GtkWidget *box;
  GtkWidget *header;
  GtkWidget *lang_label;
  GtkWidget *copy_button;
  GtkTextBuffer *code_buffer;
  GtkWidget *code_view;
  const MarkydLanguageHighlight *language;

  if (!anchor) {
    return NULL;
  }
  code = g_object_get_data(G_OBJECT(anchor), VIEWMD_CODE_SRC_DATA);
  lang = g_object_get_data(G_OBJECT(anchor), VIEWMD_CODE_LANG_DATA);
  language = (lang && lang[0]) ? markyd_code_lookup_language(lang) : NULL;

  box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_style_context_add_class(gtk_widget_get_style_context(box),
                              "viewmd-code-block");
  gtk_widget_set_halign(box, GTK_ALIGN_FILL);
  gtk_widget_set_valign(box, GTK_ALIGN_START);
  gtk_widget_set_hexpand(box, TRUE);
  gtk_widget_set_vexpand(box, FALSE);
  gtk_widget_set_margin_top(box, 8);
  gtk_widget_set_margin_bottom(box, 8);
  gtk_widget_set_margin_start(box, 4);
  gtk_widget_set_margin_end(box, 4);

  header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_style_context_add_class(gtk_widget_get_style_context(header),
                              "viewmd-code-header");
  gtk_box_pack_start(GTK_BOX(box), header, FALSE, FALSE, 0);

  lang_label = gtk_label_new(lang && lang[0] ? lang : "code");
  gtk_widget_set_halign(lang_label, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(header), lang_label, TRUE, TRUE, 0);

  copy_button = gtk_button_new_from_icon_name("edit-copy-symbolic",
                                               GTK_ICON_SIZE_MENU);
  gtk_widget_set_focus_on_click(copy_button, FALSE);
  gtk_button_set_relief(GTK_BUTTON(copy_button), GTK_RELIEF_NONE);
  gtk_widget_set_tooltip_text(copy_button, "Copy code to clipboard");
  gtk_style_context_add_class(gtk_widget_get_style_context(copy_button),
                              "viewmd-copy-button");
  g_signal_connect(copy_button, "clicked", G_CALLBACK(on_code_copy_clicked),
                   code);
  gtk_box_pack_end(GTK_BOX(header), copy_button, FALSE, FALSE, 0);

  code_buffer = create_highlighted_code_buffer(code, language);
  code_view = gtk_text_view_new_with_buffer(code_buffer);
  code_widget_text_view_setup(GTK_TEXT_VIEW(code_view));
  g_object_unref(code_buffer);
  gtk_box_pack_start(GTK_BOX(box), code_view, FALSE, FALSE, 0);

  return box;
}

static void emit_code_block_anchor(RenderCtx *ctx) {
  GtkTextChildAnchor *anchor;
  gchar *code;
  gint start_offset;
  gint end_offset;
  gboolean saved_has_output;
  guint saved_trailing_newlines;

  if (!ctx || !ctx->code_source) {
    return;
  }

  code = g_strdup(ctx->code_source->str);
  anchor = gtk_text_buffer_create_child_anchor(ctx->buffer, &ctx->iter);
  note_non_newline_output(ctx);
  g_object_set_data(G_OBJECT(anchor), VIEWMD_CODE_BLOCK_DATA,
                    GINT_TO_POINTER(1));
  g_object_set_data_full(G_OBJECT(anchor), VIEWMD_CODE_SRC_DATA, code,
                         g_free);
  g_object_set_data_full(G_OBJECT(anchor), VIEWMD_CODE_LANG_DATA,
                         g_strdup(ctx->code_language ? ctx->code_language : ""),
                         g_free);

  /* Keep the source text findable via Ctrl+F without showing it twice. */
  saved_has_output = ctx->has_output;
  saved_trailing_newlines = ctx->trailing_newlines;
  start_offset = gtk_text_iter_get_offset(&ctx->iter);
  gtk_text_buffer_insert(ctx->buffer, &ctx->iter, code, -1);
  end_offset = gtk_text_iter_get_offset(&ctx->iter);
  if (end_offset > start_offset) {
    apply_tag_by_name_offsets(ctx->buffer, TAG_INVISIBLE, start_offset,
                              end_offset);
  }
  ctx->has_output = saved_has_output;
  ctx->trailing_newlines = saved_trailing_newlines;

  insert_cstr(ctx, "\n");
}

gchar *markdown_normalize_anchor_slug(const gchar *text) {
  gchar *decoded;
  const gchar *p;
  GString *out;
  gboolean prev_dash = TRUE;

  if (!text) {
    return g_strdup("");
  }

  decoded = g_uri_unescape_string(text, NULL);
  p = decoded ? decoded : text;
  out = g_string_new(NULL);

  while (*p) {
    gunichar c = g_utf8_get_char(p);
    if (g_unichar_isalnum(c)) {
      gchar utf8[8];
      gint len = g_unichar_to_utf8(g_unichar_tolower(c), utf8);
      utf8[len] = '\0';
      g_string_append_len(out, utf8, len);
      prev_dash = FALSE;
    } else if (c == ' ' || c == '-' || c == '_') {
      if (!prev_dash) {
        g_string_append_c(out, '-');
        prev_dash = TRUE;
      }
    }
    p = g_utf8_next_char(p);
  }

  while (out->len > 0 && out->str[out->len - 1] == '-') {
    g_string_truncate(out, out->len - 1);
  }

  g_free(decoded);
  return g_string_free(out, FALSE);
}

gchar *markdown_anchor_mark_name(const gchar *fragment) {
  gchar *slug = markdown_normalize_anchor_slug(fragment);
  gchar *name = g_strdup_printf("%s%s", VIEWMD_ANCHOR_MARK_PREFIX, slug);
  g_free(slug);
  return name;
}

static void capture_heading_text(RenderCtx *ctx, const gchar *text) {
  if (!ctx || !ctx->in_heading || !ctx->heading_text || !text) {
    return;
  }
  for (const gchar *p = text; *p != '\0'; p++) {
    g_string_append_c(ctx->heading_text, (*p == '\n' || *p == '\r') ? ' ' : *p);
  }
}

static gchar *heading_anchor_key(RenderCtx *ctx) {
  gchar *base;
  guint count;
  gchar *slug;

  if (!ctx || !ctx->heading_text || ctx->heading_text->len == 0) {
    return NULL;
  }

  base = markdown_normalize_anchor_slug(ctx->heading_text->str);
  if (!base || base[0] == '\0') {
    g_free(base);
    return NULL;
  }

  count = GPOINTER_TO_UINT(g_hash_table_lookup(ctx->anchor_counts, base));
  slug = (count == 0) ? g_strdup(base) : g_strdup_printf("%s-%u", base, count);
  g_hash_table_replace(ctx->anchor_counts, g_strdup(base),
                       GUINT_TO_POINTER(count + 1));

  g_free(base);
  return slug;
}

static gchar *create_heading_anchor(RenderCtx *ctx) {
  gchar *slug;
  gchar *mark_name;
  GtkTextIter at;

  if (!ctx || !ctx->heading_text || ctx->heading_text->len == 0) {
    return NULL;
  }

  slug = heading_anchor_key(ctx);
  if (!slug) {
    return NULL;
  }

  mark_name = g_strdup_printf("%s%s", VIEWMD_ANCHOR_MARK_PREFIX, slug);
  gtk_text_buffer_get_iter_at_offset(ctx->buffer, &at, ctx->heading_start_offset);
  GtkTextMark *mark = gtk_text_buffer_create_mark(ctx->buffer, mark_name, &at, TRUE);
  if (mark) {
    g_ptr_array_add(ctx->render_state->anchor_marks, g_object_ref(mark));
  }

  g_free(mark_name);
  return slug;
}

static void table_capture_append(RenderCtx *ctx, const gchar *text) {
  if (!ctx || !ctx->table_cell_text || !text) {
    return;
  }
  gchar *escaped = g_markup_escape_text(text, -1);
  g_string_append(ctx->table_cell_text, escaped);
  g_free(escaped);
}

static gchar *table_cell_markup_to_plain(const gchar *markup) {
  gchar *plain = NULL;
  GError *error = NULL;

  if (!markup || markup[0] == '\0') {
    return g_strdup("");
  }

  if (pango_parse_markup(markup, -1, 0, NULL, &plain, NULL, &error)) {
    return plain;
  }

  if (error) {
    g_error_free(error);
  }
  return g_strdup(markup);
}

static void table_search_index_free(gpointer data) {
  ViewmdTableSearchIndex *index = (ViewmdTableSearchIndex *)data;
  if (!index) {
    return;
  }
  if (index->cells) {
    g_array_free(index->cells, TRUE);
  }
  g_free(index);
}

static void table_emit_hidden_search_text(RenderCtx *ctx, ViewmdTable *table,
                                          GtkTextChildAnchor *anchor) {
  ViewmdTableSearchIndex *index;
  gboolean saved_has_output;
  guint saved_trailing_newlines;
  const gchar *cell_sep = " ";

  if (!ctx || !ctx->buffer || !table || !anchor || !table->rows ||
      table->rows->len == 0 || table->col_count == 0) {
    return;
  }

  /* Hidden search text must not affect visible block spacing state. */
  saved_has_output = ctx->has_output;
  saved_trailing_newlines = ctx->trailing_newlines;

  index = g_new0(ViewmdTableSearchIndex, 1);
  index->cells = g_array_new(FALSE, FALSE, sizeof(ViewmdTableSearchCellRange));
  index->start_offset = gtk_text_iter_get_offset(&ctx->iter);

  for (guint r = 0; r < table->rows->len; r++) {
    ViewmdTableRow *row = g_ptr_array_index(table->rows, r);
    if (!row) {
      continue;
    }

    for (guint c = 0; c < table->col_count; c++) {
      const gchar *cell_markup = "";
      gchar *plain;
      gint cell_start;
      gint cell_end;

      if (c < row->cells->len) {
        cell_markup = g_ptr_array_index(row->cells, c);
        if (!cell_markup) {
          cell_markup = "";
        }
      }

      plain = table_cell_markup_to_plain(cell_markup);
      cell_start = gtk_text_iter_get_offset(&ctx->iter);
      if (plain && plain[0] != '\0') {
        gtk_text_buffer_insert(ctx->buffer, &ctx->iter, plain, -1);
      }
      cell_end = gtk_text_iter_get_offset(&ctx->iter);

      if (cell_end > cell_start) {
        ViewmdTableSearchCellRange cell_range = {(gint)r, (gint)c, cell_start,
                                                 cell_end};
        g_array_append_val(index->cells, cell_range);
      }

      g_free(plain);

      if (!(r + 1 == table->rows->len && c + 1 == table->col_count)) {
        gtk_text_buffer_insert(ctx->buffer, &ctx->iter, cell_sep, -1);
      }
    }
  }

  index->end_offset = gtk_text_iter_get_offset(&ctx->iter);
  if (index->end_offset > index->start_offset) {
    apply_tag_by_name_offsets(ctx->buffer, TAG_INVISIBLE, index->start_offset,
                              index->end_offset);
    g_object_set_data_full(G_OBJECT(anchor), VIEWMD_TABLE_SEARCH_INDEX_DATA, index,
                           table_search_index_free);
  } else {
    table_search_index_free(index);
  }

  ctx->has_output = saved_has_output;
  ctx->trailing_newlines = saved_trailing_newlines;
}

static void table_capture_span_enter(RenderCtx *ctx, MD_SPANTYPE type) {
  if (!ctx || !ctx->table_cell_text) {
    return;
  }

  switch (type) {
  case MD_SPAN_EM:
    g_string_append(ctx->table_cell_text, "<i>");
    break;
  case MD_SPAN_STRONG:
    g_string_append(ctx->table_cell_text, "<b>");
    break;
  case MD_SPAN_CODE:
    g_string_append(ctx->table_cell_text, "<span font_family='monospace'>");
    break;
  case MD_SPAN_DEL:
    g_string_append(ctx->table_cell_text, "<s>");
    break;
  case MD_SPAN_A:
    g_string_append(ctx->table_cell_text, "<u>");
    break;
  default:
    break;
  }
}

static void table_capture_span_leave(RenderCtx *ctx, MD_SPANTYPE type) {
  if (!ctx || !ctx->table_cell_text) {
    return;
  }

  switch (type) {
  case MD_SPAN_EM:
    g_string_append(ctx->table_cell_text, "</i>");
    break;
  case MD_SPAN_STRONG:
    g_string_append(ctx->table_cell_text, "</b>");
    break;
  case MD_SPAN_CODE:
    g_string_append(ctx->table_cell_text, "</span>");
    break;
  case MD_SPAN_DEL:
    g_string_append(ctx->table_cell_text, "</s>");
    break;
  case MD_SPAN_A:
    g_string_append(ctx->table_cell_text, "</u>");
    break;
  default:
    break;
  }
}

static void table_start_row(RenderCtx *ctx) {
  ViewmdTableRow *row;
  if (!ctx || !ctx->table_model) {
    return;
  }
  row = g_new0(ViewmdTableRow, 1);
  row->is_header = ctx->in_table_head;
  row->cells = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(ctx->table_model->rows, row);
  ctx->table_current_row = row;
  ctx->table_current_col = 0;
}

static void table_start_cell(RenderCtx *ctx, void *detail) {
  MD_BLOCK_TD_DETAIL *td = (MD_BLOCK_TD_DETAIL *)detail;
  if (!ctx || !ctx->table_current_row) {
    return;
  }

  if (ctx->table_cell_text) {
    g_string_free(ctx->table_cell_text, TRUE);
  }
  ctx->table_cell_text = g_string_new(NULL);

  if (ctx->table_model && ctx->table_current_col < ctx->table_model->aligns->len) {
    g_array_index(ctx->table_model->aligns, MD_ALIGN, ctx->table_current_col) =
        td ? td->align : MD_ALIGN_DEFAULT;
  }
}

static void table_finish_cell(RenderCtx *ctx) {
  gchar *cell;
  if (!ctx || !ctx->table_current_row || !ctx->table_cell_text) {
    return;
  }
  cell = g_strdup(ctx->table_cell_text->str);
  g_strstrip(cell);
  g_ptr_array_add(ctx->table_current_row->cells, cell);
  g_string_free(ctx->table_cell_text, TRUE);
  ctx->table_cell_text = NULL;
  ctx->table_current_col++;
}

static void table_finish_row(RenderCtx *ctx) {
  if (!ctx || !ctx->table_current_row || !ctx->table_model) {
    return;
  }
  while (ctx->table_current_row->cells->len < ctx->table_model->col_count) {
    g_ptr_array_add(ctx->table_current_row->cells, g_strdup(""));
  }
  ctx->table_current_row = NULL;
}

static void table_emit_anchor(RenderCtx *ctx) {
  GtkTextChildAnchor *anchor;

  if (!ctx || !ctx->table_model) {
    return;
  }
  if (ctx->table_model->rows->len == 0 || ctx->table_model->col_count == 0) {
    viewmd_table_free(ctx->table_model);
    ctx->table_model = NULL;
    return;
  }

  anchor = gtk_text_buffer_create_child_anchor(ctx->buffer, &ctx->iter);
  note_non_newline_output(ctx);
  g_object_set_data(G_OBJECT(anchor), VIEWMD_TABLE_ANCHOR_DATA, GINT_TO_POINTER(1));
  g_object_set_data_full(G_OBJECT(anchor), TABLE_MODEL_DATA_KEY, ctx->table_model,
                         viewmd_table_free);

  /* Keep table text searchable via Ctrl+F without showing duplicate content. */
  table_emit_hidden_search_text(ctx, ctx->table_model, anchor);

  ctx->table_model = NULL;
}

static void image_emit_anchor(RenderCtx *ctx) {
  GtkTextChildAnchor *anchor;
  gchar *alt = NULL;

  if (!ctx || !ctx->buffer || !ctx->image_src || ctx->image_src[0] == '\0') {
    return;
  }

  anchor = gtk_text_buffer_create_child_anchor(ctx->buffer, &ctx->iter);
  note_non_newline_output(ctx);
  g_object_set_data(G_OBJECT(anchor), VIEWMD_IMAGE_ANCHOR_DATA, GINT_TO_POINTER(1));
  g_object_set_data_full(G_OBJECT(anchor), VIEWMD_IMAGE_SRC_DATA,
                         g_strdup(ctx->image_src), g_free);

  if (ctx->image_alt && ctx->image_alt->len > 0) {
    alt = g_strdup(ctx->image_alt->str);
  } else {
    alt = g_strdup("");
  }
  g_object_set_data_full(G_OBJECT(anchor), VIEWMD_IMAGE_ALT_DATA, alt, g_free);

  /* Keep following markdown on a new visual line after embedded images. */
  insert_cstr(ctx, "\n");
}

static void insert_list_marker(RenderCtx *ctx, const MD_BLOCK_LI_DETAIL *detail) {
  ListState *list;
  gchar *ordered = NULL;
  gint marker_start_offset;
  gint marker_end_offset;

  if (!ctx || ctx->list_stack->len == 0) {
    return;
  }

  list = &g_array_index(ctx->list_stack, ListState, ctx->list_stack->len - 1);

  marker_start_offset = gtk_text_iter_get_offset(&ctx->iter);
  if (detail && detail->is_task) {
    insert_cstr(ctx, (detail->task_mark == 'x' || detail->task_mark == 'X')
                         ? "\xE2\x98\x91" /* ☑ */
                         : "\xE2\x98\x90"); /* ☐ */
  } else if (list->ordered) {
    ordered = g_strdup_printf("%u.", list->next_index);
    insert_cstr(ctx, ordered);
  } else {
    insert_cstr(ctx, "\xE2\x80\xA2");
  }
  marker_end_offset = gtk_text_iter_get_offset(&ctx->iter);
  apply_tag_by_name_offsets(ctx->buffer, TAG_LIST_BULLET, marker_start_offset,
                            marker_end_offset);

  insert_cstr(ctx, " ");
  if (list->ordered) {
    list->next_index++;
  }
  g_free(ordered);
}

static int on_enter_block(MD_BLOCKTYPE type, void *detail, void *userdata) {
  RenderCtx *ctx = (RenderCtx *)userdata;
  BlockState state = {type, 0, FALSE};

  /* Skip content inside collapsed sections; only headings shallower than the
     collapsed one or HTML blocks (for details end markers) get through. */
  if (ctx->skip_until_level > 0 || ctx->details_skip) {
    gboolean resume_heading = FALSE;
    if (ctx->details_skip) {
      if (type == MD_BLOCK_HTML) {
        /* let markers through; on_text decides what to render */
      } else {
        state.skipped = TRUE;
        g_array_append_val(ctx->block_stack, state);
        return 0;
      }
    } else if (type == MD_BLOCK_H) {
      MD_BLOCK_H_DETAIL *h = (MD_BLOCK_H_DETAIL *)detail;
      gint level = h ? h->level : 1;
      if (level <= ctx->skip_until_level) {
        ctx->skip_until_level = 0;
        resume_heading = TRUE;
      } else {
        state.skipped = TRUE;
        g_array_append_val(ctx->block_stack, state);
        return 0;
      }
    } else {
      state.skipped = TRUE;
      g_array_append_val(ctx->block_stack, state);
      return 0;
    }
    (void)resume_heading;
  }

  g_array_append_val(ctx->block_stack, state);

  switch (type) {
  case MD_BLOCK_DOC:
    break;

  case MD_BLOCK_P:
    if (ctx->list_item_prefix_pending) {
      ctx->list_item_prefix_pending = FALSE;
    } else {
      ensure_newlines(ctx, (ctx->list_stack->len > 0) ? 1 : 2);
    }
    break;

  case MD_BLOCK_H: {
    MD_BLOCK_H_DETAIL *h = (MD_BLOCK_H_DETAIL *)detail;
    gboolean is_details = (ctx->pending_details_id != NULL);

    ensure_newlines(ctx, 2);
    ctx->heading_start_offset = gtk_text_iter_get_offset(&ctx->iter);
    ctx->heading_level = h ? h->level : 1;
    ctx->in_heading = TRUE;
    if (!ctx->heading_text) {
      ctx->heading_text = g_string_new(NULL);
    } else {
      g_string_set_size(ctx->heading_text, 0);
    }

    if (h && h->level == 1) {
      push_active_tag_by_name(ctx, TAG_H1,
                              &g_array_index(ctx->block_stack, BlockState,
                                             ctx->block_stack->len - 1)
                                   .pushed_tags);
    } else if (h && h->level == 2) {
      push_active_tag_by_name(ctx, TAG_H2,
                              &g_array_index(ctx->block_stack, BlockState,
                                             ctx->block_stack->len - 1)
                                   .pushed_tags);
    } else {
      push_active_tag_by_name(ctx, TAG_H3,
                              &g_array_index(ctx->block_stack, BlockState,
                                             ctx->block_stack->len - 1)
                                   .pushed_tags);
    }

    /* Fold toggle anchor sits just before the heading text. */
    ctx->current_fold_anchor =
        gtk_text_buffer_create_child_anchor(ctx->buffer, &ctx->iter);
    g_object_set_data(G_OBJECT(ctx->current_fold_anchor),
                      VIEWMD_FOLD_ANCHOR_DATA, GINT_TO_POINTER(1));
    g_object_set_data(G_OBJECT(ctx->current_fold_anchor),
                      VIEWMD_FOLD_IS_DETAILS_DATA,
                      GINT_TO_POINTER(is_details ? 1 : 0));
    if (is_details) {
      g_object_set_data_full(G_OBJECT(ctx->current_fold_anchor),
                             VIEWMD_FOLD_KEY_DATA,
                             g_strdup(ctx->pending_details_id), g_free);
      ctx->in_details_summary = TRUE;
    }
    break;
  }

  case MD_BLOCK_QUOTE:
    ensure_newlines(ctx, 2);
    ctx->quote_depth++;
    push_active_tag_by_name(ctx, TAG_QUOTE,
                            &g_array_index(ctx->block_stack, BlockState,
                                           ctx->block_stack->len - 1)
                                 .pushed_tags);
    break;

  case MD_BLOCK_UL: {
    ListState list = {FALSE, 1};
    ensure_newlines(ctx, (ctx->list_stack->len > 0) ? 1 : 2);
    g_array_append_val(ctx->list_stack, list);
    break;
  }

  case MD_BLOCK_OL: {
    MD_BLOCK_OL_DETAIL *ol = (MD_BLOCK_OL_DETAIL *)detail;
    ListState list = {TRUE, (ol && ol->start > 0) ? ol->start : 1};
    ensure_newlines(ctx, (ctx->list_stack->len > 0) ? 1 : 2);
    g_array_append_val(ctx->list_stack, list);
    break;
  }

  case MD_BLOCK_LI:
    {
      MD_BLOCK_LI_DETAIL *li = detail;
      guint marker_cols = list_marker_columns(ctx, li && li->is_task);
      GtkTextTag *layout_tag = ensure_list_layout_tag(
          ctx, ctx->list_stack->len, ctx->quote_depth, marker_cols);

      ensure_newlines(ctx, 1);
      push_active_tag(ctx, layout_tag,
                      &g_array_index(ctx->block_stack, BlockState,
                                     ctx->block_stack->len - 1)
                           .pushed_tags);
      insert_list_marker(ctx, li);
      ctx->list_item_prefix_pending = TRUE;
    }
    break;

  case MD_BLOCK_HR: {
    gint start_offset;
    gint end_offset;
    ensure_newlines(ctx, 2);
    start_offset = gtk_text_iter_get_offset(&ctx->iter);
    insert_cstr(ctx, "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80");
    end_offset = gtk_text_iter_get_offset(&ctx->iter);
    apply_tag_by_name_offsets(ctx->buffer, TAG_HRULE, start_offset, end_offset);
    ensure_newlines(ctx, 2);
    break;
  }

  case MD_BLOCK_CODE:
    ensure_newlines(ctx, 2);
    ctx->in_code_block = TRUE;
    if (ctx->code_source) {
      g_string_free(ctx->code_source, TRUE);
    }
    ctx->code_source = g_string_new(NULL);
    g_free(ctx->code_language);
    ctx->code_language =
        extract_code_language_from_detail((MD_BLOCK_CODE_DETAIL *)detail);
    if (!ctx->code_language) {
      ctx->code_language = g_strdup("");
    }
    break;

  case MD_BLOCK_HTML:
    ensure_newlines(ctx, 2);
    break;

  case MD_BLOCK_TABLE: {
    MD_BLOCK_TABLE_DETAIL *tbl = (MD_BLOCK_TABLE_DETAIL *)detail;
    ensure_newlines(ctx, 2);
    if (ctx->table_model) {
      viewmd_table_free(ctx->table_model);
    }
    ctx->table_model = viewmd_table_new(tbl ? tbl->col_count : 0);
    ctx->table_current_row = NULL;
    ctx->in_table_head = FALSE;
    break;
  }

  case MD_BLOCK_THEAD:
    ctx->in_table_head = TRUE;
    break;

  case MD_BLOCK_TBODY:
    ctx->in_table_head = FALSE;
    break;

  case MD_BLOCK_TR:
    table_start_row(ctx);
    break;

  case MD_BLOCK_TH:
  case MD_BLOCK_TD:
    table_start_cell(ctx, detail);
    break;
  }

  return 0;
}

static int on_leave_block(MD_BLOCKTYPE type, void *detail, void *userdata) {
  RenderCtx *ctx = (RenderCtx *)userdata;
  (void)detail;

  /* Popped a skipped (collapsed) block: nothing was emitted. */
  if (ctx->block_stack->len > 0) {
    BlockState top =
        g_array_index(ctx->block_stack, BlockState, ctx->block_stack->len - 1);
    if (top.skipped) {
      g_array_set_size(ctx->block_stack, ctx->block_stack->len - 1);
      return 0;
    }
  }

  if (type == MD_BLOCK_H) {
    gchar *heading_slug = create_heading_anchor(ctx);
    ctx->in_heading = FALSE;

    if (ctx->current_fold_anchor) {
      const gchar *anchor_is_details = g_object_get_data(
          G_OBJECT(ctx->current_fold_anchor), VIEWMD_FOLD_IS_DETAILS_DATA);
      gboolean is_details = anchor_is_details != NULL;
      gchar *key;
      gboolean collapsed;

      if (is_details) {
        key = g_strdup(ctx->pending_details_id ? ctx->pending_details_id : "");
        collapsed = fold_is_collapsed(ctx, key, !ctx->pending_details_open);
        ctx->in_details_summary = FALSE;
      } else {
        key = heading_slug ? g_strdup(heading_slug) : NULL;
        if (!key || key[0] == '\0') {
          g_free(key);
          key = g_strdup_printf("h-%u", ctx->details_seq++);
        }
        collapsed = fold_is_collapsed(ctx, key, FALSE);
      }

      g_object_set_data_full(G_OBJECT(ctx->current_fold_anchor),
                             VIEWMD_FOLD_KEY_DATA, g_strdup(key), g_free);
      g_object_set_data(G_OBJECT(ctx->current_fold_anchor),
                        VIEWMD_FOLD_STATE_DATA,
                        GINT_TO_POINTER(collapsed ? 1 : 0));

      if (collapsed) {
        if (is_details) {
          ctx->details_skip = TRUE;
          ctx->details_skip_depth = 1;
        } else {
          ctx->skip_until_level = ctx->heading_level;
        }
      }
      g_free(key);
    }
    g_clear_pointer(&ctx->pending_details_id, g_free);
    g_free(heading_slug);
    ctx->current_fold_anchor = NULL;
    ensure_newlines(ctx, 1);
  } else if (type == MD_BLOCK_QUOTE) {
    if (ctx->quote_depth > 0) {
      ctx->quote_depth--;
    }
  } else if (type == MD_BLOCK_UL || type == MD_BLOCK_OL) {
    if (ctx->list_stack->len > 0) {
      g_array_set_size(ctx->list_stack, ctx->list_stack->len - 1);
    }
  } else if (type == MD_BLOCK_LI) {
    ctx->list_item_prefix_pending = FALSE;
    ensure_newlines(ctx, 1);
  } else if (type == MD_BLOCK_CODE) {
    emit_code_block_anchor(ctx);
    ctx->in_code_block = FALSE;
    if (ctx->code_source) {
      g_string_free(ctx->code_source, TRUE);
      ctx->code_source = NULL;
    }
    g_clear_pointer(&ctx->code_language, g_free);
    ensure_newlines(ctx, 1);
  } else if (type == MD_BLOCK_TH || type == MD_BLOCK_TD) {
    table_finish_cell(ctx);
  } else if (type == MD_BLOCK_TR) {
    table_finish_row(ctx);
  } else if (type == MD_BLOCK_TABLE) {
    table_finish_row(ctx);
    table_emit_anchor(ctx);
    ensure_newlines(ctx, 2);
  }

  if (ctx->block_stack->len > 0) {
    BlockState state =
        g_array_index(ctx->block_stack, BlockState, ctx->block_stack->len - 1);
    pop_active_tags(ctx, state.pushed_tags);
    g_array_set_size(ctx->block_stack, ctx->block_stack->len - 1);
  }

  return 0;
}

static int on_enter_span(MD_SPANTYPE type, void *detail, void *userdata) {
  RenderCtx *ctx = (RenderCtx *)userdata;
  SpanState state = {type, 0};
  gboolean capture_cell = (ctx && ctx->table_cell_text);

  g_array_append_val(ctx->span_stack, state);

  if (capture_cell) {
    table_capture_span_enter(ctx, type);
    return 0;
  }

  switch (type) {
  case MD_SPAN_EM:
    push_active_tag_by_name(ctx, TAG_ITALIC,
                            &g_array_index(ctx->span_stack, SpanState,
                                           ctx->span_stack->len - 1)
                                 .pushed_tags);
    break;

  case MD_SPAN_STRONG:
    push_active_tag_by_name(ctx, TAG_BOLD,
                            &g_array_index(ctx->span_stack, SpanState,
                                           ctx->span_stack->len - 1)
                                 .pushed_tags);
    break;

  case MD_SPAN_CODE:
    push_active_tag_by_name(ctx, TAG_CODE,
                            &g_array_index(ctx->span_stack, SpanState,
                                           ctx->span_stack->len - 1)
                                 .pushed_tags);
    break;

  case MD_SPAN_DEL:
    push_active_tag_by_name(ctx, TAG_STRIKE,
                            &g_array_index(ctx->span_stack, SpanState,
                                           ctx->span_stack->len - 1)
                                 .pushed_tags);
    break;

  case MD_SPAN_A: {
    MD_SPAN_A_DETAIL *a = (MD_SPAN_A_DETAIL *)detail;
    gchar *href = attr_to_string(a ? &a->href : NULL);
    GtkTextTag *url_tag = gtk_text_buffer_create_tag(ctx->buffer, NULL, NULL);
    g_ptr_array_add(ctx->render_state->link_tags, g_object_ref(url_tag));
    g_object_set_data_full(G_OBJECT(url_tag), VIEWMD_LINK_URL_DATA, href, g_free);
    push_active_tag_by_name(ctx, TAG_LINK,
                            &g_array_index(ctx->span_stack, SpanState,
                                           ctx->span_stack->len - 1)
                                 .pushed_tags);
    push_active_tag(ctx, url_tag,
                    &g_array_index(ctx->span_stack, SpanState,
                                   ctx->span_stack->len - 1)
                         .pushed_tags);
    break;
  }

  case MD_SPAN_IMG: {
    MD_SPAN_IMG_DETAIL *img = (MD_SPAN_IMG_DETAIL *)detail;
    g_free(ctx->image_src);
    ctx->image_src = attr_to_string(img ? &img->src : NULL);
    if (!ctx->image_alt) {
      ctx->image_alt = g_string_new(NULL);
    } else {
      g_string_set_size(ctx->image_alt, 0);
    }
    ctx->in_image = TRUE;
    break;
  }

  default:
    break;
  }

  return 0;
}

static int on_leave_span(MD_SPANTYPE type, void *detail, void *userdata) {
  RenderCtx *ctx = (RenderCtx *)userdata;
  (void)detail;

  if (ctx->span_stack->len > 0) {
    SpanState state =
        g_array_index(ctx->span_stack, SpanState, ctx->span_stack->len - 1);
    if (state.type == MD_SPAN_IMG && ctx->in_image) {
      if (ctx->skip_until_level == 0 && !ctx->details_skip) {
        image_emit_anchor(ctx);
      }
      ctx->in_image = FALSE;
      g_free(ctx->image_src);
      ctx->image_src = NULL;
      if (ctx->image_alt) {
        g_string_set_size(ctx->image_alt, 0);
      }
    }
    if (ctx->table_cell_text) {
      table_capture_span_leave(ctx, state.type);
    }
    pop_active_tags(ctx, state.pushed_tags);
    g_array_set_size(ctx->span_stack, ctx->span_stack->len - 1);
  }

  (void)type;

  return 0;
}

static int on_text(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size,
                   void *userdata) {
  RenderCtx *ctx = (RenderCtx *)userdata;
  gchar *rendered = NULL;

  if (type == MD_TEXT_BR) {
    rendered = g_strdup("\n");
  } else if (type == MD_TEXT_SOFTBR) {
    /* A single newline inside a paragraph is a soft break and folds into a
       space; only explicit hard breaks (backslash or two trailing spaces)
       become visible line breaks. Source line-wrapping must not show up. */
    rendered = g_strdup(" ");
  } else {
    rendered = md_text_to_utf8(type, text, size);
  }

  if (ctx->list_item_prefix_pending && rendered[0] != '\0') {
    ctx->list_item_prefix_pending = FALSE;
  }

  /* Details markers are only meaningful while no heading section is collapsed:
     inside a collapsed section they would otherwise leave a stale
     pending_details_id that the next visible heading would mistake for a
     summary, swallowing the rest of the document. */
  if (type == MD_TEXT_HTML && ctx->skip_until_level == 0) {
    gint seq = -1;
    gint open = -1;
    if (sscanf(rendered, "<!--vmd-details %d %d-->", &seq, &open) == 2) {
      if (ctx->details_skip_depth > 0) {
        ctx->details_skip_depth++;
      } else {
        g_free(ctx->pending_details_id);
        ctx->pending_details_id = g_strdup_printf("details-%d", seq);
        ctx->pending_details_open = (open != 0);
      }
      g_free(rendered);
      return 0;
    }
    if (sscanf(rendered, "<!--vmd-details-end %d-->", &seq) == 1) {
      if (ctx->details_skip_depth > 0) {
        ctx->details_skip_depth--;
        if (ctx->details_skip_depth == 0) {
          ctx->details_skip = FALSE;
        }
      }
      g_free(rendered);
      return 0;
    }
  }

  if (ctx->skip_until_level > 0 || ctx->details_skip) {
    g_free(rendered);
    return 0;
  }

  if (ctx->in_code_block) {
    g_string_append(ctx->code_source, rendered);
  } else if (ctx->in_image) {
    if (!ctx->image_alt) {
      ctx->image_alt = g_string_new(NULL);
    }
    g_string_append(ctx->image_alt, rendered);
  } else if (ctx->table_cell_text) {
    table_capture_append(ctx, rendered);
  } else {
    insert_cstr(ctx, rendered);
    capture_heading_text(ctx, rendered);
  }
  g_free(rendered);
  return 0;
}

void markdown_init_tags(GtkTextBuffer *buffer) {
  gtk_text_buffer_create_tag(buffer, TAG_INVISIBLE, "invisible", TRUE, NULL);

  gtk_text_buffer_create_tag(buffer, TAG_H1, "weight", PANGO_WEIGHT_BOLD, "scale",
                             2.0, "foreground", config->h1_color,
                             "pixels-below-lines", 12, NULL);
  gtk_text_buffer_create_tag(buffer, TAG_H2, "weight", PANGO_WEIGHT_BOLD, "scale",
                             1.6, "foreground", config->h2_color,
                             "pixels-below-lines", 10, NULL);
  gtk_text_buffer_create_tag(buffer, TAG_H3, "weight", PANGO_WEIGHT_BOLD, "scale",
                             1.3, "foreground", config->h3_color,
                             "pixels-below-lines", 8, NULL);

  gtk_text_buffer_create_tag(buffer, TAG_BOLD, "weight", PANGO_WEIGHT_BOLD, NULL);
  gtk_text_buffer_create_tag(buffer, TAG_ITALIC, "style", PANGO_STYLE_ITALIC,
                             NULL);
  gtk_text_buffer_create_tag(buffer, TAG_STRIKE, "strikethrough", TRUE, NULL);

  gtk_text_buffer_create_tag(buffer, TAG_CODE, "family",
                             config->code_font_family, "background",
                             config->inline_code_background, "foreground",
                             config->inline_code_foreground, NULL);

  gtk_text_buffer_create_tag(buffer, MARKYD_TAG_CODE_KW_A, "family",
                             config->code_font_family, "foreground",
                             config->h1_color, "weight", PANGO_WEIGHT_BOLD,
                             NULL);
  gtk_text_buffer_create_tag(buffer, MARKYD_TAG_CODE_KW_B, "family",
                             config->code_font_family, "foreground",
                             config->h2_color, "weight", PANGO_WEIGHT_BOLD,
                             NULL);
  gtk_text_buffer_create_tag(buffer, MARKYD_TAG_CODE_KW_C, "family",
                             config->code_font_family, "foreground",
                             config->h3_color, "weight", PANGO_WEIGHT_BOLD,
                             NULL);
  gtk_text_buffer_create_tag(buffer, MARKYD_TAG_CODE_LITERAL, "family",
                             config->code_font_family, "foreground",
                             config->inline_code_foreground, NULL);

  gtk_text_buffer_create_tag(buffer, TAG_QUOTE, "left-margin", 24, "style",
                             PANGO_STYLE_ITALIC, "foreground",
                             config->text_color, "paragraph-background",
                             config->code_background, NULL);

  gtk_text_buffer_create_tag(buffer, TAG_LIST, "left-margin", 28, NULL);
  gtk_text_buffer_create_tag(buffer, TAG_LIST_BULLET, "foreground",
                             config->list_bullet_color, NULL);

  gtk_text_buffer_create_tag(buffer, TAG_LINK, "foreground", "#2f81f7",
                             "underline", PANGO_UNDERLINE_SINGLE, NULL);

  gtk_text_buffer_create_tag(buffer, TAG_HRULE, "foreground", "#5C6370",
                             "justification", GTK_JUSTIFY_CENTER,
                             "pixels-above-lines", 6, "pixels-below-lines", 6,
                             NULL);

  gtk_text_buffer_create_tag(buffer, TAG_TABLE, "family",
                             config->code_font_family, "left-margin", 20,
                             "right-margin", 12, "paragraph-background",
                             config->code_background, NULL);
  gtk_text_buffer_create_tag(buffer, TAG_TABLE_HEADER, "family",
                             config->code_font_family, "weight",
                             PANGO_WEIGHT_BOLD, "foreground", config->h1_color,
                             NULL);
  gtk_text_buffer_create_tag(buffer, TAG_TABLE_SEPARATOR, "family",
                             config->code_font_family, "foreground", "#5C6370",
                             NULL);
}

void markdown_update_accent_tags(GtkTextBuffer *buffer) {
  GtkTextTagTable *table;
  GtkTextTag *tag;

  if (!buffer || !config) {
    return;
  }

  table = gtk_text_buffer_get_tag_table(buffer);
  if (!table) {
    return;
  }

  tag = gtk_text_tag_table_lookup(table, TAG_H1);
  if (tag) {
    g_object_set(tag, "foreground", config->h1_color, NULL);
  }
  tag = gtk_text_tag_table_lookup(table, TAG_H2);
  if (tag) {
    g_object_set(tag, "foreground", config->h2_color, NULL);
  }
  tag = gtk_text_tag_table_lookup(table, TAG_H3);
  if (tag) {
    g_object_set(tag, "foreground", config->h3_color, NULL);
  }
  tag = gtk_text_tag_table_lookup(table, TAG_LIST_BULLET);
  if (tag) {
    g_object_set(tag, "foreground", config->list_bullet_color, NULL);
  }
  tag = gtk_text_tag_table_lookup(table, MARKYD_TAG_CODE_KW_A);
  if (tag) {
    g_object_set(tag, "family", config->code_font_family, "foreground",
                 config->h1_color, NULL);
  }
  tag = gtk_text_tag_table_lookup(table, MARKYD_TAG_CODE_KW_B);
  if (tag) {
    g_object_set(tag, "family", config->code_font_family, "foreground",
                 config->h2_color, NULL);
  }
  tag = gtk_text_tag_table_lookup(table, MARKYD_TAG_CODE_KW_C);
  if (tag) {
    g_object_set(tag, "family", config->code_font_family, "foreground",
                 config->h3_color, NULL);
  }
  tag = gtk_text_tag_table_lookup(table, MARKYD_TAG_CODE_LITERAL);
  if (tag) {
    g_object_set(tag, "family", config->code_font_family, "foreground",
                 config->inline_code_foreground, NULL);
  }
  tag = gtk_text_tag_table_lookup(table, TAG_TABLE_HEADER);
  if (tag) {
    g_object_set(tag, "family", config->code_font_family, "foreground",
                 config->h1_color, NULL);
  }
  tag = gtk_text_tag_table_lookup(table, TAG_CODE);
  if (tag) {
    g_object_set(tag, "family", config->code_font_family, "foreground",
                 config->inline_code_foreground, "background",
                 config->inline_code_background, NULL);
  }
  tag = gtk_text_tag_table_lookup(table, TAG_QUOTE);
  if (tag) {
    g_object_set(tag, "foreground", config->text_color, "paragraph-background",
                 config->code_background, NULL);
  }
  tag = gtk_text_tag_table_lookup(table, TAG_TABLE);
  if (tag) {
    g_object_set(tag, "family", config->code_font_family,
                 "paragraph-background", config->code_background, NULL);
  }
}

static gfloat align_to_xalign(MD_ALIGN align) {
  switch (align) {
  case MD_ALIGN_RIGHT:
    return 1.0f;
  case MD_ALIGN_CENTER:
    return 0.5f;
  case MD_ALIGN_LEFT:
  case MD_ALIGN_DEFAULT:
  default:
    return 0.0f;
  }
}

GtkWidget *markdown_create_table_widget(GtkTextChildAnchor *anchor) {
  ViewmdTable *table;
  GtkWidget *wrapper;
  GtkWidget *grid;

  if (!anchor) {
    return NULL;
  }

  table = (ViewmdTable *)g_object_get_data(G_OBJECT(anchor), TABLE_MODEL_DATA_KEY);
  if (!table || table->col_count == 0 || !table->rows || table->rows->len == 0) {
    return NULL;
  }

  wrapper = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_style_context_add_class(gtk_widget_get_style_context(wrapper),
                              "viewmd-table");
  gtk_widget_set_halign(wrapper, GTK_ALIGN_START);
  gtk_widget_set_margin_top(wrapper, 6);
  gtk_widget_set_margin_bottom(wrapper, 6);
  gtk_widget_set_margin_start(wrapper, 8);
  gtk_widget_set_margin_end(wrapper, 8);

  grid = gtk_grid_new();
  gtk_style_context_add_class(gtk_widget_get_style_context(grid),
                              "viewmd-table-grid");
  gtk_grid_set_row_spacing(GTK_GRID(grid), 0);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 0);
  gtk_box_pack_start(GTK_BOX(wrapper), grid, TRUE, TRUE, 0);

  for (guint r = 0; r < table->rows->len; r++) {
    ViewmdTableRow *row = g_ptr_array_index(table->rows, r);
    if (!row) {
      continue;
    }

    for (guint c = 0; c < table->col_count; c++) {
      GtkWidget *cell = gtk_event_box_new();
      GtkWidget *label = gtk_label_new(NULL);
      const gchar *text = "";
      gchar *markup = NULL;
      MD_ALIGN align = (c < table->aligns->len)
                           ? g_array_index(table->aligns, MD_ALIGN, c)
                           : MD_ALIGN_DEFAULT;

      if (c < row->cells->len) {
        text = g_ptr_array_index(row->cells, c);
        if (!text) {
          text = "";
        }
      }

      gtk_style_context_add_class(gtk_widget_get_style_context(cell),
                                  "viewmd-table-cell");
      if (row->is_header) {
        gtk_style_context_add_class(gtk_widget_get_style_context(cell),
                                    "viewmd-table-header-cell");
      }
      g_object_set_data(G_OBJECT(cell), VIEWMD_TABLE_CELL_ROW_DATA,
                        GINT_TO_POINTER((gint)r));
      g_object_set_data(G_OBJECT(cell), VIEWMD_TABLE_CELL_COL_DATA,
                        GINT_TO_POINTER((gint)c));
      gtk_style_context_add_class(gtk_widget_get_style_context(label),
                                  "viewmd-table-label");
      gtk_widget_set_hexpand(cell, FALSE);
      gtk_widget_set_vexpand(cell, FALSE);
      gtk_container_add(GTK_CONTAINER(cell), label);

      gtk_label_set_xalign(GTK_LABEL(label), align_to_xalign(align));
      gtk_label_set_yalign(GTK_LABEL(label), 0.5f);
      gtk_label_set_line_wrap(GTK_LABEL(label), FALSE);
      gtk_label_set_selectable(GTK_LABEL(label), FALSE);
      gtk_widget_set_margin_start(label, 8);
      gtk_widget_set_margin_end(label, 8);
      gtk_widget_set_margin_top(label, row->is_header ? 6 : 5);
      gtk_widget_set_margin_bottom(label, row->is_header ? 6 : 5);

      if (row->is_header) {
        markup = g_strdup_printf("<b>%s</b>", text);
        gtk_label_set_markup(GTK_LABEL(label), markup);
        g_free(markup);
      } else {
        gtk_label_set_markup(GTK_LABEL(label), text);
      }

      gtk_grid_attach(GTK_GRID(grid), cell, (gint)c, (gint)r, 1, 1);
    }
  }

  return wrapper;
}

static void insert_frontmatter(RenderCtx *ctx, GPtrArray *pairs) {
  GtkTextChildAnchor *anchor;
  gint start_offset;
  gint end_offset;
  GString *search;
  gboolean first = TRUE;

  anchor = gtk_text_buffer_create_child_anchor(ctx->buffer, &ctx->iter);
  g_object_set_data(G_OBJECT(anchor), VIEWMD_FRONTMATTER_DATA,
                    GINT_TO_POINTER(1));
  g_object_set_data_full(G_OBJECT(anchor), "viewmd-frontmatter-pairs", pairs,
                         frontmatter_pairs_free);

  /* Keep the fields findable via Ctrl+F without showing them twice. */
  start_offset = gtk_text_iter_get_offset(&ctx->iter);
  search = g_string_new(NULL);
  for (guint i = 0; i < pairs->len; i++) {
    const gchar *entry = g_ptr_array_index(pairs, i);
    const gchar *tab = strchr(entry, '\t');
    if (!tab) {
      continue;
    }
    if (!first) {
      g_string_append_c(search, ' ');
    }
    g_string_append_len(search, entry, (gsize)(tab - entry));
    g_string_append_c(search, ' ');
    g_string_append(search, tab + 1);
    first = FALSE;
  }
  gtk_text_buffer_insert(ctx->buffer, &ctx->iter, search->str, search->len);
  end_offset = gtk_text_iter_get_offset(&ctx->iter);
  g_string_free(search, TRUE);
  if (end_offset > start_offset) {
    apply_tag_by_name_offsets(ctx->buffer, TAG_INVISIBLE, start_offset,
                              end_offset);
  }
  insert_cstr(ctx, "\n");
}

static GtkWidget *frontmatter_cell(const gchar *text, gboolean header) {
  GtkWidget *cell = gtk_event_box_new();
  GtkWidget *label = gtk_label_new(NULL);

  gtk_style_context_add_class(gtk_widget_get_style_context(cell),
                              "viewmd-table-cell");
  if (header) {
    gtk_style_context_add_class(gtk_widget_get_style_context(cell),
                                "viewmd-table-header-cell");
  }
  gtk_widget_set_hexpand(cell, FALSE);
  gtk_widget_set_vexpand(cell, FALSE);
  gtk_container_add(GTK_CONTAINER(cell), label);

  gtk_style_context_add_class(gtk_widget_get_style_context(label),
                              "viewmd-table-label");
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_label_set_yalign(GTK_LABEL(label), 0.5f);
  gtk_widget_set_margin_start(label, 8);
  gtk_widget_set_margin_end(label, 8);
  gtk_widget_set_margin_top(label, header ? 6 : 5);
  gtk_widget_set_margin_bottom(label, header ? 6 : 5);
  gtk_label_set_text(GTK_LABEL(label), text);

  return cell;
}

GtkWidget *markdown_create_frontmatter_widget(GtkTextChildAnchor *anchor) {
  GPtrArray *pairs;
  GtkWidget *wrapper;
  GtkWidget *grid;

  if (!anchor) {
    return NULL;
  }
  pairs = g_object_get_data(G_OBJECT(anchor), "viewmd-frontmatter-pairs");
  if (!pairs || pairs->len == 0) {
    return NULL;
  }

  wrapper = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_style_context_add_class(gtk_widget_get_style_context(wrapper),
                              "viewmd-table");
  gtk_widget_set_halign(wrapper, GTK_ALIGN_START);
  gtk_widget_set_margin_top(wrapper, 6);
  gtk_widget_set_margin_bottom(wrapper, 6);
  gtk_widget_set_margin_start(wrapper, 8);
  gtk_widget_set_margin_end(wrapper, 8);

  grid = gtk_grid_new();
  gtk_style_context_add_class(gtk_widget_get_style_context(grid),
                              "viewmd-table-grid");
  gtk_grid_set_row_spacing(GTK_GRID(grid), 0);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 0);
  gtk_box_pack_start(GTK_BOX(wrapper), grid, TRUE, TRUE, 0);

  gtk_grid_attach(GTK_GRID(grid), frontmatter_cell("Property", TRUE), 0, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), frontmatter_cell("Value", TRUE), 1, 0, 1, 1);

  for (guint i = 0; i < pairs->len; i++) {
    const gchar *entry = g_ptr_array_index(pairs, i);
    const gchar *tab = strchr(entry, '\t');
    gchar *key;
    gchar *value;
    guint row = i + 1;

    if (!tab) {
      continue;
    }
    key = g_strndup(entry, (gsize)(tab - entry));
    value = g_strdup(tab + 1);
    gtk_grid_attach(GTK_GRID(grid), frontmatter_cell(key, FALSE), 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), frontmatter_cell(value, FALSE), 1, row, 1,
                    1);
    g_free(key);
    g_free(value);
  }

  return wrapper;
}

GtkWidget *markdown_create_fold_widget(GtkTextChildAnchor *anchor) {
  GtkWidget *button;
  GtkWidget *icon;
  gboolean collapsed;

  if (!anchor) {
    return NULL;
  }
  collapsed =
      g_object_get_data(G_OBJECT(anchor), VIEWMD_FOLD_STATE_DATA) != NULL;

  /* Use theme disclosure icons (pan-down/pan-end) rather than unicode glyphs,
     which render as missing-glyph boxes on fonts without those code points. */
  button = gtk_button_new();
  gtk_widget_set_focus_on_click(button, FALSE);
  gtk_widget_set_can_focus(button, FALSE);
  gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
  gtk_widget_set_halign(button, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(button),
                              "viewmd-fold-toggle");
  icon = gtk_image_new_from_icon_name(
      collapsed ? "pan-end-symbolic" : "pan-down-symbolic",
      GTK_ICON_SIZE_MENU);
  gtk_container_add(GTK_CONTAINER(button), icon);
  gtk_widget_set_tooltip_text(button,
                              collapsed ? "Expand section" : "Collapse section");
  /* Effective collapsed state at render time; click handlers flip this and
     write the explicit override into the editor's collapse map. */
  g_object_set_data(G_OBJECT(button), "viewmd-fold-collapsed",
                    GINT_TO_POINTER(collapsed ? 1 : 0));
  return button;
}

void markdown_apply_tags(GtkTextBuffer *buffer, const gchar *source,
                         GHashTable *collapsed) {
  RenderCtx ctx;
  MD_PARSER parser = {0};
  gchar *normalized_source;
  const gchar *input;
  gint rc;

  if (!buffer) {
    return;
  }

  memset(&ctx, 0, sizeof(ctx));
  ctx.render_state = reset_render_state(buffer);
  gtk_text_buffer_set_text(buffer, "", -1);
  input = source ? source : "";

  ctx.buffer = buffer;
  ctx.active_tags = g_ptr_array_new();
  ctx.block_stack = g_array_new(FALSE, FALSE, sizeof(BlockState));
  ctx.span_stack = g_array_new(FALSE, FALSE, sizeof(SpanState));
  ctx.list_stack = g_array_new(FALSE, FALSE, sizeof(ListState));
  ctx.anchor_counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  ctx.collapsed = collapsed;
  ctx.heading_start_offset = 0;
  ctx.heading_level = 1;
  ctx.has_output = FALSE;
  ctx.trailing_newlines = 0;

  {
    GPtrArray *frontmatter;
    const gchar *body = NULL;

    frontmatter = parse_frontmatter(input, &body);
    normalized_source = transform_details(body);

    gtk_text_buffer_get_start_iter(buffer, &ctx.iter);
    if (frontmatter) {
      insert_frontmatter(&ctx, frontmatter);
    }
  }

  parser.abi_version = 0;
  parser.flags = MD_DIALECT_GITHUB | MD_FLAG_PERMISSIVEATXHEADERS;
  parser.enter_block = on_enter_block;
  parser.leave_block = on_leave_block;
  parser.enter_span = on_enter_span;
  parser.leave_span = on_leave_span;
  parser.text = on_text;
  parser.debug_log = NULL;
  parser.syntax = NULL;

  rc = md_parse(normalized_source, (MD_SIZE)strlen(normalized_source), &parser,
                &ctx);
  if (rc != 0) {
    reset_render_state(buffer);
    gtk_text_buffer_set_text(buffer, input, -1);
  }

  if (ctx.code_source) {
    g_string_free(ctx.code_source, TRUE);
  }
  g_free(ctx.code_language);
  g_free(ctx.pending_details_id);
  if (ctx.table_cell_text) {
    g_string_free(ctx.table_cell_text, TRUE);
  }
  g_free(ctx.image_src);
  if (ctx.image_alt) {
    g_string_free(ctx.image_alt, TRUE);
  }
  if (ctx.table_model) {
    viewmd_table_free(ctx.table_model);
  }
  if (ctx.heading_text) {
    g_string_free(ctx.heading_text, TRUE);
  }
  g_hash_table_destroy(ctx.anchor_counts);
  g_array_free(ctx.list_stack, TRUE);
  g_array_free(ctx.span_stack, TRUE);
  g_array_free(ctx.block_stack, TRUE);
  g_ptr_array_free(ctx.active_tags, TRUE);
  g_free(normalized_source);
}
