#include "theme_qt.h"

#include <QApplication>
#include <QFontDatabase>
#include <QImage>
#include <QRandomGenerator>

namespace pm::gui::theme {
namespace {

using pm::theme::kBlack;
using pm::theme::kBorder;
using pm::theme::kBorderHi;
using pm::theme::kElevated;
using pm::theme::kFg1;
using pm::theme::kFg2;
using pm::theme::kFg3;
using pm::theme::kFg4;
using pm::theme::kRed;
using pm::theme::kSurface;

QString hex(pm::theme::Rgb v)
{
    return QString::asprintf("#%02X%02X%02X", v.r, v.g, v.b);
}

QString g_headingFamily = QStringLiteral("Chakra Petch");
QString g_bodyFamily    = QStringLiteral("Outfit");

} // namespace

void loadFonts()
{
    const char* faces[] = {
        ":/fonts/ChakraPetch-Regular.ttf",
        ":/fonts/ChakraPetch-SemiBold.ttf",
        ":/fonts/ChakraPetch-Bold.ttf",
        ":/fonts/Outfit-Variable.ttf",
    };

    QStringList heading, body;
    for (const char* f : faces) {
        const int id = QFontDatabase::addApplicationFont(QString::fromLatin1(f));
        if (id < 0)
            continue;
        for (const QString& fam : QFontDatabase::applicationFontFamilies(id)) {
            if (fam.startsWith(QStringLiteral("Chakra")))
                heading << fam;
            else
                body << fam;
        }
    }

    // Fall back rather than fail. A missing face is a build packaging problem,
    // not a reason to refuse to start.
    if (!heading.isEmpty())
        g_headingFamily = heading.front();
    else
        g_headingFamily = QStringLiteral("Segoe UI Semibold");

    if (!body.isEmpty())
        g_bodyFamily = body.front();
    else
        g_bodyFamily = QStringLiteral("Segoe UI");

    QFont appFont(g_bodyFamily);
    appFont.setPixelSize(13);
    QApplication::setFont(appFont);
}

QFont tracked(int px, int weight, qreal emSpacing)
{
    QFont f(g_headingFamily);
    f.setPixelSize(px);
    f.setWeight(static_cast<QFont::Weight>(weight));
    // AbsoluteSpacing takes pixels, so an em figure from the design spec has to
    // be multiplied by the size it applies at.
    f.setLetterSpacing(QFont::AbsoluteSpacing, px * emSpacing);
    return f;
}

QFont body(int px, int weight)
{
    QFont f(g_bodyFamily);
    f.setPixelSize(px);
    f.setWeight(static_cast<QFont::Weight>(weight));
    return f;
}

QFont mono(int px)
{
    QFont f(QStringLiteral("Consolas"));
    f.setStyleHint(QFont::Monospace);
    f.setPixelSize(px);
    return f;
}

QString styleSheet()
{
    // Rules the design language fixes: 1px hairlines only, 2px corners
    // everywhere, red as the sole accent, and no light mode. Selection is the
    // elevated surface rather than a system highlight, because a blue bar would
    // be a second accent.
    return QStringLiteral(R"(
QWidget            { background: %1; color: %5; }
QMainWindow        { background: %1; }

QFrame#Surface     { background: %2; border: 1px solid %4; border-radius: 2px; }

QLineEdit#Filter   { background: %2; border: 1px solid %4; border-radius: 2px;
                     padding: 7px 9px; color: %8; selection-background-color: %3; }
QLineEdit#Filter:focus { border-color: %7; }

QTableView         { background: %2; border: 1px solid %4; border-radius: 2px;
                     gridline-color: transparent; outline: none;
                     selection-background-color: %3; selection-color: %8; }
QHeaderView::section { background: %1; color: %6; border: 0;
                       border-bottom: 1px solid %4; padding: 7px 10px; }
QTableView::item   { padding: 4px 10px; }

QPushButton        { background: %2; border: 1px solid %4; border-radius: 2px;
                     padding: 9px 16px; color: %5; }
QPushButton:hover  { background: %3; border-color: %9; }
QPushButton:disabled { color: %6; border-color: %4; }
QPushButton#Primary  { background: %7; border: 1px solid %7; color: %1; }
QPushButton#Primary:hover { background: #CC0000; border-color: #CC0000; }
QPushButton#Primary:disabled { background: %2; border-color: %4; color: %6; }

QCheckBox          { color: %5; spacing: 8px; }
QCheckBox::indicator { width: 13px; height: 13px; border: 1px solid %4;
                       border-radius: 2px; background: %2; }
QCheckBox::indicator:checked { background: %7; border-color: %7; }

QSplitter::handle  { background: %4; }

QScrollBar:vertical   { background: %1; width: 10px; border: 0; margin: 0; }
QScrollBar::handle:vertical { background: %4; border-radius: 2px; min-height: 24px; }
QScrollBar::handle:vertical:hover { background: %9; }
QScrollBar:horizontal { background: %1; height: 10px; border: 0; margin: 0; }
QScrollBar::handle:horizontal { background: %4; border-radius: 2px; min-width: 24px; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QToolTip           { background: %3; color: %5; border: 1px solid %4;
                     border-radius: 2px; padding: 4px 6px; }
)")
        .arg(hex(kBlack))     // %1
        .arg(hex(kSurface))   // %2
        .arg(hex(kElevated))  // %3
        .arg(hex(kBorder))    // %4
        .arg(hex(kFg2))       // %5
        .arg(hex(kFg4))       // %6
        .arg(hex(kRed))       // %7
        .arg(hex(kFg1))       // %8
        .arg(hex(kBorderHi)); // %9
}

const QPixmap& grainTile()
{
    static const QPixmap tile = [] {
        QImage img(128, 128, QImage::Format_ARGB32_Premultiplied);
        QRandomGenerator rng(0xA2C7F1u);   // fixed seed; see the header
        for (int y = 0; y < img.height(); ++y) {
            auto* line = reinterpret_cast<QRgb*>(img.scanLine(y));
            for (int x = 0; x < img.width(); ++x) {
                const int v = static_cast<int>(rng.bounded(256));
                line[x] = qPremultiply(qRgba(v, v, v, 6));   // 6/255 is about 2.5%
            }
        }
        return QPixmap::fromImage(img);
    }();
    return tile;
}

} // namespace pm::gui::theme
