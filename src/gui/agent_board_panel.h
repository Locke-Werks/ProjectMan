#pragma once

#include "agent_board.h"
#include "model.h"

#include <QMetaType>
#include <QString>
#include <QWidget>

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
// MainWindow owns one of these, the way it owns ScanController and
// DetailController, and every view of the board is a receiver of the one
// boardReady it emits. A second controller would be a second watcher thread, a
// second SessionFileReader and a second set of file reads for the same bytes.
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

    // The scanned tree buildBoard takes project names from, handed over by
    // MainWindow::feedBoard on every sweep. It lands late and changes again at
    // every sweep, so it is set rather than passed in: the worker copies the
    // latest under the lock at the top of each rebuild, and setting it wakes
    // the wait so new names appear now rather than at the next poll.
    void setProjects(ProjectList projects);

    void start();

    // Raises the cancel event and waits for the worker, so nothing can post to
    // this object afterwards.
    void shutdown();

signals:
    // By reference, and every receiver takes it by reference. The emit is
    // arranged to happen on the GUI thread (see buildAndPost), so every
    // connection from it is direct and the list is alive for the whole call:
    // the board is built once, moved once, and copied only by whoever keeps it.
    void boardReady(const pm::gui::AgentList& board);

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

    // Touched only by the worker, which is the one thread that calls read().
    SessionFileReader sessions_;
};

// Every Claude Code session on this machine, as four columns of cards that
// follow the sessions as they move.
//
// A view over an AgentList it is handed and nothing more: it owns no source, no
// thread and no files. MainWindow's BoardController is what feeds it, which is
// also what feeds the node explorer, so the two tabs cannot disagree about the
// same instant. The cost is that a panel nobody feeds shows four empty columns
// for ever; what it buys is that one setBoard call with a list built in a test
// draws the whole board with no registry, no event log and no thread.
//
// A tab of the main window rather than a window of its own. That is the point
// of the count of waiting sessions riding on the tab itself: the answer to "is
// anything asking for me" does not require being on this tab to see.
//
// The panel launches nothing. Each action emits and MainWindow answers, so the
// dock, the autonomy ladder and the Shift override keep being decided in one
// place instead of two.
class AgentBoardPanel : public QWidget {
    Q_OBJECT

public:
    explicit AgentBoardPanel(QWidget* parent = nullptr);

    // The latest board, from MainWindow's BoardController.
    void setBoard(const AgentList& board);

signals:
    // How many cards are in NEEDS YOU. MainWindow puts it on the AGENTS tab,
    // which is the whole reason its watcher runs whichever tab is showing.
    void attentionChanged(int waiting);

    // "3 live  1 need you  5 agents", for the one counts slot in the main
    // window's head row. Held there rather than repeated here, so every tab
    // reads as a view of one window instead of a window in a stack.
    void countsChanged(QString summary);

    // Raise the session's own window. Live cards only: a card in Done carries a
    // pid that belongs to nothing, or by now to something else.
    void focusRequested(unsigned long pid);

    void engageRequested(pm::fs::path cwd);
    void continueRequested(pm::fs::path cwd);
    void dispatchRequested(pm::fs::path cwd);

protected:
    bool eventFilter(QObject* watched, QEvent* e) override;

private:
    void rebuildCards();
    void refreshActions();
    void setSelected(const QString& sessionId);

    QFrame*          buildCard(const AgentCard& card);
    int              indexOf(const QString& sessionId) const;
    const AgentCard* selected() const;

    AgentList board_;
    QString   selected_;   // sessionId, so the selection survives a rebuild

    std::vector<TrackedLabel*> heads_;     // one per column, in columnOrder()
    std::vector<QVBoxLayout*>  columns_;   // the same order; holds the cards
    std::vector<QFrame*>       cards_;     // index for index with board_

    QPushButton* focus_    = nullptr;
    QPushButton* engage_   = nullptr;
    QPushButton* continue_ = nullptr;
    QPushButton* dispatch_ = nullptr;

    // Only re-emitted when it moves. The board rebuilds every few seconds and
    // almost every rebuild changes nothing, so a signal per rebuild would
    // repaint the tab label for no reason.
    int attention_ = -1;
};

} // namespace pm::gui
