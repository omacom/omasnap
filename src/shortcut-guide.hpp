#pragma once

#include <QPair>
#include <QString>
#include <QVector>
#include <QWidget>

/// Readable overlay reference, kept above the image and outside the document.
class ShortcutGuide final : public QWidget {
  Q_OBJECT
public:
  explicit ShortcutGuide(QWidget *parent);
  void setEntries(const QVector<QPair<QString, QString>> &entries);
  void setExpanded(bool expanded);
  [[nodiscard]] bool expanded() const { return expanded_; }

protected:
  bool eventFilter(QObject *watched, QEvent *event) override;
  void paintEvent(QPaintEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void leaveEvent(QEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;

private:
  [[nodiscard]] QRect toggleRect() const;
  void reflow();
  QVector<QPair<QString, QString>> entries_;
  bool expanded_ = true;
  bool hovered_ = false;
  int firstRow_ = 0;
  int visibleRows_ = 0;
  int keyWidth_ = 0;
  int contentWidth_ = 0;
};
