/* Rhizo-connector: A connector to different HF modems
 * Copyright (C) 2018 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 *
 */

/**
 * @file connector.h
 * @author Rafael Diniz
 * @date 12 Apr 2018
 * @brief Rhizo HF Connector main header file
 *
 * Rhizo HF Connector main header file.
 *
 */

#ifndef HAVE_CONNECTOR_H__
#define HAVE_CONNECTOR_H__

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include <hamlib/rig.h>

#include "buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

// 90s
#define TIMEOUT_DEFAULT 90

// multi-peer support (VARA/Mercury modems)
#define MAX_PEERS 16
// seconds to wait before calling a peer again after a failed attempt or a finished session
#define RETRY_BACKOFF_DEFAULT 120
// give up on an outbound CONNECT the modem never answered
#define CONNECT_ATTEMPT_MAX_S 300
// ignore spool files modified more recently than this (still being written)
#define SPOOL_SETTLE_S 2
// largest frame (filename + payload) accepted from the air; anything bigger
// means the stream is corrupt
#define MAX_RX_MSG_SIZE (16u * 1024u * 1024u)
// longest filename written to the output spool
#define MAX_RX_NAME_LEN 100

typedef struct{
// public
    char call_sign[32];
    char remote_call_sign[32];
    int tcp_base_port;
    char ip_address[32];
    char modem_type[32];
    bool serial_keying;
    int serial_fd;
    rig_model_t radio_type;
    char serial_path[1024];
    char input_directory[1024];
    char output_directory[1024];
    bool ofdm_mode;
    int timeout;
    uint32_t buffer_size;

// state variables
// C11 atomic is used here instead of a more pedantic code with mutexes and so on... 
    atomic_bool tcp_ret_ok;
    atomic_bool connected;
    atomic_bool waiting_for_connection;
    atomic_int timeout_counter;
    atomic_int safe_state; // this means green light for changing state

// private
    buffer in_buffer;
    buffer out_buffer;
    int data_socket;
    int control_socket;
    pthread_mutex_t msg_path_queue_mutex;
    char *msg_path_queue[4096];
    size_t msg_path_queue_size;

// multi-peer (VARA/Mercury only; ARDOP keeps the legacy single-peer path)
    char peers[MAX_PEERS][32];      // from -d CALL1,CALL2,...; peers[0] is the default
    int num_peers;
    time_t peer_retry_after[MAX_PEERS];
    int next_peer;                  // round-robin start index for outbound calls
    int retry_backoff;              // -w seconds
    int attempt_peer;               // peer index of the outstanding CONNECT, -1 if none
    time_t attempt_time;
    char session_peer[32];          // remote callsign of the current session
    atomic_bool session_active;     // CONNECTED seen, teardown not yet done
    atomic_bool session_ready;      // this session's mail has been loaded
    atomic_bool tx_hold;            // data tx thread must not send
    atomic_bool tx_idle;            // data tx thread is parked in its wait loop
    atomic_uint rx_session_gen;     // bumped on every CONNECTED/DISCONNECTED; a
                                    // received frame must start and end in one
} rhizo_conn;

#ifdef __cplusplus
};
#endif

#endif /* HAVE_CONNECTOR__ */
