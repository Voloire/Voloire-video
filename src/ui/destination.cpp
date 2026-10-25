#include "destination.h"
#include <QPainter>
#include <QPainterPath>
#include "theme.h"

namespace vv::ui {

QString Destination::service() const {
  if (platform == "twitch") return "Twitch";
  if (platform == "youtube") return "YouTube";
  if (platform == "unknown") return "Unknown service";
  return "Local recording";
}

QString Destination::caption() const {
  return streaming() ? service().toUpper() + " / " + channel : QString("LOCAL RECORDING");
}

QString Destination::actionText(bool active, bool menu) const {
  QString text;
  if (!streaming()) text = active ? "Stop recording" : (menu ? "Start recording" : "Record locally");
  else text = active ? "End " + service() + " stream" : "Go live on " + service();
  return menu ? text : text.toUpper();
}

Destination destinationFor(const std::string& output, const std::string& channelId,
                           const std::vector<vv::StreamChannel>& channels) {
  if (output != "stream") return {"local", {}};
  for (const auto& c : channels)
    if (c.id == channelId)
      return {QString::fromStdString(c.platform == "twitch" ? "twitch" : "youtube"),
              QString::fromStdString(c.platform == "twitch" && !c.twitchLogin.empty() ? c.twitchLogin : c.label)};
  // Never label an unresolved stream as a local recording or guess a service.
  return {"unknown", "Select a destination"};
}

QPixmap destinationPixmap(const QString& platform, int size, qreal dpr) {
  QPixmap pm(qRound(size * dpr), qRound(size * dpr));
  pm.setDevicePixelRatio(dpr);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  p.scale(size / 24.0, size / 24.0);
  p.setPen(Qt::NoPen);
  if (platform == "youtube") {
    p.setBrush(QColor("#FF0033"));
    p.drawRoundedRect(QRectF(1, 4.25, 22, 15.5), 4, 4);
    p.setBrush(Qt::white);
    p.drawPolygon(QPolygonF{QPointF(10, 8), QPointF(16, 12), QPointF(10, 16)});
  } else if (platform == "twitch") {
    // The Twitch chat bubble and its two vertical eyes, drawn as vectors at any DPI.
    p.setBrush(QColor("#9146FF"));
    p.drawPolygon(QPolygonF{QPointF(4, 1), QPointF(23, 1), QPointF(23, 15), QPointF(17, 21),
                           QPointF(12, 21), QPointF(8, 24), QPointF(8, 21), QPointF(2, 21), QPointF(2, 5)});
    p.setBrush(Qt::white);
    p.drawPolygon(QPolygonF{QPointF(6, 3), QPointF(21, 3), QPointF(21, 14), QPointF(17, 18),
                           QPointF(13, 18), QPointF(10, 21), QPointF(10, 18), QPointF(6, 18)});
    p.setBrush(QColor("#9146FF"));
    p.drawRect(QRectF(11, 6, 2, 7));
    p.drawRect(QRectF(17, 6, 2, 7));
  } else {
    p.setPen(QPen(isDarkTheme() ? hud::tint : col::text, 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(3, 2, 18, 20), 2, 2);
    p.drawEllipse(QPointF(12, 12), 4, 4);
  }
  return pm;
}

QIcon destinationIcon(const QString& platform) {
  QIcon icon;
  for (int size : {16, 20, 24, 32, 48, 64}) icon.addPixmap(destinationPixmap(platform, size));
  return icon;
}

}  // namespace vv::ui
