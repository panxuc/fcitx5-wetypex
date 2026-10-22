#include "common/qt_helpers.hpp"
#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocalSocket>
#include <QPluginLoader>
#include <QPushButton>
#include <QResizeEvent>
#include <QSaveFile>
#include <QScreen>
#include <QScrollArea>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTabBar>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <limits>
#include <sys/file.h>
#include <unistd.h>
#ifdef WETYPE_LAYER_SHELL
#include <LayerShellQt/Window>
#include <wayland-client.h>
#endif

#ifdef WETYPE_LAYER_SHELL
static bool layerShellPluginAvailable() {
  for (const auto &path : QCoreApplication::libraryPaths()) {
    const QDir directory(path + "/wayland-shell-integration");
    for (const auto &file : directory.entryList(QDir::Files)) {
      QPluginLoader plugin(directory.filePath(file));
      const auto keys = plugin.metaData()
                            .value("MetaData")
                            .toObject()
                            .value("Keys")
                            .toArray();
      for (const auto &key : keys)
        if (key.toString() == "layer-shell" && plugin.load())
          return true;
    }
  }
  return false;
}
#endif

static bool prepareLayerShell() {
  const auto platform = qgetenv("QT_QPA_PLATFORM");
  const bool wayland =
      platform.startsWith("wayland") ||
      (platform.isEmpty() &&
       (!qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") ||
        qEnvironmentVariable("XDG_SESSION_TYPE") == "wayland"));
  if (!wayland)
    return false;
#ifdef WETYPE_LAYER_SHELL
  // Probe before selecting the Qt shell plugin: unsupported compositors must
  // not be forced to use layer-shell (e.g. GNOME's xdg-shell-only sessions).
  if (layerShellPluginAvailable()) {
    if (auto *display = wl_display_connect(nullptr)) {
      bool supported = false;
      auto *registry = wl_display_get_registry(display);
      static const wl_registry_listener listener{
          [](void *data, wl_registry *, uint32_t, const char *name, uint32_t) {
            if (QByteArray(name) == "zwlr_layer_shell_v1")
              *static_cast<bool *>(data) = true;
          },
          [](void *, wl_registry *, uint32_t) {}};
      wl_registry_add_listener(registry, &listener, &supported);
      const bool ready = wl_display_roundtrip(display) >= 0;
      wl_registry_destroy(registry);
      wl_display_disconnect(display);
      if (ready && supported) {
        qputenv("QT_WAYLAND_SHELL_INTEGRATION", "layer-shell");
        if (platform.isEmpty())
          qputenv("QT_QPA_PLATFORM", "wayland");
        return true;
      }
    }
  } else {
    std::fputs("WeTypeX: layer-shell Qt runtime plugin is not installed.\n",
               stderr);
  }
#endif
  std::fputs("WeTypeX: layer-shell unavailable; using the Fcitx tools menu.\n",
             stderr);
  return false;
}

static QString stateDir() {
  const auto override = qEnvironmentVariable("WETYPE_STATE_DIR");
  return override.isEmpty() ? QStandardPaths::writableLocation(
                                  QStandardPaths::GenericDataLocation) +
                                  "/fcitx5-wetypex/state"
                            : override;
}
static QString candidateIcon(const QString &name) {
  return QStandardPaths::locate(QStandardPaths::GenericDataLocation,
                                "fcitx5-wetypex/ui/candidate-icons/" + name +
                                    ".svg");
}
struct VPlacement {
  QPoint anchor;
  QScreen *screen = nullptr;
  QRect workArea;
  QRect clientArea;
  QString source;
};
static QJsonDocument hyprlandRequest(const QByteArray &command) {
  const auto signature = qEnvironmentVariable("HYPRLAND_INSTANCE_SIGNATURE");
  const auto runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
  if (signature.isEmpty() || runtime.isEmpty())
    return {};
  QLocalSocket socket;
  socket.connectToServer(runtime + "/hypr/" + signature + "/.socket.sock");
  if (!socket.waitForConnected(200))
    return {};
  socket.write(command);
  socket.flush();
  QByteArray bytes;
  for (int attempt = 0; attempt < 6; ++attempt) {
    if (socket.bytesAvailable() == 0 && !socket.waitForReadyRead(100)) {
      bytes += socket.readAll();
      break;
    }
    bytes += socket.readAll();
    if (socket.state() == QLocalSocket::UnconnectedState)
      break;
  }
  bytes += socket.readAll();
  return QJsonDocument::fromJson(bytes);
}
static QRect screenWorkArea(QScreen *screen, const QJsonArray &monitors) {
  QRect work = screen->geometry();
  for (const auto &value : monitors) {
    const auto monitor = value.toObject();
    if (monitor.value("name").toString() != screen->name())
      continue;
    const auto reserved = monitor.value("reserved").toArray();
    if (reserved.size() == 4)
      work.adjust(reserved[0].toInt(), reserved[1].toInt(),
                  -reserved[2].toInt(), -reserved[3].toInt());
    break;
  }
  if (work.isEmpty())
    return screen->geometry();
  const auto available = work.intersected(screen->availableGeometry());
  return available.isEmpty() ? work : available;
}
static QScreen *screenForPoint(const QPoint &point) {
  if (auto *screen = QGuiApplication::screenAt(point))
    return screen;
  QScreen *best = nullptr;
  qint64 bestDistance = std::numeric_limits<qint64>::max();
  for (auto *screen : QGuiApplication::screens()) {
    const auto geometry = screen->geometry();
    const QPoint inside(
        std::clamp(point.x(), geometry.left(), geometry.right()),
        std::clamp(point.y(), geometry.top(), geometry.bottom()));
    const qint64 dx = point.x() - inside.x();
    const qint64 dy = point.y() - inside.y();
    const qint64 distance = dx * dx + dy * dy;
    if (distance < bestDistance) {
      bestDistance = distance;
      best = screen;
    }
  }
  return best ? best : QGuiApplication::primaryScreen();
}
static VPlacement resolvePlacement(bool relative, int x, int y, double scale) {
  VPlacement placement;
  // A keyboard-inactive layer surface still shifts Hyprland's focus to the
  // window below the popup when the two do not overlap. Remember the focused
  // window so the toolbar can stay inside it and keep the client focused.
  const auto window = hyprlandRequest("j/activewindow").object();
  const auto at = window.value("at").toArray();
  const auto windowSize = window.value("size").toArray();
  if (at.size() == 2 && windowSize.size() == 2) {
    placement.clientArea =
        QRect(QPoint(qRound(at[0].toDouble()), qRound(at[1].toDouble())),
              QSize(windowSize[0].toInt(), windowSize[1].toInt()));
  }
  const auto placeAtCursor = [&placement] {
    const auto cursor = hyprlandRequest("j/cursorpos").object();
    if (!cursor.contains("x") || !cursor.contains("y"))
      return false;
    placement.anchor = QPoint(cursor.value("x").toInt() + 12,
                              cursor.value("y").toInt() + 24);
    placement.source = QStringLiteral("hyprland cursor");
    return true;
  };
  if (relative) {
    // Wayland cursor rectangles are surface-relative. Hyprland is the only
    // compositor that exposes the focused window origin without extra
    // protocols, so resolve the surface-local point through its IPC.
    if (at.size() == 2) {
      placement.anchor = QPoint(qRound(at[0].toDouble() + x / scale),
                                qRound(at[1].toDouble() + y / scale));
      placement.source = QStringLiteral("hyprland active window");
    } else if (!placeAtCursor()) {
      placement.anchor = QPoint(x, y);
      placement.source = QStringLiteral("unresolved relative rect");
    }
  } else if (x == 0 && y == 0) {
    // Electron clients can leave the cursor rect empty. Treating that as a
    // global caret at the origin pins the toolbar to the top-left corner.
    if (!placeAtCursor()) {
      placement.anchor = QPoint(x, y);
      placement.source = QStringLiteral("empty rect");
    }
  } else {
    placement.anchor = QPoint(x, y);
    placement.source = QStringLiteral("global rect");
  }
  placement.screen = screenForPoint(placement.anchor);
  if (placement.screen)
    placement.workArea = screenWorkArea(
        placement.screen, hyprlandRequest("j/monitors").array());
  return placement;
}
static QPoint toolbarTopLeft(const VPlacement &placement, const QSize &size) {
  if (!placement.screen)
    return placement.anchor;
  constexpr int Margin = 4;
  constexpr int Gap = 6;
  constexpr int CaretHeight = 26;
  QRect bounds = placement.workArea;
  const auto client = placement.clientArea.intersected(bounds);
  if (client.width() >= size.width() && client.height() >= size.height())
    bounds = client;
  int left = placement.anchor.x();
  int top = placement.anchor.y() + Gap;
  if (top + size.height() > bounds.top() + bounds.height() - Margin)
    top = placement.anchor.y() - size.height() - CaretHeight;
  const int minLeft = bounds.left() + Margin;
  const int minTop = bounds.top() + Margin;
  const int maxLeft = bounds.left() + bounds.width() - size.width() - Margin;
  const int maxTop = bounds.top() + bounds.height() - size.height() - Margin;
  left = std::clamp(left, minLeft, std::max(minLeft, maxLeft));
  top = std::clamp(top, minTop, std::max(minTop, maxTop));
  return QPoint(left, top);
}
static void clearLayout(QLayout *layout) {
  while (auto *item = layout->takeAt(0)) {
    if (auto *widget = item->widget()) {
      widget->hide();
      widget->deleteLater();
    } else if (auto *child = item->layout()) {
      clearLayout(child);
    }
    delete item;
  }
}
static bool sendAction(quint64 session, quint64 epoch, const QString &token,
                       const QString &action, const QString &text = {}) {
  QDir().mkpath(stateDir());
  QSaveFile file(stateDir() + "/vmode-action.json");
  if (!file.open(QIODevice::WriteOnly))
    return false;
  QJsonObject value{{"version", QDateTime::currentMSecsSinceEpoch()},
                    {"session", static_cast<qint64>(session)},
                    {"epoch", static_cast<qint64>(epoch)},
                    {"token", token},
                    {"action", action},
                    {"text", text}};
  const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Compact);
  if (file.write(bytes) != bytes.size())
    return false;
  file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  return file.commit();
}
class VModeWindow : public QWidget {
  quint64 session_, epoch_;
  QString token_;
  bool actionSent_ = false;
  VPlacement placement_;
#ifdef WETYPE_LAYER_SHELL
  LayerShellQt::Window *layerSurface_ = nullptr;
#endif

public:
  VModeWindow(quint64 session, quint64 epoch, QString token)
      : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint |
                             Qt::WindowStaysOnTopHint |
                             Qt::WindowDoesNotAcceptFocus),
        session_(session), epoch_(epoch), token_(std::move(token)) {
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFocusPolicy(Qt::NoFocus);
  }
  void finish(const QString &action, const QString &text = {}) {
    if (sendAction(session_, epoch_, token_, action, text)) {
      actionSent_ = true;
      close();
    }
  }
  void setPlacement(VPlacement placement) {
    placement_ = std::move(placement);
    applyPlacement();
  }
  void applyPlacement() {
    if (!placement_.screen || !windowHandle())
      return;
    const auto topLeft = toolbarTopLeft(placement_, size());
#ifdef WETYPE_LAYER_SHELL
    if (layerSurface_) {
      const auto local = topLeft - placement_.screen->geometry().topLeft();
      layerSurface_->setMargins(QMargins(std::max(0, local.x()),
                                         std::max(0, local.y()), 0, 0));
      return;
    }
#endif
    move(topLeft);
  }
#ifdef WETYPE_LAYER_SHELL
  void setLayerSurface(LayerShellQt::Window *surface) {
    layerSurface_ = surface;
    applyPlacement();
  }
#endif

protected:
  void closeEvent(QCloseEvent *event) override {
    if (!actionSent_)
      sendAction(session_, epoch_, token_, "cancel");
    QWidget::closeEvent(event);
    // Tool windows do not necessarily trigger quitOnLastWindowClosed.
    QCoreApplication::quit();
  }
  void resizeEvent(QResizeEvent *event) override {
    QWidget::resizeEvent(event);
    applyPlacement();
  }
};
static QJsonArray hotwords() {
  QLocalSocket socket;
  socket.connectToServer(stateDir() + "/control.sock");
  if (!socket.waitForConnected(1000))
    return {};
  QJsonObject request{{"session", 1},
                      {"seq", QDateTime::currentMSecsSinceEpoch()},
                      {"epoch", 1},
                      {"op", "hotword_list"}};
  return wetype::localRequest(socket, request, 2000)
      .value("hotwords")
      .toArray();
}
static QPushButton *menuButton(const QString &title, const QString &icon) {
  auto *button = new QPushButton(title);
  button->setFocusPolicy(Qt::NoFocus);
  button->setIcon(QIcon(candidateIcon(icon)));
  button->setIconSize({22, 22});
  button->setFixedSize(96, 58);
  return button;
}
int main(int argc, char **argv) {
  const bool layerShell = prepareLayerShell();
  const auto platform = qgetenv("QT_QPA_PLATFORM");
  const bool wayland =
      platform.startsWith("wayland") ||
      (platform.isEmpty() &&
       (!qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") ||
        qEnvironmentVariable("XDG_SESSION_TYPE") == "wayland"));
  // Tell the input-method plugin to use its own candidate surface. Never map
  // an ordinary Wayland toplevel that could steal the original client's focus.
  if (wayland && !layerShell)
    return 69;
  QApplication app(argc, argv);
  app.setApplicationName("fcitx5-wetypex-vmode");
  app.setDesktopFileName(QStringLiteral("fcitx5-wetypex-vmode"));
  std::fprintf(stderr, "WeTypeX V toolbar: %s%s\n",
               app.platformName().toUtf8().constData(),
               layerShell ? " / layer-shell, keyboard focus disabled" : "");
  if (!QDir().mkpath(stateDir()))
    return 73;
  // Keep the inode in place. Kernel ownership, rather than a PID saved on
  // disk, determines whether an instance is alive, including after SIGKILL.
  QFile lock(stateDir() + "/vmode.lock");
  const int fd = ::open(QFile::encodeName(lock.fileName()).constData(),
                        O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) {
    std::perror("WeTypeX V toolbar lock");
    return 73;
  }
  if (!lock.open(fd, QIODevice::ReadWrite, QFileDevice::AutoCloseHandle)) {
    ::close(fd);
    return 73;
  }
  int locked;
  do {
    locked = ::flock(fd, LOCK_EX | LOCK_NB);
  } while (locked < 0 && errno == EINTR);
  if (locked < 0) {
    if (errno == EWOULDBLOCK || errno == EAGAIN)
      return 0;
    std::perror("WeTypeX V toolbar flock");
    return 73;
  }
  // Retain QLockFile's metadata layout so a running older installation also
  // respects this lock, and can recover it after a downgrade or forced exit.
  const auto info = QByteArray::number(app.applicationPid()) + '\n' +
                    app.applicationName().toUtf8() + '\n' +
                    QSysInfo::machineHostName().toUtf8() + '\n' +
                    QSysInfo::machineUniqueId() + '\n' +
                    QSysInfo::bootUniqueId() + '\n';
  if (!lock.resize(0) || lock.write(info) != info.size()) {
    std::fputs("WeTypeX: cannot write V toolbar lock metadata.\n", stderr);
    return 73;
  }
  lock.flush();
  quint64 session =
      argc > 1 ? QString::fromLocal8Bit(argv[1]).toULongLong() : 0;
  int x = argc > 2 ? QString::fromLocal8Bit(argv[2]).toInt() : 0;
  int y = argc > 3 ? QString::fromLocal8Bit(argv[3]).toInt() : 0;
  const auto epoch = argc > 4 ? QString::fromLocal8Bit(argv[4]).toULongLong() : 0;
  const auto token = argc > 5 ? QString::fromLocal8Bit(argv[5]) : QString();
  const bool relative = argc > 6 && QByteArray(argv[6]) == "1";
  double scale = argc > 7 ? QString::fromLocal8Bit(argv[7]).toDouble() : 1.0;
  if (!(scale > 0.0))
    scale = 1.0;
  VModeWindow window(session, epoch, token);
  const auto placement = resolvePlacement(relative, x, y, scale);
  window.setPlacement(placement);
#ifdef WETYPE_LAYER_SHELL
  if (layerShell && app.platformName().startsWith("wayland")) {
    window.winId();
    auto *screen =
        placement.screen ? placement.screen : app.primaryScreen();
    if (!screen)
      return 69;
    window.windowHandle()->setScreen(screen);
    auto *surface = LayerShellQt::Window::get(window.windowHandle());
    surface->setScope(QStringLiteral("fcitx5-wetypex-vmode"));
    surface->setLayer(LayerShellQt::Window::LayerOverlay);
    surface->setKeyboardInteractivity(
        LayerShellQt::Window::KeyboardInteractivityNone);
    surface->setAnchors(
        LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop) |
        LayerShellQt::Window::AnchorLeft);
    surface->setExclusiveZone(-1);
    window.setLayerSurface(surface);
  }
#else
  Q_UNUSED(layerShell);
#endif
  window.setWindowTitle("WeTypeX 快捷工具");
  window.setStyleSheet(
      "QWidget{background:#f7f7f7;color:#202124;font:14px 'Noto Sans CJK SC';}"
      "QPushButton{background:white;border:1px solid "
      "#e7e7e7;border-radius:8px;padding:6px;}"
      "QPushButton:hover{border-color:#23c891;background:#f2fffa;}"
      "QTabWidget::pane{border:0;} QTabBar::tab:selected{color:#23c891;}");
  auto *outer = new QVBoxLayout(&window);
  outer->setContentsMargins(8, 8, 8, 8);
  outer->setSpacing(6);
  auto *header = new QHBoxLayout;
  header->addWidget(new QLabel("快捷工具"));
  header->addStretch();
  auto *close = new QPushButton("关闭");
  close->setFocusPolicy(Qt::NoFocus);
  QObject::connect(close, &QPushButton::clicked, &window, &QWidget::close);
  header->addWidget(close);
  outer->addLayout(header);
  auto *body = new QWidget;
  auto *root = new QVBoxLayout(body);
  root->setContentsMargins(0, 0, 0, 0);
  outer->addWidget(body);
  auto showList = [&](const QString &title,
                      const QList<QPair<QString, QString>> &items) {
    clearLayout(root);
    auto *heading = new QLabel(title);
    root->addWidget(heading);
    auto *list = new QWidget;
    auto *layout = new QVBoxLayout(list);
    layout->setContentsMargins(0, 0, 0, 0);
    for (const auto &[label, text] : items) {
      auto *button = new QPushButton(label);
      button->setFocusPolicy(Qt::NoFocus);
      QObject::connect(button, &QPushButton::clicked, &window, [&, text] {
        window.finish("commit", text);
      });
      layout->addWidget(button);
    }
    layout->addStretch();
    auto *scroll = new QScrollArea;
    scroll->setFocusPolicy(Qt::NoFocus);
    scroll->setWidgetResizable(true);
    scroll->setWidget(list);
    root->addWidget(scroll);
    window.resize(430, 330);
  };
  auto *menu = new QHBoxLayout;
  auto *calculator = menuButton("计算", "icon_keybar_calculator");
  auto *clipboard = menuButton("剪贴板", "icon_keybar_clipboard");
  auto *phrases = menuButton("常用语", "icon_keybar_changyongyu");
  auto *symbols = menuButton("符号", "icon_keybar_symbol");
  for (auto *button : {calculator, clipboard, phrases, symbols})
    menu->addWidget(button);
  root->addLayout(menu);
  QObject::connect(calculator, &QPushButton::clicked, &window, [&] {
    window.finish("calculator");
  });
  QObject::connect(clipboard, &QPushButton::clicked, &window, [&] {
    QList<QPair<QString, QString>> items;
    QFile file(stateDir() + "/clipboard-history.json");
    if (file.open(QIODevice::ReadOnly))
      for (const auto &v : QJsonDocument::fromJson(file.readAll()).array())
        items.append({v.toString().left(60), v.toString()});
    showList("剪贴板", items);
  });
  QObject::connect(phrases, &QPushButton::clicked, &window, [&] {
    QList<QPair<QString, QString>> items;
    for (const auto &v : hotwords()) {
      auto o = v.toObject();
      items.append({o.value("words").toString(), o.value("words").toString()});
    }
    showList("常用语", items);
  });
  QObject::connect(symbols, &QPushButton::clicked, &window, [&] {
    clearLayout(root);
    QFile file(QStandardPaths::locate(QStandardPaths::GenericDataLocation,
                                      "fcitx5-wetypex/wetypex-symbols.json"));
    if (!file.open(QIODevice::ReadOnly))
      return;
    auto *tabs = new QTabWidget;
    tabs->setFocusPolicy(Qt::NoFocus);
    tabs->tabBar()->setFocusPolicy(Qt::NoFocus);
    for (const auto &groupValue :
         QJsonDocument::fromJson(file.readAll()).array()) {
      auto group = groupValue.toObject();
      auto *body = new QWidget;
      auto *grid = new QGridLayout(body);
      int n = 0;
      for (const auto &subValue : group.value("groupData").toArray())
        for (const auto &rowValue :
             subValue.toObject().value("subGroupData").toArray())
          for (const auto &symbolValue : rowValue.toArray()) {
            QString text = symbolValue.toString();
            auto *button = new QPushButton(text);
            button->setFocusPolicy(Qt::NoFocus);
            button->setFixedSize(38, 32);
            QObject::connect(button, &QPushButton::clicked, &window, [&, text] {
              window.finish("commit", text);
            });
            grid->addWidget(button, n / 9, n % 9);
            ++n;
          }
      auto *scroll = new QScrollArea;
      scroll->setFocusPolicy(Qt::NoFocus);
      scroll->setWidgetResizable(true);
      scroll->setWidget(body);
      const auto iconName =
          QFileInfo(group.value("iconPath").toString()).baseName();
      tabs->addTab(scroll, QIcon(candidateIcon(iconName)),
                   group.value("groupName").toString());
    }
    root->addWidget(tabs);
    window.resize(450, 360);
  });
  window.adjustSize();
  window.applyPlacement();
  const auto topLeft = toolbarTopLeft(placement, window.size());
  std::fprintf(stderr,
               "WeTypeX V toolbar placement: source=%s anchor=%d,%d screen=%s "
               "work=%d,%d %dx%d client=%d,%d %dx%d at=%d,%d size=%dx%d "
               "relative=%d scale=%.2f\n",
               placement.source.toUtf8().constData(), placement.anchor.x(),
               placement.anchor.y(),
               placement.screen ? placement.screen->name().toUtf8().constData()
                                : "(none)",
               placement.workArea.x(), placement.workArea.y(),
               placement.workArea.width(), placement.workArea.height(),
               placement.clientArea.x(), placement.clientArea.y(),
               placement.clientArea.width(), placement.clientArea.height(),
               topLeft.x(), topLeft.y(), window.width(), window.height(),
               int(relative), scale);
  window.show();
  return app.exec();
}
