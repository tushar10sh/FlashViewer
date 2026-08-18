#include "app/SettingsDialog.hpp"
#include "app/Settings.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QTabWidget>
#include <QGroupBox>
#include <QComboBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QLabel>

SettingsDialog::SettingsDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Preferences"));
    resize(480, 360);
    setupUi();
    loadValues();
}

void SettingsDialog::setupUi() {
    auto* mainLay = new QVBoxLayout(this);
    mainLay->setContentsMargins(12, 12, 12, 12);
    mainLay->setSpacing(10);

    auto* tabs = new QTabWidget(this);

    // Tab 1: Appearance & Display
    auto* appTab = new QWidget(tabs);
    auto* appLay = new QVBoxLayout(appTab);
    appLay->setContentsMargins(10, 10, 10, 10);
    appLay->setSpacing(8);

    auto* appForm = new QFormLayout();
    m_theme_combo = new QComboBox(appTab);
    m_theme_combo->addItem(tr("Dark Theme"), static_cast<int>(Theme::Dark));
    m_theme_combo->addItem(tr("Light Theme"), static_cast<int>(Theme::Light));
    appForm->addRow(tr("Theme:"), m_theme_combo);

    m_chk_perf_hud = new QCheckBox(tr("Show Performance HUD on canvas"), appTab);
    appForm->addRow(tr("HUD:"), m_chk_perf_hud);

    m_chk_plot_grid = new QCheckBox(tr("Show grid lines on plots"), appTab);
    appForm->addRow(tr("Plot Grid:"), m_chk_plot_grid);

    m_chk_legend_coords = new QCheckBox(tr("Show coordinates in Spectral Plot legend"), appTab);
    appForm->addRow(tr("Legend:"), m_chk_legend_coords);

    appLay->addLayout(appForm);
    appLay->addStretch();
    tabs->addTab(appTab, tr("General"));

    // Tab 2: Analysis & Matrix
    auto* numTab = new QWidget(tabs);
    auto* numLay = new QVBoxLayout(numTab);
    numLay->setContentsMargins(10, 10, 10, 10);
    numLay->setSpacing(8);

    auto* numGroup = new QGroupBox(tr("Numeric Dump Matrix"), numTab);
    auto* numForm = new QFormLayout(numGroup);

    m_spin_dump_font_size = new QSpinBox(numGroup);
    m_spin_dump_font_size->setRange(6, 16);
    m_spin_dump_font_size->setValue(8);
    m_spin_dump_font_size->setSuffix(tr(" pt"));
    numForm->addRow(tr("Cell Font Size:"), m_spin_dump_font_size);

    auto* noteLbl = new QLabel(
        tr("Values adapt color dynamically:\n"
           "• Dark Theme: Light silver/off-white for single band, bright RGB channels.\n"
           "• Light Theme: Dark black for single band, dark RGB channels."), numGroup);
    noteLbl->setStyleSheet("color: gray; font-size: 11px;");
    numForm->addRow(noteLbl);

    numLay->addWidget(numGroup);
    numLay->addStretch();
    tabs->addTab(numTab, tr("Numeric Dump"));

    // Tab 3: Rendering & Basemap
    auto* renderTab = new QWidget(tabs);
    auto* renderLay = new QVBoxLayout(renderTab);
    renderLay->setContentsMargins(10, 10, 10, 10);
    renderLay->setSpacing(8);

    auto* renderForm = new QFormLayout();
    m_combo_resampling = new QComboBox(renderTab);
    m_combo_resampling->addItem(tr("Bilinear (Fast)"), 0);
    m_combo_resampling->addItem(tr("Bicubic Smooth (B-Spline)"), 1);
    m_combo_resampling->addItem(tr("Bicubic Sharp (Catmull-Rom)"), 2);
    renderForm->addRow(tr("Default Resampling:"), m_combo_resampling);

    auto* osmRow = new QWidget(renderTab);
    auto* osmLay = new QHBoxLayout(osmRow);
    osmLay->setContentsMargins(0, 0, 0, 0);
    osmLay->setSpacing(6);

    m_txt_osm_url = new QLineEdit(osmRow);
    m_txt_osm_url->setPlaceholderText("https://tile.openstreetmap.org/{z}/{x}/{y}.png");
    osmLay->addWidget(m_txt_osm_url, 1);

    auto* btnResetOsm = new QPushButton(tr("Default"), osmRow);
    btnResetOsm->setToolTip(tr("Reset to OpenStreetMap default tile server"));
    connect(btnResetOsm, &QPushButton::clicked, this, [this] {
        m_txt_osm_url->setText(QStringLiteral("https://tile.openstreetmap.org/{z}/{x}/{y}.png"));
    });
    osmLay->addWidget(btnResetOsm);

    renderForm->addRow(tr("OSM Tile URL:"), osmRow);

    auto* osmHint = new QLabel(
        tr("URL template for map tiles. Standard variables: {z} (zoom), {x} (column), {y} (row).\n"
           "Example: https://tile.openstreetmap.org/{z}/{x}/{y}.png"), renderTab);
    osmHint->setStyleSheet("color: gray; font-size: 11px;");
    renderForm->addRow(osmHint);

    renderLay->addLayout(renderForm);
    renderLay->addStretch();
    tabs->addTab(renderTab, tr("Rendering"));

    mainLay->addWidget(tabs, 1);

    // Dialog Button Box
    auto* btnBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
    connect(btnBox->button(QDialogButtonBox::Ok), &QPushButton::clicked, this, [this] {
        applySettings();
        accept();
    });
    connect(btnBox->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, &SettingsDialog::applySettings);
    connect(btnBox->button(QDialogButtonBox::Cancel), &QPushButton::clicked, this, &QDialog::reject);
    mainLay->addWidget(btnBox);
}

void SettingsDialog::loadValues() {
    auto& s = Settings::instance();
    int themeIdx = m_theme_combo->findData(static_cast<int>(s.theme()));
    if (themeIdx >= 0) m_theme_combo->setCurrentIndex(themeIdx);

    m_chk_perf_hud->setChecked(s.perfHudVisible());
    m_chk_plot_grid->setChecked(s.plotGrid());
    m_chk_legend_coords->setChecked(s.legendCoords());

    m_spin_dump_font_size->setValue(s.numericDumpFontSize());

    int resampIdx = m_combo_resampling->findData(s.displayResampling());
    if (resampIdx >= 0) m_combo_resampling->setCurrentIndex(resampIdx);

    m_txt_osm_url->setText(s.osmTileUrl());
}

void SettingsDialog::applySettings() {
    auto& s = Settings::instance();
    Theme oldTheme = s.theme();
    Theme newTheme = static_cast<Theme>(m_theme_combo->currentData().toInt());
    s.setTheme(newTheme);

    s.setPerfHudVisible(m_chk_perf_hud->isChecked());
    s.setPlotGrid(m_chk_plot_grid->isChecked());
    s.setLegendCoords(m_chk_legend_coords->isChecked());

    s.setNumericDumpFontSize(m_spin_dump_font_size->value());
    s.setDisplayResampling(m_combo_resampling->currentData().toInt());

    if (!m_txt_osm_url->text().trimmed().isEmpty()) {
        s.setOsmTileUrl(m_txt_osm_url->text().trimmed());
    }

    if (oldTheme != newTheme) {
        emit themeChanged();
    }
    emit settingsApplied();
}
