# Live Arrow Flight Georeferencing — Architecture & Status

A dual-repo feature: trims-georef streams georeferenced tiles to
FlashViewer over Apache Arrow Flight, live, with a control panel to adjust
RPY attitude bias/rate and `GeoreferencerConfig` knobs (stride,
cell-seeding method, device, float precision, resample mode) and see the
raster update as the server recomputes.

Branches: `feature/live-arrow-flight` (trims-georef), `feature/arrow-flight-live-georef`
(this repo). Both sides are implemented, built, and tested — this doc
records the final architecture and the known rough edges, not a
to-do list (see "History" at the bottom for how it got here).

## Status: implemented and tested end-to-end

**trims-georef (Python)** — full test suite passing (869 passed, 10
skipped GPU-only), plus live-specific tests in `trims/tests/test_live_arrow.py`
and `trims/tests/test_streaming.py` covering a real in-process
`TrimsFlightServer` + `pyarrow.flight.FlightClient` `DoExchange` round
trip, cooperative cancellation, and streaming-vs-sequential output
comparison:
- `trims/live/arrow_tile_writer.py` — `ArrowTileWriter`: row-strip
  RecordBatch schema (one Arrow row = one output scanline,
  `FixedSizeList<T>[W]`/`List<T>` columns per band/line_src/sample_src/lat/lon/coverage),
  supporting both float32 and uint16 band dtypes.
- `trims/live/flight_server.py` — `TrimsFlightServer.do_exchange`: fully
  duplex. A `reader_worker` thread reads `config_update`/`request_extent`
  messages concurrently with a `compute_worker` thread that runs (or
  re-runs, on generation bump) `TileProcessor.run_streaming`, while the
  main thread drains an outbound queue back to the client. Handles both a
  one-shot static-output serve (`_get_output`) and a live, cancellable,
  reconfigurable session (`_session`/`_input_fn`) in the same loop.
- `trims/live/session.py` — `LiveGeorefSession`: generation counter +
  cooperative-cancellation token (`make_abort_check`), `apply_config_update()`
  rebuilding `CorrectedSupportData`/`GeoreferencerConfig` from RPY/knob
  fields and remembering the last-applied values, `effective_config_payload()`
  (for echoing current state to a client) and `cache_key()` (for the
  server-side result cache, see below).
- `trims/live/progress_bridge.py` — `FlightProgressBar`: duck-types
  `trims.engine._progress`'s bar interface and forwards ticks as
  `progress` app_metadata messages instead of printing to stderr; wired
  into `flight_server.py`'s tile loop via `_progress.py`'s pluggable
  progress-factory hook.
- `trims/engine/tile_processor.py` — `TileProcessor.run_streaming()`: see
  "Per-pixel tile ownership" below for the core algorithm. Also takes
  `should_abort`/`tile_sink` for live cancellation and streaming output.
- `trims/engine/georeferencing_engine.py` — `GeoreferencingEngine.run()`
  takes `should_abort`, polled in the fused single-pass chunked
  query+resample loop (the path `TileProcessor` always takes for tiles);
  raises `AbortedError` on cancellation.

**FlashViewer (C++)** — builds clean (`ninja FlashViewer flashviewer_tests`)
and the full Catch2 suite passes (204 test cases, 4276/4276 assertions,
15 intentionally skipped — the ones needing an external running server):
- `src/live/LiveGeorefSession.hpp/.cpp` — Arrow Flight C++ client:
  connects, handshakes, runs a background reader thread decoding
  `start`/`scene_info`/`tile`/`progress`/`done` `app_metadata` JSON,
  re-emits as `Qt::QueuedConnection` signals (the reader thread has GUI
  affinity per `LiveRasterDataset::create()`'s doc comment, so
  `AutoConnection` would pick the wrong connection type). Also
  `sendConfigUpdate()` for the client → server direction, and
  `disconnectFromServer()` which now actually calls the reader's
  `Cancel()` (not just `DoneWriting()`) so the blocking `Next()` call
  unblocks promptly on user-initiated disconnect.
- `src/live/LiveRasterDataset.hpp/.cpp` — bridges arriving tiles into a
  GDAL MEM driver dataset (`MEM:::DATAPOINTER=...`), unchanged by
  `RasterDataset`/the GL tile renderer. Emits `regionUpdated(row0,row1)`
  per tile.
- `src/core/TileCache.hpp/.cpp`, `src/render/TileRenderer.*`,
  `src/render/MapCanvas.*` — a `GpuTile::data_dirty` flag and
  `markLayerDirty()` path let a live tile update force a re-decode of an
  already-cached GPU tile without a full cache evict; `MainWindow`
  throttles this to a 300ms timer per pane during live updates, and does
  one full `invalidateLayer()` at the very end once overviews are
  rebuilt.
- `src/panels/RpyControlPanel.hpp/.cpp` — Qt control panel (RPY bias/rate/
  frame, stride, cell-locate method, use-local-cell-guess, resample mode,
  device, three dtype combos, progress bar, Live/Recompute mode toggle).
  Debounces edits client-side and guards `sendUpdate()` against sending a
  no-op `config_update` via a `m_last_sent` baseline; `applyRemoteConfig()`
  restores all controls (under `QSignalBlocker`) from a server-echoed
  `current_config` without re-triggering an update.
- `src/app/MainWindow.cpp` — `openLiveSession()`: menu action ("Connect
  Live Session…"), connects, creates the `LiveRasterDataset`/`RasterLayer`,
  wires progress/region-update/error signals, opens `RpyControlPanel`.
  Tears down and removes any existing live layer/session first, so
  reconnecting doesn't leave stacked "Live Session" layers. Progress
  dialog has a fixed max width with a word-wrapped label.
- `src/main.cpp` — `qRegisterMetaType<LiveTile>()` /
  `qRegisterMetaType<QVector<double>>()` registered at startup, required
  for the cross-thread queued signal connections to deliver.

## Core architecture: per-pixel tile ownership

The original design (row-range ownership: each output row belongs to
exactly one tile) broke down on real sensor geometry. A pushbroom swath
that's rotated relative to the output grid (the common case — track
direction rarely aligns with north-up) has tile validity that varies by
**column**, not just row: two tiles can both partially cover the same row
range, each valid in a different span of columns. Row-range ownership
either left gaps or produced a visible striped/comb pattern where
adjacent tiles disagreed about who owned a row.

`TileProcessor.run_streaming()` instead uses a full-scene accumulator
(`accum_image`, `owner_dist`, `last_line_map`, `last_sample_map`,
`last_latlon_map`, sized `[H_out, W_out]`) and per-pixel ownership: each
tile's output is scored per-pixel by distance from the tile's along-track
center line, and a pixel's owner is whichever processed tile had the
closest center among the ones that produced a valid (non-nodata) value
there. Later tiles can win pixels away from earlier ones and vice versa —
there's no "first writer wins" or blend-at-the-seam step. Because the
accumulator holds the whole scene, memory is no longer bounded by "one
tile's worth of pixels" the way the original streaming design assumed;
this was an explicit trade accepted for correctness after a row-based
fix attempt was shown, via a real scene screenshot, to still produce
gaps.

`run()` (the non-streaming, whole-scene path) is unaffected and still
alpha-blends overlapping tiles — only `run_streaming()`'s ownership
model changed.

### Resampling apron

Resampling (`blackman_sinc_resample`, `F.grid_sample(padding_mode="border")`)
needs neighbouring pixels beyond a tile's own row range to avoid
border-clamp artifacts at tile edges. `TileProcessorConfig.resample_apron_lines`
(default 8) controls how many extra rows of context each tile reads on
each side purely to supply resampling neighbours — apron pixels are
**never** claimed as owned output, only used as sampling context. This is
independent of `overlap_lines` (which controls how much tiles overlap in
their claimed output ranges, now largely a knob for redundancy/performance
rather than correctness, since ownership is per-pixel regardless of
overlap depth).

## Server-side result caching and session-state echo

- **Result cache**: once a live session finishes computing a scene, the
  server keeps the completed raster in memory (`TrimsFlightServer._live_cache`,
  process-lifetime only, one entry) keyed by `LiveGeorefSession.cache_key()`
  — an exact-equality tuple of the engine config, last-applied RPY
  bias/rate/frame, and requested bbox/resolution. A client that
  reconnects (e.g. after a crash) with an unchanged config gets the
  cached tiles replayed instantly via `_replay_cached_generation()`
  instead of triggering a full recompute.
- **Config echo**: `scene_info` now includes a `current_config` object
  (`LiveGeorefSession.effective_config_payload()`) with the session's
  current RPY bias/rate/frame, stride, cell-locate method, device, and
  dtype/resample settings. On the FlashViewer side, `SceneInfo::currentConfig`
  is parsed and applied to `RpyControlPanel` via `applyRemoteConfig()`
  whenever a session is (re)opened — so a FlashViewer crash/restart
  mid-session reconnects into the same control-panel state rather than
  resetting to defaults.

## Known risks / rough edges still worth watching

1. **Concurrent MEM-buffer read/write.** `LiveRasterDataset::onTileReceived`
   writes into the same buffer the render thread's `GDALRasterIO` reads
   via the MEM dataset, guarded only by `m_buffer_mutex` — GDAL's read
   path does not take that mutex. In practice this can only produce a
   stale-but-valid pixel value (the buffer is never reallocated after
   `onStarted`), never corrupt memory, but it's a real race by the letter
   of the C++ memory model. Acceptable as shipped; flag before anything
   performance- or correctness-sensitive is layered on top.
2. **In-memory cache is single-entry and process-lifetime only.** A
   second concurrent live session with a different config evicts the
   only cache slot; a server restart loses it. Fine for the current
   single-session-at-a-time usage pattern; revisit if multi-session or
   persistent caching becomes a real requirement.
3. **`resample_apron_lines` is a fixed default (8), not derived from the
   actual resampling kernel's support radius.** `blackman_sinc_resample`'s
   support happens to fit within 8 lines for the geometries tested; if
   the kernel or its support radius changes, this constant should be
   revisited alongside it.
4. **`run_streaming()` vs `run()` no longer agree to tight tolerance.**
   `run()` still alpha-blends overlaps; `run_streaming()` doesn't (hard
   per-pixel ownership). `trims/tests/test_streaming.py` was relaxed to
   statistical bounds (mean diff, fraction differing) rather than a tight
   per-pixel tolerance — expected, not a bug, but worth remembering if
   someone re-tightens that test without recalling why it was loosened.

## File map

**trims-georef** (`feature/live-arrow-flight`):
```
pyproject.toml                          # `live` extra
trims/live/__init__.py
trims/live/arrow_tile_writer.py         # ArrowTileWriter (float32 + uint16)
trims/live/flight_server.py             # TrimsFlightServer: full duplex do_exchange, caching
trims/live/session.py                   # LiveGeorefSession: config apply/echo/cache_key
trims/live/progress_bridge.py           # FlightProgressBar
trims/engine/tile_processor.py          # per-pixel ownership + accumulator, resample apron
trims/engine/georeferencing_engine.py   # should_abort, AbortedError
trims/engine/_progress.py               # pluggable progress-factory hook
trims/grid/forward_model_grid.py        # should_abort threading
trims/tests/test_live_arrow.py
trims/tests/test_streaming.py
```

**FlashViewer** (`feature/arrow-flight-live-georef`):
```
docs/LIVE_GEOREF_DEV_PLAN.md            # this file
cmake/Dependencies.cmake                # FV_ARROW_TARGETS
src/CMakeLists.txt
src/main.cpp                            # qRegisterMetaType<LiveTile>/<QVector<double>>
src/live/LiveGeorefSession.hpp/.cpp     # Arrow Flight C++ client, config parsing
src/live/LiveRasterDataset.hpp/.cpp     # GDAL MEM bridge
src/panels/RpyControlPanel.hpp/.cpp     # control panel, remote-config restore
src/app/MainWindow.cpp                  # openLiveSession, progress dialog, layer lifecycle
src/core/TileCache.hpp/.cpp             # data_dirty / markLayerDirty
src/render/TileRenderer.hpp/.cpp        # markLayerDirty forwarding
src/render/MapCanvas.hpp/.cpp           # markLayerDirty (no-GL-context path)
src/io/RasterDataset.hpp
tests/test_live_session.cpp
```

## History

This doc originally tracked an in-progress handoff (Milestone 1 done,
Milestone 2/build/MainWindow-wiring pending). All of that has since been
completed, and a second round of fixes/design changes landed on top of it
after real end-to-end testing surfaced problems the original design
didn't anticipate — most significantly the row-based → per-pixel
ownership rewrite, driven by real (rotated-swath) scene geometry showing
gaps that synthetic test fixtures hadn't caught. See the two feature
branches' commit history for the detailed sequence.
