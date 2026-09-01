#include "widgets.h"

#include "project_model.h"
#include "theme_qt.h"

#include <QLinearGradient>
#include <QPainter>

namespace pm::gui {
namespace {

using namespace pm::theme;

QColor chipColour(GitState state, int dirty)
{
    switch (state) {
    case GitState::Dirty: return theme::c(kWarning);
    case GitState::Clean: return theme::c(kSuccess);
    case GitState::Error: return theme::c(kRedDark);
    default:              return theme::c(kFg4);
    }
    (void)dirty;
}

} // namespace

// ---------------------------------------------------------------- TrackedLabel

TrackedLabel::TrackedLabel(const QString& text, int px, int weight, qreal em,
                           QWidget* parent)
    : QLabel(parent)
{
    setFont(theme::tracked(px, weight, em));
    TrackedLabel::setText(text);
}

void TrackedLabel::setText(const QString& text) { QLabel::setText(text.toUpper()); }

// --------------------------------------------------------------------- TopRule

TopRule::TopRule(QWidget* parent) : QWidget(parent)
{
    setFixedHeight(1);
    setAttribute(Qt::WA_TransparentForMouseEvents);
}

void TopRule::paintEvent(QPaintEvent*)
{
    QLinearGradient g(0, 0, width(), 0);
    const QColor    red = theme::c(kRed);
    g.setColorAt(0.0, QColor(red.red(), red.green(), red.blue(), 0));
    g.setColorAt(0.5, red);
    g.setColorAt(1.0, QColor(red.red(), red.green(), red.blue(), 0));

    QPainter p(this);
    p.fillRect(rect(), g);
}

// ---------------------------------------------------------------- GrainOverlay

GrainOverlay::GrainOverlay(QWidget* parent) : QWidget(parent)
{
    // Clicks pass straight through to the table beneath.
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);
}

void GrainOverlay::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.drawTiledPixmap(rect(), theme::grainTile());
}

// -------------------------------------------------------------- ProjectDelegate

ProjectDelegate::ProjectDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

QSize ProjectDelegate::sizeHint(const QStyleOptionViewItem& opt,
                                const QModelIndex& index) const
{
    QSize s = QStyledItemDelegate::sizeHint(opt, index);
    s.setHeight(26);
    return s;
}

void ProjectDelegate::paint(QPainter* p, const QStyleOptionViewItem& opt,
                            const QModelIndex& index) const
{
    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);

    const bool selected = opt.state & QStyle::State_Selected;
    const bool hovered  = opt.state & QStyle::State_MouseOver;

    QRect r = opt.rect;

    p->fillRect(r, theme::c(selected ? kElevated : (hovered ? kElevated : kSurface)));

    // The selected row's 2px red edge. This is how "red is the only accent"
    // survives a table: the row fill stays a neutral elevated surface and the
    // accent is a single hairline.
    if (selected && index.column() == ProjectModel::ColName)
        p->fillRect(QRect(r.left(), r.top(), 2, r.height()), theme::c(kRed));

    const QString text = index.data(Qt::DisplayRole).toString();
    QRect         textRect = r.adjusted(10, 0, -10, 0);

    const int column = index.column();

    if (column == ProjectModel::ColChanges || column == ProjectModel::ColOpen) {
        if (!text.isEmpty()) {
            const auto state = static_cast<GitState>(
                index.data(ProjectModel::GitStateRole).toInt());

            const QColor accent = column == ProjectModel::ColOpen
                                    ? theme::c(kInfo)
                                    : chipColour(state, 0);

            p->setFont(theme::mono(11));
            const int w = p->fontMetrics().horizontalAdvance(text) + 14;
            QRect chip(textRect.left(), r.center().y() - 9, w, 18);

            p->setPen(QPen(theme::c(kBorder), 1));
            p->setBrush(theme::c(kBlack));
            p->drawRoundedRect(chip.adjusted(0, 0, -1, -1), 2, 2);

            p->setPen(accent);
            p->drawText(chip, Qt::AlignCenter, text);
            p->restore();
            return;
        }
    }

    if (column == ProjectModel::ColName) {
        p->setFont(theme::body(13, selected ? QFont::DemiBold : QFont::Normal));
        p->setPen(theme::c(selected ? kFg1 : kFg2));
        textRect.adjust(4, 0, 0, 0);
    } else if (column == ProjectModel::ColBranch) {
        p->setFont(theme::mono(11));
        p->setPen(theme::c(kFg3));
    } else {
        p->setFont(theme::mono(11));
        p->setPen(theme::c(kFg4));
    }

    p->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                p->fontMetrics().elidedText(text, Qt::ElideRight, textRect.width()));
    p->restore();
}

} // namespace pm::gui
