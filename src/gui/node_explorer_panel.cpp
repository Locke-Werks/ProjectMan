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

// Back to front. Two passes run over this, bodies then labels, and both want
// the same order.
constexpr NodeKind kDrawOrder[] = {
    NodeKind::Session,
    NodeKind::Run,
    NodeKind::Agent,
    NodeKind::Plan,
    NodeKind::Tasks,
    NodeKind::Tool,
    NodeKind::Monitor,
    NodeKind::Shell,
};

// The boxes. Rounded just enough to not read as a hard pixel square next to a
// canvas of circles.
constexpr qreal kBoxCorner        = 2.5;
constexpr qreal kGlyphMinRadiusPx = 6.0;

// A box's second line, under its label: what the shell is saying right now.
constexpr qreal kDetailGap   = 2.0;
constexpr int   kDetailAlpha = 150;

// The task-list block. Wider than an ordinary label because it is a column of
// sentences rather than one, and the whole point of it is being readable.
constexpr qreal kTaskBlockMaxPx = 290.0;
constexpr qreal kTaskLineGap    = 3.0;   // between rows
constexpr qreal kTaskMarkPx     = 11.0;  // column the x / > sits in
constexpr qreal kTaskIndentPx   = 9.0;   // per nesting level
constexpr int   kTaskDoneAlpha  = 105;   // finished rows step back

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
        const int shells   = graph_.count(NodeKind::Shell);
        const int monitors = graph_.count(NodeKind::Monitor);

        // What is on the canvas, rather than a second copy of the board's own
        // summary. Terms drop out at zero the way the board's do, which is what
        // keeps this readable: on an ordinary machine it says "2 sessions" and
        // only grows when there is something to grow for. Todos, plans and the
        // tool in flight are left out on purpose: they are always there when a
        // session is working and counting them says nothing.
        text = QStringLiteral("%1 session%2").arg(sessions).arg(sessions == 1 ? "" : "s");
        if (runs > 0)
            text += QStringLiteral("  %1 run%2").arg(runs).arg(runs == 1 ? "" : "s");
        if (agents > 0)
            text += QStringLiteral("  %1 agent%2").arg(agents).arg(agents == 1 ? "" : "s");
        if (shells > 0)
            text += QStringLiteral("  %1 shell%2").arg(shells).arg(shells == 1 ? "" : "s");
        if (monitors > 0)
            text += QStringLiteral("  %1 monitor%2").arg(monitors).arg(monitors == 1 ? "" : "s");
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

    // Largest first, so a small node is never hidden under a big one. The boxes
    // come last of the bodies: they are what the session is doing right now and
    // an agent disc sliding over one would hide the live thing behind the
    // structural one.
    for (const NodeKind kind : kDrawOrder) {
        for (const Node& n : graph_.nodes()) {
            if (n.kind == kind)
                drawNode(p, n);
        }
    }
    // Then every label, so no body can cover text.
    for (const NodeKind kind : kDrawOrder) {
        for (const Node& n : graph_.nodes()) {
            if (n.kind == kind)
                drawLabel(p, n);
        }
    }
    // And the task blocks last: a column of rows takes more of the canvas than
    // any label, and nothing else should be written over it.
    for (const Node& n : graph_.nodes()) {
        if (n.kind == NodeKind::Tasks)
            drawTaskBlock(p, n);
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

    QPen rim(withAlpha(n.rim, n.alpha), n.rimWidth);
    if (n.dashed) {
        // In rim widths, so the dash keeps its proportions as the camera zooms.
        rim.setDashPattern({ 2.4, 2.0 });
    }
    p.setPen(rim);
    p.setBrush(withAlpha(n.fill, n.alpha));

    if (n.shape == NodeShape::Box) {
        const QRectF body(c.x() - r, c.y() - r, 2.0 * r, 2.0 * r);
        p.drawRoundedRect(body, kBoxCorner, kBoxCorner);

        // The glyph is what tells four kinds of box apart, so it is drawn
        // whenever the box is big enough to hold a character at all and
        // dropped, rather than shrunk, below that.
        if (!n.glyph.isNull() && n.glyph != QChar(' ') && r >= kGlyphMinRadiusPx) {
            QFont font = theme::mono(static_cast<int>(std::clamp(r * 1.1, 7.0, 13.0)));
            font.setBold(true);
            p.setFont(font);
            p.setPen(withAlpha(n.rim, n.alpha));
            p.drawText(body, Qt::AlignCenter, QString(n.glyph));
        }
        return;   // no progress arc on a box
    }

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
    // Every box labels itself the way a loose agent does, beside rather than
    // under: a box is wider than an agent and a label centred under a row of
    // them runs into its neighbours.
    case NodeKind::Shell:
    case NodeKind::Monitor:
    case NodeKind::Tool:
    case NodeKind::Plan:
        if (r < kLabelMinRadiusPx)
            return;
        under = false;
        break;
    // Not a label at all: a column of rows with its own layout.
    case NodeKind::Tasks:
        return;
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

    // One line centres on the node. Two share it, so the pair still reads as
    // belonging to the box rather than hanging off the bottom of it.
    const bool  two  = !n.detail.isEmpty();
    const qreal mid  = (fm.ascent() - fm.descent()) * 0.5;
    const qreal rise = two ? (fm.height() + kDetailGap) * 0.5 : 0.0;

    p.drawText(QPointF(x, c.y() + mid - rise), cut);
    if (!two)
        return;

    // What the shell is saying now, under what it was asked to do. Dimmer,
    // because the command is the identity and the output is the weather.
    const QString tail = fm.elidedText(n.detail, Qt::ElideRight, avail);
    const qreal   tadv = fm.horizontalAdvance(tail);
    const qreal   tx   = n.labelRight ? c.x() + r + kLabelGap : c.x() - r - kLabelGap - tadv;

    p.setPen(withAlpha(withAlpha(theme::c(kFg4), static_cast<qreal>(kDetailAlpha) / 255.0),
                       n.alpha));
    p.drawText(QPointF(tx, c.y() + mid - rise + fm.height() + kDetailGap), tail);
}

void NodeExplorerPanel::drawTaskBlock(QPainter& p, const Node& n) const
{
    if (n.taskLines.empty() || n.alpha <= 0.01)
        return;

    const QSizeF  view = QSizeF(size());
    const QPointF c    = camera_.toScreen(n.pos, view);
    const qreal   r    = n.radius * n.popScale * camera_.scale();
    if (r < kLabelMinRadiusPx)
        return;

    const QFont         font = theme::mono(10);
    const QFontMetricsF fm(font);
    p.setFont(font);

    // Same side rule as every other box, so a list radiates out of the cluster
    // rather than being written back over it.
    const qreal edge  = n.labelRight ? view.width() - c.x() - r - kLabelGap
                                     : c.x() - r - kLabelGap;
    const qreal avail = std::min(kTaskBlockMaxPx, edge - kLabelPad);
    if (avail < kLabelMinWidthPx + kTaskMarkPx)
        return;

    // A header only when the block is not showing everything, because a count
    // that always agrees with what is on screen is a line spent saying nothing.
    const int hidden = n.taskTotal - static_cast<int>(n.taskLines.size());
    QString   header;
    if (hidden > 0) {
        header = QStringLiteral("%1 of %2 tasks, %3 done")
                     .arg(n.taskLines.size())
                     .arg(n.taskTotal)
                     .arg(n.taskDone);
    }

    const int   rows   = static_cast<int>(n.taskLines.size()) + (header.isEmpty() ? 0 : 1);
    const qreal step   = fm.height() + kTaskLineGap;
    const qreal height = step * static_cast<qreal>(rows) - kTaskLineGap;

    // Centred on the node, so the block reads as belonging to it however long
    // it is, and clamped so a list taller than the canvas still starts on it.
    qreal y = c.y() - height * 0.5 + fm.ascent();
    y       = std::max(y, kLabelPad + fm.ascent());

    const qreal left = n.labelRight ? c.x() + r + kLabelGap
                                    : c.x() - r - kLabelGap - avail;

    if (!header.isEmpty()) {
        p.setPen(withAlpha(withAlpha(theme::c(kFg4), static_cast<qreal>(kDetailAlpha) / 255.0),
                           n.alpha));
        p.drawText(QPointF(left, y), fm.elidedText(header, Qt::ElideRight, avail));
        y += step;
    }

    for (const TaskLine& line : n.taskLines) {
        const qreal indent = kTaskIndentPx * static_cast<qreal>(line.depth);
        const qreal textX  = left + kTaskMarkPx + indent;
        const qreal room   = avail - kTaskMarkPx - indent;
        if (room < kLabelMinWidthPx)
            continue;

        // The item being worked on is the one line worth finding at a glance,
        // so it is the only one drawn at full strength.
        QColor ink = theme::c(line.active ? kFg1 : kFg3);
        if (line.done)
            ink = withAlpha(theme::c(kFg4), static_cast<qreal>(kTaskDoneAlpha) / 255.0);

        p.setPen(withAlpha(ink, n.alpha));
        if (line.mark != QChar(' '))
            p.drawText(QPointF(left + indent, y), QString(line.mark));
        p.drawText(QPointF(textX, y), fm.elidedText(line.text, Qt::ElideRight, room));
        y += step;
    }
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
