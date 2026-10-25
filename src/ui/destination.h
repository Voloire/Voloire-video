#pragma once
#include <QIcon>
#include <QString>
#include "config.h"

namespace vv::ui {

// One presentation of the real output/profile, shared by the panel, HUD and menus.
struct Destination {
  QString platform;  // local | youtube | twitch
  QString channel;
  bool streaming() const { return platform != "local"; }
  QString service() const;
  QString caption() const;
  QString actionText(bool active, bool menu = false) const;
};

Destination destinationFor(const std::string& output, const std::string& channelId,
                           const std::vector<vv::StreamChannel>& channels);
QPixmap destinationPixmap(const QString& platform, int size, qreal dpr = 1.0);
QIcon destinationIcon(const QString& platform);

}  // namespace vv::ui
