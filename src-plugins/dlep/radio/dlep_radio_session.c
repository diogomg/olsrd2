
/*
 * The olsr.org Optimized Link-State Routing daemon version 2 (olsrd2)
 * Copyright (c) 2004-2013, the olsr.org team - see HISTORY file
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

#include "common/avl.h"
#include "common/avl_comp.h"
#include "common/netaddr.h"

#include "subsystems/oonf_class.h"
#include "subsystems/oonf_layer2.h"
#include "subsystems/oonf_packet_socket.h"
#include "subsystems/oonf_stream_socket.h"
#include "subsystems/oonf_timer.h"

#include "dlep/dlep_iana.h"
#include "dlep/dlep_parser.h"
#include "dlep/dlep_static_data.h"
#include "dlep/dlep_writer.h"
#include "dlep/radio/dlep_radio.h"
#include "dlep/radio/dlep_radio_interface.h"
#include "dlep/radio/dlep_radio_session.h"

static int _cb_incoming_tcp(struct oonf_stream_session *);
static void _cb_tcp_lost(struct oonf_stream_session *);
static enum oonf_stream_session_state _cb_tcp_receive_data(struct oonf_stream_session *);

static int _handle_peer_initialization(struct dlep_radio_session *session,
    void *ptr, struct dlep_parser_index *idx);
static int _handle_peer_termination(struct dlep_radio_session *session,
    void *ptr, struct dlep_parser_index *idx);
static int _handle_peer_termination_ack(struct dlep_radio_session *session,
    void *ptr, struct dlep_parser_index *idx);

static void _cb_heartbeat_timeout(void *);
static void _cb_send_heartbeat(void *);

static void _cb_l2_neigh_added(void *);
static void _cb_l2_neigh_changed(void *);
static void _cb_l2_neigh_removed(void *);

static uint64_t _get_l2neigh_default_value(struct oonf_layer2_net *l2net,
    enum oonf_layer2_network_index idx, uint64_t def);
static uint64_t _get_l2neigh_value(struct oonf_layer2_neigh *l2neigh,
    enum oonf_layer2_neighbor_index idx, uint64_t def);

static int _generate_peer_initialization_ack(struct dlep_radio_session *session,
    struct oonf_layer2_net *l2net);
static void _generate_destination_up(struct dlep_radio_session *,
    struct oonf_layer2_neigh *);
static void _generate_destination_update(struct dlep_radio_session *,
    struct oonf_layer2_neigh *);
static void _generate_destination_down(struct dlep_radio_session *,
    struct oonf_layer2_neigh *);

static struct oonf_class _session_class = {
  .name = "DLEP TCP session",
  .size = sizeof(struct dlep_radio_session),
};

static struct oonf_timer_class _heartbeat_timeout_class = {
  .name = "DLEP radio heartbeat timeout",
  .callback = _cb_heartbeat_timeout,
};

static struct oonf_timer_class _heartbeat_timer_class = {
  .name = "DLEP radio heartbeat",
  .callback = _cb_send_heartbeat,
  .periodic = true,
};

static struct oonf_class_extension _layer2_neigh_listener = {
  .ext_name = "dlep radio",
  .class_name = LAYER2_CLASS_NEIGHBOR,

  .cb_add = _cb_l2_neigh_added,
  .cb_change = _cb_l2_neigh_changed,
  .cb_remove = _cb_l2_neigh_removed,
};

/**
 * Initialize framework for dlep radio sessions
 */
void
dlep_radio_session_init(void) {
  oonf_class_add(&_session_class);
  oonf_class_extension_add(&_layer2_neigh_listener);
  oonf_timer_add(&_heartbeat_timeout_class);
  oonf_timer_add(&_heartbeat_timer_class);
}

/**
 * Cleanup dlep radio session framework
 */
void
dlep_radio_session_cleanup(void) {
  oonf_timer_remove(&_heartbeat_timer_class);
  oonf_timer_remove(&_heartbeat_timeout_class);
  oonf_class_extension_remove(&_layer2_neigh_listener);
  oonf_class_remove(&_session_class);
}

/**
 * Initialize the callbacks for a dlep tcp socket
 * @param config tcp socket config
 */
void
dlep_radio_session_initialize_tcp_callbacks(
    struct oonf_stream_config *config) {
  config->memcookie = &_session_class;
  config->init = _cb_incoming_tcp;
  config->cleanup = _cb_tcp_lost;
  config->receive_data = _cb_tcp_receive_data;
}

/**
 * Send peer termination to router
 * @param session dlep radio session
 */
void
dlep_radio_terminate_session(struct dlep_radio_session *session) {
  if (session->state != DLEP_RADIO_SESSION_ACTIVE) {
    return;
  }

  dlep_writer_start_signal(DLEP_PEER_TERMINATION, &session->supported_tlvs);
  dlep_writer_add_status(DLEP_STATUS_OKAY);
  if (!dlep_writer_finish_signal(LOG_DLEP_RADIO)) {
    dlep_writer_send_tcp_unicast(&session->stream, &session->supported_signals);

    session->state = DLEP_RADIO_SESSION_TERMINATE;
  }
}

/**
 * Callback to send regular heartbeats over tcp session
 * @param ptr pointer to dlep radio session
 */
static void
_cb_send_heartbeat(void *ptr) {
  struct dlep_radio_session *stream = ptr;

  OONF_DEBUG(LOG_DLEP_RADIO, "Send Heartbeat (%"PRIu64")",
      stream->interface->local_heartbeat_interval);

  dlep_writer_start_signal(DLEP_HEARTBEAT, &stream->supported_tlvs);
  if (dlep_writer_finish_signal(LOG_DLEP_RADIO)) {
    return;
  }

  dlep_writer_send_tcp_unicast(&stream->stream, &stream->supported_signals);
}

/**
 * Callback triggered when remote heartbeat times out
 * @param ptr pointer to dlep radio session
 */
static void
_cb_heartbeat_timeout(void *ptr) {
  struct dlep_radio_session *stream = ptr;

  OONF_INFO(LOG_DLEP_RADIO, "Heartbeat timeout");
  oonf_stream_close(&stream->stream);
}

/**
 * Callback triggered when a new tcp session is accepted by the local socket
 * @param tcp_session pointer to tcp session object
 * @return always 0
 */
static int
_cb_incoming_tcp(struct oonf_stream_session *tcp_session) {
  struct dlep_radio_session *session;
  struct dlep_radio_if *interface;

  session = container_of(tcp_session, struct dlep_radio_session, stream);
  interface = container_of(tcp_session->comport->managed, struct dlep_radio_if, tcp);

  /* initialize back pointer */
  session->interface = interface;

  /* wait for end of Peer Initialization */
  session->state = DLEP_RADIO_SESSION_INIT;

  /* initialize timer */
  session->heartbeat_timer.cb_context = session;
  session->heartbeat_timer.class = &_heartbeat_timer_class;

  session->heartbeat_timeout.cb_context = session;
  session->heartbeat_timeout.class = &_heartbeat_timeout_class;

  /* initialize mandatory signals */
  memcpy(&session->supported_signals, &dlep_mandatory_signals,
      sizeof(struct dlep_bitmap));

  /* initialize mandatory tlvs */
  memcpy(&session->supported_tlvs, &dlep_mandatory_tlvs,
      sizeof(struct dlep_bitmap));

  /* copy timer remote interval */
  session->remote_heartbeat_interval = interface->remote_heartbeat_interval;
  OONF_DEBUG(LOG_DLEP_RADIO, "Heartbeat interval is: %"PRIu64"\n", interface->remote_heartbeat_interval);

  /* start heartbeat timeout */
  oonf_timer_set(&session->heartbeat_timeout, session->remote_heartbeat_interval * 2);

  /* start heartbeat */
  oonf_timer_set(&session->heartbeat_timer, session->interface->local_heartbeat_interval);

  /* attach to session tree of interface */
  session->_node.key = &session->stream.remote_socket;
  avl_insert(&interface->session_tree, &session->_node);

  return 0;
}

/**
 * Callback when a tcp session is lost and must be closed
 * @param tcp_session pointer to tcp session object
 */
static void
_cb_tcp_lost(struct oonf_stream_session *tcp_session) {
  struct dlep_radio_session *session;
  struct dlep_radio_if *interface;
  struct netaddr_str nbuf;

  session = container_of(tcp_session, struct dlep_radio_session, stream);
  interface = container_of(tcp_session->comport->managed, struct dlep_radio_if, tcp);

  OONF_DEBUG(LOG_DLEP_RADIO, "Lost tcp session to %s",
      netaddr_socket_to_string(&nbuf, &tcp_session->remote_socket));

  /* reset timers */
  oonf_timer_stop(&session->heartbeat_timer);
  oonf_timer_stop(&session->heartbeat_timeout);

  /* remove from session tree of interface */
  avl_remove(&interface->session_tree, &session->_node);
}

/**
 * Callback to receive data over oonf_stream_socket
 * @param tcp_session pointer to tcp session
 * @return tcp session state
 */
static enum oonf_stream_session_state
_cb_tcp_receive_data(struct oonf_stream_session *tcp_session) {
  struct dlep_radio_session *session;
  struct dlep_parser_index idx;
  uint16_t siglen;
  int signal, result;
  struct netaddr_str nbuf;

  session = container_of(tcp_session, struct dlep_radio_session, stream);

  if ((signal = dlep_parser_read(&idx, abuf_getptr(&tcp_session->in),
      abuf_getlen(&tcp_session->in), &siglen)) < 0) {
    if (signal != DLEP_PARSER_INCOMPLETE_HEADER
        && signal != DLEP_PARSER_INCOMPLETE_SIGNAL) {
      OONF_WARN_HEX(LOG_DLEP_RADIO,
          abuf_getptr(&tcp_session->in), abuf_getlen(&tcp_session->in)
          "Could not parse incoming TCP signal from %s: %d",
          netaddr_to_string(&nbuf, &tcp_session->remote_address), signal);

      return STREAM_SESSION_CLEANUP;
    }
    return STREAM_SESSION_ACTIVE;
  }

  if (session->state == DLEP_RADIO_SESSION_INIT
      && signal != DLEP_PEER_INITIALIZATION) {
    OONF_WARN(LOG_DLEP_RADIO,
        "Received TCP signal %d before Peer Initialization", signal);
    return STREAM_SESSION_CLEANUP;
  }

  if (session->state == DLEP_RADIO_SESSION_TERMINATE
      && signal != DLEP_PEER_TERMINATION_ACK) {
    OONF_DEBUG(LOG_DLEP_RADIO,
        "Ignore signal %d when waiting for Termination Ack", signal);

    /* remove signal from input buffer */
    abuf_pull(&tcp_session->in, siglen);

    return STREAM_SESSION_ACTIVE;
  }

  OONF_INFO(LOG_DLEP_RADIO, "Received TCP signal %d", signal);

  result = 0;
  switch (signal) {
    case DLEP_PEER_INITIALIZATION:
      result = _handle_peer_initialization(
          session, abuf_getptr(&tcp_session->in), &idx);
      break;
    case DLEP_HEARTBEAT:
      OONF_DEBUG(LOG_DLEP_RADIO, "Received TCP heartbeat, reset interval to %"PRIu64,
          session->remote_heartbeat_interval * 2);
      oonf_timer_set(&session->heartbeat_timeout, session->remote_heartbeat_interval * 2);
      break;
    case DLEP_PEER_TERMINATION:
      result = _handle_peer_termination(
          session, abuf_getptr(&tcp_session->in), &idx);
      break;
    case DLEP_PEER_TERMINATION_ACK:
      result = _handle_peer_termination_ack(
          session, abuf_getptr(&tcp_session->in), &idx);
      break;
    case DLEP_DESTINATION_UP_ACK:
      break;
    case DLEP_DESTINATION_DOWN_ACK:
      break;
    default:
      OONF_WARN(LOG_DLEP_RADIO,
          "Received illegal signal in TCP from %s: %u",
          netaddr_to_string(&nbuf, &tcp_session->remote_address), signal);
      return STREAM_SESSION_CLEANUP;
  }

  /* remove signal from input buffer */
  abuf_pull(&tcp_session->in, siglen);

  return result != 0 ? STREAM_SESSION_CLEANUP :STREAM_SESSION_ACTIVE;
}

/**
 * Handle peer initialization from router
 * @param session dlep radio session
 * @param ptr pointer to begin of signal
 * @param idx dlep parser index
 * @return -1 if an error happened, 0 otherwise
 */
static int
_handle_peer_initialization(struct dlep_radio_session *session,
    void *ptr, struct dlep_parser_index *idx) {
  struct oonf_layer2_neigh *l2neigh;
  struct oonf_layer2_net *l2net;
  uint8_t *buffer;
  char peer[256];
  int pos;

  buffer = ptr;

  /* get peer type */
  peer[0] = 0;
  pos = idx->idx[DLEP_PEER_TYPE_TLV];
  if (pos) {
    dlep_parser_get_peer_type(peer, &buffer[pos]);

    OONF_INFO(LOG_DLEP_RADIO, "Router peer type: %s", peer);
  }

  /* get reference to l2 network data */
  l2net = oonf_layer2_net_add(session->interface->name);
  if (!l2net) {
    return -1;
  }

  /* get heartbeat interval */
  pos = idx->idx[DLEP_HEARTBEAT_INTERVAL_TLV];
  dlep_parser_get_heartbeat_interval(
      &session->remote_heartbeat_interval, &buffer[pos]);

  /* reset heartbeat timeout */
  oonf_timer_set(&session->heartbeat_timeout, session->remote_heartbeat_interval*2);

  /* add supported signals */
  pos = idx->idx[DLEP_OPTIONAL_SIGNALS_TLV];
  dlep_parser_get_optional_signal(&session->supported_signals, &buffer[pos]);

  /* add supported tlvs */
  pos = idx->idx[DLEP_OPTIONAL_DATA_ITEMS_TLV];
  dlep_parser_get_optional_tlv(&session->supported_tlvs, &buffer[pos]);

  if (_generate_peer_initialization_ack(session, l2net)) {
    return -1;
  }

  /* activate session */
  session->state = DLEP_RADIO_SESSION_ACTIVE;

  /* generate Destination Up for all active neighbors */
  avl_for_each_element(&l2net->neighbors, l2neigh, _node) {
    _generate_destination_up(session, l2neigh);
  }
  return 0;
}

/**
 * Handle peer termination from router
 * @param session dlep radio session
 * @param ptr pointer to begin of signal
 * @param idx dlep parser index
 * @return -1 if an error happened, 0 otherwise
 */
static int
_handle_peer_termination(struct dlep_radio_session *session,
    void *ptr, struct dlep_parser_index *idx) {
  uint8_t *buffer = ptr;
  enum dlep_status status;
  int pos;

  pos = idx->idx[DLEP_STATUS_TLV];
  if (pos) {
    dlep_parser_get_status(&status, &buffer[pos]);
    OONF_DEBUG(LOG_DLEP_RADIO, "Peer termination status: %u",
        status);
  }

  /* send Peer Termination Ack */
  dlep_writer_start_signal(DLEP_PEER_TERMINATION_ACK, &session->supported_tlvs);
  dlep_writer_add_status(DLEP_STATUS_OKAY);

  if (dlep_writer_finish_signal(LOG_DLEP_RADIO)) {
    return -1;
  }

  dlep_writer_send_tcp_unicast(&session->stream, &session->supported_signals);
  return 0;
}

/**
 * Handle peer termination ack from router
 * @param session dlep radio session
 * @param ptr pointer to begin of signal
 * @param idx dlep parser index
 * @return -1 if an error happened, 0 otherwise
 */
static int
_handle_peer_termination_ack(struct dlep_radio_session *session,
    void *ptr, struct dlep_parser_index *idx) {
  uint8_t *buffer = ptr;
  enum dlep_status status;
  int pos;
  struct netaddr_str nbuf;

  pos = idx->idx[DLEP_STATUS_TLV];
  if (pos) {
    dlep_parser_get_status(&status, &buffer[pos]);
    OONF_DEBUG(LOG_DLEP_RADIO, "Peer termination ack status: %u",
        status);
  }

  if (session->state != DLEP_RADIO_SESSION_TERMINATE) {
    OONF_WARN(LOG_DLEP_RADIO, "Got Peer Termination ACK without"
        " sending a Peer Terminate from %s",
        netaddr_socket_to_string(&nbuf, &session->stream.remote_socket));
  }

  /* terminate session */
  return -1;
}

static void
_cb_l2_neigh_added(void *ptr) {
  struct oonf_layer2_neigh *l2neigh;
  struct oonf_layer2_net *l2net;
  struct dlep_radio_if *dlep_if;
  struct dlep_radio_session *dlep_session;

  struct netaddr_str nbuf1;

  /* get l2neighbor and l2network */
  l2neigh = ptr;
  l2net = l2neigh->network;

  OONF_DEBUG(LOG_DLEP_RADIO, "Received neighbor addition for %s on interface %s",
      netaddr_to_string(&nbuf1, &l2neigh->addr), l2net->name);

  avl_for_each_element(&dlep_radio_if_tree, dlep_if, _node) {
    if (strcmp(dlep_if->name, l2net->name) != 0) {
      continue;
    }

    avl_for_each_element(&dlep_if->session_tree, dlep_session, _node) {
      if (dlep_session->state != DLEP_RADIO_SESSION_ACTIVE) {
        continue;
      }

      _generate_destination_up(dlep_session, l2neigh);
    }
  }
}

static void
_cb_l2_neigh_changed(void *ptr) {
  struct oonf_layer2_neigh *l2neigh;
  struct oonf_layer2_net *l2net;
  struct dlep_radio_if *dlep_if;
  struct dlep_radio_session *dlep_session;

  struct netaddr_str nbuf1;

  /* get l2neighbor and l2network */
  l2neigh = ptr;
  l2net = l2neigh->network;

  OONF_DEBUG(LOG_DLEP_RADIO, "Received neighbor addition for %s on interface %s",
      netaddr_to_string(&nbuf1, &l2neigh->addr), l2net->name);

  avl_for_each_element(&dlep_radio_if_tree, dlep_if, _node) {
    if (strcmp(dlep_if->name, l2net->name) != 0) {
      continue;
    }

    avl_for_each_element(&dlep_if->session_tree, dlep_session, _node) {
      if (dlep_session->state != DLEP_RADIO_SESSION_ACTIVE) {
        continue;
      }

      _generate_destination_update(dlep_session, l2neigh);
    }
  }
}

static void
_cb_l2_neigh_removed(void *ptr) {
  struct oonf_layer2_neigh *l2neigh;
  struct oonf_layer2_net *l2net;
  struct dlep_radio_if *dlep_if;
  struct dlep_radio_session *dlep_session;

  struct netaddr_str nbuf1;

  /* get l2neighbor and l2network */
  l2neigh = ptr;
  l2net = l2neigh->network;

  OONF_DEBUG(LOG_DLEP_RADIO, "Received neighbor addition for %s on interface %s",
      netaddr_to_string(&nbuf1, &l2neigh->addr), l2net->name);

  avl_for_each_element(&dlep_radio_if_tree, dlep_if, _node) {
    if (strcmp(dlep_if->name, l2net->name) != 0) {
      continue;
    }

    avl_for_each_element(&dlep_if->session_tree, dlep_session, _node) {
      if (dlep_session->state != DLEP_RADIO_SESSION_ACTIVE) {
        continue;
      }

      _generate_destination_down(dlep_session, l2neigh);
    }
  }
}

static uint64_t
_get_l2neigh_default_value(struct oonf_layer2_net *l2net,
    enum oonf_layer2_network_index idx, uint64_t def) {
  const struct oonf_layer2_data *data;

  if (!l2net) {
    return def;
  }

  data = &l2net->neighdata[idx];
  if (oonf_layer2_has_value(data)) {
    return oonf_layer2_get_value(data);
  }
  else {
    return def;
  }
}

static uint64_t
_get_l2neigh_value(struct oonf_layer2_neigh *l2neigh,
    enum oonf_layer2_neighbor_index idx, uint64_t def) {
  const struct oonf_layer2_data *data;

  if (!l2neigh) {
    return def;
  }

  data = oonf_layer2_neigh_get_value(l2neigh, idx);
  if (data) {
    return oonf_layer2_get_value(data);
  }
  else {
    return def;
  }
}

static int
_generate_peer_initialization_ack(struct dlep_radio_session *session,
    struct oonf_layer2_net *l2net) {
  /* create PEER initialization ACK */
  dlep_writer_start_signal(DLEP_PEER_INITIALIZATION_ACK, &session->supported_tlvs);

  /* add mandatory TLVs */
  dlep_writer_add_heartbeat_tlv(session->interface->local_heartbeat_interval);
  dlep_writer_add_mdrr(_get_l2neigh_default_value(l2net, OONF_LAYER2_NEIGH_RX_MAX_BITRATE, 0));
  dlep_writer_add_mdrt(_get_l2neigh_default_value(l2net, OONF_LAYER2_NEIGH_RX_MAX_BITRATE, 0));
  dlep_writer_add_cdrr(0);
  dlep_writer_add_cdrt(0);
  dlep_writer_add_optional_signals();
  dlep_writer_add_optional_data_items();

  /* assemble signal */
  if (dlep_writer_finish_signal(LOG_DLEP_RADIO)) {
    return -1;
  }

  /* send signal */
  dlep_writer_send_tcp_unicast(&session->stream, &session->supported_signals);

  return 0;
}

static void
_generate_destination_up(struct dlep_radio_session *session,
    struct oonf_layer2_neigh *l2neigh) {
  dlep_writer_start_signal(DLEP_DESTINATION_UP, &session->supported_tlvs);
  dlep_writer_add_mac_tlv(&l2neigh->addr);

  dlep_writer_add_mdrr(
      _get_l2neigh_value(l2neigh, OONF_LAYER2_NEIGH_RX_MAX_BITRATE, 0));
  dlep_writer_add_mdrt(
      _get_l2neigh_value(l2neigh, OONF_LAYER2_NEIGH_TX_MAX_BITRATE, 0));
  dlep_writer_add_cdrr(
      _get_l2neigh_value(l2neigh, OONF_LAYER2_NEIGH_RX_BITRATE, 0));
  dlep_writer_add_cdrt(
      _get_l2neigh_value(l2neigh, OONF_LAYER2_NEIGH_TX_BITRATE, 0));

  if (dlep_writer_finish_signal(LOG_DLEP_RADIO)) {
    dlep_radio_terminate_session(session);
    return;
  }

  dlep_writer_send_tcp_unicast(&session->stream, &session->supported_signals);
}

static void
_generate_destination_update(struct dlep_radio_session *session,
    struct oonf_layer2_neigh *l2neigh) {
  dlep_writer_start_signal(DLEP_DESTINATION_UPDATE, &session->supported_tlvs);
  dlep_writer_add_mac_tlv(&l2neigh->addr);

  dlep_writer_add_mdrr(
      _get_l2neigh_value(l2neigh, OONF_LAYER2_NEIGH_RX_MAX_BITRATE, 0));
  dlep_writer_add_mdrt(
      _get_l2neigh_value(l2neigh, OONF_LAYER2_NEIGH_TX_MAX_BITRATE, 0));
  dlep_writer_add_cdrr(
      _get_l2neigh_value(l2neigh, OONF_LAYER2_NEIGH_RX_BITRATE, 0));
  dlep_writer_add_cdrt(
      _get_l2neigh_value(l2neigh, OONF_LAYER2_NEIGH_TX_BITRATE, 0));

  if (dlep_writer_finish_signal(LOG_DLEP_RADIO)) {
    dlep_radio_terminate_session(session);
    return;
  }

  dlep_writer_send_tcp_unicast(&session->stream, &session->supported_signals);
}

static void
_generate_destination_down(struct dlep_radio_session *session,
    struct oonf_layer2_neigh *l2neigh) {
  dlep_writer_start_signal(DLEP_DESTINATION_DOWN, &session->supported_tlvs);
  dlep_writer_add_mac_tlv(&l2neigh->addr);

  if (dlep_writer_finish_signal(LOG_DLEP_RADIO)) {
    dlep_radio_terminate_session(session);
    return;
  }

  dlep_writer_send_tcp_unicast(&session->stream, &session->supported_signals);
}
