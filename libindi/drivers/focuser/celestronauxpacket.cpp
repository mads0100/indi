/*
    Celestron Focuser for SCT and EDGEHD

    Copyright (C) 2019 Chris Rowland
    Copyright (C) 2019 Jasem Mutlaq (mutlaqja@ikarustech.com)

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

*/


#include "celestronauxpacket.h"
#include "indicom.h"
#include "indilogger.h"

#include <termios.h>
#include <stdio.h>

#define SHORT_TIMEOUT 2

namespace Aux
{
// holds the string generated by toHexStr
char debugStr[301];

// free function to return the contents of a buffer as a string containing a series of hex numbers
char * toHexStr(buffer data)
{
    int sz = (int)data.size();
    if (sz > 100)
        sz = 100;
    for (int i = 0; i < sz; i++)
    {
        snprintf(&debugStr[i*3], 301, "%02X ", data[i]);
    }
    return debugStr;
}

std::string Communicator::Device;

Packet::Packet(Target source, Target destination, Command command, buffer data)
{
    this->command = command;
    this->source = source;
    this->destination = destination;
    this->data = data;
    this->length = data.size() + 3;
}

void Packet::FillBuffer(buffer &buff)
{
    buff.resize(this->length + 3);
    buff[0] = AUX_HDR;
    buff[1] = length;
    buff[2] = source;
    buff[3] = destination;
    buff[4] = command;
    for (uint32_t i = 0; i < data.size(); i++)
    {
        buff[5 + i] = data[i];
    }

    buff.back() = checksum(buff);

    DEBUGFDEVICE(Communicator::Device.c_str(), INDI::Logger::DBG_DEBUG, "fillBuffer <%s>", toHexStr(buff));
}

bool Packet::Parse(buffer packet)
{
    if (packet.size() < 6)  // must contain header, len, src, dest, cmd and checksum at least
    {
        DEBUGDEVICE(Communicator::Device.c_str(), INDI::Logger::DBG_ERROR, "Parse size < 6");
        return false;
    }
    if (packet[0] != AUX_HDR)
    {
        DEBUGDEVICE(Communicator::Device.c_str(), INDI::Logger::DBG_ERROR, "Parse [0] |= 0x3b");
        return false;
    }

    length = packet[1];
    // length must be correct
    if (packet.size() != length + 3)
    {
        DEBUGFDEVICE(Communicator::Device.c_str(), INDI::Logger::DBG_ERROR, "Parse size %i |= length+3 %i",
                     packet.size(), length + 3);
        return false;
    }

    source = static_cast<Target>(packet[2]);
    destination = static_cast<Target>(packet[3]);
    command = static_cast<Command>(packet[4]);
    data  = buffer(packet.begin() + 5, packet.end() - 1);

    uint8_t cs = checksum(packet);
    uint8_t cb = packet[length + 2];

    if (cs != cb)
    {
        DEBUGFDEVICE(Communicator::Device.c_str(), INDI::Logger::DBG_WARNING, "Parse checksum error cs %i |= cb %i", cs, cb);
    }
    return  cb == cs;
}

uint8_t Packet::checksum(buffer packet)
{
    int cs = 0;
    for (int i = 1; i < packet[1] + 2; i++)
    {
        cs += packet[i];
    }
    return (-cs & 0xff);
}

/////////////////////////////////////////////
/////////// Communicator
/////////////////////////////////////////////

Communicator::Communicator()
{
    this->source = Target::NEX_REMOTE;
}

Communicator::Communicator(Target source)
{
    this->source = source;
}

bool Communicator::sendPacket(int portFD, Target dest, Command cmd, buffer data)
{
    Packet pkt(source, dest, cmd, data);

    buffer txbuff;
    pkt.FillBuffer(txbuff);
    int ns;

    int ttyrc = 0;
    tcflush(portFD, TCIOFLUSH);
    if ( (ttyrc = tty_write(portFD, reinterpret_cast<const char *>(txbuff.data()), txbuff.size(), &ns)) != TTY_OK)
    {
        char errmsg[MAXRBUF];
        tty_error_msg(ttyrc, errmsg, MAXRBUF);
        DEBUGFDEVICE(Communicator::Device.c_str(), INDI::Logger::DBG_ERROR, "sendPacket fail tty %i %s, ns %i", ttyrc, errmsg, ns);
        return false;
    }
    return true;
}

bool Communicator::readPacket(int portFD, Packet &reply)
{
    char rxbuf[] = {0};
    int nr = 0, ttyrc = 0;
    // look for header
    while(rxbuf[0] != Packet::AUX_HDR)
    {
        if ( (ttyrc = tty_read(portFD, rxbuf, 1, SHORT_TIMEOUT, &nr) != TTY_OK))
        {
            char errmsg[MAXRBUF];
            tty_error_msg(ttyrc, errmsg, MAXRBUF);
            DEBUGFDEVICE(Communicator::Device.c_str(), INDI::Logger::DBG_ERROR,
                         "readPacket fail read hdr tty %i %s, nr %i", ttyrc, errmsg, nr);
            return false;		// read failure is instantly fatal
        }
    }
    // get length
    if (tty_read(portFD, rxbuf, 1, SHORT_TIMEOUT, &nr) != TTY_OK)
    {
        char errmsg[MAXRBUF];
        tty_error_msg(ttyrc, errmsg, MAXRBUF);
        DEBUGFDEVICE(Communicator::Device.c_str(), INDI::Logger::DBG_ERROR, "readPacket fail read len tty %i %s, ns %i", ttyrc, errmsg, nr);
        return false;
    }

    int len = rxbuf[0];
    buffer packet(2);
    packet[0] = Packet::AUX_HDR;
    packet[1] = len;

    // get source, destination, command, data and checksum
    char rxdata[MAXRBUF] = {0};
    if ( (ttyrc = tty_read(portFD, rxdata, len + 1, SHORT_TIMEOUT, &nr) != TTY_OK))
    {
        char errmsg[MAXRBUF];
        tty_error_msg(ttyrc, errmsg, MAXRBUF);
        DEBUGFDEVICE(Communicator::Device.c_str(), INDI::Logger::DBG_ERROR, "readPacket fail data tty %i %s, nr %i", ttyrc, errmsg, nr);
        return false;
    }

    packet.insert(packet.end(), rxdata, rxdata + len + 1);

    DEBUGFDEVICE(Communicator::Device.c_str(), INDI::Logger::DBG_DEBUG, "RES <%s>", toHexStr(packet));

    return reply.Parse(packet);
}

// send command with data and reply
bool Communicator::sendCommand(int portFD, Target dest, Command cmd, buffer data, buffer &reply)
{
    int num_tries = 0;

    while (num_tries++ < 3)
    {
        if (!sendPacket(portFD, dest, cmd, data))
            return false;       // failure to send is fatal

        Packet pkt;
        if (!readPacket(portFD, pkt))
            continue;           // try again

        // check the packet is the one we want
        if (pkt.command != cmd || pkt.destination != Target::APP || pkt.source != dest)
        {
            DEBUGFDEVICE(Communicator::Device.c_str(), INDI::Logger::DBG_ERROR, "sendCommand pkt.command %i cmd %i, pkt.destination %i pkt.source %i dest %i",
                         pkt.command, cmd, pkt.destination, pkt.source, dest);
            continue;           // wrong packet, try again
        }

        reply = pkt.data;
        return true;
    }
    return false;
}

// send command with reply but no data
bool Communicator::sendCommand(int portFD, Target dest, Command cmd, buffer &reply)
{
    buffer data(0);
    return sendCommand(portFD, dest, cmd, data, reply);
}

// send command with data but no reply
bool Communicator::commandBlind(int portFD, Target dest, Command cmd, buffer data)
{
    buffer reply;
    return sendCommand(portFD, dest, cmd, data, reply);
}

}
