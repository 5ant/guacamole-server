/*
 * Copyright (C) 2013 Glyptodon LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "config.h"

#include "client.h"
#include "client-handlers.h"
#include "error.h"
#include "instruction.h"
#include "layer.h"
#include "pool.h"
#include "protocol.h"
#include "socket.h"
#include "stream.h"
#include "timestamp.h"
#include "user.h"

#ifdef HAVE_OSSP_UUID_H
#include <ossp/uuid.h>
#else
#include <uuid.h>
#endif

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

guac_layer __GUAC_DEFAULT_LAYER = {
    .index = 0
};

const guac_layer* GUAC_DEFAULT_LAYER = &__GUAC_DEFAULT_LAYER;

/**
 * Socket read handler which operates on each of the sockets of all connected
 * users, unifying the results.
 */
static ssize_t __guac_socket_broadcast_read_handler(guac_socket* socket,
        void* buf, size_t count) {
    /* STUB */
    return 0;

}

/**
 * Socket write handler which operates on each of the sockets of all connected
 * users, unifying the results.
 */
static ssize_t __guac_socket_broadcast_write_handler(guac_socket* socket,
        const void* buf, size_t count) {
    /* STUB */
    return 0;
}

/**
 * Socket select handler which operates on each of the sockets of all connected
 * users, unifying the results.
 */
static int __guac_socket_broadcast_select_handler(guac_socket* socket, int usec_timeout) {
    /* STUB */
    return 0;
}

guac_layer* guac_client_alloc_layer(guac_client* client) {

    /* Init new layer */
    guac_layer* allocd_layer = malloc(sizeof(guac_layer));
    allocd_layer->index = guac_pool_next_int(client->__layer_pool)+1;

    return allocd_layer;

}

guac_layer* guac_client_alloc_buffer(guac_client* client) {

    /* Init new layer */
    guac_layer* allocd_layer = malloc(sizeof(guac_layer));
    allocd_layer->index = -guac_pool_next_int(client->__buffer_pool) - 1;

    return allocd_layer;

}

void guac_client_free_buffer(guac_client* client, guac_layer* layer) {

    /* Release index to pool */
    guac_pool_free_int(client->__buffer_pool, -layer->index - 1);

    /* Free layer */
    free(layer);

}

void guac_client_free_layer(guac_client* client, guac_layer* layer) {

    /* Release index to pool */
    guac_pool_free_int(client->__layer_pool, layer->index);

    /* Free layer */
    free(layer);

}

guac_stream* guac_client_alloc_stream(guac_client* client) {

    guac_stream* allocd_stream;
    int stream_index;

    /* Refuse to allocate beyond maximum */
    if (client->__stream_pool->active == GUAC_CLIENT_MAX_STREAMS)
        return NULL;

    /* Allocate stream */
    stream_index = guac_pool_next_int(client->__stream_pool);

    /* Initialize stream */
    allocd_stream = &(client->__output_streams[stream_index]);
    allocd_stream->index = stream_index;
    allocd_stream->data = NULL;
    allocd_stream->ack_handler = NULL;
    allocd_stream->blob_handler = NULL;
    allocd_stream->end_handler = NULL;

    return allocd_stream;

}

void guac_client_free_stream(guac_client* client, guac_stream* stream) {

    /* Release index to pool */
    guac_pool_free_int(client->__stream_pool, stream->index);

    /* Mark stream as closed */
    stream->index = GUAC_CLIENT_CLOSED_STREAM_INDEX;

}

/**
 * Returns a newly allocated string containing a guaranteed-unique connection
 * identifier string which is 37 characters long and begins with a '$'
 * character.  If an error occurs, NULL is returned, and no memory is
 * allocated.
 */
static char* __guac_generate_connection_id() {

    char* buffer;
    char* identifier;
    size_t identifier_length;

    uuid_t* uuid;

    /* Attempt to create UUID object */
    if (uuid_create(&uuid) != UUID_RC_OK) {
        guac_error = GUAC_STATUS_NO_MEMORY;
        guac_error_message = "Could not allocate memory for UUID";
        return NULL;
    }

    /* Generate random UUID */
    if (uuid_make(uuid, UUID_MAKE_V4) != UUID_RC_OK) {
        uuid_destroy(uuid);
        guac_error = GUAC_STATUS_NO_MEMORY;
        guac_error_message = "UUID generation failed";
        return NULL;
    }

    /* Allocate buffer for future formatted ID */
    buffer = malloc(UUID_LEN_STR + 2);
    if (buffer == NULL) {
        uuid_destroy(uuid);
        guac_error = GUAC_STATUS_NO_MEMORY;
        guac_error_message = "Could not allocate memory for connection ID";
        return NULL;
    }

    identifier = &(buffer[1]);
    identifier_length = UUID_LEN_STR + 1;

    /* Build connection ID from UUID */
    if (uuid_export(uuid, UUID_FMT_STR, &identifier, &identifier_length) != UUID_RC_OK) {
        free(buffer);
        uuid_destroy(uuid);
        guac_error = GUAC_STATUS_BAD_STATE;
        guac_error_message = "Conversion of UUID to connection ID failed";
        return NULL;
    }

    uuid_destroy(uuid);

    buffer[0] = '$';
    buffer[UUID_LEN_STR + 1] = '\0';
    return buffer;

}

guac_client* guac_client_alloc() {

    pthread_mutexattr_t lock_attributes;
    int i;

    /* Allocate new client */
    guac_client* client = malloc(sizeof(guac_client));
    if (client == NULL) {
        guac_error = GUAC_STATUS_NO_MEMORY;
        guac_error_message = "Could not allocate memory for client";
        return NULL;
    }

    /* Init new client */
    memset(client, 0, sizeof(guac_client));

    client->last_received_timestamp =
        client->last_sent_timestamp = guac_timestamp_current();

    client->state = GUAC_CLIENT_RUNNING;

    /* Generate ID */
    client->connection_id = __guac_generate_connection_id();
    if (client->connection_id == NULL) {
        free(client);
        return NULL;
    }

    /* Allocate buffer and layer pools */
    client->__buffer_pool = guac_pool_alloc(GUAC_BUFFER_POOL_INITIAL_SIZE);
    client->__layer_pool = guac_pool_alloc(GUAC_BUFFER_POOL_INITIAL_SIZE);

    /* Allocate stream pool */
    client->__stream_pool = guac_pool_alloc(0);

    /* Initialze streams */
    client->__input_streams = malloc(sizeof(guac_stream) * GUAC_CLIENT_MAX_STREAMS);
    client->__output_streams = malloc(sizeof(guac_stream) * GUAC_CLIENT_MAX_STREAMS);

    for (i=0; i<GUAC_CLIENT_MAX_STREAMS; i++) {
        client->__input_streams[i].index = GUAC_CLIENT_CLOSED_STREAM_INDEX;
        client->__output_streams[i].index = GUAC_CLIENT_CLOSED_STREAM_INDEX;
    }

    /* Init locks */
    pthread_mutexattr_init(&lock_attributes);
    pthread_mutexattr_setpshared(&lock_attributes, PTHREAD_PROCESS_SHARED);

    pthread_mutex_init(&(client->__users_lock), &lock_attributes);

    /* Set up socket to broadcast to all users */
    guac_socket* socket = guac_socket_alloc();
    client->socket = socket;
    socket->data   = client;

    socket->read_handler   = __guac_socket_broadcast_read_handler;
    socket->write_handler  = __guac_socket_broadcast_write_handler;
    socket->select_handler = __guac_socket_broadcast_select_handler;

    return client;

}

void guac_client_free(guac_client* client) {

    /* Remove all users */
    while (client->__users != NULL)
        guac_client_remove_user(client, client->__users);

    if (client->free_handler) {

        /* FIXME: Errors currently ignored... */
        client->free_handler(client);

    }

    /* Free layer pools */
    guac_pool_free(client->__buffer_pool);
    guac_pool_free(client->__layer_pool);

    /* Free streams */
    free(client->__input_streams);
    free(client->__output_streams);

    /* Free stream pool */
    guac_pool_free(client->__stream_pool);

    pthread_mutex_destroy(&(client->__users_lock));
    free(client);
}

int guac_client_handle_instruction(guac_client* client, guac_instruction* instruction) {

    /* For each defined instruction */
    __guac_instruction_handler_mapping* current = __guac_instruction_handler_map;
    while (current->opcode != NULL) {

        /* If recognized, call handler */
        if (strcmp(instruction->opcode, current->opcode) == 0)
            return current->handler(client, instruction);

        current++;
    }

    /* If unrecognized, ignore */
    return 0;

}

void vguac_client_log_info(guac_client* client, const char* format,
        va_list ap) {

    /* Call handler if defined */
    if (client->log_info_handler != NULL)
        client->log_info_handler(client, format, ap);

}

void vguac_client_log_error(guac_client* client, const char* format,
        va_list ap) {

    /* Call handler if defined */
    if (client->log_error_handler != NULL)
        client->log_error_handler(client, format, ap);

}

void guac_client_log_info(guac_client* client, const char* format, ...) {

    va_list args;
    va_start(args, format);

    vguac_client_log_info(client, format, args);

    va_end(args);

}

void guac_client_log_error(guac_client* client, const char* format, ...) {

    va_list args;
    va_start(args, format);

    vguac_client_log_error(client, format, args);

    va_end(args);

}

void guac_client_stop(guac_client* client) {
    client->state = GUAC_CLIENT_STOPPING;
}

void vguac_client_abort(guac_client* client, guac_protocol_status status,
        const char* format, va_list ap) {

    /* Only relevant if client is running */
    if (client->state == GUAC_CLIENT_RUNNING) {

        /* Log detail of error */
        vguac_client_log_error(client, format, ap);

        /* Send error immediately, limit information given */
        guac_protocol_send_error(client->socket, "Aborted. See logs.", status);
        guac_socket_flush(client->socket);

        /* Stop client */
        guac_client_stop(client);

    }

}

void guac_client_abort(guac_client* client, guac_protocol_status status,
        const char* format, ...) {

    va_list args;
    va_start(args, format);

    vguac_client_abort(client, status, format, args);

    va_end(args);

}

guac_user* guac_client_add_user(guac_client* client, guac_socket* socket) {

    guac_user* user = malloc(sizeof(guac_user));
    user->socket = socket;
    user->leave_handler = NULL;

    /* Call handler, if defined */
    if (client->join_handler)
        client->join_handler(client, user);

    /* Insert new user as head */
    pthread_mutex_lock(&(client->__users_lock));

    user->__prev = NULL;
    user->__next = client->__users;

    if (client->__users != NULL)
        client->__users->__prev = user;

    client->__users = user;

    pthread_mutex_unlock(&(client->__users_lock));

    return user;

}

void guac_client_remove_user(guac_client* client, guac_user* user) {

    /* Call handler, if defined */
    if (user->leave_handler)
        user->leave_handler(client, user);
    else if (client->leave_handler)
        client->leave_handler(client, user);

    pthread_mutex_lock(&(client->__users_lock));

    /* Update prev / head */
    if (user->__prev != NULL)
        user->__prev->__next = user->__next;
    else
        client->__users = user->__next;

    /* Update next */
    if (user->__next != NULL)
        user->__next->__prev = user->__prev;

    pthread_mutex_unlock(&(client->__users_lock));

    /* Clean up user */
    guac_socket_free(user->socket);
    free(user);

}

