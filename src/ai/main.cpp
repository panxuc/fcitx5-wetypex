#include "../common/qt_helpers.hpp"
#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPalette>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <QWidget>
#include <QWindow>

namespace {
QString asset(const char *name) {
  return QStandardPaths::locate(
      QStandardPaths::GenericDataLocation,
      QStringLiteral("fcitx5-wetypex/ui/ai-icons/") +
          QString::fromLatin1(name));
}

QString prepareResponse(QString html, const QString &query) {
  // The original host drives per-character fades and theme through a WebView
  // bridge. Preserve the server-rendered document and reveal its light state.
  html.remove(QRegularExpression(
      QStringLiteral("<script\\b[^>]*>[\\s\\S]*?</script\\s*>"),
      QRegularExpression::CaseInsensitiveOption));
  html.replace(QStringLiteral("fade-in_fade-in__L_93a"),
               QStringLiteral("wetypex-visible"));
  html.replace(QStringLiteral("opacity:0"), QStringLiteral("opacity:1"));
  const QString queryHtml =
      query.isEmpty()
          ? QString()
          : QStringLiteral("<div class='wetypex-query-row'><div class='wetypex-query'>%1</div></div>")
                .arg(query.toHtmlEscaped());
  html.replace(QStringLiteral("<body>"), QStringLiteral("<body>") + queryHtml);
  html.replace(
      QStringLiteral("</head>"),
      QStringLiteral(
          "<style>:root{color-scheme:light}.wr_dynamicTheme{"
          "--WT_BC2:rgba(0,0,0,.8)!important}"
          "html,body{height:auto!important;min-height:100%!important;margin:0;"
          "background:#fafafa!important;overflow-x:hidden!important;overflow-y:auto!important}"
          ".pc_wr_main_container__6Toi1{height:auto!important;"
          "min-height:calc(100vh - 46px)!important;overflow:visible!important}"
          ".pc_wr_main_content__91zKP{overflow:visible!important;min-height:0!important}"
          ".wr_main_scroll{height:auto!important;overflow:visible!important}"
          "::-webkit-scrollbar{width:6px}"
          "::-webkit-scrollbar-thumb{background:rgba(0,0,0,.08);border-radius:3px}"
          "::-webkit-scrollbar-track{background:transparent}"
          ".wetypex-visible{opacity:1!important;visibility:visible!important}"
          ".wetypex-query-row{display:flex;justify-content:flex-end;padding:14px 16px 4px}"
          ".wetypex-query{position:relative;max-width:82%;padding:5px 12px;background:#d9f3e9;"
          "border-radius:12px 12px 2px 12px;color:#222;font-size:12px;line-height:18px;"
          "white-space:pre-wrap;word-break:break-word}"
          ".wetypex-query:after{content:'';position:absolute;right:-5px;bottom:0;"
          "width:8px;height:10px;background:#d9f3e9;clip-path:polygon(0 0,0 100%,100% 100%)}"
          ".pc_markdown-body__TGijn{font-size:13px!important;line-height:22px!important}"
          ".pc_markdown-body__TGijn p,.pc_markdown-body__TGijn li{"
          "font-size:13px!important;line-height:22px!important}"
          ".pc_markdown-body__TGijn h1,.pc_markdown-body__TGijn h2,"
          ".pc_markdown-body__TGijn h3,.pc_markdown-body__TGijn h4,"
          ".pc_markdown-body__TGijn h5,.pc_markdown-body__TGijn h6{"
          "font-size:14px!important;line-height:22px!important;margin:10px 0 4px!important}"
          "</style></head>"));
  return html;
}

QString statusPage(const QString &query, const QString &message, bool animated) {
  const QString dots = animated
                           ? QStringLiteral("<span class='dots'><span></span>"
                                            "<span></span><span></span></span>")
                           : QString();
  return QStringLiteral(
             "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><style>"
             "html,body{width:100%;height:100%;margin:0;background:#fafafa;color:#222;"
             "font-family:-apple-system,'PingFang SC','Microsoft YaHei',sans-serif}"
             "body{overflow-y:auto}"
             ".query-row{display:flex;justify-content:flex-end;padding:14px 16px 4px}"
             ".query{max-width:82%;padding:5px 12px;background:#d9f3e9;"
             "border-radius:12px 12px 2px 12px;font-size:12px;line-height:18px;"
             "white-space:pre-wrap;word-break:break-word}"
             ".status{display:flex;align-items:center;padding:12px 18px;"
             "font-size:13px;line-height:20px;color:#6b7075}"
             ".dots{display:inline-flex;align-items:center;margin-left:6px}"
             ".dots span{width:4px;height:4px;margin-left:3px;border-radius:50%;"
             "background:#8a8f94;animation:blink 1.2s infinite ease-in-out}"
             ".dots span:nth-child(2){animation-delay:.16s}"
             ".dots span:nth-child(3){animation-delay:.32s}"
             "@keyframes blink{0%,80%,100%{opacity:.2;transform:translateY(0)}"
             "40%{opacity:1;transform:translateY(-2px)}}"
             "</style></head><body>"
             "<div class='query-row'><div class='query'>%1</div></div>"
             "<div class='status'>%2%3</div></body></html>")
      .arg(query.toHtmlEscaped(), message, dots);
}

class AiWindow final : public QWidget {
public:
  explicit AiWindow(const QString &initialResponse, const QString &initialQuery,
                    const QString &initialAsk) {
    setWindowFlag(Qt::FramelessWindowHint);
    setWindowIcon(QIcon::fromTheme(QStringLiteral("fcitx5-wetypex")));
    setWindowTitle(QStringLiteral("问Ai"));
    resize(440, 566);
    setMinimumSize(352, 453);
    setAutoFillBackground(true);
    QPalette windowPalette = palette();
    windowPalette.setColor(QPalette::Window, QColor(QStringLiteral("#fafafa")));
    setPalette(windowPalette);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(1, 1, 1, 1);
    outer->setSpacing(0);
    auto *surface = new QWidget;
    surface->setObjectName(QStringLiteral("surface"));
    surface->setAttribute(Qt::WA_StyledBackground, true);
    outer->addWidget(surface);
    auto *root = new QVBoxLayout(surface);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *title = new QWidget;
    title->setFixedHeight(31);
    auto *titleLayout = new QHBoxLayout(title);
    titleLayout->setContentsMargins(10, 0, 3, 0);
    titleLayout->setSpacing(0);
    auto *leftChromeSpacer = new QWidget;
    leftChromeSpacer->setFixedWidth(66);
    titleLayout->addWidget(leftChromeSpacer);
    titleLayout->addStretch();
    auto *logo = new QLabel;
    logo->setPixmap(QIcon(asset("ai-logo-light.svg")).pixmap(36, 15));
    logo->setFixedSize(36, 15);
    logo->setAlignment(Qt::AlignCenter);
    titleLayout->addWidget(logo);
    titleLayout->addStretch();
    auto makeChromeButton = [](const char *icon, const char *name) {
      auto *button = new QPushButton;
      button->setFixedSize(24, 26);
      button->setIcon(QIcon(asset(icon)));
      button->setIconSize(QSize(12, 12));
      button->setFlat(true);
      button->setFocusPolicy(Qt::NoFocus);
      button->setObjectName(QString::fromLatin1(name));
      return button;
    };
    auto *minimize = makeChromeButton("chrome-minimize.png", "chromeButton");
    auto *maximize = makeChromeButton("chrome-maximize.png", "chromeButton");
    auto *close = makeChromeButton("chrome-close.png", "closeButton");
    titleLayout->addWidget(minimize);
    titleLayout->addWidget(maximize);
    titleLayout->addWidget(close);
    root->addWidget(title);

    response_ = new QWebEngineView;
    response_->setContextMenuPolicy(Qt::NoContextMenu);
    response_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    response_->setMinimumSize(0, 0);
    response_->page()->setBackgroundColor(QColor(QStringLiteral("#fafafa")));
    root->addWidget(response_, 1);

    auto *composer = new QWidget;
    composer->setObjectName(QStringLiteral("composer"));
    auto *composerLayout = new QVBoxLayout(composer);
    composerLayout->setContentsMargins(15, 3, 15, 6);
    composerLayout->setSpacing(9);
    auto *polishRow = new QHBoxLayout;
    polish_ = new QPushButton(QStringLiteral("优化表达"));
    polish_->setObjectName(QStringLiteral("polish"));
    polish_->setIcon(QIcon(asset("ai-polish.svg")));
    polish_->setIconSize(QSize(14, 14));
    polish_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    polish_->setFixedHeight(26);
    polishRow->addWidget(polish_);
    polishRow->addStretch();
    composerLayout->addLayout(polishRow);
    auto *inputShell = new QWidget;
    inputShell->setObjectName(QStringLiteral("inputShell"));
    inputShell->setFixedHeight(38);
    auto *inputLayout = new QHBoxLayout(inputShell);
    inputLayout->setContentsMargins(12, 0, 9, 0);
    inputLayout->setSpacing(8);
    input_ = new QLineEdit;
    input_->setPlaceholderText(QStringLiteral("输入问题，获取 AI 回答"));
    input_->setFrame(false);
    send_ = new QPushButton;
    send_->setObjectName(QStringLiteral("sendButton"));
    send_->setFixedSize(22, 22);
    send_->setIcon(QIcon(asset("ai-send.svg")));
    send_->setIconSize(QSize(22, 22));
    send_->setCursor(Qt::PointingHandCursor);
    send_->setFocusPolicy(Qt::NoFocus);
    send_->setEnabled(false);
    inputLayout->addWidget(input_, 1);
    inputLayout->addWidget(send_);
    composerLayout->addWidget(inputShell);
    root->addWidget(composer);

    setStyleSheet(QStringLiteral(R"(
      #surface { background: #fafafa; border: 1px solid #d9dfe4;
                 border-radius: 0; }
      #chromeButton, #closeButton { border: 0; border-radius: 0;
                                   background: transparent; }
      #chromeButton:hover { background: #ececec; }
      #closeButton:hover { background: #e75b52; }
      QWebEngineView { background: #fafafa; border: 0; }
      #composer { background: #fafafa; }
      #polish { color: #555; background: #fff; border: 1px solid #e4e4e4;
                border-radius: 6px; padding: 0 9px; font-size: 11px; }
      #polish:hover { background: #f4f4f4; }
      #polish:disabled { color: #aaa; background: #f6f6f6; }
      #inputShell { background: #fff; border: 1px solid #e3e6e8;
                    border-radius: 14px; }
      #sendButton { border: 0; border-radius: 11px; background: #23c891; padding: 0; }
      #sendButton:hover { background: #1fbd88; }
      #sendButton:disabled { background: #a5e0ca; }
      QLineEdit { background: transparent; color: #222; font-size: 11px;
                  selection-background-color: #23c891; }
      QLineEdit:disabled { color: #999; }
    )"));

    connect(close, &QPushButton::clicked, this, &QWidget::close);
    connect(minimize, &QPushButton::clicked, this, &QWidget::showMinimized);
    connect(maximize, &QPushButton::clicked, this, [this, maximize] {
      if (isMaximized()) {
        showNormal();
        maximize->setIcon(QIcon(asset("chrome-maximize.png")));
      } else {
        showMaximized();
        maximize->setIcon(QIcon(asset("chrome-unmaximize.png")));
      }
    });
    connect(input_, &QLineEdit::textChanged, this, [this] { refreshControls(); });
    connect(input_, &QLineEdit::returnPressed, this, [this] { ask(false); });
    connect(send_, &QPushButton::clicked, this, [this] { ask(false); });
    connect(polish_, &QPushButton::clicked, this, [this] { ask(true); });

    if (!initialAsk.isEmpty()) {
      input_->setText(initialAsk);
      QTimer::singleShot(0, this, [this] { ask(false); });
    } else if (!initialResponse.isEmpty()) {
      loadResponse(initialResponse, initialQuery);
    } else {
      response_->setHtml(QStringLiteral(
          "<html><body style='height:100%;margin:0;background:#fafafa'>"
          "</body></html>"));
    }
  }

protected:
  void mousePressEvent(QMouseEvent *event) override {
    if (event->button() != Qt::LeftButton ||
        wetype::mousePosition(event).y() > 31)
      return;

#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    if (windowHandle() && windowHandle()->startSystemMove()) {
      event->accept();
      return;
    }
#endif

    dragOffset_ =
        wetype::mouseGlobalPosition(event) - frameGeometry().topLeft();
    dragging_ = true;
    event->accept();
  }
  void mouseMoveEvent(QMouseEvent *event) override {
    if (dragging_ && event->buttons().testFlag(Qt::LeftButton))
      move(wetype::mouseGlobalPosition(event) - dragOffset_);
  }
  void mouseReleaseEvent(QMouseEvent *) override { dragging_ = false; }

private:
  bool loadResponse(const QString &path, const QString &query) {
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly))
      return false;
    response_->setHtml(prepareResponse(QString::fromUtf8(input.readAll()), query),
                       QUrl(QStringLiteral(
                           "https://cdn.weread.qq.com/web/winktemplaterendersvr/")));
    return true;
  }
  void refreshControls() {
    send_->setEnabled(!busy_ && !input_->text().trimmed().isEmpty());
    polish_->setEnabled(!busy_);
  }
  void ask(bool polish) {
    if (busy_)
      return;
    QString question = input_->text().trimmed();
    if (question.isEmpty())
      return;
    const QString displayedQuestion = question;
    auto requestDir = std::make_shared<QTemporaryDir>(
        QDir::tempPath() + QStringLiteral("/fcitx5-wetypex-ai-XXXXXX"));
    if (!requestDir->isValid()) {
      response_->setHtml(
          statusPage(displayedQuestion,
                     QStringLiteral("无法创建临时目录，请稍后重试"), false));
      return;
    }
    if (polish)
      question.prepend(
          QStringLiteral("请优化下面这段表达，并只给出优化后的文本：\n"));
    busy_ = true;
    input_->clear();
    input_->setPlaceholderText(QStringLiteral("正在生成回答…"));
    refreshControls();
    response_->setHtml(statusPage(displayedQuestion,
                                  QStringLiteral("正在生成回答"), true));
    const QString output =
        requestDir->filePath(QStringLiteral("response.html"));
    auto *process = new QProcess(this);
    wetype::onProcessDone(
        process, this,
        [this, process, output, displayedQuestion,
         requestDir](int code, QProcess::ExitStatus status) {
          busy_ = false;
          input_->setPlaceholderText(QStringLiteral("输入问题，获取 AI 回答"));
          const bool loaded =
              status == QProcess::NormalExit && code == 0 &&
              loadResponse(output, displayedQuestion);
          if (!loaded) {
            if (input_->text().isEmpty())
              input_->setText(displayedQuestion);
            response_->setHtml(statusPage(displayedQuestion,
                                          QStringLiteral("生成失败，请稍后重试"),
                                          false));
          }
          refreshControls();
          process->deleteLater();
        });
    process->start(QStringLiteral(WETYPE_AI_HELPER),
                   {QStringLiteral("--fetch"), output, question});
  }

  QWebEngineView *response_ = nullptr;
  QLineEdit *input_ = nullptr;
  QPushButton *send_ = nullptr;
  QPushButton *polish_ = nullptr;
  QPoint dragOffset_;
  bool dragging_ = false;
  bool busy_ = false;
};
} // namespace

int main(int argc, char **argv) {
  // Wayland GPU compositing gives WebEngine a separate surface whose page
  // background shows the desktop through it; software compositing stays opaque.
  QByteArray webEngineFlags = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
  const auto platform = qgetenv("QT_QPA_PLATFORM");
  const bool wayland =
      platform.startsWith("wayland") ||
      (platform.isEmpty() &&
       (!qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") ||
        qEnvironmentVariable("XDG_SESSION_TYPE") == "wayland"));
  if (wayland && !webEngineFlags.contains("--disable-gpu")) {
    if (!webEngineFlags.isEmpty())
      webEngineFlags += ' ';
    webEngineFlags += "--disable-gpu";
  }
  qputenv("QTWEBENGINE_CHROMIUM_FLAGS", webEngineFlags);
  QApplication app(argc, argv);
  app.setApplicationName(QStringLiteral("fcitx5-wetypex-ai"));
  app.setDesktopFileName(QStringLiteral("fcitx5-wetypex-ai"));
  QString initialResponse;
  QString initialQuery;
  QString initialAsk;
  if (argc >= 2 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--ask")) {
    initialAsk = argc >= 3 ? QString::fromLocal8Bit(argv[2]) : QString();
  } else {
    initialResponse = argc >= 2 ? QString::fromLocal8Bit(argv[1]) : QString();
    initialQuery = argc >= 3 ? QString::fromLocal8Bit(argv[2]) : QString();
  }
  AiWindow window(initialResponse, initialQuery, initialAsk);
  window.show();
  return app.exec();
}
