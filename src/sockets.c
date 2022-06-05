/*
 *  Copyright (C) 2022 Andri Yngvasin <andri@yngvason.is>
 *  Copyright (C) 2011-2012 Christian Beier <dontmind@freeshell.org>
 *  Copyright (C) 1999 AT&T Laboratories Cambridge.  All Rights Reserved.
 *
 *  This is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This software is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this software; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 *  USA.
 */

/*
 * sockets.c - functions to deal with sockets.
 */

#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <assert.h>
#include <sys/param.h>
#include "rfb/rfbclient.h"
#include "sockets.h"
#include "tls.h"
#include "sasl.h"

void run_main_loop_once(void);

rfbBool errorMessageOnReadFailure = TRUE;

void ReadToBuffer(rfbClient* client) {
	if (client->buffered == RFB_BUF_SIZE)
		return;

	ssize_t size;

#if defined(LIBVNCSERVER_HAVE_GNUTLS) || defined(LIBVNCSERVER_HAVE_LIBSSL)
	if (client->tlsSession) {
		size = ReadFromTLS(client, client->buf + client->buffered,
				RFB_BUF_SIZE - client->buffered);
	} else
#endif
#ifdef LIBVNCSERVER_HAVE_SASL
	if (client->saslconn) {
		size = ReadFromSASL(client, client->buf + client->buffered,
				RFB_BUF_SIZE - client->buffered);
	} else
#endif
	{
		size = recv(client->sock, client->buf + client->buffered,
				RFB_BUF_SIZE - client->buffered, MSG_DONTWAIT);
	}

	if (size > 0)
		client->buffered += size;
}

rfbBool ReadFromRFBServer(rfbClient* client, char *out, unsigned int n)
{
	if (!out)
		return FALSE;

	while (n != 0) {
		while (n != 0 && client->buffered == 0) {
			run_main_loop_once();
			ReadToBuffer(client);
		}

		unsigned int size = MIN(client->buffered, n);
		memcpy(out, client->buf, size);

		client->buffered -= size;
		memmove(client->buf, client->buf + size, client->buffered);

		out += size;
		n -= size;
	}

	return TRUE;
}

/*
 * Write an exact number of bytes, and don't return until you've sent them.
 */
rfbBool
WriteToRFBServer(rfbClient* client, const char *buf, unsigned int n)
{
  fd_set fds;
  int i = 0;
  int j;
  const char *obuf = buf;
#ifdef LIBVNCSERVER_HAVE_SASL
  const char *output;
  unsigned int outputlen;
  int err;
#endif /* LIBVNCSERVER_HAVE_SASL */

  if (client->serverPort==-1)
    return TRUE; /* vncrec playing */

  if (client->tlsSession) {
    /* WriteToTLS() will guarantee either everything is written, or error/eof returns */
    i = WriteToTLS(client, buf, n);
    if (i <= 0) return FALSE;

    return TRUE;
  }
#ifdef LIBVNCSERVER_HAVE_SASL
  if (client->saslconn) {
    err = sasl_encode(client->saslconn,
                      buf, n,
                      &output, &outputlen);
    if (err != SASL_OK) {
      rfbClientLog("Failed to encode SASL data %s",
                   sasl_errstring(err, NULL, NULL));
      return FALSE;
    }
    obuf = output;
    n = outputlen;
  }
#endif /* LIBVNCSERVER_HAVE_SASL */

  while (i < n) {
    j = write(client->sock, obuf + i, (n - i));
    if (j <= 0) {
      if (j < 0) {
	if (errno == EWOULDBLOCK ||
		errno == EAGAIN) {
	  FD_ZERO(&fds);
	  FD_SET(client->sock,&fds);

	  if (select(client->sock+1, NULL, &fds, NULL, NULL) <= 0) {
	    rfbClientErr("select\n");
	    return FALSE;
	  }
	  j = 0;
	} else {
	  rfbClientErr("write\n");
	  return FALSE;
	}
      } else {
	rfbClientLog("write failed\n");
	return FALSE;
      }
    }
    i += j;
  }
  return TRUE;
}


static rfbBool WaitForConnected(int socket, unsigned int secs)
{
  fd_set writefds;
  fd_set exceptfds;
  struct timeval timeout;

  timeout.tv_sec=secs;
  timeout.tv_usec=0;

  FD_ZERO(&writefds);
  FD_SET(socket, &writefds);
  FD_ZERO(&exceptfds);
  FD_SET(socket, &exceptfds);
  if (select(socket+1, NULL, &writefds, &exceptfds, &timeout)==1) {
    int so_error;
    socklen_t len = sizeof so_error;
    getsockopt(socket, SOL_SOCKET, SO_ERROR, &so_error, &len);
    if (so_error!=0)
      return FALSE;
    return TRUE;
  }

  return FALSE;
}


rfbSocket
ConnectClientToTcpAddr(unsigned int host, int port)
{
  rfbSocket sock = ConnectClientToTcpAddrWithTimeout(host, port, DEFAULT_CONNECT_TIMEOUT);
  /* put socket back into blocking mode for compatibility reasons */
  if (sock != RFB_INVALID_SOCKET) {
    SetBlocking(sock);
  }
  return sock;
}

rfbSocket
ConnectClientToTcpAddrWithTimeout(unsigned int host, int port, unsigned int timeout)
{
  rfbSocket sock;
  struct sockaddr_in addr;
  int one = 1;

  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = host;

  sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock == RFB_INVALID_SOCKET) {
    rfbClientErr("ConnectToTcpAddr: socket (%s)\n",strerror(errno));
    return RFB_INVALID_SOCKET;
  }

  if (!SetNonBlocking(sock))
    return FALSE;

  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    if (!((errno == EWOULDBLOCK || errno == EINPROGRESS) && WaitForConnected(sock, timeout))) {
      rfbClientErr("ConnectToTcpAddr: connect\n");
      rfbCloseSocket(sock);
      return RFB_INVALID_SOCKET;
    }
  }

  if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
		 (char *)&one, sizeof(one)) < 0) {
    rfbClientErr("ConnectToTcpAddr: setsockopt\n");
    rfbCloseSocket(sock);
    return RFB_INVALID_SOCKET;
  }

  return sock;
}

rfbSocket
ConnectClientToTcpAddr6(const char *hostname, int port)
{
  rfbSocket sock = ConnectClientToTcpAddr6WithTimeout(hostname, port, DEFAULT_CONNECT_TIMEOUT);
  /* put socket back into blocking mode for compatibility reasons */
  if (sock != RFB_INVALID_SOCKET) {
    SetBlocking(sock);
  }
  return sock;
}

rfbSocket
ConnectClientToTcpAddr6WithTimeout(const char *hostname, int port, unsigned int timeout)
{
#ifdef LIBVNCSERVER_IPv6
  rfbSocket sock;
  int n;
  struct addrinfo hints, *res, *ressave;
  char port_s[10];
  int one = 1;

  snprintf(port_s, 10, "%d", port);
  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if ((n = getaddrinfo(strcmp(hostname,"") == 0 ? "localhost": hostname, port_s, &hints, &res)))
  {
    rfbClientErr("ConnectClientToTcpAddr6: getaddrinfo (%s)\n", gai_strerror(n));
    return RFB_INVALID_SOCKET;
  }

  ressave = res;
  sock = RFB_INVALID_SOCKET;
  while (res)
  {
    sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock != RFB_INVALID_SOCKET)
    {
      if (SetNonBlocking(sock)) {
        if (connect(sock, res->ai_addr, res->ai_addrlen) == 0) {
          break;
        } else {
          if ((errno == EWOULDBLOCK || errno == EINPROGRESS) && WaitForConnected(sock, timeout))
            break;
          rfbCloseSocket(sock);
          sock = RFB_INVALID_SOCKET;
        }
      } else {
        rfbCloseSocket(sock);
        sock = RFB_INVALID_SOCKET;
      }
    }
    res = res->ai_next;
  }
  freeaddrinfo(ressave);

  if (sock == RFB_INVALID_SOCKET)
  {
    rfbClientErr("ConnectClientToTcpAddr6: connect\n");
    return RFB_INVALID_SOCKET;
  }

  if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
		 (char *)&one, sizeof(one)) < 0) {
    rfbClientErr("ConnectToTcpAddr: setsockopt\n");
    rfbCloseSocket(sock);
    return RFB_INVALID_SOCKET;
  }

  return sock;

#else

  rfbClientErr("ConnectClientToTcpAddr6: IPv6 disabled\n");
  return RFB_INVALID_SOCKET;

#endif
}

rfbSocket
ConnectClientToUnixSock(const char *sockFile)
{
  rfbSocket sock = ConnectClientToUnixSockWithTimeout(sockFile, DEFAULT_CONNECT_TIMEOUT);
  /* put socket back into blocking mode for compatibility reasons */
  if (sock != RFB_INVALID_SOCKET) {
    SetBlocking(sock);
  }
  return sock;
}

rfbSocket
ConnectClientToUnixSockWithTimeout(const char *sockFile, unsigned int timeout)
{
  rfbSocket sock;
  struct sockaddr_un addr;
  addr.sun_family = AF_UNIX;
  if(strlen(sockFile) + 1 > sizeof(addr.sun_path)) {
      rfbClientErr("ConnectToUnixSock: socket file name too long\n");
      return RFB_INVALID_SOCKET;
  }
  strcpy(addr.sun_path, sockFile);

  sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock == RFB_INVALID_SOCKET) {
    rfbClientErr("ConnectToUnixSock: socket (%s)\n",strerror(errno));
    return RFB_INVALID_SOCKET;
  }

  if (!SetNonBlocking(sock))
    return RFB_INVALID_SOCKET;

  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr.sun_family) + strlen(addr.sun_path)) < 0 &&
      !(errno == EINPROGRESS && WaitForConnected(sock, timeout))) {
    rfbClientErr("ConnectToUnixSock: connect\n");
    rfbCloseSocket(sock);
    return RFB_INVALID_SOCKET;
  }

  return sock;
}


/*
 * SetNonBlocking sets a socket into non-blocking mode.
 */

rfbBool
SetNonBlocking(rfbSocket sock)
{
  int flags = fcntl(sock, F_GETFL);
  if(flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
    rfbClientErr("Setting socket to non-blocking failed: %s\n",strerror(errno));
    return FALSE;
  }
  return TRUE;
}


rfbBool SetBlocking(rfbSocket sock)
{
  int flags = fcntl(sock, F_GETFL);
  if(flags < 0 || fcntl(sock, F_SETFL, flags & ~O_NONBLOCK) < 0) {
    rfbClientErr("Setting socket to blocking failed: %s\n",strerror(errno));
    return FALSE;
  }
  return TRUE;
}


/*
 * SetDSCP sets a socket's IP QoS parameters aka Differentiated Services Code Point field
 */

rfbBool
SetDSCP(rfbSocket sock, int dscp)
{
  int level, cmd;
  struct sockaddr addr;
  socklen_t addrlen = sizeof(addr);

  if(getsockname(sock, &addr, &addrlen) != 0) {
    rfbClientErr("Setting socket QoS failed while getting socket address: %s\n",strerror(errno));
    return FALSE;
  }

  switch(addr.sa_family)
    {
#if defined LIBVNCSERVER_IPv6 && defined IPV6_TCLASS
    case AF_INET6:
      level = IPPROTO_IPV6;
      cmd = IPV6_TCLASS;
      break;
#endif
    case AF_INET:
      level = IPPROTO_IP;
      cmd = IP_TOS;
      break;
    default:
      rfbClientErr("Setting socket QoS failed: Not bound to IP address");
      return FALSE;
    }

  if(setsockopt(sock, level, cmd, (void*)&dscp, sizeof(dscp)) != 0) {
    rfbClientErr("Setting socket QoS failed: %s\n", strerror(errno));
    return FALSE;
  }

  return TRUE;
}



/*
 * StringToIPAddr - convert a host string to an IP address.
 */

rfbBool
StringToIPAddr(const char *str, unsigned int *addr)
{
  struct hostent *hp;

  if (strcmp(str,"") == 0) {
    *addr = htonl(INADDR_LOOPBACK); /* local */
    return TRUE;
  }

  *addr = inet_addr(str);

  if (*addr != -1)
    return TRUE;

  hp = gethostbyname(str);

  if (hp) {
    *addr = *(unsigned int *)hp->h_addr;
    return TRUE;
  }

  return FALSE;
}


/*
 * Test if the other end of a socket is on the same machine.
 */

rfbBool
SameMachine(rfbSocket sock)
{
  struct sockaddr_in peeraddr, myaddr;
  socklen_t addrlen = sizeof(struct sockaddr_in);

  getpeername(sock, (struct sockaddr *)&peeraddr, &addrlen);
  getsockname(sock, (struct sockaddr *)&myaddr, &addrlen);

  return (peeraddr.sin_addr.s_addr == myaddr.sin_addr.s_addr);
}
