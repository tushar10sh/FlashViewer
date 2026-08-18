#pragma once
#include <QObject>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class QNetworkReply;

struct OsmTileKey {
    int z{0}, x{0}, y{0};
    bool operator==(const OsmTileKey& o) const {
        return z==o.z && x==o.x && y==o.y;
    }
};

namespace std {
template<> struct hash<OsmTileKey> {
    size_t operator()(const OsmTileKey& k) const {
        return hash<int>{}(k.z) ^ (hash<int>{}(k.x) << 10) ^ (hash<int>{}(k.y) << 20);
    }
};
}

struct OsmConnectionResult {
    bool ok{false};
    int statusCode{0};
    QString errorString;
};

class OsmTileProvider : public QObject {
    Q_OBJECT
public:
    explicit OsmTileProvider(QObject* parent = nullptr);

    static bool validateUrlTemplate(const QString& url, QString* errorReason = nullptr);
    static OsmConnectionResult testConnection(const QString& url, int timeoutMs = 5000);

    void           setUrlTemplate(const QString& url);
    const QString& urlTemplate() const { return m_url_template; }

    QImage requestTile(int z, int x, int y);
    QImage cachedTile(int z, int x, int y) const;

    void clearCache();

signals:
    void tileReady(int z, int x, int y);

private slots:
    void onReplyFinished(QNetworkReply* reply);

private:
    QString tileUrl(int z, int x, int y) const;

    QNetworkAccessManager*      m_nam{nullptr};
    QString                     m_url_template;
    bool                        m_h2_disabled{false};
    std::unordered_map<OsmTileKey, QImage>        m_cache;
    std::unordered_map<OsmTileKey, bool>          m_pending;
    std::unordered_map<QNetworkReply*, OsmTileKey> m_reply_keys;
};
