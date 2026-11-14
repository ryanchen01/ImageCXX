# Image I/O

The adapters return project-owned `Image` buffers. Reads and writes accept cancellation and progress callbacks and must run on the scheduler rather than the UI thread. Invalid dimensions, truncated input, unsupported encodings and cancelled operations throw standard exceptions.

| Adapter | Pixel types | Metadata | Storage |
| --- | --- | --- | --- |
| Qt PNG | UInt8/UInt16 grayscale, RGB8, RGBA8 | ImageCXX text preserves axes, calibration, channels, attributes and sequence metadata | Decoded RAM |
| Qt JPEG | UInt8 grayscale, RGB8 | Lossy pixels; chunked ImageCXX text preserves metadata and is verified before publishing | Decoded RAM |
| libtiff TIFF/BigTIFF | All model types, contiguous sample layout, strips or tiles | ImageCXX description preserves full metadata and shape; ImageJ descriptions import C/Z/T ordering and calibration; ordinary TIFF resolution is imported | Decoded RAM |
| ITK MHA/MHD, NRRD/NHDR | All model types supported by the corresponding ITK codec | Spatial geometry and ImageCXX metadata | Decoded RAM |
| ITK NIfTI, compressed NIfTI | Supported scalar and RGB(A) types | Stored samples and type, standard intensity slope/intercept, spatial geometry and conventional T/C axis order; an ImageCXX comment extension retains full metadata | Decoded RAM |
| DCMTK DICOM | UInt8, UInt16, Int16 grayscale; RGB8 | Patient geometry, rescale slope/intercept, textual tags and nested sequences; UTF-8 conversion | Decoded RAM |
| Raw binary | All model types, X-fastest | Explicit import options or adjacent `.json` sidecar; byte order and header offset supported | Read-only mapping when native byte order permits; RAM otherwise |

## Importing folders and raw files

- **Folder imports:** Folders are scanned recursively and deduplicated. Selecting an `.mhd` header and its payload together opens only the header. Unrecognized files in folders are skipped, while explicitly selected unsupported files report an error.
- **Raw binary files:** Opening `.raw` or `.bin` files (or using **File > Import raw binary...**) opens an import dialog for pixel type, dimensions, image count, header offset, and byte order unless an adjacent `.json` sidecar is present. Folder scans skip raw files without a `.json` sidecar so detached payloads are not imported twice.
- **DICOM series:** Folder and multi-file imports group single-frame scalar DICOM files by Study Instance UID and Series Instance UID, ordering slices by patient coordinates. Series with mismatched geometry, spacing, calibration, or units are rejected together rather than silently split.

## Format behavior and limits

- **TIFF and rasters:** Color rasters currently support 8-bit components. TIFF rejects planar samples, palette images, unsupported orientations, and inconsistent pages. Four-sample TIFFs must specify associated or unassociated alpha (converted to straight RGBA on import, exported as unassociated alpha).
- **NRRD and NIfTI:** Native NRRD imports preserve matching spatial unit labels and normalize mixed units to millimeters. NIfTI exports convert supported spatial units to millimeters and time units to seconds; reading retains stored sample types and applies intensity slope/intercept via `ImageMetadata`.
- **DICOM:** Supports native, RLE, JPEG, and JPEG-LS encodings via DCMTK (enhanced per-frame functional groups are rejected). Single-file multiframe grayscale opens as a Z stack; export writes a 2D secondary-capture image and preserves full ImageCXX metadata in a private group `7777` `IMAGECXX_METADATA` element.
- **2D slices from 3D volumes:** A 2D slice with an embedded 3D plane keeps its full origin, depth unit, and direction in PNG, TIFF, and DICOM. Scientific exports (MHA/NRRD/NIfTI) reject single slices with embedded 3D planes instead of discarding geometry.

## Saving and metadata privacy

- **Include full metadata (off by default):** Default saves and batch exports keep pixels and numeric calibration while stripping names, arbitrary attributes, DICOM tags/sequences, and source snapshots. Check **Include full metadata** in the Save dialog when a full metadata round trip is needed.
- **Source snapshots:** Normalizing metadata preserves the original source snapshot in `ImageMetadata::originalMetadata` across saves without nesting history.
- **Safe writes:** Most formats encode to a temporary file and publish atomically via `QSaveFile`, leaving existing files untouched on error or cancellation. Detached MHD/NHDR exports write a uniquely named immutable payload before updating the header; raw exports require unused destination paths for both the sidecar and data file.

## Image Properties panel

- **Tag display:** Shows native NIfTI header fields with readable labels or DICOM tags with numeric IDs (for example, `(0010,0010) Patient Name`) and collapsible sequences.
- **Search and copy:** Search matches names, tags, and values regardless of spaces or case, keeping matching sequences visible. Select multiple rows with `Shift` or `Ctrl`/`Cmd` and copy as CSV (`Ctrl`/`Cmd+C`) or use the context menu to copy properties or values.

