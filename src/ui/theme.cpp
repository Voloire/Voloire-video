#include "theme.h"
#include <QDir>
#include <QFontDatabase>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>

namespace vv::ui {

static QString g_mono = "Consolas";
static bool g_dark = false;

void setDarkTheme(bool dark) { g_dark = dark; }
bool isDarkTheme() { return g_dark; }

void loadFonts(const QString& dir) {
  QDir d(dir);
  for (const QString& f : d.entryList({"*.ttf"}, QDir::Files)) QFontDatabase::addApplicationFont(d.filePath(f));
  for (const QString& fam : QFontDatabase::families())
    if (fam.startsWith("Ubuntu Mono")) { g_mono = fam; break; }
}

QString monoFamily() { return g_mono; }

QString styleSheet(bool dark) {
  if (dark) {
    return R"(
* { font-family: 'Ubuntu', 'Segoe UI'; font-size: 12px; color: #D6E4D6; }
QWidget#root { background: #14201E; border: 1px solid #2F3633; }
QScrollArea, QWidget#scrollBody { background: transparent; border: none; }
QLabel { background: transparent; }
QLabel#title { font-size: 14px; font-weight: 700; letter-spacing: 2px; color: #D6E4D6; }
QLabel#sub { color: #8A9590; font-size: 10px; letter-spacing: 1px; }
QLabel#section { color: #9BB0A3; font-size: 10px; font-weight: 700; letter-spacing: 1.5px; }
QLabel#status { color: #6BB384; font-weight: 700; font-size: 11px; }
QLabel#err { color: #EF5350; font-size: 11px; }
QFrame#card { background: #1B2825; border: 1px solid #2F3633; border-radius: 6px; }
QFrame#sep { background: #2F3633; max-height: 1px; min-height: 1px; border: none; }
QTabWidget::pane { border: none; background: transparent; }
QTabBar::tab {
  background: #1B2825; color: #8A9590; border: 1px solid #2F3633; border-radius: 5px;
  padding: 5px 14px; margin-right: 6px; font-weight: 700; font-size: 11px; letter-spacing: 1px; }
QTabBar::tab:selected { background: #2F5D3F; color: #FFFFFF; border: 1px solid #3E7A52; }
QTabBar::tab:hover:!selected { background: #22332F; color: #D6E4D6; }
QComboBox, QSpinBox, QLineEdit {
  background: #14201E; border: 1px solid #2F3633; border-radius: 5px; padding: 2px 6px; min-height: 18px; color: #D6E4D6;
  selection-background-color: #2F5D3F; selection-color: #FFFFFF; }
QComboBox:hover, QSpinBox:hover, QLineEdit:hover, QComboBox:focus, QSpinBox:focus, QLineEdit:focus { border: 1px solid #4F8A5F; }
QComboBox QAbstractItemView { background: #1B2825; border: 1px solid #2F3633; selection-background-color: #154239; selection-color: #D6E4D6; color: #D6E4D6; outline: none; }
QPushButton { background: #22312E; border: 1px solid #2F3633; border-radius: 5px; padding: 3px 10px; color: #D6E4D6; font-weight: 700; letter-spacing: 0.5px; }
QPushButton:hover { border: 1px solid #4F8A5F; color: #FFFFFF; background: #154239; }
QPushButton:pressed { background: #2F5D3F; }
QPushButton:disabled { color: #596560; border: 1px solid #24302D; background: #182320; }
QPushButton#rec { font-size: 13px; padding: 6px 18px; background: #2F5D3F; border: 1px solid #3E7A52; color: #FFFFFF; }
QPushButton#rec:hover { background: #386E4B; border: 1px solid #4F8A5F; }
QPushButton#rec[active="true"] { background: #C0392B; border: 1px solid #C0392B; }
QPushButton#rec[active="true"]:hover { background: #A93226; border: 1px solid #A93226; }
QPushButton#flat { border: none; background: transparent; padding: 1px 6px; font-size: 14px; color: #8A9590; }
QPushButton#flat:hover { color: #D6E4D6; }
QRadioButton, QCheckBox { spacing: 6px; }
QRadioButton::indicator, QCheckBox::indicator { width: 11px; height: 11px; border: 1px solid #45544E; background: #14201E; border-radius: 5px; }
QCheckBox::indicator { border-radius: 3px; }
QRadioButton::indicator:checked, QCheckBox::indicator:checked { background: #4F8A5F; border: 1px solid #6BB384; }
QMenu { background: #14201E; border: 1px solid #2F3633; padding: 4px; }
QMenu::item { padding: 5px 22px 5px 18px; border: 1px solid transparent; color: #D6E4D6; }
QMenu::item:selected { background: #154239; border: 1px solid #2F3633; color: #D6E4D6; }
QMenu::item:disabled { color: #8A9590; }
QMenu::separator { height: 1px; background: #2F3633; margin: 4px 6px; }
QMenu::indicator { width: 10px; height: 10px; margin-left: 6px; }
QMenu::indicator:checked { background: #154239; border: 1px solid #D6E4D6; border-radius: 5px; }
QToolTip { background: #1B2825; color: #D6E4D6; border: 1px solid #2F3633; padding: 4px; }
QScrollBar:vertical { background: #14201E; width: 6px; }
QScrollBar::handle:vertical { background: #2F3633; min-height: 20px; border-radius: 3px; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; }
QSizeGrip { background: transparent; width: 12px; height: 12px; }
)";
  }
  return R"(
* { font-family: 'Ubuntu', 'Segoe UI'; font-size: 12px; color: #2C3A30; }
QWidget#root { background: #F0F4EA; border: 1px solid #DCE3D4; }
QScrollArea, QWidget#scrollBody { background: transparent; border: none; }
QLabel { background: transparent; }
QLabel#title { font-size: 14px; font-weight: 700; letter-spacing: 2px; color: #2C3A30; }
QLabel#sub { color: #7C8A7C; font-size: 10px; letter-spacing: 1px; }
QLabel#section { color: #75857A; font-size: 10px; font-weight: 700; letter-spacing: 1.5px; }
QLabel#status { color: #3E6B4C; font-weight: 700; font-size: 11px; }
QLabel#err { color: #B3372B; font-size: 11px; }
QFrame#card { background: #FFFFFF; border: 1px solid #E2E8DA; border-radius: 6px; }
QFrame#sep { background: #E2E8DA; max-height: 1px; min-height: 1px; border: none; }
QTabWidget::pane { border: none; background: transparent; }
QTabBar::tab {
  background: #FFFFFF; color: #6C7A6E; border: 1px solid #DCE3D4; border-radius: 5px;
  padding: 5px 14px; margin-right: 6px; font-weight: 700; font-size: 11px; letter-spacing: 1px; }
QTabBar::tab:selected { background: #2F5D3F; color: #FFFFFF; border: 1px solid #2F5D3F; }
QTabBar::tab:hover:!selected { background: #E4EDDD; color: #2C3A30; }
QComboBox, QSpinBox, QLineEdit {
  background: #FFFFFF; border: 1px solid #DCE3D4; border-radius: 5px; padding: 2px 6px; min-height: 18px;
  selection-background-color: #2F5D3F; selection-color: #FFFFFF; }
QComboBox:hover, QSpinBox:hover, QLineEdit:hover, QComboBox:focus, QSpinBox:focus, QLineEdit:focus { border: 1px solid #3E6B4C; }
QComboBox QAbstractItemView { background: #FFFFFF; border: 1px solid #DCE3D4; selection-background-color: #E4EDDD; selection-color: #2C3A30; outline: none; }
QPushButton { background: #FFFFFF; border: 1px solid #DCE3D4; border-radius: 5px; padding: 3px 10px; color: #33413A; font-weight: 700; letter-spacing: 0.5px; }
QPushButton:hover { border: 1px solid #3E6B4C; color: #2F5D3F; background: #F4F8EF; }
QPushButton:pressed { background: #E4EDDD; }
QPushButton:disabled { color: #AAB4A6; border: 1px solid #E6EBE1; background: #F7FAF3; }
QPushButton#rec { font-size: 13px; padding: 6px 18px; background: #2F5D3F; border: 1px solid #2F5D3F; color: #FFFFFF; }
QPushButton#rec:hover { background: #28503A; border: 1px solid #28503A; }
QPushButton#rec[active="true"] { background: #C0392B; border: 1px solid #C0392B; }
QPushButton#rec[active="true"]:hover { background: #A93226; border: 1px solid #A93226; }
QPushButton#flat { border: none; background: transparent; padding: 1px 6px; font-size: 14px; color: #7C8A7C; }
QPushButton#flat:hover { color: #2F5D3F; }
QRadioButton, QCheckBox { spacing: 6px; }
QRadioButton::indicator, QCheckBox::indicator { width: 11px; height: 11px; border: 1px solid #C9D4C2; background: #FFFFFF; border-radius: 5px; }
QCheckBox::indicator { border-radius: 3px; }
QRadioButton::indicator:checked, QCheckBox::indicator:checked { background: #2F5D3F; border: 1px solid #2F5D3F; }
QMenu { background: #FFFFFF; border: 1px solid #DCE3D4; padding: 4px; }
QMenu::item { padding: 5px 22px 5px 18px; border: 1px solid transparent; color: #33413A; }
QMenu::item:selected { background: #E4EDDD; border: 1px solid #E4EDDD; color: #2C3A30; }
QMenu::item:disabled { color: #AAB4A6; }
QMenu::separator { height: 1px; background: #E2E8DA; margin: 4px 6px; }
QMenu::indicator { width: 10px; height: 10px; margin-left: 6px; }
QMenu::indicator:checked { background: #2F5D3F; border: 1px solid #2F5D3F; border-radius: 5px; }
QToolTip { background: #FFFFFF; color: #2C3A30; border: 1px solid #DCE3D4; padding: 4px; }
QScrollBar:vertical { background: #F0F4EA; width: 6px; }
QScrollBar::handle:vertical { background: #C9D4C2; min-height: 20px; border-radius: 3px; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; }
QSizeGrip { background: transparent; width: 12px; height: 12px; }
)";
}

QString hudMenuStyleSheet() {
  return R"(
QMenu { background: #14201E; border: 1px solid #2F3633; padding: 4px; }
QMenu::item { padding: 6px 26px 6px 22px; border: 1px solid transparent; color: #D6E4D6; }
QMenu::item:selected { background: #154239; border: 1px solid #2F3633; color: #D6E4D6; }
QMenu::item:disabled { color: #8A9590; }
QMenu::separator { height: 1px; background: #2F3633; margin: 4px 6px; }
QMenu::indicator { width: 10px; height: 10px; margin-left: 6px; }
QMenu::indicator:checked { background: #154239; border: 1px solid #D6E4D6; border-radius: 5px; }
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
  p.fillPath(bg, hud::bg);

  QColor ring, glow;
  switch (s) {
    case IconState::Recording: ring = hud::rec; break;
    case IconState::Paused: ring = hud::paused; break;
    case IconState::Error: ring = col::alert; break;
    default: ring = hud::muted; break;
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
    QColor d = hud::rec;
    d.setAlphaF(0.55 + 0.45 * pulse);
    p.setPen(Qt::NoPen);
    p.setBrush(d);
    p.drawEllipse(c, 13 * u, 13 * u);
  } else if (s == IconState::Paused) {
    p.setPen(Qt::NoPen);
    p.setBrush(hud::paused);
    p.drawRoundedRect(QRectF(21 * u, 20 * u, 8 * u, 24 * u), 2 * u, 2 * u);
    p.drawRoundedRect(QRectF(35 * u, 20 * u, 8 * u, 24 * u), 2 * u, 2 * u);
  } else if (s == IconState::Error) {
    p.setPen(QPen(col::alert, 5 * u, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(32 * u, 18 * u), QPointF(32 * u, 36 * u));
    p.drawPoint(QPointF(32 * u, 45 * u));
  } else {
    p.setPen(QPen(hud::muted, 4 * u));
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

LevelMeter::LevelMeter(QWidget* parent) : QWidget(parent) { setFixedHeight(8); }

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
  p.setPen(QPen(g_dark ? hud::surface : col::line, 1));
  p.setBrush(g_dark ? hud::bg : col::panel2);
  p.drawRoundedRect(r, 2, 2);
  auto pos = [&](double db) { return std::clamp((db + 60.0) / 60.0, 0.0, 1.0) * (r.width() - 4); };
  QRectF bar(r.left() + 2, r.top() + 2, pos(rms_), r.height() - 4);
  QLinearGradient g(bar.topLeft(), QPointF(r.right(), r.top()));
  g.setColorAt(0, col::accent);
  g.setColorAt(0.8, col::accent2);
  g.setColorAt(1, QColor(0x8F, 0xC0, 0x9A));
  p.setPen(Qt::NoPen);
  p.setBrush(g);
  p.drawRect(bar);
  double hx = r.left() + 2 + pos(hold_);
  p.setPen(QPen(col::accent, 2));
  p.drawLine(QPointF(hx, r.top() + 2), QPointF(hx, r.bottom() - 2));
}

}  // namespace vv::ui
