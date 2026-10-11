#pragma once

#include <QFile>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QUrl>

class QNetworkReply;

namespace Flux {

class MediaCache : public QObject {
    Q_OBJECT
    Q_PROPERTY(qint64 sizeBytes READ sizeBytes NOTIFY sizeChanged)
    Q_PROPERTY(qint64 limitBytes READ limitBytes CONSTANT)
    Q_PROPERTY(QString sizeText READ sizeText NOTIFY sizeChanged)

public:
    static constexpr qint64 kLimitBytes = 10LL * 1000 * 1000 * 1000;

    explicit MediaCache(QObject *parent = nullptr);
    ~MediaCache() override;

    qint64 sizeBytes() const { return m_sizeBytes; }
    qint64 limitBytes() const { return kLimitBytes; }
    QString sizeText() const;

    Q_INVOKABLE QString cachedFileForUrl(const QString &url);
    Q_INVOKABLE void cacheMedia(const QString &url, const QString &title);
    Q_INVOKABLE void clearCache();

signals:
    void sizeChanged();

private:
    QString rootPath() const;
    QString mediaPath(const QUrl &url) const;
    void refreshSize();
    bool makeRoom(qint64 bytesNeeded);
    void finishReply(QNetworkReply *reply, const QString &path);

    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    QFile m_file;
    qint64 m_sizeBytes = 0;
    qint64 m_currentBytes = 0;
    qint64 m_expectedBytes = -1;
};

} // namespace Flux
