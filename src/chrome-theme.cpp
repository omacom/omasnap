#include "chrome-theme.hpp"

#include <QApplication>
#include <QBrush>
#include <QByteArray>
#include <QChar>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QHash>
#include <QIODevice>
#include <QLinearGradient>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QRectF>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QWidget>
#include <Qt>
#include <QtAssert>
#include <QtConcurrent/QtConcurrentRun>
#include <QtMath>
#include <QtTypes>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
using Values = QHash<QString, QString>;
QPointer<ChromeThemeWatcher> applicationTheme;

// Omarchy's color files use scalar assignments and section headers. Read just
// that data subset, including inline comments and both TOML quote styles; no
// Lua, template evaluation, or general TOML dependency is involved.
Values readValues(const QString &path) {
  QFile file(path);
  constexpr qint64 limit = 256 * 1024LL;
  if (!file.open(QIODevice::ReadOnly) || file.size() > limit)
    return {};
  const QByteArray bytes = file.read(limit + 1);
  if (bytes.size() > limit)
    return {};
  static const QRegularExpression sectionPattern(
      QStringLiteral(R"(^\[([\w.-]+)\]\s*(?:#.*)?$)"));
  static const QRegularExpression valuePattern(QStringLiteral(
      R"re(^([\w-]+)\s*=\s*(?:"([^"\\]*)"|'([^']*)'|([+-]?[0-9]+(?:\.[0-9]+)?))\s*(?:#.*)?$)re"));
  Values values;
  QString section;
  for (const QString &raw : QString::fromUtf8(bytes).split(QLatin1Char('\n'))) {
    const QString line = raw.trimmed();
    if (line.startsWith(QLatin1Char('['))) {
      const auto match = sectionPattern.match(line);
      section =
          match.hasMatch() ? match.captured(1) : QStringLiteral("invalid");
      continue;
    }
    const auto match = valuePattern.match(line);
    if (!match.hasMatch())
      continue;
    const QString key = section.isEmpty()
                            ? match.captured(1)
                            : section + QLatin1Char('.') + match.captured(1);
    values.insert(key,
                  match.captured(2) + match.captured(3) + match.captured(4));
  }
  return values;
}

QString resolve(QString value, const Values &values) {
  // Named palette colors and shell references (e.g. hyprland.active-border)
  // share a lookup. Bound chains so a malformed reference cycle is harmless.
  for (int depth = 0; depth < 16; ++depth) {
    const auto found = values.constFind(value);
    if (found == values.cend())
      return value;
    value = found.value().trimmed();
  }
  return {};
}

QColor parseColor(QString value) {
  static const QRegularExpression hyprColor(QStringLiteral(
      R"(^(?:rgb\(([0-9a-fA-F]{6})\)|rgba\(([0-9a-fA-F]{8})\))$)"));
  const auto match = hyprColor.match(value);
  if (match.hasMatch())
    value = QLatin1Char('#') + match.captured(1) + match.captured(2);
  // Omarchy uses CSS RRGGBBAA; QColor's eight-digit syntax is AARRGGBB.
  if (value.startsWith(QLatin1Char('#')) && value.size() == 9)
    value = QLatin1Char('#') + value.right(2) + value.mid(1, 6);
  return QColor::fromString(value);
}

QColor color(const Values &values, const QString &key, const QColor &fallback) {
  if (!values.contains(key))
    return fallback;
  const QColor result = parseColor(resolve(key, values));
  return result.isValid() ? result : fallback;
}

qreal alpha(const Values &values, const QString &key, qreal fallback) {
  bool ok = false;
  const qreal result = values.value(key).toDouble(&ok);
  return ok && std::isfinite(result) && result >= 0.0 && result <= 1.0
             ? result
             : fallback;
}

QColor withAlpha(QColor color, qreal opacity) {
  color.setAlphaF(color.alphaF() * static_cast<float>(opacity));
  return color;
}

ChromeThemeBorder border(const Values &values, const QString &key,
                         const ChromeThemeBorder &fallback,
                         qreal opacity = 1.0) {
  const auto faded = [opacity](ChromeThemeBorder border) {
    for (QColor &stop : border.colors)
      stop = withAlpha(stop, opacity);
    return border;
  };
  const auto parts = resolve(key, values)
                         .split(QRegularExpression(QStringLiteral("\\s+")),
                                Qt::SkipEmptyParts);
  ChromeThemeBorder result;
  for (const QString &part : parts) {
    if (part.endsWith(QStringLiteral("deg"))) {
      bool ok = false;
      result.angle = part.chopped(3).toDouble(&ok);
      if (!ok || !std::isfinite(result.angle))
        return faded(fallback);
      result.angle = std::fmod(result.angle, 360.0);
    } else {
      const QColor stop = parseColor(resolve(part, values));
      if (!stop.isValid())
        return faded(fallback);
      result.colors.push_back(stop);
    }
  }
  return faded(result.colors.isEmpty() ? fallback : result);
}

QColor controlFill(const QColor &surface, const QColor &tint, qreal opacity) {
  const qreal tintAlpha = tint.alphaF() * opacity;
  const qreal baseAlpha = surface.alphaF() * (1.0 - tintAlpha);
  const qreal totalAlpha = tintAlpha + baseAlpha;
  if (totalAlpha == 0.0)
    return Qt::transparent;
  return QColor::fromRgbF(
      static_cast<float>(
          (tint.redF() * tintAlpha + surface.redF() * baseAlpha) / totalAlpha),
      static_cast<float>(
          (tint.greenF() * tintAlpha + surface.greenF() * baseAlpha) /
          totalAlpha),
      static_cast<float>(
          (tint.blueF() * tintAlpha + surface.blueF() * baseAlpha) /
          totalAlpha),
      static_cast<float>(totalAlpha));
}

QString cssColor(const QColor &color) {
  return QStringLiteral("rgba(%1,%2,%3,%4)")
      .arg(color.red())
      .arg(color.green())
      .arg(color.blue())
      .arg(color.alpha());
}

qreal luminance(const QColor &color) {
  const auto linear = [](qreal value) {
    return value <= 0.04045 ? value / 12.92
                            : std::pow((value + 0.055) / 1.055, 2.4);
  };
  return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) +
         0.0722 * linear(color.blueF());
}
} // namespace

const ChromeTheme &chromeTheme() {
  static const ChromeTheme fallback;
  return applicationTheme ? applicationTheme->theme() : fallback;
}

QColor chromeAlpha(QColor color, int alpha) {
  return withAlpha(color, std::clamp(alpha, 0, 255) / 255.0);
}

void initializeChromeTheme(const QString &directory) {
  Q_ASSERT(!applicationTheme);
  applicationTheme = new ChromeThemeWatcher(directory, qApp);
  const auto apply = [] {
    qApp->setStyleSheet(chromeTheme().tooltipStyleSheet());
    for (QWidget *widget : QApplication::topLevelWidgets())
      widget->update();
  };
  QObject::connect(applicationTheme.data(), &ChromeThemeWatcher::changed, qApp,
                   apply);
  apply();
}

QBrush ChromeThemeBorder::brush(const QRectF &bounds) const {
  if (colors.isEmpty())
    return Qt::NoBrush;
  if (colors.size() == 1)
    return colors.constFirst();
  const qreal radians = qDegreesToRadians(angle);
  const QPointF direction(std::cos(radians), std::sin(radians));
  const qreal halfLength = (std::abs(bounds.width() * direction.x()) +
                            std::abs(bounds.height() * direction.y())) /
                           2.0;
  QLinearGradient gradient(bounds.center() - direction * halfLength,
                           bounds.center() + direction * halfLength);
  for (qsizetype index = 0; index < colors.size(); ++index)
    gradient.setColorAt(static_cast<qreal>(index) /
                            static_cast<qreal>(colors.size() - 1),
                        colors[index]);
  return gradient;
}

QString ChromeTheme::tooltipStyleSheet() const {
  return QStringLiteral(
             "QToolTip { color: %1; background-color: %2; "
             "border: 1px solid %3; border-radius: 4px; padding: 0; }"
             "QPlainTextEdit { selection-background-color: %4; "
             "selection-color: %5; }")
      .arg(cssColor(tooltipText), cssColor(tooltipBackground),
           cssColor(tooltipBorder), cssColor(accent), cssColor(accentText));
}

QString chromeThemeDirectory() {
  return QDir::home().filePath(
      QStringLiteral(".local/state/omarchy/current/theme"));
}

ChromeTheme loadChromeTheme(const QString &directory) {
  const QDir root(directory);
  Values values = readValues(root.filePath(QStringLiteral("colors.toml")));
  values.insert(readValues(root.filePath(QStringLiteral("shell.toml"))));
  ChromeTheme theme;
  const QColor background =
      color(values, QStringLiteral("popups.background"),
            color(values, QStringLiteral("background"), {}));
  const QColor foreground =
      color(values, QStringLiteral("popups.text"),
            color(values, QStringLiteral("foreground"), {}));
  // Keep the readable neutral pair when there is no usable surface/text pair.
  if (!background.isValid() || !foreground.isValid())
    return theme;
  theme.surface =
      withAlpha(background,
                alpha(values, QStringLiteral("popups.background-alpha"), 1.0));
  theme.foreground = foreground;
  theme.canvasSurface = background;
  theme.canvasSurface.setAlpha(255);
  theme.scrim = theme.canvasSurface;
  theme.muted = color(values, QStringLiteral("light_foreground"), foreground);
  theme.accent = color(values, QStringLiteral("accent"), foreground);
  theme.warning = color(values, QStringLiteral("orange"), theme.accent);
  const auto contrast = [&theme](const QColor &candidate) {
    const qreal a = luminance(theme.accent) + 0.05;
    const qreal b = luminance(candidate) + 0.05;
    return std::max(a, b) / std::min(a, b);
  };
  theme.accentText =
      contrast(background) > contrast(foreground) ? background : foreground;
  theme.buttonText =
      color(values, QStringLiteral("controls.normal-color"), foreground);
  theme.buttonHoverText =
      color(values, QStringLiteral("controls.hover-cursor-color"), foreground);
  theme.buttonSelectedText =
      color(values, QStringLiteral("controls.selected-color"), foreground);
  theme.button = controlFill(
      theme.surface, theme.buttonText,
      alpha(values, QStringLiteral("controls.normal-fill-alpha"), 0.04));
  theme.buttonHover = controlFill(
      theme.surface, theme.buttonHoverText,
      alpha(values, QStringLiteral("controls.hover-cursor-fill-alpha"), 0.08));
  theme.buttonSelected = controlFill(
      theme.surface, theme.buttonSelectedText,
      alpha(values, QStringLiteral("controls.selected-fill-alpha"), 0.18));
  theme.buttonBorder = border(
      values, QStringLiteral("controls.normal-border"), {{theme.buttonText}},
      alpha(values, QStringLiteral("controls.normal-border-alpha"), 0.4));
  theme.buttonHoverBorder =
      border(values, QStringLiteral("controls.hover-cursor-border"),
             {{theme.buttonHoverText}},
             alpha(values, QStringLiteral("controls.hover-cursor-border-alpha"),
                   0.25));
  theme.buttonSelectedBorder = border(
      values, QStringLiteral("controls.selected-border"),
      {{theme.buttonSelectedText}},
      alpha(values, QStringLiteral("controls.selected-border-alpha"), 1.0));
  const ChromeThemeBorder accent{
      {color(values, QStringLiteral("accent"), foreground)}};
  theme.activeBorder = border(
      values, QStringLiteral("popups.border"),
      border(values, QStringLiteral("hyprland.active-border"),
             border(values, QStringLiteral("hyprland_active_border"), accent)),
      alpha(values, QStringLiteral("popups.border-alpha"), 1.0));
  theme.inactiveBorder =
      border(values, QStringLiteral("hyprland_inactive_border"),
             {{chromeAlpha(foreground, 75)}});
  theme.selectionBorder = theme.activeBorder;
  theme.panelBorder = theme.activeBorder;
  theme.tooltipBackground = withAlpha(
      color(values, QStringLiteral("tooltip.background"), background),
      alpha(values, QStringLiteral("tooltip.background-alpha"), 0.97));
  theme.tooltipText = color(values, QStringLiteral("tooltip.text"), foreground);
  theme.tooltipBorder =
      border(values, QStringLiteral("tooltip.border"), {{foreground}},
             alpha(values, QStringLiteral("tooltip.border-alpha"), 1.0))
          .colors.constFirst();
  theme.toastBackground = theme.surface;
  theme.toastText = foreground;
  return theme;
}

ChromeThemeWatcher::ChromeThemeWatcher(const QString &directory,
                                       QObject *parent)
    : QObject(parent), directory_(QDir::cleanPath(directory)) {
  debounce_.setSingleShot(true);
  debounce_.setInterval(50);
  connect(&debounce_, &QTimer::timeout, this, &ChromeThemeWatcher::reload);
  connect(&files_, &QFileSystemWatcher::fileChanged, this,
          [this] { debounce_.start(); });
  connect(&files_, &QFileSystemWatcher::directoryChanged, this, [this] {
    // Omarchy swaps the whole theme directory. Reattach child watches even
    // when the new files have the same paths as the old inodes.
    rearm_ = true;
    debounce_.start();
  });
  connect(&loader_, &QFutureWatcher<Snapshot>::finished, this, [this] {
    loading_ = false;
    const auto result = loader_.result();
    if (std::exchange(rearm_, false)) {
      const auto children = files_.files() + QStringList{directory_};
      for (const auto &path : children) {
        if (files_.files().contains(path) ||
            files_.directories().contains(path))
          files_.removePath(path);
      }
    }
    const auto watched = files_.files() + files_.directories();
    bool added = false;
    for (const auto &path : result.paths) {
      if (!watched.contains(path))
        added = files_.addPath(path) || added;
    }
    for (const auto &path : watched) {
      if (!result.paths.contains(path))
        files_.removePath(path);
    }
    // Read once more after attaching a new watch: a change between the first
    // read and watch registration must not leave a stale palette on screen.
    if (added || std::exchange(pending_, false)) {
      reload();
      return;
    }
    if (theme_ != result.theme) {
      theme_ = result.theme;
      emit changed();
    }
  });
  reload();
}

void ChromeThemeWatcher::reload() {
  if (loading_) {
    pending_ = true;
    return;
  }
  loading_ = true;
  loader_.setFuture(QtConcurrent::run([directory = directory_] {
    Snapshot result{loadChromeTheme(directory), {}};
    const QDir root(directory);
    for (const auto &path :
         {directory, root.filePath(QStringLiteral("colors.toml")),
          root.filePath(QStringLiteral("shell.toml"))}) {
      if (QFileInfo::exists(path))
        result.paths.push_back(path);
    }
    // Keep a parent watch across an atomic directory replacement. If the
    // theme has not been installed yet, watch its nearest existing ancestor.
    QString parent = QFileInfo(directory).absolutePath();
    while (!QFileInfo::exists(parent) &&
           parent != QFileInfo(parent).absolutePath())
      parent = QFileInfo(parent).absolutePath();
    result.paths.push_back(parent);
    return result;
  }));
}
