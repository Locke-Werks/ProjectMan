#pragma once

#include "theme.h"

#include <QColor>
#include <QFont>
#include <QPixmap>
#include <QString>

namespace pm::gui {

// The ARCHON / Specter Point design language, translated into Qt.
//
// The token values live in core/theme.h so the console and the desktop front
// ends cannot drift apart. Only the translation is here.
namespace theme {

inline QColor c(pm::theme::Rgb v) { return QColor(v.r, v.g, v.b); }

// Loads the bundled faces. Must run before any widget exists.
void loadFonts();

// Tracked all-caps, the design language's headline treatment.
//
// This is a real font rather than a stylesheet rule because QSS has neither
// letter-spacing nor text-transform. Every tracked label, delegate-drawn cell
// and button goes through here.
QFont tracked(int px, int weight, qreal emSpacing);
QFont body(int px, int weight = QFont::Normal);
QFont mono(int px);

// Everything QSS can express, and nothing it cannot.
QString styleSheet();

// A fixed-seed noise tile at 2.5% alpha.
//
// The seed is fixed on purpose: a re-seeded tile makes the grain crawl on
// every resize, which reads as a rendering fault rather than as film.
const QPixmap& grainTile();

} // namespace theme
} // namespace pm::gui
