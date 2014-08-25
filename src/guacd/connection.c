/*
 * Copyright (C) 2014 Glyptodon LLC
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

#include "connection.h"
#include "log.h"
#include "move-fd.h"
#include "proc.h"
#include "proc-map.h"
#include "user.h"

#include <guacamole/client.h>
#include <guacamole/error.h>
#include <guacamole/instruction.h>
#include <guacamole/plugin.h>
#include <guacamole/protocol.h>
#include <guacamole/socket.h>
#include <guacamole/user.h>

#ifdef ENABLE_SSL
#include <openssl/ssl.h>
#include "socket-ssl.h"
#endif

#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>

/**
 * Adds the given socket as a new user to the given process, automatically
 * reading/writing from the socket via read/write threads. The given socket and
 * any associated resources will be freed unless the user is not added
 * successfully.
 *
 * If adding the user fails for any reason, non-zero is returned. Zero is
 * returned upon success.
 */
static int guacd_add_user(guacd_proc* proc, guac_socket* socket) {

    int user_fd = 0; /* FIXME: Use one end of socketpair */

    /* Send user file descriptor to process */
    if (!guacd_send_fd(proc->fd_socket, user_fd)) {
        guacd_log_error("Unable to add user.");
        return 1;
    }

    /* FIXME: 
     *
     * 0) Start I/O thread.
     *    a) Start thread which reads from process, writes to socket.
     *    b) In loop, read from socket, write to process.
     *    c) After loop terminates, join on thread.
     *    d) Clean up socket.
     * 1) Detach thread.
     */

    return 0;

}

/**
 * Routes the connection on the given socket according to the Guacamole
 * protocol on the given socket, adding new users and creating new client
 * processes as needed.
 */
static int guacd_route_connection(guacd_proc_map* map, guac_socket* socket) {

    /* Reset guac_error */
    guac_error = GUAC_STATUS_SUCCESS;
    guac_error_message = NULL;

    /* Get protocol from select instruction */
    guac_instruction* select = guac_instruction_expect(socket, GUACD_USEC_TIMEOUT, "select");
    if (select == NULL) {
        guacd_log_guac_error("Error reading \"select\"");
        return 1;
    }

    /* Validate args to select */
    if (select->argc != 1) {
        guacd_log_error("Bad number of arguments to \"select\" (%i)", select->argc);
        return 1;
    }

    guacd_proc* proc;

    const char* identifier = select->argv[0];

    /* If connection ID, retrieve existing process */
    if (identifier[0] == GUAC_CLIENT_ID_PREFIX) {

        proc = guacd_proc_map_retrieve(map, identifier);
        if (proc == NULL)
            guacd_log_info("Connection \"%s\" does not exist.", identifier);
        else
            guacd_log_info("Joining existing connection \"%s\"", identifier);

    }

    /* Otherwise, create new client */
    else {

        guacd_log_info("Creating new client for protocol \"%s\"", identifier);
        proc = guacd_create_proc(identifier);

        /* Log connection ID */
        guacd_log_info("Connection ID is \"%s\"", proc->client->connection_id);

    }

    guac_instruction_free(select);

    if (proc == NULL)
        return 1;

    /* Add new user (in the case of a new process, this will be the owner */
    if (guacd_add_user(proc, socket) == 0) {

        /* FIXME: The following should ONLY be done for new processes */
#if 0
        /* Store process, allowing other users to join */
        guacd_proc_map_add(map, proc);

        /* Wait for child to finish */
        waitpid(proc->pid, NULL, 0);

        /* Remove client */
        if (guacd_proc_map_remove(map, proc->client->connection_id) == NULL)
            guacd_log_error("Internal failure removing client \"%s\". Client record will never be freed.",
                    proc->client->connection_id);
        else
            guacd_log_info("Connection \"%s\" removed.", proc->client->connection_id);

        /* Free skeleton client */
        guac_client_free(proc->client);

        /* Clean up */
        close(proc->fd_socket);
        free(proc);
#endif

        return 1;
    }

    /* Add of user failed */
    else
        return 1;

}

void* guacd_connection_thread(void* data) {

    guacd_connection_context* context = (guacd_connection_context*) data;

    guacd_proc_map* map = context->map;
    int connected_socket_fd = context->connected_socket_fd;

    guac_socket* socket;

#ifdef ENABLE_SSL

    SSL_CTX* ssl_context = context->ssl_context;

    /* If SSL chosen, use it */
    if (ssl_context != NULL) {
        socket = guac_socket_open_secure(ssl_context, connected_socket_fd);
        if (socket == NULL) {
            guacd_log_guac_error("Error opening secure connection");
            free(context);
            return NULL;
        }
    }
    else
        socket = guac_socket_open(connected_socket_fd);

#else
    /* Open guac_socket */
    socket = guac_socket_open(connected_socket_fd);
#endif

    /* Route connection according to Guacamole, creating a new process if needed */
    if (guacd_route_connection(map, socket)) {
        guac_socket_free(socket);
        close(connected_socket_fd);
    }

    free(context);
    return NULL;

}

