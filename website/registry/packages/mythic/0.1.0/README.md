# MythicUI

MythicUI is a GUI library for [Mettle](https://github.com/suidvandiewereld/Mettle). It runs on Windows and Linux (X11) and it is written in Mettle all the way down. No GTK, no Qt, no libX11, no C glue.

It's immediate-mode. You write a function that describes your UI, MythicUI calls it when something changes, and your app state lives in your own variables. If a button was clicked, the button call returns true. That's it.

```mettle
if (mythic_button(ui, "Build")) { start_build(app); }
```

## Screenshots

<img width="1127" height="1140" alt="image" src="https://github.com/user-attachments/assets/bcf643e7-159a-4028-96a5-16d915453587" />

## What you get

- **Windows and Linux from one codebase.** Win32 on Windows, and the X11 protocol spoken directly over a socket on Linux.
- **It looks good out of the box.** A clean design system (light and dark themes) with Geist Serif for text and Geist Sans for labels and titles, soft shadows, rounded corners and proper focus rings.
- **Everything is drawn by MythicUI itself:** antialiased shapes, shadows, gradients, vector icons and TrueType text. The fonts are built into the library, so apps work on machines with no fonts installed.
- **Fast and quiet.** Only the parts of the window that changed get redrawn. A full 1080p repaint takes about 4 to 6 ms, a frame where nothing changed about 0.3 ms, and an idle app uses 0% CPU.
- **Layout** with rows and columns, padding, gaps, alignment, and fixed, fit, grow and percent sizes. Text can wrap or cut off with an ellipsis.
- **Widgets:** labels, headings, buttons (primary, secondary, ghost, danger, three sizes, icons), checkboxes, switches, radio buttons, sliders, single-line text inputs (selection, undo, copy and paste, placeholder, error and read-only states), progress bars, cards and panels, separators and icons.
- **Tabs and tooltips.** Underline tabs with a sliding indicator, and tooltips that show after a short hover or keyboard focus and stay inside the window.
- **Scroll views and long lists.** Smooth wheel scrolling, overlay scrollbars that fade out when you're done, and lists that only build the rows on screen, so 100k rows is fine.
- **Animated states.** Hover, press, focus and disabled all animate smoothly. There's a reduced motion option.
- **Keyboard support.** Tab and Shift+Tab move focus (and scroll the focused widget into view), Enter and Space activate, arrow keys work in radio groups, tabs, sliders and lists, and text inputs have the usual editing keys.
- **HiDPI.** Everything scales with the screen DPI.

## Getting started

You need the Mettle compiler. Put a `mettle.deps` file next to your app that points at MythicUI's `src` folder:

```
mythic=path/to/MythicUI/src
```

MythicUI draws with OpenGL 3.3 when it can, through [Glint](https://github.com/Lex-Dellimere/Glint) (clone it next to MythicUI, or put `glint=path/to/Glint/src` in a `mettle.deps` in a folder above MythicUI's), and falls back to its CPU renderer when GL is missing or fails, including a context lost later (a driver reset or a failed swap). A window's first frames are drawn on the CPU while GL starts and compiles its shaders, so it never sits blank. GL libraries are loaded at run time, so nothing extra is needed to run.
Set `MYTHIC_RENDERER=cpu` or `MYTHIC_RENDERER=gl` to pick one, or `renderer: MYTHIC_RENDERER_CPU` in the app description. Inside a frame, `mythic_renderer(ui)` tells you which one is drawing and `mythic_frame_ms(ui)` how long the last frame took.

Then write your app:

```mettle
import "mythic/mythic";

struct App { count: int32; }

fn frame(ui: MythicUi*, ud: rawptr) {
  var app: App* = (App*)ud;
  mythic_column_begin(ui, { pad: mythic_pad(24), gap: 12 });
    if (mythic_button(ui, "Click me")) { app->count += 1; }
    mythic_label(ui, "Clicked {app->count} times");
  mythic_end(ui);
}

fn main() -> int32 {
  var app: App;
  return mythic_run({ title: "Hello", width: 480, height: 320 }, &frame, (rawptr)&app);
}
```

A text input edits a `MythicTextBuf` that your app owns (a zeroed one is an empty buffer):

```mettle
if (mythic_text_input_ex(ui, "Name", &app->name, { placeholder: "Ada Lovelace" })) { app->dirty = true; }
if (mythic_last(ui).submitted) { save(app); }    // Enter was pressed
```

A scroll view is a container: open it with `mythic_scroll_begin(ui, "log", { height: mythic_grow() })` and close it with `mythic_end(ui)`. A list asks which rows are on screen and builds only those:

```mettle
mythic_list_begin(ui, "files", { width: mythic_grow(), height: mythic_grow() });
  var first: int64 = 0; var last: int64 = 0;
  mythic_list_visible_range(ui, 32, app->count, &first, &last);
  for i: int64 in first..last {
    mythic_push_id_int(ui, i);
    if (mythic_list_item(ui, app->names[i], i == app->selected)) { app->selected = i; }
    mythic_pop_id(ui);
  }
mythic_end(ui);
```

Tabs take an array of labels and the selected index. A tooltip goes right after the item it explains:

```mettle
var pages: string[3] = ["General", "Appearance", "Advanced"];
if (mythic_tabs(ui, "settings", pages, &app->page)) { app->dirty = true; }
mythic_button(ui, "Save"); mythic_tooltip(ui, "Save the settings");
```

Build it:

```sh
mettle --build -lc app.mettle -o app        # Linux (-lc: GL is loaded at run time), or Windows with the native compiler
scripts/mythic-build --windows app.mettle -o app.exe   # Windows .exe from Linux (needs mingw-w64)
```

With [mbuild](https://github.com/Lex-Dellimere/mbuild), `mbuild build` in this repo builds the showcase, and an app can depend on MythicUI with `mb_dep_path("mythic", "<path to MythicUI>")`.

## Examples

- `examples/showcase` shows what works today: cards, buttons, radio groups, checkboxes, switches, a slider, an animated build progress bar, icons, keyboard focus and a dark mode switch.

```sh
cd examples/showcase
mettle --build -lc showcase.mettle -o showcase && ./showcase
```

More examples are on the way (see below).

## How it works

Every frame your function declares the UI. MythicUI lays it out, turns it into a list of draw commands, and only redraws the screen tiles whose commands changed. The platform layer just opens a window, shows the pixels and passes input back. Platform code is behind one small interface, and the OpenGL and CPU renderers draw the same command list, so neither touches the widgets.

## Fonts

MythicUI ships Geist Serif for body text, Geist Sans (Medium and SemiBold) for labels, buttons and titles, JetBrains Mono for monospace text, and IBM Plex Mono as a second monospace, all subset in `assets/fonts`. Pick IBM Plex Mono with `mythic_theme(ui)->type.mono.font = MYTHIC_FONT_MONO_ALT;`. All of them are under the SIL Open Font License, see the license files in that folder. IBM Plex Mono reserves the name "Plex", so the subset copy is renamed "Mythic Mono Alt" as the license requires.

## Coming next

MythicUI is still being built. These are the next things to land:

- A nicer method-style API with callbacks: `ui->button("Save")`, `ui->on_click(fn() { save(app); })`
- Proper group opacity for disabled widgets
- More examples: a widget gallery, a build monitor, a settings window, a data explorer and a dashboard with charts

## License

MythicUI is under the [Apache License 2.0](LICENSE). The fonts in `assets/fonts` are under the SIL Open Font License 1.1; see [NOTICE](NOTICE).
