#pragma once

#include "agent_board.h"
#include "node_graph.h"

#include <QElapsedTimer>
#include <QPixmap>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

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

    // Whether the key is drawn. On by default, because nobody is born knowing
    // that a broken square with a ~ in it is an armed monitor, and there is no
    // menu to go looking in.
    bool legendVisible() const { return legend_; }
    void setLegendVisible(bool on);

    // Whether shells, monitors and subagents draw a terminal pane of their
    // recent output instead of a one-line label. On by default.
    bool terminalsVisible() const { return terminals_; }
    void setTerminalsVisible(bool on);

    // Tool calls the hook just logged, oldest first, for the list in the top
    // left. Dropped rather than queued while the canvas is hidden, so showing
    // it again does not replay a backlog.
    void addToolCalls(const std::vector<LoggedCall>& calls);

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

    // The session's task list, as a column of rows beside its node. Its own
    // function rather than a branch of drawLabel because it lays out rows, a
    // mark column and an indent, none of which a label has.
    void drawTaskBlock(QPainter& p, const Node& n) const;
    void drawNotice(QPainter& p, const QString& text) const;

    // A miniature terminal beside the node: a title bar carrying the label and
    // the node's last few lines underneath. Stands in for drawLabel on any node
    // with lines to show.
    void drawTerminal(QPainter& p, const Node& n) const;
    bool hasTerminal(const Node& n) const;

    // The overlay. Painted onto the canvas rather than put in a status bar, so
    // windowed and fullscreen are the same picture.
    void drawCounts(QPainter& p) const;
    void drawLegend(QPainter& p) const;
    void drawToolLog(QPainter& p) const;
    void onLogTick();

    // One line of the tool log. `y` glides toward the line's slot, which is
    // what pushes the older lines down when a new one lands on top.
    struct LogLine {
        QString tool;
        QString text;
        qint64  bornMs = 0;   // on logClock_
        qreal   y      = 0;
    };

    NodeGraph graph_;
    Camera    camera_;

    QTimer*       tick_ = nullptr;
    QElapsedTimer clock_;
    qreal         accumulator_ = 0;
    int           quiet_       = 0;   // consecutive settled frames

    bool seeded_     = false;   // a board has landed, however empty
    bool firstShow_  = true;
    bool warmNeeded_ = false;   // structure moved while nobody was looking
    bool legend_     = true;
    bool terminals_  = true;

    // Each pane rendered once and blitted every frame after. Eliding eight rows
    // of text per pane per frame was what the frame budget went on; the text
    // changes every few seconds and the frame loop runs at sixty.
    struct PaneCache {
        std::size_t hash  = 0;
        QPixmap     pixmap;
        std::uint64_t used = 0;
    };
    mutable std::unordered_map<std::string, PaneCache> panes_;
    mutable std::uint64_t                              paneFrame_ = 0;

    QString counts_;

    // Newest first. Its own timer rather than the graph's frame loop: a fading
    // line must not keep the physics awake, and a settled graph must not stop
    // a line mid-fade.
    std::deque<LogLine> log_;
    QTimer*             logTick_ = nullptr;
    QElapsedTimer       logClock_;
    qint64              logLastMs_ = 0;
};

} // namespace pm::gui
