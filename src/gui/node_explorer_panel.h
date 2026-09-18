#pragma once

#include "agent_board.h"
#include "node_graph.h"

#include <QElapsedTimer>
#include <QString>
#include <QWidget>

class QHideEvent;
class QPaintEvent;
class QResizeEvent;
class QShowEvent;
class QTimer;

// The same sessions the board shows, as a graph that settles.
//
// A view and nothing else: no panning, no zoom, no selection, no menu. The
// camera fits whatever is on the canvas, work pops in when it starts and pops
// out when it ends, and everything else slides out of the way. Anything that
// can be acted on is a card on the board; this answers the question the board
// cannot, which is what the shape of the work is.
//
// It owns no source. MainWindow's BoardController feeds it the same AgentList
// the board gets, from the same emit, so the two tabs cannot disagree.
//
// Hidden most of the time, which is what the timer rules are about: the frame
// loop runs only while this is the visible tab, and stops again as soon as the
// graph has settled.
namespace pm::gui {

class NodeExplorerPanel : public QWidget {
    Q_OBJECT

public:
    explicit NodeExplorerPanel(QWidget* parent = nullptr);

    // The latest board, from MainWindow's BoardController. Taken by reference
    // and folded straight into nodes: nothing here keeps a copy of the list.
    void setBoard(const AgentList& board);

signals:
    // "3 sessions  2 runs  5 agents", for the main window's counts slot.
    void countsChanged(QString summary);

protected:
    void paintEvent(QPaintEvent*) override;
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    void onTick();
    void wake();
    void warm();
    void publishCounts();

    void drawLinks(QPainter& p) const;
    void drawNode(QPainter& p, const Node& n) const;
    void drawLabel(QPainter& p, const Node& n) const;
    void drawNotice(QPainter& p, const QString& text) const;

    NodeGraph graph_;
    Camera    camera_;

    QTimer*       tick_ = nullptr;
    QElapsedTimer clock_;
    qreal         accumulator_ = 0;
    int           quiet_       = 0;   // consecutive settled frames

    bool seeded_     = false;   // a board has landed, however empty
    bool firstShow_  = true;
    bool warmNeeded_ = false;   // structure moved while nobody was looking

    QString counts_;
};

} // namespace pm::gui
