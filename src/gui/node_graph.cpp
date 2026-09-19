#include "node_graph.h"

#include "theme_qt.h"

#include <algorithm>
#include <cmath>
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

// n children of one parent pack into a disc of radius spacing*sqrt(n/pi).
// Feeding that back in as the rest length is what keeps a forty-agent run in
// the regime the spring was tuned for: without it the spring is stretched by
// its own crowd and the layout becomes a tug of war between saturated forces.
// 24 is two agent discs with a gap between them.
constexpr qreal kAgentSpacing = 24.0;
constexpr qreal kRunSpacing   = 60.0;

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

// Grow at once, shrink only after the smaller box has held. The slack is a
// fraction of the held fit rather than a distance in world units: the zoom now
// runs from one session filling the canvas to a hundred agents in it, and a
// fixed slack is either a third of the frame or invisible depending on which.
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

std::string sessionKey(const std::string& session)
{
    return std::string("S") + kSep + session;
}

std::string runKey(const std::string& session, const std::string& run)
{
    return std::string("R") + kSep + session + kSep + run;
}

std::string agentKey(const std::string& session, const std::string& agent)
{
    return std::string("A") + kSep + session + kSep + agent;
}

qreal massFor(NodeKind kind)
{
    switch (kind) {
    case NodeKind::Session: return kSessionMass;
    case NodeKind::Run:     return kRunMass;
    case NodeKind::Agent:   return kAgentMass;
    }
    return kAgentMass;
}

qreal radiusFor(NodeKind kind)
{
    switch (kind) {
    case NodeKind::Session: return kSessionRadius;
    case NodeKind::Run:     return kRunRadius;
    case NodeKind::Agent:   return kAgentRadius;
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

    const qreal  sx    = fit_.width() * kFitShrinkSlack;
    const qreal  sy    = fit_.height() * kFitShrinkSlack;
    const QRectF slack = content.adjusted(-sx, -sy, sx, sy);
    if (!fit_.contains(slack)) {
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
    n.restRun         = n.restRunTarget;
    n.restAgent       = n.restAgentTarget;

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
            if (inRun)
                ++runChildren[a.workflowRun];
            else
                ++loose;
        }

        const bool crowded = agentSlots.size() > static_cast<std::size_t>(kLabelCrowdLimit);
        for (const std::size_t slot : agentSlots)
            nodes_[slot].labelHidden = crowded;

        Node& s           = nodes_[si];
        s.restRunTarget   = restFor(kRestSessionRun, kRunSpacing, liveRuns);
        s.restAgentTarget = restFor(kRestSessionAgent, kAgentSpacing, loose);

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
            const qreal soft = std::max(d2, kRepelSoftening * kRepelSoftening);
            const qreal mag  = kRepelScale * a.radiusTarget * b.radiusTarget
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

        qreal rest = (child.kind == NodeKind::Run) ? parent.restRun : parent.restAgent;
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
        const qreal r = n.radiusTarget;
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
