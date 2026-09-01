#pragma once

#include "config.h"
#include "launcher.h"
#include "model.h"
#include "workitems.h"

#include <QDialog>
#include <QMainWindow>
#include <QMetaType>

#include <thread>

class QLabel;
class QLineEdit;
class QPushButton;
class QTableView;
class QTableWidget;

Q_DECLARE_METATYPE(pm::Project)

namespace pm::gui {

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

// The per-item selection the user ticks before dispatching.
class DispatchDialog : public QDialog {
    Q_OBJECT

public:
    DispatchDialog(WorkList items, const Config& cfg, QWidget* parent = nullptr);

    DispatchPlan plan() const { return plan_; }

private:
    void rebuildSummary();
    void accept() override;

    WorkList      items_;
    const Config& cfg_;
    DispatchPlan  plan_;

    QTableWidget* table_   = nullptr;
    QLabel*       summary_ = nullptr;
    QPushButton*  go_      = nullptr;
};

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(Config cfg, QWidget* parent = nullptr);

protected:
    void resizeEvent(QResizeEvent* e) override;

private slots:
    void onRowReady(int row, pm::Project project);
    void onSweepFinished(int probed, int failed, double seconds);
    void onSelectionChanged();
    void rescan();

private:
    const Project* current() const;
    void launch(LaunchMode mode);
    void openTerminal();
    void openDispatch();
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
};

} // namespace pm::gui
