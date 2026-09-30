#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>

// Called on the output worker. Paths survive previews and clipboard replacement.
QString retainClipboardImage(const QByteArray &png, QString &error);
QMap<QByteArray, QByteArray> clipboardImagePayloads(const QString &path,
                                                  const QByteArray &png);

// Private subprocess mode of the same binary, before QApplication/instance lock.
// Claims the selection, detaches, and exits when another copy replaces it.
int runClipboardImageOwner(const QString &path);
