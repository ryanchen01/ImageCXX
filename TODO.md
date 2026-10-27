# Todo

Missing or incomplete features, checked against the current source. Unchecked items include unfinished parts of existing features. Optional and deferred work is marked separately.

## Shortcut commands and editing

Register new commands through the existing `CommandRegistry`, with Qt actions, menus, shortcuts and generated parameter forms as appropriate.

- [x] Add a color picker.
- [x] Add selection properties beyond the existing ROI rename and drag/resize controls. Edit names, outline colors and visibility, rectangle/ellipse bounds and line endpoints through the command registry. Apply updates saved ROIs without changing image pixels or their saved Z/C/T positions.
- [ ] Add the overlay-add command. Use separate project-owned ROI/annotation models and the viewer's overlay layer.
- [x] Add reslice. Use ITK for resampling and spatial transformations, preserve geometry and numeric types, and declare the operation's scope.
- [x] Add orthogonal views as separate synchronized windows and keep image ownership in the model.
- [ ] Add gel-lane analysis commands.
- [ ] Add the ImageJ control panel command.
- [ ] Add the remaining ImageJ tool modifiers. Keep text fields and modal dialogs' keyboard handling intact.
- [x] Add configurable drawing colors. Fill and Draw use the selected color with type-aware scalar intensity mapping. Clear uses zero.
- [ ] Extend Draw beyond area selections.
- [x] Add paste modes beyond the existing typed internal copy/paste. Paste special supports typed or system images and current or new image destinations. Copy prepares a PNG in the background at native dimensions, scaled proportionally when needed to reach at least 320 pixels on one axis. Internal copying remains available when PNG export exceeds its memory budget.
- [ ] Add modifier-specific histogram and profile dialogs. Reuse Qt Graphs 2D in separate `QQuickWidget` plot windows, created on demand.
- [ ] Extend printing to include display LUTs and overlays. Printing currently uses the current plane's intensity range only. Keep scientific pixels separate from rendered display values.

## Viewer, models and desktop tools

- [x] Add synchronized multi-view windows. Users place and resize linked windows freely; independent image documents remain separate.
- [ ] Add a separate project-owned annotation model and annotation tools in the overlay layer. ROI outlines, ROI names, masks, crosshairs and scale bars already exist.
- [ ] Extend advanced channel/time workflows. Z/C/T browsing, animation and basic compositing already exist, but composite display currently uses at most four channels. Keep channel metadata and original pixel values in the model; apply blending in shaders.
- [ ] Add tile-based display and partial texture updates where practical. The viewer currently uploads whole changed slices and rejects slices exceeding texture or memory limits. Reuse GPU resources and keep CPU/GPU caches bounded.
- [ ] Add scatter plots and calibration curves using Qt Graphs 2D and C++ data models. Current plots cover histograms and line profiles.
- [x] Add ROI measurement plots and time-series measurement plots using the same plotting module. Plot a fixed area ROI's mean intensity across Z or Time, with calibrated axes, background cancellation and CSV export of per-plane statistics. QML/Quick details stay inside the plotting module and plot windows are created on demand.

## Chunked storage and larger-than-RAM processing

- [ ] Implement a chunked/lazy storage provider behind the existing `Storage` interface. RAM and read-only memory mapping already exist. Support region/slice access without decoding an entire compressed dataset into RAM.
- [ ] Add TensorStore-backed asynchronous chunked I/O and an OME-Zarr adapter. Parse OME-NGFF metadata explicitly and validate axes, calibration and multiscale semantics independently.
- [ ] Add processing that streams input regions and writes results to chunked or streaming destinations. Current ITK processing allocates complete output images; streamed input alone does not bound output memory.
- [ ] Extend undo/edit behavior for out-of-core arrays. Keep explicit snapshot or copy-on-write ownership and separate configurable budgets for decoded slices, GPU textures and history. Existing in-memory history is bounded, and some large processing results already open as new documents.

## Scientific I/O and algorithms

- [ ] Add OME-TIFF metadata support with libtiff and an OME-XML adapter. Explicitly map multidimensional pages, axes and calibration and preserve numeric data.
- [ ] Complete ImageJ TIFF interoperability. The reader imports page-based ImageJ descriptions, but contiguous ImageJ stacks without one directory per page are unsupported, and the writer emits ImageCXX metadata. Use a project-owned adapter with libtiff for ImageJ-compatible metadata export and validate round trips.
- [ ] Add further scientific formats according to demand through ITK I/O adapters. MHA/MHD, NIfTI and NRRD already exist. Declare supported pixel types, metadata behavior, lazy access and failure modes for each additional reader; verify metadata round trips.
- [ ] Add requested proprietary microscope formats through individual reader plugins. Track unsupported formats explicitly. Keep readers free of Java/Bio-Formats dependencies.
- [ ] Add registration and further spatial transformations through ITK. Existing processing covers Gaussian, median, threshold, binary dilation/erosion and resampling. Preserve spatial metadata and define single-slice, ROI, volume or streamed-region scope for each operation.
- [ ] Add further multidimensional filtering and segmentation workflows as needed. Current filters run on 2D/3D spatial data with independent channel/time volumes. ITK owns scientific algorithms; project code owns validation, adapters, cancellation, progress and result publication.

## Native plugins and automation

No native plugin SDK or Python worker implementation exists in the current tree.

- [ ] Add a `plugin_sdk` CMake target and native extension host with a versioned C ABI. Use opaque host-managed handles and function tables; keep Qt types, STL containers and C++ exceptions out of the public ABI.
- [ ] Add plugin manifests with ID, name, version, supported host API version and capabilities.
- [ ] Allow plugins to register commands and filters through the common command registry. Reuse typed parameter descriptions for automatic Qt forms.
- [ ] Add validated image, ROI and metadata access through handles or buffer descriptors. Define memory lifetimes, concurrency, error handling, progress, cancellation and result-return contracts.
- [ ] Add plugin reader/writer registration hooks for specialized formats.
- [ ] Add sample native plugins and packaging/version-compatibility tests. Build separate binaries for each supported OS and architecture.
- [ ] Add an optional Python worker service, started on demand in a separate process. Use structured IPC for commands and metadata and NumPy-compatible shared memory for large arrays. Define buffer ownership and cleanup; avoid whole-stack JSON serialization and repeated copies.
- [ ] Add Python script/filter execution, a controlled dependency environment, worker restart and error handling. Keep Python out of the normal launch path.
- [ ] Add scripting and macro recording through the common command definition/execution API. Menus, search, generated forms and built-in batch processing already use the registry; extend it to plugin commands and automation.

## Optional and deferred functionality

These items need a product decision or measured demand before implementation.

- [ ] Add VTK volume rendering, surface meshes and advanced 3D tools when needed. Isolate VTK in an optional module loaded on demand. Start with a separate 3D window if needed and validate Qt/VTK integration on all target platforms without assuming its OpenGL widget shares the viewer's RHI backend.
- [ ] Add selected OpenCV operations only when benchmarks show a benefit over existing implementations. OpenCV is not currently a dependency; ITK remains the primary scientific backend.
- [ ] Add GPU compute for expensive algorithms only after measuring end-to-end speedups, including transfer costs and precision. The compute backend remains undecided.
- [ ] Decide whether to implement ImageJ macro installation and exact macro-language compatibility. Macro installation is currently unbound; compatibility is explicitly a separate, non-default feature requiring its own implementation and tests. Plugin/user macro shortcut mappings are outside the current shortcut mapping.
- [ ] Decide whether any original Java-plugin compatibility layer is needed. Existing Fiji JAR plugins cannot run through the planned native C ABI.
- [ ] Choose Python runtime packaging and third-party package installation policy before distributing Python support.

## Release and validation work

Unfinished delivery and validation work.

- [ ] Add cross-platform release artifact packaging to GitHub Actions and verify deployed applications. Build/test jobs and a Qt deployment install script already exist; CI currently uploads test logs only. Bundle the required modules/plugins and establish platform packaging, signing and notarization.
- [ ] Select minimum OS versions, Linux Wayland/X11 support and supported GPU drivers through prototypes and distribution tests.
- [ ] Add representative desktop benchmarks for cold/warm launch, time to first slice, cached/uncached rapid scrolling, pan/zoom with overlays, idle/peak RAM, GPU memory/uploads/cache eviction, and processing throughput/core scaling. Current Google Benchmark coverage consists of CPU microbenchmarks.
- [ ] Measure plugin discovery and first execution after the plugin host exists.
- [ ] Add numerical comparisons against Fiji/reference implementations for the relevant workflows. Existing correctness tests do not constitute that cross-application validation.
- [ ] Record dataset shape/type/compression, storage medium, OS, CPU/GPU and application build for benchmark results on Windows, macOS and Linux.
