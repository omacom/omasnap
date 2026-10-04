/** @fileoverview Amazon S3 and S3-compatible storage (R2, B2, Wasabi, MinIO).
 */
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QNetworkReply>
#include <QXmlStreamReader>

#include "upload-http.hpp"
#include "upload-provider.hpp"
#include "upload-sigv4.hpp"

namespace upload {

namespace {

constexpr qint64 kDefaultMultipartThreshold = 50LL * 1024 * 1024;
constexpr qint64 kPartSize = 10LL * 1024 * 1024;
constexpr int kSignedLinkSeconds = 7 * 24 * 60 * 60;
const QString kAwsEndpoint = QStringLiteral("s3.amazonaws.com");
const QString kSignedLink = QStringLiteral("Signed link (7 days)");

QString text(const QVariantMap &settings, const char *key) {
  return settings.value(QString::fromLatin1(key)).toString().trimmed();
}

// The first <tag> in an S3 XML body.
QString xmlValue(const QByteArray &xml, const QString &tag) {
  QXmlStreamReader reader(xml);
  while (!reader.atEnd()) {
    if (reader.readNext() == QXmlStreamReader::StartElement &&
        reader.name() == tag)
      return reader.readElementText().trimmed();
  }
  return {};
}

class S3Job : public HttpJob {
public:
  S3Job(const QVariantMap &settings, const Services &services, QObject *parent)
      : HttpJob(settings.value(QStringLiteral("name")).toString(), services,
                parent),
        settings_(settings) {
    credentials_.accessKey = text(settings, "accessKeyId");
    credentials_.secretKey =
        settings.value(QStringLiteral("secretAccessKey")).toString();
    credentials_.region = text(settings, "region");
    if (credentials_.region.isEmpty())
      credentials_.region = QStringLiteral("us-east-1");

    // The endpoint may carry a scheme and port (e.g. MinIO on
    // http://localhost:9000); AWS regions outside us-east-1 get their own.
    QString endpoint = text(settings, "endpoint");
    if (endpoint.isEmpty())
      endpoint = kAwsEndpoint;
    const QUrl parsed = QUrl::fromUserInput(endpoint);
    scheme_ = endpoint.contains(QStringLiteral("://"))
                  ? parsed.scheme()
                  : QStringLiteral("https");
    host_ = parsed.host();
    port_ = parsed.port();
    if (host_ == kAwsEndpoint &&
        credentials_.region != QStringLiteral("us-east-1"))
      host_ = QStringLiteral("s3.%1.amazonaws.com").arg(credentials_.region);

    bucket_ = text(settings, "bucket");
    pathStyle_ = settings.value(QStringLiteral("pathStyle")).toBool();
    threshold_ =
        settings.value(QStringLiteral("multipartThreshold")).toLongLong();
    if (threshold_ <= 0)
      threshold_ = kDefaultMultipartThreshold;
    partSize_ = settings.value(QStringLiteral("partSize")).toLongLong();
    if (partSize_ <= 0)
      partSize_ = kPartSize;
  }

  void start(const QString &filePath) override {
    file_ = openFile(filePath);
    if (!file_)
      return;
    contentType_ = mimeTypeFor(filePath).toLatin1();

    const QDateTime now = QDateTime::currentDateTime();
    QString prefix = expandDateTokens(text(settings_, "objectPrefix"), now);
    while (prefix.startsWith(QLatin1Char('/')))
      prefix.remove(0, 1);
    if (!prefix.isEmpty() && !prefix.endsWith(QLatin1Char('/')))
      prefix += QLatin1Char('/');
    const QString fileName = QFileInfo(filePath).fileName();
    key_ =
        prefix + (settings_.value(QStringLiteral("uniqueNames"), true).toBool()
                      ? taggedFileName(fileName)
                      : fileName);

    size_ = file_->size();
    if (size_ > threshold_)
      startMultipart();
    else
      putObject();
  }

protected:
  void onCancelled() override {
    // Leave nothing half-uploaded (and billed) behind. Fire and forget:
    // the job itself is finishing as cancelled either way.
    if (uploadId_.isEmpty())
      return;
    const QUrl url = objectUrl({{QStringLiteral("uploadId"), uploadId_}});
    QNetworkRequest request =
        signedRequest("DELETE", url, {}, sigv4::payloadHash({}));
    QNetworkReply *reply =
        services_.network->sendCustomRequest(request, "DELETE");
    QObject::connect(reply, &QNetworkReply::finished, reply,
                     &QObject::deleteLater);
  }

private:
  QString objectPath() const {
    return pathStyle_ ? QLatin1Char('/') + bucket_ + QLatin1Char('/') + key_
                      : QLatin1Char('/') + key_;
  }
  QString objectHost() const {
    return pathStyle_ ? host_ : bucket_ + QLatin1Char('.') + host_;
  }
  QUrl objectUrl(const QList<QPair<QString, QString>> &query = {}) const {
    return sigv4::buildUrl(scheme_, objectHost(), port_, objectPath(), query);
  }

  QNetworkRequest signedRequest(const QByteArray &method, const QUrl &url,
                                const sigv4::Headers &headers,
                                const QByteArray &hash) const {
    QNetworkRequest request = newRequest(url);
    const sigv4::Headers all =
        sigv4::sign(method, url, headers, hash, credentials_,
                    QDateTime::currentDateTimeUtc());
    for (const auto &[name, value] : all) {
      if (name != "host")
        request.setRawHeader(name, value);
    }
    return request;
  }

  // Content type, ACL and storage class, sent with the object's creation.
  sigv4::Headers objectHeaders() const {
    sigv4::Headers headers{{"content-type", contentType_}};
    if (settings_.value(QStringLiteral("publicAcl")).toBool())
      headers.append({QByteArray("x-amz-acl"), QByteArray("public-read")});
    const QString storageClass = text(settings_, "storageClass");
    if (!storageClass.isEmpty() && storageClass != QStringLiteral("STANDARD"))
      headers.append(
          {QByteArray("x-amz-storage-class"), storageClass.toLatin1()});
    return headers;
  }

  QString s3Error(const Reply &reply) const {
    const QString message = xmlValue(reply.body, QStringLiteral("Message"));
    const QString code = xmlValue(reply.body, QStringLiteral("Code"));
    if (reply.status == 0)
      return requestError(reply);
    return httpError(reply, message.isEmpty() ? code : message);
  }

  void putObject() {
    const QByteArray body = file_->readAll();
    const QUrl url = objectUrl();
    QNetworkRequest request =
        signedRequest("PUT", url, objectHeaders(), sigv4::payloadHash(body));
    send(
        request, "PUT", body,
        [this](const Reply &reply) {
          if (!reply.ok()) {
            fail(s3Error(reply));
            return;
          }
          finish();
        },
        [this](qint64 sent, qint64 total) { emit progress(sent, total); });
  }

  void startMultipart() {
    const QUrl url = objectUrl({{QStringLiteral("uploads"), QString()}});
    QNetworkRequest request =
        signedRequest("POST", url, objectHeaders(), sigv4::payloadHash({}));
    send(request, "POST", QByteArray(), [this](const Reply &reply) {
      uploadId_ = reply.ok() ? xmlValue(reply.body, QStringLiteral("UploadId"))
                             : QString();
      if (uploadId_.isEmpty()) {
        fail(reply.ok()
                 ? QStringLiteral("%1 didn't start the upload.").arg(hostName_)
                 : s3Error(reply));
        return;
      }
      uploadPart();
    });
  }

  void uploadPart() {
    const qint64 offset = qint64(etags_.size()) * partSize_;
    if (offset >= size_) {
      completeMultipart();
      return;
    }
    const int partNumber = int(etags_.size()) + 1;
    const QByteArray part = file_->read(partSize_);
    const QUrl url =
        objectUrl({{QStringLiteral("partNumber"), QString::number(partNumber)},
                   {QStringLiteral("uploadId"), uploadId_}});
    QNetworkRequest request =
        signedRequest("PUT", url, {}, sigv4::payloadHash(part));
    const qint64 partLength = part.size();
    send(
        request, "PUT", part,
        [this, offset, partLength](const Reply &reply) {
          const QByteArray etag = reply.header("etag");
          if (!reply.ok() || etag.isEmpty()) {
            abortAndFail(
                reply.ok()
                    ? QStringLiteral("%1 didn't confirm a part.").arg(hostName_)
                    : s3Error(reply));
            return;
          }
          etags_ << etag;
          emit progress(offset + partLength, size_);
          uploadPart();
        },
        [this, offset](qint64 sent, qint64 total) {
          // Qt closes each request with a (0, 0) report; skip it.
          if (total > 0)
            emit progress(offset + sent, size_);
        });
  }

  void completeMultipart() {
    QByteArray xml = "<CompleteMultipartUpload>";
    for (qsizetype i = 0; i < etags_.size(); ++i) {
      xml += "<Part><PartNumber>" + QByteArray::number(i + 1) +
             "</PartNumber><ETag>" +
             QString::fromLatin1(etags_.at(i)).toHtmlEscaped().toLatin1() +
             "</ETag></Part>";
    }
    xml += "</CompleteMultipartUpload>";

    const QUrl url = objectUrl({{QStringLiteral("uploadId"), uploadId_}});
    QNetworkRequest request =
        signedRequest("POST", url, {{"content-type", "application/xml"}},
                      sigv4::payloadHash(xml));
    send(request, "POST", xml, [this](const Reply &reply) {
      // S3 can answer 200 and still report an error in the body.
      if (!reply.ok() || reply.body.contains("<Error>")) {
        abortAndFail(s3Error(reply));
        return;
      }
      uploadId_.clear();
      finish();
    });
  }

  void abortAndFail(const QString &message) {
    onCancelled();
    uploadId_.clear();
    fail(message);
  }

  void finish() {
    QString url;
    const QString customDomain = text(settings_, "customDomain");
    if (text(settings_, "linkType") == kSignedLink) {
      url = sigv4::presign("GET", objectUrl(), credentials_,
                           QDateTime::currentDateTimeUtc(), kSignedLinkSeconds)
                .toString(QUrl::FullyEncoded);
    } else if (!customDomain.isEmpty()) {
      QString base = customDomain.contains(QStringLiteral("://"))
                         ? customDomain
                         : QStringLiteral("https://") + customDomain;
      while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
      url = base + QString::fromLatin1(
                       sigv4::uriEncode(QLatin1Char('/') + key_, false));
    } else {
      url = objectUrl().toString(QUrl::FullyEncoded);
    }
    succeed({url, {}, {}});
  }

  const QVariantMap settings_;
  sigv4::Credentials credentials_;
  QString scheme_;
  QString host_;
  int port_ = -1;
  QString bucket_;
  bool pathStyle_ = false;
  qint64 threshold_ = kDefaultMultipartThreshold;
  qint64 partSize_ = kPartSize;

  QIODevice *file_ = nullptr;
  QByteArray contentType_;
  QString key_;
  qint64 size_ = 0;
  QString uploadId_;
  QList<QByteArray> etags_;
};

class S3Provider : public Provider {
public:
  QString id() const override { return QStringLiteral("s3"); }
  QString name() const override {
    return QStringLiteral("Amazon S3 or compatible");
  }
  QString description() const override {
    return QStringLiteral(
        "S3, Cloudflare R2, Backblaze B2, Wasabi, MinIO and other S3 storage.");
  }

  QList<Field> fields() const override {
    QList<Field> fields;
    Field f;

    f = {};
    f.key = QStringLiteral("endpoint");
    f.label = QStringLiteral("Endpoint");
    f.defaultValue = kAwsEndpoint;
    f.help = QStringLiteral(
        "For other stores use theirs, e.g. <account>.r2.cloudflarestorage.com "
        "or http://localhost:9000.");
    fields << f;

    f = {};
    f.key = QStringLiteral("region");
    f.label = QStringLiteral("Region");
    f.defaultValue = QStringLiteral("us-east-1");
    f.help = QStringLiteral("R2 uses \"auto\".");
    fields << f;

    f = {};
    f.key = QStringLiteral("bucket");
    f.label = QStringLiteral("Bucket");
    f.required = true;
    fields << f;

    f = {};
    f.key = QStringLiteral("accessKeyId");
    f.label = QStringLiteral("Access key ID");
    f.required = true;
    fields << f;

    f = {};
    f.key = QStringLiteral("secretAccessKey");
    f.label = QStringLiteral("Secret access key");
    f.type = Field::Secret;
    f.required = true;
    fields << f;

    f = {};
    f.key = QStringLiteral("objectPrefix");
    f.label = QStringLiteral("Folder");
    f.defaultValue = QStringLiteral("omasnap/%y/%mo");
    f.help = QStringLiteral("%y, %mo and %d become the year, month and day.");
    fields << f;

    f = {};
    f.key = QStringLiteral("uniqueNames");
    f.label = QStringLiteral("Add a random tag to file names");
    f.type = Field::Toggle;
    f.defaultValue = true;
    fields << f;

    f = {};
    f.key = QStringLiteral("linkType");
    f.label = QStringLiteral("Link");
    f.type = Field::Choice;
    f.choices = {QStringLiteral("Public URL"), kSignedLink};
    f.defaultValue = QStringLiteral("Public URL");
    f.help =
        QStringLiteral("A signed link works for private buckets but expires.");
    fields << f;

    f = {};
    f.key = QStringLiteral("customDomain");
    f.label = QStringLiteral("Custom domain");
    f.placeholder = QStringLiteral("https://cdn.example.com");
    f.help = QStringLiteral(
        "Public links use this instead of the bucket's address.");
    fields << f;

    f = {};
    f.key = QStringLiteral("publicAcl");
    f.label = QStringLiteral("Make uploads public (public-read ACL)");
    f.type = Field::Toggle;
    f.defaultValue = false;
    fields << f;

    f = {};
    f.key = QStringLiteral("pathStyle");
    f.label = QStringLiteral("Path-style addressing");
    f.type = Field::Toggle;
    f.defaultValue = false;
    f.help =
        QStringLiteral("Needed by MinIO and some other self-hosted stores.");
    fields << f;

    f = {};
    f.key = QStringLiteral("storageClass");
    f.label = QStringLiteral("Storage class");
    f.type = Field::Choice;
    f.choices = {
        QStringLiteral("STANDARD"),    QStringLiteral("INTELLIGENT_TIERING"),
        QStringLiteral("STANDARD_IA"), QStringLiteral("ONEZONE_IA"),
        QStringLiteral("GLACIER_IR"),  QStringLiteral("REDUCED_REDUNDANCY")};
    f.defaultValue = QStringLiteral("STANDARD");
    fields << f;

    f = {};
    f.key = QStringLiteral("multipartThreshold");
    f.label = QStringLiteral("Multipart threshold");
    f.type = Field::Number;
    f.defaultValue = kDefaultMultipartThreshold;
    f.hidden = true;
    fields << f;

    f = {};
    f.key = QStringLiteral("partSize");
    f.label = QStringLiteral("Part size");
    f.type = Field::Number;
    f.defaultValue = kPartSize;
    f.hidden = true;
    fields << f;

    return fields;
  }

  Job *createJob(const QVariantMap &settings, const Services &services,
                 QObject *parent) const override {
    return new S3Job(settings, services, parent);
  }
};

} // namespace

const Provider *s3Provider() {
  static const S3Provider provider;
  return &provider;
}

} // namespace upload
