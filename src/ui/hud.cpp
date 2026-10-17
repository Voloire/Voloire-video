#include "hud.h"
#include <QFontMetrics>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QTimer>
#include <cmath>
#include <windows.h>
#include "control_menu.h"
#include "panel.h"
#include "telemetry.h"
#include "theme.h"

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011  // Windows 10 2004+
#endif

namespace vv::ui {

HudBar::HudBar(MainPanel* panel, vv::UiPrefs* prefs) : QWidget(nullptr), panel_(panel), prefs_(prefs) {
  setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus);
  setAttribute(Qt::WA_TranslucentBackground);
  setAttribute(Qt::WA_ShowWithoutActivating);
  setWindowTitle("Voloire-video status");
  setFocusPolicy(Qt::NoFocus);
  refreshContent();
  restorePosition();
  tick_ = new QTimer(this);
  QObject::connect(tick_, &QTimer::timeout, this, [this] {
    pulse_ = !pulse_;
    refreshContent();
  });
  tick_->start(500);
  QObject::connect(panel_, &MainPanel::changed, this, [this] { refreshContent(); });
}

HudBar::~HudBar() = default;

std::string HudBar::dotName() const {
  switch (panel_->recState()) {
    case vv::RecState::Recording:
      if (panel_->outputKind() == "stream")  // live on YouTube: amber when the ingest is gone
        return panel_->streamStatus().value("state", std::string("live")) == "reconnecting" ? "reconnecting" : "live";
      return "recording";
    case vv::RecState::Paused: return "paused";
    default: return panel_->errorText().isEmpty() ? "idle" : "error";
  }
}

QString HudBar::displayText() const {
  QString t = label_ + "  " + panel_->timerText();
  if (!kbps_.isEmpty()) t += "  " + kbps_;
  return t;
}

void HudBar::refreshContent() {
  QString label, kbps;
  const bool stream = panel_->recState() == vv::RecState::Recording && panel_->outputKind() == "stream";
  vv::json sh = panel_->shareStatus();
  if (sh.value("state", std::string("idle")) == "running") {
    label = QString("SHARE %1%").arg((int)std::lround(sh.value("progress", 0.0) * 100));
  } else if (stream) {
    vv::json st = panel_->streamStatus();
    label = st.value("state", std::string("live")) == "reconnecting" ? "RECONN" : "LIVE";
    kbps = QString("%1M").arg(st.value("kbps", 0.0) / 1000.0, 0, 'f', 1);
  } else {
    label = QString(vv::stateName(panel_->recState())).toUpper();
    if (panel_->recState() == vv::RecState::Idle && !panel_->errorText().isEmpty()) label = "ERROR";
  }
  label_ = label;
  kbps_ = kbps;
  QFont f(monoFamily());
  f.setPixelSize(12);
  int w = std::max(160, QFontMetrics(f).horizontalAdvance(displayText()) + 44);
  if (width() != w || height() != 28) setFixedSize(w, 28);
  setToolTip(panel_->errorText().isEmpty() ? QString("Voloire-video | %1 | %2").arg(label_, panel_->timerText())
                                           : panel_->errorText());
  update();
}

void HudBar::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  QRectF r = rect().adjusted(0.5, 0.5, -0.5, -0.5);
  const double radius = r.height() / 2;
  if (testPattern_) {  // solid marker for the capture-exclusion test
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 0, 255));
    p.drawRoundedRect(r, radius, radius);
    return;
  }
  p.setPen(QPen(hud::surface, 1));
  p.setBrush(hud::bg);
  p.drawRoundedRect(r, radius, radius);

  const std::string dot = dotName();
  QColor c = hud::tint;
  double alpha = 0.5;  // idle
  if (dot == "recording" || dot == "live") { c = hud::rec; alpha = pulse_ ? 1.0 : 0.4; }
  else if (dot == "paused" || dot == "reconnecting") { c = hud::paused; alpha = 1.0; }  // amber: ingest gone
  else if (dot == "error") { c = hud::rec; alpha = 1.0; }
  c.setAlphaF(alpha);
  p.setPen(Qt::NoPen);
  p.setBrush(c);
  p.drawEllipse(QPointF(r.left() + 14, r.center().y()), 4.5, 4.5);

  if (!label_.isEmpty()) {
    QFont f(monoFamily());
    f.setPixelSize(12);
    p.setFont(f);
    p.setPen(hud::tint);
    p.drawText(QRectF(r.left() + 24, r.top(), r.width() - 30, r.height()), Qt::AlignVCenter | Qt::AlignLeft,
               displayText());
  }
}

void HudBar::showEvent(QShowEvent* e) {
  QWidget::showEvent(e);
  applyExclusion();
}

void HudBar::applyExclusion() {
  BOOL ok = SetWindowDisplayAffinity((HWND)winId(), exclude_ ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
  excluded_ = exclude_ && ok == TRUE;
  VV_LOG(Info, "hud", "display affinity", {{"excluded", exclude_}, {"applied", ok == TRUE}});
}

void HudBar::setTestPattern(bool on) {
  testPattern_ = on;
  update();
}

void HudBar::setExcludeFromCapture(bool on) {
  exclude_ = on;
  if (isVisible()) applyExclusion();
  else excluded_ = on;
}

void HudBar::setShown(bool on) {
  prefs_->hud = on;
  vv::saveUiPrefs(*prefs_);
  if (on) show(); else hide();
}

void HudBar::moveTo(int x, int y) {
  move(x, y);
  prefs_->hudX = x;
  prefs_->hudY = y;
  vv::saveUiPrefs(*prefs_);
}

void HudBar::restorePosition() {
  bool placed = prefs_->hudX >= 0 && prefs_->hudY >= 0;
  if (placed) {
    QRect saved(prefs_->hudX, prefs_->hudY, width(), height());
    placed = false;
    for (QScreen* s : QGuiApplication::screens())
      if (s->geometry().intersects(saved)) { placed = true; break; }
  }
  if (placed) {
    move(prefs_->hudX, prefs_->hudY);
    return;
  }
  QScreen* s = QGuiApplication::primaryScreen();
  QRect g = s ? s->geometry() : QRect(0, 0, 1920, 1080);
  move(g.center().x() - width() / 2, g.top() + 8);
  prefs_->hudX = pos().x();  // in memory only: the next explicit move or toggle persists it
  prefs_->hudY = pos().y();
}

void HudBar::mousePressEvent(QMouseEvent* e) {
  if (e->button() == Qt::LeftButton) {
    dragging_ = true;
    moved_ = false;
    dragFrom_ = e->globalPosition().toPoint() - frameGeometry().topLeft();
    e->accept();
  } else if (e->button() == Qt::RightButton) {
    rebuildMenu();
    menu_.popup(e->globalPosition().toPoint());
    e->accept();
  }
}

void HudBar::mouseMoveEvent(QMouseEvent* e) {
  if (!dragging_ || !(e->buttons() & Qt::LeftButton)) return;
  QPoint wanted = e->globalPosition().toPoint() - dragFrom_;
  if (!moved_ && (wanted - pos()).manhattanLength() > 4) moved_ = true;
  if (moved_) move(wanted);
  e->accept();
}

void HudBar::mouseReleaseEvent(QMouseEvent* e) {
  if (e->button() != Qt::LeftButton || !dragging_) return;
  dragging_ = false;
  if (moved_) {
    prefs_->hudX = pos().x();
    prefs_->hudY = pos().y();
    vv::saveUiPrefs(*prefs_);
  } else if (panel_->isVisible()) {
    panel_->hide();
  } else {
    panel_->show();
    panel_->raise();
    panel_->activateWindow();
  }
  e->accept();
}

void HudBar::rebuildMenu() {
  menu_.clear();
  ControlMenuHooks hooks;
  hooks.showPanel = [this] {
    panel_->show();
    panel_->raise();
    panel_->activateWindow();
  };
  hooks.hudVisible = [this] { return prefs_->hud; };
  hooks.setHudVisible = [this](bool on) { setShown(on); };
  buildControlMenu(menu_, *panel_, hooks);
}

bool HudBar::trigger(const std::vector<std::string>& path) {
  rebuildMenu();
  return triggerMenu(menu_, path);
}

vv::json HudBar::state() const {
  auto* self = const_cast<HudBar*>(this);
  self->rebuildMenu();  // like the tray: a dump is always of the current menu
  RECT r{};
  GetWindowRect((HWND)self->winId(), &r);
  return {{"visible", isVisible()},
          {"x", (int)r.left},
          {"y", (int)r.top},
          {"w", (int)(r.right - r.left)},
          {"h", (int)(r.bottom - r.top)},
          {"label", label_.toStdString()},
          {"kbps", kbps_.toStdString()},
          {"timer", panel_->timerText().toStdString()},
          {"dot", dotName()},
          {"excludedFromCapture", excluded_},
          {"menu", dumpMenu(const_cast<QMenu*>(&menu_))}};
}

}  // namespace vv::ui
