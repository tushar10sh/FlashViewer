#include "io/BinaryImportDialog.hpp"
#include "widgets/UiKit.hpp"          // FvTickCheckBox, fvMakeSection

#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDialogButtonBox>
#include <QSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QTextEdit>
#include <QLabel>
#include <QPushButton>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <fstream>
#include <sstream>
#include <iomanip>

BinaryImportDialog::BinaryImportDialog(const QString& file_path, QWidget* parent)
    : QDialog(parent)
    , m_file_path(file_path)
{
    setupUi(file_path);
    setWindowTitle(tr("Import Binary Raster — %1")
                       .arg(QFileInfo(file_path).fileName()));
    resize(560, 560);
    updateHexPreview();
    validate();
}

void BinaryImportDialog::setupUi(const QString& /*file_path*/) {
    auto* mainLay = new QVBoxLayout(this);

    // Section frames, not QGroupBoxes — a group box lays its title out inside the widget's
    // top margin, so the heading collided with the frame border. fvMakeSection puts the
    // heading inside the frame, which rules the collision out by construction.
    QVBoxLayout* paramInner = nullptr;
    auto* paramBox = fvMakeSection(tr("Image Parameters"), paramInner, this);
    auto* form     = new QFormLayout();

    m_lines_spin = new QSpinBox(paramBox);
    m_lines_spin->setRange(1, 1 << 24);
    m_lines_spin->setValue(512);
    form->addRow(tr("Lines (rows):"), m_lines_spin);

    m_samples_spin = new QSpinBox(paramBox);
    m_samples_spin->setRange(1, 1 << 24);
    m_samples_spin->setValue(512);
    form->addRow(tr("Samples (cols):"), m_samples_spin);

    m_bands_spin = new QSpinBox(paramBox);
    m_bands_spin->setRange(1, 1024);
    m_bands_spin->setValue(1);
    form->addRow(tr("Bands:"), m_bands_spin);

    m_dtype_combo = new QComboBox(paramBox);
    m_dtype_combo->addItems({"float32", "float64", "int16", "uint16",
                               "int32", "uint32", "uint8"});
    form->addRow(tr("Data type:"), m_dtype_combo);

    m_interleave_combo = new QComboBox(paramBox);
    m_interleave_combo->addItems({"BSQ", "BIL", "BIP"});
    form->addRow(tr("Interleave:"), m_interleave_combo);

    m_header_spin = new QSpinBox(paramBox);
    m_header_spin->setRange(0, 1 << 20);
    m_header_spin->setValue(0);
    m_header_spin->setSuffix(tr(" bytes"));
    form->addRow(tr("Header offset:"), m_header_spin);

    m_big_endian_chk = new FvTickCheckBox(tr("Big-endian byte order"), paramBox);
    form->addRow(QString(), m_big_endian_chk);

    paramInner->addLayout(form);
    mainLay->addWidget(paramBox);

    QVBoxLayout* hexLay = nullptr;
    auto* hexBox = fvMakeSection(tr("File Preview (first 256 bytes)"), hexLay, this);
    m_hex_preview = new QTextEdit(hexBox);
    m_hex_preview->setReadOnly(true);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSize(8);
    m_hex_preview->setFont(mono);
    m_hex_preview->setFixedHeight(120);
    hexLay->addWidget(m_hex_preview);
    mainLay->addWidget(hexBox);

    auto* btns = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(btns, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLay->addWidget(btns);

    connect(m_lines_spin,   QOverload<int>::of(&QSpinBox::valueChanged),
            this, &BinaryImportDialog::validate);
    connect(m_samples_spin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &BinaryImportDialog::validate);
    connect(m_header_spin,  QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this]{ updateHexPreview(); validate(); });
}

void BinaryImportDialog::validate() {
    // Nothing to disable
}

void BinaryImportDialog::updateHexPreview() {
    std::ifstream f(m_file_path.toStdString(), std::ios::binary);
    if (!f) { m_hex_preview->setText(tr("(cannot open file)")); return; }

    int skip = m_header_spin ? m_header_spin->value() : 0;
    f.seekg(skip);
    char buf[256];
    f.read(buf, sizeof(buf));
    auto n = static_cast<int>(f.gcount());

    std::ostringstream ss;
    for (int i = 0; i < n; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0')
           << (static_cast<unsigned int>(buf[i]) & 0xFF) << ' ';
        if ((i + 1) % 16 == 0) ss << '\n';
    }
    m_hex_preview->setText(QString::fromStdString(ss.str()));
}

BinaryRasterSpec BinaryImportDialog::spec() const {
    BinaryRasterSpec s;
    s.file_path   = m_file_path.toStdString();
    s.lines       = m_lines_spin->value();
    s.samples     = m_samples_spin->value();
    s.bands       = m_bands_spin->value();
    s.dtype       = m_dtype_combo->currentText().toStdString();
    s.header_offset = m_header_spin->value();
    s.big_endian  = m_big_endian_chk->isChecked();

    const QString il = m_interleave_combo->currentText();
    if      (il == "BIL") s.interleave = BilInterleave::BIL;
    else if (il == "BIP") s.interleave = BilInterleave::BIP;
    else                  s.interleave = BilInterleave::BSQ;

    return s;
}
