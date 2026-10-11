#include "TelepartyEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace Flux {

// ============================================================================
// Origin & Readiness Helpers
// ============================================================================

QString actionOriginToString(ActionOrigin origin) {
    switch (origin) {
    case ActionOrigin::LocalUser:       return QStringLiteral("LocalUser");
    case ActionOrigin::ScheduledRemote: return QStringLiteral("ScheduledRemote");
    case ActionOrigin::DriftCorrection: return QStringLiteral("DriftCorrection");
    case ActionOrigin::BufferRecovery:  return QStringLiteral("BufferRecovery");
    case ActionOrigin::JoinSync:        return QStringLiteral("JoinSync");
    }
    return QStringLiteral("Unknown");
}

QString readinessToCode(ParticipantReadiness readiness) {
    switch (readiness) {
    case ParticipantReadiness::Idle:      return QStringLiteral("i");
    case ParticipantReadiness::Opening:   return QStringLiteral("o");
    case ParticipantReadiness::Buffering: return QStringLiteral("b");
    case ParticipantReadiness::Seeking:   return QStringLiteral("s");
    case ParticipantReadiness::Ready:     return QStringLiteral("r");
    case ParticipantReadiness::Error:     return QStringLiteral("e");
    }
    return QStringLiteral("i");
}

ParticipantReadiness readinessFromCode(const QString &code) {
    if (code == QLatin1String("o")) return ParticipantReadiness::Opening;
    if (code == QLatin1String("b")) return ParticipantReadiness::Buffering;
    if (code == QLatin1String("s")) return ParticipantReadiness::Seeking;
    if (code == QLatin1String("r")) return ParticipantReadiness::Ready;
    if (code == QLatin1String("e")) return ParticipantReadiness::Error;
    return ParticipantReadiness::Idle;
}

QString readinessToString(ParticipantReadiness readiness) {
    switch (readiness) {
    case ParticipantReadiness::Idle:      return QStringLiteral("Idle");
    case ParticipantReadiness::Opening:   return QStringLiteral("Opening");
    case ParticipantReadiness::Buffering: return QStringLiteral("Buffering");
    case ParticipantReadiness::Seeking:   return QStringLiteral("Seeking");
    case ParticipantReadiness::Ready:     return QStringLiteral("Ready");
    case ParticipantReadiness::Error:     return QStringLiteral("Error");
    }
    return QStringLiteral("Idle");
}

QString driftCorrectionModeToString(DriftCorrectionMode mode) {
    switch (mode) {
    case DriftCorrectionMode::None:                return QStringLiteral("None");
    case DriftCorrectionMode::RateAdjust:          return QStringLiteral("RateAdjust");
    case DriftCorrectionMode::RestoreRate:         return QStringLiteral("RestoreRate");
    case DriftCorrectionMode::HardSeek:            return QStringLiteral("HardSeek");
    case DriftCorrectionMode::BufferRecoverySeek:  return QStringLiteral("BufferRecoverySeek");
    case DriftCorrectionMode::SuppressedBuffering: return QStringLiteral("SuppressedBuffering");
    case DriftCorrectionMode::SuppressedSeeking:   return QStringLiteral("SuppressedSeeking");
    }
    return QStringLiteral("None");
}

// ============================================================================
// AuthoritativeTimeline
// ============================================================================

void AuthoritativeTimeline::reset(const QString &sessionId) {
    m_snapshot = TimelineSnapshot{};
    m_snapshot.sessionId = sessionId;
}

qint64 AuthoritativeTimeline::sanitizePositionMs(qint64 posMs, qint64 durationMs) {
    const qint64 maxAllowed = (durationMs > 0 && durationMs <= kMaxMediaPositionMs)
                                  ? durationMs
                                  : kMaxMediaPositionMs;
    return std::clamp<qint64>(posMs, 0, maxAllowed);
}

double AuthoritativeTimeline::sanitizeRate(double rate) {
    if (!std::isfinite(rate) || rate <= 0.0) {
        return 1.0;
    }
    return std::clamp(rate, kMinTimelineRate, kMaxTimelineRate);
}

qint64 AuthoritativeTimeline::expectedPositionMs(qint64 estimatedHostNowMs) const {
    if (!m_snapshot.valid) {
        return 0;
    }

    const qint64 basePos = sanitizePositionMs(m_snapshot.basePositionMs, m_snapshot.durationMs);
    if (!m_snapshot.playing) {
        return basePos;
    }

    const qint64 elapsedHostMs = std::max<qint64>(0, estimatedHostNowMs - m_snapshot.hostTimestampMs);
    const double rate = sanitizeRate(m_snapshot.playbackRate);
    const qint64 advancedMs = std::llround(static_cast<double>(elapsedHostMs) * rate);
    return sanitizePositionMs(basePos + advancedMs, m_snapshot.durationMs);
}

bool AuthoritativeTimeline::shouldAcceptRevision(qint64 incomingRevision,
                                                 const QString &incomingAuthor,
                                                 const QString &hostPeerId) const {
    if (incomingRevision <= 0) {
        return false;
    }
    if (!m_snapshot.valid) {
        return true;
    }
    if (incomingRevision > m_snapshot.revision) {
        return true;
    }
    if (incomingRevision < m_snapshot.revision) {
        return false;
    }

    // Equal revision: reject if from the same author (duplicate)
    if (incomingAuthor.isEmpty() || incomingAuthor == m_snapshot.authorId) {
        return false;
    }

    // Deterministic tie-breaker for concurrent equal revisions:
    // 1. The room host always wins over a guest.
    if (!hostPeerId.isEmpty()) {
        if (incomingAuthor == hostPeerId && m_snapshot.authorId != hostPeerId) {
            return true;
        }
        if (m_snapshot.authorId == hostPeerId && incomingAuthor != hostPeerId) {
            return false;
        }
    }

    // 2. Otherwise lexicographically higher member ID wins deterministically on all nodes.
    return incomingAuthor > m_snapshot.authorId;
}

bool AuthoritativeTimeline::update(const TimelineSnapshot &next, const QString &hostPeerId) {
    if (!shouldAcceptRevision(next.revision, next.authorId, hostPeerId)) {
        return false;
    }
    forceUpdate(next);
    return true;
}

void AuthoritativeTimeline::forceUpdate(const TimelineSnapshot &next) {
    m_snapshot = next;
    m_snapshot.durationMs = std::clamp<qint64>(m_snapshot.durationMs, 0, kMaxMediaPositionMs);
    m_snapshot.basePositionMs = sanitizePositionMs(m_snapshot.basePositionMs, m_snapshot.durationMs);
    m_snapshot.playbackRate = sanitizeRate(m_snapshot.playbackRate);
    m_snapshot.valid = !m_snapshot.mediaUrl.isEmpty() || !m_snapshot.mediaId.isEmpty();
}

void AuthoritativeTimeline::migrateHostClock(const QString &newHostId,
                                             qint64 oldEstimatedHostNowMs,
                                             qint64 newHostLocalNowMs) {
    if (!m_snapshot.valid) {
        m_snapshot.authorId = newHostId;
        return;
    }

    const qint64 seamlessPos = expectedPositionMs(oldEstimatedHostNowMs);
    m_snapshot.basePositionMs = seamlessPos;
    m_snapshot.hostTimestampMs = newHostLocalNowMs;
    m_snapshot.authorId = newHostId;
    m_snapshot.revision += 1;
}

// ============================================================================
// ClockEstimator
// ============================================================================

void ClockEstimator::reset(bool isSelfAuthority, const QString &authorityPeerId) {
    m_isSelfAuthority = isSelfAuthority;
    m_authorityPeerId = authorityPeerId;
    m_hasEstimate = false;
    m_samples.clear();
    m_pendingPings.clear();
    m_peerRttSamples.clear();
    m_smoothedOffsetMs = 0;
    m_medianRttMs = 0;
    m_minRttMs = 0;
    m_worstRttMs = 0;
    m_jitterMs = 0;
    m_uncertaintyMs = isSelfAuthority ? 0 : 100;
    m_confidence = isSelfAuthority ? 1.0 : 0.0;
    m_rejectedCount = 0;
    m_consecutiveOutliers = 0;
}

void ClockEstimator::softResetForReconnect() {
    m_samples.clear();
    m_pendingPings.clear();
    m_consecutiveOutliers = 0;
    if (!m_isSelfAuthority) {
        m_confidence = m_hasEstimate ? 0.5 : 0.0;
    }
}

void ClockEstimator::setSelfAuthority(bool isSelfAuthority) {
    if (m_isSelfAuthority == isSelfAuthority) {
        return;
    }
    m_isSelfAuthority = isSelfAuthority;
    if (m_isSelfAuthority) {
        m_smoothedOffsetMs = 0;
        m_uncertaintyMs = 0;
        m_confidence = 1.0;
    }
}

void ClockEstimator::setAuthorityPeerId(const QString &peerId) {
    if (m_authorityPeerId == peerId) {
        return;
    }
    const bool wasAuthority = m_isSelfAuthority;
    reset(wasAuthority, peerId);
}

void ClockEstimator::registerOutgoingPing(qint64 t1LocalMs) {
    if (t1LocalMs <= 0) return;
    if (m_pendingPings.size() >= 32) {
        // Evict oldest ping entries if unanswered
        qint64 minPing = *std::min_element(m_pendingPings.begin(), m_pendingPings.end());
        m_pendingPings.remove(minPing);
    }
    m_pendingPings.insert(t1LocalMs);
}

bool ClockEstimator::processSample(qint64 t1, qint64 t2, qint64 t3, qint64 t4) {
    // 1. Basic structural & monotonicity validation
    if (t1 <= 0 || t4 <= 0 || t2 <= 0 || t3 <= 0) {
        ++m_rejectedCount;
        return false;
    }
    if (t4 < t1 || t3 < t2) {
        ++m_rejectedCount;
        return false;
    }

    const qint64 hostTurnaroundMs = t3 - t2;
    if (hostTurnaroundMs > kMaxHostTurnaroundMs) {
        ++m_rejectedCount;
        return false;
    }

    const qint64 rttMs = (t4 - t1) - hostTurnaroundMs;
    if (rttMs < 0 || rttMs > kMaxPlausibleRttMs) {
        ++m_rejectedCount;
        return false;
    }

    // If outgoing pings are being tracked, verify t1 was not already consumed or forged
    if (!m_pendingPings.isEmpty()) {
        if (!m_pendingPings.contains(t1)) {
            ++m_rejectedCount;
            return false;
        }
        m_pendingPings.remove(t1);
    }

    // 2. Outlier spike rejection when a stable window already exists:
    //    Single high-RTT spikes caused by TCP head-of-line blocking should not skew the clock.
    //    However, if 3 consecutive samples show higher RTT, adapt to the new network route.
    if (m_samples.size() >= 4 && m_medianRttMs > 0) {
        const qint64 outlierLimit = std::max<qint64>(600, m_medianRttMs * 4 + 250);
        if (rttMs > outlierLimit) {
            ++m_consecutiveOutliers;
            if (m_consecutiveOutliers < 3) {
                ++m_rejectedCount;
                return false;
            }
        } else {
            m_consecutiveOutliers = 0;
        }
    }

    // 3. Compute 4-timestamp NTP offset:
    //    offset = ((t2 - t1) + (t3 - t4)) / 2
    //    where estimatedHostTime = localTime + offset
    const qint64 offsetMs = std::llround(static_cast<double>((t2 - t1) + (t3 - t4)) / 2.0);

    ClockSample sample;
    sample.t1 = t1;
    sample.t2 = t2;
    sample.t3 = t3;
    sample.t4 = t4;
    sample.rttMs = rttMs;
    sample.offsetMs = offsetMs;
    sample.receivedAtLocalMs = t4;

    m_samples.append(sample);
    while (m_samples.size() > kWindowSize) {
        m_samples.removeFirst();
    }

    recomputeStatistics();
    return true;
}

void ClockEstimator::recordPeerRttSample(qint64 peerRttMs) {
    if (peerRttMs <= 0 || peerRttMs > kMaxPlausibleRttMs) {
        return;
    }
    m_peerRttSamples.append(peerRttMs);
    while (m_peerRttSamples.size() > kWindowSize) {
        m_peerRttSamples.removeFirst();
    }

    if (m_isSelfAuthority && m_samples.isEmpty()) {
        QList<qint64> sorted = m_peerRttSamples;
        std::sort(sorted.begin(), sorted.end());
        const int n = static_cast<int>(sorted.size());
        m_minRttMs = sorted.front();
        m_worstRttMs = sorted.back();
        m_medianRttMs = (n % 2 == 1)
                            ? sorted[n / 2]
                            : (sorted[n / 2 - 1] + sorted[n / 2]) / 2;

        qint64 diffSum = 0;
        for (int i = 1; i < n; ++i) {
            diffSum += std::llabs( m_peerRttSamples[i] - m_peerRttSamples[i - 1] );
        }
        m_jitterMs = (n > 1) ? (diffSum / (n - 1)) : 0;
    }
}

void ClockEstimator::recomputeStatistics() {
    if (m_samples.isEmpty()) {
        return;
    }

    const int n = static_cast<int>(m_samples.size());
    QList<ClockSample> byRtt = m_samples;
    std::sort(byRtt.begin(), byRtt.end(), [](const ClockSample &a, const ClockSample &b) {
        return a.rttMs < b.rttMs;
    });

    m_minRttMs = byRtt.front().rttMs;
    m_worstRttMs = byRtt.back().rttMs;
    m_medianRttMs = (n % 2 == 1)
                        ? byRtt[n / 2].rttMs
                        : (byRtt[n / 2 - 1].rttMs + byRtt[n / 2].rttMs) / 2;

    // Compute inter-sample RTT jitter (mean absolute consecutive RTT delta)
    if (n > 1) {
        qint64 totalDelta = 0;
        for (int i = 1; i < n; ++i) {
            totalDelta += std::llabs(m_samples[i].rttMs - m_samples[i - 1].rttMs);
        }
        m_jitterMs = totalDelta / (n - 1);
    } else {
        m_jitterMs = 0;
    }

    // Prefer lowest-RTT samples for clock-offset estimation:
    // Queueing delay only adds positive delay, so the lowest-RTT half of samples
    // experienced the least queuing asymmetry.
    const int bestCount = std::max(1, (n + 1) / 2);
    QList<qint64> bestOffsets;
    bestOffsets.reserve(bestCount);
    for (int i = 0; i < bestCount; ++i) {
        bestOffsets.append(byRtt[i].offsetMs);
    }
    std::sort(bestOffsets.begin(), bestOffsets.end());

    const qint64 targetOffsetMs = (bestCount % 2 == 1)
                                      ? bestOffsets[bestCount / 2]
                                      : (bestOffsets[bestCount / 2 - 1] + bestOffsets[bestCount / 2]) / 2;

    // Median Absolute Deviation (MAD) of offsets in the low-RTT subset
    qint64 madSum = 0;
    for (qint64 off : bestOffsets) {
        madSum += std::llabs(off - targetOffsetMs);
    }
    const qint64 offsetMadMs = madSum / bestCount;

    // Smooth clock-offset updates so single noisy samples never jump the timeline
    if (!m_hasEstimate) {
        m_smoothedOffsetMs = targetOffsetMs;
        m_hasEstimate = true;
    } else {
        const qint64 delta = targetOffsetMs - m_smoothedOffsetMs;
        if (n <= 2 && std::llabs(delta) > 500) {
            // During initial lock, snap quickly to the low-RTT median
            m_smoothedOffsetMs = targetOffsetMs;
        } else {
            const double alpha = (n < 4) ? 0.50 : 0.30;
            qint64 step = std::llround(static_cast<double>(delta) * alpha);
            // Slew-rate clamp in steady state to prevent sudden jumps
            if (n >= 4) {
                step = std::clamp<qint64>(step, -150, 150);
            }
            if (step == 0 && delta != 0) {
                step = (delta > 0) ? 1 : -1;
            }
            m_smoothedOffsetMs += step;
        }
    }

    // Uncertainty combines minimum one-way delay dispersion, RTT jitter, and offset MAD
    m_uncertaintyMs = std::max<qint64>(5, (m_minRttMs / 4) + m_jitterMs + offsetMadMs);

    // Confidence scales from 0.25 (1 sample) up to 1.0 (>= 4 samples with low jitter)
    const double sampleFactor = std::min(1.0, static_cast<double>(n) / 4.0);
    const double jitterPenalty = std::clamp(1.0 - (static_cast<double>(m_jitterMs) / 300.0), 0.25, 1.0);
    m_confidence = sampleFactor * jitterPenalty;
}

qint64 ClockEstimator::estimatedHostTimeMs(qint64 localNowMs) const {
    if (m_isSelfAuthority || !m_hasEstimate) {
        return localNowMs;
    }
    return localNowMs + m_smoothedOffsetMs;
}

qint64 ClockEstimator::localTimeForHostTimeMs(qint64 hostTimeMs) const {
    if (m_isSelfAuthority || !m_hasEstimate) {
        return hostTimeMs;
    }
    return hostTimeMs - m_smoothedOffsetMs;
}

int ClockEstimator::recommendedPingIntervalMs() const {
    if (m_samples.size() < 4) {
        return 1500; // Initial burst until locked
    }
    if (m_jitterMs > 60 || m_confidence < 0.70) {
        return 3500; // Elevated sampling under high jitter
    }
    return 10000;    // Low steady-state overhead within 150-char chat relay
}

qint64 ClockEstimator::recommendedLeadTimeMs(int activeMemberCount) const {
    if (activeMemberCount <= 1) {
        return 0;
    }

    const qint64 oneWayMs = (m_medianRttMs > 0) ? (m_medianRttMs / 2) : kDefaultOneWayMs;
    const qint64 rawLeadMs = oneWayMs + m_jitterMs + (uncertaintyMs() / 4) + 30;
    return std::clamp<qint64>(rawLeadMs, kMinLeadTimeMs, kMaxLeadTimeMs);
}

// ============================================================================
// ActionScheduler
// ============================================================================

void ActionScheduler::reset() {
    m_pending.clear();
    m_executedOrder.clear();
    m_executedSet.clear();
    m_highestRevision = 0;
    m_highestAuthor.clear();
    m_lateEventCount = 0;
    m_supersededCount = 0;
    m_duplicateCount = 0;
    m_staleDroppedCount = 0;
}

bool ActionScheduler::isEventIdSeen(const QString &eventId) const {
    if (eventId.isEmpty()) return false;
    if (m_executedSet.contains(eventId)) return true;
    for (const ScheduledAction &item : m_pending) {
        if (item.eventId == eventId) return true;
    }
    return false;
}

void ActionScheduler::recordExecutedEventId(const QString &eventId) {
    if (eventId.isEmpty() || m_executedSet.contains(eventId)) return;
    m_executedSet.insert(eventId);
    m_executedOrder.append(eventId);
    while (m_executedOrder.size() > kMaxExecutedHistory) {
        const QString oldest = m_executedOrder.takeFirst();
        m_executedSet.remove(oldest);
    }
}

int ActionScheduler::cancelSuperseded(qint64 upToRevision) {
    int removed = 0;
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (it->revision <= upToRevision) {
            it = m_pending.erase(it);
            ++removed;
            ++m_supersededCount;
        } else {
            ++it;
        }
    }
    return removed;
}

ScheduledAction ActionScheduler::applyLatePolicy(const ScheduledAction &action, qint64 estimatedHostNowMs) {
    ScheduledAction adjusted = action;
    if (action.targetHostTimestampMs <= 0) {
        return adjusted;
    }

    const qint64 lateMs = estimatedHostNowMs - action.targetHostTimestampMs;
    if (lateMs <= 0) {
        return adjusted;
    }

    // For a playing timeline (Play, or Seek/StateSync while playing), project the
    // authoritative media position forward by lateMs * playbackRate so we align to
    // where the timeline actually is right now rather than a stale past position.
    if (action.playing || action.type == ScheduledActionType::Play) {
        const double rate = AuthoritativeTimeline::sanitizeRate(action.playbackRate);
        const qint64 catchUpDelta = std::llround(static_cast<double>(lateMs) * rate);
        adjusted.targetPositionMs = AuthoritativeTimeline::sanitizePositionMs(
            action.targetPositionMs + catchUpDelta);
    }
    return adjusted;
}

ScheduleStatus ActionScheduler::schedule(const ScheduledAction &action,
                                         qint64 estimatedHostNowMs,
                                         qint64 localNowMs,
                                         ScheduledAction &outReadyAction,
                                         const QString &hostPeerId) {
    // 1. Deduplicate by eventId
    if (!action.eventId.isEmpty() && isEventIdSeen(action.eventId)) {
        ++m_duplicateCount;
        return ScheduleStatus::DuplicateIgnored;
    }

    // 2. Reject stale or losing tie-break revisions
    if (action.revision > 0 && m_highestRevision > 0) {
        if (action.revision < m_highestRevision) {
            ++m_staleDroppedCount;
            return ScheduleStatus::StaleDropped;
        }
        if (action.revision == m_highestRevision) {
            if (action.authorId == m_highestAuthor) {
                ++m_duplicateCount;
                return ScheduleStatus::DuplicateIgnored;
            }
            bool incomingWins = false;
            if (!hostPeerId.isEmpty()) {
                if (action.authorId == hostPeerId && m_highestAuthor != hostPeerId) {
                    incomingWins = true;
                } else if (m_highestAuthor == hostPeerId && action.authorId != hostPeerId) {
                    incomingWins = false;
                } else {
                    incomingWins = (action.authorId > m_highestAuthor);
                }
            } else {
                incomingWins = (action.authorId > m_highestAuthor);
            }
            if (!incomingWins) {
                ++m_staleDroppedCount;
                return ScheduleStatus::StaleDropped;
            }
        }
    }

    // 3. Cancel any pending scheduled actions superseded by this revision
    if (action.revision > 0) {
        cancelSuperseded(action.revision);
        m_highestRevision = action.revision;
        m_highestAuthor = action.authorId;
    }

    // 4. Determine whether to execute now (immediate / late catch-up) or queue for the future
    const qint64 waitMs = (action.targetHostTimestampMs > 0)
                              ? (action.targetHostTimestampMs - estimatedHostNowMs)
                              : 0;

    if (waitMs <= 0) {
        recordExecutedEventId(action.eventId);
        outReadyAction = applyLatePolicy(action, estimatedHostNowMs);
        if (action.targetHostTimestampMs > 0 && waitMs < -kLateThresholdMs) {
            ++m_lateEventCount;
            return ScheduleStatus::ExecutedLateCatchUp;
        }
        return ScheduleStatus::ExecutedImmediately;
    }

    // 5. Queue for future execution, clamped to conservative maximum wait
    ScheduledAction queued = action;
    queued.enqueuedLocalMs = localNowMs;
    if (waitMs > kMaxFutureWaitMs) {
        queued.targetHostTimestampMs = estimatedHostNowMs + kMaxFutureWaitMs;
    }

    while (m_pending.size() >= kMaxPendingActions) {
        m_pending.removeFirst();
        ++m_supersededCount;
    }

    m_pending.append(queued);
    std::sort(m_pending.begin(), m_pending.end(), [](const ScheduledAction &a, const ScheduledAction &b) {
        return a.targetHostTimestampMs < b.targetHostTimestampMs;
    });

    return ScheduleStatus::Queued;
}

QList<ScheduledAction> ActionScheduler::popDueActions(qint64 estimatedHostNowMs) {
    QList<ScheduledAction> due;
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (it->targetHostTimestampMs <= estimatedHostNowMs) {
            ScheduledAction ready = applyLatePolicy(*it, estimatedHostNowMs);
            recordExecutedEventId(ready.eventId);
            due.append(ready);
            it = m_pending.erase(it);
        } else {
            ++it;
        }
    }
    return due;
}

qint64 ActionScheduler::nextWakeupDelayMs(qint64 estimatedHostNowMs) const {
    if (m_pending.isEmpty()) {
        return -1;
    }
    const qint64 waitMs = m_pending.front().targetHostTimestampMs - estimatedHostNowMs;
    return std::clamp<qint64>(waitMs, 0, kMaxFutureWaitMs);
}

// ============================================================================
// AdaptiveDriftController
// ============================================================================

AdaptiveDriftController::AdaptiveDriftController()
    : m_config(Config{}) {}

AdaptiveDriftController::AdaptiveDriftController(const Config &config)
    : m_config(config) {}

void AdaptiveDriftController::reset() {
    m_hasDriftSample = false;
    m_lastSignedDriftMs = 0;
    m_smoothedDriftMs = 0.0;
    m_lastMode = DriftCorrectionMode::None;
    m_wasBuffering = false;
    m_pendingBufferRecovery = false;
    m_bufferRecoveredAtMs = 0;
    m_rateAdjustActive = false;
    m_rateAdjustStartedAtMs = 0;
    m_rateAdjustStartDriftMs = 0;
    m_lastRateChangeAtMs = 0;
    m_outsideDeadbandStreak = 0;
    m_rateFallbackTriggered = false;
    m_lastSeekAtMs = 0;
    m_lastSeekTargetMs = -1;
    m_lastPreSeekAbsDriftMs = 0;
    m_softCorrectionCount = 0;
    m_hardSeekCount = 0;
    m_bufferRecoveryCount = 0;
}

void AdaptiveDriftController::notifyExternalSeek(qint64 localNowMs, qint64 targetMs) {
    m_lastSeekAtMs = localNowMs;
    m_lastSeekTargetMs = targetMs;
    m_rateAdjustActive = false;
    m_outsideDeadbandStreak = 0;
    m_hasDriftSample = false;
}

void AdaptiveDriftController::notifyRateControlFailed() {
    m_rateFallbackTriggered = true;
    m_rateAdjustActive = false;
}

DriftCorrectionDecision AdaptiveDriftController::evaluate(const PlayerObservation &obs,
                                                          qint64 expectedPositionMs,
                                                          bool timelinePlaying,
                                                          double timelineRate,
                                                          qint64 clockUncertaintyMs,
                                                          qint64 localNowMs) {
    DriftCorrectionDecision decision;
    decision.expectedPositionMs = expectedPositionMs;
    decision.actualPositionMs = obs.actualPositionMs;
    decision.signedDriftMs = obs.actualPositionMs - expectedPositionMs;
    decision.targetRate = AuthoritativeTimeline::sanitizeRate(timelineRate);

    m_lastSignedDriftMs = decision.signedDriftMs;

    // 1. Suppress drift corrections while media is opening or buffering
    if (obs.isOpening || obs.isBuffering) {
        m_wasBuffering = true;
        m_rateAdjustActive = false;
        m_outsideDeadbandStreak = 0;
        decision.mode = DriftCorrectionMode::SuppressedBuffering;
        // If playback rate was previously altered, restore 1.0x so buffering isn't starved
        if (std::abs(obs.currentRate - decision.targetRate) > 0.001) {
            decision.mode = DriftCorrectionMode::RestoreRate;
        }
        m_lastMode = decision.mode;
        return decision;
    }

    // 2. Suppress drift corrections while a local/remote seek is settling
    const bool inPostSeekGrace = (m_lastSeekAtMs > 0 && (localNowMs - m_lastSeekAtMs) < m_config.postSeekSettleMs);
    if (obs.isSeekSettling || inPostSeekGrace) {
        decision.mode = DriftCorrectionMode::SuppressedSeeking;
        m_lastMode = decision.mode;
        return decision;
    }

    // 3. Detect transition out of buffering and enforce a brief decoder-settle grace window
    if (m_wasBuffering) {
        m_wasBuffering = false;
        m_pendingBufferRecovery = true;
        m_bufferRecoveredAtMs = localNowMs;
        m_hasDriftSample = false;
    }

    if (m_pendingBufferRecovery) {
        if ((localNowMs - m_bufferRecoveredAtMs) < m_config.postBufferSettleMs) {
            decision.mode = DriftCorrectionMode::SuppressedBuffering;
            m_lastMode = decision.mode;
            return decision;
        }

        // Perform a single deliberate post-buffer recovery alignment
        m_pendingBufferRecovery = false;
        decision.origin = ActionOrigin::BufferRecovery;

        if (!timelinePlaying) {
            if (!obs.isPaused) {
                decision.shouldPause = true;
            }
            if (std::llabs(decision.signedDriftMs) > m_config.pausedSeekThresholdMs && obs.isSeekable) {
                decision.mode = DriftCorrectionMode::BufferRecoverySeek;
                decision.targetSeekMs = expectedPositionMs;
                m_lastSeekAtMs = localNowMs;
                m_lastSeekTargetMs = expectedPositionMs;
                m_lastPreSeekAbsDriftMs = std::llabs(decision.signedDriftMs);
                ++m_bufferRecoveryCount;
                ++m_hardSeekCount;
                m_lastMode = decision.mode;
                return decision;
            }
        } else {
            if (obs.isPaused) {
                decision.shouldResume = true;
            }
            if (std::llabs(decision.signedDriftMs) > m_config.hardSeekThresholdMs && obs.isSeekable) {
                decision.mode = DriftCorrectionMode::BufferRecoverySeek;
                decision.targetSeekMs = expectedPositionMs;
                m_lastSeekAtMs = localNowMs;
                m_lastSeekTargetMs = expectedPositionMs;
                m_lastPreSeekAbsDriftMs = std::llabs(decision.signedDriftMs);
                ++m_bufferRecoveryCount;
                ++m_hardSeekCount;
                m_lastMode = decision.mode;
                return decision;
            }
            // If post-buffer drift is moderate (deadband < |drift| <= hardSeekThresholdMs),
            // fall through to Tier 2 rate adjustment so we don't trigger a second re-buffer!
            if (std::llabs(decision.signedDriftMs) > m_config.deadbandMs) {
                ++m_bufferRecoveryCount;
            }
        }
    }

    // 4. Update smoothed drift EMA
    if (!m_hasDriftSample) {
        m_smoothedDriftMs = static_cast<double>(decision.signedDriftMs);
        m_hasDriftSample = true;
    } else {
        m_smoothedDriftMs = 0.65 * static_cast<double>(decision.signedDriftMs) + 0.35 * m_smoothedDriftMs;
    }
    decision.smoothedDriftMs = std::llround(m_smoothedDriftMs);

    // 5. Paused timeline handling
    if (!timelinePlaying) {
        m_rateAdjustActive = false;
        m_outsideDeadbandStreak = 0;

        if (!obs.isPaused && !obs.isEndedOrStopped) {
            decision.shouldPause = true;
        }
        const bool seekCooldownOk = (m_lastSeekAtMs == 0 || (localNowMs - m_lastSeekAtMs) >= m_config.hardSeekCooldownMs);
        if (std::llabs(decision.signedDriftMs) > m_config.pausedSeekThresholdMs && obs.isSeekable && seekCooldownOk) {
            decision.mode = DriftCorrectionMode::HardSeek;
            decision.targetSeekMs = expectedPositionMs;
            m_lastSeekAtMs = localNowMs;
            m_lastSeekTargetMs = expectedPositionMs;
            m_lastPreSeekAbsDriftMs = std::llabs(decision.signedDriftMs);
            ++m_hardSeekCount;
            m_lastMode = decision.mode;
            return decision;
        }
        if (std::abs(obs.currentRate - decision.targetRate) > 0.001) {
            decision.mode = DriftCorrectionMode::RestoreRate;
        } else {
            decision.mode = DriftCorrectionMode::None;
        }
        m_lastMode = decision.mode;
        return decision;
    }

    // 6. Playing timeline: ensure player is running
    if (obs.isEndedOrStopped) {
        decision.shouldRestart = true;
        decision.targetSeekMs = expectedPositionMs;
        decision.mode = DriftCorrectionMode::HardSeek;
        m_lastSeekAtMs = localNowMs;
        ++m_hardSeekCount;
        m_lastMode = decision.mode;
        return decision;
    }
    if (obs.isPaused) {
        decision.shouldResume = true;
    }

    const qint64 absDrift = std::llabs(decision.signedDriftMs);
    const qint64 effectiveDeadbandMs = m_config.deadbandMs + std::clamp<qint64>(clockUncertaintyMs / 4, 0, 40);
    const bool seekCooldownOk = (m_lastSeekAtMs == 0 || (localNowMs - m_lastSeekAtMs) >= m_config.hardSeekCooldownMs);
    const bool canUseRateControl = obs.rateControlSupported && !m_rateFallbackTriggered;

    // 7. Tier 3: Large drift -> Controlled Hard Seek
    if (absDrift > m_config.hardSeekThresholdMs) {
        if (obs.isSeekable && seekCooldownOk) {
            m_rateAdjustActive = false;
            m_outsideDeadbandStreak = 0;
            decision.mode = DriftCorrectionMode::HardSeek;
            decision.targetSeekMs = expectedPositionMs;
            decision.targetRate = AuthoritativeTimeline::sanitizeRate(timelineRate);
            m_lastSeekAtMs = localNowMs;
            m_lastSeekTargetMs = expectedPositionMs;
            m_lastPreSeekAbsDriftMs = absDrift;
            ++m_hardSeekCount;
            m_lastMode = decision.mode;
            return decision;
        }
        // If on hard-seek cooldown (e.g. seek just snapped to a nearby keyframe),
        // allow Tier 2 rate control below to smoothly close the residual offset.
    }

    // Fallback seek when rate control is unsupported or failed convergence watchdog
    if (!canUseRateControl && absDrift > m_config.fallbackSeekThresholdMs) {
        if (obs.isSeekable && seekCooldownOk) {
            m_rateAdjustActive = false;
            decision.mode = DriftCorrectionMode::HardSeek;
            decision.targetSeekMs = expectedPositionMs;
            m_lastSeekAtMs = localNowMs;
            m_lastSeekTargetMs = expectedPositionMs;
            m_lastPreSeekAbsDriftMs = absDrift;
            ++m_hardSeekCount;
            m_lastMode = decision.mode;
            return decision;
        }
    }

    // 8. Tier 1: Deadband & Hysteresis Recovery
    const bool crossedZero = m_rateAdjustActive &&
                             ((decision.signedDriftMs >= 0 && m_rateAdjustStartDriftMs < 0) ||
                              (decision.signedDriftMs <= 0 && m_rateAdjustStartDriftMs > 0));

    if (m_rateAdjustActive && (absDrift <= m_config.recoveryBandMs || crossedZero)) {
        m_rateAdjustActive = false;
        m_outsideDeadbandStreak = 0;
        decision.mode = DriftCorrectionMode::RestoreRate;
        decision.targetRate = AuthoritativeTimeline::sanitizeRate(timelineRate);
        m_lastRateChangeAtMs = localNowMs;
        m_lastMode = decision.mode;
        return decision;
    }

    if (!m_rateAdjustActive && absDrift <= effectiveDeadbandMs) {
        m_outsideDeadbandStreak = 0;
        if (std::abs(obs.currentRate - decision.targetRate) > 0.001) {
            decision.mode = DriftCorrectionMode::RestoreRate;
        } else {
            decision.mode = DriftCorrectionMode::None;
        }
        m_lastMode = decision.mode;
        return decision;
    }

    // 9. Tier 2: Moderate drift -> Gradual Proportional Playback-Rate Adjustment
    if (!canUseRateControl) {
        if (std::abs(obs.currentRate - decision.targetRate) > 0.001) {
            decision.mode = DriftCorrectionMode::RestoreRate;
        } else {
            decision.mode = DriftCorrectionMode::None;
        }
        m_lastMode = decision.mode;
        return decision;
    }

    if (!m_rateAdjustActive) {
        ++m_outsideDeadbandStreak;
        // Require either 2 consecutive ticks outside deadband or a clear moderate drift (> 120 ms)
        if (m_outsideDeadbandStreak < 2 && absDrift < 120) {
            decision.mode = DriftCorrectionMode::None;
            m_lastMode = decision.mode;
            return decision;
        }
        m_rateAdjustActive = true;
        m_rateAdjustStartedAtMs = localNowMs;
        m_rateAdjustStartDriftMs = decision.signedDriftMs;
    } else {
        // Watchdog: if rate adjustment has been active for rateFallbackWindowMs without
        // reducing drift by at least 25 ms, mark rate control ineffective and fall back.
        if ((localNowMs - m_rateAdjustStartedAtMs) >= m_config.rateFallbackWindowMs) {
            if (absDrift >= std::llabs(m_rateAdjustStartDriftMs) - 25) {
                m_rateFallbackTriggered = true;
                m_rateAdjustActive = false;
                if (absDrift >= m_config.fallbackSeekThresholdMs && obs.isSeekable && seekCooldownOk) {
                    decision.mode = DriftCorrectionMode::HardSeek;
                    decision.targetSeekMs = expectedPositionMs;
                    m_lastSeekAtMs = localNowMs;
                    ++m_hardSeekCount;
                    m_lastMode = decision.mode;
                    return decision;
                }
                decision.mode = DriftCorrectionMode::RestoreRate;
                m_lastMode = decision.mode;
                return decision;
            }
            // Progress is being made; slide the watchdog window forward
            m_rateAdjustStartedAtMs = localNowMs;
            m_rateAdjustStartDriftMs = decision.signedDriftMs;
        }
    }

    // Proportional rate law:
    //   signedDriftMs > 0 (local is ahead)  -> deltaRate < 0 (slow down, e.g. 0.97x)
    //   signedDriftMs < 0 (local is behind) -> deltaRate > 0 (speed up, e.g. 1.03x)
    const double baseRate = AuthoritativeTimeline::sanitizeRate(timelineRate);
    const double rawDelta = -static_cast<double>(decision.signedDriftMs) / m_config.rateTimeConstantMs;
    const double computedRate = std::clamp(baseRate + rawDelta, m_config.minRate, m_config.maxRate);

    const bool rateCooldownOk = (m_lastRateChangeAtMs == 0 ||
                                 (localNowMs - m_lastRateChangeAtMs) >= m_config.rateAdjustCooldownMs);

    if (std::abs(computedRate - obs.currentRate) >= m_config.minRateStep && rateCooldownOk) {
        decision.mode = DriftCorrectionMode::RateAdjust;
        decision.targetRate = computedRate;
        m_lastRateChangeAtMs = localNowMs;
        ++m_softCorrectionCount;
    } else {
        // Keep holding current adjusted rate
        decision.mode = DriftCorrectionMode::None;
        decision.targetRate = obs.currentRate;
    }

    m_lastMode = decision.mode;
    return decision;
}

// ============================================================================
// SyncDiagnostics
// ============================================================================

QVariantMap SyncDiagnostics::toVariantMap() const {
    QVariantMap m;
    m.insert(QStringLiteral("smoothedRttMs"), smoothedRttMs);
    m.insert(QStringLiteral("minRttMs"), minRttMs);
    m.insert(QStringLiteral("worstRttMs"), worstRttMs);
    m.insert(QStringLiteral("jitterMs"), jitterMs);
    m.insert(QStringLiteral("clockOffsetMs"), clockOffsetMs);
    m.insert(QStringLiteral("clockUncertaintyMs"), clockUncertaintyMs);
    m.insert(QStringLiteral("clockConfidence"), clockConfidence);
    m.insert(QStringLiteral("clockLocked"), clockLocked);
    m.insert(QStringLiteral("clockSampleCount"), clockSampleCount);
    m.insert(QStringLiteral("rejectedSampleCount"), rejectedSampleCount);
    m.insert(QStringLiteral("scheduledLeadTimeMs"), scheduledLeadTimeMs);
    m.insert(QStringLiteral("pendingScheduledCount"), pendingScheduledCount);
    m.insert(QStringLiteral("currentRevision"), currentRevision);
    m.insert(QStringLiteral("authorityPeerId"), authorityPeerId);
    m.insert(QStringLiteral("localReadiness"), localReadiness);
    m.insert(QStringLiteral("authoritativePositionMs"), authoritativePositionMs);
    m.insert(QStringLiteral("actualPositionMs"), actualPositionMs);
    m.insert(QStringLiteral("signedDriftMs"), signedDriftMs);
    m.insert(QStringLiteral("smoothedDriftMs"), smoothedDriftMs);
    m.insert(QStringLiteral("currentPlaybackRate"), currentPlaybackRate);
    m.insert(QStringLiteral("activeCorrectionMode"), activeCorrectionMode);
    m.insert(QStringLiteral("softCorrectionCount"), softCorrectionCount);
    m.insert(QStringLiteral("hardSeekCorrectionCount"), hardSeekCorrectionCount);
    m.insert(QStringLiteral("bufferRecoveryCount"), bufferRecoveryCount);
    m.insert(QStringLiteral("lateEventCount"), lateEventCount);
    m.insert(QStringLiteral("supersededActionCount"), supersededActionCount);
    m.insert(QStringLiteral("duplicateEventCount"), duplicateEventCount);
    m.insert(QStringLiteral("staleRevisionDropCount"), staleRevisionDropCount);
    return m;
}

} // namespace Flux
