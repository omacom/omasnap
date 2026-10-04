/** @fileoverview AWS Signature Version 4 for S3 and S3-compatible stores. */
#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QPair>
#include <QString>
#include <QUrl>

// AWS Signature Version 4, as S3 and the S3-compatible stores (R2, B2,
// MinIO, Wasabi, …) expect it.
namespace upload::sigv4 {

struct Credentials {
  QString accessKey;
  QString secretKey;
  QString region;
  QString service = QStringLiteral("s3");
};

using Headers = QList<QPair<QByteArray, QByteArray>>;

// The hash S3 wants in x-amz-content-sha256 for this body.
QByteArray payloadHash(const QByteArray &body);
// For requests whose body isn't hashed (signed URLs).
inline QByteArray unsignedPayload() {
  return QByteArrayLiteral("UNSIGNED-PAYLOAD");
}

// RFC 3986 encoding as SigV4 wants it: only unreserved characters stay,
// and "/" stays too unless encodeSlash.
QByteArray uriEncode(const QString &text, bool encodeSlash);

// A URL whose path and query are already in canonical (encoded) form, so
// what's signed is exactly what's sent.
QUrl buildUrl(const QString &scheme, const QString &host, int port,
              const QString &path,
              const QList<QPair<QString, QString>> &query = {});

// The headers to send for method on url: headers plus Host, x-amz-date,
// x-amz-content-sha256 and Authorization. Every given header is signed.
Headers sign(const QByteArray &method, const QUrl &url, Headers headers,
             const QByteArray &payloadHash, const Credentials &credentials,
             const QDateTime &now);

// A link that works without credentials for expiresSeconds (up to 7 days).
QUrl presign(const QByteArray &method, const QUrl &url,
             const Credentials &credentials, const QDateTime &now,
             int expiresSeconds);

// Exposed for tests.
QByteArray canonicalRequest(const QByteArray &method, const QUrl &url,
                            const Headers &headers,
                            const QByteArray &payloadHash,
                            QByteArray *signedHeaders);
QByteArray signature(const QByteArray &canonical,
                     const Credentials &credentials, const QDateTime &now);

} // namespace upload::sigv4
