# Image I/O

The adapters return project-owned `Image` buffers. Reads and writes accept cancellation and progress callbacks and must run on the scheduler rather than the UI thread. Invalid dimensions, truncated input, unsupported encodings and cancelled operations throw standard exceptions.

| Adapter | Pixel types | Metadata | Storage |
| --- | --- | --- | --- |
| Qt PNG | UInt8/UInt16 grayscale, RGB8, RGBA8 | ImageCXX text preserves axes, calibration, channels and attributes | Decoded RAM |
| Qt JPEG | UInt8 grayscale, RGB8 | JPEG encoding is lossy; plugin metadata support varies | Decoded RAM |
| libtiff TIFF/BigTIFF | All model types, contiguous sample layout, strips or tiles | ImageCXX description preserves full metadata and shape; ImageJ descriptions import C/Z/T ordering and calibration; ordinary TIFF resolution is imported | Decoded RAM |
| ITK MHA/MHD, NRRD/NHDR | All model types supported by the corresponding ITK codec | Spatial geometry and ImageCXX metadata | Decoded RAM |
| ITK NIfTI, compressed NIfTI | Supported scalar and RGB(A) types | Stored samples and type, standard intensity slope/intercept, spatial geometry and conventional T/C axis order; arbitrary attributes, intensity unit labels and channel descriptions do not round-trip through NIfTI | Decoded RAM |
| DCMTK DICOM | UInt8, UInt16, Int16 grayscale; RGB8 | Patient geometry, rescale slope/intercept and top-level textual tags; UTF-8 conversion | Decoded RAM |
| Raw binary | All model types, X-fastest | Explicit import options or adjacent `.json` sidecar; byte order and header offset supported | Read-only mapping when native byte order permits; RAM otherwise |

Opening or dropping a `.raw` or `.bin` file without an adjacent `.json` sidecar shows the raw import dialog for image type, width, height, image count, header offset and byte order. Image count is an upper limit: import reads up to that many complete images after the header and ignores an incomplete trailing image. With channel/time dimensions, each Z position must contain all specified channels and frames. Advanced settings retain channel/time dimensions, spacing and memory mapping. File → Import raw binary uses the same dialog for any extension. Invalid layouts remain editable, and cancelling skips that file. Files with sidecars open directly. Confirming the dialog saves all options, including the requested image-count limit and whether Advanced is expanded, for later imports and app restarts. Cancelling preserves the last confirmed settings.

High-bit-depth color rasters fail explicitly because the model currently supports 8-bit color components. TIFF rejects planar samples, palette pixels, unsupported orientation and inconsistent pages. OME axis metadata and ImageJ contiguous stacks without one directory per page are outside the phase 1 adapter.

NIfTI exports convert spatial units m/cm/mm/um/nm to millimeters and time units s/ms/us/min to seconds. Unsupported or uncalibrated units fail explicitly. Standard NIfTI slope/intercept fields use float32 precision. Reading keeps original stored samples and applies calibration through `ImageMetadata`, avoiding ITK's default integer-to-float promotion. NIfTI color samples require identity intensity calibration because the format ignores color scaling.

DICOM imports native, RLE, JPEG and JPEG-LS encodings supported by DCMTK. Enhanced per-frame functional groups and unsupported compression fail explicitly. Single-file multiframe grayscale is read as a Z stack with shared geometry. Series import requires matching study/series/frame-of-reference identifiers, scalar encoding, calibration, orientation and spacing. It orders slices by patient coordinates and rejects duplicate positions, nonuniform spacing and shifted grids. Export writes one 2D secondary-capture image. Original stored pixel values stay separate from intensity calibration. MONOCHROME1 is retained in its metadata.

Desktop imports recursively scan folders and deduplicate canonical file paths.
Discovery reads DICOM headers without decoding pixel data and groups single-frame
scalar files by Study Instance UID and Series Instance UID. Multiframe and color
files remain separate. Each group goes through the existing series validation;
files with inconsistent geometry are rejected together rather than silently split.
Other supported images in the selection still open. Unrecognized non-image files
in folders are skipped; explicitly selected files retain their read errors.
Unreadable files report errors. Folder-discovered raw files need a JSON sidecar;
otherwise they are skipped so detached MHD/NHDR payloads are not imported twice.
Single-file DICOM reloads use the same reader fallback as ordinary imports.
Series also require matching photometric interpretation and intensity units.
DICOM batch admission budgets all decoded images plus the largest assembly buffer.
Reads serialize their memory checks and decoding, recheck available
memory before allocation, and support cancellation while waiting for admission.
Compressed reads also budget the encoded file while decoding its pixels.

DICOM export converts spatial units m/cm/mm/um/nm to millimeters and rejects uncalibrated units. It preserves intrinsic 2D rotations as a plane in 3D. Multi-file series results do not carry a single-file reload path, so Revert cannot replace a series with one slice.

A 2D slice with an embedded 3D plane retains its full origin, depth unit and direction through PNG, TIFF and DICOM, including unrotated volumes with an implicit identity direction. Scientific export rejects that slice instead of discarding its plane; save the original volume or choose TIFF/DICOM. Two-dimensional scientific images with intrinsic 2D geometry remain supported.

TIFF, PNG, JPEG, MHA, NIfTI, NRRD and DICOM publish with `QSaveFile` after encoding completes. TIFF, ITK and DCMTK encode in a temporary sibling directory before a bounded-buffer copy into the atomic destination. Errors and cancellation preserve an existing destination. Native codec calls may finish before cancellation is observed; publication checks cancellation again.

Detached MHD/NHDR export publishes a uniquely named immutable payload, then atomically publishes the header referring to it. Failed or cancelled saves remove the new payload. Previous payloads remain available for headers or backups that still reference them. Keep headers with their referenced payload files when moving or sharing datasets; remove old unreferenced payloads only after checking references.

Raw bytes and their sidecar cannot be replaced as one filesystem transaction. Raw export therefore requires both destination paths to be unused. It publishes the sidecar before the data and removes the new sidecar on a caught failure or cancellation. An interrupted process can leave a sidecar without data; it never changes an existing raw pair. Choose a new filename for a second export.

