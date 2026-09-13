#pragma once

#include "config.h"
#include "git.h"
#include "launcher.h"
#include "model.h"
#include "workitems.h"

#include <QDialog>
#include <QMainWindow>
#include <QMetaType>
#include <QPointer>

#include <condition_variable>
#include <mutex>
#include <thread>

class QCheckBox;
class QCloseEvent;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTableView;
class QTableWidget;

Q_DECLARE_METATYPE(pm::Project)

namespace pm::gui {

class AgentBoardWindow;
class GrainOverlay;
class ProjectFilterProxy;
class ProjectModel;
class TrackedLabel;

// Owns the worker thread and marshals every result onto the GUI thread.
//
// The scanner in core takes a StatusSink and calls it from worker threads, so
// this is the whole Qt side of the bridge: copy the value, post it, touch
// nothing else.
class ScanController : public QObject, public pm::StatusSink {
    Q_OBJECT

public:
    explicit ScanController(QObject* parent = nullptr);
    ~ScanController() override;

    void start(const Config& cfg);
    void cancel();

    // StatusSink, called FROM WORKER THREADS.
    void onStatus(std::size_t index, const Project& project) override;
    void onScanFinished(int probed, int failed, double wallSeconds) override;

signals:
    void discovered(int count);
    void rowReady(int row, pm::Project project);
    void sweepFinished(int probed, int failed, double seconds);
    void enrichFinished();

private:
    void run(Config cfg);

    std::thread      worker_;
    pm::CancelToken  token_;
    pm::ProjectList  projects_;
};

// Fetches the selected repository's changed files and recent commits.
//
// One long-lived worker with a single request slot, rather than a thread per
// selection. Arrowing down a list would otherwise start a git process per row
// and finish them out of order; here a newer request replaces an unstarted
// older one, and a result that arrives after the selection moved on is dropped
// by its token.
class DetailController : public QObject {
    Q_OBJECT

public:
    explicit DetailController(QObject* parent = nullptr);
    ~DetailController() override;

    // Returns the token to compare against in the ready handler.
    quint64 request(const pm::fs::path& path, int timeoutMs);

signals:
    void ready(quint64 token, pm::git::RepoDetail detail);

private:
    void loop();

    std::thread             worker_;
    std::mutex              mutex_;
    std::condition_variable wake_;

    pm::fs::path pending_;
    int          timeoutMs_ = 20000;
    quint64      token_     = 0;
    bool         have_      = false;
    bool         stop_      = false;
};

// The per-item selection the user ticks before dispatching.
class DispatchDialog : public QDialog {
    Q_OBJECT

public:
    DispatchDialog(WorkList items, const Config& cfg, QWidget* parent = nullptr);

    DispatchPlan plan() const { return plan_; }

private:
    // The three places that build a plan have to agree, or the summary, the
    // preview and what actually launches drift apart.
    DispatchOptions options() const;

    void rebuildSummary();
    void showPreview();
    void accept() override;

    WorkList      items_;
    const Config& cfg_;
    DispatchPlan  plan_;

    QTableWidget*   table_        = nullptr;
    QLabel*         summary_      = nullptr;
    QPushButton*    go_           = nullptr;
    QPlainTextEdit* instructions_ = nullptr;
};

// The settings surface, built from core's settings() table so the desktop and
// console front ends offer the same settings, in the same order, with the same
// wording. Adding one to that table adds it to both.
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    SettingsDialog(Config cfg, QWidget* parent = nullptr);

    const Config& config() const { return cfg_; }

private:
    void addRow(QFormLayout* form, const Setting& s);
    void refreshDerived();

    Config  cfg_;
    QLabel* autonomyNote_ = nullptr;
    QLabel* launchLine_   = nullptr;

    // Held so it can follow the autonomy ladder while it is unpinned. Left
    // stale it would claim permissions are skipped while the launch line
    // beneath it shows they are not.
    QCheckBox* skipPermissions_ = nullptr;
};

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(Config cfg, QWidget* parent = nullptr);

protected:
    void resizeEvent(QResizeEvent* e) override;
    void closeEvent(QCloseEvent* e) override;

private slots:
    void onDetailReady(quint64 token, pm::git::RepoDetail detail);
    void onRowReady(int row, pm::Project project);
    void onSweepFinished(int probed, int failed, double seconds);
    void onSelectionChanged();
    void rescan();

private:
    const Project* current() const;
    void launch(LaunchMode mode);
    void openTerminal();
    void offerDockDownload();

    // Where every Claude Code launch goes: the dock when it is in play, else a
    // window. `pressed` is the button that asked, shown as DOCKING while a
    // cold dock comes up; null for a launch with no button behind it.
    void launchSpec(const LaunchSpec& s, QPushButton* pressed);

    // Tries to put a launch in a dock column. False means the caller should do
    // what it did before the dock existed: either the dock is not in play, or
    // it refused and a refusal must not leave the button doing nothing.
    bool tryDock(const LaunchSpec& s, bool shell, QPushButton* pressed);

    // True when Shift is down, which forces a loose window for one launch.
    static bool wantsLooseWindow();

    void refreshDockNote();
    void openDispatch();

    // The dispatch dialog scoped to one project, which is what the board's
    // DISPATCH asks for. An empty path, or one the scan does not know, falls
    // back to the whole tree.
    void openDispatchFor(const fs::path& cwd);

    void openSettings();

    // Shows the agent board, raising the one already up rather than opening a
    // second. The window is WA_DeleteOnClose, so boardWindow_ is a QPointer and
    // every later use has to test it.
    void openBoard();

    // Hands the board the scan's latest result, which its cards take their
    // project names from. Called on every sweep and enrich.
    void feedBoard();

    void updateCounts();

    Config cfg_;

    ProjectModel*       model_ = nullptr;
    ProjectFilterProxy* proxy_ = nullptr;
    ScanController*     scan_  = nullptr;

    QTableView*   table_  = nullptr;
    QLineEdit*    filter_ = nullptr;
    TrackedLabel* eyebrow_ = nullptr;
    QLabel*       counts_ = nullptr;
    GrainOverlay* grain_  = nullptr;

    QLabel* detailName_   = nullptr;
    QLabel* detailPath_   = nullptr;
    QLabel* detailCommit_ = nullptr;
    QLabel* detailOpen_   = nullptr;
    QLabel* detailClaude_ = nullptr;

    QPushButton* engage_   = nullptr;
    QPushButton* resume_   = nullptr;
    QPushButton* sessions_ = nullptr;
    QPushButton* terminal_ = nullptr;
    QPushButton* dispatch_ = nullptr;
    QPushButton* agents_   = nullptr;
    QPushButton* settingsBtn_ = nullptr;

    QPointer<AgentBoardWindow> boardWindow_;

    QLabel* dockNote_ = nullptr;

    // What actually changed in the selected repository, fetched off-thread.
    QPlainTextEdit*    inspect_      = nullptr;
    DetailController*  detail_       = nullptr;
    quint64            detailToken_  = 0;

    // Resolved once and after a settings change: dock::available reads the
    // registry, and the note under the buttons is drawn from it.
    bool dockReady_ = false;
};

} // namespace pm::gui
