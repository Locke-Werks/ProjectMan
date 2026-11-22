#include "node_explorer_panel.h"

#include "theme_qt.h"

#include <QFontMetricsF>
#include <QtMath>

#include <functional>
#include <QHideEvent>
#include <QLineF>
#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <iterator>

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

// The overlay: the counts bottom left, the key bottom right. Both are painted
// onto the canvas rather than put in a status bar, so windowed and fullscreen
// are the same picture and there is no widget to theme.
constexpr qreal kChromePad = 14.0;
constexpr qreal kLegendBox = 11.0;

// One row of the key. It carries the same fields the painter reads off a real
// node, because a key drawn with different primitives than the thing it
// explains is a key that goes stale without anyone noticing.
struct LegendRow {
    const char*     text;
    bool            box;
    qreal           radius;     // discs
    char            glyph;      // boxes
    bool            dashed;
    qreal           rimWidth;
    Rgb             rim;
    Rgb             fill;
    bool            rings = false;   // the NEEDS YOU halo
    bool            arc   = false;   // a run's progress
};

// Ordered the way the canvas is read: the bodies that think, then the work
// they are doing, then the one state worth crossing the room for.
constexpr LegendRow kLegend[] = {
    { "session",    false, 7.5, '\0', false, 2.0, kFg2,      kSurface },
    { "workflow",   false, 4.6, '\0', false, 1.5, kBorderHi, kSurface, false, true },
    { "subagent",   false, 2.8, '\0', false, 1.0, kFg2,      kElevated },
    { "shell",      true,  0.0, '>',  false, 1.2, kFg2,      kElevated },
    { "monitor",    true,  0.0, '~',  true,  1.2, kFg2,      kSurface },
    { "tool call",  true,  0.0, '.',  false, 1.0, kFg3,      kSurface },
    { "task list",  true,  0.0, '#',  true,  1.0, kFg3,      kSurface },
    { "plan",       true,  0.0, '=',  true,  1.0, kFg3,      kSurface },
    { "needs you",  false, 4.0, '\0', false, 2.0, kRed,      kSurface, true },
};

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

// The terminal pane beside a shell, monitor or subagent.
constexpr qreal kTermWidthPx    = 320.0;
constexpr qreal kTermMinWidthPx = 110.0;  // narrower than this is not a terminal
constexpr qreal kTermPadX       = 6.0;
constexpr qreal kTermPadY       = 3.0;
constexpr qreal kTermRowGap     = 1.0;
constexpr qreal kTermCorner     = 3.0;
constexpr qreal kTermFillAlpha  = 0.92;  // the links behind it stay faintly visible

// The tool log, top left. A line holds at full strength, then fades and goes.
// Decoration rather than a record: a line longer than the canvas runs off the
// edge, and nothing is kept once it fades.
constexpr qint64 kLogHoldMs   = 8000;
constexpr qint64 kLogFadeMs   = 4000;
constexpr qint64 kLogFadeInMs = 160;
constexpr qreal  kLogGlideMs  = 50.0;   // time constant of the push-down
constexpr size_t kLogMaxLines = 40;
constexpr int    kLogTimerMs  = 33;
// Under the graph's own text: these are glanced at, not read.
constexpr qreal kLogAlpha = 0.6;

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
    // project filter. K and T are shortcuts MainWindow scopes to this widget;
    // nothing here reads a key itself.
    setFocusPolicy(Qt::StrongFocus);

    tick_ = new QTimer(this);
    tick_->setInterval(kTimerMs);
    // Coarse on purpose. A precise timer raises the system timer resolution for
    // the whole machine, and the accumulator absorbs the jitter anyway.
    tick_->setTimerType(Qt::CoarseTimer);
    connect(tick_, &QTimer::timeout, this, &NodeExplorerPanel::onTick);

    logTick_ = new QTimer(this);
    logTick_->setInterval(kLogTimerMs);
    logTick_->setTimerType(Qt::CoarseTimer);
    connect(logTick_, &QTimer::timeout, this, &NodeExplorerPanel::onLogTick);
    logClock_.start();
}

void NodeExplorerPanel::addToolCalls(const std::vector<LoggedCall>& calls)
{
    if (!isVisible())
        return;

    const qint64 now = logClock_.elapsed();
    for (const LoggedCall& c : calls) {
        // Every line already up gives a slot to the new one. Its y is left
        // where it is and glides down to the slot on the next ticks.
        log_.push_front({QString::fromStdString(c.tool), QString::fromStdString(c.text), now, 0});
    }
    while (log_.size() > kLogMaxLines)
        log_.pop_back();

    if (!logTick_->isActive()) {
        logLastMs_ = now;
        logTick_->start();
    }
    update();
}

void NodeExplorerPanel::onLogTick()
{
    const qint64 now = logClock_.elapsed();
    const qreal  dt  = static_cast<qreal>(now - logLastMs_);
    logLastMs_       = now;

    while (!log_.empty() && now - log_.back().bornMs >= kLogHoldMs + kLogFadeMs)
        log_.pop_back();

    const QFontMetricsF fm(theme::mono(11));
    const qreal         step = fm.height() + 2.0;
    const qreal         k    = 1.0 - std::exp(-dt / kLogGlideMs);
    qreal               slot = 0;
    for (LogLine& l : log_) {
        l.y += (slot - l.y) * k;
        slot += step;
    }

    // Only the strip the log occupies. A full-canvas repaint at 30 frames a
    // second, for as long as any line was fading, was most of what a still
    // graph cost while agents were busy. The painter clips everything else.
    qreal bottom = slot;
    for (const LogLine& l : log_)
        bottom = std::max(bottom, l.y + step);
    update(QRect(0, 0, width(), qCeil(bottom + 3.0 * step)));
    if (log_.empty())
        logTick_->stop();
}

void NodeExplorerPanel::setBoard(const AgentList& board)
{
    // Births and deaths animate only when someone is looking. A pop is an
    // event, and an event nobody saw must not be replayed on arrival.
    const bool watching = isVisible();
    const bool changed  = graph_.setBoard(board, watching);

    seeded_ = true;
    publishCounts();

    // Labels and terminal panes change text on nearly every refresh without
    // anything moving. One repaint shows it; waking the simulation would not.
    if (watching)
        update();

    if (!changed)
        return;
    if (watching)
        wake();
    else
        warmNeeded_ = true;
}

void NodeExplorerPanel::setTerminalsVisible(bool on)
{
    if (terminals_ == on)
        return;
    terminals_ = on;
    update();
}

bool NodeExplorerPanel::hasTerminal(const Node& n) const
{
    if (!terminals_ || n.term.empty())
        return false;
    switch (n.kind) {
    case NodeKind::Shell:
    case NodeKind::Monitor:
        return true;
    // A crowd too big to label is far too big for twenty panes.
    case NodeKind::Agent:
        return !n.labelHidden;
    default:
        return false;
    }
}

void NodeExplorerPanel::drawTerminal(QPainter& p, const Node& n) const
{
    if (!hasTerminal(n) || n.alpha <= 0.01)
        return;

    const QSizeF  view = QSizeF(size());
    const QPointF c    = camera_.toScreen(n.pos, view);
    const qreal   r    = n.radius * n.popScale * camera_.scale();
    if (r < kLabelMinRadiusPx)
        return;

    // Built once: QFont construction and metrics are not free, and this runs
    // for every pane every frame.
    static const QFont         titleFont = theme::mono(10);
    static const QFont         bodyFont  = theme::mono(9);
    static const QFontMetricsF tfm(titleFont);
    static const QFontMetricsF bfm(bodyFont);

    // Same side rule as every label, so panes radiate out of the cluster.
    const qreal edge  = n.labelRight ? view.width() - c.x() - r - kLabelGap
                                     : c.x() - r - kLabelGap;
    // Whole pixels, so a pane drifting near the edge re-renders once per pixel
    // of width it loses rather than on every sub-pixel step of the simulation.
    const qreal width = std::floor(std::min(kTermWidthPx, edge - kLabelPad));
    if (width < kTermMinWidthPx)
        return;

    const qreal titleH = std::ceil(tfm.height() + 2.0 * kTermPadY);
    const qreal rowH   = bfm.height() + kTermRowGap;
    const qreal bodyH  = rowH * static_cast<qreal>(n.term.size()) - kTermRowGap + 2.0 * kTermPadY;
    const qreal height = std::ceil(titleH + bodyH);

    const qreal left = n.labelRight ? c.x() + r + kLabelGap : c.x() - r - kLabelGap - width;
    qreal       top  = c.y() - height * 0.5;
    top = std::clamp(top, kLabelPad, std::max(kLabelPad, view.height() - kLabelPad - height));

    // Everything the picture depends on. Position and alpha are not in it:
    // those are applied when the cached picture is drawn.
    std::size_t h = std::hash<std::string>{}(n.label.toStdString());
    const auto mix = [&h](std::size_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); };
    mix(static_cast<std::size_t>(width));
    mix(static_cast<std::size_t>(n.labelColor.rgba()));
    for (const OutputLine& line : n.term) {
        mix(std::hash<std::string>{}(line.text));
        mix(static_cast<std::size_t>(line.tone));
    }

    const qreal dpr  = devicePixelRatioF();
    PaneCache&  slot = panes_[n.key];
    slot.used        = paneFrame_;

    if (slot.hash != h || slot.pixmap.isNull() || slot.pixmap.devicePixelRatioF() != dpr) {
        slot.hash = h;
        QPixmap pm(QSize(qCeil(width * dpr), qCeil(height * dpr)));
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);

        QPainter q(&pm);
        q.setRenderHint(QPainter::Antialiasing, true);
        q.setRenderHint(QPainter::TextAntialiasing, true);

        const QRectF pane(0.5, 0.5, width - 1.0, height - 1.0);
        const QRectF title(0.0, 0.0, width, titleH);

        q.setPen(QPen(theme::c(kBorder), 1.0));
        q.setBrush(withAlpha(theme::c(kBlack), kTermFillAlpha));
        q.drawRoundedRect(pane, kTermCorner, kTermCorner);

        q.setPen(Qt::NoPen);
        q.setBrush(withAlpha(theme::c(kElevated), kTermFillAlpha));
        q.setClipRect(title);
        q.drawRoundedRect(pane, kTermCorner, kTermCorner);
        q.setClipping(false);

        const qreal textW = width - 2.0 * kTermPadX;
        q.setFont(titleFont);
        q.setPen(n.labelColor);
        q.drawText(QPointF(kTermPadX, kTermPadY + tfm.ascent()),
                   tfm.elidedText(n.label, Qt::ElideRight, textW));

        q.setFont(bodyFont);
        qreal y = titleH + kTermPadY + bfm.ascent();
        for (const OutputLine& line : n.term) {
            pm::theme::Rgb ink = kFg2;
            if (line.tone == OutputLine::Tone::Command)
                ink = kInfo;
            else if (line.tone == OutputLine::Tone::Quiet)
                ink = kFg4;
            q.setPen(theme::c(ink));
            q.drawText(QPointF(kTermPadX, y),
                       bfm.elidedText(QString::fromStdString(line.text), Qt::ElideRight, textW));
            y += rowH;
        }
        q.end();
        slot.pixmap = std::move(pm);
    }

    // Pop with the node, so a pane does not hang at full size beside a box that
    // is still growing or already retracting. Snapped to whole pixels so the
    // blit is a copy rather than a resample.
    const qreal oldOpacity = p.opacity();
    p.setOpacity(oldOpacity * n.alpha);
    p.drawPixmap(QPointF(std::round(left), std::round(top)), slot.pixmap);
    p.setOpacity(oldOpacity);
}

void NodeExplorerPanel::setLegendVisible(bool on)
{
    if (legend_ == on)
        return;
    legend_ = on;
    // A repaint, not a wake. The graph has not moved, and starting the frame
    // loop to show a legend would keep a settled canvas awake for 0.4s for
    // nothing.
    update();
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
        // nothing is running. The key stays off here: a legend for shapes that
        // are not on the canvas is decoration.
        drawNotice(p, seeded_ ? QStringLiteral("Nothing running.")
                              : QStringLiteral("Reading sessions."));
        drawToolLog(p);
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
    // Terminal panes over the labels: each is opaque, and a neighbour's label
    // drawn across one would be read as a line of its output.
    ++paneFrame_;
    for (const NodeKind kind : kDrawOrder) {
        for (const Node& n : graph_.nodes()) {
            if (n.kind == kind)
                drawTerminal(p, n);
        }
    }
    // Drop pictures for panes that were not drawn this frame: the node went,
    // or its pane is off. Cheap, and it bounds the cache by what is on screen.
    for (auto it = panes_.begin(); it != panes_.end();) {
        if (it->second.used != paneFrame_)
            it = panes_.erase(it);
        else
            ++it;
    }
    // And the task blocks last: a column of rows takes more of the canvas than
    // any label, and nothing else should be written over it.
    for (const Node& n : graph_.nodes()) {
        if (n.kind == NodeKind::Tasks)
            drawTaskBlock(p, n);
    }

    // The overlay sits over everything, because it is the one thing on screen
    // that is not the graph and it has to stay readable whatever the graph
    // does underneath it.
    drawCounts(p);
    if (legend_)
        drawLegend(p);
    drawToolLog(p);
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
    if (n.label.isEmpty() || n.alpha <= 0.01 || hasTerminal(n))
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

// ---------------------------------------------------------------- the overlay

void NodeExplorerPanel::drawCounts(QPainter& p) const
{
    if (counts_.isEmpty())
        return;

    const QFont font = theme::tracked(10, QFont::DemiBold, 0.14);
    p.setFont(font);
    p.setPen(theme::c(kFg4));
    p.drawText(QPointF(kChromePad, static_cast<qreal>(height()) - kChromePad),
               counts_.toUpper());
}

// The key, bottom right.
//
// Every sample is drawn by the same primitives the canvas uses rather than
// described in words, because the thing being explained is a shape. A row
// saying "a square with a > in it is a shell" is a sentence to be decoded; a
// square with a > in it beside the word "shell" is the thing itself.
//
// It is the one piece of chrome on the canvas, and the one thing here that a
// key can turn off, because a legend earns its space on the first day and
// stops earning it on the tenth.
void NodeExplorerPanel::drawLegend(QPainter& p) const
{
    const QFont label = theme::body(11);
    const QFont head  = theme::tracked(10, QFont::DemiBold, 0.14);
    const QFont glyph = theme::mono(9);

    const QFontMetricsF fm(label);
    const qreal         step = std::max(15.0, fm.height() + 3.0);
    const qreal         cell = 18.0;   // the sample column
    const qreal         gap  = 8.0;    // sample to text

    // Widest thing in the block decides the block, footer included. It is
    // wider than any row label and it is drawn in its own font, so measuring
    // the rows alone put the right-aligned block where the footer ran off the
    // edge of the canvas.
    const QFont         foot = theme::body(10);
    const QFontMetricsF footFm(foot);
    const QString       footText = QStringLiteral("solid rim runs now, broken waits");

    qreal textW = footFm.horizontalAdvance(footText) - cell - gap;
    for (const LegendRow& r : kLegend)
        textW = std::max(textW, fm.horizontalAdvance(QString::fromUtf8(r.text)));

    // Header, a gap under it, one line per row, and the footer. Counted in
    // whole steps because the block is laid out from its bottom edge upward:
    // get this short and the footer is drawn off the bottom, which is exactly
    // what it did.
    const qreal rows   = static_cast<qreal>(std::size(kLegend));
    const qreal blockW = cell + gap + textW;
    const qreal blockH = step * (rows + 2.8);

    // Nothing is worth covering the graph with. On a window too small to hold
    // the key beside the work, the work wins and the key is simply not drawn.
    if (blockW + kChromePad * 2 > static_cast<qreal>(width()) * 0.45
        || blockH + kChromePad * 2 > static_cast<qreal>(height()) * 0.8)
        return;

    const qreal right = static_cast<qreal>(width()) - kChromePad;
    const qreal left  = right - blockW;
    qreal       y     = static_cast<qreal>(height()) - kChromePad - blockH + step;

    p.setFont(head);
    p.setPen(theme::c(kFg4));
    p.drawText(QPointF(left, y), QStringLiteral("KEY"));
    y += step * 1.6;

    for (const LegendRow& r : kLegend) {
        const QPointF centre(left + cell * 0.5, y - fm.ascent() * 0.4);

        p.setPen(QPen(theme::c(r.rim), r.rimWidth,
                      r.dashed ? Qt::DotLine : Qt::SolidLine));
        p.setBrush(theme::c(r.fill));

        if (r.box) {
            const qreal h = kLegendBox * 0.5;
            p.drawRect(QRectF(centre.x() - h, centre.y() - h, kLegendBox, kLegendBox));
            if (r.glyph != '\0') {
                p.setFont(glyph);
                p.setPen(theme::c(kFg2));
                p.drawText(QRectF(centre.x() - h, centre.y() - h, kLegendBox, kLegendBox),
                           Qt::AlignCenter, QString(QChar(r.glyph)));
            }
        } else {
            p.drawEllipse(centre, r.radius, r.radius);
            // The session in NEEDS YOU wears rings, and the only way to show
            // that is to draw them.
            if (r.rings) {
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(theme::c(kRed), 1.0));
                p.drawEllipse(centre, r.radius + 2.5, r.radius + 2.5);
                p.drawEllipse(centre, r.radius + 5.0, r.radius + 5.0);
            }
            // A run carries an arc saying how far through its agents it is.
            if (r.arc) {
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(theme::c(kFg1), 1.6));
                const QRectF box(centre.x() - r.radius - 2.0, centre.y() - r.radius - 2.0,
                                 (r.radius + 2.0) * 2, (r.radius + 2.0) * 2);
                p.drawArc(box, 90 * 16, -230 * 16);
            }
        }

        p.setBrush(Qt::NoBrush);
        p.setFont(label);
        p.setPen(theme::c(kFg3));
        p.drawText(QPointF(left + cell + gap, y), QString::fromUtf8(r.text));
        y += step;
    }

    // The one rule that is not a shape: what a rim means.
    p.setFont(foot);
    p.setPen(theme::c(kFg4));
    p.drawText(QPointF(left, y + step * 0.2), footText);
}

// Every tool call on the machine as it happens, top left: the newest on top,
// pushing the rest down, each fading on its own clock.
void NodeExplorerPanel::drawToolLog(QPainter& p) const
{
    if (log_.empty())
        return;

    const QFont         font = theme::mono(11);
    const QFontMetricsF fm(font);
    const qreal         gap  = 8.0;
    const qint64        now  = logClock_.elapsed();
    const qreal         maxY = static_cast<qreal>(height()) - kChromePad;

    p.setFont(font);
    for (const LogLine& l : log_) {
        const qreal base = kChromePad + fm.ascent() + l.y;
        if (base > maxY)
            break;

        const qint64 age = now - l.bornMs;
        qreal        a   = 1.0;
        if (age < kLogFadeInMs)
            a = static_cast<qreal>(age) / static_cast<qreal>(kLogFadeInMs);
        else if (age > kLogHoldMs) {
            // Ease in, so a line lingers near full strength before it goes.
            const qreal t = std::min(1.0, static_cast<qreal>(age - kLogHoldMs)
                                              / static_cast<qreal>(kLogFadeMs));
            a = 1.0 - t * t;
        }
        a *= kLogAlpha;
        if (a <= 0.0)
            continue;

        p.setPen(withAlpha(theme::c(kFg2), a));
        p.drawText(QPointF(kChromePad, base), l.tool);
        p.setPen(withAlpha(theme::c(kFg3), a));
        p.drawText(QPointF(kChromePad + fm.horizontalAdvance(l.tool) + gap, base), l.text);
    }
}

} // namespace pm::gui
