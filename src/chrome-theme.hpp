#pragma once

#include <QBrush>
#include <QColor>
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QObject>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>

// Theme snapshots are public value data shared between the worker and GUI.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct ChromeThemeBorder {
  QVector<QColor> colors;
  qreal angle = 0.0;
  [[nodiscard]] QBrush brush(const QRectF &bounds) const;
  bool operator==(const ChromeThemeBorder &) const = default;
};

// Explicit, usable defaults also cover missing or malformed theme data. These
// are paint values only; Qt's platform theme and the chrome fonts stay pinned.
struct ChromeTheme {
  QColor surface{18, 18, 22};
  QColor foreground{245, 245, 247};
  QColor canvasSurface{36, 36, 36};
  QColor scrim{0, 0, 0};
  QColor muted{169, 182, 203};
  QColor accent{10, 132, 255};
  QColor accentText{255, 255, 255};
  QColor warning{255, 159, 10};
  QColor button{34, 34, 40, 244};
  QColor buttonHover{48, 48, 56, 248};
  QColor buttonSelected{66, 66, 75, 250};
  QColor buttonText{245, 245, 247};
  QColor buttonHoverText{245, 245, 247};
  QColor buttonSelectedText{245, 245, 247};
  ChromeThemeBorder buttonBorder{{QColor(255, 255, 255, 26)}};
  ChromeThemeBorder buttonHoverBorder{{QColor(255, 255, 255, 40)}};
  ChromeThemeBorder buttonSelectedBorder{{QColor(255, 255, 255, 64)}};
  ChromeThemeBorder activeBorder{{QColor(245, 245, 247, 210)}};
  ChromeThemeBorder inactiveBorder{{QColor(245, 245, 247, 75)}};
  ChromeThemeBorder selectionBorder{{QColor(10, 132, 255)}};
  ChromeThemeBorder panelBorder{{QColor(255, 255, 255, 34)}};
  QColor tooltipBackground{18, 18, 22};
  QColor tooltipText{245, 245, 247};
  QColor tooltipBorder{58, 58, 64};
  QColor toastBackground{12, 12, 16, 205};
  QColor toastText{240, 240, 245};
  [[nodiscard]] QString tooltipStyleSheet() const;
  bool operator==(const ChromeTheme &) const = default;
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

[[nodiscard]] QString chromeThemeDirectory();
// Disk reads and parsing belong on a worker, never in a paint/input handler.
[[nodiscard]] ChromeTheme loadChromeTheme(const QString &directory);
/// Shared paint values. No file access or initialization in the paint path.
[[nodiscard]] const ChromeTheme &chromeTheme();
[[nodiscard]] QColor chromeAlpha(QColor color, int alpha);
/// Called once by main; workers load the palette and watch live theme changes.
/// An explicit directory also allows isolated theme integration tests.
void initializeChromeTheme(const QString &directory = chromeThemeDirectory());

class ChromeThemeWatcher final : public QObject {
  Q_OBJECT
public:
  explicit ChromeThemeWatcher(const QString &directory,
                              QObject *parent = nullptr);
  [[nodiscard]] const ChromeTheme &theme() const { return theme_; }

signals:
  void changed();

private:
  struct Snapshot {
    ChromeTheme theme;
    QStringList paths;
  };
  void reload();
  QString directory_;
  ChromeTheme theme_;
  QFileSystemWatcher files_;
  QTimer debounce_;
  QFutureWatcher<Snapshot> loader_;
  bool loading_ = false;
  bool pending_ = false;
  bool rearm_ = false;
};
