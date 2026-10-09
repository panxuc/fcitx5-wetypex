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
#include <QLockFile>
#include <QLocalSocket>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTabBar>
#include <QVBoxLayout>
#include <QWidget>

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

protected:
  void closeEvent(QCloseEvent *event) override {
    if (!actionSent_)
      sendAction(session_, epoch_, token_, "cancel");
    QWidget::closeEvent(event);
    // Tool windows do not necessarily trigger quitOnLastWindowClosed.
    QCoreApplication::quit();
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
  QApplication app(argc, argv);
  app.setApplicationName("fcitx5-wetypex-vmode");
  QDir().mkpath(stateDir());
  QLockFile lock(stateDir() + "/vmode.lock");
  lock.setStaleLockTime(0);
  if (!lock.tryLock())
    return 0;
  quint64 session =
      argc > 1 ? QString::fromLocal8Bit(argv[1]).toULongLong() : 0;
  int x = argc > 2 ? QString::fromLocal8Bit(argv[2]).toInt() : 0;
  int y = argc > 3 ? QString::fromLocal8Bit(argv[3]).toInt() : 0;
  const auto epoch = argc > 4 ? QString::fromLocal8Bit(argv[4]).toULongLong() : 0;
  const auto token = argc > 5 ? QString::fromLocal8Bit(argv[5]) : QString();
  VModeWindow window(session, epoch, token);
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
  window.move(x, y);
  window.show();
  return app.exec();
}
