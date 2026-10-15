#pragma once

#include <QAbstractListModel>
#include <QString>
#include <QVariantMap>
#include <vector>

namespace Flux {

struct MediaRoot {
    QString id;
    QString name;
    QString group;
    QString url;
    QString serverOrigin;
    QString href;
};

class MediaLibraryModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum LibraryRoles {
        IdRole = Qt::UserRole + 1,
        NameRole,
        GroupRole,
        UrlRole,
        ServerOriginRole,
        HrefRole,
        DisplayNameRole
    };

    explicit MediaLibraryModel(QObject *parent = nullptr);
    ~MediaLibraryModel() override = default;

    // QAbstractListModel overrides
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return static_cast<int>(m_roots.size()); }

    bool loadDefault();
    bool loadFromFile(const QString &path);
    // serverType 1 = Dhaka Flix, 2 = ICC FTP. For ICC FTP the session token and the server's
    // address (from `baseUrl`, any URL on that server) replace what is stored in the library file.
    bool loadServer(int serverType, const QString &session = QString(), const QString &baseUrl = QString());
    int serverType() const { return m_serverType; }

    const std::vector<MediaRoot>& roots() const { return m_roots; }
    const MediaRoot* rootAt(int index) const;
    const MediaRoot* findById(const QString &id) const;

    Q_INVOKABLE QVariantMap getRoot(int index) const;
    Q_INVOKABLE int indexOfId(const QString &id) const;

signals:
    void countChanged();

private:
    void adjustIccRoots(const QString &session, const QString &baseUrl);

    std::vector<MediaRoot> m_roots;
    int m_serverType = 1;
};

} // namespace Flux
