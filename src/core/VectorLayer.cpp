#include "core/VectorLayer.hpp"
#include <QFileInfo>

VectorLayer::VectorLayer(std::shared_ptr<VectorDataset> ds)
    : m_layer_id(++s_next_id)
    , m_ds(std::move(ds))
{
    if (m_ds) {
        QString baseName = QFileInfo(QString::fromStdString(m_ds->filePath())).completeBaseName();
        if (!baseName.isEmpty()) {
            setName(baseName);
        } else {
            setName(QString::fromStdString(m_ds->layerName()));
        }
    }
}
