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

#include <QPointF>

struct BandHistogram {
    double minVal{0.0};
    double maxVal{1.0};
    double meanVal{0.0};
    double stdVal{1.0};
    std::vector<int64_t> counts;
    std::vector<double> binEdges;
};
Q_DECLARE_METATYPE(BandHistogram)

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

    // Used by RpyControlPanel::sendUpdate() to suppress sending (and thereby
    // bumping the session generation / restarting the whole scene's compute
    // -- see LiveGeorefSession::sendConfigUpdate()) a config_update whose
    // content is byte-for-byte identical to the last one actually sent. A
    // spurious extra call to onAnyControlChanged() -- e.g. any incidental
    // Qt signal fired by the control panel without the user actually
    // changing a value -- would otherwise still restart a live session from
    // scratch even though nothing meaningful changed.
    bool operator==(const LiveConfigUpdate& o) const {
        return rpyBiasRad[0] == o.rpyBiasRad[0] && rpyBiasRad[1] == o.rpyBiasRad[1] &&
               rpyBiasRad[2] == o.rpyBiasRad[2] &&
               rpyRateRadPerS[0] == o.rpyRateRadPerS[0] && rpyRateRadPerS[1] == o.rpyRateRadPerS[1] &&
               rpyRateRadPerS[2] == o.rpyRateRadPerS[2] &&
               rpyTRefS == o.rpyTRefS && rpyFrame == o.rpyFrame &&
               stride == o.stride && cellLocateMethod == o.cellLocateMethod &&
               useLocalCellGuess == o.useLocalCellGuess && device == o.device &&
               dtypeGeo == o.dtypeGeo && dtypePixel == o.dtypePixel && dtypeMaps == o.dtypeMaps &&
               resampleMode == o.resampleMode && preview == o.preview;
    }
    bool operator!=(const LiveConfigUpdate& o) const { return !(*this == o); }
};

struct SceneInfo {
    QVector<QPointF> cornerLatLons;  // lon, lat
    QVector<QPointF> cornerXY;       // east, north
    QVector<double> bbox;           // [min_east, min_north, max_east, max_north]
    int H{0};
    int W{0};
    double gsdM{10.0};
    double baseResolutionM{10.0};
    double minResolutionM{10.0};     // finest allowable sampling distance (m/px)
    QStringList bandIds;
    QVector<int> rgbPreference;
    QVector<double> geotransform;
    int epsg{0};
    QString bandDtype{QStringLiteral("float32")};
    double nodataValue{0.0};
    std::vector<BandHistogram> histograms;
    // The session's CURRENT effective RPY/config state (see
    // trims.live.session.LiveGeorefSession.effective_config_payload's doc
    // comment) -- present so a (re)connecting client can restore its
    // controls to match what the server is actually running with, rather
    // than showing defaults regardless of what a previous, now-disconnected
    // client had configured. hasCurrentConfig is false only against an
    // older server that doesn't send this field at all; currentConfig.preview
    // is meaningless here (never set from the wire) and should be ignored.
    bool hasCurrentConfig{false};
    LiveConfigUpdate currentConfig;
};
Q_DECLARE_METATYPE(SceneInfo)

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
class LiveGeorefSession : public QObject {
    Q_OBJECT
public:
    // location: "grpc://host:port", matching TrimsFlightServer's bind address.
    explicit LiveGeorefSession(QString location, QObject* parent = nullptr);
    ~LiveGeorefSession() override;

    // Connects and sends the initial handshake message.
    // Call startReading() after setting up signal connections to begin
    // the background reader thread without missing initial events.
    bool connectToServer(QString* errorOut = nullptr);
    bool startReading();
    void disconnectFromServer();

    bool isConnected() const { return m_connected.load(); }
    int currentGeneration() const { return m_generation.load(); }

    // Bumps the generation counter, serializes `update` to a config_update
    // app_metadata JSON message, and writes it on the shared DoExchange stream.
    int sendConfigUpdate(const LiveConfigUpdate& update);

    // Requests georeferencing of a specific ROI bounding box and resolution.
    int sendExtentRequest(const QVector<double>& bbox, double resolutionM = 0.0);

signals:
    void sceneInfoReceived(const SceneInfo& info);
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
