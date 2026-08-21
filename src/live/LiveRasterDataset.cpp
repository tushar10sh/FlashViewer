#include "live/LiveRasterDataset.hpp"
#include "live/LiveGeorefSession.hpp"
#include "io/RasterDataset.hpp"
#include "util/Logger.hpp"

#include <ogr_spatialref.h>
#include <cpl_string.h>
#include <cpl_conv.h>

#include <cstdio>
#include <cstring>

std::shared_ptr<LiveRasterDataset> LiveRasterDataset::create(std::shared_ptr<LiveGeorefSession> session) {
    std::shared_ptr<LiveRasterDataset> self(new LiveRasterDataset(std::move(session)));
    // Qt::QueuedConnection explicitly, NOT AutoConnection: LiveGeorefSession
    // emits from a raw std::thread (readLoop), never moved to a real
    // QThread, so its QObject affinity is still whatever thread constructed
    // it (the GUI thread) -- Qt's AutoConnection compares sender/receiver
    // AFFINITY, not the calling thread, sees "same thread" here, and picks
    // DirectConnection. Without this, onStarted/onTileReceived run
    // SYNCHRONOUSLY on the background reader thread, touching GDAL/Qt
    // objects outside their normal thread-safety assumptions -- see
    // MainWindow::openLiveSession's matching fix for the full symptom this
    // caused (rendering only working after a user-driven, correctly-
    // main-thread zoom interaction).
    connect(self->m_session.get(), &LiveGeorefSession::started,
            self.get(), &LiveRasterDataset::onStarted, Qt::QueuedConnection);
    connect(self->m_session.get(), &LiveGeorefSession::tileReceived,
            self.get(), &LiveRasterDataset::onTileReceived, Qt::QueuedConnection);
    connect(self->m_session.get(), &LiveGeorefSession::errorOccurred,
            self.get(), &LiveRasterDataset::sessionError, Qt::QueuedConnection);
    return self;
}

LiveRasterDataset::LiveRasterDataset(std::shared_ptr<LiveGeorefSession> session)
    : m_session(std::move(session)) {}

LiveRasterDataset::~LiveRasterDataset() = default;

void LiveRasterDataset::onStarted(int H, int W, QStringList bandIds, QVector<double> geotransform,
                                   int epsg, QString bandDtype, double nodataValue, int generation) {
    std::lock_guard<std::mutex> lock(m_buffer_mutex);
    m_H = H;
    m_W = W;
    m_bandIds = std::move(bandIds);
    m_bandDtype = std::move(bandDtype);
    m_generation = generation;

    const size_t bands = static_cast<size_t>(m_bandIds.size());
    if (H <= 0 || W <= 0 || bands == 0) {
        FV_WARN("LiveRasterDataset: invalid raster shape {}x{}x{} from live session", bands, H, W);
        return;
    }
    const size_t nElems = bands * static_cast<size_t>(H) * static_cast<size_t>(W);
    const bool isU16 = (m_bandDtype == QStringLiteral("uint16"));

    // GDAL's documented MEM driver in-memory-buffer open syntax: wraps
    // caller-owned memory (no copy) as a raster GDALOpenEx can open by
    // string, exactly like RasterDataset::open() already expects -- see
    // GDAL's "MEM" driver docs, "Create a MEM dataset handle from a memory
    // buffer". BSQ layout: PIXELOFFSET = one element (4 bytes Float32, 2
    // bytes UInt16), LINEOFFSET = one row, BANDOFFSET = one full band plane
    // -- matches the BSQ layout onTileReceived() writes into the buffer.
    char ptrStr[32];
    long long pixelOffset, lineOffset, bandOffset;
    const char* gdalType;
    if (isU16) {
        m_bufferU16.assign(nElems, uint16_t{0});
        m_buffer.clear();
        std::snprintf(ptrStr, sizeof(ptrStr), "%p", static_cast<void*>(m_bufferU16.data()));
        pixelOffset = static_cast<long long>(sizeof(uint16_t));
        gdalType = "UInt16";
    } else {
        m_buffer.assign(nElems, 0.0f);
        m_bufferU16.clear();
        std::snprintf(ptrStr, sizeof(ptrStr), "%p", static_cast<void*>(m_buffer.data()));
        pixelOffset = static_cast<long long>(sizeof(float));
        gdalType = "Float32";
    }
    lineOffset = pixelOffset * W;
    bandOffset = lineOffset * H;
    const std::string path = QString(
        "MEM:::DATAPOINTER=%1,PIXELS=%2,LINES=%3,BANDS=%4,DATATYPE=%5,"
        "PIXELOFFSET=%6,LINEOFFSET=%7,BANDOFFSET=%8")
        .arg(ptrStr).arg(W).arg(H).arg(bands).arg(gdalType)
        .arg(pixelOffset).arg(lineOffset).arg(bandOffset)
        .toStdString();

    CPLSetThreadLocalConfigOption("GDAL_MEM_ENABLE_OPEN", "YES");
    m_dataset = RasterDataset::open(path);
    if (!m_dataset) {
        FV_WARN("LiveRasterDataset: RasterDataset::open() failed for MEM path '{}'", path);
        return;
    }

    if (geotransform.size() == 6) {
        double gt[6];
        for (int i = 0; i < 6; ++i) gt[i] = geotransform[i];
        m_dataset->setGeoTransformOverride(gt);
    }
    // RasterDataset has no setCrsOverride(int epsg) -- only setCrsOverride(wkt)
    // -- translate via OGR, same as the coordinate-assignment dialog (FR-IO-9)
    // already does for geoloc-warped datasets missing a native CRS.
    if (epsg > 0) {
        OGRSpatialReference srs;
        if (srs.importFromEPSG(epsg) == OGRERR_NONE) {
            char* wkt = nullptr;
            if (srs.exportToWkt(&wkt) == OGRERR_NONE && wkt) {
                m_dataset->setCrsOverride(wkt);
            }
            if (wkt) CPLFree(wkt);
        } else {
            FV_WARN("LiveRasterDataset: OGR could not resolve EPSG:{}", epsg);
        }
    }

    // Out-of-swath pixels are real (~1/3 of a typical rotated-swath scene's
    // north-up bounding box) and otherwise get treated as valid nodataValue
    // (usually 0.0) data in every stat/stretch/render computation -- see
    // RasterDataset::setNoDataOverride's doc comment.
    m_dataset->setNoDataOverride(nodataValue);

    emit ready();
}

void LiveRasterDataset::onTileReceived(LiveTile tile, int generation) {
    if (generation != m_generation) return;  // stale generation -- see plan's cancellation design

    std::lock_guard<std::mutex> lock(m_buffer_mutex);
    const int h = tile.row1 - tile.row0;
    if (m_dataset == nullptr || h <= 0 || tile.width != m_W || tile.row1 > m_H) {
        FV_WARN("LiveRasterDataset: dropping tile rows [{},{}) -- shape mismatch or not ready",
                tile.row0, tile.row1);
        return;
    }

    if (tile.bandDtype != m_bandDtype) {
        FV_WARN("LiveRasterDataset: dropping tile rows [{},{}) -- bandDtype '{}' != session '{}'",
                tile.row0, tile.row1, tile.bandDtype.toStdString(), m_bandDtype.toStdString());
        return;
    }
    const bool isU16 = (m_bandDtype == QStringLiteral("uint16"));

    for (int b = 0; b < tile.bandIds.size() && b < m_bandIds.size(); ++b) {
        // Bands are matched by position, not name -- the server always
        // builds ArrowTileWriter with the same band_ids/ordering for the
        // lifetime of one do_exchange session (see flight_server.py), so
        // tile.bandIds[b] == m_bandIds[b] is expected to hold; a mismatch
        // here would indicate a server-side band-set change mid-session,
        // which isn't a supported live-session transition.
        if (isU16) {
            const uint16_t* src = tile.imageU16.data() + static_cast<size_t>(b) * h * tile.width;
            uint16_t* dst = m_bufferU16.data()
                + (static_cast<size_t>(b) * m_H + tile.row0) * static_cast<size_t>(m_W);
            std::memcpy(dst, src, static_cast<size_t>(h) * tile.width * sizeof(uint16_t));
        } else {
            const float* src = tile.image.data() + static_cast<size_t>(b) * h * tile.width;
            float* dst = m_buffer.data()
                + (static_cast<size_t>(b) * m_H + tile.row0) * static_cast<size_t>(m_W);
            std::memcpy(dst, src, static_cast<size_t>(h) * tile.width * sizeof(float));
        }
    }

    emit regionUpdated(tile.row0, tile.row1);
}
