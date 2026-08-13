#include "widgets/ChartTools.hpp"
#include "widgets/SvgIconButton.hpp"
#include "widgets/UiKit.hpp"   // fvPaintCurveSwatch, kFvCurveSwatchW/H
#include "util/Logger.hpp"

#include <QtCharts/QChart>
#include <QtCharts/QValueAxis>

#include <QApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QFileDialog>
#include <QFileInfo>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleValidator>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QSaveFile>
#include <QSvgGenerator>
#include <QScrollArea>
#include <QToolButton>
#include <QResizeEvent>
#include <QStringList>
#include <QSvgRenderer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <cmath>
#include <functional>
#include <utility>

// ---------------------------------------------------------------------------
// FvChartView
// ---------------------------------------------------------------------------

FvChartView::FvChartView(QChart* chart, QWidget* parent) : QChartView(chart, parent) {
    setRenderHint(QPainter::Antialiasing);
    setMouseTracking(true);
    // The gesture is "drag anywhere in the plot to pan", so the whole view advertises it.
    setCursor(Qt::OpenHandCursor);
}

void FvChartView::captureHome() {
    m_home.clear();
    if (!chart()) return;
    for (auto* ax : chart()->axes())
        if (auto* va = qobject_cast<QValueAxis*>(ax))
            m_home.insert(va, Home{va->min(), va->max(), true});
    emitViewChanged();
}

void FvChartView::emitViewChanged() {
    bool atHome = true;
    if (chart()) {
        for (auto* ax : chart()->axes()) {
            auto* va = qobject_cast<QValueAxis*>(ax);
            if (!va) continue;
            auto it = m_home.constFind(va);
            if (it == m_home.constEnd() || !it->valid) continue;
            // Compare against the span, not an absolute epsilon: the axes carry raw data
            // values, which for a projected CRS or a radiance band are large numbers where
            // a fixed epsilon would report "moved" for every chart.
            const double span = std::abs(it->max - it->min);
            const double tol  = span > 0.0 ? span * 1e-9 : 1e-12;
            if (std::abs(va->min() - it->min) > tol || std::abs(va->max() - it->max) > tol) {
                atHome = false;
                break;
            }
        }
    }
    emit viewChanged(atHome);
}

void FvChartView::zoomBy(double factor) {
    if (!chart() || factor <= 0.0) return;
    for (auto* ax : chart()->axes()) {
        auto* va = qobject_cast<QValueAxis*>(ax);
        if (!va) continue;
        const double c    = (va->min() + va->max()) * 0.5;
        const double half = (va->max() - va->min()) * 0.5 * factor;
        // A degenerate span cannot be zoomed out of, and zooming in past the double's
        // resolution around `c` would collapse the axis to a single value.
        if (!std::isfinite(half) || half <= 0.0) continue;
        va->setRange(c - half, c + half);
    }
    emitViewChanged();
}

void FvChartView::resetZoom() {
    if (!chart()) return;
    for (auto* ax : chart()->axes()) {
        auto* va = qobject_cast<QValueAxis*>(ax);
        if (!va) continue;
        auto it = m_home.constFind(va);
        if (it != m_home.constEnd() && it->valid) va->setRange(it->min, it->max);
    }
    emitViewChanged();
}

void FvChartView::panByPixels(double dx, double dy) {
    if (!chart()) return;
    const QRectF plot = chart()->plotArea();
    if (plot.width() <= 0.0 || plot.height() <= 0.0) return;

    for (auto* ax : chart()->axes()) {
        auto* va = qobject_cast<QValueAxis*>(ax);
        if (!va) continue;
        const double span = va->max() - va->min();
        if (!std::isfinite(span) || span == 0.0) continue;

        const bool horizontal = chart()->axes(Qt::Horizontal).contains(ax);
        // Drag right ⇒ the data follows the cursor, so the visible window moves LEFT.
        // The Y axis grows upward while screen y grows downward, hence the sign flip.
        const double shift = horizontal ? -span * (dx / plot.width())
                                        :  span * (dy / plot.height());
        va->setRange(va->min() + shift, va->max() + shift);
    }
    emitViewChanged();
}

void FvChartView::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        m_dragging = true;
        m_last_pos = e->position();
        setCursor(Qt::ClosedHandCursor);
        e->accept();
        return;
    }
    QChartView::mousePressEvent(e);
}

void FvChartView::mouseMoveEvent(QMouseEvent* e) {
    if (m_dragging) {
        const QPointF d = e->position() - m_last_pos;
        m_last_pos = e->position();
        panByPixels(d.x(), d.y());
        e->accept();
        return;
    }
    QChartView::mouseMoveEvent(e);
}

void FvChartView::mouseReleaseEvent(QMouseEvent* e) {
    if (m_dragging && e->button() == Qt::LeftButton) {
        m_dragging = false;
        setCursor(Qt::OpenHandCursor);
        e->accept();
        return;
    }
    QChartView::mouseReleaseEvent(e);
}

void FvChartView::wheelEvent(QWheelEvent* e) {
    const int notches = e->angleDelta().y();
    if (notches == 0) { QChartView::wheelEvent(e); return; }
    // Fractional notches (touchpads, hi-res wheels) scale the step rather than rounding to
    // a full notch, so a slow scroll is a slow zoom instead of nothing at all.
    zoomBy(std::pow(kStep, -notches / 120.0));
    e->accept();
}

// ---------------------------------------------------------------------------
// FvChartLegend
// ---------------------------------------------------------------------------

namespace {

// Legend width bounds. The maximum is what keeps the row's buttons on screen (see the
// constructor); the minimum keeps a two-line entry from becoming a column of single letters.
constexpr int kLegendMinWidth = 150;
constexpr int kLegendMaxWidth = 240;

// Shorten any single WORD that cannot fit the column, leaving the rest intact. QLabel's word
// wrap breaks at spaces only, so one long token — a file name is exactly that — would otherwise
// set the row's width and be clipped without any sign that text is missing. Breaking inside the
// name (an earlier attempt) wrapped `Landsat_TOA_BT_100m.tif` across three lines and made the
// legend unreadable, which is the fault this replaces: an ellipsis says "shortened", a
// mid-name break says nothing. The full text always remains in the tooltip.
QString fvFitWords(const QString& text, const QFontMetrics& fm, int width) {
    if (width <= 0) return text;
    QStringList lines;
    for (const QString& line : text.split(QChar::LineFeed)) {
        QStringList words;
        for (const QString& w : line.split(QLatin1Char(' ')))
            words << (fm.horizontalAdvance(w) > width
                          ? fm.elidedText(w, Qt::ElideRight, width) : w);
        lines << words.join(QLatin1Char(' '));
    }
    return lines.join(QChar::LineFeed);
}

// The swatch is painted, not iconised: it has to show a pen STYLE, and QIcon would force a
// pixmap regenerated on every theme and DPI change for no benefit.
class SwatchWidget : public QWidget {
public:
    SwatchWidget(const QColor& c, Qt::PenStyle st, QWidget* parent)
        : QWidget(parent), m_color(c), m_style(st) {
        setFixedSize(kFvCurveSwatchW, kFvCurveSwatchH);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        fvPaintCurveSwatch(&p, rect(), m_color, m_style, palette());
    }
private:
    QColor       m_color;
    Qt::PenStyle m_style;
};

}  // namespace

FvChartLegend::FvChartLegend(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* body = new QWidget(scroll);
    m_rows = new QVBoxLayout(body);
    m_rows->setContentsMargins(2, 2, 2, 2);
    m_rows->setSpacing(6);
    m_rows->addStretch(1);            // keep entries top-aligned
    scroll->setWidget(body);
    outer->addWidget(scroll);

    // Wide enough for a wrapped "«pane» / «layer»" line without swallowing the plot, and
    // CAPPED so it cannot claim more. Without the cap a row's preferred width is the label's
    // one-line width, which for a legend entry is the whole "Pane 1 / file.tif (x, y)" string:
    // the legend then asked for hundreds of pixels, overran the panel, and pushed its own ✎
    // and 🗑 off the right-hand edge — with the horizontal scrollbar off, they simply vanished.
    setMinimumWidth(kLegendMinWidth);
    setMaximumWidth(kLegendMaxWidth);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

    // A legend is an annotation, not body text: at 90% it reads as subordinate to the plot and
    // fits appreciably more of a file name per line, which is the whole difficulty here. Set on
    // the legend, so every row inherits it and fvFitWords measures the size actually drawn.
    QFont f = font();
    if (f.pointSizeF() > 0.0)  f.setPointSizeF(f.pointSizeF() * 0.9);
    else if (f.pixelSize() > 0) f.setPixelSize(qMax(1, qRound(f.pixelSize() * 0.9)));
    setFont(f);
}

void FvChartLegend::setEntries(const QVector<FvLegendEntry>& entries) {
    m_entries = entries;
    rebuild();
}

void FvChartLegend::changeEvent(QEvent* e) {
    // The swatch border and the ✎ icon are palette-dependent.
    if (e->type() == QEvent::PaletteChange || e->type() == QEvent::StyleChange) rebuild();
    QWidget::changeEvent(e);
}

int FvChartLegend::labelWidth() const {
    // What a row's label may occupy: the column, less its margins, the swatch and the gap.
    const int w = width() > 0 ? width() : kLegendMaxWidth;
    return w - 4 /*body margins*/ - kFvCurveSwatchW - 6 /*row spacing*/ - 4;
}

void FvChartLegend::rebuild() {
    while (m_rows->count() > 1) {                 // leave the trailing stretch
        QLayoutItem* it = m_rows->takeAt(0);
        if (it->widget()) it->widget()->deleteLater();
        delete it;
    }

    const QFontMetrics fm(font());
    const int avail = labelWidth();

    for (const FvLegendEntry& e : m_entries) {
        auto* row = new QWidget(this);
        auto* lay = new QHBoxLayout(row);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(6);

        auto* sw = new SwatchWidget(e.color, e.style, row);
        // Top-aligned: a two-line label must not drag the swatch to its centre, or the rows
        // stop lining up with each other.
        lay->addWidget(sw, 0, Qt::AlignTop);

        auto* label = new QLabel(fvFitWords(e.text, fm, avail), row);
        label->setWordWrap(true);
        label->setToolTip(e.text);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        // Ignored horizontally: the label takes whatever the row has left rather than demanding
        // its own preferred width — which, for a wrapping label, is its ONE-LINE width.
        // heightForWidth must be re-armed by hand: setWordWrap() sets it on the policy, and
        // assigning a fresh policy here would drop it, clipping a two-line entry to one.
        QSizePolicy sp(QSizePolicy::Ignored, QSizePolicy::Minimum);
        sp.setHeightForWidth(true);
        label->setSizePolicy(sp);
        lay->addWidget(label, 1);

        m_rows->insertWidget(m_rows->count() - 1, row);
    }
}

void FvChartLegend::deleteEntry(int index) {
    if (index >= 0 && index < m_entries.size()) emit entryDeleted(index);
}

void FvChartLegend::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    // The elision depends on the column's width, so a resize has to re-measure. Only on a
    // WIDTH change: a vertical resize cannot alter what fits on a line.
    if (e->oldSize().width() != e->size().width() && !m_entries.isEmpty()) rebuild();
}

// ---------------------------------------------------------------------------
// fvApplyAxisTicks
// ---------------------------------------------------------------------------

void fvApplyAxisTicks(QValueAxis* axis, const FvAxisTicks& t) {
    if (!axis || !t.manual) return;
    const double lo = std::min(t.start, t.end);
    const double hi = std::max(t.start, t.end);
    axis->setRange(lo, hi);
    // Direction lives in `reverse`, never in an inverted range: QValueAxis expects min < max,
    // and so does every zoom/pan calculation in FvChartView.
    axis->setReverse(t.end < t.start);
    if (t.step == 0.0) return;                       // range pinned, tick spacing left to Qt

    axis->setTickType(QValueAxis::TicksDynamic);
    axis->setTickAnchor(lo);
    axis->setTickInterval(std::abs(t.step));
    // "%d" would print 1.5 and 2.0 as the same label. Only widen the format when the step is
    // actually fractional, so a band axis keeps its integer ticks.
    if (std::abs(t.step - std::round(t.step)) > 1e-12
        && axis->labelFormat() == QLatin1String("%d"))
        axis->setLabelFormat(QStringLiteral("%g"));
}

// ---------------------------------------------------------------------------
// fvEditPlotLabels
// ---------------------------------------------------------------------------

namespace {

// A section header that folds its contents away. The dialog has three sections and a plot can
// carry dozens of curves, so "every row visible" needs somewhere to put the ones you are not
// working on. QToolButton rather than a styled QLabel: it already draws an arrow, takes focus
// and toggles from the keyboard.
void fvMakeFold(const QString& text, QWidget* body, QVBoxLayout* into, bool open) {
    auto* head = new QToolButton(body->parentWidget());
    head->setText(text);
    head->setCheckable(true);
    head->setChecked(open);
    head->setAutoRaise(true);
    head->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    head->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    QObject::connect(head, &QToolButton::toggled, body, [head, body](bool on) {
        head->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        body->setVisible(on);
    });
    body->setVisible(open);
    into->addWidget(head);
    into->addWidget(body);
}

// One row of the Titles section: Show + caption + the field.
struct TitleRow {
    QCheckBox* show{nullptr};
    QLineEdit* edit{nullptr};
};

TitleRow fvAddTitleRow(QFormLayout* form, QWidget* parent, const FvLabelSpec& spec) {
    TitleRow r;
    auto* line = new QWidget(parent);
    auto* lay  = new QHBoxLayout(line);
    lay->setContentsMargins(0, 0, 0, 0);
    r.show = new FvTickCheckBox(QObject::tr("Show"), line);
    r.show->setChecked(spec.shown);
    r.edit = new QLineEdit(spec.text, line);
    // The automatic text as placeholder: it shows what clearing the field restores, so Reset
    // needs no separate control and cannot fall out of step with the real default.
    r.edit->setPlaceholderText(spec.automatic);
    r.edit->setMinimumWidth(280);
    r.edit->setEnabled(spec.shown);
    QObject::connect(r.show, &QCheckBox::toggled, r.edit, &QLineEdit::setEnabled);
    lay->addWidget(r.show);
    lay->addWidget(r.edit, 1);
    form->addRow(spec.caption, line);
    return r;
}

// One axis's three fields. Blank means "not set" -- QLineEdit rather than QDoubleSpinBox,
// because a spin box has no empty state to mean "automatic" with.
struct TickRow {
    QLineEdit *start{nullptr}, *end{nullptr}, *step{nullptr};
};

TickRow fvAddTickRow(QFormLayout* form, QWidget* parent, const QString& caption,
                     const FvAxisTicks& t) {
    TickRow r;
    auto* line = new QWidget(parent);
    auto* lay  = new QHBoxLayout(line);
    lay->setContentsMargins(0, 0, 0, 0);
    auto mk = [&](const QString& ph) {
        auto* e = new QLineEdit(line);
        e->setPlaceholderText(ph);
        e->setValidator(new QDoubleValidator(e));
        e->setMaximumWidth(90);
        return e;
    };
    r.start = mk(QObject::tr("start"));
    r.end   = mk(QObject::tr("end"));
    r.step  = mk(QObject::tr("step"));
    if (t.manual) {
        r.start->setText(QString::number(t.start, 'g', 10));
        r.end->setText(QString::number(t.end, 'g', 10));
        if (t.step != 0.0) r.step->setText(QString::number(t.step, 'g', 10));
    }
    lay->addWidget(r.start);
    lay->addWidget(r.end);
    lay->addWidget(r.step);
    lay->addStretch(1);
    form->addRow(caption, line);
    return r;
}

// Read one axis's fields. False (with `err` filled) when the three do not describe an axis.
bool fvReadTicks(const TickRow& row, const QString& axis, FvAxisTicks& out, QString& err) {
    const QString s = row.start->text().trimmed();
    const QString e = row.end->text().trimmed();
    const QString p = row.step->text().trimmed();
    out = FvAxisTicks{};
    if (s.isEmpty() && e.isEmpty() && p.isEmpty()) return true;      // auto-fit, the default
    if (s.isEmpty() || e.isEmpty()) {
        err = QObject::tr("%1: give both a start and an end, or leave all three empty.").arg(axis);
        return false;
    }
    out.manual = true;
    out.start  = s.toDouble();
    out.end    = e.toDouble();
    if (out.start == out.end) {
        err = QObject::tr("%1: start and end must differ.").arg(axis);
        return false;
    }
    if (p.isEmpty()) return true;                                    // range set, ticks auto
    out.step = p.toDouble();
    if (out.step == 0.0) {
        err = QObject::tr("%1: a step of 0 names no interval.").arg(axis);
        return false;
    }
    // The step's SIGN must agree with the direction the range asks for. Reinterpreting it
    // silently would draw an axis the user did not describe; an error says which field is wrong.
    const bool descending = out.end < out.start;
    if (descending != (out.step < 0.0)) {
        err = descending
            ? QObject::tr("%1: end is below start, so the step must be negative.").arg(axis)
            : QObject::tr("%1: end is above start, so the step must be positive.").arg(axis);
        return false;
    }
    return true;
}

}  // namespace

bool fvEditPlotLabels(QWidget* parent, FvPlotLabels& labels) {
    QDialog dlg(parent);
    dlg.setWindowTitle(QObject::tr("Plot Labels"));

    auto* outer = new QVBoxLayout(&dlg);

    // ---- Titles -------------------------------------------------------------
    auto* titles    = new QWidget(&dlg);
    auto* titleForm = new QFormLayout(titles);
    titleForm->setContentsMargins(12, 0, 0, 6);
    const TitleRow rTitle = fvAddTitleRow(titleForm, titles, labels.title);
    const TitleRow rX     = fvAddTitleRow(titleForm, titles, labels.xTitle);
    const TitleRow rY     = fvAddTitleRow(titleForm, titles, labels.yTitle);
    fvMakeFold(QObject::tr("Titles"), titles, outer, true);
    if (!labels.hasPlot) {
        rTitle.show->setEnabled(false);
        rTitle.edit->setEnabled(false);
    }

    // ---- Legend -------------------------------------------------------------
    auto* legendBody = new QWidget(&dlg);
    auto* legendLay  = new QVBoxLayout(legendBody);
    legendLay->setContentsMargins(12, 0, 0, 6);

    struct LegendRowUi {
        QCheckBox*  show{nullptr};
        QLineEdit*  edit{nullptr};
        QToolButton* del{nullptr};
    };
    auto* rows = new QVector<LegendRowUi>();      // owned by the dialog, freed with it
    QObject::connect(&dlg, &QObject::destroyed, [rows] { delete rows; });
    rows->reserve(labels.legend.size());

    for (const FvLabelSpec& spec : labels.legend) {
        auto* row = new QWidget(legendBody);
        auto* lay = new QHBoxLayout(row);
        lay->setContentsMargins(0, 0, 0, 0);

        LegendRowUi ui;
        ui.show = new FvTickCheckBox(QString(), row);
        ui.show->setChecked(spec.shown);
        ui.show->setToolTip(QObject::tr("Show this entry in the legend. The curve is drawn "
                                        "either way."));

        auto* swatch = new QLabel(row);
        swatch->setFixedSize(kFvCurveSwatchW, kFvCurveSwatchH);
        QPixmap pm(kFvCurveSwatchW, kFvCurveSwatchH);
        pm.fill(Qt::transparent);
        {
            QPainter sp(&pm);
            fvPaintCurveSwatch(&sp, QRect(0, 0, kFvCurveSwatchW, kFvCurveSwatchH),
                               spec.color, spec.style, dlg.palette());
        }
        swatch->setPixmap(pm);

        ui.edit = new QLineEdit(spec.text, row);
        ui.edit->setPlaceholderText(spec.automatic);
        ui.edit->setMinimumWidth(260);
        ui.edit->setEnabled(spec.shown);
        QObject::connect(ui.show, &QCheckBox::toggled, ui.edit, &QLineEdit::setEnabled);

        // Checkable, not one-shot: the deletion lands on OK with every other edit, so while the
        // dialog is open it has to be both visible and revocable.
        ui.del = new QToolButton(row);
        ui.del->setText(QObject::tr("Delete"));
        ui.del->setToolTip(QObject::tr("Delete this curve when OK is pressed"));
        ui.del->setCheckable(true);

        lay->addWidget(ui.show);
        lay->addWidget(swatch);
        lay->addWidget(ui.edit, 1);
        lay->addWidget(ui.del);
        legendLay->addWidget(row);
        rows->push_back(ui);

        const int idx = rows->size() - 1;
        QObject::connect(ui.del, &QToolButton::toggled, ui.del, [rows, idx](bool on) {
            LegendRowUi& r = (*rows)[idx];
            QFont f = r.edit->font();
            f.setStrikeOut(on);
            r.edit->setFont(f);
            r.edit->setEnabled(!on && r.show->isChecked());
            r.show->setEnabled(!on);
        });
    }
    if (labels.legend.isEmpty())
        legendLay->addWidget(new QLabel(QObject::tr("Nothing is plotted."), legendBody));
    legendLay->addStretch(1);

    // Dozens of curves would otherwise make a dialog taller than the screen.
    auto* legendScroll = new QScrollArea(&dlg);
    legendScroll->setWidgetResizable(true);
    legendScroll->setFrameShape(QFrame::NoFrame);
    legendScroll->setWidget(legendBody);
    legendScroll->setMaximumHeight(260);
    fvMakeFold(QObject::tr("Legend"), legendScroll, outer, true);

    // ---- Axis ticks ---------------------------------------------------------
    auto* ticks     = new QWidget(&dlg);
    auto* ticksForm = new QFormLayout(ticks);
    ticksForm->setContentsMargins(12, 0, 0, 6);
    const TickRow rXt = fvAddTickRow(ticksForm, ticks, QObject::tr("X axis:"), labels.xTicks);
    const TickRow rYt = fvAddTickRow(ticksForm, ticks, QObject::tr("Y axis:"), labels.yTicks);
    auto* tickNote = new QLabel(QObject::tr(
        "Leave empty to fit automatically. For a descending axis put the higher value in "
        "start and give a negative step."), ticks);
    tickNote->setWordWrap(true);
    ticksForm->addRow(QString(), tickNote);
    fvMakeFold(QObject::tr("Axis ticks"), ticks, outer,
               labels.xTicks.manual || labels.yTicks.manual);

    // ---- Buttons ------------------------------------------------------------
    auto* err = new QLabel(&dlg);
    err->setWordWrap(true);
    err->setStyleSheet(QStringLiteral("color: #c0392b;"));
    err->hide();
    outer->addWidget(err);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    outer->addWidget(buttons);

    FvAxisTicks xt, yt;
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, [&] {
        QString msg;
        // Validate BEFORE accepting: a rejected axis keeps the dialog open with the reason on
        // screen, rather than being dropped on the floor once it has closed.
        if (!fvReadTicks(rXt, QObject::tr("X axis"), xt, msg)
            || !fvReadTicks(rYt, QObject::tr("Y axis"), yt, msg)) {
            err->setText(msg);
            err->show();
            return;
        }
        dlg.accept();
    });

    if (dlg.exec() != QDialog::Accepted) return false;

    auto readTitle = [](const TitleRow& r, FvLabelSpec& spec) {
        spec.shown = r.show->isChecked();
        spec.text  = r.edit->text().trimmed();
    };
    readTitle(rTitle, labels.title);
    readTitle(rX, labels.xTitle);
    readTitle(rY, labels.yTitle);
    for (int i = 0; i < labels.legend.size() && i < rows->size(); ++i) {
        labels.legend[i].shown   = (*rows)[i].show->isChecked();
        labels.legend[i].text    = (*rows)[i].edit->text().trimmed();
        labels.legend[i].deleted = (*rows)[i].del->isChecked();
    }
    labels.xTicks = xt;
    labels.yTicks = yt;
    return true;
}

// ---------------------------------------------------------------------------
// FvChartToolbar
// ---------------------------------------------------------------------------

namespace {

// Rasterize a themed SVG at device resolution — same approach as SvgIconButton, which is
// why the hand hint sits at the same weight as the buttons beside it.
QPixmap renderSvg(const QString& path, int logicalSide, qreal dpr) {
    QSvgRenderer r(path);
    if (!r.isValid()) return {};
    const int px = qMax(1, qRound(logicalSide * dpr));
    QImage img(px, px, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    r.render(&p, QRect(0, 0, px, px));
    p.end();
    img.setDevicePixelRatio(dpr);
    return QPixmap::fromImage(img);
}

}  // namespace

FvChartToolbar::FvChartToolbar(FvChartView* chartView, QWidget* parent)
    : QWidget(parent), m_view(chartView) {
    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(2);

    auto mkButton = [this](const QString& tip) {
        auto* b = new SvgIconButton(this);
        b->setFixedSize(24, 24);
        b->setToolTip(tip);
        m_layout->addWidget(b);
        return b;
    };

    m_zoom_in  = mkButton(tr("Zoom in"));
    m_zoom_out = mkButton(tr("Zoom out"));
    m_home     = mkButton(tr("Reset to the full plot"));

    // The hand is a CAPTION, not a control: panning has no mode to enter — left-drag always
    // pans — so a clickable hand would be a button that does nothing when pressed.
    m_hand = new QLabel(this);
    m_hand->setFixedSize(24, 24);
    m_hand->setAlignment(Qt::AlignCenter);
    m_hand->setToolTip(tr("Drag the plot to pan · scroll to zoom"));
    m_layout->addWidget(m_hand);

    m_edit = mkButton(tr("Edit the titles, the legend and the axis ticks…"));
    m_save = mkButton(tr("Save the plot…"));

    // The icon cluster is one group and stays tight (spacing 2); everything a panel appends
    // after it is a separate control, so it gets real breathing room — see addTrailingWidget.
    m_layout->addSpacing(kFvChartTrailingGap);

    if (m_view) {
        connect(m_zoom_in,  &SvgIconButton::clicked, m_view, &FvChartView::zoomIn);
        connect(m_zoom_out, &SvgIconButton::clicked, m_view, &FvChartView::zoomOut);
        connect(m_home,     &SvgIconButton::clicked, m_view, &FvChartView::resetZoom);
        connect(m_view, &FvChartView::viewChanged,
                this,   [this](bool atHome) { m_home->setEnabled(!atHome); });
        m_home->setEnabled(false);
    }
    // Not inside the view guard: the labels belong to the OWNER, and editing them means
    // nothing to the chart view.
    connect(m_edit, &SvgIconButton::clicked, this, &FvChartToolbar::editLabelsRequested);
    connect(m_save, &SvgIconButton::clicked, this, &FvChartToolbar::save);

    refreshIcons();
}

void FvChartToolbar::setCsvProvider(std::function<QString()> provider) {
    m_csv = std::move(provider);
}

void FvChartToolbar::addTrailingWidget(QWidget* w) {
    if (!w) return;
    // A gap BETWEEN trailing controls, not just before the first: a combo, a checkbox and a
    // push button butted together at the icon row's 2 px read as one crowded strip.
    if (m_has_trailing) m_layout->addSpacing(kFvChartTrailingGap);
    m_layout->addWidget(w);
    m_has_trailing = true;
}

void FvChartToolbar::refreshIcons() {
    const bool isDark = QApplication::palette().window().color().lightness() < 128;
    const QString sfx = isDark ? QStringLiteral("_dark") : QStringLiteral("_light");
    m_zoom_in->setSvgPath(":/icons/zoom_in"  + sfx + ".svg");
    m_zoom_out->setSvgPath(":/icons/zoom_out" + sfx + ".svg");
    m_home->setSvgPath(":/icons/home"        + sfx + ".svg");
    m_edit->setSvgPath(":/icons/pencil"      + sfx + ".svg");
    m_save->setSvgPath(":/icons/save"        + sfx + ".svg");
    m_hand->setPixmap(renderSvg(":/icons/hand" + sfx + ".svg", 18, devicePixelRatioF()));
}

void FvChartToolbar::changeEvent(QEvent* e) {
    if (e->type() == QEvent::PaletteChange || e->type() == QEvent::StyleChange)
        refreshIcons();
    QWidget::changeEvent(e);
}

void FvChartToolbar::save() {
    if (!m_view) return;
    // The legend is a SIBLING widget, not part of the chart view, so exporting the view alone
    // would silently drop it. `m_export` is the container holding both.
    QWidget* src = m_export ? m_export : static_cast<QWidget*>(m_view);

    const QString pngF = tr("PNG image (*.png)");
    const QString svgF = tr("SVG image (*.svg)");
    const QString csvF = tr("CSV data (*.csv)");
    QString filters = pngF + ";;" + svgF;
    if (m_csv) filters += ";;" + csvF;

    QString selected = pngF;
    QString path = QFileDialog::getSaveFileName(
        this, tr("Save Plot"), m_suggested + ".png", filters, &selected);
    if (path.isEmpty()) return;

    // The chosen filter is authoritative for the FORMAT; a missing/foreign extension is
    // appended rather than silently writing PNG bytes into a file named .csv.
    const QString ext = selected == svgF ? QStringLiteral("svg")
                      : selected == csvF ? QStringLiteral("csv")
                                         : QStringLiteral("png");
    if (QFileInfo(path).suffix().compare(ext, Qt::CaseInsensitive) != 0)
        path += "." + ext;

    bool ok = false;
    QString reason;

    if (ext == QLatin1String("csv")) {
        const QString text = m_csv ? m_csv() : QString();
        if (text.isEmpty()) {
            reason = tr("There is nothing plotted to export.");
        } else {
            // QSaveFile: a failed write leaves the previous file intact rather than a
            // truncated one, which matters when overwriting an earlier export.
            QSaveFile f(path);
            if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
                f.write(text.toUtf8());
                ok = f.commit();
                if (!ok) reason = f.errorString();
            } else {
                reason = f.errorString();
            }
        }
    } else if (ext == QLatin1String("svg")) {
        const QSize sz = src->size();
        QSvgGenerator gen;
        gen.setFileName(path);
        gen.setSize(sz);
        gen.setViewBox(QRect(QPoint(0, 0), sz));
        gen.setTitle(m_suggested);
        {
            QPainter p;
            if (p.begin(&gen)) {
                src->render(&p);
                ok = p.end();
            }
        }
        if (!ok) reason = tr("The SVG could not be written.");
    } else {
        // grab() captures exactly what is on screen — current zoom/pan and the live theme.
        ok = src->grab().save(path, "PNG");
        if (!ok) reason = tr("The image could not be written.");
    }

    if (ok) {
        FV_INFO("Saved plot to '{}'", path.toStdString());
    } else {
        FV_WARN("Could not save plot to '{}': {}", path.toStdString(), reason.toStdString());
        QMessageBox::warning(this, tr("Save Plot"),
                             tr("Could not save to\n%1\n\n%2").arg(path, reason));
    }
}
