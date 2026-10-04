/** @fileoverview A canned HTTP server on localhost for upload tests. */
#pragma once

#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QPair>
#include <QTcpServer>
#include <QTcpSocket>

#include <functional>

// Answers every request on localhost with a canned response and keeps what
// was sent, so uploads can be checked without touching a real host.
class FakeHttpServer : public QObject {
  Q_OBJECT

public:
  struct Request {
    QByteArray method;
    QByteArray target;
    QHash<QByteArray, QByteArray> headers;
    QByteArray body;
  };

  struct Response {
    int status = 200;
    QByteArray body;
    QList<QPair<QByteArray, QByteArray>> headers;
  };

  int status = 200;
  QByteArray reply;
  QList<QPair<QByteArray, QByteArray>> replyHeaders;
  // When set, decides each response instead of the canned one above.
  std::function<Response(const Request &)> responder;
  QList<Request> requests;

  FakeHttpServer() {
    connect(&m_server, &QTcpServer::newConnection, this, [this] {
      while (QTcpSocket *socket = m_server.nextPendingConnection()) {
        connect(socket, &QTcpSocket::readyRead, this,
                [this, socket] { read(socket); });
        connect(socket, &QTcpSocket::disconnected, socket,
                &QObject::deleteLater);
      }
    });
  }

  bool listen() { return m_server.listen(QHostAddress::LocalHost); }
  QString url(const QString &path = QStringLiteral("/upload")) const {
    return QStringLiteral("http://127.0.0.1:%1%2")
        .arg(m_server.serverPort())
        .arg(path);
  }

private:
  void read(QTcpSocket *socket) {
    QByteArray &buffer = m_buffers[socket];
    buffer += socket->readAll();
    const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
    if (headerEnd < 0)
      return;

    Request request;
    const QList<QByteArray> lines = buffer.left(headerEnd).split('\n');
    const QList<QByteArray> requestLine = lines.first().trimmed().split(' ');
    request.method = requestLine.value(0);
    request.target = requestLine.value(1);
    for (qsizetype i = 1; i < lines.size(); ++i) {
      const qsizetype colon = lines[i].indexOf(':');
      if (colon > 0)
        request.headers.insert(lines[i].left(colon).trimmed().toLower(),
                               lines[i].mid(colon + 1).trimmed());
    }
    const qsizetype length =
        request.headers.value("content-length").toLongLong();
    if (buffer.size() < headerEnd + 4 + length)
      return;
    request.body = buffer.mid(headerEnd + 4, length);
    m_buffers.remove(socket);
    requests << request;

    const Response answer =
        responder ? responder(request) : Response{status, reply, replyHeaders};
    QByteArray response =
        "HTTP/1.1 " + QByteArray::number(answer.status) + " Whatever\r\n";
    for (const auto &[name, value] : answer.headers)
      response += name + ": " + value + "\r\n";
    response += "Content-Length: " + QByteArray::number(answer.body.size()) +
                "\r\nConnection: close\r\n\r\n" + answer.body;
    socket->write(response);
    socket->disconnectFromHost();
  }

  QTcpServer m_server;
  QHash<QTcpSocket *, QByteArray> m_buffers;
};
