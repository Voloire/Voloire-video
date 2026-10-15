#pragma once
#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QString>
#include <QWidget>

namespace vv::ui {

// Black base, neon blue and azure accents.
namespace col {
inline const QColor bg(0x02, 0x04, 0x09);
inline const QColor panel(0x06, 0x0B, 0x15);
inline const QColor panel2(0x0A, 0x14, 0x24);
inline const QColor line(0x0C, 0x3C, 0x66);
inline const QColor neon(0x00, 0xB4, 0xFF);
inline const QColor cyan(0x00, 0xF0, 0xFF);
inline const QColor azure(0x58, 0xAE, 0xFF);
inline const QColor dim(0x5B, 0x74, 0x92);
inline const QColor text(0xD6, 0xEC, 0xFF);
inline const QColor alert(0xFF, 0x3D, 0x6E);
}  // namespace col

QString styleSheet();
void loadFonts(const QString& dir);  // Ubuntu, Ubuntu Mono
QString monoFamily();

enum class IconState { Idle, Recording, Paused, Error };
QPixmap trayPixmap(IconState s, int size, double pulse = 1.0);
QIcon trayIcon(IconState s, double pulse = 1.0);

// Horizontal level meter with peak hold, driven by dBFS values.
class LevelMeter : public QWidget {
 public:
  explicit LevelMeter(QWidget* parent = nullptr);
  void setLevels(double rmsDb, double peakDb);
  QSize sizeHint() const override { return {220, 14}; }
  double rmsDb() const { return rms_; }

 protected:
  void paintEvent(QPaintEvent*) override;

 private:
  double rms_ = -120, peak_ = -120, hold_ = -120;
};

}  // namespace vv::ui
