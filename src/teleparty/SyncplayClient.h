#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSet>
#include <QSslSocket>
#include <QString>
#include <QTimer>
#include <QVector>

namespace Flux {

// Syncplay protocol client over StartTLS + TLS (default: syncplay.pl:8995).
// Implements the proven connection, StartTLS upgrade, Hello handshake, State keepalive
// heartbeat, room membership tracking, and FLUX| chat relay (with automatic chunking
// and reassembly to respect the server's 150-character chat limit).
class SyncplayClient : public QObject {
    Q_OBJECT

public:
    static constexpr const char *kDefaultHost = "syncplay.pl";
    static constexpr quint16 kDefaultPort = 8995;
    static constexpr int kDefaultMaxChatLength = 150;

    explicit SyncplayClient(QObject *parent = nullptr);
    ~SyncplayClient() override;

    void connectToServer(const QString &host, quint16 port,
                         const QString &username, const QString &room);
    void disconnectFromServer();

    bool isConnected() const { return m_connected; }
    QString username() const { return m_username; }
    QString room() const { return m_room; }
    int maxChatLength() const { return m_maxChatLength; }

    // Sends a FLUX JSON object via Syncplay Chat ("FLUX|<json>").
    // Automatically chunks if the complete chat string exceeds maxChatLength().
    bool sendFluxMessage(const QJsonObject &payload);

    // Low-level helper to send a single raw Chat line (also used by tests to probe limits)
    bool sendRawChat(const QString &chatText);

    // Processes an incoming Chat object (exposed for deterministic unit testing of chunk reassembly)
    void processIncomingChatForTest(const QString &senderUsername, const QString &message);

signals:
    // Emitted when StartTLS + Hello handshake succeeds and we are in the room
    void joinedRoom(const QString &room, const QString &username, int maxChatLength);
    // Emitted when an established session drops (client will auto-reconnect)
    void disconnected();
    // Emitted when the initial connection attempt fails fatally
    void connectionFailed(const QString &reason);

    // Emitted when a complete, validated FLUX JSON object is received from another room member
    void fluxMessageReceived(const QString &senderUsername, const QJsonObject &payload);

    // Emitted for every outgoing chat frame (useful for verifying wire constraints in tests)
    void rawChatSent(const QString &chatText);

    // Room presence notifications from Syncplay's Set/List messages
    void peerPresent(const QString &username);
    void peerLeft(const QString &username);

private:
    enum class Phase {
        Disconnected,
        ConnectingTcp,
        WaitingStartTls,
        UpgradingTls,
        WaitingHello,
        Ready
    };

    struct PendingAssembly {
        int total = 0;
        int receivedCount = 0;
        int totalBytes = 0;
        qint64 createdAtMs = 0;
        QVector<QByteArray> chunks;
    };

    void openSocket();
    void onTcpConnected();
    void onEncrypted();
    void onReadyRead();
    void handleLine(const QByteArray &line);
    void handleJsonMessage(const QJsonObject &root);
    void handleChat(const QString &sender, const QString &msg);
    void handleChunk(const QString &sender, const QJsonObject &chunkObj);

    void sendHello();
    void sendStateHeartbeat(double latencyCalculation = 0.0);
    bool sendJson(const QJsonObject &obj);

    void pruneExpiredChunks();
    void recordCompletedChunkKey(const QString &key);
    void clearPeerChunks(const QString &sender);

    void handleLinkDown(const QString &reason);
    void scheduleReconnect();

    QSslSocket *m_socket = nullptr;
    QTimer m_heartbeatTimer;
    QTimer m_reconnectTimer;
    QTimer m_connectTimer;

    Phase m_phase = Phase::Disconnected;
    QString m_host;
    quint16 m_port = kDefaultPort;
    QString m_requestedUsername;
    QString m_username;
    QString m_room;
    int m_maxChatLength = kDefaultMaxChatLength;
    double m_lastServerPing = 0.0;

    QByteArray m_buffer;
    bool m_wantConnected = false;
    bool m_connected = false;
    bool m_everConnected = false;
    int m_retry = 0;

    quint64 m_nextChunkMsgId = 0;
    QHash<QString, PendingAssembly> m_pendingChunks;
    QList<QString> m_completedChunkOrder;
    QSet<QString> m_completedChunkSet;
};

} // namespace Flux
