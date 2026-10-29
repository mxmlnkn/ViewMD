# Fork value analysis: what the `forks/` tree adds
Date: 2026-09-08
Category: features
Scope: all six forks under `forks/` (`FFF-ViewMD`, `SeeMD`, `Skript`,
`ViewMD`, `lumenEh`, `nudzg-ViewMD`), compared against this base.

Method: read-only `diff -u` of each fork's `src/` against the base, plus the
fork git logs to infer intent. No build in the sandbox for the forks.

## The short version

All forks derive from the same upstream lineage (share commits like
`effa685`, `a30ce04`, `c40a410`, `55eff8c`, `0cce704`). Most of the diff
volume is rename churn plus a thin layer of AI-generated polish. Only a few
changes are substantive; the highest-value ones are bug fixes and small
features that were cherry-picked/reimplemented here.

## Per-fork value

### FFF-ViewMD (io.github.FAYOO777.FFF-ViewMD)
Largest engineering upgrade that keeps the native `GtkTextView` renderer.
- `tests/test_core.c` - 25-test `g_test` suite + `make test` (adopted here).
- Setext-H2 fix: the base `normalize_markdown_source` inserted a blank line
  before any `---` line, turning `text\n---` (valid Setext H2) into
  paragraph + thematic break. Base bug fixed here.
- HTML entity decoding in link/alt attribute text (`&amp;` etc.) instead of
  raw text. Correctness fix; adopted here.
- Transactional settings + Restore Defaults + clamped values.
- Reading layout (content width, margins, independent code font).
- Search index preserved across buffer rebuilds (begin/end editor update).
- Relative `.md` link resolution via `g_uri_resolve_relative` (with a
  regression risk for bare domains like `google.com`).
- GFM task lists, H4-H6 tags, table-cell hyperlinks, `GtkSeparator` hrules.
- Rebranding, Chinese translations, icon set, `-Werror`/CI: ignore.

### SeeMD (WebKit rewrite)
Moves preview to WebKit2GTK with an editable source preview, Ctrl+E edit
mode, save/save-as, GitHub CSS, and links routed in-app. Real but heavy:
new `libwebkit2gtk-4.1` dependency contradicts the project's no-engine +
low-memory goals.
Cherry-picked here from it:
- Comment tokenization for C/Java/Python scanners (see
  `code_highlight.c`) + a Bash scanner.
- Visible-rect image sizing (`gtk_text_view_get_visible_rect`) - worth a
  follow-up port.
- Commits build artifacts (`obj/*.o`, `seemd` binary) - anti-pattern.
- Everything else (preview, edit mode, save) is out of scope for a
  low-memory viewer.

### Skript / MarkPad
- Ctrl+mouse-wheel font zoom (clean, small).
- Native `GtkFileChooserNative` open/save dialogs (base uses a blocking
  `gtk_dialog_run` chooser - worth porting).
- Text selection in preview + table cells (`label_selectable`).
- Edit-mode toggle with scroll-position preservation; recent-docs via
  `gtk_recent_manager`.
- Dead `libportal` scaffolding left in app.c/Makefile - ignore.

### ViewMD (inner, watch-mode fork)
- `--watch`/`-w` auto-reload via `g_file_monitor` with 100ms debounce -
  useful for SciTE-like workflows; has a real DELETED-event re-arm bug for
  vim/emacs `rename` saves that must be fixed before adopting.
- `--socket` unix socket push (`VIEWMD_SOCKET=` + raw content, optional
  `CURSOR:` line) - couples to an unfinished fraction-based scroll-to-line.
- `viewmd -` stdin support - clean, orthogonal.
- Scroll-to-line itself is half-baked (fraction-based ratio is wrong for
  headings/tables) - do not port.

### lumenEh
- Tabs (`GtkNotebook`) and a per-tab file path - the model adopted for the
  tabs feature in this repo (rebuilt cleanly here).
- `g_utf8_make_valid` on set_content - genuine crash fix for non-UTF-8
  files; adopted here.
- Remote SSH loading/browser/bookmarks - niche, half-baked (cached-image
  key misses host, shell-quoting holes, `g_spawn_sync` on the main thread,
  a double-unref in the browser dialog). Ignore unless wanted.
- Solarized/dracula/nord themes + per-theme accent updates - the shape of
  the color-settings work adopted here.
- Packaging self-promotion, keyrings, APT repo scripts: ignore.

### nudzg-ViewMD
- Vim hjkl scrolling - `g_signal_emit_by_name(self->scroll, "scroll-child", ...)`
  is wrong (`scroll-child` is a `GtkScrollbar`/`GtkRange` action signal;
  `GtkScrolledWindow` has no such signal). Non-functional as written; take
  only the idea.

## Already applied to this branch

- Tabs (notebook + per-tab path) - lumenEh model, rebuilt clean.
- `g_utf8_make_valid` - lumenEh crash fix.
- Headless `g_test` suite + `make test` - FFF shape.
- Setext-H2 normalization fix - FFF.
- HTML entity decoding in attribute text - FFF/SeeMD (in reimplemented
  form, small name set).

## Deferred (documented in `reviews/ideas/future-directions.md`)

- `--watch`/stdin (inner ViewMD, needs the DELETED re-arm fix).
- Comment tokenization + Bash scanner (SeeMD) - worth adding once the
  syntax-highlight work is reviewed.
- Native file dialogs from Skript.
- Ctrl+wheel zoom (Skript).
- Visible-rect image sizing (SeeMD).

## Rebranding to ignore (all forks)

Renames of app id / window title / config dir / env names / data keys /
CSS classes / desktop file / icon set, plus README and packaging renames.
`VIEWMD_*` data-key renames in particular touch every file for zero
behavioral change - reviewing those hunks is wasted effort.
