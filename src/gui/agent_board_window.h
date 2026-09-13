#pragma once

#include "agent_board.h"
#include "model.h"

#include <QDialog>
#include <QMetaType>
#include <QString>

#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class QEvent;
class QFrame;
class QLabel;
class QPushButton;
class QVBoxLayout;

// The card and not the list. Qt already carries a QMetaTypeId for std::vector,
// so declaring the element is what makes it report AgentList as a type it
// knows; declaring the vector would stand an explicit specialisation up against
// Qt's own partial one.
Q_DECLARE_METATYPE(pm::gui::AgentCard)

namespace pm::gui {

class TrackedLabel;

// Owns the thread that watches the board's two sources and rebuilds it.
//
// Both sources are files other processes write, so there is nothing to
// subscribe to: the session registry and the event log's directory are watched
// with ReadDirectoryChangesW, and one WaitForMultipleObjects covers both of
// those plus cancellation. What a notification says is thrown away. A rebuild
// re-reads both files whatever moved, so the only useful part of a notification
// is that it arrived.
//
// The wait also times out, and a timeout rebuilds. An overflowed notification
// buffer drops changes by design, and a board that quietly stopped updating
// looks exactly like a machine with nothing running on it.
class BoardController : public QObject {
    Q_OBJECT

public:
    explicit BoardController(QObject* parent = nullptr);
    ~BoardController() override;

    // The scanned tree buildBoard takes project names from. It lands late and
    // changes again at every sweep, so it is set rather than passed in: the
    // worker copies the latest under the lock at the top of each rebuild, and
    // setting it wakes the wait so new names appear now rather than at the next
    // poll.
    void setProjects(ProjectList projects);

    void start();

    // Raises the cancel event and waits for the worker, so nothing can post to
    // this object afterwards.
    void shutdown();

signals:
    void boardReady(pm::gui::AgentList board);

private:
    void run();
    void buildAndPost();

    std::thread worker_;

    // Win32 HANDLEs, held as void* so windows.h stays out of a header the rest
    // of the GUI includes. ConsoleSession holds its two the same way.
    void* cancel_ = nullptr;
    void* wake_   = nullptr;

    std::mutex  mutex_;
    ProjectList projects_;   // guarded by mutex_
};

// Every Claude Code session on this machine, as four columns of cards that
// follow the sessions as they move. Non-modal, so the project list stays usable
// behind it.
//
// The window launches nothing. Each action emits and MainWindow answers, so the
// dock, the autonomy ladder and the Shift override keep being decided in one
// place instead of two.
class AgentBoardWindow : public QDialog {
    Q_OBJECT

public:
    explicit AgentBoardWindow(ProjectList projects, QWidget* parent = nullptr);
    ~AgentBoardWindow() override;

    // The scan's result, which cards take their project name from. MainWindow
    // hands over a fresh one whenever a sweep finishes.
    void setProjects(ProjectList projects);

signals:
    // Raise the session's own window. Live cards only: a card in Done carries a
    // pid that belongs to nothing, or by now to something else.
    void focusRequested(unsigned long pid);

    void engageRequested(pm::fs::path cwd);
    void continueRequested(pm::fs::path cwd);
    void dispatchRequested(pm::fs::path cwd);

protected:
    bool eventFilter(QObject* watched, QEvent* e) override;

private:
    void onBoardReady(AgentList board);
    void rebuildCards();
    void refreshActions();
    void setSelected(const QString& sessionId);

    QFrame*          buildCard(const AgentCard& card);
    int              indexOf(const QString& sessionId) const;
    const AgentCard* selected() const;

    AgentList board_;
    QString   selected_;   // sessionId, so the selection survives a rebuild

    // Destroyed before the widgets, which member order guarantees, and shut
    // down first thing in the destructor: the watcher thread posts to it, and
    // what it posts writes into these widgets.
    std::unique_ptr<BoardController> watch_;

    QLabel*                    counts_ = nullptr;
    std::vector<TrackedLabel*> heads_;     // one per column, in columnOrder()
    std::vector<QVBoxLayout*>  columns_;   // the same order; holds the cards
    std::vector<QFrame*>       cards_;     // index for index with board_

    QPushButton* focus_    = nullptr;
    QPushButton* engage_   = nullptr;
    QPushButton* continue_ = nullptr;
    QPushButton* dispatch_ = nullptr;
    QPushButton* close_    = nullptr;
};

} // namespace pm::gui
