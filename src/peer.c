/*
 * Peers
 */

#include <c-dvar.h>
#include <c-dvar-type.h>
#include <c-macro.h>
#include <c-rbtree.h>
#include <c-string.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include "bus.h"
#include "dbus-protocol.h"
#include "driver.h"
#include "match.h"
#include "message.h"
#include "peer.h"
#include "socket.h"
#include "user.h"
#include "util/dispatch.h"

static int peer_forward_unicast(Peer *sender, const char *destination, const char *signature, Message *message) {
        Peer *receiver;
        int r;

        receiver = bus_find_peer_by_name(sender->bus, destination);
        if (!receiver)
                return -EBADMSG;

        /* XXX: verify message contents, append sender */

        r = socket_queue_message(receiver->socket, message);
        if (r)
                return (r > 0) ? -ENOTRECOVERABLE : r;

        dispatch_file_select(&receiver->dispatch_file, EPOLLOUT);

        return 0;
}

static int peer_forward_broadcast(Peer *sender, const char *signature, Message *message) {
        return 0;
}

static int peer_dispatch_read_message(Peer *peer) {
        static const CDVarType type[] = {
                C_DVAR_T_INIT(
                        C_DVAR_T_TUPLE7(
                                C_DVAR_T_y,
                                C_DVAR_T_y,
                                C_DVAR_T_y,
                                C_DVAR_T_y,
                                C_DVAR_T_u,
                                C_DVAR_T_u,
                                C_DVAR_T_ARRAY(
                                        C_DVAR_T_TUPLE2(
                                                C_DVAR_T_y,
                                                C_DVAR_T_v
                                        )
                                )
                        )
                ), /* (yyyyuua(yv)) */
        };
        _c_cleanup_(message_unrefp) Message *message = NULL;
        _c_cleanup_(c_dvar_freep) CDVar *v = NULL;
        const char *path = NULL,
                   *interface = NULL,
                   *member = NULL,
                   *error_name = NULL,
                   *destination = NULL,
                   *sender = NULL,
                   *signature = "";
        uint32_t serial = 0, reply_serial = 0, n_fds = 0;
        uint8_t field;
        int r;

        r = socket_read_message(peer->socket, &message);
        if (r < 0)
                return r;

        /*
         * XXX: Rather than allocating @v, we should use its static versions on the stack,
         *      once provided by c-dvar.
         */

        r = c_dvar_new(&v);
        if (r)
                return (r > 0) ? -ENOTRECOVERABLE : r;

        c_dvar_begin_read(v, message->big_endian, type, message->header, message->n_header);

        c_dvar_read(v, "(yyyyuu[", NULL, NULL, NULL, NULL, NULL, &serial);

        while (c_dvar_more(v)) {
                /*
                 * XXX: What should we do on duplicates?
                 */

                c_dvar_read(v, "(y", &field);

                switch (field) {
                case DBUS_MESSAGE_FIELD_INVALID:
                        return -EBADMSG;
                case DBUS_MESSAGE_FIELD_PATH:
                        c_dvar_read(v, "<o>)", c_dvar_type_o, &path);
                        break;
                case DBUS_MESSAGE_FIELD_INTERFACE:
                        c_dvar_read(v, "<s>)", c_dvar_type_s, &interface);
                        break;
                case DBUS_MESSAGE_FIELD_MEMBER:
                        c_dvar_read(v, "<s>)", c_dvar_type_s, &member);
                        break;
                case DBUS_MESSAGE_FIELD_ERROR_NAME:
                        c_dvar_read(v, "<s>)", c_dvar_type_s, &error_name);
                        break;
                case DBUS_MESSAGE_FIELD_REPLY_SERIAL:
                        c_dvar_read(v, "<u>)", c_dvar_type_u, &reply_serial);
                        break;
                case DBUS_MESSAGE_FIELD_DESTINATION:
                        c_dvar_read(v, "<s>)", c_dvar_type_s, &destination);
                        break;
                case DBUS_MESSAGE_FIELD_SENDER:
                        /* XXX: check with dbus-daemon(1) on what to do */
                        c_dvar_read(v, "<s>)", c_dvar_type_s, &sender);
                        break;
                case DBUS_MESSAGE_FIELD_SIGNATURE:
                        c_dvar_read(v, "<g>)", c_dvar_type_g, &signature);
                        break;
                case DBUS_MESSAGE_FIELD_UNIX_FDS:
                        c_dvar_read(v, "<u>)", c_dvar_type_u, &n_fds);
                        break;
                default:
                        c_dvar_skip(v, "v)");
                        break;
                }
        }

        c_dvar_read(v, "])");

        r = c_dvar_end_read(v);
        if (r)
                return (r > 0) ? -EBADMSG : r;

        if (_c_unlikely_(n_fds > message->n_fds))
                return -EBADMSG;
        while (_c_unlikely_(n_fds < message->n_fds))
                close(message->fds[-- message->n_fds]);

        if (destination) {
                if (_c_unlikely_(c_string_equal(destination, "org.freedesktop.DBus")))
                        return driver_dispatch_interface(peer, serial, interface, member, path, signature, message);
                else
                        return peer_forward_unicast(peer, destination, signature, message);
        } else {
                return peer_forward_broadcast(peer, signature, message);
        }

        return 0;
}

static int peer_dispatch_read_line(Peer *peer) {
        char *line_in, *line_out;
        size_t *pos, n_line;
        int r;

        r = socket_read_line(peer->socket, &line_in, &n_line);
        if (r < 0)
                return r;

        r = socket_queue_line(peer->socket, SASL_MAX_OUT_LINE_LENGTH, &line_out, &pos);
        if (r < 0)
                return r;

        r = sasl_dispatch(&peer->sasl, line_in, line_out, pos);
        if (r < 0)
                return r;
        else if (r == 0)
                dispatch_file_select(&peer->dispatch_file, EPOLLOUT);
        else
                peer->authenticated = true;

        return 0;
}

static int peer_dispatch_read(Peer *peer) {
        int r;

        for (unsigned int i = 0; i < 32; i ++) {
                if (_c_likely_(peer->authenticated)) {
                        r = peer_dispatch_read_message(peer);
                } else {
                        r = peer_dispatch_read_line(peer);
                }
                if (r == -EAGAIN) {
                        /* nothing to be done */
                        dispatch_file_clear(&peer->dispatch_file, EPOLLIN);
                        return 0;
                } else if (r < 0) {
                        /* XXX: swallow error code and tear down this peer */
                        return 0;
                }
        }

        return 0;
}

static int peer_dispatch_write(Peer *peer) {
        int r;

        r = socket_write(peer->socket);
        if (r == -EAGAIN) {
                /* not able to write more */
                dispatch_file_clear(&peer->dispatch_file, EPOLLOUT);
                return 0;
        } else if (r == 0) {
                /* nothing more to write */
                dispatch_file_deselect(&peer->dispatch_file, EPOLLOUT);
        } else if (r < 0) {
                /* XXX: swallow error code and tear down this peer */
                return 0;
        }

        return 0;
}

int peer_dispatch(DispatchFile *file, uint32_t mask) {
        Peer *peer = c_container_of(file, Peer, dispatch_file);
        int r;

        if (mask & EPOLLIN) {
                r = peer_dispatch_read(peer);
                if (r < 0)
                        return r;
        }

        if (mask & EPOLLOUT) {
                r = peer_dispatch_write(peer);
                if (r < 0)
                        return r;
        }

        return 0;
}

static int peer_get_peersec(int fd, char **labelp, size_t *lenp) {
        _c_cleanup_(c_freep) char *label = NULL;
        char *l;
        socklen_t len = 1023;
        int r;

        label = malloc(len + 1);
        if (!label)
                return -ENOMEM;

        for (;;) {
                r = getsockopt(fd, SOL_SOCKET, SO_PEERSEC, label, &len);
                if (r >= 0) {
                        label[len] = '\0';
                        *lenp = len;
                        *labelp = label;
                        label = NULL;
                        break;
                } else if (errno == ENOPROTOOPT) {
                        *lenp = 0;
                        *labelp = NULL;
                        break;
                } else if (errno != ERANGE)
                        return -errno;

                l = realloc(label, len + 1);
                if (!l)
                        return -ENOMEM;

                label = l;
        }

        return 0;
}

/**
 * peer_new() - XXX
 */
int peer_new(Peer **peerp,
             Bus *bus,
             int fd) {
        _c_cleanup_(peer_freep) Peer *peer = NULL;
        _c_cleanup_(user_entry_unrefp) UserEntry *user = NULL;
        _c_cleanup_(c_freep) char *seclabel = NULL;
        size_t n_seclabel;
        struct ucred ucred;
        socklen_t socklen = sizeof(ucred);
        int r;

        r = getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &ucred, &socklen);
        if (r < 0)
                return -errno;

        r = user_registry_ref_entry(&bus->users, &user, ucred.uid);
        if (r < 0)
                return r;

        if (user->n_peers < 1)
                return -EDQUOT;

        r = peer_get_peersec(fd, &seclabel, &n_seclabel);
        if (r < 0)
                return r;

        peer = calloc(1, sizeof(*peer));
        if (!peer)
                return -ENOMEM;

        user->n_peers --;

        peer->bus = bus;
        c_rbnode_init(&peer->rb);
        peer->user = user;
        user = NULL;
        peer->pid = ucred.pid;
        peer->seclabel = seclabel;
        seclabel = NULL;
        peer->n_seclabel = n_seclabel;
        peer->dispatch_file = (DispatchFile)DISPATCH_FILE_NULL(peer->dispatch_file);
        sasl_init(&peer->sasl, ucred.uid, bus->guid);

        r = socket_new(&peer->socket, fd);
        if (r < 0)
                return r;

        r = dispatch_file_init(&peer->dispatch_file,
                               &bus->dispatcher,
                               &bus->ready_list,
                               peer_dispatch,
                               fd,
                               EPOLLIN | EPOLLOUT);
        if (r < 0)
                return r;

        peer->id = bus->ids ++;

        *peerp = peer;
        peer = NULL;
        return 0;
}

/**
 * peer_free() - XXX
 */
Peer *peer_free(Peer *peer) {
        CRBNode *node, *next;

        if (!peer)
                return NULL;

        assert(!peer->names.root);
        assert(!c_rbnode_is_linked(&peer->rb));

        peer->user->n_peers ++;

        for (node = c_rbtree_first_postorder(&peer->match_rules),
             next = c_rbnode_next_postorder(node);
             node;
             node = next, next = c_rbnode_next_postorder(node)) {
                MatchRule *rule = c_container_of(node, MatchRule, rb_peer);

                match_rule_free(&rule->n_refs, NULL);
        }

        dispatch_file_deinit(&peer->dispatch_file);
        sasl_deinit(&peer->sasl);
        socket_free(peer->socket);
        user_entry_unref(peer->user);
        free(peer->seclabel);
        free(peer);

        return NULL;
}

void peer_start(Peer *peer) {
        return dispatch_file_select(&peer->dispatch_file, EPOLLIN);
}

void peer_stop(Peer *peer) {
        return dispatch_file_deselect(&peer->dispatch_file, EPOLLIN);
}
