#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QTemporaryDir>
#include <QVariantMap>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
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
// Synthetic Binary Container Generators (MP4 ISOBMFF & MKV EBML)
// ============================================================================

QByteArray makeDeterministicEssence(quint32 seed, int byteCount = 360 * 1024) {
    QByteArray buf(byteCount, Qt::Uninitialized);
    quint64 state = (static_cast<quint64>(seed) << 32) ^ 0x9E3779B97F4A7C15ULL;
    uchar *out = reinterpret_cast<uchar *>(buf.data());
    for (int i = 0; i < byteCount; ++i) {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        const quint64 val = state * 2685821657736338717ULL;
        out[i] = static_cast<uchar>((val >> ((i & 7) * 8)) & 0xFF);
    }
    return buf;
}

void appendBe32(QByteArray &buf, quint32 v) {
    uchar tmp[4];
    qToBigEndian<quint32>(v, tmp);
    buf.append(reinterpret_cast<const char *>(tmp), 4);
}

void appendBe64(QByteArray &buf, quint64 v) {
    uchar tmp[8];
    qToBigEndian<quint64>(v, tmp);
    buf.append(reinterpret_cast<const char *>(tmp), 8);
}

QByteArray makeMp4Box(const char type[4], const QByteArray &payload) {
    QByteArray box;
    appendBe32(box, static_cast<quint32>(8 + payload.size()));
    box.append(type, 4);
    box.append(payload);
    return box;
}

bool writeSyntheticMp4(const QString &filePath,
                       qint64 durationMs,
                       const QByteArray &essencePayload,
                       int width = 1920,
                       int height = 1080,
                       const char codecFourCC[4] = "avc1") {
    QDir().mkpath(QFileInfo(filePath).absolutePath());
    QFile f(filePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }

    // 1. ftyp box
    QByteArray ftypPayload;
    ftypPayload.append("isom", 4);
    appendBe32(ftypPayload, 0x00000200U);
    ftypPayload.append("mp41", 4);
    const QByteArray ftypBox = makeMp4Box("ftyp", ftypPayload);

    // 2. moov -> mvhd + trak(tkhd + mdia(minf(stbl(stsd))))
    QByteArray mvhdPayload(28, '\0');
    if (durationMs > 0) {
        qToBigEndian<quint32>(1000U, reinterpret_cast<uchar *>(mvhdPayload.data() + 12)); // timescale = 1000 Hz
        qToBigEndian<quint32>(static_cast<quint32>(durationMs),
                              reinterpret_cast<uchar *>(mvhdPayload.data() + 16));
    }
    const QByteArray mvhdBox = makeMp4Box("mvhd", mvhdPayload);

    QByteArray tkhdPayload(84, '\0');
    qToBigEndian<quint32>(static_cast<quint32>(width) << 16,
                          reinterpret_cast<uchar *>(tkhdPayload.data() + 76));
    qToBigEndian<quint32>(static_cast<quint32>(height) << 16,
                          reinterpret_cast<uchar *>(tkhdPayload.data() + 80));
    const QByteArray tkhdBox = makeMp4Box("tkhd", tkhdPayload);

    QByteArray stsdPayload(16, '\0');
    qToBigEndian<quint32>(1U, reinterpret_cast<uchar *>(stsdPayload.data() + 4));  // entryCount = 1
    qToBigEndian<quint32>(16U, reinterpret_cast<uchar *>(stsdPayload.data() + 8)); // sampleEntrySize
    std::memcpy(stsdPayload.data() + 12, codecFourCC, 4);
    const QByteArray stsdBox = makeMp4Box("stsd", stsdPayload);
    const QByteArray stblBox = makeMp4Box("stbl", stsdBox);
    const QByteArray minfBox = makeMp4Box("minf", stblBox);
    const QByteArray mdiaBox = makeMp4Box("mdia", minfBox);
    const QByteArray trakBox = makeMp4Box("trak", tkhdBox + mdiaBox);
    const QByteArray moovBox = makeMp4Box("moov", mvhdBox + trakBox);

    // 3. mdat box containing the media essence payload
    const QByteArray mdatBox = makeMp4Box("mdat", essencePayload);

    f.write(ftypBox);
    f.write(moovBox);
    f.write(mdatBox);
    f.close();
    return true;
}

bool writeSyntheticMkv(const QString &filePath,
                       qint64 durationMs,
                       const QByteArray &essencePayload,
                       const QByteArray &videoCodecId = QByteArrayLiteral("V_MPEGH/ISO/HEVC"),
                       const QByteArray &audioCodecId = QByteArrayLiteral("A_EAC3")) {
    QDir().mkpath(QFileInfo(filePath).absolutePath());
    QFile f(filePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }

    QByteArray mkv;
    // 1. EBML Header (0x1A45DFA3)
    mkv.append("\x1A\x45\xDF\xA3\x88matroska", 13);

    // 2. Segment Header (0x18538067) with unknown/streaming VINT size
    mkv.append("\x18\x53\x80\x67\x01\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 12);

    // 3. SegmentInfo (0x1549A966)
    QByteArray segInfoPayload;
    // TimestampScale (0x2AD7B1), size = 3 (0x83), value = 1,000,000 ns (1 ms = 0x0F4240)
    segInfoPayload.append("\x2A\xD7\xB1\x83\x0F\x42\x40", 7);
    if (durationMs > 0) {
        // Duration (0x4489), size = 8 (0x88), IEEE-754 double
        segInfoPayload.append("\x44\x89\x88", 3);
        const double durFloat = static_cast<double>(durationMs);
        quint64 durBits = 0;
        std::memcpy(&durBits, &durFloat, sizeof(double));
        appendBe64(segInfoPayload, durBits);
    }
    mkv.append("\x15\x49\xA9\x66", 4);
    mkv.append(static_cast<char>(0x80 | (segInfoPayload.size() & 0x7F)));
    mkv.append(segInfoPayload);

    // 4. Tracks metadata stub containing Matroska CodecID strings
    mkv.append("\x16\x54\xAE\x6B", 4);
    const QByteArray tracksStub = videoCodecId + QByteArrayLiteral(" ") + audioCodecId;
    mkv.append(static_cast<char>(0x80 | (tracksStub.size() & 0x7F)));
    mkv.append(tracksStub);

    // 5. First Cluster (0x1F43B675) with 4-byte VINT size (0x10000000 | size) + essencePayload
    mkv.append("\x1F\x43\xB6\x75", 4);
    const quint32 clusterVint = 0x10000000U | (static_cast<quint32>(essencePayload.size()) & 0x0FFFFFFFU);
    appendBe32(mkv, clusterVint);
    mkv.append(essencePayload);

    f.write(mkv);
    f.close();
    return true;
}

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
    bool autoOpenOnRemoteRequest = true;
    int outboundActionEvents = 0;
    QStringList outboundEventTypes;
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
                             if (!raw->autoOpenOnRemoteRequest) return;
                             const qint64 dur = raw->sync->hostLocalDescriptor().durationMs > 0
                                                    ? raw->sync->hostLocalDescriptor().durationMs
                                                    : 7200000;
                             raw->player->setUrl(url);
                             raw->player->setSimulatedState(QStringLiteral("Playing"), startMs, dur, false, 100.0f);
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
    int privacyLeaksDetected() const { return m_privacyLeaksDetected; }
    const QList<QJsonObject> &sentPayloads() const { return m_sentPayloads; }

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

        m_sentPayloads.append(msg);
        if (LocalMediaDescriptor::containsPrivatePathLeakInJson(msg)) {
            ++m_privacyLeaksDetected;
        }

        // Verify compact wire size for normal synchronization messages
        const QString wireFrame = QStringLiteral("FLUX|") +
                                  QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact));
        const QString eventType = msg.value(QStringLiteral("e")).toString();
        if (!eventType.isEmpty()) {
            senderNode->outboundEventTypes.append(eventType);
        }
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
    QList<QJsonObject> m_sentPayloads;
    qint64 m_simTimeMs = 0;
    qint64 m_hostToGuestDelayMs = 50;
    qint64 m_guestToHostDelayMs = 50;
    qint64 m_maxJitterMs = 0;
    double m_lossRate = 0.0;
    double m_duplicateRate = 0.0;
    double m_reorderRate = 0.0;
    int m_maxSingleFrameChars = 0;
    int m_totalPacketsSent = 0;
    int m_privacyLeaksDetected = 0;
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

    // =========================================================================
    // Test 7: Local Media Inspection, Binary Container Parsing (MP4/MKV),
    //         Content Fingerprinting, Compatibility Verification & Privacy Guard
    // =========================================================================
    {
        std::cout << "\n[Test 7] Local media inspection, binary MP4/MKV parsing, fingerprinting, compatibility verification & privacy guard..." << std::endl;

        QTemporaryDir tempDir;
        TEST_ASSERT(tempDir.isValid(), "QTemporaryDir must be valid");
        LocalMediaInspector::clearCache();

        const QByteArray essenceInterstellar = makeDeterministicEssence(1001, 380 * 1024);
        const QByteArray essenceInception    = makeDeterministicEssence(2002, 380 * 1024);
        const QByteArray essenceEpisode1     = makeDeterministicEssence(3003, 320 * 1024);
        const QByteArray essenceEpisode2     = makeDeterministicEssence(4004, 320 * 1024);

        // 7a: Two identical local files at completely different paths ("HostDrive/Movies/Interstellar.mkv"
        //     vs "GuestDrive/Downloads/Interstellar.mkv") -> MatchedExact
        const QString hostExactPath  = tempDir.filePath(QStringLiteral("HostDrive/Movies/Interstellar.mkv"));
        const QString guestExactPath = tempDir.filePath(QStringLiteral("GuestDrive/Downloads/Interstellar.mkv"));
        TEST_ASSERT(writeSyntheticMkv(hostExactPath, 10140000, essenceInterstellar), "Write hostExactPath MKV");
        TEST_ASSERT(writeSyntheticMkv(guestExactPath, 10140000, essenceInterstellar), "Write guestExactPath MKV");

        LocalCompatibilityReason inspectErr = LocalCompatibilityReason::None;
        LocalMediaDescriptor hostExactDesc  = LocalMediaInspector::inspectFile(hostExactPath, QString(), 0, false, &inspectErr);
        TEST_ASSERT(inspectErr == LocalCompatibilityReason::None && hostExactDesc.durationMs == 10140000,
                    "MKV binary EBML parser must extract exact 10,140,000ms duration without external hint");
        TEST_ASSERT(hostExactDesc.containerFormat == QLatin1String("MKV") &&
                        hostExactDesc.videoCodec == QLatin1String("HEVC") &&
                        hostExactDesc.audioCodec == QLatin1String("EAC3"),
                    "MKV binary parser must detect container and codecs");
        hostExactDesc.sessionId = QStringLiteral("88888");
        hostExactDesc.revision  = 1;
        hostExactDesc.mediaId   = LocalMediaDescriptor::generateSessionMediaId(QStringLiteral("88888"), 1, hostExactDesc);

        const LocalMediaDescriptor guestExactDesc = LocalMediaInspector::inspectFile(guestExactPath, QString(), 0, false, &inspectErr);
        const LocalCompatibilityResult resExact = LocalMediaVerifier::verify(hostExactDesc, guestExactDesc);
        TEST_ASSERT(resExact.isCompatible() &&
                        resExact.state == LocalVerificationState::VerifiedCompatible &&
                        resExact.reason == LocalCompatibilityReason::MatchedExact &&
                        resExact.exactContentMatch &&
                        resExact.durationDeltaMs == 0,
                    "Identical local files at different paths must verify as MatchedExact");

        // 7b: Same movie with different filenames & different containers ("Interstellar.mkv" vs
        //     "Interstellar_1080p.mp4") sharing the same media essence and duration within tolerance
        const QString guestRemuxMp4Path = tempDir.filePath(QStringLiteral("GuestLibrary/SciFi/Interstellar_1080p.mp4"));
        TEST_ASSERT(writeSyntheticMp4(guestRemuxMp4Path, 10141400, essenceInterstellar, 1920, 1080, "hvc1"),
                    "Write guestRemuxMp4Path MP4 with +1400ms container duration delta");

        const LocalMediaDescriptor guestRemuxDesc = LocalMediaInspector::inspectFile(guestRemuxMp4Path, QString(), 0, false, &inspectErr);
        TEST_ASSERT(guestRemuxDesc.durationMs == 10141400 &&
                        guestRemuxDesc.containerFormat == QLatin1String("MP4") &&
                        guestRemuxDesc.width == 1920 &&
                        guestRemuxDesc.height == 1080,
                    "MP4 binary ISOBMFF parser must extract mvhd duration (10,141,400ms) and tkhd 1920x1080 dimensions");
        TEST_ASSERT(guestRemuxDesc.fileSizeBytes != hostExactDesc.fileSizeBytes,
                    "Remuxed MKV vs MP4 must have different total file sizes");

        const LocalCompatibilityResult resRemux = LocalMediaVerifier::verify(hostExactDesc, guestRemuxDesc);
        TEST_ASSERT(resRemux.isCompatible() &&
                        resRemux.state == LocalVerificationState::VerifiedCompatible &&
                        resRemux.reason == LocalCompatibilityReason::MatchedRemuxOrReencode &&
                        resRemux.durationDeltaMs == 1400,
                    "Same movie in different containers/filenames with duration delta 1400ms <= 2000ms must verify as MatchedRemuxOrReencode");

        // 7c: Different movies ("Interstellar.mkv" vs "Inception.mkv") -> TitleMismatch
        const QString inceptionPath = tempDir.filePath(QStringLiteral("GuestLibrary/Inception.2010.1080p.BluRay.x265.mkv"));
        TEST_ASSERT(writeSyntheticMkv(inceptionPath, 8880000, essenceInception), "Write Inception MKV");
        const LocalMediaDescriptor inceptionDesc = LocalMediaInspector::inspectFile(inceptionPath);
        const LocalCompatibilityResult resDiffMovie = LocalMediaVerifier::verify(hostExactDesc, inceptionDesc);
        TEST_ASSERT(!resDiffMovie.isCompatible() &&
                        resDiffMovie.state == LocalVerificationState::IncompatibleMedia &&
                        resDiffMovie.reason == LocalCompatibilityReason::TitleMismatch,
                    "Different movies (Interstellar vs Inception) must fail with TitleMismatch");

        // 7d: Different episodes ("Severance.S01E01.1080p.mkv" vs "Severance.S01E02.1080p.mkv") -> EpisodeMismatch
        const QString ep1Path = tempDir.filePath(QStringLiteral("Series/Severance.S01E01.1080p.WEB-DL.mkv"));
        const QString ep2Path = tempDir.filePath(QStringLiteral("Series/Severance.S01E02.1080p.WEB-DL.mkv"));
        TEST_ASSERT(writeSyntheticMkv(ep1Path, 3300000, essenceEpisode1), "Write S01E01 MKV");
        TEST_ASSERT(writeSyntheticMkv(ep2Path, 3300000, essenceEpisode2), "Write S01E02 MKV");
        LocalMediaDescriptor ep1Desc = LocalMediaInspector::inspectFile(ep1Path);
        ep1Desc.sessionId = QStringLiteral("88888");
        ep1Desc.revision  = 2;
        ep1Desc.mediaId   = LocalMediaDescriptor::generateSessionMediaId(QStringLiteral("88888"), 2, ep1Desc);
        const LocalMediaDescriptor ep2Desc = LocalMediaInspector::inspectFile(ep2Path);
        TEST_ASSERT(ep1Desc.episodeTag == QLatin1String("s01e01") && ep2Desc.episodeTag == QLatin1String("s01e02"),
                    "Episode tags s01e01 and s01e02 must be extracted accurately");
        const LocalCompatibilityResult resEpMismatch = LocalMediaVerifier::verify(ep1Desc, ep2Desc);
        TEST_ASSERT(!resEpMismatch.isCompatible() &&
                        resEpMismatch.reason == LocalCompatibilityReason::EpisodeMismatch,
                    "Different episodes of the same series must fail with EpisodeMismatch");

        // 7e: Different cuts/editions ("Dune.2021.Extended.Cut.mkv" vs "Dune.2021.Theatrical.Cut.mkv") -> EditionMismatch
        const QString extCutPath = tempDir.filePath(QStringLiteral("Editions/Dune.2021.Extended.Cut.1080p.mkv"));
        const QString thCutPath  = tempDir.filePath(QStringLiteral("Editions/Dune.2021.Theatrical.Cut.1080p.mkv"));
        TEST_ASSERT(writeSyntheticMkv(extCutPath, 9300000, essenceEpisode1), "Write Extended Cut MKV");
        TEST_ASSERT(writeSyntheticMkv(thCutPath, 9300000, essenceEpisode2), "Write Theatrical Cut MKV");
        LocalMediaDescriptor extCutDesc = LocalMediaInspector::inspectFile(extCutPath);
        extCutDesc.sessionId = QStringLiteral("88888");
        extCutDesc.revision  = 3;
        extCutDesc.mediaId   = LocalMediaDescriptor::generateSessionMediaId(QStringLiteral("88888"), 3, extCutDesc);
        const LocalMediaDescriptor thCutDesc = LocalMediaInspector::inspectFile(thCutPath);
        const LocalCompatibilityResult resEdition = LocalMediaVerifier::verify(extCutDesc, thCutDesc);
        TEST_ASSERT(!resEdition.isCompatible() &&
                        resEdition.reason == LocalCompatibilityReason::EditionMismatch,
                    "Extended Cut vs Theatrical Cut must fail with EditionMismatch");

        // 7f: Same title with incompatible duration (> 2000ms difference) -> DurationMismatch
        const QString badDurPath = tempDir.filePath(QStringLiteral("GuestLibrary/Interstellar.2014.1080p.mkv"));
        TEST_ASSERT(writeSyntheticMkv(badDurPath, 10140000 - 45000, essenceInception), "Write truncated duration MKV");
        const LocalMediaDescriptor badDurDesc = LocalMediaInspector::inspectFile(badDurPath);
        const LocalCompatibilityResult resDurMismatch = LocalMediaVerifier::verify(hostExactDesc, badDurDesc);
        TEST_ASSERT(!resDurMismatch.isCompatible() &&
                        resDurMismatch.reason == LocalCompatibilityReason::DurationMismatch &&
                        resDurMismatch.durationDeltaMs == 45000,
                    "Same title with 45s duration difference must fail with DurationMismatch");

        // 7g: Same filename and same duration (120,000ms), but DIFFERENT content essence -> FingerprintMismatch
        const QString sampleA = tempDir.filePath(QStringLiteral("CompareA/SampleMovie.mkv"));
        const QString sampleB = tempDir.filePath(QStringLiteral("CompareB/SampleMovie.mkv"));
        TEST_ASSERT(writeSyntheticMkv(sampleA, 120000, essenceEpisode1), "Write SampleMovie A (120s)");
        TEST_ASSERT(writeSyntheticMkv(sampleB, 120000, essenceEpisode2), "Write SampleMovie B (120s, different content)");
        LocalMediaDescriptor descSampleA = LocalMediaInspector::inspectFile(sampleA);
        descSampleA.sessionId = QStringLiteral("88888");
        descSampleA.revision  = 4;
        descSampleA.mediaId   = LocalMediaDescriptor::generateSessionMediaId(QStringLiteral("88888"), 4, descSampleA);
        const LocalMediaDescriptor descSampleB = LocalMediaInspector::inspectFile(sampleB);
        const LocalCompatibilityResult resSameNameDiffContent = LocalMediaVerifier::verify(descSampleA, descSampleB);
        TEST_ASSERT(!resSameNameDiffContent.isCompatible() &&
                        resSameNameDiffContent.reason == LocalCompatibilityReason::FingerprintMismatch,
                    "Two files with identical filename and identical 120,000ms duration but different content must fail with FingerprintMismatch");

        // 7h: Missing/unknown duration (0ms in container and 0ms knownDurationMs) -> UnresolvedMetadata (never silently accepted)
        const QString noDurPath = tempDir.filePath(QStringLiteral("Broken/MysteryClip.mkv"));
        TEST_ASSERT(writeSyntheticMkv(noDurPath, /*durationMs=*/0, essenceEpisode1), "Write 0-duration MKV");
        LocalCompatibilityReason noDurErr = LocalCompatibilityReason::None;
        const LocalMediaDescriptor noDurDesc = LocalMediaInspector::inspectFile(noDurPath, QString(), 0, false, &noDurErr);
        TEST_ASSERT(noDurErr == LocalCompatibilityReason::InsufficientMetadata && noDurDesc.durationMs == 0,
                    "File with unresolved duration (0ms) must report InsufficientMetadata");
        const LocalCompatibilityResult resNoDur = LocalMediaVerifier::verify(hostExactDesc, noDurDesc);
        TEST_ASSERT(!resNoDur.isCompatible() &&
                        resNoDur.state == LocalVerificationState::UnresolvedMetadata &&
                        resNoDur.reason == LocalCompatibilityReason::InsufficientMetadata,
                    "Unresolved duration must fail with UnresolvedMetadata and never be silently accepted");

        // 7i: Missing file, empty file, and unsupported container extension
        LocalCompatibilityReason errMissing = LocalCompatibilityReason::None;
        LocalMediaInspector::inspectFile(tempDir.filePath(QStringLiteral("NonExistent.mkv")), QString(), 0, false, &errMissing);
        TEST_ASSERT(errMissing == LocalCompatibilityReason::MissingFile, "Non-existent file must report MissingFile");

        const QString emptyFilePath = tempDir.filePath(QStringLiteral("EmptyFile.mp4"));
        {
            QFile ef(emptyFilePath);
            TEST_ASSERT(ef.open(QIODevice::WriteOnly | QIODevice::Truncate), "Create empty file");
            ef.close();
        }
        LocalCompatibilityReason errEmpty = LocalCompatibilityReason::None;
        LocalMediaInspector::inspectFile(emptyFilePath, QString(), 0, false, &errEmpty);
        TEST_ASSERT(errEmpty == LocalCompatibilityReason::UnreadableOrEmptyFile, "0-byte file must report UnreadableOrEmptyFile");

        const QString unsupportedPath = tempDir.filePath(QStringLiteral("Notes.txt"));
        {
            QFile uf(unsupportedPath);
            TEST_ASSERT(uf.open(QIODevice::WriteOnly | QIODevice::Truncate), "Create txt file");
            uf.write("not a video container");
            uf.close();
        }
        LocalCompatibilityReason errUnsup = LocalCompatibilityReason::None;
        LocalMediaInspector::inspectFile(unsupportedPath, QString(), 0, false, &errUnsup);
        TEST_ASSERT(errUnsup == LocalCompatibilityReason::UnsupportedContainer, ".txt file must report UnsupportedContainer");

        // 7j: Cache invalidation when a local file is modified/replaced on disk + async worker inspection
        const QString mutablePath = tempDir.filePath(QStringLiteral("Mutable/Clip.mp4"));
        TEST_ASSERT(writeSyntheticMp4(mutablePath, 60000, essenceEpisode1), "Write initial Mutable/Clip.mp4");
        const LocalMediaDescriptor cachedBefore = LocalMediaInspector::inspectFile(mutablePath);
        TEST_ASSERT(!LocalMediaInspector::hasFileChangedSince(mutablePath, cachedBefore),
                    "Unmodified file must not report changed");

        // Replace file with larger payload and different content
        const QByteArray largerEssence = makeDeterministicEssence(9999, 440 * 1024);
        TEST_ASSERT(writeSyntheticMp4(mutablePath, 60000, largerEssence), "Overwrite Mutable/Clip.mp4 with modified content");
        TEST_ASSERT(LocalMediaInspector::hasFileChangedSince(mutablePath, cachedBefore),
                    "hasFileChangedSince must detect modified local file on disk");
        const LocalMediaDescriptor cachedAfter = LocalMediaInspector::inspectFile(mutablePath);
        TEST_ASSERT(cachedAfter.contentFingerprint != cachedBefore.contentFingerprint &&
                        cachedAfter.fileSizeBytes != cachedBefore.fileSizeBytes,
                    "Inspector cache must invalidate and recompute fingerprint when file size/mtime changes");

        // Async worker inspection check
        bool asyncDone = false;
        LocalMediaDescriptor asyncDesc;
        LocalMediaInspector::inspectFileAsync(
            &app,
            hostExactPath,
            QStringLiteral("Interstellar"),
            0,
            false,
            [&](LocalMediaDescriptor desc, LocalCompatibilityReason err) {
                if (err == LocalCompatibilityReason::None) {
                    asyncDesc = std::move(desc);
                    asyncDone = true;
                }
            });
        QElapsedTimer asyncWait;
        asyncWait.start();
        while (!asyncDone && asyncWait.elapsed() < 2000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        }
        TEST_ASSERT(asyncDone && asyncDesc.contentFingerprint == hostExactDesc.contentFingerprint,
                    "Asynchronous worker inspection must produce identical descriptor to synchronous inspection");

        // 7k: Privacy leak guard (reject Windows drive letters, UNC paths, backslashes, file:// URLs)
        TEST_ASSERT(!LocalMediaDescriptor::containsPrivatePathLeakInJson(hostExactDesc.toNetworkJson()),
                    "Valid LocalMediaDescriptor network JSON must contain zero private path leaks");
        TEST_ASSERT(LocalMediaDescriptor::containsPrivatePathLeak(QStringLiteral("D:\\Movies\\Interstellar.mkv")),
                    "Windows drive path with backslashes must be flagged as privacy leak");
        TEST_ASSERT(LocalMediaDescriptor::containsPrivatePathLeak(QStringLiteral("C:/Users/shifa/Videos/film.mp4")),
                    "Windows drive path with forward slashes must be flagged as privacy leak");
        TEST_ASSERT(LocalMediaDescriptor::containsPrivatePathLeak(QStringLiteral("file:///C:/Movies/film.mp4")),
                    "file:// URL must be flagged as privacy leak");
        TEST_ASSERT(LocalMediaDescriptor::containsPrivatePathLeak(QStringLiteral("\\\\NAS-SERVER\\Share\\film.mkv")),
                    "UNC network path must be flagged as privacy leak");

        std::cout << "PASS: Local media inspection, binary MP4/MKV parsing, fingerprinting, compatibility verification & privacy guard verified." << std::endl;
    }

    // =========================================================================
    // Test 8: Local Session Protocol ("lopen", "lver", "lstart"), Host Authority,
    //         Readiness Gate, Unverified Guest Protection, Timeout & File Invalidation
    // =========================================================================
    {
        std::cout << "\n[Test 8] Local session protocol, readiness gate, host authority, timeout/retry/cancel, and file invalidation..." << std::endl;

        QTemporaryDir tempDir;
        TEST_ASSERT(tempDir.isValid(), "QTemporaryDir must be valid");
        LocalMediaInspector::clearCache();

        const QByteArray essenceMovie1 = makeDeterministicEssence(5151, 350 * 1024);
        const QByteArray essenceWrong  = makeDeterministicEssence(6262, 350 * 1024);

        const QString hostMoviePath      = tempDir.filePath(QStringLiteral("HostPC/Cinema/The.Prestige.2006.1080p.mkv"));
        const QString guestCompatibleMp4 = tempDir.filePath(QStringLiteral("GuestPC/Videos/The_Prestige_2006_Remux.mp4"));
        const QString guestWrongMovie    = tempDir.filePath(QStringLiteral("GuestPC/Videos/Memento.2000.1080p.mkv"));

        TEST_ASSERT(writeSyntheticMkv(hostMoviePath, 7800000, essenceMovie1), "Write Host Prestige MKV");
        TEST_ASSERT(writeSyntheticMp4(guestCompatibleMp4, 7800800, essenceMovie1), "Write Guest Prestige MP4 (+800ms)");
        TEST_ASSERT(writeSyntheticMkv(guestWrongMovie, 6780000, essenceWrong), "Write Guest Memento MKV");

        SimulatedNetworkHarness net;
        SimulatedNode *host  = net.addNode(QStringLiteral("FX_10000001"), 100000, true, QStringLiteral("88442"));
        SimulatedNode *guest = net.addNode(QStringLiteral("FX_20000002"), 30000, false, QStringLiteral("88442"));
        net.stepMs(600); // Room discovery & clock lock

        bool guestPromptFired = false;
        QString promptedTitle;
        qint64 promptedDurationMs = 0;
        QObject::connect(guest->sync.get(), &TelepartySync::localMediaPromptRequested,
                         [&](const QString &title, qint64 durMs, const QString &) {
                             guestPromptFired = true;
                             promptedTitle = title;
                             promptedDurationMs = durMs;
                         });

        // 8a: Non-host attempting to inject "lopen" or "lstart" is rejected by Host/Peers
        {
            QVariantMap fakeOpen = LocalMediaInspector::inspectFile(guestWrongMovie).toNetworkJson().toVariantMap();
            fakeOpen.insert(QStringLiteral("sid"), QStringLiteral("88442"));
            fakeOpen.insert(QStringLiteral("mid"), QStringLiteral("LM_deadbeef00000001"));
            fakeOpen.insert(QStringLiteral("rev"), 99);
            QJsonObject spoofMsg;
            spoofMsg.insert(QStringLiteral("v"), 1);
            spoofMsg.insert(QStringLiteral("t"), QStringLiteral("event"));
            spoofMsg.insert(QStringLiteral("f"), QStringLiteral("FX_20000002"));
            spoofMsg.insert(QStringLiteral("i"), 77);
            spoofMsg.insert(QStringLiteral("e"), QStringLiteral("lopen"));
            spoofMsg.insert(QStringLiteral("d"), QJsonObject::fromVariantMap(fakeOpen));
            host->session->simulateReceiveMessage(QStringLiteral("FX_20000002"), spoofMsg);
            TEST_ASSERT(!host->sync->isLocalSession(),
                        "Host must reject 'lopen' event sent by a non-host participant");
        }

        // 8b: Host opens local video -> broadcasts "lopen" -> held Paused waiting for Guest readiness
        host->player->setUrl(QUrl::fromLocalFile(hostMoviePath).toString());
        host->player->setSimulatedState(QStringLiteral("Playing"), 15000, 7800000, false, 100.0f);
        TEST_ASSERT(host->sync->broadcastLocalOpen(hostMoviePath, QStringLiteral("The Prestige")),
                    "Host broadcastLocalOpen must succeed");

        TEST_ASSERT(host->sync->isLocalSession(), "Host must enter LocalFile Teleparty mode");
        TEST_ASSERT(!host->sync->isLocalPlaybackCommitted(),
                    "Playback must NOT be committed when Guest has not verified yet");
        TEST_ASSERT(host->player->isPaused(),
                    "Host player must be held PAUSED at initial position while waiting for Guest verification");

        // Even if Host user clicks Play before Guest is ready, Host must stay paused and NOT commit start
        host->player->simulateUserTogglePlay(true);
        TEST_ASSERT(host->player->isPaused() && !host->sync->isLocalPlaybackCommitted(),
                    "Host manual Play before Guest readiness must be blocked and held paused");

        net.stepMs(500);
        TEST_ASSERT(guestPromptFired &&
                        promptedTitle.compare(QStringLiteral("The Prestige"), Qt::CaseInsensitive) == 0 &&
                        promptedDurationMs == 7800000,
                    "Guest must receive 'lopen' and emit localMediaPromptRequested with title and duration");
        TEST_ASSERT(guest->sync->isLocalSession() &&
                        guest->sync->localVerificationState() == LocalVerificationState::WaitingForLocalSelection,
                    "Guest must enter WaitingForLocalSelection state upon receiving 'lopen'");

        // 8c: Guest selects INCOMPATIBLE local video ("Memento.2000.1080p.mkv")
        guest->outboundActionEvents = 0;
        const bool wrongAccepted = guest->sync->selectGuestLocalMedia(guestWrongMovie);
        TEST_ASSERT(!wrongAccepted &&
                        guest->sync->localVerificationState() == LocalVerificationState::IncompatibleMedia &&
                        guest->sync->localCompatibilityReason() == LocalCompatibilityReason::TitleMismatch,
                    "Guest selecting wrong movie must enter IncompatibleMedia (TitleMismatch)");
        net.stepMs(500);

        TEST_ASSERT(!host->sync->isLocalPlaybackCommitted() && host->player->isPaused(),
                    "Host must remain paused when Guest reports IncompatibleMedia");

        // Verify unverified/incompatible Guest play/pause/seek attempts do NOT broadcast or affect Host
        guest->player->simulateUserTogglePlay(true);
        guest->player->simulateUserSeek(99000);
        net.stepMs(300);
        TEST_ASSERT(guest->outboundActionEvents == 0 && host->player->timeMs() == 15000 && host->player->isPaused(),
                    "Unverified/incompatible Guest actions must emit 0 outbound events and never affect Host");

        // 8d: Readiness timeout -> retry -> cancel workflow
        host->sync->setLocalReadinessTimeoutMs(1000);
        net.stepMs(1200);
        TEST_ASSERT(host->sync->isLocalReadinessTimedOut() && !host->sync->localErrorText().isEmpty(),
                    "Readiness timeout must trigger when Guest fails to ready within timeout window");
        TEST_ASSERT(host->player->isPaused() && !host->sync->isLocalPlaybackCommitted(),
                    "Readiness timeout must keep playback paused");

        host->sync->setLocalReadinessTimeoutMs(20000);
        host->sync->retryLocalReadinessWait();
        TEST_ASSERT(!host->sync->isLocalReadinessTimedOut(),
                    "retryLocalReadinessWait() must clear timeout state and resume waiting");

        // 8e: Guest selects COMPATIBLE local video ("The_Prestige_2006_Remux.mp4") and progresses
        //     OpeningMedia -> BufferingMedia -> ReadyToSync -> Host commits "lstart" -> both play!
        guest->autoOpenOnRemoteRequest = false;
        const QString guestResolvedUrl = QUrl::fromLocalFile(guestCompatibleMp4).toString();
        guest->player->setUrl(guestResolvedUrl);
        guest->player->setSimulatedState(QStringLiteral("Opening"), 0, 7800800, true, 0.0f);

        TEST_ASSERT(guest->sync->selectGuestLocalMedia(guestCompatibleMp4),
                    "Guest selecting compatible remuxed MP4 must pass verification");
        TEST_ASSERT(guest->sync->localVerificationState() == LocalVerificationState::OpeningMedia,
                    "Guest with player in 'Opening' state must report OpeningMedia");
        net.stepMs(300);
        TEST_ASSERT(!host->sync->isLocalPlaybackCommitted() && host->player->isPaused(),
                    "Host must not commit start while Guest is still OpeningMedia");

        // Transition Guest to BufferingMedia
        guest->player->setSimulatedState(QStringLiteral("Buffering 60%"), 15000, 7800800, true, 60.0f);
        net.stepMs(300);
        TEST_ASSERT(guest->sync->localVerificationState() == LocalVerificationState::BufferingMedia &&
                        !host->sync->isLocalPlaybackCommitted(),
                    "Host must not commit start while Guest is still BufferingMedia");

        // Transition Guest to Paused (decoded & seekable at initial position) -> ReadyToSync!
        guest->player->setSimulatedState(QStringLiteral("Paused"), 15000, 7800800, false, 100.0f);
        net.stepMs(600);

        TEST_ASSERT(guest->sync->localVerificationState() == LocalVerificationState::ReadyToSync,
                    "Guest must reach ReadyToSync once local media is open, seekable, and buffered");
        TEST_ASSERT(host->sync->isLocalPlaybackCommitted() && guest->sync->isLocalPlaybackCommitted(),
                    "Host must commit 'lstart' and both Host & Guest must be committed once all participants are ReadyToSync");
        TEST_ASSERT(host->player->isPlaying() && guest->player->isPlaying(),
                    "Both Host and Guest must automatically start synchronized playback upon 'lstart' commit");

        net.stepMs(2000);
        TEST_ASSERT(std::llabs(guest->player->timeMs() - host->player->timeMs()) <= 80,
                    "Host and Guest local video playback must stay aligned within 80ms deadband");

        // 8f: Mid-session file replacement on Guest disk invalidates Guest verification and pauses Guest
        TEST_ASSERT(writeSyntheticMp4(guestCompatibleMp4, 7800800, essenceWrong),
                    "Overwrite Guest local MP4 on disk mid-session with different content");
        net.stepMs(600);
        TEST_ASSERT(guest->sync->localVerificationState() == LocalVerificationState::IncompatibleMedia &&
                        guest->sync->localCompatibilityReason() == LocalCompatibilityReason::FingerprintMismatch &&
                        !guest->sync->isLocalPlaybackCommitted() &&
                        guest->player->isPaused(),
                    "Replacing/modifying local file on disk mid-session must invalidate Guest verification and pause Guest");

        // 8g: Cancel local media session cleanly resets LocalFile mode
        host->sync->cancelLocalMediaSession();
        TEST_ASSERT(!host->sync->isLocalSession() && host->player->isPaused(),
                    "cancelLocalMediaSession() must exit LocalFile mode and pause player");

        TEST_ASSERT(net.privacyLeaksDetected() == 0,
                    "Zero private filesystem paths, drive letters, or file:// URLs may ever be transmitted on the wire");

        std::cout << "PASS: Local session protocol, readiness gate, host authority, timeout/retry/cancel, and mid-session file invalidation verified." << std::endl;
    }

    // =========================================================================
    // Test 9: Multi-Client Local Video Teleparty End-to-End Lifecycle
    //         (Play/Pause/Rapid Seek, 3-Tier Drift Control, Buffering Recovery,
    //          Mid-Session Local Video Switch, Reconnect Re-validation & Host Migration)
    // =========================================================================
    {
        std::cout << "\n[Test 9] Multi-client Local Video Teleparty full synchronization lifecycle..." << std::endl;

        QTemporaryDir tempDir;
        TEST_ASSERT(tempDir.isValid(), "QTemporaryDir must be valid");
        LocalMediaInspector::clearCache();

        const QByteArray essenceFilm1 = makeDeterministicEssence(7001, 360 * 1024);
        const QByteArray essenceFilm2 = makeDeterministicEssence(8002, 360 * 1024);

        // Three participants with their own local copies of Film 1 ("Blade.Runner.2049") and Film 2 ("Arrival")
        const QString hostFilm1   = tempDir.filePath(QStringLiteral("Host/Movies/Blade.Runner.2049.2017.1080p.mkv"));
        const QString guest1Film1 = tempDir.filePath(QStringLiteral("Guest1/Library/Blade_Runner_2049_2017.mp4"));
        const QString guest2Film1 = tempDir.filePath(QStringLiteral("Guest2/Media/Blade.Runner.2049.2017.mkv"));

        const QString hostFilm2   = tempDir.filePath(QStringLiteral("Host/Movies/Arrival.2016.1080p.mkv"));
        const QString guest1Film2 = tempDir.filePath(QStringLiteral("Guest1/Library/Arrival.2016.1080p.mp4"));
        const QString guest2Film2 = tempDir.filePath(QStringLiteral("Guest2/Media/Arrival.2016.1080p.mkv"));

        TEST_ASSERT(writeSyntheticMkv(hostFilm1,   9800000, essenceFilm1), "Write Host Film 1 MKV");
        TEST_ASSERT(writeSyntheticMp4(guest1Film1, 9800500, essenceFilm1), "Write Guest1 Film 1 MP4");
        TEST_ASSERT(writeSyntheticMkv(guest2Film1, 9800000, essenceFilm1), "Write Guest2 Film 1 MKV");

        TEST_ASSERT(writeSyntheticMkv(hostFilm2,   6960000, essenceFilm2), "Write Host Film 2 MKV");
        TEST_ASSERT(writeSyntheticMp4(guest1Film2, 6960400, essenceFilm2), "Write Guest1 Film 2 MP4");
        TEST_ASSERT(writeSyntheticMkv(guest2Film2, 6960000, essenceFilm2), "Write Guest2 Film 2 MKV");

        SimulatedNetworkHarness net;
        net.setOneWayLatencyMs(55, 85); // Asymmetric 140ms RTT

        SimulatedNode *host   = net.addNode(QStringLiteral("FX_10000001"), 100000, true,  QStringLiteral("99001"));
        SimulatedNode *guest1 = net.addNode(QStringLiteral("FX_20000002"), 40000,  false, QStringLiteral("99001"));
        SimulatedNode *guest2 = net.addNode(QStringLiteral("FX_30000003"), 15000,  false, QStringLiteral("99001"));
        net.stepMs(800); // Room discovery + clock offset lock

        // 9a: Host declares Film 1 at 30,000ms; playback waits until BOTH Guest 1 and Guest 2 verify & ready
        host->player->setUrl(QUrl::fromLocalFile(hostFilm1).toString());
        host->player->setSimulatedState(QStringLiteral("Paused"), 30000, 9800000, false, 100.0f);
        TEST_ASSERT(host->sync->broadcastLocalOpen(hostFilm1, QStringLiteral("Blade Runner 2049")),
                    "Host broadcastLocalOpen Film 1");
        net.stepMs(400);

        // Guest 1 selects compatible MP4 copy first; Guest 2 has NOT selected yet -> nobody may start!
        TEST_ASSERT(guest1->sync->selectGuestLocalMedia(guest1Film1), "Guest 1 selects compatible Film 1 MP4");
        net.stepMs(500);
        TEST_ASSERT(guest1->sync->localVerificationState() == LocalVerificationState::ReadyToSync,
                    "Guest 1 must be ReadyToSync");
        TEST_ASSERT(!host->sync->isLocalPlaybackCommitted() && host->player->isPaused() && guest1->player->isPaused(),
                    "Playback must NOT start when Guest 1 is ready but Guest 2 is still WaitingForLocalSelection");

        // Now Guest 2 selects compatible MKV copy -> all 3 participants are ReadyToSync -> "lstart" commits!
        TEST_ASSERT(guest2->sync->selectGuestLocalMedia(guest2Film1), "Guest 2 selects compatible Film 1 MKV");
        net.stepMs(600);

        TEST_ASSERT(host->sync->isLocalPlaybackCommitted() &&
                        guest1->sync->isLocalPlaybackCommitted() &&
                        guest2->sync->isLocalPlaybackCommitted(),
                    "All 3 participants must have committed local playback once Guest 2 reaches ReadyToSync");
        TEST_ASSERT(host->player->isPlaying() && guest1->player->isPlaying() && guest2->player->isPlaying(),
                    "All 3 participants must be playing after 'lstart'");

        net.stepMs(2000);
        TEST_ASSERT(std::llabs(guest1->player->timeMs() - host->player->timeMs()) <= 80 &&
                        std::llabs(guest2->player->timeMs() - host->player->timeMs()) <= 80,
                    "All 3 participants must stay within 80ms deadband during normal playback");

        // 9b: Synchronized Pause, Seek, and Resume on Local Media
        host->player->simulateUserTogglePlay(false);
        net.stepMs(600);
        TEST_ASSERT(host->player->isPaused() && guest1->player->isPaused() && guest2->player->isPaused(),
                    "Host pause must pause all verified local participants");
        TEST_ASSERT(std::llabs(guest1->player->timeMs() - host->player->timeMs()) <= 40 &&
                        std::llabs(guest2->player->timeMs() - host->player->timeMs()) <= 40,
                    "Paused positions must align tightly across all local participants");

        host->player->simulateUserSeek(180000);
        QCoreApplication::processEvents();
        QElapsedTimer debounceWait;
        debounceWait.start();
        while (debounceWait.elapsed() < 80) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        }
        net.stepMs(600);
        TEST_ASSERT(std::llabs(guest1->player->timeMs() - 180000) <= 40 &&
                        std::llabs(guest2->player->timeMs() - 180000) <= 40,
                    "Host seek while paused must align all local participants to 180,000ms");

        host->player->simulateUserTogglePlay(true);
        net.stepMs(1500);
        TEST_ASSERT(host->player->isPlaying() && guest1->player->isPlaying() && guest2->player->isPlaying(),
                    "Host resume must resume all local participants");

        // 9c: Tier 2 soft rate correction (-300ms drift) and Tier 3 hard seek (-2200ms drift) on Local Media
        const int seeksBeforeModerate = guest1->sync->driftController().hardSeekCount();
        guest1->player->setSimulatedState(QStringLiteral("Playing"), guest1->player->timeMs() - 300, 9800500, false, 100.0f);
        net.stepMs(15000);
        TEST_ASSERT(guest1->sync->driftController().hardSeekCount() == seeksBeforeModerate &&
                        std::llabs(guest1->player->timeMs() - host->player->timeMs()) <= 60 &&
                        std::abs(guest1->player->playbackRate() - 1.0) < 0.001,
                    "Moderate -300ms local drift must converge smoothly via Tier 2 rate nudge without hard seek");

        const int seeksBeforeLarge = guest2->sync->driftController().hardSeekCount();
        guest2->player->setSimulatedState(QStringLiteral("Playing"), guest2->player->timeMs() - 2200, 9800000, false, 100.0f);
        net.stepMs(2500);
        TEST_ASSERT(guest2->sync->driftController().hardSeekCount() > seeksBeforeLarge &&
                        std::llabs(guest2->player->timeMs() - host->player->timeMs()) <= 80,
                    "Large -2200ms local drift must recover via Tier 3 hard seek");

        // 9d: Local decoder stall / buffering on Guest 1 -> suppressed during buffer -> single post-buffer recovery
        const qint64 stalledPos = guest1->player->timeMs();
        guest1->player->setSimulatedState(QStringLiteral("Buffering 40%"), stalledPos, 9800500, true, 40.0f);
        net.stepMs(1200); // Host advances +1200ms while Guest 1 is stalled at stalledPos
        guest1->player->setSimulatedState(QStringLiteral("Playing"), stalledPos, 9800500, false, 100.0f);
        net.stepMs(2500);
        TEST_ASSERT(guest1->sync->driftController().bufferRecoveryCount() >= 1 &&
                        std::llabs(guest1->player->timeMs() - host->player->timeMs()) <= 80,
                    "Guest 1 must realign via post-buffer recovery after local decoder stall");

        // 9e: Host switches to a SECOND local video ("Arrival.2016.1080p.mkv") mid-session
        //     -> invalidates previous readiness -> pauses guests -> requires fresh verification!
        host->player->setUrl(QUrl::fromLocalFile(hostFilm2).toString());
        host->player->setSimulatedState(QStringLiteral("Paused"), 0, 6960000, false, 100.0f);
        TEST_ASSERT(host->sync->broadcastLocalOpen(hostFilm2, QStringLiteral("Arrival")),
                    "Host switches to second local video Arrival");
        net.stepMs(500);

        TEST_ASSERT(!host->sync->isLocalPlaybackCommitted() &&
                        !guest1->sync->isLocalPlaybackCommitted() &&
                        !guest2->sync->isLocalPlaybackCommitted(),
                    "Switching local video mid-session must invalidate previous playback commit on all nodes");
        TEST_ASSERT(guest1->sync->localVerificationState() == LocalVerificationState::WaitingForLocalSelection &&
                        guest2->sync->localVerificationState() == LocalVerificationState::WaitingForLocalSelection &&
                        guest1->player->isPaused() && guest2->player->isPaused(),
                    "Guests must pause and return to WaitingForLocalSelection when Host switches local video");

        // Guests select their local copies of Film 2 ("Arrival") -> synchronized playback starts!
        TEST_ASSERT(guest1->sync->selectGuestLocalMedia(guest1Film2), "Guest 1 selects Arrival MP4");
        TEST_ASSERT(guest2->sync->selectGuestLocalMedia(guest2Film2), "Guest 2 selects Arrival MKV");
        net.stepMs(800);

        TEST_ASSERT(host->sync->isLocalPlaybackCommitted() &&
                        guest1->sync->isLocalPlaybackCommitted() &&
                        guest2->sync->isLocalPlaybackCommitted() &&
                        host->player->isPlaying() &&
                        guest1->player->isPlaying() &&
                        guest2->player->isPlaying(),
                    "All participants must start playing second local video once all have verified");

        // 9f: Guest 1 temporary disconnect & reconnect -> re-validates existing local file without re-prompting
        guest1->networkPartitioned = true;
        guest1->session->simulateDisconnect();
        host->player->simulateUserSeek(90000);
        QCoreApplication::processEvents();
        debounceWait.restart();
        while (debounceWait.elapsed() < 80) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        }
        net.stepMs(2000); // Host and Guest 2 play from 90,000 -> 92,000ms while Guest 1 is offline

        guest1->networkPartitioned = false;
        guest1->session->simulateReconnect();
        net.stepMs(3000);

        TEST_ASSERT(guest1->sync->localVerificationState() == LocalVerificationState::ReadyToSync &&
                        guest1->sync->isLocalPlaybackCommitted() &&
                        std::llabs(guest1->player->timeMs() - host->player->timeMs()) <= 80,
                    "Reconnecting Guest 1 with unchanged local file must automatically re-validate and resync to Host");

        // 9g: Original Host disconnects -> Guest 1 (FX_20000002) deterministically promotes to Host
        //     and maintains Local Teleparty session with Guest 2 (FX_30000003)
        const qint64 posBeforeHostLeave = guest1->player->timeMs();
        host->networkPartitioned = true;
        guest1->session->simulatePeerLeft(QStringLiteral("FX_10000001"));
        guest2->session->simulatePeerLeft(QStringLiteral("FX_10000001"));
        net.stepMs(2000);

        TEST_ASSERT(guest1->session->isHost() &&
                        guest1->sync->isLocalSession() &&
                        guest1->sync->isLocalPlaybackCommitted() &&
                        guest1->player->timeMs() >= posBeforeHostLeave + 1900 &&
                        std::llabs(guest2->player->timeMs() - guest1->player->timeMs()) <= 80,
                    "Deterministic host migration in Local Teleparty mode must preserve playback and sync seamlessly");

        TEST_ASSERT(net.privacyLeaksDetected() == 0,
                    "Zero private path leaks across entire multi-client Local Teleparty lifecycle");

        std::cout << "PASS: Multi-client Local Video Teleparty lifecycle verified end-to-end." << std::endl;
    }

    std::cout << "\n=== ALL ADAPTIVE SYNCHRONIZATION & LOCAL TELEPARTY TESTS PASSED ===" << std::endl;
    return 0;
}
