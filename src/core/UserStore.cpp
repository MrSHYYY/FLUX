#include "UserStore.h"
#include "Logger.h"
#include "../media/VLCPlayer.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QRegularExpression>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <algorithm>
#include <vector>

namespace Flux {

namespace {
constexpr qint64 kMinResumeMs = 1000;
constexpr double kWatchedFraction = 0.93;    // past 93% counts as watched
constexpr int kMaxHistoryEntries = 300;
constexpr int kMaxContinueEntries = 24;

struct SeriesLocation {
    QString key;
    QString url;
    QString title;
};

SeriesLocation seriesLocation(const QString &mediaUrl) {
    QUrl url(mediaUrl, QUrl::TolerantMode);
    if (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))
        return {};
    QStringList parts = url.path(QUrl::FullyDecoded).split('/', Qt::SkipEmptyParts);
    if (parts.size() < 2) return {};

    const QString episodeName = parts.constLast();
    static const QRegularExpression episodeRe(
        QStringLiteral("(?:\\bS\\d{1,2}\\s*E\\d{1,3}\\b|\\b\\d{1,2}x\\d{1,3}\\b)"),
        QRegularExpression::CaseInsensitiveOption);
    if (!episodeRe.match(episodeName).hasMatch()) return {};

    parts.removeLast(); // the episode file's parent folder
    if (parts.size() < 3) return {}; // Avoid treating a category folder as a show's folder.

    static const QRegularExpression seasonRe(
        QStringLiteral("^(?:season|series|s)[\\s._-]*\\d{1,2}(?:\\b.*)?$"),
        QRegularExpression::CaseInsensitiveOption);
    if (parts.size() > 1 && seasonRe.match(parts.constLast()).hasMatch())
        parts.removeLast();
    if (parts.isEmpty()) return {};

    const QString folderPath = QLatin1Char('/') + parts.join(QLatin1Char('/')) + QLatin1Char('/');
    url.setPath(folderPath);
    const QString folderUrl = url.toString(QUrl::FullyEncoded);
    return {folderUrl.toCaseFolded(), folderUrl, QUrl::fromPercentEncoding(parts.constLast().toUtf8())};
}
}

UserStore::UserStore(QObject *parent)
    : QObject(parent)
    , m_settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("FLUX"), QStringLiteral("FLUX")) {

    const QFileInfo settingsInfo(m_settings.fileName());
    QDir().mkpath(settingsInfo.absolutePath());
    m_historyPath = settingsInfo.absolutePath() + QStringLiteral("/history.json");
    m_posterPath = settingsInfo.absolutePath() + QStringLiteral("/posters.json");

    loadHistory();
    loadPosters();

    m_posterSaveTimer.setSingleShot(true);
    m_posterSaveTimer.setInterval(2000);
    connect(&m_posterSaveTimer, &QTimer::timeout, this, [this]() { savePosters(); });

    m_tickTimer.setInterval(5000);
    connect(&m_tickTimer, &QTimer::timeout, this, [this]() { checkpoint(true); });

    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(4000);
    connect(&m_saveTimer, &QTimer::timeout, this, [this]() { writeHistory(); });

    m_settingsTimer.setSingleShot(true);
    m_settingsTimer.setInterval(1000);
    connect(&m_settingsTimer, &QTimer::timeout, this, [this]() { m_settings.sync(); });

    FLUX_LOG_INFO("UserStore", QString("Settings: %1 | History: %2 (%3 entries)")
                  .arg(m_settings.fileName(), m_historyPath).arg(m_history.size()));
}

UserStore::~UserStore() {
    shutdown();
}

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------

QString UserStore::keyFor(const QString &url) {
    return QUrl::fromPercentEncoding(url.toUtf8());
}

QString UserStore::nameFromUrl(const QString &url) {
    QString decoded = keyFor(url);
    if (decoded.endsWith('/')) decoded.chop(1);
    const int slash = decoded.lastIndexOf('/');
    return slash >= 0 ? decoded.mid(slash + 1) : decoded;
}

// Identity of an item for poster lookups. ICC FTP addresses carry a session token that changes,
// so they are matched by their play id only; everything else by its decoded address.
QString UserStore::posterKey(const QString &url) {
    if (url.contains(QLatin1String("player.php"))) {
        static const QRegularExpression playRe(QStringLiteral("[?&]play=(\\d+)"));
        const QRegularExpressionMatch m = playRe.match(url);
        if (m.hasMatch()) return QStringLiteral("icc:") + m.captured(1);
    }
    QString key = QUrl::fromPercentEncoding(url.trimmed().toUtf8());
    while (key.endsWith(QLatin1Char('/'))) key.chop(1);
    return key.toCaseFolded();
}

QString UserStore::titleKey(const QString &title) {
    if (title.trimmed().isEmpty()) return QString();
    QString t = title.trimmed().toCaseFolded();
    static const QRegularExpression nonAlphanumRx(QStringLiteral("[^a-z0-9]+"));
    t.replace(nonAlphanumRx, QStringLiteral(" "));
    t = t.trimmed();
    return t.isEmpty() ? QString() : (QStringLiteral("title:") + t);
}

QString UserStore::folderKey(const QString &url) {
    if (url.trimmed().isEmpty()) return QString();
    QString key = QUrl::fromPercentEncoding(url.trimmed().toUtf8());
    while (key.endsWith(QLatin1Char('/'))) key.chop(1);
    return key.toCaseFolded();
}

// ----------------------------------------------------------------------------
// Player tracking
// ----------------------------------------------------------------------------

void UserStore::attachPlayer(VLCPlayer *player) {
    m_player = player;
    if (!m_player) return;
    connect(m_player, &VLCPlayer::stateChanged, this, &UserStore::onPlayerStateChanged);
    connect(m_player, &VLCPlayer::userSeeked, this, [this](qint64) {
        checkpoint(true, true);
        writeHistory();
    });
    connect(m_player, &VLCPlayer::aboutToStop, this, [this]() {
        checkpoint(true);
        writeHistory();
    });
}

void UserStore::onPlayerStateChanged() {
    if (!m_player || m_currentKey.isEmpty()) return;
    const QString st = m_player->state();

    if (st == QLatin1String("Playing")) {
        if (!m_tickTimer.isActive()) m_tickTimer.start();
        return;
    }

    if (st == QLatin1String("Ended")) {
        m_tickTimer.stop();
        auto it = m_history.find(m_currentKey);
        if (it != m_history.end() && keyFor(m_player->url()) == m_currentPlaybackKey) {
            it->watched = true;
            it->positionMs = 0;
            it->lastPlayed = QDateTime::currentMSecsSinceEpoch();
            ++m_revision;
            emit historyChanged();
            m_dirty = true;
            scheduleSave();
            writeHistory();
        }
        return;
    }

    if (st == QLatin1String("Paused") || st == QLatin1String("Stopped") || st == QLatin1String("Error")) {
        m_tickTimer.stop();
        checkpoint(true);
        writeHistory();
    }
}

void UserStore::checkpoint(bool notify, bool allowZeroPosition) {
    if (!m_player || m_currentKey.isEmpty()) return;
    // Only record progress for the file this entry actually belongs to
    if (keyFor(m_player->url()) != m_currentPlaybackKey) return;

    const qint64 pos = m_player->timeMs();
    const qint64 dur = m_player->durationMs();
    // A seek to zero is intentional. Ordinary zero-clock samples (startup/stop) are ignored.
    if ((pos <= 0 && !allowZeroPosition) || pos < 0) return;

    auto it = m_history.find(m_currentKey);
    if (it == m_history.end()) return;

    HistoryEntry &e = it.value();
    if (dur > 0) e.durationMs = dur;
    e.lastPlayed = QDateTime::currentMSecsSinceEpoch();

    if (dur > 0 && pos >= static_cast<qint64>(static_cast<double>(dur) * kWatchedFraction)) {
        e.watched = true;
        e.positionMs = 0;
    } else {
        e.positionMs = pos;
        if (pos >= kMinResumeMs || allowZeroPosition) e.watched = false; // replaying or seeking back
    }

    m_dirty = true;
    scheduleSave();

    if (notify) {
        ++m_revision;
        emit historyChanged();
    }
}

void UserStore::noteStart(const QString &url, const QString &title, const QString &playbackUrl) {
    const QString key = keyFor(url);
    if (key.isEmpty()) return;

    // Capture the previous file's final position before switching to the new one
    checkpoint(false);

    HistoryEntry &e = m_history[key];
    e.url = url;
    e.title = nameFromUrl(url);
    // Addresses like ".../player.php?play=123" have no readable file name: use the title we were given
    if (e.title.isEmpty() || ((url.contains(QLatin1String("player.php")) || e.title.contains('?')) && !title.isEmpty())) {
        e.title = title;
    }
    if (e.title.isEmpty()) e.title = title;
    e.lastPlayed = QDateTime::currentMSecsSinceEpoch();
    e.watched = false;
    if (e.positionMs < 0) e.positionMs = 0;

    m_currentKey = key;
    m_currentPlaybackKey = keyFor(playbackUrl.isEmpty() ? url : playbackUrl);
    if (!playbackUrl.isEmpty() && playbackUrl != url) {
        const QString p = posterFor(url, title);
        if (!p.isEmpty()) {
            rememberPoster(playbackUrl, p, title);
        }
    }
    m_dirty = true;
    scheduleSave();

    ++m_revision;
    emit historyChanged();
}

void UserStore::notePartyMedia(const QString &url, const QString &title, qint64 positionMs, qint64 durationMs) {
    const QString key = keyFor(url);
    if (key.isEmpty()) return;

    m_partyKey = key;

    HistoryEntry &e = m_history[key];
    e.url = url;
    e.title = nameFromUrl(url);
    if (e.title.isEmpty()) e.title = title;
    e.positionMs = std::max<qint64>(1000, positionMs);
    if (durationMs > 0) {
        e.durationMs = durationMs;
    } else if (e.durationMs <= 0) {
        e.durationMs = std::max<qint64>(e.positionMs * 2, 3600000);
    }
    e.lastPlayed = QDateTime::currentMSecsSinceEpoch();
    e.watched = false;

    m_dirty = true;
    scheduleSave();

    ++m_revision;
    emit historyChanged();
}

void UserStore::clearPartyMedia() {
    if (m_partyKey.isEmpty()) return;
    m_partyKey.clear();
    ++m_revision;
    emit historyChanged();
}

// ----------------------------------------------------------------------------
// Queries
// ----------------------------------------------------------------------------

QVariantMap UserStore::progressFor(const QString &url) const {
    QVariantMap map;
    const auto it = m_history.constFind(keyFor(url));
    if (it == m_history.constEnd()) return map;

    const HistoryEntry &e = it.value();
    const double fraction = (e.durationMs > 0 && e.positionMs > 0)
                            ? std::clamp(static_cast<double>(e.positionMs) / static_cast<double>(e.durationMs), 0.0, 1.0)
                            : 0.0;
    map["positionMs"] = e.positionMs;
    map["durationMs"] = e.durationMs;
    map["fraction"] = fraction;
    map["watched"] = e.watched;
    return map;
}

qint64 UserStore::resumePositionFor(const QString &url) const {
    const auto it = m_history.constFind(keyFor(url));
    if (it == m_history.constEnd()) return 0;

    const HistoryEntry &e = it.value();
    if (e.watched) return 0;
    if (e.positionMs < kMinResumeMs) return 0;
    if (e.durationMs > 0 && e.positionMs >= static_cast<qint64>(static_cast<double>(e.durationMs) * kWatchedFraction)) return 0;
    return e.positionMs;
}

QVariantList UserStore::continueWatching() const {
    std::vector<const HistoryEntry *> allEntries;
    allEntries.reserve(static_cast<size_t>(m_history.size()));

    for (auto it = m_history.constBegin(); it != m_history.constEnd(); ++it) {
        const HistoryEntry &e = it.value();
        const bool isParty = (!m_partyKey.isEmpty() && it.key() == m_partyKey);
        if (!isParty) {
            if (e.watched) continue;
            if (e.durationMs > 0 && e.positionMs >= static_cast<qint64>(static_cast<double>(e.durationMs) * kWatchedFraction)) continue;
        }
        allEntries.push_back(&e);
    }

    const auto recentFirst = [this](const HistoryEntry *a, const HistoryEntry *b) {
        if (!m_partyKey.isEmpty()) {
            const bool aParty = (keyFor(a->url) == m_partyKey);
            const bool bParty = (keyFor(b->url) == m_partyKey);
            if (aParty != bParty) return aParty;
        }
        return a->lastPlayed > b->lastPlayed;
    };
    std::sort(allEntries.begin(), allEntries.end(), recentFirst);

    QVariantList list;
    for (const HistoryEntry *e : allEntries) {
        if (list.size() >= kMaxContinueEntries) break;
        QVariantMap m;
        m["url"] = e->url;
        m["title"] = e->title;
        m["posterUrl"] = posterFor(e->url, e->title);
        m["positionMs"] = e->positionMs;
        m["durationMs"] = e->durationMs;
        m["fraction"] = e->durationMs > 0
                        ? std::clamp(static_cast<double>(e->positionMs) / static_cast<double>(e->durationMs), 0.0, 1.0)
                        : 0.0;
        m["lastPlayed"] = e->lastPlayed;
        m["isFolder"] = false;
        list.append(m);
    }
    return list;
}

// ----------------------------------------------------------------------------
// Mutations
// ----------------------------------------------------------------------------

void UserStore::removeFromHistory(const QString &url) {
    if (m_history.remove(keyFor(url))) {
        m_dirty = true;
        scheduleSave();
        ++m_revision;
        emit historyChanged();
    }
}

void UserStore::markWatched(const QString &url, bool watched) {
    auto it = m_history.find(keyFor(url));
    if (it == m_history.end()) return;
    it->watched = watched;
    if (watched) it->positionMs = 0;
    m_dirty = true;
    scheduleSave();
    ++m_revision;
    emit historyChanged();
}

void UserStore::clearHistory() {
    m_history.clear();
    m_dirty = true;
    scheduleSave();
    ++m_revision;
    emit historyChanged();
}

// ----------------------------------------------------------------------------
// Settings
// ----------------------------------------------------------------------------

QVariant UserStore::value(const QString &key, const QVariant &defaultValue) const {
    return m_settings.value(key, defaultValue);
}

int UserStore::intValue(const QString &key, int defaultValue) const {
    bool ok = false;
    const int v = m_settings.value(key, defaultValue).toInt(&ok);
    return ok ? v : defaultValue;
}

bool UserStore::boolValue(const QString &key, bool defaultValue) const {
    return m_settings.value(key, defaultValue).toBool();
}

void UserStore::setValue(const QString &key, const QVariant &value) {
    m_settings.setValue(key, value);
    m_settingsTimer.start();
}

bool UserStore::autoplayNext() const {
    return m_settings.value(QStringLiteral("player/autoplayNext"), true).toBool();
}

void UserStore::setAutoplayNext(bool enabled) {
    if (autoplayNext() == enabled) return;
    setValue(QStringLiteral("player/autoplayNext"), enabled);
    emit autoplayNextChanged();
}

bool UserStore::topPicksFirst() const {
    return m_settings.value(QStringLiteral("ui/topPicksFirst"), true).toBool();
}

void UserStore::setTopPicksFirst(bool first) {
    if (topPicksFirst() == first) return;
    setValue(QStringLiteral("ui/topPicksFirst"), first);
    emit topPicksFirstChanged();
}

int UserStore::activeServer() const {
    return m_settings.value(QStringLiteral("server/activeServer"), 2).toInt();
}

void UserStore::setActiveServer(int server) {
    if (server < 1 || server > 2) server = 1;
    if (activeServer() != server) {
        m_settings.setValue(QStringLiteral("server/activeServer"), server);
        m_settingsTimer.start();
        emit serverChanged();
    }
}

QString UserStore::iccftpUrl() const {
    return m_settings.value(QStringLiteral("server/iccftpUrl"),
        QStringLiteral("http://10.16.100.244/dashboard.php?session=b1bdfdae1f7e206b628274ef7eafadcd6094e07c46cd85b157c595174a369f96")).toString();
}

void UserStore::setIccftpUrl(const QString &url) {
    QString trimmed = url.trimmed();
    if (trimmed.isEmpty()) return;
    m_settings.setValue(QStringLiteral("server/iccftpUrl"), trimmed);

    const QUrl u(trimmed);
    const QUrlQuery q(u);
    const QString session = q.queryItemValue(QStringLiteral("session"));
    if (!session.isEmpty()) {
        m_settings.setValue(QStringLiteral("server/iccftpSession"), session);
    }
    m_settingsTimer.start();
    emit serverChanged();
}

QString UserStore::iccftpSession() const {
    return m_settings.value(QStringLiteral("server/iccftpSession"),
        QStringLiteral("b1bdfdae1f7e206b628274ef7eafadcd6094e07c46cd85b157c595174a369f96")).toString();
}

void UserStore::setIccftpSession(const QString &session) {
    QString trimmed = session.trimmed();
    if (trimmed.isEmpty()) return;
    m_settings.setValue(QStringLiteral("server/iccftpSession"), trimmed);
    m_settingsTimer.start();
    emit serverChanged();
}

void UserStore::testServerConnection(int server, const QString &customUrl, const QJSValue &callback) {
    auto *nam = new QNetworkAccessManager(this);
    QUrl targetUrl;
    if (server == 1) {
        targetUrl = QUrl(customUrl.isEmpty() ? QStringLiteral("http://172.16.50.7/DHAKA-FLIX-7/") : customUrl);
    } else {
        targetUrl = QUrl(customUrl.isEmpty() ? iccftpUrl() : customUrl);
    }

    QNetworkRequest request(targetUrl);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = nam->get(request);

    connect(reply, &QNetworkReply::finished, this, [nam, reply, callback]() mutable {
        reply->deleteLater();
        nam->deleteLater();

        bool ok = false;
        QString message;

        if (reply->error() == QNetworkReply::NoError) {
            int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (code >= 200 && code < 400) {
                ok = true;
                message = QStringLiteral("Connection successful! Server responded with HTTP %1.").arg(code);
            } else {
                message = QStringLiteral("Server returned HTTP status code %1.").arg(code);
            }
        } else {
            message = reply->errorString();
        }

        if (callback.isCallable()) {
            QJSValueList args;
            args << ok << message;
            callback.call(args);
        }
    });
}

// ----------------------------------------------------------------------------
// Posters
// ----------------------------------------------------------------------------

void UserStore::rememberPoster(const QString &url, const QString &posterUrl, const QString &title) {
    const QString poster = posterUrl.trimmed();
    if (poster.isEmpty()) return;

    bool updated = false;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    if (!url.trimmed().isEmpty()) {
        const QString key = posterKey(url);
        auto it = m_posters.find(key);
        if (it == m_posters.end() || it->url != poster) {
            PosterEntry entry;
            entry.url = poster;
            entry.usedAt = now;
            m_posters.insert(key, entry);
            updated = true;
        } else {
            it->usedAt = now;
        }

        const QString fKey = folderKey(url);
        if (!fKey.isEmpty() && fKey != key) {
            m_posters[fKey] = {poster, now};
            updated = true;
        }
    }

    const QString tKey = titleKey(title);
    if (!tKey.isEmpty()) {
        auto it = m_posters.find(tKey);
        if (it == m_posters.end() || it->url != poster) {
            PosterEntry entry;
            entry.url = poster;
            entry.usedAt = now;
            m_posters.insert(tKey, entry);
            updated = true;
        } else {
            it->usedAt = now;
        }
    }

    if (!updated) return;

    // Keep the hash bounded: forget the oldest posters first
    constexpr int kMaxPosters = 1500;
    if (m_posters.size() > kMaxPosters) {
        std::vector<std::pair<qint64, QString>> order;
        order.reserve(static_cast<size_t>(m_posters.size()));
        for (auto p = m_posters.constBegin(); p != m_posters.constEnd(); ++p) {
            order.emplace_back(p.value().usedAt, p.key());
        }
        std::sort(order.begin(), order.end());
        for (size_t i = 0; i < 300 && i < order.size(); ++i) m_posters.remove(order[i].second);
    }

    m_postersDirty = true;
    if (!m_posterSaveTimer.isActive()) m_posterSaveTimer.start();

    ++m_posterRevision;
    emit postersChanged();
}

QString UserStore::posterFor(const QString &url, const QString &title) const {
    if (!url.trimmed().isEmpty()) {
        const auto it = m_posters.constFind(posterKey(url));
        if (it != m_posters.constEnd() && !it->url.isEmpty()) return it->url;

        const auto fIt = m_posters.constFind(folderKey(url));
        if (fIt != m_posters.constEnd() && !fIt->url.isEmpty()) return fIt->url;

        const QString mapped = seriesFolderFor(url);
        if (!mapped.isEmpty() && mapped != url) {
            const auto mIt = m_posters.constFind(posterKey(mapped));
            if (mIt != m_posters.constEnd() && !mIt->url.isEmpty()) return mIt->url;
        }
    }

    if (!title.trimmed().isEmpty()) {
        const auto tIt = m_posters.constFind(titleKey(title));
        if (tIt != m_posters.constEnd() && !tIt->url.isEmpty()) return tIt->url;
    }

    if (!url.trimmed().isEmpty()) {
        const QString name = nameFromUrl(url);
        if (!name.isEmpty() && name != title) {
            const auto nIt = m_posters.constFind(titleKey(name));
            if (nIt != m_posters.constEnd() && !nIt->url.isEmpty()) return nIt->url;
        }
    }

    return QString();
}

void UserStore::mapSeriesFolder(const QString &mediaFolderUrl, const QString &webFolderUrl) {
    if (mediaFolderUrl.trimmed().isEmpty() || webFolderUrl.trimmed().isEmpty()) return;
    const QString k = folderKey(mediaFolderUrl);
    if (k.isEmpty()) return;
    if (m_seriesFolders.value(k) == webFolderUrl) return;

    m_seriesFolders.insert(k, webFolderUrl);
    m_postersDirty = true;
    if (!m_posterSaveTimer.isActive()) m_posterSaveTimer.start();
}

QString UserStore::seriesFolderFor(const QString &mediaFolderUrl) const {
    if (mediaFolderUrl.trimmed().isEmpty()) return QString();
    return m_seriesFolders.value(folderKey(mediaFolderUrl));
}

void UserStore::loadPosters() {
    QFile file(m_posterPath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) return;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (doc.isArray()) {
        for (const QJsonValue &v : doc.array()) {
            const QJsonObject o = v.toObject();
            const QString key = o.value("k").toString();
            const QString url = o.value("u").toString();
            if (key.isEmpty() || url.isEmpty()) continue;
            PosterEntry e;
            e.url = url;
            e.usedAt = static_cast<qint64>(o.value("t").toDouble());
            m_posters.insert(key, e);
        }
    } else if (doc.isObject()) {
        const QJsonObject rootObj = doc.object();
        for (const QJsonValue &v : rootObj.value("posters").toArray()) {
            const QJsonObject o = v.toObject();
            const QString key = o.value("k").toString();
            const QString url = o.value("u").toString();
            if (key.isEmpty() || url.isEmpty()) continue;
            PosterEntry e;
            e.url = url;
            e.usedAt = static_cast<qint64>(o.value("t").toDouble());
            m_posters.insert(key, e);
        }
        for (const QJsonValue &v : rootObj.value("seriesFolders").toArray()) {
            const QJsonObject o = v.toObject();
            const QString m = o.value("m").toString();
            const QString w = o.value("w").toString();
            if (!m.isEmpty() && !w.isEmpty()) {
                m_seriesFolders.insert(m, w);
            }
        }
    }
}

void UserStore::savePosters() {
    if (!m_postersDirty) return;

    QJsonObject rootObj;

    QJsonArray posterArr;
    for (auto it = m_posters.constBegin(); it != m_posters.constEnd(); ++it) {
        QJsonObject o;
        o["k"] = it.key();
        o["u"] = it.value().url;
        o["t"] = static_cast<double>(it.value().usedAt);
        posterArr.append(o);
    }
    rootObj["posters"] = posterArr;

    QJsonArray folderArr;
    for (auto it = m_seriesFolders.constBegin(); it != m_seriesFolders.constEnd(); ++it) {
        QJsonObject o;
        o["m"] = it.key();
        o["w"] = it.value();
        folderArr.append(o);
    }
    rootObj["seriesFolders"] = folderArr;

    QSaveFile file(m_posterPath);
    if (!file.open(QIODevice::WriteOnly)) {
        FLUX_LOG_WARN("UserStore", "Could not write posters file: " + m_posterPath);
        return;
    }
    file.write(QJsonDocument(rootObj).toJson(QJsonDocument::Compact));
    if (file.commit()) m_postersDirty = false;
}

// ----------------------------------------------------------------------------
// Persistence
// ----------------------------------------------------------------------------

void UserStore::scheduleSave() {
    if (!m_saveTimer.isActive()) m_saveTimer.start();
}

void UserStore::loadHistory() {
    QFile file(m_historyPath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) return;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray()) {
        FLUX_LOG_WARN("UserStore", "history.json is unreadable; starting with empty history");
        return;
    }

    for (const QJsonValue &v : doc.array()) {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        HistoryEntry e;
        e.url = o.value("url").toString();
        if (e.url.isEmpty()) continue;
        e.title = o.value("title").toString();
        e.positionMs = static_cast<qint64>(o.value("positionMs").toDouble());
        e.durationMs = static_cast<qint64>(o.value("durationMs").toDouble());
        e.lastPlayed = static_cast<qint64>(o.value("lastPlayed").toDouble());
        e.watched = o.value("watched").toBool();
        m_history.insert(keyFor(e.url), e);
    }
}

void UserStore::writeHistory() {
    if (!m_dirty) return;

    // Keep the file bounded: drop the oldest entries first
    if (m_history.size() > kMaxHistoryEntries) {
        std::vector<std::pair<qint64, QString>> order;
        order.reserve(static_cast<size_t>(m_history.size()));
        for (auto it = m_history.constBegin(); it != m_history.constEnd(); ++it) {
            order.emplace_back(it.value().lastPlayed, it.key());
        }
        std::sort(order.begin(), order.end());
        const int excess = static_cast<int>(m_history.size()) - kMaxHistoryEntries;
        int removed = 0;
        for (const auto &entry : order) {
            if (removed >= excess) break;
            if (entry.second == m_currentKey) continue;
            m_history.remove(entry.second);
            ++removed;
        }
    }

    QJsonArray arr;
    for (auto it = m_history.constBegin(); it != m_history.constEnd(); ++it) {
        const HistoryEntry &e = it.value();
        QJsonObject o;
        o["url"] = e.url;
        o["title"] = e.title;
        o["positionMs"] = static_cast<double>(e.positionMs);
        o["durationMs"] = static_cast<double>(e.durationMs);
        o["lastPlayed"] = static_cast<double>(e.lastPlayed);
        o["watched"] = e.watched;
        arr.append(o);
    }

    QSaveFile file(m_historyPath);
    if (!file.open(QIODevice::WriteOnly)) {
        FLUX_LOG_WARN("UserStore", "Could not write history file: " + m_historyPath);
        scheduleSave();
        return;
    }
    file.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    if (file.commit()) {
        m_dirty = false;
    } else {
        FLUX_LOG_WARN("UserStore", "Could not commit history file: " + m_historyPath);
        scheduleSave();
    }
}

void UserStore::shutdown() {
    m_tickTimer.stop();
    checkpoint(false);
    writeHistory();
    m_posterSaveTimer.stop();
    savePosters();
    m_settings.sync();
}

} // namespace Flux
