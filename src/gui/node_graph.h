#pragma once

#include "agent_board.h"

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// The agent board as a shape rather than as four columns.
//
// Same data, folded the other way: every live session is a body, every workflow
// run and subagent under it is a smaller body tied to it by a spring, and the
// whole thing is left to settle. A card says a session has four agents; this
// says what four agents around one session looks like, which is the question
// the board cannot answer because a column has no room for it.
//
// Nothing here knows about a widget. The panel owns the frame loop and the
// painting; this owns the bodies, the forces and the camera, so the layout can
// be stepped and inspected without a window.
namespace pm::gui {

// The fixed substep the integrator is tuned for. The damping constant it uses
// is derived for exactly this dt, so a frame is stepped in whole multiples of
// it rather than by the frame time: the same birth then looks identical at 30
// and at 144 frames a second, and a hitch costs substeps rather than lurching.
inline constexpr qreal kFixedStep = 1.0 / 120.0;

enum class NodeKind {
    Session,   // one live Claude Code session
    Run,       // one Workflow tool run inside it
    Agent,     // one subagent, of a run or of the session itself
};

// A node is born, lives, and is retired when its work ends. Dying is a real
// state rather than a deletion because the node goes on being simulated while
// it shrinks: it retracts toward its parent and stops pushing on its
// neighbours, so the survivors flow into the space it vacates instead of
// snapping into a hole that appeared between two frames.
enum class NodePhase { Born, Alive, Dying };

struct Node {
    NodeKind    kind = NodeKind::Agent;
    std::string key;
    std::string parentKey;    // empty on a session
    int         parentSlot = -1;

    // World units, which are logical pixels at camera scale 1. The only three
    // fields the simulation writes.
    QPointF pos;
    QPointF vel;
    QPointF force;

    qreal radius       = 0;   // chases radiusTarget, so a run grows smoothly
    qreal radiusTarget = 0;
    qreal mass         = 1;

    // What this node hands its children as a spring rest length. A session
    // holds two, because its runs sit further out than its own loose agents.
    qreal restRun         = 0;
    qreal restRunTarget   = 0;
    qreal restAgent       = 0;
    qreal restAgentTarget = 0;

    // Sessions only: how far its furthest descendant reaches, which is what
    // keeps two sessions' subtrees from sitting on top of each other.
    qreal clusterRadius = 0;

    NodePhase phase     = NodePhase::Born;
    qreal     animMs    = 0;   // negative holds a node at nothing until its turn
    qreal     popScale  = 0;
    qreal     alpha     = 0;
    qreal     influence = 0;   // its weight in every force it takes part in

    // Derived once when the board lands, not per frame: none of it changes
    // between refreshes and a switch over AgentColumn has no business in a
    // paint loop.
    QString label;
    bool    labelHidden = false;   // too many of them under one session to read
    QColor  fill;
    QColor  rim;
    QColor  labelColor;
    qreal   rimWidth   = 1.0;
    qreal   progress   = -1.0;   // runs only, 0..1; negative draws no arc
    bool    progressBad = false; // the run has a failed agent in it
    bool    attention   = false; // the session is in NEEDS YOU
    bool    labelRight  = true;  // which side its label sits on

    int           spawnOrdinal = 0;
    std::uint64_t seen         = 0;
};

// The view, which nobody drives. It fits whatever the graph is doing, with a
// margin, and glides rather than snapping.
class Camera {
public:
    // `content` is the box the graph currently occupies. Grows the held fit
    // immediately and shrinks it only after the smaller box has held, because a
    // graph settling inward shrinks its own bounds by a few units a frame and a
    // camera that chased that would pump in and out for the whole settle.
    void retarget(const QRectF& content, qreal elapsedMs);

    void advance(const QSizeF& view, qreal seconds);

    // Take this box as the fit and go there now, with no hysteresis and no
    // glide. What a tab switch needs, and nothing else.
    void frame(const QRectF& content, const QSizeF& view);

    // Straight to the target, for a tab opening on work that happened while it
    // was hidden. Gliding in from where the camera was two minutes ago would
    // read as a loading animation.
    void snap(const QSizeF& view);

    bool    atRest() const { return atRest_; }
    qreal   scale() const { return scale_; }
    QPointF toScreen(const QPointF& world, const QSizeF& view) const;

private:
    qreal targetScale(const QSizeF& view) const;

    QRectF  fit_;
    QPointF centre_;
    qreal   logScale_ = 0;
    qreal   scale_    = 1;
    qreal   shrinkHeldMs_ = 0;
    bool    atRest_   = false;
};

class NodeGraph {
public:
    // Fold a board into the node set, by the stable ids already in it, so a
    // node keeps its position across a refresh that changed nothing about it.
    //
    // `animate` false applies births and deaths instantly. A pop is an event,
    // and an event that happened while the tab was hidden must not be replayed
    // when someone arrives.
    //
    // Returns whether anything worth redrawing moved. Deliberately false for a
    // changed activity line or prompt: those change on nearly every refresh,
    // nothing here draws them, and waking the simulation for them would burn a
    // core for ever on a still picture.
    bool setBoard(const AgentList& board, bool animate);

    void step(qreal dt);          // one fixed substep
    void advance(qreal seconds);  // the animations, on wall time
    void sweepDead();             // retire whatever finished dying

    // Run the layout out in one go, with every node treated as fully arrived.
    // For a tab being opened on work that happened while it was hidden: the
    // alternative is a graph that flies in from its seed positions every time,
    // which reads as a loading state rather than as a view.
    void settle(int steps);

    // Hold every node at nothing and release them in order, which is the one
    // time the whole graph pops in at once.
    void stagger(qreal perNodeMs, qreal capMs);

    // Stop dead. A settled graph keeps a residual drift that is invisible while
    // the timer is stopped and shows as a twitch on the next wake.
    void rest();

    // Every node Alive and slower than the sleep floor.
    bool settled() const;

    // The box the drawn discs occupy. Null when there is nothing to draw.
    QRectF bounds() const;

    const std::vector<Node>& nodes() const { return nodes_; }
    bool  empty() const { return nodes_.empty(); }
    int   count(NodeKind kind) const;

private:
    std::size_t touch(const std::string& key, NodeKind kind, const std::string& parentKey,
                      bool animate, bool* born);
    void        resolveParents();

    std::vector<Node>                            nodes_;
    std::unordered_map<std::string, std::size_t> index_;
    std::uint64_t                                stamp_ = 0;
    int                                          sessionOrdinal_ = 0;
};

} // namespace pm::gui
