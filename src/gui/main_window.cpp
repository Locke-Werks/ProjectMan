#include "main_window.h"

#include "discovery.h"
#include "enrich.h"
#include "launcher.h"
#include "project_model.h"
#include "scanner.h"
#include "theme_qt.h"
#include "widgets.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QResizeEvent>
#include <QShortcut>
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
    auto* cancel = new QPushButton(QStringLiteral("CANCEL"));
    go_ = new QPushButton(QStringLiteral("GO"));
    go_->setObjectName(QStringLiteral("Primary"));

    for (QPushButton* b : { all, none, cancel, go_ })
        b->setFont(theme::tracked(11, QFont::Bold, 0.14));

    buttons->addWidget(all);
    buttons->addWidget(none);
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
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(go_, &QPushButton::clicked, this, &QDialog::accept);

    rebuildSummary();
}

void DispatchDialog::rebuildSummary()
{
    DispatchOptions opt;
    opt.allowCommit = cfg_.dispatchCommit;
    opt.allowPush   = cfg_.dispatchPush;
    opt.maxRepos    = cfg_.dispatchMaxRepos;

    const DispatchPlan preview = buildDispatchPlan(items_, opt);
    const int          n       = static_cast<int>(preview.items.size());
    const int          repos   = static_cast<int>(preview.repos.size());

    summary_->setText(
        n == 0 ? QStringLiteral("Nothing selected.")
               : QStringLiteral("%1 item%2 across %3 repositor%4. %5")
                     .arg(n)
                     .arg(n == 1 ? "" : "s")
                     .arg(repos)
                     .arg(repos == 1 ? "y" : "ies")
                     .arg(cfg_.dispatchPush ? QStringLiteral("Will commit and push.")
                                            : QStringLiteral("Will commit, never push.")));
    go_->setEnabled(n > 0);
}

void DispatchDialog::accept()
{
    DispatchOptions opt;
    opt.allowCommit = cfg_.dispatchCommit;
    opt.allowPush   = cfg_.dispatchPush;
    opt.maxRepos    = cfg_.dispatchMaxRepos;

    plan_ = buildDispatchPlan(items_, opt);
    if (plan_.items.empty())
        return;
    QDialog::accept();
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

    dl->addStretch(1);

    engage_   = new QPushButton(QStringLiteral("ENGAGE"));
    resume_   = new QPushButton(QStringLiteral("CONTINUE"));
    sessions_ = new QPushButton(QStringLiteral("SESSIONS"));
    terminal_ = new QPushButton(QStringLiteral("TERMINAL"));
    dispatch_ = new QPushButton(QStringLiteral("DISPATCH"));
    engage_->setObjectName(QStringLiteral("Primary"));

    for (QPushButton* b : { engage_, resume_, sessions_, terminal_, dispatch_ })
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

    split->addWidget(detail);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    split->setSizes({ 780, 480 });
    root->addWidget(split, 1);

    grain_ = new GrainOverlay(central);
    grain_->raise();

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
        return;
    }

    detailName_->setText(QString::fromStdString(p->displayName()).toUpper());
    detailPath_->setText(QString::fromStdString(p->path.string())
                         + (p->ownerRepo.empty()
                                ? QString()
                                : QStringLiteral("\n") + QString::fromStdString(p->ownerRepo)));

    if (p->git.lastCommitUnix) {
        detailCommit_->setText(QStringLiteral("%1 — %2")
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
}

void MainWindow::launch(LaunchMode mode)
{
    const Project* p = current();
    if (!p)
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

    // A terminal without Claude Code attached. The config's claude args do not
    // apply, so this goes through the shell rather than the launcher.
    LaunchSpec s;
    s.cwd = p->path;

    std::string err;
    if (!openInTerminal(s, cfg_, &err))
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

void MainWindow::rescan()
{
    counts_->setText(QStringLiteral("scanning"));
    scan_->start(cfg_);
}

} // namespace pm::gui
