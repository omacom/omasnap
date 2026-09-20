#include <QCommandLineParser>
#include <QCommandLineOption>
/** @fileoverview Resolves local image targets accepted by the command line. */
#include "cli-path.hpp"

#include <QCommandLineParser>
#include <QCommandLineOption>

#include <QFileInfo>
#include <QUrl>

QString resolveLocalImagePath(const QString &target) {
  if (target.isEmpty())
    return {};

  const QUrl url(target);
  QString path;
  if (url.isLocalFile())
    path = url.toLocalFile();
  else if (!url.scheme().isEmpty() &&
           target.startsWith(url.scheme() + QStringLiteral("://"),
                             Qt::CaseInsensitive))
    return {};
  else
    path = target;

  const QFileInfo file(path);
  return file.isFile() ? file.absoluteFilePath() : QString{};
}

void configureCaptureCommandLine(QCommandLineParser &parser, bool beforeQt) {
  // QApplication consumes these before the normal parse. Recognize them in
  // the early shell-role parse too, without exposing or applying them here.
  if (beforeQt) {
    parser.setSingleDashWordOptionMode(QCommandLineParser::ParseAsLongOptions);
    for (const char *name : {"platform", "platformpluginpath", "platformtheme",
                             "plugin", "qmljsdebugger", "qwindowgeometry",
                             "qwindowicon", "qwindowtitle", "session",
                             "style", "stylesheet"}) {
      QCommandLineOption option(QString::fromLatin1(name), QString(),
                                 QStringLiteral("value"));
      option.setFlags(QCommandLineOption::HiddenFromHelp);
      parser.addOption(option);
    }
    for (const char *name : {"reverse", "widgetcount"}) {
      QCommandLineOption option(QString::fromLatin1(name));
      option.setFlags(QCommandLineOption::HiddenFromHelp);
      parser.addOption(option);
    }
  }
  parser.setApplicationDescription(QStringLiteral(
      "Native Wayland screenshot and annotation overlay for Hyprland and "
      "Omarchy.\n"
      "\n"
      "With no target, drag for a region, click a window, or click open "
      "space for\nthe full focused monitor. Captures copy immediately and show a preview for\n"
      "10 seconds; use its pin button or Ctrl+P to keep it, or Edit to annotate.\n"
      "Use --editor to edit before output.\n"
      "\n"
      "Only one capture overlay runs at a time. Starting omasnap again while "
      "an\noverlay is open dismisses it: the running instance is asked to "
      "quit and the\nnew process exits without capturing, so the same hotkey "
      "opens and closes the\noverlay. Quick output (--copy, --save) dismisses "
      "it the same way instead of\nscreenshotting the overlay. With --file (or "
      "an image path) or --clipboard, the running\ninstance is stopped and "
      "the editor opens on that image instead.\n"
      "\n"
      "Exit codes: 0 success, including dismissing a running overlay; 1 "
      "capture,\nimage, or single-instance lock failure; 2 usage error."));
  parser.addHelpOption();
  parser.addVersionOption();
  const QCommandLineOption fullscreenOption(
      QStringLiteral("capture-fullscreen"),
      QStringLiteral("Start with the entire focused monitor selected."));
  const QCommandLineOption windowOption(
      {QStringLiteral("capture-window"), QStringLiteral("capture-windows")},
      QStringLiteral("Start in window selection mode."));
  const QCommandLineOption regionOption(
      QStringLiteral("capture-region"),
      QStringLiteral("Start in freeform region selection mode."));
  parser.addOption(fullscreenOption);
  parser.addOption(windowOption);
  parser.addOption(regionOption);
  const QCommandLineOption copyOption(
      QStringLiteral("copy"),
      QStringLiteral("Copy the capture without opening an editor or pin."));
  const QCommandLineOption saveOption(
      QStringLiteral("save"),
      QStringLiteral("Save the capture without opening an editor or pin."));
  parser.addOption(copyOption);
  parser.addOption(saveOption);
  const QCommandLineOption fileOption(
      QStringLiteral("file"),
      QStringLiteral("Open an existing image file in the annotation editor "
                     "instead of capturing the screen."),
      QStringLiteral("path"));
  parser.addOption(fileOption);
  const QCommandLineOption clipboardOption(
      QStringLiteral("clipboard"),
      QStringLiteral("Open the current clipboard image in the annotation "
                     "editor instead of capturing the screen."));
  parser.addOption(clipboardOption);
  const QCommandLineOption pinOption(
      QStringLiteral("pin"),
      QStringLiteral("Show an image as a floating window pinned on every workspace."),
      QStringLiteral("path"));
  parser.addOption(pinOption);
  QCommandLineOption previewOption(
      QStringLiteral("preview"), QString(), QStringLiteral("path"));
  previewOption.setFlags(QCommandLineOption::HiddenFromHelp);
  parser.addOption(previewOption);
  const QCommandLineOption editorOption(
      QStringLiteral("editor"),
      QStringLiteral("Edit before output, using overlay (fullscreen) or "
                     "window (a normal compositor window). Also configurable "
                     "as [editor] mode in omasnap.conf; W switches a live "
                     "editor between the two."),
      QStringLiteral("mode"));
  parser.addOption(editorOption);
  QCommandLineOption handoffMonitor(QStringLiteral("handoff-monitor"), QString(),
                                     QStringLiteral("name"));
  handoffMonitor.setFlags(QCommandLineOption::HiddenFromHelp);
  parser.addOption(handoffMonitor);
  QCommandLineOption handoffToken(QStringLiteral("handoff-token"), QString(),
                                  QStringLiteral("token"));
  handoffToken.setFlags(QCommandLineOption::HiddenFromHelp);
  parser.addOption(handoffToken);
  QCommandLineOption pinDocument(QStringLiteral("pin-document"), QString(),
                                 QStringLiteral("path"));
  pinDocument.setFlags(QCommandLineOption::HiddenFromHelp);
  parser.addOption(pinDocument);
  const QCommandLineOption scrollOption(
      QStringLiteral("scroll"),
      QStringLiteral("Capture a scrolling region and stitch it into one tall "
                     "image, then copy it and show a timed preview."));
  parser.addOption(scrollOption);
  parser.addPositionalArgument(
      QStringLiteral("target"),
      QStringLiteral("Capture mode (smart, region, windows, fullscreen, scroll) or the "
                     "path of an image file to edit."),
      QStringLiteral("[target]"));
}

bool windowedEditorRequested(const QCommandLineParser &parser, bool defaultWindow) {
  const QString mode = parser.value(QStringLiteral("editor")).trimmed().toLower();
  const bool window = mode.isEmpty() ? defaultWindow : mode == QStringLiteral("window");
  if (!window || parser.isSet(QStringLiteral("pin")) ||
      parser.isSet(QStringLiteral("preview")))
    return false;
  if (!parser.value(QStringLiteral("file")).isEmpty() ||
      parser.isSet(QStringLiteral("clipboard")))
    return true;
  const QStringList positional = parser.positionalArguments();
  return positional.size() == 1 && !resolveLocalImagePath(positional.first()).isEmpty();
}
