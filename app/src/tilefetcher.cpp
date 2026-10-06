#include "tilefetcher.hpp"
#include "bbportlog.hpp"

#include <QElapsedTimer>
#include <QMutexLocker>

#ifdef BBPORT_HAVE_NATIVE_TLS
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"
#endif

namespace {

const int kIdleCloseMs = 30000;     // close an unused connection (radio can sleep)
const int kRequestDeadlineMs = 20000;

QByteArray headerValue(const QList<QPair<QByteArray, QByteArray> > &headers, const char *name)
{
    for (int i = 0; i < headers.size(); ++i)
        if (qstricmp(headers.at(i).first.constData(), name) == 0) return headers.at(i).second;
    return QByteArray();
}

int parseMaxAge(const QByteArray &cacheControl)
{
    QList<QByteArray> parts = cacheControl.split(',');
    for (int i = 0; i < parts.size(); ++i) {
        QByteArray p = parts.at(i).trimmed().toLower();
        if (p.startsWith("max-age=")) return p.mid(8).toInt();
    }
    return 0;
}

} // namespace

// ------------------------------------------------------------------ fetcher

TileFetcher::TileFetcher(const QByteArray &host, int connections, QObject *parent) :
    QObject(parent), m_stopping(false)
{
    for (int i = 0; i < connections; ++i) {
        TileConnection *c = new TileConnection(this, i, host);
        m_workers << c;
        c->start();
    }
}

TileFetcher::~TileFetcher()
{
    {
        QMutexLocker locker(&m_mutex);
        m_stopping = true;
        m_queue.clear();
        m_cond.wakeAll();
    }
    // Workers notice m_stopping within one read-timeout slice (5 s at most).
    for (int i = 0; i < m_workers.size(); ++i) {
        m_workers.at(i)->wait(8000);
        delete m_workers.at(i);
    }
}

void TileFetcher::enqueue(const TileJob &job)
{
    QMutexLocker locker(&m_mutex);
    for (int i = 0; i < m_queue.size(); ++i) {
        if (m_queue.at(i).key == job.key) {
            m_queue[i] = job;
            m_cond.wakeOne();
            return;
        }
    }
    m_queue.append(job);
    m_cond.wakeOne();
}

void TileFetcher::setWanted(const QSet<QString> &keys)
{
    QMutexLocker locker(&m_mutex);
    m_wanted = keys;
    for (int i = m_queue.size() - 1; i >= 0; --i)
        if (!m_wanted.contains(m_queue.at(i).key)) m_queue.removeAt(i);
}

bool TileFetcher::takeJob(TileJob *job, int waitMs)
{
    QMutexLocker locker(&m_mutex);
    if (m_queue.isEmpty() && !m_stopping) m_cond.wait(&m_mutex, waitMs);
    if (m_stopping || m_queue.isEmpty()) return false;
    int best = 0;
    for (int i = 1; i < m_queue.size(); ++i)
        if (m_queue.at(i).priority < m_queue.at(best).priority) best = i;
    *job = m_queue.takeAt(best);
    return true;
}

bool TileFetcher::stopping()
{
    QMutexLocker locker(&m_mutex);
    return m_stopping;
}

void TileFetcher::deliver(const QString &key, int status, const QByteArray &body,
                          const QByteArray &etag, int maxAge, const QString &error)
{
    // Emitted from the worker thread: receivers in the UI thread get it queued.
    emit finished(key, status, body, etag, maxAge, error);
}

// ------------------------------------------------------------------ connection

#ifdef BBPORT_HAVE_NATIVE_TLS

namespace {

struct CaStore
{
    mbedtls_x509_crt cacert;
    bool ok;
    CaStore() : ok(false)
    {
        mbedtls_x509_crt_init(&cacert);
        ok = mbedtls_x509_crt_parse_file(&cacert, "app/native/assets/cacert.pem") >= 0;
    }
};

CaStore *caStore()
{
    static QMutex mutex;
    static CaStore *store = 0;
    QMutexLocker locker(&mutex);
    if (!store) store = new CaStore();
    return store;
}

struct Conn
{
    TileFetcher *owner;
    int index;
    QByteArray host;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_config conf;
    mbedtls_ssl_context ssl;
    mbedtls_net_context net;
    mbedtls_ssl_session saved;
    bool haveSaved;
    bool connected;
    int requestsOnConn;
    QByteArray pending;   // bytes read past the end of the previous response
};

QString mbedErr(int ret)
{
    char buf[200];
    mbedtls_strerror(ret, buf, sizeof(buf));
    return QString::fromLatin1(buf);
}

void closeConn(Conn &c)
{
    if (!c.connected) return;
    mbedtls_ssl_close_notify(&c.ssl);
    mbedtls_net_free(&c.net);
    mbedtls_net_init(&c.net);
    c.connected = false;
    c.pending.clear();
}

bool openConn(Conn &c, QString *error)
{
    QElapsedTimer t;
    t.start();
    mbedtls_net_free(&c.net);
    mbedtls_net_init(&c.net);
    int ret = mbedtls_net_connect(&c.net, c.host.constData(), "443", MBEDTLS_NET_PROTO_TCP);
    if (ret != 0) { *error = "connect: " + mbedErr(ret); return false; }
    mbedtls_ssl_session_reset(&c.ssl);
    mbedtls_ssl_set_bio(&c.ssl, &c.net, mbedtls_net_send, mbedtls_net_recv, mbedtls_net_recv_timeout);
    if (c.haveSaved) mbedtls_ssl_set_session(&c.ssl, &c.saved);
    while ((ret = mbedtls_ssl_handshake(&c.ssl)) != 0) {
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE || ret == MBEDTLS_ERR_SSL_TIMEOUT) {
            if (c.owner->stopping() || t.elapsed() > 15000) { *error = "handshake timeout"; mbedtls_net_free(&c.net); mbedtls_net_init(&c.net); return false; }
            continue;
        }
        *error = "handshake: " + mbedErr(ret);
        mbedtls_net_free(&c.net);
        mbedtls_net_init(&c.net);
        return false;
    }
    uint32_t flags = mbedtls_ssl_get_verify_result(&c.ssl);
    if (flags != 0) {
        *error = QString("certificato non valido (0x%1)").arg(flags, 0, 16);
        mbedtls_net_free(&c.net);
        mbedtls_net_init(&c.net);
        return false;
    }
    // Keep the session for an abbreviated handshake on the next reconnect.
    mbedtls_ssl_session_free(&c.saved);
    mbedtls_ssl_session_init(&c.saved);
    c.haveSaved = mbedtls_ssl_get_session(&c.ssl, &c.saved) == 0;
    c.connected = true;
    c.requestsOnConn = 0;
    c.pending.clear();
    bbportLog(QString("[tiles] %1 conn#%2 aperta in %3 ms").arg(QString(c.host)).arg(c.index).arg(t.elapsed()));
    return true;
}

// Appends what is available to c.pending. Returns >0 bytes, 0 = peer closed,
// <0 = error or deadline/stop.
int readMore(Conn &c, const QElapsedTimer &deadline)
{
    unsigned char buf[8192];
    for (;;) {
        int n = mbedtls_ssl_read(&c.ssl, buf, sizeof(buf));
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE || n == MBEDTLS_ERR_SSL_TIMEOUT) {
            if (c.owner->stopping() || deadline.elapsed() > kRequestDeadlineMs) return -1;
            continue;
        }
        if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n == 0) return 0;
        if (n < 0) return n;
        c.pending.append(reinterpret_cast<const char *>(buf), n);
        return n;
    }
}

bool ensureBytes(Conn &c, int needed, const QElapsedTimer &deadline)
{
    while (c.pending.size() < needed)
        if (readMore(c, deadline) <= 0) return false;
    return true;
}

struct Response
{
    int status;
    QByteArray body;
    QByteArray etag;
    int maxAge;
    bool closeAfter;
};

bool doRequest(Conn &c, const TileJob &job, Response *r, QString *error)
{
    QElapsedTimer deadline;
    deadline.start();
    QByteArray req = "GET " + job.path + " HTTP/1.1\r\nHost: " + c.host +
                     "\r\nUser-Agent: BerryMaps/0.1 (BlackBerry 10)\r\nAccept-Encoding: identity\r\n";
    if (!job.etag.isEmpty()) req += "If-None-Match: " + job.etag + "\r\n";
    req += "\r\n";
    int written = 0;
    while (written < req.size()) {
        int ret = mbedtls_ssl_write(&c.ssl, reinterpret_cast<const unsigned char *>(req.constData()) + written,
                                    req.size() - written);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (ret < 0) { *error = "write: " + mbedErr(ret); return false; }
        written += ret;
    }

    int headerEnd;
    while ((headerEnd = c.pending.indexOf("\r\n\r\n")) < 0) {
        int n = readMore(c, deadline);
        if (n <= 0) { *error = n == 0 ? "connessione chiusa dal server" : "lettura intestazioni fallita"; return false; }
    }
    QList<QByteArray> lines = c.pending.left(headerEnd).split('\n');
    c.pending.remove(0, headerEnd + 4);
    QList<QByteArray> statusParts = lines.isEmpty() ? QList<QByteArray>() : lines.at(0).trimmed().split(' ');
    r->status = statusParts.size() >= 2 ? statusParts.at(1).toInt() : 0;
    if (r->status <= 0) { *error = "riga di stato HTTP non valida"; return false; }
    QList<QPair<QByteArray, QByteArray> > headers;
    for (int i = 1; i < lines.size(); ++i) {
        QByteArray line = lines.at(i).trimmed();
        int colon = line.indexOf(':');
        if (colon > 0) headers.append(qMakePair(line.left(colon).trimmed(), line.mid(colon + 1).trimmed()));
    }
    r->etag = headerValue(headers, "ETag");
    r->maxAge = parseMaxAge(headerValue(headers, "Cache-Control"));
    r->closeAfter = headerValue(headers, "Connection").toLower().contains("close");
    r->body.clear();

    if (r->status == 304 || r->status == 204 || r->status < 200) return true;

    if (headerValue(headers, "Transfer-Encoding").toLower().contains("chunked")) {
        for (;;) {
            int lineEnd;
            while ((lineEnd = c.pending.indexOf("\r\n")) < 0)
                if (readMore(c, deadline) <= 0) { *error = "chunk interrotto"; return false; }
            bool ok = false;
            int size = c.pending.left(lineEnd).trimmed().split(';').at(0).toInt(&ok, 16);
            c.pending.remove(0, lineEnd + 2);
            if (!ok) { *error = "chunk non valido"; return false; }
            if (!ensureBytes(c, size + 2, deadline)) { *error = "chunk interrotto"; return false; }
            r->body.append(c.pending.constData(), size);
            c.pending.remove(0, size + 2);
            if (size == 0) return true;
        }
    }
    QByteArray cl = headerValue(headers, "Content-Length");
    if (!cl.isEmpty()) {
        int len = cl.toInt();
        if (!ensureBytes(c, len, deadline)) { *error = "corpo interrotto"; return false; }
        r->body = c.pending.left(len);
        c.pending.remove(0, len);
        return true;
    }
    // No length: body runs until the server closes the connection.
    while (readMore(c, deadline) > 0) {}
    r->body = c.pending;
    c.pending.clear();
    r->closeAfter = true;
    return true;
}

} // namespace

void TileConnection::run()
{
    CaStore *ca = caStore();
    if (!ca->ok) {
        bbportLog("[tiles] ERRORE: impossibile leggere assets/cacert.pem");
        return;
    }
    Conn c;
    c.owner = m_owner;
    c.index = m_index;
    c.host = m_host;
    c.haveSaved = false;
    c.connected = false;
    c.requestsOnConn = 0;
    mbedtls_entropy_init(&c.entropy);
    mbedtls_ctr_drbg_init(&c.drbg);
    mbedtls_ssl_config_init(&c.conf);
    mbedtls_ssl_init(&c.ssl);
    mbedtls_net_init(&c.net);
    mbedtls_ssl_session_init(&c.saved);

    bool ready = mbedtls_ctr_drbg_seed(&c.drbg, mbedtls_entropy_func, &c.entropy,
                                       reinterpret_cast<const unsigned char *>("berrymaps"), 9) == 0
        && mbedtls_ssl_config_defaults(&c.conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT) == 0;
    if (ready) {
        mbedtls_ssl_conf_authmode(&c.conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&c.conf, &ca->cacert, 0);
        mbedtls_ssl_conf_rng(&c.conf, mbedtls_ctr_drbg_random, &c.drbg);
        // Bounded read slices (see BBport's TlsRequestThread for why the
        // timeout-aware recv is needed on QNX instead of SO_RCVTIMEO).
        mbedtls_ssl_conf_read_timeout(&c.conf, 5000);
        ready = mbedtls_ssl_setup(&c.ssl, &c.conf) == 0 && mbedtls_ssl_set_hostname(&c.ssl, c.host.constData()) == 0;
    }
    if (!ready) bbportLog(QString("[tiles] conn#%1 ERRORE inizializzazione TLS").arg(m_index));

    QElapsedTimer idle;
    idle.start();
    while (ready && !m_owner->stopping()) {
        TileJob job;
        if (!m_owner->takeJob(&job, 1000)) {
            if (c.connected && idle.elapsed() > kIdleCloseMs) {
                closeConn(c);
                bbportLog(QString("[tiles] conn#%1 chiusa per inattivita").arg(m_index));
            }
            continue;
        }
        QElapsedTimer t;
        t.start();
        QString error;
        Response r;
        bool ok = false;
        // A reused connection may have been closed by Google while idle: one
        // retry on a fresh connection before reporting a failure.
        for (int attempt = 0; attempt < 2 && !ok && !m_owner->stopping(); ++attempt) {
            bool reused = c.connected;
            if (!c.connected && !openConn(c, &error)) break;
            ok = doRequest(c, job, &r, &error);
            if (!ok) {
                closeConn(c);
                if (!reused) break;
            }
        }
        if (ok) {
            ++c.requestsOnConn;
            bbportLog(QString("[tiles] conn#%1 %2 -> %3, %4 KB, %5 ms (richiesta %6 sulla connessione)")
                          .arg(m_index).arg(job.key).arg(r.status).arg(r.body.size() / 1024.0, 0, 'f', 1)
                          .arg(t.elapsed()).arg(c.requestsOnConn));
            if (r.closeAfter) closeConn(c);
            m_owner->deliver(job.key, r.status, r.body, r.etag, r.maxAge, QString());
        } else {
            bbportLog(QString("[tiles] conn#%1 %2 FALLITA: %3").arg(m_index).arg(job.key).arg(error));
            m_owner->deliver(job.key, 0, QByteArray(), QByteArray(), 0, error);
        }
        idle.restart();
    }

    closeConn(c);
    mbedtls_ssl_session_free(&c.saved);
    mbedtls_ssl_free(&c.ssl);
    mbedtls_ssl_config_free(&c.conf);
    mbedtls_ctr_drbg_free(&c.drbg);
    mbedtls_entropy_free(&c.entropy);
}

#else

void TileConnection::run()
{
    bbportLog("[tiles] TLS nativo non disponibile in questa build (solo dispositivo)");
}

#endif /* BBPORT_HAVE_NATIVE_TLS */
