#ifndef VOICEGUIDE_HPP_
#define VOICEGUIDE_HPP_

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QPointer>

class QNetworkAccessManager;
class QNetworkReply;
namespace bb { namespace multimedia { class MediaPlayer; } }

// Spoken navigation prompts (PROGETTO.md §23).
//
// BB10 has no text-to-speech API, so the text goes to the BerryAssistant
// server (Piper, Italian voice "paola") via POST /v1/tts, which answers with
// ~10 KB of Ogg/Opus; libopus decodes it to WAV and the system MediaPlayer
// plays it. Server URL and token are BerryAssistant's own files on the phone:
//   /accounts/1000/shared/misc/berryassistant_server.txt
//   /accounts/1000/shared/misc/berryassistant_token.txt
// A newer prompt replaces one still downloading; one that arrives while
// another is playing waits for it (latest wins).
class VoiceGuide : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(bool available READ available CONSTANT)
public:
    explicit VoiceGuide(QObject *parent = 0);

    bool enabled() const { return m_enabled; }
    void setEnabled(bool on);
    bool available() const { return !m_server.isEmpty() && !m_token.isEmpty(); }

    Q_INVOKABLE void say(const QString &text);

signals:
    void enabledChanged();

private slots:
    void onReply();
    void onPlaybackCompleted();

private:
    static QString normalize(const QString &text);
    void play(const QString &path);

    QNetworkAccessManager *m_nam;
    bb::multimedia::MediaPlayer *m_player;
    QPointer<QNetworkReply> m_reply;
    QString m_server, m_token;
    bool m_enabled;
    bool m_playing;
    QString m_pendingPath;
    int m_fileIndex;
};

#endif /* VOICEGUIDE_HPP_ */
