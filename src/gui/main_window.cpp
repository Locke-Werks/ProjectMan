#include "main_window.h"

#include "discovery.h"
#include "dock.h"
#include "enrich.h"
#include "launcher.h"
#include "project_model.h"
#include "scanner.h"
#include "strutil.h"
#include "theme_qt.h"
#include "widgets.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QShortcut>
#include <QSpinBox>
#include <QSplitter>
#include <QTableView>
#include <QTableWidget>
#include <QVBoxLayout>

namespace pm::gui {
namespace {

using namespace pm::theme;

QLabel* caption(const QString& text, pm::theme::Rgb colour, int px = 12)
{
    auto* l = new QLabel(text);
    l->setFont(theme::body(px));
    l->setStyleSheet(QStringLiteral("color: %1;").arg(theme::c(colour).name()));
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return l;
}

} // namespace

// ------------------------------------------------------------- ScanController

ScanController::ScanController(QObject* parent) : QObject(parent) {}

ScanController::~ScanController()
{
    cancel();
    if (worker_.joinable())
        worker_.join();
}

void ScanController::cancel() { token_.request_stop(); }

void ScanController::start(const Config& cfg)
{
    if (worker_.joinable()) {
        token_.request_stop();
        worker_.join();
    }
    token_.reset();
    worker_ = std::thread(&ScanController::run, this, cfg);
}

void ScanController::run(Config cfg)
{
    DiscoveryOptions opts;
    opts.root              = cfg.root;
    opts.descendContainers = cfg.descendContainers;
    opts.includePlain      = cfg.includePlain;
    opts.exclude           = cfg.exclude;

    projects_ = discover(opts, nullptr);

    QMetaObject::invokeMethod(this, [this, n = static_cast<int>(projects_.size())] {
        emit discovered(n);
    }, Qt::QueuedConnection);

    git::ProbeOptions po;
    po.timeoutMs = cfg.probeTimeoutMs;

    ScanPool pool(cfg.scanThreads, po);
    pool.start(projects_, *this, token_);
    pool.wait();

    if (token_.stop_requested())
        return;

    // The slow half, after the list is already usable.
    enrichClaudeState(projects_, cfg);
    enrichChecklists(projects_, cfg);
    enrichGitHub(projects_, cfg, /*force=*/false, nullptr, nullptr);

    for (size_t i = 0; i < projects_.size(); ++i) {
        QMetaObject::invokeMethod(this, [this, i, p = projects_[i]] {
            emit rowReady(static_cast<int>(i), p);
        }, Qt::QueuedConnection);
    }

    QMetaObject::invokeMethod(this, [this] { emit enrichFinished(); },
                              Qt::QueuedConnection);
}

void ScanController::onStatus(std::size_t index, const Project& project)
{
    // Worker thread. Copy the value and post it; never touch a widget here.
    QMetaObject::invokeMethod(this, [this, index, p = project] {
        emit rowReady(static_cast<int>(index), p);
    }, Qt::QueuedConnection);
}

void ScanController::onScanFinished(int probed, int failed, double wallSeconds)
{
    QMetaObject::invokeMethod(this, [this, probed, failed, wallSeconds] {
        emit sweepFinished(probed, failed, wallSeconds);
    }, Qt::QueuedConnection);
}

// ------------------------------------------------------------ DetailController

DetailController::DetailController(QObject* parent) : QObject(parent)
{
    worker_ = std::thread(&DetailController::loop, this);
}

DetailController::~DetailController()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    // Joined, not detached: the worker posts back to this object, so it must be
    // finished before the object goes.
    if (worker_.joinable())
        worker_.join();
}

quint64 DetailController::request(const pm::fs::path& path, int timeoutMs)
{
    quint64 token = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_    = path;
        timeoutMs_  = timeoutMs;
        have_       = true;
        token       = ++token_;
    }
    wake_.notify_one();
    return token;
}

void DetailController::loop()
{
    for (;;) {
        pm::fs::path path;
        int          timeoutMs = 20000;
        quint64      token     = 0;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return have_ || stop_; });
            if (stop_)
                return;
            path      = pending_;
            timeoutMs = timeoutMs_;
            token     = token_;
            have_     = false;
        }

        pm::git::ProbeOptions po;
        po.timeoutMs = timeoutMs;

        const pm::git::RepoProbe  probe = pm::git::probe(path);
        const pm::git::RepoDetail d     = pm::git::detail(path, probe, 200, po);

        // Emitted from inside the posted lambda, so the signal runs on the GUI
        // thread as a direct call and RepoDetail never crosses as a queued
        // argument needing a registered metatype.
        QMetaObject::invokeMethod(this, [this, token, d] {
            emit ready(token, d);
        }, Qt::QueuedConnection);
    }
}

// ------------------------------------------------------------- DispatchDialog

DispatchDialog::DispatchDialog(WorkList items, const Config& cfg, QWidget* parent)
    : QDialog(parent), items_(std::move(items)), cfg_(cfg)
{
    setWindowTitle(QStringLiteral("Dispatch"));
    resize(940, 640);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(18, 14, 18, 16);
    root->setSpacing(12);

    root->addWidget(new TrackedLabel(QStringLiteral("// Dispatch"), 15, QFont::Bold, 0.18));

    auto* blurb = caption(
        QStringLiteral("Tick the work to hand over, then GO. One Claude Code session "
                       "gets every selected repository and a briefing of what is "
                       "outstanding. It will ask you when something is unclear."),
        kFg3);
    blurb->setWordWrap(true);
    root->addWidget(blurb);

    root->addWidget(caption(QStringLiteral("GENERAL INSTRUCTIONS (OPTIONAL)"), kFg4));

    instructions_ = new QPlainTextEdit(this);
    instructions_->setPlaceholderText(
        QStringLiteral("One job to do across the selected repositories, in your own "
                       "words. For example: ensure every default branch is named "
                       "main, and rename it where it is not. Left empty, the session "
                       "works the ticked items as found."));
    instructions_->setFont(theme::mono(11));
    // Three lines. Enough for a directive, small enough that the item table
    // stays the thing the dialog is about.
    instructions_->setFixedHeight(72);
    root->addWidget(instructions_);

    connect(instructions_, &QPlainTextEdit::textChanged, this,
            &DispatchDialog::rebuildSummary);

    table_ = new QTableWidget(static_cast<int>(items_.size()), 4, this);
    table_->setHorizontalHeaderLabels({ QStringLiteral(""), QStringLiteral("KIND"),
                                        QStringLiteral("PROJECT"),
                                        QStringLiteral("OUTSTANDING") });
    table_->verticalHeader()->hide();
    table_->setShowGrid(false);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    table_->setColumnWidth(0, 34);
    table_->setColumnWidth(1, 120);
    table_->setColumnWidth(2, 240);

    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const WorkItem& w = items_[static_cast<size_t>(i)];

        auto* check = new QCheckBox;
        check->setChecked(w.selected);
        // Centred in its own cell, because a bare checkbox in a table cell
        // otherwise hugs the left edge and reads as misaligned.
        auto* holder = new QWidget;
        auto* hl     = new QHBoxLayout(holder);
        hl->setContentsMargins(0, 0, 0, 0);
        hl->setAlignment(Qt::AlignCenter);
        hl->addWidget(check);
        table_->setCellWidget(i, 0, holder);

        connect(check, &QCheckBox::toggled, this, [this, i](bool on) {
            items_[static_cast<size_t>(i)].selected = on;
            rebuildSummary();
        });

        auto* kind = new QTableWidgetItem(QString::fromLatin1(kindLabel(w.kind)));
        kind->setForeground(theme::c(kFg4));
        kind->setFont(theme::mono(11));
        table_->setItem(i, 1, kind);

        auto* proj = new QTableWidgetItem(QString::fromStdString(w.project));
        proj->setForeground(theme::c(kFg1));
        table_->setItem(i, 2, proj);

        auto* what = new QTableWidgetItem(QString::fromStdString(w.summary));
        what->setForeground(theme::c(kFg3));
        table_->setItem(i, 3, what);
    }

    root->addWidget(table_, 1);

    summary_ = caption(QString(), kFg2);
    root->addWidget(summary_);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);

    auto* all = new QPushButton(QStringLiteral("ALL"));
    auto* none = new QPushButton(QStringLiteral("NONE"));
    auto* preview = new QPushButton(QStringLiteral("PREVIEW"));
    auto* cancel = new QPushButton(QStringLiteral("CANCEL"));
    go_ = new QPushButton(QStringLiteral("GO"));
    go_->setObjectName(QStringLiteral("Primary"));

    for (QPushButton* b : { all, none, preview, cancel, go_ })
        b->setFont(theme::tracked(11, QFont::Bold, 0.14));

    buttons->addWidget(all);
    buttons->addWidget(none);
    buttons->addWidget(preview);
    buttons->addWidget(cancel);
    buttons->addWidget(go_);
    root->addLayout(buttons);

    const auto setAll = [this](bool on) {
        for (int i = 0; i < table_->rowCount(); ++i) {
            if (auto* holder = table_->cellWidget(i, 0)) {
                if (auto* cb = holder->findChild<QCheckBox*>())
                    cb->setChecked(on);
            }
        }
    };
    connect(all, &QPushButton::clicked, this, [setAll] { setAll(true); });
    connect(none, &QPushButton::clicked, this, [setAll] { setAll(false); });
    connect(preview, &QPushButton::clicked, this, &DispatchDialog::showPreview);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(go_, &QPushButton::clicked, this, &QDialog::accept);

    rebuildSummary();
}

DispatchOptions DispatchDialog::options() const
{
    DispatchOptions opt;
    opt.autonomy     = cfg_.autonomy;
    opt.maxRepos     = cfg_.dispatchMaxRepos;
    opt.maxItems     = cfg_.dispatchMaxItems;
    opt.instructions = instructions_
                           ? instructions_->toPlainText().trimmed().toStdString()
                           : std::string();
    return opt;
}

void DispatchDialog::showPreview()
{
    const DispatchPlan plan = buildDispatchPlan(items_, options());

    QDialog box(this);
    box.setWindowTitle(QStringLiteral("Briefing"));
    box.resize(900, 720);

    auto* layout = new QVBoxLayout(&box);
    layout->setContentsMargins(18, 14, 18, 16);
    layout->setSpacing(12);

    layout->addWidget(new TrackedLabel(QStringLiteral("// Briefing"), 15, QFont::Bold, 0.18));
    layout->addWidget(caption(
        plan.items.empty()
            ? QStringLiteral("Nothing is selected, so there is no briefing to show.")
            : QStringLiteral("Exactly what the session is handed, before it starts."),
        kFg3));

    auto* text = new QPlainTextEdit(QString::fromStdString(plan.briefing), &box);
    text->setReadOnly(true);
    text->setFont(theme::mono(11));
    text->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    layout->addWidget(text, 1);

    auto* row = new QHBoxLayout;
    row->addStretch(1);
    auto* copy  = new QPushButton(QStringLiteral("COPY"));
    auto* close = new QPushButton(QStringLiteral("CLOSE"));
    for (QPushButton* b : { copy, close })
        b->setFont(theme::tracked(11, QFont::Bold, 0.14));
    row->addWidget(copy);
    row->addWidget(close);
    layout->addLayout(row);

    connect(copy, &QPushButton::clicked, &box, [&plan] {
        QApplication::clipboard()->setText(QString::fromStdString(plan.briefing));
    });
    connect(close, &QPushButton::clicked, &box, &QDialog::accept);

    box.exec();
}

void DispatchDialog::rebuildSummary()
{
    const DispatchPlan preview = buildDispatchPlan(items_, options());
    const int          n       = static_cast<int>(preview.items.size());
    const int          repos   = static_cast<int>(preview.repos.size());

    summary_->setText(
        n == 0 ? QStringLiteral("Nothing selected.")
               : QStringLiteral("%1 item%2 across %3 repositor%4. %5")
                     .arg(n)
                     .arg(n == 1 ? "" : "s")
                     .arg(repos)
                     .arg(repos == 1 ? "y" : "ies")
                     .arg(QString::fromStdString(dispatchSummary(cfg_.autonomy))));
    go_->setEnabled(n > 0);
}

void DispatchDialog::accept()
{
    plan_ = buildDispatchPlan(items_, options());
    if (plan_.items.empty())
        return;
    QDialog::accept();
}

// ------------------------------------------------------------- SettingsDialog

SettingsDialog::SettingsDialog(Config cfg, QWidget* parent)
    : QDialog(parent), cfg_(std::move(cfg))
{
    setWindowTitle(QStringLiteral("Settings"));
    resize(760, 720);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(18, 14, 18, 16);
    root->setSpacing(12);

    root->addWidget(new TrackedLabel(QStringLiteral("// Settings"), 15, QFont::Bold, 0.18));

    auto* where = caption(QString::fromStdString(Config::filePath().string()), kFg4, 11);
    where->setFont(theme::mono(11));
    root->addWidget(where);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* page = new QWidget;
    auto* form = new QFormLayout(page);
    form->setContentsMargins(0, 8, 8, 8);
    form->setSpacing(10);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    for (const Setting& s : settings())
        addRow(form, s);

    scroll->setWidget(page);
    root->addWidget(scroll, 1);

    autonomyNote_ = caption(QString(), kFg2);
    autonomyNote_->setWordWrap(true);
    root->addWidget(autonomyNote_);

    launchLine_ = caption(QString(), kFg4, 11);
    launchLine_->setFont(theme::mono(11));
    launchLine_->setWordWrap(true);
    root->addWidget(launchLine_);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto* cancel = new QPushButton(QStringLiteral("CANCEL"));
    auto* save   = new QPushButton(QStringLiteral("SAVE"));
    save->setObjectName(QStringLiteral("Primary"));
    for (QPushButton* b : { cancel, save })
        b->setFont(theme::tracked(11, QFont::Bold, 0.14));
    buttons->addWidget(cancel);
    buttons->addWidget(save);
    root->addLayout(buttons);

    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(save, &QPushButton::clicked, this, [this] {
        std::string err;
        if (!cfg_.save(&err)) {
            QMessageBox::warning(this, QStringLiteral("ProjectMan"),
                                 QString::fromStdString(err));
            return;
        }
        accept();
    });

    refreshDerived();
}

void SettingsDialog::refreshDerived()
{
    autonomyNote_->setText(QStringLiteral("%1  %2")
                               .arg(QString::fromLatin1(autonomyLabel(cfg_.autonomy)))
                               .arg(QString::fromLatin1(autonomySummary(cfg_.autonomy))));
    autonomyNote_->setStyleSheet(
        QStringLiteral("color: %1;").arg(theme::c(kFg2).name()));

    // While unpinned this follows the ladder, so it has to be re-read rather
    // than left at whatever it was when the dialog opened.
    if (skipPermissions_ && !cfg_.skipPermissionsExplicit) {
        QSignalBlocker block(skipPermissions_);
        skipPermissions_->setChecked(cfg_.resolvedSkipPermissions());
    }

    LaunchSpec  spec;
    QStringList args;
    for (const std::string& a : claudeArgs(spec, cfg_))
        args << QString::fromStdString(a);
    launchLine_->setText(QStringLiteral("claude ") + args.join(QLatin1Char(' ')));
}

void SettingsDialog::addRow(QFormLayout* form, const Setting& s)
{
    const QString current = QString::fromStdString(readSetting(cfg_, s.key));
    const QString key     = QString::fromLatin1(s.key);

    auto* label = new TrackedLabel(QString::fromLatin1(s.label), 11, QFont::DemiBold, 0.1);
    label->setToolTip(QString::fromLatin1(s.help));

    QWidget* editor = nullptr;

    const auto apply = [this, key](const QString& value) {
        std::string err;
        if (!applySetting(cfg_, key.toStdString(), value.toStdString(), &err)) {
            QMessageBox::warning(this, QStringLiteral("ProjectMan"),
                                 QString::fromStdString(err));
            return;
        }
        refreshDerived();
    };

    switch (s.kind) {
    case SettingKind::Bool: {
        auto* box = new QCheckBox;
        // launch.skip_permissions reads back as "true (from autonomy)" when it
        // is unpinned, so the checkbox reflects the first word only.
        box->setChecked(current.startsWith(QStringLiteral("true")));
        connect(box, &QCheckBox::toggled, this, [apply](bool on) {
            apply(on ? QStringLiteral("true") : QStringLiteral("false"));
        });
        if (key == QLatin1String("launch.skip_permissions")) {
            skipPermissions_ = box;
            box->setToolTip(QStringLiteral(
                "Follows the autonomy ladder until you change it here."));
        }
        editor = box;
        break;
    }

    case SettingKind::Int: {
        auto* spin = new QSpinBox;
        spin->setRange(s.min, s.max);
        spin->setValue(current.toInt());
        connect(spin, &QSpinBox::valueChanged, this,
                [apply](int v) { apply(QString::number(v)); });
        editor = spin;
        break;
    }

    case SettingKind::Choice: {
        auto* combo = new QComboBox;
        for (const QString& opt :
             QString::fromLatin1(s.choices).split(QLatin1Char(','))) {
            combo->addItem(opt.isEmpty() ? QStringLiteral("(default)") : opt, opt);
        }
        const int idx = combo->findData(current);
        combo->setCurrentIndex(idx >= 0 ? idx : 0);
        connect(combo, &QComboBox::currentIndexChanged, this, [apply, combo](int i) {
            apply(combo->itemData(i).toString());
        });
        editor = combo;
        break;
    }

    case SettingKind::Text:
    case SettingKind::Path:
    case SettingKind::StringList: {
        auto* edit = new QLineEdit(current);
        edit->setObjectName(QStringLiteral("Filter"));   // reuse the field styling
        if (s.kind == SettingKind::StringList)
            edit->setPlaceholderText(QStringLiteral("comma separated"));
        // Committed on edit rather than per keystroke, so a half-typed path is
        // never validated and rejected mid-word.
        connect(edit, &QLineEdit::editingFinished, this,
                [apply, edit] { apply(edit->text()); });
        editor = edit;
        break;
    }
    }

    if (editor)
        form->addRow(label, editor);
}

// ----------------------------------------------------------------- MainWindow

MainWindow::MainWindow(Config cfg, QWidget* parent)
    : QMainWindow(parent), cfg_(std::move(cfg))
{
    setWindowTitle(QStringLiteral("ProjectMan"));
    resize(1280, 780);

    auto* central = new QWidget;
    setCentralWidget(central);

    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    root->addWidget(new TopRule);

    auto* head = new QHBoxLayout;
    head->setContentsMargins(18, 12, 18, 10);
    eyebrow_ = new TrackedLabel(QStringLiteral("// Projects"), 15, QFont::Bold, 0.18);
    eyebrow_->setStyleSheet(
        QStringLiteral("color: %1;").arg(theme::c(kRed).name()));
    head->addWidget(eyebrow_);
    head->addStretch(1);
    counts_ = caption(QStringLiteral("scanning"), kFg4, 11);
    counts_->setFont(theme::mono(11));
    head->addWidget(counts_);
    root->addLayout(head);

    filter_ = new QLineEdit;
    filter_->setObjectName(QStringLiteral("Filter"));
    filter_->setPlaceholderText(QStringLiteral("Filter projects"));
    filter_->setFont(theme::body(13));
    auto* filterWrap = new QHBoxLayout;
    filterWrap->setContentsMargins(18, 0, 18, 12);
    filterWrap->addWidget(filter_);
    root->addLayout(filterWrap);

    auto* split = new QSplitter(Qt::Horizontal);

    table_ = new QTableView;
    model_ = new ProjectModel(this);
    proxy_ = new ProjectFilterProxy(this);
    proxy_->setSourceModel(model_);
    table_->setModel(proxy_);
    table_->setItemDelegate(new ProjectDelegate(this));
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setShowGrid(false);
    table_->setMouseTracking(true);
    // Banded rows would break "black on black": the alternate colour is a
    // second surface the design language does not have.
    table_->setAlternatingRowColors(false);
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setDefaultSectionSize(26);
    table_->horizontalHeader()->setFont(theme::tracked(10, QFont::DemiBold, 0.16));
    table_->horizontalHeader()->setSectionResizeMode(ProjectModel::ColName,
                                                     QHeaderView::Stretch);
    table_->setSortingEnabled(true);
    table_->sortByColumn(ProjectModel::ColLast, Qt::DescendingOrder);
    split->addWidget(table_);

    auto* detail = new QWidget;
    auto* dl     = new QVBoxLayout(detail);
    dl->setContentsMargins(18, 14, 18, 14);
    dl->setSpacing(9);

    dl->addWidget(new TrackedLabel(QStringLiteral("// Detail"), 11, QFont::DemiBold, 0.2));

    detailName_ = caption(QString(), kFg1, 17);
    detailName_->setFont(theme::tracked(17, QFont::Bold, 0.03));
    dl->addWidget(detailName_);

    detailPath_ = caption(QString(), kFg4, 11);
    detailPath_->setFont(theme::mono(11));
    detailPath_->setWordWrap(true);
    dl->addWidget(detailPath_);

    detailCommit_ = caption(QString(), kFg2);
    detailCommit_->setWordWrap(true);
    dl->addWidget(detailCommit_);

    detailOpen_ = caption(QString(), kFg3);
    detailOpen_->setWordWrap(true);
    dl->addWidget(detailOpen_);

    detailClaude_ = caption(QString(), kFg3);
    detailClaude_->setWordWrap(true);
    dl->addWidget(detailClaude_);

    inspect_ = new QPlainTextEdit;
    inspect_->setReadOnly(true);
    inspect_->setFont(theme::mono(11));
    inspect_->setLineWrapMode(QPlainTextEdit::NoWrap);
    inspect_->setFrameShape(QFrame::NoFrame);
    inspect_->setPlaceholderText(QStringLiteral("Select a project."));
    dl->addWidget(inspect_, 1);

    engage_      = new QPushButton(QStringLiteral("ENGAGE"));
    settingsBtn_ = new QPushButton(QStringLiteral("SETTINGS"));
    resume_   = new QPushButton(QStringLiteral("CONTINUE"));
    sessions_ = new QPushButton(QStringLiteral("SESSIONS"));
    terminal_ = new QPushButton(QStringLiteral("TERMINAL"));
    dispatch_ = new QPushButton(QStringLiteral("DISPATCH"));
    engage_->setObjectName(QStringLiteral("Primary"));

    for (QPushButton* b : { engage_, resume_, sessions_, terminal_, dispatch_,
                            settingsBtn_ })
        b->setFont(theme::tracked(11, QFont::Bold, 0.14));

    dl->addWidget(engage_);
    auto* row2 = new QHBoxLayout;
    row2->addWidget(resume_);
    row2->addWidget(sessions_);
    dl->addLayout(row2);
    auto* row3 = new QHBoxLayout;
    row3->addWidget(terminal_);
    row3->addWidget(dispatch_);
    dl->addLayout(row3);
    dl->addWidget(settingsBtn_);

    // Where a launch lands, and the only place the dock is named now that
    // it is not a button. Reads as a statement of fact rather than an advert
    // when it is missing.
    dockNote_ = caption(QString(), kFg4);
    dockNote_->setWordWrap(true);
    dl->addWidget(dockNote_);

    split->addWidget(detail);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    split->setSizes({ 780, 480 });
    root->addWidget(split, 1);

    grain_ = new GrainOverlay(central);
    grain_->raise();

    detail_ = new DetailController(this);
    connect(detail_, &DetailController::ready, this, &MainWindow::onDetailReady);

    scan_ = new ScanController(this);
    connect(scan_, &ScanController::rowReady, this, &MainWindow::onRowReady);
    connect(scan_, &ScanController::sweepFinished, this, &MainWindow::onSweepFinished);
    connect(scan_, &ScanController::enrichFinished, this, &MainWindow::updateCounts);
    connect(scan_, &ScanController::discovered, this, [this](int n) {
        ProjectList blanks(static_cast<size_t>(n));
        model_->setProjects(std::move(blanks));
    });

    connect(filter_, &QLineEdit::textChanged, proxy_, &ProjectFilterProxy::setSearch);
    connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            &MainWindow::onSelectionChanged);

    connect(engage_, &QPushButton::clicked, this, [this] { launch(LaunchMode::New); });
    connect(resume_, &QPushButton::clicked, this, [this] { launch(LaunchMode::Continue); });
    connect(sessions_, &QPushButton::clicked, this, [this] { launch(LaunchMode::Resume); });
    connect(terminal_, &QPushButton::clicked, this, &MainWindow::openTerminal);
    connect(dispatch_, &QPushButton::clicked, this, &MainWindow::openDispatch);
    connect(settingsBtn_, &QPushButton::clicked, this, &MainWindow::openSettings);

    // The same bindings as the console front end, so muscle memory carries
    // between the two. Ctrl-modified throughout, because the filter box owns
    // every bare keystroke.
    new QShortcut(QKeySequence(Qt::Key_F5), this, [this] { rescan(); });
    new QShortcut(QKeySequence::Find, this, [this] { filter_->setFocus(); });
    new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_D), this,
                  [this] { openDispatch(); });
    new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), this,
                  [this] { launch(LaunchMode::New); });
    new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_R), this,
                  [this] { launch(LaunchMode::Continue); });
    new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_E), this,
                  [this] { launch(LaunchMode::Resume); });
    new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_T), this,
                  [this] { openTerminal(); });
    new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Comma), this,
                  [this] { openSettings(); });

    refreshDockNote();
    onSelectionChanged();
    scan_->start(cfg_);
}

void MainWindow::resizeEvent(QResizeEvent* e)
{
    QMainWindow::resizeEvent(e);
    if (grain_ && centralWidget()) {
        grain_->setGeometry(centralWidget()->rect());
        grain_->raise();
    }
}

void MainWindow::onRowReady(int row, pm::Project project)
{
    model_->applyStatus(row, project);
    updateCounts();
}

void MainWindow::onSweepFinished(int probed, int failed, double seconds)
{
    counts_->setText(QStringLiteral("%1 probed  %2 failed  %3 s")
                         .arg(probed)
                         .arg(failed)
                         .arg(seconds, 0, 'f', 2));
    updateCounts();
}

void MainWindow::updateCounts()
{
    int dirty = 0, open = 0;
    for (const Project& p : model_->projects()) {
        if (p.git.state == GitState::Dirty)
            ++dirty;
        open += p.open.total();
    }
    counts_->setText(QStringLiteral("%1 projects   %2 dirty   %3 open")
                         .arg(model_->rowCount())
                         .arg(dirty)
                         .arg(open));
}

const Project* MainWindow::current() const
{
    const QModelIndexList sel = table_->selectionModel()->selectedRows();
    if (sel.isEmpty())
        return nullptr;
    return model_->at(proxy_->mapToSource(sel.front()).row());
}

void MainWindow::onSelectionChanged()
{
    const Project* p = current();

    const bool have = p != nullptr;
    for (QPushButton* b : { engage_, resume_, sessions_, terminal_ })
        b->setEnabled(have);

    if (!have) {
        detailName_->setText(QStringLiteral("Nothing selected"));
        detailPath_->clear();
        detailCommit_->clear();
        detailOpen_->clear();
        detailClaude_->clear();
        inspect_->clear();
        detailToken_ = 0;
        return;
    }

    detailName_->setText(QString::fromStdString(p->displayName()).toUpper());
    detailPath_->setText(QString::fromStdString(p->path.string())
                         + (p->ownerRepo.empty()
                                ? QString()
                                : QStringLiteral("\n") + QString::fromStdString(p->ownerRepo)));

    if (p->git.lastCommitUnix) {
        detailCommit_->setText(QStringLiteral("%1: %2")
                                   .arg(QString::fromStdString(p->git.lastCommitAuthor))
                                   .arg(QString::fromStdString(p->git.lastCommitSubject)));
    } else if (!p->git.error.empty()) {
        detailCommit_->setText(QString::fromStdString(p->git.error));
    } else {
        detailCommit_->clear();
    }

    QStringList bits;
    if (p->open.uncommitted)
        bits << QStringLiteral("%1 uncommitted").arg(p->open.uncommitted);
    if (p->open.unpushed)
        bits << QStringLiteral("%1 unpushed").arg(p->open.unpushed);
    if (p->open.unpulled)
        bits << QStringLiteral("%1 behind").arg(p->open.unpulled);
    if (p->open.stashes)
        bits << QStringLiteral("%1 stashed").arg(p->open.stashes);
    if (p->open.openPrs)
        bits << QStringLiteral("%1 open PR").arg(p->open.openPrs);
    if (p->open.openIssues)
        bits << QStringLiteral("%1 issue").arg(p->open.openIssues);
    if (p->open.checklistItems)
        bits << QStringLiteral("%1 unchecked").arg(p->open.checklistItems);
    detailOpen_->setText(bits.isEmpty() ? QStringLiteral("Nothing outstanding")
                                        : bits.join(QStringLiteral("   ")));

    if (!p->open.claudeTitle.empty() || !p->open.claudeLastPrompt.empty()) {
        detailClaude_->setText(
            QStringLiteral("// %1\n%2")
                .arg(QString::fromStdString(p->open.claudeTitle).toUpper())
                .arg(QString::fromStdString(p->open.claudeLastPrompt)));
    } else {
        detailClaude_->clear();
    }

    // Off-thread: this is three git invocations, and arrowing down the list
    // would otherwise hitch on every row.
    if (p->kind == ProjectKind::Repo) {
        inspect_->setPlainText(QStringLiteral("Reading..."));
        detailToken_ = detail_->request(p->path, cfg_.probeTimeoutMs);
    } else {
        inspect_->clear();
        detailToken_ = 0;
    }
}

bool MainWindow::wantsLooseWindow()
{
    // Read at the moment of the click rather than from the event, so it works
    // for the keyboard shortcuts as well as the buttons.
    return (QApplication::keyboardModifiers() & Qt::ShiftModifier) != 0;
}

bool MainWindow::tryDock(const Project& p, LaunchMode mode, bool shell)
{
    if (!dockReady_ || wantsLooseWindow())
        return false;

    LaunchSpec s;
    s.cwd  = p.path;
    s.mode = mode;

    // A cold dock can sit behind a UAC prompt for as long as the user takes to
    // answer, so the button says what it is doing rather than freezing.
    QPushButton* pressed = shell ? terminal_ : engage_;
    const QString  was   = pressed->text();
    pressed->setEnabled(false);
    pressed->setText(QStringLiteral("DOCKING"));
    QApplication::processEvents();

    std::string        why;
    const dock::Status st = shell ? dock::launchShell(p.path, cfg_, &why)
                                  : dock::launch(s, cfg_, &why);

    pressed->setText(was);
    pressed->setEnabled(true);

    if (st == dock::Status::Ok)
        return true;

    if (st == dock::Status::NotInstalled) {
        // Only reachable if it was uninstalled since the note was drawn.
        dockReady_ = false;
        refreshDockNote();
        offerDockDownload();
        return false;
    }

    // Anything else is the dock being full or on its way out. Say so, then let
    // the caller open a window: a refusal is not a reason to do nothing.
    QString msg = QString::fromLatin1(dock::statusText(st));
    if (!why.empty())
        msg += QStringLiteral(":\n\n") + QString::fromStdString(why);
    msg += QStringLiteral("\n\nOpening a window instead.");
    QMessageBox::information(this, QStringLiteral("ProjectMan"), msg);
    return false;
}

void MainWindow::launch(LaunchMode mode)
{
    const Project* p = current();
    if (!p)
        return;

    if (tryDock(*p, mode, /*shell=*/false))
        return;

    LaunchSpec s;
    s.cwd  = p->path;
    s.mode = mode;

    std::string err;
    if (!openInTerminal(s, cfg_, &err))
        QMessageBox::warning(this, QStringLiteral("ProjectMan"), QString::fromStdString(err));
}

void MainWindow::openTerminal()
{
    const Project* p = current();
    if (!p)
        return;

    if (tryDock(*p, LaunchMode::New, /*shell=*/true))
        return;

    // A terminal with nothing attached. openInTerminal always appends
    // claude.exe, so this needs the shell path or TERMINAL is just ENGAGE
    // again, which is what it was through 0.1.0.
    std::string err;
    if (!openShellInTerminal(p->path, cfg_, &err))
        QMessageBox::warning(this, QStringLiteral("ProjectMan"), QString::fromStdString(err));
}

void MainWindow::openDispatch()
{
    WorkList items = collectWorkItems(model_->projects());
    if (items.empty()) {
        QMessageBox::information(this, QStringLiteral("ProjectMan"),
                                 QStringLiteral("Nothing outstanding to dispatch."));
        return;
    }
    preselect(items, cfg_.dispatchMaxRepos);

    DispatchDialog dlg(std::move(items), cfg_, this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const DispatchPlan plan = dlg.plan();
    LaunchSpec         s    = dispatchSpec(plan, cfg_);

    std::string err;
    if (!openInTerminal(s, cfg_, &err))
        QMessageBox::warning(this, QStringLiteral("ProjectMan"), QString::fromStdString(err));
}

void MainWindow::onDetailReady(quint64 token, pm::git::RepoDetail d)
{
    // A result for a row the user has already moved off is not wrong, it is
    // late. Dropping it is the whole reason the token exists.
    if (token != detailToken_)
        return;

    if (!d.error.empty()) {
        inspect_->setPlainText(QString::fromStdString(d.error));
        return;
    }

    QString out;

    if (d.filesTotal == 0) {
        out += QStringLiteral("CHANGES  working tree clean\n");
    } else {
        out += QStringLiteral("CHANGES  %1 file%2")
                   .arg(d.filesTotal)
                   .arg(d.filesTotal == 1 ? "" : "s");
        if (d.added || d.removed) {
            out += QStringLiteral("  +%1 -%2").arg(d.added).arg(d.removed);
        }
        out += QChar('\n');

        // Widest path in the list, so the counts line up without pushing the
        // short names miles from their own column.
        int width = 0;
        for (const git::ChangedFile& f : d.files)
            width = std::max(width, static_cast<int>(f.path.size()));
        width = std::min(width, 60);

        for (const git::ChangedFile& f : d.files) {
            QString path = QString::fromStdString(f.path);
            if (path.size() > 60)
                path = QStringLiteral("...") + path.right(57);

            out += QStringLiteral("  %1  %2")
                       .arg(QString::fromStdString(f.code), -2)
                       .arg(path, -width);

            if (f.binary)
                out += QStringLiteral("  binary");
            else if (f.added || f.removed)
                out += QStringLiteral("  +%1 -%2").arg(f.added).arg(f.removed);
            else
                out += QStringLiteral("  ") + QString::fromStdString(f.label);

            out += QChar('\n');
        }

        const int hidden = d.filesTotal - static_cast<int>(d.files.size());
        if (hidden > 0)
            out += QStringLiteral("  ... and %1 more\n").arg(hidden);
    }

    if (!d.recent.empty()) {
        out += QStringLiteral("\nRECENT\n");
        for (const git::Commit& c : d.recent) {
            out += QStringLiteral("  %1  %2  %3\n")
                       .arg(QString::fromStdString(c.shortOid), -8)
                       .arg(QString::fromStdString(relativeAge(c.when)), -5)
                       .arg(QString::fromStdString(c.subject));
        }
    }

    inspect_->setPlainText(out);
}

void MainWindow::refreshDockNote()
{
    dockReady_ = dock::available(cfg_);

    if (dockReady_) {
        dockNote_->setText(
            QStringLiteral("Launches into a Docked Console column. "
                           "Hold Shift for a window of its own."));
        return;
    }

    // Not an advert. It says what will happen, and names the thing that would
    // change it, because a person who has never heard of Docked Console has no
    // way to find out that the option exists.
    dockNote_->setText(
        cfg_.dockAuto
            ? QStringLiteral("Launches open a new window. Docked Console, if you "
                             "install it, holds them in a strip instead.")
            : QStringLiteral("Launches open a new window. Turn on \"Use the dock\" "
                             "in Settings to put them in a Docked Console column."));
}

void MainWindow::offerDockDownload()
{
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(QStringLiteral("ProjectMan"));
    box.setText(QStringLiteral("Docked Console is not installed."));
    box.setInformativeText(
        QStringLiteral("DOCK puts the session in a Docked Console column, so it "
                       "needs Docked Console 0.4.0 or newer.\n\n"
                       "If it is already installed somewhere unusual, set "
                       "dock.exe in Settings instead."));

    QPushButton* get  = box.addButton(QStringLiteral("Download Installer"),
                                      QMessageBox::AcceptRole);
    QPushButton* page = box.addButton(QStringLiteral("Release Notes"),
                                      QMessageBox::ActionRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(get);
    box.exec();

    const char* url = nullptr;
    if (box.clickedButton() == get)
        url = dock::installerUrl();
    else if (box.clickedButton() == page)
        url = dock::releasePage();
    if (!url)
        return;

    std::string err;
    if (!dock::openDownload(url, &err)) {
        QMessageBox::warning(this, QStringLiteral("ProjectMan"),
                             QStringLiteral("Could not open a browser:\n\n")
                                 + QString::fromStdString(err));
    }
}

void MainWindow::openSettings()
{
    SettingsDialog dlg(cfg_, this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const Config before = cfg_;
    cfg_ = dlg.config();

    // Only a change that alters what gets indexed is worth a sweep. Autonomy
    // and the launch flags take effect on the next launch by themselves.
    const bool rescanNeeded = before.root != cfg_.root
                           || before.exclude != cfg_.exclude
                           || before.includePlain != cfg_.includePlain
                           || before.descendContainers != cfg_.descendContainers
                           || before.githubEnabled != cfg_.githubEnabled
                           || before.githubOwners != cfg_.githubOwners;
    // dock.use_dock is one of the settings just edited, and the note under the
    // buttons is drawn from it.
    refreshDockNote();

    if (rescanNeeded)
        rescan();
    else
        onSelectionChanged();
}

void MainWindow::rescan()
{
    counts_->setText(QStringLiteral("scanning"));
    scan_->start(cfg_);
}

} // namespace pm::gui
