#include "dispatch_run_window.h"

#include "theme_qt.h"
#include "widgets.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTimer>
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

QString clock(qint64 ms)
{
    const qint64 seconds = ms / 1000;
    return QStringLiteral("%1:%2")
        .arg(seconds / 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

// ------------------------------------------------------ DispatchRunController

DispatchRunController::DispatchRunController(QObject* parent) : QObject(parent) {}

DispatchRunController::~DispatchRunController() { shutdown(); }

void DispatchRunController::start(DispatchPlan plan, Config cfg)
{
    shutdown();
    token_.reset();
    running_ = true;
    worker_  = std::thread(&DispatchRunController::run, this, std::move(plan), std::move(cfg));
}

void DispatchRunController::stop() { token_.request_stop(); }

void DispatchRunController::shutdown()
{
    token_.request_stop();
    if (worker_.joinable())
        worker_.join();
    running_ = false;
}

void DispatchRunController::run(DispatchPlan plan, Config cfg)
{
    const RunOutcome out = runDispatch(plan, cfg, token_, *this);

    // Emitted from inside the posted lambda, so the signals run on the GUI
    // thread as direct calls and nothing crosses as a queued argument.
    QMetaObject::invokeMethod(
        this,
        [this, headline = QString::fromStdString(out.summary.headline),
         state     = static_cast<int>(out.summary.state),
         sessionId = QString::fromStdString(out.stats.sessionId)] {
            running_ = false;
            if (!sessionId.isEmpty())
                emit sessionKnown(sessionId);
            emit finished(headline, state);
        },
        Qt::QueuedConnection);
}

void DispatchRunController::onEvent(const claude::Event& e)
{
    // Reader thread. Render here, where the event is, and post only what the
    // window needs to draw.
    const bool    init    = e.kind == claude::EventKind::Init;
    const bool    tool    = e.kind == claude::EventKind::ToolUse;
    const QString session = QString::fromStdString(e.sessionId);

    QString text;
    int     style   = -1;
    if (const auto line = claude::render(e)) {
        text  = QString::fromStdString(line->text);
        style = static_cast<int>(line->style);
    }

    QMetaObject::invokeMethod(
        this,
        [this, init, tool, session, text, style] {
            if (init && !session.isEmpty())
                emit sessionKnown(session);
            if (tool)
                emit toolCall();
            if (style >= 0)
                emit lineReady(text, style);
        },
        Qt::QueuedConnection);
}

// ---------------------------------------------------------- DispatchRunWindow

DispatchRunWindow::DispatchRunWindow(DispatchPlan plan, const Config& cfg, bool dockAvailable,
                                     QWidget* parent)
    : QDialog(parent, Qt::Window), plan_(std::move(plan)), cfg_(cfg)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QStringLiteral("Dispatch run"));
    resize(960, 720);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(18, 14, 18, 16);
    root->setSpacing(12);

    root->addWidget(new TrackedLabel(QStringLiteral("// Dispatch run"), 15, QFont::Bold, 0.18));

    const std::vector<std::string> head = claude::describeRun(plan_, cfg_);
    if (!head.empty())
        root->addWidget(caption(QString::fromStdString(head.front()), kFg3));

    QStringList repos;
    for (size_t i = 1; i < head.size(); ++i)
        repos << QString::fromStdString(head[i]).trimmed();
    if (!repos.isEmpty()) {
        auto* where = caption(repos.join(QLatin1Char('\n')), kFg4, 11);
        where->setFont(theme::mono(11));
        root->addWidget(where);
    }

    transcript_ = new QPlainTextEdit(this);
    transcript_->setReadOnly(true);
    transcript_->setFont(theme::mono(11));
    transcript_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    transcript_->setPlaceholderText(QStringLiteral("Starting claude..."));
    root->addWidget(transcript_, 1);

    status_ = caption(QStringLiteral("STARTING"), kFg3, 11);
    status_->setFont(theme::mono(11));
    root->addWidget(status_);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);

    stop_     = new QPushButton(QStringLiteral("STOP"));
    copy_     = new QPushButton(QStringLiteral("COPY"));
    continue_ = new QPushButton(dockAvailable ? QStringLiteral("CONTINUE IN DOCK")
                                              : QStringLiteral("CONTINUE"));
    close_    = new QPushButton(QStringLiteral("CLOSE"));
    // The action that matters once the run is over. Disabled until then, in
    // the Primary style's own disabled look, rather than swapping styles on a
    // button while it is on screen.
    continue_->setObjectName(QStringLiteral("Primary"));
    continue_->setEnabled(false);
    continue_->setToolTip(QStringLiteral(
        "Resume this session interactively, with the same repositories."));

    for (QPushButton* b : { stop_, copy_, continue_, close_ })
        b->setFont(theme::tracked(11, QFont::Bold, 0.14));

    buttons->addWidget(stop_);
    buttons->addWidget(copy_);
    buttons->addWidget(continue_);
    buttons->addWidget(close_);
    root->addLayout(buttons);

    connect(stop_, &QPushButton::clicked, this, [this] {
        stopping_ = true;
        stop_->setEnabled(false);
        status_->setText(QStringLiteral("STOPPING"));
        run_->stop();
    });
    connect(copy_, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(transcript_->toPlainText());
    });
    connect(continue_, &QPushButton::clicked, this, &DispatchRunWindow::requestContinue);
    connect(close_, &QPushButton::clicked, this, &QWidget::close);

    run_ = std::make_unique<DispatchRunController>();
    connect(run_.get(), &DispatchRunController::lineReady, this, &DispatchRunWindow::appendLine);
    connect(run_.get(), &DispatchRunController::toolCall, this, [this] { ++toolCalls_; });
    connect(run_.get(), &DispatchRunController::sessionKnown, this,
            [this](const QString& id) { sessionId_ = id; });
    connect(run_.get(), &DispatchRunController::finished, this, &DispatchRunWindow::onFinished);

    ticker_ = new QTimer(this);
    ticker_->setInterval(1000);
    connect(ticker_, &QTimer::timeout, this, &DispatchRunWindow::tick);
    ticker_->start();
    clock_.start();

    run_->start(plan_, cfg_);
}

DispatchRunWindow::~DispatchRunWindow()
{
    // First, before any widget goes: the worker posts to the controller, and
    // the controller's slots write into these widgets.
    if (run_)
        run_->shutdown();
}

bool DispatchRunWindow::running() const { return run_ && run_->running(); }

void DispatchRunWindow::stopNow()
{
    if (run_)
        run_->shutdown();
}

void DispatchRunWindow::appendLine(const QString& text, int style)
{
    QScrollBar* bar    = transcript_->verticalScrollBar();
    const bool  pinned = bar->value() >= bar->maximum() - 4;

    pm::theme::Rgb colour = kFg2;
    switch (static_cast<claude::LineStyle>(style)) {
    case claude::LineStyle::Text:  colour = kFg2; break;
    case claude::LineStyle::Tool:  colour = kFg4; break;
    case claude::LineStyle::Error: colour = kRed; break;
    case claude::LineStyle::Raw:
    case claude::LineStyle::Meta:  colour = kFg3; break;
    }

    QTextCharFormat fmt;
    fmt.setForeground(theme::c(colour));

    QTextCursor cursor(transcript_->document());
    cursor.movePosition(QTextCursor::End);
    if (!transcript_->document()->isEmpty())
        cursor.insertBlock();
    cursor.insertText(text, fmt);

    // A message from the session reads as a paragraph, so it gets air below.
    if (static_cast<claude::LineStyle>(style) == claude::LineStyle::Text)
        cursor.insertBlock();

    // Follow the output only while the reader was already at the bottom.
    // Someone scrolled up to read something is left where they are.
    if (pinned)
        bar->setValue(bar->maximum());
}

void DispatchRunWindow::onFinished(const QString& headline, int state)
{
    finished_ = true;
    ticker_->stop();

    status_->setText(headline);
    appendLine(headline, static_cast<int>(claude::LineStyle::Meta));

    stop_->setEnabled(false);
    // A stopped run still has its transcript on disk, so it can be picked up
    // as well as a finished one. Only a run that never started cannot.
    continue_->setEnabled(!sessionId_.isEmpty());

    (void)state;
}

void DispatchRunWindow::tick()
{
    if (finished_)
        return;
    status_->setText(QStringLiteral("%1  %2  %3 tool call%4")
                         .arg(stopping_ ? QStringLiteral("STOPPING") : QStringLiteral("RUNNING"))
                         .arg(clock(clock_.elapsed()))
                         .arg(toolCalls_)
                         .arg(toolCalls_ == 1 ? "" : "s"));
}

void DispatchRunWindow::requestContinue()
{
    if (sessionId_.isEmpty())
        return;
    emit continueRequested(continueSpec(plan_, cfg_, sessionId_.toStdString()));
}

void DispatchRunWindow::closeEvent(QCloseEvent* e)
{
    if (running()) {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("ProjectMan"),
            QStringLiteral("The dispatch is still running. Stop it and close?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            e->ignore();
            return;
        }
        stopNow();
    }
    e->accept();
}

void DispatchRunWindow::reject()
{
    // Escape. The same path as CLOSE, so a running dispatch is never dropped
    // without the question.
    close();
}

} // namespace pm::gui
