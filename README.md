# MDView

**A Markdown viewer plugin for Total Commander.**

Press F3 on any `.md` file and get a clean, fully rendered preview — dark mode, syntax highlighting, table of contents, find-in-page, and more. Single C file, zero dependencies, ~120 KB.

![License](https://img.shields.io/badge/license-MIT-blue.svg)

---

## Screenshots

> *TODO: Add screenshots of light mode, dark mode, and the help overlay.*

## Features

- **Full Markdown rendering** — headings, bold, italic, strikethrough, links, images, tables with column alignment, fenced and indented code blocks, blockquotes (nested), ordered/unordered/task lists, horizontal rules, autolinks, and escape sequences
- **Syntax highlighting** — JavaScript, TypeScript, Python, C, C++, C#, Java, Rust, Go, SQL, Bash, CSS/SCSS, PHP, HTML, and XML
- **Dark / light mode** — toggle with Ctrl+D, or auto-detected from your Windows theme on first launch
- **Adjustable layout** — zoom in/out, widen/narrow the reading column
- **Line numbers** — toggle on code blocks with Ctrl+L
- **Table of Contents** — auto-generated sidebar from your headings
- **Find in page** — incremental search with match highlighting and navigation
- **Expand / collapse** — long code blocks and blockquotes are collapsed by default with a "Show more" button
- **Persistent settings** — font size, theme, column width, and line numbers are saved and restored between sessions
- **Print support** — Ctrl+P renders a clean printable version
- **Progress bar** — subtle reading position indicator at the top of the viewport

## Keyboard Shortcuts

| Shortcut | Action |
|---|---|
| `Ctrl` `+` | Zoom in |
| `Ctrl` `-` | Zoom out |
| `Ctrl` `0` | Reset zoom |
| `Ctrl` `W` | Widen columns |
| `Ctrl` `Shift` `W` | Narrow columns |
| `Ctrl` `D` | Toggle dark / light mode |
| `Ctrl` `L` | Toggle line numbers |
| `Ctrl` `T` | Table of Contents |
| `Ctrl` `F` | Find in page |
| `Ctrl` `P` | Print |
| `Ctrl` `G` | Go to top |
| `Esc` | Close viewer |
| `F1` | Show shortcut reference |

Press **F1** inside the viewer for an on-screen reference with keycap-styled indicators.

## Download

Grab the latest release from the [Releases](../../releases) page. The zip contains both 32-bit and 64-bit builds.

## Installation

### Automatic

Open the downloaded `.zip` file **inside Total Commander** (navigate to it and press Enter or double-click). The included `pluginst.inf` will trigger TC's automatic plugin installer.

### Manual

1. Extract `mdview.wlx` (32-bit) or `mdview.wlx64` (64-bit) to a directory of your choice
2. In Total Commander: **Configuration → Options → Plugins → Lister (WLX) → Add**
3. Select the `.wlx` / `.wlx64` file
4. The detect string auto-configures for `.md`, `.markdown`, `.mkd`, and `.mkdn` extensions

## Usage

1. Navigate to any Markdown file in Total Commander
2. Press **F3** to open the lister
3. Use keyboard shortcuts to customise the view — your preferences are saved automatically

## Building from Source

The entire plugin is a single C file. Cross-compile from Linux with MinGW, or build natively on Windows with any GCC or MSVC toolchain.

```bash
# 32-bit
i686-w64-mingw32-gcc -shared -o mdview.wlx mdview.c mdview.def \
    -lole32 -loleaut32 -luuid -ladvapi32 -O2 -s -static-libgcc

# 64-bit
x86_64-w64-mingw32-gcc -shared -o mdview.wlx64 mdview.c mdview.def \
    -lole32 -loleaut32 -luuid -ladvapi32 -O2 -s -static-libgcc
```

No external libraries or build systems required.

## How It Works

MDView is a WLX lister plugin — a DLL that Total Commander loads when you press F3 on a matching file type. It contains a built-in Markdown-to-HTML converter and embeds an MSHTML (IE11) WebBrowser control to render the output. Keyboard input is handled by subclassing the browser's internal window, giving reliable hotkey interception without interfering with normal scrolling or TC's own key handling. Settings are persisted via TC's standard INI file mechanism.

## File List

| File | Description |
|---|---|
| `mdview.c` | Complete plugin source (~1250 lines) |
| `mdview.def` | DLL export definitions |
| `pluginst.inf` | Total Commander auto-install manifest |
| `test.md` | Sample document exercising all features |

## License

[MIT](LICENSE)
