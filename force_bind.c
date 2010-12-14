/*
 * Description: Force bind on a specified address
 * Author: Catalin(ux) M. BOIE
 * E-mail: catab at embedromix dot ro
 * Web: http://kernel.embedromix.ro/us/
 */

#define __USE_GNU
#define	_GNU_SOURCE
#define __USE_XOPEN2K
#define __USE_LARGEFILE64
#define __USE_FILE_OFFSET64

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <dirent.h>
#include <asm/unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>


static int		(*old_bind)(int sockfd, const struct sockaddr *addr, socklen_t addrlen) = NULL;
static int		(*old_setsockopt)(int sockfd, int level, int optname, const void *optval, socklen_t optlen);
static int		(*old_socket)(int domain, int type, int protocol);
static char		*force_address = NULL;
static int		force_port = -1;
static unsigned int	set_tos = 0, tos;
static unsigned int	set_keepalive = 0, keepalive;

/* Functions */

static void set_ka(int sock)
{
	int flag, ret;

	flag = (keepalive > 0) ? 1 : 0;
	ret = old_setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));
	syslog(LOG_INFO, "force_bind: changing SO_KEEPALIVE to 1 (ret=%d).\n", ret);
}

static void set_ka_idle(int sock)
{
	int ret;

	ret = old_setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepalive, sizeof(keepalive));
	syslog(LOG_INFO, "force_bind: changing TCP_KEEPIDLE to %us (ret=%d).\n",
		keepalive, ret);
}

void init(void)
{
	static unsigned char inited = 0;
	char *x;

	if (inited == 1)
		return;

	inited = 1;

	x = getenv("FORCE_BIND_ADDRESS");
	if (x != NULL) {
		force_address = x;
		syslog(LOG_INFO, "force_bind: Force bind to address %s.\n",
			force_address);
	}

	x = getenv("FORCE_BIND_PORT");
	if (x != NULL) {
		force_port = strtol(x, NULL, 10);
		syslog(LOG_INFO, "force_bind: Force bind to port %d.\n",
			force_port);
	}

	/* tos */
	x = getenv("FORCE_NET_TOS");
	if (x != NULL) {
		set_tos = 1;
		tos = strtoul(x, NULL, 0);
		syslog(LOG_INFO, "force_bind: Force TOS to %hhu.\n",
			tos);
	}

	/* keep alive */
	x = getenv("FORCE_NET_KA");
	if (x != NULL) {
		set_keepalive = 1;
		keepalive = strtoul(x, NULL, 0);
		syslog(LOG_INFO, "force_bind: Force KA to %u.\n",
			keepalive);
	}

	old_bind = dlsym(RTLD_NEXT, "bind");
	if (old_bind == NULL) {
		syslog(LOG_ERR, "force_bind: Cannot resolve 'bind'!\n");
		exit(1);
	}

	old_setsockopt = dlsym(RTLD_NEXT, "setsockopt");
	if (old_setsockopt == NULL) {
		syslog(LOG_ERR, "force_bind: Cannot resolve 'setsockopt'!\n");
		exit(1);
	}

	old_socket = dlsym(RTLD_NEXT, "socket");
	if (old_socket == NULL) {
		syslog(LOG_ERR, "force_bind: Cannot resolve 'socket'!\n");
		exit(1);
	}
}

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
	int err;
	struct sockaddr new;
	struct sockaddr_in *sa4;
	struct sockaddr_in6 *sa6;
	void *p = NULL;
	unsigned short *pport = NULL;

	init();

	if ((addr->sa_family != AF_INET) && (addr->sa_family != AF_INET6)) {
		syslog(LOG_INFO, "force_bind: unsupported family=%u!\n",
			addr->sa_family);
		return old_bind(sockfd, addr, addrlen);
	}

	memcpy(&new, addr, sizeof(struct sockaddr));

	switch (new.sa_family) {
		case AF_INET:
			sa4 = (struct sockaddr_in *) &new;
			p = &sa4->sin_addr;
			pport = &sa4->sin_port;
			break;

		case AF_INET6:
			sa6 = (struct sockaddr_in6 *) &new;
			p = &sa6->sin6_addr.s6_addr;
			pport = &sa6->sin6_port;
			break;
	}

	if (force_address != NULL) {
		err = inet_pton(new.sa_family, force_address, p);
		if (err != 1) {
			syslog(LOG_INFO, "force_bind: cannot convert [%s] (%d)!\n",
				force_address, err);
			return old_bind(sockfd, addr, addrlen);
		}
	}

	if (force_port != -1)
		*pport = htons(force_port);

	return old_bind(sockfd, &new, addrlen);
}

int setsockopt(int sockfd, int level, int optname, const void *optval,
	socklen_t optlen)
{
	init();

	if (level == SOL_SOCKET) {
		if ((optname == SO_KEEPALIVE) && (set_keepalive == 1)) {
			set_ka(sockfd);
			return 0;
		}
	}

	if (level == IPPROTO_IP) {
		if ((optname == IP_TOS) && (set_tos == 1)) {
			syslog(LOG_INFO, "force_bind: changing TOS from %hhu to %hhu.\n",
				*(char *)optval, tos);
			optval = &tos;
			optlen = sizeof(tos);
		}
	}

	if (level == IPPROTO_TCP) {
		if ((optname == TCP_KEEPIDLE) && (set_keepalive == 1)) {
			set_ka_idle(sockfd);
			return 0;
		}
	}

	return old_setsockopt(sockfd, level, optname, optval, optlen);
}

/*
 * 'socket' is hijacked to be able to call setsockopt on it.
 */
int socket(int domain, int type, int protocol)
{
	int sock;

	init();

	sock = old_socket(domain, type, protocol);
	if (sock == -1)
		return -1;

	if (set_tos == 1)
		setsockopt(sock, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));

	if (set_keepalive == 1) {
		set_ka(sock);

		if (type == SOCK_STREAM)
			set_ka_idle(sock);
	}

	return sock;
}

