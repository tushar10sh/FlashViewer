# Installing FlashViewer

## 1. Minimum Requirements

### 1.1 Hardware

| Component | Minimum | Notes |
|---|---|---|
| GPU | OpenGL 4.1 Core Profile | Mandatory — the application will not launch without it |
| GPU examples | NVIDIA Kepler (GTX 600+), AMD GCN (HD 7000+), Intel HD 4000+, any Apple Silicon | Apple Silicon uses macOS's Metal → OpenGL 4.1 translation layer |
| MSAA | 4× support | Supported by virtually all OpenGL 4.1-capable GPUs |
| System RAM | 4 GB minimum, 8 GB recommended | Large rasters benefit from more RAM for tile decode buffers |
| GPU VRAM | 2 GB minimum | Tile cache defaults to 512 tiles; each 256×256 float32 tile ≈ 256 KB |
| Disk | 200 MB for the application | OSM tile cache adds up to 512 MB at `~/.cache/FlashViewer/osm/` |

### 1.2 Software — All Platforms

- CMake 3.25 or later
- Ninja build system
- Qt 6.4 or later (Core, Widgets, OpenGL, OpenGLWidgets, Charts, Network, **Svg** modules).
  Svg renders the themed toolbar/menu icons and the pane sync badges; the **test** binary links it
  too (`widgets/UiKit` is compiled into the suite), so it is required for `flashviewer_tests`, not
  only for the app. On Linux/macOS it is the `qt6-svg-dev` / `qt6-qtsvg-devel` package listed
  below; on Windows the aqt desktop package already contains it.
- GDAL 3.8 or later
- muParser 2.3 or later
- spdlog 1.x
- nlohmann-json 3.x
- GLM 0.9.9 or later
- Catch2 v3 (required only for running the test suite)

### 1.3 Platform-Specific Prerequisites

**Linux (Ubuntu 24.04 LTS or later)**  
All dependencies are available via `apt`. See Section 2.1 for the exact package list.

**Fedora (38 or later) and RHEL 9 / AlmaLinux 9 / Rocky Linux 9**  
All dependencies are available via `dnf`. RHEL-based systems also require EPEL and the CRB repository enabled first. See Section 2.2.

**macOS (Sequoia 15 or later)**  
- Xcode Command Line Tools (`xcode-select --install`)
- Homebrew (`brew.sh`)
- All C++ dependencies installed via `brew install`. See Section 2.4.

**Windows 10 / 11 (64-bit)**  
- Visual Studio 2022 with the "Desktop development with C++" workload (MSVC v143 toolchain)
- Git for Windows
- Qt 6.6.3 installed via `aqtinstall` (see Section 2.5.2)
- Miniforge/Miniconda — provides GDAL, muParser, spdlog, nlohmann-json, GLM, and Catch2 as prebuilt conda-forge packages (see Section 2.5.3)

---

## 2. Building from Source

### 2.1 Linux (Ubuntu 24.04 LTS or later)

#### 2.1.1 Install system dependencies

```bash
sudo apt-get update
sudo apt-get install -y \
  cmake ninja-build pkg-config \
  qt6-base-dev qt6-base-dev-tools \
  qt6-charts-dev \
  qt6-svg-dev \
  libgdal-dev \
  libmuparser-dev \
  libspdlog-dev \
  nlohmann-json3-dev \
  libglm-dev \
  catch2 \
  libgl-dev \
  libxkbcommon-dev \
  xvfb
```

> **Note:** On Ubuntu 24.04, `qt6-base-dev` already includes the Qt OpenGL and Qt OpenGLWidgets development headers. Do not install `libqt6opengl6-dev` or `libqt6openglwidgets6-dev` — those package names do not exist in this Ubuntu release.

#### 2.1.2 Clone the repository

```bash
git clone https://github.com/san-aria/FlashViewer.git
cd FlashViewer
```

#### 2.1.3 Configure

```bash
cmake --preset linux-release
```

#### 2.1.4 Build

```bash
cmake --build build/linux-release --parallel
```

#### 2.1.5 Run

```bash
./build/linux-release/FlashViewer
```

A display server (X11 or Wayland) must be active. To run on a headless server, prefix with `xvfb-run -a`.

---

### 2.2 Fedora (38 or later) and RHEL 9 / AlmaLinux 9 / Rocky Linux 9

#### 2.2.1 Fedora 38 or later

Install all dependencies from the default Fedora repos:

```bash
sudo dnf install -y \
  cmake ninja-build pkgconf-pkg-config \
  qt6-qtbase-devel \
  qt6-qtcharts-devel \
  qt6-qtsvg-devel \
  gdal-devel \
  muparser-devel \
  spdlog-devel \
  nlohmann-json-devel \
  glm-devel \
  catch2-devel \
  mesa-libGL-devel \
  libxkbcommon-devel \
  xorg-x11-server-Xvfb
```

> **Note:** `qt6-qtbase-devel` on Fedora 38+ already includes the Qt OpenGL and Qt OpenGLWidgets development headers. No separate OpenGL Qt package is needed.

#### 2.2.2 Clone the repository

```bash
git clone https://github.com/san-aria/FlashViewer.git
cd FlashViewer
```

#### 2.2.3 Configure

```bash
cmake --preset linux-release
```

#### 2.2.4 Build

```bash
cmake --build build/linux-release --parallel
```

#### 2.2.5 Run

```bash
./build/linux-release/FlashViewer
```

A display server (X11 or Wayland) must be active. To run on a headless server, prefix with `xvfb-run -a`.

---

### 2.3 RHEL 9 / AlmaLinux 9 / Rocky Linux 9

#### 2.3.1 Enable EPEL and CRB

```bash
sudo dnf install -y epel-release
sudo dnf config-manager --set-enabled crb
sudo dnf makecache
```

> **EPEL** (Extra Packages for Enterprise Linux) provides Qt6, GDAL, muParser, and the other dependencies that are absent from the RHEL 9 base repos. The **CodeReady Builder** (CRB) repository adds additional Qt6 and GL development headers required at build time. On AlmaLinux 9 and Rocky Linux 9 both repos are free. On a Red Hat subscription, CRB is included; if `--set-enabled crb` does not work, enable it via:
> ```bash
> subscription-manager repos --enable codeready-builder-for-rhel-9-x86_64-rpms
> ```

#### 2.3.2 Install system dependencies

```bash
sudo dnf install -y \
  cmake ninja-build pkgconf-pkg-config \
  qt6-qtbase-devel \
  qt6-qtcharts-devel \
  qt6-qtsvg-devel \
  gdal-devel \
  muparser-devel \
  spdlog-devel \
  json-devel \
  glm-devel \
  mesa-libGL-devel \
  libxkbcommon-devel \
  xorg-x11-server-Xvfb
```

> **Note on Catch2:** EPEL 9 ships `catch2-devel` 2.13 (Catch2 v2), which does not satisfy FlashViewer's `find_package(Catch2 3 REQUIRED)`. Do **not** install `catch2-devel` on RHEL 9 / AlmaLinux 9 / Rocky Linux 9 — the configure step below includes `-DBUILD_TESTING=OFF` to skip the test suite. The main application builds and runs without Catch2.

#### 2.3.3 Clone the repository

```bash
git clone https://github.com/san-aria/FlashViewer.git
cd FlashViewer
```

#### 2.3.4 Configure

```bash
cmake --preset linux-release -DBUILD_TESTING=OFF
```

#### 2.3.5 Build

```bash
cmake --build build/linux-release --parallel
```

#### 2.3.6 Run

```bash
./build/linux-release/FlashViewer
```

A display server (X11 or Wayland) must be active. To run on a headless server, prefix with `xvfb-run -a`.

---

### 2.4 macOS (Sequoia 15 or later)

#### 2.4.1 Install Xcode Command Line Tools

```bash
xcode-select --install
```

#### 2.4.2 Install Homebrew dependencies

```bash
brew update
brew install cmake ninja qt@6 gdal muparser spdlog glm nlohmann-json catch2
```

#### 2.4.3 Clone the repository

```bash
git clone https://github.com/san-aria/FlashViewer.git
cd FlashViewer
```

#### 2.4.4 Configure

```bash
cmake --preset macos-release \
  -DCMAKE_PREFIX_PATH="$(brew --prefix qt@6);$(brew --prefix gdal);$(brew --prefix muparser);$(brew --prefix spdlog);$(brew --prefix glm);$(brew --prefix nlohmann-json);$(brew --prefix catch2)"
```

#### 2.4.5 Build

```bash
cmake --build build/macos-release --parallel
```

#### 2.4.6 Run

```bash
open build/macos-release/FlashViewer.app
```

To create a distributable DMG:

```bash
# Deploy Qt frameworks and ad-hoc sign each dylib as it is modified
"$(brew --prefix qt@6)/bin/macdeployqt" build/macos-release/FlashViewer.app \
  -libpath="$(brew --prefix)/lib" \
  -libpath="$(brew --prefix qt@6)/lib" \
  -codesign=- -verbose=1

# Re-sign the entire bundle (covers any dylibs macdeployqt skipped)
codesign --deep --force --sign - build/macos-release/FlashViewer.app

# Package into a compressed DMG
hdiutil create -volname "FlashViewer" \
  -srcfolder build/macos-release/FlashViewer.app \
  -ov -format UDZO FlashViewer-macos.dmg
```

---

### 2.5 Windows 10 / 11 (64-bit)

#### 2.5.1 Install prerequisites

1. Download and install [Visual Studio 2022](https://visualstudio.microsoft.com/) (or the standalone **Build Tools for Visual Studio 2022**) with the **Desktop development with C++** workload — this provides the MSVC v143 compiler (`cl.exe`).
2. Install [Git for Windows](https://git-scm.com/download/win).
3. All build commands below must run in a shell where the MSVC compiler is on `PATH` (see §2.5.5). Verify with `cl` and `cmake --version`.

#### 2.5.2 Install Qt 6 via aqtinstall

```powershell
pip install aqtinstall
aqt install-qt windows desktop 6.6.3 win64_msvc2019_64 -m qtcharts
```

Note the install location (e.g. `C:\Qt\6.6.3\msvc2019_64`); its `bin` directory
is added to `PATH` in the build shell in §2.5.5.

#### 2.5.3 Create the conda environment

The C++ dependencies (GDAL, muParser, spdlog, nlohmann-json, GLM, Catch2) are
provided as prebuilt conda-forge packages, defined in `environment-windows.yml`. Install
[Miniforge](https://github.com/conda-forge/miniforge) (or Miniconda), then:

```powershell
conda env create -f environment-windows.yml
```

This creates an environment named `flashviewer-windows` (a one-time step). You
activate it in the build shell in §2.5.5.

> **NetCDF/HDF5 support:** conda-forge ships GDAL format drivers as separate
> plugins, so `environment-windows.yml` includes `libgdal-netcdf` and
> `libgdal-hdf5` alongside GDAL. Verify after creation with
> `gdalinfo --formats` (it should list `netCDF` and `HDF5`). These plugins also
> back the **Select Variables** dialog's *One multi-band layer* mode, which stacks
> the picked variables through GDAL's VRT driver (always available); without them
> the file cannot be opened at all, so there is nothing extra to install for it.

#### 2.5.4 Clone the repository

```powershell
git clone https://github.com/san-aria/FlashViewer.git
cd FlashViewer
```

#### 2.5.5 Enter the MSVC build environment

The configure and build steps need the MSVC compiler (`cl.exe`) on `PATH`. Open a
shell that has it, then activate the conda env and add Qt to `PATH` **inside that
same shell** (these settings do not persist across shells):

- **Easiest:** launch the **x64 Native Tools Command Prompt for VS 2022** (or
  **Developer PowerShell for VS 2022**) from the Start menu, or
- **From a plain PowerShell** (works for the standalone Build Tools), run:
  ```powershell
  & "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
  ```

Then, in that shell:

```powershell
conda activate flashviewer-windows
$env:PATH = "C:\Qt\6.6.3\msvc2019_64\bin;" + $env:PATH   # Qt bin (build + runtime DLLs)
```

#### 2.5.6 Configure

Point CMake at the conda dependencies and the aqt Qt installation via
`CMAKE_PREFIX_PATH` (no toolchain file is required — the project locates every
dependency with `find_package`):

```powershell
cmake --preset windows-release `
  -DCMAKE_PREFIX_PATH="$env:CONDA_PREFIX/Library;C:/Qt/6.6.3/msvc2019_64"
```

Because conda-forge ships prebuilt binaries, configure completes in seconds —
there is no dependency source compilation.

#### 2.5.7 Build

```powershell
cmake --build build/windows-release --parallel
```

#### 2.5.8 Run

Run it from the activated `flashviewer-windows` environment (so the GDAL runtime
DLLs in `%CONDA_PREFIX%\Library\bin` are on `PATH`) with the Qt `bin` directory
also on `PATH`:

```powershell
.\build\windows-release\FlashViewer.exe
```

To produce a self-contained, deployable folder, bundle the Qt DLLs with
`windeployqt` and copy the conda GDAL runtime DLLs next to the executable:

```powershell
windeployqt build\windows-release\FlashViewer.exe
Copy-Item "$env:CONDA_PREFIX\Library\bin\*.dll" build\windows-release\ -Force
```

---

### 2.6 Building with conda (Linux & macOS) — optional

> **This is an optional, secondary path.** The default and primary way to build
> on Linux and macOS is the system-package method above (apt / dnf / Homebrew).
> Use conda if you prefer an isolated, reproducible toolchain with no `sudo` and
> no per-distro package juggling. All dependencies — **including Qt 6 with
> Charts** — come from conda-forge; no Homebrew/apt `-dev` packages or
> `aqtinstall` are required.

#### 2.6.1 Install Miniforge

Install [Miniforge](https://github.com/conda-forge/miniforge) (or Miniconda).
The environment files pin the conda-forge channel and include the GDAL format-
driver plugins (`libgdal-netcdf`, `libgdal-hdf5`) needed for NetCDF/HDF5 support.

> **The C++ toolchain is bundled (self-contained).** The environment files now
> include a matching conda-forge compiler and C++ runtime, so no system compiler
> is required: **Linux** ships `gcc_linux-64` / `gxx_linux-64` / `sysroot_linux-64`
> plus `libgcc-ng` / `libstdcxx-ng`; **macOS** ships `cxx-compiler` (arch-correct
> Clang + MacOSX SDK) plus `libcxx`. Conda's activation scripts set `CC`/`CXX` to
> the bundled compiler — configure CMake from *within the activated env* so it is
> picked up.
>
> ⚠️ **GCC ABI warning (Linux).** conda-forge binaries (Qt, GDAL, …) are built
> against **GCC 12**'s libstdc++ ABI. If you build FlashViewer with a **system GCC
> older than 12** against these conda-forge libraries, you will hit link/runtime
> errors such as `undefined reference to ...__cxx11...` or
> `GLIBCXX_3.4.x not found`. **Use the bundled conda toolchain** (the default now —
> just activate the env and let `CC`/`CXX` point at
> `x86_64-conda-linux-gnu-gcc/g++`), **or** ensure your system compiler is
> **GCC ≥ 12**. Do not mix an older system GCC with conda-forge binaries.

#### 2.6.2 Linux

```bash
conda env create -f environment-linux.yml
conda activate flashviewer-linux

cmake --preset linux-release -DCMAKE_PREFIX_PATH="$CONDA_PREFIX"
cmake --build build/linux-release --parallel
./build/linux-release/FlashViewer        # run from within the activated env
```

> **OpenGL / mesa:** FlashViewer needs the **system GPU's** OpenGL 4.1 Core
> driver. If conda pulls in `mesa`, its `libGL` can shadow the system driver and
> cause software rendering or an "OpenGL 4.1 not available" error. Keep the
> system `libGL` ahead of `$CONDA_PREFIX/lib` on `LD_LIBRARY_PATH` if you hit
> this.
>
> **Qt xcb plugin:** if launch fails with *"could not load the Qt platform
> plugin xcb"*, install the system runtime libs
> `sudo apt-get install -y libxcb-cursor0 libxkbcommon-x11-0 libgl1 libegl1`
> (or set `QT_QPA_PLATFORM_PLUGIN_PATH="$CONDA_PREFIX/plugins/platforms"`). For a
> headless machine, run with `QT_QPA_PLATFORM=offscreen`.

#### 2.6.3 macOS (Intel and Apple Silicon)

```bash
conda env create -f environment-macos.yml
conda activate flashviewer-macos

cmake --preset macos-release -DCMAKE_PREFIX_PATH="$CONDA_PREFIX"
cmake --build build/macos-release --parallel
open build/macos-release/FlashViewer.app
```

> macOS exposes OpenGL 4.1 Core as its maximum version — exactly what FlashViewer
> targets — so the system OpenGL framework is sufficient (do not add `mesa`). The
> `cocoa` Qt platform plugin works without extra setup; for headless test runs use
> `QT_QPA_PLATFORM=offscreen`.

See `environment-linux.yml` / `environment-macos.yml` for the full notes.

---

### 2.7 Notes for contributors

Two repository-level conventions worth knowing before your first commit.

**Line endings are normalised by the repo, not by your Git config.**
[`.gitattributes`](../.gitattributes) declares `* text=auto`, so text files are stored as
LF and checked out with your platform's native endings; `.sh` / `.bash` are pinned to LF
and `.bat` / `.cmd` to CRLF, where the ending is load-bearing rather than cosmetic; and
raster and archive extensions are marked binary so a test fixture or packaging artefact
cannot be corrupted by newline conversion. You do **not** need to set `core.autocrlf` —
with it declared in the repo, a Linux clone and a Windows clone produce identical blobs.
It renormalises nothing already committed: `git add --renormalize .` restages no file that
was not already modified.

**The product version lives in exactly one file.** The root [`VERSION`](../VERSION) file is
authoritative. `CMakeLists.txt` reads it before `project()`, `cmake/Version.cmake` generates
`build/<preset>/generated/version.h` with `FLASHVIEWER_VERSION_STRING`, and
`src/app/Application.cpp` uses that for `setApplicationVersion` — which is what the startup
log banner and **Help → Licenses** report. To cut a new version, edit `VERSION` and rebuild;
do not add a literal anywhere else. (The README badge is the one copy that is updated by
hand.)

---

## 3. Verifying the Installation

### 3.1 Run the test suite

The test suite contains 168 Catch2 tests covering geo-transforms, settings,
colormaps, the tile cache, the Phase 0 test-infrastructure harnesses
(fixtures / GDAL oracle / mock network / offscreen-GL), the application shell
(theme, layout persistence, licenses manifest), raster opening/formats
(GeoTIFF, binary-raw VRT, NetCDF subdatasets, the combined multi-band variable
stack and its grid-compatibility probe, metadata), no-data/NaN
rendering and no-data edge-bleed suppression, selectable display resampling
(bilinear / bicubic B-spline / bicubic Catmull-Rom), the RGB/pseudocolor/opacity
render pipeline, LOD selection, the navigation camera (pan / zoom / fit /
screen↔geo round-trip), value⇄percentile stretch conversions and auto-stretch
statistics, the colorbar corner re-anchor, the world view presented on open and on
Fit, the basemap's longitude wrap-copy range and its sub-pixel Mercator row
tessellation, the New-Pane position picker's slot → (layout mode, region)
mapping, coordinate-array georeferencing (fill masking, longitude conventions,
1-D axis fitting and the 2-D swath warp), the plot curve-styling rule (a pane's first curve is its pane colour exactly; the
lightness ramp stays separable within a family), the two plot windows — layer removal while a
plot is open, Persist across scopes, deleting one curve, closing a window discarding its plots,
what a legend entry says with and without coordinates, the hand-set axis range that keeps min
below max and reverses instead, and the legend's half-container width ceiling — the point
mirrored between synced panes of different Project CRS — and the Layers panel — its grouping and
multi-delete logic plus the selection-persistence cases, which drive the **real**
`LayerPanel` widget and include a pixel check of a selected row's pane band under
hover against the shipped theme. Tests that need an unavailable capability skip
gracefully — the offscreen-GL render tests when no OpenGL 4.1 context is present,
the NetCDF subdataset tests when the GDAL netCDF driver is absent, and the
widget-focus section of the variable-picker wheel test on a platform that will not
give the window keyboard focus.

**Ubuntu (headless):**
```bash
cd build/linux-release
xvfb-run -a ctest --output-on-failure
```

**Fedora / RHEL (headless):**
```bash
cd build/linux-release
xvfb-run -a ctest --output-on-failure
```

**macOS:**
```bash
cd build/macos-release
QT_QPA_PLATFORM=offscreen ctest --output-on-failure
```

**Windows:**
```powershell
cd build\windows-release
ctest --output-on-failure
```

All tests must report `Passed` (render/NetCDF cases may report `Skipped` where the
required capability is unavailable, which is not a failure). The performance
absolute-threshold cases (`TC-PERF-01/03/04/05/07`) also `Skipped` unless
`FV_PERF_RHP` is set, since they are authoritative only on the Reference Hardware
Profile (SRS §6.1).

> **Optional — performance instrumentation build.** Configure with
> `-DFV_PERF_INSTRUMENT=ON` to additionally enable verbose per-operation timing logs
> for benchmarking (the always-on Performance HUD and the `[perf][logic]` tests need no
> special flag). CI runs these in a dedicated **non-gating** perf lane
> (`.github/workflows/ci-perf.yml`) that trends the numbers without failing the build;
> select the cases locally with `ctest -R "TC-PERF|TC-CAP-03"`.

> **Optional — the real-data georeferencing case.** `TC-IO-27` re-runs the
> coordinate-array projection end to end on the sample satellite swath in
> `sample_data/NetCDF_data/IMAGE0002.nc`. It is **hidden** so neither CI nor a plain
> `ctest` run depends on an 8 MB data file, and it is not registered as a CTest test;
> run it directly against the test binary, from anywhere:
> ```bash
> ./flashviewer_tests "[.geoloc-realdata]"        # Linux/macOS
> .\flashviewer_tests.exe "[.geoloc-realdata]"    # Windows
> ```
> It `SKIP`s cleanly if the sample file is absent. The equivalent synthetic cases
> (`TC-IO-17…26`) run in the normal suite and need no data files.

### 3.2 Smoke test

1. Launch FlashViewer.
2. Use **File → Open** (`Ctrl+O`) to open any GeoTIFF.
3. Confirm the raster renders, the scale bar appears at the bottom of the canvas, and the cursor coordinates update in the status bar as you move the mouse.
4. Press `Space` to fit the layer to the view.
5. Open the **Layers** dock and confirm the raster is listed under a **collapsible, pane-coloured group header** for its pane, and that its visibility toggle draws a **tick in the pane's colour** when checked.
6. Open the **GPU Monitor** dock (View → Panels) and confirm the resident-VRAM readout/sparkline updates as you pan and zoom.
7. Press `S` (or **Tools → Spectral Plot**) — the **Spectral Plot** opens as a separate window titled with the active layer. Turn on **Inspect** (`I`), click a pixel, and confirm the curve appears and its values match the **Pixel Inspector** table. Zoom with the ⊕/⊖ buttons or the wheel, drag the plot to pan, press ⌂ to return, and use **Save** to write a PNG. Confirm the curve is drawn in the layer's **pane colour** and that the legend to the right of the plot shows that colour together with the line style. Press the ✎ to open the labels dialog: rename the legend entry, then clear the field to confirm the automatic label returns, and untick **Show** on an axis title to confirm it disappears from the plot. Move and resize the window, close it and press `S` again to confirm it reopens where you left it, then press `P` and repeat the check on the **Scan/Pixel Profile** window. In the Profile, press **Compute**, tick **Persist curves**, activate a second layer and Compute again — both profiles should sit on one chart. Activate each of those layers in the Layers panel in turn: the same plot comes back for either, while a layer never profiled shows a blank chart named after it. Close the Profile panel and reopen it to confirm it comes back empty — neither plot keeps anything across a close. In either plot, press Delete on a legend row in the ✎ dialog and confirm that entry and its curve go on OK while the rest stay.
8. In the **Log** dock, click **Export Logs…** and confirm a `.txt` file is written.

### 3.3 Data directories and the application log

FlashViewer writes a size-bounded **rotating application log** (2 MB × 3 files) and
stores caches in the standard per-user locations:

| Item | Windows | Linux | macOS |
|---|---|---|---|
| Log file | `%APPDATA%\FlashViewer\FlashViewer\logs\flashviewer.log` | `~/.local/share/FlashViewer/FlashViewer/logs/flashviewer.log` | `~/Library/Application Support/FlashViewer/FlashViewer/logs/flashviewer.log` |
| OSM tile cache | `%LOCALAPPDATA%\FlashViewer\osm\` | `~/.cache/FlashViewer/osm/` | `~/Library/Caches/FlashViewer/osm/` |

The exact log path is also printed to the console at startup. All errors — including
GDAL errors and caught exceptions — are logged here with a severity level and shown
in the in-app Log panel; the **Export Logs…** button saves the panel contents to a
file of your choice.

### 3.4 Remote-URL security

When opening remote rasters (**File → Open URL**, `Ctrl+U`), FlashViewer validates
the URL before any network fetch: only `http`/`https`/`/vsicurl/`/`/vsis3/`/`/vsigs/`
schemes are accepted, and hosts resolving to loopback, link-local, private, or
cloud-metadata ranges (e.g. `169.254.169.254`, `10.0.0.0/8`, `127.0.0.0/8`) are
rejected (SSRF protection). TLS certificate verification is always enabled; HTTP
redirects/retries/timeouts are bounded and GDAL's block cache is capped.

---

## 4. Troubleshooting

### 4.1 "OpenGL 4.1 not available" at launch

FlashViewer requires OpenGL 4.1 Core Profile. If you see this error:

- **All platforms:** Update your GPU drivers to the latest version.
- **Linux:** Ensure Mesa 22 or later is installed (`glxinfo | grep "OpenGL version"`). On older distros, add the upstream Mesa PPA.
- **Virtual machines:** Most hypervisors expose at most OpenGL 3.3. Use a bare-metal GPU or a VM with GPU passthrough.
- **Windows (integrated GPU):** Intel HD Graphics 4000 and later support OpenGL 4.1 after updating the driver from Intel's download center.

### 4.2 GDAL or muParser not found at configure time

- **Linux (Ubuntu):** Confirm `libgdal-dev` and `libmuparser-dev` are installed: `dpkg -l | grep -E 'gdal|muparser'`.
- **Linux (Fedora / RHEL):** Confirm `gdal-devel` and `muparser-devel` are installed: `rpm -q gdal-devel muparser-devel`. On RHEL, also confirm EPEL is enabled: `dnf repolist | grep epel`.
- **macOS:** Confirm the Homebrew formula is installed and that `CMAKE_PREFIX_PATH` includes the output of `$(brew --prefix gdal)` and `$(brew --prefix muparser)`.
- **Windows:** Confirm the `flashviewer-windows` conda environment is active (`conda activate flashviewer-windows`) and that `CMAKE_PREFIX_PATH` includes `$env:CONDA_PREFIX/Library`. If a dependency is still not found, recreate the environment with `conda env create -f environment-windows.yml --force`.

### 4.3 Remote URL or COG fails to open

- Confirm network access is not blocked by a firewall or proxy.
- For authenticated S3 buckets, set the `AWS_ACCESS_KEY_ID` and `AWS_SECRET_ACCESS_KEY` environment variables before launching FlashViewer (GDAL reads them automatically).
- If cached tiles appear stale or corrupt, delete the GDAL VSI curl cache directory: `~/.cache/FlashViewer/osm/` on Linux/macOS, or `%LOCALAPPDATA%\FlashViewer\osm\` on Windows.

### 4.4 Black screen on first launch (Linux, no display)

If you launch FlashViewer over SSH without X forwarding, OpenGL will have no display to render to. Two options:

- Enable X forwarding: `ssh -X user@host` then launch normally.
- Use a virtual framebuffer: `xvfb-run -a ./build/linux-release/FlashViewer`.

Set `QT_QPA_PLATFORM=offscreen` only for test runs — it disables the real window and is not suitable for interactive use.

### 4.5 `dnf install` fails with "No such package" on RHEL 9

Most packages (Qt6, GDAL, muParser, spdlog, etc.) are in EPEL 9 or CRB, not the base RHEL 9 repos.

- Confirm EPEL is enabled: `dnf repolist | grep epel`
- Confirm CRB is enabled: `dnf repolist | grep crb` (should show `crb` or `codeready-builder`)
- If either is missing, re-run the Step 1 commands from Section 2.3.1.
- On pure Red Hat Enterprise Linux (not AlmaLinux or Rocky Linux), enable CRB via your subscription:
  ```bash
  subscription-manager repos --enable codeready-builder-for-rhel-9-x86_64-rpms
  ```
- After enabling both repos, re-run `sudo dnf makecache` before retrying the install.
