# ViewMD

A lightweight GTK 3 markdown viewer for desktop Linux. It is ideal as your default app for opening `.md` files. Uses the excellent library [md4c](https://github.com/mity/md4c) for markdown parsing. This project is sort of a companion app/fork of my other markdown project [TrayMD](https://github.com/rabfulton/TrayMD). 

![ViewMD](assets/screenshot.png)

## Features

- **Native GTK viewer** - No webviews
- **Read-only rendering** - Focused on viewing markdown files
- **Minimal UI** - Clean toolbar with open and settings buttons
- **Lightweight** - No web technologies, small codebase (~8 kLOC excluding md4c), fast startup
- **Hyperlink support** - Left click opens links and internal anchors
- **Document search** - `Ctrl+F` with next/previous match navigation
- **Raw source access** - Right-click to view or copy the raw markdown
- **Local image support** - Local images are resized to fit the the document window

## Supported Markdown

| Syntax | Description |
|--------|-------------|
| `# Header 1` | Large bold header |
| `## Header 2` | Medium bold header |
| `### Header 3` | Small bold header |
| `**bold**` | Bold text |
| `*italic*` | Italic text |
| `` `code` `` | Inline code |
| <code>```...```</code> | Code block |
| `- item` | List item |
| `> quote` | Block quote |
| `[text](url)` | Link |
| `~~strike~~` | Strikethrough |
| `\| table \| row \|` | Markdown tables |
| `---` | Horizontal rule |

Code blocks currently support a beta version of keyword highlighting for fenced languages `c`, `java`, and `python`.

## Installation

### Arch Linux (AUR)

https://aur.archlinux.org/packages/viewmd

### Debian/Ubuntu (.deb)

Install a built `.deb` package with:

```bash
sudo apt install ./viewmd_*.deb
```

### Fedora/RHEL (.rpm)

Install a built `.rpm` package with:

```bash
sudo dnf install ./viewmd-*.rpm
```

## Usage

Run `viewmd` to start the application.

- **Open button**: Open a markdown document
- **Reload button**: Reload the currently open document from disk
- **Settings button**: Adjust theme, fonts, and markdown accent colors

### View or Copy the Raw Source

Right-click anywhere in the document and choose **View Source** to open the
raw markdown in a read-only monospace tab, or **Copy Source** to put the
whole raw markdown on the clipboard.

### Find in Document

- Press `Ctrl+F` to open search.
- Type to highlight matches as you search.
- Press `Enter` for next match and `Shift+Enter` for previous match.
- Press `Esc` to close search.

### Set as Default `.md` Viewer

After installing, associate markdown MIME types with `viewmd.desktop`:

```bash
xdg-mime default viewmd.desktop text/markdown
xdg-mime default viewmd.desktop text/x-markdown
```

Verify the current default:

```bash
xdg-mime query default text/markdown
```

Test by opening a markdown file through your desktop association:

```bash
xdg-open README.md
```


## Building From Source

```bash
make
sudo make install
```

This installs:
- Binary to `/usr/local/bin/viewmd`
- Desktop file to `/usr/local/share/applications/viewmd.desktop`

### Uninstallation

```bash
sudo make uninstall
```
### Dependencies

### Arch Linux
```bash
sudo pacman -S gtk3
```

### Ubuntu/Debian
```bash
sudo apt install libgtk-3-dev
```

### Fedora
```bash
sudo dnf install gtk3-devel
```

## Why not GTK 4?

ViewMD stays on GTK 3 deliberately. This project exists because a markdown
viewer should be small in memory with many documents open - and the merged
rendering model of GTK 4 works directly against that goal.

Measured on a real desktop (AMD Radeon 780M, Mesa radeonsi, Ubuntu 24.04)
with the exact same viewer-shaped window (header bar + two-tab notebook +
scrolled text view with a rendered document) built once against GTK 3 and
once against GTK 4:

| toolkit | VmRSS | PSS |
|---|---|---|
| GTK 3 | ~35.5 MB | ~8.6 MB |
| GTK 4 | ~212.6 MB | ~165.1 MB |

That is roughly **6x the resident set** (and ~19x the proportional PSS) for a
plain text viewer, for the same two-tab probe. The entire difference comes
from GTK 4's OpenGL-first renderer: rendering goes through the retained
scene-graph (`GskRenderer`) over OpenGL, and on AMD/radeonsi creating that GL
context pulls Mesa's LLVM driver stack (`libLLVM` plus `libgallium`, tens of
MB) and its caches resident in the process. GTK 3's immediate-mode
Cairo path never loads any of it, even on a machine with a working GPU
(radeonsi is LLVM-based, so the software stack stays mapped regardless).

GTK 4 does ship a Cairo software renderer (`GSK_RENDERER=cairo`), but in our
measurement that still kept the Mesa/LLVM working set resident, and the
toolkit migration itself is large (event controllers instead of widget
event signals, no `GtkContainer`/`GdkScreen`/`gtk_dialog_run`, a DnD rework),
while our child-anchor-widget design for tables, code panels and fold
toggles would be the only thing that survives mostly unchanged. Given the
measured memory gap, GTK 4 is not worth it for a low-memory viewer.

### Corroboration

The mechanism and the order of magnitude are confirmed by GTK developers and
users:

- The official [GTK 4 drawing model](https://docs.gtk.org/gtk4/drawing-model.html)
  documents the switch from GTK 3's immediate-mode Cairo painting to a
  retained scene graph of `GskRenderNode`s, rendered via OpenGL/Vulkan/Cairo.
- A GTK developer on [GNOME Discourse](https://discourse.gnome.org/t/valgrind-reports-memory-leak-with-gtk4/25598)
  explains the one-off cost: "GTK4 initialises Vulkan or OpenGL, and the
  driver can initialise their own shader cache", with Valgrind traces showing
  `gdk_display_init_vulkan` / `gskrenderer.c` even before any window work - on
  the same AMD-Mesa stack this project measured.
- Third-party measurements are in the same range: an empty GTK 4 window
  ~20 MB vs ~5.5 MB on GTK 3
  ([r/GTK](https://www.reddit.com/r/GTK/comments/vv55p4/gtk4_memory_usage_unexpectedly_high/)),
  simple GTK 4 dialogs > 100 MiB while comparable GTK 3 apps sit around 50 MiB
  ([HN](https://news.ycombinator.com/item?id=46092441)),
  and a "3 fold" increase after a GTK 3 to GTK 4 port
  ([EndeavourOS forum](https://forum.endeavouros.com/t/how-to-significantly-reduce-memory-usage-memory-usage-in-gtk4-and-libadwaita-apps/79360)).

Two caveats keep this fair:

- Most of the delta is a fixed, one-time GL/driver cost, not a per-window or
  per-tab one. For a tool whose selling point is many documents open, a large
  fixed baseline is still the wrong trade, but it does not multiply with the
  number of tabs.
- `GSK_RENDERER=cairo` avoids GL and roughly halves GTK 4 memory, but the
  [GskCairoRenderer](https://docs.gtk.org/gsk4/ctor.CairoRenderer.new.html) is
  documented as a debug/comparison tool whose "usage should be avoided" (no
  3D transforms, incomplete), and GTK env variables are not stable API.
  GTK 4.16.2+ also reduced NGL renderer mmap pressure, but reports still show
  GTK 4 sitting far above GTK 3.

Reproduce the measurement yourself:

```bash
sudo apt install libgtk-4-dev   # once
make -C bench run
```

### GTK 3 support horizon

GTK 3.24 is in maintenance mode: no new features, but still maintained
upstream for important bug/crash fixes, with an announced annual release
cadence (latest 3.24.52, March 2026; next expected ~March 2027) and **no
published end-of-life date**
([gtk-3-24 releases](https://download.gnome.org/sources/gtk/3.24/),
[Phoronix](https://www.phoronix.com/news/GTK3-Annual-Release-Cadence)).
It remains packaged broadly: Ubuntu 22.04/24.04 LTS and the 2026 LTS line
carry `libgtk-3` (as `libgtk-3-0t64` on the 64-bit-time transition), Debian
12/13 and Fedora still ship it, and LTS/ESM cycles extend coverage into the
late 2020s. For ViewMD that makes GTK 3 a fine runtime dependency through
this decade; a toolkit move should only be revisited if upstream actually
declares EOL or a distro drops it - and Wayland-specific rendering regressions
are the area most likely to need a backport.

## Packaging Templates

- Arch: `packaging/arch/PKGBUILD`
- Debian: `packaging/deb/control.in`
- RPM: `packaging/rpm/viewmd.spec.in`
## License

MIT License

## Other Useful Projects
- TrayMD is an app for taking notes in markdown with live editing [TrayMD](https://github.com/rabfulton/TrayMD)
- Preditor is an image viewer with a similar philosophy to viewmd [preditor](https://github.com/rabfulton/preditor)
- Try my AI panel plugin for XFCE [XFCE Ask](https://github.com/rabfulton/xfce-ask)
- For a feature complete AI application try out [ChatGTK](https://github.com/rabfulton/ChatGTK)
- A lightweight speech to text implementation [Auriscribe](https://github.com/rabfulton/Auriscribe)
- A lightweight local movie database and browser [ReelVault](https://github.com/rabfulton/ReelVault)
