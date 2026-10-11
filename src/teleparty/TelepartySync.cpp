#include "TelepartySync.h"
#include "TelepartySession.h"
#include "../media/VLCPlayer.h"
#include "../core/Logger.h"

#include <QDateTime>
#include <QUrl>
#include <algorithm>

namespace Flux {

namespace {

constexpr int kMaxUrlLength = 2048;
constexpr int kMaxTitleLength = 300;
constexpr qint64 kOpenDedupeMs = 8000;     // same video opened twice within this = one open
constexpr int kSeekMergeMs = 40;           // VLCPlayer already coalesces local seeks; keep this minimal
constexpr int kStateWaitMs = 5000;         // how long we accept a "state" answer after asking
constexpr qint64 kPlayDriftMs = 800;       // playing: ignore drift smaller than this
constexpr qint64 kPauseDriftMs = 400;      // paused: everybody should show the same frame
constexpr qint64 kCatchUpDriftMs = 800;

struct BuiltinCatalogEntry {
    const char *id;
    const char *title;
    const char *url;
};

constexpr BuiltinCatalogEntry kBuiltinCatalog[] = {
    {
        "ant-man",
        "Ant-Man and the Wasp: Quantumania",
        "http://172.16.50.14/DHAKA-FLIX-14/English%20Movies%20%281080p%29/%282023%29%201080p/Ant-Man%20and%20the%20Wasp-Quantumania%20%282023%29%201080p%20DSNP/Ant-Man%20and%20the%20Wasp%20Quantumania%20%282023%29%201080p%20DSNP-WEB%20x265%20HEVC%2010bit%20AAC%205.1%20MSubs-PSA.mkv"
    },
    {
        "civil-war",
        "Civil War",
        "http://172.16.50.14/DHAKA-FLIX-14/English%20Movies%20%281080p%29/%282024%29%201080p/Civil%20War%20%282024%29%201080p%20%5BDual%20Audio%5D/Civil%20War%20%282024%29%201080p%20BluRay%20x265%20HEVC%20ESub%20%5BDual%20Audio%5D%5BHindi%205.1%2BEnglish%205.1%5D%20-mkvC.mkv"
    },
    {
        "kingdom-s1e1",
        "Kingdom S1E1",
        "http://172.16.50.14/DHAKA-FLIX-14/KOREAN%20TV%20%26%20WEB%20Series/Kingdom%20%28TV%20Series%202019%E2%80%93%20%29%201080p%20%5BDual%20Audio%5D/Season%201/Kingdom%20S01E01%20%201080p%20NF%20WEBRip%20x265%20HEVC%20MSubs%20%5BDual%20Audio%5D%5BEnglish%205.1%2BKorean%205.1%5D%20-OlaM.mkv"
    }
};

qint64 nowMs() {
    return QDateTime::currentMSecsSinceEpoch();
}

qint64 cleanTime(const QVariant &v) {
    // Positions come from other machines: never trust them blindly
    const qint64 t = v.toLongLong();
    constexpr qint64 kMaxMs = 48LL * 3600 * 1000;
    return std::clamp<qint64>(t, 0, kMaxMs);
}

bool hasUnsafeCharactersOrTraversal(const QString &str) {
    if (str.contains(QLatin1Char('\\'))) return true;
    for (const QChar &ch : str) {
        if (ch.unicode() < 0x20 || ch.unicode() == 0x7F) return true;
    }
    const QString decoded = QUrl::fromPercentEncoding(str.toUtf8());
    if (decoded.contains(QLatin1Char('\\'))) return true;
    const QStringList segments = decoded.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &seg : segments) {
        if (seg == QLatin1String(".") || seg == QLatin1String("..")) {
            return true;
        }
    }
    return false;
}

bool isBuiltinCatalogId(const QString &id) {
    for (const auto &entry : kBuiltinCatalog) {
        if (id == QLatin1String(entry.id)) {
            return true;
        }
    }
    return false;
}

QString deriveTitleFromPath(const QString &relPath) {
    const QString decoded = QUrl::fromPercentEncoding(relPath.toUtf8());
    const int slash = decoded.lastIndexOf(QLatin1Char('/'));
    QString name = (slash >= 0) ? decoded.mid(slash + 1) : decoded;
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot > 0) {
        name = name.left(dot);
    }
    return name.trimmed();
}

} // namespace

TelepartySync::TelepartySync(TelepartySession *session, VLCPlayer *player, QObject *parent)
    : QObject(parent)
    , m_session(session)
    , m_player(player)
    , m_catalog(this) {

    m_stateTimer.setSingleShot(true);
    connect(&m_stateTimer, &QTimer::timeout, this, [this]() { m_needState = false; });

    m_seekTimer.setSingleShot(true);
    m_seekTimer.setInterval(kSeekMergeMs);
    connect(&m_seekTimer, &QTimer::timeout, this, &TelepartySync::flushSeek);

    // Session -> player
    if (m_session) {
        connect(m_session, &TelepartySession::eventReceived, this, &TelepartySync::onEvent);
        connect(m_session, &TelepartySession::stateChanged, this, &TelepartySync::onSessionStateChanged);
        connect(m_session, &TelepartySession::memberJoined, this, [this](const QString &) {
            // Someone (re)joined: tell them what we are watching
            sendState();
        });
        connect(m_session, &TelepartySession::joinedSession, this, [this]() {
            resetParty();
            // A newcomer learns what the party is watching from the members' answers
            if (!m_session->isHost()) {
                armNeedState();
                m_session->sendEvent(QStringLiteral("syncme"));
            }
        });
    }

    // Player -> session
    if (m_player) {
        connect(m_player, &VLCPlayer::userToggledPlay, this, &TelepartySync::onUserToggledPlay);
        connect(m_player, &VLCPlayer::userSeeked, this, &TelepartySync::onUserSeeked);
        connect(m_player, &VLCPlayer::stateChanged, this, &TelepartySync::onPlayerStateChanged);
    }
}

// ============================================================================
// Catalog validation & compact movie IDs
// ============================================================================

QString TelepartySync::encodeCatalogMovieId(const QString &url) const {
    if (url.isEmpty() || url.size() > kMaxUrlLength) return QString();
    if (hasUnsafeCharactersOrTraversal(url)) return QString();

    const QUrl u(url);
    const QString scheme = u.scheme().toLower();
    if (!u.isValid() || (scheme != QLatin1String("http") && scheme != QLatin1String("https")) || u.host().isEmpty()) {
        return QString();
    }

    const QString decodedUrl = QUrl::fromPercentEncoding(url.toUtf8());

    // 1. Check built-in catalog items for ultra-short IDs ("ant-man", "civil-war", "kingdom-s1e1")
    for (const auto &entry : kBuiltinCatalog) {
        const QString entryUrl = QString::fromLatin1(entry.url);
        if (url == entryUrl || decodedUrl == QUrl::fromPercentEncoding(entryUrl.toUtf8())) {
            return QString::fromLatin1(entry.id);
        }
    }

    // 2. Match against the 17 official BDIX catalog roots in MediaLibraryModel
    const MediaRoot *bestRoot = nullptr;
    QString bestRelPath;
    int bestRootLen = -1;

    for (const MediaRoot &root : m_catalog.roots()) {
        QString rootBase = root.url;
        if (!rootBase.endsWith(QLatin1Char('/'))) {
            rootBase += QLatin1Char('/');
        }

        if (url.startsWith(rootBase, Qt::CaseInsensitive)) {
            const QString rel = url.mid(rootBase.size());
            if (!rel.isEmpty() && static_cast<int>(rootBase.size()) > bestRootLen) {
                bestRoot = &root;
                bestRelPath = rel;
                bestRootLen = static_cast<int>(rootBase.size());
            }
            continue;
        }

        // Percent-decoded prefix match (handles e.g. "(1080p)" vs "%281080p%29" in server responses)
        const QString decodedRoot = QUrl::fromPercentEncoding(rootBase.toUtf8());
        if (decodedUrl.startsWith(decodedRoot, Qt::CaseInsensitive)) {
            const int slashCount = static_cast<int>(rootBase.count(QLatin1Char('/')));
            int slashesSeen = 0;
            int cutPos = -1;
            for (int i = 0; i < url.size(); ++i) {
                if (url.at(i) == QLatin1Char('/')) {
                    if (++slashesSeen == slashCount) {
                        cutPos = i + 1;
                        break;
                    }
                }
            }
            if (cutPos > 0 && cutPos < url.size()) {
                const QString rel = url.mid(cutPos);
                if (!rel.isEmpty() && static_cast<int>(decodedRoot.size()) > bestRootLen) {
                    bestRoot = &root;
                    bestRelPath = rel;
                    bestRootLen = static_cast<int>(decodedRoot.size());
                }
            }
        }
    }

    if (!bestRoot || bestRelPath.isEmpty()) {
        return QString();
    }

    return bestRoot->id + QLatin1Char(':') + bestRelPath;
}

bool TelepartySync::resolveCatalogMovie(const QVariantMap &data, QString &outUrl, QString &outTitle) const {
    QString id = data.value(QStringLiteral("id")).toString().trimmed();
    QString providedTitle = data.value(QStringLiteral("n")).toString().trimmed();
    if (providedTitle.isEmpty()) {
        providedTitle = data.value(QStringLiteral("title")).toString().trimmed();
    }

    if (id.isEmpty()) {
        const QString rawUrl = data.value(QStringLiteral("url")).toString().trimmed();
        if (!rawUrl.isEmpty()) {
            id = encodeCatalogMovieId(rawUrl);
        }
    }

    if (id.isEmpty() || id.size() > kMaxUrlLength) {
        return false;
    }

    // 1. Check built-in short movie IDs
    for (const auto &entry : kBuiltinCatalog) {
        if (id == QLatin1String(entry.id)) {
            outUrl = QString::fromLatin1(entry.url);
            outTitle = (providedTitle.isEmpty() ? QString::fromLatin1(entry.title) : providedTitle)
                           .left(kMaxTitleLength);
            return true;
        }
    }

    // 2. Validate "<rootId>:<relativePath>" against MediaLibraryModel
    const int colon = id.indexOf(QLatin1Char(':'));
    if (colon <= 0 || colon >= id.size() - 1) {
        return false;
    }

    const QString rootId = id.left(colon).trimmed();
    const QString relPath = id.mid(colon + 1).trimmed();

    const MediaRoot *root = m_catalog.findById(rootId);
    if (!root) {
        return false;
    }

    if (relPath.isEmpty() ||
        relPath.startsWith(QLatin1Char('/')) ||
        relPath.contains(QLatin1Char('?')) ||
        relPath.contains(QLatin1Char('#')) ||
        hasUnsafeCharactersOrTraversal(relPath)) {
        return false;
    }

    const QString decodedRel = QUrl::fromPercentEncoding(relPath.toUtf8());
    if (decodedRel.contains(QLatin1Char(':'))) {
        return false; // reject embedded drive letters or schemes
    }

    QString rootBase = root->url;
    if (!rootBase.endsWith(QLatin1Char('/'))) {
        rootBase += QLatin1Char('/');
    }

    const QString fullUrl = rootBase + relPath;
    const QUrl parsed(fullUrl);
    if (!parsed.isValid() || parsed.host().isEmpty()) {
        return false;
    }

    outUrl = fullUrl;
    const QString fallbackTitle = deriveTitleFromPath(relPath);
    outTitle = (providedTitle.isEmpty() ? fallbackTitle : providedTitle).left(kMaxTitleLength);
    return true;
}

// ============================================================================
// UI hooks
// ============================================================================

void TelepartySync::setWatching(bool watching) {
    if (m_watching == watching) return;
    m_watching = watching;
    emit watchingChanged();

    // Coming back to a video the party is on (e.g. via "Now Playing"): catch up with it
    if (watching && m_session && m_session->active() && !m_currentUrl.isEmpty()
        && m_player && m_player->url() == m_currentUrl) {
        armNeedState();
        m_session->sendEvent(QStringLiteral("syncme"));
    }
}

bool TelepartySync::isStreamUrl(const QString &url) const {
    return !encodeCatalogMovieId(url).isEmpty();
}

void TelepartySync::broadcastOpen(const QString &url, const QString &title) {
    if (!m_session || !m_session->active()) return;
    const QString movieId = encodeCatalogMovieId(url);
    if (movieId.isEmpty()) return;

    flushSeek();
    noteOpen(url, title.left(kMaxTitleLength));
    m_announceOnPlay = true;
    m_needState = false;
    m_stateTimer.stop();

    QVariantMap d;
    d.insert(QStringLiteral("id"), movieId);
    if (!isBuiltinCatalogId(movieId) && !m_currentTitle.isEmpty()) {
        d.insert(QStringLiteral("n"), m_currentTitle);
    }
    m_session->sendEvent(QStringLiteral("open"), d);
}

void TelepartySync::adoptCurrent(const QString &url, const QString &title) {
    if (!m_session || !m_session->active() || !isStreamUrl(url)) return;
    m_currentUrl = url;
    m_currentTitle = title.left(kMaxTitleLength);
    m_partyTimeMs = m_player ? m_player->timeMs() : 0;
    m_partyDurationMs = m_player ? m_player->durationMs() : 0;
    m_partyPlaying = playerRunning();
    m_partyClock.start();
    emit currentMediaChanged();
}

void TelepartySync::joinPartyPlayback() {
    if (!m_session || !m_session->active() || m_currentUrl.isEmpty()) return;

    const qint64 startMs = partyTimeMs();
    m_pendingSync = true;
    m_pendingTimeMs = startMs;
    m_pendingPlaying = m_partyPlaying;
    m_pendingClock.start();

    armNeedState();
    m_session->sendEvent(QStringLiteral("syncme"));

    FLUX_LOG_INFO("Teleparty", QString("Joining party playback: %1 at %2")
                                   .arg(m_currentTitle.isEmpty() ? m_currentUrl : m_currentTitle, formatTime(startMs)));
    emit remoteOpenRequested(m_currentUrl, m_currentTitle, startMs);
}

// ============================================================================
// Local user -> everyone else
// ============================================================================

void TelepartySync::onUserToggledPlay(bool playing, qint64 timeMs) {
    if (!m_session || !m_session->active()) return;
    if (m_currentUrl.isEmpty() && m_player && isStreamUrl(m_player->url())) {
        m_currentUrl = m_player->url();
        emit currentMediaChanged();
    }
    if (m_currentUrl.isEmpty()) return;

    m_partyTimeMs = qMax<qint64>(0, timeMs);
    m_partyPlaying = playing;
    m_partyClock.start();

    flushSeek(); // keep the order the user performed things in
    QVariantMap d;
    d.insert(QStringLiteral("t"), m_partyTimeMs);
    m_session->sendEvent(playing ? QStringLiteral("play") : QStringLiteral("pause"), d);
}

void TelepartySync::onUserSeeked(qint64 timeMs) {
    if (!m_session || !m_session->active()) return;
    if (m_currentUrl.isEmpty() && m_player && isStreamUrl(m_player->url())) {
        m_currentUrl = m_player->url();
        emit currentMediaChanged();
    }
    if (m_currentUrl.isEmpty()) return;

    m_partyTimeMs = qMax<qint64>(0, timeMs);
    m_partyClock.start();

    m_pendingSeekMs = m_partyTimeMs;
    m_hasPendingSeek = true;
    m_seekTimer.start();
}

void TelepartySync::flushSeek() {
    m_seekTimer.stop();
    if (!m_hasPendingSeek) return;
    m_hasPendingSeek = false;

    if (!m_session || !m_session->active() || m_currentUrl.isEmpty()) return;
    QVariantMap d;
    d.insert(QStringLiteral("t"), m_pendingSeekMs);
    m_session->sendEvent(QStringLiteral("seek"), d);
}

void TelepartySync::sendState() {
    if (!m_session || !m_session->active() || !m_player) return;
    if (m_currentUrl.isEmpty() && isStreamUrl(m_player->url())
        && m_player->state() != QLatin1String("Idle") && m_player->state() != QLatin1String("Stopped")) {
        m_currentUrl = m_player->url();
        emit currentMediaChanged();
    }
    if (m_currentUrl.isEmpty()) return;
    if (m_needState || m_player->url() != m_currentUrl) return; // not in a position to answer
    const QString st = m_player->state();
    if (st == QLatin1String("Opening") || st == QLatin1String("Idle") || st == QLatin1String("Stopped")) return;

    const QString movieId = encodeCatalogMovieId(m_currentUrl);
    if (movieId.isEmpty()) return;

    QVariantMap d;
    d.insert(QStringLiteral("id"), movieId);
    if (!isBuiltinCatalogId(movieId) && !m_currentTitle.isEmpty()) {
        d.insert(QStringLiteral("n"), m_currentTitle);
    }
    d.insert(QStringLiteral("t"), m_player->timeMs());
    d.insert(QStringLiteral("d"), m_player->durationMs());
    d.insert(QStringLiteral("p"), playerRunning() ? 1 : 0);
    m_session->sendEvent(QStringLiteral("state"), d);
}

// ============================================================================
// Everyone else -> local player
// ============================================================================

void TelepartySync::onEvent(const QString &type, const QVariantMap &data, const QString &) {
    if (type == QLatin1String("open")) {
        handleOpen(data);
    } else if (type == QLatin1String("state")) {
        handleState(data);
    } else if (type == QLatin1String("syncme")) {
        sendState();
    } else if (type == QLatin1String("play") || type == QLatin1String("pause") || type == QLatin1String("seek")) {
        const qint64 t = cleanTime(data.value(QStringLiteral("t")));
        m_partyTimeMs = t;
        if (type == QLatin1String("play")) m_partyPlaying = true;
        else if (type == QLatin1String("pause")) m_partyPlaying = false;
        m_partyClock.start();

        if (!m_currentUrl.isEmpty()) {
            emit partyMediaAvailable(m_currentUrl, m_currentTitle, t, m_partyDurationMs);
        }

        // Not on the player page (or not loaded on the party video yet): keep tracking the
        // party's timestamp so clicking Continue Watching / Now Playing starts right there.
        if (!m_watching || m_currentUrl.isEmpty() || !m_player || m_player->url() != m_currentUrl) return;

        if (type == QLatin1String("play")) {
            align(t, true, QStringLiteral("Resumed by a member"));
        } else if (type == QLatin1String("pause")) {
            align(t, false, QStringLiteral("Paused by a member"));
        } else {
            applySeek(t);
        }
    }
}

void TelepartySync::handleOpen(const QVariantMap &data) {
    QString url;
    QString title;
    if (!resolveCatalogMovie(data, url, title)) {
        FLUX_LOG_WARN("Teleparty", "Rejected remote 'open' event: movie ID / URL is not in the local catalog");
        return;
    }

    // Autoplay can make two members open the same next episode at the same moment
    if (url == m_lastOpenUrl && nowMs() - m_lastOpenAt < kOpenDedupeMs) return;

    noteOpen(url, title);
    m_syncOnPlay = true;
    m_needState = false;
    m_stateTimer.stop();

    FLUX_LOG_INFO("Teleparty", QString("A member opened: %1").arg(title.isEmpty() ? url : title));
    emit remoteOpenRequested(url, title, 0);
}

void TelepartySync::handleState(const QVariantMap &data) {
    if (!m_needState && !m_currentUrl.isEmpty()) return; // we did not ask (or already have an answer)

    QString url;
    QString title;
    if (!resolveCatalogMovie(data, url, title)) {
        FLUX_LOG_WARN("Teleparty", "Rejected remote 'state' event: movie ID / URL is not in the local catalog");
        return;
    }

    m_needState = false;
    m_stateTimer.stop();

    const qint64 t = cleanTime(data.value(QStringLiteral("t")));
    const qint64 dur = cleanTime(data.value(QStringLiteral("d")));
    const bool playing = data.contains(QStringLiteral("p"))
                             ? (data.value(QStringLiteral("p")).toInt() != 0)
                             : data.value(QStringLiteral("playing"), true).toBool();

    const bool changed = (m_currentUrl != url || m_currentTitle != title);
    m_currentUrl = url;
    m_currentTitle = title;
    m_partyTimeMs = t;
    if (dur > 0) m_partyDurationMs = dur;
    m_partyPlaying = playing;
    m_partyClock.start();

    if (changed) emit currentMediaChanged();
    emit partyMediaAvailable(url, title, t, m_partyDurationMs);

    // Already on that video on the player page: line up with the others
    if (m_watching && m_player && url == m_player->url()) {
        if (m_player->state() == QLatin1String("Opening") || m_pendingSync) {
            m_pendingSync = true;
            m_pendingTimeMs = t;
            m_pendingPlaying = playing;
            m_pendingClock.start();
        } else {
            align(t, playing, QString());
        }
        return;
    }

    // Late joiner on the Home / Search page: do NOT force-open or reset the party;
    // the video is now placed first in Continue Watching and the Now Playing pill is active.
    FLUX_LOG_INFO("Teleparty", QString("Party is watching: %1 at %2")
                                   .arg(title.isEmpty() ? url : title, formatTime(t)));
}

void TelepartySync::align(qint64 timeMs, bool playing, const QString &note) {
    if (!m_player) return;
    const QString st = m_player->state();

    if (st == QLatin1String("Ended") || st == QLatin1String("Stopped")) {
        if (playing) m_player->remoteRestart(timeMs);
    } else if (playing) {
        if (qAbs(m_player->timeMs() - timeMs) > kPlayDriftMs) m_player->remoteSeekTo(timeMs);
        if (m_player->isPaused()) m_player->remoteResume();
    } else {
        m_player->remotePause();
        if (qAbs(m_player->timeMs() - timeMs) > kPauseDriftMs) m_player->remoteSeekTo(timeMs);
    }

    if (!note.isEmpty()) emit activity(note);
}

void TelepartySync::applySeek(qint64 timeMs) {
    if (!m_player) return;
    m_player->remoteSeekTo(timeMs);
    emit activity(QStringLiteral("Skipped to ") + formatTime(timeMs));
}

void TelepartySync::onPlayerStateChanged() {
    if (!m_player) return;
    const QString st = m_player->state();
    if (st == QLatin1String("Error")) {
        m_pendingSync = false;
        m_syncOnPlay = false;
        m_announceOnPlay = false;
        return;
    }
    if (st != QLatin1String("Playing")) return;

    if (m_announceOnPlay) {
        m_announceOnPlay = false;
        sendState();
    }
    if (m_syncOnPlay) {
        m_syncOnPlay = false;
        armNeedState();
        if (m_session) m_session->sendEvent(QStringLiteral("syncme"));
    }

    if (!m_pendingSync) return;

    m_pendingSync = false;
    // The party kept playing while we were loading
    const qint64 target = m_pendingTimeMs + (m_pendingPlaying ? m_pendingClock.elapsed() : 0);
    if (qAbs(m_player->timeMs() - target) > kCatchUpDriftMs) m_player->remoteSeekTo(target);
    if (!m_pendingPlaying) m_player->remotePause();
}

// ============================================================================
// Session bookkeeping
// ============================================================================

void TelepartySync::onSessionStateChanged() {
    if (!m_session) return;
    const QString s = m_session->state();

    if (s == QLatin1String("idle")) {
        resetParty();
    } else if (s == QLatin1String("joined") && m_prevSessionState == QLatin1String("reconnecting")) {
        // We may have missed things while offline; our re-announcement makes the other
        // members send their state, which we are now ready to accept
        armNeedState();
        m_session->sendEvent(QStringLiteral("syncme"));
    }
    m_prevSessionState = s;
}

void TelepartySync::armNeedState() {
    m_needState = true;
    m_stateTimer.start(kStateWaitMs);
}

void TelepartySync::noteOpen(const QString &url, const QString &title) {
    const bool changed = (m_currentUrl != url || m_currentTitle != title);
    m_currentUrl = url;
    m_currentTitle = title;
    m_partyTimeMs = 0;
    m_partyDurationMs = 0;
    m_partyPlaying = true;
    m_partyClock.start();
    m_lastOpenUrl = url;
    m_lastOpenAt = nowMs();
    m_pendingSync = false;
    m_syncOnPlay = false;
    m_announceOnPlay = false;
    if (changed) emit currentMediaChanged();
}

void TelepartySync::resetParty() {
    const bool hadMedia = !m_currentUrl.isEmpty();
    m_currentUrl.clear();
    m_currentTitle.clear();
    m_partyTimeMs = 0;
    m_partyDurationMs = 0;
    m_partyPlaying = true;
    m_partyClock.invalidate();
    m_lastOpenUrl.clear();
    m_lastOpenAt = 0;
    m_needState = false;
    m_pendingSync = false;
    m_syncOnPlay = false;
    m_announceOnPlay = false;
    m_hasPendingSeek = false;
    m_stateTimer.stop();
    m_seekTimer.stop();
    if (hadMedia) emit currentMediaChanged();
}

bool TelepartySync::playerRunning() const {
    if (!m_player) return false;
    const QString st = m_player->state();
    return st == QLatin1String("Playing") || st == QLatin1String("Opening") || st.startsWith(QLatin1String("Buffering"));
}

qint64 TelepartySync::partyTimeMs() const {
    if (m_partyPlaying && m_partyClock.isValid()) {
        return m_partyTimeMs + m_partyClock.elapsed();
    }
    return m_partyTimeMs;
}

QString TelepartySync::formatTime(qint64 ms) {
    const qint64 total = qMax<qint64>(0, ms) / 1000;
    const qint64 h = total / 3600;
    const qint64 m = (total % 3600) / 60;
    const qint64 s = total % 60;
    if (h > 0) {
        return QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

} // namespace Flux
