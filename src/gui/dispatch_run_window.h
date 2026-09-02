#pragma once

#include "config.h"
#include "dispatch_run.h"
#include "launcher.h"
#include "workitems.h"

#include <QDialog>
#include <QElapsedTimer>
#include <QMetaType>

#include <memory>
#include <thread>

class QCloseEvent;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTimer;

Q_DECLARE_METATYPE(pm::LaunchSpec)

namespace pm::gui {

// Owns the worker thread that runs `claude -p` and marshals every event onto
// the GUI thread. The same shape as ScanController: the core calls the sink
// from a reader thread, and the only thing done there is to render the event
// and post the result.
class DispatchRunController : public QObject, public pm::RunSink {
    Q_OBJECT

public:
    explicit DispatchRunController(QObject* parent = nullptr);
    ~DispatchRunController() override;

    void start(DispatchPlan plan, Config cfg);

    // Raises the token and returns at once; `finished` follows once the child
    // is gone.
    void stop();

    // Raises the token and waits for the worker, so nothing can post to this
    // object afterwards.
    void shutdown();

    bool running() const { return running_; }

    // RunSink, called FROM A READER THREAD.
    void onEvent(const claude::Event& e) override;

signals:
    void lineReady(QString text, int style);
    void toolCall();
    void sessionKnown(QString sessionId);
    void finished(QString headline, int state);

private:
    void run(DispatchPlan plan, Config cfg);

    std::thread     worker_;
    pm::CancelToken token_;
    bool            running_ = false;   // GUI thread only
};

// The run, shown as it happens: what was dispatched, each thing the session
// says and does, and how it ended. Non-modal, so the list stays usable.
class DispatchRunWindow : public QDialog {
    Q_OBJECT

public:
    DispatchRunWindow(DispatchPlan plan, const Config& cfg, bool dockAvailable,
                      QWidget* parent = nullptr);
    ~DispatchRunWindow() override;

    bool running() const;

    // Stops the run and waits for the child to be gone. For a main window
    // that is closing.
    void stopNow();

signals:
    // CONTINUE: resume the session interactively, with the same repositories.
    void continueRequested(pm::LaunchSpec spec);

protected:
    void closeEvent(QCloseEvent* e) override;
    void reject() override;

private:
    void appendLine(const QString& text, int style);
    void onFinished(const QString& headline, int state);
    void tick();
    void requestContinue();

    DispatchPlan plan_;
    Config       cfg_;

    // Destroyed before the widgets, which member order guarantees, and shut
    // down first thing in the destructor, so a reader thread can never post
    // to a controller whose window is half gone.
    std::unique_ptr<DispatchRunController> run_;

    QPlainTextEdit* transcript_ = nullptr;
    QLabel*         status_     = nullptr;
    QPushButton*    stop_       = nullptr;
    QPushButton*    copy_       = nullptr;
    QPushButton*    continue_   = nullptr;
    QPushButton*    close_      = nullptr;

    QTimer*       ticker_ = nullptr;
    QElapsedTimer clock_;
    int           toolCalls_ = 0;
    QString       sessionId_;
    bool          finished_ = false;
    bool          stopping_ = false;
};

} // namespace pm::gui
