/*
 * qmqtt_client_p.cpp - qmqtt client private
 *
 * Copyright (c) 2013  Ery Lee <ery.lee at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of mqttc nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "qmqtt_client_p.h"
#include "qmqtt_message.h"
#include <QLoggingCategory>
#include <QUuid>
#ifndef QT_NO_SSL
#include <QFile>
#include <QSslConfiguration>
#include <QSslKey>
#endif // QT_NO_SSL

Q_LOGGING_CATEGORY(client, "qmqtt.client")

static const quint8 QOS0 = 0x00;
static const quint8 QOS1 = 0x01;
static const quint8 QOS2 = 0x02;

QMQTT::ClientPrivate::ClientPrivate(Client* qq_ptr)
    : _host(QHostAddress::LocalHost)
    , _port(1883)
    , _gmid(1)
    , _version(MQTTVersion::V3_1_0)
    , _clientId(QUuid::createUuid().toString())
    , _cleanSession(false)
    , _keepAlive(300)
    , _connectionState(STATE_INIT)
    , _willQos(0)
    , _willRetain(false)
    , q_ptr(qq_ptr)
{
}

QMQTT::ClientPrivate::~ClientPrivate()
{
}

void QMQTT::ClientPrivate::init(const QHostAddress& host, const quint16 port, NetworkInterface* network)
{
    _host = host;
    _port = port;
    if(network == NULL)
    {
        init(new Network);
    }
    else
    {
        init(network);
    }
}

#ifndef QT_NO_SSL
void QMQTT::ClientPrivate::init(const QString& hostName, const quint16 port,
                                const QSslConfiguration &config, const bool ignoreSelfSigned)
{
    _hostName = hostName;
    _port = port;
    init(new Network(config, ignoreSelfSigned));
}
#endif // QT_NO_SSL

void QMQTT::ClientPrivate::init(const QString& hostName, const quint16 port, const bool ssl,
                                const bool ignoreSelfSigned)
{
    _hostName = hostName;
    _port = port;
    if (ssl)
    {
#ifndef QT_NO_SSL
        QSslConfiguration sslConf = QSslConfiguration::defaultConfiguration();
        QList<QSslCertificate> certs = QSslCertificate::fromPath(QStringLiteral("./cert.crt"));
        if (!certs.isEmpty())
            sslConf.setLocalCertificate(certs.first());
        QFile file(QStringLiteral("./cert.key"));
        if (file.open(QIODevice::ReadOnly)) {
            sslConf.setPrivateKey(QSslKey(file.readAll(), QSsl::Rsa));
        }
        sslConf.setPeerVerifyMode(QSslSocket::VerifyNone);
        init(hostName, port, sslConf, ignoreSelfSigned);
#else
        Q_UNUSED(ignoreSelfSigned)
        qCritical() << "SSL not supported in this QT build";
#endif // QT_NO_SSL
    }
    else
    {
        init(new Network);
    }
}

#ifdef QT_WEBSOCKETS_LIB
void QMQTT::ClientPrivate::init(const QString& url,
                                const QString& origin,
                                QWebSocketProtocol::Version version,
                                const QSslConfiguration* sslConfig,
                                bool ignoreSelfSigned)
{
    _hostName = url;
    init(new Network(origin, version, sslConfig, ignoreSelfSigned));
}

#endif // QT_WEBSOCKETS_LIB

void QMQTT::ClientPrivate::init(NetworkInterface* network)
{
    Q_Q(Client);

    _network.reset(network);

    QObject::connect(&_timer, &QTimer::timeout, q, &Client::onTimerPingReq);
    QObject::connect(_network.data(), &Network::connected,
                     q, &Client::onNetworkConnected);
    QObject::connect(_network.data(), &Network::disconnected,
                     q, &Client::onNetworkDisconnected);
    QObject::connect(_network.data(), &Network::received,
                     q, &Client::onNetworkReceived);
    QObject::connect(_network.data(), &Network::error,
                     q, &Client::onNetworkError);
}

void QMQTT::ClientPrivate::connectToHost()
{
    if (_hostName.isEmpty())
    {
        _network->connectToHost(_host, _port);
    }
    else
    {
        _network->connectToHost(_hostName, _port);
    }
}

void QMQTT::ClientPrivate::onNetworkConnected()
{
    sendConnect();
    startKeepAlive();
}

void QMQTT::ClientPrivate::sendConnect()
{
    quint8 header = CONNECT;
    quint8 flags = 0;

    //header
    Frame frame(header);

    //flags
    flags = FLAG_CLEANSESS(flags, _cleanSession ? 1 : 0 );
    flags = FLAG_WILL(flags, willTopic().isEmpty() ? 0 : 1);
    if (!willTopic().isEmpty())
    {
        flags = FLAG_WILLQOS(flags, willQos());
        flags = FLAG_WILLRETAIN(flags, willRetain() ? 1 : 0);
    }
    if (!username().isEmpty())
    {
        flags = FLAG_USERNAME(flags, 1);
        flags = FLAG_PASSWD(flags, !password().isEmpty() ? 1 : 0);
    }

    //payload
    if(_version == V3_1_1)
    {
        frame.writeString(QStringLiteral(PROTOCOL_MAGIC_3_1_1));
    }
    else
    {
        frame.writeString(QStringLiteral(PROTOCOL_MAGIC_3_1_0));
    }
    frame.writeChar(_version);
    frame.writeChar(flags);
    frame.writeInt(_keepAlive);
    frame.writeString(_clientId);
    if(!willTopic().isEmpty())
    {
        frame.writeString(willTopic());
        if(!willMessage().isEmpty())
        {
            frame.writeByteArray(_willMessage);
        }
    }
    if (!_username.isEmpty())
    {
        frame.writeString(_username);
        if (!_password.isEmpty())
        {
            frame.writeByteArray(_password);
        }
    }
    _network->sendFrame(frame);
}

quint16 QMQTT::ClientPrivate::sendPublish(const Message &message)
{
    quint16 msgid = message.id();

    quint8 header = PUBLISH;
    header = SETRETAIN(header, message.retain() ? 1 : 0);
    header = SETQOS(header, message.qos());
    header = SETDUP(header, message.dup() ? 1 : 0);
    Frame frame(header);
    frame.writeString(message.topic());
    if(message.qos() > QOS0) {
        if (msgid == 0)
            msgid = nextmid();
        frame.writeInt(msgid);
    }
    if(!message.payload().isEmpty()) {
        frame.writeRawData(message.payload());
    }
    _network->sendFrame(frame);
    return msgid;
}

void QMQTT::ClientPrivate::sendPuback(const quint8 type, const quint16 mid)
{
    Frame frame(type);
    frame.writeInt(mid);
    _network->sendFrame(frame);
}

quint16 QMQTT::ClientPrivate::sendSubscribe(const QString & topic, const quint8 qos)
{
    quint16 mid = nextmid();
    Frame frame(SETQOS(SUBSCRIBE, QOS1));
    frame.writeInt(mid);
    frame.writeString(topic);
    frame.writeChar(qos);
    _network->sendFrame(frame);
    return mid;
}
quint16 QMQTT::ClientPrivate::sendSubscribes(const QMap<QString, quint8> map)
{
    quint16 mid = nextmid();
    Frame frame(SETQOS(SUBSCRIBE, QOS1));
    frame.writeInt(mid);
    QMapIterator<QString, quint8> i(map);
     while (i.hasNext()) {
         i.next();
         frame.writeString( i.key());
         frame.writeChar(i.value());
     }
    _network->sendFrame(frame);
    return mid;
}

quint16 QMQTT::ClientPrivate::sendUnsubscribe(const QString &topic)
{
    quint16 mid = nextmid();
    Frame frame(SETQOS(UNSUBSCRIBE, QOS1));
    frame.writeInt(mid);
    frame.writeString(topic);
    _network->sendFrame(frame);
    return mid;
}
quint16 QMQTT::ClientPrivate::sendUnsubscribes(const QStringList &topic)
{
    quint16 mid = nextmid();
    Frame frame(SETQOS(UNSUBSCRIBE, QOS1));
    frame.writeInt(mid);
    for (int i=0;i<topic.size();i++) {
        frame.writeString(topic.at(i));
    }
    _network->sendFrame(frame);
    return mid;
}
void QMQTT::ClientPrivate::onTimerPingReq()
{
    Frame frame(PINGREQ);
    _network->sendFrame(frame);
}

void QMQTT::ClientPrivate::disconnectFromHost()
{
    sendDisconnect();
    _network->disconnectFromHost();
}

void QMQTT::ClientPrivate::sendDisconnect()
{
    Frame frame(DISCONNECT);
    _network->sendFrame(frame);
}

void QMQTT::ClientPrivate::startKeepAlive()
{
    _timer.setInterval(_keepAlive*1000);
    _timer.start();
}

void QMQTT::ClientPrivate::stopKeepAlive()
{
    _timer.stop();
}

quint16 QMQTT::ClientPrivate::nextmid()
{
    return _gmid++;
}

quint16 QMQTT::ClientPrivate::publish(const Message& message)
{
    Q_Q(Client);
    quint16 msgid = sendPublish(message);

    // Emit published only at QOS0
    if (message.qos() == QOS0)
        emit q->published(message, msgid);
    else
        _midToMessage[msgid] = message;

    return msgid;
}

void QMQTT::ClientPrivate::puback(const quint8 type, const quint16 msgid)
{
    sendPuback(type, msgid);
}

void QMQTT::ClientPrivate::subscribe(const QString& topic, const quint8 qos)
{
    quint16 msgid = sendSubscribe(topic, qos);
    QMap<QString,quint8> map;
    map[topic] = qos;
    _midToTopic[msgid] = map;
}
void QMQTT::ClientPrivate::subscribes(const QMap<QString, quint8> map)
{
    quint16 msgid = sendSubscribes(map);
    _midToTopic[msgid] = map;
}
void QMQTT::ClientPrivate::unsubscribe(const QString& topic)
{
    quint16 msgid = sendUnsubscribe(topic);
    QStringList tlist;
    tlist << topic;
    _midToTopicList[msgid] = tlist;
}

void QMQTT::ClientPrivate::unsubscribes(const QStringList& topics)
{
    quint16 msgid = sendUnsubscribes(topics);
    _midToTopicList[msgid] = topics;
}
void QMQTT::ClientPrivate::onNetworkDisconnected()
{
    Q_Q(Client);

    stopKeepAlive();
    _midToTopic.clear();
    _midToTopicList.clear();
    _midToMessage.clear();
    emit q->disconnected();
}

void QMQTT::ClientPrivate::onNetworkReceived(const QMQTT::Frame& frm)
{
    QMQTT::Frame frame(frm);
    quint8 qos = 0;
    bool retain, dup;
    QString topic;
    quint16 mid = 0;
    quint8 header = frame.header();
    quint8 type = GETTYPE(header);

    switch(type)
    {
    case CONNACK:
        frame.readChar();
        handleConnack(frame.readChar());
        break;
    case PUBLISH:
        qos = GETQOS(header);
        retain = GETRETAIN(header);
        dup = GETDUP(header);
        topic = frame.readString();
        if( qos > QOS0) {
            mid = frame.readInt();
        }
        handlePublish(Message(mid, topic, frame.data(), qos, retain, dup));
        break;
    case PUBACK:
    case PUBREC:
    case PUBREL:
    case PUBCOMP:
        mid = frame.readInt();
        handlePuback(type, mid);
        break;
    case SUBACK: {
        mid = frame.readInt();
        QMap<QString,quint8> topicMap = _midToTopic.take(mid);
        QMapIterator<QString, quint8> i(topicMap);
        QMap<QString,quint8> resultMap;
        quint8 tmpqos = 0;
         while (i.hasNext()) {
             i.next();
             tmpqos=frame.readChar();
             if (topic.isEmpty()) {
                 topic=i.key();
                 qos=tmpqos;
             }
             resultMap[i.key()] = frame.readChar();
         }
        handleSuback(topic, qos,topicMap);
        break;
    }
    case UNSUBACK: {
        QStringList topiclist = _midToTopicList.take(mid);
        handleUnsuback( topiclist);
        break;
    }
    case PINGRESP:
        handlePingresp();
        break;
    default:
        break;
    }
}

void QMQTT::ClientPrivate::handleConnack(const quint8 ack)
{
    Q_Q(Client);

    switch (ack)
    {
    case 0:
        emit q->connected();
        break;
    case 1:
        emit q->error(MqttUnacceptableProtocolVersionError);
        break;
    case 2:
        emit q->error(MqttIdentifierRejectedError);
        break;
    case 3:
        emit q->error(MqttServerUnavailableError);
        break;
    case 4:
        emit q->error(MqttBadUserNameOrPasswordError);
        break;
    case 5:
        emit q->error(MqttNotAuthorizedError);
        break;
    default:
        emit q->error(UnknownError);
        break;
    }
}

void QMQTT::ClientPrivate::handlePublish(const Message& message)
{
    Q_Q(Client);

    if(message.qos() == QOS1)
    {
        sendPuback(PUBACK, message.id());
    }
    else if(message.qos() == QOS2)
    {
        sendPuback(PUBREC, message.id());
    }
    emit q->received(message);
}

void QMQTT::ClientPrivate::handlePuback(const quint8 type, const quint16 msgid)
{
    Q_Q(Client);

    if(type == PUBREC)
    {
        sendPuback(PUBREL, msgid);
    }
    else if (type == PUBREL)
    {
        sendPuback(PUBCOMP, msgid);
    }
    else if (type == PUBACK || type == PUBCOMP)
    {
        // Emit published on PUBACK at QOS1 and on PUBCOMP at QOS2
        const Message &message = _midToMessage.take(msgid);
        emit q->published(message, msgid);
    }
}

void QMQTT::ClientPrivate::handlePingresp() {
    Q_Q(Client);
    emit q->pingresp();
}

void QMQTT::ClientPrivate::handleSuback(const QString &topic, const quint8 qos, const QMap<QString,quint8>& topicMap) {
    Q_Q(Client);
    emit q->subscribed(topic, qos, topicMap);
}

void QMQTT::ClientPrivate::handleUnsuback(const QStringList & topics) {
    Q_Q(Client);
    emit q->unsubscribed(topics.at(0),topics);
}

bool QMQTT::ClientPrivate::autoReconnect() const
{
    return _network->autoReconnect();
}

void QMQTT::ClientPrivate::setAutoReconnect(const bool autoReconnect)
{
    _network->setAutoReconnect(autoReconnect);
}

bool QMQTT::ClientPrivate::autoReconnectInterval() const
{
    return _network->autoReconnectInterval();
}

void QMQTT::ClientPrivate::setAutoReconnectInterval(const int autoReconnectInterval)
{
    _network->setAutoReconnectInterval(autoReconnectInterval);
}

bool QMQTT::ClientPrivate::isConnectedToHost() const
{
    return _network->isConnectedToHost();
}

QMQTT::ConnectionState QMQTT::ClientPrivate::connectionState() const
{
    return _connectionState;
}

void QMQTT::ClientPrivate::setCleanSession(const bool cleanSession)
{
    _cleanSession = cleanSession;
}

bool QMQTT::ClientPrivate::cleanSession() const
{
    return _cleanSession;
}

void QMQTT::ClientPrivate::setKeepAlive(const quint16 keepAlive)
{
    _keepAlive = keepAlive;
}

quint16 QMQTT::ClientPrivate::keepAlive() const
{
    return _keepAlive;
}

void QMQTT::ClientPrivate::setPassword(const QByteArray& password)
{
    _password = password;
}

QByteArray QMQTT::ClientPrivate::password() const
{
    return _password;
}

void QMQTT::ClientPrivate::setUsername(const QString& username)
{
    _username = username;
}

QString QMQTT::ClientPrivate::username() const
{
    return _username;
}

void QMQTT::ClientPrivate::setVersion(const MQTTVersion version)
{
    _version = version;
}

QMQTT::MQTTVersion QMQTT::ClientPrivate::version() const
{
    return _version;
}

void QMQTT::ClientPrivate::setClientId(const QString& clientId)
{
    if(clientId.isEmpty())
    {
        _clientId = QUuid::createUuid().toString();
    }
    else
    {
        _clientId = clientId;
    }
}

QString QMQTT::ClientPrivate::clientId() const
{
    return _clientId;
}

void QMQTT::ClientPrivate::setPort(const quint16 port)
{
    _port = port;
}

quint16 QMQTT::ClientPrivate::port() const
{
    return _port;
}

void QMQTT::ClientPrivate::setHost(const QHostAddress& host)
{
    _host = host;
}

QHostAddress QMQTT::ClientPrivate::host() const
{
    return _host;
}

void QMQTT::ClientPrivate::setHostName(const QString& hostName)
{
    _hostName = hostName;
}

QString QMQTT::ClientPrivate::hostName() const
{
    return _hostName;
}

QString QMQTT::ClientPrivate::willTopic() const
{
    return _willTopic;
}

void QMQTT::ClientPrivate::setWillTopic(const QString& willTopic)
{
    _willTopic = willTopic;
}

quint8 QMQTT::ClientPrivate::willQos() const
{
    return _willQos;
}

void QMQTT::ClientPrivate::setWillQos(const quint8 willQos)
{
    _willQos = willQos;
}

bool QMQTT::ClientPrivate::willRetain() const
{
    return _willRetain;
}

void QMQTT::ClientPrivate::setWillRetain(const bool willRetain)
{
    _willRetain = willRetain;
}

QByteArray QMQTT::ClientPrivate::willMessage() const
{
    return _willMessage;
}

void QMQTT::ClientPrivate::setWillMessage(const QByteArray& willMessage)
{
    _willMessage = willMessage;
}

void QMQTT::ClientPrivate::onNetworkError(QAbstractSocket::SocketError socketError)
{
    Q_Q(Client);

    switch (socketError)
    {
    case QAbstractSocket::ConnectionRefusedError:
        emit q->error(SocketConnectionRefusedError);
        break;
    case QAbstractSocket::RemoteHostClosedError:
        emit q->error(SocketRemoteHostClosedError);
        break;
    case QAbstractSocket::HostNotFoundError:
        emit q->error(SocketHostNotFoundError);
        break;
    case QAbstractSocket::SocketAccessError:
        emit q->error(SocketAccessError);
        break;
    case QAbstractSocket::SocketResourceError:
        emit q->error(SocketResourceError);
        break;
    case QAbstractSocket::SocketTimeoutError:
        emit q->error(SocketTimeoutError);
        break;
    case QAbstractSocket::DatagramTooLargeError:
        emit q->error(SocketDatagramTooLargeError);
        break;
    case QAbstractSocket::NetworkError:
        emit q->error(SocketNetworkError);
        break;
    case QAbstractSocket::AddressInUseError:
        emit q->error(SocketAddressInUseError);
        break;
    case QAbstractSocket::SocketAddressNotAvailableError:
        emit q->error(SocketAddressNotAvailableError);
        break;
    case QAbstractSocket::UnsupportedSocketOperationError:
        emit q->error(SocketUnsupportedSocketOperationError);
        break;
    case QAbstractSocket::UnfinishedSocketOperationError:
        emit q->error(SocketUnfinishedSocketOperationError);
        break;
    case QAbstractSocket::ProxyAuthenticationRequiredError:
        emit q->error(SocketProxyAuthenticationRequiredError);
        break;
    case QAbstractSocket::SslHandshakeFailedError:
        emit q->error(SocketSslHandshakeFailedError);
        break;
    case QAbstractSocket::ProxyConnectionRefusedError:
        emit q->error(SocketProxyConnectionRefusedError);
        break;
    case QAbstractSocket::ProxyConnectionClosedError:
        emit q->error(SocketProxyConnectionClosedError);
        break;
    case QAbstractSocket::ProxyConnectionTimeoutError:
        emit q->error(SocketProxyConnectionTimeoutError);
        break;
    case QAbstractSocket::ProxyNotFoundError:
        emit q->error(SocketProxyNotFoundError);
        break;
    case QAbstractSocket::ProxyProtocolError:
        emit q->error(SocketProxyProtocolError);
        break;
    case QAbstractSocket::OperationError:
        emit q->error(SocketOperationError);
        break;
    case QAbstractSocket::SslInternalError:
        emit q->error(SocketSslInternalError);
        break;
    case QAbstractSocket::SslInvalidUserDataError:
        emit q->error(SocketSslInvalidUserDataError);
        break;
    case QAbstractSocket::TemporaryError:
        emit q->error(SocketTemporaryError);
        break;
    default:
        emit q->error(UnknownError);
        break;
    }
}
