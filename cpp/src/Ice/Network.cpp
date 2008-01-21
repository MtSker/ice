// **********************************************************************
//
// Copyright (c) 2003-2007 ZeroC, Inc. All rights reserved.
//
// This copy of Ice is licensed to you under the terms described in the
// ICE_LICENSE file included in this distribution.
//
// **********************************************************************

//
// The following is required on HP-UX in order to bring in
// the definition for the ip_mreq structure.
//
#ifdef __hpux
#undef _XOPEN_SOURCE_EXTENDED
#define _XOPEN_SOURCE
#include <netinet/in.h>
#endif

#include <IceUtil/StaticMutex.h>
#include <Ice/Network.h>
#include <Ice/LocalException.h>
#include <Ice/Properties.h> // For setTcpBufSize
#include <Ice/LoggerUtil.h> // For setTcpBufSize

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#elif defined(__linux) || defined(__APPLE__) || defined(__FreeBSD__)
#  include <ifaddrs.h>
#  include <net/if.h>
#else
#  include <sys/ioctl.h>
#  include <net/if.h>
#  ifdef __sun
#    include <sys/sockio.h>
#  endif
#endif

using namespace std;
using namespace Ice;
using namespace IceInternal;

#ifdef __sun
#    define INADDR_NONE (in_addr_t)0xffffffff
#endif

namespace
{

vector<struct sockaddr_storage>
getLocalAddresses(ProtocolSupport protocol)
{
    vector<struct sockaddr_storage> result;

#if defined(_WIN32)
    try
    {
        for(int i = 0; i < 2; i++)
        {
            if((i == 0 && protocol == EnableIPv6) || (i == 1 && protocol == EnableIPv4))
            {
                continue;
            }

            SOCKET fd = createSocket(false, i == 0 ? AF_INET : AF_INET6);

            vector<unsigned char> buffer;
            buffer.resize(1024);
            unsigned long len = 0;
            DWORD rs = WSAIoctl(fd, SIO_ADDRESS_LIST_QUERY, 0, 0, 
                                &buffer[0], static_cast<DWORD>(buffer.size()),
                                &len, 0, 0);
            if(rs == SOCKET_ERROR)
            {
                //
                // If the buffer wasn't big enough, resize it to the
                // required length and try again.
                //
                if(getSocketErrno() == WSAEFAULT)
                {
                    buffer.resize(len);
                    rs = WSAIoctl(fd, SIO_ADDRESS_LIST_QUERY, 0, 0, 
                                  &buffer[0], static_cast<DWORD>(buffer.size()),
                                  &len, 0, 0);
                }

                if(rs == SOCKET_ERROR)
                {
                    closeSocketNoThrow(fd);
                    SocketException ex(__FILE__, __LINE__);
                    ex.error = getSocketErrno();
                    throw ex;
                }
            }

            //        
            // Add the local interface addresses.
            //
            SOCKET_ADDRESS_LIST* addrs = reinterpret_cast<SOCKET_ADDRESS_LIST*>(&buffer[0]);
            for (int i = 0; i < addrs->iAddressCount; ++i)
            {
		sockaddr_storage addr;
		memcpy(&addr, addrs->Address[i].lpSockaddr, addrs->Address[i].iSockaddrLength);
		if(addr.ss_family == AF_INET && protocol != EnableIPv6)
		{
		    if(reinterpret_cast<struct sockaddr_in*>(&addr)->sin_addr.s_addr != 0)
		    {
			result.push_back(addr);
		    }
		}
		else if(addr.ss_family == AF_INET6 && protocol != EnableIPv4)
		{
		    struct in6_addr* inaddr6 = &reinterpret_cast<struct sockaddr_in6*>(&addr)->sin6_addr;
		    if(!IN6_IS_ADDR_UNSPECIFIED(inaddr6) && !IN6_IS_ADDR_LOOPBACK(inaddr6))
		    {
			result.push_back(addr);
		    }
		}
            }

            closeSocket(fd);
        }
    }
    catch(const Ice::LocalException&)
    {
        //
        // TODO: Warning?
        //
    }
#elif defined(__linux) || defined(__APPLE__) || defined(__FreeBSD__)
    struct ifaddrs* ifap;
    if(::getifaddrs(&ifap) == SOCKET_ERROR)
    {
        SocketException ex(__FILE__, __LINE__);
        ex.error = getSocketErrno();
        throw ex;
    }

    struct ifaddrs* curr = ifap;
    while(curr != 0)
    {
        if(curr->ifa_addr && !(curr->ifa_flags & IFF_LOOPBACK))  // Don't include loopback interface addresses
        {
            if(curr->ifa_addr->sa_family == AF_INET && protocol != EnableIPv6)
            {
		sockaddr_storage addr;
		memcpy(&addr, curr->ifa_addr, sizeof(sockaddr_in));
                if(reinterpret_cast<struct sockaddr_in*>(&addr)->sin_addr.s_addr != 0)
                {
                    result.push_back(addr);
                }
            }
            else if(curr->ifa_addr->sa_family == AF_INET6 && protocol != EnableIPv4)
            {
		sockaddr_storage addr;
		memcpy(&addr, curr->ifa_addr, sizeof(sockaddr_in6));
                if(!IN6_IS_ADDR_UNSPECIFIED(&reinterpret_cast<struct sockaddr_in6*>(&addr)->sin6_addr))
                {
                    result.push_back(*reinterpret_cast<struct sockaddr_storage*>(curr->ifa_addr));
                }
            }
        }
    
        curr = curr->ifa_next;
    }

    ::freeifaddrs(ifap);
#else
    for(int i = 0; i < 2; i++)
    {
        if((i == 0 && protocol == EnableIPv6) || (i == 1 && protocol == EnableIPv4))
        {
            continue;
        }
        SOCKET fd = createSocket(false, i == 0 ? AF_INET : AF_INET6);

#ifdef _AIX
        int cmd = CSIOCGIFCONF;
#else
        int cmd = SIOCGIFCONF;
#endif
        struct ifconf ifc;
        int numaddrs = 10;
        int old_ifc_len = 0;

        //
        // Need to call ioctl multiple times since we do not know up front
        // how many addresses there will be, and thus how large a buffer we need.
        // We keep increasing the buffer size until subsequent calls return
        // the same length, meaning we have all the addresses.
        //
        while(true)
        {
            int bufsize = numaddrs * static_cast<int>(sizeof(struct ifreq));
            ifc.ifc_len = bufsize;
            ifc.ifc_buf = (char*)malloc(bufsize);
        
            int rs = ioctl(fd, cmd, &ifc);
            if(rs == SOCKET_ERROR)
            {
                free(ifc.ifc_buf);
                closeSocketNoThrow(fd);
                SocketException ex(__FILE__, __LINE__);
                ex.error = getSocketErrno();
                throw ex;
            }
            else if(ifc.ifc_len == old_ifc_len)
            {
                //
                // Returned same length twice in a row, finished.
                //
                break;
            }
            else
            {
                old_ifc_len = ifc.ifc_len;
            }
        
            numaddrs += 10;
            free(ifc.ifc_buf);
        }
        closeSocket(fd);

        numaddrs = ifc.ifc_len / static_cast<int>(sizeof(struct ifreq));
        struct ifreq* ifr = ifc.ifc_req;
        for(int i = 0; i < numaddrs; ++i)
        {
            if(!(ifr[i].ifr_flags & IFF_LOOPBACK)) // Don't include loopback interface addresses
            {
                if(ifr[i].ifr_addr.sa_family == AF_INET && protocol != EnableIPv6)
                {
		    sockaddr_storage addr;
		    memcpy(&addr, &ifr[i].ifr_addr, sizeof(sockaddr_in));
		    if(reinterpret_cast<struct sockaddr_in*>(&addr)->sin_addr.s_addr != 0)
		    {
			result.push_back(addr);
		    }
                }
                else if(ifr[i].ifr_addr.sa_family == AF_INET6 && protocol != EnableIPv4)
                {
		    sockaddr_storage addr;
		    memcpy(&addr, &ifr[i].ifr_addr, sizeof(sockaddr_in6));
		    if(!IN6_IS_ADDR_UNSPECIFIED(&reinterpret_cast<struct sockaddr_in6*>(&addr)->sin6_addr))
		    {
			result.push_back(addr);
		    }
                }
            }
        }
        free(ifc.ifc_buf);
    }
#endif

    return result;
}

void
getAddressImpl(const string& host, int port, struct sockaddr_storage& addr, ProtocolSupport protocol, bool server)
{
    //
    // We now use getaddrinfo() on Windows.
    //
// #ifdef _WIN32
    
//         //
//         // Windows XP has getaddrinfo(), but we don't want to require XP to run Ice.
//         //
        
//         //
//         // gethostbyname() is thread safe on Windows, with a separate hostent per thread
//         //
//         struct hostent* entry;
//         int retry = 5;
//         do
//         {
//             entry = gethostbyname(host.c_str());
//         }
//         while(entry == 0 && WSAGetLastError() == WSATRY_AGAIN && --retry >= 0);
        
//         if(entry == 0)
//         {
//             DNSException ex(__FILE__, __LINE__);

//             ex.error = WSAGetLastError();
//             ex.host = host;
//             throw ex;
//         }
//         memcpy(&addr.sin_addr, entry->h_addr, entry->h_length);

// #else

    memset(&addr, 0, sizeof(struct sockaddr_storage));
    struct addrinfo* info = 0;
    int retry = 5;
    
    struct addrinfo hints = { 0 };

    if(server)
    {
        //
        // If host is empty, getaddrinfo will return the wildcard
        // address instead of the loopack address.
        // 
        hints.ai_flags |= AI_PASSIVE;
    }

    if(protocol == EnableIPv4)
    {
        hints.ai_family = PF_INET;
    }
    else if(protocol == EnableIPv6)
    {
        hints.ai_family = PF_INET6;
    }
    else
    {
        hints.ai_family = PF_UNSPEC;
    }

    int rs = 0;
    do
    {
        if(host.empty())
        {
            rs = getaddrinfo(0, "1", &hints, &info);
        }
        else
        {
            rs = getaddrinfo(host.c_str(), 0, &hints, &info);    
        }
    }
    while(info == 0 && rs == EAI_AGAIN && --retry >= 0);
    
    if(rs != 0)
    {
        DNSException ex(__FILE__, __LINE__);
        ex.error = rs;
        ex.host = host;
        throw ex;
    }

    memcpy(&addr, info->ai_addr, info->ai_addrlen);
    if(info->ai_family != PF_INET)
    {
        reinterpret_cast<sockaddr_in*>(&addr)->sin_port = htons(port);
    }
    else if(info->ai_family != AF_INET6)
    {
        reinterpret_cast<sockaddr_in6*>(&addr)->sin6_port = htons(port);
    }
    else // Unknown address family.
    {
        freeaddrinfo(info);
        DNSException ex(__FILE__, __LINE__);
        ex.host = host;
        throw ex;
    }
    freeaddrinfo(info);
}

bool
isWildcard(const string& host, ProtocolSupport protocol)
{
    try
    {
        sockaddr_storage addr;
        getAddressImpl(host, 0, addr, protocol, false);
        if(addr.ss_family == AF_INET)
        {
            struct sockaddr_in* addrin = reinterpret_cast<sockaddr_in*>(&addr);
            if(addrin->sin_addr.s_addr == INADDR_ANY)
            {
                return true;
            }
        }
        else if(addr.ss_family)
        {
            struct sockaddr_in6* addrin6 = reinterpret_cast<sockaddr_in6*>(&addr);
            if(IN6_IS_ADDR_UNSPECIFIED(&addrin6->sin6_addr))
            {
                return true;
            }
        }
    }
    catch(const DNSException&)
    {
    }
    return false;
}

}

int
IceInternal::getSocketErrno()
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool
IceInternal::interrupted()
{
#ifdef _WIN32
    return WSAGetLastError() == WSAEINTR;
#else
#   ifdef EPROTO
    return errno == EINTR || errno == EPROTO;
#   else
    return errno == EINTR;
#   endif
#endif
}

bool
IceInternal::acceptInterrupted()
{
    if(interrupted())
    {
        return true;
    }

#ifdef _WIN32
    int error = WSAGetLastError();
    return error == WSAECONNABORTED ||
           error == WSAECONNRESET ||
           error == WSAETIMEDOUT;
#else
    return errno == ECONNABORTED ||
           errno == ECONNRESET ||
           errno == ETIMEDOUT;
#endif
}

bool
IceInternal::noBuffers()
{
#ifdef _WIN32
    int error = WSAGetLastError();
    return error == WSAENOBUFS ||
           error == WSAEFAULT;
#else
    return errno == ENOBUFS;
#endif
}

bool
IceInternal::wouldBlock()
{
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

bool
IceInternal::connectFailed()
{
#ifdef _WIN32
    int error = WSAGetLastError();
    return error == WSAECONNREFUSED ||
           error == WSAETIMEDOUT ||
           error == WSAENETUNREACH ||
           error == WSAEHOSTUNREACH ||
           error == WSAECONNRESET ||
           error == WSAESHUTDOWN ||
           error == WSAECONNABORTED;
#else
    return errno == ECONNREFUSED ||
           errno == ETIMEDOUT ||
           errno == ENETUNREACH ||
           errno == EHOSTUNREACH ||
           errno == ECONNRESET ||
           errno == ESHUTDOWN ||
           errno == ECONNABORTED;
#endif
}

bool
IceInternal::connectionRefused()
{
#ifdef _WIN32
    int error = WSAGetLastError();
    return error == WSAECONNREFUSED;
#else
    return errno == ECONNREFUSED;
#endif
}

bool
IceInternal::connectInProgress()
{
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EINPROGRESS;
#endif
}

bool
IceInternal::connectionLost()
{
#ifdef _WIN32
    int error = WSAGetLastError();
    return error == WSAECONNRESET ||
           error == WSAESHUTDOWN ||
           error == WSAENOTCONN ||
           error == WSAECONNABORTED;
#else
    return errno == ECONNRESET ||
           errno == ENOTCONN ||
           errno == ESHUTDOWN ||
           errno == ECONNABORTED ||
           errno == EPIPE;
#endif
}

bool
IceInternal::notConnected()
{
#ifdef _WIN32
    return WSAGetLastError() == WSAENOTCONN;
#elif defined(__APPLE__) || defined(__FreeBSD__)
    return errno == ENOTCONN || errno == EINVAL;
#else
    return errno == ENOTCONN;
#endif
}

bool
IceInternal::recvTruncated()
{
#ifdef _WIN32
    return WSAGetLastError() == WSAEMSGSIZE;
#else
    // We don't get an error under Linux if a datagram is truncated.
    return false;
#endif
}

SOCKET
IceInternal::createSocket(bool udp, int family)
{
    SOCKET fd;

    if(udp)
    {
        fd = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    }
    else
    {
        fd = socket(family, SOCK_STREAM, IPPROTO_TCP);
    }

    if(fd == INVALID_SOCKET)
    {
        SocketException ex(__FILE__, __LINE__);
        ex.error = getSocketErrno();
        throw ex;
    }

    if(!udp)
    {
        setTcpNoDelay(fd);
        setKeepAlive(fd);
    }

    return fd;
}

void
IceInternal::closeSocket(SOCKET fd)
{
#ifdef _WIN32
    int error = WSAGetLastError();
    if(closesocket(fd) == SOCKET_ERROR)
    {
        SocketException ex(__FILE__, __LINE__);
        ex.error = getSocketErrno();
        throw ex;
    }
    WSASetLastError(error);
#else
    int error = errno;
    if(close(fd) == SOCKET_ERROR)
    {
        SocketException ex(__FILE__, __LINE__);
        ex.error = getSocketErrno();
        throw ex;
    }
    errno = error;
#endif
}

void
IceInternal::closeSocketNoThrow(SOCKET fd)
{
#ifdef _WIN32
    int error = WSAGetLastError();
    closesocket(fd);
    WSASetLastError(error);
#else
    int error = errno;
    close(fd);
    errno = error;
#endif
}
    
void
IceInternal::shutdownSocketWrite(SOCKET fd)
{
    if(shutdown(fd, SHUT_WR) == SOCKET_ERROR)
    {
        //
        // Ignore errors indicating that we are shutdown already.
        //
#if defined(_WIN32)
        int error = WSAGetLastError();
	//
	// Under Vista its possible to get a WSAECONNRESET. See
	// http://bugzilla.zeroc.com/bugzilla/show_bug.cgi?id=1739 for
	// some details.
	//
        if(error == WSAENOTCONN || error == WSAECONNRESET)
        {
            return;
        }
#elif defined(__APPLE__) || defined(__FreeBSD__)
        if(errno == ENOTCONN || errno == EINVAL)
        {
            return;
        }
#else
        if(errno == ENOTCONN)
        {
            return;
        }
#endif
        SocketException ex(__FILE__, __LINE__);
        ex.error = getSocketErrno();
        throw ex;
    }
}
    
void
IceInternal::shutdownSocketReadWrite(SOCKET fd)
{
    if(shutdown(fd, SHUT_RDWR) == SOCKET_ERROR)
    {
        //
        // Ignore errors indicating that we are shutdown already.
        //
#if defined(_WIN32)
        int error = WSAGetLastError();
	//
	// Under Vista its possible to get a WSAECONNRESET. See
	// http://bugzilla.zeroc.com/bugzilla/show_bug.cgi?id=1739 for
	// some details.
	//
        if(error == WSAENOTCONN || error == WSAECONNRESET)
        {
            return;
        }
#elif defined(__APPLE__) || defined(__FreeBSD__)
        if(errno == ENOTCONN || errno == EINVAL)
        {
            return;
        }
#else
        if(errno == ENOTCONN)
        {
            return;
        }
#endif

        SocketException ex(__FILE__, __LINE__);
        ex.error = getSocketErrno();
        throw ex;
    }
}
    
void
IceInternal::setBlock(SOCKET fd, bool block)
{
    if(block)
    {
#ifdef _WIN32
        unsigned long arg = 0;
        if(ioctlsocket(fd, FIONBIO, &arg) == SOCKET_ERROR)
        {
            closeSocketNoThrow(fd);
            SocketException ex(__FILE__, __LINE__);
            ex.error = WSAGetLastError();
            throw ex;
        }
#else
        int flags = fcntl(fd, F_GETFL);
        flags &= ~O_NONBLOCK;
        if(fcntl(fd, F_SETFL, flags) == SOCKET_ERROR)
        {
            closeSocketNoThrow(fd);
            SocketException ex(__FILE__, __LINE__);
            ex.error = errno;
            throw ex;
        }
#endif
    }
    else
    {
#ifdef _WIN32
        unsigned long arg = 1;
        if(ioctlsocket(fd, FIONBIO, &arg) == SOCKET_ERROR)
        {
            closeSocketNoThrow(fd);
            SocketException ex(__FILE__, __LINE__);
            ex.error = WSAGetLastError();
            throw ex;
        }
#else
        int flags = fcntl(fd, F_GETFL);
        flags |= O_NONBLOCK;
        if(fcntl(fd, F_SETFL, flags) == SOCKET_ERROR)
        {
            closeSocketNoThrow(fd);
            SocketException ex(__FILE__, __LINE__);
            ex.error = errno;
            throw ex;
        }
#endif
    }
}

void
IceInternal::setTcpNoDelay(SOCKET fd)
{
    int flag = 1;
    if(setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, int(sizeof(int))) == SOCKET_ERROR)
    {
        closeSocketNoThrow(fd);
        SocketException ex(__FILE__, __LINE__);
        ex.error = getSocketErrno();
        throw ex;
    }
}
    
void
IceInternal::setKeepAlive(SOCKET fd)
{
    int flag = 1;
    if(setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (char*)&flag, int(sizeof(int))) == SOCKET_ERROR)
    {
        closeSocketNoThrow(fd);
        SocketException ex(__FILE__, __LINE__);
        ex.error = getSocketErrno();
        throw ex;
    }
}

void
IceInternal::setSendBufferSize(SOCKET fd, int sz)
{
    if(setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (char*)&sz, int(sizeof(int))) == SOCKET_ERROR)
    {
        closeSocketNoThrow(fd);
        SocketException ex(__FILE__, __LINE__);
        ex.error = getSocketErrno();
        throw ex;
    }
}

int
IceInternal::getSendBufferSize(SOCKET fd)
{
    int sz;
    socklen_t len = sizeof(sz);
    if(getsockopt(fd, SOL_SOCKET, SO_SNDBUF, (char*)&sz, &len) == SOCKET_ERROR || len != sizeof(sz))
    {
        closeSocketNoThrow(fd);
        SocketException ex(__FILE__, __LINE__);
        ex.error = getSocketErrno();
        throw ex;
    }
    return sz;
}

void
IceInternal::setRecvBufferSize(SOCKET fd, int sz)
{
    if(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (char*)&sz, int(sizeof(int))) == SOCKET_ERROR)
    {
        closeSocketNoThrow(fd);
        SocketException ex(__FILE__, __LINE__);
        ex.error = getSocketErrno();
        throw ex;
    }
}

int
IceInternal::getRecvBufferSize(SOCKET fd)
{
    int sz;
    socklen_t len = sizeof(sz);
    if(getsockopt(fd, SOL_SOCKET, SO_RCVBUF, (char*)&sz, &len) == SOCKET_ERROR || len != sizeof(sz))
    {
        closeSocketNoThrow(fd);
        SocketException ex(__FILE__, __LINE__);
        ex.error = getSocketErrno();
        throw ex;
    }
    return sz;
}

void
IceInternal::setMcastGroup(SOCKET fd, const struct sockaddr_storage& group, const string& interface)
{
    int rc;
    if(group.ss_family == AF_INET)
    {
        struct ip_mreq mreq;
        mreq.imr_multiaddr = reinterpret_cast<const struct sockaddr_in*>(&group)->sin_addr;
        struct sockaddr_storage addr;
        getAddress(interface, 0, addr, EnableIPv4);
        mreq.imr_interface = reinterpret_cast<const struct sockaddr_in*>(&addr)->sin_addr;
        rc = setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char*)&mreq, int(sizeof(mreq)));
    }
    else
    {
        struct ipv6_mreq mreq;
        mreq.ipv6mr_multiaddr = reinterpret_cast<const struct sockaddr_in6*>(&group)->sin6_addr;
        mreq.ipv6mr_interface = atoi(interface.c_str()); 
        rc = setsockopt(fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, (char*)&mreq, int(sizeof(mreq)));
    }
    if(rc == SOCKET_ERROR)
    {
        closeSocketNoThrow(fd);
        SocketException ex(__FILE__, __LINE__);
        ex.error = getSocketErrno();
        throw ex;
    }
}

void
IceInternal::setMcastInterface(SOCKET fd, const string& interface, bool IPv4)
{
    int rc;
    if(IPv4)
    {
        struct sockaddr_storage addr;
        getAddress(interface, 0, addr, EnableIPv4);
        struct in_addr iface = reinterpret_cast<const struct sockaddr_in*>(&addr)->sin_addr;
        rc = setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, (char*)&iface, int(sizeof(iface)));
    }
    else
    {
        int interfaceNum = atoi(interface.c_str());
        rc = setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, (char*)&interfaceNum, int(sizeof(int)));
    }
    if(rc == SOCKET_ERROR)
    {
        closeSocketNoThrow(fd);
        SocketException ex(__FILE__, __LINE__);
        ex.error = getSocketErrno();
        throw ex;
    }
}

void
IceInternal::setMcastTtl(SOCKET fd, int ttl, bool IPv4)
{
    int rc;
    if(IPv4)
    {
        rc = setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, (char*)&ttl, int(sizeof(int)));
    }
    else
    {
        rc = setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, (char*)&ttl, int(sizeof(int)));
    }
    if(rc == SOCKET_ERROR)
    {
        closeSocketNoThrow(fd);
        SocketException ex(__FILE__, __LINE__);
        ex.error = getSocketErrno();
        throw ex;
    }
}

void
IceInternal::setReuseAddress(SOCKET fd, bool reuse)
{
    int flag = reuse ? 1 : 0;
    if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char*)&flag, int(sizeof(int))) == SOCKET_ERROR)
    {
        closeSocketNoThrow(fd);
        SocketException ex(__FILE__, __LINE__);
        ex.error = getSocketErrno();
        throw ex;
    }
}

void
IceInternal::doBind(SOCKET fd, struct sockaddr_storage& addr)
{
    int size;
    if(addr.ss_family == AF_INET)
    {
        size = sizeof(sockaddr_in);
    }
    else if(addr.ss_family == AF_INET6)
    {
        size = sizeof(sockaddr_in6);
    }
    else
    {
        assert(false);
        size = 0; // Keep the compiler happy.
    }

    if(bind(fd, reinterpret_cast<struct sockaddr*>(&addr), size) == SOCKET_ERROR)
    {
        closeSocketNoThrow(fd);
        SocketException ex(__FILE__, __LINE__);
        ex.error = getSocketErrno();
        throw ex;
    }

    socklen_t len = static_cast<socklen_t>(sizeof(addr));
#ifdef NDEBUG
    getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &len);
#else
    int ret = getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &len);
    assert(ret != SOCKET_ERROR);
#endif
}

void
IceInternal::doListen(SOCKET fd, int backlog)
{
repeatListen:
    if(::listen(fd, backlog) == SOCKET_ERROR)
    {
        if(interrupted())
        {
            goto repeatListen;
        }
        
        closeSocketNoThrow(fd);
        SocketException ex(__FILE__, __LINE__);
        ex.error = getSocketErrno();
        throw ex;
    }
}

bool
IceInternal::doConnect(SOCKET fd, struct sockaddr_storage& addr, int timeout)
{
repeatConnect:
    int size;
    if(addr.ss_family == AF_INET)
    {
        size = sizeof(sockaddr_in);
    }
    else if(addr.ss_family == AF_INET6)
    {
        size = sizeof(sockaddr_in6);
    }
    else
    {
        assert(false);
        size = 0; // Keep the compiler happy.
    }

    if(::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), size) == SOCKET_ERROR)
    {
        if(interrupted())
        {
            goto repeatConnect;
        }
        
        if(connectInProgress())
        {
            if(timeout == 0)
            {
                return false;
            }

            try
            {
                doFinishConnect(fd, timeout);
            }
            catch(const Ice::LocalException&)
            {
                closeSocketNoThrow(fd);
                throw;
            }
            return true;
        }
    
        closeSocketNoThrow(fd);
        if(connectionRefused())
        {
            ConnectionRefusedException ex(__FILE__, __LINE__);
            ex.error = getSocketErrno();
            throw ex;
        }
        else if(connectFailed())
        {
            ConnectFailedException ex(__FILE__, __LINE__);
            ex.error = getSocketErrno();
            throw ex;
        }
        else
        {
            SocketException ex(__FILE__, __LINE__);
            ex.error = getSocketErrno();
            throw ex;
        }
    }

#if defined(__linux)
    //
    // Prevent self connect (self connect happens on Linux when a client tries to connect to
    // a server which was just deactivated if the client socket re-uses the same ephemeral
    // port as the server).
    //
    struct sockaddr_storage localAddr;
    fdToLocalAddress(fd, localAddr);
    if(compareAddress(addr, localAddr) == 0)
    {
        ConnectionRefusedException ex(__FILE__, __LINE__);
        ex.error = 0; // No appropriate errno
        throw ex;
    }
#endif
    return true;
}

void
IceInternal::doFinishConnect(SOCKET fd, int timeout)
{
    //
    // Note: we don't close the socket if there's an exception. It's the responsability
    // of the caller to do so.
    //

    if(timeout != 0)
    {
    repeatSelect:
#ifdef _WIN32
        fd_set wFdSet;
        fd_set eFdSet;
        FD_ZERO(&wFdSet);
        FD_ZERO(&eFdSet);
        FD_SET(fd, &wFdSet);
        FD_SET(fd, &eFdSet);

        int ret;
        if(timeout >= 0)
        {
            struct timeval tv;
            tv.tv_sec = timeout / 1000;
            tv.tv_usec = (timeout - tv.tv_sec * 1000) * 1000;
            ret = ::select(static_cast<int>(fd + 1), 0, &wFdSet, &eFdSet, &tv);
        }
        else
        {
            ret = ::select(static_cast<int>(fd + 1), 0, &wFdSet, &eFdSet, 0);
        }
#else
        struct pollfd pollFd[1];
        pollFd[0].fd = fd;
        pollFd[0].events = POLLOUT;
        int ret = ::poll(pollFd, 1, timeout);
#endif
        if(ret == 0)
        {
            throw ConnectTimeoutException(__FILE__, __LINE__);
        }
        else if(ret == SOCKET_ERROR)
        {
            if(interrupted())
            {
                goto repeatSelect;
            }
                
            SocketException ex(__FILE__, __LINE__);
            ex.error = getSocketErrno();
            throw ex;
        }
    }

    int val;

    //
    // Strange windows bug: The following call to Sleep() is
    // necessary, otherwise no error is reported through
    // getsockopt.
    //
#ifdef _WIN32
    Sleep(0);
#endif
    socklen_t len = static_cast<socklen_t>(sizeof(int));
    if(getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&val), &len) == SOCKET_ERROR)
    {
        SocketException ex(__FILE__, __LINE__);
        ex.error = getSocketErrno();
        throw ex;
    }
            
    if(val > 0)
    {
#ifdef _WIN32
        WSASetLastError(val);
#else
        errno = val;
#endif
        if(connectionRefused())
        {
            ConnectionRefusedException ex(__FILE__, __LINE__);
            ex.error = getSocketErrno();
            throw ex;
        }
        else if(connectFailed())
        {
            ConnectFailedException ex(__FILE__, __LINE__);
            ex.error = getSocketErrno();
            throw ex;
        }
        else
        {
            SocketException ex(__FILE__, __LINE__);
            ex.error = getSocketErrno();
            throw ex;
        }
    }

#if defined(__linux)
    //
    // Prevent self connect (self connect happens on Linux when a client tries to connect to
    // a server which was just deactivated if the client socket re-uses the same ephemeral
    // port as the server).
    //
    struct sockaddr_storage localAddr;
    fdToLocalAddress(fd, localAddr);
    struct sockaddr_storage remoteAddr;
    if(fdToRemoteAddress(fd, remoteAddr) && compareAddress(remoteAddr, localAddr) == 0)
    {
        ConnectionRefusedException ex(__FILE__, __LINE__);
        ex.error = 0; // No appropriate errno
        throw ex;
    }
#endif
}

SOCKET
IceInternal::doAccept(SOCKET fd, int timeout)
{
#ifdef _WIN32
    SOCKET ret;
#else
    int ret;
#endif

repeatAccept:
    if((ret = ::accept(fd, 0, 0)) == INVALID_SOCKET)
    {
        if(acceptInterrupted())
        {
            goto repeatAccept;
        }

        if(wouldBlock())
        {
        repeatSelect:
            int rs;
#ifdef _WIN32
            fd_set fdSet;
            FD_ZERO(&fdSet);
            FD_SET(fd, &fdSet);
            if(timeout >= 0)
            {
                struct timeval tv;
                tv.tv_sec = timeout / 1000;
                tv.tv_usec = (timeout - tv.tv_sec * 1000) * 1000;
                rs = ::select(static_cast<int>(fd + 1), &fdSet, 0, 0, &tv);
            }
            else
            {
                rs = ::select(static_cast<int>(fd + 1), &fdSet, 0, 0, 0);
            }
#else
            struct pollfd pollFd[1];
            pollFd[0].fd = fd;
            pollFd[0].events = POLLIN;
            rs = ::poll(pollFd, 1, timeout);
#endif
            
            if(rs == SOCKET_ERROR)
            {
                if(interrupted())
                {
                    goto repeatSelect;
                }
                
                SocketException ex(__FILE__, __LINE__);
                ex.error = getSocketErrno();
                throw ex;
            }
            
            if(rs == 0)
            {
                throw TimeoutException(__FILE__, __LINE__);
            }
            
            goto repeatAccept;
        }
        
        SocketException ex(__FILE__, __LINE__);
        ex.error = getSocketErrno();
        throw ex;
    }

    setTcpNoDelay(ret);
    setKeepAlive(ret);
    return ret;
}

void
IceInternal::getAddressForServer(const string& host, int port, struct sockaddr_storage& addr, ProtocolSupport protocol)
{
    getAddressImpl(host, port, addr, protocol, true);
}

void
IceInternal::getAddress(const string& host, int port, struct sockaddr_storage& addr, ProtocolSupport protocol)
{
    getAddressImpl(host, port, addr, protocol, false);
}

vector<struct sockaddr_storage>
IceInternal::getAddresses(const string& host, int port, ProtocolSupport protocol, bool blocking)
{
    //
    // We now use getaddrinfo() on Windows.
    //
// #ifdef _WIN32
//         //
//         // Windows XP has getaddrinfo(), but we don't want to require XP to run Ice.
//         //
        
//         //
//         // gethostbyname() is thread safe on Windows, with a separate hostent per thread
//         //
//         struct hostent* entry = 0;
//         int retry = 5;

//         do
//         {
//             entry = gethostbyname(host.c_str());
//         }
//         while(entry == 0 && h_errno == TRY_AGAIN && --retry >= 0);
    
//         if(entry == 0)
//         {
//             DNSException ex(__FILE__, __LINE__);
//             ex.error = h_errno;
//             ex.host = host;
//             throw ex;
//         }

//         char** p = entry->h_addr_list;
//         while(*p)
//         {
//             memcpy(&addr.sin_addr, *p, entry->h_length);
//             result.push_back(addr);
//             p++;
//         } 

// #else

    vector<struct sockaddr_storage> result;
    struct sockaddr_storage addr;
    memset(&addr, 0, sizeof(struct sockaddr_storage));

    struct addrinfo* info = 0;
    int retry = 5;

    struct addrinfo hints = { 0 };
    if(protocol == EnableIPv4)
    {
        hints.ai_family = PF_INET;
    }
    else if(protocol == EnableIPv6)
    {
        hints.ai_family = PF_INET6;
    }
    else
    {
        hints.ai_family = PF_UNSPEC;
    }

    if(!blocking)
    {
        hints.ai_flags = AI_NUMERICHOST;
    }
        
    int rs = 0;
    do
    {
        if(host.empty())
        {
            rs = getaddrinfo(0, "1", &hints, &info); // Get the address of the loopback interface
        }
        else
        {
            rs = getaddrinfo(host.c_str(), 0, &hints, &info);
        }
    }
    while(info == 0 && rs == EAI_AGAIN && --retry >= 0);

    if(!blocking && rs == EAI_NONAME)
    {
        return result; // Empty result indicates that a blocking lookup is necessary.
    }
    else if(rs != 0)
    {
        DNSException ex(__FILE__, __LINE__);
        ex.error = rs;
        ex.host = host;
        throw ex;
    }

    struct addrinfo* p;
    for(p = info; p != NULL; p = p->ai_next)
    {
        memcpy(&addr, p->ai_addr, p->ai_addrlen);
        if(p->ai_family == PF_INET)
        {
            struct sockaddr_in* addrin = reinterpret_cast<sockaddr_in*>(&addr);
            addrin->sin_port = htons(port);
        }
        else if(p->ai_family == PF_INET6)
        {
            struct sockaddr_in6* addrin6 = reinterpret_cast<sockaddr_in6*>(&addr);
            addrin6->sin6_port = htons(port);
        }

        bool found = false;
        for(unsigned int i = 0; i < result.size(); ++i)
        {
            if(compareAddress(result[i], addr) == 0)
            {
                found = true;
                break;
            }
        }
        if(!found)
        {
            result.push_back(addr);
        }
    }

    freeaddrinfo(info);

    if(result.size() == 0)
    {
        DNSException ex(__FILE__, __LINE__);
        ex.host = host;
        throw ex;
    }

    return result;
}

int
IceInternal::compareAddress(const struct sockaddr_storage& addr1, const struct sockaddr_storage& addr2)
{
    if(addr1.ss_family < addr2.ss_family)
    {
        return -1;
    }
    else if(addr2.ss_family < addr1.ss_family)
    {
        return 1;
    }

    if(addr1.ss_family == AF_INET)
    {
        const struct sockaddr_in* addr1in = reinterpret_cast<const sockaddr_in*>(&addr1);
        const struct sockaddr_in* addr2in = reinterpret_cast<const sockaddr_in*>(&addr2);

        if(addr1in->sin_port < addr2in->sin_port)
        {
            return -1;
        }
        else if(addr2in->sin_port < addr1in->sin_port)
        {
            return 1;
        }

        if(addr1in->sin_addr.s_addr < addr2in->sin_addr.s_addr)
        {
            return -1;
        }
        else if(addr2in->sin_addr.s_addr < addr1in->sin_addr.s_addr)
        {
            return 1;
        }
    }
    else
    {
        const struct sockaddr_in6* addr1in = reinterpret_cast<const sockaddr_in6*>(&addr1);
        const struct sockaddr_in6* addr2in = reinterpret_cast<const sockaddr_in6*>(&addr2);

        if(addr1in->sin6_port < addr2in->sin6_port)
        {
            return -1;
        }
        else if(addr2in->sin6_port < addr1in->sin6_port)
        {
            return 1;
        }
        
        int res = memcmp(&addr1in->sin6_addr, &addr2in->sin6_addr, sizeof(struct in6_addr));
        if(res < 0)
        {
            return -1;
        }
        else if(res > 0)
        {
            return 1;
        }
    }

    return 0;
}

void
IceInternal::createPipe(SOCKET fds[2])
{
#ifdef _WIN32

    SOCKET fd = createSocket(false, AF_INET);
    setBlock(fd, true);
    
    struct sockaddr_storage addr;
    memset(&addr, 0, sizeof(addr));

    struct sockaddr_in* addrin = reinterpret_cast<struct sockaddr_in*>(&addr);
    addrin->sin_family = AF_INET;
    addrin->sin_port = htons(0);
    addrin->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    
    doBind(fd, addr);
    doListen(fd, 1);

    try
    {
        fds[0] = createSocket(false, AF_INET);
    }
    catch(...)
    {
        ::closesocket(fd);
        throw;
    }

    try
    {
        setBlock(fds[0], true);
        doConnect(fds[0], addr, -1);
    }
    catch(...)
    {
        ::closesocket(fd);
        throw;
    }

    try
    {
        fds[1] = doAccept(fd, -1);
    }
    catch(...)
    {
        ::closesocket(fds[0]);
        ::closesocket(fd);
        throw;
    }

    ::closesocket(fd);

    try
    {
        setBlock(fds[1], true);
    }
    catch(...)
    {
        ::closesocket(fds[0]);
        ::closesocket(fd);
        throw;
    }

#else

    if(::pipe(fds) != 0)
    {
        SyscallException ex(__FILE__, __LINE__);
        ex.error = getSystemErrno();
        throw ex;
    }

    try
    {
        setBlock(fds[0], true);
    }
    catch(...)
    {
        closeSocketNoThrow(fds[1]);
        throw;
    }

    try
    {
        setBlock(fds[1], true);
    }
    catch(...)
    {
        closeSocketNoThrow(fds[0]);
        throw;
    }

#endif
}

#ifdef _WIN32

string
IceInternal::errorToString(int error)
{
    if(error < WSABASEERR)
    {
        LPVOID lpMsgBuf = 0;
        DWORD ok = FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                                 FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                                 NULL,
                                 error,
                                 MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), // Default language
                                 (LPTSTR)&lpMsgBuf,
                                 0,
                                 NULL);
        if(ok)
        {
            LPCTSTR msg = (LPCTSTR)lpMsgBuf;
            assert(msg && strlen((const char*)msg) > 0);
            string result = (const char*)msg;
            if(result[result.length() - 1] == '\n')
            {
                result = result.substr(0, result.length() - 2);
            }
            LocalFree(lpMsgBuf);
            return result;
        }
        else
        {
            ostringstream os;
            os << "unknown error: " << error;
            return os.str();
        }
    }

    switch(error)
    {
    case WSAEINTR:
        return "WSAEINTR";
        
    case WSAEBADF:
        return "WSAEBADF";
        
    case WSAEACCES:
        return "WSAEACCES";
        
    case WSAEFAULT:
        return "WSAEFAULT";
        
    case WSAEINVAL:
        return "WSAEINVAL";
        
    case WSAEMFILE:
        return "WSAEMFILE";
        
    case WSAEWOULDBLOCK:
        return "WSAEWOULDBLOCK";
        
    case WSAEINPROGRESS:
        return "WSAEINPROGRESS";
        
    case WSAEALREADY:
        return "WSAEALREADY";
        
    case WSAENOTSOCK:
        return "WSAENOTSOCK";
        
    case WSAEDESTADDRREQ:
        return "WSAEDESTADDRREQ";
        
    case WSAEMSGSIZE:
        return "WSAEMSGSIZE";
        
    case WSAEPROTOTYPE:
        return "WSAEPROTOTYPE";
        
    case WSAENOPROTOOPT:
        return "WSAENOPROTOOPT";
        
    case WSAEPROTONOSUPPORT:
        return "WSAEPROTONOSUPPORT";
        
    case WSAESOCKTNOSUPPORT:
        return "WSAESOCKTNOSUPPORT";
        
    case WSAEOPNOTSUPP:
        return "WSAEOPNOTSUPP";
        
    case WSAEPFNOSUPPORT:
        return "WSAEPFNOSUPPORT";
        
    case WSAEAFNOSUPPORT:
        return "WSAEAFNOSUPPORT";
        
    case WSAEADDRINUSE:
        return "WSAEADDRINUSE";
        
    case WSAEADDRNOTAVAIL:
        return "WSAEADDRNOTAVAIL";
        
    case WSAENETDOWN:
        return "WSAENETDOWN";
        
    case WSAENETUNREACH:
        return "WSAENETUNREACH";
        
    case WSAENETRESET:
        return "WSAENETRESET";
        
    case WSAECONNABORTED:
        return "WSAECONNABORTED";
        
    case WSAECONNRESET:
        return "WSAECONNRESET";
        
    case WSAENOBUFS:
        return "WSAENOBUFS";
        
    case WSAEISCONN:
        return "WSAEISCONN";
        
    case WSAENOTCONN:
        return "WSAENOTCONN";
        
    case WSAESHUTDOWN:
        return "WSAESHUTDOWN";
        
    case WSAETOOMANYREFS:
        return "WSAETOOMANYREFS";
        
    case WSAETIMEDOUT:
        return "WSAETIMEDOUT";
        
    case WSAECONNREFUSED:
        return "WSAECONNREFUSED";
        
    case WSAELOOP:
        return "WSAELOOP";
        
    case WSAENAMETOOLONG:
        return "WSAENAMETOOLONG";
        
    case WSAEHOSTDOWN:
        return "WSAEHOSTDOWN";
        
    case WSAEHOSTUNREACH:
        return "WSAEHOSTUNREACH";
        
    case WSAENOTEMPTY:
        return "WSAENOTEMPTY";
        
    case WSAEPROCLIM:
        return "WSAEPROCLIM";
        
    case WSAEUSERS:
        return "WSAEUSERS";
        
    case WSAEDQUOT:
        return "WSAEDQUOT";
        
    case WSAESTALE:
        return "WSAESTALE";
        
    case WSAEREMOTE:
        return "WSAEREMOTE";
        
    case WSAEDISCON:
        return "WSAEDISCON";
        
    case WSASYSNOTREADY:
        return "WSASYSNOTREADY";
        
    case WSAVERNOTSUPPORTED:
        return "WSAVERNOTSUPPORTED";
        
    case WSANOTINITIALISED:
        return "WSANOTINITIALISED";
        
    case WSAHOST_NOT_FOUND:
        return "WSAHOST_NOT_FOUND";
        
    case WSATRY_AGAIN:
        return "WSATRY_AGAIN";
        
    case WSANO_RECOVERY:
        return "WSANO_RECOVERY";
        
    case WSANO_DATA:
        return "WSANO_DATA";

    default:
    {
        ostringstream os;
        os << "unknown socket error: " << error;
        return os.str();
    }
    }
}

string
IceInternal::errorToStringDNS(int error)
{
    return errorToString(error);
}

#else

string
IceInternal::errorToString(int error)
{
    return strerror(error);
}

string
IceInternal::errorToStringDNS(int error)
{
    return gai_strerror(error);
}

#endif

string
IceInternal::lastErrorToString()
{
#ifdef _WIN32
    return errorToString(WSAGetLastError());
#else
    return errorToString(errno);
#endif
}

std::string
IceInternal::fdToString(SOCKET fd)
{
    if(fd == INVALID_SOCKET)
    {
        return "<closed>";
    }

    struct sockaddr_storage localAddr;
    fdToLocalAddress(fd, localAddr);

    struct sockaddr_storage remoteAddr;
    bool peerConnected = fdToRemoteAddress(fd, remoteAddr);

    return addressesToString(localAddr, remoteAddr, peerConnected);
};

std::string
IceInternal::addressesToString(const struct sockaddr_storage& localAddr, const struct sockaddr_storage& remoteAddr,
                               bool peerConnected)
{
    ostringstream s;
    s << "local address = " << addrToString(localAddr);
    if(peerConnected)
    {
        s << "\nremote address = " << addrToString(remoteAddr);
    }
    else
    {
        s << "\nremote address = <not connected>";
    }
    return s.str();
}

void
IceInternal::fdToLocalAddress(SOCKET fd, struct sockaddr_storage& addr)
{
    socklen_t len = static_cast<socklen_t>(sizeof(struct sockaddr_storage));
    if(getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &len) == SOCKET_ERROR)
    {
        closeSocketNoThrow(fd);
        SocketException ex(__FILE__, __LINE__);
        ex.error = getSocketErrno();
        throw ex;
    }
}

bool
IceInternal::fdToRemoteAddress(SOCKET fd, struct sockaddr_storage& addr)
{
    socklen_t len = static_cast<socklen_t>(sizeof(struct sockaddr_storage));
    if(getpeername(fd, reinterpret_cast<struct sockaddr*>(&addr), &len) == SOCKET_ERROR)
    {
        if(notConnected())
        {
            return false;
        }
        else
        {
            closeSocketNoThrow(fd);
            SocketException ex(__FILE__, __LINE__);
            ex.error = getSocketErrno();
            throw ex;
        }
    }

    return true;
}

string
IceInternal::inetAddrToString(const struct sockaddr_storage& ss)
{
    int size;
    if(ss.ss_family == AF_INET)
    {
        size = sizeof(sockaddr_in);
    }
    else if(ss.ss_family == AF_INET6)
    {
        size = sizeof(sockaddr_in6);
    }

    char namebuf[1024];
    namebuf[0] = '\0';
    getnameinfo(reinterpret_cast<const struct sockaddr *>(&ss), size, namebuf, sizeof(namebuf), 0, 0, NI_NUMERICHOST);
    return string(namebuf);
}

string
IceInternal::addrToString(const struct sockaddr_storage& addr)
{
    ostringstream s;
    string port;
    s << inetAddrToString(addr) << ':';
    if(addr.ss_family == AF_INET)
    {
        const struct sockaddr_in* addrin = reinterpret_cast<const sockaddr_in*>(&addr);
        s << ntohs(addrin->sin_port);
    }
    else
    {
        const struct sockaddr_in6* addrin = reinterpret_cast<const sockaddr_in6*>(&addr);
        s << ntohs(addrin->sin6_port);
    }
    return s.str();
}

int
IceInternal::getPort(const struct sockaddr_storage& addr)
{
    if(addr.ss_family == AF_INET)
    {
        return ntohs(reinterpret_cast<const sockaddr_in*>(&addr)->sin_port);
    }
    else
    {
        return ntohs(reinterpret_cast<const sockaddr_in6*>(&addr)->sin6_port);
    }
}

vector<string>
IceInternal::getHostsForEndpointExpand(const string& host, ProtocolSupport protocolSupport)
{
    vector<string> hosts;
    if(host.empty() || isWildcard(host, protocolSupport))
    {
        vector<struct sockaddr_storage> addrs = getLocalAddresses(protocolSupport);
        for(vector<struct sockaddr_storage>::const_iterator p = addrs.begin(); p != addrs.end(); ++p)
        {
            //
            // NOTE: We don't publish link-local IPv6 addresses as these addresses can only 
            // be accessed in general with a scope-id.
            //
            if(p->ss_family != AF_INET6 || 
               !IN6_IS_ADDR_LINKLOCAL(&reinterpret_cast<const struct sockaddr_in6*>(&(*p))->sin6_addr))
            {
                hosts.push_back(inetAddrToString(*p));
            }
        }

        if(hosts.empty())
        {
            if(protocolSupport != EnableIPv6)
            {
                hosts.push_back("127.0.0.1");
            }
            if(protocolSupport != EnableIPv4)
            {
                hosts.push_back("0:0:0:0:0:0:0:1");
            }
        }
    }
    return hosts; // An empty host list indicates to just use the given host.
}

void
IceInternal::setTcpBufSize(SOCKET fd, const Ice::PropertiesPtr& properties, const Ice::LoggerPtr& logger)
{
    assert(fd != INVALID_SOCKET);

    //
    // By default, on Windows we use a 128KB buffer size. On Unix
    // platforms, we use the system defaults.
    //
#ifdef _WIN32
    const int dfltBufSize = 128 * 1024;
#else
    const int dfltBufSize = 0;
#endif
    Int sizeRequested;

    sizeRequested = properties->getPropertyAsIntWithDefault("Ice.TCP.RcvSize", dfltBufSize);
    if(sizeRequested > 0)
    {
        //
        // Try to set the buffer size. The kernel will silently adjust
        // the size to an acceptable value. Then read the size back to
        // get the size that was actually set.
        //
        setRecvBufferSize(fd, sizeRequested);
        int size = getRecvBufferSize(fd);
        if(size < sizeRequested) // Warn if the size that was set is less than the requested size.
        {
            Ice::Warning out(logger);
            out << "TCP receive buffer size: requested size of " << sizeRequested << " adjusted to " << size;
        }
    }

    sizeRequested = properties->getPropertyAsIntWithDefault("Ice.TCP.SndSize", dfltBufSize);
    if(sizeRequested > 0)
    {
        //
        // Try to set the buffer size. The kernel will silently adjust
        // the size to an acceptable value. Then read the size back to
        // get the size that was actually set.
        //
        setSendBufferSize(fd, sizeRequested);
        int size = getSendBufferSize(fd);
        if(size < sizeRequested) // Warn if the size that was set is less than the requested size.
        {
            Ice::Warning out(logger);
            out << "TCP send buffer size: requested size of " << sizeRequested << " adjusted to " << size;
        }
    }
}
