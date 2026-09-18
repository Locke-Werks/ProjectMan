#include "node_explorer_panel.h"

#include "theme_qt.h"

#include <QFontMetricsF>
#include <QHideEvent>
#include <QLineF>
#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace pm::gui {
namespace {

using namespace pm::theme;

// About 62 frames a second, which is faster than anything on this canvas moves.
// A zero interval would run as fast as the event loop allows and burn a core
// drawing the same frame twice.
constexpr int kTimerMs = 16;

// A frame is stepped in whole substeps of kFixedStep. Eight absorbs a 66ms
// hitch and then drops the rest rather than spiralling, and a frame longer than
// kMaxCatchUpMs is a stall: catching up on it in one go would teleport the
// graph, so the missing time is simply lost.
constexpr int   kMaxSubSteps   = 8;
constexpr qreal kMaxCatchUpMs  = 100.0;

// Settled for this many consecutive frames and the timer stops. About 0.4s,
// which is long enough that a graph pausing between two arrivals does not stop
// and start the timer twice a second.
constexpr int kSleepFrames = 24;

// Two seconds of simulation, run in one go when a tab is opened on work that
// arrived while it was hidden.
constexpr int kWarmSteps = 240;

// The one time the whole graph pops in at once, on the first open.
constexpr qreal kRevealStaggerMs    = 25.0;
constexpr qreal kRevealStaggerCapMs = 400.0;

// Logical pixels, all of them, because none of this scales with the camera.
constexpr qreal kLabelGap         = 7.0;    // between a rim and its text
constexpr qreal kLabelPad         = 8.0;    // kept clear of the viewport edge
constexpr qreal kLabelMaxPx       = 150.0;  // no label is worth more canvas
// Except a session's, which is the one name on the canvas worth reading whole:
// derived names run to "revenant-m0-foundations" and cutting one to
// "revenant-m0-foun" loses the half that says which session it is.
constexpr qreal kSessionLabelMaxPx = 240.0;
constexpr qreal kLabelMinWidthPx  = 24.0;   // below this there is nothing to read
constexpr qreal kLabelMinRadiusPx = 5.0;    // below this the node is a dot

// The two rings on a session that wants something. Hairlines rather than a
// glow, and they do not pulse: an animation that never ends is a graph that can
// never satisfy the sleep rule, so the tab would burn a core to breathe.
constexpr qreal kHaloInner      = 3.0;
constexpr qreal kHaloOuter      = 6.0;
constexpr int   kHaloInnerAlpha = 70;
constexpr int   kHaloOuterAlpha = 30;

// A link is structure when it hangs off a session and detail when it hangs off
// a run, which is what lets a hundred leaf links read as one spray rather than
// as mush. Both tiers are drawn in a foreground grey rather than in a border
// grey: a hairline at kBorder on a black canvas is very nearly nothing, and
// what the links carry here is the whole point of the view.
constexpr int   kLinkAlphaMajor = 215;
constexpr int   kLinkAlphaMinor = 150;
constexpr qreal kLinkWidthMajor = 1.6;
constexpr qreal kLinkWidthMinor = 1.1;

// Thick enough to read against the rim it sits on.
constexpr qreal kProgressWidth = 2.5;

// A node smaller than this is not worth the two draw calls.
constexpr qreal kMinDrawRadius = 0.4;

QColor withAlpha(QColor c, qreal a)
{
    // setAlpha(int) rather than setAlphaF: the F-suffixed QColor setters take a
    // float in Qt 6 and /WX turns the qreal into an error.
    c.setAlpha(std::clamp(qRound(static_cast<qreal>(c.alpha()) * a), 0, 255));
    return c;
}

} // namespace

NodeExplorerPanel::NodeExplorerPanel(QWidget* parent) : QWidget(parent)
{
    // The stylesheet paints QWidget backgrounds, but a widget with its own
    // paintEvent draws none of it, so the canvas fills its own black.
    setAttribute(Qt::WA_OpaquePaintEvent);

    // showTab hands the page the focus so bare keystrokes stop reaching the
    // project filter. Nothing here reads a key; this is only so they land
    // somewhere harmless.
    setFocusPolicy(Qt::StrongFocus);

    tick_ = new QTimer(this);
    tick_->setInterval(kTimerMs);
    // Coarse on purpose. A precise timer raises the system timer resolution for
    // the whole machine, and the accumulator absorbs the jitter anyway.
    tick_->setTimerType(Qt::CoarseTimer);
    connect(tick_, &QTimer::timeout, this, &NodeExplorerPanel::onTick);
}

void NodeExplorerPanel::setBoard(const AgentList& board)
{
    // Births and deaths animate only when someone is looking. A pop is an
    // event, and an event nobody saw must not be replayed on arrival.
    const bool watching = isVisible();
    const bool changed  = graph_.setBoard(board, watching);

    seeded_ = true;
    publishCounts();

    if (!changed)
        return;
    if (watching)
        wake();
    else
        warmNeeded_ = true;
}

void NodeExplorerPanel::publishCounts()
{
    QString text;
    if (graph_.empty()) {
        text = QStringLiteral("no sessions");
    } else {
        const int sessions = graph_.count(NodeKind::Session);
        const int runs     = graph_.count(NodeKind::Run);
        const int agents   = graph_.count(NodeKind::Agent);

        // What the three node sizes are, rather than a second copy of the
        // board's own summary. Terms drop out at zero the way the board's do.
        text = QStringLiteral("%1 session%2").arg(sessions).arg(sessions == 1 ? "" : "s");
        if (runs > 0)
            text += QStringLiteral("  %1 run%2").arg(runs).arg(runs == 1 ? "" : "s");
        if (agents > 0)
            text += QStringLiteral("  %1 agent%2").arg(agents).arg(agents == 1 ? "" : "s");
    }

    if (text == counts_)
        return;
    counts_ = text;
    emit countsChanged(counts_);
}

void NodeExplorerPanel::wake()
{
    quiet_       = 0;
    accumulator_ = 0;
    clock_.restart();
    if (!tick_->isActive())
        tick_->start();
}

void NodeExplorerPanel::warm()
{
    graph_.settle(kWarmSteps);
    graph_.sweepDead();
    camera_.frame(graph_.bounds(), QSizeF(size()));
}

void NodeExplorerPanel::onTick()
{
    const qreal elapsedMs = std::min(static_cast<qreal>(clock_.restart()), kMaxCatchUpMs);
    const qreal elapsed   = elapsedMs / 1000.0;

    accumulator_ += elapsed;
    int steps = 0;
    while (accumulator_ >= kFixedStep && steps < kMaxSubSteps) {
        graph_.step(kFixedStep);
        accumulator_ -= kFixedStep;
        ++steps;
    }
    if (steps == kMaxSubSteps)
        accumulator_ = 0;

    // Both on wall time rather than on substeps, so a dropped substep shortens
    // the physics instead of freezing a pop in mid-air.
    graph_.advance(elapsed);
    graph_.sweepDead();

    camera_.retarget(graph_.bounds(), elapsedMs);
    camera_.advance(QSizeF(size()), elapsed);

    update();

    quiet_ = (graph_.settled() && camera_.atRest()) ? quiet_ + 1 : 0;
    if (quiet_ >= kSleepFrames) {
        graph_.rest();
        accumulator_ = 0;
        tick_->stop();
    }
}

void NodeExplorerPanel::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);

    if (firstShow_) {
        firstShow_ = false;
        warm();
        graph_.stagger(kRevealStaggerMs, kRevealStaggerCapMs);
    } else if (warmNeeded_) {
        warm();
    }

    warmNeeded_ = false;
    wake();
}

void NodeExplorerPanel::hideEvent(QHideEvent* e)
{
    // Everything stays where it is. Coming back to a graph that re-seeded every
    // time would read as a loading state rather than as a view.
    tick_->stop();
    QWidget::hideEvent(e);
}

void NodeExplorerPanel::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    // The fit is a function of the widget size, so the camera has to run again.
    // Left to the next tick rather than recomputed here, which keeps the glide.
    if (isVisible())
        wake();
}

// ------------------------------------------------------------------- painting

void NodeExplorerPanel::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), theme::c(kBlack));
    p.setRenderHint(QPainter::Antialiasing, true);

    if (graph_.empty()) {
        // Two states that must not look alike: nothing has been read yet, and
        // nothing is running.
        drawNotice(p, seeded_ ? QStringLiteral("Nothing running.")
                              : QStringLiteral("Reading sessions."));
        return;
    }

    drawLinks(p);

    // Largest first, so a small node is never hidden under a big one.
    for (const NodeKind kind : { NodeKind::Session, NodeKind::Run, NodeKind::Agent }) {
        for (const Node& n : graph_.nodes()) {
            if (n.kind == kind)
                drawNode(p, n);
        }
    }
    // Then every label, so no disc can cover text.
    for (const NodeKind kind : { NodeKind::Session, NodeKind::Run, NodeKind::Agent }) {
        for (const Node& n : graph_.nodes()) {
            if (n.kind == kind)
                drawLabel(p, n);
        }
    }
}

void NodeExplorerPanel::drawLinks(QPainter& p) const
{
    const QSizeF          view  = QSizeF(size());
    const std::vector<Node>& all = graph_.nodes();

    for (const Node& child : all) {
        if (child.parentSlot < 0)
            continue;
        const Node& parent = all[static_cast<std::size_t>(child.parentSlot)];

        const QPointF a  = camera_.toScreen(parent.pos, view);
        const QPointF b  = camera_.toScreen(child.pos, view);
        const qreal   ra = parent.radius * parent.popScale * camera_.scale();
        const qreal   rb = child.radius * child.popScale * camera_.scale();

        QLineF      line(a, b);
        const qreal len = line.length();
        // Trimmed at both rims, and dropped when the discs overlap: the stub
        // left between two touching nodes reads as a smudge.
        if (len < 1.0 || ra + rb + 2.0 >= len)
            continue;
        line.setP1(a + (b - a) * (ra / len));
        line.setP2(b + (a - b) * (rb / len));

        const bool   major = parent.kind == NodeKind::Session;
        const int    alpha = major ? kLinkAlphaMajor : kLinkAlphaMinor;
        const QColor base  = withAlpha(theme::c(major ? kFg3 : kFg4),
                                       static_cast<qreal>(alpha) / 255.0);

        p.setPen(QPen(withAlpha(base, child.alpha), major ? kLinkWidthMajor : kLinkWidthMinor));
        p.drawLine(line);
    }
}

void NodeExplorerPanel::drawNode(QPainter& p, const Node& n) const
{
    const QSizeF  view = QSizeF(size());
    const QPointF c    = camera_.toScreen(n.pos, view);
    const qreal   r    = n.radius * n.popScale * camera_.scale();
    if (r < kMinDrawRadius || n.alpha <= 0.01)
        return;

    if (n.attention) {
        const auto ring = [&](qreal out, int alpha) {
            const QColor cc = withAlpha(withAlpha(theme::c(kRed),
                                                  static_cast<qreal>(alpha) / 255.0),
                                        n.alpha);
            p.setPen(QPen(cc, 1.0));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(c, r + out, r + out);
        };
        ring(kHaloInner, kHaloInnerAlpha);
        ring(kHaloOuter, kHaloOuterAlpha);
    }

    p.setPen(QPen(withAlpha(n.rim, n.alpha), n.rimWidth));
    p.setBrush(withAlpha(n.fill, n.alpha));
    p.drawEllipse(c, r, r);

    if (n.progress < 0.0)
        return;

    // Progress from twelve o'clock, clockwise. Qt's positive direction is
    // counter-clockwise, and a progress ring that runs backwards reads as
    // broken.
    const int span = qRound(360.0 * std::clamp(n.progress, 0.0, 1.0) * 16.0);
    if (span == 0)
        return;

    const QColor arc = n.progressBad ? theme::c(kRed) : theme::c(kFg1);
    p.setPen(QPen(withAlpha(arc, n.alpha), kProgressWidth));
    p.setBrush(Qt::NoBrush);
    p.drawArc(QRectF(c.x() - r, c.y() - r, 2.0 * r, 2.0 * r), 90 * 16, -span);
}

void NodeExplorerPanel::drawLabel(QPainter& p, const Node& n) const
{
    if (n.label.isEmpty() || n.alpha <= 0.01)
        return;

    const QSizeF  view = QSizeF(size());
    const QPointF c    = camera_.toScreen(n.pos, view);
    const qreal   r    = n.radius * n.popScale * camera_.scale();

    QString text  = n.label;
    QFont   font  = theme::mono(10);
    bool    under = true;

    switch (n.kind) {
    case NodeKind::Session:
        // Upper-cased here the way TrackedLabel upper-cases everywhere else, so
        // the two tabs read as one product.
        font = theme::tracked(11, QFont::DemiBold, 0.14);
        text = text.toUpper();
        break;
    case NodeKind::Run:
        break;
    case NodeKind::Agent:
        if (n.labelHidden || r < kLabelMinRadiusPx)
            return;
        under = false;
        break;
    }

    const QFontMetricsF fm(font);
    p.setFont(font);
    p.setPen(withAlpha(n.labelColor, n.alpha));

    if (under) {
        const qreal cap = n.kind == NodeKind::Session ? kSessionLabelMaxPx : kLabelMaxPx;
        const qreal avail = std::min(cap, view.width() - 2.0 * kLabelPad);
        if (avail < kLabelMinWidthPx)
            return;
        const QString cut = fm.elidedText(text, Qt::ElideRight, avail);
        const qreal   adv = fm.horizontalAdvance(cut);
        const qreal   x   = std::clamp(c.x() - adv * 0.5, kLabelPad,
                                       std::max(kLabelPad, view.width() - kLabelPad - adv));
        p.drawText(QPointF(x, c.y() + r + kLabelGap + fm.ascent()), cut);
        return;
    }

    // Beside the node, on the side away from its parent, so labels radiate out
    // of a cluster instead of stacking down one edge of it.
    const qreal edge  = n.labelRight ? view.width() - c.x() - r - kLabelGap
                                     : c.x() - r - kLabelGap;
    const qreal avail = std::min(kLabelMaxPx, edge - kLabelPad);
    if (avail < kLabelMinWidthPx)
        return;

    // Elided from the right and not the middle, unlike the board's tool lines:
    // what tells two agents in one run apart is the label the script gave them,
    // and that is at the front.
    const QString cut = fm.elidedText(text, Qt::ElideRight, avail);
    const qreal   adv = fm.horizontalAdvance(cut);
    const qreal   x   = n.labelRight ? c.x() + r + kLabelGap : c.x() - r - kLabelGap - adv;

    p.drawText(QPointF(x, c.y() + (fm.ascent() - fm.descent()) * 0.5), cut);
}

void NodeExplorerPanel::drawNotice(QPainter& p, const QString& text) const
{
    const QFont font = theme::body(12);
    p.setFont(font);
    p.setPen(theme::c(kFg4));

    const QFontMetricsF fm(font);
    const qreal         adv = fm.horizontalAdvance(text);
    p.drawText(QPointF(static_cast<qreal>(width()) * 0.5 - adv * 0.5,
                       static_cast<qreal>(height()) * 0.5),
               text);
}

} // namespace pm::gui
