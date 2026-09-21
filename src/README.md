# pdfviewer

A minimal PDF viewer: Qt 6 (C++) + Qt WebEngine + PDF.js.

## Build on Fedora

```bash
sudo dnf install cmake gcc-c++ curl unzip qt6-qtbase-devel qt6-qtwebengine-devel

./scripts/fetch-pdfjs.sh          # downloads the latest PDF.js release into ./pdfjs
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"

./build/pdfviewer some.pdf        # or run with no argument and press Ctrl+O
```

## Usage

| Action | How |
|---|---|
| Open a file | `pdfviewer file.pdf`, **Ctrl+O**, or PDF.js's own Open button |
| Open another PDF | just start `pdfviewer other.pdf` again: it opens a new window in the running instance |
| Text selection / hand tool | Buttons in the toolbar (or **S** / **H**) |
| Print | PDF.js Print button or **Ctrl+P** (opens the CUPS print dialog) |
| Save / download | PDF.js toolbar (asks where to save) |
| Quit | **Ctrl+Q** |

Zoom, search, thumbnails, outline, presentation mode, etc. are all PDF.js's own UI.

## One process, many windows

Starting `pdfviewer` while it is already running does not start a second copy:
the new launch hands its files to the running one (over a socket in
`$XDG_RUNTIME_DIR`), which opens a window per file, and exits immediately. This
also means only one process ever uses the persistent WebEngine profile on disk;
two processes sharing that storage is a known way to end up with a blank window.
If the socket cannot be used, the process falls back to an off-the-record profile
instead of touching the shared storage. Closing the last window quits the program.

## Look and feel

- **Always dark:** the window and web view are painted in PDF.js's dark background
  color from the first frame, and PDF.js is forced into its dark theme before its first
  paint, so there is no white flash while a document loads (and no dark-to-light jump
  when the viewer appears). To switch back to following the desktop theme, remove
  `kDarkThemeScript`, `kBackground` and their use in the `Viewer` constructor.
- **Toolbar:** the text-selection and hand tools are shown in the main toolbar
  (PDF.js only has them in the >> menu). They click the original buttons and mirror
  their state and localised tooltip, so shortcuts and translations keep working.
  On narrow windows they hide again and remain available in the >> menu.
- **Font:** the PDF.js interface uses the desktop's UI font (`QApplication::font()`).
  The PDF content itself is unaffected.
- **Program icon:** `document-viewer` from your icon theme; if the theme has none,
  the first available of `org.gnome.Papers`, `org.gnome.Evince`, `evince`, `okular`,
  `application-pdf`. Edit `appIcon()` in `main.cpp` to change the list.

Under **Wayland** the dock/window-switcher icon comes from the desktop file, not
from the program, so install it (also adds "PDF Viewer" to the app menu and the
"Open With" list for PDFs):

```bash
cmake --install build --prefix ~/.local
```

If the icon is missing there, change `Icon=document-viewer` in `pdfviewer.desktop`
to a name your theme has, e.g. `org.gnome.Papers` or `org.gnome.Evince`.

## Printing (CUPS)

Print uses Qt's `QPrintDialog`, which lists the printers known to CUPS
(`lpstat -p`) and offers copies, page range, duplex, paper size, etc. It also
has "Print to File (PDF)", handy for testing. Annotations you added in PDF.js are printed.

Requirements: a running CUPS (`systemctl status cups`, the Fedora default) and no
extra packages - `qt6-qtbase-devel` already includes Qt PrintSupport.

Implementation notes: PDF.js renders pages into a hidden container and calls
`window.print()`, then deletes the container again right away, so a dialog opened
afterwards prints blank pages. `main.cpp` therefore injects a small gate script in
front of PDF.js's `window.print` that calls a synchronous `confirm()`; the
`QWebEnginePage::javaScriptConfirm` override shows the dialog while the page is
frozen, and only then lets PDF.js proceed. The native print step
(`QWebEngineView::printRequested`) then prints with the configured `QPrinter`.

## How it works

`pdfjs://app/` is a custom URL scheme served by `PdfJsSchemeHandler`:

- `pdfjs://app/web/viewer.html` and friends come from the `pdfjs/` folder
- `pdfjs://app/doc/...` streams whichever PDF is currently open

Since viewer and document share an origin, PDF.js's `?file=` check passes and
no `file://` workarounds (`--allow-file-access-from-files`) are needed.

## Legacy vs. modern PDF.js

`fetch-pdfjs.sh` downloads PDF.js's **legacy** build by default. The "modern"
build uses very new JavaScript built-ins without polyfills (e.g.
`Map.prototype.getOrInsertComputed`) and fails inside Qt WebEngine's Chromium with
`TypeError: ... getOrInsertComputed is not a function` and a blank window.
The legacy build bundles those polyfills. Only use `./scripts/fetch-pdfjs.sh modern`
if your Qt WebEngine is very recent.

After switching, re-run `cmake --build build` to refresh the bundled copy.
Pin a specific release with `PDFJS_VERSION=5.4.149 ./scripts/fetch-pdfjs.sh`.

## Where PDF.js is looked up

`$PDFJS_DIR`, then `<binary dir>/pdfjs`, then `<binary dir>/../share/pdfviewer/pdfjs`
(`cmake --install` uses the last one).
