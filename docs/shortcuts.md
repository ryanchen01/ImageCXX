# Keyboard shortcuts

Ctrl means Command on macOS. Alt means Option. Shift keeps its usual meaning. Text editors, value fields and modal dialogs retain their own keyboard handling.

| Keys | ImageCXX command |
| --- | --- |
| Ctrl+J, Ctrl+Shift+C | Display |
| Ctrl+N, Ctrl+Shift+N, Ctrl+Shift+V | New image, text, clipboard image |
| Ctrl+O, Ctrl+Shift+O, Alt+Shift+O | Open, next file, previous file |
| Ctrl+Shift+B | Sample stack |
| Ctrl+S, Ctrl+R, Ctrl+P, Ctrl+W | Save image, revert, print, close |
| Ctrl+Z, Ctrl+Y | Undo, redo |
| Ctrl+X, Ctrl+C, Ctrl+V | Cut, copy, paste |
| Alt+C | Copy rendered pixels to system clipboard |
| Backspace, Delete, Ctrl+F, Ctrl+D | Clear, fill, draw outline |
| Ctrl+Shift+I, Ctrl+Shift+S, Ctrl+Shift+R | Invert pixels, smooth, repeat processing |
| Ctrl+A, Ctrl+Shift+A, Ctrl+Shift+E, Ctrl+T | Select all, none, restore, add to ROI manager |
| Ctrl+I, Ctrl+Shift+P, Ctrl+Shift+Z | Info, properties, channel controls |
| Ctrl+Shift+T, Ctrl+E | Threshold form, resampling form |
| Ctrl+Shift+X, Ctrl+Shift+D | Crop, duplicate |
| Ctrl+M, Ctrl+H, Ctrl+K | Measure, histogram, line profile |
| Ctrl+L | Find command |
| Ctrl+Shift+G | Capture screen |
| Ctrl+Shift+F, ], Tab, Enter | Show windows, show windows, next image, toolbar |
| Ctrl+0, 4, 5 | Fit, fit, 100% zoom |
| +, -, Up, Down | Zoom in/out |
| Left/Right, </> | Stack navigation |
| Ctrl+Left/Right, Ctrl+</> | Hyperstack slices |
| Alt+Left/Right, Alt+</> | Hyperstack frames |
| Backslash | Toggle animation |
| Space held while dragging | Temporary pan |

Navigation keys operate in the image canvas. Unmodified hyperstack navigation changes channels. Alt skips ten positions in ordinary stacks. With an active selection, arrows move it one pixel; Alt+arrows resize rectangles and ellipses. Shift+arrows bypass selection movement. Enter completes a polygon while the polygon tool is active.

Draw a selection, then press Ctrl+T to retain it in the ROI manager. Select None clears the active selection without deleting saved ROIs. Internal copy/paste preserves scientific pixel types and requires matching destination types. Pixel edits affect the current Z/C/T plane and support undo.
