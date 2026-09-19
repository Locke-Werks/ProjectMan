#include "node_graph.h"

#include "theme_qt.h"

#include <QDateTime>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>

namespace pm::gui {
namespace {

using namespace pm::theme;

// ------------------------------------------------------------------ the bodies

// World units, which are logical pixels at camera scale 1. A session is big
// enough to carry a name under it at the zoom clamp; an agent is small enough
// that a run with a hundred of them is still a shape the camera can frame.
constexpr qreal kSessionRadius = 26.0;
constexpr qreal kRunRadius     = 15.0;
constexpr qreal kAgentRadius   = 8.0;

// The boxes. Half the side of the square, so a shell is 20 units across
// against an agent's 16 diameter: near enough to sit in the same crowd, far
// enough that the shape reads before the size does. A todo is smaller because
// there are many of them and none is a running thing.
constexpr qreal kShellRadius   = 10.0;
constexpr qreal kMonitorRadius = 10.0;
constexpr qreal kToolRadius    = 7.0;
constexpr qreal kTasksRadius   = 9.0;

// Half a row of the block, per row, added to what the task node asks the layout
// for. Ten rows at font size 10 is about 140 logical pixels tall, so half of
// that is 70 world units at scale 1, and this lands near it without the node
// itself growing at all. See Node::spaceRadius.
constexpr qreal kTasksRowSpace = 7.0;
constexpr qreal kPlanRadius    = 9.0;

// A run that spawned forty agents should read as a bigger thing than one that
// spawned two, without reading as a second session. log2 because the difference
// worth seeing is between 2 and 20, not between 100 and 173.
constexpr qreal kRunGrowth    = 2.2;
constexpr qreal kRunRadiusMax = 26.0;

// Mass is what makes the hierarchy read as one. Every force below is equal and
// opposite, so a session barely moves when its agents pull on it purely because
// it is six times heavier than they are.
constexpr qreal kSessionMass = 6.0;
constexpr qreal kRunMass     = 2.5;
constexpr qreal kAgentMass   = 1.0;

// A box is lighter than an agent so it gets out of the way rather than shoving
// a subtree around: what a session has spawned is the structure, and what it is
// doing hangs off the side of it.
constexpr qreal kWorkMass = 0.8;

// --------------------------------------------------------------- the repulsion

// F = kRepelScale * r_i * r_j / d^2, which is 100 force units for ANY pair at
// the moment their rims touch: 400 r^2 / (2r)^2 = 100. One constant therefore
// gives a session, a run and an agent the same sense of contact without a
// second table of per-kind charges.
constexpr qreal kRepelScale = 400.0;

// Shifted so the force reaches zero at the cutoff rather than stepping off it.
// Two agents 260 apart are worth a third of a force unit, which is under the
// noise floor of everything else, so the far field is not worth computing.
constexpr qreal kRepelCutoff    = 260.0;
constexpr qreal kRepelSoftening = 6.0;

// Two nodes on the same point is a division by zero. Past this they are pushed
// apart along a direction derived from their slots, so a coincident pair always
// separates the same way rather than chattering between two directions.
constexpr qreal kCoincident2 = 1e-4;

// ------------------------------------------------------------------ the links

// Centre to centre, each clearing both rims with a gap you can see a link
// through.
constexpr qreal kRestSessionRun   = 120.0;
constexpr qreal kRestSessionAgent = 86.0;
constexpr qreal kRestRunAgent     = 64.0;

// Boxes sit further out than loose agents and closer in than runs. They carry
// two lines of text each, so the ring they occupy has to be wide enough for the
// text not to cross the ring inside it.
constexpr qreal kRestSessionWork = 104.0;

// A subagent to a shell it opened. Shorter than the session's ring: an agent is
// half the radius of a session and carries far less around it, so a box at the
// session's distance would read as belonging to the session behind it.
constexpr qreal kRestAgentWork = 58.0;

// n children of one parent pack into a disc of radius spacing*sqrt(n/pi).
// Feeding that back in as the rest length is what keeps a forty-agent run in
// the regime the spring was tuned for: without it the spring is stretched by
// its own crowd and the layout becomes a tug of war between saturated forces.
// 24 is two agent discs with a gap between them.
constexpr qreal kAgentSpacing = 24.0;
constexpr qreal kRunSpacing   = 60.0;

// Wider than an agent's, because a box's text runs sideways out of it and two
// boxes packed to their rims have their labels written over each other.
constexpr qreal kWorkSpacing = 46.0;

// Hooke, and no more than Hooke. With the damping below this puts an agent at a
// damping ratio just over 1: it slides into place in about a second and a half
// and never oscillates.
constexpr qreal kSpringStiffness = 6.0;

// Insurance against a rest-length bug, not a shaping tool. Nothing in ordinary
// running comes near it.
constexpr qreal kMaxSpringForce = 2500.0;

// ------------------------------------------------- keeping the clusters apart

// Two sessions' subtrees as discs that refuse to interpenetrate. Nine pairs at
// worst, so it is free, and it is the only thing that can separate two clusters
// wider than the repulsion cutoff: pairwise repulsion cannot see across one.
constexpr qreal kClusterPush = 20.0;
constexpr qreal kClusterGap  = 40.0;

// Insurance only. Every internal force is equal and opposite, so the graph
// cannot translate itself and the fence never fires in practice.
constexpr qreal kFenceRadius = 900.0;
constexpr qreal kFenceRamp   = 120.0;
constexpr qreal kFenceForce  = 900.0;

// ------------------------------------------------------------- the integration

// v *= this every substep, for the fixed substep the panel uses. The rate is
// 5.0 per second, chosen against kSpringStiffness to land on critical damping.
// It is a per-step constant and not a per-second one, which is the whole reason
// the timestep is fixed rather than the frame time.
constexpr qreal kDampPerStep = 0.959190;

// A node crossing the canvas in under a second is already wrong. This is here
// so a pathological force cannot put one at a million and take the camera.
constexpr qreal kMaxSpeed = 1400.0;

// How fast a smoothed quantity chases its target, per second. 2.5 is about a
// second to arrive: a run that gains forty agents opens its disc over a second
// instead of flinging them.
constexpr qreal kSmoothRate = 2.5;

// ----------------------------------------------------------------- the births

// Consecutive children of one parent fan out instead of stacking, which is what
// the golden angle is for. 2.39996 radians.
constexpr qreal kGoldenAngle = 2.39996322972865332;

constexpr qreal kBirthOffsetFraction = 0.45;   // of the parent's rest length
constexpr qreal kBirthSpeed          = 40.0;   // world units per second, outward
constexpr qreal kSessionBirthRadius  = 200.0;

// A node that was already there and came back starts its pop partway in, so a
// refresh that momentarily lost a card does not replay the whole birth.
constexpr qreal kResurrectFraction = 0.35;

// ------------------------------------------------------------------- the pop

// A pop has to be over before you have finished registering it, or the canvas
// reads as laggy rather than as alive. An arrival is worth more attention than
// a departure, so it gets longer.
constexpr qreal kBirthMs = 340.0;
constexpr qreal kDeathMs = 260.0;

// How long a tool call stays on the canvas after it started.
//
// Nothing records when a tool call ends, so this is the whole lifetime of the
// box and not a grace period after one. Twelve seconds against a board that
// rebuilds every five: long enough that a call is still there when the next
// refresh lands, so a box fades rather than blinking, and short enough that
// what is on screen is what is happening now.
constexpr std::int64_t kToolLingerMs = 12000;

// easeOutBack and easeInBack. c1 = c3 - 1 in both, which is what makes the
// curve exactly zero at the closed end: a newborn starts at no radius at all
// rather than at a visible dot, and a corpse ends at nothing.
constexpr qreal kBirthBackC1 = 2.0;
constexpr qreal kBirthBackC3 = 3.0;
constexpr qreal kDeathBackC1 = 1.7;
constexpr qreal kDeathBackC3 = 2.7;

// Opaque well before it has finished growing, so a node pops in solid and then
// settles rather than fading up while it swells.
constexpr qreal kBirthAlphaFraction = 0.55;

// How much of the birth a node's push on its neighbours ramps over. This is the
// single thing that makes a settle look organic: a newborn arriving at full
// strength detonates, and everything around it jumps.
constexpr qreal kBirthInfluenceFraction = 0.60;

// A dying node's spring pulls it home as it goes, so it vacates its space
// before it disappears and the survivors flow in rather than snapping in.
constexpr qreal kDeathRestFraction = 0.25;

// ---------------------------------------------------------------- the camera

// Screen-space margin in logical pixels. The graph fills everything inside it,
// whatever the graph happens to be: one session alone is a large circle rather
// than a small one adrift in a black field, and the canvas earns its space at
// every size.
constexpr qreal kMarginX = 100.0;
constexpr qreal kMarginY = 100.0;

// Room under the content for a session's name, which is drawn at a fixed size
// below its node and is therefore not in the box being fitted. Taken off the
// available height rather than added to the box, which would make the fit a
// function of the scale it produces.
constexpr qreal kLabelAllowance = 30.0;

// Not a look, a guard. The fit decides the scale, and the only way it asks for
// more than this is a box that has collapsed to nothing, which is a bug rather
// than a picture. The floor is the same in reverse: past it the nodes are
// smaller than their own hairlines.
constexpr qreal kMinScale = 0.15;
constexpr qreal kMaxScale = 100.0;

// Per second, frame-rate corrected where it is used. 3.2 is 96% of the way
// there in a second.
constexpr qreal kCameraRate = 3.2;

// An exponential approach never actually arrives, and the sleep rule needs it
// to, so inside these it is assigned and declared at rest.
constexpr qreal kCameraSnapScale  = 0.002;   // in log space
constexpr qreal kCameraSnapCentre = 0.4;     // world units

// Grow at once; shrink or recentre only after the new box has held. The slack
// is a fraction of the held fit rather than a distance in world units: the zoom
// runs from one session filling the canvas to a hundred agents in it, and a
// fixed slack is either a third of the frame or invisible depending on which.
//
// It is the deadband for both tests in retarget, which is what keeps a settling
// graph from dragging the camera around by a few units a frame. The hold is
// what makes a departure read as deliberate rather than twitchy: the node takes
// kDeathMs to leave, and the camera waits this long after the box has stopped
// changing before it commits to the new one. The glide itself is kCameraRate.
constexpr qreal kFitShrinkSlack   = 0.04;
constexpr qreal kFitShrinkHoldMs  = 400.0;

// Nothing is moving. 0.5 world units per second is a tenth of a pixel a frame
// at the zoom clamp, which is under the threshold at which anything is visible.
constexpr qreal kSleepSpeed = 0.5;

// ----------------------------------------------------------------- the labels

// World units a node has to be clear of its parent's centre line before its
// label changes sides.
constexpr qreal kSideHysteresis = 6.0;

// A run with a hundred agents cannot have a hundred readable labels at any
// zoom, and a hundred unreadable ones are worse than none: they turn the thing
// into a texture. Counted per session, and only agents are ever suppressed.
constexpr int kLabelCrowdLimit = 20;

// -------------------------------------------------------------------- helpers

constexpr char kSep = '\x1f';   // cannot appear in any id Claude Code writes

// Unix milliseconds. Qt's rather than <chrono>'s, because this file already has
// Qt and QDateTime is what the rest of the GUI would reach for.
std::int64_t nowUnixMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

std::string sessionKey(const std::string& session)
{
    return std::string("S") + kSep + session;
}

std::string runKey(const std::string& session, const std::string& run)
{
    return std::string("R") + kSep + session + kSep + run;
}

// The boxes. A distinct prefix each, so a task and a todo that happen to share
// an id are still two nodes, and so a node keeps its position when it changes
// nothing but its text.
std::string taskKey(const std::string& session, const std::string& task)
{
    return std::string("K") + kSep + session + kSep + task;
}

// One per CALL, keyed by tool_use_id.
//
// It was one per session, holding whatever had been called most recently, and
// that was the wrong shape: a session making three calls a second retitled one
// box faster than it could be read. A key per call gives each one its own node,
// its own birth, and its own death when its linger runs out.
std::string toolKey(const std::string& session, const std::string& call)
{
    return std::string("T") + kSep + session + kSep + call;
}

// One per session: the list is a block, not a node per item.
std::string tasksKey(const std::string& session)
{
    return std::string("D") + kSep + session;
}

std::string planKey(const std::string& session)
{
    return std::string("P") + kSep + session;
}

std::string agentKey(const std::string& session, const std::string& agent)
{
    return std::string("A") + kSep + session + kSep + agent;
}

// A box: something the session is doing, rather than something it spawned.
// Everything that is true of one of these is true of all of them, which is why
// this is a predicate and not five cases repeated at every call site.
bool isWork(NodeKind kind)
{
    switch (kind) {
    case NodeKind::Shell:
    case NodeKind::Monitor:
    case NodeKind::Tool:
    case NodeKind::Tasks:
    case NodeKind::Plan:
        return true;
    case NodeKind::Session:
    case NodeKind::Run:
    case NodeKind::Agent:
        break;
    }
    return false;
}

qreal massFor(NodeKind kind)
{
    switch (kind) {
    case NodeKind::Session: return kSessionMass;
    case NodeKind::Run:     return kRunMass;
    case NodeKind::Agent:   return kAgentMass;
    case NodeKind::Shell:
    case NodeKind::Monitor:
    case NodeKind::Tool:
    case NodeKind::Tasks:
    case NodeKind::Plan:    return kWorkMass;
    }
    return kAgentMass;
}

qreal radiusFor(NodeKind kind)
{
    switch (kind) {
    case NodeKind::Session: return kSessionRadius;
    case NodeKind::Run:     return kRunRadius;
    case NodeKind::Agent:   return kAgentRadius;
    case NodeKind::Shell:   return kShellRadius;
    case NodeKind::Monitor: return kMonitorRadius;
    case NodeKind::Tool:    return kToolRadius;
    case NodeKind::Tasks:   return kTasksRadius;
    case NodeKind::Plan:    return kPlanRadius;
    }
    return kAgentRadius;
}

// The rest length a parent hands n children, so the disc they pack into has
// room for all of them.
qreal restFor(qreal base, qreal spacing, int children)
{
    if (children <= 1)
        return base;
    const qreal packed = spacing * std::sqrt(static_cast<qreal>(children) / 3.14159265358979324);
    return std::max(base, packed);
}

qreal smoothstep(qreal x) { return x * x * (3.0 - 2.0 * x); }

qreal easeOutCubic(qreal x)
{
    const qreal inv = 1.0 - x;
    return 1.0 - inv * inv * inv;
}

qreal easeOutBack(qreal t)
{
    const qreal u = t - 1.0;
    return 1.0 + kBirthBackC3 * u * u * u + kBirthBackC1 * u * u;
}

qreal easeInBackDown(qreal t)
{
    return 1.0 - (kDeathBackC3 * t * t * t - kDeathBackC1 * t * t);
}

qreal length(const QPointF& p) { return std::hypot(p.x(), p.y()); }

// The one place a node's colours are decided, so the canvas and the board read
// the same state the same way. Status colours stay out of it: the design
// language keeps red as the only accent and confines the rest to badge chips.
void paintSession(Node& n, AgentColumn column)
{
    n.fill      = theme::c(kSurface);
    n.rimWidth  = 2.0;
    n.attention = column == AgentColumn::NeedsYou;

    switch (column) {
    case AgentColumn::NeedsYou:
        n.rim        = theme::c(kRed);
        n.labelColor = theme::c(kFg1);
        break;
    case AgentColumn::Working:
        n.rim        = theme::c(kFg2);
        n.labelColor = theme::c(kFg1);
        break;
    case AgentColumn::Idle:
    case AgentColumn::Done:
        n.rim        = theme::c(kFg4);
        n.labelColor = theme::c(kFg3);
        break;
    }
}

void paintAgent(Node& n, const SubAgent& a)
{
    n.rimWidth   = 1.0;
    n.labelColor = theme::c(kFg3);
    if (a.failed) {
        n.fill = theme::c(kRedDark);
        n.rim  = theme::c(kRed);
        return;
    }

    switch (a.state) {
    case AgentState::Running:
        n.fill = theme::c(kElevated);
        n.rim  = theme::c(kFg2);
        break;
    // Dimmer on purpose, and for the reason the board's own tooltip gives: a
    // meta file proves it started and nothing has spoken for it since, which is
    // a weaker claim and should look like one.
    case AgentState::Spawned:
    case AgentState::Finished:
        n.fill = theme::c(kSurface);
        n.rim  = theme::c(kFg4);
        break;
    }
}

// Past this the indent eats more width than the nesting is worth showing.
constexpr int kMaxTaskIndent = 3;

// What a block shows before it starts leaving things out, and how much of that
// budget goes to what is already finished. A forty-item list is read for what is
// being worked on and what is left, so the done items are the ones that give way.
constexpr std::size_t kMaxTaskLines = 9;
constexpr std::size_t kMaxTaskDone  = 2;

// Drop what a long list can spare, oldest finished item first.
//
// What is dropped is still counted: the node keeps the totals and the block says
// how many it is not showing, so a cut list never reads as a short one.
void trimTaskLines(std::vector<TaskLine>* lines)
{
    if (!lines || lines->size() <= kMaxTaskLines)
        return;

    std::size_t seenDone = 0;
    for (const TaskLine& l : *lines) {
        if (l.done)
            ++seenDone;
    }

    std::size_t dropDone = seenDone > kMaxTaskDone ? seenDone - kMaxTaskDone : 0;
    if (dropDone > 0) {
        std::vector<TaskLine> kept;
        kept.reserve(lines->size());
        for (TaskLine& l : *lines) {
            if (l.done && dropDone > 0) {
                --dropDone;
                continue;
            }
            kept.push_back(std::move(l));
        }
        *lines = std::move(kept);
    }

    // Still too long: keep the head, because the active item and everything
    // blocked behind it are at the top of what is left.
    if (lines->size() > kMaxTaskLines)
        lines->resize(kMaxTaskLines);
}

// Whether a rebuilt block says the same thing as the one already on the node.
// Only what is drawn is compared, which is what keeps a refresh that changed
// nothing from waking the simulation.
bool sameTaskLines(const std::vector<TaskLine>& a, const std::vector<TaskLine>& b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].mark != b[i].mark || a[i].depth != b[i].depth || a[i].text != b[i].text)
            return false;
    }
    return true;
}

// The boxes. Every one of them is square, dim-filled and labelled beside
// itself; what separates them is the glyph and whether the rim is solid.
//
// THE RIM IS THE STATE, and it carries exactly one bit. Solid means something
// is executing this instant: a shell with its output file still held open, the
// tool call in flight. Broken means armed or inert, nothing running: a monitor
// between two events is waiting, not working, and a plan is a document that was
// written once. That is the only state these carry, so every paint function
// below sets it rather than leaving it to the default, because a rim that is
// right by accident is one that goes wrong when a node changes hands.
//
// THE GLYPH IS THE KIND, one character each and all of them distinct:
//
//   >  shell        a command, running
//   ~  monitor      a watch, armed
//   .  tool         whatever the session is calling right now
//   #  task list    the session's items, drawn as a block beside it
//   =  plan         the plan document it wrote
void paintShell(Node& n, const BackgroundTask& t)
{
    n.shape      = NodeShape::Box;
    n.rimWidth   = 1.2;
    n.labelColor = theme::c(kFg3);
    n.glyph      = QChar('>');
    n.fill       = theme::c(kElevated);
    n.rim        = theme::c(kFg2);
    n.dashed     = false;
    (void)t;
}

void paintMonitor(Node& n)
{
    n.shape      = NodeShape::Box;
    n.rimWidth   = 1.2;
    n.labelColor = theme::c(kFg3);
    n.glyph      = QChar('~');
    n.fill       = theme::c(kSurface);
    n.rim        = theme::c(kFg2);
    // Armed, not working: a monitor between two events is running nothing.
    n.dashed = true;
}

void paintTool(Node& n)
{
    n.shape      = NodeShape::Box;
    n.rimWidth   = 1.0;
    n.labelColor = theme::c(kFg4);
    n.glyph      = QChar('.');
    n.fill       = theme::c(kSurface);
    n.rim        = theme::c(kFg3);
    n.dashed     = false;
}

void paintTasks(Node& n)
{
    n.shape      = NodeShape::Box;
    n.rimWidth   = 1.0;
    n.labelColor = theme::c(kFg3);
    n.glyph      = QChar('#');
    n.fill       = theme::c(kSurface);
    n.rim        = theme::c(kFg3);
    n.dashed     = true;
}

void paintPlan(Node& n)
{
    n.shape      = NodeShape::Box;
    n.rimWidth   = 1.0;
    n.labelColor = theme::c(kFg4);
    n.glyph      = QChar('=');
    n.fill       = theme::c(kSurface);
    n.rim        = theme::c(kFg3);
    n.dashed     = true;
}

} // namespace

// --------------------------------------------------------------------- Camera

void Camera::retarget(const QRectF& content, qreal elapsedMs)
{
    if (content.isNull())
        return;

    if (fit_.isNull()) {
        fit_          = content;
        shrinkHeldMs_ = 0;
        return;
    }

    // Something left the frame. Growing is not negotiable and not delayed.
    if (!fit_.contains(content)) {
        fit_          = fit_.united(content);
        shrinkHeldMs_ = 0;
        return;
    }

    // Inside the held fit. Adopt it once the difference is worth a move and has
    // held, measured on size AND on position.
    //
    // Position is the half that was missing, and its absence was not a slow
    // recentre but no recentre at all. The test used to be whether the fit
    // contained the content grown by the slack on all four sides, which asks
    // the content to have shrunk away from every edge at once. Agents do not
    // leave evenly: when the ones on one side go, the survivors' box is smaller
    // but sits against the opposite edge, the grown box crosses it, and the
    // early return fires every frame while zeroing the hold. The camera then
    // held a box the graph had vacated for as long as the tab stayed open.
    const qreal sx = fit_.width() * kFitShrinkSlack;
    const qreal sy = fit_.height() * kFitShrinkSlack;

    const bool smaller = content.width() < fit_.width() - 2.0 * sx
                      || content.height() < fit_.height() - 2.0 * sy;

    const QPointF drift = content.center() - fit_.center();
    const bool    moved = std::abs(drift.x()) > sx || std::abs(drift.y()) > sy;

    if (!smaller && !moved) {
        shrinkHeldMs_ = 0;
        return;
    }

    shrinkHeldMs_ += elapsedMs;
    if (shrinkHeldMs_ >= kFitShrinkHoldMs) {
        fit_          = content;
        shrinkHeldMs_ = 0;
    }
}

qreal Camera::targetScale(const QSizeF& view) const
{
    if (fit_.isNull())
        return scale_;

    const qreal availW = std::max(40.0, view.width() - 2.0 * kMarginX);
    const qreal availH = std::max(40.0, view.height() - 2.0 * kMarginY - kLabelAllowance);
    const qreal boxW   = std::max(1.0, fit_.width());
    const qreal boxH   = std::max(1.0, fit_.height());

    return std::clamp(std::min(availW / boxW, availH / boxH), kMinScale, kMaxScale);
}

void Camera::advance(const QSizeF& view, qreal seconds)
{
    if (fit_.isNull()) {
        atRest_ = true;
        return;
    }

    const qreal target    = targetScale(view);
    const qreal logTarget = std::log(target);

    // Rate corrected for the frame length rather than a fixed fraction per
    // frame, which would make the camera visibly faster on a 144Hz monitor.
    //
    // Scale is smoothed in log space: the same additive step is a bigger
    // multiplicative step at small scale, so a linear chase makes zooming out
    // feel faster than zooming in and the camera lurches out and creeps back.
    const qreal a = 1.0 - std::exp(-kCameraRate * seconds);
    logScale_ += (logTarget - logScale_) * a;
    centre_ += (fit_.center() - centre_) * a;
    scale_ = std::exp(logScale_);

    const QPointF off = fit_.center() - centre_;
    atRest_ = std::abs(logTarget - logScale_) < kCameraSnapScale
           && length(off) < kCameraSnapCentre;
    if (atRest_) {
        logScale_ = logTarget;
        scale_    = target;
        centre_   = fit_.center();
    }
}

void Camera::frame(const QRectF& content, const QSizeF& view)
{
    if (content.isNull())
        return;
    fit_          = content;
    shrinkHeldMs_ = 0;
    snap(view);
}

void Camera::snap(const QSizeF& view)
{
    if (fit_.isNull())
        return;
    scale_    = targetScale(view);
    logScale_ = std::log(scale_);
    centre_   = fit_.center();
    atRest_   = true;
}

QPointF Camera::toScreen(const QPointF& world, const QSizeF& view) const
{
    return QPointF((world.x() - centre_.x()) * scale_ + view.width() * 0.5,
                   (world.y() - centre_.y()) * scale_ + view.height() * 0.5);
}

// ------------------------------------------------------------------ NodeGraph

std::size_t NodeGraph::touch(const std::string& key, NodeKind kind,
                             const std::string& parentKey, bool animate, bool* born)
{
    if (const auto it = index_.find(key); it != index_.end()) {
        Node& n = nodes_[it->second];
        n.seen  = stamp_;

        // It went away and came back. The three sources are files other
        // processes write, so one refresh that momentarily loses a card must
        // not kill a node and build a new one in its place.
        if (n.phase == NodePhase::Dying) {
            n.phase  = NodePhase::Born;
            n.animMs = kBirthMs * kResurrectFraction;
            if (born)
                *born = true;
        }
        return it->second;
    }

    Node n;
    n.kind         = kind;
    n.key          = key;
    n.parentKey    = parentKey;
    n.seen         = stamp_;
    n.mass         = massFor(kind);
    n.radiusTarget = radiusFor(kind);
    n.restRunTarget   = kRestSessionRun;
    n.restAgentTarget = (kind == NodeKind::Session) ? kRestSessionAgent : kRestRunAgent;
    n.restWorkTarget  = kRestSessionWork;
    n.restRun         = n.restRunTarget;
    n.restAgent       = n.restAgentTarget;
    n.restWork        = n.restWorkTarget;

    // On a ray out of its parent, so the pop reads as emerging from it.
    if (const auto p = index_.find(parentKey); !parentKey.empty() && p != index_.end()) {
        Node&       parent = nodes_[p->second];
        const qreal angle  = kGoldenAngle * static_cast<qreal>(parent.spawnOrdinal++);
        const qreal rest   = (kind == NodeKind::Run) ? parent.restRun : parent.restAgent;
        const qreal off    = kBirthOffsetFraction * std::max(rest, kRestRunAgent);
        const QPointF dir(std::cos(angle), std::sin(angle));

        n.pos = parent.pos + dir * off;
        n.vel = dir * kBirthSpeed;
    } else {
        QPointF     sum;
        int         live  = 0;
        for (const Node& s : nodes_) {
            if (s.kind != NodeKind::Session)
                continue;
            sum += s.pos;
            ++live;
        }
        const QPointF centre = live > 0 ? sum / static_cast<qreal>(live) : QPointF();
        const qreal   angle  = kGoldenAngle * static_cast<qreal>(sessionOrdinal_++);
        n.pos = centre + QPointF(std::cos(angle), std::sin(angle)) * kSessionBirthRadius;
    }

    if (!animate) {
        n.phase     = NodePhase::Alive;
        n.popScale  = 1.0;
        n.alpha     = 1.0;
        n.influence = 1.0;
        n.radius    = n.radiusTarget;
    }

    index_.emplace(key, nodes_.size());
    nodes_.push_back(std::move(n));
    if (born)
        *born = true;
    return nodes_.size() - 1;
}

bool NodeGraph::setBoard(const AgentList& board, bool animate)
{
    ++stamp_;
    bool changed = false;

    // One clock for the whole fold. A monitor's node appears or not depending
    // on whether its watch has run out, and two sessions judged a millisecond
    // apart would be a refresh that disagrees with itself.
    const std::int64_t now = nowUnixMs();

    for (const AgentCard& card : board) {
        // A session whose process is gone is not on this canvas. The graph is
        // what is happening, and a machine's worth of finished sessions would
        // crowd out the two that are running.
        if (card.column == AgentColumn::Done)
            continue;

        const std::string sKey = sessionKey(card.sessionId);

        bool              born = false;
        const std::size_t si   = touch(sKey, NodeKind::Session, {}, animate, &born);
        changed |= born;
        {
            Node&         s     = nodes_[si];
            const QString label = QString::fromStdString(card.name);
            changed |= s.label != label || s.attention != (card.column == AgentColumn::NeedsYou);
            s.label = label;
            paintSession(s, card.column);
        }

        // Which runs are on the canvas, so an agent knows whether the run it
        // names is there for it to hang off.
        std::unordered_map<std::string, std::size_t> runs;
        int                                          liveRuns = 0;

        for (const WorkflowRun& w : card.workflows) {
            if (w.finished())
                continue;

            born                 = false;
            const std::size_t ri = touch(runKey(card.sessionId, w.runId), NodeKind::Run,
                                         sKey, animate, &born);
            changed |= born;

            Node& r = nodes_[ri];
            QString label = QString::fromStdString(w.name);
            if (!w.phase.empty())
                label += QStringLiteral("  ") + QString::fromStdString(w.phase);

            const qreal grown = std::min(kRunRadiusMax,
                                         kRunRadius + kRunGrowth
                                             * std::log2(1.0 + static_cast<qreal>(w.spawned)));
            const qreal progress = w.spawned > 0
                                     ? static_cast<qreal>(w.done) / static_cast<qreal>(w.spawned)
                                     : -1.0;

            changed |= r.label != label || r.progress != progress
                    || r.progressBad != (w.failed > 0) || r.radiusTarget != grown;

            r.label        = label;
            r.progress     = progress;
            r.progressBad  = w.failed > 0;
            r.radiusTarget = grown;
            r.fill         = theme::c(kSurface);
            r.rim          = theme::c(kBorderHi);
            r.rimWidth     = 1.5;
            r.labelColor   = theme::c(kFg3);

            runs.emplace(w.runId, ri);
            ++liveRuns;
        }

        int                                  loose = 0;
        std::unordered_map<std::string, int> runChildren;
        std::vector<std::size_t>             agentSlots;

        // Where each live subagent landed, so a shell it opened can hang off it
        // rather than off the session.
        std::unordered_map<std::string, std::size_t> agentNodes;

        for (const SubAgent& a : card.agents) {
            if (a.state == AgentState::Finished)
                continue;

            // A run that has ended is proof its agents have, so an agent naming
            // one that is not on the canvas is a source disagreeing with
            // itself. It hangs off the session rather than off nothing.
            const auto        at     = runs.find(a.workflowRun);
            const bool        inRun  = at != runs.end();
            const std::string parent = inRun ? nodes_[at->second].key : sKey;

            born                 = false;
            const std::size_t ai = touch(agentKey(card.sessionId, a.id), NodeKind::Agent,
                                         parent, animate, &born);
            changed |= born;

            Node&         n     = nodes_[ai];
            const QString label = QString::fromStdString(agentLabel(a));
            const QColor  rim   = n.rim;
            changed |= n.label != label;
            n.label = label;
            paintAgent(n, a);
            changed |= n.rim != rim;

            agentSlots.push_back(ai);
            agentNodes.emplace(a.id, ai);
            if (inRun)
                ++runChildren[a.workflowRun];
            else
                ++loose;
        }

        const bool crowded = agentSlots.size() > static_cast<std::size_t>(kLabelCrowdLimit);
        for (const std::size_t slot : agentSlots)
            nodes_[slot].labelHidden = crowded;

        // ---------------------------------------------- what the session is doing

        // One helper for all five box kinds, because the only thing that
        // differs between them is which paint function runs.
        int work = 0;

        const auto boxUnder = [&](const std::string& parent, const std::string& key,
                                  NodeKind kind, const QString& label, const QString& detail,
                                  auto&& paint) {
            bool              fresh = false;
            const std::size_t bi    = touch(key, kind, parent, animate, &fresh);
            changed |= fresh;

            Node&        n   = nodes_[bi];
            const QColor rim = n.rim;
            changed |= n.label != label || n.detail != detail;
            n.label  = label;
            n.detail = detail;
            paint(n);
            changed |= n.rim != rim;
            return bi;
        };

        // The common case: a box hanging off the session, counted into the ring
        // the session hands its own boxes.
        const auto box = [&](const std::string& key, NodeKind kind, const QString& label,
                             const QString& detail, auto&& paint) {
            const std::size_t bi = boxUnder(sKey, key, kind, label, detail, paint);
            ++work;
            return bi;
        };

        // Shells and monitors. Only tasks the log could name are drawn: an
        // unnamed one is the foreground tool call this session is making right
        // now, which the Tool node below already stands for, and drawing both
        // would show one command twice. See taskCalls in agent_board.cpp for
        // why a named task is exactly a backgrounded one.
        for (const BackgroundTask& t : card.tasks) {
            if (t.kind == TaskKind::Unknown || !t.live(now))
                continue;

            const QString label = t.label.empty()
                                    ? QString::fromLatin1(taskKindLabel(t.kind))
                                    : QString::fromStdString(t.label);

            // A subagent's shell belongs to the subagent. Every task in a
            // session shares one directory whoever opened it, so the only thing
            // that knows this is the agent_id on the hook event, carried here
            // by the join.
            //
            // An agent that has finished is not on the canvas, and neither is
            // one whose meta file has not been read yet. Its shell falls back
            // to the session rather than to nothing, the same way an agent
            // naming a finished run does.
            std::string owner = sKey;
            bool        nested = false;
            if (!t.agentId.empty()) {
                if (const auto at = agentNodes.find(t.agentId); at != agentNodes.end()) {
                    owner  = nodes_[at->second].key;
                    nested = true;
                }
            }

            const std::size_t ti =
                t.kind == TaskKind::Monitor
                    ? boxUnder(owner, taskKey(card.sessionId, t.id), NodeKind::Monitor, label,
                               QString::fromStdString(t.tail), [](Node& n) { paintMonitor(n); })
                    : boxUnder(owner, taskKey(card.sessionId, t.id), NodeKind::Shell, label,
                               QString::fromStdString(t.tail),
                               [&t](Node& n) { paintShell(n, t); });

            // Only the session's own boxes share the session's ring. One under
            // an agent sits close to that agent instead, and must not widen a
            // ring it is not in.
            if (nested)
                nodes_[ti].restWorkTarget = kRestAgentWork;
            else
                ++work;
        }

        // Every tool call the session and its subagents made recently, one box
        // each, keyed by tool_use_id so a call keeps its node across refreshes.
        //
        // Each lingers kToolLingerMs from when it started and then goes. That is
        // a display choice standing in for a fact nobody writes down: nothing
        // records when a tool call ends, so there is no moment to retire one on.
        // Without the linger a call would appear and vanish inside a single
        // refresh, which is what a box popping into existence and out again in
        // the same frame looks like, and the eye gets nothing from it.
        //
        // A call by a subagent hangs off that subagent, the same as its shells.
        for (const ToolCall& call : card.tools) {
            if (call.tsMs <= 0 || now - call.tsMs > kToolLingerMs)
                continue;

            std::string owner  = sKey;
            bool        nested = false;
            if (!call.agentId.empty()) {
                if (const auto at = agentNodes.find(call.agentId); at != agentNodes.end()) {
                    owner  = nodes_[at->second].key;
                    nested = true;
                }
            }

            const std::size_t ci =
                boxUnder(owner, toolKey(card.sessionId, call.id), NodeKind::Tool,
                         QString::fromStdString(call.tool),
                         QString::fromStdString(call.detail),
                         [](Node& n) { paintTool(n); });

            if (nested)
                nodes_[ci].restWorkTarget = kRestAgentWork;
            else
                ++work;
        }

        // The task list, as one block hanging off the session.
        //
        // Ordered so the chain reads top to bottom: an item goes straight after
        // the item blocking it, indented under it. That is a depth-first walk of
        // the blocking relation, which is what turns blockedBy into something
        // readable without drawing a single edge.
        //
        // Only backwards, and only to one blocker. Blocking is a graph and this
        // is a walk: an item can be blocked by several, and nothing promises the
        // relation is acyclic, so taking the earliest blocker by list position
        // means every edge points at a lower index and a cycle cannot form.
        if (!card.todos.empty()) {
            std::unordered_map<std::string, std::size_t> order;
            for (std::size_t i = 0; i < card.todos.size(); ++i)
                order.emplace(card.todos[i].id, i);

            const std::size_t                     count = card.todos.size();
            std::vector<std::vector<std::size_t>> blocked(count);
            std::vector<std::size_t>              roots;

            for (std::size_t i = 0; i < count; ++i) {
                std::size_t best = i;
                for (const std::string& id : card.todos[i].blockedBy) {
                    const auto at = order.find(id);
                    if (at != order.end() && at->second < best)
                        best = at->second;
                }
                if (best == i)
                    roots.push_back(i);
                else
                    blocked[best].push_back(i);
            }

            std::vector<TaskLine> lines;
            lines.reserve(count);
            int done = 0;

            // Iterative rather than recursive: the nesting is bounded only by
            // the list length, and the list is written by something outside
            // this program.
            std::vector<std::pair<std::size_t, int>> stack;
            for (auto it = roots.rbegin(); it != roots.rend(); ++it)
                stack.emplace_back(*it, 0);

            while (!stack.empty()) {
                const std::size_t i     = stack.back().first;
                const int         depth = stack.back().second;
                stack.pop_back();

                const TodoItem& item = card.todos[i];

                TaskLine line;
                line.done   = item.done();
                line.active = item.active();
                line.mark   = line.done ? QChar('x') : (line.active ? QChar('>') : QChar(' '));
                line.depth  = std::min(depth, kMaxTaskIndent);
                line.text   = QString::fromStdString(
                    line.active && !item.activeForm.empty() ? item.activeForm : item.subject);
                if (line.done)
                    ++done;
                lines.push_back(std::move(line));

                for (auto it = blocked[i].rbegin(); it != blocked[i].rend(); ++it)
                    stack.emplace_back(*it, depth + 1);
            }

            const int total = static_cast<int>(lines.size());
            trimTaskLines(&lines);

            const std::size_t ti = box(tasksKey(card.sessionId), NodeKind::Tasks, QString(),
                                       QString(), [](Node& b) { paintTasks(b); });

            Node& t = nodes_[ti];
            if (!sameTaskLines(t.taskLines, lines) || t.taskDone != done || t.taskTotal != total)
                changed = true;

            // The room the block needs, which is nothing like the size of the
            // node that anchors it. Rows only: the block is far wider than it is
            // tall, but widening the footprint to match would shove the whole
            // cluster sideways, and a circle big enough to cover the text is
            // worse than one big enough to keep a neighbour out of it.
            const qreal rows = static_cast<qreal>(lines.size()) + (total > 9 ? 1.0 : 0.0);
            t.spaceRadius    = kTasksRadius + kTasksRowSpace * rows;

            t.taskLines = std::move(lines);
            t.taskTotal = total;
            t.taskDone  = done;
        }

        for (const PlanDoc& plan : card.plans) {
            box(planKey(card.sessionId), NodeKind::Plan, QString::fromStdString(plan.title),
                QString(), [](Node& n) { paintPlan(n); });
        }

        Node& s            = nodes_[si];
        s.restRunTarget    = restFor(kRestSessionRun, kRunSpacing, liveRuns);
        s.restAgentTarget  = restFor(kRestSessionAgent, kAgentSpacing, loose);
        s.restWorkTarget   = restFor(kRestSessionWork, kWorkSpacing, work);

        for (const auto& [runId, slot] : runs) {
            nodes_[slot].restAgentTarget =
                restFor(kRestRunAgent, kAgentSpacing, runChildren[runId]);
        }
    }

    for (Node& n : nodes_) {
        if (n.seen == stamp_ || n.phase == NodePhase::Dying)
            continue;
        n.phase  = NodePhase::Dying;
        n.animMs = animate ? 0.0 : kDeathMs;
        changed  = true;
    }

    if (!animate) {
        sweepDead();
        for (Node& n : nodes_) {
            n.radius    = n.radiusTarget;
            n.restRun   = n.restRunTarget;
            n.restAgent = n.restAgentTarget;
            n.restWork  = n.restWorkTarget;
        }
    }

    resolveParents();
    return changed;
}

void NodeGraph::resolveParents()
{
    for (Node& n : nodes_) {
        n.parentSlot = -1;
        if (n.parentKey.empty())
            continue;
        if (const auto it = index_.find(n.parentKey); it != index_.end())
            n.parentSlot = static_cast<int>(it->second);
    }
}

void NodeGraph::step(qreal dt)
{
    const std::size_t count = nodes_.size();
    if (count == 0)
        return;

    for (Node& n : nodes_)
        n.force = QPointF();

    // Every pair. Two magnitude compares reject almost all of them before the
    // multiply, so the real cost is the neighbours inside the cutoff. Cheaper
    // schemes exist above a couple of thousand nodes and would cost the one
    // case that matters here: where two clusters touch, the nodes on the seam
    // have to see each other, and only pairwise repulsion can do that.
    for (std::size_t i = 0; i + 1 < count; ++i) {
        Node& a = nodes_[i];
        for (std::size_t j = i + 1; j < count; ++j) {
            Node& b = nodes_[j];

            qreal dx = b.pos.x() - a.pos.x();
            if (dx > kRepelCutoff || dx < -kRepelCutoff)
                continue;
            qreal dy = b.pos.y() - a.pos.y();
            if (dy > kRepelCutoff || dy < -kRepelCutoff)
                continue;

            qreal d2 = dx * dx + dy * dy;
            if (d2 > kRepelCutoff * kRepelCutoff)
                continue;

            if (d2 < kCoincident2) {
                const qreal ang = kGoldenAngle * static_cast<qreal>(i * 2 + j);
                dx = std::cos(ang);
                dy = std::sin(ang);
                d2 = 1.0;
            }

            // Charge is the target radius, not the drawn one: the birth already
            // ramps a newborn in through influence, and scaling by the animated
            // radius as well would double-count it into a nudge.
            //
            // And the room a node needs rather than the size it is drawn, which
            // are the same for everything but a task block. See
            // Node::spaceRadius: a block is nine rows of text hanging off a node
            // the size of a full stop, and charging by the dot is what let a
            // neighbour settle underneath the text.
            const qreal ra   = a.spaceRadius > 0 ? a.spaceRadius : a.radiusTarget;
            const qreal rb   = b.spaceRadius > 0 ? b.spaceRadius : b.radiusTarget;
            const qreal soft = std::max(d2, kRepelSoftening * kRepelSoftening);
            const qreal mag  = kRepelScale * ra * rb
                            * (1.0 / soft - 1.0 / (kRepelCutoff * kRepelCutoff));
            if (mag <= 0.0)
                continue;

            const qreal   k = mag * a.influence * b.influence / std::sqrt(d2);
            const QPointF f(dx * k, dy * k);
            a.force -= f;
            b.force += f;
        }
    }

    // How far each session's subtree reaches, which is what the cluster push
    // works on.
    for (Node& n : nodes_) {
        if (n.kind == NodeKind::Session)
            n.clusterRadius = n.radius;
    }
    for (const Node& c : nodes_) {
        if (c.kind == NodeKind::Session)
            continue;
        int slot  = c.parentSlot;
        int guard = 0;
        while (slot >= 0 && nodes_[static_cast<std::size_t>(slot)].kind != NodeKind::Session
               && guard++ < 4) {
            slot = nodes_[static_cast<std::size_t>(slot)].parentSlot;
        }
        if (slot < 0)
            continue;
        Node& s = nodes_[static_cast<std::size_t>(slot)];
        if (s.kind != NodeKind::Session)
            continue;
        s.clusterRadius = std::max(s.clusterRadius, length(c.pos - s.pos) + c.radius);
    }

    for (std::size_t i = 0; i + 1 < count; ++i) {
        Node& a = nodes_[i];
        if (a.kind != NodeKind::Session)
            continue;
        for (std::size_t j = i + 1; j < count; ++j) {
            Node& b = nodes_[j];
            if (b.kind != NodeKind::Session)
                continue;

            const QPointF off = b.pos - a.pos;
            const qreal   d   = std::max(length(off), 1.0);
            const qreal   gap = a.clusterRadius + b.clusterRadius + kClusterGap - d;
            if (gap <= 0.0)
                continue;

            const qreal   k = kClusterPush * gap * a.influence * b.influence / d;
            const QPointF f(off.x() * k, off.y() * k);
            a.force -= f;
            b.force += f;
        }
    }

    // Springs, equal and opposite. Nothing pins a parent: mass is what makes
    // the hierarchy, and breaking Newton's third law here would let the whole
    // graph translate itself off the canvas.
    for (std::size_t i = 0; i < count; ++i) {
        Node& child = nodes_[i];
        if (child.parentSlot < 0)
            continue;

        const std::size_t ps = static_cast<std::size_t>(child.parentSlot);
        if (ps == i)
            continue;
        Node& parent = nodes_[ps];

        qreal rest = child.kind == NodeKind::Run ? parent.restRun
                   : isWork(child.kind)         ? parent.restWork
                                                : parent.restAgent;
        if (child.phase == NodePhase::Dying) {
            const qreal t = std::clamp(child.animMs / kDeathMs, 0.0, 1.0);
            rest *= 1.0 + (kDeathRestFraction - 1.0) * t;
        }

        const QPointF off = parent.pos - child.pos;
        const qreal   d   = std::max(length(off), 0.01);
        const qreal   mag = std::clamp(kSpringStiffness * (d - rest),
                                       -kMaxSpringForce, kMaxSpringForce);

        // The spring is not faded by the child's influence. A dying node is
        // still tied to its parent while it retracts; what it stops doing is
        // shoving its neighbours.
        const qreal   k = mag / d;
        const QPointF f(off.x() * k, off.y() * k);
        child.force += f;
        parent.force -= f;
    }

    for (Node& n : nodes_) {
        if (n.kind != NodeKind::Session)
            continue;
        const qreal d = length(n.pos);
        if (d <= kFenceRadius || d < 0.01)
            continue;
        const qreal k = kFenceForce * std::min(1.0, (d - kFenceRadius) / kFenceRamp) / d;
        n.force -= QPointF(n.pos.x() * k, n.pos.y() * k);
    }

    // Semi-implicit Euler: velocity first, then position from the new velocity.
    // Forward Euler adds energy on a spring and the fix costs nothing.
    for (Node& n : nodes_) {
        n.vel += n.force * (dt / n.mass);
        n.vel *= kDampPerStep;

        const qreal speed = length(n.vel);
        if (speed > kMaxSpeed)
            n.vel *= kMaxSpeed / speed;

        n.pos += n.vel * dt;
    }
}

void NodeGraph::advance(qreal seconds)
{
    const qreal ms = seconds * 1000.0;
    const qreal a  = 1.0 - std::exp(-kSmoothRate * seconds);

    for (Node& n : nodes_) {
        n.radius += (n.radiusTarget - n.radius) * a;
        n.restRun += (n.restRunTarget - n.restRun) * a;
        n.restAgent += (n.restAgentTarget - n.restAgent) * a;
        n.restWork += (n.restWorkTarget - n.restWork) * a;

        n.animMs += ms;

        switch (n.phase) {
        case NodePhase::Born: {
            // Negative holds a node at nothing until its turn, which is how the
            // first reveal is staggered.
            if (n.animMs < 0.0) {
                n.popScale  = 0.0;
                n.alpha     = 0.0;
                n.influence = 0.0;
                break;
            }
            const qreal t = std::clamp(n.animMs / kBirthMs, 0.0, 1.0);
            n.popScale    = easeOutBack(t);
            n.alpha       = easeOutCubic(std::min(1.0, t / kBirthAlphaFraction));
            n.influence   = smoothstep(std::min(1.0, t / kBirthInfluenceFraction));
            if (t >= 1.0) {
                n.phase     = NodePhase::Alive;
                n.popScale  = 1.0;
                n.alpha     = 1.0;
                n.influence = 1.0;
            }
            break;
        }
        case NodePhase::Alive:
            n.popScale  = 1.0;
            n.alpha     = 1.0;
            n.influence = 1.0;
            break;
        case NodePhase::Dying: {
            const qreal t = std::clamp(n.animMs / kDeathMs, 0.0, 1.0);
            n.popScale    = std::max(0.0, easeInBackDown(t));
            // Three quarters visible at the halfway point and then gone fast,
            // so you see that it went rather than that it was never there.
            n.alpha     = std::max(0.0, 1.0 - t * t);
            n.influence = 1.0 - smoothstep(t);
            break;
        }
        }
    }

    // Which side a label sits on, decided here rather than while painting so
    // the paint pass writes nothing. The hysteresis is what stops a label
    // flipping across its node every few frames while the layout settles.
    for (Node& n : nodes_) {
        if (n.parentSlot < 0)
            continue;
        const Node& parent = nodes_[static_cast<std::size_t>(n.parentSlot)];
        if (n.pos.x() > parent.pos.x() + kSideHysteresis)
            n.labelRight = true;
        else if (n.pos.x() < parent.pos.x() - kSideHysteresis)
            n.labelRight = false;
    }
}

void NodeGraph::settle(int steps)
{
    for (Node& n : nodes_) {
        n.phase     = NodePhase::Alive;
        n.animMs    = 0;
        n.popScale  = 1.0;
        n.alpha     = 1.0;
        n.influence = 1.0;
        n.radius    = n.radiusTarget;
        n.restRun   = n.restRunTarget;
        n.restAgent = n.restAgentTarget;
        n.restWork  = n.restWorkTarget;
    }
    for (int i = 0; i < steps; ++i)
        step(kFixedStep);
    rest();
}

void NodeGraph::stagger(qreal perNodeMs, qreal capMs)
{
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        Node& n     = nodes_[i];
        n.phase     = NodePhase::Born;
        n.animMs    = -std::min(perNodeMs * static_cast<qreal>(i), capMs);
        n.popScale  = 0.0;
        n.alpha     = 0.0;
        n.influence = 0.0;
    }
}

void NodeGraph::rest()
{
    for (Node& n : nodes_)
        n.vel = QPointF();
}

void NodeGraph::sweepDead()
{
    const std::size_t was = nodes_.size();
    std::erase_if(nodes_, [](const Node& n) {
        return n.phase == NodePhase::Dying && n.animMs >= kDeathMs;
    });
    if (nodes_.size() == was)
        return;

    index_.clear();
    for (std::size_t i = 0; i < nodes_.size(); ++i)
        index_.emplace(nodes_[i].key, i);
    resolveParents();
}

bool NodeGraph::settled() const
{
    for (const Node& n : nodes_) {
        if (n.phase != NodePhase::Alive)
            return false;
        if (length(n.vel) > kSleepSpeed)
            return false;
    }
    return true;
}

QRectF NodeGraph::bounds() const
{
    QRectF box;
    for (const Node& n : nodes_) {
        // A node on its way out stops claiming space the moment it starts
        // dying, and every other node claims the size it is going to be rather
        // than the size it is this frame. Both are the same guard: the fit
        // drives the zoom all the way to the margins now, so a radius sliding
        // toward zero would drive the scale toward the clamp and the camera
        // would lunge at whatever was leaving.
        if (n.phase == NodePhase::Dying)
            continue;
        const qreal r = n.spaceRadius > 0 ? n.spaceRadius : n.radiusTarget;
        if (r <= 0.01)
            continue;
        box = box.united(QRectF(n.pos.x() - r, n.pos.y() - r, 2.0 * r, 2.0 * r));
    }
    return box;
}

int NodeGraph::count(NodeKind kind) const
{
    int n = 0;
    for (const Node& node : nodes_) {
        if (node.kind == kind && node.phase != NodePhase::Dying)
            ++n;
    }
    return n;
}

} // namespace pm::gui
