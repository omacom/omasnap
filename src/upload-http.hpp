/** @fileoverview A job made of chained HTTP requests, plus helpers shared by
 * providers. */
#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QNetworkRequest>
#include <QPair>
#include <QPointer>
#include <QUrl>

#include <functional>

#include "upload-provider.hpp"

class QHttpMultiPart;
class QIODevice;
class QNetworkReply;

namespace upload {

// What came back from one request.
struct Reply {
  int status = 0;
  QByteArray body;
  QUrl url;
  // Keyed by lower-cased header name.
  QHash<QByteArray, QByteArray> headers;
  // Set when there was no HTTP answer at all (DNS, TLS, refused, stalled).
  QString networkError;

  bool ok() const { return status >= 200 && status < 300; }
  QByteArray header(const QByteArray &name) const {
    return headers.value(name.toLower());
  }
};

// A single line of a response body, for error messages.
QString snippet(const QByteArray &body, int length = 200);
bool isWebLink(const QString &text);

// "cloud.example.com/" -> "https://cloud.example.com": a scheme when missing,
// no trailing slash, query or fragment.
QString normalizeServerUrl(const QString &url);
// %y, %mo, %d, %h, %mi and %s become the year, month, day, hour, minute and
// second of now.
QString expandDateTokens(QString text, const QDateTime &now);
// "clip_trimmed.mp4" -> "clip_trimmed-k3x9qa.mp4", so repeat uploads of a
// same-named trim never overwrite each other.
QString taggedFileName(const QString &fileName);
QString mimeTypeFor(const QString &path);

// A multipart/form-data body: fields first, then the file (taken over).
QHttpMultiPart *formData(const QList<QPair<QString, QString>> &fields,
                         const QString &fileField, const QString &fileName,
                         QIODevice *file, const QString &mimeType);

// A job made of HTTP requests run one after another. Subclasses chain them
// with send(), and end with succeed() or fail(); cancelling, stalls and the
// one-outcome rule are handled here.
class HttpJob : public Job {
  Q_OBJECT

public:
  HttpJob(const QString &hostName, const Services &services, QObject *parent);
  ~HttpJob() override;

  void cancel() override;

protected:
  using Handler = std::function<void(const Reply &)>;
  using Progress = std::function<void(qint64 sent, qint64 total)>;

  // Sends one request. body may be a QByteArray, a QIODevice (taken over)
  // or a QHttpMultiPart (taken over); onProgress, when given, reports the
  // upload side of this request.
  void send(QNetworkRequest request, const QByteArray &method,
            const QByteArray &body, Handler handler, Progress onProgress = {});
  void send(QNetworkRequest request, const QByteArray &method, QIODevice *body,
            Handler handler, Progress onProgress = {});
  void send(QNetworkRequest request, const QByteArray &method,
            QHttpMultiPart *body, Handler handler, Progress onProgress = {});

  // "<host> answered HTTP 404: <why>", with why from the body if not given.
  QString httpError(const Reply &reply, const QString &why = QString()) const;
  // httpError, or the network error when there was no answer.
  QString requestError(const Reply &reply,
                       const QString &why = QString()) const;

  void succeed(const Outcome &outcome);
  void fail(const QString &message);
  bool isDone() const { return done_; }
  // Runs once when cancel() stops a job that's still going (e.g. to abort
  // a multipart upload on the server).
  virtual void onCancelled() {}

  QNetworkRequest newRequest(const QUrl &url) const;
  // Opens the file to upload; on failure it has already failed the job.
  QIODevice *openFile(const QString &path);

  const QString hostName_;
  Services services_;

private:
  void track(QNetworkReply *reply, Handler handler, Progress onProgress);

  QPointer<QNetworkReply> reply_;
  bool done_ = false;
  bool cancelled_ = false;
};

} // namespace upload
