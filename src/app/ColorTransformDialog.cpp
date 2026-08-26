#include "app/ColorTransformDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace trace::app {

using trace::core::ColorTransform;

namespace {

// A row's config string lives in the item's data, never in its visible text.
// The built-in rows show OCIO's own UI names -- "Academy Color Encoding System -
// CG Config [COLORSPACES v4.0.0] [ACES v2.0] [OCIO v2.5]" -- and the string that
// has to reach CreateFromFile is "ocio://cg-config-v4.0.0_aces-v2.0_ocio-v2.5".
// Deriving one from the other would be parsing a label.
constexpr int kConfigStringRole = Qt::UserRole;
// Marks the Browse row, which is an action rather than a value.
constexpr int kBrowseRole = Qt::UserRole + 1;

} // namespace

ColorTransformDialog::ColorTransformDialog(const ColorTransform::Config& current,
                                           QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Color Transform"));
    setModal(true);

    configCombo_ = new QComboBox(this);
    inputCombo_ = new QComboBox(this);
    displayCombo_ = new QComboBox(this);
    viewCombo_ = new QComboBox(this);

    // Long config and view names -- "ACES 2.0 - SDR 100 nits (Rec.709)" and the
    // registry's UI names -- would otherwise stretch the dialog to the width of
    // the longest entry in the list. Elide in the closed box; the popup shows
    // the full text.
    for (QComboBox* c : {configCombo_, inputCombo_, displayCombo_, viewCombo_}) {
        c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        c->setMinimumContentsLength(38);
    }

    statusLabel_ = new QLabel(this);
    statusLabel_->setWordWrap(true);
    // The config's own path is one unbroken token and a wrapping label holding
    // one demands a very wide minimum -- the fault the Movie Inspector's source
    // path row already paid for, on the one window whose job is to say which
    // claim is which.
    statusLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto* form = new QFormLayout;
    form->addRow(tr("&Config:"), configCombo_);
    form->addRow(tr("&Input colour space:"), inputCombo_);
    form->addRow(tr("&Display:"), displayCombo_);
    form->addRow(tr("&View:"), viewCombo_);

    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* root = new QVBoxLayout(this);
    root->addLayout(form);
    root->addWidget(statusLabel_);
    root->addStretch(1);
    root->addWidget(buttons_);

    const bool seeded = current.kind == ColorTransform::Kind::DisplayView;
    populateConfigs(seeded ? current.configPath : QString());

    if (seeded) {
        reloadFromConfig(/*keepSelection=*/false);
        // Restore the saved selection where the config still offers it. A saved
        // name that has since disappeared from the config falls back to the
        // default rather than being added as a phantom row.
        const auto restore = [](QComboBox* c, const QString& want) {
            if (want.isEmpty()) return;
            const int i = c->findText(want);
            if (i >= 0) c->setCurrentIndex(i);
        };
        restore(inputCombo_, current.inputSpace);
        restore(displayCombo_, current.display);
        reloadViews(/*keepSelection=*/false);
        restore(viewCombo_, current.view);
    } else {
        reloadFromConfig(/*keepSelection=*/false);
    }

    connect(configCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) {
                if (populating_) return;
                if (index >= 0 && configCombo_->itemData(index, kBrowseRole).toBool()) {
                    browseForConfig();
                    return;
                }
                lastConfigIndex_ = index;
                // keepSelection=false: a colour-space or view name carried from
                // one config into another is how a dialog ends up naming
                // something the config it compiles does not contain.
                reloadFromConfig(/*keepSelection=*/false);
            });

    connect(displayCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) {
                if (populating_) return;
                reloadViews(/*keepSelection=*/false);
            });
}

void ColorTransformDialog::populateConfigs(const QString& preferred) {
    populating_ = true;
    configCombo_->clear();

    const auto addRow = [this](const QString& label, const QString& value) {
        configCombo_->addItem(label);
        configCombo_->setItemData(configCombo_->count() - 1, value, kConfigStringRole);
    };

    // 1. $OCIO, and ONLY when it is actually set. OCIO's own CreateFromEnv()
    //    would happily answer with a "Color management disabled" raw config
    //    when it is not, which is a row that looks like it loaded something.
    const QByteArray env = qgetenv("OCIO");
    if (!env.isEmpty()) {
        const QString path = QString::fromLocal8Bit(env);
        addRow(tr("$OCIO - %1").arg(QFileInfo(path).fileName()), path);
    }

    // 2. The built-in configs, recommended first, so a machine with no colour
    //    management installed still has a working ACES setup.
    for (const auto& b : ColorTransform::builtinConfigs()) {
        addRow(b.recommended ? tr("%1  (recommended)").arg(b.label) : b.label, b.uri);
    }

    // 3. Whatever config is currently in force, if it is a file that is not
    //    already one of the rows above.
    if (!preferred.isEmpty()) {
        bool present = false;
        for (int i = 0; i < configCombo_->count(); ++i) {
            if (configCombo_->itemData(i, kConfigStringRole).toString() == preferred) {
                present = true;
                break;
            }
        }
        if (!present) addRow(QFileInfo(preferred).fileName(), preferred);
    }

    addRow(tr("Browse for a config file..."), QString());
    browseIndex_ = configCombo_->count() - 1;
    configCombo_->setItemData(browseIndex_, true, kBrowseRole);

    int want = 0;
    if (!preferred.isEmpty()) {
        for (int i = 0; i < configCombo_->count(); ++i) {
            if (configCombo_->itemData(i, kConfigStringRole).toString() == preferred) {
                want = i;
                break;
            }
        }
    }
    configCombo_->setCurrentIndex(want);
    lastConfigIndex_ = want;
    populating_ = false;
}

QString ColorTransformDialog::selectedConfigString() const {
    const int i = configCombo_->currentIndex();
    if (i < 0) return QString();
    return configCombo_->itemData(i, kConfigStringRole).toString();
}

void ColorTransformDialog::reloadFromConfig(bool keepSelection) {
    const QString cfg = selectedConfigString();
    const QString keepInput = keepSelection ? inputCombo_->currentText() : QString();
    const QString keepDisplay = keepSelection ? displayCombo_->currentText() : QString();

    QString err;
    const QStringList spaces = ColorTransform::colorSpaces(cfg, err);
    QString dispErr;
    const QStringList disps = ColorTransform::displays(cfg, dispErr);
    if (err.isEmpty()) err = dispErr;

    populating_ = true;
    inputCombo_->clear();
    inputCombo_->addItems(spaces);
    displayCombo_->clear();
    displayCombo_->addItems(disps);
    populating_ = false;

    // THE DEFAULT INPUT IS THE scene_linear ROLE. Never
    // getColorSpaceFromFilepath() -- see the header for the two measured wrong
    // answers it gives on the two configs in the asset set.
    QString roleErr;
    const QString sceneLinear = ColorTransform::sceneLinearSpace(cfg, roleErr);
    const QString wantInput = (!keepInput.isEmpty() && spaces.contains(keepInput))
                                  ? keepInput : sceneLinear;
    if (!wantInput.isEmpty()) {
        const int i = inputCombo_->findText(wantInput);
        if (i >= 0) inputCombo_->setCurrentIndex(i);
    }

    QString dErr;
    const QString defaultDisplay = ColorTransform::defaultDisplay(cfg, dErr);
    const QString wantDisplay = (!keepDisplay.isEmpty() && disps.contains(keepDisplay))
                                    ? keepDisplay : defaultDisplay;
    if (!wantDisplay.isEmpty()) {
        const int i = displayCombo_->findText(wantDisplay);
        if (i >= 0) displayCombo_->setCurrentIndex(i);
    }

    reloadViews(keepSelection);

    if (err.isEmpty() && !roleErr.isEmpty() && sceneLinear.isEmpty()) err = roleErr;
    updateStatus(err);
}

void ColorTransformDialog::reloadViews(bool keepSelection) {
    const QString cfg = selectedConfigString();
    const QString display = displayCombo_->currentText();
    const QString keepView = keepSelection ? viewCombo_->currentText() : QString();

    QString err;
    const QStringList vs = ColorTransform::views(cfg, display, err);

    populating_ = true;
    viewCombo_->clear();
    viewCombo_->addItems(vs);
    populating_ = false;

    QString vErr;
    const QString defaultView = ColorTransform::defaultView(cfg, display, vErr);
    const QString wantView = (!keepView.isEmpty() && vs.contains(keepView))
                                 ? keepView : defaultView;
    if (!wantView.isEmpty()) {
        const int i = viewCombo_->findText(wantView);
        if (i >= 0) viewCombo_->setCurrentIndex(i);
    }
}

void ColorTransformDialog::browseForConfig() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Choose an OpenColorIO config"), QString(),
        tr("OpenColorIO config (*.ocio);;All files (*)"));
    if (path.isEmpty()) {
        // Cancelled. Put the combo back where it was -- leaving it on the
        // Browse row would show an action as if it were the chosen config, and
        // selectedConfigString() would answer empty, which resolves to the
        // DEFAULT config rather than to nothing. Silently loading a different
        // config than the one named is exactly what this dialog must not do.
        populating_ = true;
        configCombo_->setCurrentIndex(lastConfigIndex_);
        populating_ = false;
        return;
    }

    populateConfigs(path);
    reloadFromConfig(/*keepSelection=*/false);
}

void ColorTransformDialog::updateStatus(const QString& error) {
    // WHICH CONFIG IS IN FORCE, SAID ON SCREEN. The requirement is explicit, and
    // it is the same rule the HUD's `renderer` and `planar` fields follow: the
    // answer is not decided by anything the user typed, so it has to be reported
    // rather than reasoned about.
    const QString cfg = selectedConfigString();
    ColorTransform::ConfigSource src = ColorTransform::ConfigSource::File;
    QString resolved = cfg;
    if (cfg.isEmpty()) {
        resolved = ColorTransform::defaultConfigString(&src);
    } else if (cfg.startsWith(QLatin1String("ocio://"))) {
        src = ColorTransform::ConfigSource::Builtin;
    } else if (cfg == QString::fromLocal8Bit(qgetenv("OCIO"))) {
        src = ColorTransform::ConfigSource::Env;
    }

    QString where;
    switch (src) {
        case ColorTransform::ConfigSource::Builtin:
            where = tr("built-in, compiled into OpenColorIO");
            break;
        case ColorTransform::ConfigSource::Env:
            where = tr("from the $OCIO environment variable");
            break;
        default:
            where = tr("chosen file");
            break;
    }

    if (!error.isEmpty()) {
        statusLabel_->setText(tr("Config: %1\n%2").arg(resolved, error));
        // A config that will not load must not be acceptable. Failing here is
        // better than failing after OK, where the previous configuration would
        // silently stay in force and the dialog would look as though it worked.
        buttons_->button(QDialogButtonBox::Ok)->setEnabled(false);
        return;
    }
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(true);
    statusLabel_->setText(tr("Config: %1\n(%2)").arg(resolved, where));
}

ColorTransform::Config ColorTransformDialog::result() const {
    ColorTransform::Config cfg;
    cfg.kind = ColorTransform::Kind::DisplayView;
    // The RESOLVED string, not an empty "use the default". Persisting empty
    // would mean the saved configuration silently changed meaning if $OCIO were
    // later set or unset -- the transform would be a function of the
    // environment rather than of what the user chose.
    const QString sel = selectedConfigString();
    cfg.configPath = sel.isEmpty() ? ColorTransform::defaultConfigString() : sel;
    cfg.inputSpace = inputCombo_->currentText();
    cfg.display = displayCombo_->currentText();
    cfg.view = viewCombo_->currentText();
    return cfg;
}

} // namespace trace::app
