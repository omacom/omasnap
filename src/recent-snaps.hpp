/** @fileoverview The short shelf of earlier captures the select overlay offers
 *  to reopen. Each entry keeps the editor's working document (full source plus
 *  operation log) so a reopened capture keeps its layers editable, and a small
 *  pre-rendered thumbnail so listing them costs nothing at startup. */
#pragma once

#include <QImage>
#include <QString>
#include <QVector>
#include <memory>
#include <optional>

struct OperationLog;
class QLockFile;

struct RecentSnap {
  /// Full-resolution source the editor reopens.
  QString sourcePath;
  /// Operation log sidecar; empty when the entry has none.
  QString logPath;
  QString thumbPath;
  /// When it was taken, ms since the epoch; 0 when unknown.
  qint64 stampMs = 0;
  /// Flattened preview, longest edge `kRecentThumbEdge`. Null when not loaded.
  QImage thumbnail;
};

/// How many captures the shelf keeps; older ones are pruned on record.
constexpr int kRecentSnapLimit = 5;
/// Longest edge of a stored thumbnail, in pixels.
constexpr int kRecentThumbEdge = 320;

/// `$OMASNAP_RECENT_DIR`, else `$XDG_STATE_HOME/omasnap/recent`. Created on
/// demand; empty when it cannot be.
[[nodiscard]] QString recentSnapsDirectory();

/// Newest first, at most `kRecentSnapLimit`. Thumbnails are decoded when
/// `loadThumbnails` is set. Worker-only: waits for captures still being saved.
[[nodiscard]] QVector<RecentSnap> listRecentSnaps(bool loadThumbnails = true);

/// Reserve a capture before showing its preview. The output worker can then
/// report completion and save history afterward; readers wait for this capture
/// instead of reopening a flattened preview while its source is still encoding.
/// Keep the reservation alive until record() and any replacement cleanup finish.
class RecentSnapWriter final {
public:
  explicit RecentSnapWriter(QString recentId);
  ~RecentSnapWriter();
  /// Publish before removing replaced, then prune while holding the shelf lock.
  [[nodiscard]] bool record(const QImage &source, const OperationLog &log,
                            const QImage &rendered, QString &error,
                            const RecentSnap *replaced = nullptr);

private:
  QString recentId_;
  QString root_;
  QString stem_;
  QString error_;
  std::unique_ptr<QLockFile> pending_;
};

/// Saves a working document and thumbnail on a worker. Replaces an entry with
/// the same log.recentId and prunes beyond the limit; never consumes live files.
[[nodiscard]] bool recordRecentSnap(const QImage &source,
                                    const OperationLog &log,
                                    const QImage &rendered, QString &error);

/// Worker-only. Waits for this capture's pending write, if any. A timeout is an
/// error, not permission to replace its editable layers with flattened pixels.
[[nodiscard]] std::optional<RecentSnap> findRecentSnap(const QString &recentId,
                                                      QString *error = nullptr);

/// Deletes one entry's files.
void removeRecentSnap(const RecentSnap &snap);
