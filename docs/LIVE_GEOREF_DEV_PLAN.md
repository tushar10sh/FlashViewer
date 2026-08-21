# Live Arrow Flight Georeferencing — Handoff Dev/Test Plan

Continuation of a dual-repo feature: trims-georef streams georeferenced
tiles to FlashViewer over Apache Arrow Flight, live, with a control panel to
adjust RPY attitude bias/rate and `GeoreferencerConfig` knobs (stride,
cell-seeding method, device, float precision, resample mode) and see the
raster update. Full design/rationale: see the plan this branch was built
from (`i-want-this-library-greedy-thunder` in the originating session) —
this doc is the **practical continuation checklist**, not a re-derivation
of that design.

Branches: `feature/live-arrow-flight` (trims-georef), this repo is on
`feature/arrow-flight-live-georef`.

## Status: what's actually done vs. not

**trims-georef (Python) — implemented AND tested**, using a real venv
(torch/numpy/pyarrow/pytest installed, `pip install -e .`), against
in-process Arrow Flight gRPC server/client — not just unit logic:
- `trims/live/arrow_tile_writer.py` — `ArrowTileWriter`: row-strip
  RecordBatch schema (one Arrow row = one output scanline,
  `FixedSizeList<T>[W]` columns per band/line_src/sample_src/lat/lon/coverage).
- `trims/live/flight_server.py` — `TrimsFlightServer`: Milestone-1 one-shot
  `DoExchange` service. Sends a `start` message (H, W, band_ids,
  geotransform, epsg) before tiles, then `tile` messages, then `done`.
- `trims/live/session.py` — `LiveGeorefSession` (Python side): generation
  counter + cooperative-cancellation token + `apply_config_update()` that
  rebuilds `CorrectedSupportData` from RPY fields — **written but not yet
  wired into `flight_server.py`'s `do_exchange` loop**. Milestone 1's
  server is one-shot only; it does not yet read `config_update` messages
  from the client mid-stream. That's the next Python-side task (see below).
- `trims/live/progress_bridge.py` — `FlightProgressBar`: duck-types
  `trims.engine._progress`'s bar interface, forwards ticks to a callback
  instead of stderr — **written but not yet threaded through
  `TileProcessor.run_streaming`/`GeoreferencingEngine.run`'s actual
  `_progress.make_bar`/`chunk_bar`/`tile_bar` call sites.** Those still
  print to stderr; nothing calls `FlightProgressBar` yet.
- `trims/engine/tile_processor.py` — `TileProcessor.run_streaming()` gained
  real `should_abort` (checked once per tile + forwarded into each tile's
  `GeoreferencingEngine.run`) and `tile_sink` (called with the same
  `[bands,h,W]` chunks that get written to the GeoTIFF) parameters.
  `StreamingResult` gained `aborted: bool`.
- `trims/engine/georeferencing_engine.py` — `GeoreferencingEngine.run()`
  gained `should_abort`, polled in the fused single-pass chunked
  query+resample loop only (the path `TileProcessor` always takes for
  tiles) — NOT in the two-pass/refinement or non-chunked paths. Raises the
  new `AbortedError`.
- Tests: `trims/tests/test_live_arrow.py` — 3 tests, all passing:
  `ArrowTileWriter` round-trip, a real in-process `TrimsFlightServer` +
  `pyarrow.flight.FlightClient` `DoExchange` round-trip matching
  `GeoreferencingEngine.run()`'s in-memory output bit-for-bit (within
  float32 tolerance), and `should_abort`/`tile_sink` cooperative
  cancellation.
- **Full regression check**: `trims/tests/` — 790 passed, 67 skipped
  (GPU-only), 0 failed, run against the modified `tile_processor.py`/
  `georeferencing_engine.py`. No existing behavior broke.
- `pyproject.toml` — new `live` extra (`pyarrow>=14`).

**FlashViewer (C++) — written but UNCOMPILED and UNTESTED.** This
environment has no `cmake`, no Qt6, no GDAL dev headers, and no Arrow C++
— there was no way to build or run any of this here. Treat every C++ file
below as a first draft that needs a real build to find its mistakes:
- `src/live/LiveGeorefSession.hpp/.cpp` — Arrow Flight C++ client:
  connects, does the handshake, runs a background reader thread decoding
  `start`/`tile`/`progress`/`done` `app_metadata` JSON, re-emits as Qt
  signals. Also `sendConfigUpdate()` for the client → server direction
  (not yet consumed server-side, see above).
- `src/live/LiveRasterDataset.hpp/.cpp` — bridges arriving tiles into a
  GDAL **MEM driver** dataset opened via the `MEM:::DATAPOINTER=...`
  in-memory-buffer string syntax, then `RasterDataset::open(path)`
  **unchanged** — so `RasterLayer`/the GL tile renderer need no changes.
  Emits `regionUpdated(row0,row1)` per tile; no per-region GL invalidation
  hook exists in `TileCache`/`LayerManager` today, so the intended wiring
  is `LayerManager::notifyLayerChanged(index)` (whole-layer, coarser than
  ideal — see Known Risks).
- `src/panels/RpyControlPanel.hpp/.cpp` — Qt control panel (RPY bias/rate/
  frame, stride, cell-locate method, use-local-cell-guess, resample mode,
  device, three dtype combos, progress bar). Implements the two-tier
  preview/full-resolution debounce (300ms) entirely client-side.
- `cmake/Dependencies.cmake` — new Arrow/ArrowFlight `find_package` block
  (`FV_ARROW_TARGETS`).
- `src/CMakeLists.txt` — new files registered, `FV_ARROW_TARGETS` linked.

**NOT done at all:**
- MainWindow wiring. No menu action, no `DatasetFactory` entry point, no
  member fields added to `MainWindow.hpp`/`.cpp`. See "Next task 1" below
  for exactly what to add and where — deliberately left out rather than
  hand-editing a 3000+ line file blind with no compiler to catch mistakes.
- `qRegisterMetaType<LiveTile>()` / `qRegisterMetaType<QVector<double>>()`
  — required once at startup (e.g. `main.cpp`) for the cross-thread queued
  signal connections `LiveGeorefSession` emits from its reader thread to
  actually deliver. Not yet added anywhere.
- Server-side live `config_update` handling (Milestone 2 in the original
  plan) — `TrimsFlightServer.do_exchange` still only serves one static
  scene and returns; it does not loop reading further client messages,
  rebuild `CorrectedSupportData`, or re-run `TileProcessor.run_streaming`.
  `trims/live/session.py`'s `LiveGeorefSession` (Python) has the pieces
  (`apply_config_update`, `make_abort_check`) but nothing calls them yet.
- Any FlashViewer test (Catch2 or manual) — impossible without a build.

## Next tasks, in order

1. **Wire `TrimsFlightServer.do_exchange` to loop on live config updates**
   (`trims/live/flight_server.py`). After sending `start`, instead of just
   sending all tiles once and returning: construct a
   `trims.live.session.LiveGeorefSession`, spawn the tile-sending work
   (initial full-res pass) so it runs against `should_abort=session.make_abort_check(generation)`
   and `tile_sink=<writes an ArrowTileWriter batch to `writer`>`, and
   concurrently keep calling `reader.read_chunk()` in a loop: on a
   `config_update` message, call `session.apply_config_update(payload)`
   (bumps generation, rebuilds `CorrectedSupportData`/`GeoreferencerConfig`),
   cancel the in-flight `TileProcessor.run_streaming` call, and kick off a
   new one with the new generation/config. This needs a background
   thread or asyncio-style structure since `do_exchange` must read AND
   write concurrently on one stream — the existing single-threaded
   sequential loop in the current `do_exchange` won't do both at once.
   Test analogously to `test_tile_processor_should_abort_and_tile_sink` in
   `trims/tests/test_live_arrow.py`, but driving it through a real
   `FlightClient` sending a `config_update` mid-stream and asserting the
   tiles received after it reflect the new RPY bias (cross-check against
   directly constructing `CorrectedSupportData` with the same bias and
   comparing geometry, like `satellites/tests/test_ocm3.py` already does).
2. **Thread `FlightProgressBar` through the actual progress call sites.**
   `trims/engine/_progress.py`'s `tile_bar`/`chunk_bar`/`make_bar` need an
   optional way to accept a caller-supplied bar factory (or
   `TileProcessor`/`GeoreferencingEngine` need an `on_progress` callable
   parameter that constructs a `trims.live.progress_bridge.FlightProgressBar`
   instead of the default stderr bar) so a live session's tile loop reports
   real progress over the wire instead of only stderr.
3. **Get a build environment for FlashViewer** (this environment has none):
   install cmake, Ninja, Qt6 (≥6.4, Core/Widgets/OpenGL/OpenGLWidgets/
   Charts/Network/Svg), GDAL ≥3.8, muParser, spdlog, nlohmann-json, GLM,
   Catch2, **and Arrow C++ with Flight enabled** (verify Flight
   specifically — many distro Arrow packages ship without it). Then:
   ```
   cmake --preset macos-debug   # or linux-debug/windows-debug per docs/INSTALL.md
   cmake --build build/macos-debug --parallel
   ```
   Expect real compile errors on first attempt — see "Known risks" below
   for the specific spots most likely to need adjusting.
4. **Fix whatever `LiveGeorefSession.cpp`'s Arrow Flight C++ calls need**
   against the actually-installed Arrow version (see Known Risks #1).
5. **Add the MainWindow entry point** (see exact instructions below).
6. **Add `qRegisterMetaType` calls** in `main.cpp` before any live session
   can be created.
7. **Write `tests/test_live_session.cpp`** (Catch2, next to
   `test_io_formats.cpp`): spin up a real `TrimsFlightServer` (Python,
   via a test fixture that shells out `python -m trims.live...` or a
   small pytest-side helper script) or, simpler for a first pass, a
   trivial mock Arrow Flight C++ server, and assert `LiveRasterDataset`'s
   MEM dataset reads back the same pixels a static `RasterDataset` opened
   from an equivalent GeoTIFF would.
8. **Manual end-to-end check**: run a Python script that builds a
   `TrimsFlightServer` around one real georeferenced scene (mirroring
   `notebooks/ocm_exercises/ocm3_georef.py`'s pipeline, per the original
   plan's OCM-3-first note — though the live-session code itself was built
   sensor-agnostic per the "generalize" scope decision), connect
   FlashViewer's new live-session UI, and visually confirm the raster
   renders and updates.

### Next task 1 in detail: MainWindow wiring

Mirror the existing "Open URL" action exactly
(`src/app/MainWindow.cpp` ~line 793-809):

```cpp
auto* actLiveSession = fileMenu->addAction(tr("Connect &Live Session…"));
connect(actLiveSession, &QAction::triggered, this, [this] {
    bool ok;
    QString location = QInputDialog::getText(this, tr("Connect Live Georeferencing Session"),
        tr("Enter Flight server address (grpc://host:port):"), QLineEdit::Normal,
        QStringLiteral("grpc://localhost:8815"), &ok);
    if (!ok || location.isEmpty()) return;
    ErrorReporter::runGuarded("Connect Live Session", [&] { openLiveSession(location); });
});
```

Add `void openLiveSession(const QString& location);` as a new private
method (declared in `MainWindow.hpp`, defined in `MainWindow.cpp`), plus
member fields:

```cpp
// MainWindow.hpp, forward decls: class RpyControlPanel; class LiveRasterDataset;
RpyControlPanel* m_rpy_panel{nullptr};
std::unordered_map<uint64_t, std::shared_ptr<LiveRasterDataset>> m_live_sessions;  // keyed by RasterLayer::layerId()
```

`openLiveSession()` body, mirroring `openFiles()`'s
`auto layer = std::make_shared<RasterLayer>(sub_ds); ... m_layer_mgr->addLayer(layer);`
pattern (`MainWindow.cpp` ~line 440-446):

```cpp
void MainWindow::openLiveSession(const QString& location) {
    auto session = std::make_shared<LiveGeorefSession>(location);
    QString err;
    if (!session->connectToServer(&err)) {
        QMessageBox::warning(this, tr("Connection Failed"), err);
        return;
    }
    auto liveDs = LiveRasterDataset::create(session);
    connect(liveDs.get(), &LiveRasterDataset::ready, this, [this, liveDs] {
        auto layer = std::make_shared<RasterLayer>(liveDs->dataset());
        layer->setName(tr("Live Session"));
        layer->setPaneId(kDefaultPaneId);   // or preparePaneForRasterLayer(...) if available for this ctor shape
        m_layer_mgr->addLayer(layer);
        m_live_sessions[layer->layerId()] = liveDs;
        connect(liveDs.get(), &LiveRasterDataset::regionUpdated, this, [this, id = layer->layerId()](int, int) {
            for (int i = 0; i < m_layer_mgr->count(); ++i)
                if (auto l = m_layer_mgr->layerAt(i))
                    if (auto rl = std::dynamic_pointer_cast<RasterLayer>(l); rl && rl->layerId() == id) {
                        m_layer_mgr->notifyLayerChanged(i);
                        break;
                    }
        });
        if (!m_rpy_panel) {
            m_rpy_panel = new RpyControlPanel(this);
            fvApplyPlotWindowFlags(m_rpy_panel);   // same treatment as m_mtf_panel, see MainWindow.cpp:1746-1751
        }
        m_rpy_panel->setSession(session);
        m_rpy_panel->show();
    });
    connect(liveDs.get(), &LiveRasterDataset::sessionError, this, [this](QString msg) {
        QMessageBox::warning(this, tr("Live Session Error"), msg);
    });
    // Remove from m_live_sessions in a LayerManager::layerAboutToBeRemoved handler
    // (already connected elsewhere for temp-file cleanup, FR-LYR-4) -- add a case
    // for m_live_sessions.erase(layerId) there rather than a new connection, to
    // keep the "what happens when a layer is removed" logic in one place.
}
```

**This is unverified pseudocode-grade C++** (never compiled) — treat it as
a strong starting point, not a drop-in patch. Check `preparePaneForRasterLayer`'s
actual signature before using/skipping it, and check how the existing
`layerAboutToBeRemoved` handler (search `MainWindow.cpp` for
`layerAboutToBeRemoved`) is structured before adding a case to it.

## Known risks / things most likely to break on first build

1. **Arrow Flight C++ API version drift.** `LiveGeorefSession.cpp` uses
   the classic `Status`-returning API
   (`FlightClient::Connect(location, &client)` via `arrow::Result`,
   `FlightClient::DoExchange(descriptor, &writer, &reader)`,
   `FlightStreamReader::Next(&chunk)`). Some Arrow releases restructure
   parts of this around `arrow::Result<>`-wrapped return values instead of
   out-parameters. **First thing to check when this fails to compile.**
2. **GDAL MEM driver open-string syntax.** `LiveRasterDataset.cpp` builds
   a `MEM:::DATAPOINTER=<ptr>,PIXELS=...,LINES=...,BANDS=...,DATATYPE=Float32,
   PIXELOFFSET=...,LINEOFFSET=...,BANDOFFSET=...` string and passes it to
   `RasterDataset::open()` (→ `GDALOpenEx`). This is documented GDAL MEM
   driver behavior but was **never exercised against a real GDAL build**
   here — test it standalone (a tiny throwaway `main()` that builds a
   small float buffer, opens it this way, and reads a pixel back with
   `GDALRasterIO`) before debugging it through the full FlashViewer stack.
3. **`RasterDataset::open()` failure mode is silent-ish.** It returns
   `nullptr` on failure (per its existing contract) — `LiveRasterDataset`
   logs a warning but the live session then has a `ready()` that never
   fires; there's no user-facing error surfaced for "the MEM open itself
   failed" distinct from "the Flight connection failed." Worth tightening
   once the MEM-open path is verified to work at all.
4. **Whole-layer invalidation, not per-region.** `regionUpdated(row0,row1)`
   carries a real row range, but the wiring above collapses it to
   `LayerManager::notifyLayerChanged(index)` (whole layer) because no
   finer hook was found in `TileCache`. For a large raster with frequent
   small tile updates this likely means visibly more redraw work than
   necessary — profile before optimizing; don't assume it's a problem
   without measuring on a real scene.
5. **Concurrent MEM-buffer read/write.** `LiveRasterDataset::onTileReceived`
   writes into the same `std::vector<float>` the render thread's
   `GDALRasterIO` reads via the MEM dataset, guarded only by
   `m_buffer_mutex` — but GDAL's read path does NOT take that mutex (it
   can't, it doesn't know about it). This is a real data race by the
   letter of the C++ memory model, even though in practice it can only
   produce a stale-but-valid pixel value, never corrupt memory (buffer is
   never reallocated after `onStarted`). Acceptable for a first pass;
   flag to whoever reviews this before shipping anything performance- or
   correctness-sensitive on it.
6. **Server is one-shot (see "Next task 1").** Don't spend time debugging
   `RpyControlPanel`'s slider behavior against a live server until task 1
   is done — right now the server sends one scene and stops, so nothing
   the panel sends will visibly change anything yet.

## File map

**trims-georef** (`feature/live-arrow-flight`):
```
pyproject.toml                          # new `live` extra
trims/live/__init__.py                  # new
trims/live/arrow_tile_writer.py         # new
trims/live/flight_server.py             # new
trims/live/session.py                   # new
trims/live/progress_bridge.py           # new
trims/engine/tile_processor.py          # modified: should_abort, tile_sink, StreamingResult.aborted
trims/engine/georeferencing_engine.py   # modified: should_abort, AbortedError
trims/tests/test_live_arrow.py          # new, 3 tests, all passing
```

**FlashViewer** (`feature/arrow-flight-live-georef`):
```
docs/LIVE_GEOREF_DEV_PLAN.md            # this file
cmake/Dependencies.cmake                # modified: FV_ARROW_TARGETS
src/CMakeLists.txt                      # modified: new files registered + linked
src/live/LiveGeorefSession.hpp/.cpp     # new, UNCOMPILED
src/live/LiveRasterDataset.hpp/.cpp     # new, UNCOMPILED
src/panels/RpyControlPanel.hpp/.cpp     # new, UNCOMPILED
```
