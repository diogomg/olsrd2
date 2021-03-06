/*
 * os_fd_generic_getrawsocket.h
 *
 *  Created on: Jan 12, 2016
 *      Author: rogge
 */

#ifndef _OS_FD_GENERIC_GETRAWSOCKET_H_
#define _OS_FD_GENERIC_GETRAWSOCKET_H_

#include "common/common_types.h"
#include "subsystems/os_fd.h"

EXPORT int os_fd_generic_getrawsocket(struct os_fd *sock,
    const union netaddr_socket *bind_to,
    int protocol, size_t recvbuf, const struct os_interface_data *interf,
    enum oonf_log_source log_src);

#endif /* _OS_FD_GENERIC_GETRAWSOCKET_H_ */
