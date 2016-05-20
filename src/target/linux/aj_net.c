/**
 * @file
 */
/******************************************************************************
 * Copyright AllSeen Alliance. All rights reserved.
 *
 *    Permission to use, copy, modify, and/or distribute this software for any
 *    purpose with or without fee is hereby granted, provided that the above
 *    copyright notice and this permission notice appear in all copies.
 *
 *    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *    WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *    WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *    ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 ******************************************************************************/

/**
 * Per-module definition of the current module for debug logging.  Must be defined
 * prior to first inclusion of aj_debug.h
 */

#define AJ_MODULE NET
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <sys/fcntl.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <linux/in6.h>
#include <assert.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <netdb.h>
#include <ifaddrs.h>

#include <ajtcl/aj_target.h>
#include <ajtcl/aj_bufio.h>
#include <ajtcl/aj_net.h>
#include <ajtcl/aj_util.h>
#include <ajtcl/aj_debug.h>
#include <ajtcl/aj_connect.h>
#include <ajtcl/aj_bus.h>
#include <ajtcl/aj_disco.h>
#include <ajtcl/aj_config.h>
#include <ajtcl/aj_std.h>

#ifdef AJ_ARDP
#include <ajtcl/aj_ardp.h>
#endif

/**
 * Turn on per-module debug printing by setting this variable to non-zero value
 * (usually in debugger).
 */
#ifndef NDEBUG
uint8_t dbgNET = 0;
#endif

#define INVALID_SOCKET (-1)

/*
 * IANA assigned IPv4 multicast group for AllJoyn.
 */
static const char AJ_IPV4_MULTICAST_GROUP[] = "224.0.0.113";

/*
 * IANA assigned IPv6 multicast group for AllJoyn.
 */
static const char AJ_IPV6_MULTICAST_GROUP[] = "ff02::13a";

/*
 * IANA assigned UDP multicast port for AllJoyn
 */
#define AJ_UDP_PORT 9956

/*
 * IANA-assigned IPv4 multicast group for mDNS.
 */
static const char MDNS_IPV4_MULTICAST_GROUP[] = "224.0.0.251";

/*
 * IANA-assigned IPv6 multicast group for mDNS.
 */
static const char MDNS_IPV6_MULTICAST_GROUP[] = "ff02::fb";

/*
 * IANA-assigned UDP multicast port for mDNS
 */
#define MDNS_UDP_PORT 5353

/**
 * Target-specific contexts for network I/O
 */
typedef struct {
    int tcpSock;
    int udpSock;
} NetContext;

typedef struct {
    int udpSock;
    int udp6Sock;
    int mDnsSock;
    int mDns6Sock;
    int mDnsRecvSock;
    int mDns6RecvSock;
    struct sockaddr_in6 mDnsRecvAddr;
    uint16_t mDnsRecvPort;
    uint16_t mDns6RecvPort;
    int scope_id;
} MCastContext;

static NetContext netContext = { INVALID_SOCKET, INVALID_SOCKET };
static MCastContext mCastContext = { INVALID_SOCKET, INVALID_SOCKET, INVALID_SOCKET, INVALID_SOCKET };

#ifdef AJ_ARDP
/**
 * Need to predeclare a few things for ARDP
 */
static AJ_Status AJ_Net_ARDP_Connect(AJ_BusAttachment* bus, const AJ_Service* service);
static void AJ_Net_ARDP_Disconnect(AJ_NetSocket* netSock);
static AJ_Status RewriteSenderInfo(AJ_IOBuffer* buf, struct sockaddr* saddr);

#endif // AJ_ARDP

#ifdef AJ_TCP
static AJ_Status CloseNetSock(AJ_NetSocket* netSock)
{
    NetContext* context = (NetContext*)netSock->rx.context;
    if (context) {
        if (context->tcpSock != INVALID_SOCKET) {
            struct linger l;
            l.l_onoff = 1;
            l.l_linger = 0;
            setsockopt(context->tcpSock, SOL_SOCKET, SO_LINGER, (void*)&l, sizeof(l));
            shutdown(context->tcpSock, SHUT_RDWR);
            close(context->tcpSock);
        }
        context->tcpSock = INVALID_SOCKET;
        memset(netSock, 0, sizeof(AJ_NetSocket));
    }
    return AJ_OK;
}
#endif

static AJ_Status CloseMCastSock(AJ_MCastSocket* mcastSock)
{
    MCastContext* context = (MCastContext*)mcastSock->rx.context;
    if (context) {
        if (context->udpSock != INVALID_SOCKET) {
            close(context->udpSock);
        }
        if (context->udp6Sock != INVALID_SOCKET) {
            close(context->udp6Sock);
        }
        if (context->mDnsSock != INVALID_SOCKET) {
            close(context->mDnsSock);
        }
        if (context->mDns6Sock != INVALID_SOCKET) {
            close(context->mDns6Sock);
        }
        if (context->mDnsRecvSock != INVALID_SOCKET) {
            close(context->mDnsRecvSock);
        }
        if (context->mDns6RecvSock != INVALID_SOCKET) {
            close(context->mDns6RecvSock);
        }
        context->udpSock = context->udp6Sock = context->mDnsSock = context->mDns6Sock = context->mDnsRecvSock = context->mDns6RecvSock = INVALID_SOCKET;
        memset(mcastSock, 0, sizeof(AJ_MCastSocket));
    }
    return AJ_OK;
}

#ifdef AJ_TCP
AJ_Status AJ_Net_Send(AJ_IOBuffer* buf)
{
    NetContext* context = (NetContext*) buf->context;
    ssize_t ret;
    size_t tx = AJ_IO_BUF_AVAIL(buf);

    AJ_InfoPrintf(("AJ_Net_Send(buf=0x%p)\n", buf));

    assert(buf->direction == AJ_IO_BUF_TX);

    if (tx > 0) {
        ret = send(context->tcpSock, buf->readPtr, tx, MSG_NOSIGNAL);
        if (ret == -1) {
            AJ_ErrPrintf(("AJ_Net_Send(): send() failed. errno=\"%s\", status=AJ_ERR_WRITE\n", strerror(errno)));
            return AJ_ERR_WRITE;
        }
        buf->readPtr += ret;
    }
    if (AJ_IO_BUF_AVAIL(buf) == 0) {
        AJ_IO_BUF_RESET(buf);
    }

    AJ_InfoPrintf(("AJ_Net_Send(): status=AJ_OK\n"));
    return AJ_OK;
}
#endif

/*
 * An eventfd handle used for interrupting a network read blocked on select
 */
static int interruptFd = INVALID_SOCKET;

/*
 * The socket that is blocked in select
 */
static uint8_t blocked;

/*
 * This function is called to cancel a pending select.
 */
void AJ_Net_Interrupt()
{
    if (blocked) {
        uint64_t u64;
        if (write(interruptFd, &u64, sizeof(u64)) < 0) {
            AJ_ErrPrintf(("AJ_Net_Interrupt(): write() failed. errno=\"%s\"\n", strerror(errno)));
        }
    }
}

#ifdef AJ_TCP
AJ_Status AJ_Net_Recv(AJ_IOBuffer* buf, uint32_t len, uint32_t timeout)
{
    NetContext* context = (NetContext*) buf->context;
    AJ_Status status = AJ_OK;
    size_t rx = AJ_IO_BUF_SPACE(buf);
    fd_set fds;
    int rc = 0;
    int maxFd = context->tcpSock;
    struct timeval tv = { timeout / 1000, 1000 * (timeout % 1000) };

    // AJ_InfoPrintf(("AJ_Net_Recv(buf=0x%p, len=%d, timeout=%d)\n", buf, len, timeout));

    assert(buf->direction == AJ_IO_BUF_RX);

    FD_ZERO(&fds);
    FD_SET(context->tcpSock, &fds);
    if (interruptFd >= 0) {
        FD_SET(interruptFd, &fds);
        maxFd = max(maxFd, interruptFd);
    }
    blocked = TRUE;
    rc = select(maxFd + 1, &fds, NULL, NULL, &tv);
    blocked = FALSE;
    if (rc == 0) {
        return AJ_ERR_TIMEOUT;
    }
    if ((interruptFd >= 0) && FD_ISSET(interruptFd, &fds)) {
        uint64_t u64;
        if (read(interruptFd, &u64, sizeof(u64)) < 0) {
            AJ_ErrPrintf(("AJ_Net_Recv(): read() failed during interrupt. errno=\"%s\"\n", strerror(errno)));
        }
        return AJ_ERR_INTERRUPTED;
    }
    rx = min(rx, len);
    if (rx) {
        ssize_t ret = recv(context->tcpSock, buf->writePtr, rx, 0);
        if ((ret == -1) || (ret == 0)) {
            AJ_ErrPrintf(("AJ_Net_Recv(): recv() failed. errno=\"%s\"\n", strerror(errno)));
            status = AJ_ERR_READ;
        } else {
            AJ_InfoPrintf(("AJ_Net_Recv(): recv'd %d from tcp\n", ret));
            buf->writePtr += ret;
        }
    }
    return status;
}
#endif

static uint8_t rxData[AJ_RX_DATA_SIZE];
static uint8_t txData[AJ_TX_DATA_SIZE];

#ifdef AJ_TCP
static AJ_Status AJ_TCP_Connect(AJ_BusAttachment* bus, const AJ_Service* service)
{
    int ret;
    struct sockaddr_storage addrBuf;
    socklen_t addrSize;
    int tcpSock = INVALID_SOCKET;

    interruptFd = eventfd(0, O_NONBLOCK);  // Use O_NONBLOCK instead of EFD_NONBLOCK due to bug in OpenWrt's uCLibc
    if (interruptFd < 0) {
        AJ_ErrPrintf(("AJ_TCP_Connect(): failed to created interrupt event\n"));
        goto ConnectError;
    }

    memset(&addrBuf, 0, sizeof(addrBuf));

    tcpSock = socket(AF_INET, SOCK_STREAM, 0);
    if (tcpSock == INVALID_SOCKET) {
        AJ_ErrPrintf(("AJ_TCP_Connect(): socket() failed.  status=AJ_ERR_CONNECT\n"));
        goto ConnectError;
    }
    if (service->addrTypes & AJ_ADDR_TCP4) {
        struct sockaddr_in* sa = (struct sockaddr_in*)&addrBuf;
        sa->sin_family = AF_INET;
        sa->sin_port = htons(service->ipv4port);
        sa->sin_addr.s_addr = service->ipv4;
        addrSize = sizeof(struct sockaddr_in);
        AJ_InfoPrintf(("AJ_TCP_Connect(): Connect to \"%s:%u\"\n", inet_ntoa(sa->sin_addr), service->ipv4port));;
    } else if (service->addrTypes & AJ_ADDR_TCP6) {
        struct sockaddr_in6* sa = (struct sockaddr_in6*)&addrBuf;
        sa->sin6_family = AF_INET6;
        sa->sin6_port = htons(service->ipv6port);
        memcpy(sa->sin6_addr.s6_addr, service->ipv6, sizeof(sa->sin6_addr.s6_addr));
        addrSize = sizeof(struct sockaddr_in6);
    } else {
        AJ_ErrPrintf(("AJ_TCP_Connect(): Invalid addrTypes %u, status=AJ_ERR_CONNECT\n", service->addrTypes));
        goto ConnectError;
    }


    ret = connect(tcpSock, (struct sockaddr*)&addrBuf, addrSize);
    if (ret < 0) {
        AJ_ErrPrintf(("AJ_TCP_Connect(): connect() failed. errno=\"%s\", status=AJ_ERR_CONNECT\n", strerror(errno)));
        goto ConnectError;
    } else {
        netContext.tcpSock = tcpSock;
        AJ_IOBufInit(&bus->sock.rx, rxData, sizeof(rxData), AJ_IO_BUF_RX, &netContext);
        bus->sock.rx.recv = AJ_Net_Recv;
        AJ_IOBufInit(&bus->sock.tx, txData, sizeof(txData), AJ_IO_BUF_TX, &netContext);
        bus->sock.tx.send = AJ_Net_Send;
        AJ_InfoPrintf(("AJ_TCP_Connect(): status=AJ_OK\n"));
    }

    return AJ_OK;

ConnectError:
    if (interruptFd != INVALID_SOCKET) {
        close(interruptFd);
        interruptFd = INVALID_SOCKET;
    }

    if (tcpSock != INVALID_SOCKET) {
        close(tcpSock);
    }

    return AJ_ERR_CONNECT;
}
#endif


AJ_Status AJ_Net_Connect(AJ_BusAttachment* bus, const AJ_Service* service)
{
    AJ_Status status = AJ_ERR_CONNECT;

    AJ_InfoPrintf(("AJ_Net_Connect(bus=0x%p, addrType=%d.)\n", bus, service->addrTypes));

#ifdef AJ_ARDP
    if (service->addrTypes & (AJ_ADDR_UDP4 | AJ_ADDR_UDP6)) {
        status = AJ_Net_ARDP_Connect(bus, service);
        if (status == AJ_OK) {
            return status;
        }
    }
#endif

#ifdef AJ_TCP
    if (service->addrTypes & (AJ_ADDR_TCP4 | AJ_ADDR_TCP6)) {
        status = AJ_TCP_Connect(bus, service);
    }
#endif

    return status;
}

void AJ_Net_Disconnect(AJ_NetSocket* netSock)
{
    if (interruptFd >= 0) {
        close(interruptFd);
        interruptFd = INVALID_SOCKET;
    }

    if (netContext.udpSock != INVALID_SOCKET) {
#ifdef AJ_ARDP
        // we are using UDP!
        AJ_Net_ARDP_Disconnect(netSock);
        memset(netSock, 0, sizeof(AJ_NetSocket));
#endif
    } else if (netContext.tcpSock != INVALID_SOCKET) {
#ifdef AJ_TCP
        CloseNetSock(netSock);
#endif
    }
}

static uint8_t sendToBroadcast(int sock, uint16_t port, AJ_IOBuffer* buf, size_t tx)
{
    ssize_t ret = -1;
    uint8_t sendSucceeded = FALSE;
    struct ifaddrs* addrs;
    struct ifaddrs* addr;

    getifaddrs(&addrs);
    addr = addrs;

    while (addr != NULL) {
        if (addr->ifa_addr != NULL &&
            ((addr->ifa_addr->sa_family == AF_INET) || (AF_INET6 == addr->ifa_addr->sa_family)) &&
            (addr->ifa_flags & IFF_UP) && (addr->ifa_flags & IFF_RUNNING)) {
            char addrbuf[INET_ADDRSTRLEN];
            struct sockaddr_in* sin_bcast = (struct sockaddr_in*) addr->ifa_ifu.ifu_broadaddr;

            if (NULL != sin_bcast) {
                sin_bcast->sin_port = htons(port);
                ((struct sockaddr_in*)addr->ifa_addr)->sin_port = htons(port);
                inet_ntop(AF_INET, &(sin_bcast->sin_addr), addrbuf, sizeof(addrbuf));
                AJ_InfoPrintf(("sendToBroadcast: sending to bcast addr %s\n", addrbuf));
                if (buf->flags & AJ_IO_BUF_MDNS) {
                    if (RewriteSenderInfo(buf, (struct sockaddr*)addr->ifa_addr) != AJ_OK) {
                        AJ_WarnPrintf(("AJ_Net_SendTo(): RewriteSenderInfo failed.\n"));
                    } else {
                        tx = AJ_IO_BUF_AVAIL(buf);
                        ret = sendto(sock, buf->readPtr, tx, MSG_NOSIGNAL, (struct sockaddr*) sin_bcast, sizeof(struct sockaddr_in));
                        if (tx == ret) {
                            sendSucceeded = TRUE;
                        } else {
                            AJ_ErrPrintf(("sendToBroadcast(): sendto failed. errno=\"%s\"\n", strerror(errno)));
                        }
                    }
                }
            }
        }

        addr = addr->ifa_next;
    }
    freeifaddrs(addrs);
    return sendSucceeded;
}

static AJ_Status RewriteSenderInfo(AJ_IOBuffer* buf, struct sockaddr* saddr)
{
    uint16_t sidVal;
    const char snd[4] = { 'd', 'n', 'e', 's' };
    const char sid[] = { 's', 'i', 'd', '=' };
    static char ipv[] = { 'i', 'p', 'v', '4', '=' };
    static char upcv[] = { 'u', 'p', 'c', 'v', '4', '=' };
    char sidStr[6];
    char ipvStr[40];
    char upcvStr[6];
    uint8_t* pkt;
    uint16_t dataLength;
    int match;
    AJ_Status status;

    // first, pluck the search ID from the mDNS header
    sidVal = *(buf->readPtr) << 8;
    sidVal += *(buf->readPtr + 1);

    // convert to strings
    status = AJ_IntToString((int32_t) sidVal, sidStr, sizeof(sidStr));
    if (status != AJ_OK) {
        return AJ_ERR_WRITE;
    }
    if (saddr->sa_family == AF_INET) {
        upcv[4] = '4';
        ipv[3] = '4';
        status = AJ_IntToString((int32_t)htons(((struct sockaddr_in*)saddr)->sin_port), upcvStr, sizeof(upcvStr));
        if (status != AJ_OK) {
            return AJ_ERR_WRITE;
        }
        status = AJ_InetToString(htonl(((struct sockaddr_in*)saddr)->sin_addr.s_addr), ipvStr, sizeof(ipvStr));
        if (status != AJ_OK) {
            return AJ_ERR_WRITE;
        }
    } else if (saddr->sa_family == AF_INET6) {
        upcv[4] = '6';
        ipv[3] = '6';
        status = AJ_IntToString((int32_t)htons(((struct sockaddr_in6*)saddr)->sin6_port), upcvStr, sizeof(upcvStr));
        if (status != AJ_OK) {
            return AJ_ERR_WRITE;
        }
        if (NULL == inet_ntop(AF_INET6, &((struct sockaddr_in6*)saddr)->sin6_addr, ipvStr, sizeof(ipvStr))) {
            return AJ_ERR_WRITE;
        }
    }
    // ASSUMPTIONS: sender-info resource record is the final resource record in the packet.
    // sid, ipv4, and upcv4 key value pairs are the final three key/value pairs in the record.
    // The length of the other fields in the record are static.
    //
    // search backwards through packet to find the start of "sender-info"
    pkt = buf->writePtr;
    match = 0;
    do {
        if (*(pkt--) == snd[match]) {
            match++;
        } else {
            match = 0;
        }
    } while (pkt != buf->readPtr && match != 4);
    if (match != 4) {
        return AJ_ERR_WRITE;
    }

    // move forward to the Data Length field
    pkt += 22;

    // actual data length is the length of the static values already in the buffer plus
    // the three dynamic key-value pairs to re-write
    dataLength = 23 + 1 + sizeof(sid) + strlen(sidStr) + 1 + sizeof(ipv) + strlen(ipvStr) + 1 + sizeof(upcv) + strlen(upcvStr);
    *pkt++ = (dataLength >> 8) & 0xFF;
    *pkt++ = dataLength & 0xFF;

    // move forward past the static key-value pairs
    pkt += 23;

    // ASSERT: must be at the start of "sid="
    assert(*(pkt + 1) == 's');

    // re-write new values
    *pkt++ = sizeof(sid) + strlen(sidStr);
    memcpy(pkt, sid, sizeof(sid));
    pkt += sizeof(sid);
    memcpy(pkt, sidStr, strlen(sidStr));
    pkt += strlen(sidStr);

    *pkt++ = sizeof(ipv) + strlen(ipvStr);
    memcpy(pkt, ipv, sizeof(ipv));
    pkt += sizeof(ipv);
    memcpy(pkt, ipvStr, strlen(ipvStr));
    pkt += strlen(ipvStr);

    *pkt++ = sizeof(upcv) + strlen(upcvStr);
    memcpy(pkt, upcv, sizeof(upcv));
    pkt += sizeof(upcv);
    memcpy(pkt, upcvStr, strlen(upcvStr));
    pkt += strlen(upcvStr);

    buf->writePtr = pkt;

    return AJ_OK;
}

AJ_Status AJ_Net_SendTo(AJ_IOBuffer* buf)
{
    ssize_t ret = -1;
    uint8_t sendSucceeded = FALSE;
    size_t tx = AJ_IO_BUF_AVAIL(buf);
    MCastContext* context = (MCastContext*) buf->context;
    AJ_InfoPrintf(("AJ_Net_SendTo(buf=0x%p)\n", buf));
    assert(buf->direction == AJ_IO_BUF_TX);

    if (tx > 0) {
        if ((context->udpSock != INVALID_SOCKET) && (buf->flags & AJ_IO_BUF_AJ)) {
            struct sockaddr_in sin;
            sin.sin_family = AF_INET;
            sin.sin_port = htons(AJ_UDP_PORT);

            if (inet_pton(AF_INET, AJ_IPV4_MULTICAST_GROUP, &sin.sin_addr) == 1) {
                ret = sendto(context->udpSock, buf->readPtr, tx, MSG_NOSIGNAL, (struct sockaddr*)&sin, sizeof(sin));
                if (tx == ret) {
                    sendSucceeded = TRUE;
                } else {
                    AJ_ErrPrintf(("AJ_Net_SendTo(): sendto AJ IPv4 failed. errno=\"%s\"\n", strerror(errno)));
                }
            } else {
                AJ_ErrPrintf(("AJ_Net_SendTo(): Invalid AJ IP address. errno=\"%s\"\n", strerror(errno)));
            }

            if (sendToBroadcast(context->udpSock, AJ_UDP_PORT, buf, tx) == TRUE) {
                sendSucceeded = TRUE;
            } // leave sendSucceeded unchanged if FALSE
        }

        // now sendto the ipv6 address
        if ((context->udp6Sock != INVALID_SOCKET) && (buf->flags & AJ_IO_BUF_AJ)) {
            struct sockaddr_in6 sin6;
            sin6.sin6_family = AF_INET6;
            sin6.sin6_flowinfo = 0;
            sin6.sin6_scope_id = 0;
            sin6.sin6_port = htons(AJ_UDP_PORT);
            if (inet_pton(AF_INET6, AJ_IPV6_MULTICAST_GROUP, &sin6.sin6_addr) == 1) {
                ret = sendto(context->udp6Sock, buf->readPtr, tx, MSG_NOSIGNAL, (struct sockaddr*) &sin6, sizeof(sin6));
                if (tx == ret) {
                    sendSucceeded = TRUE;
                } else {
                    AJ_ErrPrintf(("AJ_Net_SendTo(): sendto AJ IPv6 failed. errno=\"%s\"\n", strerror(errno)));
                }
            } else {
                AJ_ErrPrintf(("AJ_Net_SendTo(): Invalid AJ IPv6 address. errno=\"%s\"\n", strerror(errno)));
            }
        }
    }

    if (buf->flags & AJ_IO_BUF_MDNS) {
        if (RewriteSenderInfo(buf, (struct sockaddr*)&context->mDnsRecvAddr) != AJ_OK) {
            AJ_WarnPrintf(("AJ_Net_SendTo(): RewriteSenderInfo failed.\n"));
            tx = 0;
        } else {
            tx = AJ_IO_BUF_AVAIL(buf);
        }
    }

    if (tx > 0) {
        if ((context->mDnsSock != INVALID_SOCKET) && (buf->flags & AJ_IO_BUF_MDNS)) {
            struct sockaddr_in sin;
            sin.sin_family = AF_INET;
            sin.sin_port = htons(MDNS_UDP_PORT);

            if (inet_pton(AF_INET, MDNS_IPV4_MULTICAST_GROUP, &sin.sin_addr) == 1) {
                ret = sendto(context->mDnsSock, buf->readPtr, tx, MSG_NOSIGNAL, (struct sockaddr*)&sin, sizeof(sin));
                if (tx == ret) {
                    sendSucceeded = TRUE;
                } else {
                    AJ_ErrPrintf(("AJ_Net_SendTo(): sendto mDNS IPv4 failed. errno=\"%s\"\n", strerror(errno)));
                }
            } else {
                AJ_ErrPrintf(("AJ_Net_SendTo(): Invalid mDNS IP address. errno=\"%s\"\n", strerror(errno)));
            }

            if (sendToBroadcast(context->mDnsSock, MDNS_UDP_PORT, buf, tx) == TRUE) {
                sendSucceeded = TRUE;
            } // leave sendSucceeded unchanged if FALSE
        }

        if ((context->mDns6Sock != INVALID_SOCKET) && (buf->flags & AJ_IO_BUF_MDNS)) {
            struct sockaddr_in6 sin6;
            sin6.sin6_family = AF_INET6;
            sin6.sin6_flowinfo = 0;
            sin6.sin6_scope_id = 0;
            sin6.sin6_port = htons(MDNS_UDP_PORT);
            if (inet_pton(AF_INET6, MDNS_IPV6_MULTICAST_GROUP, &sin6.sin6_addr) == 1) {

                if (RewriteSenderInfo(buf, (struct sockaddr*)&sin6) != AJ_OK) {
                    AJ_WarnPrintf(("AJ_Net_SendTo(): RewriteSenderInfo failed.\n"));
                    tx = 0;
                } else {
                    tx = AJ_IO_BUF_AVAIL(buf);
                    ret = sendto(context->mDns6Sock, buf->readPtr, tx, MSG_NOSIGNAL, (struct sockaddr*) &sin6, sizeof(sin6));
                    if (tx == ret) {
                        sendSucceeded = TRUE;
                    } else {
                        AJ_ErrPrintf(("AJ_Net_SendTo(): sendto mDNS IPv6 failed. errno=\"%s\"\n", strerror(errno)));
                    }
                }
            } else {
                AJ_ErrPrintf(("AJ_Net_SendTo(): Invalid mDNS IPv6 address. errno=\"%s\"\n", strerror(errno)));
            }
        }

        if (!sendSucceeded) {
            /* Not a single send succeeded, return an error */
            AJ_ErrPrintf(("AJ_Net_SendTo(): sendto() failed. errno=\"%s\", status=AJ_ERR_WRITE\n", strerror(errno)));
            return AJ_ERR_WRITE;
        }
        buf->readPtr += ret;
    }
    AJ_IO_BUF_RESET(buf);
    AJ_InfoPrintf(("AJ_Net_SendTo(): status=AJ_OK\n"));
    return AJ_OK;
}

AJ_Status AJ_Net_RecvFrom(AJ_IOBuffer* buf, uint32_t len, uint32_t timeout)
{
    MCastContext* context = (MCastContext*) buf->context;
    AJ_Status status = AJ_OK;
    ssize_t ret;
    size_t rx;
    fd_set fds;
    int maxFd = INVALID_SOCKET;
    int rc = 0;
    struct sockaddr_storage sa;
    socklen_t addrlen;
    struct timeval tv = { timeout / 1000, 1000 * (timeout % 1000) };

    // AJ_InfoPrintf(("AJ_Net_RecvFrom(buf=0x%p, len=%d, timeout=%d)\n", buf, len, timeout));

    assert(buf->direction == AJ_IO_BUF_RX);
    assert(context->mDnsRecvSock != INVALID_SOCKET);

    FD_ZERO(&fds);
    FD_SET(context->mDnsRecvSock, &fds);
    maxFd = context->mDnsRecvSock;

    if (context->mDns6RecvSock != INVALID_SOCKET) {
        FD_SET(context->mDns6RecvSock, &fds);
        maxFd = max(maxFd, context->mDns6RecvSock);
    }

    if (context->udpSock != INVALID_SOCKET) {
        FD_SET(context->udpSock, &fds);
        maxFd = max(maxFd, context->udpSock);
    }

    if (context->udp6Sock != INVALID_SOCKET) {
        FD_SET(context->udp6Sock, &fds);
        maxFd = max(maxFd, context->udp6Sock);
    }
    if (context->mDnsSock != INVALID_SOCKET) {
        FD_SET(context->mDnsSock, &fds);
        maxFd = max(maxFd, context->mDnsSock);
    }
    if (context->mDns6Sock != INVALID_SOCKET) {
        FD_SET(context->mDns6Sock, &fds);
        maxFd = max(maxFd, context->mDns6Sock);
    }

    rc = select(maxFd + 1, &fds, NULL, NULL, &tv);
    if (rc == 0) {
        AJ_InfoPrintf(("AJ_Net_RecvFrom(): select() timed out. status=AJ_ERR_TIMEOUT\n"));
        return AJ_ERR_TIMEOUT;
    }

    // we need to read from the first socket that has data available.

    rx = AJ_IO_BUF_SPACE(buf);
    if (context->mDnsRecvSock != INVALID_SOCKET && FD_ISSET(context->mDnsRecvSock, &fds)) {
        rx = min(rx, len);
        if (rx) {
            ret = recvfrom(context->mDnsRecvSock, buf->writePtr, rx, 0, NULL, 0);
            if (ret == -1) {
                AJ_ErrPrintf(("AJ_Net_RecvFrom(): mDnsRecvSock recvfrom() failed. errno=\"%s\"\n", strerror(errno)));
                status = AJ_ERR_READ;
            } else {
                AJ_InfoPrintf(("AJ_Net_RecvFrom(): recv'd %d from mDNS\n", (int) ret));
                buf->flags |= AJ_IO_BUF_MDNS;
                buf->writePtr += ret;
                status = AJ_OK;
                goto Finished;
            }
        }
    }
    rx = AJ_IO_BUF_SPACE(buf);
    if (context->mDns6RecvSock != INVALID_SOCKET && FD_ISSET(context->mDns6RecvSock, &fds)) {
        rx = min(rx, len);
        if (rx) {
            addrlen = sizeof(sa);
            ret = recvfrom(context->mDns6RecvSock, buf->writePtr, rx, 0, (struct sockaddr*)&sa, &addrlen);
            if (ret == -1) {
                AJ_ErrPrintf(("AJ_Net_RecvFrom(): mDns6RecvSock recvfrom() failed. errno=\"%s\"\n", strerror(errno)));
                status = AJ_ERR_READ;
            } else {
                AJ_InfoPrintf(("AJ_Net_RecvFrom(): recv'd %d from mDNS\n", (int) ret));
                if (sa.ss_family == AF_INET6) {
                    struct sockaddr_in6*sin6 = (struct sockaddr_in6*)&sa;
                    buf->scope_id = sin6->sin6_scope_id;
                }
                buf->flags |= AJ_IO_BUF_MDNS;
                buf->writePtr += ret;
                status = AJ_OK;
                goto Finished;
            }
        }
    }

    rx = AJ_IO_BUF_SPACE(buf);
    if (context->udp6Sock != INVALID_SOCKET && FD_ISSET(context->udp6Sock, &fds)) {
        rx = min(rx, len);
        if (rx) {
            addrlen = sizeof(sa);
            ret = recvfrom(context->udp6Sock, buf->writePtr, rx, 0, (struct sockaddr*)&sa, &addrlen);
            if (ret == -1) {
                AJ_ErrPrintf(("AJ_Net_RecvFrom(): recvfrom() failed. errno=\"%s\"\n", strerror(errno)));
                status = AJ_ERR_READ;
            } else {
                AJ_InfoPrintf(("AJ_Net_RecvFrom(): recv'd %d from udp6\n", (int) ret));
                if (sa.ss_family == AF_INET6) {
                    struct sockaddr_in6*sin6 = (struct sockaddr_in6*)&sa;
                    buf->scope_id = sin6->sin6_scope_id;
                }
                buf->flags |= AJ_IO_BUF_AJ;
                buf->writePtr += ret;
                status = AJ_OK;
                goto Finished;
            }
        }
    }

    rx = AJ_IO_BUF_SPACE(buf);
    if (context->udpSock != INVALID_SOCKET && FD_ISSET(context->udpSock, &fds)) {
        rx = min(rx, len);
        if (rx) {
            ret = recvfrom(context->udpSock, buf->writePtr, rx, 0, NULL, 0);
            if (ret == -1) {
                AJ_ErrPrintf(("AJ_Net_RecvFrom(): recvfrom() failed. errno=\"%s\"\n", strerror(errno)));
                status = AJ_ERR_READ;
            } else {
                AJ_InfoPrintf(("AJ_Net_RecvFrom(): recv'd %d from udp\n", (int) ret));
                buf->flags |= AJ_IO_BUF_AJ;
                buf->writePtr += ret;
                status = AJ_OK;
                goto Finished;
            }
        }
    }
    rx = AJ_IO_BUF_SPACE(buf);
    if (context->mDnsSock != INVALID_SOCKET && FD_ISSET(context->mDnsSock, &fds)) {
        rx = min(rx, len);
        if (rx) {
            ret = recvfrom(context->mDnsSock, buf->writePtr, rx, 0, NULL, 0);
            if (ret == -1) {
                AJ_ErrPrintf(("AJ_Net_RecvFrom(): recvfrom() failed. errno=\"%s\"\n", strerror(errno)));
                status = AJ_ERR_READ;
            } else {
                AJ_InfoPrintf(("AJ_Net_RecvFrom(): recv'd %d from udp\n", (int) ret));
                buf->flags |= AJ_IO_BUF_MDNS;
                buf->writePtr += ret;
                status = AJ_OK;
                goto Finished;
            }
        }
    }
    rx = AJ_IO_BUF_SPACE(buf);
    if (context->mDns6Sock != INVALID_SOCKET && FD_ISSET(context->mDns6Sock, &fds)) {
        rx = min(rx, len);
        if (rx) {
            addrlen = sizeof(sa);
            ret = recvfrom(context->mDns6Sock, buf->writePtr, rx, 0, (struct sockaddr*)&sa, &addrlen);
            if (ret == -1) {
                AJ_ErrPrintf(("AJ_Net_RecvFrom(): recvfrom() failed. errno=\"%s\"\n", strerror(errno)));
                status = AJ_ERR_READ;
            } else {
                AJ_InfoPrintf(("AJ_Net_RecvFrom(): recv'd %d from udp\n", (int) ret));
                if (sa.ss_family == AF_INET6) {
                    struct sockaddr_in6*sin6 = (struct sockaddr_in6*)&sa;
                    buf->scope_id = sin6->sin6_scope_id;
                }
                buf->flags |= AJ_IO_BUF_MDNS;
                buf->writePtr += ret;
                status = AJ_OK;
                goto Finished;
            }
        }
    }


Finished:
    if (status != AJ_OK) {
        AJ_InfoPrintf(("AJ_Net_RecvFrom(): status=%s\n", AJ_StatusText(status)));
    }
    return status;
}

/*
 * Need enough space to receive a complete name service packet when used in UDP
 * mode.  NS expects MTU of 1500 subtracts UDP, IP and ethertype overhead.
 * 1500 - 8 -20 - 18 = 1454.  txData buffer size needs to be big enough to hold
 * max(NS WHO-HAS for one name (4 + 2 + 256 = 262),
 *     mDNS query for one name (194 + 5 + 5 + 15 + 256 = 475)) = 475
 */
static uint8_t rxDataMCast[1454];
static uint8_t txDataMCast[475];

static int MCastUp4(const char group[], uint16_t port)
{
    int ret;
    struct ip_mreq mreq;
    struct sockaddr_in sin;
    int reuse = 1;
    int bcast = 1;
    int mcastSock;

    mcastSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (mcastSock == INVALID_SOCKET) {
        AJ_ErrPrintf(("MCastUp4(): socket() fails. status=AJ_ERR_READ\n"));
        return INVALID_SOCKET;
    }

    ret = setsockopt(mcastSock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (ret != 0) {
        AJ_ErrPrintf(("MCastUp4(): setsockopt(SO_REUSEADDR) failed. errno=\"%s\", status=AJ_ERR_READ\n", strerror(errno)));
        goto ExitError;
    }

    // enable IP broadcast on this socket.
    // This is needed for bcast router discovery
    int r = setsockopt(mcastSock, SOL_SOCKET, SO_BROADCAST, (void*) &bcast, sizeof(bcast));
    if (r != 0) {
        AJ_ErrPrintf(("BcastUp4(): setsockopt(SOL_SOCKET, SO_BROADCAST) failed. errno=\"%s\"\n", strerror(errno)));
        goto ExitError;
    }

    /*
     * Bind supplied port
     */
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = INADDR_ANY;
    ret = bind(mcastSock, (struct sockaddr*) &sin, sizeof(sin));
    if (ret < 0) {
        AJ_ErrPrintf(("MCastUp4(): bind() failed. errno=\"%s\", status=AJ_ERR_READ\n", strerror(errno)));
        goto ExitError;
    }

    /*
     * Join our multicast group
     */
    memset(&mreq, 0, sizeof(mreq));
    inet_pton(AF_INET, group, &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = INADDR_ANY;
    ret = setsockopt(mcastSock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
    if (ret < 0) {
        /*
         * Not all Linux based systems setup an IPv4 multicast route.
         * Since we were successful in setting up IPv4 broadcast for
         * this socket, we'll just use that and not use IPv4 multicast.
         */
        AJ_WarnPrintf(("MCastUp4(): setsockopt(IP_ADD_MEMBERSHIP) failed. errno=\"%s\", status=AJ_ERR_READ\n", strerror(errno)));
    }

    return mcastSock;

ExitError:
    close(mcastSock);
    return INVALID_SOCKET;
}

static int MCastUp6(const char* group, uint16_t port)
{
    int ret;
    struct ipv6_mreq mreq6;
    struct sockaddr_in6 sin6;
    int reuse = 1;
    int mcastSock;

    mcastSock = socket(AF_INET6, SOCK_DGRAM, 0);
    if (mcastSock == INVALID_SOCKET) {
        AJ_ErrPrintf(("MCastUp6(): socket() fails. errno=\"%s\" status=AJ_ERR_READ\n", strerror(errno)));
        return INVALID_SOCKET;
    }

    ret = setsockopt(mcastSock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (ret != 0) {
        AJ_ErrPrintf(("MCastUp6(): setsockopt(SO_REUSEADDR) failed. errno=\"%s\", status=AJ_ERR_READ\n", strerror(errno)));
        goto ExitError;
    }

    /*
     * Bind supplied port
     */
    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;
    sin6.sin6_port = htons(port);
    sin6.sin6_addr = in6addr_any;
    ret = bind(mcastSock, (struct sockaddr*) &sin6, sizeof(sin6));
    if (ret < 0) {
        AJ_ErrPrintf(("MCastUp6(): bind() failed. errno=\"%s\", status=AJ_ERR_READ\n", strerror(errno)));
        goto ExitError;
    }

    /*
     * Join multicast group
     */
    memset(&mreq6, 0, sizeof(mreq6));
    inet_pton(AF_INET6, group, &mreq6.ipv6mr_multiaddr);
    mreq6.ipv6mr_interface = 0;
    ret = setsockopt(mcastSock, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq6, sizeof(mreq6));
    if (ret < 0) {
        AJ_ErrPrintf(("MCastUp6(): setsockopt(IP_ADD_MEMBERSHIP) failed. errno=\"%s\", status=AJ_ERR_READ\n", strerror(errno)));
        goto ExitError;
    }

    return mcastSock;

ExitError:
    close(mcastSock);
    return INVALID_SOCKET;
}

static uint8_t chooseMDnsRecvAddr(struct sockaddr_in6* sin)
{
    struct ifaddrs* addrs;
    struct ifaddrs* addr;
    FILE* f;
    char line[100];
    char* saveptr;
    char* iface;
    char defIface[IFNAMSIZ + 1] = { 0 };
    char* dest;
    char* metricStr;
    int metric;
    int defMetric = INT_MAX;
    /* Grab the interface for the default route */
    f = fopen("/proc/net/route", "r");
    if (NULL != f) {
        while (NULL != fgets(line, sizeof(line), f)) {
            iface = strtok_r(line, " \t", &saveptr);  /* Iface */
            dest = strtok_r(NULL, " \t", &saveptr);   /* Destination */
            metricStr = strtok_r(NULL, " \t", &saveptr);  /* Gateway */
            metricStr = strtok_r(NULL, " \t", &saveptr);  /* Flags */
            metricStr = strtok_r(NULL, " \t", &saveptr);  /* Use */
            metricStr = strtok_r(NULL, " \t", &saveptr);  /* Metric */

            if ((NULL != iface)  && (NULL != dest)) {
                if (0 == strcmp(dest, "00000000")) {
                    metric = atoi(metricStr);
                    if (metric < defMetric) {
                        defMetric = metric;
                        strncpy(defIface, iface, IFNAMSIZ);
                    }
                }
            }
        }
        fclose(f);
    } else {
        AJ_WarnPrintf(("Unable to open /proc/net/route\n"));
    }
    getifaddrs(&addrs);
    addr = addrs;
    while (addr != NULL) {
        // Choose the interface that is enabled and matches the best default route otherwise
        // Choose first IPv4 address that is not LOOPBACK and is enabled
        if (addr->ifa_addr != NULL &&
            (addr->ifa_addr->sa_family == AF_INET || addr->ifa_addr->sa_family == AF_INET6)  &&

            !(addr->ifa_flags & IFF_LOOPBACK) && (addr->ifa_flags & IFF_UP) && (addr->ifa_flags & IFF_RUNNING)) {
            /* If we do not have a default route or if we have a default route,
               only choose address on the interface with a default route */

            if ((0 == defIface[0]) || (0 == strcmp(defIface, addr->ifa_name))) {
                if (AF_INET == addr->ifa_addr->sa_family) {
                    memcpy(sin, addr->ifa_addr, sizeof(struct sockaddr_in));
                    return TRUE;
                } else if (AF_INET6 == addr->ifa_addr->sa_family) {
                    memcpy(sin, addr->ifa_addr, sizeof(struct sockaddr_in6));
                    return TRUE;
                }
            }
        }
        addr = addr->ifa_next;
    }
    freeifaddrs(addrs);
    return FALSE;
}

static int MDnsRecvUp()
{
    int ret;
    struct sockaddr_in sin;
    int reuse = 1;
    int recvSock;

    recvSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (recvSock == INVALID_SOCKET) {
        AJ_ErrPrintf(("MDnsRecvUp(): socket() fails. status=AJ_ERR_READ\n"));
        goto ExitError;
    }

    ret = setsockopt(recvSock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (ret != 0) {
        AJ_ErrPrintf(("MDnsRecvUp(): setsockopt(SO_REUSEADDR) failed. errno=\"%s\", status=AJ_ERR_READ\n", strerror(errno)));
        goto ExitError;
    }

    sin.sin_family = AF_INET;
    sin.sin_port = htons(0);
    sin.sin_addr.s_addr = INADDR_ANY;
    ret = bind(recvSock, (struct sockaddr*) &sin, sizeof(sin));
    if (ret < 0) {
        AJ_ErrPrintf(("MDnsRecvUp(): bind() failed. errno=\"%s\", status=AJ_ERR_READ\n", strerror(errno)));
        goto ExitError;
    }
    return recvSock;

ExitError:
    close(recvSock);
    return INVALID_SOCKET;
}
static int MDns6RecvUp()
{
    int ret;
    struct sockaddr_in6 sin6;
    int reuse = 1;
    int recvSock;
    do {
        recvSock = socket(AF_INET6, SOCK_DGRAM, 0);
        if (recvSock == INVALID_SOCKET) {
            AJ_ErrPrintf(("MDns6RecvUp(): socket() fails. status=AJ_ERR_READ\n"));
            break;
        }

        ret = setsockopt(recvSock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        if (ret != 0) {
            AJ_ErrPrintf(("MDnsRecvUp(): setsockopt(SO_REUSEADDR) failed. errno=\"%s\", status=AJ_ERR_READ\n", strerror(errno)));
            close(recvSock);
            recvSock = INVALID_SOCKET;
            break;
        }

        sin6.sin6_family = AF_INET6;
        sin6.sin6_port = htons(0);
        sin6.sin6_addr = in6addr_any;
        ret = bind(recvSock, (struct sockaddr*) &sin6, sizeof(sin6));
        if (ret < 0) {
            AJ_ErrPrintf(("MDnsRecvUp(): bind() failed. errno=\"%s\", status=AJ_ERR_READ\n", strerror(errno)));
            close(recvSock);
            recvSock = INVALID_SOCKET;
            break;
        }
    } while (0);
    return recvSock;
}

AJ_Status AJ_Net_MCastUp(AJ_MCastSocket* mcastSock)
{
    struct sockaddr_storage addrBuf;
    socklen_t addrLen = sizeof(addrBuf);
    struct sockaddr_in* sin;
    AJ_Status status = AJ_ERR_READ;

    mCastContext.mDnsRecvSock = MDnsRecvUp();
    mCastContext.mDns6RecvSock = MDns6RecvUp();
    if (mCastContext.mDnsRecvSock == INVALID_SOCKET) {
        AJ_ErrPrintf(("AJ_Net_MCastUp(): MDnsRecvUp for mDnsRecvSock failed"));
        return status;
    }
    if (getsockname(mCastContext.mDnsRecvSock, (struct sockaddr*) &addrBuf, &addrLen)) {
        AJ_ErrPrintf(("AJ_Net_MCastUp(): getsockname for mDnsRecvSock failed"));
        goto ExitError;
    }
    if (FALSE == chooseMDnsRecvAddr(&mCastContext.mDnsRecvAddr)) {
        AJ_ErrPrintf(("AJ_Net_MCastUp(): no mDNS recv address"));
        goto ExitError;
    }
    // The offsets of the sockaddr_in and the sockaddr_in6 port are the same so we cheat
    sin = (struct sockaddr_in*) &addrBuf;
    mCastContext.mDnsRecvAddr.sin6_port = ntohs(sin->sin_port);

    {
        char addrbuf[INET6_ADDRSTRLEN];
        if (NULL != inet_ntop(mCastContext.mDnsRecvAddr.sin6_family, &mCastContext.mDnsRecvAddr,
                              addrbuf, sizeof(addrbuf))) {
            AJ_InfoPrintf(("AJ_Net_MCastUp(): mDNS recv on %s-%d\n", addrbuf, ntohs(mCastContext.mDnsRecvAddr.sin6_port)));
        }
    }

    mCastContext.mDnsSock = MCastUp4(MDNS_IPV4_MULTICAST_GROUP, MDNS_UDP_PORT);
    mCastContext.mDns6Sock = MCastUp6(MDNS_IPV6_MULTICAST_GROUP, MDNS_UDP_PORT);
    if (AJ_GetMinProtoVersion() < 10) {
        mCastContext.udpSock = MCastUp4(AJ_IPV4_MULTICAST_GROUP, 0);
        mCastContext.udp6Sock = MCastUp6(AJ_IPV6_MULTICAST_GROUP, 0);
    }

    if (mCastContext.udpSock != INVALID_SOCKET || mCastContext.udp6Sock != INVALID_SOCKET ||
        mCastContext.mDnsSock != INVALID_SOCKET || mCastContext.mDns6Sock != INVALID_SOCKET) {
        AJ_IOBufInit(&mcastSock->rx, rxDataMCast, sizeof(rxDataMCast), AJ_IO_BUF_RX, &mCastContext);
        mcastSock->rx.recv = AJ_Net_RecvFrom;
        AJ_IOBufInit(&mcastSock->tx, txDataMCast, sizeof(txDataMCast), AJ_IO_BUF_TX, &mCastContext);
        mcastSock->tx.send = AJ_Net_SendTo;
        status = AJ_OK;
    }
    return status;

ExitError:
    close(mCastContext.mDnsRecvSock);
    if (INVALID_SOCKET != mCastContext.mDns6RecvSock) {
        close(mCastContext.mDns6RecvSock);
    }
    return status;
}

void AJ_Net_MCastDown(AJ_MCastSocket* mcastSock)
{
    MCastContext* context = (MCastContext*) mcastSock->rx.context;
    AJ_InfoPrintf(("AJ_Net_MCastDown(mcastSock=0x%p)\n", mcastSock));

    if (context->udpSock != INVALID_SOCKET) {
        struct ip_mreq mreq;
        inet_pton(AF_INET, AJ_IPV4_MULTICAST_GROUP, &mreq.imr_multiaddr);
        mreq.imr_interface.s_addr = INADDR_ANY;
        setsockopt(context->udpSock, IPPROTO_IP, IP_DROP_MEMBERSHIP, (char*) &mreq, sizeof(mreq));
    }

    if (context->udp6Sock != INVALID_SOCKET) {
        struct ipv6_mreq mreq6;
        inet_pton(AF_INET6, AJ_IPV6_MULTICAST_GROUP, &mreq6.ipv6mr_multiaddr);
        mreq6.ipv6mr_interface = 0;
        setsockopt(context->udp6Sock, IPPROTO_IPV6, IPV6_LEAVE_GROUP, &mreq6, sizeof(mreq6));
    }

    if (context->mDnsSock != INVALID_SOCKET) {
        struct ip_mreq mreq;
        inet_pton(AF_INET, MDNS_IPV4_MULTICAST_GROUP, &mreq.imr_multiaddr);
        mreq.imr_interface.s_addr = INADDR_ANY;
        setsockopt(context->udpSock, IPPROTO_IP, IP_DROP_MEMBERSHIP, (char*) &mreq, sizeof(mreq));
    }

    if (context->mDns6Sock != INVALID_SOCKET) {
        struct ipv6_mreq mreq6;
        inet_pton(AF_INET6, MDNS_IPV6_MULTICAST_GROUP, &mreq6.ipv6mr_multiaddr);
        mreq6.ipv6mr_interface = 0;
        setsockopt(context->udp6Sock, IPPROTO_IPV6, IPV6_LEAVE_GROUP, &mreq6, sizeof(mreq6));
    }

    CloseMCastSock(mcastSock);
}

#ifdef AJ_ARDP

static AJ_Status AJ_ARDP_UDP_Send(void* context, uint8_t* buf, size_t len, size_t* sent, uint8_t confirm)
{
    AJ_Status status = AJ_OK;
    ssize_t ret;
    NetContext* ctx = (NetContext*) context;

    AJ_InfoPrintf(("AJ_ARDP_UDP_Send(buf=0x%p, len=%lu)\n", buf, len));

    // we can send( rather than sendto( because we did a UDP connect()
    ret = send(ctx->udpSock, buf, len, (confirm == TRUE) ? MSG_CONFIRM : 0);
    if (ret == -1) {
        status = AJ_ERR_WRITE;
    } else {
        *sent = (size_t) ret;
    }

    return status;
}

static AJ_Status AJ_ARDP_UDP_Recv(void* context, uint8_t** data, uint32_t* recved, uint32_t timeout)
{
    fd_set fds;
    struct timeval tv = { timeout / 1000, 1000 * (timeout % 1000) };
    int ret;
    NetContext* ctx = (NetContext*) context;
    int maxFd = max(ctx->udpSock, interruptFd);

    /**
     * Let the platform code own this buffer.  This makes it easier to avoid double-buffering
     * on platforms that allow it.
     */
    static uint8_t buffer[UDP_SEGBMAX];

    *data = NULL;

    AJ_InfoPrintf(("AJ_ARDP_UDP_Recv(data=0x%p, recved=0x%p, timeout=%u)\n", data, recved, timeout));

    FD_ZERO(&fds);
    FD_SET(ctx->udpSock, &fds);
    if (interruptFd > 0) {
        FD_SET(interruptFd, &fds);
    }

    blocked = TRUE;
    ret = select(maxFd + 1, &fds, NULL, NULL, &tv);
    blocked = FALSE;

    if (ret == 0) {
        // timeout!
        return AJ_ERR_TIMEOUT;
    } else if (ret == -1) {
        perror("select");
        return AJ_ERR_READ;
    } else if ((interruptFd > 0) && FD_ISSET(interruptFd, &fds)) {
        uint64_t u64;
        read(interruptFd, &u64, sizeof(u64));
        return AJ_ERR_INTERRUPTED;
    } else if (FD_ISSET(ctx->udpSock, &fds)) {
        ret = recvfrom(ctx->udpSock, buffer, sizeof(buffer), 0, NULL, 0);

        if (ret == -1) {
            // this will only happen if we are on a local machine
            perror("recvfrom");
            return AJ_ERR_READ;
        }

        *recved = ret;
        *data = buffer;
    }

    return AJ_OK;
}

static AJ_Status AJ_Net_ARDP_Connect(AJ_BusAttachment* bus, const AJ_Service* service)
{
    int udpSock = INVALID_SOCKET;
    AJ_Status status;
    struct sockaddr_storage addrBuf;
    socklen_t addrSize;
    int ret;
    int i;

    AJ_ARDP_InitFunctions(AJ_ARDP_UDP_Recv, AJ_ARDP_UDP_Send);

    memset(&addrBuf, 0, sizeof(addrBuf));

    interruptFd = eventfd(0, O_NONBLOCK);  // Use O_NONBLOCK instead of EFD_NONBLOCK due to bug in OpenWrt's uCLibc
    if (interruptFd < 0) {
        AJ_ErrPrintf(("AJ_Net_ARDP_Connect(): failed to created interrupt event\n"));
        return AJ_ERR_CONNECT;
    }
    for (i = 0; i < 2; i++) {
        if ((0 == i) && (service->addrTypes & AJ_ADDR_UDP4)) {
            struct sockaddr_in* sa = (struct sockaddr_in*) &addrBuf;
            udpSock = socket(AF_INET, SOCK_DGRAM, 0);
            if (udpSock == INVALID_SOCKET) {
                AJ_ErrPrintf(("AJ_Net_ARDP_Connect(): socket() failed.  status=AJ_ERR_CONNECT\n"));
                continue;
            }

            sa->sin_family = AF_INET;
            sa->sin_port = htons(service->ipv4portUdp);
            sa->sin_addr.s_addr = service->ipv4Udp;
            addrSize = sizeof(struct sockaddr_in);
            AJ_InfoPrintf(("AJ_Net_ARDP_Connect(): Connect to \"%s:%u\"\n", inet_ntoa(sa->sin_addr), service->ipv4portUdp));;
        }
        if ((1 == i) && (service->addrTypes & AJ_ADDR_UDP6)) {
            struct sockaddr_in6* sa = (struct sockaddr_in6*) &addrBuf;
            udpSock = socket(AF_INET6, SOCK_DGRAM, 0);
            if (udpSock == INVALID_SOCKET) {
                AJ_ErrPrintf(("AJ_Net_ARDP_Connect(): socket() failed.  status=AJ_ERR_CONNECT\n"));
                continue;
            }

            sa->sin6_family = AF_INET6;
            sa->sin6_port = htons(service->ipv6portUdp);
            sa->sin6_scope_id = service->scope_id;
            memcpy(sa->sin6_addr.s6_addr, service->ipv6Udp, sizeof(sa->sin6_addr.s6_addr));
            addrSize = sizeof(struct sockaddr_in6);
        }

        // When you 'connect' a UDP socket, it means that this is the default sendto address.
        // Therefore, we don't have to make the address a global variable and can
        // simply use send() rather than sendto().  See: man 7 udp
        ret = connect(udpSock, (struct sockaddr*) &addrBuf, addrSize);

        // must do this before calling AJ_MarshalMethodCall!
        if (ret == 0) {
            netContext.udpSock = udpSock;
            AJ_IOBufInit(&bus->sock.rx, rxData, sizeof(rxData), AJ_IO_BUF_RX, &netContext);
            bus->sock.rx.recv = AJ_ARDP_Recv;
            AJ_IOBufInit(&bus->sock.tx, txData, sizeof(txData), AJ_IO_BUF_TX, &netContext);
            bus->sock.tx.send = AJ_ARDP_Send;
        } else {
            AJ_ErrPrintf(("AJ_Net_ARDP_Connect(): Error connecting\n"));
            perror("connect");
            continue;
        }

        status = AJ_ARDP_UDP_Connect(bus, &netContext, service, &bus->sock);
        if (status != AJ_OK) {
            AJ_Net_ARDP_Disconnect(&bus->sock);
            continue;
        }

        return AJ_OK;
    }
    AJ_ErrPrintf(("AJ_Net_ARDP_Connect(): Connect failed status=AJ_ERR_CONNECT\n"));
    if (interruptFd != INVALID_SOCKET) {
        close(interruptFd);
        interruptFd = INVALID_SOCKET;
    }

    if (udpSock != INVALID_SOCKET) {
        close(udpSock);
    }

    return AJ_ERR_CONNECT;
}

static void AJ_Net_ARDP_Disconnect(AJ_NetSocket* netSock)
{
    AJ_ARDP_Disconnect(FALSE);

    close(netContext.udpSock);
    netContext.udpSock = INVALID_SOCKET;
    memset(netSock, 0, sizeof(AJ_NetSocket));
}

#endif // AJ_ARDP
