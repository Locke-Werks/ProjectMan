#include "agent_board_window.h"

#include "strutil.h"
#include "theme_qt.h"
#include "widgets.h"

#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QVariant>

// After the Qt headers on purpose. windows.h is a wall of macros and Qt's
// declarations are better off parsed before it lands.
#include <windows.h>

#include <system_error>
#include <utility>

namespace pm::gui {
namespace {

using namespace pm::theme;

// Rebuild this often whatever the directories say. ReadDirectoryChangesW drops
// changes when its buffer overflows, and the board has no way of knowing that
// happened: an empty column and a wedged watcher look the same.
constexpr DWORD kPollMs = 5000;

// One action arrives as a burst. The registry file is rewritten, the hook
// appends, and a turn writes several events in a row, so the wait is given a
// moment to go quiet and the whole burst costs one rebuild.
constexpr DWORD kSettleMs = 150;

constexpr DWORD kWatchFlags =
    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE;

// The kernel writes FILE_NOTIFY_INFORMATION records here and nothing ever reads
// them, but the buffer still has to exist and still has to be DWORD aligned,
// which is why it is declared as an array of DWORD rather than of bytes.
constexpr DWORD kNotifyBytes = 8192;

// Cancel is first because WaitForMultipleObjects reports the lowest signalled
// handle: behind two churning directories, anything further along would be
// reached late or not at all.
constexpr DWORD kCancelSlot = 0;
constexpr DWORD kWakeSlot   = 1;

// A card sits in a fixed column, so the long lines are cut to fit rather than
// left to stretch the layout. The full text stays on the tooltip.
constexpr int kColumnPx   = 250;
constexpr int kCardTextPx = 200;

// One watched directory: the handle, the event the wait sleeps on, and the
// buffer the kernel fills. Non-copyable, because the OVERLAPPED holds a pointer
// into this object for as long as a read is in flight.
class DirWatch {
public:
    DirWatch() = default;
    ~DirWatch() { close(); }

    DirWatch(const DirWatch&)            = delete;
    DirWatch& operator=(const DirWatch&) = delete;

    bool open(const fs::path& dir)
    {
        dir_ = CreateFileW(dir.wstring().c_str(), FILE_LIST_DIRECTORY,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                           nullptr);
        if (dir_ == INVALID_HANDLE_VALUE) {
            dir_ = nullptr;
            return false;
        }

        // Manual reset, so a completion that lands while the wait is off
        // rebuilding is still signalled when the wait comes back.
        ov_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov_.hEvent)
            return false;

        arm();
        return true;
    }

    bool   valid() const { return dir_ != nullptr && ov_.hEvent != nullptr; }
    HANDLE event() const { return ov_.hEvent; }

    // Completes the read that just fired and starts the next one.
    void rearm()
    {
        DWORD bytes = 0;
        GetOverlappedResult(dir_, &ov_, &bytes, FALSE);
        arm();
    }

    void close()
    {
        if (dir_) {
            // Drain the read in flight before the buffer goes. It is a member of
            // this object and the kernel still holds a pointer to it.
            CancelIo(dir_);
            DWORD bytes = 0;
            GetOverlappedResult(dir_, &ov_, &bytes, TRUE);
            CloseHandle(dir_);
            dir_ = nullptr;
        }
        if (ov_.hEvent) {
            CloseHandle(ov_.hEvent);
            ov_.hEvent = nullptr;
        }
    }

private:
    // A failed arm leaves the event reset rather than signalled, so the wait
    // falls back to its timeout instead of spinning on a handle nothing will
    // ever complete.
    void arm()
    {
        ResetEvent(ov_.hEvent);
        ReadDirectoryChangesW(dir_, buffer_, kNotifyBytes, FALSE, kWatchFlags, nullptr, &ov_,
                              nullptr);
    }

    HANDLE     dir_ = nullptr;
    OVERLAPPED ov_{};
    DWORD      buffer_[kNotifyBytes / sizeof(DWORD)]{};
};

QLabel* caption(const QString& text, pm::theme::Rgb colour, int px = 12)
{
    auto* l = new QLabel(text);
    l->setFont(theme::body(px));
    l->setStyleSheet(QStringLiteral("color: %1;").arg(theme::c(colour).name()));
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return l;
}

// A line inside a card. Deliberately not selectable: a label that takes the
// mouse press swallows the click that selects the card under it. The background
// is cleared because the global sheet paints every QWidget black, which on a
// card reads as four black boxes on a lighter surface.
QLabel* cardLine(pm::theme::Rgb colour, const QFont& font)
{
    auto* l = new QLabel;
    l->setFont(font);
    l->setStyleSheet(
        QStringLiteral("color: %1; background: transparent;").arg(theme::c(colour).name()));
    return l;
}

void setElided(QLabel* l, const QString& text, Qt::TextElideMode mode = Qt::ElideRight)
{
    l->setText(l->fontMetrics().elidedText(text, mode, kCardTextPx));
    if (l->text() != text)
        l->setToolTip(text);
}

// The selected card follows the table's rule: the fill moves to the elevated
// surface and the accent is a single red edge, never a red fill. The edge is
// 2px in both states so selecting a card does not shift its text sideways.
QString cardStyle(bool selected)
{
    return QStringLiteral("QFrame#AgentCard { background: %1; border: 1px solid %2;"
                          " border-left: 2px solid %3; border-radius: 2px; }")
        .arg(theme::c(selected ? kElevated : kSurface).name())
        .arg(theme::c(kBorder).name())
        .arg(theme::c(selected ? kRed : kBorder).name());
}

// Which card a click landed on. A QFrame has no clicked signal, and one filter
// on the window beats a widget subclass for something rebuilt wholesale.
constexpr const char* kSessionProperty = "pmSession";

} // namespace

// ------------------------------------------------------------ BoardController

BoardController::BoardController(QObject* parent) : QObject(parent)
{
    // Manual reset on both: a signal raised while the worker is between waits
    // has to still be there when it comes back.
    cancel_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    wake_   = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

BoardController::~BoardController()
{
    shutdown();
    if (cancel_)
        CloseHandle(static_cast<HANDLE>(cancel_));
    if (wake_)
        CloseHandle(static_cast<HANDLE>(wake_));
}

void BoardController::setProjects(ProjectList projects)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        projects_ = std::move(projects);
    }
    if (wake_)
        SetEvent(static_cast<HANDLE>(wake_));
}

void BoardController::start()
{
    shutdown();
    if (cancel_)
        ResetEvent(static_cast<HANDLE>(cancel_));
    worker_ = std::thread(&BoardController::run, this);
}

void BoardController::shutdown()
{
    if (cancel_)
        SetEvent(static_cast<HANDLE>(cancel_));
    if (worker_.joinable())
        worker_.join();
}

void BoardController::run()
{
    const fs::path sessionsDir = sessionRegistryDir();
    const fs::path log         = eventLogPath();
    const fs::path agentsDir   = log.empty() ? fs::path() : log.parent_path();

    // `pm hook` creates this on its first write, which on a machine where no
    // session has run yet is never. CreateFileW cannot open a directory that is
    // not there, and a watch that failed to open is a board that only ever
    // updates on the poll.
    if (!agentsDir.empty()) {
        std::error_code ec;
        fs::create_directories(agentsDir, ec);
    }

    DirWatch watch[2];
    if (!sessionsDir.empty())
        watch[0].open(sessionsDir);
    if (!agentsDir.empty())
        watch[1].open(agentsDir);

    HANDLE    handles[4] = {};
    DirWatch* source[4]  = {};
    DWORD     count      = 0;
    handles[count++]     = static_cast<HANDLE>(cancel_);
    handles[count++]     = static_cast<HANDLE>(wake_);
    for (DirWatch& w : watch) {
        if (!w.valid())
            continue;
        source[count]  = &w;
        handles[count] = w.event();
        ++count;
    }

    const auto consume = [&](DWORD slot) {
        if (slot == kWakeSlot) {
            ResetEvent(static_cast<HANDLE>(wake_));
            return;
        }
        if (DirWatch* w = source[slot])
            w->rearm();
    };

    buildAndPost();

    bool stop = false;
    while (!stop) {
        const DWORD fired = WaitForMultipleObjects(count, handles, FALSE, kPollMs);
        if (fired == WAIT_TIMEOUT) {
            buildAndPost();
            continue;
        }
        // WAIT_OBJECT_0 is zero, so the return value is the index of the handle
        // that fired. Anything at or past `count` is WAIT_FAILED, which there
        // is no recovering from here.
        if (fired == kCancelSlot || fired >= count)
            break;

        consume(fired);

        for (;;) {
            const DWORD again = WaitForMultipleObjects(count, handles, FALSE, kSettleMs);
            if (again == WAIT_TIMEOUT)
                break;
            if (again == kCancelSlot || again >= count) {
                stop = true;
                break;
            }
            consume(again);
        }

        if (!stop)
            buildAndPost();
    }
}

void BoardController::buildAndPost()
{
    ProjectList projects;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        projects = projects_;
    }

    // Both reads and the fold run here rather than on the GUI thread. They open
    // and parse files, which is the whole reason this thread exists.
    const std::vector<RegistryEntry> registry = readRegistry(sessionRegistryDir());
    const std::vector<AgentEvent>    events   = readEvents(eventLogPath());
    const AgentList                  board    = buildBoard(registry, events, projects);

    // Emitted from inside the posted lambda, so the signal runs on the GUI
    // thread as a direct call and the list never crosses as a queued argument.
    QMetaObject::invokeMethod(
        this, [this, board] { emit boardReady(board); }, Qt::QueuedConnection);
}

// ----------------------------------------------------------- AgentBoardWindow

AgentBoardWindow::AgentBoardWindow(ProjectList projects, QWidget* parent)
    : QDialog(parent, Qt::Window)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QStringLiteral("Agents"));
    resize(1180, 740);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(18, 14, 18, 16);
    root->setSpacing(12);

    auto* head    = new QHBoxLayout;
    auto* eyebrow = new TrackedLabel(QStringLiteral("// Agents"), 15, QFont::Bold, 0.18);
    eyebrow->setStyleSheet(QStringLiteral("color: %1;").arg(theme::c(kRed).name()));
    head->addWidget(eyebrow);
    head->addStretch(1);

    counts_ = caption(QStringLiteral("reading"), kFg4, 11);
    counts_->setFont(theme::mono(11));
    head->addWidget(counts_);
    root->addLayout(head);

    auto* board = new QHBoxLayout;
    board->setSpacing(12);

    for (const AgentColumn col : columnOrder()) {
        auto* column = new QWidget;
        column->setMinimumWidth(kColumnPx);

        auto* cl = new QVBoxLayout(column);
        cl->setContentsMargins(0, 0, 0, 0);
        cl->setSpacing(8);

        auto* title = new TrackedLabel(QString::fromLatin1(columnLabel(col)), 11,
                                       QFont::DemiBold, 0.2);
        // The accent goes on the column that is asking for something.
        title->setStyleSheet(
            QStringLiteral("color: %1;")
                .arg(theme::c(col == AgentColumn::NeedsYou ? kRed : kFg3).name()));
        cl->addWidget(title);
        heads_.push_back(title);

        auto* scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

        auto* page = new QWidget;
        auto* pl   = new QVBoxLayout(page);
        pl->setContentsMargins(0, 0, 6, 0);   // room for the scrollbar
        pl->setSpacing(8);
        pl->addStretch(1);

        scroll->setWidget(page);
        cl->addWidget(scroll, 1);
        columns_.push_back(pl);

        board->addWidget(column, 1);
    }
    root->addLayout(board, 1);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);

    focus_    = new QPushButton(QStringLiteral("FOCUS"));
    engage_   = new QPushButton(QStringLiteral("ENGAGE"));
    continue_ = new QPushButton(QStringLiteral("CONTINUE"));
    dispatch_ = new QPushButton(QStringLiteral("DISPATCH"));
    close_    = new QPushButton(QStringLiteral("CLOSE"));

    // Getting to the session that is asking for you is what the board is for.
    focus_->setObjectName(QStringLiteral("Primary"));

    focus_->setToolTip(
        QStringLiteral("Bring the window this session is running in to the front.\n"
                       "Best effort in a Docked Console column: a pane shares its host\n"
                       "window with every other pane, so the right tab cannot be selected."));
    engage_->setToolTip(QStringLiteral("Start a new session in this project."));
    continue_->setToolTip(QStringLiteral("Resume the most recent session in this project."));
    dispatch_->setToolTip(QStringLiteral("Hand this project's outstanding work to a session."));

    for (QPushButton* b : { focus_, engage_, continue_, dispatch_, close_ }) {
        b->setFont(theme::tracked(11, QFont::Bold, 0.14));
        buttons->addWidget(b);
    }
    root->addLayout(buttons);

    connect(focus_, &QPushButton::clicked, this, [this] {
        if (const AgentCard* c = selected(); c && c->pid != 0)
            emit focusRequested(c->pid);
    });
    connect(engage_, &QPushButton::clicked, this, [this] {
        if (const AgentCard* c = selected(); c && !c->cwd.empty())
            emit engageRequested(c->cwd);
    });
    connect(continue_, &QPushButton::clicked, this, [this] {
        if (const AgentCard* c = selected(); c && !c->cwd.empty())
            emit continueRequested(c->cwd);
    });
    connect(dispatch_, &QPushButton::clicked, this, [this] {
        if (const AgentCard* c = selected(); c && !c->cwd.empty())
            emit dispatchRequested(c->cwd);
    });
    // CLOSE, Escape and the title-bar X all end in QDialog's own close path,
    // which hides the window and honours WA_DeleteOnClose by itself. Nothing to
    // override: this window asks nothing on the way out, and a reject() that
    // calls close() re-enters the close already in flight, which the is_closing
    // guard drops, leaving a window that cannot be shut at all.
    connect(close_, &QPushButton::clicked, this, &QWidget::close);

    refreshActions();

    watch_ = std::make_unique<BoardController>();
    connect(watch_.get(), &BoardController::boardReady, this, &AgentBoardWindow::onBoardReady);
    watch_->setProjects(std::move(projects));
    watch_->start();
}

AgentBoardWindow::~AgentBoardWindow()
{
    // First, before any widget goes: the watcher posts to the controller, and
    // what the controller posts rebuilds these columns.
    if (watch_)
        watch_->shutdown();
}

void AgentBoardWindow::setProjects(ProjectList projects)
{
    if (watch_)
        watch_->setProjects(std::move(projects));
}

void AgentBoardWindow::onBoardReady(AgentList board)
{
    board_ = std::move(board);
    rebuildCards();
    refreshActions();
}

void AgentBoardWindow::rebuildCards()
{
    // Cleared and rebuilt whole. The board arrives as one list of a handful of
    // cards, four times a minute at rest, and a model with a view over it would
    // be more machinery than that needs.
    for (QVBoxLayout* column : columns_) {
        while (QLayoutItem* item = column->takeAt(0)) {
            if (QWidget* w = item->widget()) {
                // Unparented first, or it stays on screen until the delete is
                // delivered and overlaps the cards replacing it.
                w->setParent(nullptr);
                w->deleteLater();
            }
            delete item;
        }
    }

    cards_.assign(board_.size(), nullptr);

    const std::vector<AgentColumn>& order = columnOrder();
    for (size_t ci = 0; ci < order.size() && ci < columns_.size(); ++ci) {
        int placed = 0;
        for (size_t i = 0; i < board_.size(); ++i) {
            if (board_[i].column != order[ci])
                continue;
            QFrame* card = buildCard(board_[i]);
            cards_[i]    = card;
            columns_[ci]->addWidget(card);
            ++placed;
        }
        columns_[ci]->addStretch(1);

        if (ci < heads_.size())
            heads_[ci]->setText(QStringLiteral("%1 (%2)")
                                    .arg(QString::fromLatin1(columnLabel(order[ci])))
                                    .arg(placed));
    }

    int live  = 0;
    int needs = 0;
    for (const AgentCard& c : board_) {
        if (c.live)
            ++live;
        if (c.column == AgentColumn::NeedsYou)
            ++needs;
    }
    counts_->setText(board_.empty()
                         ? QStringLiteral("no sessions")
                         : QStringLiteral("%1 live  %2 need you").arg(live).arg(needs));
}

QFrame* AgentBoardWindow::buildCard(const AgentCard& card)
{
    const QString session = QString::fromStdString(card.sessionId);

    auto* frame = new QFrame;
    frame->setObjectName(QStringLiteral("AgentCard"));
    frame->setStyleSheet(cardStyle(session == selected_));
    frame->setCursor(Qt::PointingHandCursor);
    frame->setProperty(kSessionProperty, session);
    frame->installEventFilter(this);

    auto* cl = new QVBoxLayout(frame);
    cl->setContentsMargins(10, 8, 10, 9);
    cl->setSpacing(3);

    auto* name = cardLine(card.live ? kFg1 : kFg3, theme::body(13, QFont::Bold));
    setElided(name, QString::fromStdString(card.name));
    cl->addWidget(name);

    // A session working somewhere ProjectMan does not index has no project name.
    // The tail of its path says more about where it is than the head does.
    auto* project = cardLine(kFg4, theme::mono(11));
    if (card.project.empty())
        setElided(project, QString::fromStdString(card.cwd.string()), Qt::ElideLeft);
    else
        setElided(project, QString::fromStdString(card.project));
    cl->addWidget(project);

    // A card in NeedsYou is on the board to be answered, so the question it
    // stopped to ask goes in front of the tool it stopped inside.
    if (!card.attention.empty()) {
        auto* attention = cardLine(kRed, theme::body(12));
        setElided(attention, QString::fromStdString(card.attention));
        cl->addWidget(attention);
    } else if (!card.activity.empty()) {
        auto* activity = cardLine(kFg2, theme::mono(11));
        setElided(activity, QString::fromStdString(card.activity));
        cl->addWidget(activity);
    }

    if (!card.prompt.empty()) {
        auto* prompt = cardLine(kFg3, theme::body(11));
        setElided(prompt, QString::fromStdString(card.prompt));
        cl->addWidget(prompt);
    }

    const std::int64_t ms = card.lastActivityMs > 0 ? card.lastActivityMs : card.startedAtMs;
    QString            foot = QString::fromStdString(pm::relativeAge(ms / 1000));
    if (card.live && card.pid != 0)
        foot += QStringLiteral("  pid %1").arg(card.pid);

    auto* age = cardLine(kFg4, theme::mono(10));
    age->setText(foot);
    cl->addWidget(age);

    return frame;
}

void AgentBoardWindow::refreshActions()
{
    const AgentCard* c = selected();

    focus_->setEnabled(c != nullptr && c->live && c->pid != 0);

    const bool haveCwd = c != nullptr && !c->cwd.empty();
    engage_->setEnabled(haveCwd);
    continue_->setEnabled(haveCwd);
    dispatch_->setEnabled(haveCwd);
}

void AgentBoardWindow::setSelected(const QString& sessionId)
{
    if (selected_ == sessionId)
        return;

    const int was = indexOf(selected_);
    selected_     = sessionId;
    const int now = indexOf(selected_);

    const auto restyle = [this](int index, bool on) {
        if (index < 0 || static_cast<size_t>(index) >= cards_.size())
            return;
        if (QFrame* card = cards_[static_cast<size_t>(index)])
            card->setStyleSheet(cardStyle(on));
    };
    restyle(was, false);
    restyle(now, true);

    refreshActions();
}

int AgentBoardWindow::indexOf(const QString& sessionId) const
{
    if (sessionId.isEmpty())
        return -1;
    for (size_t i = 0; i < board_.size(); ++i) {
        if (QString::fromStdString(board_[i].sessionId) == sessionId)
            return static_cast<int>(i);
    }
    return -1;
}

const AgentCard* AgentBoardWindow::selected() const
{
    const int i = indexOf(selected_);
    return i < 0 ? nullptr : &board_[static_cast<size_t>(i)];
}

bool AgentBoardWindow::eventFilter(QObject* watched, QEvent* e)
{
    if (e->type() == QEvent::MouseButtonPress) {
        // The card's own labels take no mouse interaction, so a click anywhere
        // on a card arrives here having propagated up to the frame.
        const QVariant session = watched->property(kSessionProperty);
        if (session.isValid())
            setSelected(session.toString());
    }
    return QDialog::eventFilter(watched, e);
}

} // namespace pm::gui
