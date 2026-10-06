#include "voiceguide.hpp"
#include "oggopusdecoder.hpp"
#include "bbportlog.hpp"
#ifdef BBPORT_HAVE_NATIVE_TLS
#include "tlsnetworkaccessmanager.hpp"
#endif

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegExp>
#include <QSettings>
#include <QUrl>

#include <bb/data/JsonDataAccess>
#include <bb/multimedia/MediaError>
#include <bb/multimedia/MediaPlayer>

using bb::multimedia::MediaPlayer;
using bb::multimedia::MediaError;

namespace {

QString readLine(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).trimmed() : QString();
}

} // namespace

VoiceGuide::VoiceGuide(QObject *parent) :
    QObject(parent), m_nam(0), m_player(new MediaPlayer(this)),
    m_enabled(true), m_playing(false), m_fileIndex(0)
{
#ifdef BBPORT_HAVE_NATIVE_TLS
    m_nam = new TlsNetworkAccessManager(this);
#else
    m_nam = new QNetworkAccessManager(this);
#endif
    m_server = readLine("/accounts/1000/shared/misc/berryassistant_server.txt");
    while (m_server.endsWith('/')) m_server.chop(1);
    m_token = readLine("/accounts/1000/shared/misc/berryassistant_token.txt");
    m_enabled = QSettings().value("voice/on", true).toBool();
    connect(m_player, SIGNAL(playbackCompleted()), this, SLOT(onPlaybackCompleted()));
    bbportLog(QString("[voice] %1").arg(available() ? "server configurato" : "server BerryAssistant non configurato: voce non disponibile"));
}

void VoiceGuide::setEnabled(bool on)
{
    if (on == m_enabled) return;
    m_enabled = on;
    QSettings().setValue("voice/on", on);
    if (!on) {
        if (m_reply) m_reply->abort();
        m_pendingPath.clear();
        m_player->stop();
        m_playing = false;
    }
    emit enabledChanged();
}

// Street abbreviations as Google writes them, which Piper would spell out.
QString VoiceGuide::normalize(const QString &text)
{
    QString t = text;
    t.replace(QString::fromUtf8("·"), ",");
    t.replace(QRegExp("\\bP\\.zz?a\\b"), "Piazza");
    t.replace(QRegExp("\\bP\\.le\\b"), "Piazzale");
    t.replace(QRegExp("\\bV\\.le\\b"), "Viale");
    t.replace(QRegExp("\\bC\\.so\\b"), "Corso");
    t.replace(QRegExp("\\bL\\.go\\b"), "Largo");
    t.replace(QRegExp("\\bV\\.lo\\b"), "Vicolo");
    t.replace(QRegExp("\\bS\\.S\\.\\s*"), "Strada Statale ");
    t.replace(QRegExp("\\bS\\.P\\.\\s*"), "Strada Provinciale ");
    t.replace(QRegExp("\\bSS(\\d+)"), "Strada Statale \\1");
    t.replace(QRegExp("\\bSP(\\d+)"), "Strada Provinciale \\1");
    t.replace(QRegExp("\\bA(\\d+)\\b"), "A \\1");
    return t.simplified();
}

void VoiceGuide::say(const QString &text)
{
    if (!m_enabled || !available() || text.trimmed().isEmpty()) return;
    const QString spoken = normalize(text);
    if (m_reply) m_reply->abort();   // an older prompt still downloading is stale now

    QVariantMap body;
    body["text"] = spoken;
    QByteArray json;
    bb::data::JsonDataAccess jda;
    jda.saveToBuffer(body, &json);
    QNetworkRequest req(QUrl(m_server + "/v1/tts"));
    req.setRawHeader("Content-Type", "application/json");
    req.setRawHeader("Authorization", "Bearer " + m_token.toUtf8());
    m_reply = m_nam->post(req, json);
    m_reply->setProperty("text", spoken);
    connect(m_reply, SIGNAL(finished()), this, SLOT(onReply()));
}

void VoiceGuide::onReply()
{
    QNetworkReply *r = qobject_cast<QNetworkReply *>(sender());
    if (!r) return;
    r->deleteLater();
    if (r != m_reply || r->error() == QNetworkReply::OperationCanceledError || !m_enabled) return;
    int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QByteArray audio = r->readAll();
    if (status != 200 || audio.isEmpty()) {
        bbportLog(QString("[voice] sintesi fallita (HTTP %1)").arg(status));
        return;
    }
    QByteArray wav;
    if (audio.startsWith("OggS")) {
        QElapsedTimer t;
        t.start();
        if (!OggOpusDecoder::decodeToWav(audio, &wav)) {
            bbportLog("[voice] decodifica Opus fallita");
            return;
        }
        bbportLog(QString("[voice] \"%1\": %2 byte Opus -> WAV in %3 ms")
                      .arg(r->property("text").toString()).arg(audio.size()).arg(t.elapsed()));
    } else {
        wav = audio;   // the server's WAV fallback
    }
    // Two alternating files: one may still be playing while the next is written.
    const QString path = QString("%1/voice%2.wav").arg(QDir::homePath()).arg(m_fileIndex);
    m_fileIndex = 1 - m_fileIndex;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(wav) != wav.size()) return;
    f.close();
    if (m_playing) m_pendingPath = path;   // after the current prompt
    else play(path);
}

void VoiceGuide::play(const QString &path)
{
    m_player->stop();
    m_player->setSourceUrl(QUrl::fromLocalFile(path));
    MediaError::Type err = m_player->play();
    m_playing = err == MediaError::None;
    if (!m_playing) bbportLog(QString("[voice] riproduzione fallita: %1").arg(int(err)));
}

void VoiceGuide::onPlaybackCompleted()
{
    m_playing = false;
    if (!m_pendingPath.isEmpty()) {
        QString next = m_pendingPath;
        m_pendingPath.clear();
        play(next);
    }
}
