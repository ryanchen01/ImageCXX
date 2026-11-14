# Keyboard shortcuts

Ctrl means Command on macOS. Alt means Option. Shift keeps its usual meaning. Text editors, value fields and modal dialogs retain their own keyboard handling.

| Keys | ImageCXX command |
| --- | --- |
| Ctrl+J, Ctrl+Shift+C | Display |
| Ctrl+N, Ctrl+Shift+N, Ctrl+Shift+V | New image, text, clipboard image |
| Ctrl+O, Ctrl+Shift+O, Alt+Shift+O | Open, next file, previous file |
| Ctrl+S, Ctrl+R, Ctrl+P, Ctrl+W | Save image, revert, print, close |
| Ctrl+Z, Ctrl+Y | Undo, redo |
| Ctrl+X, Ctrl+C, Ctrl+V | Cut, copy, paste |
| Alt+C | Copy, including a PNG for other applications |
| Backspace, Delete, Ctrl+F, Ctrl+D | Clear, fill, draw outline |
| Ctrl+Shift+I, Ctrl+Shift+S, Ctrl+Shift+R | Invert pixels, smooth, repeat processing |
| Ctrl+A, Ctrl+Shift+A, Ctrl+Shift+E, Ctrl+T | Select all, none, restore, add to ROI manager |
| Ctrl+I, Ctrl+Shift+P, Ctrl+Shift+Z | Info, properties, channel controls |
| Ctrl+Shift+T, Ctrl+E | Threshold form, resampling form |
| Ctrl+Shift+X, Ctrl+Shift+D | Crop, duplicate |
| Ctrl+M, Ctrl+H, Ctrl+K | Measure, histogram, line profile |
| L, Ctrl+L | Focus command search in the bottom bar |
| Ctrl+Shift+G | Capture screen |
| Ctrl+Shift+F, ], Tab, Enter | Show windows, show windows, next image, toolbar |
| Ctrl+0, 4, 5 | Fit, fit, 100% zoom |
| +, -, Up, Down | Zoom in/out |
| Left/Right, </> | Stack navigation |
| Ctrl+Left/Right, Ctrl+</> | Hyperstack slices |
| Alt+Left/Right, Alt+</> | Hyperstack frames |
| Backslash | Toggle animation |
| Space held while dragging | Temporary pan |

## Navigation and selections

- **Canvas keys:** `Left`/`Right` step through channels in hyperstacks (`Ctrl` for slices, `Alt` for frames; `Alt` skips 10 positions in ordinary stacks). With an active selection, arrow keys nudge it by 1 pixel (`Alt`+arrows resize rectangles/ellipses; `Shift`+arrows ignore the selection). Press `Enter` to finish a polygon.
- **Mouse and trackpad:** Pinch or hold `Ctrl`/`Cmd` while scrolling to zoom. Plain scrolling browses stack slices (or zooms a single image). Middle-drag or hold `Space` while dragging to pan.
- **Command search:** Press `L` or `Ctrl+L` to focus the bottom-right search bar, type a command name or category, and press `Enter` (or click a result) to run it.

## ROIs and drawing

- **ROI manager:** Press `Ctrl+T` to save the active selection to the ROI manager. **Select none** clears the canvas selection without deleting saved ROIs.
- **Selection properties:** **Edit > Selection properties** lets you edit an ROI's name, color, visibility, and zero-based pixel coordinates (`Apply` updates without closing, `OK` saves and closes, `Cancel` reverts to the last `Apply`). Polygon vertices remain editable on the canvas.
- **Drawing color:** **Edit > Drawing color** (or the toolbar swatch, white by default) sets the color used by **Fill** and **Draw** on the current Z/C/T plane (undoable). Scalar images map the chosen grayscale intensity to the image's numeric range; **Clear** writes zero.

## Copy and paste

- **Copy and Cut:** Store the selected bounds (or current plane) at full scientific precision internally, while exporting a display-rendered PNG in the background for other apps (images under 320 px on both sides scale up proportionally to 320 px on the longer edge).
- **Paste:** Pastes the internal typed copy across ImageCXX windows when pixel types match, or falls back to the system clipboard image. Pastes at the selection's top-left corner or image origin, or opens a new image if none is active.
- **Paste special:** **Edit > Paste special** chooses between **Automatic** or **System image** as the source and **Current image** or **New image** as the destination. `Ctrl+Shift+V` always opens the clipboard image as a new image, preserving 16-bit grayscale and RGB/RGBA alpha.

