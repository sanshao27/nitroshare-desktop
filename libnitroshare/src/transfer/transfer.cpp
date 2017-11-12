/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2017 Nathan Osman
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <cstring>

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QtEndian>

#include <nitroshare/application.h>
#include <nitroshare/bundle.h>
#include <nitroshare/handler.h>
#include <nitroshare/handlerregistry.h>
#include <nitroshare/item.h>
#include <nitroshare/jsonutil.h>
#include <nitroshare/logger.h>
#include <nitroshare/message.h>
#include <nitroshare/packet.h>
#include <nitroshare/transfer.h>
#include <nitroshare/transport.h>

#include "transfer_p.h"

const QString MessageTag = "transfer";

TransferPrivate::TransferPrivate(Transfer *parent,
                                 Application *application,
                                 Transport *transport,
                                 Bundle *bundle)
    : QObject(parent),
      q(parent),
      application(application),
      transport(transport),
      bundle(bundle),
      protocolState(TransferHeader),
      direction(bundle ? Transfer::Send : Transfer::Receive),
      state(bundle ? Transfer::Connecting : Transfer::InProgress),
      progress(0),
      itemIndex(0),
      itemCount(bundle ? bundle->rowCount(QModelIndex()) : 0),
      bytesTransferred(0),
      bytesTotal(bundle ? bundle->totalSize() : 0),
      currentItem(nullptr),
      currentItemBytesTransferred(0),
      currentItemBytesTotal(0)
{
    // If sending data, trigger the first packet after connection
    if (direction == Transfer::Send) {
        connect(transport, &Transport::connected, this, [&]() {
            emit q->stateChanged(state = Transfer::InProgress);
            onPacketSent();
        });
    }

    connect(transport, &Transport::packetReceived, this, &TransferPrivate::onPacketReceived);
    connect(transport, &Transport::packetSent, this, &TransferPrivate::onPacketSent);
    connect(transport, &Transport::error, this, &TransferPrivate::onError);
}

void TransferPrivate::sendTransferHeader()
{
    QJsonObject object{
        { "name", application->deviceName() },
        { "count", QString::number(bundle->rowCount(QModelIndex())) },
        { "size", QString::number(bundle->totalSize()) }
    };

    Packet packet(Packet::Json, QJsonDocument(object).toJson());
    transport->sendPacket(&packet);

    // The next packet will be an item header
    protocolState = ItemHeader;
}

void TransferPrivate::sendItemHeader()
{
    // Grab the next item and attempt to open it
    currentItem = bundle->index(itemIndex, 0).data(Qt::UserRole).value<Item*>();
    if (!currentItem->open(Item::Read)) {
        setError(tr("unable to open \"%1\" for reading").arg(currentItem->name()), true);
        return;
    }

    // Reset transfer stats
    currentItemBytesTransferred = 0;
    currentItemBytesTotal = currentItem->size();

    // Build a JSON object with all of the properties
    QJsonObject object = JsonUtil::objectToJson(currentItem);

    // Send the item header
    Packet packet(Packet::Json, QJsonDocument(object).toJson());
    transport->sendPacket(&packet);

    // If the item has a size, switch states; otherwise send the next item
    if (currentItemBytesTotal) {
        protocolState = ItemContent;
    } else {
        sendNext();
    }
}

void TransferPrivate::sendItemContent()
{
    QByteArray data = currentItem->read();
    Packet packet(Packet::Binary, data);
    transport->sendPacket(&packet);

    // Increment the number of bytes written to the socket
    bytesTransferred += data.length();
    currentItemBytesTransferred += data.length();

    updateProgress();

    // If the item completed, send the next one
    if (currentItemBytesTransferred >= currentItemBytesTotal) {
        sendNext();
    }
}

void TransferPrivate::sendNext()
{
    // Close the current item and increment the index
    currentItem->close();
    ++itemIndex;

    // If all items have been sent, move to the finished state and wait for
    // the success packet; otherwise, prepare to send the next item
    if (itemIndex == itemCount) {
        protocolState = Finished;
    } else {
        protocolState = ItemHeader;
    }
}

void TransferPrivate::processTransferHeader(Packet *packet)
{
    QJsonParseError error;
    QJsonObject object = QJsonDocument::fromJson(packet->content(), &error).object();
    if (error.error != QJsonParseError::NoError) {
        setError(QString("transfer header: %1").arg(error.errorString()), true);
        return;
    }

    // If the device name was provided, use it
    deviceName = object.value("name").toString();
    if (!deviceName.isEmpty()) {
        emit q->deviceNameChanged(deviceName);
    }

    // Strings must be used for 64-bit numbers
    itemCount = object.value("count").toString().toInt();
    bytesTotal = object.value("size").toString().toLongLong();

    // Prepare to receive the first item
    protocolState = ItemHeader;
}

void TransferPrivate::processItemHeader(Packet *packet)
{
    QJsonParseError error;
    QJsonObject object = QJsonDocument::fromJson(packet->content(), &error).object();
    if (error.error != QJsonParseError::NoError) {
        setError(QString("item header: %1").arg(error.errorString()), true);
        return;
    }

    // In order to maintain compatibility with legacy versions (which is very
    // desirable), if "type" is not in the object, assume "file" unless
    // "directory" is present (in which case, use that)
    QString type;
    if (object.contains("type")) {
        type = object.value("type").toString();
    } else {
        if (object.contains("directory")) {
            type = "directory";
        } else {
            type = "file";
        }
    }

    // Attempt to locate a handler for the type
    Handler *handler = application->handlerRegistry()->find(type);
    if (!handler) {
        setError(tr("unrecognized item type \"%1\"").arg(type), true);
        return;
    }

    // Use the handler to create an item and open it
    currentItem = handler->createItem(type, object.toVariantMap());
    currentItem->setParent(this);
    if (!currentItem->open(Item::Write)) {
        setError(tr("unable to open \"%1\" for writing").arg(currentItem->name()), true);
        return;
    }

    // Reset transfer stats
    currentItemBytesTransferred = 0;
    currentItemBytesTotal = currentItem->size();

    // If the item has a size, switch states; otherwise receive the next item
    if (currentItemBytesTotal) {
        protocolState = ItemContent;
    } else {
        processNext();
    }
}

void TransferPrivate::processItemContent(Packet *packet)
{
    currentItem->write(packet->content());

    // Add the number of bytes to the global & current item totals
    bytesTransferred += packet->content().size();
    currentItemBytesTransferred += packet->content().size();

    updateProgress();

    // If the current item is complete, advance to the next item or finish
    if (currentItemBytesTransferred >= currentItemBytesTotal) {
        processNext();
    }
}

void TransferPrivate::processNext()
{
    // Close & free the current item and increment the index
    currentItem->close();
    delete currentItem;
    ++itemIndex;

    // If there are no more items, send the success packet
    if (itemIndex == itemCount) {
        setSuccess(true);
    } else {
        protocolState = ItemHeader;
    }
}

void TransferPrivate::updateProgress()
{
    int newProgress = 0;
    if (bytesTotal) {
        newProgress = static_cast<int>(100.0 *
            static_cast<double>(bytesTransferred) /
            static_cast<double>(bytesTotal));
    }

    // Only update progress if it has actually changed
    if (newProgress != progress) {
        emit q->progressChanged(progress = newProgress);
    }
}

void TransferPrivate::setSuccess(bool send)
{
    if (send) {
        Packet packet(Packet::Success);
        transport->sendPacket(&packet);
    }

    emit q->stateChanged(state = Transfer::Succeeded);

    // Both peers should be aware that the transfer succeeded at this point
    transport->close();
}

void TransferPrivate::setError(const QString &message, bool send)
{
    application->logger()->log(new Message(
        Message::Error,
        MessageTag,
        message
    ));

    if (send) {
        Packet packet(Packet::Error, message.toUtf8());
        transport->sendPacket(&packet);
    }

    emit q->errorChanged(error = message);
    emit q->stateChanged(state = Transfer::Failed);

    // An error on either end necessitates the transport be closed
    transport->close();

    // The protocol dictates that the transfer is now "finished"
    protocolState = Finished;
}

void TransferPrivate::onPacketReceived(Packet *packet)
{
    // If an error packet is received, set the error and quit
    if (packet->type() == Packet::Error) {
        setError(packet->content());
        return;
    }

    if (direction == Transfer::Send) {

        // The only packet expected when sending items is the success packet
        // which indicates the receiver got all of the files
        if (protocolState == Finished && packet->type() == Packet::Success) {
            setSuccess();
            return;
        }

    } else {

        // Dispatch the packet to the appropriate method based on state
        switch (protocolState) {
        case TransferHeader:
            processTransferHeader(packet);
            return;
        case ItemHeader:
            processItemHeader(packet);
            return;
        case ItemContent:
            processItemContent(packet);
            return;
        case Finished:
            return;
        }
    }

    // Any other packet was unexpected - assume this is an error
    setError(tr("protocol error - unexpected packet"), true);
}

void TransferPrivate::onPacketSent()
{
    // We don't care about sent packets when receiving data
    if (direction == Transfer::Receive) {
        return;
    }

    switch (protocolState) {
    case TransferHeader:
        sendTransferHeader();
        break;
    case ItemHeader:
        sendItemHeader();
        break;
    case ItemContent:
        sendItemContent();
        break;
    case Finished:
        break;
    }
}

void TransferPrivate::onError(const QString &message)
{
    setError(message, true);
}

Transfer::Transfer(Application *application, Transport *transport, Bundle *bundle, QObject *parent)
    : QObject(parent),
      d(new TransferPrivate(this, application, transport, bundle))
{
}

Transfer::Direction Transfer::direction() const
{
    return d->direction;
}

Transfer::State Transfer::state() const
{
    return d->state;
}

int Transfer::progress() const
{
    return d->progress;
}

QString Transfer::deviceName() const
{
    return d->deviceName;
}

QString Transfer::error() const
{
    return d->error;
}

bool Transfer::isFinished() const
{
    return d->state == Failed || d->state == Succeeded;
}

void Transfer::cancel()
{
    d->setError(tr("transfer cancelled"), true);
}
