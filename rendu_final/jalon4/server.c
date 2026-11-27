#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "chained_list.h"
#include <ctype.h>

#define MAX_CNN 20

struct channel_list_state {
    char names[MAX_CNN][NICK_LEN];
    int count;
};

int die(int ret_value, const char *msg) {
    if (ret_value < 0) {
        perror(msg);
        exit(EXIT_FAILURE);
    }
    return 0;
}

int send_message(int sockfd, const void *buf, size_t len) {
    size_t bytes_sent = 0;
    const char *ptr = (const char *)buf;
    while (bytes_sent < len) {
        ssize_t ret = write(sockfd, ptr + bytes_sent, len - bytes_sent);
        if (ret < 0) {
            perror("On writing");
            return -1;
        }
        if (ret == 0) {
            return -1;
        }
        bytes_sent += (size_t)ret;
    }
    return 0;
}

int recv_message(int sockfd, void *buf, size_t len) {
    size_t bytes_read = 0;
    char *ptr = (char *)buf;
    while (bytes_read < len) {
        ssize_t ret = read(sockfd, ptr + bytes_read, len - bytes_read);
        if (ret < 0) {
            perror("On reading");
            return -1;
        }
        if (ret == 0) {
            return -1;
        }
        bytes_read += (size_t)ret;
    }
    return 0;
}

int send_protocol_message(int sockfd, const struct message *msg, const char *payload) {
    if (send_message(sockfd, msg, sizeof(struct message)) == -1) {
        return -1;
    }
    if (msg->pld_len > 0 && payload != NULL) {
        if (send_message(sockfd, payload, (size_t)msg->pld_len) == -1) {
            return -1;
        }
    }
    return 0;
}

int recv_protocol_message(int sockfd, struct message *msg, char *payload, size_t max_payload_len) {
    if (recv_message(sockfd, msg, sizeof(struct message)) == -1) {
        return -1;
    }

    if (msg->pld_len < 0 || (size_t)msg->pld_len >= max_payload_len) {
        perror("Invalid pld_len");
        return -1;
    }
    if (msg->pld_len > 0) {
        if (recv_message(sockfd, payload, (size_t)msg->pld_len) == -1) {
            return -1;
        }
        payload[msg->pld_len] = '\0';
    } else {
        payload[0] = '\0';
    }
    return 0;
}

int valid_nickname(const char *infos, char *error, size_t error_size) {
    const char *nul = memchr(infos, '\0', NICK_LEN);
    if (nul == NULL) {
        snprintf(error, error_size, "Error: nickname is not NUL-terminated");
        return 0;
    }

    size_t len = (size_t)(nul - infos);
    if (len == 0) {
        snprintf(error, error_size, "Error: nickname cannot be empty");
        return 0;
    }
    if (len > NICK_LEN - 1) {
        snprintf(error, error_size, "Error: nickname exceeds 127 characters");
        return 0;
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)infos[i];
        if (!isalnum(c)) {
            snprintf(error, error_size, "Error: nickname must contain only ASCII letters and digits");
            return 0;
        }
    }
    return 1;
}

int nickname_is_used(const char *nickname, int client_index, struct list *lst) {
    for (int i = 0; i < MAX_CNN; i++) {
        struct node *client = get_node_by_index(lst, i);

        if (client != NULL && i != client_index && client->nickname[0] != '\0' &&
            strcmp(client->nickname, nickname) == 0) {
            return 1;
        }
    }
    return 0;
}

int valid_channel_name(const char *name, char *error, size_t error_size) {
    const char *nul = memchr(name, '\0', NICK_LEN);
    
    if (nul == NULL) {
        snprintf(error, error_size, "Error: channel name is not NUL-terminated");
        return 0;
    }

    size_t len = (size_t)(nul - name);
    if (len == 0) {
        snprintf(error, error_size, "Error: channel name cannot be empty");
        return 0;
    }
    if (len >= NICK_LEN) {
        snprintf(error, error_size, "Error: channel name is too long");
        return 0;
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!((c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9'))) {
            snprintf(error, error_size, "Error: channel name must contain only letters and digits");
            return 0;
        }
    }
    return 1;
}

int channel_exists(struct channel_list_state *channels, const char *name) {
    for (int i = 0; i < channels->count; i++) {
        if (strcmp(channels->names[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

int channel_member_count(struct list *lst, const char *channel_name) {
    int count = 0;
    struct node *current = lst->head;

    while (current != NULL) {
        if (strcmp(current->channel_name, channel_name) == 0) {
            count++;
        }
        current = current->next;
    }
    return count;
}

void remove_channel(struct channel_list_state *channels, const char *name) {
    int found = -1;
    for (int i = 0; i < channels->count; i++) {
        if (strcmp(channels->names[i], name) == 0) {
            found = i;
            break;
        }
    }

    if (found == -1) {
        return;
    }

    for (int i = found; i < channels->count - 1; i++) {
        memmove(channels->names[i], channels->names[i + 1], sizeof(channels->names[i]));
    }
    channels->names[channels->count - 1][0] = '\0';
    channels->count--;
}

int handle_client_message(int sockfd, int client_index, struct list *lst,
                          struct channel_list_state *channels) {
    struct message msg;
    char pld_recv[PROTO_MAX_PAYLOAD + 1];
    char pld_load[PROTO_MAX_PAYLOAD + 1];

    char nickname_server[NICK_LEN] = "Server";

    memset(&msg, 0, sizeof(msg));
    if (recv_protocol_message(sockfd, &msg, pld_recv, sizeof(pld_recv)) == -1) {
        return -1;
    }

    printf("Client %d | type=%s (%d) | sender=%s | infos=%s | pld_len=%d | payload=%s\n",
           client_index, msg_type_str[msg.type], msg.type, msg.nick_sender,
           msg.infos, msg.pld_len, pld_recv);

    if (msg.type == NICKNAME_NEW) {
        struct message reply;
        char error[INFOS_LEN];
        memset(&reply, 0, sizeof(reply));
        reply.type = NICKNAME_NEW;
        reply.status = OK;
        reply.pld_len = 0;
        struct node *client = get_node_by_index(lst, client_index);
        strncpy(reply.nick_sender, client->nickname, NICK_LEN - 1);
        reply.nick_sender[NICK_LEN - 1] = '\0';

        if (strcasecmp(msg.infos, nickname_server) == 0) {
            snprintf(reply.infos, INFOS_LEN, "Error: nickname is reserved");
            reply.status = ERROR;
            if (send_protocol_message(sockfd, &reply, NULL) == -1) {
                return -1;
            }
            return 0;
        }

        if (!valid_nickname(msg.infos, error, sizeof(error))) {
            strncpy(reply.infos, error, INFOS_LEN - 1);
            reply.infos[INFOS_LEN - 1] = '\0';
            reply.status = ERROR;
            if (send_protocol_message(sockfd, &reply, NULL) == -1) {
                return -1;
            }
            return 0;
        }

        if (nickname_is_used(msg.infos, client_index, lst)) {
            snprintf(reply.infos, INFOS_LEN, "Error: nickname already in use");
            reply.status = ERROR;
            if (send_protocol_message(sockfd, &reply, NULL) == -1) {
                return -1;
            }
            return 0;
        }

        strncpy(client->nickname, msg.infos, NICK_LEN - 1);
        client->nickname[NICK_LEN - 1] = '\0';
        strncpy(reply.infos, client->nickname, NICK_LEN - 1);
        reply.infos[NICK_LEN - 1] = '\0';
        strncpy(reply.nick_sender, nickname_server, NICK_LEN - 1);
        reply.nick_sender[NICK_LEN - 1] = '\0';

        reply.pld_len = snprintf(pld_load, sizeof(pld_load), "Welcome on the chat %s", client->nickname);

        if (send_protocol_message(sockfd, &reply, pld_load) == -1) {
            return -1;
        }
        return 0;
    }

    if (msg.type == NICKNAME_LIST) {
        // Send the list of nicknames to the client
        struct message reply;
        memset(&reply, 0, sizeof(reply));
        reply.type = NICKNAME_LIST;
        reply.status = OK;
        reply.pld_len = 0;

        struct node *client = lst->head;
        size_t index = 0;
        while (client != NULL) {
            reply.pld_len += snprintf(pld_load + index, sizeof(pld_load) - index, "- %s\n", client->nickname);
            index = reply.pld_len;
            client = client->next;
        }

        strncpy(reply.nick_sender, nickname_server, NICK_LEN - 1);
        reply.nick_sender[NICK_LEN - 1] = '\0';

        if (send_protocol_message(sockfd, &reply, reply.pld_len > 0 ? pld_load : NULL) == -1) {
            return -1;
        }
        return 0;
    }

    if (msg.type == NICKNAME_INFOS) {
        struct message reply;
        memset(&reply, 0, sizeof(reply));
        reply.type = NICKNAME_INFOS;

        struct node *client = find_client_by_nickname(msg.infos, lst);
        if (client != NULL) {
            struct tm connection_time;
            char connected_at[32];
            if (localtime_r(&client->connected_at, &connection_time) == NULL) {
                perror("localtime_r");
                return -1;
            }
            if (strftime(connected_at, sizeof(connected_at), "%Y/%m/%d@%H:%M",
                         &connection_time) == 0) {
                fprintf(stderr, "Could not format client connection time\n");
                return -1;
            }

            reply.status = OK;
            reply.pld_len = snprintf(pld_load, sizeof(pld_load), "%s connected since %s with IP address %s and port number %d\n", client->nickname, connected_at, inet_ntoa(client->addr.sin_addr), ntohs(client->addr.sin_port));
            strncpy(reply.infos, client->nickname, INFOS_LEN - 1);
            reply.infos[INFOS_LEN - 1] = '\0';
        } else {
            reply.status = ERROR;
            reply.pld_len = snprintf(pld_load, sizeof(pld_load), "User not found\n");
            strncpy(reply.infos, "User not found", INFOS_LEN - 1);
            reply.infos[INFOS_LEN - 1] = '\0';
        }

        strncpy(reply.nick_sender, nickname_server, NICK_LEN - 1);
        reply.nick_sender[NICK_LEN - 1] = '\0';

        if (send_protocol_message(sockfd, &reply, pld_load) == -1) {
            return -1;
        }
        return 0;
    }

    if (msg.type == BROADCAST_SEND) {
        struct node *sender = get_node_by_index(lst, client_index);
        if (sender == NULL) {
            return -1;
        }
        strncpy(pld_load, pld_recv, (size_t)msg.pld_len);
        pld_load[msg.pld_len] = '\0';

        struct message reply;
        memset(&reply, 0, sizeof(reply));
        reply.type = BROADCAST_SEND;
        reply.pld_len = msg.pld_len;

        strncpy(reply.nick_sender, sender->nickname, NICK_LEN - 1);
        reply.nick_sender[NICK_LEN - 1] = '\0';

        struct node *current = lst->head;
        while (current != NULL) {
            if (strcmp(current->nickname, msg.nick_sender) != 0) {
                if (send_protocol_message(current->fd, &reply, reply.pld_len > 0 ? pld_load : NULL) == -1) {
                    return -1;
                }
            }
            current = current->next;
        }
        return 0;
    }

    if (msg.type == UNICAST_SEND) {
        struct node *sender = get_node_by_index(lst, client_index);
        if (sender == NULL) {
            return -1;
        }
        struct node *dest = find_client_by_nickname(msg.infos, lst);
        strncpy(pld_load, pld_recv, (size_t)msg.pld_len);
        pld_load[msg.pld_len] = '\0';

        struct message reply;
        memset(&reply, 0, sizeof(reply));
        reply.type = UNICAST_SEND;
        reply.pld_len = msg.pld_len;
        reply.status = OK;

        strncpy(reply.nick_sender, sender->nickname, NICK_LEN - 1);
        reply.nick_sender[NICK_LEN - 1] = '\0';

        if (dest != NULL) {
            if (send_protocol_message(dest->fd, &reply, reply.pld_len > 0 ? pld_load : NULL) == -1) {
                return -1;
            }
        } else {
            reply.status = ERROR;
            strncpy(reply.infos, msg.infos, INFOS_LEN - 1);
            reply.infos[INFOS_LEN - 1] = '\0';
            reply.pld_len = snprintf(pld_load, sizeof(pld_load),
                                     "User '%s' not found", msg.infos);
            if (send_protocol_message(sockfd, &reply, pld_load) == -1) {
                return -1;
            }
        }
        return 0;
    }

    if (msg.type == ECHO_SEND) {
        struct node *sender = get_node_by_index(lst, client_index);
        if (sender == NULL) {
            return -1;
        }

        struct message reply;
        memset(&reply, 0, sizeof(reply));
        reply.type = ECHO_SEND;
        reply.status = OK;
        reply.pld_len = msg.pld_len;
        strncpy(reply.nick_sender, nickname_server, NICK_LEN - 1);
        reply.nick_sender[NICK_LEN - 1] = '\0';

        if (send_protocol_message(sockfd, &reply, reply.pld_len > 0 ? pld_recv : NULL) == -1) {
            return -1;
        }
        return 0;
    }

    if (msg.type == MULTICAST_CREATE) {
        struct message reply;
        char error[INFOS_LEN];
        memset(&reply, 0, sizeof(reply));
        reply.type = MULTICAST_CREATE;
        reply.status = OK;
        reply.pld_len = 0;
        strncpy(reply.nick_sender, nickname_server, NICK_LEN - 1);
        reply.nick_sender[NICK_LEN - 1] = '\0';

        if (!valid_channel_name(msg.infos, error, sizeof(error))) {
            reply.status = ERROR;
            reply.pld_len = snprintf(pld_load, sizeof(pld_load), "%s", error);
            strncpy(reply.infos, error, INFOS_LEN - 1);
            reply.infos[INFOS_LEN - 1] = '\0';
            if (send_protocol_message(sockfd, &reply, pld_load) == -1) {
                return -1;
            }
            return 0;
        }

        if (channel_exists(channels, msg.infos)) {
            reply.status = ERROR;
            reply.pld_len = snprintf(pld_load, sizeof(pld_load),
                                     "Error: channel %s already exists", msg.infos);
            strncpy(reply.infos, "Channel already exists", INFOS_LEN - 1);
            reply.infos[INFOS_LEN - 1] = '\0';
            if (send_protocol_message(sockfd, &reply, pld_load) == -1) {
                return -1;
            }
            return 0;
        }

        if (channels->count >= MAX_CNN) {
            reply.status = ERROR;
            reply.pld_len = snprintf(pld_load, sizeof(pld_load),
                                     "Error: maximum number of channels reached");
            if (send_protocol_message(sockfd, &reply, pld_load) == -1) {
                return -1;
            }
            return 0;
        }

        // create channel
        strncpy(channels->names[channels->count], msg.infos, NICK_LEN - 1);
        channels->names[channels->count][NICK_LEN - 1] = '\0';
        channels->count++;

        struct node *creator = get_node_by_index(lst, client_index);
        if (creator == NULL) {
            return -1;
        }

        char old_channel[NICK_LEN];
        snprintf(old_channel, sizeof(old_channel), "%s", creator->channel_name);

        // He leaves his current living room if there is one.
        if (old_channel[0] != '\0') {
            creator->channel_name[0] = '\0';

            if (channel_member_count(lst, old_channel) == 0) {
                remove_channel(channels, old_channel);

                struct message destroyed;
                memset(&destroyed, 0, sizeof(destroyed));
                destroyed.type = MULTICAST_QUIT;
                destroyed.status = OK;
                snprintf(destroyed.nick_sender, sizeof(destroyed.nick_sender), "Server");
                snprintf(destroyed.infos, sizeof(destroyed.infos), "%s", old_channel);
                destroyed.pld_len = snprintf(pld_load, sizeof(pld_load),
                                             "You were the last user in this channel, %s has been destroyed",
                                             old_channel);
                if (send_protocol_message(sockfd, &destroyed, pld_load) == -1) {
                    return -1;
                }
            } else {
                struct message departure;
                memset(&departure, 0, sizeof(departure));
                departure.type = MULTICAST_QUIT;
                departure.status = OK;
                snprintf(departure.nick_sender, sizeof(departure.nick_sender),
                         "%s", creator->nickname);
                snprintf(departure.infos, sizeof(departure.infos), "%s", old_channel);
                departure.pld_len = snprintf(pld_load, sizeof(pld_load),
                                             "INFO> %s has quit %s",
                                             creator->nickname, old_channel);

                struct node *member = lst->head;
                while (member != NULL) {
                    if (strcmp(member->channel_name, old_channel) == 0) {
                        if (send_protocol_message(member->fd, &departure, pld_load) == -1) {
                            return -1;
                        }
                    }
                    member = member->next;
                }
            }
        }

        strncpy(creator->channel_name, msg.infos, NICK_LEN - 1);
        creator->channel_name[NICK_LEN - 1] = '\0';

        strncpy(reply.infos, msg.infos, INFOS_LEN - 1);
        reply.infos[INFOS_LEN - 1] = '\0';
        reply.pld_len = snprintf(pld_load, sizeof(pld_load),
                                 "You have created channel %s", msg.infos);
        if (send_protocol_message(sockfd, &reply, pld_load) == -1) {
            return -1;
        }

        // join the channel
        struct message joined;
        memset(&joined, 0, sizeof(joined));
        joined.type = MULTICAST_JOIN;
        joined.status = OK;
        strncpy(joined.nick_sender, nickname_server, NICK_LEN - 1);
        joined.nick_sender[NICK_LEN - 1] = '\0';
        strncpy(joined.infos, msg.infos, INFOS_LEN - 1);
        joined.infos[INFOS_LEN - 1] = '\0';
        joined.pld_len = snprintf(pld_load, sizeof(pld_load),
                                  "You have joined %s", msg.infos);
        if (send_protocol_message(sockfd, &joined, pld_load) == -1) {
            return -1;
        }
        return 0;
    }

    if (msg.type == MULTICAST_LIST) {
        struct message reply;
        memset(&reply, 0, sizeof(reply));
        reply.type = MULTICAST_LIST;
        reply.status = OK;
        strncpy(reply.nick_sender, nickname_server, NICK_LEN - 1);
        reply.nick_sender[NICK_LEN - 1] = '\0';

        pld_load[0] = '\0';
        size_t index = 0;
        reply.pld_len = 0;
        for (int i = 0; i < channels->count; i++) {
            reply.pld_len += snprintf(pld_load + index,
                                      sizeof(pld_load) - index,
                                      "- %s\n", channels->names[i]);
            index = (size_t)reply.pld_len;
        }

        if (send_protocol_message(sockfd, &reply,
                                  reply.pld_len > 0 ? pld_load : NULL) == -1) {
            return -1;
        }
        return 0;
    }

    if (send_protocol_message(sockfd, &msg, msg.pld_len > 0 ? pld_load : NULL) == -1) {
        return -1;
    }

    return 0;
}

int handle_bind(char *port) {
    struct addrinfo hints, *result, *rp;
    int sfd;
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(NULL, port, &hints, &result) != 0) {
        perror("getaddrinfo()");
        exit(EXIT_FAILURE);
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sfd == -1) {
            continue;
        }

        if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) == -1) {
            perror("setsockopt - in main");
            close(sfd);
            freeaddrinfo(result);
            exit(EXIT_FAILURE);
        }

        if (bind(sfd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }
        close(sfd);
    }

    if (rp == NULL) {
        perror("Could not bind");
        freeaddrinfo(result);
        exit(EXIT_FAILURE);
    }

    freeaddrinfo(result);
    return sfd;
}

int handle_multiple_connection(int listen_df, struct list *lst,
                               struct channel_list_state *channels) {
    struct sockaddr_in client_addr;
    socklen_t size_addr = sizeof(client_addr);
    struct pollfd fds[MAX_CNN];

    fds[0].fd = listen_df;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    for (int i = 1; i < MAX_CNN; i++) {
        fds[i].fd = -1;
        fds[i].events = 0;
        fds[i].revents = 0;
    }

    while (1) {
        poll(fds, MAX_CNN, -1);

        for (int i = 0; i < MAX_CNN; i++) {
            if (fds[i].fd == -1) {
                continue;
            }

            if ((fds[i].revents & POLLIN) && fds[i].fd == listen_df) {
                size_addr = sizeof(client_addr);
                int new_fd = accept(listen_df, (struct sockaddr *)&client_addr, &size_addr);
                if (new_fd < 0) {
                    perror("accept()");
                    fds[i].revents = 0;
                    continue;
                }
                printf("New connection accepted\n");
                time_t now = time(NULL);

                for (int j = 1; j < MAX_CNN; j++) {
                    if (fds[j].fd == -1) {
                        fds[j].fd = new_fd;
                        fds[j].events = POLLIN;
                        fds[j].revents = 0;
                        struct node *node = add_node(lst, new_fd, j, client_addr, now);
                        print_node(node);
                        break;
                    }
                }
                print_list(lst);
                fds[i].revents = 0;
            } else if ((fds[i].revents & POLLIN) && fds[i].fd != listen_df) {
                if (handle_client_message(fds[i].fd, i, lst, channels) == -1) {
                    printf("Connection closed by client %d\n", i);
                    close(fds[i].fd);
                    remove_by_fd(lst, fds[i].fd);
                    print_list(lst);
                    fds[i].fd = -1;
                    fds[i].events = 0;
                }
                fds[i].revents = 0;
            } else if ((fds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) && fds[i].fd != listen_df) {
                printf("Connection closed/error on client %d\n", i);
                remove_by_fd(lst, fds[i].fd);
                print_list(lst);
                close(fds[i].fd);
                fds[i].fd = -1;
                fds[i].events = 0;
                fds[i].revents = 0;
            }
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    char *port = argv[1];
    int sfd = handle_bind(port);
    if (listen(sfd, SOMAXCONN) != 0) {
        perror("listen()");
        close(sfd);
        exit(EXIT_FAILURE);
    }

    struct list lst = {0};
    struct channel_list_state channels = {0};
    handle_multiple_connection(sfd, &lst, &channels);
    close(sfd);
    return EXIT_SUCCESS;
}
