/***************************************************************************
 *   Copyright (C) 2005-2014 by the Quassel Project                        *
 *   devel@quassel-irc.org                                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) version 3.                                           *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.         *
 ***************************************************************************/

#include <QHostAddress>
#include <QTcpSocket>

#include "datastreampeer.h"
#include "quassel.h"

using namespace Protocol;

DataStreamPeer::DataStreamPeer(::AuthHandler *authHandler, QTcpSocket *socket, quint16 features, QObject *parent)
    : RemotePeer(authHandler, socket, parent),
    _blockSize(0),
    _useCompression(false)
{
    Q_UNUSED(features);

    _stream.setDevice(socket);
    _stream.setVersion(QDataStream::Qt_4_2);
}


void DataStreamPeer::setSignalProxy(::SignalProxy *proxy)
{
    RemotePeer::setSignalProxy(proxy);

    // FIXME only in compat mode
    if (proxy) {
        // enable compression now if requested - the initial handshake is uncompressed in the legacy protocol!
        _useCompression = socket()->property("UseCompression").toBool();
        if (_useCompression)
            qDebug() << "Using compression for peer:" << qPrintable(socket()->peerAddress().toString());
    }

}


quint16 DataStreamPeer::supportedFeatures()
{
    return 0;
}


bool DataStreamPeer::acceptsFeatures(quint16 peerFeatures)
{
    Q_UNUSED(peerFeatures);
    return true;
}


quint16 DataStreamPeer::enabledFeatures() const
{
    return 0;
}


void DataStreamPeer::onSocketDataAvailable()
{
    QVariant item;
    while (readSocketData(item)) {
        // if no sigproxy is set, we're in handshake mode and let the data be handled elsewhere
        if (!signalProxy())
            handleHandshakeMessage(item);
        else
            handlePackedFunc(item);
    }
}


bool DataStreamPeer::readSocketData(QVariant &item)
{
    if (_blockSize == 0) {
        if (socket()->bytesAvailable() < 4)
            return false;
        _stream >> _blockSize;
    }

    if (_blockSize > 1 << 22) {
        close("Peer tried to send package larger than max package size!");
        return false;
    }

    if (_blockSize == 0) {
        close("Peer tried to send 0 byte package!");
        return false;
    }

    if (socket()->bytesAvailable() < _blockSize) {
        emit transferProgress(socket()->bytesAvailable(), _blockSize);
        return false;
    }

    emit transferProgress(_blockSize, _blockSize);

    _blockSize = 0;

    if (_useCompression) {
        QByteArray rawItem;
        _stream >> rawItem;

        int nbytes = rawItem.size();
        if (nbytes <= 4) {
            const char *data = rawItem.constData();
            if (nbytes < 4 || (data[0] != 0 || data[1] != 0 || data[2] != 0 || data[3] != 0)) {
                close("Peer sent corrupted compressed data!");
                return false;
            }
        }

        rawItem = qUncompress(rawItem);

        QDataStream itemStream(&rawItem, QIODevice::ReadOnly);
        itemStream.setVersion(QDataStream::Qt_4_2);
        itemStream >> item;
    }
    else {
        _stream >> item;
    }

    if (!item.isValid()) {
        close("Peer sent corrupt data: unable to load QVariant!");
        return false;
    }

    return true;
}


void DataStreamPeer::writeSocketData(const QVariant &item)
{
    if (!socket()->isOpen()) {
        qWarning() << Q_FUNC_INFO << "Can't write to a closed socket!";
        return;
    }

    QByteArray block;
    QDataStream out(&block, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_4_2);

    if (_useCompression) {
        QByteArray rawItem;
        QDataStream itemStream(&rawItem, QIODevice::WriteOnly);
        itemStream.setVersion(QDataStream::Qt_4_2);
        itemStream << item;

        rawItem = qCompress(rawItem);

        out << rawItem;
    }
    else {
        out << item;
    }

    _stream << block;  // also writes the length as part of the serialization format
}


/*** Handshake messages ***/

/* These messages are transmitted during handshake phase, which in case of the legacy protocol means they have
 * a structure different from those being used after the handshake.
 * Also, the legacy handshake does not fully match the redesigned one, so we'll have to do various mappings here.
 */

void DataStreamPeer::handleHandshakeMessage(const QVariant &msg)
{
    QVariantMap m = msg.toMap();

    QString msgType = m["MsgType"].toString();
    if (msgType.isEmpty()) {
        emit protocolError(tr("Invalid handshake message!"));
        return;
    }

    if (msgType == "ClientInit") {
#ifndef QT_NO_COMPRESS
        // FIXME only in compat mode
        if (m["UseCompression"].toBool()) {
            socket()->setProperty("UseCompression", true);
        }
#endif
        handle(RegisterClient(m["ClientVersion"].toString(), false)); // UseSsl obsolete
    }

    else if (msgType == "ClientInitReject") {
        handle(ClientDenied(m["Error"].toString()));
    }

    else if (msgType == "ClientInitAck") {
#ifndef QT_NO_COMPRESS
        if (m["SupportsCompression"].toBool())
            socket()->setProperty("UseCompression", true);
#endif
        handle(ClientRegistered(m["CoreFeatures"].toUInt(), m["Configured"].toBool(), m["StorageBackends"].toList(), false, QDateTime())); // SupportsSsl and coreStartTime obsolete
    }

    else if (msgType == "CoreSetupData") {
        QVariantMap map = m["SetupData"].toMap();
        handle(SetupData(map["AdminUser"].toString(), map["AdminPasswd"].toString(), map["Backend"].toString(), map["ConnectionProperties"].toMap()));
    }

    else if (msgType == "CoreSetupReject") {
        handle(SetupFailed(m["Error"].toString()));
    }

    else if (msgType == "CoreSetupAck") {
        handle(SetupDone());
    }

    else if (msgType == "ClientLogin") {
        handle(Login(m["User"].toString(), m["Password"].toString()));
    }

    else if (msgType == "ClientLoginReject") {
        handle(LoginFailed(m["Error"].toString()));
    }

    else if (msgType == "ClientLoginAck") {
        handle(LoginSuccess());
    }

    else if (msgType == "SessionInit") {
        QVariantMap map = m["SessionState"].toMap();
        handle(SessionState(map["Identities"].toList(), map["BufferInfos"].toList(), map["NetworkIds"].toList()));
    }

    else {
        emit protocolError(tr("Unknown protocol message of type %1").arg(msgType));
    }
}


void DataStreamPeer::dispatch(const RegisterClient &msg) {
    QVariantMap m;
    m["MsgType"] = "ClientInit";
    m["ClientVersion"] = msg.clientVersion;
    m["ClientDate"] = Quassel::buildInfo().buildDate;

    writeSocketData(m);
}


void DataStreamPeer::dispatch(const ClientDenied &msg) {
    QVariantMap m;
    m["MsgType"] = "ClientInitReject";
    m["Error"] = msg.errorString;

    writeSocketData(m);
}


void DataStreamPeer::dispatch(const ClientRegistered &msg) {
    QVariantMap m;
    m["MsgType"] = "ClientInitAck";
    m["CoreFeatures"] = msg.coreFeatures;
    m["StorageBackends"] = msg.backendInfo;
    m["LoginEnabled"] = m["Configured"] = msg.coreConfigured;

    writeSocketData(m);
}


void DataStreamPeer::dispatch(const SetupData &msg)
{
    QVariantMap map;
    map["AdminUser"] = msg.adminUser;
    map["AdminPasswd"] = msg.adminPassword;
    map["Backend"] = msg.backend;
    map["ConnectionProperties"] = msg.setupData;

    QVariantMap m;
    m["MsgType"] = "CoreSetupData";
    m["SetupData"] = map;
    writeSocketData(m);
}


void DataStreamPeer::dispatch(const SetupFailed &msg)
{
    QVariantMap m;
    m["MsgType"] = "CoreSetupReject";
    m["Error"] = msg.errorString;

    writeSocketData(m);
}


void DataStreamPeer::dispatch(const SetupDone &msg)
{
    Q_UNUSED(msg)

    QVariantMap m;
    m["MsgType"] = "CoreSetupAck";

    writeSocketData(m);
}


void DataStreamPeer::dispatch(const Login &msg)
{
    QVariantMap m;
    m["MsgType"] = "ClientLogin";
    m["User"] = msg.user;
    m["Password"] = msg.password;

    writeSocketData(m);
}


void DataStreamPeer::dispatch(const LoginFailed &msg)
{
    QVariantMap m;
    m["MsgType"] = "ClientLoginReject";
    m["Error"] = msg.errorString;

    writeSocketData(m);
}


void DataStreamPeer::dispatch(const LoginSuccess &msg)
{
    Q_UNUSED(msg)

    QVariantMap m;
    m["MsgType"] = "ClientLoginAck";

    writeSocketData(m);
}


void DataStreamPeer::dispatch(const SessionState &msg)
{
    QVariantMap m;
    m["MsgType"] = "SessionInit";

    QVariantMap map;
    map["BufferInfos"] = msg.bufferInfos;
    map["NetworkIds"] = msg.networkIds;
    map["Identities"] = msg.identities;
    m["SessionState"] = map;

    writeSocketData(m);
}


/*** Standard messages ***/

void DataStreamPeer::handlePackedFunc(const QVariant &packedFunc)
{
    QVariantList params(packedFunc.toList());

    if (params.isEmpty()) {
        qWarning() << Q_FUNC_INFO << "Received incompatible data:" << packedFunc;
        return;
    }

    // TODO: make sure that this is a valid request type
    RequestType requestType = (RequestType)params.takeFirst().value<int>();
    switch (requestType) {
        case Sync: {
            if (params.count() < 3) {
                qWarning() << Q_FUNC_INFO << "Received invalid sync call:" << params;
                return;
            }
            QByteArray className = params.takeFirst().toByteArray();
            QString objectName = params.takeFirst().toString();
            QByteArray slotName = params.takeFirst().toByteArray();
            handle(Protocol::SyncMessage(className, objectName, slotName, params));
            break;
        }
        case RpcCall: {
            if (params.empty()) {
                qWarning() << Q_FUNC_INFO << "Received empty RPC call!";
                return;
            }
            QByteArray slotName = params.takeFirst().toByteArray();
            handle(Protocol::RpcCall(slotName, params));
            break;
        }
        case InitRequest: {
            if (params.count() != 2) {
                qWarning() << Q_FUNC_INFO << "Received invalid InitRequest:" << params;
                return;
            }
            QByteArray className = params[0].toByteArray();
            QString objectName = params[1].toString();
            handle(Protocol::InitRequest(className, objectName));
            break;
        }
        case InitData: {
            if (params.count() != 3) {
                qWarning() << Q_FUNC_INFO << "Received invalid InitData:" << params;
                return;
            }
            QByteArray className = params[0].toByteArray();
            QString objectName = params[1].toString();
            QVariantMap initData = params[2].toMap();
            handle(Protocol::InitData(className, objectName, initData));
            break;
        }
        case HeartBeat: {
            if (params.count() != 1) {
                qWarning() << Q_FUNC_INFO << "Received invalid HeartBeat:" << params;
                return;
            }
            // The legacy protocol would only send a QTime, no QDateTime
            // so we assume it's sent today, which works in exactly the same cases as it did in the old implementation
            QDateTime dateTime = QDateTime::currentDateTime().toUTC();
            dateTime.setTime(params[0].toTime());
            handle(Protocol::HeartBeat(dateTime));
            break;
        }
        case HeartBeatReply: {
            if (params.count() != 1) {
                qWarning() << Q_FUNC_INFO << "Received invalid HeartBeat:" << params;
                return;
            }
            // The legacy protocol would only send a QTime, no QDateTime
            // so we assume it's sent today, which works in exactly the same cases as it did in the old implementation
            QDateTime dateTime = QDateTime::currentDateTime().toUTC();
            dateTime.setTime(params[0].toTime());
            handle(Protocol::HeartBeatReply(dateTime));
            break;
        }

    }
}


void DataStreamPeer::dispatch(const Protocol::SyncMessage &msg)
{
    dispatchPackedFunc(QVariantList() << (qint16)Sync << msg.className << msg.objectName << msg.slotName << msg.params);
}


void DataStreamPeer::dispatch(const Protocol::RpcCall &msg)
{
    dispatchPackedFunc(QVariantList() << (qint16)RpcCall << msg.slotName << msg.params);
}


void DataStreamPeer::dispatch(const Protocol::InitRequest &msg)
{
    dispatchPackedFunc(QVariantList() << (qint16)InitRequest << msg.className << msg.objectName);
}


void DataStreamPeer::dispatch(const Protocol::InitData &msg)
{
    dispatchPackedFunc(QVariantList() << (qint16)InitData << msg.className << msg.objectName << msg.initData);
}


void DataStreamPeer::dispatch(const Protocol::HeartBeat &msg)
{
    dispatchPackedFunc(QVariantList() << (qint16)HeartBeat << msg.timestamp.time());
}


void DataStreamPeer::dispatch(const Protocol::HeartBeatReply &msg)
{
    dispatchPackedFunc(QVariantList() << (qint16)HeartBeatReply << msg.timestamp.time());
}


void DataStreamPeer::dispatchPackedFunc(const QVariantList &packedFunc)
{
    writeSocketData(QVariant(packedFunc));
}
