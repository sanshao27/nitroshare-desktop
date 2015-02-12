/**
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Nathan Osman
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
 **/

#ifndef NS_SOCKETSENDER_H
#define NS_SOCKETSENDER_H

#include <QHostAddress>

#include "../device/device.h"
#include "../filesystem/bundle.h"
#include "socket.h"

class SocketSender : public Socket
{
    Q_OBJECT

public:

    SocketSender(const Device *device, BundlePointer bundle);

private:

    virtual void initialize();
    virtual void processPacket(const QByteArray &data);
    virtual void writeNextPacket();

    void writeTransferHeader();
    void writeFileHeader();
    void writeFile();

    // Data specific to a transfer that sends files
    QHostAddress mAddress;
    quint16 mPort;
    BundlePointer mBundle;

    // Current transfer state
    enum {
        WritingTransferHeader,
        WritingFileHeader,
        WritingFile,
        Finished
    } mState;

    // Iterator pointing to the file currently being written
    Bundle::const_iterator mIterator;

    // Data for the file currently being written
    QFile mFile;
    qint64 mFileBytesRemaining;
    QByteArray mFileBuffer;
};

#endif // NS_SOCKETSENDER_H
