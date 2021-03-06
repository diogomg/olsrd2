
/*
 * The olsr.org Optimized Link-State Routing daemon version 2 (olsrd2)
 * Copyright (c) 2004-2015, the olsr.org team - see HISTORY file
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in
 *   the documentation and/or other materials provided with the
 *   distribution.
 * * Neither the name of olsr.org, olsrd nor the names of its
 *   contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * Visit http://www.olsr.org for more information.
 *
 * If you find this software useful feel free to make a donation
 * to the project. For more information see the website or contact
 * the copyright holders.
 *
 */

/**
 * @file
 */

#include <errno.h>
#include <fcntl.h>

#include "common/common_types.h"
#include "common/netaddr.h"
#include "core/oonf_logging.h"

#include "subsystems/os_interface_data.h"
#include "subsystems/os_fd.h"
#include "subsystems/os_generic/os_fd_generic_getrawsocket.h"

/**
 * Creates a new raw socket and configures it
 * @param bind_to address to bind the socket to
 * @param protocol IP protocol number
 * @param recvbuf size of input buffer for socket
 * @param interf pointer to interface to bind socket on,
 *   NULL if socket should not be bound to an interface
 * @param log_src logging source for error messages
 * @return socket filedescriptor, -1 if an error happened
 */
int
os_fd_generic_getrawsocket(struct os_fd *sock,
    const union netaddr_socket *bind_to,
    int protocol, size_t recvbuf, const struct os_interface_data *interf,
    enum oonf_log_source log_src __attribute__((unused))) {

  static const int zero = 0;
  int family;

  family = bind_to->std.sa_family;
  sock->fd = socket(family, SOCK_RAW, protocol);
  if (sock->fd < 0) {
    OONF_WARN(log_src, "Cannot open socket: %s (%d)", strerror(errno), errno);
    return -1;
  }

  if (family == AF_INET) {
    if (setsockopt (sock->fd, IPPROTO_IP, IP_HDRINCL, &zero, sizeof(zero)) < 0) {
      OONF_WARN(log_src, "Cannot disable IP_HDRINCL for socket: %s (%d)", strerror(errno), errno);
      os_fd_close(sock);
      return -1;
    }
  }

  if (os_fd_configsocket(sock, bind_to, recvbuf, true, interf, log_src)) {
    os_fd_close(sock);
    return -1;
  }
  return 0;
}
