/** @fileoverview Anonymous Imgur uploads with the user's client ID. */
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include "upload-http.hpp"
#include "upload-provider.hpp"

namespace upload {

namespace {

const QString kImgurApi = QStringLiteral("https://api.imgur.com");

class ImgurJob : public HttpJob {
public:
  ImgurJob(const QVariantMap &settings, const Services &services,
           QObject *parent)
      : HttpJob(settings.value(QStringLiteral("name")).toString(), services,
                parent),
        api_(settings.value(QStringLiteral("apiUrl"), kImgurApi).toString()),
        clientId_(
            settings.value(QStringLiteral("clientId")).toString().trimmed()) {}

  void start(const QString &filePath) override {
    QIODevice *file = openFile(filePath);
    if (!file)
      return;
    const QString mimeType = mimeTypeFor(filePath);
    // Imgur takes videos in "video" and pictures in "image".
    const QString field = mimeType.startsWith(QStringLiteral("video/"))
                              ? QStringLiteral("video")
                              : QStringLiteral("image");

    QNetworkRequest request =
        newRequest(QUrl(api_ + QStringLiteral("/3/upload")));
    request.setRawHeader("Authorization", "Client-ID " + clientId_.toUtf8());
    QHttpMultiPart *body =
        formData({{QStringLiteral("type"), QStringLiteral("file")}}, field,
                 QFileInfo(filePath).fileName(), file, mimeType);
    send(
        request, "POST", body,
        [this](const Reply &reply) {
          const QJsonObject data = QJsonDocument::fromJson(reply.body)
                                       .object()
                                       .value(QStringLiteral("data"))
                                       .toObject();
          const QString link = data.value(QStringLiteral("link")).toString();
          if (!reply.ok() || !isWebLink(link)) {
            // Imgur puts its reason in data.error, as a string or an object.
            const QJsonValue error = data.value(QStringLiteral("error"));
            fail(requestError(reply, error.isObject()
                                         ? error.toObject()
                                               .value(QStringLiteral("message"))
                                               .toString()
                                         : error.toString()));
            return;
          }
          const QString deleteHash =
              data.value(QStringLiteral("deletehash")).toString();
          succeed(
              {link,
               {},
               deleteHash.isEmpty()
                   ? QString()
                   : QStringLiteral("https://imgur.com/delete/") + deleteHash});
        },
        [this](qint64 sent, qint64 total) { emit progress(sent, total); });
  }

private:
  const QString api_;
  const QString clientId_;
};

class ImgurProvider : public Provider {
public:
  QString id() const override { return QStringLiteral("imgur"); }
  QString name() const override { return QStringLiteral("Imgur"); }
  QString description() const override {
    return QStringLiteral("Anonymous Imgur uploads with your own client ID.");
  }

  QList<Field> fields() const override {
    Field clientId;
    clientId.key = QStringLiteral("clientId");
    clientId.label = QStringLiteral("Client ID");
    clientId.required = true;
    clientId.help = QStringLiteral(
        "Register an application at api.imgur.com/oauth2/addclient "
        "(anonymous usage) to get one.");

    Field api;
    api.key = QStringLiteral("apiUrl");
    api.label = QStringLiteral("API");
    api.defaultValue = kImgurApi;
    api.hidden = true;
    return {clientId, api};
  }

  Job *createJob(const QVariantMap &settings, const Services &services,
                 QObject *parent) const override {
    return new ImgurJob(settings, services, parent);
  }
};

} // namespace

const Provider *imgurProvider() {
  static const ImgurProvider provider;
  return &provider;
}

} // namespace upload
