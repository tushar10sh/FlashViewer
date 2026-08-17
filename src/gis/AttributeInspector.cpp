#include "gis/AttributeInspector.hpp"
#include "gis/PixelSampler.hpp"
#include "gis/CrsUtil.hpp"
#include "core/LayerManager.hpp"
#include "core/RasterLayer.hpp"
#include "core/Layer.hpp"
#include "io/RasterDataset.hpp"
#include "util/Logger.hpp"

#include <QApplication>
#include <QEvent>
#include <QVariant>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QToolButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QLabel>
#include <cmath>
#include <limits>

AttributeInspector::AttributeInspector(QWidget* parent) : QWidget(parent) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(4,4,4,4);

    m_coord_label = new QLabel(tr("Click on map to inspect pixel values"), this);
    m_coord_label->setAlignment(Qt::AlignCenter);
    lay->addWidget(m_coord_label);

    // Scrollable stack of per-pane collapsible drop-downs (Phase 6.8).
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_groups = new QWidget(scroll);
    m_groups_layout = new QVBoxLayout(m_groups);
    m_groups_layout->setContentsMargins(0,0,0,0);
    m_groups_layout->setSpacing(6);
    m_groups_layout->addStretch(1);   // keep sections top-aligned
    scroll->setWidget(m_groups);
    lay->addWidget(scroll, 1);
}

void AttributeInspector::setLayerManager(LayerManager* mgr) {
    m_mgr = mgr;
}

void AttributeInspector::clearGroups() {
    // Remove every widget the stretch pushes down, leaving the trailing stretch item.
    while (m_groups_layout->count() > 1) {
        QLayoutItem* it = m_groups_layout->takeAt(0);
        if (it->widget()) it->widget()->deleteLater();
        delete it;
    }
}

void AttributeInspector::styleHeader(QToolButton* header, const QColor& paneColor) {
    const QColor c = paneColor.isValid() ? paneColor : palette().highlight().color();
    // Theme-compliant foreground (readable over the translucent pane-colour fill):
    // near-black in light themes, near-white in dark themes (matches PaneChrome).
    const bool isDark = QApplication::palette().window().color().lightness() < 128;
    const QString fg = isDark ? "#e6edf3" : "#1f2328";
    header->setStyleSheet(QString(
        "QToolButton { color: %4; text-align: left; padding: 4px 6px;"
        " border: 1px solid rgba(%1,%2,%3,120); border-radius: 4px;"
        " background: rgba(%1,%2,%3,55); }")
        .arg(c.red()).arg(c.green()).arg(c.blue()).arg(fg));
}

void AttributeInspector::changeEvent(QEvent* e) {
    // The header foreground depends on the theme; a palette/style switch must re-style the
    // live drop-down headers immediately (not only on the next inspect click).
    if (m_groups && (e->type() == QEvent::PaletteChange || e->type() == QEvent::StyleChange)) {
        const auto headers = m_groups->findChildren<QToolButton*>();
        for (auto* h : headers)
            styleHeader(h, h->property("fvPaneColor").value<QColor>());
    }
    QWidget::changeEvent(e);
}

void AttributeInspector::inspectGroups(double geo_x, double geo_y, const std::string& geoWkt,
                                       const QVector<InspectPaneGroup>& groups) {
    auto fmt = fvFormatCoordinates(geo_x, geo_y, geoWkt);
    m_coord_label->setText(fmt.multi_line);

    clearGroups();

    int shown = 0;
    for (const auto& g : groups) {
        // Sample this pane's layers; skip the whole group if nothing lands in-bounds.
        std::vector<std::pair<QString, std::vector<double>>> rows;
        for (const auto& e : g.layers) {
            std::vector<double> vals;
            if (fvSamplePixelBands(e.layer, geo_x, geo_y, geoWkt, vals))
                rows.emplace_back(e.name, std::move(vals));
        }
        if (rows.empty()) continue;

        // Header: pane name, black text on translucent pane colour (matches the LayerPanel
        // row fill, paneColor@55). QToolButton toggles the content below it.
        auto* section = new QWidget(m_groups);
        section->setProperty("fvPaneId", QVariant::fromValue<qulonglong>(g.paneId));
        auto* col = new QVBoxLayout(section);
        col->setContentsMargins(0,0,0,0);
        col->setSpacing(2);

        auto* header = new QToolButton(section);
        header->setText(g.paneLabel);
        header->setCheckable(true);
        header->setChecked(true);
        header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        header->setArrowType(Qt::DownArrow);
        header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        header->setProperty("fvPaneColor", g.paneColor);   // for re-styling on theme switch
        styleHeader(header, g.paneColor);
        col->addWidget(header);

        // Content: compact Layer/Band/Value table sized to its rows (no inner scrollbar;
        // the outer QScrollArea handles overflow).
        auto* table = new QTableWidget(section);
        table->setColumnCount(3);
        table->setHorizontalHeaderLabels({tr("Layer"), tr("Band"), tr("Value")});
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        table->verticalHeader()->setVisible(false);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

        for (const auto& [name, vals] : rows) {
            for (int b = 0; b < static_cast<int>(vals.size()); ++b) {
                int r = table->rowCount();
                table->insertRow(r);
                table->setItem(r, 0, new QTableWidgetItem(b == 0 ? name : QString()));
                table->setItem(r, 1, new QTableWidgetItem(QString("Band %1").arg(b+1)));
                double v = vals[static_cast<size_t>(b)];
                QString valStr = std::isnan(v) ? tr("N/A") : QString::number(v, 'g', 8);
                table->setItem(r, 2, new QTableWidgetItem(valStr));
            }
        }
        // Fixed height = header + all rows (no scrolling within a section).
        table->resizeRowsToContents();
        int h = table->horizontalHeader()->height() + 2;
        for (int r = 0; r < table->rowCount(); ++r) h += table->rowHeight(r);
        table->setFixedHeight(h);
        col->addWidget(table);

        QObject::connect(header, &QToolButton::toggled, table, [header, table](bool on) {
            table->setVisible(on);
            header->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        });

        m_groups_layout->insertWidget(m_groups_layout->count() - 1, section);
        ++shown;
    }

    if (shown == 0)
        m_coord_label->setText(fmt.multi_line + "\n(" + tr("No data at this location") + ")");
}

void AttributeInspector::removePaneGroup(uint64_t paneId) {
    for (int i = 0; i < m_groups_layout->count(); ++i) {
        QLayoutItem* it = m_groups_layout->itemAt(i);
        QWidget* w = it ? it->widget() : nullptr;
        if (w && w->property("fvPaneId").toULongLong() == paneId) {
            QLayoutItem* taken = m_groups_layout->takeAt(i);
            taken->widget()->deleteLater();
            delete taken;
            // If only the trailing stretch remains, no pane groups are shown.
            if (m_groups_layout->count() <= 1)
                m_coord_label->setText(tr("Click on map to inspect pixel values"));
            return;
        }
    }
}
