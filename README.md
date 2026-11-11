# <img src="assets/jotilla.png" width="36" height="36" alt="Greek iota on a yellow note"> Jotilla - Sticky notes, on silicon

Small, native notes with autosave, an archive and a calendar. No extra dependencies.

**[Visit the Jotilla website](https://garaugarau.github.io/Jotilla/)**

## Fork enhancements (`marcalfa`)

Built on top of [GarauGarau/Jotilla](https://github.com/GarauGarau/Jotila) (`windows` branch) with four Windows additions:

- **Global `Always on top` toggle** — right-click the tray icon → **Options** → **Always on top** (persisted in `notes.ini`, applied immediately to all notes, dialogs, and the archive browser).
- **Persistent `Notes archive` placement** — remembers position, size, monitor, and maximized state across minimize-to-tray, close, and exit, with safe fallback if a monitor is disconnected.
- **Note bar date display** — shows the assigned calendar date after the title (`Title (Weekday DD-MM-YYYY)`) and highlights today/past dates in contrast-safe red.
- **Tray privacy mode (`Hide all notes` / `Show all notes`)** — hides all open notes and the archive browser from the desktop, switches the tray icon to slate-blue (`assets/jotilla_hidden.ico`), and restores them from the tray menu.

## Download

| System | Installer | Portable · no installation |
|---|---|---|
| Windows 10/11 · x64 | [Setup (.exe)](https://github.com/GarauGarau/Jotilla/releases/download/v1.10.0/Jotilla-Setup-x64.exe) | [Portable ZIP](https://github.com/GarauGarau/Jotilla/releases/download/v1.10.0/Jotilla-portable-x64.zip) |
| macOS 13+ · Apple Silicon / Intel · preview | [Setup (.pkg)](https://github.com/GarauGarau/Jotilla/releases/download/v1.10.0/Jotilla-Setup-universal.pkg) | [Portable ZIP](https://github.com/GarauGarau/Jotilla/releases/download/v1.10.0/Jotilla-portable-universal.zip) |

Run the installer, or extract the portable ZIP and launch Jotilla. For the Mac portable app, select the included `Notes.jotillaarchive` folder when prompted.

[Release notes and checksums](https://github.com/GarauGarau/Jotilla/releases). The macOS preview is not notarized.

![Formatting, checklists and a collapsed note](docs/screenshots/notes.png)

| Action | Windows | macOS |
|---|---|---|
| New note (global) | **Ctrl + Alt + +** | **Ctrl + Option + Cmd + N** |
| Open note explorer | **Ctrl + E** | **Cmd + E** |
| Search notes and tags | **Ctrl + Shift + F** | **Cmd + F** |
| Turn `#Title` into a title | **Shift + Enter** | **Shift + Enter** |
| Start a checklist after `*` | **Shift + Enter**; **Enter** continues it | **Shift + Enter**; **Enter** continues it |
| Bold, italic, underline | **Ctrl + B / I / U** | **Cmd + B / I / U** |
| Strikethrough | **Ctrl + Shift + X** | **Cmd + Shift + X** |
| Undo / redo | **Ctrl + Z / Y** | **Cmd + Z / Cmd + Shift + Z** |
| Resize note, or selected text | **Ctrl + + / −**, **Ctrl + wheel** or pinch | **Cmd + + / −**, **Cmd + wheel** or pinch |
| Edit tags | **Ctrl + T** | **Cmd + T** |

Drag the bar to move; double-click its title to rename. **− / □** folds or expands; **×** closes and keeps the note in the archive. Resizing preserves visible content.

Open notes return to their saved positions, sizes and folded state, with their title bars kept reachable if a monitor is disconnected.

On Windows, right-click a note's bar and choose **Scrollable text** to allow longer
text with vertical scrolling and no visible scroll bar. Use the mouse wheel,
Page Up/Down or Ctrl+Home/End. Apply it to this note, or also make it the default
for new notes. Both choices survive restart; other existing notes keep their mode.

Click the note's **calendar icon** to assign, change or remove its day. The square calendar button beside **Archive** switches to the year view: circled days open a note, or a compact title list when several share the day. Type a year directly in either calendar. In day lists, **↑ / ↓**, **Page Up / Down**, **Home / End** navigate, **Enter** opens and **Esc** returns. Search also filters calendar notes.

Right-click the text for [80 color palettes](docs/palettes.md); select text first to color just that selection. **← / →** browse, click applies, **★** applies a favorite to this note and new notes, including after restart. Right-click the bar for more options.

Search by text or tags: `#work`, `"release plan"`, or `-#` for untagged notes. **F5** reloads external edits. Double-click the tray or menu bar icon to open the explorer, single-click to recall the last used note (or the explorer if it is unavailable). Create notes with the global shortcut or **New note** in the icon’s right-click menu. The last-note identity is saved with the session, independently for each archive.

| Notes and shortcuts | Archive and calendar |
|---|---|
| ![Note editing walkthrough](docs/screenshots/notes-demo.gif) | ![Archive and calendar walkthrough](docs/screenshots/browser-demo.gif) |

Notes save as UTF-8 text in the archive folder. Empty notes stay unsaved. **Archive** moves or switches folders; back up the whole folder, including `notes.ini` and RTF files. Updates and uninstall preserve your notes.

## Why Jotilla?

The name brings together three ideas:

- **Jota** recalls [iota](https://www.treccani.it/vocabolario/iota1/), the Greek letter ι in the logo and a word for a tiny amount: small thoughts, kept in a small app.
- **Jotting** is English for a brief note. To [jot something down](https://dictionary.cambridge.org/dictionary/english/jot) is to write it quickly before you forget it.
- **Postilla** is Italian for a [short annotation or added comment](https://www.treccani.it/vocabolario/postilla/), often written in a text's margin. It supplies the double **l** and the idea of a note alongside what you are doing.

## Source and builds

| Platform | Branch | Build, installation and data |
|---|---|---|
| Windows | [`windows`](https://github.com/GarauGarau/Jotilla/tree/windows) · default | [Windows guide](docs/windows.md) |
| macOS | [`macos`](https://github.com/GarauGarau/Jotilla/tree/macos) | [macOS guide](https://github.com/GarauGarau/Jotilla/blob/macos/docs/macos.md) · [Feature coverage and validation](https://github.com/GarauGarau/Jotilla/blob/macos/docs/macos-validation.md) |

The website lives on its own [`gh-pages` branch](https://github.com/GarauGarau/Jotilla/tree/gh-pages), with [source and publishing instructions](https://github.com/GarauGarau/Jotilla/blob/gh-pages/README.md).

[Performance measurements](docs/performance.md).

`tools/capture.ps1` regenerates the real-app screenshots and GIFs using ImageMagick and local artwork (`-BackgroundRoot`). Backgrounds: Birmingham Museums Trust — *Sundown, Loch Rannoch*, *February Fill Dyke* and *Walton-on-the-Naze*.
