#include "gis/ScaleBar.hpp"
#include "gis/GeoScale.hpp"
#include <QPainter>
#include <QPaintEvent>
#include <QFont>
#include <QFontMetrics>
#include <algorithm>
#include <cmath>

ScaleBar::ScaleBar(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFixedHeight(40);
    setFixedWidth(160);  // initial placeholder; overwritten on first update()
}

double ScaleBar::niceDistance(double units_per_px, int maxPx) const {
    return fvNiceDistance(units_per_px, maxPx);   // shared with the status readout (GeoScale.hpp)
}

void ScaleBar::update(const Camera& camera, bool is_geographic) {
    double units_per_px = camera.scale();
    if (units_per_px <= 0) { m_valid = false; QWidget::update(); return; }

    const int kMaxBarPx = 140;
    double dist = niceDistance(units_per_px, kMaxBarPx);
    m_scale_px = dist / units_per_px;
    m_scale_value = dist;

    if (is_geographic) {
        // Geographic Project CRS → show the CRS's native angular unit (degrees), stepping to
        // arcminutes/arcseconds for small spans so the number stays legible. `dist` is the
        // nice-rounded span already in degrees (the camera's units).
        if (dist >= 1.0)
            m_scale_label = QString("%1°").arg(dist, 0, 'g', 3);
        else if (dist * 60.0 >= 1.0)
            m_scale_label = QString("%1′").arg(dist * 60.0, 0, 'g', 3);
        else
            m_scale_label = QString("%1″").arg(dist * 3600.0, 0, 'g', 3);
    } else {
        if (dist >= 1e6)
            m_scale_label = QString("%1 Mm").arg(dist / 1e6, 0, 'g', 3);
        else if (dist >= 1000)
            m_scale_label = QString("%1 km").arg(dist / 1000.0, 0, 'g', 3);
        else
            m_scale_label = QString("%1 m").arg(dist, 0, 'g', 3);
    }
    m_valid = true;
    {
        constexpr int kPad = 8;
        QFont f = this->font();
        f.setPointSize(8);
        f.setBold(true);
        QFontMetrics fm(f);
        int contentW = std::max(static_cast<int>(m_scale_px),
                                fm.horizontalAdvance(m_scale_label));
        setFixedWidth(2 * kPad + contentW);
    }
    QWidget::update();
}

void ScaleBar::paintEvent(QPaintEvent*) {
    if (!m_valid) return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const int kPad  = 8;   // even border padding on all four sides
    const int kBarH = 8;   // tick height
    const int kGap  = 3;   // gap between ruler bottom and text top

    QFont font = this->font();
    font.setPointSize(8);
    font.setBold(true);
    p.setFont(font);
    QFontMetrics fm(font);

    p.fillRect(rect(), QColor(0, 0, 0, 90));

    // Ruler positioned in the upper portion with kPad top padding
    const int barY = kPad + kBarH / 2;
    const int x0   = kPad;
    const int x1   = x0 + static_cast<int>(m_scale_px);

    p.setPen(QPen(Qt::white, 1.5));
    p.drawLine(x0, barY - kBarH / 2, x0, barY + kBarH / 2);  // left tick
    p.drawLine(x1, barY - kBarH / 2, x1, barY + kBarH / 2);  // right tick
    p.drawLine(x0, barY,             x1, barY);               // horizontal bar

    // Label centered horizontally beneath the bar
    const int barW   = static_cast<int>(m_scale_px);
    const int labelW = fm.horizontalAdvance(m_scale_label);
    const int labelX = std::max(kPad, x0 + (barW - labelW) / 2);
    const int labelY = barY + kBarH / 2 + kGap + fm.ascent();
    p.setPen(Qt::white);
    p.drawText(labelX, labelY, m_scale_label);
}
