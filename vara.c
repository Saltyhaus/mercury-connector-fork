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
 * Vara support routines
 */

/**
 * @file vara.c
 * @author Rafael Diniz
 * @date 12 Apr 2018
 * @brief VARA modem support functions
 *
 * All the specific code for supporting VARA.
 *
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sched.h>

#include "common.h"
#include "net.h"
#include <strings.h>
#include <time.h>

#include "spool.h"
#include "vara.h"
#include "radio_io.h"

void *vara_data_worker_thread_tx(void *conn)
{
    rhizo_conn *connector = (rhizo_conn *) conn;
    uint8_t *buffer;
    uint32_t buf_size;

    while(connector->tcp_ret_ok){

        // check if we are connected, otherwise, wait
        // tx_hold: session setup/teardown in progress, the buffer may be
        // reset under us, so stay parked (and say so via tx_idle).
        // Announce "busy" BEFORE re-checking tx_hold (seq_cst atomics), so the
        // control thread can never reset the buffer while we are reading it.
        connector->safe_state++;
        while (true){
            if (connector->connected && !connector->tx_hold &&
                ring_buffer_count_bytes(&connector->in_buffer.buf) > 0){
                connector->tx_idle = false;
                if (connector->connected && !connector->tx_hold)
                    break;
            }
            connector->tx_idle = true;
            if (connector->tcp_ret_ok == false){
                connector->safe_state--;
                goto exit_local;
            }
            sleep(1);
        }
        connector->safe_state--;

        connector->timeout_counter = 0;

        // read header
        read_buffer(&connector->in_buffer, (uint8_t *) &buf_size, sizeof(buf_size)); // \todo if the two parties in a connection have different endianess, we are in trouble

        buffer = (uint8_t *) malloc (buf_size);
        memset(buffer, 0, buf_size);
        read_buffer(&connector->in_buffer, buffer, buf_size);

        connector->tcp_ret_ok &= tcp_write(connector->data_socket, (uint8_t *) &buf_size, sizeof(buf_size) );
        connector->tcp_ret_ok &= tcp_write(connector->data_socket, buffer, buf_size);

        free(buffer);
    }

exit_local:
    return EXIT_SUCCESS;
}

void *vara_data_worker_thread_rx(void *conn)
{
    rhizo_conn *connector = (rhizo_conn *) conn;
    int block_size = 64;
    uint8_t buffer[64];
    uint32_t buf_size;


    while(connector->tcp_ret_ok)
    {
        connector->safe_state++;
        while (connector->connected == false) {
            if (connector->tcp_ret_ok == false){
                connector->safe_state--;
                goto exit_local;
            }
            sleep(1);
        }
        buf_size = 0;

        connector->tcp_ret_ok &= tcp_read(connector->data_socket, (uint8_t *) &buf_size, sizeof(buf_size));
        connector->safe_state--;

        write_buffer(&connector->out_buffer, (uint8_t *) &buf_size, sizeof(buf_size));

        connector->timeout_counter = 0;

        while (buf_size != 0)
        {
            if (buf_size >= block_size)
            {
                connector->tcp_ret_ok &= tcp_read(connector->data_socket, buffer, block_size);
                write_buffer(&connector->out_buffer, buffer, block_size);
                buf_size -= block_size;
            }
            else
            {
                connector->tcp_ret_ok &= tcp_read(connector->data_socket, buffer, buf_size);
                write_buffer(&connector->out_buffer, buffer, buf_size);
                buf_size = 0;
            }
            connector->timeout_counter = 0;
        }
    }

exit_local:
    return EXIT_SUCCESS;
}

void *vara_control_worker_thread_rx(void *conn)
{
    rhizo_conn *connector = (rhizo_conn *) conn;
    uint8_t rcv_byte;
    uint8_t buffer[1024];
    int counter = 0;
    bool new_cmd = false;

    while(connector->tcp_ret_ok){
        connector->tcp_ret_ok &= tcp_read(connector->control_socket, &rcv_byte, 1);

        if (rcv_byte == '\r')
        {
            buffer[counter] = 0;
            counter = 0;
            new_cmd = true;
        }
        else
        {
            buffer[counter] = rcv_byte;
            counter++;
            new_cmd = false;
        }

        if (new_cmd)
        {
            if (!strcmp((char *) buffer, "DISCONNECTED"))
            {
                fprintf(stderr, "TNC: %s\n", buffer);
                connector->tx_hold = true;        // park tx before anything else
                connector->connected = false;
                connector->waiting_for_connection = false;
            }
            else
            {
            // other commands here
                if (!memcmp(buffer, "CONNECTED", strlen("CONNECTED")))
                {
                    // "CONNECTED <source> <destination> <bw>": the remote
                    // station is whichever callsign is not ours (source on
                    // inbound calls, destination on our own outbound calls)
                    char src[32] = {0}, dst[32] = {0};
                    const char *peer = NULL;

                    sscanf((char *) buffer, "CONNECTED %31s %31s", src, dst);
                    if (src[0] && strcasecmp(src, connector->call_sign))
                        peer = src;
                    else if (dst[0] && strcasecmp(dst, connector->call_sign))
                        peer = dst;

                    fprintf(stderr, "TNC: %s\n", buffer);

                    connector->tx_hold = true;   // nothing goes out until this peer's mail is loaded
                    connector->session_ready = false;
                    snprintf(connector->session_peer, sizeof(connector->session_peer), "%s",
                             peer ? peer : "");
                    connector->session_active = true;
                    connector->connected = true;
                    connector->waiting_for_connection = false;
                }
                else
                {
                    if (!memcmp(buffer, "BUFFER", strlen("BUFFER")))
                    {
                        uint32_t buf_size;
                        sscanf( (char *) buffer, "BUFFER %u", &buf_size);
                        fprintf(stderr, "BUFFER: %u\n", buf_size);

                        if (buf_size != 0)
                            connector->timeout_counter = 0;

                        // our delete messages mechanism
                        // only delete what this session actually handed to
                        // the modem: buffer drained, tx thread finished
                        // writing, and the session's mail was loaded
                        if (buf_size == 0 &&
                            ring_buffer_count_bytes(&connector->in_buffer.buf) == 0 &&
                            connector->connected == true &&
                            connector->session_ready &&
                            connector->tx_idle)
                        {

                            fprintf(stderr, "Messages successfully sent. Erasing messages...\n");
                            remove_all_msg_path_queue(connector);

                        }
                    }
                    else
                    {
                        if (connector->serial_keying == true)
                        {
                            if (!memcmp(buffer, "PTT ON", strlen("PTT ON")))
                            {
                                key_on();
                            }
                            if (!memcmp(buffer, "PTT OFF", strlen("PTT OFF")))
                            {
                                key_off();
                            }
                        }
                        fprintf(stderr, "%s\n", buffer);
                    }
                }
            }
        }
    }

    return EXIT_SUCCESS;
}

void *vara_control_worker_thread_tx(void *conn)
{
    rhizo_conn *connector = (rhizo_conn *) conn;
    char buffer[1024];

    // We set a call sign
    memset(buffer,0,sizeof(buffer));
    sprintf(buffer, "MYCALL %s\r", connector->call_sign);
    connector->tcp_ret_ok &= tcp_write(connector->control_socket, (uint8_t *) buffer, strlen(buffer));

    memset(buffer,0,sizeof(buffer));
    strcpy(buffer,"LISTEN ON\r");
    connector->tcp_ret_ok &= tcp_write(connector->control_socket, (uint8_t *) buffer, strlen(buffer));

    memset(buffer,0,sizeof(buffer));
    strcpy(buffer,"PUBLIC OFF\r");
    connector->tcp_ret_ok &= tcp_write(connector->control_socket, (uint8_t *) buffer, strlen(buffer));

    memset(buffer,0,sizeof(buffer));
    strcpy(buffer,"BW2300\r");
    connector->tcp_ret_ok &= tcp_write(connector->control_socket, (uint8_t *) buffer, strlen(buffer));

//    if (vara_p2p_mode)
//    {
        memset(buffer,0,sizeof(buffer));
        strcpy(buffer,"P2P SESSION\r");
        connector->tcp_ret_ok &= tcp_write(connector->control_socket, (uint8_t *) buffer, strlen(buffer));
//    }


    // 1Hz function
    while(connector->tcp_ret_ok)
    {

        time_t now = time(NULL);

        // Session teardown: the link is gone. Once the data tx thread is
        // parked, drop whatever was not handed to the modem (files are kept
        // and retried next session with the right station).
        if (connector->session_active && !connector->connected && connector->tx_idle)
        {
            size_t pending = connector->msg_path_queue_size;
            int idx = find_peer(connector, connector->session_peer);

            spool_reset_tx(connector);
            if (idx >= 0)
                connector->peer_retry_after[idx] = now + connector->retry_backoff;

            fprintf(stderr, "Session with %s ended (%zu unacknowledged message(s) kept for retry).\n",
                    connector->session_peer[0] ? connector->session_peer : "unknown", pending);

            connector->session_peer[0] = 0;
            connector->session_ready = false;
            connector->attempt_peer = -1;
            connector->session_active = false;
        }

        // Session setup: load ONLY this station's mail, then release tx.
        if (connector->session_active && connector->connected &&
            !connector->session_ready && connector->tx_idle)
        {
            int idx = find_peer(connector, connector->session_peer);

            spool_reset_tx(connector);   // never carry bytes over from another session
            if (idx >= 0)
            {
                int n = spool_load_peer(connector, idx);
                fprintf(stderr, "Session with %s: %d message(s) queued.\n", connector->peers[idx], n);
            }
            else
            {
                fprintf(stderr, "Session with %s (not a configured peer): receive only.\n",
                        connector->session_peer[0] ? connector->session_peer : "unknown station");
            }
            connector->attempt_peer = -1;
            connector->session_ready = true;
            connector->tx_hold = false;
        }
        // Mid-session: pick up mail for this station that arrived after setup.
        else if (connector->session_active && connector->connected && connector->session_ready)
        {
            int idx = find_peer(connector, connector->session_peer);
            if (idx >= 0)
            {
                int n = spool_load_peer(connector, idx);
                if (n > 0)
                    fprintf(stderr, "Session with %s: %d more message(s) queued.\n", connector->peers[idx], n);
            }
        }

        // Outbound attempt that never became a session (modem answered
        // DISCONNECTED, or never answered at all): back off that peer.
        if (connector->attempt_peer >= 0 && !connector->session_active)
        {
            if (!connector->waiting_for_connection ||
                now - connector->attempt_time > CONNECT_ATTEMPT_MAX_S)
            {
                fprintf(stderr, "Call to %s failed, retrying in %d s.\n",
                        connector->peers[connector->attempt_peer], connector->retry_backoff);
                connector->peer_retry_after[connector->attempt_peer] = now + connector->retry_backoff;
                connector->attempt_peer = -1;
                connector->waiting_for_connection = false;
            }
        }

        // Logic to start a connection: idle, and some peer (not backing off)
        // has mail waiting. Peers take turns so none is starved.
        if (connector->connected == false &&
            !connector->session_active &&
            !connector->waiting_for_connection)
        {
            for (int k = 0; k < connector->num_peers; k++)
            {
                int idx = (connector->next_peer + k) % connector->num_peers;

                if (now < connector->peer_retry_after[idx] || !spool_peer_has_mail(connector, idx))
                    continue;

                // \todo try to add some entropy in order to avoid on air clashes
                memset(buffer,0,sizeof(buffer));
                snprintf(buffer, sizeof(buffer), "CONNECT %s %s\r", connector->call_sign,
                         connector->peers[idx]);
                connector->tcp_ret_ok &= tcp_write(connector->control_socket, (uint8_t *)buffer, strlen(buffer));

                fprintf(stderr, "CONNECTING... %s\n", buffer);

                connector->attempt_peer = idx;
                connector->attempt_time = now;
                connector->next_peer = (idx + 1) % connector->num_peers;
                connector->waiting_for_connection = true;
                break;
            }
        }

        // Logic to disconnect on timeout
        if (connector->timeout_counter >= connector->timeout &&
            connector->connected == true)
        {

            connector->tx_hold = true;
            connector->connected = false;

            while (connector->safe_state != 2)
            { // this means we have both data threads in the safe zone
                sched_yield();
            }
            fprintf(stderr, "DISCONNECTING BY TIMEOUT...\n");

            memset(buffer,0,sizeof(buffer));
            sprintf(buffer,"DISCONNECT\r");
            connector->tcp_ret_ok &= tcp_write(connector->control_socket, (uint8_t *)buffer, strlen(buffer));

        }

        sleep(1);

    }

    return EXIT_SUCCESS;
}

bool initialize_modem_vara(rhizo_conn *connector)
{
    connector->tcp_ret_ok &= tcp_connect(connector->ip_address, connector->tcp_base_port, &connector->control_socket);
    connector->tcp_ret_ok &= tcp_connect(connector->ip_address, connector->tcp_base_port+1, &connector->data_socket);

    if (connector->tcp_ret_ok == false)
    {
        fprintf(stderr, "Connection to TNC failure.\n");
        return false;
    }

    // we start our control thread
    pthread_t tid1;
    pthread_create(&tid1, NULL, vara_control_worker_thread_rx, (void *) connector);

    // we start our control tx thread
    pthread_t tid2;
    pthread_create(&tid2, NULL, vara_control_worker_thread_tx, (void *) connector);

    pthread_t tid3;
    pthread_create(&tid3, NULL, vara_data_worker_thread_tx, (void *) connector);

    pthread_t tid4;
    pthread_create(&tid4, NULL, vara_data_worker_thread_rx, (void *) connector);

    pthread_t tid5;
    pthread_create(&tid5, NULL, connection_timeout_thread, (void *) connector);

    pthread_join(tid1, NULL);
    pthread_join(tid2, NULL);
    pthread_join(tid3, NULL);
    pthread_join(tid4, NULL);
    pthread_join(tid5, NULL);

    return true;
}
