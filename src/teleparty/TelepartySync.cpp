#include "TelepartySync.h"
#include "TelepartySession.h"
#include "../media/VLCPlayer.h"
#include "../core/Logger.h"

#include <QUrl>
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace Flux {

namespace {

constexpr int kMaxUrlLength = 2048;
constexpr int kMaxTitleLength = 300;
constexpr qint64 kOpenDedupeMs = 8000;       // same video opened twice within this = one open
constexpr int kSeekMergeMs = 40;             // VLCPlayer already coalesces local seeks; keep this minimal
constexpr int kStateWaitMs = 5000;           // how long we accept a "state" answer after asking
constexpr int kDriftTickIntervalMs = 500;    // periodic drift evaluation cadence
constexpr qint64 kCatchUpDriftMs = 650;      // initial join catch-up seek threshold
constexpr qint64 kSoftLogCooldownMs = 2000;  // rate-limit soft drift logs
constexpr qint64 kReadinessMinIntervalMs = 800;

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

qint64 cleanTime(const QVariant &v) {
    // Positions come from other machines: never trust them blindly
    const qint64 t = v.toLongLong();
    return AuthoritativeTimeline::sanitizePositionMs(t);
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
    , m_catalog(this)
    , m_clock(std::make_shared<SystemMonotonicClock>()) {

    m_stateTimer.setSingleShot(true);
    connect(&m_stateTimer, &QTimer::timeout, this, [this]() { m_needState = false; });

    m_seekTimer.setSingleShot(true);
    m_seekTimer.setInterval(kSeekMergeMs);
    connect(&m_seekTimer, &QTimer::timeout, this, &TelepartySync::flushSeek);

    m_driftTimer.setInterval(kDriftTickIntervalMs);
    connect(&m_driftTimer, &QTimer::timeout, this, &TelepartySync::evaluateDriftTick);

    m_clockSyncTimer.setInterval(1500);
    connect(&m_clockSyncTimer, &QTimer::timeout, this, &TelepartySync::sendClockPing);

    m_scheduleTimer.setSingleShot(true);
    connect(&m_scheduleTimer, &QTimer::timeout, this, &TelepartySync::processScheduledActions);

    // Session -> player
    if (m_session) {
        connect(m_session, &TelepartySession::eventReceived, this, &TelepartySync::onEvent);
        connect(m_session, &TelepartySession::stateChanged, this, &TelepartySync::onSessionStateChanged);
        connect(m_session, &TelepartySession::authorityChanged, this, &TelepartySync::onAuthorityChanged);
        connect(m_session, &TelepartySession::memberLeft, this, [this](const QString &peerId) {
            m_peerReadiness.remove(peerId);
        });
        connect(m_session, &TelepartySession::memberJoined, this, [this](const QString &) {
            // Someone (re)joined: tell them what we are watching
            sendState();
        });
        connect(m_session, &TelepartySession::joinedSession, this, [this]() {
            resetParty();
            m_clockEstimator.reset(m_session->isHost(), m_session->hostMemberId());

            if (m_player) {
                m_driftTimer.start();
                if (!m_session->isHost()) {
                    m_clockSyncTimer.start(m_clockEstimator.recommendedPingIntervalMs());
                    sendClockPing();
                }
            }

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
        connect(m_player, &VLCPlayer::bufferingChanged, this, &TelepartySync::onPlayerBufferingChanged);
    }
}

void TelepartySync::setMonotonicClock(std::shared_ptr<MonotonicClock> clock) {
    if (clock) {
        m_clock = std::move(clock);
    } else {
        m_clock = std::make_shared<SystemMonotonicClock>();
    }
}

qint64 TelepartySync::localNowMs() const {
    return m_clock ? m_clock->nowMs() : 0;
}

qint64 TelepartySync::estimatedHostNowMs() const {
    return m_clockEstimator.estimatedHostTimeMs(localNowMs());
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
    const qint64 rev = m_timeline.nextRevision();
    const qint64 hostNow = estimatedHostNowMs();
    noteOpen(url, title.left(kMaxTitleLength), rev, hostNow, m_session->memberId());
    m_announceOnPlay = true;
    m_needState = false;
    m_stateTimer.stop();

    QVariantMap d;
    d.insert(QStringLiteral("id"), movieId);
    d.insert(QStringLiteral("rev"), rev);
    d.insert(QStringLiteral("ht"), hostNow);
    if (!isBuiltinCatalogId(movieId) && !m_currentTitle.isEmpty()) {
        d.insert(QStringLiteral("n"), m_currentTitle);
    }
    m_session->sendEvent(QStringLiteral("open"), d);
}

void TelepartySync::adoptCurrent(const QString &url, const QString &title) {
    if (!m_session || !m_session->active() || !isStreamUrl(url)) return;
    const QString movieId = encodeCatalogMovieId(url);
    if (movieId.isEmpty()) return;

    m_currentUrl = url;
    m_currentTitle = title.left(kMaxTitleLength);
    m_partyTimeMs = m_player ? m_player->timeMs() : 0;
    m_partyDurationMs = m_player ? m_player->durationMs() : 0;
    m_partyPlaying = playerRunning();
    m_partyUpdatedLocalMs = localNowMs();

    TimelineSnapshot snap;
    snap.sessionId = m_session->code();
    snap.revision = m_timeline.nextRevision();
    snap.authorId = m_session->memberId();
    snap.mediaId = movieId;
    snap.mediaUrl = m_currentUrl;
    snap.mediaTitle = m_currentTitle;
    snap.durationMs = m_partyDurationMs;
    snap.basePositionMs = m_partyTimeMs;
    snap.hostTimestampMs = estimatedHostNowMs();
    snap.playing = m_partyPlaying;
    snap.playbackRate = m_player ? m_player->playbackRate() : 1.0;
    snap.valid = true;
    m_timeline.forceUpdate(snap);

    emit currentMediaChanged();
    emit diagnosticsChanged();
}

void TelepartySync::joinPartyPlayback() {
    if (!m_session || !m_session->active() || m_currentUrl.isEmpty()) return;

    const qint64 startMs = partyTimeMs();
    m_pendingSync = true;
    m_pendingTimeMs = startMs;
    m_pendingPlaying = m_timeline.isValid() ? m_timeline.snapshot().playing : m_partyPlaying;
    m_pendingStartedLocalMs = localNowMs();

    armNeedState();
    m_session->sendEvent(QStringLiteral("syncme"));

    FLUX_LOG_INFO("Teleparty", QString("Joining party playback: %1 at %2")
                                   .arg(m_currentTitle.isEmpty() ? m_currentUrl : m_currentTitle, formatTime(startMs)));
    emit remoteOpenRequested(m_currentUrl, m_currentTitle, startMs);
}

// ============================================================================
// Local user -> everyone else (ActionOrigin::LocalUser only)
// ============================================================================

void TelepartySync::onUserToggledPlay(bool playing, qint64 timeMs) {
    // Prevent feedback loops: remote scheduled actions, drift corrections, and buffer
    // recoveries never broadcast outbound events.
    if (m_internalActionDepth > 0) return;
    if (!m_session || !m_session->active()) return;

    if (m_currentUrl.isEmpty() && m_player && isStreamUrl(m_player->url())) {
        m_currentUrl = m_player->url();
        emit currentMediaChanged();
    }
    if (m_currentUrl.isEmpty()) return;

    flushSeek(); // keep the order the user performed things in

    const qint64 posMs = AuthoritativeTimeline::sanitizePositionMs(timeMs, m_partyDurationMs);
    const qint64 localNow = localNowMs();
    const qint64 hostNow = estimatedHostNowMs();
    const qint64 leadMs = m_useScheduledLeadTime ? scheduledLeadTimeMs() : 0;
    const qint64 targetHostMs = hostNow + leadMs;
    const qint64 rev = m_timeline.nextRevision();

    m_partyTimeMs = posMs;
    m_partyPlaying = playing;
    m_partyUpdatedLocalMs = localNow;

    TimelineSnapshot snap = m_timeline.snapshot();
    snap.sessionId = m_session->code();
    snap.revision = rev;
    snap.authorId = m_session->memberId();
    if (snap.mediaId.isEmpty()) snap.mediaId = encodeCatalogMovieId(m_currentUrl);
    snap.mediaUrl = m_currentUrl;
    snap.mediaTitle = m_currentTitle;
    if (m_player && m_player->durationMs() > 0) snap.durationMs = m_player->durationMs();
    snap.basePositionMs = posMs;
    snap.hostTimestampMs = targetHostMs;
    snap.playing = playing;
    snap.playbackRate = 1.0;
    snap.valid = true;
    m_timeline.forceUpdate(snap);
    m_scheduler.cancelSuperseded(rev);

    QVariantMap d;
    d.insert(QStringLiteral("t"), posMs);
    d.insert(QStringLiteral("rev"), rev);
    d.insert(QStringLiteral("ht"), targetHostMs);
    m_session->sendEvent(playing ? QStringLiteral("play") : QStringLiteral("pause"), d);
    emit diagnosticsChanged();
}

void TelepartySync::onUserSeeked(qint64 timeMs) {
    if (m_internalActionDepth > 0) return;
    if (!m_session || !m_session->active()) return;

    if (m_currentUrl.isEmpty() && m_player && isStreamUrl(m_player->url())) {
        m_currentUrl = m_player->url();
        emit currentMediaChanged();
    }
    if (m_currentUrl.isEmpty()) return;

    m_partyTimeMs = AuthoritativeTimeline::sanitizePositionMs(timeMs, m_partyDurationMs);
    m_partyUpdatedLocalMs = localNowMs();
    m_driftController.notifyExternalSeek(m_partyUpdatedLocalMs, m_partyTimeMs);

    m_pendingSeekMs = m_partyTimeMs;
    m_hasPendingSeek = true;
    m_seekTimer.start();
}

void TelepartySync::flushSeek() {
    m_seekTimer.stop();
    if (!m_hasPendingSeek) return;
    m_hasPendingSeek = false;

    if (!m_session || !m_session->active() || m_currentUrl.isEmpty()) return;

    const qint64 posMs = AuthoritativeTimeline::sanitizePositionMs(m_pendingSeekMs, m_partyDurationMs);
    const qint64 hostNow = estimatedHostNowMs();
    const qint64 leadMs = m_useScheduledLeadTime ? scheduledLeadTimeMs() : 0;
    const qint64 targetHostMs = hostNow + leadMs;
    const qint64 rev = m_timeline.nextRevision();

    TimelineSnapshot snap = m_timeline.snapshot();
    snap.sessionId = m_session->code();
    snap.revision = rev;
    snap.authorId = m_session->memberId();
    if (snap.mediaId.isEmpty()) snap.mediaId = encodeCatalogMovieId(m_currentUrl);
    snap.mediaUrl = m_currentUrl;
    snap.mediaTitle = m_currentTitle;
    if (m_player && m_player->durationMs() > 0) snap.durationMs = m_player->durationMs();
    snap.basePositionMs = posMs;
    snap.hostTimestampMs = targetHostMs;
    snap.playing = m_timeline.isValid() ? m_timeline.snapshot().playing : m_partyPlaying;
    snap.playbackRate = 1.0;
    snap.valid = true;
    m_timeline.forceUpdate(snap);
    m_scheduler.cancelSuperseded(rev);

    QVariantMap d;
    d.insert(QStringLiteral("t"), posMs);
    d.insert(QStringLiteral("rev"), rev);
    d.insert(QStringLiteral("ht"), targetHostMs);
    m_session->sendEvent(QStringLiteral("seek"), d);
    emit diagnosticsChanged();
}

void TelepartySync::sendState() {
    if (!m_session || !m_session->active() || !m_player) return;
    if (m_hasPendingSeek) {
        flushSeek();
    }
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

    const qint64 posMs = m_player->timeMs();
    const qint64 durMs = m_player->durationMs();
    const bool playing = playerRunning();
    const qint64 hostNow = estimatedHostNowMs();
    const qint64 rev = std::max<qint64>(1, m_timeline.revision());

    if (!m_timeline.isValid() || m_session->isHost()) {
        TimelineSnapshot snap = m_timeline.snapshot();
        snap.sessionId = m_session->code();
        snap.revision = rev;
        snap.authorId = m_session->memberId();
        snap.mediaId = movieId;
        snap.mediaUrl = m_currentUrl;
        snap.mediaTitle = m_currentTitle;
        snap.durationMs = durMs;
        snap.basePositionMs = posMs;
        snap.hostTimestampMs = hostNow;
        snap.playing = playing;
        snap.playbackRate = 1.0;
        snap.valid = true;
        m_timeline.forceUpdate(snap);
    }

    QVariantMap d;
    d.insert(QStringLiteral("id"), movieId);
    if (!isBuiltinCatalogId(movieId) && !m_currentTitle.isEmpty()) {
        d.insert(QStringLiteral("n"), m_currentTitle);
    }
    d.insert(QStringLiteral("t"), posMs);
    d.insert(QStringLiteral("d"), durMs);
    d.insert(QStringLiteral("p"), playing ? 1 : 0);
    d.insert(QStringLiteral("rev"), m_timeline.revision());
    d.insert(QStringLiteral("ht"), hostNow);
    m_session->sendEvent(QStringLiteral("state"), d);
}

// ============================================================================
// Clock Synchronization ("ts") & Participant Readiness ("rd")
// ============================================================================

void TelepartySync::sendClockPing() {
    if (!m_session || !m_session->active() || m_session->isHost()) return;
    if (m_session->memberCount() <= 1) return;

    const qint64 t1 = localNowMs();
    m_lastClockPingLocalMs = t1;
    m_clockEstimator.registerOutgoingPing(t1);

    QVariantMap d;
    d.insert(QStringLiteral("q"), t1);
    if (m_clockEstimator.medianRttMs() > 0) {
        d.insert(QStringLiteral("l"), m_clockEstimator.medianRttMs());
    }
    m_session->sendEvent(QStringLiteral("ts"), d);
}

void TelepartySync::handleTimeSync(const QVariantMap &data, const QString &from) {
    if (!m_session || !m_session->active()) return;

    // 1. Incoming Pong (contains "r" = t2 and "s" = t3)
    if (data.contains(QStringLiteral("r")) && data.contains(QStringLiteral("s"))) {
        const QString targetPeer = data.value(QStringLiteral("p")).toString();
        if (!targetPeer.isEmpty() && targetPeer != m_session->memberId()) {
            return; // Pong addressed to a different participant
        }
        // Only accept time reference responses from the host / authority peer
        const QString authPeer = m_session->hostMemberId();
        if (!authPeer.isEmpty() && from != authPeer) {
            return;
        }

        const qint64 t1 = data.value(QStringLiteral("q")).toLongLong();
        const qint64 t2 = data.value(QStringLiteral("r")).toLongLong();
        const qint64 t3 = data.value(QStringLiteral("s")).toLongLong();
        const qint64 t4 = localNowMs();

        const bool hadEstimateBefore = m_clockEstimator.hasEstimate();
        if (m_clockEstimator.processSample(t1, t2, t3, t4)) {
            // If a "state" snapshot arrived before our first clock sample locked,
            // re-anchor the timeline to the exact host timestamp now!
            if (!hadEstimateBefore && m_reanchorStateOnFirstClockLock && m_timeline.isValid()) {
                m_reanchorStateOnFirstClockLock = false;
                TimelineSnapshot snap = m_timeline.snapshot();
                snap.basePositionMs = m_rawStateBasePositionMs;
                snap.hostTimestampMs = m_rawStateHostTimestampMs;
                m_timeline.forceUpdate(snap);
            }

            // Recalculate any pending scheduled transition timers with the refined clock offset
            processScheduledActions();
            updateScheduleTimer();

            // Adapt ping interval (fast initial burst -> slower steady state)
            if (m_clockSyncTimer.isActive()) {
                m_clockSyncTimer.setInterval(m_clockEstimator.recommendedPingIntervalMs());
            }
            emit diagnosticsChanged();
        }
        return;
    }

    // 2. Incoming Ping (contains "q" = t1): Host / authority responds with (t1, t2, t3)
    if (data.contains(QStringLiteral("q"))) {
        const bool isAuthority = m_session->isHost() ||
                                 (m_session->hostMemberId() == m_session->memberId());
        if (!isAuthority) {
            return;
        }

        const qint64 t2 = localNowMs();
        const qint64 t1 = data.value(QStringLiteral("q")).toLongLong();
        const qint64 reportedRtt = data.value(QStringLiteral("l"), 0).toLongLong();
        if (reportedRtt > 0) {
            m_clockEstimator.recordPeerRttSample(reportedRtt);
        }

        const qint64 t3 = localNowMs();
        QVariantMap pong;
        pong.insert(QStringLiteral("p"), from);
        pong.insert(QStringLiteral("q"), t1);
        pong.insert(QStringLiteral("r"), t2);
        pong.insert(QStringLiteral("s"), t3);
        m_session->sendEvent(QStringLiteral("ts"), pong);
        emit diagnosticsChanged();
    }
}

void TelepartySync::updateLocalReadiness(bool allowBroadcast) {
    ParticipantReadiness next = ParticipantReadiness::Idle;
    if (m_player && !m_currentUrl.isEmpty() && m_player->url() == m_currentUrl) {
        const QString st = m_player->state();
        if (st == QLatin1String("Error")) {
            next = ParticipantReadiness::Error;
        } else if (st == QLatin1String("Opening")) {
            next = ParticipantReadiness::Opening;
        } else if (m_player->isBuffering() || st.startsWith(QLatin1String("Buffering"))) {
            next = ParticipantReadiness::Buffering;
        } else if (m_player->isSeekSettling()) {
            next = ParticipantReadiness::Seeking;
        } else if (st == QLatin1String("Playing") || st == QLatin1String("Paused")) {
            next = ParticipantReadiness::Ready;
        }
    }

    if (next == m_localReadiness) {
        return;
    }
    m_localReadiness = next;

    if (!allowBroadcast || !m_session || !m_session->active() || m_session->memberCount() <= 1) {
        return;
    }

    const qint64 now = localNowMs();
    if (m_lastReadinessSentLocalMs > 0 && (now - m_lastReadinessSentLocalMs) < kReadinessMinIntervalMs
        && next != ParticipantReadiness::Ready) {
        return;
    }
    m_lastReadinessSentLocalMs = now;

    QVariantMap d;
    d.insert(QStringLiteral("s"), readinessToCode(m_localReadiness));
    d.insert(QStringLiteral("t"), m_player ? m_player->timeMs() : 0);
    d.insert(QStringLiteral("rev"), m_timeline.revision());
    m_session->sendEvent(QStringLiteral("rd"), d);
}

void TelepartySync::handleReadiness(const QVariantMap &data, const QString &from) {
    const QString code = data.value(QStringLiteral("s")).toString();
    m_peerReadiness.insert(from, readinessFromCode(code));
}

// ============================================================================
// Everyone else -> local player
// ============================================================================

void TelepartySync::onEvent(const QString &type, const QVariantMap &data, const QString &from) {
    if (type == QLatin1String("open")) {
        handleOpen(data, from);
    } else if (type == QLatin1String("state")) {
        handleState(data, from);
    } else if (type == QLatin1String("syncme")) {
        sendState();
    } else if (type == QLatin1String("ts")) {
        handleTimeSync(data, from);
    } else if (type == QLatin1String("rd")) {
        handleReadiness(data, from);
    } else if (type == QLatin1String("play") || type == QLatin1String("pause") || type == QLatin1String("seek")) {
        handlePlaybackTransition(type, data, from);
    }
}

void TelepartySync::handleOpen(const QVariantMap &data, const QString &from) {
    QString url;
    QString title;
    if (!resolveCatalogMovie(data, url, title)) {
        FLUX_LOG_WARN("Teleparty", "Rejected remote 'open' event: movie ID / URL is not in the local catalog");
        return;
    }

    const qint64 now = localNowMs();
    // Autoplay can make two members open the same next episode at the same moment
    if (url == m_lastOpenUrl && m_lastOpenAt > 0 && (now - m_lastOpenAt) < kOpenDedupeMs) return;

    const qint64 rev = data.contains(QStringLiteral("rev"))
                           ? data.value(QStringLiteral("rev")).toLongLong()
                           : m_timeline.nextRevision();
    const qint64 ht = data.contains(QStringLiteral("ht"))
                          ? data.value(QStringLiteral("ht")).toLongLong()
                          : estimatedHostNowMs();

    noteOpen(url, title, rev, ht, from);
    m_syncOnPlay = true;
    m_needState = false;
    m_stateTimer.stop();

    FLUX_LOG_INFO("Teleparty", QString("A member opened: %1 (rev=%2)").arg(title.isEmpty() ? url : title).arg(rev));
    emit remoteOpenRequested(url, title, 0);
}

void TelepartySync::handleState(const QVariantMap &data, const QString &from) {
    const qint64 incomingRev = data.value(QStringLiteral("rev"), 0).toLongLong();
    const bool isHigherRevision = (incomingRev > 0 && incomingRev > m_timeline.revision());

    if (!m_needState && !m_currentUrl.isEmpty() && !isHigherRevision) {
        return; // we did not ask (or already have an up-to-date revision)
    }

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
    const double rate = AuthoritativeTimeline::sanitizeRate(data.value(QStringLiteral("r"), 1.0).toDouble());
    const qint64 rawHt = data.value(QStringLiteral("ht"), 0).toLongLong();
    const qint64 rev = (incomingRev > 0) ? incomingRev : std::max<qint64>(1, m_timeline.nextRevision());

    const bool changed = (m_currentUrl != url || m_currentTitle != title);
    m_currentUrl = url;
    m_currentTitle = title;
    m_partyTimeMs = t;
    if (dur > 0) m_partyDurationMs = dur;
    m_partyPlaying = playing;
    m_partyUpdatedLocalMs = localNowMs();

    TimelineSnapshot snap;
    snap.sessionId = m_session ? m_session->code() : QString();
    snap.revision = rev;
    snap.authorId = from;
    snap.mediaId = encodeCatalogMovieId(url);
    snap.mediaUrl = url;
    snap.mediaTitle = title;
    snap.durationMs = m_partyDurationMs;
    snap.playing = playing;
    snap.playbackRate = rate;
    snap.valid = true;

    if (rawHt > 0 && m_clockEstimator.hasEstimate()) {
        snap.basePositionMs = t;
        snap.hostTimestampMs = rawHt;
        m_reanchorStateOnFirstClockLock = false;
    } else {
        // Clock estimator hasn't locked its first sample yet; compensate with estimated one-way delay
        // and store rawHt so the timeline re-anchors as soon as the first "ts" pong arrives.
        const qint64 oneWayCompMs = playing ? ClockEstimator::kDefaultOneWayMs : 0;
        snap.basePositionMs = AuthoritativeTimeline::sanitizePositionMs(t + oneWayCompMs, m_partyDurationMs);
        snap.hostTimestampMs = estimatedHostNowMs();
        if (rawHt > 0) {
            m_reanchorStateOnFirstClockLock = true;
            m_rawStateHostTimestampMs = rawHt;
            m_rawStateBasePositionMs = t;
        }
    }
    m_timeline.forceUpdate(snap);

    const qint64 liveExpectedPos = m_timeline.expectedPositionMs(estimatedHostNowMs());

    if (changed) emit currentMediaChanged();
    emit partyMediaAvailable(url, title, liveExpectedPos, m_partyDurationMs);
    emit diagnosticsChanged();

    // Already on that video on the player page: line up with the others
    if (m_watching && m_player && url == m_player->url()) {
        if (m_player->state() == QLatin1String("Opening") || m_player->isBuffering() || m_pendingSync) {
            m_pendingSync = true;
            m_pendingTimeMs = liveExpectedPos;
            m_pendingPlaying = playing;
            m_pendingStartedLocalMs = localNowMs();
        } else {
            align(liveExpectedPos, playing, QString(), ActionOrigin::JoinSync);
        }
        return;
    }

    // Late joiner on the Home / Search page: do NOT force-open or reset the party;
    // the video is now placed first in Continue Watching and the Now Playing pill is active.
    FLUX_LOG_INFO("Teleparty", QString("Party is watching: %1 at %2 (rev=%3)")
                                   .arg(title.isEmpty() ? url : title, formatTime(liveExpectedPos))
                                   .arg(rev));
}

void TelepartySync::handlePlaybackTransition(const QString &type, const QVariantMap &data, const QString &from) {
    const qint64 t = cleanTime(data.value(QStringLiteral("t")));
    const qint64 rev = data.contains(QStringLiteral("rev"))
                           ? data.value(QStringLiteral("rev")).toLongLong()
                           : m_timeline.nextRevision();
    const qint64 rawHt = data.value(QStringLiteral("ht"), 0).toLongLong();
    const double rate = AuthoritativeTimeline::sanitizeRate(data.value(QStringLiteral("r"), 1.0).toDouble());
    const qint64 seq = data.value(QStringLiteral("_seq"), 0).toLongLong();

    const QString eventId = (seq > 0)
                                ? QStringLiteral("%1:%2").arg(from).arg(seq)
                                : data.value(QStringLiteral("eid")).toString();

    const qint64 estHostNow = estimatedHostNowMs();
    const qint64 targetHostMs = (rawHt > 0 && m_clockEstimator.hasEstimate()) ? rawHt : estHostNow;

    ScheduledAction action;
    action.revision = rev;
    action.eventId = eventId;
    action.authorId = from;
    action.targetPositionMs = t;
    action.targetHostTimestampMs = targetHostMs;
    action.playbackRate = rate;
    action.origin = ActionOrigin::ScheduledRemote;

    if (type == QLatin1String("play")) {
        action.type = ScheduledActionType::Play;
        action.playing = true;
        action.activityNote = QStringLiteral("Resumed by a member");
    } else if (type == QLatin1String("pause")) {
        action.type = ScheduledActionType::Pause;
        action.playing = false;
        action.activityNote = QStringLiteral("Paused by a member");
    } else {
        action.type = ScheduledActionType::Seek;
        action.playing = m_timeline.isValid() ? m_timeline.snapshot().playing : m_partyPlaying;
        action.activityNote = QStringLiteral("Skipped to ") + formatTime(t);
    }

    const QString hostPeerId = m_session ? m_session->hostMemberId() : QString();
    ScheduledAction readyAction;
    const ScheduleStatus status = m_scheduler.schedule(action, estHostNow, localNowMs(), readyAction, hostPeerId);

    if (status == ScheduleStatus::DuplicateIgnored || status == ScheduleStatus::StaleDropped) {
        FLUX_LOG_DEBUG("Teleparty",
                       QString("Ignored %1 '%2' event from '%3' (rev=%4, currentRev=%5)")
                           .arg(status == ScheduleStatus::DuplicateIgnored ? "duplicate" : "stale/out-of-order",
                                type, from)
                           .arg(rev)
                           .arg(m_timeline.revision()));
        emit diagnosticsChanged();
        return;
    }

    // Update authoritative timeline
    TimelineSnapshot snap = m_timeline.snapshot();
    snap.sessionId = m_session ? m_session->code() : QString();
    snap.revision = rev;
    snap.eventId = eventId;
    snap.authorId = from;
    if (!m_currentUrl.isEmpty()) {
        if (snap.mediaId.isEmpty()) snap.mediaId = encodeCatalogMovieId(m_currentUrl);
        snap.mediaUrl = m_currentUrl;
        snap.mediaTitle = m_currentTitle;
    }
    snap.durationMs = m_partyDurationMs;
    snap.basePositionMs = t;
    snap.hostTimestampMs = targetHostMs;
    snap.playing = action.playing;
    snap.playbackRate = rate;
    snap.valid = !m_currentUrl.isEmpty() || snap.valid;
    m_timeline.forceUpdate(snap);

    const qint64 expectedNowPos = m_timeline.isValid() ? m_timeline.expectedPositionMs(estHostNow) : t;
    m_partyTimeMs = expectedNowPos;
    m_partyPlaying = action.playing;
    m_partyUpdatedLocalMs = localNowMs();

    if (!m_currentUrl.isEmpty()) {
        emit partyMediaAvailable(m_currentUrl, m_currentTitle, expectedNowPos, m_partyDurationMs);
    }

    if (status == ScheduleStatus::Queued) {
        updateScheduleTimer();
        emit diagnosticsChanged();
        return;
    }

    executeScheduledAction(readyAction);
    emit diagnosticsChanged();
}

void TelepartySync::processScheduledActions() {
    if (m_scheduler.pendingCount() == 0) {
        return;
    }

    const qint64 estHostNow = estimatedHostNowMs();
    const QList<ScheduledAction> due = m_scheduler.popDueActions(estHostNow);
    for (const ScheduledAction &action : due) {
        executeScheduledAction(action);
    }
    updateScheduleTimer();
    if (!due.isEmpty()) {
        emit diagnosticsChanged();
    }
}

void TelepartySync::updateScheduleTimer() {
    const qint64 delayMs = m_scheduler.nextWakeupDelayMs(estimatedHostNowMs());
    if (delayMs < 0) {
        m_scheduleTimer.stop();
    } else {
        m_scheduleTimer.start(static_cast<int>(delayMs));
    }
}

void TelepartySync::executeScheduledAction(const ScheduledAction &action) {
    // Not on the player page (or not loaded on the party video yet): keep tracking the
    // party's timestamp so clicking Continue Watching / Now Playing starts right there.
    if (!m_watching || m_currentUrl.isEmpty() || !m_player || m_player->url() != m_currentUrl) {
        return;
    }

    if (action.type == ScheduledActionType::Play) {
        align(action.targetPositionMs, true, action.activityNote, action.origin);
    } else if (action.type == ScheduledActionType::Pause) {
        align(action.targetPositionMs, false, action.activityNote, action.origin);
    } else if (action.type == ScheduledActionType::Seek) {
        applySeek(action.targetPositionMs, action.activityNote, action.origin);
        ScopedInternalAction guard(m_internalActionDepth);
        if (action.playing && m_player->isPaused()) {
            m_player->remoteResume();
        } else if (!action.playing && !m_player->isPaused()) {
            m_player->remotePause();
        }
    }
}

void TelepartySync::align(qint64 timeMs, bool playing, const QString &note, ActionOrigin origin) {
    if (!m_player) return;
    ScopedInternalAction guard(m_internalActionDepth);

    const QString st = m_player->state();
    const qint64 localNow = localNowMs();
    const bool isOpeningOrBuffering = (st == QLatin1String("Opening") ||
                                       m_player->isBuffering() ||
                                       st.startsWith(QLatin1String("Buffering")));

    if (st == QLatin1String("Ended") || st == QLatin1String("Stopped")) {
        if (playing) {
            m_player->remoteRestart(timeMs);
            m_driftController.notifyExternalSeek(localNow, timeMs);
        }
    } else if (playing) {
        const qint64 drift = std::llabs(m_player->timeMs() - timeMs);
        const qint64 threshold = (origin == ActionOrigin::JoinSync)
                                     ? kCatchUpDriftMs
                                     : m_driftController.config().hardSeekThresholdMs;
        const bool canSeekNow = !isOpeningOrBuffering &&
                                (origin == ActionOrigin::JoinSync || !m_player->isSeekSettling());
        if (canSeekNow && drift > threshold) {
            m_player->remoteSetRate(1.0);
            m_player->remoteSeekTo(timeMs);
            m_driftController.notifyExternalSeek(localNow, timeMs);
        }
        if (m_player->isPaused()) {
            m_player->remoteResume();
        }
    } else {
        m_player->remoteSetRate(1.0);
        m_player->remotePause();
        const qint64 drift = std::llabs(m_player->timeMs() - timeMs);
        if (!isOpeningOrBuffering && drift > m_driftController.config().pausedSeekThresholdMs) {
            m_player->remoteSeekTo(timeMs);
            m_driftController.notifyExternalSeek(localNow, timeMs);
        }
    }

    if (!note.isEmpty()) emit activity(note);
}

void TelepartySync::applySeek(qint64 timeMs, const QString &note, ActionOrigin) {
    if (!m_player) return;
    ScopedInternalAction guard(m_internalActionDepth);

    m_player->remoteSetRate(1.0);
    m_player->remoteSeekTo(timeMs);
    m_driftController.notifyExternalSeek(localNowMs(), timeMs);
    const QString msg = note.isEmpty() ? (QStringLiteral("Skipped to ") + formatTime(timeMs)) : note;
    emit activity(msg);
}

// ============================================================================
// Periodic Three-Tier Adaptive Drift Controller Loop
// ============================================================================

void TelepartySync::evaluateDriftTick() {
    processScheduledActions();
    updateLocalReadiness(true);

    if (!m_session || !m_session->active() || !m_watching || !m_player) {
        return;
    }
    if (m_currentUrl.isEmpty() || m_player->url() != m_currentUrl || !m_timeline.isValid()) {
        return;
    }
    if (m_pendingSync || m_needState || m_scheduler.pendingCount() > 0) {
        return;
    }

    const qint64 localNow = localNowMs();
    m_lastDriftTickLocalMs = localNow;

    const QString st = m_player->state();
    if (st == QLatin1String("Idle") || st == QLatin1String("Error")) {
        return;
    }

    PlayerObservation obs;
    obs.actualPositionMs = m_player->timeMs();
    obs.durationMs = m_player->durationMs();
    obs.isPlaying = m_player->isPlaying();
    obs.isPaused = m_player->isPaused();
    obs.isBuffering = m_player->isBuffering() || st.startsWith(QLatin1String("Buffering"));
    obs.isOpening = (st == QLatin1String("Opening"));
    obs.isSeekSettling = m_player->isSeekSettling();
    obs.isSeekable = m_player->isSeekable();
    obs.isEndedOrStopped = (st == QLatin1String("Ended") || st == QLatin1String("Stopped"));
    obs.currentRate = m_player->playbackRate();
    obs.rateControlSupported = m_rateControlSupported;

    const qint64 estHostNow = estimatedHostNowMs();
    const TimelineSnapshot &snap = m_timeline.snapshot();
    const qint64 expectedPos = m_timeline.expectedPositionMs(estHostNow);

    const DriftCorrectionDecision dec = m_driftController.evaluate(
        obs, expectedPos, snap.playing, snap.playbackRate, m_clockEstimator.uncertaintyMs(), localNow);

    {
        ScopedInternalAction guard(m_internalActionDepth);

        if (dec.shouldRestart) {
            m_player->remoteRestart(dec.targetSeekMs >= 0 ? dec.targetSeekMs : expectedPos);
            emit diagnosticsChanged();
            return;
        }
        if (dec.shouldPause) {
            m_player->remotePause();
        }
        if (dec.shouldResume) {
            m_player->remoteResume();
        }

        switch (dec.mode) {
        case DriftCorrectionMode::RateAdjust: {
            const bool accepted = m_player->remoteSetRate(dec.targetRate);
            if (!accepted) {
                m_rateControlSupported = false;
                m_driftController.notifyRateControlFailed();
                FLUX_LOG_WARN("Teleparty", "Playback rate adjustment unsupported by stream; falling back to seek control");
            } else if (m_lastSoftLogLocalMs == 0 || (localNow - m_lastSoftLogLocalMs) >= kSoftLogCooldownMs) {
                m_lastSoftLogLocalMs = localNow;
                FLUX_LOG_DEBUG("Teleparty",
                               QString("Soft drift rate adjust: drift=%1ms, rate=%2x, rev=%3")
                                   .arg(dec.signedDriftMs)
                                   .arg(dec.targetRate, 0, 'f', 3)
                                   .arg(snap.revision));
            }
            break;
        }
        case DriftCorrectionMode::RestoreRate: {
            m_player->remoteSetRate(dec.targetRate);
            FLUX_LOG_DEBUG("Teleparty",
                           QString("Restored playback rate to %1x (drift=%2ms)")
                               .arg(dec.targetRate, 0, 'f', 2)
                               .arg(dec.signedDriftMs));
            break;
        }
        case DriftCorrectionMode::HardSeek:
        case DriftCorrectionMode::BufferRecoverySeek: {
            m_player->remoteSetRate(snap.playbackRate);
            if (dec.targetSeekMs >= 0) {
                m_player->remoteSeekTo(dec.targetSeekMs);
                FLUX_LOG_INFO("Teleparty",
                              QString("%1 seek to %2ms (drift=%3ms, rev=%4)")
                                  .arg(dec.mode == DriftCorrectionMode::BufferRecoverySeek ? "Buffer-recovery" : "Hard",
                                       QString::number(dec.targetSeekMs),
                                       QString::number(dec.signedDriftMs),
                                       QString::number(snap.revision)));
            }
            break;
        }
        case DriftCorrectionMode::None:
        case DriftCorrectionMode::SuppressedBuffering:
        case DriftCorrectionMode::SuppressedSeeking:
            break;
        }
    }

    emit diagnosticsChanged();
}

void TelepartySync::stepSimulation() {
    const qint64 now = localNowMs();
    if (m_hasPendingSeek && (now - m_partyUpdatedLocalMs) >= kSeekMergeMs) {
        flushSeek();
    }
    processScheduledActions();

    if (m_session && m_session->active()) {
        if (!m_session->isHost() && m_session->memberCount() > 1) {
            const int pingInterval = m_clockEstimator.recommendedPingIntervalMs();
            if (m_lastClockPingLocalMs == 0 || (now - m_lastClockPingLocalMs) >= pingInterval) {
                sendClockPing();
            }
        }
        if (m_lastDriftTickLocalMs == 0 || (now - m_lastDriftTickLocalMs) >= kDriftTickIntervalMs) {
            evaluateDriftTick();
        }
    }
}

void TelepartySync::onPlayerBufferingChanged() {
    if (!m_player) return;
    updateLocalReadiness(true);
    // If buffering just started while playback rate was nudged, immediately restore 1.0x
    if (m_player->isBuffering() && std::abs(m_player->playbackRate() - 1.0) > 0.001) {
        ScopedInternalAction guard(m_internalActionDepth);
        m_player->remoteSetRate(1.0);
    }
}

void TelepartySync::onPlayerStateChanged() {
    if (!m_player) return;
    updateLocalReadiness(true);

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
    // Calculate exact expected position on the authoritative timeline (or fallback elapsed clock)
    const qint64 target = m_timeline.isValid()
                              ? m_timeline.expectedPositionMs(estimatedHostNowMs())
                              : (m_pendingTimeMs + (m_pendingPlaying ? std::max<qint64>(0, localNowMs() - m_pendingStartedLocalMs) : 0));
    const bool shouldPlay = m_timeline.isValid() ? m_timeline.snapshot().playing : m_pendingPlaying;

    ScopedInternalAction guard(m_internalActionDepth);
    if (std::llabs(m_player->timeMs() - target) > kCatchUpDriftMs) {
        m_player->remoteSeekTo(target);
        m_driftController.notifyExternalSeek(localNowMs(), target);
    }
    if (!shouldPlay) {
        m_player->remotePause();
    }
}

// ============================================================================
// Session & Authority Bookkeeping
// ============================================================================

void TelepartySync::onAuthorityChanged(const QString &authorityMemberId, bool localIsAuthority) {
    if (localIsAuthority) {
        const bool wasGuest = !m_clockEstimator.isSelfAuthority();
        const qint64 oldEstHostNow = estimatedHostNowMs();
        const qint64 newLocalNow = localNowMs();

        m_clockEstimator.setSelfAuthority(true);
        m_clockEstimator.setAuthorityPeerId(authorityMemberId);
        m_clockSyncTimer.stop();

        if (wasGuest && m_timeline.isValid()) {
            m_timeline.migrateHostClock(authorityMemberId, oldEstHostNow, newLocalNow);
            sendState();
        }
    } else {
        if (m_clockEstimator.authorityPeerId() != authorityMemberId || m_clockEstimator.isSelfAuthority()) {
            m_clockEstimator.reset(false, authorityMemberId);
            if (m_player && m_session && m_session->active()) {
                m_clockSyncTimer.start(m_clockEstimator.recommendedPingIntervalMs());
                sendClockPing();
            }
        }
    }
    emit diagnosticsChanged();
}

void TelepartySync::onSessionStateChanged() {
    if (!m_session) return;
    const QString s = m_session->state();

    if (s == QLatin1String("idle")) {
        resetParty();
    } else if (s == QLatin1String("joined") && m_prevSessionState == QLatin1String("reconnecting")) {
        // Reconnected after a drop: preserve known monotonic clock offset while restarting
        // fast ping bursts, and request current room state
        m_clockEstimator.softResetForReconnect();
        if (m_player && !m_session->isHost()) {
            m_clockSyncTimer.start(m_clockEstimator.recommendedPingIntervalMs());
            sendClockPing();
        }
        armNeedState();
        m_session->sendEvent(QStringLiteral("syncme"));
    }
    m_prevSessionState = s;
}

void TelepartySync::armNeedState() {
    m_needState = true;
    m_stateTimer.start(kStateWaitMs);
}

void TelepartySync::noteOpen(const QString &url, const QString &title, qint64 rev,
                             qint64 hostTimestampMs, const QString &authorId) {
    const bool changed = (m_currentUrl != url || m_currentTitle != title);
    m_currentUrl = url;
    m_currentTitle = title;
    m_partyTimeMs = 0;
    m_partyDurationMs = 0;
    m_partyPlaying = true;
    m_partyUpdatedLocalMs = localNowMs();
    m_lastOpenUrl = url;
    m_lastOpenAt = m_partyUpdatedLocalMs;
    m_pendingSync = false;
    m_syncOnPlay = false;
    m_announceOnPlay = false;
    m_reanchorStateOnFirstClockLock = false;
    m_rateControlSupported = true;

    m_scheduler.reset();
    m_driftController.reset();
    m_scheduleTimer.stop();

    if (m_player && std::abs(m_player->playbackRate() - 1.0) > 0.001) {
        ScopedInternalAction guard(m_internalActionDepth);
        m_player->remoteSetRate(1.0);
    }

    TimelineSnapshot snap;
    snap.sessionId = m_session ? m_session->code() : QString();
    snap.revision = std::max<qint64>(1, rev);
    snap.authorId = authorId;
    snap.mediaId = encodeCatalogMovieId(url);
    snap.mediaUrl = url;
    snap.mediaTitle = title;
    snap.durationMs = 0;
    snap.basePositionMs = 0;
    snap.hostTimestampMs = hostTimestampMs;
    snap.playing = true;
    snap.playbackRate = 1.0;
    snap.valid = true;
    m_timeline.forceUpdate(snap);

    if (changed) emit currentMediaChanged();
    emit diagnosticsChanged();
}

void TelepartySync::resetParty() {
    const bool hadMedia = !m_currentUrl.isEmpty();
    m_currentUrl.clear();
    m_currentTitle.clear();
    m_partyTimeMs = 0;
    m_partyDurationMs = 0;
    m_partyPlaying = true;
    m_partyUpdatedLocalMs = 0;
    m_lastOpenUrl.clear();
    m_lastOpenAt = 0;
    m_needState = false;
    m_pendingSync = false;
    m_syncOnPlay = false;
    m_announceOnPlay = false;
    m_hasPendingSeek = false;
    m_reanchorStateOnFirstClockLock = false;
    m_rateControlSupported = true;
    m_lastDriftTickLocalMs = 0;
    m_lastClockPingLocalMs = 0;
    m_lastSoftLogLocalMs = 0;
    m_lastReadinessSentLocalMs = 0;
    m_localReadiness = ParticipantReadiness::Idle;
    m_peerReadiness.clear();

    m_stateTimer.stop();
    m_seekTimer.stop();
    m_driftTimer.stop();
    m_clockSyncTimer.stop();
    m_scheduleTimer.stop();

    m_timeline.reset();
    m_clockEstimator.reset();
    m_scheduler.reset();
    m_driftController.reset();

    if (m_player && std::abs(m_player->playbackRate() - 1.0) > 0.001) {
        ScopedInternalAction guard(m_internalActionDepth);
        m_player->remoteSetRate(1.0);
    }

    if (hadMedia) emit currentMediaChanged();
    emit diagnosticsChanged();
}

bool TelepartySync::playerRunning() const {
    if (!m_player) return false;
    const QString st = m_player->state();
    return st == QLatin1String("Playing") || st == QLatin1String("Opening") || st.startsWith(QLatin1String("Buffering"));
}

qint64 TelepartySync::partyTimeMs() const {
    if (m_timeline.isValid()) {
        return m_timeline.expectedPositionMs(estimatedHostNowMs());
    }
    if (m_partyPlaying && m_partyUpdatedLocalMs > 0) {
        return m_partyTimeMs + std::max<qint64>(0, localNowMs() - m_partyUpdatedLocalMs);
    }
    return m_partyTimeMs;
}

// ============================================================================
// Observability & Diagnostics
// ============================================================================

qint64 TelepartySync::scheduledLeadTimeMs() const {
    const int members = m_session ? m_session->memberCount() : 1;
    return m_clockEstimator.recommendedLeadTimeMs(members);
}

qint64 TelepartySync::authoritativePositionMs() const {
    return m_timeline.isValid() ? m_timeline.expectedPositionMs(estimatedHostNowMs()) : m_partyTimeMs;
}

qint64 TelepartySync::actualPositionMs() const {
    return m_player ? m_player->timeMs() : 0;
}

double TelepartySync::currentPlaybackRate() const {
    return m_player ? m_player->playbackRate() : 1.0;
}

SyncDiagnostics TelepartySync::diagnostics() const {
    SyncDiagnostics diag;
    diag.smoothedRttMs = m_clockEstimator.medianRttMs();
    diag.minRttMs = m_clockEstimator.minRttMs();
    diag.worstRttMs = m_clockEstimator.worstRttMs();
    diag.jitterMs = m_clockEstimator.jitterMs();
    diag.clockOffsetMs = m_clockEstimator.clockOffsetMs();
    diag.clockUncertaintyMs = m_clockEstimator.uncertaintyMs();
    diag.clockConfidence = m_clockEstimator.confidence();
    diag.clockLocked = m_clockEstimator.isLocked();
    diag.clockSampleCount = m_clockEstimator.sampleCount();
    diag.rejectedSampleCount = m_clockEstimator.rejectedCount();

    diag.scheduledLeadTimeMs = scheduledLeadTimeMs();
    diag.pendingScheduledCount = m_scheduler.pendingCount();

    diag.currentRevision = m_timeline.revision();
    diag.authorityPeerId = m_session ? m_session->hostMemberId() : m_clockEstimator.authorityPeerId();
    diag.localReadiness = readinessToString(m_localReadiness);
    diag.authoritativePositionMs = authoritativePositionMs();
    diag.actualPositionMs = actualPositionMs();
    diag.signedDriftMs = m_driftController.lastSignedDriftMs();
    diag.smoothedDriftMs = m_driftController.smoothedDriftMs();
    diag.currentPlaybackRate = currentPlaybackRate();
    diag.activeCorrectionMode = activeCorrectionMode();

    diag.softCorrectionCount = m_driftController.softCorrectionCount();
    diag.hardSeekCorrectionCount = m_driftController.hardSeekCount();
    diag.bufferRecoveryCount = m_driftController.bufferRecoveryCount();
    diag.lateEventCount = m_scheduler.lateEventCount();
    diag.supersededActionCount = m_scheduler.supersededCount();
    diag.duplicateEventCount = m_scheduler.duplicateCount() + (m_session ? m_session->duplicateEventCount() : 0);
    diag.staleRevisionDropCount = m_scheduler.staleDroppedCount();
    return diag;
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
