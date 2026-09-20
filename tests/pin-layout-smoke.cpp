/** @fileoverview Tests stacked pin slots and compositor dispatch strings. */
#include "pin-layout-smoke.hpp"

#include "pin-layout.hpp"
#include "cli-path.hpp"
#include <QCommandLineParser>
#include <QJsonArray>
#include <QRectF>
#include <QTransform>
#include <QtTypes>
#include <cmath>

#include <QSet>

bool runPinLayoutSmoke(QString &error) {
  // The frame follows the display's shape at a fixed width, clamps the
  // extremes, and guesses 16:9 when the display cannot be asked.
  if (pinFrameSize(QSize(2560, 1600)) != QSize(200, 125) ||
      pinFrameSize(QSize(3440, 1440)) != QSize(200, 84) ||
      pinFrameSize(QSize(1080, 1920)) != QSize(200, 356) ||
      pinFrameSize(QSize(1000, 5000)) != QSize(200, 400) ||
      pinFrameSize(QSize(5000, 500)) != QSize(200, 50) ||
      pinFrameSize(QSize()) != QSize(200, 113)) {
    error = QStringLiteral("Pin frames did not follow the display's shape");
    return false;
  }

  // Free drops must keep the whole pin reachable on every screen edge,
  // including the reported case where only three pixels remain visible.
  const QRect mainScreen(0, 0, 3072, 1728);
  const QSize preview(200, 113);
  const QList<QPair<QRect, QRect>> drops{
      {{3069, 1629, 200, 113}, {2858, 1601, 200, 113}},
      {{-197, 600, 200, 113}, {14, 600, 200, 113}},
      {{1200, -110, 200, 113}, {1200, 14, 200, 113}},
      {{1200, 1725, 200, 113}, {1200, 1601, 200, 113}},
      {{1200, 600, 200, 113}, {1200, 600, 200, 113}},
      {{5000, 3000, 200, 113}, {2858, 1601, 200, 113}}};
  for (const auto &[drop, expected] : drops) {
    const QRect visible = pinVisibleRect(drop, mainScreen, 14);
    if (visible != expected || !mainScreen.contains(visible) ||
        visible.size() != preview) {
      error = QStringLiteral("An edge drop left the pin clipped or changed its size");
      return false;
    }
  }
  const QRect leftScreen(-1024, 0, 1024, 600);
  if (pinVisibleRect(QRect(-1150, 580, 200, 113), leftScreen, 14) !=
          QRect(-1010, 473, 200, 113) ||
      pinVisibleRect(QRect(-150, 150, 200, 113), mainScreen, 14) !=
          QRect(14, 150, 200, 113) ||
      pinVisibleRect(QRect(2400, 1800, 200, 113), mainScreen, 14) !=
          QRect(2400, 1601, 200, 113)) {
    error = QStringLiteral("Drop bounds did not restore the starting monitor");
    return false;
  }
  // A tiny output cannot hold the image, but its controls stay at the
  // output's top-left. Missing geometry leaves the pin alone.
  if (pinVisibleRect(QRect(60, 80, 200, 113), QRect(10, 20, 80, 60), 14) !=
          QRect(10, 20, 200, 113) ||
      pinVisibleRect(QRect(60, 80, 200, 113), {}, 14) != QRect(60, 80, 200, 113)) {
    error = QStringLiteral("Drop bounds mishandled tiny or unavailable outputs");
    return false;
  }

  // A bar may reserve any edge (or several). Insets are already logical
  // pixels even on a scaled monitor; both free drops and stacks respect them.
  struct BarCase {
    QJsonArray reserved;
    QRect area;
    QPoint drop;
    QPoint restored;
    QPoint packed;
  };
  const QList<BarCase> bars{
      {{36, 0, 0, 0}, {36, 0, 3036, 1728}, {-50, 500}, {50, 500}, {2858, 1601}},
      {{0, 26, 0, 0}, {0, 26, 3072, 1702}, {500, 10}, {500, 40}, {2858, 1601}},
      {{0, 0, 30, 0}, {0, 0, 3042, 1728}, {2900, 500}, {2828, 500}, {2828, 1601}},
      {{0, 0, 0, 40}, {0, 0, 3072, 1688}, {500, 1640}, {500, 1561}, {2858, 1561}},
      {{36, 26, 30, 40}, {36, 26, 3006, 1662}, {-50, 1640}, {50, 1561}, {2828, 1561}},
      {{0, 0, 0, 0}, {0, 0, 3072, 1728}, {500, -50}, {500, 14}, {2858, 1601}}};
  QJsonObject monitor{{QStringLiteral("width"), 6144},
                      {QStringLiteral("height"), 3456},
                      {QStringLiteral("scale"), 2}};
  for (const auto &bar : bars) {
    monitor.insert(QStringLiteral("reserved"), bar.reserved);
    const QRect area = pinMonitorWorkArea(monitor);
    const auto packed = pinPackedPosition({}, area.size(), preview, 10, 14);
    if (area != bar.area ||
        pinVisibleRect(QRect(bar.drop, preview), area, 14) != QRect(bar.restored, preview) ||
        !packed || *packed + area.topLeft() != bar.packed ||
        pinFrameSize(pinMonitorGeometry(monitor).size()) != preview) {
      error = QStringLiteral("Pin placement ignored a reserved edge or changed its aspect ratio");
      return false;
    }
  }

  // Every control explains itself; an index outside the controls is empty.
  QSet<QString> tips;
  for (int control = 0; control < 6; ++control) {
    if (pinControlTip(control).isEmpty()) {
      error = QStringLiteral("A pin control has no tooltip");
      return false;
    }
    tips.insert(pinControlTip(control));
  }
  if (tips.size() != 6 || !pinControlTip(6).isEmpty() ||
      !pinControlTip(-1).isEmpty() || pinControlTip(5, true) == pinControlTip(5, false)) {
    error = QStringLiteral("Pin control tooltips repeat or overflow");
    return false;
  }

  const QList<QPair<QStringList, bool>> invocations{
      {{QStringLiteral("--pin"), QStringLiteral("image.png")}, true},
      {{QStringLiteral("--pin=image.png")}, true},
      {{QStringLiteral("--preview=image.png")}, true},
      {{QStringLiteral("-platform"), QStringLiteral("offscreen"),
         QStringLiteral("--preview"), QStringLiteral("image.png")}, true},
      {{QStringLiteral("-platform"), QStringLiteral("offscreen"),
         QStringLiteral("--pin=image.png")}, true},
      {{QStringLiteral("--pin"), QStringLiteral("image.png"),
         QStringLiteral("-platformtheme"), QStringLiteral("gtk3")}, true},
      {{QStringLiteral("--"), QStringLiteral("--pin")}, false},
      {{QStringLiteral("--"), QStringLiteral("--pin=image.png")}, false},
      {{QStringLiteral("--"), QStringLiteral("--preview=image.png")}, false},
      {{QStringLiteral("--file"), QStringLiteral("--pin")}, false}};
  for (const auto &[arguments, pinned] : invocations) {
    QCommandLineParser parser;
    configureCaptureCommandLine(parser, true);
    if (!parser.parse(QStringList{QStringLiteral("omasnap")} + arguments) ||
        (parser.isSet(QStringLiteral("pin")) || parser.isSet(QStringLiteral("preview"))) != pinned) {
      error = QStringLiteral("Pin argv selected the wrong Wayland shell");
      return false;
    }
  }

  const QSize screen(400, 300);
  const QSize pin(100, 80);

  const QVector<QPair<QString, QRect>> deck{
      {QStringLiteral("newest"), QRect(QPoint(), pin)},
      {QStringLiteral("middle"), QRect(QPoint(), pin)},
      {QStringLiteral("oldest"), QRect(QPoint(), pin)}};
  const auto folded = pinStackLayout(deck, {}, screen, 10, 14, false);
  const auto fanned = pinStackLayout(deck, {}, screen, 10, 14, true);
  for (int depth = 0; depth < 20; ++depth) {
    const qreal tilt = pinStackTilt(depth, false);
    if (pinStackTilt(depth, true) != 0.0 || (depth == 0 && tilt != 0.0) ||
        (depth > 0 && (std::abs(tilt) > 3.0 ||
                       (depth % 2 == 0 ? tilt <= 0.0 : tilt >= 0.0)))) {
      error = QStringLiteral("The pin deck tilt did not stay bounded and alternate beneath a straight front card");
      return false;
    }
  }
  for (const QSize frame : {QSize(200, 50), QSize(200, 113), QSize(200, 400)}) {
    const QRectF card = QRectF(QPointF(), frame).adjusted(1, 1, -1, -1);
    for (const qreal tilt : {-3.0, -2.0, 0.0, 2.0, 3.0}) {
      const QTransform transform = pinCardTransform(frame, tilt);
      if (!QRectF(QPointF(), frame).contains(transform.mapRect(card)) ||
          (tilt == 0.0 && !transform.isIdentity())) {
        error = QStringLiteral("A tilted pin clips a corner or resizes an upright card");
        return false;
      }
    }
  }
  if (folded.size() != 3 || fanned.size() != 3 ||
      folded.at(0).second != fanned.at(0).second ||
      folded.at(0).second.top() - folded.at(1).second.top() != 12 ||
      folded.at(1).second.top() - folded.at(2).second.top() != 12 ||
      folded.at(0).second.bottom() != screen.height() - 15) {
    error = QStringLiteral("The idle pin deck moved its front card or lost its compact lips");
    return false;
  }
  for (qsizetype index = 0; index < fanned.size(); ++index) {
    if (fanned.at(index).first != deck.at(index).first ||
        !QRect(QPoint(14, 14), screen - QSize(28, 28)).contains(fanned.at(index).second)) {
      error = QStringLiteral("The pin fan changed card order or crossed an edge inset");
      return false;
    }
    for (qsizetype other = index + 1; other < fanned.size(); ++other) {
      if (fanned.at(index).second.intersects(fanned.at(other).second)) {
        error = QStringLiteral("The pin fan left a card covered");
        return false;
      }
    }
  }
  if (pinStackLayout(fanned, {}, screen, 10, 14, false) != folded ||
      pinStackLayout(folded, {}, screen, 10, 14, true) != fanned) {
    error = QStringLiteral("Opening and closing the pin fan drifted its layout");
    return false;
  }
  auto wrappedDeck = deck;
  wrappedDeck.push_back({QStringLiteral("fourth"), QRect(QPoint(), pin)});
  const auto wrappedFan = pinStackLayout(wrappedDeck, {}, screen, 10, 14, true);
  const auto wrappedFold = pinStackLayout(wrappedDeck, {}, screen, 10, 14, false);
  if (wrappedFan.size() != 4 || wrappedFold.size() != 4 ||
      wrappedFan.at(3).second != QRect(176, 206, 100, 80) ||
      wrappedFold.at(3).second != wrappedFan.at(3).second) {
    error = QStringLiteral("The pin deck did not keep wrapped columns on screen");
    return false;
  }
  const QVector<QRect> freePins{QRect(260, 195, 40, 20)};
  for (const bool expanded : {false, true}) {
    const auto besideFree = pinStackLayout(deck, freePins, screen, 10, 14, expanded);
    if (besideFree.size() != 3) {
      error = QStringLiteral("A free pin prevented an otherwise fitting deck");
      return false;
    }
    for (const auto &card : besideFree) {
      if (card.second.intersects(freePins.constFirst())) {
        error = QStringLiteral("Folding or fanning a deck covered a freely placed pin");
        return false;
      }
    }
  }
  const QRect workArea(-400, 26, screen.width(), screen.height());
  QVector<QRect> globalFan;
  for (const auto &card : fanned)
    globalFan.push_back(card.second.translated(workArea.topLeft()));
  const QRect hotZone = pinStackHotZone(globalFan, workArea);
  const QPoint gapPoint(globalFan.at(0).center().x(), globalFan.at(0).top() - 5);
  if (!hotZone.contains(gapPoint) || !workArea.contains(hotZone) ||
      hotZone.contains(QPoint(workArea.left() + 20, workArea.top() + 20)) ||
      !pinStackHotZone({}, workArea).isEmpty()) {
    error = QStringLiteral("The pin fan hover zone lost a gap or covered unrelated desktop");
    return false;
  }
  if (!pinStackLayout(deck, {}, QSize(90, 70), 10, 14, false).isEmpty() ||
      !pinStackLayout(deck, {}, QSize(120, 100), 10, 14, true).isEmpty()) {
    error = QStringLiteral("An overflowing deck returned a partial layout");
    return false;
  }

  // An empty corner takes the first pin snug against the margins; the next
  // ones pack one gap above whatever is there, whatever its size, and a
  // full column starts a new one to the left.
  const QPoint first = pinPackedPosition({}, screen, pin, 10, 14).value_or(QPoint());
  if (first != QPoint(286, 206)) {
    error = QStringLiteral("The first pin did not land in the corner");
    return false;
  }
  const QPoint second =
      pinPackedPosition({QRect(first, pin)}, screen, pin, 10, 14).value_or(QPoint());
  if (second != QPoint(286, 116)) {
    error = QStringLiteral("The second pin did not pack above the first");
    return false;
  }
  const QRect oddSize(QPoint(280, 150), QSize(110, 130));
  if (pinPackedPosition({oddSize}, screen, pin, 10, 14) != QPoint(286, 60)) {
    error = QStringLiteral("An odd-sized pin was not packed above snugly");
    return false;
  }
  const QVector<QRect> fullColumn{QRect(286, 206, 100, 80),
                                  QRect(286, 116, 100, 80),
                                  QRect(286, 26, 100, 80)};
  if (pinPackedPosition(fullColumn, screen, pin, 10, 14) !=
      QPoint(176, 206)) {
    error = QStringLiteral("A full column did not wrap to a new one");
    return false;
  }
  const QRect negativeTop(280, -20, 110, 310);
  const auto besideNegative = pinPackedPosition({negativeTop}, screen, pin, 10, 14);
  if (!besideNegative || QRect(*besideNegative, pin).intersects(negativeTop)) {
    error = QStringLiteral("A negative-top blocker was ignored during packing");
    return false;
  }
  const QRect elsewhere(QPoint(20, 20), QSize(100, 80));
  if (pinPackedPosition({elsewhere}, screen, pin, 10, 14) !=
      QPoint(286, 206)) {
    error = QStringLiteral("A pin away from the column blocked the corner");
    return false;
  }

  // Column membership is hugging the right edge; dragging a pin away from
  // it takes the pin out of the column, whatever its height.
  if (!pinInColumn(QRect(286, 26, 100, 80), screen, 14, 10) ||
      !pinInColumn(QRect(282, 140, 104, 120), screen, 14, 10) ||
      pinInColumn(QRect(200, 26, 100, 80), screen, 14, 10)) {
    error = QStringLiteral("Column membership did not follow the right edge");
    return false;
  }

  if (!pinInColumn(QRect(176, 206, 100, 80), screen, 14, 10)) {
    error = QStringLiteral("A wrapped pin was excluded from compaction");
    return false;
  }
  const QVector<QPair<QString, QRect>> wrappedColumn{
      {QStringLiteral("a"), QRect(286, 206, 100, 80)},
      {QStringLiteral("b"), QRect(286, 116, 100, 80)},
      {QStringLiteral("c"), QRect(286, 26, 100, 80)},
      {QStringLiteral("d"), QRect(176, 206, 100, 80)}};
  const auto wrappedPlan = pinInsertionPlan(
      wrappedColumn, {}, QRect(176, 110, 100, 80), screen, 10, 14);
  if (wrappedPlan.index != 4 || wrappedPlan.spot != QRect(176, 116, 100, 80)) {
    error = QStringLiteral("Insertion did not target the wrapped column");
    return false;
  }

  // Dragging a pin over the column spreads the others around a hole where
  // it would land; any overlap with that column is close enough to snap.
  const QVector<QPair<QString, QRect>> column{
      {QStringLiteral("low"), QRect(286, 206, 100, 80)},
      {QStringLiteral("high"), QRect(286, 116, 100, 80)}};
  const PinInsertionPlan between = pinInsertionPlan(
      column, {}, QRect(286, 140, 100, 80), screen, 10, 14);
  if (between.index != 1 || between.spot != QRect(286, 116, 100, 80) ||
      between.spread.size() != 2 ||
      between.spread.at(0) !=
          QPair(QStringLiteral("low"), QRect(286, 206, 100, 80)) ||
      between.spread.at(1) !=
          QPair(QStringLiteral("high"), QRect(286, 26, 100, 80))) {
    error = QStringLiteral("Hovering between pins did not open a hole there");
    return false;
  }
  // Any overlap with the stack joins it, however slight; a drag that
  // clears the stack entirely, even right beside it, stays out.
  const PinInsertionPlan grazing = pinInsertionPlan(
      column, {}, QRect(200, 140, 100, 80), screen, 10, 14);
  if (grazing.index != 1) {
    error = QStringLiteral("A partial overlap with the stack did not join it");
    return false;
  }
  if (pinInsertionPlan(column, {}, QRect(120, 140, 100, 80), screen, 10, 14)
          .index != -1) {
    error = QStringLiteral("A drag clear of the stack joined it anyway");
    return false;
  }
  const QVector<QPair<QString, QRect>> emptyColumn;
  if (pinInsertionPlan(emptyColumn, {}, QRect(240, 180, 100, 80), screen, 10,
                       14)
          .index != 0 ||
      pinInsertionPlan(emptyColumn, {}, QRect(60, 60, 100, 80), screen, 10,
                       14)
          .index != -1) {
    error = QStringLiteral("An empty stack's corner spot did not gate joining");
    return false;
  }
  // A pin nudged off the top of the stack still overlaps the spot it came
  // from and snaps back there; dragged fully past it, it is free.
  const PinInsertionPlan nudged = pinInsertionPlan(
      column, {}, QRect(250, 10, 100, 80), screen, 10, 14);
  if (nudged.index != 2 || nudged.spot != QRect(286, 26, 100, 80)) {
    error = QStringLiteral("A nudged top pin did not snap back to its seat");
    return false;
  }
  if (pinInsertionPlan(column, {}, QRect(150, 20, 100, 80), screen, 10, 14)
          .index != -1) {
    error = QStringLiteral("A pin dragged past its seat was still captured");
    return false;
  }
  // A stack that has not packed down yet is still the stack the user sees:
  // overlapping a pin's live position joins even when the packed baseline
  // is elsewhere.
  const QVector<QPair<QString, QRect>> floating{
      {QStringLiteral("high"), QRect(286, 40, 100, 80)}};
  if (pinInsertionPlan(floating, {}, QRect(240, 20, 100, 80), screen, 10, 14)
          .index == -1) {
    error = QStringLiteral("Overlapping a live pin did not join the stack");
    return false;
  }
  // The band spans the vacancies too: with one pin floating high on a
  // tall screen, a drag into the empty stretch between it and the bottom
  // seats still folds in; beside the band it stays out.
  const QSize tall(400, 600);
  const QVector<QPair<QString, QRect>> lofty{
      {QStringLiteral("high"), QRect(286, 30, 100, 80)}};
  if (pinInsertionPlan(lofty, {}, QRect(240, 200, 100, 80), tall, 10, 14)
          .index == -1) {
    error = QStringLiteral("A drag into the column's vacancy stayed out");
    return false;
  }
  if (pinInsertionPlan(lofty, {}, QRect(100, 200, 100, 80), tall, 10, 14)
          .index != -1) {
    error = QStringLiteral("A drag beside the column folded in");
    return false;
  }
  const PinInsertionPlan below = pinInsertionPlan(
      column, {}, QRect(286, 216, 100, 80), screen, 10, 14);
  if (below.index != 0 || below.spot != QRect(286, 206, 100, 80) ||
      below.spread.at(0) !=
          QPair(QStringLiteral("low"), QRect(286, 116, 100, 80))) {
    error = QStringLiteral("Hovering the corner did not open the bottom slot");
    return false;
  }
  if (pinInsertionPlan(column, {}, QRect(60, 140, 100, 80), screen, 10, 14)
          .index != -1) {
    error = QStringLiteral("A drag far from the column planned an insertion");
    return false;
  }

  // The dispatch expressions are Lua for a Lua-configured Hyprland;
  // a placement that silently does nothing is exactly the failure these guard.
  const QString title = QStringLiteral("0x1234");
  if (pinFloatDispatch(title) !=
          QStringLiteral(
              "hl.dsp.window.float({ window = \"address:0x1234\" })") ||
      pinPinDispatch(title) !=
          QStringLiteral(
              "hl.dsp.window.pin({ window = \"address:0x1234\" })") ||
      pinMoveDispatch(title, 120, 40) !=
          QStringLiteral("hl.dsp.window.move({ x = 120, y = 40, relative = "
                         "false, window = \"address:0x1234\" })") ||
      pinFocusDispatch(title) !=
          QStringLiteral("hl.dsp.focus({ window = \"address:0x1234\" })")) {
    error = QStringLiteral("Hyprland dispatch expressions were malformed");
    return false;
  }
  QVector<QRect> occupied;
  const QSize wide(2400, 110);
  for (int index = 0; index < 21; ++index) {
    const auto at = pinPackedPosition(occupied, wide, pin, 10, 14);
    if (!at || !QRect(QPoint(), wide).contains(QRect(*at, pin))) {
      error = QStringLiteral("Packing stopped before all visible columns were used");
      return false;
    }
    for (const QRect &blocker : occupied) {
      if (blocker.intersects(QRect(*at, pin))) {
        error = QStringLiteral("Packing reused an occupied slot");
        return false;
      }
    }
    occupied.push_back(QRect(*at, pin));
  }
  if (pinPackedPosition(occupied, wide, pin, 10, 14) ||
      pinPackedPosition({}, QSize(90, 70), pin, 10, 14)) {
    error = QStringLiteral("A full or undersized output returned an unsafe slot");
    return false;
  }
  QJsonObject rotated{{QStringLiteral("x"), -1080},
                             {QStringLiteral("y"), 200},
                             {QStringLiteral("width"), 3840},
                             {QStringLiteral("height"), 2160},
                             {QStringLiteral("scale"), 2},
                             {QStringLiteral("transform"), 1}};
  if (pinMonitorGeometry(rotated) != QRect(-1080, 200, 1080, 1920) ||
      pinMonitorWorkArea(rotated) != pinMonitorGeometry(rotated)) {
    error = QStringLiteral("Pin monitor geometry lost origin, scale or transform");
    return false;
  }
  rotated.insert(QStringLiteral("reserved"), QJsonArray{40, 30, 20, 10});
  if (pinMonitorWorkArea(rotated) != QRect(-1040, 230, 1020, 1880)) {
    error = QStringLiteral("Reserved edges were scaled or rotated a second time");
    return false;
  }
  return true;
}
