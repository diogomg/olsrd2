
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

#ifndef OS_INTERFACE_H_
#define OS_INTERFACE_H_

#include <stdio.h>
#include <sys/time.h>

#include "common/common_types.h"
#include "common/list.h"
#include "core/oonf_logging.h"
#include "subsystems/oonf_timer.h"
#include "subsystems/os_interface_data.h"

/*! subsystem identifier */
#define OONF_OS_INTERFACE_SUBSYSTEM "os_interface"

/**
 * operation system listener for interface events
 */
struct os_interface_if_listener {
  /**
   * Callback triggered when the interface changesd
   * @param if_index interface index
   * @param up true if interface is up
   */
  void (*if_changed)(unsigned if_index, bool up);

  /*! hook to global list of listeners */
  struct list_entity _node;
};

struct os_interface;
struct os_interface_address;

/* include os-specific headers */
#if defined(__linux__)
#include "subsystems/os_linux/os_interface_linux.h"
#elif defined (BSD)
#include "subsystems/os_bsd/os_interface_bsd.h"
#elif defined (_WIN32)
#include "subsystems/os_win32/os_interface_win32.h"
#else
#error "Unknown operation system"
#endif

/**
 * Handler for changing an interface address
 */
struct os_interface_address {
  /*! used for delivering feedback about netlink commands */
  struct os_interface_address_internal _internal;

  /*! interface address */
  struct netaddr address;

  /*! index of interface */
  unsigned int if_index;

  /*! address scope */
  enum os_addr_scope scope;

  /*! set or reset address */
  bool set;

  /**
   * Callback triggered when interface address has been changed
   * @param addr this interface address object
   * @param error error code, 0 if everything is fine
   */
  void (*cb_finished)(struct os_interface_address *addr, int error);
};

/**
 * Representation of an operation system interface
 */
struct os_interface {
  /*! data of interface */
  struct os_interface_data data;

  /**
   * usage counter to allow multiple instances to add the same
   * interface
   */
  uint32_t usage_counter;

  /**
   * usage counter to keep track of the number of users on
   * this interface who want to send mesh traffic
   */
  uint32_t mesh_counter;

  /**
   * When an interface change handler triggers a 'interface not ready'
   * error the interface should be triggered again. The variable stores
   * the last interval until the next trigger.
   */
  uint64_t retrigger_timeout;

  /**
   * used to store internal state of interfaces before
   * configuring them for manet data forwarding.
   * Only used by os_specific code.
   */
  uint32_t _original_state;

  /*! hook interfaces into global tree */
  struct avl_node _node;

  /*! timer for lazy interface change handling */
  struct oonf_timer_instance _change_timer;
};

/* prototypes for all os_system functions */
static INLINE void os_interface_listener_add(struct os_interface_if_listener *);
static INLINE void os_interface_listener_remove(struct os_interface_if_listener *);
static INLINE int os_interface_state_set(const char *dev, bool up);
static INLINE int os_interface_address_set(struct os_interface_address *addr);
static INLINE void os_interface_address_interrupt(struct os_interface_address *addr);
static INLINE int os_interface_mac_set_by_name(const char *, struct netaddr *mac);

static INLINE int os_interface_update(struct os_interface_data *, const char *);
static INLINE int os_interface_init_mesh(struct os_interface *);
static INLINE void os_interface_cleanup_mesh(struct os_interface *);

static INLINE int os_interface_mac_set(struct os_interface_data *ifdata, struct netaddr *mac);

#endif /* OS_INTERFACE_H_ */
