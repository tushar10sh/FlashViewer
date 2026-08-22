#include <arrow/api.h>
#include <arrow/flight/api.h>

#include "live/LiveGeorefSession.hpp"
#include "util/Logger.hpp"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {

// Trivial 1-column schema/batch used for the handshake message and for any
// control-only message (config_update) that carries no real columnar
// payload -- Arrow Flight requires SOME RecordBatch per message even when
// the actual content lives in app_metadata. Mirrors
// trims.live.flight_server._empty_batch's counterpart on the Python side.
std::shared_ptr<arrow::Schema> dummySchema() {
    static auto schema = arrow::schema({arrow::field("_", arrow::int8())});
    return schema;
}

std::shared_ptr<arrow::RecordBatch> dummyBatch() {
    auto schema = dummySchema();
    arrow::Int8Builder builder;
    auto status = builder.Append(0);
    (void)status;
    std::shared_ptr<arrow::Array> arr;
    status = builder.Finish(&arr);
    (void)status;
    return arrow::RecordBatch::Make(schema, 1, {arr});
}

// Extract a flat (num_rows * width) vector from a List<T> or FixedSizeList<T> column,
// assuming no nulls (the Python writer never produces any). `T` is the
// Arrow value C type (float, double); `ArrowArrayT` its typed array class.
template <typename ArrowArrayT, typename T>
std::vector<T> extractListColumn(const std::shared_ptr<arrow::Array>& column) {
    if (!column || column->length() == 0) return {};
    if (column->type_id() == arrow::Type::FIXED_SIZE_LIST) {
        auto fsl = std::static_pointer_cast<arrow::FixedSizeListArray>(column);
        auto values = std::dynamic_pointer_cast<ArrowArrayT>(fsl->values());
        if (!values) return {};
        const int64_t list_size = fsl->list_type()->list_size();
        const int64_t n = fsl->length() * list_size;
        const T* raw = values->raw_values();
        if (!raw) return {};
        const int64_t offset = fsl->offset() * list_size;
        return std::vector<T>(raw + offset, raw + offset + n);
    } else if (column->type_id() == arrow::Type::LIST) {
        auto list_arr = std::static_pointer_cast<arrow::ListArray>(column);
        auto values = std::dynamic_pointer_cast<ArrowArrayT>(list_arr->values());
        if (!values) return {};
        const int64_t offset0 = list_arr->value_offset(0);
        const int64_t offset1 = list_arr->value_offset(list_arr->length());
        const T* raw = values->raw_values();
        if (!raw) return {};
        return std::vector<T>(raw + offset0, raw + offset1);
    }
    return {};
}

std::vector<float> extractBandColumn(const std::shared_ptr<arrow::Array>& column) {
    return extractListColumn<arrow::FloatArray, float>(column);
}

std::vector<uint16_t> extractBandColumnU16(const std::shared_ptr<arrow::Array>& column) {
    return extractListColumn<arrow::UInt16Array, uint16_t>(column);
}

std::vector<double> extractDoubleListColumn(const std::shared_ptr<arrow::Array>& column) {
    return extractListColumn<arrow::DoubleArray, double>(column);
}

// Like json::value<double>(key, fallback), but also falls back (instead of
// throwing a type_error) when the field is present but JSON `null` -- e.g.
// trims.live.progress_bridge.FlightProgressBar emits `"eta_s": null` on its
// very first tick, before any rate estimate exists (see
// FlightProgressBar._emit's `eta_s = ... if rate > 0 else None`). Plain
// json::value() only substitutes the fallback for an ABSENT key; a present-
// but-null key still tries (and fails) to convert null to double.
double optionalDouble(const json& meta, const char* key, double fallback) {
    if (!meta.contains(key) || meta[key].is_null()) return fallback;
    return meta.value(key, fallback);
}

// line_src/sample_src are float64 OR float32 depending on the session's
// negotiated dtype_maps (see LiveConfigUpdate::dtypeMaps / ArrowTileWriter's
// build_schema) -- always widen to double for LiveTile so callers don't need
// to know which was in effect.
std::vector<double> extractMapColumn(const std::shared_ptr<arrow::Array>& column) {
    if (!column || column->length() == 0) return {};
    arrow::Type::type val_type = arrow::Type::NA;
    if (column->type_id() == arrow::Type::FIXED_SIZE_LIST) {
        val_type = std::static_pointer_cast<arrow::FixedSizeListType>(column->type())->value_type()->id();
    } else if (column->type_id() == arrow::Type::LIST) {
        val_type = std::static_pointer_cast<arrow::ListType>(column->type())->value_type()->id();
    }
    if (val_type == arrow::Type::FLOAT) {
        auto floats = extractListColumn<arrow::FloatArray, float>(column);
        return std::vector<double>(floats.begin(), floats.end());
    } else if (val_type == arrow::Type::DOUBLE) {
        return extractListColumn<arrow::DoubleArray, double>(column);
    }
    return {};
}

}  // namespace

LiveGeorefSession::LiveGeorefSession(QString location, QObject* parent)
    : QObject(parent), m_location(std::move(location)) {}

LiveGeorefSession::~LiveGeorefSession() {
    disconnectFromServer();
}

bool LiveGeorefSession::connectToServer(QString* errorOut) {
    auto loc_result = arrow::flight::Location::Parse(m_location.toStdString());
    if (!loc_result.ok()) {
        if (errorOut) *errorOut = QString::fromStdString(loc_result.status().ToString());
        return false;
    }

    auto client_result = arrow::flight::FlightClient::Connect(loc_result.ValueOrDie());
    if (!client_result.ok()) {
        if (errorOut) *errorOut = QString::fromStdString(client_result.status().ToString());
        return false;
    }
    m_client = std::move(client_result.ValueOrDie());

    auto descriptor = arrow::flight::FlightDescriptor::Command("live-georef-session");
    auto exchange_res = m_client->DoExchange(descriptor);
    if (!exchange_res.ok()) {
        if (errorOut) *errorOut = QString::fromStdString(exchange_res.status().ToString());
        return false;
    }
    auto exchange_val = std::move(exchange_res).ValueUnsafe();
    m_writer = std::move(exchange_val.writer);
    m_reader = std::move(exchange_val.reader);

    // Handshake: TrimsFlightServer.do_exchange() reads (and currently
    // ignores) one initial message before it starts writing "start"/"tile"
    // batches -- see flight_server.py's module docstring for what this
    // grows into once live config_update handling lands server-side.
    auto status = m_writer->Begin(dummySchema());
    if (status.ok()) status = m_writer->WriteRecordBatch(*dummyBatch());
    if (!status.ok()) {
        if (errorOut) *errorOut = QString::fromStdString(status.ToString());
        return false;
    }

    m_connected.store(true);
    m_stop.store(false);
    return true;
}

bool LiveGeorefSession::startReading() {
    if (!m_connected.load() || m_reader_thread.joinable()) return false;
    m_stop.store(false);
    m_reader_thread = std::thread(&LiveGeorefSession::readLoop, this);
    return true;
}

void LiveGeorefSession::disconnectFromServer() {
    m_stop.store(true);
    {
        std::lock_guard<std::mutex> lock(m_write_mutex);
        if (m_writer) {
            auto status = m_writer->DoneWriting();
            (void)status;
        }
    }
    // DoneWriting() only half-closes OUR write side; the server (see
    // TrimsFlightServer._serve_live_generation) can otherwise keep
    // computing/streaming an in-flight generation for a long time (a full
    // real scene is dozens of multi-second tiles), during which
    // m_reader_thread stays blocked inside m_reader->Next() waiting for the
    // next chunk. Since this method runs synchronously on the caller's
    // thread (the GUI thread, for the Cancel button -- see
    // MainWindow::openLiveSession's QProgressDialog::canceled handler), that
    // join() below would otherwise block the whole UI for as long as the
    // server keeps producing data. Cancel() aborts the underlying call so
    // Next() returns (with a non-ok/cancelled status) immediately.
    if (m_reader) {
        m_reader->Cancel();
    }
    if (m_reader_thread.joinable()) m_reader_thread.join();
    m_writer.reset();
    m_reader.reset();
    m_client.reset();
    m_connected.store(false);
}

int LiveGeorefSession::sendConfigUpdate(const LiveConfigUpdate& update) {
    const int generation = m_generation.fetch_add(1) + 1;

    json payload = {
        {"type", "config_update"},
        {"generation", generation},
        {"preview", update.preview},
        {"rpy_bias_rad", {update.rpyBiasRad[0], update.rpyBiasRad[1], update.rpyBiasRad[2]}},
        {"rpy_rate_rad_per_s", {update.rpyRateRadPerS[0], update.rpyRateRadPerS[1], update.rpyRateRadPerS[2]}},
        {"rpy_t_ref_s", update.rpyTRefS},
        {"rpy_frame", update.rpyFrame.toStdString()},
        {"stride", update.stride},
        {"cell_locate_method", update.cellLocateMethod.toStdString()},
        {"use_local_cell_guess", update.useLocalCellGuess},
        {"device", update.device.toStdString()},
        {"dtype_geo", update.dtypeGeo.toStdString()},
        {"dtype_pixel", update.dtypePixel.toStdString()},
        {"dtype_maps", update.dtypeMaps.toStdString()},
        {"resample_mode", update.resampleMode.toStdString()},
    };
    const std::string body = payload.dump();
    auto metadata = arrow::Buffer::FromString(body);

    std::lock_guard<std::mutex> lock(m_write_mutex);
    if (!m_writer) return generation;
    auto status = m_writer->WriteWithMetadata(*dummyBatch(), metadata);
    if (!status.ok()) {
        FV_WARN("LiveGeorefSession::sendConfigUpdate write failed: {}", status.ToString());
    }
    return generation;
}

int LiveGeorefSession::sendExtentRequest(const QVector<double>& bbox, double resolutionM) {
    const int generation = m_generation.fetch_add(1) + 1;
    json payload = {
        {"type", "request_extent"},
        {"generation", generation},
    };
    if (bbox.size() == 4) {
        payload["bbox"] = {bbox[0], bbox[1], bbox[2], bbox[3]};
    }
    if (resolutionM > 0.0) {
        payload["resolution_m"] = resolutionM;
    }
    const std::string body = payload.dump();
    auto metadata = arrow::Buffer::FromString(body);

    std::lock_guard<std::mutex> lock(m_write_mutex);
    if (!m_writer) return generation;
    auto status = m_writer->WriteWithMetadata(*dummyBatch(), metadata);
    if (!status.ok()) {
        FV_WARN("LiveGeorefSession::sendExtentRequest write failed: {}", status.ToString());
    }
    return generation;
}

void LiveGeorefSession::readLoop() {
    while (!m_stop.load()) {
        auto chunk_res = m_reader->Next();
        if (!chunk_res.ok()) {
            // A deliberate disconnectFromServer() call sets m_stop and then
            // calls m_reader->Cancel() to unblock exactly this Next() call --
            // that's an expected, user-initiated stop, not a real session
            // error, so don't pop an error dialog for it (see
            // disconnectFromServer()'s comment).
            if (!m_stop.load()) {
                emit errorOccurred(QString::fromStdString(chunk_res.status().ToString()));
            }
            break;
        }
        auto chunk = std::move(chunk_res).ValueUnsafe();
        // End of stream: Arrow signals this with a chunk carrying neither
        // data nor app_metadata.
        if (chunk.data == nullptr && chunk.app_metadata == nullptr) break;

        json meta;
        if (chunk.app_metadata) {
            try {
                meta = json::parse(chunk.app_metadata->ToString());
            } catch (const json::parse_error& e) {
                FV_WARN("LiveGeorefSession: malformed app_metadata: {}", e.what());
                continue;
            }
        }
        // Everything below reads app_metadata fields via json::value<T>(),
        // which throws a type_error (not caught by the parse_error handler
        // above) if a present field's actual JSON type doesn't match T --
        // e.g. a null where a number was expected (see optionalDouble's doc
        // comment for a real example). readLoop() runs on a bare std::thread,
        // so an uncaught exception here calls std::terminate() and aborts
        // the whole process instead of just dropping one malformed message.
        try {

        const std::string type = meta.value("type", "");
        const int generation = meta.value("generation", 0);

        if (type == "scene_info") {
            SceneInfo info;
            info.H = meta.value("H", 0);
            info.W = meta.value("W", 0);
            info.gsdM = meta.value("gsd_m", 10.0);
            info.baseResolutionM = meta.value("base_resolution_m", info.gsdM);
            info.minResolutionM = meta.value("min_resolution_m", info.baseResolutionM);
            info.epsg = meta.value("epsg", 0);
            info.bandDtype = QString::fromStdString(meta.value("band_dtype", "float32"));
            info.nodataValue = meta.value("nodata_value", 0.0);
            for (const auto& b : meta.value("band_ids", std::vector<std::string>{}))
                info.bandIds << QString::fromStdString(b);
            for (int idx : meta.value("rgb_preference", std::vector<int>{}))
                info.rgbPreference << idx;
            for (double v : meta.value("geotransform", std::vector<double>{}))
                info.geotransform << v;
            for (double v : meta.value("bbox", std::vector<double>{}))
                info.bbox << v;

            if (meta.contains("corner_latlons") && meta["corner_latlons"].is_array()) {
                for (const auto& pt : meta["corner_latlons"]) {
                    if (pt.is_array() && pt.size() >= 2) {
                        info.cornerLatLons.append(QPointF(pt[1].get<double>(), pt[0].get<double>()));
                    }
                }
            }
            if (meta.contains("corner_xy") && meta["corner_xy"].is_array()) {
                for (const auto& pt : meta["corner_xy"]) {
                    if (pt.is_array() && pt.size() >= 2) {
                        info.cornerXY.append(QPointF(pt[0].get<double>(), pt[1].get<double>()));
                    }
                }
            }
            if (meta.contains("histograms") && meta["histograms"].is_array()) {
                for (const auto& h : meta["histograms"]) {
                    BandHistogram hist;
                    hist.minVal = h.value("min", 0.0);
                    hist.maxVal = h.value("max", 1.0);
                    hist.meanVal = h.value("mean", 0.0);
                    hist.stdVal = h.value("std", 1.0);
                    hist.counts = h.value("counts", std::vector<int64_t>{});
                    hist.binEdges = h.value("bin_edges", std::vector<double>{});
                    info.histograms.push_back(hist);
                }
            }
            if (meta.contains("current_config") && meta["current_config"].is_object()) {
                const auto& cc = meta["current_config"];
                LiveConfigUpdate& lc = info.currentConfig;
                auto bias = cc.value("rpy_bias_rad", std::vector<double>{0.0, 0.0, 0.0});
                auto rate = cc.value("rpy_rate_rad_per_s", std::vector<double>{0.0, 0.0, 0.0});
                for (int i = 0; i < 3 && i < static_cast<int>(bias.size()); ++i) lc.rpyBiasRad[i] = bias[i];
                for (int i = 0; i < 3 && i < static_cast<int>(rate.size()); ++i) lc.rpyRateRadPerS[i] = rate[i];
                lc.rpyTRefS = cc.value("rpy_t_ref_s", 0.0);
                lc.rpyFrame = QString::fromStdString(cc.value("rpy_frame", "hill"));
                lc.stride = cc.value("stride", 32);
                lc.cellLocateMethod = QString::fromStdString(cc.value("cell_locate_method", "affine_index"));
                lc.useLocalCellGuess = cc.value("use_local_cell_guess", false);
                lc.device = QString::fromStdString(cc.value("device", "cpu"));
                lc.dtypeGeo = QString::fromStdString(cc.value("dtype_geo", "float64"));
                lc.dtypePixel = QString::fromStdString(cc.value("dtype_pixel", "float32"));
                lc.dtypeMaps = QString::fromStdString(cc.value("dtype_maps", "float64"));
                lc.resampleMode = QString::fromStdString(cc.value("resample_mode", "bicubic"));
                lc.preview = false;   // not meaningful here -- never set from the wire
                info.hasCurrentConfig = true;
            }
            m_band_dtype = info.bandDtype;
            emit sceneInfoReceived(info);
        } else if (type == "start") {
            QStringList bandIds;
            for (const auto& b : meta.value("band_ids", std::vector<std::string>{}))
                bandIds << QString::fromStdString(b);
            QVector<double> gt;
            for (double v : meta.value("geotransform", std::vector<double>{})) gt << v;
            m_band_dtype = QString::fromStdString(meta.value("band_dtype", "float32"));
            emit started(meta.value("H", 0), meta.value("W", 0), bandIds, gt,
                         meta.value("epsg", 0), m_band_dtype,
                         meta.value("nodata_value", 0.0), generation);
        } else if (type == "tile" && chunk.data && chunk.data->num_rows() > 0) {
            LiveTile tile;
            tile.row0 = meta.value("row0", 0);
            tile.row1 = meta.value("row1", 0);
            tile.width = 0;
            int latIdx = chunk.data->schema()->GetFieldIndex("lat");
            if (latIdx >= 0) {
                auto col = chunk.data->column(latIdx);
                if (col->type_id() == arrow::Type::FIXED_SIZE_LIST) {
                    tile.width = static_cast<int>(std::static_pointer_cast<arrow::FixedSizeListArray>(col)->list_type()->list_size());
                } else if (col->type_id() == arrow::Type::LIST) {
                    auto la = std::static_pointer_cast<arrow::ListArray>(col);
                    tile.width = static_cast<int>(la->value_length(0));
                }
            }
            if (tile.width <= 0) continue;

            for (const auto& field : chunk.data->schema()->fields()) {
                if (field->name().rfind("band_", 0) == 0)
                    tile.bandIds << QString::fromStdString(field->name().substr(5));
            }
            for (const auto& bandId : tile.bandIds) {
                auto col = chunk.data->GetColumnByName("band_" + bandId.toStdString());
                if (!col) continue;
                arrow::Type::type val_type = arrow::Type::NA;
                if (col->type_id() == arrow::Type::FIXED_SIZE_LIST) {
                    val_type = std::static_pointer_cast<arrow::FixedSizeListType>(col->type())->value_type()->id();
                } else if (col->type_id() == arrow::Type::LIST) {
                    val_type = std::static_pointer_cast<arrow::ListType>(col->type())->value_type()->id();
                }
                if (val_type == arrow::Type::UINT16) {
                    tile.bandDtype = QStringLiteral("uint16");
                    auto band_data = extractBandColumnU16(col);
                    tile.imageU16.insert(tile.imageU16.end(), band_data.begin(), band_data.end());
                } else {
                    tile.bandDtype = QStringLiteral("float32");
                    auto band_data = extractBandColumn(col);
                    tile.image.insert(tile.image.end(), band_data.begin(), band_data.end());
                }
            }
            tile.lineSrc = extractMapColumn(chunk.data->GetColumnByName("line_src"));
            tile.sampleSrc = extractMapColumn(chunk.data->GetColumnByName("sample_src"));
            tile.lat = extractDoubleListColumn(chunk.data->GetColumnByName("lat"));
            tile.lon = extractDoubleListColumn(chunk.data->GetColumnByName("lon"));
            emit tileReceived(tile, generation);
        } else if (type == "progress") {
            emit progressUpdated(meta.value("current", 0), meta.value("total", 0),
                                  QString::fromStdString(meta.value("stage", "")),
                                  optionalDouble(meta, "eta_s", -1.0), generation);
        } else if (type == "done") {
            emit finished(generation);
        }

        } catch (const std::exception& e) {
            // Deliberately not re-touching `meta` here (e.g. re-reading
            // "type") -- the exception below is proof at least one field
            // didn't have the JSON type this code assumed, so any further
            // json::value() call in this handler carries the same risk.
            FV_WARN("LiveGeorefSession: dropping malformed app_metadata message: {}", e.what());
        }
    }
}
