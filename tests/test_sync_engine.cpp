#include <QCoreApplication>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <random>

#include "../src/core/Logger.h"
#include "../src/media/VLCPlayer.h"
#include "../src/teleparty/SyncplayClient.h"
#include "../src/teleparty/TelepartyEngine.h"
#include "../src/teleparty/TelepartySession.h"
#include "../src/teleparty/TelepartySync.h"

using namespace Flux;

namespace {

#define TEST_ASSERT(cond, msg)                                                                   \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            std::cerr << "FAIL [" << __LINE__ << "]: " << (msg) << std::endl;                    \
            return 1;                                                                            \
        }                                                                                        \
    } while (0)

constexpr const char *kAntManUrl =
    "http://172.16.50.14/DHAKA-FLIX-14/English%20Movies%20%281080p%29/%282023%29%201080p/"
    "Ant-Man%20and%20the%20Wasp-Quantumania%20%282023%29%201080p%20DSNP/"
    "Ant-Man%20and%20the%20Wasp%20Quantumania%20%282023%29%201080p%20DSNP-WEB%20x265%20HEVC%2010bit%20AAC%205.1%20MSubs-PSA.mkv";

constexpr const char *kCivilWarUrl =
    "http://172.16.50.14/DHAKA-FLIX-14/English%20Movies%20%281080p%29/%282024%29%201080p/"
    "Civil%20War%20%282024%29%201080p%20%5BDual%20Audio%5D/"
    "Civil%20War%20%282024%29%201080p%20BluRay%20x265%20HEVC%20ESub%20%5BDual%20Audio%5D%5BHindi%205.1%2BEnglish%205.1%5D%20-mkvC.mkv";

// ============================================================================
// Simulated Network Harness for Deterministic Multi-Client Testing
// ============================================================================
struct SimulatedNode {
    QString id;
    std::shared_ptr<SimulatedClock> clock;
    std::unique_ptr<TelepartySession> session;
    std::unique_ptr<VLCPlayer> player;
    std::unique_ptr<TelepartySync> sync;
    bool networkPartitioned = false;
    int outboundActionEvents = 0;
};

struct InFlightPacket {
    qint64 deliverAtSimMs = 0;
    QString senderId;
    QString recipientId;
    QJsonObject payload;
};

class SimulatedNetworkHarness {
public:
    SimulatedNetworkHarness() : m_rng(42) {}
    ~SimulatedNetworkHarness() {
        for (auto &n : m_nodes) {
            if (n && n->session) {
                n->session->setSimulatedTransport(nullptr);
            }
        }
    }

    SimulatedNode *addNode(const QString &memberId, qint64 initialClockMs, bool isHost,
                           const QString &roomCode = QStringLiteral("54321")) {
        auto node = std::make_unique<SimulatedNode>();
        node->id = memberId;
        node->clock = std::make_shared<SimulatedClock>(initialClockMs);
        node->session = std::make_unique<TelepartySession>();
        node->player = std::make_unique<VLCPlayer>();
        node->player->setSimulatedMode(true);
        node->sync = std::make_unique<TelepartySync>(node->session.get(), node->player.get());
        node->sync->setMonotonicClock(node->clock);
        node->sync->setWatching(true);

        SimulatedNode *raw = node.get();
        node->session->setSimulatedTransport([this, raw](const QString &sender, const QJsonObject &msg) {
            return enqueueBroadcast(raw, sender, msg);
        });

        QObject::connect(node->sync.get(), &TelepartySync::remoteOpenRequested,
                         [raw](const QString &url, const QString &, qint64 startMs) {
                             raw->player->setUrl(url);
                             raw->player->setSimulatedState(QStringLiteral("Playing"), startMs, 7200000, false, 100.0f);
                         });

        node->session->simulateJoinRoom(roomCode, isHost, memberId);
        m_nodes.push_back(std::move(node));
        return raw;
    }

    void setOneWayLatencyMs(qint64 hostToGuestMs, qint64 guestToHostMs) {
        m_hostToGuestDelayMs = hostToGuestMs;
        m_guestToHostDelayMs = guestToHostMs;
    }

    void setJitterMs(qint64 maxJitterMs) { m_maxJitterMs = maxJitterMs; }
    void setPacketLossRate(double rate) { m_lossRate = std::clamp(rate, 0.0, 1.0); }
    void setDuplicateRate(double rate) { m_duplicateRate = std::clamp(rate, 0.0, 1.0); }
    void setReorderRate(double rate) { m_reorderRate = std::clamp(rate, 0.0, 1.0); }

    int maxSingleFrameChars() const { return m_maxSingleFrameChars; }
    int totalPacketsSent() const { return m_totalPacketsSent; }

    void stepMs(qint64 deltaMs, qint64 stepGranularityMs = 25) {
        qint64 remaining = deltaMs;
        while (remaining > 0) {
            const qint64 dt = std::min(remaining, stepGranularityMs);
            remaining -= dt;
            m_simTimeMs += dt;

            for (auto &n : m_nodes) {
                n->clock->advanceMs(dt);
                n->player->advanceSimulatedTime(dt);
            }

            deliverDuePackets();

            for (auto &n : m_nodes) {
                n->sync->stepSimulation();
            }

            deliverDuePackets();
        }
    }

private:
    bool enqueueBroadcast(SimulatedNode *senderNode, const QString &senderId, const QJsonObject &msg) {
        if (senderNode->networkPartitioned) {
            return false;
        }

        // Verify compact wire size for normal synchronization messages
        const QString wireFrame = QStringLiteral("FLUX|") +
                                  QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact));
        const QString eventType = msg.value(QStringLiteral("e")).toString();
        const bool isNormalSyncEvent = (eventType == QLatin1String("play") ||
                                        eventType == QLatin1String("pause") ||
                                        eventType == QLatin1String("seek") ||
                                        eventType == QLatin1String("ts") ||
                                        eventType == QLatin1String("rd") ||
                                        eventType == QLatin1String("syncme"));
        if (isNormalSyncEvent) {
            m_maxSingleFrameChars = std::max(m_maxSingleFrameChars, static_cast<int>(wireFrame.size()));
        }
        if (eventType == QLatin1String("play") ||
            eventType == QLatin1String("pause") ||
            eventType == QLatin1String("seek") ||
            eventType == QLatin1String("open")) {
            senderNode->outboundActionEvents++;
        }

        ++m_totalPacketsSent;

        std::uniform_real_distribution<double> uni(0.0, 1.0);
        std::uniform_int_distribution<qint64> jitterDist(-m_maxJitterMs, m_maxJitterMs);

        for (auto &dest : m_nodes) {
            if (dest->id == senderId || dest->networkPartitioned) continue;

            // Never drop initial room discovery ("hello" / "presence") unless testing loss after join
            const QString msgType = msg.value(QStringLiteral("t")).toString();
            const bool isDiscovery = (msgType == QLatin1String("hello") || msgType == QLatin1String("presence"));
            if (!isDiscovery && m_lossRate > 0.0 && uni(m_rng) < m_lossRate) {
                continue;
            }

            const bool senderIsHost = senderNode->session->isHost();
            const qint64 baseDelay = senderIsHost ? m_hostToGuestDelayMs : m_guestToHostDelayMs;
            const qint64 jitter = (m_maxJitterMs > 0) ? jitterDist(m_rng) : 0;
            qint64 delay = std::max<qint64>(1, baseDelay + jitter);

            if (!isDiscovery && m_reorderRate > 0.0 && uni(m_rng) < m_reorderRate) {
                delay += 120; // Delay packet so subsequent packet overtakes it
            }

            InFlightPacket pkt;
            pkt.deliverAtSimMs = m_simTimeMs + delay;
            pkt.senderId = senderId;
            pkt.recipientId = dest->id;
            pkt.payload = msg;
            m_inFlight.append(pkt);

            if (!isDiscovery && m_duplicateRate > 0.0 && uni(m_rng) < m_duplicateRate) {
                InFlightPacket dup = pkt;
                dup.deliverAtSimMs += 15;
                m_inFlight.append(dup);
            }
        }
        return true;
    }

    void deliverDuePackets() {
        std::sort(m_inFlight.begin(), m_inFlight.end(), [](const InFlightPacket &a, const InFlightPacket &b) {
            return a.deliverAtSimMs < b.deliverAtSimMs;
        });

        while (!m_inFlight.isEmpty() && m_inFlight.front().deliverAtSimMs <= m_simTimeMs) {
            const InFlightPacket pkt = m_inFlight.takeFirst();
            for (auto &dest : m_nodes) {
                if (dest->id == pkt.recipientId && !dest->networkPartitioned) {
                    dest->session->simulateReceiveMessage(pkt.senderId, pkt.payload);
                }
            }
        }
    }

    std::vector<std::unique_ptr<SimulatedNode>> m_nodes;
    QList<InFlightPacket> m_inFlight;
    qint64 m_simTimeMs = 0;
    qint64 m_hostToGuestDelayMs = 50;
    qint64 m_guestToHostDelayMs = 50;
    qint64 m_maxJitterMs = 0;
    double m_lossRate = 0.0;
    double m_duplicateRate = 0.0;
    double m_reorderRate = 0.0;
    int m_maxSingleFrameChars = 0;
    int m_totalPacketsSent = 0;
    std::mt19937 m_rng;
};

} // namespace

int main(int argc, char *argv[]) {
    QGuiApplication app(argc, argv);

    std::cout << "=== FLUX Adaptive Watch-Party Synchronization Engine Test Suite ===" << std::endl;

    // =========================================================================
    // Test 1: Clock-Offset & RTT Estimator (Symmetric, Asymmetric, Jitter, Outliers, Reset)
    // =========================================================================
    {
        std::cout << "\n[Test 1] Clock-offset & RTT estimation (4-timestamp NTP protocol)..." << std::endl;

        // 1a: Symmetric latency (True offset = +75,000 ms, one-way = 60 ms, RTT = 120 ms)
        {
            ClockEstimator est;
            est.reset(false, QStringLiteral("FX_host0001"));

            const qint64 trueOffsetMs = 75000; // HostTime = LocalTime + 75000
            const qint64 oneWayMs = 60;
            for (int i = 0; i < 6; ++i) {
                const qint64 t1 = 10000 + i * 1500;
                const qint64 t2 = t1 + trueOffsetMs + oneWayMs;
                const qint64 t3 = t2 + 2; // 2ms turnaround on host
                const qint64 t4 = t1 + 2 * oneWayMs + 2;
                est.registerOutgoingPing(t1);
                TEST_ASSERT(est.processSample(t1, t2, t3, t4), "Symmetric sample should be accepted");
            }

            TEST_ASSERT(est.isLocked(), "Estimator should be locked after 6 samples");
            TEST_ASSERT(est.clockOffsetMs() == trueOffsetMs, "Symmetric offset should converge to +75000 ms");
            TEST_ASSERT(est.medianRttMs() == 120, "Median RTT should be 120 ms");
            TEST_ASSERT(est.minRttMs() == 120, "Min RTT should be 120 ms");
            TEST_ASSERT(est.jitterMs() == 0, "Jitter on constant-delay link should be 0 ms");
            TEST_ASSERT(est.estimatedHostTimeMs(20000) == 95000, "estimatedHostTimeMs conversion must match");
            TEST_ASSERT(est.localTimeForHostTimeMs(95000) == 20000, "localTimeForHostTimeMs conversion must match");
            TEST_ASSERT(est.recommendedPingIntervalMs() == 10000, "Locked low-jitter link should use 10s ping interval");
        }

        // 1b: Asymmetric latency (uplink = 40 ms, downlink = 160 ms, RTT = 200 ms, true offset = -30,000 ms)
        {
            ClockEstimator est;
            est.reset(false, QStringLiteral("FX_host0001"));

            const qint64 trueOffsetMs = -30000; // Host clock is 30s behind local clock
            const qint64 uplinkMs = 40;
            const qint64 downlinkMs = 160;
            for (int i = 0; i < 6; ++i) {
                const qint64 t1 = 50000 + i * 1500;
                const qint64 t2 = (t1 + uplinkMs) + trueOffsetMs;
                const qint64 t3 = t2 + 1;
                const qint64 t4 = (t3 - trueOffsetMs) + downlinkMs;
                est.registerOutgoingPing(t1);
                TEST_ASSERT(est.processSample(t1, t2, t3, t4), "Asymmetric sample should be accepted");
            }

            // Theoretical NTP asymmetry bias = (uplink - downlink) / 2 = (40 - 160) / 2 = -60 ms
            const qint64 expectedAsymOffset = trueOffsetMs + (uplinkMs - downlinkMs) / 2;
            TEST_ASSERT(est.medianRttMs() == 200, "Asymmetric RTT should be 200 ms");
            TEST_ASSERT(std::llabs(est.clockOffsetMs() - expectedAsymOffset) <= 2,
                        "Asymmetric offset should match theoretical NTP midpoint");
            TEST_ASSERT(std::llabs(est.clockOffsetMs() - trueOffsetMs) <= est.medianRttMs() / 2,
                        "Asymmetric offset error must be strictly bounded by RTT/2");
        }

        // 1c: High queuing jitter + preference for low-RTT samples
        {
            ClockEstimator est;
            est.reset(false, QStringLiteral("FX_host0001"));

            const qint64 trueOffsetMs = 50000;
            // Alternate between clean low-RTT packets (oneWay=40/40) and congested downlink spikes (oneWay=40/280)
            const qint64 downlinks[] = {40, 280, 42, 320, 40, 260, 44, 300};
            for (int i = 0; i < 8; ++i) {
                const qint64 t1 = 10000 + i * 1000;
                const qint64 up = 40;
                const qint64 down = downlinks[i];
                const qint64 t2 = t1 + up + trueOffsetMs;
                const qint64 t3 = t2 + 1;
                const qint64 t4 = t1 + up + down + 1;
                est.registerOutgoingPing(t1);
                TEST_ASSERT(est.processSample(t1, t2, t3, t4), "Jitter sample should be accepted");
            }

            // Because ClockEstimator filters for the lowest-RTT half of the window, the congested
            // 260..320ms downlink spikes are excluded from the target offset median!
            TEST_ASSERT(std::llabs(est.clockOffsetMs() - trueOffsetMs) <= 15,
                        "Low-RTT sample preference must keep clock offset within 15ms of true offset under heavy jitter");
            TEST_ASSERT(est.jitterMs() > 100, "Jitter metric must reflect high RTT variance");
            TEST_ASSERT(est.recommendedPingIntervalMs() == 3500, "High-jitter link should use 3500ms ping interval");
        }

        // 1d: Malformed, duplicate, and extreme outlier spike rejection
        {
            ClockEstimator est;
            est.reset(false, QStringLiteral("FX_host0001"));

            // Populate 4 stable samples at RTT = 80ms, offset = +10000ms
            for (int i = 0; i < 4; ++i) {
                const qint64 t1 = 1000 + i * 1000;
                est.registerOutgoingPing(t1);
                TEST_ASSERT(est.processSample(t1, t1 + 10040, t1 + 10041, t1 + 81), "Baseline sample valid");
            }
            const qint64 baselineOffset = est.clockOffsetMs();

            // Negative RTT (t4 < t1)
            est.registerOutgoingPing(6000);
            TEST_ASSERT(!est.processSample(6000, 16040, 16041, 5900), "Negative elapsed local time must be rejected");

            // Non-monotonic host timestamps (t3 < t2)
            est.registerOutgoingPing(7000);
            TEST_ASSERT(!est.processSample(7000, 17040, 17030, 7080), "t3 < t2 must be rejected");

            // Host stalled turnaround (t3 - t2 > 2000ms)
            est.registerOutgoingPing(8000);
            TEST_ASSERT(!est.processSample(8000, 18040, 21000, 11080), "Excessive host turnaround must be rejected");

            // Unsolicited / duplicate t1 pong
            TEST_ASSERT(!est.processSample(1000, 11040, 11041, 1081), "Duplicate pong for already-consumed t1 must be rejected");

            // Single transient TCP stall spike (RTT = 1800ms on an 80ms link)
            est.registerOutgoingPing(9000);
            TEST_ASSERT(!est.processSample(9000, 19040, 19041, 10801), "Single extreme RTT spike must be rejected");
            TEST_ASSERT(est.clockOffsetMs() == baselineOffset, "Rejected samples must not alter clock offset");
            TEST_ASSERT(est.rejectedCount() == 5, "Rejected sample counter must equal 5");

            // Reset on reconnect
            est.reset(false, QStringLiteral("FX_host0002"));
            TEST_ASSERT(!est.hasEstimate() && est.sampleCount() == 0 && est.recommendedPingIntervalMs() == 1500,
                        "Reset must clear state and return to initial burst interval");
        }

        std::cout << "PASS: ClockEstimator verified across symmetric, asymmetric, high-jitter, outlier, and reset cases." << std::endl;
    }

    // =========================================================================
    // Test 2: Future Action Scheduler & Late-Event Policy
    // =========================================================================
    {
        std::cout << "\n[Test 2] Future action scheduling, clock-update recalculation, late policy, and superseding..." << std::endl;

        ActionScheduler sched;

        // 2a: Schedule future Play action (200ms in the future)
        ScheduledAction playAct;
        playAct.revision = 1;
        playAct.eventId = QStringLiteral("FX_host:1");
        playAct.authorId = QStringLiteral("FX_host");
        playAct.type = ScheduledActionType::Play;
        playAct.targetPositionMs = 30000;
        playAct.targetHostTimestampMs = 50200;
        playAct.playbackRate = 1.0;
        playAct.playing = true;

        ScheduledAction ready;
        ScheduleStatus st = sched.schedule(playAct, /*estimatedHostNowMs=*/50000, /*localNowMs=*/10000, ready);
        TEST_ASSERT(st == ScheduleStatus::Queued, "Future action must be queued");
        TEST_ASSERT(sched.pendingCount() == 1, "Pending count must be 1");
        TEST_ASSERT(sched.nextWakeupDelayMs(50000) == 200, "Wakeup delay must be 200ms");

        // 2b: Clock-offset improvement shifts estimatedHostNowMs from 50000 -> 50080 (+80ms)
        TEST_ASSERT(sched.nextWakeupDelayMs(50080) == 120,
                    "Refined clock offset must immediately recalculate remaining delay to 120ms");

        // Pop at 50200 -> executes on time at 30000ms
        QList<ScheduledAction> due = sched.popDueActions(50200);
        TEST_ASSERT(due.size() == 1 && due.front().targetPositionMs == 30000,
                    "Scheduled play must fire at targetHostTimestampMs with exact position");

        // 2c: Late-event policy: Play anchored at ht=60000, pos=45000 arrives at estHostNow=60320 (320ms late)
        ScheduledAction latePlay;
        latePlay.revision = 2;
        latePlay.eventId = QStringLiteral("FX_host:2");
        latePlay.authorId = QStringLiteral("FX_host");
        latePlay.type = ScheduledActionType::Play;
        latePlay.targetPositionMs = 45000;
        latePlay.targetHostTimestampMs = 60000;
        latePlay.playbackRate = 1.0;
        latePlay.playing = true;

        st = sched.schedule(latePlay, /*estimatedHostNowMs=*/60320, /*localNowMs=*/20320, ready);
        TEST_ASSERT(st == ScheduleStatus::ExecutedLateCatchUp, "Late play must trigger ExecutedLateCatchUp");
        TEST_ASSERT(ready.targetPositionMs == 45320,
                    "Late play policy must project targetPositionMs forward by elapsed 320ms to 45320ms");
        TEST_ASSERT(sched.lateEventCount() == 1, "Late event counter must increment");

        // 2d: Superseded revision cancellation
        ScheduledAction queuedPlay;
        queuedPlay.revision = 3;
        queuedPlay.eventId = QStringLiteral("FX_host:3");
        queuedPlay.authorId = QStringLiteral("FX_host");
        queuedPlay.type = ScheduledActionType::Play;
        queuedPlay.targetPositionMs = 50000;
        queuedPlay.targetHostTimestampMs = 70500;
        queuedPlay.playing = true;
        TEST_ASSERT(sched.schedule(queuedPlay, 70000, 30000, ready) == ScheduleStatus::Queued, "Rev 3 queued");

        ScheduledAction newerPause;
        newerPause.revision = 4;
        newerPause.eventId = QStringLiteral("FX_host:4");
        newerPause.authorId = QStringLiteral("FX_host");
        newerPause.type = ScheduledActionType::Pause;
        newerPause.targetPositionMs = 50100;
        newerPause.targetHostTimestampMs = 70100;
        newerPause.playing = false;
        TEST_ASSERT(sched.schedule(newerPause, 70100, 30100, ready) == ScheduleStatus::ExecutedImmediately,
                    "Newer Pause rev 4 executes immediately");
        TEST_ASSERT(sched.pendingCount() == 0 && sched.supersededCount() == 1,
                    "Older queued Play rev 3 must be cancelled when rev 4 arrives");

        // 2e: Duplicate and out-of-order stale revision rejection
        TEST_ASSERT(sched.schedule(newerPause, 70150, 30150, ready) == ScheduleStatus::DuplicateIgnored,
                    "Duplicate eventId must be ignored");
        ScheduledAction staleSeek = newerPause;
        staleSeek.revision = 3;
        staleSeek.eventId = QStringLiteral("FX_host:99");
        TEST_ASSERT(sched.schedule(staleSeek, 70150, 30150, ready) == ScheduleStatus::StaleDropped,
                    "Out-of-order older revision (rev 3 < rev 4) must be dropped");
        TEST_ASSERT(sched.duplicateCount() == 1 && sched.staleDroppedCount() == 1,
                    "Duplicate and stale counters must be accurate");

        std::cout << "PASS: ActionScheduler verified for future queueing, clock-update recalculation, late catch-up, and superseding." << std::endl;
    }

    // =========================================================================
    // Test 3: Three-Tier Adaptive Drift Controller
    // =========================================================================
    {
        std::cout << "\n[Test 3] Three-tier adaptive drift controller (deadband, rate nudge, hysteresis, hard seek, fallback)..." << std::endl;

        // 3a: Deadband (|drift| <= 80ms -> None)
        {
            AdaptiveDriftController ctrl;
            PlayerObservation obs;
            obs.actualPositionMs = 10045;
            obs.durationMs = 7200000;
            obs.isPlaying = true;
            obs.currentRate = 1.0;

            DriftCorrectionDecision d1 = ctrl.evaluate(obs, /*expected=*/10000, true, 1.0, /*uncertainty=*/10, /*now=*/1000);
            TEST_ASSERT(d1.mode == DriftCorrectionMode::None && d1.signedDriftMs == 45,
                        "Drift of +45ms within deadband must produce DriftCorrectionMode::None");

            obs.actualPositionMs = 9935;
            DriftCorrectionDecision d2 = ctrl.evaluate(obs, /*expected=*/10000, true, 1.0, /*uncertainty=*/10, /*now=*/1500);
            TEST_ASSERT(d2.mode == DriftCorrectionMode::None && d2.signedDriftMs == -65,
                        "Drift of -65ms within deadband must produce DriftCorrectionMode::None");
            TEST_ASSERT(ctrl.softCorrectionCount() == 0 && ctrl.hardSeekCount() == 0,
                        "Zero corrections should be counted inside deadband");
        }

        // 3b: Moderate negative & positive drift -> gradual rate adjustment & hysteresis recovery to 1.0x
        {
            AdaptiveDriftController ctrl;
            VLCPlayer simPlayer;
            simPlayer.setSimulatedMode(true);
            simPlayer.setSimulatedState(QStringLiteral("Playing"), /*timeMs=*/9700, 7200000, false, 100.0f);

            qint64 simClockMs = 10000;
            qint64 hostTimelinePosMs = 10000; // Player starts 300ms BEHIND timeline (-300ms drift)

            bool sawRateAboveOne = false;
            bool restoredToOne = false;

            for (int tick = 0; tick < 30; ++tick) {
                PlayerObservation obs;
                obs.actualPositionMs = simPlayer.timeMs();
                obs.durationMs = simPlayer.durationMs();
                obs.isPlaying = simPlayer.isPlaying();
                obs.currentRate = simPlayer.playbackRate();
                obs.rateControlSupported = true;

                DriftCorrectionDecision dec = ctrl.evaluate(obs, hostTimelinePosMs, true, 1.0, 10, simClockMs);
                if (dec.mode == DriftCorrectionMode::RateAdjust) {
                    TEST_ASSERT(dec.targetRate > 1.0 && dec.targetRate <= 1.04,
                                "Negative drift (-300ms behind) must command bounded speedup rate in (1.0, 1.04]");
                    simPlayer.remoteSetRate(dec.targetRate);
                    sawRateAboveOne = true;
                } else if (dec.mode == DriftCorrectionMode::RestoreRate) {
                    simPlayer.remoteSetRate(dec.targetRate);
                    restoredToOne = true;
                    break;
                }

                simClockMs += 500;
                hostTimelinePosMs += 500;
                simPlayer.advanceSimulatedTime(500);
            }

            TEST_ASSERT(sawRateAboveOne, "Controller must engage >1.0x playback rate for -300ms drift");
            TEST_ASSERT(restoredToOne && std::abs(simPlayer.playbackRate() - 1.0) < 0.001,
                        "Controller must restore playback rate to 1.0x once drift enters recovery hysteresis band");
            TEST_ASSERT(std::llabs(simPlayer.timeMs() - hostTimelinePosMs) <= 35,
                        "Player position must converge within <= 35ms recovery band");
            TEST_ASSERT(ctrl.hardSeekCount() == 0, "Moderate drift must converge without any hard seeks");

            // Now test moderate POSITIVE drift (+260ms ahead of timeline -> slow down < 1.0x)
            simPlayer.setSimulatedState(QStringLiteral("Playing"), hostTimelinePosMs + 260, 7200000, false, 100.0f);
            bool sawRateBelowOne = false;
            restoredToOne = false;

            for (int tick = 0; tick < 30; ++tick) {
                PlayerObservation obs;
                obs.actualPositionMs = simPlayer.timeMs();
                obs.durationMs = simPlayer.durationMs();
                obs.isPlaying = simPlayer.isPlaying();
                obs.currentRate = simPlayer.playbackRate();
                obs.rateControlSupported = true;

                DriftCorrectionDecision dec = ctrl.evaluate(obs, hostTimelinePosMs, true, 1.0, 10, simClockMs);
                if (dec.mode == DriftCorrectionMode::RateAdjust) {
                    TEST_ASSERT(dec.targetRate < 1.0 && dec.targetRate >= 0.96,
                                "Positive drift (+260ms ahead) must command bounded slowdown rate in [0.96, 1.0)");
                    simPlayer.remoteSetRate(dec.targetRate);
                    sawRateBelowOne = true;
                } else if (dec.mode == DriftCorrectionMode::RestoreRate) {
                    simPlayer.remoteSetRate(dec.targetRate);
                    restoredToOne = true;
                    break;
                }

                simClockMs += 500;
                hostTimelinePosMs += 500;
                simPlayer.advanceSimulatedTime(500);
            }

            TEST_ASSERT(sawRateBelowOne && restoredToOne,
                        "Positive moderate drift must slow down (< 1.0x) and restore to 1.0x upon convergence");
        }

        // 3c: Large drift (> 650ms) -> Hard seek + cooldown + keyframe-snap residual convergence via rate control
        {
            AdaptiveDriftController ctrl;
            VLCPlayer simPlayer;
            simPlayer.setSimulatedMode(true);
            // Simulate coarse 2000ms keyframes (e.g. seeking to 20,350ms snaps to 20,000ms)
            simPlayer.setSimulatedKeyframeStepMs(2000);
            simPlayer.setSimulatedState(QStringLiteral("Playing"), /*timeMs=*/18000, 7200000, false, 100.0f);

            qint64 simClockMs = 50000;
            qint64 hostTimelinePosMs = 20350; // Drift = -2350ms (Large drift!)

            PlayerObservation obs;
            obs.actualPositionMs = simPlayer.timeMs();
            obs.durationMs = simPlayer.durationMs();
            obs.isPlaying = true;
            obs.isSeekable = true;
            obs.currentRate = 1.0;

            DriftCorrectionDecision dec = ctrl.evaluate(obs, hostTimelinePosMs, true, 1.0, 10, simClockMs);
            TEST_ASSERT(dec.mode == DriftCorrectionMode::HardSeek && dec.targetSeekMs == 20350,
                        "Drift of -2350ms must trigger HardSeek to 20350ms");
            simPlayer.remoteSeekTo(dec.targetSeekMs);
            // Because of 2000ms keyframe granularity, simPlayer snapped to 20000ms (-350ms residual drift)
            TEST_ASSERT(simPlayer.timeMs() == 20000, "Simulated keyframe snap should land at 20000ms");

            // Step past the 800ms post-seek settle guard and let Tier 2 rate control close the 350ms keyframe gap!
            for (int tick = 0; tick < 30; ++tick) {
                simClockMs += 500;
                hostTimelinePosMs += 500;
                simPlayer.advanceSimulatedTime(500);

                obs.actualPositionMs = simPlayer.timeMs();
                obs.isSeekSettling = simPlayer.isSeekSettling();
                obs.currentRate = simPlayer.playbackRate();

                dec = ctrl.evaluate(obs, hostTimelinePosMs, true, 1.0, 10, simClockMs);
                if (dec.mode == DriftCorrectionMode::RateAdjust || dec.mode == DriftCorrectionMode::RestoreRate) {
                    simPlayer.remoteSetRate(dec.targetRate);
                } else if (dec.mode == DriftCorrectionMode::HardSeek) {
                    simPlayer.remoteSeekTo(dec.targetSeekMs);
                }
            }

            TEST_ASSERT(ctrl.hardSeekCount() == 1,
                        "Controller must NOT thrash with repeated seeks to the same keyframe; hardSeekCount must stay 1");
            TEST_ASSERT(std::llabs(simPlayer.timeMs() - hostTimelinePosMs) <= 40,
                        "Tier 2 rate control must smoothly eliminate the residual keyframe offset");
        }

        // 3d: Rate control unsupported -> automatic fallback to seek
        {
            TelepartySession session;
            VLCPlayer player;
            player.setSimulatedMode(true);
            player.setSimulatedRateSupported(false); // libVLC rejects set_rate

            auto clock = std::make_shared<SimulatedClock>(10000);
            TelepartySync sync(&session, &player);
            sync.setMonotonicClock(clock);
            sync.setWatching(true);

            session.simulateJoinRoom(QStringLiteral("11111"), false, QStringLiteral("FX_guest01"));
            player.setUrl(QString::fromLatin1(kAntManUrl));
            player.setSimulatedState(QStringLiteral("Playing"), 10000, 7200000, false, 100.0f);

            // Receive authoritative state at 10420ms (-420ms moderate drift)
            QVariantMap stateData;
            stateData.insert(QStringLiteral("id"), QStringLiteral("ant-man"));
            stateData.insert(QStringLiteral("t"), 10420);
            stateData.insert(QStringLiteral("d"), 7200000);
            stateData.insert(QStringLiteral("p"), 1);
            stateData.insert(QStringLiteral("rev"), 1);
            stateData.insert(QStringLiteral("ht"), 10000);
            QJsonObject msg;
            msg.insert(QStringLiteral("v"), 1);
            msg.insert(QStringLiteral("t"), QStringLiteral("event"));
            msg.insert(QStringLiteral("f"), QStringLiteral("FX_host001"));
            msg.insert(QStringLiteral("i"), 1);
            msg.insert(QStringLiteral("e"), QStringLiteral("state"));
            msg.insert(QStringLiteral("d"), QJsonObject::fromVariantMap(stateData));
            session.simulateReceiveMessage(QStringLiteral("FX_host001"), msg);

            // First drift tick attempts RateAdjust -> player rejects -> fallback activates -> next tick seeks!
            sync.evaluateDriftTick();
            TEST_ASSERT(sync.driftController().isRateFallbackActive(),
                        "Rejected remoteSetRate must activate rate-control fallback");
            clock->advanceMs(500);
            player.advanceSimulatedTime(500);
            sync.evaluateDriftTick();
            TEST_ASSERT(sync.driftController().hardSeekCount() >= 1,
                        "Fallback mode must align player using controlled seek when rate adjustment is unsupported");
        }

        std::cout << "PASS: Three-tier AdaptiveDriftController verified (deadband, rate nudge, hysteresis, keyframe seek, fallback)." << std::endl;
    }

    // =========================================================================
    // Test 4: Buffering & Seek-Guard Suppression + Single Post-Buffer Recovery
    // =========================================================================
    {
        std::cout << "\n[Test 4] Buffering/opening/seek-guard suppression and post-buffer recovery alignment..." << std::endl;

        AdaptiveDriftController ctrl;
        PlayerObservation obs;
        obs.actualPositionMs = 10000;
        obs.durationMs = 7200000;
        obs.isPlaying = false;
        obs.isOpening = true;
        obs.isBuffering = true;
        obs.currentRate = 1.03; // Previously nudged rate

        // 4a: While Opening/Buffering, drift is -2500ms, yet seeks must be suppressed and rate reset to 1.0x
        DriftCorrectionDecision dec = ctrl.evaluate(obs, /*expected=*/12500, true, 1.0, 10, /*now=*/20000);
        TEST_ASSERT(dec.mode == DriftCorrectionMode::RestoreRate && std::abs(dec.targetRate - 1.0) < 0.001,
                    "Entering buffering with non-1.0 rate must immediately restore rate to 1.0x");
        TEST_ASSERT(ctrl.hardSeekCount() == 0, "Must never seek while Opening/Buffering");

        obs.currentRate = 1.0;
        obs.isOpening = false;
        obs.isBuffering = true;
        dec = ctrl.evaluate(obs, /*expected=*/13000, true, 1.0, 10, /*now=*/20500);
        TEST_ASSERT(dec.mode == DriftCorrectionMode::SuppressedBuffering && ctrl.hardSeekCount() == 0,
                    "Must return SuppressedBuffering while buffering");

        // 4b: Buffer finishes at t=21000, but player is still within postBufferSettleMs (300ms)
        obs.isBuffering = false;
        obs.isPlaying = true;
        dec = ctrl.evaluate(obs, /*expected=*/13500, true, 1.0, 10, /*now=*/21100);
        TEST_ASSERT(dec.mode == DriftCorrectionMode::SuppressedBuffering && ctrl.hardSeekCount() == 0,
                    "Must wait for postBufferSettleMs before issuing post-buffer recovery");

        // At t=21400 (>= 300ms after buffer recovery), perform exactly ONE BufferRecoverySeek
        dec = ctrl.evaluate(obs, /*expected=*/13800, true, 1.0, 10, /*now=*/21400);
        TEST_ASSERT(dec.mode == DriftCorrectionMode::BufferRecoverySeek &&
                        dec.origin == ActionOrigin::BufferRecovery &&
                        dec.targetSeekMs == 13800 &&
                        ctrl.bufferRecoveryCount() == 1,
                    "Must perform exactly one BufferRecoverySeek once buffer settles");

        // 4c: While seek is settling (isSeekSettling == true), further corrections are suppressed
        obs.actualPositionMs = 13800;
        obs.isSeekSettling = true;
        dec = ctrl.evaluate(obs, /*expected=*/14000, true, 1.0, 10, /*now=*/21600);
        TEST_ASSERT(dec.mode == DriftCorrectionMode::SuppressedSeeking,
                    "Must suppress drift corrections while seek guard is active");

        std::cout << "PASS: Buffering/seeking suppression and post-buffer recovery alignment verified." << std::endl;
    }

    // =========================================================================
    // Test 5: Feedback-Loop Prevention & Local Seek Debouncing
    // =========================================================================
    {
        std::cout << "\n[Test 5] Feedback-loop prevention and local seek debouncing..." << std::endl;

        SimulatedNetworkHarness net;
        SimulatedNode *host = net.addNode(QStringLiteral("FX_10000001"), 50000, true);
        SimulatedNode *guest = net.addNode(QStringLiteral("FX_20000002"), 10000, false);
        net.stepMs(500); // Complete room join & initial clock sync

        guest->outboundActionEvents = 0;

        // Host opens movie, pauses, resumes, seeks, and Guest performs drift corrections & buffer recovery
        host->player->setUrl(QString::fromLatin1(kAntManUrl));
        host->player->setSimulatedState(QStringLiteral("Playing"), 0, 7200000, false, 100.0f);
        host->sync->broadcastOpen(QString::fromLatin1(kAntManUrl), QStringLiteral("Ant-Man"));
        net.stepMs(1000);

        host->player->simulateUserTogglePlay(false);
        net.stepMs(1000);

        host->player->simulateUserSeek(60000);
        net.stepMs(1000);

        host->player->simulateUserTogglePlay(true);
        net.stepMs(1000);

        // Induce moderate drift (-300ms) and large drift (-1500ms) on Guest
        guest->player->setSimulatedState(QStringLiteral("Playing"), guest->player->timeMs() - 300, 7200000, false, 100.0f);
        net.stepMs(2000);
        guest->player->setSimulatedState(QStringLiteral("Playing"), guest->player->timeMs() - 1500, 7200000, false, 100.0f);
        net.stepMs(2000);

        TEST_ASSERT(guest->outboundActionEvents == 0,
                    "Guest must emit ZERO outbound play/pause/seek/open events during remote actions and drift corrections");

        // Rapid local user seek coalescing on Host (20 rapid slider drags before timer flushes)
        const int beforeHostEvents = host->outboundActionEvents;
        for (int i = 1; i <= 20; ++i) {
            host->player->simulateUserSeek(60000 + i * 1000);
        }
        QCoreApplication::processEvents();
        // Wait for the 40ms seek debounce timer
        QElapsedTimer waitTimer;
        waitTimer.start();
        while (waitTimer.elapsed() < 80) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        }
        const int seekEventsSent = host->outboundActionEvents - beforeHostEvents;
        TEST_ASSERT(seekEventsSent == 1,
                    "20 rapid local seeks within debounce window must coalesce into exactly 1 outbound seek event");

        std::cout << "PASS: Zero feedback loops and 20->1 seek debouncing verified." << std::endl;
    }

    // =========================================================================
    // Test 6: Multi-Client Simulated Network Lifecycle (Join-in-Progress, Media Switch,
    //         Asymmetric Jitter/Loss/Duplication/Reorder, Outage Reconnect, Host Migration)
    // =========================================================================
    {
        std::cout << "\n[Test 6] End-to-end multi-client simulation under adverse network conditions & host migration..." << std::endl;

        SimulatedNetworkHarness net;
        // Configure asymmetric latency (Host->Guest = 70ms, Guest->Host = 170ms -> RTT = 240ms)
        net.setOneWayLatencyMs(70, 170);

        SimulatedNode *host = net.addNode(QStringLiteral("FX_10000001"), /*clock=*/100000, true, QStringLiteral("77777"));
        host->player->setUrl(QString::fromLatin1(kAntManUrl));
        host->player->setSimulatedState(QStringLiteral("Playing"), /*timeMs=*/45000, 7200000, false, 100.0f);
        host->sync->adoptCurrent(QString::fromLatin1(kAntManUrl), QStringLiteral("Ant-Man"));

        // Advance Host for 2 seconds -> Host is at 47,000ms
        net.stepMs(2000);
        const qint64 hostPosBeforeJoin = host->player->timeMs();
        TEST_ASSERT(hostPosBeforeJoin == 47000, "Host should be at 47000ms before guest joins");

        // 6a: Guest 1 joins in progress while Host is playing
        SimulatedNode *guest1 = net.addNode(QStringLiteral("FX_20000002"), /*clock=*/20000, false, QStringLiteral("77777"));
        net.stepMs(1500); // Discovery + clock sync + state announcement

        TEST_ASSERT(guest1->sync->hasPartyMedia() && guest1->sync->currentUrl() == QLatin1String(kAntManUrl),
                    "Guest 1 must learn the active party movie from Host's state");
        TEST_ASSERT(host->player->isPlaying() && host->player->timeMs() >= hostPosBeforeJoin + 1400,
                    "Joining guest must NEVER stall or reset the Host's playback");

        // Guest 1 clicks Join Party Playback -> buffers for 1000ms while Host keeps playing -> recovers and converges!
        guest1->sync->joinPartyPlayback();
        guest1->player->setSimulatedState(QStringLiteral("Buffering 50%"), guest1->player->timeMs(), 7200000, true, 50.0f);
        net.stepMs(1000);
        guest1->player->setSimulatedState(QStringLiteral("Playing"), guest1->player->timeMs(), 7200000, false, 100.0f);
        net.stepMs(5000);

        const qint64 joinDrift = std::llabs(guest1->player->timeMs() - host->player->timeMs());
        TEST_ASSERT(joinDrift <= 80,
                    QString("Guest 1 must converge within deadband after join-in-progress (drift=%1ms)")
                        .arg(joinDrift)
                        .toStdString());

        // 6b: Enable adverse network conditions (jitter +/-40ms, 15% packet duplication, 15% reordering)
        //     and switch media mid-session to Civil War
        net.setJitterMs(40);
        net.setDuplicateRate(0.15);
        net.setReorderRate(0.15);

        host->player->setUrl(QString::fromLatin1(kCivilWarUrl));
        host->player->setSimulatedState(QStringLiteral("Playing"), 0, 6500000, false, 100.0f);
        host->sync->broadcastOpen(QString::fromLatin1(kCivilWarUrl), QStringLiteral("Civil War"));
        net.stepMs(4000);

        TEST_ASSERT(guest1->sync->currentUrl() == QLatin1String(kCivilWarUrl) &&
                        guest1->player->url() == QLatin1String(kCivilWarUrl),
                    "Guest 1 must switch to new movie Civil War cleanly");
        TEST_ASSERT(std::llabs(guest1->player->timeMs() - host->player->timeMs()) <= 80,
                    "Host and Guest 1 must remain synchronized on the switched movie");

        // 6c: Temporary network outage on Guest 1 (disconnect -> Host seeks to 120,000ms -> Guest 1 reconnects)
        net.setDuplicateRate(0.0);
        net.setReorderRate(0.0);
        guest1->networkPartitioned = true;
        guest1->session->simulateDisconnect();
        TEST_ASSERT(guest1->session->state() == QLatin1String("reconnecting"),
                    "Guest 1 must enter reconnecting state during outage");

        host->player->simulateUserSeek(120000);
        net.stepMs(2000); // Host plays from 120,000 -> 122,000ms while Guest 1 is offline

        guest1->networkPartitioned = false;
        guest1->session->simulateReconnect();
        net.stepMs(4000); // Reconnect resync + post-seek settle

        TEST_ASSERT(guest1->session->state() == QLatin1String("joined"), "Guest 1 must return to joined state");
        TEST_ASSERT(std::llabs(guest1->player->timeMs() - host->player->timeMs()) <= 80,
                    "Guest 1 must resynchronize to Host's new timeline after reconnect");

        // 6d: Add Guest 2, then disconnect original Host (FX_10000001) -> verify deterministic Host Migration!
        SimulatedNode *guest2 = net.addNode(QStringLiteral("FX_30000003"), /*clock=*/5000, false, QStringLiteral("77777"));
        net.stepMs(1500);
        guest2->sync->joinPartyPlayback();
        net.stepMs(3000);

        const qint64 posBeforeHostDrop = guest1->player->timeMs();
        host->networkPartitioned = true;
        guest1->session->simulatePeerLeft(QStringLiteral("FX_10000001"));
        guest2->session->simulatePeerLeft(QStringLiteral("FX_10000001"));
        net.stepMs(2000);

        TEST_ASSERT(guest1->session->isHost() && guest1->session->hostMemberId() == QLatin1String("FX_20000002"),
                    "FX_20000002 must deterministically promote to Host when FX_10000001 disconnects");
        TEST_ASSERT(!guest2->session->isHost() && guest2->session->hostMemberId() == QLatin1String("FX_20000002"),
                    "FX_30000003 must deterministically recognize FX_20000002 as the new Host");
        TEST_ASSERT(guest1->player->timeMs() >= posBeforeHostDrop + 1900,
                    "Host migration must re-anchor timeline seamlessly without jumping or pausing playback");
        TEST_ASSERT(std::llabs(guest2->player->timeMs() - guest1->player->timeMs()) <= 80,
                    "Remaining participants must stay synchronized under the new Host");

        // Verify 150-char Syncplay wire constraint across all simulated messages
        TEST_ASSERT(net.maxSingleFrameChars() <= SyncplayClient::kDefaultMaxChatLength,
                    QString("All normal sync wire frames must be <= 150 chars (max=%1)")
                        .arg(net.maxSingleFrameChars())
                        .toStdString());

        const SyncDiagnostics diag = guest1->sync->diagnostics();
        TEST_ASSERT(!diag.toVariantMap().isEmpty() && diag.currentRevision > 0,
                    "SyncDiagnostics must expose structured observability metrics");

        std::cout << "PASS: Multi-client simulation verified (join-in-progress, media switch, jitter/dup/reorder, "
                     "outage reconnect, host migration, max single-frame wire length="
                  << net.maxSingleFrameChars() << " <= 150 chars)." << std::endl;
    }

    std::cout << "\n=== ALL ADAPTIVE SYNCHRONIZATION ENGINE TESTS PASSED ===" << std::endl;
    return 0;
}
