/*
 * proxy_client.c: client side of the communication with the libvirt proxy.
 *
 * Copyright (C) 2006 Red Hat, Inc.
 *
 * See COPYING.LIB for the License of this software
 *
 * Daniel Veillard <veillard@redhat.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include "proxy.h"
#include "internal.h"

#define STANDALONE

static int debug = 1;

/************************************************************************
 *									*
 *			Error handling					*
 *									*
 ************************************************************************/

/**
 * virProxyError:
 * @conn: the connection if available
 * @error: the error noumber
 * @info: extra information string
 *
 * Handle an error at the xend daemon interface
 */
static void
virProxyError(virConnectPtr conn, virErrorNumber error, const char *info)
{
    const char *errmsg;

    if (error == VIR_ERR_OK)
        return;

#if 0
    errmsg = __virErrorMsg(error, info);
    __virRaiseError(conn, NULL, VIR_FROM_XEND, error, VIR_ERR_ERROR,
                    errmsg, info, NULL, 0, 0, errmsg, info);
#endif
}

/************************************************************************
 *									*
 *	Automatic startup of the proxy server if it is not running	*
 *									*
 ************************************************************************/
/**
 * virProxyFindServerPath:
 *
 * Tries to find the path to the gam_server binary.
 * 
 * Returns path on success or NULL in case of error.
 */
static const char *
virProxyFindServerPath(void)
{
    static const char *serverPaths[] = {
#ifdef STANDALONE
        "./libvirt_proxy",
	BUILDDIR "/proxy/libvirt_proxy",
#endif
        BINDIR "/libvirt_proxy",
        NULL
    };
    int i;
    const char *debugProxy = getenv("LIBVIRT_DEBUG_PROXY");

    if (debugProxy)
        return(debugProxy);

    for (i = 0; serverPaths[i]; i++) {
        if (access(serverPaths[i], X_OK | R_OK) == 0) {
            return serverPaths[i];
        }
    }
    return NULL;
}

/**
 * virProxyForkServer:
 *
 * Forks and try to launch the proxy server processing the requests for
 * libvirt when communicating with Xen.
 *
 * Returns 0 in case of success or -1 in case of detected error.
 */
static int
virProxyForkServer(void)
{
    const char *proxyPath = virProxyFindServerPath();
    int ret, pid, status;

    if (!proxyPath) {
        fprintf(stderr, "failed to find libvirt_proxy\n");
	return(-1);
    }

    if (debug)
        fprintf(stderr, "Asking to launch %s\n", proxyPath);

    /* Become a daemon */
    pid = fork();
    if (pid == 0) {
        long open_max;
	long i;

        /* don't hold open fd opened from the client of the library */
	open_max = sysconf (_SC_OPEN_MAX);
	for (i = 0; i < open_max; i++)
	    fcntl (i, F_SETFD, FD_CLOEXEC);

        setsid();
        if (fork() == 0) {
            execl(proxyPath, proxyPath, NULL);
            fprintf(stderr, "failed to exec %s\n", proxyPath);
        }
        /*
         * calling exit() generate troubles for termination handlers
         */
        _exit(0);
    }

    /*
     * do a waitpid on the intermediate process to avoid zombies.
     */
retry_wait:
    ret = waitpid(pid, &status, 0);
    if (ret < 0) {
        if (errno == EINTR)
            goto retry_wait;
    }

    return (0);
}

/************************************************************************
 *									*
 *		Processing of client sockets				*
 *									*
 ************************************************************************/

/**
 * virProxyOpenClientSocket:
 * @path: the fileame for the socket
 *
 * try to connect to the socket open by libvirt_proxy
 *
 * Returns the associated file descriptor or -1 in case of failure
 */
static int
virProxyOpenClientSocket(const char *path) {
    int fd;
    struct sockaddr_un addr;
    int trials = 0;

retry:
    fd = socket(PF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "Failed to create unix socket");
	return(-1);
    }

    /*
     * Abstract socket do not hit the filesystem, way more secure and 
     * garanteed to be atomic
     */
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    strncpy(&addr.sun_path[1], path, (sizeof(addr) - 4) - 2);

    /*
     * now bind the socket to that address and listen on it
     */
    if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Failed to connect to socket %s\n", path);
	close(fd);
	if (trials < 3) {
	    if (virProxyForkServer() < 0)
	        return(-1);
	    trials++;
	    usleep(5000 * trials * trials);
	    goto retry;
	}
	return (-1);
    }

    if (debug > 0)
        fprintf(stderr, "connected to unix socket %s via %d\n", path, fd);

    return (fd);
}

/**
 * virProxyCloseClientSocket:
 * @fd: the file descriptor for the socket
 *
 * Close the socket from that client
 *
 * Returns 0 in case of success and -1 in case of error
 */
static int
virProxyCloseClientSocket(int fd) {
    int ret;

    if (fd < 0)
        return(-1);

    ret = close(fd);
    if (ret != 0)
	fprintf(stderr, "Failed to close socket %d\n", fd);
    else if (debug > 0)
	fprintf(stderr, "Closed socket %d\n", fd);
    return(ret);
}

/**
 * virProxyReadClientSocket:
 * @fd: the socket 
 * @buffer: the target memory area
 * @len: the lenght in bytes
 *
 * Process a read from a client socket
 *
 * Returns the number of byte read or -1 in case of error.
 */
static int
virProxyReadClientSocket(int fd, char *buffer, int len) {
    int ret;

    if ((fd < 0) || (buffer == NULL) || (len < 0))
        return(-1);

retry:
    ret = read(fd, buffer, len);
    if (ret < 0) {
        if (errno == EINTR) {
	    if (debug > 0)
	        fprintf(stderr, "read socket %d interrupted\n", fd);
	    goto retry;
	}
        fprintf(stderr, "Failed to read socket %d\n", fd);
	return(-1);
    }

    if (debug)
	fprintf(stderr, "read %d bytes from socket %d\n",
		ret, fd);
    return(ret);
}

/**
 * virProxyWriteClientSocket:
 * @fd: the socket 
 * @data: the data
 * @len: the lenght of data in bytes
 *
 * Process a read from a client socket
 */
static int
virProxyWriteClientSocket(int fd, const char *data, int len) {
    int ret;

    if ((fd < 0) || (data == NULL) || (len < 0))
        return(-1);

retry:
    ret = write(fd, data, len);
    if (ret < 0) {
        if (errno == EINTR) {
	    if (debug > 0)
	        fprintf(stderr, "write socket %d, %d bytes interrupted\n",
		        fd, len);
	    goto retry;
	}
        fprintf(stderr, "Failed to write to socket %d\n", fd);
	return(-1);
    }
    if (debug)
	fprintf(stderr, "wrote %d bytes to socket %d\n",
		len, fd);

    return(0);
}

/************************************************************************
 *									*
 *			Proxy commands processing			*
 *									*
 ************************************************************************/

/**
 * xenProxyClose:
 * @conn: pointer to the hypervisor connection
 *
 * Shutdown the Xen proxy communication layer
 */
void
xenProxyClose(virConnectPtr conn) {
    if ((conn == NULL) || (conn->proxy < 0))
        return;
    virProxyCloseClientSocket(conn->proxy);
    conn->proxy = -1;
}

static int 
xenProxyCommand(virConnectPtr conn, virProxyPacketPtr request,
                virProxyPacketPtr *answer) {
    static int serial = 0;
    int ret;
    virProxyPacketPtr res = NULL;
    char packet[4096];

    if ((conn == NULL) || (conn->proxy < 0))
        return(-1);

    /*
     * normal communication serial numbers are in 0..4095
     */
    ++serial;
    if (serial >= 4096)
        serial = 0;
    request->version = PROXY_PROTO_VERSION;
    request->serial = serial;
    ret  = virProxyWriteClientSocket(conn->proxy, (const char *) request,
                                     request->len);
    if (ret < 0)
        return(-1);
retry:
    if (answer == NULL) {
        /* read in situ */
	ret  = virProxyReadClientSocket(conn->proxy, (char *) request,
	                                sizeof(virProxyPacket));
	if (ret < 0)
	    return(-1);
	if (ret != sizeof(virProxyPacket)) {
	    fprintf(stderr,
		"Communication error with proxy: got %d bytes of %d\n",
		    ret, sizeof(virProxyPacket));
	    xenProxyClose(conn);
	    return(-1);
	}
	res = request;
	if (res->len != sizeof(virProxyPacket)) {
	    fprintf(stderr,
		"Communication error with proxy: expected %d bytes got %d\n",
		    sizeof(virProxyPacket), res->len);
	    xenProxyClose(conn);
	    return(-1);
	}
    } else {
        /* read in packet and duplicate if needed */
        ret  = virProxyReadClientSocket(conn->proxy, &packet[0],
	                                sizeof(virProxyPacket));
	if (ret < 0)
	    return(-1);
	if (ret != sizeof(virProxyPacket)) {
	    fprintf(stderr,
		"Communication error with proxy: got %d bytes of %d\n",
		    ret, sizeof(virProxyPacket));
	    xenProxyClose(conn);
	    return(-1);
	}
	res = (virProxyPacketPtr) &packet[0];
	if ((res->len < sizeof(virProxyPacket)) ||
	    (res->len > sizeof(packet))) {
	    fprintf(stderr,
		"Communication error with proxy: got %d bytes packet\n",
		    res->len);
	    xenProxyClose(conn);
	    return(-1);
	}
	if (res->len > sizeof(virProxyPacket)) {
	    ret  = virProxyReadClientSocket(conn->proxy, &packet[ret],
	                                    res->len - ret);
	    if (ret != (int) (res->len - sizeof(virProxyPacket))) {
		fprintf(stderr,
		    "Communication error with proxy: got %d bytes of %d\n",
			ret, sizeof(virProxyPacket));
		xenProxyClose(conn);
		return(-1);
	    }
	}
    }
    /*
     * do more checks on the incoming packet.
     */
    if ((res == NULL) || (res->version != PROXY_PROTO_VERSION) ||
        (res->len < sizeof(virProxyPacket))) {
	fprintf(stderr,
	    "Communication error with proxy: malformed packet\n");
	xenProxyClose(conn);
	return(-1);
    }
    if (res->serial != serial) {
        TODO /* Asynchronous communication */
	fprintf(stderr, "gor asynchronous packet number %d\n", res->serial);
        goto retry;
    }
    if (answer != NULL)
	*answer = res;
    return(0);
}

/**
 * xenProxyInit:
 * @conn: pointer to the hypervisor connection
 *
 * Try to initialize the Xen proxy communication layer
 *
 * Returns 0 in case of success, and -1 in case of failure
 */
int
xenProxyInit(virConnectPtr conn) {
    virProxyPacket req;
    int ret;
    int fd;
        

    if (conn == NULL)
        return(-1);

    if (conn->proxy <= 0) {
	fd = virProxyOpenClientSocket(PROXY_SOCKET_PATH);
	if (fd < 0) {
	    return(-1);
	}
	conn->proxy = fd;
    }

    memset(&req, 0, sizeof(req));
    req.command = VIR_PROXY_NONE;
    req.len = sizeof(req);
    ret = xenProxyCommand(conn, &req, NULL);
    if ((ret < 0) || (req.command != VIR_PROXY_NONE)) {
        xenProxyClose(conn);
	return(-1);
    }
    return(0);
}

/************************************************************************
 *									*
 *			Driver entry points				*
 *									*
 ************************************************************************/

/**
 * xenProxyGetVersion:
 * @conn: pointer to the Xen Daemon block
 * @hvVer: return value for the version of the running hypervisor (OUT)
 *
 * Get the version level of the Hypervisor running.
 *
 * Returns -1 in case of error, 0 otherwise. if the version can't be
 *    extracted by lack of capacities returns 0 and @hvVer is 0, otherwise
 *    @hvVer value is major * 1,000,000 + minor * 1,000 + release
 */
static int
xenProxyGetVersion(virConnectPtr conn, unsigned long *hvVer)
{
    virProxyPacket req;
    int ret;

    if (!VIR_IS_CONNECT(conn)) {
        virProxyError(conn, VIR_ERR_INVALID_CONN, __FUNCTION__);
        return (-1);
    }
    if (hvVer == NULL) {
        virProxyError(conn, VIR_ERR_INVALID_ARG, __FUNCTION__);
	return (-1);
    }
    memset(&req, 0, sizeof(req));
    req.command = VIR_PROXY_VERSION;
    req.len = sizeof(req);
    ret = xenProxyCommand(conn, &req, NULL);
    if (ret < 0) {
        xenProxyClose(conn);
	return(-1);
    }
    *hvVer = req.data.larg;
    return(0);
}

/**
 * xenProxyNodeGetInfo:
 * @conn: pointer to the Xen Daemon block
 * @info: pointer to a virNodeInfo structure allocated by the user
 * 
 * Extract hardware information about the node.
 *
 * Returns 0 in case of success and -1 in case of failure.
 */
static int
xenProxyNodeGetInfo(virConnectPtr conn, virNodeInfoPtr info) {
}

/**
 * xenProxyListDomains:
 * @conn: pointer to the hypervisor connection
 * @ids: array to collect the list of IDs of active domains
 * @maxids: size of @ids
 *
 * Collect the list of active domains, and store their ID in @maxids
 * TODO: this is quite expensive at the moment since there isn't one
 *       xend RPC providing both name and id for all domains.
 *
 * Returns the number of domain found or -1 in case of error
 */
static int
xenProxyListDomains(virConnectPtr conn, int *ids, int maxids)
{
}

/**
 * xenProxyNumOfDomains:
 * @conn: pointer to the hypervisor connection
 *
 * Provides the number of active domains.
 *
 * Returns the number of domain found or -1 in case of error
 */
static int
xenProxyNumOfDomains(virConnectPtr conn)
{
}

/**
 * xenProxyLookupByID:
 * @conn: pointer to the hypervisor connection
 * @id: the domain ID number
 *
 * Try to find a domain based on the hypervisor ID number
 *
 * Returns the domain name (to be freed) or NULL in case of failure
 */
static char *
xenProxyLookupByID(virConnectPtr conn, int id) {
}

/**
 * xenProxyLookupByUUID:
 * @conn: pointer to the hypervisor connection
 * @uuid: the raw UUID for the domain
 *
 * Try to lookup a domain on xend based on its UUID.
 *
 * Returns the domain id or -1 in case of error
 */
static int
xenProxyLookupByUUID(virConnectPtr conn, const unsigned char *uuid)
{
}

/**
 * xenProxyDomainLookupByName:
 * @conn: A xend instance
 * @name: The name of the domain
 *
 * This method looks up information about a domain based on its name
 *
 * Returns domain id or -1 in case of error
 */
static int
xenProxyDomainLookupByName(virConnectPtr conn, const char *domname)
{
}


/**
 * xenProxyDomainGetMaxMemory:
 * @domain: pointer to the domain block
 *
 * Ask the Xen Daemon for the maximum memory allowed for a domain
 *
 * Returns the memory size in kilobytes or 0 in case of error.
 */
unsigned long
xenProxyDomainGetMaxMemory(virDomainPtr domain)
{
}

/**
 * xenProxyDomainGetInfo:
 * @domain: a domain object
 * @info: pointer to a virDomainInfo structure allocated by the user
 *
 * This method looks up information about a domain and update the
 * information block provided.
 *
 * Returns 0 in case of success, -1 in case of error
 */
int
xenProxyDomainGetInfo(virDomainPtr domain, virDomainInfoPtr info)
{
}

#ifdef STANDALONE
int main(int argc, char **argv) {
    int ret;
    unsigned long ver;
    virConnect conn;

    memset(&conn, 0, sizeof(conn));
    ret = xenProxyInit(&conn);
    if (ret == 0) {
        ret = xenProxyGetVersion(&conn, &ver);
	if (ret != 0) {
	    fprintf(stderr, "Failed to get version from proxy\n");
	} else {
	    printf("Proxy running with version %lu\n", ver);
	}
	xenProxyClose(&conn);
    }
    exit(0);
}
#endif
