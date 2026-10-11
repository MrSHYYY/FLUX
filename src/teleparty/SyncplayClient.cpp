#include "SyncplayClient.h"
#include "../core/Logger.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSignalBlocker>
#include <QSslError>
#include <algorithm>

namespace Flux {

namespace {

constexpr int kHeartbeatIntervalMs = 1500;
constexpr int kConnectTimeoutMs = 12000;
constexpr int kMaxBufferBytes = 64 * 1024;
constexpr int kMaxLineBytes = 16 * 1024;
constexpr int kMaxPayloadBytes = 8 * 1024;
constexpr int kMaxEncodedPayloadBytes = 12 * 1024;
constexpr int kMaxChunksPerMessage = 64;
constexpr int kMaxPendingAssemblies = 32;
constexpr int kMaxCompletedCache = 128;
constexpr qint64 kChunkTimeoutMs = 10000;
constexpr int kChunkEnvelopeReserve = 50; // reserves room for FLUX|{"t":"c","m":"xxxx","i":99,"n":99,"d":""}

qint64 nowMs() {
    return QDateTime::currentMSecsSinceEpoch();
}

double nowSec() {
    return static_cast<double>(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
}

} // namespace

SyncplayClient::SyncplayClient(QObject *parent)
    : QObject(parent) {
    m_heartbeatTimer.setInterval(kHeartbeatIntervalMs);
    connect(&m_heartbeatTimer, &QTimer::timeout, this, [this]() {
        if (m_connected && m_phase == Phase::Ready) {
            sendStateHeartbeat(m_lastServerPing);
        }
        pruneExpiredChunks();
    });

    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this, [this]() {
        if (m_wantConnected) {
            openSocket();
        }
    });

    m_connectTimer.setSingleShot(true);
    m_connectTimer.setInterval(kConnectTimeoutMs);
    connect(&m_connectTimer, &QTimer::timeout, this, [this]() {
        handleLinkDown(QStringLiteral("Connection timed out"));
    });
}

SyncplayClient::~SyncplayClient() {
    m_wantConnected = false;
    m_connectTimer.stop();
    m_reconnectTimer.stop();
    m_heartbeatTimer.stop();
    if (m_socket) {
        const QSignalBlocker blocker(m_socket);
        m_socket->abort();
    }
}

void SyncplayClient::connectToServer(const QString &host, quint16 port,
                                     const QString &username, const QString &room) {
    m_host = host.trimmed().isEmpty() ? QString::fromLatin1(kDefaultHost) : host.trimmed();
    m_port = port == 0 ? kDefaultPort : port;
    m_requestedUsername = username.trimmed();
    m_username = m_requestedUsername;
    m_room = room.trimmed();
    m_maxChatLength = kDefaultMaxChatLength;
    m_lastServerPing = 0.0;

    m_wantConnected = true;
    m_everConnected = false;
    m_retry = 0;
    m_pendingChunks.clear();
    m_completedChunkOrder.clear();
    m_completedChunkSet.clear();

    openSocket();
}

void SyncplayClient::disconnectFromServer() {
    m_wantConnected = false;
    m_connectTimer.stop();
    m_reconnectTimer.stop();
    m_heartbeatTimer.stop();
    m_phase = Phase::Disconnected;

    if (m_socket) {
        const QSignalBlocker blocker(m_socket);
        if (m_connected && m_socket->state() == QAbstractSocket::ConnectedState) {
            m_socket->flush();
            m_socket->disconnectFromHost();
        } else {
            m_socket->abort();
        }
    }

    m_connected = false;
    m_buffer.clear();
    m_pendingChunks.clear();
}

void SyncplayClient::openSocket() {
    m_buffer.clear();
    m_connected = false;
    m_lastServerPing = 0.0;
    m_heartbeatTimer.stop();

    if (m_socket) {
        const QSignalBlocker blocker(m_socket);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }

    m_socket = new QSslSocket(this);
    m_socket->setPeerVerifyMode(QSslSocket::VerifyPeer);
    m_socket->setPeerVerifyName(m_host);

    connect(m_socket, &QAbstractSocket::connected, this, &SyncplayClient::onTcpConnected);
    connect(m_socket, &QSslSocket::encrypted, this, &SyncplayClient::onEncrypted);
    connect(m_socket, &QIODevice::readyRead, this, &SyncplayClient::onReadyRead);
    connect(m_socket, &QAbstractSocket::disconnected, this, [this]() {
        handleLinkDown(QStringLiteral("Connection closed"));
    });
    connect(m_socket, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        if (m_socket) {
            handleLinkDown(m_socket->errorString());
        }
    });
    connect(m_socket, &QSslSocket::sslErrors, this, [this](const QList<QSslError> &errors) {
        QStringList reasons;
        for (const QSslError &e : errors) {
            FLUX_LOG_WARN("Teleparty", QString("TLS certificate error: %1").arg(e.errorString()));
            reasons.append(e.errorString());
        }
        handleLinkDown(QStringLiteral("TLS certificate verification failed: ") + reasons.join(QStringLiteral("; ")));
    });

    m_phase = Phase::ConnectingTcp;
    m_connectTimer.start();
    // 1. Plain TCP connection first; Syncplay negotiates StartTLS in-band
    m_socket->connectToHost(m_host, m_port);
}

void SyncplayClient::onTcpConnected() {
    if (!m_socket || m_phase != Phase::ConnectingTcp) return;

    m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    m_socket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);

    m_phase = Phase::WaitingStartTls;
    // 2. Send StartTLS request: {"TLS": {"startTLS": "send"}}
    QJsonObject tlsInner;
    tlsInner.insert(QStringLiteral("startTLS"), QStringLiteral("send"));
    QJsonObject tlsRoot;
    tlsRoot.insert(QStringLiteral("TLS"), tlsInner);
    sendJson(tlsRoot);
}

void SyncplayClient::onEncrypted() {
    if (!m_socket || m_phase != Phase::UpgradingTls) return;

    m_phase = Phase::WaitingHello;
    // 3. Send Syncplay Hello handshake over the encrypted TLS channel
    sendHello();
}

void SyncplayClient::sendHello() {
    QJsonObject features;
    features.insert(QStringLiteral("sharedPlaylists"), true);
    features.insert(QStringLiteral("chat"), true);
    features.insert(QStringLiteral("featureList"), true);
    features.insert(QStringLiteral("readiness"), true);
    features.insert(QStringLiteral("managedRooms"), true);
    features.insert(QStringLiteral("persistentRooms"), false);
    features.insert(QStringLiteral("uiMode"), QStringLiteral("GUI"));

    QJsonObject roomObj;
    roomObj.insert(QStringLiteral("name"), m_room);

    QJsonObject hello;
    hello.insert(QStringLiteral("username"), m_requestedUsername);
    hello.insert(QStringLiteral("room"), roomObj);
    hello.insert(QStringLiteral("version"), QStringLiteral("1.2.255"));
    hello.insert(QStringLiteral("realversion"), QStringLiteral("1.7.6"));
    hello.insert(QStringLiteral("features"), features);

    QJsonObject root;
    root.insert(QStringLiteral("Hello"), hello);
    sendJson(root);
}

void SyncplayClient::sendStateHeartbeat(double latencyCalculation) {
    QJsonObject ping;
    ping.insert(QStringLiteral("clientLatencyCalculation"), nowSec());
    ping.insert(QStringLiteral("clientRtt"), 0.0);
    if (latencyCalculation > 0.0) {
        ping.insert(QStringLiteral("latencyCalculation"), latencyCalculation);
    }

    QJsonObject playstate;
    playstate.insert(QStringLiteral("position"), 0.0);
    playstate.insert(QStringLiteral("paused"), true);
    playstate.insert(QStringLiteral("doSeek"), false);

    QJsonObject state;
    state.insert(QStringLiteral("ping"), ping);
    state.insert(QStringLiteral("playstate"), playstate);

    QJsonObject root;
    root.insert(QStringLiteral("State"), state);
    sendJson(root);
}

bool SyncplayClient::sendJson(const QJsonObject &obj) {
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState) {
        return false;
    }
    QByteArray line = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    line.append("\r\n");
    const qint64 written = m_socket->write(line);
    m_socket->flush();
    return written == line.size();
}

bool SyncplayClient::sendRawChat(const QString &chatText) {
    if (!m_connected || m_phase != Phase::Ready) {
        return false;
    }
    const QString cleaned = chatText.trimmed();
    if (cleaned.isEmpty()) {
        return false;
    }
    QJsonObject root;
    root.insert(QStringLiteral("Chat"), cleaned);
    const bool ok = sendJson(root);
    if (ok) {
        emit rawChatSent(cleaned);
    }
    return ok;
}

bool SyncplayClient::sendFluxMessage(const QJsonObject &payload) {
    if (!m_connected || m_phase != Phase::Ready) {
        return false;
    }

    const QByteArray jsonBytes = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    if (jsonBytes.isEmpty() || jsonBytes.size() > kMaxPayloadBytes) {
        FLUX_LOG_WARN("Teleparty",
                      QString("Refusing to send invalid or oversized FLUX payload (%1 bytes)")
                          .arg(jsonBytes.size()));
        return false;
    }

    const QString fullSingle = QStringLiteral("FLUX|") + QString::fromUtf8(jsonBytes);
    const int measuredLen = std::max(static_cast<int>(fullSingle.size()),
                                     static_cast<int>(fullSingle.toUtf8().size()));
    if (measuredLen <= m_maxChatLength) {
        return sendRawChat(fullSingle);
    }

    // Payload exceeds MaxChat (150 chars): split into bounded Base64Url JSON chunks
    const QByteArray b64 = jsonBytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    const int maxSliceLen = std::max(16, m_maxChatLength - kChunkEnvelopeReserve);
    const int totalChunks = static_cast<int>((b64.size() + maxSliceLen - 1) / maxSliceLen);
    if (totalChunks < 2 || totalChunks > kMaxChunksPerMessage) {
        FLUX_LOG_WARN("Teleparty",
                      QString("Cannot chunk FLUX payload requiring %1 chunks (limit %2)")
                          .arg(totalChunks)
                          .arg(kMaxChunksPerMessage));
        return false;
    }

    const QString msgId = QString::number(++m_nextChunkMsgId, 36);
    for (int i = 0; i < totalChunks; ++i) {
        const QByteArray slice = b64.mid(i * maxSliceLen, maxSliceLen);
        QJsonObject chunkObj;
        chunkObj.insert(QStringLiteral("t"), QStringLiteral("c"));
        chunkObj.insert(QStringLiteral("m"), msgId);
        chunkObj.insert(QStringLiteral("i"), i);
        chunkObj.insert(QStringLiteral("n"), totalChunks);
        chunkObj.insert(QStringLiteral("d"), QString::fromLatin1(slice));

        const QString chunkChat = QStringLiteral("FLUX|") +
            QString::fromUtf8(QJsonDocument(chunkObj).toJson(QJsonDocument::Compact));
        const int chunkLen = std::max(static_cast<int>(chunkChat.size()),
                                      static_cast<int>(chunkChat.toUtf8().size()));
        if (chunkLen > m_maxChatLength) {
            FLUX_LOG_ERROR("Teleparty",
                           QString("Internal error: chunk length %1 exceeds maxChatLength %2")
                               .arg(chunkLen)
                               .arg(m_maxChatLength));
            return false;
        }
        if (!sendRawChat(chunkChat)) {
            return false;
        }
    }
    return true;
}

void SyncplayClient::onReadyRead() {
    if (!m_socket) return;
    m_buffer.append(m_socket->readAll());

    if (m_buffer.size() > kMaxBufferBytes) {
        FLUX_LOG_WARN("Teleparty", "Syncplay receive buffer overflow; resetting connection");
        m_buffer.clear();
        handleLinkDown(QStringLiteral("Protocol buffer overflow"));
        return;
    }

    while (true) {
        // While UpgradingTls is in progress, do not parse lines until encrypted() fires
        if (m_phase == Phase::UpgradingTls) {
            return;
        }

        const int nl = m_buffer.indexOf('\n');
        if (nl < 0) {
            return;
        }

        const QByteArray rawLine = m_buffer.left(nl).trimmed();
        m_buffer.remove(0, nl + 1);

        if (rawLine.isEmpty()) {
            continue;
        }
        if (rawLine.size() > kMaxLineBytes) {
            FLUX_LOG_WARN("Teleparty", "Discarding oversized line from Syncplay server");
            continue;
        }

        handleLine(rawLine);
    }
}

void SyncplayClient::handleLine(const QByteArray &line) {
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        FLUX_LOG_WARN("Teleparty", "Ignored non-JSON or malformed line from Syncplay server");
        return;
    }

    const QJsonObject root = doc.object();

    if (m_phase == Phase::WaitingStartTls) {
        const QString startTls = root.value(QStringLiteral("TLS")).toObject()
                                     .value(QStringLiteral("startTLS")).toString();
        if (startTls == QLatin1String("true")) {
            m_phase = Phase::UpgradingTls;
            m_socket->startClientEncryption();
        } else {
            m_everConnected = false;
            handleLinkDown(QStringLiteral("Server rejected StartTLS negotiation"));
        }
        return;
    }

    handleJsonMessage(root);
}

void SyncplayClient::handleJsonMessage(const QJsonObject &root) {
    // 1. Hello response
    if (root.contains(QStringLiteral("Hello")) && root.value(QStringLiteral("Hello")).isObject()) {
        const QJsonObject hello = root.value(QStringLiteral("Hello")).toObject();
        const QString assignedUser = hello.value(QStringLiteral("username")).toString().trimmed();
        if (!assignedUser.isEmpty()) {
            m_username = assignedUser;
        }
        const QString assignedRoom = hello.value(QStringLiteral("room")).toObject()
                                         .value(QStringLiteral("name")).toString().trimmed();
        if (!assignedRoom.isEmpty()) {
            m_room = assignedRoom;
        }

        const QJsonObject features = hello.value(QStringLiteral("features")).toObject();
        const int serverMaxChat = features.value(QStringLiteral("maxChatMessageLength"))
                                      .toInt(kDefaultMaxChatLength);
        m_maxChatLength = std::clamp(serverMaxChat, 50, 4096);

        m_connectTimer.stop();
        m_phase = Phase::Ready;
        m_connected = true;
        m_everConnected = true;
        m_retry = 0;
        m_heartbeatTimer.start();

        // Send initial keepalive State and request current room occupant list
        sendStateHeartbeat(0.0);
        QJsonObject listReq;
        listReq.insert(QStringLiteral("List"), QJsonValue::Null);
        sendJson(listReq);

        emit joinedRoom(m_room, m_username, m_maxChatLength);
        return;
    }

    // 2. State keepalive ping from server
    if (root.contains(QStringLiteral("State")) && root.value(QStringLiteral("State")).isObject()) {
        const QJsonObject stateObj = root.value(QStringLiteral("State")).toObject();
        const QJsonObject pingObj = stateObj.value(QStringLiteral("ping")).toObject();
        if (pingObj.contains(QStringLiteral("latencyCalculation"))) {
            const double lat = pingObj.value(QStringLiteral("latencyCalculation")).toDouble(0.0);
            if (lat > 0.0) {
                m_lastServerPing = lat;
                sendStateHeartbeat(lat);
            }
        }
        return;
    }

    // 3. Chat message
    if (root.contains(QStringLiteral("Chat")) && root.value(QStringLiteral("Chat")).isObject()) {
        const QJsonObject chat = root.value(QStringLiteral("Chat")).toObject();
        const QString sender = chat.value(QStringLiteral("username")).toString().trimmed();
        const QString msg = chat.value(QStringLiteral("message")).toString();
        handleChat(sender, msg);
        return;
    }

    // 4. Set (room join / leave notifications)
    if (root.contains(QStringLiteral("Set")) && root.value(QStringLiteral("Set")).isObject()) {
        const QJsonObject setObj = root.value(QStringLiteral("Set")).toObject();
        if (setObj.contains(QStringLiteral("user")) && setObj.value(QStringLiteral("user")).isObject()) {
            const QJsonObject usersObj = setObj.value(QStringLiteral("user")).toObject();
            for (auto it = usersObj.begin(); it != usersObj.end(); ++it) {
                const QString uname = it.key().trimmed();
                if (uname.isEmpty() || uname == m_username || !it.value().isObject()) {
                    continue;
                }
                const QJsonObject udata = it.value().toObject();
                const QString userRoom = udata.value(QStringLiteral("room")).toObject()
                                             .value(QStringLiteral("name")).toString().trimmed();
                const QJsonObject evt = udata.value(QStringLiteral("event")).toObject();
                if (evt.value(QStringLiteral("left")).toBool()) {
                    if (userRoom.isEmpty() || userRoom == m_room) {
                        clearPeerChunks(uname);
                        emit peerLeft(uname);
                    }
                } else if (evt.value(QStringLiteral("joined")).toBool()) {
                    if (userRoom.isEmpty() || userRoom == m_room) {
                        emit peerPresent(uname);
                    }
                } else if (!userRoom.isEmpty() && userRoom != m_room) {
                    // User switched away to a different room
                    clearPeerChunks(uname);
                    emit peerLeft(uname);
                }
            }
        }
        return;
    }

    // 5. List (initial room occupants snapshot)
    if (root.contains(QStringLiteral("List")) && root.value(QStringLiteral("List")).isObject()) {
        const QJsonObject listObj = root.value(QStringLiteral("List")).toObject();
        if (listObj.contains(m_room) && listObj.value(m_room).isObject()) {
            const QJsonObject roomUsers = listObj.value(m_room).toObject();
            for (auto it = roomUsers.begin(); it != roomUsers.end(); ++it) {
                const QString uname = it.key().trimmed();
                if (!uname.isEmpty() && uname != m_username) {
                    emit peerPresent(uname);
                }
            }
        }
        return;
    }

    // 6. Server Error
    if (root.contains(QStringLiteral("Error")) && root.value(QStringLiteral("Error")).isObject()) {
        const QString errMsg = root.value(QStringLiteral("Error")).toObject()
                                   .value(QStringLiteral("message")).toString();
        FLUX_LOG_WARN("Teleparty", QString("Syncplay server error: %1").arg(errMsg));
        handleLinkDown(errMsg.isEmpty() ? QStringLiteral("Server error") : errMsg);
        return;
    }
}

void SyncplayClient::processIncomingChatForTest(const QString &senderUsername, const QString &message) {
    handleChat(senderUsername, message);
}

void SyncplayClient::handleChat(const QString &sender, const QString &msg) {
    if (sender.isEmpty() || sender == m_username) {
        return; // ignore our own echo
    }
    if (!msg.startsWith(QLatin1String("FLUX|"))) {
        return; // ignore non-FLUX chat
    }
    if (msg.size() > kMaxLineBytes) {
        return;
    }

    const QByteArray jsonPart = msg.mid(5).toUtf8();
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonPart, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        FLUX_LOG_WARN("Teleparty",
                      QString("Rejected truncated or malformed FLUX chat message from %1 (%2 chars)")
                          .arg(sender)
                          .arg(msg.size()));
        return;
    }

    const QJsonObject obj = doc.object();
    if (obj.value(QStringLiteral("t")).toString() == QLatin1String("c")) {
        handleChunk(sender, obj);
        return;
    }

    emit fluxMessageReceived(sender, obj);
}

void SyncplayClient::handleChunk(const QString &sender, const QJsonObject &chunkObj) {
    pruneExpiredChunks();

    const QString msgId = chunkObj.value(QStringLiteral("m")).toString().trimmed();
    const int idx = chunkObj.value(QStringLiteral("i")).toInt(-1);
    const int total = chunkObj.value(QStringLiteral("n")).toInt(-1);
    const QString slice = chunkObj.value(QStringLiteral("d")).toString();

    if (msgId.isEmpty() || msgId.size() > 16 ||
        total < 2 || total > kMaxChunksPerMessage ||
        idx < 0 || idx >= total ||
        slice.isEmpty() || slice.size() > m_maxChatLength) {
        FLUX_LOG_WARN("Teleparty", QString("Rejected invalid FLUX chunk header from %1").arg(sender));
        return;
    }

    const QString key = sender + QLatin1Char(':') + msgId;
    if (m_completedChunkSet.contains(key)) {
        return; // duplicate chunk for an already-completed message
    }

    if (!m_pendingChunks.contains(key)) {
        if (m_pendingChunks.size() >= kMaxPendingAssemblies) {
            // Evict the oldest pending assembly to bound memory
            auto oldestIt = m_pendingChunks.begin();
            for (auto it = m_pendingChunks.begin(); it != m_pendingChunks.end(); ++it) {
                if (it.value().createdAtMs < oldestIt.value().createdAtMs) {
                    oldestIt = it;
                }
            }
            m_pendingChunks.erase(oldestIt);
        }

        PendingAssembly init;
        init.total = total;
        init.receivedCount = 0;
        init.totalBytes = 0;
        init.createdAtMs = nowMs();
        init.chunks.resize(total);
        m_pendingChunks.insert(key, std::move(init));
    }

    PendingAssembly &asmBuf = m_pendingChunks[key];
    if (asmBuf.total != total) {
        FLUX_LOG_WARN("Teleparty", QString("Discarded FLUX chunk with mismatched total from %1").arg(sender));
        m_pendingChunks.remove(key);
        return;
    }

    if (!asmBuf.chunks[idx].isEmpty()) {
        return; // duplicate chunk index; ignore
    }

    if (asmBuf.totalBytes + slice.size() > kMaxEncodedPayloadBytes) {
        FLUX_LOG_WARN("Teleparty", QString("Discarded oversized chunked FLUX payload from %1").arg(sender));
        m_pendingChunks.remove(key);
        return;
    }

    asmBuf.chunks[idx] = slice.toLatin1();
    asmBuf.totalBytes += static_cast<int>(slice.size());
    ++asmBuf.receivedCount;

    if (asmBuf.receivedCount < asmBuf.total) {
        return; // waiting for remaining chunks
    }

    // All chunks received: reassemble in strict index order (0 .. total - 1)
    QByteArray combinedB64;
    combinedB64.reserve(asmBuf.totalBytes);
    for (int k = 0; k < asmBuf.total; ++k) {
        combinedB64.append(asmBuf.chunks[k]);
    }
    m_pendingChunks.remove(key);
    recordCompletedChunkKey(key);

    const auto decoded = QByteArray::fromBase64Encoding(
        combinedB64, QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.isEmpty() || decoded.decoded.size() > kMaxPayloadBytes) {
        FLUX_LOG_WARN("Teleparty", QString("Discarded invalid Base64 or oversized reassembled payload from %1").arg(sender));
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(decoded.decoded, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        FLUX_LOG_WARN("Teleparty", QString("Discarded malformed JSON after chunk reassembly from %1").arg(sender));
        return;
    }

    const QJsonObject fullObj = doc.object();
    if (fullObj.value(QStringLiteral("t")).toString() == QLatin1String("c")) {
        return; // reject nested chunk envelopes
    }

    emit fluxMessageReceived(sender, fullObj);
}

void SyncplayClient::pruneExpiredChunks() {
    if (m_pendingChunks.isEmpty()) return;
    const qint64 cutoff = nowMs() - kChunkTimeoutMs;
    for (auto it = m_pendingChunks.begin(); it != m_pendingChunks.end();) {
        if (it.value().createdAtMs < cutoff) {
            it = m_pendingChunks.erase(it);
        } else {
            ++it;
        }
    }
}

void SyncplayClient::recordCompletedChunkKey(const QString &key) {
    if (m_completedChunkSet.contains(key)) return;
    if (m_completedChunkOrder.size() >= kMaxCompletedCache) {
        const QString oldest = m_completedChunkOrder.takeFirst();
        m_completedChunkSet.remove(oldest);
    }
    m_completedChunkOrder.append(key);
    m_completedChunkSet.insert(key);
}

void SyncplayClient::clearPeerChunks(const QString &sender) {
    const QString prefix = sender + QLatin1Char(':');
    for (auto it = m_pendingChunks.begin(); it != m_pendingChunks.end();) {
        if (it.key().startsWith(prefix)) {
            it = m_pendingChunks.erase(it);
        } else {
            ++it;
        }
    }
}

void SyncplayClient::handleLinkDown(const QString &reason) {
    if (!m_wantConnected) return;

    m_connectTimer.stop();
    const bool wasConnected = m_connected;
    m_connected = false;
    m_phase = Phase::Disconnected;
    m_heartbeatTimer.stop();
    m_pendingChunks.clear();

    if (m_socket) {
        const QSignalBlocker blocker(m_socket);
        m_socket->abort();
    }

    if (!m_everConnected) {
        m_wantConnected = false;
        FLUX_LOG_WARN("Teleparty", QString("Could not connect to Syncplay server: %1").arg(reason));
        emit connectionFailed(reason);
        return;
    }

    FLUX_LOG_WARN("Teleparty", QString("Syncplay connection lost: %1").arg(reason));
    if (wasConnected) {
        emit disconnected();
    }
    scheduleReconnect();
}

void SyncplayClient::scheduleReconnect() {
    if (m_reconnectTimer.isActive()) return;
    const int delay = std::min(10000, 1500 * (1 << std::min(m_retry, 3)));
    ++m_retry;
    m_reconnectTimer.start(delay);
}

} // namespace Flux
