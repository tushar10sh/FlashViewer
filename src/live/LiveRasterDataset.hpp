#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>
#include <mutex>
#include <vector>

class RasterDataset;
class LiveGeorefSession;
struct LiveTile;

// Bridges a LiveGeorefSession's incoming Arrow tiles into a GDAL MEM-driver
// RasterDataset, so the rest of FlashViewer's pipeline (RasterLayer, the GL
// tile renderer, TileCache) needs NO changes -- the same "produce something
// GDALOpenEx-openable" pattern BinaryRasterParser already uses (a synthesized
// .vrt) rather than a new raster subtype. See the plan doc's "GDAL MEM driver
// dataset" bridge design.
//
// Owns the pixel buffer the MEM dataset directly aliases (GDAL's MEM driver,
// opened via the documented "MEM:::DATAPOINTER=..." in-memory-buffer syntax,
// wraps caller memory without copying it) -- this object must outlive
// dataset() and any RasterLayer built from it.
class LiveRasterDataset : public QObject {
    Q_OBJECT
public:
    // Connects `session` (must not already be connected) and returns a
    // LiveRasterDataset that will allocate its pixel buffer and open the
    // backing MEM dataset once the server's "start" message defines the
    // raster shape. dataset() is null until ready() fires; check
    // session->connectToServer()'s return value for immediate connection
    // failure before relying on ready() firing at all.
    static std::shared_ptr<LiveRasterDataset> create(std::shared_ptr<LiveGeorefSession> session);

    ~LiveRasterDataset() override;

    std::shared_ptr<RasterDataset> dataset() const { return m_dataset; }
    std::shared_ptr<LiveGeorefSession> session() const { return m_session; }

signals:
    // Emitted once, after the MEM dataset is constructed and dataset() is
    // safe to wrap in a RasterLayer (e.g. via LayerManager::addLayer).
    void ready();
    // Emitted after every tile is written into the pixel buffer, with the
    // affected output row range. There is no finer-than-layer invalidation
    // hook in TileCache/LayerManager today (see LayerManager::notifyLayerChanged,
    // index-granularity only) -- callers should connect this to
    // notifyLayerChanged(index) for the layer this dataset backs, at least
    // until/unless a per-region invalidation path is added to TileCache.
    void regionUpdated(int row0, int row1);
    void sessionError(QString message);

private slots:
    void onStarted(int H, int W, QStringList bandIds, QVector<double> geotransform, int epsg,
                    QString bandDtype, double nodataValue, int generation);
    void onTileReceived(LiveTile tile, int generation);

private:
    explicit LiveRasterDataset(std::shared_ptr<LiveGeorefSession> session);

    std::shared_ptr<LiveGeorefSession> m_session;
    std::shared_ptr<RasterDataset> m_dataset;

    // Guards m_buffer/m_bufferU16's CONTENTS against a torn concurrent read
    // from a render-thread RasterIO call while onTileReceived is mid-memcpy;
    // it does NOT make individual reads atomic across that race (a reader
    // may see a stale-but-valid value for a pixel mid-update, never garbage
    // memory, since neither buffer is ever reallocated after onStarted).
    std::mutex m_buffer_mutex;
    // Exactly one of these is allocated, per m_bandDtype -- "float32" (default,
    // matches EngineOutput.output_image) uses m_buffer; "uint16" (real sensor
    // DN, e.g. LISS-3's native 12-bit-in-uint16) uses m_bufferU16. BSQ layout
    // either way: bands * H * W elements, aliased by m_dataset via GDAL's MEM
    // driver at the matching DATATYPE (Float32 | UInt16).
    std::vector<float> m_buffer;
    std::vector<uint16_t> m_bufferU16;
    QString m_bandDtype{QStringLiteral("float32")};
    QStringList m_bandIds;
    int m_H{0};
    int m_W{0};
    int m_generation{0};
};
