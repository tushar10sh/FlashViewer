# FlashViewer

**GPU-accelerated satellite image viewer for large geospatial rasters**

![CI Linux](https://github.com/san-aria/FlashViewer/actions/workflows/ci-linux.yml/badge.svg?branch=main)
![CI macOS](https://github.com/san-aria/FlashViewer/actions/workflows/ci-macos.yml/badge.svg?branch=main)
![CI Windows](https://github.com/san-aria/FlashViewer/actions/workflows/ci-windows.yml/badge.svg?branch=main)
![CI Linux (conda)](https://github.com/san-aria/FlashViewer/actions/workflows/ci-linux-conda.yml/badge.svg?branch=main)
![CI macOS (conda)](https://github.com/san-aria/FlashViewer/actions/workflows/ci-macos-conda.yml/badge.svg?branch=main)
![Version](https://img.shields.io/badge/version-0.2.0-blue)

## Overview

FlashViewer is an open-source desktop application for navigating and analyzing large satellite and aerial imagery. It renders multi-gigabyte rasters at interactive frame rates using a tile-based LOD system on the GPU, so scientists and analysts can pan and zoom without waiting for data to load.

The application integrates display, band compositing, colormap control, spectral analysis, raster math, and geo-operations in a single Qt6 window. It runs on Linux, macOS, and Windows and opens files via drag-and-drop, a file chooser, or a remote URL — including Cloud-Optimized GeoTIFFs streamed directly from S3 or HTTP.

## Features

### Visualization and Rendering

- OpenGL 4.1 Core Profile tile renderer; 256×256 tile grid with asynchronous decode on a worker thread pool (`max(2, CPU_cores − 1)` threads)
- Orthographic camera with mouse-drag pan, scroll-wheel zoom, and Space to fit all layers
- RGB composite mode (select any three bands as R/G/B) and single-band pseudocolor mode
- MSAA 4× antialiasing; 24-bit depth buffer; all bands stored as `GL_R32F` textures
- No-data handling: a per-layer no-data value/color is honored, and non-finite (NaN/±Inf) pixels are always rendered transparent — even when the file declares no no-data value; interpolation never bleeds no-data into valid pixels at data boundaries (no spurious edge line)
- Selectable display resampling (**View → Display Resampling**): bilinear, bicubic (smooth, B-spline), and bicubic (sharp, Catmull-Rom) — all no-data-aware, **seamless across tiles** (no block boundaries), and reverting to crisp nearest pixels at native-or-greater magnification; the choice is per-layer and persisted as the default for new layers
- Progressive refresh while tiles stream in, with a bounded 30 s repaint budget so a stalled load never repaints indefinitely
- The camera auto-fits the first layer added; subsequent layers preserve the current view (`Space` re-fits all layers on demand)
- Drag-and-drop file loading onto the main window
- Screenshot export (`Ctrl+Shift+S`): captures the map without pane chrome (ID/gear) or border; in a multi-pane layout you choose **Active pane** or **Entire pane layout** (each region's front pane composited at its on-screen position, empty regions filled with the theme canvas background). Hidden colorbars/scale bars are excluded.
- Dockable panels with a **View → Panels** menu to show/hide each panel and re-open a closed one at its original default location (the two plots are separate windows and are opened from **Tools** instead) (plus **Reset Panel Layout** to restore the whole default arrangement); panels support nested docking (drop into the upper half, lower half, or full of a side area), and the title-bar float/close buttons use themed vector icons

### Supported File Formats

| Format | Extensions |
|---|---|
| GeoTIFF / Cloud-Optimized GeoTIFF (COG) | `.tif` `.tiff` `.geotiff` |
| NetCDF | `.nc` |
| HDF5 | `.hdf` `.hdf5` `.h5` |
| ENVI Image | `.img` |
| Binary Raw (BSQ / BIL / BIP) | `.bin` `.raw` `.dat` |
| GDAL Virtual Dataset | `.vrt` |
| JPEG 2000 | `.jp2` |
| Remote / Cloud (URL) | `http://` `https://` `/vsicurl/` `/vsis3/` `/vsigs/` |

Binary Raw import dialog lets the user specify lines, samples, bands, data type (uint8 / int16 / uint16 / int32 / uint32 / float32 / float64), interleave (BSQ / BIL / BIP), header offset, and byte order. A hex preview of the first 256 bytes is shown as a reference. COG remote reads use a GDAL VSI curl cache of 128 MB with a 512 KB chunk size.

Multi-variable files (NetCDF, HDF5) open through a **Select Variables** dialog listing every subdataset, with a **Load as** choice:

- **Separate layers (one per variable)** — the default: one layer per selected variable, each with its own band/colormap/stretch controls and a variable drop-down for switching subdataset in place. Switching takes a deliberate act: scrolling the Layers panel with the pointer over that drop-down scrolls the list, and the wheel changes the variable only after you click the drop-down.
- **One multi-band layer** — available once two or more variables are selected: the picked variables are stacked into a single layer where band *k* is variable *k*, so three or more variables arrive ready to view as an **RGB composite**. Bands are named after their variables throughout the UI. Combining requires the variables to share raster size, grid, and CRS; when they do not, FlashViewer says why in a non-blocking notice and loads them as separate layers instead. The stack is a managed temporary `.vrt` that is deleted when the layer is removed.

### Georeferencing an Unreferenced Raster

Any raster that opens **without a spatial reference** — an unset geotransform, a missing CRS, or
both — is offered an **Assign Coordinates** dialog, with Skip always available. This is not limited
to NetCDF/HDF5: a binary-raw import and a GeoTIFF written without a projection get it too.

- **Coordinate arrays** may be taken from variables of the file being opened *or* from a **separate
  file** (each of X and Y has its own Browse button; a multi-variable file prompts for which
  variable). Their shape decides how they are used:
  - **1-D axes** (`lon[width]`, `lat[height]`) become an affine geotransform, fitted from the median
    step with the origin back-projected from the first valid sample.
  - **2-D geolocation arrays** (`lon[h][w]`, `lat[h][w]` — a satellite swath, one coordinate per
    pixel) are **not** affine, so the variable is *projected onto a regular grid* through them. The
    result is a lazy warped VRT, so a large swath opens immediately; its intermediate files are
    managed temporaries removed with the layer. Switching the layer's variable re-applies the
    projection rather than dropping back to an unreferenced grid.
- **Fill and out-of-range coordinates are masked before use.** Non-finite samples and the declared
  `_FillValue` are dropped; for a geographic CRS latitude is bounded by ±90 and longitude by ±360,
  and longitudes in the 0…360 convention are normalised to −180…180 rather than rejected (a
  projected CRS gets no such bound). An **amber strip** in the dialog reports how many samples were
  masked, and an assignment that cannot produce a sensible grid is refused with a reason instead of
  placing the layer at a meaningless extent.
- **The CRS** may come from a variable in the file, a typed EPSG / WKT / PROJ string, or the CRS
  picker behind *Choose…*. Assigning only a CRS leaves a valid existing grid untouched.

### Band and Colormap Control

- Band Selector widget: choose R/G/B band indices for composite mode or a single band for pseudocolor mode; the colormap appears only in Gray mode (hosted on the Gray page of the band selector). Switching RGB↔Gray keeps a constant panel geometry (no reflow), and the controls keep a fixed size and full width whether the dock spans half or full of the panel (scrolling when the dock is short rather than squeezing the elements)
- Bands that carry a name in the file are listed by it — `Band 2 — u10` rather than `Band 2` — in the band drop-downs, and the Layer Info panel's Bands row reads e.g. `3 (t2m, u10, v10)`. This is what keeps the variables of a combined multi-band NetCDF/HDF layer identifiable, and it applies to any raster with band descriptions
- Re-pointing a channel at another band brings that band's own contrast with it: the 1/99 stretch is re-derived for the channels that moved (the histogram handles land on the new band's range instead of the previous band's), while a stretch you set by hand on a channel that did not move is preserved. The change appears in one step — no intermediate frame showing the previous band's pixels through the new range
- 6 built-in colormaps: **gray**, **viridis**, **jet**, **hot**, **RdYlGn**, **plasma**
- All colormaps are 256-entry RGBA8 LUTs uploaded as `GL_TEXTURE_1D` with `GL_LINEAR` filtering
- Histogram Panel: histogram binned over the full data range, with the view zoomed by default to the Auto-Stretch (1/99) window; draggable min/max stretch handles (the x-window stays fixed while dragging — the spin boxes update live and the view re-settles on release); clip by **value** or by **percentile (0–100 %)** via a mode switch (the two stay in sync, and only the active pair is shown for a compact panel); spin boxes with up/down arrows for both; and an Auto Stretch button that fills the value fields with the 1/99-percentile values and the percentile fields with 1/99
- In RGB composite mode the Histogram Panel splits into **three per-channel histograms** (R, G, B) with bins coloured red/green/blue (tuned to read with equal visual weight), each with its own value/percentile clip and Auto-Stretch; every channel stretches its band independently in the rendered composite. The panel is scrollable, and the tabbed Layer-Properties/Histogram group defaults to about half the left column's height so the three histograms aren't squished
- Colormap Legend overlay: draggable gradient strip with 5 labeled ticks, vertical or horizontal orientation (re-anchors to its corner on orientation change); hidden automatically for RGB composite layers. The colorbar represents the pane's **topmost visible layer** (visible and opacity > 0), not the active layer, so hiding/zeroing the top layer reveals the next one's colorbar and an all-hidden pane shows none. Each pane has a **"Show Colorbar"** gear-menu toggle (next to "Show Scale Bar"), plus a per-layer right-click "Show Colorbar". Setting a layer's opacity to 0 also unchecks its visibility

### Multi-Pane and Sync

- Add panes at any time via **View → New Pane** (`Ctrl+Shift+N`), which asks for the pane's name and its **position**. The name is pre-filled with the next free default (`Pane N` — closing *Pane 2* frees that name again, so numbering does not creep upward); the last pane is always kept. A new pane is **independent** (not auto-synced), and each pane has a top-left **ID label + gear menu** (Close / Edit ID / Sync With / Color) and a focus border when active
- The **position picker** is a snap-layout graphic in the style of the Windows 11 snap flyout: the four layout presets drawn as miniature layouts whose individual cells you click. Because a cell names a **layout mode** as well as a region, picking one the current layout doesn't have — a quadrant while the window is split in halves — **switches the layout** to Quarter and drops the new pane in exactly that quadrant. Each cell previews where the existing panes will land as pane-coloured dots, a summary line spells out the consequence before you commit, and arrow keys drive it as well as the mouse
- A single, app-wide layer list drives every pane: **each layer belongs to exactly one pane**, and a pane shows only its own layers. Newly opened rasters appear in the **active pane** (the one last clicked); re-assign a layer by dragging it from the Layers panel onto a pane, onto another pane's **group** in the Layers panel, onto a region's **stacking pill** (works even when that pane is stacked behind the displayed one), or via the **"To Pane"** context submenu; dropping a layer on an empty layout region spawns a pane there
- Selecting a layer marks its pane active but **never re-stacks the region** — so in Full-Window mode you can drag a layer from the pane in front onto a pane behind it without the target disappearing. **Double-click** a layer row or a pane group header (or use *Show This Pane*) to deliberately bring that pane to the front
- **Per-pane Project CRS on move**: each pane has its own Project CRS (defaults to its bottom-most layer's CRS, or an explicit override). Moving a layer into an **empty** pane makes that pane **adopt the layer's CRS**; moving it into a **populated** pane keeps that pane's CRS and reprojects the layer on the fly. If a layer can't be reprojected into the pane's CRS it is **shown in its own native CRS** (it may not align) with a brief, non-modal notice — never a raw GDAL/PROJ error
- **Layout modes** (**View → Pane Layout**): Full, Half (side-by-side or top/bottom), and Quarter (2×2). Regions hold a **stack** of panes (a pill strip picks the shown one); place a pane by dragging its ID label onto a region
- **Per-pane colour-coding**: each pane has a distinct, theme-aware colour that tints its border/ID and, in the Layers panel, its rows' background band, visibility ticks and opacity sliders (row text stays the theme's normal colour — the band carries the pane, and bold marks a pane header or the active layer). A new pane takes the first colour no pane on the canvas is currently using, so closing a pane frees its colour rather than shifting the sequence — two live panes never share one; the **active layer's name is shown in bold** (theme-compliant) in every pane
- **The Layers panel is grouped by pane**: one collapsible, pane-coloured group header per pane, listing that pane's layers in draw order. Every pane gets a header — **including empty ones**, so an empty pane stays visible as a drop target and can be closed from the panel
- **Sync With** (unified master/slave): the gear menu links panes into a group — the invoker becomes the **master** (★), the others **slaves** (mirror icon); linked panes match the master's view and stay linked for pan/zoom, and a shared **ghost cursor** shows the pointer's geographic position in every synced pane. Un-sync (or closing a synced pane) dissolves the group; non-synced panes keep independent cameras
- **Sync indication, colour-coded to the master**: the ★ / mirror badge is repeated wherever a pane is named — the pane chrome, each region's **stacking pill** (so a pane stacked behind another still declares its sync state) and each **pane group header in the Layers panel**, at the far right of the header band — and every badge in a group, the master's ★ included, is painted in the **master's pane colour**. Shape says *which role*, colour says *whose group*; a tooltip names the master in words ("Synced to «Pane 1»"), so the colour is never the only channel. Recolouring or renaming the master updates every badge live
- **Per-pane pixel inspect**: in inspect mode, left-click reports the pane's representative/active raster, right-click all its visible rasters; results are grouped under a **collapsible drop-down per pane** (header coloured by the pane), and a shared **red highlight square** marks the sampled pixel in every synced pane. The same selection also feeds the **Spectral Plot** window, so the numbers in the table and the curves on the chart can never describe different layers

### GIS Overlays

- Scale bar (QPainter overlay): auto-scales to a readable distance and performs degree→km conversion (111.32 km/°) for geographic CRS; each pane's scale bar can be shown/hidden from that pane's gear menu ("Show Scale Bar")
- Coordinate display in the status bar: live lat/lon at the cursor (6 decimal places)
- Zoom level in the status bar: metres-per-pixel (auto-switching to km/px when large; geographic CRSs use the same 111.32 km/° approximation as the scale bar), tracking the active pane and updating live on wheel/keyboard zoom, fit, and sync
- CRS name in the status bar: extracted from the active layer's WKT
- Pixel inspector: `Ctrl+click` reads and displays all band values at the clicked location

### Raster Math

- Expression editor backed by muParser
- Per-band variables named `L<layer>B<band>` (e.g. `L1B1`, `L2B3`); an expandable layer→band tree lets you click a band to insert its token
- Live validation with error highlighting; quick-insert buttons for `abs`, `sqrt`, `exp`, `log`, `sin`, `cos`, `min`, `max`
- Works across layers in **different panes**; an **Input CRS handling** selector reprojects inputs to the output CRS (default) or aligns by pixel — input warping is skipped when all inputs share a CRS — with a live **Reference CRS** readout (the first referenced layer) and a mismatch warning
- **Output CRS** (distinct loaded CRS) and **Output pane** are chosen **independently**; the Output pane defaults to **New Pane** (created on demand). A warning appears if an existing pane's CRS differs from the Output CRS (on-the-fly display reprojection is deferred)
- Independent **input/output resampling** (nearest / bilinear / bicubic / lanczos) and optional no-data masking (result is NaN where any input is no-data)
- A live **result preview** (`Result → <pane> · <CRS>`); tile-by-tile evaluation writes a new GeoTIFF in the output CRS
- A single **Run** handles both cases: a **temporary output** (default — the file is auto-deleted when its result layer is removed from the Layers Panel) or a **permanent GeoTIFF path** (uncheck "temporary output" and browse). (Consolidates the former separate *Run* and *Run and Save As*.)
- Example: `(L1B5 - L1B4) / (L1B5 + L1B4)` produces an NDVI layer

### Analysis and Plots

- **Spectral Plot** (`Tools → Spectral Plot`, `S`): a **separate window** that stays above the main window without covering other applications, and reopens at the size and position you left it. It plots the multi-band spectrum of pixels picked in **Inspect mode**, and it holds one plot per *inspect selection* rather than one curve per click:
  - The panel records **only while it is open**, and **closing it discards the plots** — they are a live working set, not a document. (The colour scheme is a preference and is remembered.)
  - **Left-click** plots each target pane's topmost/active layer; **right-click** plots every visible raster — the same rule the Pixel Inspector follows, so the curves and the table always describe the same selection
  - When panes are **synced**, the click covers the whole sync group and the spectra are **merged into one plot**, titled with the panes it spans
  - **Activating a layer** in the Layers Panel brings up that layer's plot; every member of a merged plot brings up the merge. A layer never inspected shows a blank chart titled with its name
  - A click **replaces** the curves by default; tick **Persist curves** to add them instead. Persist works **across selections too**: a pixel from one pane and a pixel from another, *unsynced*, pane end up on one chart, and activating either layer brings the comparison back. **Clear** discards the plot on screen
  - No-data bands are drawn as gaps
- **Legend**: vertical, to the right of the plot, one entry per curve with **wrapping** text — the file name on one line, the coordinates on the next, and a name too long for the column wrapped *inside* the name rather than cut off. Drag the **divider** between plot and legend to give the labels as much room as you want, up to half the window; the position is remembered per plot window. Each key shows the curve's **colour and its line style**, which is what separates two layers of the same pane. It scrolls when there are more curves than fit, and it is included in the saved PNG/SVG.
- **Editing labels** — the ✎ in the toolbar opens one dialog for everything the plot labels, in three collapsible sections:
  - **Titles**: the plot title and both axis titles. Clear a field to restore the automatic text, which is shown as grey placeholder while you edit
  - **Legend**: one row per curve — a **Show** box that hides the entry while leaving the curve drawn, the label itself, and **Delete**, which removes the curve
  - **Axis ticks**: **start**, **end** and **step** per axis, replacing the automatic fit. Leave them empty to fit automatically. For a descending axis put the higher value in start and give a **negative** step; a step whose sign disagrees with the range is refused rather than reinterpreted. A hand-set range becomes what the ⌂ button returns to
  - Everything applies on **OK** and is abandoned on **Cancel**, deletions included. A legend rename is remembered **per curve identity** — the layer in the Spectral Plot, the layer and the statistic in the Profile — so it survives the next click or Compute, and replaces only the layer name: the coordinates (or the mode and statistic) stay. Edited labels appear in the PNG, SVG and CSV exports.
- **Grid** and **Coords** toggles sit in the toolbar. Grid draws or hides the grid lines; **Coords** (Spectral Plot only) drops the sampled coordinates from every legend entry at once. Both are remembered between sessions.
- **Which pane is this curve from?** A curve's **hue is its pane's colour** — the same swatch as the pane pill and the Layers-panel band. A left-click, which plots one curve per pane, is drawn in that colour exactly; further curves of the same pane keep the hue and step through lightness, each with its own line style (solid / dashed / dotted / dash-dot), so layers within a pane stay distinguishable without relying on colour alone. Hue never varies within a pane — the pane palette's own hues are as little as 14° apart, so a visible hue spread would read as a *different* pane. If you would rather have maximum separation than pane meaning, either plot's scheme selector switches to the **original fixed 10-hue palette**; both panels share the one setting and it is remembered between sessions.
- **Navigating and saving a plot** (both the Spectral Plot and the Scan/Pixel Profile carry the same row):
  - Zoom with the ⊕ / ⊖ buttons or the mouse wheel; **left-drag anywhere in the plot to pan** — there is no pan mode to switch into; the ⌂ button returns to the auto-fitted range and is enabled only while you have moved away from it
  - **Save** browses to a location and writes **PNG** or **SVG** (the plot exactly as displayed — zoom, pan, legend and theme) or **CSV** (the plotted numbers: one row per band and one column per curve for the Spectral Plot, one row per row/column index for the Profile; no-data cells are left empty)
- **Scan/Pixel Profile** (`Tools → Scan/Pixel Profile`, `P`): a **separate window** on the same terms as the Spectral Plot. Computes aggregate statistics across scan rows or pixel columns, with each curve drawn in its layer's pane colour. It keeps one plot per *compute selection*, on the same terms as the Spectral Plot:
  - **Compute** profiles the active layer; when that layer's pane is **synced**, it profiles every visible raster across the whole sync group and **merges them into one plot**, titled with the panes it spans
  - **Activating a layer** in the Layers Panel brings up that layer's profile; every member of a merged plot brings up the merge. A layer never profiled shows a blank chart titled with its name
  - A Compute **replaces** the plot by default; tick **Persist curves** to add to it instead, so profiles of several layers — or a Mean and a Median of one layer — sit on one chart. **Clear** discards the plot on screen
  - Like the Spectral Plot, the panel records **only while it is open**: closing it discards the profiles and any label edits
  - **Mask No-Data** (on by default) excludes each layer's own no-data value and any non-finite samples from the statistics, so a scene padded with `-9999` is described by its data rather than its padding. A row or column left with nothing valid is drawn as a gap and exported as an empty cell — never as zero. The toggle sets the rule the **next Compute** uses; it never re-reads a raster on its own
  - Modes: Scan (row-wise) / Pixel (column-wise)
  - Statistics: Mean, Median, Standard Deviation, Quantile (configurable *p* in the range 0.0–1.0)
- **Layer Info Panel**: file path, CRS, dimensions, band count, no-data value, and per-band statistics — long values (full path, full CRS WKT) word-wrap within the panel

### OpenStreetMap Basemap

- Toggleable XYZ tile basemap rendered beneath all raster layers (which composite over it bottom-to-top); **off by default** on every launch
- Reprojected into the pane's project CRS so it co-registers **under projected (e.g. UTM) layers**, not only geographic ones — pixel-accurate via per-tile tessellation (each vertex individually transformed), re-warped when the pane's CRS changes
- Opens on a **world view centred on the prime meridian** and returns to it when all layers are cleared, when enabled with no layers loaded, or on **Fit** (`Space`) with nothing loaded — so the basemap never gets stranded off-screen and Fit is always the way back
- Standard `{z}/{x}/{y}` URL template; tile source is configurable
- 512 MB local disk cache (`~/.cache/FlashViewer/osm/`) via `QNetworkDiskCache`
- **Repeats in longitude**: an XYZ grid covers ±180° exactly once, so the world is drawn as copies of the grid, one per 360° in view, joined seamlessly at the antimeridian. Panning into a copy costs draw calls only — never extra downloads
- **Sub-pixel latitude placement at every zoom**: a Web Mercator tile's texture rows are evenly spaced in Mercator *y*, not in latitude, so each tile is subdivided into rows taken from the exact inverse Mercator (row count derived from the error, decaying to a single quad where one is already accurate). Basemap coastlines line up with a global lon/lat raster's land mask **at world zoom**, not only zoomed in

### GDAL Operations

- **Merge**: mosaics selected layers to an LZW-compressed GeoTIFF. Inputs in **different CRS** are supported — choose an **Output CRS** and *Reproject to output CRS* (default, a GDAL multi-source warp mosaic) or *Pixel-align* for already co-registered inputs; a note warns when the selection spans multiple CRS
- **Warp / Reproject**: reprojects to any EPSG code, GeoTIFF output, with a **selectable resampling method** (nearest / bilinear / bicubic / lanczos) that **defaults by data character** — nearest for integer/categorical rasters, bilinear for continuous (float) — so class values are preserved by default. No-data is excluded from the resampling kernel, so cubic/lanczos never smear no-data into valid pixels at swath edges
- **No-data**: both tabs take an optional **input no-data** (an extra source value to discard, beyond NaN/±Inf) and **output no-data** (stamped into the result GeoTIFF so it is auto-loaded on open)
- **Output handling**: "Add result to a pane" (with an Output-pane picker incl. "New Pane") and "Use a temporary output file" — a temporary result is **auto-deleted when its layer is removed** from the Layers Panel; uncheck it to keep a permanent file
- Both operations run asynchronously with a real GDAL progress bar

### Errors, Logging and Security

- **Central error reporting**: GDAL errors and caught exceptions are funnelled
  through a single `ErrorReporter` into the in-app log and a **dismissable,
  non-fatal status banner** — the app reports and stays stable instead of crashing
- **Log panel**: leveled, colour-coded entries with an **Export Logs…** button
  (saves the log to a `.txt`/`.log` of your choice) and a **Clear** button
- **Application log file**: a size-bounded **rotating log** is written to the
  per-user app-data directory (`%APPDATA%/FlashViewer/logs/flashviewer.log` on
  Windows; `~/.local/share/FlashViewer/logs/` on Linux;
  `~/Library/Application Support/FlashViewer/logs/` on macOS)
- **Remote-URL security (SSRF hardening)**: user-supplied URLs pass a `UrlGuard`
  check before any fetch — only `http`/`https`/`/vsicurl/`/`/vsis3/`/`/vsigs/`
  schemes are allowed, and hosts resolving to loopback/link-local/private/cloud-
  metadata ranges (e.g. `169.254.169.254`, `10/8`, `127/8`) are blocked. TLS
  certificate verification is always on; HTTP redirects, retries and timeouts are
  bounded (`GDAL_HTTP_*`), and decoder memory is capped (`GDAL_CACHEMAX`)
- **GPU-context resilience**: on OpenGL context loss the canvas rebuilds its GL
  resources rather than terminating

### GPU Monitor

- A dockable **GPU Monitor** panel (View → Panels) shows a live sparkline and
  readout of **estimated GPU memory** held by resident raster tiles as they are
  uploaded and evicted, summed across panes, alongside the GPU renderer/vendor/GL
  version. The figure is an estimate (bands × width × height × 4 bytes), not a
  driver query, and the same accounting bounds VRAM use under pressure

### Performance HUD

- An optional on-canvas **Performance HUD** (**View → Performance HUD**, off by
  default, remembered across sessions) overlays live performance readouts — frame
  rate and frame-time percentiles (avg/p99/max), the last file-open first-tiles
  latency, the main-thread stall count, and this pane's estimated VRAM — making the
  performance KPIs observable at a glance; a frame over the 100 ms budget is flagged.
  A build compiled with `-DFV_PERF_INSTRUMENT=ON` additionally logs verbose
  per-operation timings for benchmarking

## Keyboard Shortcuts

| Shortcut | Action |
|---|---|
| `Ctrl+O` | Open raster file |
| `Ctrl+U` | Open remote URL / COG |
| `Space` | Fit camera to all loaded layers |
| `Ctrl+M` | Raster Math expression editor |
| `S` | Show the Spectral Plot panel |
| `P` | Show the Scan/Pixel Profile panel |
| `Ctrl+Shift+N` | Add new pane |
| `Ctrl+Q` | Quit |
| `Ctrl+click` | Pixel inspect (all bands at clicked location) |
| `Del` | Remove the layers/panes selected in the Layers panel |
| `F2` | Rename the selected layer in the Layers panel |

## Architecture

### Rendering Pipeline

```
Disk
  → GDALDataset::RasterIO  (worker thread; one mutex per dataset)
  → float32 tile buffer    (256×256 pixels, N bands)
  → GL_R32F Texture2D      (one texture per band per tile)
  → tile.vert              (geo coordinates → NDC via orthographic view-proj)
  → tile_rgb.frag          (sample 3 band textures; per-band linear stretch → RGB)
    OR tile_gray.frag      (sample 1 band texture; stretch to [0,1]; index GL_TEXTURE_1D colormap LUT)
  → Framebuffer            (MSAA 4×)
```

LOD zoom level: `floor(log2(native_resolution / screen_resolution_at_scale))`

### Layer Model

Every data source — raw bands, raster math result, imported raster — is a `RasterLayer`. `LayerManager` owns an ordered list and emits Qt signals (`layerAdded`, `layerRemoved`, `layerChanged`) that drive the UI, renderer, histogram, and spectral plotter.

The Layers panel renders that one flat list **grouped by pane** — a collapsible header per pane over its layer rows. Each row has a visibility toggle (an outlined box with a **tick in the row's pane colour**) and an opacity slider. Selection is the removal set — there are no separate delete checkboxes: **Shift/Ctrl-click** any mix of layers and pane group headers, and selecting a pane also highlights every layer under it in that pane's colour. A **Delete** button (or `Del`) removes the whole selection; if that would empty a pane, you are asked whether to **keep the empty pane or close it too**. The last pane is never removed — its layers are cleared instead. While more than one layer is selected the single-subject panels (Band, Colormap, No-data, Histogram, Layer Info) show their empty state. The ▲/▼ buttons reorder a layer against its **pane siblings**, and dragging a row into another pane's group re-assigns it to that pane. A selection only changes when you change it: it survives the panel rebuilding itself (a pane rename, recolour or sync change), survives releasing the button away from the row you pressed, and is untouched by simply moving the pointer over the list — while clicking the empty background still deselects, as usual. Hovering a selected row keeps its pane-coloured band; only unselected rows take the theme's hover highlight.

### Threading Model

The UI thread never blocks. GDAL I/O and tile decode run on a fixed `ThreadPool`. Heavy operations (warp, merge) run on a dedicated `GdalWorker` (`QThread`) and report progress via Qt signals. Network tile fetches for the OSM basemap are dispatched through Qt's non-blocking `QNetworkAccessManager`.

### Colormap System

Each colormap is a `std::array<glm::u8vec4, 256>` loaded from a built-in definition at startup. It is uploaded once as a `GL_TEXTURE_1D` (`GL_RGBA8`, 256 texels, `GL_LINEAR`). The fragment shader samples it: `texture(u_colormap, stretch(v))` → RGBA.

### Tile Cache

LRU cache with a default capacity of 512 tiles. Cache key: `TileKey = {layer_id, zoom, tx, ty}`. Each entry holds a GL texture handle. Evicted entries release their GPU memory immediately. Eviction is bounded by both the tile count **and** a byte budget (`residentBytes()`, GL_R32F = bands × width × height × 4); the same estimate feeds the GPU Monitor panel and degrades gracefully under `GL_OUT_OF_MEMORY`.

## Version Releases

| Version | Date | Notes |
|---|---|---|
| 0.2.0 | *in development* | Post-0.1.0 manual-test findings (Phases 15–19: opacity & colorbar, per-pane CRS on layer move, Layer Panel overhaul with pane grouping + multi-select, NetCDF/HDF multi-band variable stack), then: the product version single-sourced from the root `VERSION` file; one shared implementation for the tick checkbox and section-heading idioms across every panel and dialog; the basemap repeating correctly in longitude and placed to sub-pixel accuracy in latitude; a world view on open and on Fit; the **New Pane position picker**; and **colour-coded sync badges** shown wherever a pane is named (pane chrome, region pills, Layers-panel pane headers), each drawn in the sync master's colour; and **coordinate-array georeferencing** for rasters that carry no spatial reference — 2-D geolocation arrays (satellite swaths) are now *projected* onto a regular grid instead of being mistaken for 1-D axes, fill and out-of-range coordinates are masked before use with the count reported to the user, and the assignment dialog is offered for **any** unreferenced format (binary raw included), accepting coordinate arrays from a separate file and a projection chosen through the CRS picker. plus a round of Layers-panel fixes — a selection that no longer disappears on a rebuild, a release away from the row, or a hover (including over a variable drop-down), a wheel that scrolls the panel instead of switching variables, and a band change that re-derives its own stretch without a flicker; and the two **plot windows** (Spectral Plot and Scan/Pixel Profile) keyed by inspect/compute scope, with a resizable wrapping legend, one dialog for every label, hand-set axis ticks, and Grid/Coords toggles — each discarding its plots when closed, opening on nothing until Compute is pressed, mirroring an inspect click into each synced pane’s own Project CRS, and profiling **every layer selected in the Layers panel** in one press — across panes, with no need to sync them. **174 automated tests passing** on Linux and Windows. Accessibility, i18n, packaging/installers and the license audit remain outstanding (SRS Appendix E). |
| 0.1.0 | 2026-07-22 | **Released base.** Implementation revamp Phases 0–12 (test infrastructure, rendering/resampling, multi-pane + sync, on-the-fly per-pane CRS reprojection, GDAL ops, error handling & SSRF guard, performance instrumentation + HUD) closed by the Phase-14 conformance sync; Linux, macOS, and Windows CI; **113 automated tests passing**. Accessibility, i18n, packaging/installers, and the license audit are deferred to 0.2.0 (SRS Appendix E). |

## Building

FlashViewer builds on Linux, macOS, and Windows. Two provisioning options:

- **Native (default):** system packages — `apt`/`dnf` on Linux, Homebrew on macOS,
  and conda-forge + `aqtinstall` on Windows.
- **conda (optional):** a self-contained conda-forge environment (deps, Qt **and**
  the C++ toolchain) via `environment-linux.yml` / `environment-macos.yml` /
  `environment-windows.yml` — no system `-dev` packages or `sudo` required.
  NetCDF/HDF5 support comes from the `libgdal-netcdf` / `libgdal-hdf5` driver
  plugins listed in those env files.
  > ⚠️ **Linux GCC ABI:** conda-forge binaries are built against **GCC 12**. Build
  > with the **bundled conda toolchain** (activate the env; `CC`/`CXX` point at the
  > conda GCC) or a **system GCC ≥ 12** — mixing conda-forge libs with an older
  > system GCC causes `GLIBCXX`/libstdc++ ABI errors. See [INSTALL.md](INSTALL.md) §2.6.

See [INSTALL.md](INSTALL.md) for full instructions.

## License

FlashViewer is released under the [MIT License](LICENSE). The product license and
the licenses of all bundled third-party components are viewable in-app via
**Help → Licenses** and listed in [`THIRD_PARTY_LICENSES.md`](../resources/THIRD_PARTY_LICENSES.md).
