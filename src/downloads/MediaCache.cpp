#include "MediaCache.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStandardPaths>
#include <algorithm>

namespace Flux {

QString MediaCache::sizeText() const {
    const double gigabytes = double(m_sizeBytes) / 1000000000.0;
    return QString::number(gigabytes, 'f', 2) + QStringLiteral(" GB of 10 GB");
}

MediaCache::MediaCache(QObject *parent) : QObject(parent) {
    QDir().mkpath(rootPath());
    const QFileInfoList partials = QDir(rootPath()).entryInfoList({QStringLiteral("*.part")}, QDir::Files);
    for (const QFileInfo &file : partials) QFile::remove(file.absoluteFilePath());
    refreshSize();
}

MediaCache::~MediaCache() {
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
    }
    if (m_file.isOpen()) m_file.close();
}

QString MediaCache::rootPath() const {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
        .filePath(QStringLiteral("media"));
}

QString MediaCache::mediaPath(const QUrl &url) const {
    const QByteArray hash = QCryptographicHash::hash(url.toString(QUrl::FullyEncoded).toUtf8(),
                                                      QCryptographicHash::Sha256).toHex();
    QString suffix = QFileInfo(url.path()).suffix().toLower();
    if (suffix.size() > 8 || !suffix.contains(QRegularExpression(QStringLiteral("^[a-z0-9]+$")))) suffix.clear();
    return QDir(rootPath()).filePath(QString::fromLatin1(hash) + (suffix.isEmpty() ? QString() : QLatin1Char('.') + suffix));
}

QString MediaCache::cachedFileForUrl(const QString &source) {
    const QUrl url(source);
    if (!url.isValid() || (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))) return {};
    const QString path = mediaPath(url);
    QFileInfo file(path);
    if (!file.exists() || !file.isFile() || file.size() <= 0) return {};
    QFile touch(path);
    if (touch.open(QIODevice::ReadOnly)) {
        touch.setFileTime(QDateTime::currentDateTime(), QFileDevice::FileModificationTime);
    }
    return QUrl::fromLocalFile(path).toString();
}

bool MediaCache::makeRoom(qint64 bytesNeeded) {
    if (bytesNeeded < 0 || bytesNeeded > kLimitBytes) return false;
    QDir dir(rootPath());
    QFileInfoList files = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Time);
    qint64 total = 0;
    for (const QFileInfo &f : files) {
        if (!f.fileName().endsWith(QLatin1String(".part"))) total += f.size();
    }
    for (auto it = files.crbegin(); total + bytesNeeded > kLimitBytes && it != files.crend(); ++it) {
        if (it->absoluteFilePath() == m_file.fileName() || it->fileName().endsWith(QLatin1String(".part"))) continue;
        const qint64 size = it->size();
        QFile::remove(it->absoluteFilePath());
        total -= size;
    }
    refreshSize();
    return total + bytesNeeded <= kLimitBytes;
}

void MediaCache::cacheMedia(const QString &source, const QString &) {
    if (m_reply || cachedFileForUrl(source).size() > 0) return;
    const QUrl url(source);
    if (!url.isValid() || (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))) return;
    const QString path = mediaPath(url);
    const QString partPath = path + QStringLiteral(".part");
    QDir().mkpath(rootPath());
    m_file.setFileName(partPath);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    m_currentBytes = 0;
    m_expectedBytes = -1;

    QNetworkRequest request(url);
    request.setPriority(QNetworkRequest::LowPriority);
    request.setTransferTimeout(30000);
    QNetworkReply *reply = m_network.get(request);
    m_reply = reply;
    connect(reply, &QNetworkReply::metaDataChanged, this, [this, reply]() {
        const qint64 length = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        if (length > 0) {
            m_expectedBytes = length;
            if (!makeRoom(length)) reply->abort();
        }
    });
    connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
        const QByteArray bytes = reply->readAll();
        if (!makeRoom(m_currentBytes + bytes.size()) || m_file.write(bytes) != bytes.size()) {
            reply->abort();
            return;
        }
        m_currentBytes += bytes.size();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, path]() {
        finishReply(reply, path);
    });
}

void MediaCache::finishReply(QNetworkReply *reply, const QString &path) {
    const QByteArray tail = reply->readAll();
    bool ok = reply->error() == QNetworkReply::NoError;
    if (ok && !tail.isEmpty()) {
        ok = makeRoom(m_currentBytes + tail.size()) && m_file.write(tail) == tail.size();
        m_currentBytes += ok ? tail.size() : 0;
    }
    reply->deleteLater();
    m_reply = nullptr;
    m_file.flush();
    m_file.close();
    if (m_expectedBytes > 0 && m_currentBytes != m_expectedBytes) ok = false;
    if (ok && m_currentBytes > 0) {
        QFile::remove(path);
        ok = QFile::rename(m_file.fileName(), path);
    }
    if (!ok) QFile::remove(m_file.fileName());
    refreshSize();
}

void MediaCache::clearCache() {
    if (m_reply) {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    if (m_file.isOpen()) m_file.close();
    QDir(rootPath()).removeRecursively();
    QDir().mkpath(rootPath());
    m_currentBytes = 0;
    refreshSize();
}

void MediaCache::refreshSize() {
    qint64 size = 0;
    const QFileInfoList files = QDir(rootPath()).entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
    for (const QFileInfo &file : files) {
        if (!file.fileName().endsWith(QLatin1String(".part"))) size += file.size();
    }
    if (size != m_sizeBytes) {
        m_sizeBytes = size;
        emit sizeChanged();
    }
}

} // namespace Flux
