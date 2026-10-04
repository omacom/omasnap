/** @fileoverview Helpers shared by the upload smoke tests. */
#pragma once

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QtTest>

#include "upload-provider.hpp"
#include "upload-sxcu.hpp"

// How a job ended.
struct UploadOutcome {
  bool done = false;
  bool ok = false;
  QString url;
  QString thumbnailUrl;
  QString deletionUrl;
  QString error;
};

// Runs a job to completion (or 10s), then deletes it.
inline UploadOutcome runJob(upload::Job *job, const QString &path,
                            int timeoutMs = 10000) {
  UploadOutcome outcome;
  if (!job) {
    outcome.error = QStringLiteral("no job");
    return outcome;
  }
  QObject::connect(
      job, &upload::Job::finished, job, [&](const upload::Outcome &result) {
        outcome = {
            true, true, result.url, result.thumbnailUrl, result.deletionUrl,
            {}};
      });
  QObject::connect(job, &upload::Job::failed, job, [&](const QString &message) {
    outcome = {true, false, {}, {}, {}, message};
  });
  job->start(path);
  if (!QTest::qWaitFor([&] { return outcome.done; }, timeoutMs))
    outcome.error = QStringLiteral("timed out");
  delete job;
  return outcome;
}

inline QJsonObject pairsObject(const sxcu::Pairs &pairs) {
  QJsonObject object;
  for (const auto &[key, value] : pairs)
    object.insert(key, value);
  return object;
}

// The .sxcu document for a destination, for handing to the sxcu provider.
inline QString sxcuJson(const sxcu::Destination &d) {
  QJsonObject o{
      {QStringLiteral("Name"), d.name},
      {QStringLiteral("RequestMethod"), d.requestMethod},
      {QStringLiteral("RequestURL"), d.requestUrl},
      {QStringLiteral("Body"), d.body},
      {QStringLiteral("FileFormName"), d.fileFormName},
      {QStringLiteral("Parameters"), pairsObject(d.parameters)},
      {QStringLiteral("Headers"), pairsObject(d.headers)},
      {QStringLiteral("Arguments"), pairsObject(d.arguments)},
      {QStringLiteral("URL"), d.url},
      {QStringLiteral("ThumbnailURL"), d.thumbnailUrl},
      {QStringLiteral("DeletionURL"), d.deletionUrl},
      {QStringLiteral("ErrorMessage"), d.errorMessage},
  };
  return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

// Services good enough for a job that doesn't store or delegate.
inline upload::Services testServices(QNetworkAccessManager *network) {
  upload::Services services;
  services.network = network;
  services.saveSettings = [](const QVariantMap &) {};
  services.openUrl = [](const QUrl &) { return true; };
  return services;
}
