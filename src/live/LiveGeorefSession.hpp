#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QMetaType>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace arrow { namespace flight {
class FlightClient;
class FlightStreamWriter;
class FlightStreamReader;
}}

// Per-axis RPY bias/rate + the GeoreferencerConfig subset a live session can
// push to a trims-georef Arrow Flight server. Field names/units mirror
// trims/live/session.py's LIVE_CONFIG_FIELDS and
// trims.refinement.corrected_support_data.CorrectedSupportData's constructor
// (rpy_bias, rpy_rate, t_ref, frame) -- see the plan doc's "Control payload"
// section for the wire-level app_metadata shape this gets serialized to.
struct LiveConfigUpdate {
    double rpyBiasRad[3]{0.0, 0.0, 0.0};       // [roll, pitch, yaw], radians
    double rpyRateRadPerS[3]{0.0, 0.0, 0.0};   // rad/s
    double rpyTRefS{0.0};                       // seconds since J2000
    QString rpyFrame{QStringLiteral("hill")};   // "hill" | "body"

    int stride{32};
    QString cellLocateMethod{QStringLiteral("affine_index")};  // 'affine_index' | 'grid_walk' | 'bilinear_global'
    bool useLocalCellGuess{false};
    QString device{QStringLiteral("cpu")};       // 'cpu' | 'cuda'
    QString dtypeGeo{QStringLiteral("float64")};
    QString dtypePixel{QStringLiteral("float32")};
    QString dtypeMaps{QStringLiteral("float64")};
    QString resampleMode{QStringLiteral("bicubic")};  // 'bilinear' | 'bicubic' | 'blackman_sinc'

    // Two-tier preview/full-resolution interactivity (see the plan doc):
    // RpyControlPanel sends preview=true immediately on every slider tick,
    // then a debounced preview=false once the slider settles.
    bool preview{false};
};

// One tile's worth of georeferenced pixel + geolocation data, unpacked from
// an Arrow RecordBatch matching trims.live.arrow_tile_writer.ArrowTileWriter's
// schema: one Arrow row = one output scanline, band/line_src/sample_src/lat/lon
// columns are FixedSizeList<T>[W]. `coverage` (also in the Python schema) is
// intentionally NOT carried here -- LiveRasterDataset derives "no coverage"
// from nodata comparison on the pixel values it already has, avoiding the
// extra bit-packed BooleanArray unpack.
struct LiveTile {
    int row0{0};
    int row1{0};      // exclusive
    int width{0};
    QStringList bandIds;
    // "float32" | "uint16" -- matches the session's ArrowTileWriter band_dtype
    // (see trims.live.arrow_tile_writer.build_schema), fixed for the whole
    // session (set from the "start" message, same value on every tile).
    // Selects which of image/imageU16 below is populated.
    QString bandDtype{QStringLiteral("float32")};
    // BSQ layout: bandIds.size() planes of (row1-row0) x width -- band b,
    // local row r starts at [(b * (row1-row0) + r) * width] in whichever of
    // these is populated. uint16 is real sensor DN (e.g. LISS-3's native
    // 12-bit-in-uint16) already rounded server-side, not truncated float.
    std::vector<float> image;          // bandDtype == "float32"
    std::vector<uint16_t> imageU16;    // bandDtype == "uint16"
    std::vector<double> lineSrc;    // (row1-row0) x width, row-major
    std::vector<double> sampleSrc;  // (row1-row0) x width, row-major
    std::vector<double> lat;        // (row1-row0) x width, row-major
    std::vector<double> lon;        // (row1-row0) x width, row-major
};
Q_DECLARE_METATYPE(LiveTile)

// Owns one bidirectional Arrow Flight DoExchange stream to a
// trims.live.flight_server.TrimsFlightServer instance. Runs a background
// reader thread that decodes incoming "start"/"tile"/"progress"/"done"
// app_metadata JSON envelopes (see the plan's wire-protocol section) and
// re-emits them as Qt signals.
//
// THREADING: signals are emitted from the reader thread, not the Qt UI
// thread. Connect to them with Qt::QueuedConnection (the default for
// signals crossing between QObjects that live in different threads, as
// long as both have a running event loop) rather than assuming
// direct/same-thread delivery. Call qRegisterMetaType<LiveTile>() and
// qRegisterMetaType<QVector<double>>() once at startup (e.g. in main())
// before connecting to `tileReceived`/`started` across threads.
//
// BUILD NOTE: written against the classic Status-returning Arrow Flight
// C++ API (FlightClient::DoExchange(descriptor, &writer, &reader) and
// FlightStreamReader::Next(&chunk)). This has been stable across many
// Arrow releases, but some newer versions prefer arrow::Result<>-wrapped
// equivalents -- check trims-georef's pinned pyarrow/arrow-cpp version
// (cmake/Dependencies.cmake's new Arrow entry) against the installed
// Arrow C++ headers on first build and adjust call sites if needed. This
// file has not been compiled in the environment that authored it.
class LiveGeorefSession : public QObject {
    Q_OBJECT
public:
    // location: "grpc://host:port", matching TrimsFlightServer's bind address.
    explicit LiveGeorefSession(QString location, QObject* parent = nullptr);
    ~LiveGeorefSession() override;

    // Connects, sends the initial handshake message, and starts the
    // background reader thread. Returns false (with *errorOut set) on
    // immediate connection failure; asynchronous failures surface via
    // errorOccurred().
    bool connectToServer(QString* errorOut = nullptr);
    void disconnectFromServer();

    bool isConnected() const { return m_connected.load(); }
    int currentGeneration() const { return m_generation.load(); }

    // Bumps the generation counter, serializes `update` to a config_update
    // app_metadata JSON message, and writes it on the shared DoExchange
    // stream. Thread-safe (guarded by m_write_mutex) -- RpyControlPanel
    // calls this from the Qt UI thread while the reader thread concurrently
    // drains server -> client messages on the same logical stream (Arrow
    // Flight DoExchange read/write directions are independent).
    // Returns the new generation number.
    int sendConfigUpdate(const LiveConfigUpdate& update);

signals:
    void started(int H, int W, QStringList bandIds, QVector<double> geotransform, int epsg,
                 QString bandDtype, double nodataValue, int generation);
    void tileReceived(LiveTile tile, int generation);
    void progressUpdated(int current, int total, QString stage, double etaS, int generation);
    void finished(int generation);
    void errorOccurred(QString message);

private:
    void readLoop();

    QString m_location;
    std::unique_ptr<arrow::flight::FlightClient> m_client;
    std::unique_ptr<arrow::flight::FlightStreamWriter> m_writer;
    std::unique_ptr<arrow::flight::FlightStreamReader> m_reader;
    std::thread m_reader_thread;
    std::mutex m_write_mutex;
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_stop{false};
    std::atomic<int> m_generation{0};
    // Set from the "start" message's band_dtype field (readLoop only, no
    // cross-thread access before "started" fires and after it's fixed for
    // the session's lifetime, so no separate lock needed).
    QString m_band_dtype{QStringLiteral("float32")};
};
