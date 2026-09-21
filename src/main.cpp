// Minimal PDF viewer: Qt WebEngine + PDF.js
//
// Everything (the PDF.js viewer *and* the document being viewed) is served
// through one custom URL scheme, pdfjs://app/. That gives the viewer and the
// PDF the same origin, so PDF.js's origin check passes, ES modules and web
// workers load normally, and no file:// security switches are needed.

#include <QAction>
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QColor>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMainWindow>
#include <QMessageBox>
#include <QPalette>
#include <QPrintDialog>
#include <QPrinter>
#include <QStandardPaths>
#include <QUrl>
#include <QWebEngineDownloadRequest>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineUrlScheme>
#include <QWebEngineUrlSchemeHandler>
#include <QWebEngineView>

#include <functional>
#include <memory>

#include <unistd.h>

// ---------------------------------------------------------------------------
// pdfjs://app/doc/<id>/<name> -> a PDF registered with addDocument() (one per window)
// pdfjs://app/<path>   -> <pdfjs root>/<path>   (viewer.html, pdf.mjs, ...)
// ---------------------------------------------------------------------------
class PdfJsSchemeHandler : public QWebEngineUrlSchemeHandler
{
public:
    explicit PdfJsSchemeHandler(const QString &root)
        : m_root(QDir::cleanPath(QFileInfo(root).absoluteFilePath())) {}

    // Registers a PDF and returns the id to use in its /doc/<id>/<name> URL.
    int addDocument(const QString &path)
    {
        m_documents.insert(++m_lastId, path);
        return m_lastId;
    }

    void requestStarted(QWebEngineUrlRequestJob *job) override
    {
        const QString urlPath = job->requestUrl().path();

        QString filePath;
        if (urlPath.startsWith(QLatin1String("/doc/"))) {
            filePath = m_documents.value(urlPath.section(QLatin1Char('/'), 2, 2).toInt());
        } else {
            const QString candidate = QDir::cleanPath(m_root + urlPath);
            if (candidate.startsWith(m_root + QLatin1Char('/')))   // no path traversal
                filePath = candidate;
        }

        auto *file = new QFile(filePath, job);   // owned by the job, freed with it
        if (filePath.isEmpty() || !QFileInfo(filePath).isFile()
                || !file->open(QIODevice::ReadOnly)) {
            job->fail(QWebEngineUrlRequestJob::UrlNotFound);
            return;
        }
        job->reply(mimeType(filePath), file);
    }

private:
    static QByteArray mimeType(const QString &path)
    {
        static const QHash<QString, QByteArray> types = {
            {"html", "text/html"},        {"js", "text/javascript"},
            {"mjs", "text/javascript"},   {"css", "text/css"},
            {"json", "application/json"}, {"svg", "image/svg+xml"},
            {"png", "image/png"},         {"gif", "image/gif"},
            {"wasm", "application/wasm"}, {"pdf", "application/pdf"},
            {"ftl", "text/plain"},
        };
        return types.value(QFileInfo(path).suffix().toLower(),
                           QByteArray("application/octet-stream"));
    }

    QString m_root;
    QHash<int, QString> m_documents;
    int m_lastId = 0;
};

// ---------------------------------------------------------------------------
// Dark background, always
//
// Without this the window starts white (empty web view), then turns PDF.js's
// light gray once the viewer loads - a visible flash. So:
//  * the web view and the window are dark from the very first frame, in the
//    same color PDF.js uses for its dark theme, and
//  * PDF.js is forced into its dark theme *at document creation*, before the
//    first paint, regardless of the desktop's light/dark setting.
// ---------------------------------------------------------------------------
static const QColor kBackground(42, 42, 46);      // PDF.js dark: rgb(42 42 46)

static const char kDarkThemeScript[] = R"JS(
(() => {
  const sheet = new CSSStyleSheet();
  sheet.replaceSync(`
    :root { color-scheme: dark !important; }
    html  { background-color: rgb(42 42 46); }
  `);
  document.adoptedStyleSheets = [...document.adoptedStyleSheets, sheet];
})();
)JS";

// ---------------------------------------------------------------------------
// UI tweaks injected into the PDF.js viewer page
//
//  1. Use the desktop's UI font instead of Chromium's built-in default.
//  2. Show the "text selection" and "hand" tools in the main toolbar. PDF.js
//     only offers them in the >> menu. The new buttons just click the existing
//     ones and mirror their state and (already localised) tooltip, so keyboard
//     shortcuts, presentation mode and translations keep working.
//
// @FONT@ is replaced with a JSON array holding the font family name.
// ---------------------------------------------------------------------------
static const char kUiTweaksScript[] = R"JS(
(() => {
  const [fontFamily] = @FONT@;

  // A constructed stylesheet: PDF.js's CSP (style-src 'self') blocks <style> elements.
  const sheet = new CSSStyleSheet();
  sheet.replaceSync(`
    html, body, button, input, select, option, textarea, dialog,
    #toolbarContainer, .toolbarButton, .popup, .editorParamsSlider,
    #sidebarContainer :is(input, button, select) {
      font-family: ${JSON.stringify(fontFamily)}, system-ui, sans-serif !important;
    }
    #pvToolSelect::before {
      -webkit-mask-image: var(--secondaryToolbarButton-selectTool-icon);
              mask-image: var(--secondaryToolbarButton-selectTool-icon);
    }
    #pvToolHand::before {
      -webkit-mask-image: var(--secondaryToolbarButton-handTool-icon);
              mask-image: var(--secondaryToolbarButton-handTool-icon);
    }
  `);
  document.adoptedStyleSheets = [...document.adoptedStyleSheets, sheet];

  const right = document.getElementById("toolbarViewerRight");
  const tools = [["pvToolSelect", "cursorSelectTool"], ["pvToolHand", "cursorHandTool"]];
  if (!right || tools.some(([, orig]) => !document.getElementById(orig))) return;

  const group = document.createElement("div");
  group.className = "toolbarHorizontalGroup hiddenMediumView";   // hides on narrow windows

  for (const [id, origId] of tools) {
    const orig = document.getElementById(origId);
    const btn = document.createElement("button");
    btn.id = id;
    btn.type = "button";
    btn.className = "toolbarButton";
    btn.tabIndex = 0;
    btn.addEventListener("click", () => orig.click());
    const sync = () => {
      btn.classList.toggle("toggled", orig.classList.contains("toggled"));
      btn.title = orig.title;                       // PDF.js fills this in, localised
      btn.setAttribute("aria-label", orig.title);
      btn.setAttribute("aria-pressed", String(orig.classList.contains("toggled")));
    };
    new MutationObserver(sync).observe(orig, {
      attributes: true, attributeFilter: ["class", "title"],
    });
    sync();
    group.append(btn);
  }

  const separator = document.createElement("div");
  separator.className = "verticalToolbarSeparator hiddenMediumView";
  right.prepend(group, separator);
})();
)JS";

// ---------------------------------------------------------------------------
// Printing
//
// PDF.js prints by rendering every page into a hidden container and then
// calling window.print(). It removes that container again almost immediately,
// so a print dialog opened *after* window.print() (e.g. from printRequested)
// would print an empty page.
//
// So we put a gate in front of PDF.js's window.print: it calls confirm() with
// a magic message. confirm() is synchronous - the page is frozen while Qt shows
// the (CUPS-aware) QPrintDialog - and only if the user accepts does PDF.js
// start its normal pipeline. When that reaches the native print, Qt emits
// printRequested and we print with the QPrinter the user just configured.
// ---------------------------------------------------------------------------
static const char kPrintGateScript[] = R"JS(
(() => {
  const nativePrint = window.print;   // PDF.js captures this one at load time
  let viewerPrint = null;             // ...and then assigns its own window.print
  function gate() {
    const pages = window.PDFViewerApplication?.pagesCount ?? 0;
    if (!confirm("__pdfviewer_print__:" + pages)) return;
    (viewerPrint ?? nativePrint).call(window);
  }
  Object.defineProperty(window, "print", {
    configurable: true,
    get() { return viewerPrint ? gate : nativePrint; },
    set(fn) { viewerPrint = fn; },
  });
})();
)JS";

static QString uiTweaksSource()
{
    // Family name as a JSON array, e.g. ["Adwaita Sans"] - safe to paste into JS.
    const QString family = QString::fromUtf8(
        QJsonDocument(QJsonArray{QApplication::font().family()}).toJson(QJsonDocument::Compact));
    return QString::fromLatin1(kUiTweaksScript).replace(QLatin1String("@FONT@"), family);
}

class ViewerPage : public QWebEnginePage
{
public:
    using QWebEnginePage::QWebEnginePage;

    // Show the print dialog; return true if the user accepted.
    std::function<bool(int pageCount)> printDialog;

protected:
    bool javaScriptConfirm(const QUrl &origin, const QString &message) override
    {
        static const QString prefix = QStringLiteral("__pdfviewer_print__:");
        if (printDialog && message.startsWith(prefix))
            return printDialog(message.mid(prefix.size()).toInt());
        return QWebEnginePage::javaScriptConfirm(origin, message);
    }
};

// ---------------------------------------------------------------------------
class Viewer : public QMainWindow
{
public:
    Viewer(QWebEngineProfile *profile, PdfJsSchemeHandler *handler)
        : m_handler(handler)
    {
        setAttribute(Qt::WA_DeleteOnClose);      // one process, many windows

        m_view = new QWebEngineView(this);
        auto *page = new ViewerPage(profile, m_view);
        page->printDialog = [this](int pages) { return askPrinter(pages); };

        QWebEngineScript gate;
        gate.setName(QStringLiteral("pdfviewer-print-gate"));
        gate.setSourceCode(QString::fromLatin1(kPrintGateScript));
        gate.setInjectionPoint(QWebEngineScript::DocumentCreation);
        gate.setWorldId(QWebEngineScript::MainWorld);      // must see the page's window.print
        page->scripts().insert(gate);

        QWebEngineScript tweaks;
        tweaks.setName(QStringLiteral("pdfviewer-ui-tweaks"));
        tweaks.setSourceCode(uiTweaksSource());
        tweaks.setInjectionPoint(QWebEngineScript::DocumentReady);   // DOM exists, viewer not yet initialised
        tweaks.setWorldId(QWebEngineScript::ApplicationWorld);       // isolated from PDF.js, shares the DOM
        page->scripts().insert(tweaks);

        QWebEngineScript dark;
        dark.setName(QStringLiteral("pdfviewer-dark-theme"));
        dark.setSourceCode(QString::fromLatin1(kDarkThemeScript));
        dark.setInjectionPoint(QWebEngineScript::DocumentCreation);  // before the first paint
        dark.setWorldId(QWebEngineScript::ApplicationWorld);
        page->scripts().insert(dark);

        page->setBackgroundColor(kBackground);   // shown until the page has painted
        QPalette pal = palette();
        pal.setColor(QPalette::Window, kBackground);
        setPalette(pal);
        setAutoFillBackground(true);

        m_view->setPage(page);
        // PDF.js does the rendering, not Chromium's built-in PDF plugin.
        m_view->settings()->setAttribute(QWebEngineSettings::PdfViewerEnabled, false);
        setCentralWidget(m_view);
        resize(1000, 800);
        setWindowTitle(tr("PDF Viewer"));

        connect(m_view, &QWebEngineView::titleChanged, this, [this](const QString &t) {
            if (!t.isEmpty() && !t.startsWith(QLatin1String("pdfjs://")))
                setWindowTitle(t);
        });

        // Native print step, reached after the user accepted the print dialog.
        connect(m_view, &QWebEngineView::printRequested, this, [this] {
            if (m_printer)
                m_view->print(m_printer.get());
        });
        connect(m_view, &QWebEngineView::printFinished, this, [this](bool ok) {
            m_printer.reset();               // must outlive the job until this signal
            if (!ok)
                QMessageBox::warning(this, tr("Print"), tr("Printing failed."));
        });

        // PDF.js "Save"/"Download" buttons end up here.
        connect(profile, &QWebEngineProfile::downloadRequested, this,
                [this](QWebEngineDownloadRequest *dl) {
            if (dl->page() != m_view->page())    // the profile is shared by all windows
                return;
            const QString target = QFileDialog::getSaveFileName(
                this, tr("Save PDF"),
                QDir(dl->downloadDirectory()).filePath(dl->downloadFileName()));
            if (target.isEmpty()) {
                dl->cancel();
                return;
            }
            const QFileInfo fi(target);
            dl->setDownloadDirectory(fi.absolutePath());
            dl->setDownloadFileName(fi.fileName());
            dl->accept();
        });

        auto *openAction = new QAction(this);
        openAction->setShortcut(QKeySequence::Open);          // Ctrl+O
        connect(openAction, &QAction::triggered, this, [this] {
            const QString path = QFileDialog::getOpenFileName(
                this, tr("Open PDF"), QString(), tr("PDF files (*.pdf);;All files (*)"));
            if (!path.isEmpty())
                open(path);
        });
        addAction(openAction);

        auto *quitAction = new QAction(this);
        quitAction->setShortcut(QKeySequence::Quit);          // Ctrl+Q
        connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);
        addAction(quitAction);

        showEmpty();
    }

    void showEmpty() { m_view->load(QUrl(QStringLiteral("pdfjs://app/web/viewer.html?file="))); }

    void open(const QString &path)
    {
        const QFileInfo fi(path);
        if (!fi.isFile()) {
            QMessageBox::warning(this, tr("Open PDF"), tr("Cannot read \"%1\".").arg(path));
            return;
        }
        m_docName = fi.fileName();

        // The unique id serves this window's file and defeats caching; the
        // trailing file name is what PDF.js shows as the title fallback.
        const int id = m_handler->addDocument(fi.absoluteFilePath());
        const QString docPath = QStringLiteral("/doc/%1/%2").arg(id).arg(fi.fileName());
        const QString query = QString::fromLatin1(QUrl::toPercentEncoding(docPath));
        m_view->load(QUrl(QStringLiteral("pdfjs://app/web/viewer.html?file=") + query));
    }

private:
    // Runs inside the page's confirm() call (see kPrintGateScript).
    bool askPrinter(int pageCount)
    {
        auto printer = std::make_unique<QPrinter>(QPrinter::HighResolution);
        printer->setDocName(m_docName);                      // job title in the CUPS queue

        QPrintDialog dialog(printer.get(), this);            // lists the CUPS printers
        dialog.setWindowTitle(tr("Print"));
        if (pageCount > 0)
            dialog.setMinMax(1, pageCount);                  // enables the page-range choice
        if (dialog.exec() != QDialog::Accepted)
            return false;

        m_printer = std::move(printer);
        return true;
    }

    PdfJsSchemeHandler *m_handler;
    QWebEngineView *m_view = nullptr;
    std::unique_ptr<QPrinter> m_printer;
    QString m_docName;
};

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Single-instance support
//
// Message from a new launch to the running instance: each file path followed
// by a NUL byte, then a final '\n'. (An empty list just means "new window".)
// ---------------------------------------------------------------------------
static QString instanceSocketName()
{
    // XDG_RUNTIME_DIR is private to the user; the uid keeps the /tmp fallback unique.
    QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (dir.isEmpty())
        dir = QDir::tempPath();
    return QStringLiteral("%1/pdfviewer-%2.sock").arg(dir).arg(getuid());
}

// Returns true if a running instance accepted the request.
static bool sendToRunningInstance(const QString &socketName, const QStringList &files)
{
    QLocalSocket socket;
    socket.connectToServer(socketName);
    if (!socket.waitForConnected(500))
        return false;                                     // nobody there (or stale socket)

    QByteArray message;
    for (const QString &file : files)
        message += file.toUtf8() + '\0';
    message += '\n';

    socket.write(message);
    const bool sent = socket.waitForBytesWritten(2000);
    socket.disconnectFromServer();
    if (socket.state() != QLocalSocket::UnconnectedState)
        socket.waitForDisconnected(1000);
    return sent;
}

// "document-viewer" if the icon theme has it, otherwise the icon of a common
// document-viewer application, otherwise the generic PDF icon.
static QIcon appIcon()
{
    static const char *const names[] = {
        "document-viewer", "org.gnome.Papers", "org.gnome.Evince",
        "evince", "okular", "application-pdf",
    };
    for (const char *name : names)
        if (QIcon::hasThemeIcon(QLatin1String(name)))
            return QIcon::fromTheme(QLatin1String(name));
    return {};
}

static QString findPdfJsRoot(const QString &appDir)
{
    const QStringList candidates = {
        qEnvironmentVariable("PDFJS_DIR"),
        appDir + "/pdfjs",
        appDir + "/../share/pdfviewer/pdfjs",
    };
    for (const QString &dir : candidates)
        if (!dir.isEmpty() && QFileInfo::exists(dir + "/web/viewer.html"))
            return QDir::cleanPath(dir);
    return {};
}

int main(int argc, char *argv[])
{
    // Custom schemes must be registered before QApplication is constructed.
    QWebEngineUrlScheme scheme("pdfjs");
    scheme.setSyntax(QWebEngineUrlScheme::Syntax::Host);
    QWebEngineUrlScheme::Flags flags = QWebEngineUrlScheme::SecureScheme
                                     | QWebEngineUrlScheme::LocalAccessAllowed
                                     | QWebEngineUrlScheme::CorsEnabled;
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    flags |= QWebEngineUrlScheme::FetchApiAllowed;   // PDF.js uses fetch()
#endif
    scheme.setFlags(flags);
    QWebEngineUrlScheme::registerScheme(scheme);

    QApplication app(argc, argv);
    app.setApplicationName("pdfviewer");
    app.setWindowIcon(appIcon());                       // X11 / window decorations
    QGuiApplication::setDesktopFileName("pdfviewer");   // Wayland: matches pdfviewer.desktop

    QCommandLineParser parser;
    parser.setApplicationDescription("Minimal PDF viewer (Qt WebEngine + PDF.js)");
    parser.addHelpOption();
    parser.addPositionalArgument("file", "PDF file to open", "[file]");
    parser.process(app);

    // Command-line files as absolute paths (a running instance has another cwd).
    QStringList files;
    for (const QString &arg : parser.positionalArguments())
        files << QFileInfo(arg).absoluteFilePath();

    // Single instance: if pdfviewer is already running, hand the files to it
    // (it opens a new window for each) and exit. This also guarantees that only
    // one process ever uses the persistent WebEngine profile on disk.
    const QString socketName = instanceSocketName();
    if (sendToRunningInstance(socketName, files))
        return 0;

    const QString root = findPdfJsRoot(app.applicationDirPath());
    if (root.isEmpty()) {
        QMessageBox::critical(nullptr, "PDF Viewer",
            "PDF.js not found.\n\nRun scripts/fetch-pdfjs.sh, rebuild, "
            "or point PDFJS_DIR at an unpacked PDF.js release.");
        return 1;
    }

    // Become the instance others connect to. removeServer() clears a stale
    // socket left behind by a crashed run.
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    QLocalServer::removeServer(socketName);
    const bool primary = server.listen(socketName);

    // Declaration order = reverse destruction order: profile before handler.
    // Only the primary instance touches the on-disk profile (keeps zoom/last
    // page); should we fail to become primary, stay off the record instead of
    // fighting another process over the same storage.
    PdfJsSchemeHandler handler(root);
    auto profile = primary ? std::make_unique<QWebEngineProfile>("pdfviewer")
                           : std::make_unique<QWebEngineProfile>();
    profile->installUrlSchemeHandler("pdfjs", &handler);

    const auto openWindow = [&](const QString &file) {
        auto *window = new Viewer(profile.get(), &handler);   // deletes itself on close
        window->show();
        if (!file.isEmpty())
            window->open(file);
        window->raise();
        window->activateWindow();
    };
    const auto openFiles = [&](const QStringList &list) {
        if (list.isEmpty())
            openWindow({});
        for (const QString &file : list)
            openWindow(file);
    };

    QObject::connect(&server, &QLocalServer::newConnection, &server, [&] {
        while (QLocalSocket *socket = server.nextPendingConnection()) {
            auto buffer = std::make_shared<QByteArray>();
            const auto onData = [socket, buffer, openFiles] {
                buffer->append(socket->readAll());
                if (!buffer->endsWith('\n'))              // message not complete yet
                    return;
                QStringList list;
                for (const QByteArray &path : buffer->chopped(1).split('\0'))
                    if (!path.isEmpty())
                        list << QString::fromUtf8(path);
                buffer->clear();
                socket->deleteLater();
                openFiles(list);
            };
            QObject::connect(socket, &QLocalSocket::readyRead, socket, onData);
            if (socket->bytesAvailable())                 // data may already have arrived
                onData();
        }
    });

    openFiles(files);
    const int result = app.exec();

    // Windows delete themselves when closed; make sure that has happened
    // before the profile they use goes away.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    return result;
}
