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

// Extract a flat (num_rows * width) vector from a FixedSizeList<T> column,
// assuming no nulls (the Python writer never produces any). `T` is the
// Arrow value C type (float, double); `ArrowArrayT` its typed array class.
template <typename ArrowArrayT, typename T>
std::vector<T> extractFixedSizeListColumn(const std::shared_ptr<arrow::Array>& column) {
    auto fsl = std::static_pointer_cast<arrow::FixedSizeListArray>(column);
    auto values = std::static_pointer_cast<ArrowArrayT>(fsl->values());
    const int64_t list_size = fsl->list_type()->list_size();
    const int64_t n = fsl->length() * list_size;
    const T* raw = values->raw_values();
    // FixedSizeListArray::values() already accounts for the parent array's
    // offset (each element's flat values start at fsl->offset() * list_size),
    // so the flat values for THIS batch's rows begin here:
    const int64_t offset = fsl->offset() * list_size;
    return std::vector<T>(raw + offset, raw + offset + n);
}

std::vector<float> extractBandColumn(const std::shared_ptr<arrow::Array>& column) {
    return extractFixedSizeListColumn<arrow::FloatArray, float>(column);
}

std::vector<uint16_t> extractBandColumnU16(const std::shared_ptr<arrow::Array>& column) {
    return extractFixedSizeListColumn<arrow::UInt16Array, uint16_t>(column);
}

std::vector<double> extractDoubleListColumn(const std::shared_ptr<arrow::Array>& column) {
    return extractFixedSizeListColumn<arrow::DoubleArray, double>(column);
}

// line_src/sample_src are float64 OR float32 depending on the session's
// negotiated dtype_maps (see LiveConfigUpdate::dtypeMaps / ArrowTileWriter's
// build_schema) -- always widen to double for LiveTile so callers don't need
// to know which was in effect.
std::vector<double> extractMapColumn(const std::shared_ptr<arrow::Array>& column) {
    auto fsl = std::static_pointer_cast<arrow::FixedSizeListArray>(column);
    if (fsl->value_type()->id() == arrow::Type::FLOAT) {
        auto floats = extractFixedSizeListColumn<arrow::FloatArray, float>(column);
        return std::vector<double>(floats.begin(), floats.end());
    }
    return extractFixedSizeListColumn<arrow::DoubleArray, double>(column);
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

void LiveGeorefSession::readLoop() {
    while (!m_stop.load()) {
        auto chunk_res = m_reader->Next();
        if (!chunk_res.ok()) {
            emit errorOccurred(QString::fromStdString(chunk_res.status().ToString()));
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
        const std::string type = meta.value("type", "");
        const int generation = meta.value("generation", 0);

        if (type == "start") {
            QStringList bandIds;
            for (const auto& b : meta.value("band_ids", std::vector<std::string>{}))
                bandIds << QString::fromStdString(b);
            QVector<double> gt;
            for (double v : meta.value("geotransform", std::vector<double>{})) gt << v;
            // Absent on an older server (pre-band_dtype/nodata_value):
            // defaults match ArrowTileWriter's own default and
            // GeoreferencerConfig.output_nodata_value's default (0.0), so
            // behavior against an older server is unchanged.
            m_band_dtype = QString::fromStdString(meta.value("band_dtype", "float32"));
            emit started(meta.value("H", 0), meta.value("W", 0), bandIds, gt,
                         meta.value("epsg", 0), m_band_dtype,
                         meta.value("nodata_value", 0.0), generation);
        } else if (type == "tile" && chunk.data) {
            LiveTile tile;
            tile.row0 = meta.value("row0", 0);
            tile.row1 = meta.value("row1", 0);
            tile.bandDtype = m_band_dtype;
            tile.width = chunk.data->num_rows() > 0
                ? static_cast<int>(std::static_pointer_cast<arrow::FixedSizeListArray>(
                      chunk.data->column(chunk.data->schema()->GetFieldIndex("lat")))
                      ->list_type()->list_size())
                : 0;
            for (const auto& field : chunk.data->schema()->fields()) {
                if (field->name().rfind("band_", 0) == 0)
                    tile.bandIds << QString::fromStdString(field->name().substr(5));
            }
            const int h = static_cast<int>(chunk.data->num_rows());
            const bool isU16 = (tile.bandDtype == QStringLiteral("uint16"));
            if (isU16) tile.imageU16.reserve(static_cast<size_t>(tile.bandIds.size()) * h * tile.width);
            else       tile.image.reserve(static_cast<size_t>(tile.bandIds.size()) * h * tile.width);
            for (const auto& bandId : tile.bandIds) {
                auto col = chunk.data->GetColumnByName("band_" + bandId.toStdString());
                if (isU16) {
                    auto band_data = extractBandColumnU16(col);
                    tile.imageU16.insert(tile.imageU16.end(), band_data.begin(), band_data.end());
                } else {
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
                                  meta.value("eta_s", -1.0), generation);
        } else if (type == "done") {
            emit finished(generation);
        }
    }
}
