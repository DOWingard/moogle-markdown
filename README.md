# <img src="./resources/icons/sc-apps-moogle.svg" align="left" width="32" style="padding-right:5px"> moogle markdown

*moogle* is a distraction-free Markdown editor for Linux and Windows. It is a fork of
KDE's ghostwriter. It keeps ghostwriter's writing environment, themes, live preview,
outline, statistics and export options, and adds:

- **In-place rendered view.** With Live Preview off, press <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>V</kbd>
  (View ▸ Swap Raw/Rendered View) to swap the editor between raw Markdown and the
  rendered document. The scroll position, caret and selection carry over in both
  directions.
- **A moogle mascot.** A small, translucent moogle sits in the editor's bottom-right
  corner: the classic moogle in light mode and the Organization XIII moogle in dark mode.
  It ignores the mouse, hides in distraction-free mode, and can be turned off with
  View ▸ Show Mascot.

## The rendered view

The rendered view shows the document as the built-in cmark-gfm processor renders it,
with the same theme CSS as the live preview. It is read-only, but you can still navigate
it: click to place the caret, use the arrow, Page Up/Down and Home/End keys, extend a
selection with <kbd>Shift</kbd>, and copy. <kbd>Ctrl</kbd>+click opens a link.

Swapping keeps your place:

| Swap | What carries over |
|------|-------------------|
| raw → rendered | The source line at the top of the editor goes to the top of the view. The caret and selection map to the same characters in the rendered text. |
| rendered → raw | If you scrolled, clicked or selected in the rendered view, those positions map back to the Markdown source. If you didn't, the editor returns exactly as you left it. |

Markup that has no rendered form, such as `**`, link URLs, list numbers and fence
lines, maps to the nearest visible character. Editing commands are disabled while
the rendered view is showing. Find, Replace and spell check switch back to raw first.
Turning Live Preview on also switches back to raw, since the two views are alternatives.

The rendered view always uses cmark-gfm, even if a different processor is selected for
the live preview, because cmark-gfm can report where each block comes from in the source.

## Build

You need Qt 6 (Base, Svg, WebEngine, WebChannel, Tools), KDE Frameworks 6 (CoreAddons,
ConfigWidgets, WidgetsAddons, XmlGui, Sonnet) and extra-cmake-modules.

Arch Linux:

    sudo pacman -S cmake ninja extra-cmake-modules qt6-base qt6-svg qt6-webengine qt6-webchannel qt6-tools \
        kcoreaddons kconfigwidgets kwidgetsaddons kxmlgui sonnet

Debian/Ubuntu:

    sudo apt install cmake ninja-build extra-cmake-modules qt6-base-dev qt6-svg-dev qt6-webengine-dev \
        qt6-webchannel-dev qt6-tools-dev libkf6coreaddons-dev libkf6configwidgets-dev \
        libkf6widgetsaddons-dev libkf6xmlgui-dev libkf6sonnet-dev

Then:

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    sudo cmake --install build    # optional

The executable is `build/bin/moogle`.

## Usage

    moogle [--disable-gpu] [file.md]

`--disable-gpu` switches to software rendering, which can help on systems where
OpenGL misbehaves.

The live preview and export can also use Pandoc, MultiMarkdown or cmark if they are
on your `PATH`.

Settings live under `~/.config/moogle.local/moogle.conf`. Drafts, backups and custom
themes live under `~/.local/share/moogle/`.

## Tests

The unit tests are built with `-DBUILD_TESTING=ON`. The rendered view's
source-to-DOM mapping has its own round-trip test, which needs headless Chrome:

    cmake --build build --target cmark-gfm
    autotest/renderedview/mapping_test.py --cmark build/bin/cmark-gfm [--chrome /path/to/chrome]

## Mascot artwork

The mascot images are 2D renders of 3D model rips, produced offline by
`tools/mascot/render_obj.py` (a small numpy rasterizer for textured OBJ files) and
`tools/mascot/make_assets.py` (sprites, app icons, About banner):

    python render_obj.py MODEL.obj out.png --yaw 25 --size 1024 --ss 3 --flip-v
    python make_assets.py light.png dark.png <repo root>

- Dark mode: the XIII Moogle from *Kingdom Hearts 358/2 Days* (Nintendo DS), ripped by **Zerox**.
- Light mode: the Moogle from *Kingdom Hearts* (PlayStation 2), from The Models Resource.

The moogle is a character owned by Square Enix. This project is not affiliated with or
endorsed by Square Enix. The model rips are not covered by the GPL.

## Licensing

The source code is licensed under the [GNU General Public License Version 3](http://www.gnu.org/licenses/gpl.html)
or later, like the ghostwriter code it is built from. Bundled third-party code
(cmark-gfm, MathJax, React) and icons have their own GPL-compatible licenses. See the
COPYING and LICENSE files in their folders.
