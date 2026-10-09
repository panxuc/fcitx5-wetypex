#include "../common/qt_helpers.hpp"
#include "original_windows.hpp"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDBusInterface>
#include <QDBusPendingCall>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QLocalSocket>
#include <QMessageBox>
#include <QMovie>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <functional>
#include <tuple>

using namespace original_ui;

static QString configDir() {
  return QStandardPaths::writableLocation(
             QStandardPaths::GenericConfigLocation) +
         "/fcitx5";
}
static QJsonObject readSettings() {
  const QString current = configDir() + "/wetypex.json";
  QFile file(current);
  if (!file.open(QIODevice::ReadOnly))
    return {};
  return QJsonDocument::fromJson(file.readAll()).object();
}
static QJsonObject readJsonObject(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return {};
  return QJsonDocument::fromJson(file.readAll()).object();
}
static void runAccount(QWidget *owner, const QStringList &arguments,
                       std::function<void(int, const QJsonObject &)> done) {
  auto *process = new QProcess(owner);
  process->setProgram(
      qEnvironmentVariable("WETYPE_ACCOUNT_TOOL", WETYPE_ACCOUNT_TOOL));
  process->setArguments(arguments);
  wetype::onProcessDone(
      process, owner,
      [process, done = std::move(done)](int code, QProcess::ExitStatus) {
        auto result =
            QJsonDocument::fromJson(process->readAllStandardOutput()).object();
        done(code, result);
        process->deleteLater();
      });
  process->start();
}
static bool writeFile(const QString &name, const QByteArray &bytes) {
  QDir().mkpath(QFileInfo(name).absolutePath());
  QSaveFile file(name);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() &&
         file.commit();
}
static bool writeSetting(const QString &key, const QJsonValue &value) {
  auto settings = readSettings();
  settings[key] = value;
  if (!writeFile(configDir() + "/wetypex.json",
                 QJsonDocument(settings).toJson()))
    return false;
  QDBusInterface controller("org.fcitx.Fcitx5", "/controller",
                            "org.fcitx.Fcitx.Controller1");
  controller.asyncCall("ReloadAddonConfig", "wetypex");
  return true;
}
static QJsonObject engineControl(const QJsonObject &request,
                                 QString *error = nullptr) {
  QLocalSocket socket;
  socket.connectToServer(
      QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
      "/fcitx5-wetypex/state/control.sock");
  if (!socket.waitForConnected(1500)) {
    // The official settings app can edit common phrases even before its input
    // service is active. Use the same engine protocol in a short-lived process
    // when the resident Fcitx addon has no control socket yet.
    QProcess backend;
    backend.setProgram(qEnvironmentVariable("WETYPE_BACKEND_LAUNCHER",
                                            QStringLiteral(WETYPE_BACKEND)));
    backend.start();
    if (!backend.waitForStarted(3000)) {
      if (error)
        *error = "输入核心无法启动";
      return {};
    }
    QElapsedTimer deadline;
    deadline.start();
    QByteArray pending;
    bool ready = false;
    while (deadline.elapsed() < 180000 &&
           backend.state() != QProcess::NotRunning) {
      if (!backend.waitForReadyRead(
              qMin(1000, 180000 - int(deadline.elapsed()))))
        continue;
      pending += backend.readAllStandardOutput();
      qsizetype newline = -1;
      while ((newline = pending.indexOf('\n')) >= 0) {
        const auto line = pending.left(newline);
        pending.remove(0, newline + 1);
        if (QJsonDocument::fromJson(line).object().value("event") == "ready") {
          ready = true;
          break;
        }
      }
      if (ready)
        break;
    }
    if (!ready) {
      backend.kill();
      backend.waitForFinished();
      if (error)
        *error = "输入核心初始化失败";
      return {};
    }
    backend.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
    backend.waitForBytesWritten(3000);
    QJsonObject object;
    deadline.restart();
    while (deadline.elapsed() < 10000 &&
           backend.state() != QProcess::NotRunning) {
      if (!backend.waitForReadyRead(qMin(500, 10000 - int(deadline.elapsed()))))
        continue;
      pending += backend.readAllStandardOutput();
      const auto newline = pending.indexOf('\n');
      if (newline < 0)
        continue;
      object = QJsonDocument::fromJson(pending.left(newline)).object();
      break;
    }
    backend.write("{\"op\":\"quit\",\"session\":1,\"seq\":0}\n");
    backend.waitForFinished(3000);
    if (backend.state() != QProcess::NotRunning) {
      backend.kill();
      backend.waitForFinished();
    }
    if (object.isEmpty() && error)
      *error = "输入核心没有响应";
    else if (object.contains("error") && error)
      *error = object.value("error").toString();
    return object;
  }
  const auto object = wetype::localRequest(socket, request, 3000);
  if (object.contains("error") && error)
    *error = object.value("error").toString();
  return object;
}

static QJsonObject hotwordRequest(const QString &operation,
                                  const QJsonObject &fields = {},
                                  QString *error = nullptr) {
  QJsonObject request = fields;
  request["session"] = 1;
  request["seq"] = QDateTime::currentMSecsSinceEpoch();
  request["epoch"] = 1;
  request["op"] = operation;
  return engineControl(request, error);
}

static bool editHotword(QWidget *owner, const QJsonObject &existing = {}) {
  QDialog dialog(owner, Qt::Dialog | Qt::FramelessWindowHint);
  dialog.setModal(true);
  dialog.setObjectName("windowSurface");
  dialog.setFixedSize(430, 285);
  auto *layout = new QVBoxLayout(&dialog);
  layout->setContentsMargins(22, 16, 22, 20);
  auto *title = new QLabel(existing.isEmpty() ? "添加常用语" : "编辑常用语");
  title->setObjectName("pageHeading");
  title->setAlignment(Qt::AlignCenter);
  layout->addWidget(title);
  layout->addWidget(new QLabel("常用语内容"));
  auto *words = new QPlainTextEdit(existing.value("words").toString());
  words->setFixedHeight(92);
  layout->addWidget(words);
  layout->addWidget(new QLabel("输入码（汉字、拼音或首字母）"));
  auto *key = new QLineEdit(existing.value("key").toString());
  layout->addWidget(key);
  auto *buttons = new QHBoxLayout;
  buttons->addStretch();
  auto *cancel = new QPushButton("取消"), *save = greenButton("保存");
  buttons->addWidget(cancel);
  buttons->addWidget(save);
  layout->addLayout(buttons);
  QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
  QObject::connect(save, &QPushButton::clicked, [&] {
    const auto text = words->toPlainText().trimmed();
    const auto shortcut = key->text().trimmed();
    if (text.isEmpty() || shortcut.isEmpty()) {
      QMessageBox::warning(&dialog, "常用语", "内容和输入码不能为空");
      return;
    }
    QString error;
    QJsonObject fields{{"id", existing.value("id").toString(QString::number(
                                  QDateTime::currentMSecsSinceEpoch()))},
                       {"key", shortcut},
                       {"words", text}};
    hotwordRequest("hotword_set", fields, &error);
    if (!error.isEmpty()) {
      QMessageBox::warning(&dialog, "常用语", error);
      return;
    }
    dialog.accept();
  });
  return dialog.exec() == QDialog::Accepted;
}
static QMap<QString, QString> readAppearance() {
  QMap<QString, QString> result;
  QFile file(configDir() + "/conf/classicui.conf");
  if (!file.open(QIODevice::ReadOnly))
    return result;
  for (const auto &line : QString::fromUtf8(file.readAll()).split('\n')) {
    if (line.startsWith('#'))
      continue;
    int equals = line.indexOf('=');
    if (equals <= 0)
      continue;
    auto value = line.mid(equals + 1).trimmed();
    if (value.startsWith('"') && value.endsWith('"'))
      value = value.mid(1, value.size() - 2);
    result[line.left(equals).trimmed()] = value;
  }
  return result;
}
static void setIni(QString &text, const QString &key, const QString &value) {
  QRegularExpression expression("^" + QRegularExpression::escape(key) + "=.*$",
                                QRegularExpression::MultilineOption);
  if (text.contains(expression))
    text.replace(expression, key + "=" + value);
  else
    text += key + "=" + value + "\n";
}
static QFrame *rowsCard(const QList<QWidget *> &rows) {
  auto *frame = card();
  auto *layout = new QVBoxLayout(frame);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  for (int i = 0; i < rows.size(); ++i) {
    layout->addWidget(rows[i]);
    if (i + 1 < rows.size())
      layout->addWidget(separator());
  }
  return frame;
}
static QLabel *iconLabel(const QString &name, int size = 20) {
  auto *label = new QLabel;
  label->setFixedSize(size, size);
  label->setPixmap(tinted(name, QColor("#263b40"), size));
  label->setAlignment(Qt::AlignCenter);
  label->setStyleSheet("background:transparent;");
  return label;
}
static QLabel *imageIconLabel(const QString &name, int size = 20) {
  auto *label = new QLabel;
  label->setFixedSize(size, size);
  label->setPixmap(
      QPixmap(imageAsset(name))
          .scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
  label->setAlignment(Qt::AlignCenter);
  label->setStyleSheet("background:transparent;");
  return label;
}
static QWidget *iconRow(const QString &icon, const QString &title,
                        const QString &subtitle = {},
                        QWidget *trailing = nullptr, int height = 62) {
  auto *widget = new QWidget;
  widget->setFixedHeight(height);
  auto *layout = new QHBoxLayout(widget);
  layout->setContentsMargins(15, 0, 15, 0);
  layout->setSpacing(14);
  layout->addWidget(icon.endsWith(".png") ? imageIconLabel(icon)
                                          : iconLabel(icon));
  auto *labels = new QVBoxLayout;
  labels->setSpacing(2);
  labels->addStretch();
  auto *name = new QLabel(title);
  name->setObjectName("rowTitle");
  labels->addWidget(name);
  if (!subtitle.isEmpty()) {
    auto *detail = new QLabel(subtitle);
    detail->setObjectName("rowSubtitle");
    labels->addWidget(detail);
  }
  labels->addStretch();
  layout->addLayout(labels, 1);
  if (trailing)
    layout->addWidget(trailing, 0, Qt::AlignVCenter);
  return widget;
}
static Toggle *toggle(bool checked, bool implemented, const QString &key = {}) {
  auto *control = new Toggle(checked);
  if (!implemented) {
    control->setEnabled(false);
    control->setToolTip("原版执行路径适配中；当前保持原版默认值，只读");
  } else if (!key.isEmpty()) {
    QObject::connect(control, &QAbstractButton::toggled,
                     [key](bool value) { writeSetting(key, value); });
  }
  return control;
}

class PairingOverlay final : public QWidget {
  enum class Stage { Code, Status, Confirm };
  QProcess process_;
  QTimer poll_;
  QLabel *subtitle_ = nullptr;
  QLabel *message_ = nullptr;
  QLabel *digits_[6]{};
  QString code_;
  Stage stage_ = Stage::Code;
  int transientFailures_ = 0;

  void showFailure(const QString &text) {
    subtitle_->setText("暂时无法获取匹配码");
    message_->setText(text);
    for (auto *digit : digits_)
      digit->clear();
  }
  void start(const QStringList &arguments) {
    if (process_.state() != QProcess::NotRunning)
      return;
    process_.setProgram(
        qEnvironmentVariable("WETYPE_ACCOUNT_TOOL", WETYPE_ACCOUNT_TOOL));
    process_.setArguments(arguments);
    process_.start();
  }
  void finish(int exitCode) {
    const auto document =
        QJsonDocument::fromJson(process_.readAllStandardOutput());
    const auto result = document.object();
    if (exitCode || !result.value("ok").toBool()) {
      if (stage_ == Stage::Status && !code_.isEmpty()) {
        // Status checks share the original account transport with background
        // sync. A temporary lock or network failure must not erase a code
        // that remains valid on the server.
        ++transientFailures_;
        poll_.start(qMin(5000, 1000 + transientFailures_ * 500));
      } else {
        showFailure("请检查网络连接后重试");
      }
      return;
    }
    if (stage_ == Stage::Code) {
      code_ = result.value("match_code").toString();
      if (code_.size() != 6) {
        showFailure("服务返回了无效匹配码");
        return;
      }
      for (int i = 0; i < 6; ++i)
        digits_[i]->setText(code_.mid(i, 1));
      subtitle_->setText("匹配码5分钟内有效");
      message_->setText(
          "请在你的另一台设备中进入「手机端微信输入法设置 → 跨设备\n"
          "→ 粘贴传送 → 关联设备」输入上方匹配码关联");
      transientFailures_ = 0;
      poll_.start(1500);
      return;
    }
    if (stage_ == Stage::Status) {
      const int status = result.value("status").toInt();
      if (status == 0) {
        transientFailures_ = 0;
        poll_.start(1500);
        return;
      }
      if (status == 1 && !result.value("bind_id").toString().isEmpty()) {
        if (result.value("show_reconfirm").toBool()) {
          // The reconfirm layout has not been observed yet. Keep the real
          // server state intact instead of silently accepting a rebind.
          subtitle_->setText("请在另一台设备确认关联");
          message_->setText("确认后，本机将完成设备关联");
          poll_.start(1500);
          return;
        }
        stage_ = Stage::Confirm;
        subtitle_->setText("正在关联设备…");
        start({"confirm-bind", result.value("bind_id").toString()});
        return;
      }
      showFailure(status == 2 ? "另一台设备取消了关联"
                              : "匹配码已失效，请关闭后重新获取");
      return;
    }
    subtitle_->setText("设备关联成功");
    message_->setText("现在可以在关联设备间使用同步功能");
    QTimer::singleShot(900, this, [this] { deleteLater(); });
  }

public:
  explicit PairingOverlay(QWidget *parent) : QWidget(parent), process_(this) {
    setGeometry(parent->rect());
    setObjectName("pairingOverlay");
    setStyleSheet("QWidget#pairingOverlay{background:rgba(0,0,0,72);}");
    auto *dialog = new QFrame(this);
    dialog->setObjectName("pairingDialog");
    dialog->setGeometry(149, 144, 420, 280);
    dialog->setStyleSheet(
        "QFrame#pairingDialog{background:white;border:0;border-radius:12px;}"
        "QLabel{background:transparent;}"
        "QLabel#pairingTitle{font-size:18px;color:#171717;}"
        "QLabel#pairingSubtitle{font-size:14px;color:#aaa;}"
        "QLabel#pairingDigit{font-size:32px;color:#23c891;background:#fafafa;"
        "border-radius:8px;}"
        "QLabel#pairingMessage{font-size:14px;color:#aaa;}");
    auto *close = new SourceIconButton(asset("icon_windows_close_btn"), dialog);
    close->setGeometry(384, 0, 46, 28);
    QObject::connect(close, &QAbstractButton::clicked, this,
                     &QWidget::deleteLater);
    auto *title = new QLabel("本机匹配码", dialog);
    title->setObjectName("pairingTitle");
    title->setAlignment(Qt::AlignCenter);
    title->setGeometry(70, 45, 280, 28);
    subtitle_ = new QLabel("正在获取匹配码…", dialog);
    subtitle_->setObjectName("pairingSubtitle");
    subtitle_->setAlignment(Qt::AlignCenter);
    subtitle_->setGeometry(70, 72, 280, 22);
    for (int i = 0; i < 6; ++i) {
      digits_[i] = new QLabel(dialog);
      digits_[i]->setObjectName("pairingDigit");
      digits_[i]->setAlignment(Qt::AlignCenter);
      digits_[i]->setGeometry(47 + i * 56, 112, 48, 65);
    }
    message_ =
        new QLabel("请在你的另一台设备中进入「手机端微信输入法设置 → 跨设备\n"
                   "→ 粘贴传送 → 关联设备」输入上方匹配码关联",
                   dialog);
    message_->setObjectName("pairingMessage");
    message_->setAlignment(Qt::AlignCenter);
    message_->setGeometry(36, 205, 348, 48);
    close->raise();
    close->show();
    QObject::connect(
        &process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
        this, [this](int code, QProcess::ExitStatus) { finish(code); });
    QObject::connect(&process_, &QProcess::errorOccurred, this,
                     [this](QProcess::ProcessError error) {
                       if (error == QProcess::FailedToStart)
                         showFailure("账户服务无法启动，请检查安装后重试");
                     });
    QObject::connect(&poll_, &QTimer::timeout, this, [this] {
      poll_.stop();
      stage_ = Stage::Status;
      start({"pairing-status", code_});
    });
    raise();
    setFocusPolicy(Qt::StrongFocus);
    setFocus();
    show();
    start({"pairing-code"});
  }

protected:
  void keyPressEvent(QKeyEvent *event) override {
    if (event->key() == Qt::Key_Escape) {
      deleteLater();
      event->accept();
      return;
    }
    QWidget::keyPressEvent(event);
  }
};
static QWidget *radioRow(QRadioButton *radio, QWidget *trailing = nullptr) {
  auto *widget = new QWidget;
  widget->setFixedHeight(46);
  auto *layout = new QHBoxLayout(widget);
  layout->setContentsMargins(15, 0, 15, 0);
  layout->addWidget(radio);
  layout->addStretch();
  if (trailing)
    layout->addWidget(trailing);
  return widget;
}
static QWidget *shortcutRow(const QString &title, bool checked,
                            const QStringList &keys,
                            const QString &subtitle = {},
                            bool implemented = false,
                            const QString &setting = {}) {
  auto *container = new QWidget;
  container->setFixedHeight(subtitle.isEmpty() ? 48 : 62);
  auto *layout = new QHBoxLayout(container);
  layout->setContentsMargins(15, 0, 15, 0);
  auto *check = new Check;
  check->setChecked(checked);
  check->setEnabled(implemented);
  if (!implemented)
    check->setToolTip("原版快捷键尚未接入 Fcitx5，当前只读");
  layout->addWidget(check);
  auto *labels = new QVBoxLayout;
  labels->setSpacing(1);
  labels->addStretch();
  auto *name = new QLabel(title);
  name->setObjectName("rowTitle");
  labels->addWidget(name);
  if (!subtitle.isEmpty()) {
    auto *detail = new QLabel(subtitle);
    detail->setObjectName("rowSubtitle");
    labels->addWidget(detail);
  }
  labels->addStretch();
  layout->addLayout(labels, 1);
  for (const auto &key : keys)
    layout->addWidget(keyCap(key));
  if (implemented && !setting.isEmpty())
    QObject::connect(check, &QCheckBox::toggled,
                     [setting](bool value) { writeSetting(setting, value); });
  return container;
}
static void applyClassicTheme(int mode, int size, QLabel *status) {
  QString path = configDir() + "/conf/classicui.conf";
  QString backup = configDir() + "/wetype-appearance-backup.json";
  QFile file(path);
  QString text;
  if (file.open(QIODevice::ReadOnly))
    text = QString::fromUtf8(file.readAll());
  if (!QFile::exists(backup)) {
    QJsonObject object;
    object["existed"] = QFile::exists(path);
    object["content"] = text;
    if (!writeFile(backup, QJsonDocument(object).toJson())) {
      status->setText("无法备份原主题，未更改配置");
      return;
    }
  }
  setIni(text, "Theme", mode == 2 ? "wetypex-dark" : "wetypex-light");
  setIni(text, "DarkTheme", "wetypex-dark");
  setIni(text, "UseDarkTheme", mode == 0 ? "True" : "False");
  setIni(text, "UseAccentColor", "False");
  setIni(text, "PreferTextIcon", "False");
  setIni(text, "Font", "Noto Sans CJK SC " + QString::number(size));
  if (!writeFile(path, text.toUtf8())) {
    status->setText("保存外观失败");
    return;
  }
  QDBusInterface controller("org.fcitx.Fcitx5", "/controller",
                            "org.fcitx.Fcitx.Controller1");
  auto reply = controller.call("ReloadAddonConfig", "classicui");
  status->setText(reply.type() == QDBusMessage::ErrorMessage
                      ? "配置已保存，Fcitx5 下次启动时生效"
                      : "外观已应用");
}

using ToggleSpec = std::tuple<QString, QString, bool>;
static void runToggleDialog(QWidget *owner, const QString &title,
                            const QList<ToggleSpec> &options) {
  QDialog dialog(owner, Qt::Dialog | Qt::FramelessWindowHint);
  dialog.setModal(true);
  dialog.setObjectName("windowSurface");
  dialog.setFixedSize(
      430, std::min(540, 86 + static_cast<int>(options.size()) * 48));
  auto *layout = new QVBoxLayout(&dialog);
  layout->setContentsMargins(20, 12, 20, 20);
  auto *heading = new QWidget;
  heading->setFixedHeight(38);
  auto *headingLayout = new QHBoxLayout(heading);
  headingLayout->setContentsMargins(0, 0, 0, 0);
  headingLayout->addStretch();
  auto *label = new QLabel(title);
  label->setObjectName("pageHeading");
  headingLayout->addWidget(label);
  headingLayout->addStretch();
  auto *close = new SourceIconButton(asset("icon_windows_close_btn"));
  close->setFixedSize(26, 26);
  headingLayout->addWidget(close);
  QObject::connect(close, &QAbstractButton::clicked, &dialog, &QDialog::accept);
  layout->addWidget(heading);
  auto *scroll = new QScrollArea;
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  auto *body = new QWidget;
  auto *bodyLayout = new QVBoxLayout(body);
  bodyLayout->setContentsMargins(0, 0, 0, 0);
  bodyLayout->setSpacing(0);
  const auto current = readSettings();
  QList<QWidget *> rows;
  for (const auto &[name, key, defaultValue] : options)
    rows.append(
        row(name, {},
            toggle(wetype::settingBool(current.value(key), defaultValue), true,
                   key),
            48));
  bodyLayout->addWidget(rowsCard(rows));
  bodyLayout->addStretch();
  scroll->setWidget(body);
  layout->addWidget(scroll, 1);
  dialog.exec();
}

static void runLanguageDialog(QWidget *owner) {
  QDialog dialog(owner, Qt::Dialog | Qt::FramelessWindowHint);
  dialog.setModal(true);
  dialog.setObjectName("windowSurface");
  dialog.setFixedSize(420, 190);
  auto *layout = new QVBoxLayout(&dialog);
  layout->setContentsMargins(20, 14, 20, 20);
  auto *title = new QLabel("默认输入语言");
  title->setObjectName("pageHeading");
  title->setAlignment(Qt::AlignCenter);
  layout->addWidget(title);
  auto *chinese = new QRadioButton("中文");
  auto *english = new QRadioButton("英文");
  (readSettings().value("default_language").toString() == "english" ? english
                                                                    : chinese)
      ->setChecked(true);
  layout->addWidget(rowsCard({radioRow(chinese), radioRow(english)}));
  QObject::connect(chinese, &QRadioButton::toggled, [](bool checked) {
    if (checked)
      writeSetting("default_language", "chinese");
  });
  QObject::connect(english, &QRadioButton::toggled, [](bool checked) {
    if (checked)
      writeSetting("default_language", "english");
  });
  auto *close = greenButton("完成");
  QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
  layout->addWidget(close, 0, Qt::AlignRight);
  dialog.exec();
}

int main(int argc, char **argv) {
  qunsetenv("WAYLAND_SOCKET");
  QApplication app(argc, argv);
  app.setApplicationName("fcitx5-wetypex-settings");
  app.setDesktopFileName("fcitx5-wetypex-settings");
  app.setWindowIcon(appIcon());
  const auto args = app.arguments();
  const auto settings = readSettings();
  const auto appearance = readAppearance();
  app.setStyleSheet(R"(
    * { font-family:"Microsoft YaHei","Noto Sans CJK SC",sans-serif; font-size:14px; color:#202124; }
    QWidget#windowSurface,QWidget#contentPage,QScrollArea#pageScroll,QScrollArea#pageScroll>QWidget>QWidget { background:#f7f7f7; }
    QWidget#sidebar { background:qlineargradient(x1:0,y1:0,x2:1,y2:1,stop:0 #ddf8fd,stop:1 #e6f3ff); }
    QLabel#brand { font-size:17px; font-weight:600; background:transparent; }
    QLabel#pageHeading { font-size:16px; background:transparent; }
    QLabel#rowTitle { font-size:14px; background:transparent; }
    QLabel#rowSubtitle { font-size:11px; color:#969696; background:transparent; }
    QLabel#keyCap { color:#606266; background:#f1f1f1; border-radius:4px; padding:0 7px; font-size:12px; }
    QFrame#card { background:#fff; border:1px solid #e9e9e9; border-radius:10px; }
    QFrame#separator { background:#eee; border:0; margin-left:15px; margin-right:15px; }
    QListWidget#navigation { background:transparent; border:0; outline:0; padding:0; }
    QListWidget#navigation::item { height:40px; margin:0 0 1px 0; padding-left:14px; border-radius:7px; }
    QListWidget#navigation::item:selected { background:#23c891; color:white; }
    QListWidget#navigation::item:hover:!selected { background:rgba(255,255,255,0.42); }
    QScrollBar:vertical { width:8px; margin:7px 1px; background:transparent; }
    QScrollBar::handle:vertical { min-height:45px; border-radius:3px; background:#d4d7da; }
    QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical { height:0; }
    QRadioButton,QCheckBox { spacing:8px; background:transparent; }
    QRadioButton::indicator,QCheckBox::indicator { width:18px; height:18px; }
    QRadioButton::indicator:checked { width:8px; height:8px; background:white; border:5px solid #23c891; border-radius:9px; }
    QRadioButton::indicator:unchecked { background:white; border:1px solid #cfd1d2; border-radius:9px; }
    QCheckBox::indicator:checked { background:#23c891; border:1px solid #23c891; border-radius:3px; }
    QCheckBox::indicator:unchecked { background:white; border:1px solid #cfd1d2; border-radius:3px; }
    QComboBox,QSpinBox { min-height:25px; padding:0 8px; border:0; background:transparent; color:#777; }
    QPushButton { min-height:26px; padding:0 14px; background:white; border:1px solid #d9d9d9; border-radius:4px; }
    QPushButton:hover { border-color:#23c891; }
    QPushButton#greenButton { color:white; background:#23c891; border-color:#23c891; min-height:28px; }
    QTabBar#segments { background:#ededed; border-radius:7px; }
    QTabBar#segments::tab { background:transparent; min-width:112px; height:26px; margin:2px; padding:0 4px; color:#888; }
    QTabBar#segments::tab:selected { background:white; color:#202124; border-radius:5px; }
    QSlider::groove:horizontal { height:4px; border-radius:2px; background:#dedede; }
    QSlider::handle:horizontal { width:16px; margin:-7px 0; border:1px solid #c9c9c9; border-radius:8px; background:white; }
    QToolButton#closeButton { border:0; background:transparent; font-size:20px; color:#555; }
    QToolButton#closeButton:hover { background:#e9e9e9; }
  )");

  Window window;
  window.setObjectName("windowSurface");
  window.setWindowTitle("WeTypeX");
  auto *root = new QHBoxLayout(&window);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);
  auto *sidebar = new QWidget;
  sidebar->setObjectName("sidebar");
  sidebar->setFixedWidth(211);
  if (!imageAsset("side_bar_background_light.png").isEmpty())
    sidebar->setStyleSheet("QWidget#sidebar{border-image:url('" +
                           imageAsset("side_bar_background_light.png") +
                           "') 0 0 0 0 stretch stretch;}");
  auto *sideLayout = new QVBoxLayout(sidebar);
  sideLayout->setContentsMargins(7, 25, 7, 18);
  sideLayout->setSpacing(8);
  auto *brandRow = new QWidget;
  brandRow->setFixedHeight(32);
  auto *brandLayout = new QHBoxLayout(brandRow);
  brandLayout->setContentsMargins(10, 0, 0, 0);
  brandLayout->setSpacing(8);
  auto *logo = new QLabel;
  logo->setFixedSize(24, 24);
  logo->setPixmap(appIcon().pixmap(24, 24));
  brandLayout->addWidget(logo);
  auto *brand = new QLabel("WeTypeX");
  brand->setObjectName("brand");
  brandLayout->addWidget(brand);
  brandLayout->addStretch();
  sideLayout->addWidget(brandRow);
  auto *navigation = new QListWidget;
  navigation->setObjectName("navigation");
  navigation->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  const QStringList navNames = {"输入",   "语音输入",  "常用语和剪贴板",
                                "外观",   "快捷键",    "跨设备",
                                "手机版", "升级和反馈"};
  const QStringList navIcons = {"input",     "voice",   "phrases", "appearance",
                                "shortcuts", "devices", "mobile",  "about"};
  for (int i = 0; i < navNames.size(); ++i) {
    auto *item = new QListWidgetItem(navIcon(navIcons[i]), navNames[i]);
    item->setSizeHint({190, 40});
    navigation->addItem(item);
  }
  sideLayout->addWidget(navigation, 1);
  auto *deviceNotice = new QLabel(sidebar);
  deviceNotice->setFixedSize(8, 8);
  deviceNotice->setStyleSheet("background:#ff5151;border-radius:4px;");
  deviceNotice->move(179, 282);
  deviceNotice->raise();
  root->addWidget(sidebar);
  auto *right = new QWidget;
  right->setObjectName("contentPage");
  auto *rightLayout = new QVBoxLayout(right);
  rightLayout->setContentsMargins(0, 0, 0, 0);
  rightLayout->setSpacing(0);
  auto *close = new SourceIconButton(asset("icon_windows_close_btn"), right);
  close->setObjectName("closeButton");
  auto *pages = new QStackedWidget;
  rightLayout->addWidget(pages, 1);
  root->addWidget(right, 1);
  QObject::connect(close, &QAbstractButton::clicked, &window, &QWidget::close);
  close->move(458, 0);
  close->raise();

  auto input = page("输入");
  auto *pinyin = new QRadioButton("拼音输入"),
       *doublePinyin = new QRadioButton("双拼输入"),
       *wubi = new QRadioButton("五笔输入");
  auto *inputGroup = new QButtonGroup(&window);
  inputGroup->addButton(pinyin, 0);
  inputGroup->addButton(doublePinyin, 1);
  inputGroup->addButton(wubi, 5);
  auto mode = settings.value("input_mode")
                  .toString(settings.value("keyboard").toInt() == 5 ? "wubi"
                                                                    : "pinyin");
  (mode == "wubi"            ? wubi
   : mode == "double_pinyin" ? doublePinyin
                             : pinyin)
      ->setChecked(true);
  auto *doubleScheme = new QComboBox;
  doubleScheme->addItems(
      {"自然码", "搜狗", "微软", "小鹤", "拼音加加", "紫光", "智能 ABC"});
  doubleScheme->setCurrentIndex(
      settings.value("double_pinyin_scheme").toInt(0));
  doubleScheme->setEnabled(doublePinyin->isChecked());
  auto *wubiScheme = new QComboBox;
  wubiScheme->addItems({"86 五笔", "98 五笔", "新世纪五笔"});
  wubiScheme->setCurrentIndex(settings.value("wubi_solution").toInt(0));
  auto *wubiSetup = new QPushButton("设置");
  QObject::connect(wubiSetup, &QPushButton::clicked, [&window] {
    runToggleDialog(&window, "五笔功能",
                    {{"五笔拼音混输", "wubi_pinyin", false},
                     {"唯一候选自动上屏", "wubi_unique_commit", false},
                     {"输入下一码时上屏", "wubi_next_commit", false},
                     {"通配符编码提示", "wubi_wildcard_comment", false}});
  });
  input.body->addWidget(rowsCard(
      {radioRow(pinyin), radioRow(doublePinyin, doubleScheme),
       radioRow(wubi, wubiScheme), row("五笔功能", {}, wubiSetup, 44)}));
  QObject::connect(inputGroup,
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
                   &QButtonGroup::idClicked,
#else
                   qOverload<int>(&QButtonGroup::buttonClicked),
#endif
                   [=](int id) {
    writeSetting("input_mode", id == 5   ? "wubi"
                               : id == 1 ? "double_pinyin"
                                         : "pinyin");
    writeSetting("keyboard", id == 5 ? 5 : 0);
    doubleScheme->setEnabled(id == 1);
  });
  QObject::connect(
      doubleScheme, qOverload<int>(&QComboBox::currentIndexChanged),
      [](int value) { writeSetting("double_pinyin_scheme", value); });
  QObject::connect(wubiScheme, qOverload<int>(&QComboBox::currentIndexChanged),
                   [](int value) { writeSetting("wubi_solution", value); });
  auto *emojiSetup = new QPushButton("设置...");
  QObject::connect(emojiSetup, &QPushButton::clicked, [&window] {
    runToggleDialog(&window, "表情和颜文字推荐",
                    {{"微信表情", "wechat_emoji", true},
                     {"Emoji", "normal_emoji", true},
                     {"颜文字", "kaomoji", true},
                     {"大表情", "large_emoji", true},
                     {"符号表情", "symbol_emoji", true}});
  });
  input.body->addWidget(rowsCard(
      {row("智能拼写", "精准匹配候选词，大幅提升打字效率",
           toggle(wetype::settingBool(settings.value("smart_input"), true),
                  true, "smart_input")),
       row("表情和颜文字推荐", {}, emojiSetup, 50)}));
  auto *languageSetup = new QPushButton("设置..."),
       *fuzzySetup = new QPushButton("设置...");
  QObject::connect(languageSetup, &QPushButton::clicked,
                   [&window] { runLanguageDialog(&window); });
  QObject::connect(fuzzySetup, &QPushButton::clicked, [&window] {
    runToggleDialog(&window, "模糊拼音",
                    {{"n/l", "fuzzy_nl", false},
                     {"r/l", "fuzzy_rl", false},
                     {"h/f", "fuzzy_hf", false},
                     {"g/k", "fuzzy_gk", false},
                     {"an/ang", "fuzzy_an_ang", false},
                     {"ian/iang", "fuzzy_ian_iang", false},
                     {"uan/uang", "fuzzy_uan_uang", false},
                     {"c/ch", "fuzzy_c_ch", false},
                     {"s/sh", "fuzzy_s_sh", false},
                     {"z/zh", "fuzzy_z_zh", false},
                     {"hui/fei", "fuzzy_hui_fei", false},
                     {"en/eng", "fuzzy_en_eng", false},
                     {"in/ing", "fuzzy_in_ing", false},
                     {"on/ong", "fuzzy_on_ong", false},
                     {"huang/wang", "fuzzy_huang_wang", false},
                     {"un/ong", "fuzzy_un_ong", false},
                     {"un/iong", "fuzzy_un_iong", false},
                     {"an/ai", "fuzzy_an_ai", false},
                     {"eng/ong", "fuzzy_eng_ong", false}});
  });
  input.body->addWidget(rowsCard(
      {row("输入中文时，将「/?」按键替换为 、", {},
           toggle(
               wetype::settingBool(settings.value("slash_punctuation"), true),
               true, "slash_punctuation"),
           52),
       row("符号自动转换",
           "数字间部分符号处理为英文标点，例如 12：00 替换为 12:00",
           toggle(
               wetype::settingBool(settings.value("symbol_auto_change"), true),
               true, "symbol_auto_change")),
       row("符号自动补全", "自动补全成对符号的右半部分",
           toggle(wetype::settingBool(settings.value("symbol_auto_pair"), true),
                  true, "symbol_auto_pair")),
       row("默认输入语言（中文/英文）", {}, languageSetup, 52),
       row("模糊拼音", {}, fuzzySetup, 52)}));
  input.body->addWidget(rowsCard(
      {row("单机模式",
           "无需网络，单机离线使用。不支持跨设备、表情推荐、问 AI 等联网功能",
           toggle(wetype::settingBool(settings.value("standalone"), false),
                  true, "standalone"))}));
  pages->addWidget(input.widget);

  auto voice = page("语音输入");
  auto *voiceHero = card();
  auto *voiceHeroLayout = new QVBoxLayout(voiceHero);
  voiceHeroLayout->setContentsMargins(16, 18, 16, 18);
  auto *voiceSample = new VoiceSample;
  voiceHeroLayout->addWidget(voiceSample);
  voiceHeroLayout->addWidget(separator());
  const QList<QPair<QString, QString>> voiceFeatures = {
      {"icon_speech", "语音实时转写到输入框，表达又快又准"},
      {"icon_time", "连续输入不限时长，超长内容轻松记"},
      {"icon_many", "支持中文、英文及多种方言自由混说"}};
  for (const auto &[icon, text] : voiceFeatures) {
    auto *feature = new QWidget;
    auto *layout = new QHBoxLayout(feature);
    layout->setContentsMargins(4, 1, 4, 1);
    layout->setSpacing(10);
    layout->addWidget(iconLabel(icon, 18));
    layout->addWidget(new QLabel(text), 1);
    if (icon == "icon_many")
      layout->addWidget(iconLabel("icon_about_arrow", 14));
    voiceHeroLayout->addWidget(feature);
  }
  voice.body->addWidget(voiceHero);
  voice.body->addWidget(rowsCard(
      {row("快捷键", {}, nullptr, 44),
       shortcutRow(
           "启动语音输入",
           wetype::settingBool(settings.value("voice_launch_shortcut"), true),
           {"Ctrl", "Win", "Shift", "×"},
           "按下可开启语音输入，按任意键均可结束", true,
           "voice_launch_shortcut"),
       shortcutRow(
           "按住说话",
           wetype::settingBool(settings.value("voice_hold_shortcut"), true),
           {"Ctrl", "Win", "×"}, "按住可语音输入，松手结束", true,
           "voice_hold_shortcut")}));
  auto *microphone = new QComboBox;
  microphone->addItem("自动检测");
  QProcess pipewire;
  pipewire.start(QStringLiteral("pw-dump"), QStringList{});
  if (pipewire.waitForFinished(2000)) {
    const auto nodes =
        QJsonDocument::fromJson(pipewire.readAllStandardOutput()).array();
    for (const auto &value : nodes) {
      const auto object = value.toObject();
      const auto info = object.value("info").toObject();
      const auto props = info.value("props").toObject();
      if (props.value("media.class").toString() != "Audio/Source")
        continue;
      const auto target = props.value("node.name").toString();
      const auto name = props.value("node.description").toString(target);
      if (!target.isEmpty())
        microphone->addItem(name, target);
    }
  }
  const auto savedMicrophone = settings.value("voice_microphone").toString();
  if (!savedMicrophone.isEmpty()) {
    const int index = microphone->findData(savedMicrophone);
    if (index >= 0)
      microphone->setCurrentIndex(index);
  }
  QObject::connect(
      microphone, qOverload<int>(&QComboBox::currentIndexChanged), [microphone](int index) {
        writeSetting("voice_microphone",
                     index > 0
                         ? QJsonValue::fromVariant(microphone->itemData(index))
                         : QJsonValue("自动检测"));
      });
  auto *punctuationMode = new QComboBox;
  punctuationMode->addItem("智能标点");
  punctuationMode->addItem("添加完整标点");
  punctuationMode->addItem("句末不加句号");
  punctuationMode->addItem("空格替换标点");
  punctuationMode->setCurrentText(
      settings.value("voice_punctuation").toString("智能标点"));
  QObject::connect(
      punctuationMode, &QComboBox::currentTextChanged,
      [](const QString &value) { writeSetting("voice_punctuation", value); });
  voice.body->addWidget(
      rowsCard({row("麦克风", "设置语音输入的默认麦克风", microphone),
                row("标点设置", {}, punctuationMode),
                row("语音智能整理", {},
                    toggle(wetype::settingBool(
                               settings.value("voice_smart_polish"), true),
                           true, "voice_smart_polish"),
                    52)}));
  pages->addWidget(voice.widget);

  auto phrases = page("", false);
  phrases.widget->layout()->setContentsMargins(20, 0, 22, 0);
  auto *phraseTabs = segments({"常用语", "剪贴板"});
  auto *tabRow = new QHBoxLayout;
  tabRow->addStretch();
  tabRow->addWidget(phraseTabs);
  tabRow->addStretch();
  phrases.body->addLayout(tabRow);
  auto *phraseStack = new QStackedWidget;
  auto *common = new QWidget;
  auto *commonLayout = new QVBoxLayout(common);
  commonLayout->setContentsMargins(0, 0, 0, 0);
  commonLayout->setSpacing(16);
  auto *demo = card();
  auto *demoLayout = new QVBoxLayout(demo);
  demoLayout->setContentsMargins(12, 12, 12, 12);
  auto *demoBox = new QLabel;
  if (!imageAsset("hotword_guide.gif").isEmpty()) {
    auto *movie = new QMovie(imageAsset("hotword_guide.gif"), {}, demoBox);
    movie->setScaledSize({436, 98});
    demoBox->setMovie(movie);
    movie->start();
  } else
    demoBox->setText("原版常用语引导资源未安装");
  demoBox->setFixedHeight(98);
  demoBox->setStyleSheet("background:#d8f7e5;border-radius:6px;color:#202124;");
  demoLayout->addWidget(demoBox);
  demoLayout->addWidget(new QLabel(
      "添加文字到「常用语」后，输入前 3 个字或其拼音首字母即可使用"));
  commonLayout->addWidget(demo);
  auto *phraseList = new QListWidget;
  phraseList->setFrameShape(QFrame::NoFrame);
  phraseList->setAlternatingRowColors(false);
  phraseList->setStyleSheet(
      "QListWidget{background:white;border-radius:10px;padding:4px;}"
      "QListWidget::item{min-height:42px;border-bottom:1px solid #eeeeee;}"
      "QListWidget::item:selected{background:#dff8ef;color:#202124;} ");
  auto *addPhrase = greenButton("添加");
  auto *editPhrase = new QPushButton("编辑"),
       *deletePhrase = new QPushButton("删除");
  auto *phraseButtons = new QHBoxLayout;
  phraseButtons->addStretch();
  phraseButtons->addWidget(deletePhrase);
  phraseButtons->addWidget(editPhrase);
  phraseButtons->addWidget(addPhrase);
  commonLayout->addLayout(phraseButtons);
  commonLayout->addWidget(phraseList, 1);
  auto reloadPhrases = [phraseList, editPhrase, deletePhrase] {
    QString error;
    const auto result = hotwordRequest("hotword_list", {}, &error);
    phraseList->clear();
    for (const auto &value : result.value("hotwords").toArray()) {
      const auto item = value.toObject();
      auto *row = new QListWidgetItem(item.value("words").toString() + "    " +
                                          item.value("key").toString(),
                                      phraseList);
      row->setData(Qt::UserRole, item);
    }
    if (!error.isEmpty())
      phraseList->addItem(error);
    const bool hasItems = phraseList->count() > 0;
    phraseList->setVisible(hasItems);
    editPhrase->setVisible(hasItems);
    deletePhrase->setVisible(hasItems);
  };
  QObject::connect(addPhrase, &QPushButton::clicked, [&, reloadPhrases] {
    if (editHotword(&window))
      reloadPhrases();
  });
  QObject::connect(editPhrase, &QPushButton::clicked, [&, reloadPhrases] {
    if (auto *current = phraseList->currentItem();
        current &&
        editHotword(&window, current->data(Qt::UserRole).toJsonObject()))
      reloadPhrases();
  });
  QObject::connect(deletePhrase, &QPushButton::clicked, [&, reloadPhrases] {
    auto *current = phraseList->currentItem();
    if (!current)
      return;
    const auto item = current->data(Qt::UserRole).toJsonObject();
    if (item.value("id").toString().isEmpty())
      return;
    QString error;
    hotwordRequest(
        "hotword_set",
        {{"id", item.value("id").toString()}, {"key", ""}, {"words", ""}},
        &error);
    if (!error.isEmpty())
      QMessageBox::warning(&window, "常用语", error);
    else
      reloadPhrases();
  });
  QObject::connect(
      phraseList, &QListWidget::itemDoubleClicked,
      [&, reloadPhrases](QListWidgetItem *item) {
        if (editHotword(&window, item->data(Qt::UserRole).toJsonObject()))
          reloadPhrases();
      });
  reloadPhrases();
  phraseStack->addWidget(common);
  auto *clipboard = new QWidget;
  auto *clipboardLayout = new QVBoxLayout(clipboard);
  clipboardLayout->setContentsMargins(0, 0, 0, 0);
  clipboardLayout->addWidget(rowsCard({row(
      "剪贴板", "复制的内容将在剪贴板中展示",
      toggle(wetype::settingBool(settings.value("clipboard_enabled"), false),
             true, "clipboard_enabled"))}));
  clipboardLayout->addStretch();
  phraseStack->addWidget(clipboard);
  phrases.body->addWidget(phraseStack, 1);
  QObject::connect(phraseTabs, &QTabBar::currentChanged, phraseStack,
                   &QStackedWidget::setCurrentIndex);
  pages->addWidget(phrases.widget);

  auto visual = page("外观", false);
  auto *preview =
      new QLabel("<span style='background:#13b77a;color:white;padding:5px'>1 "
                 "输入</span>　2 输入法　3 诗心　4 为新　5 唯心");
  preview->setAlignment(Qt::AlignCenter);
  preview->setFixedHeight(180);
  preview->setStyleSheet(
      "background:#ccefd3;border-radius:10px;font-size:18px;");
  visual.body->addWidget(preview);
  auto *candidateSize = new QSlider(Qt::Horizontal);
  candidateSize->setRange(10, 18);
  candidateSize->setValue(settings.value("candidate_size").toInt(13));
  auto *sliderBox = new QWidget;
  sliderBox->setFixedHeight(120);
  auto *sliderLayout = new QVBoxLayout(sliderBox);
  sliderLayout->setContentsMargins(15, 10, 15, 8);
  sliderLayout->addWidget(new QLabel("候选字大小"));
  sliderLayout->addWidget(candidateSize);
  auto *ticks =
      new QLabel("最小　　　　　　　　　　　 默认　　　　　　　　　　　最大");
  ticks->setObjectName("rowSubtitle");
  sliderLayout->addWidget(ticks);
  auto *sliderCard = card();
  auto *sliderCardLayout = new QVBoxLayout(sliderCard);
  sliderCardLayout->setContentsMargins(0, 0, 0, 0);
  sliderCardLayout->addWidget(sliderBox);
  visual.body->addWidget(sliderCard);
  auto *theme = new QComboBox;
  theme->addItems({"跟随系统", "浅色", "深色"});
  int themeMode = appearance.value("UseDarkTheme", "True") == "True" ? 0
                  : appearance.value("Theme").contains("dark")       ? 2
                                                                     : 1;
  theme->setCurrentIndex(themeMode);
  visual.body->addWidget(rowsCard({row("主题模式", {}, theme, 48)}));
  auto *appearanceStatus = new QLabel;
  appearanceStatus->setObjectName("rowSubtitle");
  visual.body->addWidget(appearanceStatus);
  auto applyVisual = [=] {
    writeSetting("candidate_size", candidateSize->value());
    writeSetting("theme_mode", theme->currentIndex());
    applyClassicTheme(theme->currentIndex(), candidateSize->value(),
                      appearanceStatus);
  };
  QObject::connect(candidateSize, &QSlider::sliderReleased, applyVisual);
  QObject::connect(theme, qOverload<int>(&QComboBox::currentIndexChanged),
                   [=](int) { applyVisual(); });
  pages->addWidget(visual.widget);

  auto shortcuts = page("快捷键");
  shortcuts.body->addWidget(rowsCard(
      {row("中英文切换", {}, nullptr, 44),
       shortcutRow("使用 shift",
                   wetype::settingBool(settings.value("shift_switch"), true),
                   {"shift"}, {}, true, "shift_switch"),
       shortcutRow("使用 ctrl",
                   wetype::settingBool(settings.value("ctrl_switch"), false),
                   {"ctrl"}, {}, true, "ctrl_switch")}));
  auto *defaultShortcut =
      new QLabel("系统默认支持“ctrl + 空格”切换中英文  修改");
  defaultShortcut->setObjectName("rowSubtitle");
  shortcuts.body->addWidget(defaultShortcut);
  shortcuts.body->addWidget(rowsCard(
      {row("快捷使用", {}, nullptr, 44),
       shortcutRow("AI 助手  Beta",
                   wetype::settingBool(settings.value("ai_assistant"), true),
                   {"="}, "输入后按「=」可使用 AI 提问、表情推荐等功能", true,
                   "ai_assistant"),
       shortcutRow(
           "V 模式", wetype::settingBool(settings.value("v_mode"), true), {"V"},
           "按「v」打开快捷功能栏，可使用计算、剪贴板、常用语、符号等功能",
           true, "v_mode")}));
  shortcuts.body->addWidget(rowsCard(
      {row("语音输入", {}, nullptr, 44),
       shortcutRow(
           "启动语音输入",
           wetype::settingBool(settings.value("voice_launch_shortcut"), true),
           {"Ctrl", "Win", "Shift", "×"},
           "按下可开启语音输入，按任意键均可结束", true,
           "voice_launch_shortcut"),
       shortcutRow(
           "按住说话",
           wetype::settingBool(settings.value("voice_hold_shortcut"), true),
           {"Ctrl", "Win", "×"}, "按住可语音输入，松手结束", true,
           "voice_hold_shortcut")}));
  shortcuts.body->addWidget(rowsCard(
      {row("输入状态切换", {}, nullptr, 44),
       shortcutRow(
           "全半角输入切换",
           wetype::settingBool(settings.value("half_full_switch"), false),
           {"shift", "backslash-icon"}, {}, true, "half_full_switch"),
       shortcutRow(
           "中文下中英标点切换",
           wetype::settingBool(settings.value("punctuation_switch"), true),
           {"ctrl", "。"}, {}, true, "punctuation_switch"),
       shortcutRow(
           "简繁体输入切换",
           wetype::settingBool(settings.value("traditional_switch"), false),
           {"ctrl", "shift", "F"}, {}, true, "traditional_switch")}));
  shortcuts.body->addWidget(rowsCard(
      {row("翻页按字", {}, new QLabel("向上翻　向下翻"), 44),
       shortcutRow(
           "减号等号",
           wetype::settingBool(settings.value("page_minus_equal"), true),
           {"−", "="}, {}, true, "page_minus_equal"),
       shortcutRow("左右中括号",
                   wetype::settingBool(settings.value("page_brackets"), true),
                   {"[", "]"}, {}, true, "page_brackets"),
       shortcutRow(
           "逗号句号",
           wetype::settingBool(settings.value("page_comma_period"), false),
           {"，", "。"}, {}, true, "page_comma_period"),
       shortcutRow("shift + tab / tab",
                   wetype::settingBool(settings.value("page_shift_tab"), false),
                   {"shift + tab", "tab"}, {}, true, "page_shift_tab")}));
  shortcuts.body->addWidget(rowsCard(
      {row("候选词选择", {}, nullptr, 44),
       shortcutRow(
           "使用分号、引号选择第 2 位、第 3 位候选词",
           wetype::settingBool(settings.value("select_semicolon_quote"), false),
           {"；", "’"}, {}, true, "select_semicolon_quote"),
       shortcutRow("使用左、右 ctrl 选择第 2 位、第 3 位候选词",
                   wetype::settingBool(settings.value("select_ctrl"), false),
                   {"ctrl", "backslash-icon", "ctrl"}, {}, true,
                   "select_ctrl")}));
  pages->addWidget(shortcuts.widget);

  auto devices = page("跨设备");
  const auto syncState = readJsonObject(
      QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
      "/fcitx5-wetypex/state/sync-state.json");
  const qint64 syncGroup = wetype::jsonInteger(syncState.value("group_id"));
  const int syncFunctions = syncState.value("func_switch").toInt();
  const bool syncAvailable =
      syncGroup > 0 &&
      !wetype::settingBool(settings.value("standalone"), false);
  auto *deviceHero = new QWidget;
  auto *deviceHeroLayout = new QHBoxLayout(deviceHero);
  deviceHeroLayout->setContentsMargins(82, 0, 82, 0);
  auto deviceImage = [](const QString &name, const QSize &size) {
    auto *label = new QLabel;
    label->setFixedSize(size);
    label->setPixmap(
        QPixmap(imageAsset(name))
            .scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    label->setAlignment(Qt::AlignCenter);
    return label;
  };
  deviceHeroLayout->addWidget(
      deviceImage("icon_iphone_default_light.png", {52, 75}));
  deviceHeroLayout->addWidget(iconLabel("icon_left", 14));
  deviceHeroLayout->addWidget(
      deviceImage("icon_computer_default_light.png", {105, 75}));
  deviceHeroLayout->addWidget(iconLabel("icon_right", 14));
  deviceHeroLayout->addWidget(
      deviceImage("icon_Android_default_light.png", {52, 75}));
  deviceHero->setFixedHeight(95);
  deviceHero->setStyleSheet("font-size:20px;background:transparent;");
  devices.body->addWidget(deviceHero);
  auto *transfer = greenButton("传文件");
  transfer->setEnabled(
      !wetype::settingBool(settings.value("standalone"), false));
  QObject::connect(transfer, &QPushButton::clicked, [] {
    QProcess::startDetached(QStringLiteral(WETYPE_TRANSFER), {});
  });
  auto *clipboardSync = new Toggle(syncFunctions & 1);
  auto *dictionarySync = new Toggle(syncFunctions & 4);
  auto *phraseSync = new Toggle(syncFunctions & 2);
  for (auto *control : {clipboardSync, dictionarySync, phraseSync})
    control->setEnabled(syncAvailable);
  auto *syncNow = greenButton("立即请求同步");
  syncNow->setEnabled(syncAvailable);
  auto requestSync = [=, &window] {
    syncNow->setEnabled(false);
    runAccount(&window, {"sync-now"},
               [=, &window](int code, const QJsonObject &result) {
                 syncNow->setEnabled(syncAvailable);
                 if (!code && result.value("requested").toBool())
                   QMessageBox::information(
                       &window, "同步",
                       "已请求同步，请稍后检查关联设备的词库与常用语。");
                 else
                   QMessageBox::warning(
                       &window, "同步",
                       result.value("error").toString(
                           "同步请求失败，请检查网络和输入法状态。"));
               });
  };
  QObject::connect(syncNow, &QPushButton::clicked, requestSync);
  auto updateFunctions = [=, &window](bool) {
    const int mask = (syncFunctions & ~7) |
                     (clipboardSync->isChecked() ? 1 : 0) |
                     (phraseSync->isChecked() ? 2 : 0) |
                     (dictionarySync->isChecked() ? 4 : 0);
    writeSetting("device_clipboard_sync", clipboardSync->isChecked());
    writeSetting("device_dictionary_sync", dictionarySync->isChecked());
    writeSetting("device_phrase_sync", phraseSync->isChecked());
    for (auto *control : {clipboardSync, dictionarySync, phraseSync})
      control->setEnabled(false);
    runAccount(
        &window,
        {"set-functions", QString::number(syncGroup), QString::number(mask)},
        [=, &window](int code, const QJsonObject &result) {
          const bool ok = !code && result.value("ok").toBool();
          if (!ok) {
            QSignalBlocker a(clipboardSync), b(dictionarySync), c(phraseSync);
            clipboardSync->setChecked(syncFunctions & 1);
            phraseSync->setChecked(syncFunctions & 2);
            dictionarySync->setChecked(syncFunctions & 4);
          }
          for (auto *control : {clipboardSync, dictionarySync, phraseSync})
            control->setEnabled(syncAvailable);
          if (ok)
            runAccount(&window, {"sync-now", "--refresh-state"},
                       [](int, const QJsonObject &) {});
        });
  };
  QObject::connect(clipboardSync, &QAbstractButton::toggled, updateFunctions);
  QObject::connect(dictionarySync, &QAbstractButton::toggled, updateFunctions);
  QObject::connect(phraseSync, &QAbstractButton::toggled, updateFunctions);
  devices.body->addWidget(rowsCard(
      {iconRow("icon_transfer_copy", "跨设备复制粘贴",
               "在电脑复制文字、图片后，手机上可立即粘贴", clipboardSync),
       iconRow("icon_transfer_glossary", "个人词库同步",
               "关联设备之间同步个人词库", dictionarySync),
       iconRow("icon_transfer_common", "常用语同步", "关联设备之间同步常用语",
               phraseSync),
       row("同步词库与常用语", "请求关联设备的最新同步状态", syncNow),
       iconRow("icon_transfer_filetransfer", "隔空传送",
               "跨设备发送图片、视频和文件", transfer)}));
  auto *deviceLabel = new QLabel("我的设备");
  deviceLabel->setObjectName("rowSubtitle");
  devices.body->addWidget(deviceLabel);
  auto *matchCode = greenButton("查看匹配码"),
       *mobileDownload = greenButton("下载手机版");
  matchCode->setEnabled(
      !wetype::settingBool(settings.value("standalone"), false));
  QObject::connect(mobileDownload, &QPushButton::clicked, [] {
    QDesktopServices::openUrl(QUrl("https://z.weixin.qq.com/"));
  });
  QString localModel;
  QFile modelFile("/sys/class/dmi/id/product_name");
  if (modelFile.open(QIODevice::ReadOnly))
    localModel = QString::fromUtf8(modelFile.readAll()).trimmed();
  if (localModel.isEmpty())
    localModel = "LINUX";
  QList<QWidget *> deviceRows;
  deviceRows.append(iconRow("icon_transfer_computer_mini.png", localModel,
                            "本机", matchCode));
  for (const auto &entry : syncState.value("devices").toArray()) {
    const auto device = entry.toObject();
    const auto name = device.value("name").toString();
    const int platform = device.value("platform").toInt();
    if (name.isEmpty() || (name == localModel && platform == 5))
      continue;
    const QString icon = platform == 1   ? "icon_transfer_android_mini.png"
                         : platform == 2 ? "icon_transfer_ios_mini.png"
                                         : "icon_transfer_computer_mini.png";
    deviceRows.append(iconRow(
        icon, name, device.value("client_version").toString(), nullptr, 58));
  }
  if (!syncGroup)
    deviceRows.append(iconRow("icon_transfer_ios_mini.png",
                              "关联 iOS/Android 版本即可体验跨设备同步", {},
                              mobileDownload));
  devices.body->addWidget(rowsCard(deviceRows));
  auto *associate = new QPushButton("关联设备");
  associate->setIcon(QIcon(asset("icon_sync_device")));
  associate->setStyleSheet("color:#00bf83;border:1px solid "
                           "#e9e9e9;background:white;min-height:42px;");
  associate->setEnabled(
      !wetype::settingBool(settings.value("standalone"), false));
  auto showPairing = [&window] {
    if (window.findChild<QWidget *>("pairingOverlay"))
      return;
    new PairingOverlay(&window);
  };
  QObject::connect(matchCode, &QPushButton::clicked, showPairing);
  QObject::connect(associate, &QPushButton::clicked, showPairing);
  devices.body->addWidget(associate);
  pages->addWidget(devices.widget);

  auto mobile = page("手机版", false);
  auto *phone = new QLabel;
  if (!imageAsset("entry_phone_light.png").isEmpty())
    phone->setPixmap(
        QPixmap(imageAsset("entry_phone_light.png"))
            .scaled(460, 307, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
  else
    phone->setText("手机版界面资源未安装");
  phone->setAlignment(Qt::AlignCenter);
  phone->setFixedHeight(307);
  phone->setStyleSheet("background:#ccefd3;border-radius:10px;font-family:"
                       "monospace;font-size:16px;");
  mobile.body->addWidget(phone);
  auto *downloadRow = new QHBoxLayout;
  auto *iosDownload = greenButton("下载 iOS 版");
  iosDownload->setIcon(QIcon(asset("icon_entry_ios")));
  auto *androidDownload = greenButton("下载 Android 版");
  androidDownload->setIcon(QIcon(asset("icon_entry_android")));
  downloadRow->addWidget(iosDownload);
  downloadRow->addWidget(androidDownload);
  auto openMobileDownload = [] {
    QDesktopServices::openUrl(QUrl("https://z.weixin.qq.com/"));
  };
  QObject::connect(iosDownload, &QPushButton::clicked, openMobileDownload);
  QObject::connect(androidDownload, &QPushButton::clicked, openMobileDownload);
  mobile.body->addLayout(downloadRow);
  pages->addWidget(mobile.widget);

  auto about = page("", false);
  auto *aboutLogo = new QLabel;
  aboutLogo->setPixmap(appIcon().pixmap(70, 70));
  aboutLogo->setAlignment(Qt::AlignCenter);
  about.body->addSpacing(25);
  about.body->addWidget(aboutLogo);
  auto *aboutName = new QLabel("WeTypeX");
  aboutName->setAlignment(Qt::AlignCenter);
  aboutName->setStyleSheet("font-size:22px;background:transparent;");
  about.body->addWidget(aboutName);
  auto *version = new QLabel(QStringLiteral(WETYPE_VERSION));
  version->setObjectName("rowSubtitle");
  version->setAlignment(Qt::AlignCenter);
  about.body->addWidget(version);
  auto *feedback = new QToolButton;
  feedback->setIcon(QIcon(asset("icon_about_arrow")));
  feedback->setStyleSheet("border:0;background:transparent;");
  QObject::connect(feedback, &QToolButton::clicked, [] {
    QDesktopServices::openUrl(
        QUrl("https://github.com/panxuc/fcitx5-wetypex/issues"));
  });
  about.body->addWidget(rowsCard(
      {row("有新版本时自动更新", {},
           toggle(wetype::settingBool(settings.value("auto_update"), true),
                  true, "auto_update"),
           48),
       row("我要反馈", {}, feedback, 48)}));
  about.body->addStretch();
  auto *copyright =
      new QLabel("WeTypeX 社区项目\n原版核心及相关商标归其权利人所有\n"
                 "WeTypeX 与腾讯无隶属关系");
  copyright->setObjectName("rowSubtitle");
  copyright->setAlignment(Qt::AlignCenter);
  about.body->addWidget(copyright);
  pages->addWidget(about.widget);

  QObject::connect(navigation, &QListWidget::currentRowChanged, pages,
                   &QStackedWidget::setCurrentIndex);
  navigation->setCurrentRow(0);
  int pageArg = args.indexOf("--page");
  if (pageArg >= 0 && pageArg + 1 < args.size())
    navigation->setCurrentRow(qBound(0, args[pageArg + 1].toInt(), 7));
  window.show();
  QTimer::singleShot(0, close, [close] {
    close->raise();
    close->show();
  });
  return app.exec();
}
