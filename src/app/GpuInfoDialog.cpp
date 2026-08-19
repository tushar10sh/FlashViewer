#include "app/GpuInfoDialog.hpp"
#include "util/Logger.hpp"
#include "widgets/UiKit.hpp"          // fvMakeSection

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QProcess>
#include <QCoreApplication>
#include <QDir>
#include <QFile>

// Enumerate GPUs from /sys/class/drm if available (Linux only)
static QStringList enumerateGpus() {
    QStringList gpus;
    QDir drm("/sys/class/drm");
    if (!drm.exists()) return gpus;

    const auto entries = drm.entryList({"card[0-9]*"}, QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto& entry : entries) {
        if (entry.contains('-')) continue;  // skip card0-HDMI-A-1 etc.
        QString uevent = QString("/sys/class/drm/%1/device/uevent").arg(entry);
        QFile f(uevent);
        if (!f.open(QIODevice::ReadOnly)) continue;
        QString pci_id;
        for (const auto& line : f.readAll().split('\n')) {
            QString s = QString::fromLatin1(line).trimmed();
            if (s.startsWith("PCI_ID=") || s.startsWith("PCI_SLOT_NAME="))
                pci_id += s + "  ";
        }
        gpus << QString("%1  [%2]").arg(entry, pci_id.trimmed());
    }
    return gpus;
}

GpuInfoDialog::GpuInfoDialog(const GlInfo& info, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("GPU Information"));
    setMinimumWidth(500);
    buildUi(info);
}

void GpuInfoDialog::buildUi(const GlInfo& info) {
    auto* lay = new QVBoxLayout(this);

    // Shared section frame (widgets/UiKit.hpp) — this lambda used to be a private copy of
    // it; it now just adds the frame to the dialog's layout.
    auto makeSection = [&](const QString& title) -> std::pair<QFrame*, QVBoxLayout*> {
        QVBoxLayout* fl = nullptr;
        auto* frame = fvMakeSection(title, fl, this);
        lay->addWidget(frame);
        return {frame, fl};
    };

    // --- Current GL info ---
    auto [glBox, glLay] = makeSection(tr("Active OpenGL Context"));
    auto* glForm = new QFormLayout();
    glForm->addRow(tr("Renderer:"), new QLabel(info.renderer, glBox));
    glForm->addRow(tr("Vendor:"),   new QLabel(info.vendor,   glBox));
    glForm->addRow(tr("Version:"),  new QLabel(info.version,  glBox));
    glLay->addLayout(glForm);

    // --- Enumerated GPUs ---
    QStringList gpus = enumerateGpus();
    if (!gpus.isEmpty()) {
        auto [enumBox, enumLay] = makeSection(tr("Detected GPU Devices (/sys/class/drm)"));
        for (const auto& g : gpus)
            enumLay->addWidget(new QLabel(g, enumBox));
    }

    // --- Env-var GPU selection ---
    auto [envBox, envLay] = makeSection(tr("GPU Selection (Environment Variable)"));

    auto* varsWidget = new QWidget(envBox);
    auto* varsLay = new QVBoxLayout(varsWidget);
    varsLay->setContentsMargins(0, 0, 0, 0);
    varsLay->setSpacing(1);
    for (const QString& line : QStringList{
            tr("Set an environment variable to force GPU selection, then restart."),
            tr("Common variables:"),
            tr("  DRI_PRIME=1                         (Linux: discrete GPU via PRIME)"),
            tr("  __GLX_VENDOR_LIBRARY_NAME=nvidia    (NVIDIA on Wayland)"),
            tr("  MESA_VK_DEVICE_SELECT=0000:01:00.0  (Mesa Vulkan device)")})
        varsLay->addWidget(new QLabel(line, varsWidget));
    envLay->addWidget(varsWidget);

    auto* presets = new QComboBox(envBox);
    presets->addItem(tr("(custom)"), QString());
    presets->addItem("LIBGL_ALWAYS_SOFTWARE=1 (CPU-only / llvmpipe)", "LIBGL_ALWAYS_SOFTWARE=1");
    presets->addItem("DRI_PRIME=1",                        "DRI_PRIME=1");
    presets->addItem("DRI_PRIME=0",                        "DRI_PRIME=0");
    presets->addItem("__GLX_VENDOR_LIBRARY_NAME=nvidia",   "__GLX_VENDOR_LIBRARY_NAME=nvidia");
    presets->addItem("__GLX_VENDOR_LIBRARY_NAME=mesa",     "__GLX_VENDOR_LIBRARY_NAME=mesa");
    envLay->addWidget(presets);

    auto* envRow = new QHBoxLayout();
    m_env_edit = new QLineEdit(envBox);
    m_env_edit->setPlaceholderText(tr("VAR=VALUE  (leave empty to clear)"));
    envRow->addWidget(m_env_edit);
    envLay->addLayout(envRow);

    connect(presets, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, presets](int idx) {
        QString val = presets->itemData(idx).toString();
        if (!val.isEmpty()) m_env_edit->setText(val);
    });

    // Restore current env setting if any
    QString current = qgetenv("DRI_PRIME");
    if (!current.isEmpty()) m_env_edit->setText("DRI_PRIME=" + current);

    // --- Buttons ---
    auto* btns = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto* restartBtn = btns->addButton(tr("Apply && Restart"), QDialogButtonBox::ActionRole);
    connect(restartBtn, &QPushButton::clicked, this, &GpuInfoDialog::applyAndRestart);
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(btns);
}

void GpuInfoDialog::applyAndRestart() {
    QString envLine = m_env_edit->text().trimmed();

    QString varName, varValue;
    if (!envLine.isEmpty()) {
        int eq = envLine.indexOf('=');
        if (eq < 1) {
            QMessageBox::warning(this, tr("Invalid"),
                tr("Please enter an environment variable in VAR=VALUE format."));
            return;
        }
        varName  = envLine.left(eq);
        varValue = envLine.mid(eq + 1);
    }

    auto res = QMessageBox::question(this,
        tr("Restart FlashViewer?"),
        envLine.isEmpty()
            ? tr("Restart without any GPU env override?")
            : tr("Restart with:\n  %1\n\nProceed?").arg(envLine),
        QMessageBox::Yes | QMessageBox::No);
    if (res != QMessageBox::Yes) return;

    QStringList args = QCoreApplication::arguments().mid(1);
    QString prog = QCoreApplication::applicationFilePath();

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (!varName.isEmpty())
        env.insert(varName, varValue);

    QProcess proc;
    proc.setProgram(prog);
    proc.setArguments(args);
    proc.setProcessEnvironment(env);
    if (!proc.startDetached()) {
        QMessageBox::critical(this, tr("Restart Failed"),
            tr("Could not launch: %1").arg(prog));
        return;
    }
    QCoreApplication::quit();
}
