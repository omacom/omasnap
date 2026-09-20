/** @fileoverview Owns locks for pinned snapshot files. */
#pragma once

#include <QString>
#include <memory>

/** Tracks a pinned snapshot while its window is open. */
class PinSnapshotFile {
public:
  explicit PinSnapshotFile(QString path);
  ~PinSnapshotFile();

  PinSnapshotFile(const PinSnapshotFile &) = delete;
  PinSnapshotFile &operator=(const PinSnapshotFile &) = delete;

  [[nodiscard]] bool isLocked() const;
  [[nodiscard]] QString path() const { return path_; }
  [[nodiscard]] QString previewPath() const { return path_ + QStringLiteral(".preview.png"); }
  [[nodiscard]] static bool isOwnedPath(const QString &path);
  void preserveForEditor();

private:
  QString path_;
  int fd_ = -1;
  bool preserve_ = false;
};

/** Private, editable copy of a pin. Call on a worker; user files stay untouched. */
[[nodiscard]] std::shared_ptr<PinSnapshotFile>
copyPinDocument(const QString &path, QString &error);
