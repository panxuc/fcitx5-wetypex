#include "../common/qt_helpers.hpp"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWebEngineView>
#include <QWidget>

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
      QStringLiteral("<style>:root{color-scheme:light}.wr_dynamicTheme{"
                     "--WT_BC2:rgba(0,0,0,.8)!important}body{background:#fafafa}"
                     ".wetypex-visible{opacity:1!important;visibility:visible!important}"
                     ".wetypex-query-row{display:flex;justify-content:flex-end;padding:14px 16px 4px}"
                     ".wetypex-query{position:relative;max-width:82%;padding:5px 12px;background:#d9f3e9;"
                     "border-radius:12px 12px 2px 12px;color:#222;font-size:12px;line-height:18px}"
                     ".wetypex-query:after{content:'';position:absolute;right:-5px;bottom:0;"
                     "width:8px;height:10px;background:#d9f3e9;clip-path:polygon(0 0,0 100%,100% 100%)}"
                     ".pc_markdown-body__TGijn p{font-size:13px!important;line-height:22px!important}"
                     "</style></head>"));
  return html;
}

class AiWindow final : public QWidget {
public:
  explicit AiWindow(const QString &initialResponse, const QString &initialQuery)
      : currentQuery_(initialQuery) {
    setWindowFlag(Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowIcon(QIcon::fromTheme(QStringLiteral("fcitx5-wetypex")));
    setWindowTitle(QStringLiteral("问Ai"));
    resize(440, 566);
    setMinimumSize(352, 453);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(1, 1, 1, 1);
    outer->setSpacing(0);
    auto *surface = new QWidget;
    surface->setObjectName(QStringLiteral("surface"));
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
    polish_->setFixedWidth(69);
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
                 border-radius: 9px; }
      #chromeButton, #closeButton { border: 0; border-radius: 0;
                                   background: transparent; }
      #chromeButton:hover { background: #ececec; }
      #closeButton:hover { background: #e75b52; }
      QWebEngineView { background: #fafafa; border: 0; }
      #composer { background: #fafafa; }
      #polish { color: #555; background: #fff; border: 1px solid #e4e4e4;
                border-radius: 6px; padding: 0 9px; font-size: 11px; }
      #polish:hover { background: #f4f4f4; }
      #inputShell { background: #fff; border: 1px solid #f0f0f0;
                    border-radius: 14px; }
      #sendButton { border: 0; border-radius: 11px; background: #23c891; padding: 0; }
      #sendButton:disabled { background: #beefde; }
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
    connect(input_, &QLineEdit::textChanged, this, [this](const QString &text) {
      send_->setEnabled(!text.trimmed().isEmpty());
    });
    connect(input_, &QLineEdit::returnPressed, this, [this] { ask(false); });
    connect(send_, &QPushButton::clicked, this, [this] { ask(false); });
    connect(polish_, &QPushButton::clicked, this, [this] { ask(true); });

    if (!initialResponse.isEmpty())
      loadResponse(initialResponse, initialQuery);
    else
      response_->setHtml(QStringLiteral(
          "<html><body style='background:#fafafa'></body></html>"));
  }

protected:
  void mousePressEvent(QMouseEvent *event) override {
    if (wetype::mousePosition(event).y() <= 31 && event->button() == Qt::LeftButton) {
      dragOffset_ = wetype::mouseGlobalPosition(event) - frameGeometry().topLeft();
      dragging_ = true;
    }
  }
  void mouseMoveEvent(QMouseEvent *event) override {
    if (dragging_ && event->buttons().testFlag(Qt::LeftButton))
      move(wetype::mouseGlobalPosition(event) - dragOffset_);
  }
  void mouseReleaseEvent(QMouseEvent *) override { dragging_ = false; }

private:
  void loadResponse(const QString &path, const QString &query) {
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly))
      return;
    response_->setHtml(prepareResponse(QString::fromUtf8(input.readAll()), query),
                       QUrl(QStringLiteral(
                           "https://cdn.weread.qq.com/web/winktemplaterendersvr/")));
  }
  void ask(bool polish) {
    QString question = input_->text().trimmed();
    if (question.isEmpty())
      return;
    const QString displayedQuestion = question;
    if (polish)
      question.prepend(
          QStringLiteral("请优化下面这段表达，并只给出优化后的文本：\n"));
    input_->setEnabled(false);
    currentQuery_ = displayedQuestion;
    send_->setEnabled(false);
    polish_->setEnabled(false);
    const QString output =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
        QStringLiteral("/ai-response.html");
    QDir().mkpath(QFileInfo(output).absolutePath());
    auto *process = new QProcess(this);
    wetype::onProcessDone(
        process, this,
        [this, process, output](int code, QProcess::ExitStatus status) {
          input_->setEnabled(true);
          polish_->setEnabled(true);
          if (status == QProcess::NormalExit && code == 0) {
            loadResponse(output, currentQuery_);
            input_->clear();
          }
          send_->setEnabled(!input_->text().trimmed().isEmpty());
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
  QString currentQuery_;
};
} // namespace

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  app.setApplicationName(QStringLiteral("fcitx5-wetypex-ai"));
  AiWindow window(argc >= 2 ? QString::fromLocal8Bit(argv[1]) : QString(),
                  argc >= 3 ? QString::fromLocal8Bit(argv[2]) : QString());
  window.show();
  return app.exec();
}
