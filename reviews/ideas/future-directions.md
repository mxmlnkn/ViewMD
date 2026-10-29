# Future directions
Date: 2026-09-08
Category: ideas
Scope: exploratory proposals for the ViewMD fork. Lower confidence than
`features/` entries - each is a sketch, not a plan.

## Memory benchmark methodology

If you intend to market the fork on memory, watch out for the shared-library
caveat. The RSS delta per extra process is *not* the RSS of a fresh process:
libgtk-3, libpango, libgdk, libcairo, libfontconfig, the md4c object, plus
the .text of `viewmd` itself are all shared with every other running `viewmd`
through the kernel's page cache. The per-tab overhead inside one process is
much smaller than a second process's marginal RSS. Benchmarks that quote
"~50 MB peak RSS per process" are technically true but don't tell you what
opening five documents in five processes vs. five tabs in one process costs.

A useful pair of numbers:
- One process, one tab, then five tabs. The latter minus the former is the
  per-tab editor + widget cost, which is the real scaling story.
- One process vs. five processes, same five documents loaded. The difference
  at the `ps`/`pmap` level is dominated by shareable pages; subtract the
  total cost of `mmap`-shared libraries to get the truthful working-set
  delta. `/proc/$pid/smaps_rollup` gives per-process PSS, which approximates
  the fair share and is the right number to quote.

## On-demand per-tab rendering

The renderer rebuilds the entire `GtkTextBuffer` on every content change.
With many tabs open (the SciTE usage pattern), this multiplies. One idea:
keep the `source_content` cached per tab, and only re-render when the tab
becomes active (or when the user explicitly collapses/expands a section
in a background tab). The active tab gets the live `GtkTextView` + child
widgets; background tabs get a frozen `source_content` + a cached
heading/section list, and re-render on switch. Memory cost: hold the source
text + a small "last-rendered summary" per tab.

To avoid surprising navigation (jumping into a background tab that looks
stale), keep the buffer for background tabs but avoid widget construction
(images, tables, code-block trees) until activation.

## SciTE / Scintilla lexer integration

SeeMD added a Bash scanner and C/Java/Python comment tokenization on top of
the existing per-line tokenizers. The user's SciTE color-style preferences
likely need many more languages (Rust, Go, JSON, Markdown, YAML, ini,
Python triple-quoted strings, SQL...). SciTE's lexers live in
`scintilla/lexers/` and are not trivially embeddable; the cleanest path is
to translate the user's SciTE color files into GTK `markyd_code_*` tag
palettes + small scanner extensions per language. The Lua/SciTE lexer API
itself is heavy and not worth pulling in for a viewer.

A leaner idea: convert `.properties` color files into a palette table that
the existing `ensure_code_tags` consults, and add one tiny scanner per
language you actually open.

## C++ / Rust port - analysis update (2026-09-08, measured)

Porting gives type safety / RAII and, in Rust, eliminates the memory-safety
bug classes that actually bit this session (freeing a string literal,
use-after-free on a freed editor, double-free of a GObject-owned string).
That is a genuine benefit for long-run maintenance - arguably the strongest
honest argument. What a port does *not* give: any change in memory footprint
or rendering performance, because both are dominated by GTK/Pango and the
GtkTextBuffer/widget tree, which are identical regardless of the glue
language.

Measured on this build (VmRSS / PSS from /proc, after a doc is rendered):

- one process, one tab: ~33 MB / ~20 MB
- one process, three tabs (a, b, longline): ~33 MB / ~20 MB (tabs are ~free)
- three processes, one tab each (parallel): ~33 MB / ~20, ~15, ~13 MB
  (the 2nd/3rd process each adds only ~12-13 MB PSS; the 33 MB baseline is
  the single copy of libgtk-3/pango/cairo mmap'd once in the page cache)

So for the SciTE-like "many documents" goal, the language is irrelevant: the
lever is *tabs within one process*, which are already supported and cost
next to nothing, versus launching more processes. A port leaves the ~20-35 MB
baseline and the per-document cost untouched (gtkmm/gtk-rs wrap the same C
libraries; per-object overhead is negligible, a Rust binary's extra .text is
shared per-process).

Interop with md4c: trivial from C++ (`#include "md4c.h"`, C callbacks are
plain function pointers). From Rust it means either a small unsafe FFI shim
or - the better path - dropping md4c for pulldown-cmark (pure Rust,
CommonMark + GFM tables, and a streaming Event API that is conceptually the
same shape as md4c's callbacks). If a Rust port happens, use
gtk4-rs + pulldown-cmark rather than gtk3-rs + an md4c shim.

Recommended ordering (independent of the author's C++ fluency):
1. Stay C + the headless test suite + an ASAN build (ASAN already caught a
   use-after-free during development). Cheapest, ships features now.
2. If C++: do it incrementally, not as a port - keep the C rendering core
   (markdown.c, code_highlight.c, md4c) and move the ownership-heavy
   periphery (config, editor/window glue, uri/utf8) to C++ with
   std::string/unique_ptr against the GTK C API. Lower risk, keeps md4c.
3. Rust only as a self-contained learning project the author writes
   themselves (gtk4-rs + pulldown-cmark), deliberately not blocking the C
   tool. Do not do it *instead of* shipping features.

Neither language changes the bottleneck: feature surface, not language.
## GTK4 baseline - measured (2026-09-08, host re-measured 2026-09-10)

Two small probes (same widget mix as ViewMD: window + header bar + two-tab
notebook + scrolled text view with a rendered ~1 KB document) were built
against GTK3 and GTK4 and their RSS measured via /proc (VmRSS/PSS).

The first run was in the GPU-less sandbox, where GTK4's GL renderer fell back
to llvmpipe: GTK3 ~25.3/13.5 MB, GTK4 ~99-147/86-134 MB. There the
libLLVM/libgallium working set accounted for the difference; neither
`GSK_RENDERER=cairo` nor `GDK_DEBUG=gl-disable` avoided loading it.

Re-running the same probes on the host (AMD Radeon 780M, Mesa radeonsi,
Ubuntu 24.04) gave:

- GTK3: ~35.5 MB RSS / 8.6 MB PSS
- GTK4: ~212.6 MB RSS / 165.1 MB PSS (~6x RSS, ~19x PSS)

The mechanism is GTK4's retained Gsk/OpenGL renderer: creating the GL context
keeps Mesa's LLVM driver stack (libLLVM, libgallium, shader caches) resident,
and the GL scene graph adds the rest. GTK3's immediate-mode Cairo path never
loads that stack, even with radeonsi driving the GPU.

Corroboration (2026-09): official GTK4 drawing model docs (retained Gsk scene
graph via GL/Vulkan), a GTK developer on GNOME Discourse explaining the
one-off "GTK4 initialises Vulkan or OpenGL, and the driver can initialise
their own shader cache" with valgrind traces on AMD-Mesa, plus third-party
reports in the same order of magnitude. Caveats: much of the delta is a fixed
one-time GL cost (it does not grow per tab), `GSK_RENDERER=cairo` roughly
halves memory but is officially marked debug-only/avoid, and GTK 4.16.2+
trimmed NGL mmap pressure without closing the gap.

Verdict: stay on GTK3. GTK4 additionally requires real migration work (event
controllers instead of widget event signals, no GtkContainer/GdkScreen/
gtk_dialog_run, GtkDropTarget for DnD, headerbar renames);
gtk_text_view_add_child_at_anchor survives, so the child-anchor widget design
(tables, code panels, fold toggles) carries over.

GTK3 support horizon (2026-09 confirmation): gtk-3-24 is maintenance-only,
still shipped (3.24.52, Mar 2026), annualized release cadence, no published
EOL; carried by Debian 12/13, Ubuntu 22.04/24.04/26.04 LTS (libgtk-3-0t64)
and Fedora 44/45. Revisit a toolkit change only if EOL is declared or a
supported distro drops it; watch Wayland rendering as the likely backport
area.

## Watch mode (deferred cherry-pick)

`--watch` from the inner ViewMD fork is the right next ergonomic feature
for the SciTE-style workflow, but it has a real bug to fix before adopting:
the `G_FILE_MONITOR_EVENT_DELETED` handler returns early, so editors that
save via `rename` (vim, emacs, most GUI editors) silently break the watch
because the monitor is now watching a stale inode. Re-arming on
DELETED/MOVED is a one-line fix in the callback.

## Link activation UX

A small paper cut in the base: a link activates on button release, so a
text selection that happens to end on a link (e.g. selecting a sentence
that contains a URL) opens the URL the moment the user releases the mouse.
The fix is to require click+release inside the same anchor mark and to
suppress activation if the cursor moved between press and release, which
is the standard pattern.

## Window icon

`assets/viewmd.desktop` references `accessories-text-editor` (the generic
text-editor icon). An actual branded icon improves task-bar identification
and signals that this is a distinct project. The cost is small: one PNG in
multiple sizes (16/24/32/48/64/96/256), an `Icon=` entry in the desktop file,
and a `gtk_window_set_default_icon_name`/`set_icon` call in `markyd_app_new`.
