#include "BookmarkManager.h"
#include "Logger.h"
#include "UserStore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QUrl>

namespace Flux {

BookmarkManager::BookmarkManager(QObject *parent)
    : QAbstractListModel(parent) {

    // Keep the file next to the settings (same per-user app-data folder)
    const QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                             QStringLiteral("FLUX"), QStringLiteral("FLUX"));
    const QFileInfo info(settings.fileName());
    QDir().mkpath(info.absolutePath());
    m_path = info.absolutePath() + QStringLiteral("/bookmarks.json");

    load();

    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(800);
    connect(&m_saveTimer, &QTimer::timeout, this, [this]() { save(); });

    FLUX_LOG_INFO("Bookmarks", QString("Loaded %1 bookmarks from %2").arg(m_entries.size()).arg(m_path));
}

BookmarkManager::~BookmarkManager() {
    flush();
}

// ----------------------------------------------------------------------------
// Model
// ----------------------------------------------------------------------------

int BookmarkManager::rowCount(const QModelIndex &parent) const {
    if (parent.isValid()) return 0;
    return static_cast<int>(m_entries.size());
}

QVariant BookmarkManager::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_entries.size())) {
        return QVariant();
    }
    const Entry &e = m_entries[static_cast<size_t>(index.row())];

    switch (role) {
    case TitleRole:       return e.title;
    case PathRole:        return e.parentPath;
    case IsFolderRole:    return e.isFolder;
    case SizeRole:        return e.formattedSize;
    case PlayUrlRole:     return e.url;
    case ExtensionRole:   return e.extension;
    case RawHrefRole:     return e.rawHref;
    case LibraryNameRole: return e.libraryName;
    case GroupRole:       return e.group;
    case PosterUrlRole: {
        if (!e.posterUrl.isEmpty()) return e.posterUrl;
        if (m_userStore) {
            const QString p = m_userStore->posterFor(e.url, e.title);
            if (!p.isEmpty()) return p;
        }
        return QString();
    }
    case LocalPathRole: {
        if (e.url.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive)) {
            QString lp = QUrl(e.url).toLocalFile();
            if (!lp.isEmpty()) return lp;
        }
        if (!e.parentPath.isEmpty() && (e.parentPath.contains(QLatin1String(":\\")) || e.parentPath.contains(QLatin1String(":/")))) {
            return e.parentPath;
        }
        return QString();
    }
    case IsLocalRole: {
        if (e.url.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive)) return true;
        if (!e.parentPath.isEmpty() && (e.parentPath.contains(QLatin1String(":\\")) || e.parentPath.contains(QLatin1String(":/")))) {
            return true;
        }
        if (!e.url.isEmpty() && !e.url.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive) && !e.url.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)) {
            return true;
        }
        return false;
    }
    default:              return QVariant();
    }
}

QHash<int, QByteArray> BookmarkManager::roleNames() const {
    QHash<int, QByteArray> roles;
    roles[TitleRole]       = "title";
    roles[PathRole]        = "parentPath";
    roles[IsFolderRole]    = "isFolder";
    roles[SizeRole]        = "formattedSize";
    roles[PlayUrlRole]     = "playUrl";
    roles[ExtensionRole]   = "extension";
    roles[RawHrefRole]     = "rawHref";
    roles[LibraryNameRole] = "libraryName";
    roles[GroupRole]       = "group";
    roles[LocalPathRole]   = "localPath";
    roles[IsLocalRole]     = "isLocal";
    roles[PosterUrlRole]   = "posterUrl";
    return roles;
}

// ----------------------------------------------------------------------------
// Operations
// ----------------------------------------------------------------------------

QString BookmarkManager::keyFor(const QString &url) {
    QString key = QUrl::fromPercentEncoding(url.trimmed().toUtf8());
    while (key.endsWith(QLatin1Char('/'))) key.chop(1);   // folder URLs may or may not end in '/'
    return key;
}

bool BookmarkManager::isBookmarked(const QString &url) const {
    return m_keys.contains(keyFor(url));
}

bool BookmarkManager::toggle(const QString &url, const QString &title, bool isFolder,
                             const QString &formattedSize, const QString &extension,
                             const QString &libraryName, const QString &parentPath,
                             const QString &posterUrl) {
    const QString key = keyFor(url);
    if (key.isEmpty()) return false;

    if (m_keys.contains(key)) {
        remove(url);
        return false;
    }

    QString poster = posterUrl.trimmed();
    if (poster.isEmpty() && m_userStore) {
        poster = m_userStore->posterFor(url, title);
    }

    Entry e;
    e.url = url;
    e.title = title;
    e.parentPath = parentPath;
    e.formattedSize = formattedSize;
    e.extension = extension;
    e.libraryName = libraryName;
    e.isFolder = isFolder;
    e.posterUrl = poster;
    e.rawHref = QUrl(url, QUrl::TolerantMode).path(QUrl::FullyEncoded);
    e.addedAt = QDateTime::currentMSecsSinceEpoch();

    beginInsertRows(QModelIndex(), 0, 0);
    m_entries.insert(m_entries.begin(), std::move(e));
    endInsertRows();

    m_keys.insert(key);
    changed();
    FLUX_LOG_INFO("Bookmarks", "Bookmarked: " + title);

    if (poster.isEmpty()) {
        resolveMissingPosters();
    }
    return true;
}

void BookmarkManager::setUserStore(UserStore *store) {
    m_userStore = store;
    resolveMissingPosters();
}

void BookmarkManager::resolveMissingPosters() {
    for (size_t i = 0; i < m_entries.size(); ++i) {
        Entry &e = m_entries[i];
        if (e.posterUrl.isEmpty() && m_userStore) {
            const QString p = m_userStore->posterFor(e.url, e.title);
            if (!p.isEmpty()) {
                e.posterUrl = p;
                m_dirty = true;
                QModelIndex idx = index(static_cast<int>(i), 0);
                emit dataChanged(idx, idx, {PosterUrlRole});
            }
        }

        if (e.posterUrl.isEmpty() && e.url.contains(QLatin1String("player.php"))) {
            const QString itemUrl = e.url;
            QNetworkRequest req{QUrl(itemUrl, QUrl::TolerantMode)};
            req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
            req.setTransferTimeout(8000);
            QNetworkReply *reply = m_network.get(req);
            connect(reply, &QNetworkReply::finished, this, [this, reply, itemUrl]() {
                reply->deleteLater();
                if (reply->error() != QNetworkReply::NoError) return;
                const QString html = QString::fromUtf8(reply->readAll());
                QString pagePosterUrl;
                static const QRegularExpression posterImgRx(
                    QStringLiteral("<img[^>]+src=[\"'](files/[^\"']+)[\"']"),
                    QRegularExpression::CaseInsensitiveOption);
                QRegularExpressionMatch pMatch = posterImgRx.match(html);
                QUrl base(itemUrl);
                if (pMatch.hasMatch()) {
                    QString rel = pMatch.captured(1).trimmed();
                    if (!rel.startsWith('/')) rel.prepend('/');
                    pagePosterUrl = base.scheme() + QStringLiteral("://") + base.authority() + rel;
                } else {
                    static const QRegularExpression posterScriptRx(
                        QStringLiteral("poster(?:Image)?['\"]?\\s*:\\s*['\"](files/[^'\"]+)['\"]"),
                        QRegularExpression::CaseInsensitiveOption);
                    QRegularExpressionMatch pScriptMatch = posterScriptRx.match(html);
                    if (pScriptMatch.hasMatch()) {
                        QString rel = pScriptMatch.captured(1).trimmed();
                        if (!rel.startsWith('/')) rel.prepend('/');
                        pagePosterUrl = base.scheme() + QStringLiteral("://") + base.authority() + rel;
                    }
                }
                if (!pagePosterUrl.isEmpty()) {
                    for (size_t idx = 0; idx < m_entries.size(); ++idx) {
                        if (m_entries[idx].url == itemUrl) {
                            m_entries[idx].posterUrl = pagePosterUrl;
                            m_dirty = true;
                            if (m_userStore) m_userStore->rememberPoster(itemUrl, pagePosterUrl, m_entries[idx].title);
                            QModelIndex mi = index(static_cast<int>(idx), 0);
                            emit dataChanged(mi, mi, {PosterUrlRole});
                            break;
                        }
                    }
                    if (!m_saveTimer.isActive()) m_saveTimer.start();
                }
            });
        }
    }
    if (m_dirty && !m_saveTimer.isActive()) m_saveTimer.start();
}

void BookmarkManager::remove(const QString &url) {
    const QString key = keyFor(url);
    for (size_t i = 0; i < m_entries.size(); ++i) {
        if (keyFor(m_entries[i].url) != key) continue;

        beginRemoveRows(QModelIndex(), static_cast<int>(i), static_cast<int>(i));
        m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(i));
        endRemoveRows();

        m_keys.remove(key);
        changed();
        return;
    }
}

void BookmarkManager::clear() {
    if (m_entries.empty()) return;
    beginResetModel();
    m_entries.clear();
    m_keys.clear();
    endResetModel();
    changed();
}

void BookmarkManager::changed() {
    ++m_revision;
    emit revisionChanged();
    emit countChanged();
    m_dirty = true;
    if (!m_saveTimer.isActive()) m_saveTimer.start();
}

// ----------------------------------------------------------------------------
// Persistence
// ----------------------------------------------------------------------------

void BookmarkManager::load() {
    QFile file(m_path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) return;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray()) {
        FLUX_LOG_WARN("Bookmarks", "bookmarks.json is unreadable; starting with no bookmarks");
        return;
    }

    for (const QJsonValue &v : doc.array()) {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();

        Entry e;
        e.url = o.value("url").toString();
        const QString key = keyFor(e.url);
        if (key.isEmpty() || m_keys.contains(key)) continue;

        e.title = o.value("title").toString();
        e.parentPath = o.value("parentPath").toString();
        e.formattedSize = o.value("formattedSize").toString();
        e.extension = o.value("extension").toString();
        e.libraryName = o.value("libraryName").toString();
        e.group = o.value("group").toString();
        e.rawHref = o.value("rawHref").toString();
        e.posterUrl = o.value("posterUrl").toString();
        e.isFolder = o.value("isFolder").toBool();
        e.addedAt = static_cast<qint64>(o.value("addedAt").toDouble());

        m_keys.insert(key);
        m_entries.push_back(std::move(e));
    }
}

void BookmarkManager::save() {
    if (!m_dirty) return;

    QJsonArray arr;
    for (const Entry &e : m_entries) {
        QJsonObject o;
        o["url"] = e.url;
        o["title"] = e.title;
        o["parentPath"] = e.parentPath;
        o["formattedSize"] = e.formattedSize;
        o["extension"] = e.extension;
        o["libraryName"] = e.libraryName;
        o["group"] = e.group;
        o["rawHref"] = e.rawHref;
        o["posterUrl"] = e.posterUrl;
        o["isFolder"] = e.isFolder;
        o["addedAt"] = static_cast<double>(e.addedAt);
        arr.append(o);
    }

    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly)) {
        FLUX_LOG_WARN("Bookmarks", "Could not write bookmarks file: " + m_path);
        return;
    }
    file.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    if (file.commit()) {
        m_dirty = false;
    }
}

void BookmarkManager::flush() {
    m_saveTimer.stop();
    save();
}

} // namespace Flux
