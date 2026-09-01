#pragma once

#include <QLabel>
#include <QStyledItemDelegate>
#include <QWidget>

namespace pm::gui {

// A label in tracked all-caps. QSS has neither letter-spacing nor
// text-transform, so both happen here rather than in the stylesheet.
class TrackedLabel : public QLabel {
    Q_OBJECT
public:
    TrackedLabel(const QString& text, int px, int weight, qreal em,
                 QWidget* parent = nullptr);
    void setText(const QString& text);
};

// The single 1px red scanline the design language puts at the top of every
// screen, fading out at both ends.
class TopRule : public QWidget {
    Q_OBJECT
public:
    explicit TopRule(QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent*) override;
};

// The 2.5% film grain, tiled over everything.
//
// Transparent to the mouse, so it is a purely visual layer and clicks reach the
// table underneath. This is the literal translation of the web version's
// position:absolute; inset:0; pointer-events:none.
class GrainOverlay : public QWidget {
    Q_OBJECT
public:
    explicit GrainOverlay(QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent*) override;
};

// Paints the project rows: a 2px red left edge on the selected row, and status
// chips with 2px corners. Status colours appear only inside a chip, never as a
// section accent.
class ProjectDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit ProjectDelegate(QObject* parent = nullptr);

    void  paint(QPainter* p, const QStyleOptionViewItem& opt,
                const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& opt,
                   const QModelIndex& index) const override;
};

} // namespace pm::gui
