#pragma once
#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QString>
#include <QWidget>

namespace vv::ui {

// Light sage pages, white cards and deep green accents (docs/design/palette.png).
namespace col {
inline const QColor bg(0xF0, 0xF4, 0xEA);
inline const QColor panel(0xFF, 0xFF, 0xFF);
inline const QColor panel2(0xF4, 0xF8, 0xEF);
inline const QColor line(0xDC, 0xE3, 0xD4);
inline const QColor accent(0x2F, 0x5D, 0x3F);
inline const QColor accent2(0x4F, 0x8A, 0x5F);
inline const QColor dim(0x7C, 0x8A, 0x7C);
inline const QColor text(0x2C, 0x3A, 0x30);
inline const QColor rec(0xC0, 0x39, 0x2B);
inline const QColor paused(0xB0, 0x86, 0x1E);
inline const QColor alert(0xB3, 0x37, 0x2B);
}  // namespace col

// The status bar and the shared control menus: sage on near-black, red recording dot
// (docs/design/palette.png, "Menu dark").
namespace hud {
inline const QColor bg(0x14, 0x20, 0x1E);
inline const QColor surface(0x2F, 0x36, 0x33);
inline const QColor accent(0x15, 0x42, 0x39);
inline const QColor tint(0xD6, 0xE4, 0xD6);
inline const QColor muted(0x8A, 0x95, 0x90);
inline const QColor rec(0xEF, 0x27, 0x46);
inline const QColor paused(0xFF, 0xB0, 0x00);
}  // namespace hud

QString styleSheet(bool dark = false);
void setDarkTheme(bool dark);
bool isDarkTheme();
QString hudMenuStyleSheet();  // the shared control menus, in the hud tokens
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
  QSize sizeHint() const override { return {220, 8}; }
  double rmsDb() const { return rms_; }

 protected:
  void paintEvent(QPaintEvent*) override;

 private:
  double rms_ = -120, peak_ = -120, hold_ = -120;
};

}  // namespace vv::ui
