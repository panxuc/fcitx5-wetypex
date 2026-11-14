#pragma once
#include "../common/qt_helpers.hpp"

#include <QAbstractButton>
#include <QBoxLayout>
#include <QFrame>
#include <QCursor>
#include <QGuiApplication>
#include <QFile>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QMap>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScreen>
#include <QStyle>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSizeGrip>
#include <QStandardPaths>
#include <QSvgRenderer>
#include <QTabBar>
#include <QToolButton>
#include <QWidget>
#include <QWindow>

namespace original_ui {

constexpr auto kGreen = "#23c891";
constexpr auto kContent = "#f7f7f7";
constexpr auto kMuted = "#969696";

inline QString asset(const QString &name) {
  return QStandardPaths::locate(QStandardPaths::GenericDataLocation,
                                "fcitx5-wetypex/ui/icons/" + name + ".svg");
}
inline QString imageAsset(const QString &name) {
  return QStandardPaths::locate(QStandardPaths::GenericDataLocation,
                                "fcitx5-wetypex/ui/images/" + name);
}
inline QIcon appIcon() {
  auto icon = QIcon::fromTheme("fcitx5-wetypex");
  if (icon.isNull())
    icon = QIcon(
        QStandardPaths::locate(QStandardPaths::GenericDataLocation,
                               "icons/hicolor/256x256/apps/fcitx5-wetypex.png"));
  return icon;
}

inline QPixmap tinted(const QString &name, const QColor &color, int size = 20) {
  QPixmap pixmap = QIcon(asset(name)).pixmap(size, size);
  if (!pixmap.isNull()) {
    QPainter painter(&pixmap);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(pixmap.rect(), color);
  }
  return pixmap;
}

inline QIcon navIcon(const QString &name) {
  QIcon icon;
  icon.addPixmap(tinted(name, QColor("#263b40")), QIcon::Normal, QIcon::Off);
  icon.addPixmap(tinted(name, Qt::white), QIcon::Selected, QIcon::Off);
  return icon;
}

class Toggle final : public QAbstractButton {
public:
  explicit Toggle(bool checked = false, QWidget *parent = nullptr)
      : QAbstractButton(parent) {
    setCheckable(true);
    setChecked(checked);
    setCursor(Qt::PointingHandCursor);
    setFixedSize(36, 20);
  }
  QSize sizeHint() const override { return {36, 20}; }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QColor track = isChecked() ? QColor(kGreen) : QColor("#c6c7c8");
    painter.setPen(Qt::NoPen);
    painter.setBrush(track);
    painter.drawRoundedRect(rect(), 10, 10);
    painter.setBrush(Qt::white);
    painter.drawEllipse(QRectF(isChecked() ? 18 : 2, 2, 16, 16));
  }
};

class Check final : public QAbstractButton {
public:
  explicit Check(bool checked = false, QWidget *parent = nullptr)
      : QAbstractButton(parent) {
    setCheckable(true);
    setChecked(checked);
    setCursor(Qt::PointingHandCursor);
    setFixedSize(18, 18);
  }
  QSize sizeHint() const override { return {18, 18}; }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(isChecked() ? QColor(kGreen) : QColor("#cfd1d2"), 1));
    painter.setBrush(isChecked() ? QColor(kGreen) : Qt::white);
    painter.drawRoundedRect(QRectF(0.5, 0.5, 17, 17), 3, 3);
    if (isChecked()) {
      QPen check(Qt::white, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
      painter.setPen(check);
      painter.drawLine(QPointF(4.5, 9), QPointF(7.6, 12.1));
      painter.drawLine(QPointF(7.6, 12.1), QPointF(13.7, 5.8));
    }
  }
};

class SourceIconButton final : public QAbstractButton {
  QSvgRenderer renderer_;

  static QByteArray readSource(const QString &path) {
    if (path.isEmpty())
      return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
      return {};
    QByteArray source = file.readAll();
    // Qt SVG does not understand the Display-P3 CSS emitted by the source
    // program. Remove only that redundant style attribute; the original fill,
    // geometry and path remain unchanged.
    qsizetype begin = 0;
    while ((begin = source.indexOf(" style=\"", begin)) >= 0) {
      const qsizetype end = source.indexOf('"', begin + 8);
      if (end < 0)
        break;
      source.remove(begin, end - begin + 1);
    }
    return source;
  }

public:
  explicit SourceIconButton(const QString &path, QWidget *parent = nullptr)
      : QAbstractButton(parent), renderer_(readSource(path)) {
    setFixedSize(46, 28);
    setCursor(Qt::PointingHandCursor);
    setAccessibleName(QStringLiteral("关闭"));
    setToolTip(QStringLiteral("关闭"));
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    if (underMouse())
      painter.fillRect(rect(), QColor(0, 0, 0, 12));
    if (renderer_.isValid()) {
      // Paint at the widget's current size and device pixel ratio, including
      // the smaller buttons in dialogs and moves between scaled screens.
      renderer_.render(&painter, QRectF(rect()));
    } else {
      // First-run windows also need an icon before upstream assets exist.
      painter.setRenderHint(QPainter::Antialiasing);
      painter.setPen(QPen(QColor("#555555"), 1.5));
      const QPointF center = QRectF(rect()).center();
      painter.drawLine(center + QPointF(-5, -5), center + QPointF(5, 5));
      painter.drawLine(center + QPointF(5, -5), center + QPointF(-5, 5));
    }
  }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  void enterEvent(QEnterEvent *event) override {
#else
  void enterEvent(QEvent *event) override {
#endif
    QAbstractButton::enterEvent(event);
    update();
  }
  void leaveEvent(QEvent *event) override {
    QAbstractButton::leaveEvent(event);
    update();
  }
};

class VoiceSample final : public QWidget {
public:
  explicit VoiceSample(QWidget *parent = nullptr) : QWidget(parent) {
    setFixedHeight(70);
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const int center = width() / 2;
    painter.setPen(QPen(QColor("#dedede"), 1.4, Qt::SolidLine,
                        Qt::RoundCap));
    const int heights[] = {7, 16, 24, 12, 20, 28, 16, 10, 20, 13, 6};
    for (int i = 0; i < 11; ++i) {
      const int x = center - 91 + i * 6;
      painter.drawLine(QPointF(x, 35 - heights[i] / 2.0),
                       QPointF(x, 35 + heights[i] / 2.0));
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor("#f2f3f4"));
    painter.drawEllipse(QPointF(center, 35), 20, 20);
    QPixmap microphone = tinted("icon_menu_speechvoice", QColor("#666"), 20);
    painter.drawPixmap(center - 10, 25, microphone);
    painter.setPen(QColor("#e2e2e2"));
    painter.setFont(font());
    painter.drawText(QRect(center + 45, 0, 80, 70), Qt::AlignVCenter,
                     QStringLiteral("你好"));
  }
};

class Window final : public QWidget {
  QPoint dragOffset_;
  QSizeGrip *sizeGrip_ = nullptr;
  QPointer<QAbstractButton> closeButton_;
  bool firstShow_ = true;

  void fitInitialScreen() {
    auto *screen = windowHandle() ? windowHandle()->screen() : nullptr;
    if (!screen)
      screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
      screen = QGuiApplication::primaryScreen();
    if (!screen)
      return;
    auto frame = frameGeometry().size() - size();
    if (!windowFlags().testFlag(Qt::FramelessWindowHint))
      frame.setHeight(qMax(frame.height(),
                           style()->pixelMetric(QStyle::PM_TitleBarHeight)));
    const auto available =
        (screen->availableGeometry().size() - frame - QSize(24, 24))
            .expandedTo(QSize(1, 1));
    setMinimumSize(QSize(600, 450).boundedTo(available));
    if (!isMaximized() && !isFullScreen())
      resize(size().boundedTo(available));
  }

  void updateSizeGrip() {
    sizeGrip_->resize(sizeGrip_->sizeHint());
    sizeGrip_->move(width() - sizeGrip_->width(), height() - sizeGrip_->height());
    sizeGrip_->setVisible(!isMaximized() && !isFullScreen());
    sizeGrip_->raise();
  }

public:
  void anchorCloseButton(QAbstractButton *button) {
    closeButton_ = button;
    button->parentWidget()->installEventFilter(this);
    button->move(button->parentWidget()->width() - button->width(), 0);
    button->raise();
  }
  explicit Window(QWidget *parent = nullptr) : QWidget(parent) {
    auto flags = Qt::Window | Qt::WindowMinMaxButtonsHint;
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    flags |= Qt::FramelessWindowHint;
#endif
    // Qt 5.12 has no public system-move/resize API for Wayland. Its native
    // window frame supplies those operations, including maximize/restore.
    setWindowFlags(flags);
    setAttribute(Qt::WA_TranslucentBackground);
    setMinimumSize(600, 450);
    resize(900, 820);
    sizeGrip_ = new QSizeGrip(this);
    sizeGrip_->setStyleSheet("background:transparent;");
    fitInitialScreen();
  }
  ~Window() override {
    for (auto *process :
         findChildren<QProcess *>(QString(), Qt::FindDirectChildrenOnly))
      wetype::stopProcess(*process, this);
  }

protected:
  bool eventFilter(QObject *object, QEvent *event) override {
    if (closeButton_ && object == closeButton_->parentWidget() &&
        event->type() == QEvent::Resize) {
      closeButton_->move(closeButton_->parentWidget()->width() -
                             closeButton_->width(),
                         0);
      closeButton_->raise();
    }
    return QWidget::eventFilter(object, event);
  }
  void mouseDoubleClickEvent(QMouseEvent *event) override {
    if (event->button() == Qt::LeftButton &&
        wetype::mousePosition(event).y() < 58) {
      dragOffset_ = {};
      isMaximized() ? showNormal() : showMaximized();
      event->accept();
      return;
    }
    QWidget::mouseDoubleClickEvent(event);
  }
  void mousePressEvent(QMouseEvent *event) override {
    if (event->button() != Qt::LeftButton ||
        wetype::mousePosition(event).y() >= 58)
      return;

#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    dragOffset_ = {};
    if (windowHandle() && windowHandle()->startSystemMove()) {
      event->accept();
      return;
    }
#endif

    dragOffset_ =
        wetype::mouseGlobalPosition(event) - frameGeometry().topLeft();
    event->accept();
  }
  void mouseMoveEvent(QMouseEvent *event) override {
    if (event->buttons().testFlag(Qt::LeftButton) && !dragOffset_.isNull()) {
      move(wetype::mouseGlobalPosition(event) - dragOffset_);
      event->accept();
    }
  }
  void mouseReleaseEvent(QMouseEvent *) override { dragOffset_ = {}; }
  void resizeEvent(QResizeEvent *event) override {
    QWidget::resizeEvent(event);
    if (sizeGrip_)
      updateSizeGrip();
  }
  void showEvent(QShowEvent *event) override {
    QWidget::showEvent(event);
    if (firstShow_) {
      firstShow_ = false;
      fitInitialScreen();
      if (auto *handle = windowHandle())
        QObject::connect(handle, &QWindow::screenChanged, this,
                         [this](QScreen *) { fitInitialScreen(); });
    }
    updateSizeGrip();
  }
  void changeEvent(QEvent *event) override {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange && sizeGrip_)
      updateSizeGrip();
  }
};

inline QFrame *card(QWidget *parent = nullptr) {
  auto *frame = new QFrame(parent);
  frame->setObjectName("card");
  frame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
  return frame;
}

inline QFrame *separator() {
  auto *line = new QFrame;
  line->setObjectName("separator");
  line->setFixedHeight(1);
  return line;
}

inline QWidget *row(const QString &title, const QString &subtitle = {},
                    QWidget *trailing = nullptr, int height = 62) {
  auto *widget = new QWidget;
  widget->setObjectName("settingRow");
  widget->setFixedHeight(height);
  auto *layout = new QHBoxLayout(widget);
  layout->setContentsMargins(15, 0, 15, 0);
  auto *text = new QVBoxLayout;
  text->setSpacing(2);
  text->addStretch();
  auto *titleLabel = new QLabel(title);
  titleLabel->setObjectName("rowTitle");
  text->addWidget(titleLabel);
  if (!subtitle.isEmpty()) {
    auto *subtitleLabel = new QLabel(subtitle);
    subtitleLabel->setObjectName("rowSubtitle");
    subtitleLabel->setWordWrap(true);
    text->addWidget(subtitleLabel);
  }
  text->addStretch();
  layout->addLayout(text, 1);
  if (trailing)
    layout->addWidget(trailing, 0, Qt::AlignVCenter);
  return widget;
}

struct Page {
  QWidget *widget;
  QVBoxLayout *body;
  QScrollArea *scroll;
};

inline Page page(const QString &title, bool scroll = true) {
  auto *widget = new QWidget;
  widget->setObjectName("contentPage");
  auto *layout = new QVBoxLayout(widget);
  layout->setContentsMargins(20, 27, scroll ? 13 : 22, 0);
  layout->setSpacing(16);
  auto *heading = new QLabel(title);
  heading->setObjectName("pageHeading");
  heading->setContentsMargins(12, 0, 0, 0);
  heading->setFixedHeight(20);
  layout->addWidget(heading);
  auto *viewport = new QWidget;
  auto *body = new QVBoxLayout(viewport);
  body->setContentsMargins(0, 0, 0, 20);
  body->setSpacing(20);
  body->setAlignment(Qt::AlignTop);
  auto *area = new QScrollArea;
  area->setObjectName("pageScroll");
  area->setFrameShape(QFrame::NoFrame);
  area->setWidgetResizable(true);
  area->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  area->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  area->setWidget(viewport);
  layout->addWidget(area, 1);
  return {widget, body, area};
}

inline QLabel *keyCap(const QString &text) {
  auto *label = new QLabel(text);
  label->setObjectName("keyCap");
  label->setAlignment(Qt::AlignCenter);
  label->setMinimumWidth(qMax(25, text.size() * 8 + 16));
  label->setFixedHeight(22);
  static const QMap<QString, QString> icons = {
      {QStringLiteral("×"), "icon_delete_key"},
      {QStringLiteral("backslash-icon"), "icon_tips_backslash"},
      {QStringLiteral("["), "icon_tips_%5B"},
      {QStringLiteral("]"), "icon_tips_%5D"},
      {QStringLiteral("。"), "icon_tips_%E3%80%82"},
      {QStringLiteral("，"), "icon_tips_%EF%BC%8C"},
      {QStringLiteral("；"), "icon_tips_;"},
      {QStringLiteral("shift + tab"), "icon_tips_shift+tab_windows"},
      {QStringLiteral("tab"), "icon_tips_tab_windows"}};
  if (icons.contains(text)) {
    label->setText({});
    label->setPixmap(QIcon(asset(icons.value(text))).pixmap(14, 14));
  }
  return label;
}

inline QPushButton *greenButton(const QString &text) {
  auto *button = new QPushButton(text);
  button->setObjectName("greenButton");
  button->setCursor(Qt::PointingHandCursor);
  return button;
}

inline QTabBar *segments(const QStringList &labels) {
  auto *tabs = new QTabBar;
  tabs->setObjectName("segments");
  tabs->setExpanding(true);
  tabs->setDrawBase(false);
  for (const auto &label : labels)
    tabs->addTab(label);
  tabs->setFixedSize(248, 30);
  return tabs;
}

} // namespace original_ui
