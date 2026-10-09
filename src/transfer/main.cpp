#include "../common/qt_helpers.hpp"
#include <QAbstractSocket>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QNetworkInterface>
#include <QProcess>
#include <QProcessEnvironment>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>
#include <functional>

namespace {
QString dataRoot() {
  return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
         QStringLiteral("/fcitx5-wetypex");
}
QString transferAsset(const QString &directory, const QString &name) {
  return QStandardPaths::locate(QStandardPaths::GenericDataLocation,
                                QStringLiteral("fcitx5-wetypex/ui/") +
                                    directory + QLatin1Char('/') + name);
}

QString localAddress() {
  QString defaultInterface;
  QFile routes(QStringLiteral("/proc/net/route"));
  if (routes.open(QIODevice::ReadOnly)) {
    for (const auto &line : routes.readAll().split('\n')) {
      const auto columns = line.simplified().split(' ');
      if (columns.size() >= 4 && columns[1] == "00000000" &&
          (columns[3].toUInt(nullptr, 16) & 2)) {
        defaultInterface = QString::fromLocal8Bit(columns[0]);
        break;
      }
    }
  }
  auto virtualInterface = [](const QString &name) {
    for (const auto &prefix :
         {QStringLiteral("docker"), QStringLiteral("veth"),
          QStringLiteral("virbr"), QStringLiteral("vmnet"),
          QStringLiteral("br-"), QStringLiteral("tun"), QStringLiteral("tap")})
      if (name.startsWith(prefix))
        return true;
    return false;
  };
  QString fallback;
  QString privateAddress;
  for (const auto &interface : QNetworkInterface::allInterfaces()) {
    if (!(interface.flags() & QNetworkInterface::IsUp) ||
        (interface.flags() & QNetworkInterface::IsLoopBack))
      continue;
    for (const auto &entry : interface.addressEntries()) {
      if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol)
        continue;
      const QString address = entry.ip().toString();
      if (address.startsWith(QStringLiteral("169.254.")))
        continue;
      if (interface.name() == defaultInterface &&
          !virtualInterface(interface.name()))
        return address;
      if (privateAddress.isEmpty() && !virtualInterface(interface.name()) &&
          (address.startsWith(QStringLiteral("10.")) ||
           address.startsWith(QStringLiteral("192.168.")) ||
           address.startsWith(QStringLiteral("172."))))
        privateAddress = address;
      if (fallback.isEmpty())
        fallback = address;
    }
  }
  return privateAddress.isEmpty() ? fallback : privateAddress;
}

bool readVarint(const QByteArray &data, qsizetype &offset, quint64 &value) {
  value = 0;
  for (unsigned shift = 0; shift < 64 && offset < data.size(); shift += 7) {
    const quint8 byte = quint8(data.at(offset++));
    value |= quint64(byte & 0x7f) << shift;
    if (!(byte & 0x80))
      return true;
  }
  return false;
}

struct GrpcInfo {
  QString ip;
  int port = 0;
  QString ca;
  QString serverName;
};

void parseEndpoint(const QByteArray &data, GrpcInfo &info) {
  qsizetype offset = 0;
  while (offset < data.size()) {
    quint64 tag = 0;
    if (!readVarint(data, offset, tag))
      return;
    const int field = int(tag >> 3), wire = int(tag & 7);
    if (wire == 0) {
      quint64 value = 0;
      if (!readVarint(data, offset, value))
        return;
      if (field == 2 && !info.port)
        info.port = int(value);
    } else if (wire == 2) {
      quint64 size = 0;
      if (!readVarint(data, offset, size) ||
          size > quint64(data.size() - offset))
        return;
      const QByteArray value = data.mid(offset, qsizetype(size));
      offset += qsizetype(size);
      if (field == 1 && info.ip.isEmpty())
        info.ip = QString::fromUtf8(value);
    } else {
      return;
    }
  }
}

void parseAvailableNets(const QByteArray &data, GrpcInfo &info) {
  qsizetype offset = 0;
  while (offset < data.size()) {
    quint64 tag = 0, size = 0;
    if (!readVarint(data, offset, tag) || (tag & 7) != 2 ||
        !readVarint(data, offset, size) || size > quint64(data.size() - offset))
      return;
    if ((tag >> 3) == 2)
      parseEndpoint(data.mid(offset, qsizetype(size)), info);
    offset += qsizetype(size);
  }
}

GrpcInfo parseGrpcInfo(const QByteArray &data) {
  GrpcInfo info;
  qsizetype offset = 0;
  while (offset < data.size()) {
    quint64 tag = 0;
    if (!readVarint(data, offset, tag))
      break;
    const int field = int(tag >> 3), wire = int(tag & 7);
    if (wire == 0) {
      quint64 value = 0;
      if (!readVarint(data, offset, value))
        break;
      if (field == 2)
        info.port = int(value);
    } else if (wire == 2) {
      quint64 size = 0;
      if (!readVarint(data, offset, size) ||
          size > quint64(data.size() - offset))
        break;
      const QByteArray value = data.mid(offset, qsizetype(size));
      offset += qsizetype(size);
      if (field == 1)
        info.ip = QString::fromUtf8(value);
      else if (field == 3)
        info.ca = QString::fromUtf8(value);
      else if (field == 4)
        info.serverName = QString::fromUtf8(value);
      else if (field == 9)
        parseAvailableNets(value, info);
    } else {
      break;
    }
  }
  return info;
}
} // namespace

class TransferWindow final : public QWidget {
public:
  TransferWindow() {
    setWindowTitle(QStringLiteral("WeTypeX 隔空传送"));
    setWindowIcon(QIcon::fromTheme(QStringLiteral("fcitx5-wetypex")));
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    resize(317, 519);
    setStyleSheet(QStringLiteral(
        "QWidget{background:#f7f7f7;color:#202020;font-size:14px}"
        "QLabel{background:transparent;}"
        "QLabel#title{font-size:18px;font-weight:500}"
        "QLabel#hint{color:#8c8c8c;font-size:13px}"
        "QPushButton{border:0;border-radius:5px;padding:9px "
        "18px;background:#18c690;color:white}"
        "QPushButton:disabled{background:#b7e9da;color:#f7fffc}"
        "QProgressBar{border:0;background:#e9e9e9;border-radius:2px;height:4px}"
        "QProgressBar::chunk{background:#18c690;border-radius:2px}"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    home_ = new QWidget;
    auto *homeLayout = new QVBoxLayout(home_);
    homeLayout->setContentsMargins(16, 14, 16, 18);
    homeLayout->setSpacing(12);
    auto *heading = new QHBoxLayout;
    heading->addSpacing(28);
    auto *homeTitle = new QLabel(QStringLiteral("隔空传送"));
    homeTitle->setAlignment(Qt::AlignCenter);
    heading->addWidget(homeTitle, 1);
    auto sourceButton = [](const QString &icon, const QString &accessible) {
      auto *button = new QToolButton;
      button->setIcon(QIcon(transferAsset(QStringLiteral("transfer-icons"),
                                          icon + QStringLiteral(".svg"))));
      button->setIconSize({18, 18});
      button->setFixedSize(28, 28);
      button->setAccessibleName(accessible);
      button->setToolTip(accessible);
      button->setStyleSheet(QStringLiteral(
          "QToolButton{border:0;background:transparent;}"
          "QToolButton:hover{background:#e9e9e9;border-radius:4px;}"));
      return button;
    };
    auto *settings = sourceButton(QStringLiteral("icon_statusbar_setup"),
                                  QStringLiteral("设置"));
    auto *close =
        sourceButton(QStringLiteral("icon_tips_close"), QStringLiteral("关闭"));
    heading->addWidget(settings);
    heading->addWidget(close);
    homeLayout->addLayout(heading);
    auto *section = new QLabel(QStringLiteral("我的设备"));
    section->setStyleSheet(
        QStringLiteral("color:#888;font-size:13px;margin-left:14px;"));
    homeLayout->addWidget(section);
    auto card = [sourceButton](const QString &icon, const QString &name,
                               const QString &detail) {
      auto *button = new QPushButton;
      button->setFixedHeight(62);
      button->setStyleSheet(QStringLiteral(
          "QPushButton{background:white;border:0;border-radius:10px;"
          "text-align:left;color:#202020;padding:0;}"
          "QPushButton:hover{background:#fbfbfb;}"));
      auto *row = new QHBoxLayout(button);
      row->setContentsMargins(15, 0, 10, 0);
      auto *image = new QLabel;
      image->setPixmap(QIcon(transferAsset(QStringLiteral("transfer-icons"),
                                           icon + QStringLiteral(".svg")))
                           .pixmap(28, 28));
      row->addWidget(image);
      auto *labels = new QVBoxLayout;
      labels->setSpacing(2);
      labels->addStretch();
      labels->addWidget(new QLabel(name));
      auto *subtitle = new QLabel(detail);
      subtitle->setStyleSheet(QStringLiteral("color:#999;font-size:12px;"));
      labels->addWidget(subtitle);
      labels->addStretch();
      row->addLayout(labels, 1);
      auto *arrow = sourceButton(QStringLiteral("icon_tips_arrow"),
                                 QStringLiteral("进入"));
      arrow->setIconSize({8, 14});
      arrow->setStyleSheet(QStringLiteral(
          "QToolButton{border:0;background:#f7f7f7;border-radius:14px;}"));
      arrow->setAttribute(Qt::WA_TransparentForMouseEvents);
      row->addWidget(arrow);
      return button;
    };
    auto *myDevices = card(QStringLiteral("icon_transmission_equipment"),
                           QStringLiteral("关联我的其他设备"),
                           QStringLiteral("关联后即可互传文件"));
    auto *others =
        card(QStringLiteral("icon_transmission_others"),
             QStringLiteral("传给其他人"), QStringLiteral("查看我的二维码"));
    homeLayout->addWidget(myDevices);
    homeLayout->addWidget(others);
    homeLayout->addStretch();
    layout->addWidget(home_);
    title_ = new QLabel(QStringLiteral("我的二维码"));
    title_->setObjectName(QStringLiteral("title"));
    title_->setAlignment(Qt::AlignCenter);
    qr_ = new QLabel;
    qr_->setAlignment(Qt::AlignCenter);
    qr_->setFixedSize(286, 286);
    status_ = new QLabel(QStringLiteral("正在初始化安全传输…"));
    status_->setObjectName(QStringLiteral("hint"));
    status_->setAlignment(Qt::AlignCenter);
    status_->setWordWrap(true);
    progress_ = new QProgressBar;
    progress_->setRange(0, 1000);
    progress_->hide();
    send_ = new QPushButton(QStringLiteral("发送文件"));
    send_->setEnabled(false);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(send_);
    buttons->addStretch();
    layout->addWidget(title_);
    layout->addWidget(qr_, 1, Qt::AlignHCenter);
    layout->addWidget(status_);
    layout->addWidget(progress_);
    layout->addLayout(buttons);
    for (auto *widget :
         QList<QWidget *>{title_, qr_, status_, progress_, send_})
      widget->hide();
    qrClose_ =
        sourceButton(QStringLiteral("icon_tips_close"), QStringLiteral("关闭"));
    qrClose_->setParent(this);
    qrClose_->hide();
    // The phone starts connecting as soon as the QR code is accepted.  Poll
    // quickly enough that the official mobile-side handshake does not expire
    // while the desktop is still waiting for its dispatch result.
    poll_.setInterval(250);
    connect(&poll_, &QTimer::timeout, this, [this] { pollPeer(); });
    connect(&session_, &QProcess::readyReadStandardOutput, this,
            [this] { readSessionEvents(); });
    connect(&session_, &QProcess::errorOccurred, this, [this] {
      status_->setText(QStringLiteral("安全传输核心启动失败"));
    });
    connect(send_, &QPushButton::clicked, this, [this] {
      QMenu menu(this);
      auto *files = menu.addAction(
          QIcon(transferAsset(QStringLiteral("transfer-icons"),
                              QStringLiteral("icon_filetype_general.svg"))),
          QStringLiteral("发送文件"));
      auto *directory = menu.addAction(
          QIcon(transferAsset(QStringLiteral("transfer-icons"),
                              QStringLiteral("icon_filetype_folder.svg"))),
          QStringLiteral("发送文件夹"));
      auto *chosen = menu.exec(send_->mapToGlobal(QPoint(0, send_->height())));
      if (chosen == files) {
        for (const auto &file :
             QFileDialog::getOpenFileNames(this, QStringLiteral("发送文件")))
          sendSession({{QStringLiteral("action"), QStringLiteral("upload")},
                       {QStringLiteral("path"), file}});
      } else if (chosen == directory) {
        const auto path = QFileDialog::getExistingDirectory(
            this, QStringLiteral("发送文件夹"));
        if (!path.isEmpty())
          sendSession(
              {{QStringLiteral("action"), QStringLiteral("upload_directory")},
               {QStringLiteral("path"), path}});
      }
    });
    connect(close, &QToolButton::clicked, this, &QWidget::close);
    connect(qrClose_, &QToolButton::clicked, this, &QWidget::close);
    connect(settings, &QToolButton::clicked, this, [] {
      QProcess::startDetached(QStringLiteral(WETYPE_SETTINGS),
                              {QStringLiteral("--page"), QStringLiteral("5")});
    });
    connect(myDevices, &QPushButton::clicked, this, [this, myDevices] {
      runAccount(
          {QStringLiteral("group-info")},
          [this, myDevices](const QJsonObject &result) {
            QMenu menu(this);
            const qint64 self =
                wetype::jsonInteger(result.value(QStringLiteral("self_uin")));
            for (const auto &value :
                 result.value(QStringLiteral("devices")).toArray()) {
              const auto device = value.toObject();
              const qint64 uin =
                  wetype::jsonInteger(device.value(QStringLiteral("uin")));
              if (uin <= 0 || uin == self)
                continue;
              const int platform =
                  device.value(QStringLiteral("platform")).toInt();
              const QString icon =
                  platform == 1
                      ? QStringLiteral("icon_transmission_android_big.svg")
                  : platform == 2
                      ? QStringLiteral("icon_transmission_ios_big.svg")
                      : QStringLiteral("icon_transmission_macos_big.svg");
              auto *action = menu.addAction(
                  QIcon(transferAsset(QStringLiteral("transfer-icons"), icon)),
                  device.value(QStringLiteral("name")).toString());
              action->setData(uin);
            }
            if (menu.actions().isEmpty()) {
              QProcess::startDetached(
                  QStringLiteral(WETYPE_SETTINGS),
                  {QStringLiteral("--page"), QStringLiteral("5")});
              return;
            }
            auto *chosen = menu.exec(myDevices->mapToGlobal(
                QPoint(myDevices->width() - menu.sizeHint().width(),
                       myDevices->height())));
            if (!chosen)
              return;
            boundUin_ = QString::number(chosen->data().toLongLong());
            showTransferPage(false);
            startSession();
          });
    });
    connect(others, &QPushButton::clicked, this, [this] {
      showTransferPage(true);
      startSession();
    });
    if (qEnvironmentVariableIsSet("WETYPE_TRANSFER_DIRECT"))
      QTimer::singleShot(0, others, &QPushButton::click);
  }

  ~TransferWindow() override {
    if (session_.state() != QProcess::NotRunning) {
      sendSession({{QStringLiteral("action"), QStringLiteral("quit")}});
      session_.waitForFinished(500);
    }
  }

private:
  void showTransferPage(bool showQr) {
    home_->hide();
    resize(317, 519);
    for (auto *widget :
         QList<QWidget *>{title_, qr_, status_, progress_, send_})
      widget->show();
    progress_->hide();
    send_->setEnabled(false);
    qrClose_->move(width() - 36, 8);
    qrClose_->raise();
    qrClose_->show();
    if (!showQr) {
      title_->setText(QStringLiteral("隔空传送"));
      const QPixmap connected(
          transferAsset(QStringLiteral("transfer-images"),
                        QStringLiteral("img_connection_pc_to_phone.png")));
      if (!connected.isNull())
        qr_->setPixmap(connected.scaled(240, 104, Qt::KeepAspectRatio,
                                        Qt::SmoothTransformation));
      status_->setText(QStringLiteral("正在邀请关联设备…"));
    }
    show();
  }

  void startSession() {
    const QString root = dataRoot();
    const QString runtime = qEnvironmentVariable(
        "WETYPE_FLURRY_RUNTIME", root + QStringLiteral("/runtime/flurry"));
    const QString host = qEnvironmentVariable(
        "WETYPE_ENGINE_HOST", QStringLiteral(WETYPE_ENGINE_HOST));
    const QString state = root + QStringLiteral("/state/transfer");
    const QString downloads =
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) +
        QStringLiteral("/WeTypeX");
    QDir().mkpath(state + QStringLiteral("/parts"));
    QDir().mkpath(downloads);
    wxpStarted_ = false;
    grpcAdvertised_ = false;
    lastDispatch_.clear();
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("WETYPE_SUPPORT_DIR"),
                       QStringLiteral(WETYPE_SUPPORT_DIR));
    environment.insert(QStringLiteral("WETYPE_FLURRY_DOWNLOAD_DIR"), downloads);
    environment.insert(QStringLiteral("WETYPE_FLURRY_TEMP_DIR"),
                       state + QStringLiteral("/parts"));
    session_.setProcessEnvironment(environment);
    session_.setStandardErrorFile(state + QStringLiteral("/transport.log"),
                                  QIODevice::Append);
    const QString wxp2p = QFileInfo(runtime).absolutePath() +
                          QStringLiteral("/wxp2p");
    preloadedWxp2p_ = QFileInfo::exists(wxp2p + QStringLiteral("/manifest.txt"));
    if (preloadedWxp2p_) {
      environment.insert(QStringLiteral("WETYPE_AUX_RUNTIME"), wxp2p);
      environment.remove(QStringLiteral("WETYPE_WXP2P_DISPATCH_FILE"));
      session_.setProcessEnvironment(environment);
      session_.start(host,
                     {runtime, QStringLiteral("flurry-wxp2p-server")});
    } else {
      session_.start(host, {runtime, QStringLiteral("flurry-server")});
    }
  }

  void startWxp2p(const QByteArray &dispatch, const QJsonObject &peer) {
    if (dispatch.isEmpty() || dispatch == lastDispatch_)
      return;
    lastDispatch_ = dispatch;
    if (wxpStarted_)
      grpcAdvertised_ = false;
    const QString root = dataRoot();
    const QString state = root + QStringLiteral("/state/transfer");
    const QString path = state + QStringLiteral("/wxp2p-dispatch.bin");
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(dispatch) != dispatch.size() || !file.commit()) {
      status_->setText(QStringLiteral("无法保存手机传输调度信息"));
      return;
    }
    QFile::setPermissions(path,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const QString peerCaPath = state + QStringLiteral("/peer-ca.pem");
    const QByteArray peerCa = QByteArray::fromBase64(
        peer.value(QStringLiteral("public_cer")).toString().toLatin1());
    QSaveFile peerCaFile(peerCaPath);
    if (peerCa.isEmpty() || !peerCaFile.open(QIODevice::WriteOnly) ||
        peerCaFile.write(peerCa) != peerCa.size() || !peerCaFile.commit()) {
      status_->setText(QStringLiteral("无法保存手机安全凭据"));
      return;
    }
    QFile::setPermissions(peerCaPath,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (preloadedWxp2p_ && !wxpStarted_ &&
        session_.state() == QProcess::Running) {
      wxpStarted_ = true;
      status_->setText(QStringLiteral("正在通过安全直连或中继连接手机…"));
      sendSession({{QStringLiteral("action"), QStringLiteral("wxp2p_start")},
                   {QStringLiteral("dispatch_path"), path},
                   {QStringLiteral("remote_ca_file"), peerCaPath}});
      return;
    }
    if (session_.state() != QProcess::NotRunning) {
      sendSession({{QStringLiteral("action"), QStringLiteral("quit")}});
      if (!session_.waitForFinished(1000)) {
        session_.kill();
        session_.waitForFinished(1000);
      }
    }
    sessionOutput_.clear();
    const QString runtime = qEnvironmentVariable(
        "WETYPE_FLURRY_RUNTIME", root + QStringLiteral("/runtime/flurry"));
    const QString host = qEnvironmentVariable(
        "WETYPE_ENGINE_HOST", QStringLiteral(WETYPE_ENGINE_HOST));
    auto environment = session_.processEnvironment();
    environment.insert(QStringLiteral("WETYPE_AUX_RUNTIME"),
                       QFileInfo(runtime).absolutePath() +
                           QStringLiteral("/wxp2p"));
    environment.insert(QStringLiteral("WETYPE_WXP2P_DISPATCH_FILE"), path);
    environment.insert(QStringLiteral("WETYPE_FLURRY_REMOTE_CA_FILE"),
                       peerCaPath);
    session_.setProcessEnvironment(environment);
    status_->setText(QStringLiteral("正在通过安全直连或中继连接手机…"));
    wxpStarted_ = true;
    session_.start(host, {runtime, QStringLiteral("flurry-wxp2p-server")});
  }

  void readSessionEvents() {
    sessionOutput_ += session_.readAllStandardOutput();
    qsizetype newline = -1;
    while ((newline = sessionOutput_.indexOf('\n')) >= 0) {
      const QByteArray line = sessionOutput_.left(newline);
      sessionOutput_.remove(0, newline + 1);
      const QJsonObject event = QJsonDocument::fromJson(line).object();
      const QString type = event.value(QStringLiteral("event")).toString();
      if (type == QStringLiteral("ready")) {
        preloadedWxp2p_ =
            event.value(QStringLiteral("transport")).toString() ==
            QStringLiteral("wxp2p");
        requestCode(event);
      } else if (type == QStringLiteral("grpc_ready")) {
        advertiseGrpc(event);
      } else if (type == QStringLiteral("role_started")) {
        status_->setText(QStringLiteral("已找到设备，正在建立安全连接…"));
      } else if (type == QStringLiteral("connected")) {
        poll_.stop();
        const QPixmap connected(
            transferAsset(QStringLiteral("transfer-images"),
                          QStringLiteral("img_connection_pc_to_phone.png")));
        if (!connected.isNull())
          qr_->setPixmap(connected.scaled(240, 104, Qt::KeepAspectRatio,
                                          Qt::SmoothTransformation));
        title_->setText(QStringLiteral("隔空传送"));
        status_->setText(QStringLiteral("已连接，可以收发文件"));
        send_->setEnabled(true);
      } else if (type == QStringLiteral("disconnected")) {
        status_->setText(QStringLiteral("设备连接已断开"));
        send_->setEnabled(false);
        if (!transferCode_.isEmpty())
          poll_.start();
      } else if (type == QStringLiteral("connect_end")) {
        status_->setText(QStringLiteral("未能连接设备，正在等待手机重试…"));
        send_->setEnabled(false);
        if (!transferCode_.isEmpty())
          poll_.start();
      } else if (type.endsWith(QStringLiteral("_progress"))) {
        const qint64 done =
            wetype::jsonInteger(event.value(QStringLiteral("transferred")));
        const qint64 total = wetype::jsonInteger(event.value(QStringLiteral("total")));
        progress_->show();
        progress_->setValue(total > 0 ? int(done * 1000 / total) : 0);
        status_->setText(type.startsWith(QStringLiteral("upload"))
                             ? QStringLiteral("正在发送文件…")
                             : QStringLiteral("正在接收文件…"));
      } else if (type.endsWith(QStringLiteral("_done"))) {
        const int result = event.value(QStringLiteral("status")).toInt();
        if (result == 0) {
          progress_->setValue(1000);
          status_->setText(type.startsWith(QStringLiteral("upload"))
                               ? QStringLiteral("文件发送完成")
                               : QStringLiteral("文件已保存到下载目录"));
        } else {
          progress_->hide();
          const QString error = event.value(QStringLiteral("error")).toString();
          status_->setText(type.startsWith(QStringLiteral("upload"))
                               ? QStringLiteral("文件发送失败%1")
                                     .arg(error.isEmpty()
                                              ? QString()
                                              : QStringLiteral("：") + error)
                               : QStringLiteral("文件保存失败%1")
                                     .arg(error.isEmpty()
                                              ? QString()
                                              : QStringLiteral("：") + error));
        }
      } else if (type == QStringLiteral("start_failed")) {
        status_->setText(QStringLiteral("无法建立安全传输连接"));
      }
    }
  }

  void requestCode(const QJsonObject &identity) {
    const bool wxp2p =
        identity.value(QStringLiteral("transport")).toString() ==
        QStringLiteral("wxp2p");
    if (wxp2p) {
      const QString path = dataRoot() + QStringLiteral("/state/p2p-info.json");
      QDir().mkpath(QFileInfo(path).absolutePath());
      QSaveFile file(path);
      const QJsonObject info{
          {QStringLiteral("ip"), localAddress()},
          {QStringLiteral("ca_cert"),
           identity.value(QStringLiteral("ca_cert"))},
          {QStringLiteral("server_name"),
           identity.value(QStringLiteral("server_name"))}};
      if (!file.open(QIODevice::WriteOnly) ||
          file.write(QJsonDocument(info).toJson(QJsonDocument::Compact)) < 0 ||
          !file.commit()) {
        status_->setText(QStringLiteral("无法保存传输会话"));
        return;
      }
      QFile::setPermissions(path,
                            QFileDevice::ReadOwner | QFileDevice::WriteOwner);
      const QStringList accountArguments =
          boundUin_.isEmpty()
              ? QStringList{QStringLiteral("p2p-code"), path}
              : QStringList{QStringLiteral("p2p-init"), boundUin_, path};
      requestCodeFromAccount(accountArguments);
      return;
    }
    const QString ip = localAddress();
    if (ip.isEmpty()) {
      status_->setText(QStringLiteral("未找到可用于设备直连的局域网地址"));
      return;
    }
    const QString path = dataRoot() + QStringLiteral("/state/p2p-info.json");
    QDir().mkpath(QFileInfo(path).absolutePath());
    QJsonObject info{
        {QStringLiteral("ip"), ip},
        {QStringLiteral("port"), identity.value(QStringLiteral("port"))},
        {QStringLiteral("ca_cert"), identity.value(QStringLiteral("ca_cert"))},
        {QStringLiteral("server_name"),
         identity.value(QStringLiteral("server_name"))}};
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(QJsonDocument(info).toJson(QJsonDocument::Compact)) < 0 ||
        !file.commit()) {
      status_->setText(QStringLiteral("无法保存传输会话"));
      return;
    }
    QFile::setPermissions(path,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const QStringList accountArguments =
        boundUin_.isEmpty()
            ? QStringList{QStringLiteral("p2p-code"), path}
            : QStringList{QStringLiteral("p2p-init"), boundUin_, path};
    requestCodeFromAccount(accountArguments);
  }

  void requestCodeFromAccount(const QStringList &accountArguments) {
    runAccount(accountArguments, [this](const QJsonObject &result) {
      const QByteArray png = QByteArray::fromBase64(
          result.value(QStringLiteral("transfer_image")).toString().toLatin1());
      QPixmap image;
      transferCode_ = result.value(QStringLiteral("transfer_code")).toString();
      lastDispatch_.clear();
      if (!result.value(QStringLiteral("ok")).toBool() ||
          transferCode_.isEmpty() ||
          (boundUin_.isEmpty() && !image.loadFromData(png, "PNG"))) {
        status_->setText(
            QStringLiteral("无法创建传输二维码，请检查配对和网络状态"));
        return;
      }
      if (boundUin_.isEmpty())
        qr_->setPixmap(image.scaled(286, 286, Qt::KeepAspectRatio,
                                    Qt::SmoothTransformation));
      const int minutes =
          qMax(1, result.value(QStringLiteral("expiration")).toInt() / 60000);
      status_->setText(
          boundUin_.isEmpty()
              ? QStringLiteral("请使用手机扫描；二维码将在 %1 分钟后失效")
                    .arg(minutes)
              : QStringLiteral("已发送邀请；邀请将在 %1 分钟后失效")
                    .arg(minutes));
      poll_.start();
    });
  }

  void pollPeer() {
    if (accountBusy_ || transferCode_.isEmpty())
      return;
    runAccount(
        {QStringLiteral("p2p-peer"), transferCode_},
        [this](const QJsonObject &result) {
          const QJsonObject peer =
              result.value(QStringLiteral("peer")).toObject();
          if (!result.value(QStringLiteral("ok")).toBool())
            return;
          const QByteArray dispatch = QByteArray::fromBase64(
              result.value(QStringLiteral("dispatch_buf"))
                  .toString()
                  .toLatin1());
          if (!dispatch.isEmpty()) {
            startWxp2p(dispatch, peer);
            return;
          }
          if (!peer.value(QStringLiteral("ready")).toBool())
            return;
          const auto grpc = parseGrpcInfo(
              QByteArray::fromBase64(peer.value(QStringLiteral("grpc_net_info"))
                                         .toString()
                                         .toLatin1()));
          if (grpc.ca.isEmpty())
            return;
          poll_.stop();
          const QString caPath =
              dataRoot() + QStringLiteral("/state/transfer/peer-ca.pem");
          QSaveFile ca(caPath);
          if (!ca.open(QIODevice::WriteOnly) ||
              ca.write(grpc.ca.toUtf8()) < 0 || !ca.commit()) {
            status_->setText(QStringLiteral("无法保存对端安全凭据"));
            return;
          }
          QFile::setPermissions(caPath, QFileDevice::ReadOwner |
                                            QFileDevice::WriteOwner);
          sendSession({{QStringLiteral("action"), QStringLiteral("server")},
                       {QStringLiteral("remote_ca_file"), caPath}});
        });
  }

  void advertiseGrpc(const QJsonObject &identity) {
    if (grpcAdvertised_ || transferCode_.isEmpty())
      return;
    const QString ip = localAddress();
    const int port = identity.value(QStringLiteral("port")).toInt();
    if (ip.isEmpty() || port <= 0) {
      status_->setText(QStringLiteral("无法发布手机传输端点"));
      return;
    }
    grpcAdvertised_ = true;
    poll_.stop();
    const QString path =
        dataRoot() + QStringLiteral("/state/transfer/p2p-update.json");
    QSaveFile file(path);
    const QJsonObject info{
        {QStringLiteral("ip"), ip},
        {QStringLiteral("port"), port},
        {QStringLiteral("ca_cert"),
         identity.value(QStringLiteral("ca_cert"))},
        {QStringLiteral("server_name"),
         identity.value(QStringLiteral("server_name"))},
        {QStringLiteral("preferred_client"), false}};
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(QJsonDocument(info).toJson(QJsonDocument::Compact)) < 0 ||
        !file.commit()) {
      status_->setText(QStringLiteral("无法保存传输端点"));
      grpcAdvertised_ = false;
      poll_.start();
      return;
    }
    QFile::setPermissions(path,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    runAccount({QStringLiteral("p2p-update"), transferCode_, path},
               [this](const QJsonObject &result) {
                 if (!result.value(QStringLiteral("ok")).toBool()) {
                   status_->setText(
                       QStringLiteral("无法向手机发布安全传输端点"));
                   grpcAdvertised_ = false;
                   poll_.start();
                   return;
                 }
                 status_->setText(
                     QStringLiteral("已发布安全端点，正在连接手机…"));
                 poll_.start();
               });
  }

  void runAccount(const QStringList &arguments,
                  std::function<void(const QJsonObject &)> callback) {
    if (accountBusy_)
      return;
    accountBusy_ = true;
    auto *process = new QProcess(this);
    wetype::onProcessDone(
        process, this,
        [this, process, callback = std::move(callback)](int,
                                                        QProcess::ExitStatus) {
          accountBusy_ = false;
          const QJsonObject result =
              QJsonDocument::fromJson(process->readAllStandardOutput())
                  .object();
          process->deleteLater();
          callback(result);
        });
    process->start(qEnvironmentVariable("WETYPE_ACCOUNT_TOOL",
                                        QStringLiteral(WETYPE_ACCOUNT_TOOL)),
                   arguments);
  }

  void sendSession(const QJsonObject &command) {
    if (session_.state() == QProcess::Running) {
      session_.write(QJsonDocument(command).toJson(QJsonDocument::Compact));
      session_.write("\n");
    }
  }

  QLabel *title_ = nullptr;
  QWidget *home_ = nullptr;
  QToolButton *qrClose_ = nullptr;
  QLabel *qr_ = nullptr;
  QLabel *status_ = nullptr;
  QProgressBar *progress_ = nullptr;
  QPushButton *send_ = nullptr;
  QProcess session_;
  QByteArray sessionOutput_;
  QTimer poll_;
  QString transferCode_;
  QString boundUin_;
  QByteArray lastDispatch_;
  bool accountBusy_ = false;
  bool preloadedWxp2p_ = false;
  bool wxpStarted_ = false;
  bool grpcAdvertised_ = false;
};

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  app.setApplicationName(QStringLiteral("fcitx5-wetypex-transfer"));
  TransferWindow window;
  window.show();
  return app.exec();
}
