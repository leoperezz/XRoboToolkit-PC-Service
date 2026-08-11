#include "AudioStreamServer.h"

#include <QDebug>
#include <QtEndian>

namespace {
constexpr int kHeaderSize = 36;
constexpr quint32 kMaxAudioPayload = 256u * 1024u;
constexpr qint64 kMaxPendingSdkBytes = 256 * 1024;
}

AudioStreamServer::AudioStreamServer(QObject *parent) : QObject(parent)
{
    connect(&m_deviceServer, &QTcpServer::newConnection,
            this, &AudioStreamServer::acceptDeviceConnection);
    connect(&m_sdkServer, &QTcpServer::newConnection,
            this, &AudioStreamServer::acceptSdkConnection);
}

bool AudioStreamServer::start(quint16 devicePort, quint16 sdkPort)
{
    const bool deviceOk = m_deviceServer.listen(QHostAddress::AnyIPv4, devicePort);
    const bool sdkOk = m_sdkServer.listen(QHostAddress::LocalHost, sdkPort);
    if (!deviceOk || !sdkOk) {
        qWarning() << "audio stream server failed" << m_deviceServer.errorString()
                   << m_sdkServer.errorString();
        stop();
        return false;
    }
    qDebug() << "audio stream ports: device" << devicePort << "sdk" << sdkPort;
    return true;
}

void AudioStreamServer::stop()
{
    m_deviceServer.close();
    m_sdkServer.close();
    const auto devices = m_deviceBuffers.keys();
    for (QTcpSocket *socket : devices) {
        socket->disconnectFromHost();
        socket->deleteLater();
    }
    const auto clients = m_sdkClients.values();
    for (QTcpSocket *socket : clients) {
        socket->disconnectFromHost();
        socket->deleteLater();
    }
    m_deviceBuffers.clear();
    m_sdkClients.clear();
}

void AudioStreamServer::acceptDeviceConnection()
{
    while (m_deviceServer.hasPendingConnections()) {
        QTcpSocket *socket = m_deviceServer.nextPendingConnection();
        socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        m_deviceBuffers.insert(socket, {});
        connect(socket, &QTcpSocket::readyRead, this, &AudioStreamServer::readDeviceData);
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] { removeDevice(socket); });
        qDebug() << "PICO microphone connected from" << socket->peerAddress();
    }
}

void AudioStreamServer::acceptSdkConnection()
{
    while (m_sdkServer.hasPendingConnections()) {
        QTcpSocket *socket = m_sdkServer.nextPendingConnection();
        socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        m_sdkClients.insert(socket);
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
            m_sdkClients.remove(socket);
            socket->deleteLater();
        });
    }
}

void AudioStreamServer::removeDevice(QTcpSocket *socket)
{
    m_deviceBuffers.remove(socket);
    socket->deleteLater();
}

void AudioStreamServer::readDeviceData()
{
    auto *socket = qobject_cast<QTcpSocket *>(sender());
    if (!socket || !m_deviceBuffers.contains(socket)) return;
    QByteArray &buffer = m_deviceBuffers[socket];
    buffer.append(socket->readAll());

    while (buffer.size() >= kHeaderSize) {
        if (buffer.left(4) != "XRAU") {
            qWarning() << "invalid PICO audio stream magic";
            socket->disconnectFromHost();
            return;
        }
        const auto *header = reinterpret_cast<const uchar *>(buffer.constData());
        const quint16 version = qFromBigEndian<quint16>(header + 4);
        const quint32 payloadSize = qFromBigEndian<quint32>(header + 32);
        if (version != 1 || payloadSize == 0 || payloadSize > kMaxAudioPayload) {
            qWarning() << "invalid PICO audio packet" << version << payloadSize;
            socket->disconnectFromHost();
            return;
        }
        const int packetSize = kHeaderSize + static_cast<int>(payloadSize);
        if (buffer.size() < packetSize) return;
        const QByteArray packet = buffer.left(packetSize);
        buffer.remove(0, packetSize);
        relayPacket(packet);
    }
}

void AudioStreamServer::relayPacket(const QByteArray &packet)
{
    for (QTcpSocket *client : m_sdkClients) {
        if (client->state() == QAbstractSocket::ConnectedState &&
            client->bytesToWrite() <= kMaxPendingSdkBytes) {
            client->write(packet);
        }
    }
}
