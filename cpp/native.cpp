#include "loaf/native.h"
#include "loaf/portal.h"
#include "loaf/thumbnails.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <expected>
#include <fcntl.h>
#include <memory>
#include <unistd.h>
#include <QClipboard>
#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocale>
#include <QMimeData>
#include <QMimeDatabase>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QTimer>
#include <QTranslator>
#include <QUrl>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
namespace loaf {
namespace {

constexpr auto kService = "io.github.m_uvex.Loaf"_L1;
constexpr auto kPath = "/io/github/m_uvex/Loaf"_L1;

// Single owner of the language state: requested locale name, the installed
// translator (parented to the QCoreApplication instance), and the QML engine.
struct LanguageState {
    QString requestedLocale;
    QTranslator *translator = nullptr;
    QPointer<QQmlApplicationEngine> engine;
};
LanguageState languageState;

[[nodiscard]] QString toQString(rust::Str s)
{
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

// The locale to use: the override when valid, else the system locale.
[[nodiscard]] QLocale resolvedLocale()
{
    if (!languageState.requestedLocale.isEmpty()) {
        const QLocale locale(languageState.requestedLocale);
        if (locale != QLocale::c())
            return locale;
    }
    return QLocale::system();
}

// Stores the override locale and makes the resolved locale the default.
QLocale storeRequestedLocale(rust::Str localeName)
{
    languageState.requestedLocale = toQString(localeName);
    const QLocale locale = resolvedLocale();
    QLocale::setDefault(locale);
    return locale;
}

// Installs the translator for `locale` in place of the previous one.
// When no catalogue matches the previous translator is removed and none is
// installed, so source (English) strings render.
bool installTranslatorFor(const QLocale &locale)
{
    if (languageState.translator) {
        QCoreApplication::removeTranslator(languageState.translator);
        languageState.translator->deleteLater();
        languageState.translator = nullptr;
    }
    auto *candidate = new QTranslator(QCoreApplication::instance());
    if (!candidate->load(locale, u"loaf"_s, u"_"_s, u":/i18n"_s)) {
        delete candidate;
        return false;
    }
    QCoreApplication::installTranslator(candidate);
    languageState.translator = candidate;
    return true;
}

[[nodiscard]] rust::String toRust(const QString &s)
{
    const QByteArray utf8 = s.toUtf8();
    return rust::String(utf8.constData(), static_cast<size_t>(utf8.size()));
}

[[nodiscard]] rust::String jsonToRust(const QJsonObject &object)
{
    return toRust(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

[[nodiscard]] rust::String jsonToRust(const QJsonArray &array)
{
    return toRust(QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)));
}

// Accepts local paths and Loaf MTP locations. MTP validation happens in Rust.
[[nodiscard]] QStringList toLocalPaths(const QStringList &items)
{
    QStringList out;
    for (const QString &item : items) {
        if (item.isEmpty())
            continue;
        if (item.startsWith("mtp://"_L1)) {
            out << item;
            continue;
        }
        const QUrl url = item.startsWith('/'_L1) ? QUrl::fromLocalFile(item) : QUrl::fromUserInput(item);
        if (url.isLocalFile())
            out << QFileInfo(url.toLocalFile()).absoluteFilePath();
    }
    return out;
}
} // namespace

LoafNative::LoafNative(QObject *parent)
    : QObject(parent)
{
    QDBusConnection::sessionBus().connect(u"org.kde.kmtpd5"_s,
        u"/modules/kmtpd"_s, u"org.kde.kmtp.Daemon"_s,
        u"devicesChanged"_s, this, SLOT(notifyMtpDevicesChanged()));
    if (QClipboard *cb = QGuiApplication::clipboard()) {
        // Parse once per change: reading another app's clipboard can block,
        // so menus must never do it synchronously.
        connect(cb, &QClipboard::dataChanged, this, [this] {
            m_clipboard = readClipboard();
            Q_EMIT clipboardChanged();
        });
        m_clipboard = readClipboard();
    }
}

void LoafNative::setClipboardFiles(const QStringList &paths, bool cut)
{
    QList<QUrl> urls;
    QByteArray gnome = cut ? "cut" : "copy";
    QStringList plain;
    bool hasMtp = false;
    for (const QString &p : paths) {
        const bool mtp = p.startsWith("mtp://"_L1);
        hasMtp |= mtp;
        const QUrl u = mtp ? QUrl(p) : QUrl::fromLocalFile(p);
        urls << u;
        gnome += '\n' + u.toEncoded();
        plain << p;
    }
    auto *data = new QMimeData;
    // Loaf MTP addresses are private provider identities, not KIO URLs.
    // Keep them in our MIME format so copying between Loaf windows works.
    if (!hasMtp) {
        data->setUrls(urls);
        data->setData(u"x-special/gnome-copied-files"_s, gnome);
        data->setData(u"application/x-kde-cutselection"_s, cut ? "1" : "0");
    }
    data->setData(u"application/x-loaf-files"_s, gnome);
    data->setText(plain.join('\n'_L1));
    QGuiApplication::clipboard()->setMimeData(data);
}

QVariantMap LoafNative::clipboardFiles() const
{
    return m_clipboard;
}

QVariantMap LoafNative::readClipboard() const
{
    QVariantMap result{{u"cut"_s, false}, {u"paths"_s, QStringList()}};
    const QMimeData *data = QGuiApplication::clipboard()->mimeData();
    if (!data)
        return result;
    bool cut = false;
    QStringList paths;
    const bool ours = data->hasFormat(u"application/x-loaf-files"_s);
    if (ours || data->hasFormat(u"x-special/gnome-copied-files"_s)) {
        const QList<QByteArray> lines = data->data(ours ? u"application/x-loaf-files"_s : u"x-special/gnome-copied-files"_s).split('\n');
        for (int i = 0; i < lines.size(); ++i) {
            const QByteArray line = lines[i].trimmed();
            if (i == 0 && (line == "cut" || line == "copy")) {
                cut = line == "cut";
                continue;
            }
            if (line.isEmpty())
                continue;
            const QUrl u = QUrl::fromEncoded(line);
            if (u.isLocalFile())
                paths << u.toLocalFile();
            else if (ours && u.scheme() == "mtp"_L1 && !u.host().isEmpty())
                paths << u.toString(QUrl::FullyEncoded);
        }
    }
    if (paths.isEmpty() && data->hasUrls()) {
        for (const QUrl &u : data->urls()) {
            if (u.isLocalFile())
                paths << u.toLocalFile();
        }
        cut = data->data(u"application/x-kde-cutselection"_s) == "1";
    }
    result[u"cut"_s] = cut;
    result[u"paths"_s] = paths;
    return result;
}

bool LoafNative::clipboardHasFiles() const
{
    return !clipboardFiles().value(u"paths"_s).toStringList().isEmpty();
}

void LoafNative::setClipboardText(const QString &text)
{
    QGuiApplication::clipboard()->setText(text);
}

void LoafNative::clearClipboardIfOurs()
{
    const QMimeData *data = QGuiApplication::clipboard()->mimeData();
    if (data && data->hasFormat(u"application/x-loaf-files"_s))
        QGuiApplication::clipboard()->clear();
}

QString LoafNative::mimeType(const QString &path, bool sniffContents) const
{
    static QMimeDatabase db;
    return db.mimeTypeForFile(path, sniffContents ? QMimeDatabase::MatchDefault : QMimeDatabase::MatchExtension).name();
}

QString LoafNative::mimeComment(const QString &path) const
{
    static QMimeDatabase db;
    return db.mimeTypeForFile(path).comment();
}

QStringList LoafNative::takePendingOpen()
{
    QStringList out;
    out.swap(m_pending);
    return out;
}

void LoafNative::queueOpen(const QStringList &paths, const QString &mode)
{
    m_pending = paths;
    if (!mode.isEmpty())
        m_mode = mode;
    Q_EMIT openRequested(paths, mode);
}

bool LoafNative::isAdmin() const
{
    return !qEnvironmentVariable("LOAF_ADMIN").isEmpty();
}

void LoafNative::respondPicker(const QString &handle, uint code, const QString &resultsJson)
{
    if (m_portal)
        m_portal->respond(handle, code, resultsJson);
}

void LoafNative::cancelPicker(const QString &handle)
{
    respondPicker(handle, 1, u"{}"_s);
}

bool LoafNative::fileExists(const QString &path) const
{
    return QFileInfo::exists(path);
}
bool LoafNative::isDirectory(const QString &path) const
{
    return QFileInfo(path).isDir();
}

void LoafNative::setPickerTransient(QWindow *window, const QString &parentWindow, bool modal) const
{
    if (!window)
        return;
    // Wayland needs xdg-foreign to make one window transient for another,
    // which Qt Quick does not expose; X11 window ids work.
    if (QGuiApplication::platformName() != "xcb"_L1 || !parentWindow.startsWith("x11:"_L1))
        return;
    bool ok = false;
    const qulonglong id = QStringView(parentWindow).sliced(4).toULongLong(&ok, 0);
    if (!ok || id == 0)
        return;
    QWindow *parent = QWindow::fromWinId(static_cast<WId>(id));
    if (!parent)
        return;
    window->setTransientParent(parent);
    if (modal)
        window->setModality(Qt::WindowModal);
    // fromWinId only wraps the foreign window; drop the wrapper with the picker.
    connect(window, &QObject::destroyed, parent, &QObject::deleteLater);
}

FileManager1Adaptor::FileManager1Adaptor(QObject *host, LoafNative *native)
    : QDBusAbstractAdaptor(host), m_native(native) {}

void FileManager1Adaptor::ShowFolders(const QStringList &uris, const QString &)
{
    Q_EMIT m_native->openRequested(toLocalPaths(uris), u"tab"_s);
}

void FileManager1Adaptor::ShowItems(const QStringList &uris, const QString &)
{
    Q_EMIT m_native->openRequested(toLocalPaths(uris), u"select"_s);
}

void FileManager1Adaptor::ShowItemProperties(const QStringList &uris, const QString &)
{
    Q_EMIT m_native->openRequested(toLocalPaths(uris), u"properties"_s);
}

LoafAppAdaptor::LoafAppAdaptor(QObject *host, LoafNative *native)
    : QDBusAbstractAdaptor(host), m_native(native) {}

void LoafAppAdaptor::Open(const QStringList &paths, const QString &mode)
{
    Q_EMIT m_native->openRequested(toLocalPaths(paths), mode.isEmpty() ? u"tab"_s : mode);
}

bool forward_to_running_instance(const rust::Vec<rust::String> &paths, rust::Str mode)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected() || !bus.interface())
        return false;
    if (!bus.interface()->isServiceRegistered(QString(kService)))
        return false;
    QStringList raw;
    for (const rust::String &p : paths)
        raw << QString::fromUtf8(p.data(), static_cast<qsizetype>(p.size()));
    QDBusMessage msg = QDBusMessage::createMethodCall(QString(kService), QString(kPath),
                                                      u"io.github.m_uvex.Loaf"_s, u"Open"_s);
    msg << toLocalPaths(raw) << toQString(mode);
    // A hung instance must not stall this launch for long.
    QDBusMessage reply = bus.call(msg, QDBus::Block, 3000);
    return reply.type() == QDBusMessage::ReplyMessage;
}

void setup_engine(QQmlApplicationEngine &engine, const rust::Vec<rust::String> &paths, rust::Str mode)
{
    engine.addImportPath(u"qrc:/qt/qml"_s);
    constexpr std::array<QLatin1StringView, 3> kSystemQmlPaths{
        "/usr/lib/x86_64-linux-gnu/qt6/qml"_L1,
        "/usr/lib/qt6/qml"_L1,
        "/usr/lib64/qt6/qml"_L1,
    };
    for (const QLatin1StringView p : kSystemQmlPaths) {
        const QString path(p);
        if (QDir(path).exists()) {
            engine.addImportPath(path);
            break;
        }
    }

    auto *native = new LoafNative(&engine);
    QStringList raw;
    for (const rust::String &p : paths)
        raw << QString::fromUtf8(p.data(), static_cast<qsizetype>(p.size()));
    // mode: "tab" or "mini", optionally with ",new-window" or ",portal".
    const QString modeStr = toQString(mode);
    const bool newWindow = modeStr.contains("new-window"_L1);
    const bool portalMode = modeStr.contains("portal"_L1);
    native->setPortalMode(portalMode);
    native->queueOpen(toLocalPaths(raw), modeStr.section(','_L1, 0, 0));

    QDBusConnection bus = QDBusConnection::sessionBus();
    // A separate window or an admin (root) instance must not take over the
    // user's services.
    if (bus.isConnected() && !native->isAdmin() && !newWindow) {
        // One object per path, each exporting just its own interface.
        auto *appObj = new QObject(native);
        new LoafAppAdaptor(appObj, native);
        auto *fmObj = new QObject(native);
        new FileManager1Adaptor(fmObj, native);
        if (bus.registerService(QString(kService)))
            bus.registerObject(QString(kPath), appObj, QDBusConnection::ExportAdaptors);
        else
            qWarning("loaf: another instance already owns %ls", qUtf16Printable(QString(kService)));
        // FileManager1 stays with whichever file manager claimed it first.
        if (bus.registerService(u"org.freedesktop.FileManager1"_s))
            bus.registerObject(u"/org/freedesktop/FileManager1"_s, fmObj, QDBusConnection::ExportAdaptors);

        // The portal backend serves file chooser requests on demand; in
        // portal mode (D-Bus activation) it is the reason we are running,
        // so losing the race to another backend means there is nothing to
        // do and the process can exit quietly.
        if (!native->initPortal(portalMode)) {
            if (portalMode)
                QTimer::singleShot(0, QCoreApplication::instance(), &QCoreApplication::quit);
            else
                qWarning("loaf: another backend already owns org.freedesktop.impl.portal.desktop.loaf");
        }
    }
    if (portalMode)
        // Only picker windows are shown; the process exits when the last
        // one closes and no file manager window was opened meanwhile.
        QGuiApplication::setQuitOnLastWindowClosed(false);

    languageState.engine = &engine;
    // Fallback when no catalogue matches: no translator, source (English)
    // strings render unchanged.
    installTranslatorFor(resolvedLocale());

    engine.rootContext()->setContextProperty(u"nativeBridge"_s, native);
    engine.addImageProvider(u"thumb"_s, new ThumbnailProvider);
}

void set_application_language(rust::Str localeName)
{
    storeRequestedLocale(localeName);
}

void apply_language(rust::Str localeName)
{
    const QLocale locale = storeRequestedLocale(localeName);
    installTranslatorFor(locale);
    if (languageState.engine)
        languageState.engine->retranslate();
}

rust::String languages_json()
{
    QFile file(u":/loaf/settings/languages.json"_s);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return toRust(u"[]"_s);
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isArray())
        return toRust(u"[]"_s);
    QJsonArray available;
    for (const QJsonValue &entry : document.array()) {
        const QString code = entry.toObject().value(u"value"_s).toString();
        if (code.isEmpty())
            continue;
        if (!QFile::exists(u":/i18n/loaf_%1.qm"_s.arg(code)))
            continue;
        available.append(entry);
    }
    return jsonToRust(available);
}

rust::String mime_type_for(rust::Str path, bool sniff)
{
    static QMimeDatabase db;
    return toRust(db.mimeTypeForFile(toQString(path), sniff ? QMimeDatabase::MatchDefault : QMimeDatabase::MatchExtension).name());
}

MimeFilterMatcher::MimeFilterMatcher(const rust::Vec<rust::String> &filterMimes)
{
    filterTypes.reserve(static_cast<qsizetype>(filterMimes.size()));
    for (const rust::String &filterMime : filterMimes)
        filterTypes.append(database.mimeTypeForName(toQString(filterMime)));
}

bool MimeFilterMatcher::matches(const rust::Str fileName) const
{
    const QMimeType actual = database.mimeTypeForFile(toQString(fileName), QMimeDatabase::MatchExtension);
    if (!actual.isValid())
        return false;
    return std::ranges::any_of(filterTypes, [&actual](const QMimeType &filter) {
        return filter.isValid() && actual.inherits(filter.name());
    });
}

std::unique_ptr<MimeFilterMatcher> new_mime_filter_matcher(const rust::Vec<rust::String> &filterMimes)
{
    return std::make_unique<MimeFilterMatcher>(filterMimes);
}

rust::String mime_comment_for(rust::Str mime)
{
    static QMimeDatabase db;
    return toRust(db.mimeTypeForName(toQString(mime)).comment());
}

// True when the saved window opacity is below 1. Only then does the window
// get an alpha channel: an opaque window lets the compositor skip blending
// it every frame, which matters on integrated GPUs.
namespace {
bool wantsAlphaBuffer()
{
    QString dir = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (dir.isEmpty())
        dir = QDir::homePath() + u"/.config"_s;
    QFile f(dir + u"/loaf/config.json"_s);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QJsonValue v = QJsonDocument::fromJson(f.readAll()).object().value(u"backgroundOpacity"_s);
    bool ok = v.isDouble();
    const double opacity = v.isString() ? v.toString().toDouble(&ok) : v.toDouble(1.0);
    return ok && opacity < 0.999; // 0 (fully transparent) needs alpha too
}

bool g_alphaBuffer = false;
} // namespace

bool LoafNative::alphaBuffer() const
{
    return g_alphaBuffer;
}

void set_application_identity(rust::Str version, rust::Str window_class)
{
    g_alphaBuffer = wantsAlphaBuffer();
    QQuickWindow::setDefaultAlphaBuffer(g_alphaBuffer);
    QCoreApplication::setApplicationVersion(toQString(version));
    // The application name is the X11 WM_CLASS instance name and the desktop
    // file id is the Wayland app id / X11 class name. Both take `--class` so
    // window rules match whichever part the compositor looks at.
    QString windowClass = toQString(window_class);
    if (windowClass.isEmpty())
        windowClass = u"loaf"_s;
    QCoreApplication::setApplicationName(windowClass);
    QCoreApplication::setOrganizationName(u"loaf"_s);
    QGuiApplication::setApplicationDisplayName(u"Loaf"_s);
    QGuiApplication::setDesktopFileName(windowClass);
}

// Explicit messages avoid QDBusInterface's synchronous introspection before its
// timeout is set. These helpers run on Rust workers, never the GUI thread.
namespace {
QDBusMessage mtpCall(const QString &path, const QString &interface,
                     const QString &method, const QVariantList &arguments = {})
{
    auto message = QDBusMessage::createMethodCall(u"org.kde.kmtpd5"_s, path, interface, method);
    message.setArguments(arguments);
    return QDBusConnection::sessionBus().call(message, QDBus::Block, 1500);
}

QVariantMap mtpProperties(const QString &path, const QString &interface)
{
    QDBusReply<QVariantMap> reply = mtpCall(path, u"org.freedesktop.DBus.Properties"_s,
                                         u"GetAll"_s, {interface});
    return reply.isValid() ? reply.value() : QVariantMap{};
}
} // namespace

rust::String list_kmtp_devices_json()
{
    // Plain Rust unit tests do not create a Qt application / D-Bus dispatcher.
    if (!QCoreApplication::instance())
        return toRust(u"[]"_s);
    QJsonArray array;
    QDBusReply<QList<QDBusObjectPath>> reply = mtpCall(u"/modules/kmtpd"_s,
        u"org.kde.kmtp.Daemon"_s, u"listDevices"_s);
    if (!reply.isValid())
        return toRust(u"[]"_s); // KDE MTP support is optional.
    for (const auto &device : reply.value()) {
        const auto properties = mtpProperties(device.path(), u"org.kde.kmtp.Device"_s);
        const QString udi = properties.value(u"udi"_s).toString();
        if (udi.isEmpty())
            continue; // Never silently open a different device via mtp:/.
        const QString name = properties.value(u"friendlyName"_s).toString();
        const QString udiHex = QString::fromLatin1(udi.toUtf8().toHex());
        const QString url = u"mtp://loaf/"_s + udiHex + '/'_L1;
        quint64 total = 0, free = 0;
        QDBusReply<QList<QDBusObjectPath>> storages = mtpCall(device.path(),
            u"org.kde.kmtp.Device"_s, u"listStorages"_s);
        if (storages.isValid()) {
            for (const auto &storage : storages.value()) {
                const auto props = mtpProperties(storage.path(), u"org.kde.kmtp.Storage"_s);
                total += props.value(u"maxCapacity"_s).toULongLong();
                free += props.value(u"freeSpaceInBytes"_s).toULongLong();
            }
        }
        array.append(QJsonObject{{u"name"_s, name}, {u"udi"_s, udi},
            {u"url"_s, url},
            {u"model"_s, name}, {u"total_bytes"_s, static_cast<double>(total)},
            {u"free_bytes"_s, static_cast<double>(free)}});
    }
    return jsonToRust(array);
}

namespace {
class Mtp {
    Q_DECLARE_TR_FUNCTIONS(Mtp)
};

rust::String mtpError(const QString &message)
{
    const QJsonObject result{{u"entries"_s, QJsonArray{}}, {u"error"_s, message}};
    return jsonToRust(result);
}

struct ParsedMtpUrl {
    QString targetUdi;
    QStringList parts;
};

std::expected<ParsedMtpUrl, QString> parseMtpUrl(const QString &urlStr)
{
    if (!urlStr.startsWith(u"mtp://"_s))
        return std::unexpected(Mtp::tr("Invalid MTP URL scheme"));

    QString pathWithoutScheme = urlStr.sliced(6);
    if (pathWithoutScheme.startsWith("loaf/"_L1))
        pathWithoutScheme.remove(0, 5);
    if (pathWithoutScheme.endsWith('/'_L1))
        pathWithoutScheme.chop(1);
    if (pathWithoutScheme.contains('?'_L1) || pathWithoutScheme.contains('#'_L1))
        return std::unexpected(Mtp::tr("URL query and fragment are not supported"));

    const QStringList parts = pathWithoutScheme.split('/'_L1);
    if (parts.isEmpty() || parts[0].isEmpty())
        return std::unexpected(Mtp::tr("Empty MTP device identifier"));

    const QString &udiHex = parts[0];
    const QByteArray udiBytes = QByteArray::fromHex(udiHex.toLatin1());
    if (udiBytes.isEmpty() || udiHex != QString::fromLatin1(udiBytes.toHex()))
        return std::unexpected(Mtp::tr("Invalid UDI hex encoding"));

    return ParsedMtpUrl{QString::fromUtf8(udiBytes), parts};
}

std::expected<QString, QString> decodeMtpSubPath(const QStringList &parts, qsizetype startIndex)
{
    QString subPath = u"/"_s;
    for (qsizetype i = startIndex; i < parts.size(); ++i) {
        const QString decodedSeg = QUrl::fromPercentEncoding(parts[i].toUtf8());
        if (decodedSeg.isEmpty() || decodedSeg == u"."_s || decodedSeg == u".."_s
            || decodedSeg.contains('/'_L1) || decodedSeg.contains('\0'_L1)) {
            return std::unexpected(Mtp::tr("Invalid path component in URL"));
        }
        if (subPath.length() > 1)
            subPath += '/'_L1;
        subPath += decodedSeg;
    }
    return subPath;
}
} // namespace

rust::String list_mtp_directory(rust::Str url)
{
    if (!QCoreApplication::instance()) {
        return mtpError(Mtp::tr("D-Bus application instance not available"));
    }

    const auto parsed = parseMtpUrl(toQString(url));
    if (!parsed)
        return mtpError(parsed.error());
    const QString &targetUdi = parsed->targetUdi;
    const QStringList &parts = parsed->parts;
    // 1. Locate device
    QDBusReply<QList<QDBusObjectPath>> reply = mtpCall(u"/modules/kmtpd"_s,
        u"org.kde.kmtp.Daemon"_s, u"listDevices"_s);
    if (!reply.isValid()) {
        return mtpError(Mtp::tr("KDE MTP daemon: %1").arg(reply.error().message()));
    }

    const auto devices = reply.value();
    const auto deviceIt = std::ranges::find_if(devices, [&targetUdi](const QDBusObjectPath &device) {
        return mtpProperties(device.path(), u"org.kde.kmtp.Device"_s).value(u"udi"_s).toString() == targetUdi;
    });
    const QDBusObjectPath devicePath = deviceIt != devices.end() ? *deviceIt : QDBusObjectPath{};

    if (devicePath.path().isEmpty()) {
        return mtpError(Mtp::tr("Device is disconnected"));
    }

    // 2. Query storages
    QDBusReply<QList<QDBusObjectPath>> storagesReply = mtpCall(devicePath.path(),
        u"org.kde.kmtp.Device"_s, u"listStorages"_s);
    if (!storagesReply.isValid() || storagesReply.value().isEmpty()) {
        return mtpError(Mtp::tr("No storages found. Unlock the device and enable file transfer."));
    }

    // Root listing: return storage descriptions as directories
    if (parts.size() == 1) {
        QJsonArray entries;
        for (const auto &storage : storagesReply.value()) {
            const auto props = mtpProperties(storage.path(), u"org.kde.kmtp.Storage"_s);
            const QString desc = props.value(u"description"_s).toString();
            if (desc.isEmpty())
                continue;
            entries.append(QJsonObject{
                {u"name"_s, desc},
                {u"is_dir"_s, true},
                {u"size"_s, 0.0},
                {u"modified"_s, 0.0}
            });
        }
        if (entries.isEmpty())
            return mtpError(Mtp::tr("No accessible storage. Unlock the phone and enable file transfer."));
        QJsonObject res;
        res.insert(u"entries"_s, entries);
        return jsonToRust(res);
    }

    // Decode storage description from percent-encoded URL component
    const QString storageDesc = QUrl::fromPercentEncoding(parts[1].toUtf8());
    QList<QDBusObjectPath> matchingStorages;
    for (const auto &storage : storagesReply.value()) {
        const auto props = mtpProperties(storage.path(), u"org.kde.kmtp.Storage"_s);
        if (props.value(u"description"_s).toString() == storageDesc) {
            matchingStorages.append(storage);
        }
    }

    if (matchingStorages.size() > 1) {
        return mtpError(Mtp::tr("Ambiguous storage: multiple storages share the exact same description"));
    }
    if (matchingStorages.isEmpty()) {
        return mtpError(Mtp::tr("Storage not found on device"));
    }

    const QDBusObjectPath targetStoragePath = matchingStorages.first();

    const auto subPathRes = decodeMtpSubPath(parts, 2);
    if (!subPathRes)
        return mtpError(subPathRes.error());
    const QString &subPath = *subPathRes;

    // Call getFilesAndFolders on storage with 30000ms timeout
    auto callMsg = QDBusMessage::createMethodCall(u"org.kde.kmtpd5"_s,
        targetStoragePath.path(), u"org.kde.kmtp.Storage"_s, u"getFilesAndFolders"_s);
    callMsg.setArguments({subPath});
    QDBusMessage callReply = QDBusConnection::sessionBus().call(callMsg, QDBus::Block, 30000);

    if (callReply.type() != QDBusMessage::ReplyMessage) {
        return mtpError(Mtp::tr("Failed to list MTP storage: %1").arg(callReply.errorMessage()));
    }

    const QVariantList args = callReply.arguments();
    if (args.size() != 2 || callReply.signature() != "a(uuustxs)i"_L1) {
        return mtpError(Mtp::tr("Unexpected response from MTP service"));
    }

    const int resultStatus = args[1].toInt();
    if (resultStatus == 1) {
        return mtpError(Mtp::tr("Path not found on MTP device"));
    } else if (resultStatus == 2) {
        return mtpError(Mtp::tr("Path is a file, not a directory"));
    } else if (resultStatus != 0) {
        return mtpError(Mtp::tr("Error listing path on MTP device"));
    }

    const QDBusArgument dbusArg = args[0].value<QDBusArgument>();
    QJsonArray entries;
    dbusArg.beginArray();
    while (!dbusArg.atEnd()) {
        quint32 itemId = 0;
        quint32 parentId = 0;
        quint32 storageId = 0;
        QString filename;
        quint64 filesize = 0;
        qint64 modificationdate = 0;
        QString filetype;

        dbusArg.beginStructure();
        dbusArg >> itemId >> parentId >> storageId >> filename >> filesize >> modificationdate >> filetype;
        dbusArg.endStructure();

        const bool isDir = (filetype == u"inode/directory"_s);
        entries.append(QJsonObject{
            {u"name"_s, filename},
            {u"is_dir"_s, isDir},
            {u"size"_s, static_cast<double>(filesize)},
            {u"modified"_s, static_cast<double>(modificationdate)}
        });
    }
    dbusArg.endArray();

    QJsonObject res;
    res.insert(u"entries"_s, entries);
    return jsonToRust(res);
}

namespace {
QMutex s_mtpTransferMutex;
class MtpSignalConnection {
public:
    MtpSignalConnection(const QString &storagePath, MtpTransferReceiver &receiver)
        : m_storagePath(storagePath)
        , m_receiver(&receiver)
    {
        auto bus = QDBusConnection::sessionBus();
        m_finishedConnected = bus.connect(u"org.kde.kmtpd5"_s, m_storagePath, u"org.kde.kmtp.Storage"_s,
            u"copyFinished"_s, m_receiver, SLOT(onCopyFinished(int)));
        m_progressConnected = bus.connect(u"org.kde.kmtpd5"_s, m_storagePath, u"org.kde.kmtp.Storage"_s,
            u"copyProgress"_s, m_receiver, SLOT(onCopyProgress(quint64,quint64)));
    }

    ~MtpSignalConnection()
    {
        auto bus = QDBusConnection::sessionBus();
        if (m_finishedConnected) {
            bus.disconnect(u"org.kde.kmtpd5"_s, m_storagePath, u"org.kde.kmtp.Storage"_s,
                u"copyFinished"_s, m_receiver, SLOT(onCopyFinished(int)));
        }
        if (m_progressConnected) {
            bus.disconnect(u"org.kde.kmtpd5"_s, m_storagePath, u"org.kde.kmtp.Storage"_s,
                u"copyProgress"_s, m_receiver, SLOT(onCopyProgress(quint64,quint64)));
        }
    }

    MtpSignalConnection(const MtpSignalConnection &) = delete;
    MtpSignalConnection &operator=(const MtpSignalConnection &) = delete;

    [[nodiscard]] bool finishedConnected() const { return m_finishedConnected; }

private:
    QString m_storagePath;
    MtpTransferReceiver *m_receiver;
    bool m_finishedConnected = false;
    bool m_progressConnected = false;
};
class ScopedFd {
public:
    explicit ScopedFd(int fd, QByteArray unlinkOnFailPath = {}) noexcept
        : m_fd(fd)
        , m_unlinkPath(std::move(unlinkOnFailPath))
        , m_unlinkOnClose(fd >= 0 && !m_unlinkPath.isEmpty())
    {}

    ~ScopedFd()
    {
        close();
        if (m_unlinkOnClose)
            ::unlink(m_unlinkPath.constData());
    }

    ScopedFd(const ScopedFd &) = delete;
    ScopedFd &operator=(const ScopedFd &) = delete;

    [[nodiscard]] int get() const noexcept { return m_fd; }
    [[nodiscard]] bool valid() const noexcept { return m_fd >= 0; }

    void close() noexcept
    {
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
    }

    void keep() noexcept { m_unlinkOnClose = false; }

private:
    int m_fd = -1;
    QByteArray m_unlinkPath;
    bool m_unlinkOnClose = false;
};

void waitForTransfer(MtpTransferReceiver &receiver)
{
    QEventLoop loop;
    QObject::connect(&receiver, &MtpTransferReceiver::finished, &loop, &QEventLoop::quit);

    QTimer idleTimer;
    idleTimer.setSingleShot(true);
    QObject::connect(&idleTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&receiver, &MtpTransferReceiver::progressReceived, &idleTimer, [&idleTimer]() {
        idleTimer.start(30s);
    });

    idleTimer.start(30s);
    if (!receiver.m_finished)
        loop.exec();
}
} // namespace
rust::String mtp_operation_json(rust::Str action, rust::Str url, rust::Str argument)
{
    if (!QCoreApplication::instance()) {
        const QJsonObject err{{u"error"_s, Mtp::tr("D-Bus application instance not available")}};
        return jsonToRust(err);
    }

    const QString actionStr = toQString(action);
    const QString urlStr = toQString(url);
    const QString argStr = toQString(argument);
    QMutexLocker<QMutex> mutationLock(actionStr == u"stat"_s ? nullptr : &s_mtpTransferMutex);

    const auto parsed = parseMtpUrl(urlStr);
    if (!parsed) {
        const QJsonObject err{{u"error"_s, parsed.error()}};
        return jsonToRust(err);
    }
    const QString &targetUdi = parsed->targetUdi;
    const QStringList &parts = parsed->parts;

    // Locate device
    QDBusReply<QList<QDBusObjectPath>> reply = mtpCall(u"/modules/kmtpd"_s,
        u"org.kde.kmtp.Daemon"_s, u"listDevices"_s);
    if (!reply.isValid()) {
        const QJsonObject err{{u"error"_s, Mtp::tr("KDE MTP daemon is not available")}};
        return jsonToRust(err);
    }

    const auto devices = reply.value();
    const auto deviceIt = std::ranges::find_if(devices, [&targetUdi](const QDBusObjectPath &device) {
        return mtpProperties(device.path(), u"org.kde.kmtp.Device"_s).value(u"udi"_s).toString() == targetUdi;
    });
    const QDBusObjectPath devicePath = deviceIt != devices.end() ? *deviceIt : QDBusObjectPath{};

    if (devicePath.path().isEmpty()) {
        const QJsonObject err{{u"error"_s, Mtp::tr("Device is disconnected")}};
        return jsonToRust(err);
    }

    // Locate storages
    QDBusReply<QList<QDBusObjectPath>> storagesReply = mtpCall(devicePath.path(),
        u"org.kde.kmtp.Device"_s, u"listStorages"_s);
    if (!storagesReply.isValid() || storagesReply.value().isEmpty()) {
        const QJsonObject err{{u"error"_s, Mtp::tr("No storages found. Unlock the device and enable file transfer.")}};
        return jsonToRust(err);
    }

    // Reject mutations on device root
    if (parts.size() <= 1 && actionStr != u"stat"_s) {
        const QJsonObject err{{u"error"_s, Mtp::tr("Mutations on device root are not supported")}};
        return jsonToRust(err);
    }

    if (parts.size() == 1 && actionStr == u"stat"_s) {
        // Synthesize metadata for device root
        const auto devProps = mtpProperties(devicePath.path(), u"org.kde.kmtp.Device"_s);
        const QString friendlyName = devProps.value(u"friendlyName"_s).toString();
        const QJsonObject res{
            {u"exists"_s, true},
            {u"name"_s, friendlyName.isEmpty() ? u"Phone"_s : friendlyName},
            {u"is_dir"_s, true},
            {u"size"_s, 0.0},
            {u"modified"_s, 0.0}
        };
        return jsonToRust(res);
    }

    // Match storage description
    const QString storageDesc = QUrl::fromPercentEncoding(parts[1].toUtf8());
    QList<QDBusObjectPath> matchingStorages;
    for (const auto &storage : storagesReply.value()) {
        const auto props = mtpProperties(storage.path(), u"org.kde.kmtp.Storage"_s);
        if (props.value(u"description"_s).toString() == storageDesc) {
            matchingStorages.append(storage);
        }
    }

    if (matchingStorages.size() > 1) {
        const QJsonObject err{{u"error"_s, Mtp::tr("Ambiguous storage: multiple storages share the exact same description")}};
        return jsonToRust(err);
    }
    if (matchingStorages.isEmpty()) {
        if (actionStr == u"stat"_s) {
            const QJsonObject res{{u"exists"_s, false}};
            return jsonToRust(res);
        }
        const QJsonObject err{{u"error"_s, Mtp::tr("Storage not found on device")}};
        return jsonToRust(err);
    }

    const QDBusObjectPath targetStoragePath = matchingStorages.first();

    // Storage root checks
    if (parts.size() == 2) {
        if (actionStr == u"stat"_s) {
            const QJsonObject res{
                {u"exists"_s, true},
                {u"name"_s, storageDesc},
                {u"is_dir"_s, true},
                {u"size"_s, 0.0},
                {u"modified"_s, 0.0}
            };
            return jsonToRust(res);
        } else if (actionStr != u"mkdir"_s && actionStr != u"upload"_s) {
            const QJsonObject err{{u"error"_s, Mtp::tr("Cannot rename or delete storage root")}};
            return jsonToRust(err);
        }
    }

    const auto subPathRes = decodeMtpSubPath(parts, 2);
    if (!subPathRes) {
        const QJsonObject err{{u"error"_s, subPathRes.error()}};
        return jsonToRust(err);
    }
    const QString &subPath = *subPathRes;

    if (actionStr == u"stat"_s) {
        auto msg = QDBusMessage::createMethodCall(u"org.kde.kmtpd5"_s,
            targetStoragePath.path(), u"org.kde.kmtp.Storage"_s, u"getFileMetadata"_s);
        msg.setArguments({subPath});
        QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 30000);
        if (reply.type() != QDBusMessage::ReplyMessage) {
            const QJsonObject err{{u"error"_s, u"D-Bus call getFileMetadata failed: %1"_s.arg(reply.errorMessage())}};
            return jsonToRust(err);
        }
        const QVariantList args = reply.arguments();
        if (args.isEmpty() || reply.signature() != "(uuustxs)"_L1) {
            const QJsonObject err{{u"error"_s, u"Unexpected getFileMetadata response signature"_s}};
            return jsonToRust(err);
        }
        const QDBusArgument arg = args[0].value<QDBusArgument>();
        quint32 itemId = 0, parentId = 0, storageId = 0;
        QString filename;
        quint64 filesize = 0;
        qint64 modificationdate = 0;
        QString filetype;
        arg.beginStructure();
        arg >> itemId >> parentId >> storageId >> filename >> filesize >> modificationdate >> filetype;
        arg.endStructure();

        if (itemId == 0) {
            const QJsonObject res{{u"exists"_s, false}};
            return jsonToRust(res);
        }
        const bool isDir = (filetype == u"inode/directory"_s);
        const QJsonObject res{
            {u"exists"_s, true},
            {u"name"_s, filename},
            {u"is_dir"_s, isDir},
            {u"size"_s, static_cast<double>(filesize)},
            {u"modified"_s, static_cast<double>(modificationdate)}
        };
        return jsonToRust(res);
    } else if (actionStr == u"mkdir"_s) {
        if (argStr.isEmpty() || argStr == u"."_s || argStr == u".."_s
            || argStr.contains('/'_L1) || argStr.contains('\0'_L1)) {
            const QJsonObject err{{u"error"_s, Mtp::tr("Invalid directory name")}};
            return jsonToRust(err);
        }
        QString targetPath = subPath;
        if (targetPath.length() > 1) {
            targetPath += '/'_L1;
        }
        targetPath += argStr;

        auto msg = QDBusMessage::createMethodCall(u"org.kde.kmtpd5"_s,
            targetStoragePath.path(), u"org.kde.kmtp.Storage"_s, u"createFolder"_s);
        msg.setArguments({targetPath});
        QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 30000);
        if (reply.type() != QDBusMessage::ReplyMessage) {
            const QJsonObject err{{u"error"_s, u"D-Bus call createFolder failed: %1"_s.arg(reply.errorMessage())}};
            return jsonToRust(err);
        }
        const qint64 ret = reply.arguments().value(0).toLongLong();
        if (ret > 0) {
            const QJsonObject res{{u"ok"_s, true}};
            return jsonToRust(res);
        } else if (ret == -1) {
            const QJsonObject err{{u"error"_s, Mtp::tr("Directory already exists")}};
            return jsonToRust(err);
        } else if (ret == -2) {
            const QJsonObject err{{u"error"_s, Mtp::tr("File blocks directory creation")}};
            return jsonToRust(err);
        } else {
            const QJsonObject err{{u"error"_s, Mtp::tr("Failed to create folder on MTP device")}};
            return jsonToRust(err);
        }
    } else if (actionStr == u"rename"_s) {
        if (subPath == u"/"_s) {
            const QJsonObject err{{u"error"_s, Mtp::tr("Cannot rename storage root")}};
            return jsonToRust(err);
        }
        if (argStr.isEmpty() || argStr == u"."_s || argStr == u".."_s
            || argStr.contains('/'_L1) || argStr.contains('\0'_L1)) {
            const QJsonObject err{{u"error"_s, Mtp::tr("Invalid new name for rename")}};
            return jsonToRust(err);
        }
        const QString renamedUrl = urlStr.first(urlStr.lastIndexOf('/'_L1) + 1)
            + QString::fromLatin1(QUrl::toPercentEncoding(argStr));
        const rust::String targetUrl = toRust(renamedUrl);
        const rust::String targetReply = mtp_operation_json(rust::Str("stat"), targetUrl, rust::Str(""));
        const QJsonObject targetState = QJsonDocument::fromJson(QByteArray(targetReply.data(), static_cast<qsizetype>(targetReply.size()))).object();
        if (targetState.contains(u"error"_s) || targetState.value(u"exists"_s).toBool())
            return mtpError(Mtp::tr("Rename target already exists or could not be checked"));
        auto msg = QDBusMessage::createMethodCall(u"org.kde.kmtpd5"_s,
            targetStoragePath.path(), u"org.kde.kmtp.Storage"_s, u"setFileName"_s);
        msg.setArguments({subPath, argStr});
        QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 30000);
        if (reply.type() != QDBusMessage::ReplyMessage) {
            const QJsonObject err{{u"error"_s, u"D-Bus call setFileName failed: %1"_s.arg(reply.errorMessage())}};
            return jsonToRust(err);
        }
        const int ret = reply.arguments().value(0).toInt();
        if (ret == 0) {
            const QJsonObject res{{u"ok"_s, true}};
            return jsonToRust(res);
        } else {
            const QJsonObject err{{u"error"_s, Mtp::tr("Failed to rename object on MTP device (code %1)").arg(ret)}};
            return jsonToRust(err);
        }
    } else if (actionStr == u"delete"_s) {
        if (subPath == u"/"_s) {
            const QJsonObject err{{u"error"_s, Mtp::tr("Cannot delete storage root")}};
            return jsonToRust(err);
        }
        auto msg = QDBusMessage::createMethodCall(u"org.kde.kmtpd5"_s,
            targetStoragePath.path(), u"org.kde.kmtp.Storage"_s, u"deleteObject"_s);
        msg.setArguments({subPath});
        QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 30000);
        if (reply.type() != QDBusMessage::ReplyMessage) {
            const QJsonObject err{{u"error"_s, u"D-Bus call deleteObject failed: %1"_s.arg(reply.errorMessage())}};
            return jsonToRust(err);
        }
        const int ret = reply.arguments().value(0).toInt();
        if (ret == 0) {
            const QJsonObject res{{u"ok"_s, true}};
            return jsonToRust(res);
        } else {
            const QJsonObject err{{u"error"_s, Mtp::tr("Failed to delete object on MTP device (code %1)").arg(ret)}};
            return jsonToRust(err);
        }
    } else if (actionStr == u"download"_s) {
        if (subPath == u"/"_s) {
            const QJsonObject err{{u"error"_s, Mtp::tr("Cannot download storage root")}};
            return jsonToRust(err);
        }
        if (argStr.isEmpty() || !QDir::isAbsolutePath(argStr)) {
            const QJsonObject err{{u"error"_s, Mtp::tr("Local destination must be an absolute path")}};
            return jsonToRust(err);
        }

        const QByteArray localPathBytes = argStr.toUtf8();
        ScopedFd fd(::open(localPathBytes.constData(), O_CREAT | O_EXCL | O_WRONLY, 0644), localPathBytes);
        if (!fd.valid()) {
            const QJsonObject err{{u"error"_s, Mtp::tr("Failed to create destination file (already exists or permission denied)")}};
            return jsonToRust(err);
        }

        MtpTransferReceiver receiver;
        {
            MtpSignalConnection signalConn(targetStoragePath.path(), receiver);
            if (!signalConn.finishedConnected()) {
                const QJsonObject err{{u"error"_s, u"Failed to connect to storage copyFinished signal"_s}};
                return jsonToRust(err);
            }

            QDBusUnixFileDescriptor dbusFd(fd.get());
            auto msg = QDBusMessage::createMethodCall(u"org.kde.kmtpd5"_s,
                targetStoragePath.path(), u"org.kde.kmtp.Storage"_s, u"getFileToFileDescriptor"_s);
            msg.setArguments({QVariant::fromValue(dbusFd), subPath});

            QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 30000);
            if (reply.type() != QDBusMessage::ReplyMessage) {
                const QJsonObject err{{u"error"_s, u"D-Bus call getFileToFileDescriptor failed: %1"_s.arg(reply.errorMessage())}};
                return jsonToRust(err);
            }

            const int ret = reply.arguments().value(0).toInt();
            if (ret != 0) {
                const QJsonObject err{{u"error"_s, u"Failed to start MTP download (code %1)"_s.arg(ret)}};
                return jsonToRust(err);
            }

            waitForTransfer(receiver);
        }

        const bool flushed = !receiver.m_finished || receiver.m_result != 0 || ::fsync(fd.get()) == 0;
        fd.close();
        if (!flushed)
            return mtpError(Mtp::tr("Could not flush downloaded file"));

        if (!receiver.m_finished) {
            const QJsonObject err{{u"error"_s, Mtp::tr("Download timed out (no progress for 30s)")}};
            return jsonToRust(err);
        }

        if (receiver.m_result != 0) {
            const QJsonObject err{{u"error"_s, Mtp::tr("Download failed with result %1").arg(receiver.m_result)}};
            return jsonToRust(err);
        }

        fd.keep();
        const QJsonObject res{{u"ok"_s, true}};
        return jsonToRust(res);
    } else if (actionStr == u"upload"_s) {
        const rust::String current = mtp_operation_json(rust::Str("stat"), url, rust::Str(""));
        const QJsonObject targetState = QJsonDocument::fromJson(QByteArray(current.data(), static_cast<qsizetype>(current.size()))).object();
        if (targetState.contains(u"error"_s) || targetState.value(u"exists"_s).toBool()) {
            return mtpError(Mtp::tr("Upload target already exists or could not be checked"));
        }

        if (argStr.isEmpty() || !QDir::isAbsolutePath(argStr)) {
            const QJsonObject err{{u"error"_s, Mtp::tr("Local source must be an absolute path")}};
            return jsonToRust(err);
        }
        if (subPath == u"/"_s) {
            const QJsonObject err{{u"error"_s, Mtp::tr("Destination path must include a filename")}};
            return jsonToRust(err);
        }

        const QByteArray localPathBytes = argStr.toUtf8();
        ScopedFd fd(::open(localPathBytes.constData(), O_RDONLY));
        if (!fd.valid()) {
            const QJsonObject err{{u"error"_s, Mtp::tr("Failed to open local source file")}};
            return jsonToRust(err);
        }

        MtpTransferReceiver receiver;
        {
            MtpSignalConnection signalConn(targetStoragePath.path(), receiver);
            if (!signalConn.finishedConnected()) {
                const QJsonObject err{{u"error"_s, Mtp::tr("Failed to connect to storage copyFinished signal")}};
                return jsonToRust(err);
            }

            QDBusUnixFileDescriptor dbusFd(fd.get());
            auto msg = QDBusMessage::createMethodCall(u"org.kde.kmtpd5"_s,
                targetStoragePath.path(), u"org.kde.kmtp.Storage"_s, u"sendFileFromFileDescriptor"_s);
            msg.setArguments({QVariant::fromValue(dbusFd), subPath});

            QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::Block, 30000);
            if (reply.type() != QDBusMessage::ReplyMessage) {
                const QJsonObject err{{u"error"_s, u"D-Bus call sendFileFromFileDescriptor failed: %1"_s.arg(reply.errorMessage())}};
                return jsonToRust(err);
            }

            const int ret = reply.arguments().value(0).toInt();
            if (ret != 0) {
                const QJsonObject err{{u"error"_s, Mtp::tr("Failed to start MTP upload (code %1)").arg(ret)}};
                return jsonToRust(err);
            }

            waitForTransfer(receiver);
        }

        fd.close();
        if (!receiver.m_finished) {
            const QJsonObject err{{u"error"_s, Mtp::tr("Upload timed out (no progress for 30s)")}};
            return jsonToRust(err);
        }

        if (receiver.m_result != 0) {
            const QJsonObject err{{u"error"_s, Mtp::tr("Upload failed with result %1").arg(receiver.m_result)}};
            return jsonToRust(err);
        }

        const QJsonObject res{{u"ok"_s, true}};
        return jsonToRust(res);
    } else {
        const QJsonObject err{{u"error"_s, Mtp::tr("Unknown action: %1").arg(actionStr)}};
        return jsonToRust(err);
    }
}

} // namespace loaf
