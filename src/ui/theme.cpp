#include "theme.h"
#include <QDir>
#include <QFontDatabase>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>

namespace vv::ui {

static QString g_mono = "Consolas";

void loadFonts(const QString& dir) {
  QDir d(dir);
  for (const QString& f : d.entryList({"*.ttf"}, QDir::Files)) QFontDatabase::addApplicationFont(d.filePath(f));
  for (const QString& fam : QFontDatabase::families())
    if (fam.startsWith("Ubuntu Mono")) { g_mono = fam; break; }
}

QString monoFamily() { return g_mono; }

QString styleSheet() {
  return R"(
* { font-family: 'Ubuntu', 'Segoe UI'; font-size: 13px; color: #D6ECFF; }
QWidget#root { background: #020409; border: 1px solid #0C3C66; }
QLabel { background: transparent; }
QLabel#title { font-size: 17px; font-weight: 700; letter-spacing: 3px; color: #00F0FF; }
QLabel#sub { color: #5B7492; font-size: 11px; letter-spacing: 2px; }
QLabel#section { color: #00B4FF; font-size: 11px; font-weight: 700; letter-spacing: 3px; }
QLabel#status { color: #58AEFF; }
QLabel#err { color: #FF3D6E; }
QFrame#card { background: #060B15; border: 1px solid #0C3C66; border-radius: 3px; }
QFrame#sep { background: #0C3C66; max-height: 1px; min-height: 1px; border: none; }
QComboBox, QSpinBox, QLineEdit {
  background: #0A1424; border: 1px solid #0C3C66; border-radius: 2px; padding: 4px 8px; min-height: 20px;
  selection-background-color: #00B4FF; selection-color: #020409; }
QComboBox:hover, QSpinBox:hover, QLineEdit:hover, QComboBox:focus, QSpinBox:focus, QLineEdit:focus { border: 1px solid #00B4FF; }
QComboBox::drop-down { border: none; width: 22px; }
QComboBox::down-arrow { image: none; border-left: 4px solid transparent; border-right: 4px solid transparent; border-top: 5px solid #00B4FF; margin-right: 8px; }
QComboBox QAbstractItemView { background: #060B15; border: 1px solid #00B4FF; selection-background-color: #0C3C66; selection-color: #00F0FF; outline: none; }
QPushButton { background: #0A1424; border: 1px solid #0C3C66; border-radius: 2px; padding: 6px 14px; color: #58AEFF; font-weight: 700; letter-spacing: 1px; }
QPushButton:hover { border: 1px solid #00B4FF; color: #00F0FF; background: #0B1C34; }
QPushButton:pressed { background: #00B4FF; color: #020409; }
QPushButton:disabled { color: #2A3B52; border: 1px solid #0A2036; }
QPushButton#rec { font-size: 15px; padding: 10px 22px; border: 1px solid #00B4FF; color: #00F0FF; }
QPushButton#rec[active="true"] { background: #00B4FF; color: #020409; }
QPushButton#flat { border: none; background: transparent; padding: 2px 8px; font-size: 15px; }
QPushButton#flat:hover { color: #00F0FF; }
QRadioButton, QCheckBox { spacing: 8px; }
QRadioButton::indicator, QCheckBox::indicator { width: 12px; height: 12px; border: 1px solid #0C3C66; background: #0A1424; border-radius: 6px; }
QCheckBox::indicator { border-radius: 2px; }
QRadioButton::indicator:checked, QCheckBox::indicator:checked { background: #00B4FF; border: 1px solid #00F0FF; }
QMenu { background: #060B15; border: 1px solid #00B4FF; padding: 4px; }
QMenu::item { padding: 6px 26px 6px 22px; border: 1px solid transparent; }
QMenu::item:selected { background: #0B1C34; border: 1px solid #0C3C66; color: #00F0FF; }
QMenu::item:disabled { color: #5B7492; }
QMenu::separator { height: 1px; background: #0C3C66; margin: 4px 6px; }
QMenu::indicator { width: 10px; height: 10px; margin-left: 6px; }
QMenu::indicator:checked { background: #00B4FF; border: 1px solid #00F0FF; border-radius: 5px; }
QToolTip { background: #060B15; color: #00F0FF; border: 1px solid #00B4FF; padding: 4px; }
QScrollBar:vertical { background: #060B15; width: 8px; }
QScrollBar::handle:vertical { background: #0C3C66; min-height: 20px; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; }
)";
}

QPixmap trayPixmap(IconState s, int size, double pulse) {
  QPixmap pm(size, size);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  const double u = size / 64.0;
  QRectF r(2 * u, 2 * u, 60 * u, 60 * u);
  QPainterPath bg;
  bg.addRoundedRect(r, 14 * u, 14 * u);
  p.fillPath(bg, QColor(2, 4, 9));

  QColor ring, glow;
  switch (s) {
    case IconState::Recording: ring = col::cyan; break;
    case IconState::Paused: ring = col::azure; break;
    case IconState::Error: ring = col::alert; break;
    default: ring = col::dim; break;
  }
  glow = ring;
  glow.setAlphaF(0.30);
  p.setPen(QPen(glow, 7 * u));
  p.setBrush(Qt::NoBrush);
  p.drawRoundedRect(r.adjusted(2 * u, 2 * u, -2 * u, -2 * u), 12 * u, 12 * u);
  p.setPen(QPen(ring, 3 * u));
  p.drawRoundedRect(r.adjusted(2 * u, 2 * u, -2 * u, -2 * u), 12 * u, 12 * u);

  QPointF c(32 * u, 32 * u);
  if (s == IconState::Recording) {
    QColor d = col::cyan;
    d.setAlphaF(0.55 + 0.45 * pulse);
    p.setPen(Qt::NoPen);
    p.setBrush(d);
    p.drawEllipse(c, 13 * u, 13 * u);
  } else if (s == IconState::Paused) {
    p.setPen(Qt::NoPen);
    p.setBrush(col::azure);
    p.drawRoundedRect(QRectF(21 * u, 20 * u, 8 * u, 24 * u), 2 * u, 2 * u);
    p.drawRoundedRect(QRectF(35 * u, 20 * u, 8 * u, 24 * u), 2 * u, 2 * u);
  } else if (s == IconState::Error) {
    p.setPen(QPen(col::alert, 5 * u, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(32 * u, 18 * u), QPointF(32 * u, 36 * u));
    p.drawPoint(QPointF(32 * u, 45 * u));
  } else {
    p.setPen(QPen(col::dim, 4 * u));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(c, 12 * u, 12 * u);
  }
  return pm;
}

QIcon trayIcon(IconState s, double pulse) {
  QIcon i;
  for (int sz : {16, 24, 32, 48, 64}) i.addPixmap(trayPixmap(s, sz, pulse));
  return i;
}

LevelMeter::LevelMeter(QWidget* parent) : QWidget(parent) { setFixedHeight(14); }

void LevelMeter::setLevels(double rmsDb, double peakDb) {
  rms_ = rmsDb;
  peak_ = peakDb;
  hold_ = std::max(peakDb, hold_ - 1.2);
  update();
}

void LevelMeter::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  QRectF r = rect().adjusted(0.5, 0.5, -0.5, -0.5);
  p.setPen(QPen(col::line, 1));
  p.setBrush(col::panel2);
  p.drawRoundedRect(r, 2, 2);
  auto pos = [&](double db) { return std::clamp((db + 60.0) / 60.0, 0.0, 1.0) * (r.width() - 4); };
  QRectF bar(r.left() + 2, r.top() + 2, pos(rms_), r.height() - 4);
  QLinearGradient g(bar.topLeft(), QPointF(r.right(), r.top()));
  g.setColorAt(0, col::neon);
  g.setColorAt(0.8, col::cyan);
  g.setColorAt(1, QColor(255, 255, 255));
  p.setPen(Qt::NoPen);
  p.setBrush(g);
  p.drawRect(bar);
  double hx = r.left() + 2 + pos(hold_);
  p.setPen(QPen(col::cyan, 2));
  p.drawLine(QPointF(hx, r.top() + 2), QPointF(hx, r.bottom() - 2));
}

}  // namespace vv::ui
