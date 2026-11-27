#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include "msg_struct.h"
#include <ctype.h>
#include <errno.h>

#define BUFFER_SIZE 1024
#define NO_FILE_TRANSFER -1 // Macro indicating no file transfer is pending.
#define PORT_FILE_TRANSFER "8081"

struct pending_file_request {
    int type; // A msg_type, or NO_FILE_TRANSFER when no transfer is pending.
    char peer[NICK_LEN];
    char filename[INFOS_LEN];
    int file_fd;
    char download_path[PATH_MAX];
};

int die(int ret_value, char *msg) {
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
            if (errno == EINTR) {
                continue;
            }
            perror("On writing");
            return -1;
        }
        if (ret == 0) {
            fprintf(stderr, "Write returned zero bytes\n");
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

int verify_nickname(const char *nick) {
    int nick_len = strlen(nick);
    if (nick_len >= NICK_LEN) {
        fprintf(stderr, "Nickname too long\n");
        return -1;
    }

    for (int i = 0; i < nick_len; i++) {
        if (!isalnum(nick[i])) {
            fprintf(stderr, "Nickname contains invalid characters\n");
            return -1;
        }
    }
    return 0;
}

static int valid_filename(const char *name) {
    size_t len = strlen(name);
    return len > 0 && len < INFOS_LEN &&
           strcmp(name, ".") != 0 && strcmp(name, "..") != 0 &&
           strchr(name, '/') == NULL && strchr(name, '\\') == NULL;
}

int handle_bind_for_file_transfert(char *port) {
    struct addrinfo hints, *result, *rp;
    int ftfd;
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(NULL, port, &hints, &result) != 0) {
        perror("getaddrinfo()");
        exit(EXIT_FAILURE);
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        ftfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (ftfd == -1) {
            continue;
        }

        if (setsockopt(ftfd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) == -1) {
            perror("setsockopt - in main");
            close(ftfd);
            freeaddrinfo(result);
            exit(EXIT_FAILURE);
        }

        if (bind(ftfd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }
        close(ftfd);
    }

    if (rp == NULL) {
        perror("Could not bind");
        freeaddrinfo(result);
        exit(EXIT_FAILURE);
    }

    freeaddrinfo(result);
    return ftfd;
}

int echo_client(int sockfd, const char *nick, struct pending_file_request *pending_file_request, struct pollfd *fds) {
    char buff[BUFFER_SIZE];
    char endpoint[INET_ADDRSTRLEN + 6];
    
    memset(buff, 0, BUFFER_SIZE);
    if (read(STDIN_FILENO, buff, BUFFER_SIZE - 1) <= 0) {
        return -1;
    }

    struct message msg;
    memset(&msg, 0, sizeof(msg));
    strncpy(msg.nick_sender, nick, NICK_LEN - 1); // Copy the current nickname into the message sender field
    char *payload = "";
    int command_found = 0;

    if (pending_file_request->type == FILE_REQUEST &&
        pending_file_request->file_fd == -1) {
        // Handle pending file request confirmation (Y/N)
        if (strncmp(buff, "Y\n", 2) == 0 || strncmp(buff, "y\n", 2) == 0) {
            msg.type = FILE_ACCEPT;
            command_found = 1;
            strncpy(msg.infos, pending_file_request->peer, INFOS_LEN - 1);
            msg.infos[INFOS_LEN - 1] = '\0';
            if (fds[2].fd != -1 || fds[3].fd != -1) {
                fprintf(stderr, "A file transfer is already awaiting a connection.\n");
                return 0;
            }
            int listener_fd = handle_bind_for_file_transfert(PORT_FILE_TRANSFER); // Bind to any available port for file transfer
            if (listener_fd == -1) {
                return -1;
            }
            struct sockaddr_in local_addr;
            struct sockaddr_in listen_addr;
            socklen_t local_len = sizeof(local_addr);
            socklen_t listen_len = sizeof(listen_addr);
            char address[INET_ADDRSTRLEN];
            if (listen(listener_fd, 1) == -1 ||
                getsockname(listener_fd, (struct sockaddr *)&listen_addr, &listen_len) == -1 ||
                getsockname(sockfd, (struct sockaddr *)&local_addr, &local_len) == -1) { // Get local address for the main socket to determine the correct IP for file transfer
                perror("Could not prepare file transfer listener");
                close(listener_fd);
                return -1;
            }
            if (local_addr.sin_family != AF_INET ||
                inet_ntop(AF_INET, &local_addr.sin_addr, address, sizeof(address)) == NULL) { // Convert local IPv4 address to string for file transfer endpoint
                fprintf(stderr, "Could not determine local IPv4 address\n");
                close(listener_fd);
                return -1;
            }
            msg.pld_len = snprintf(endpoint, sizeof(endpoint), "%s:%u",
                                   address, (unsigned int)ntohs(listen_addr.sin_port));
            if (msg.pld_len < 0 || (size_t)msg.pld_len >= sizeof(endpoint)) {
                fprintf(stderr, "Could not format file transfer address\n");
                close(listener_fd);
                return -1;
            }
            payload = endpoint;
            fds[2].fd = listener_fd;
            fds[2].revents = 0;
            pending_file_request->type = FILE_ACCEPT;
        } else if (strncmp(buff, "N\n", 2) == 0 || strncmp(buff, "n\n", 2) == 0) {
            msg.type = FILE_REJECT;
            command_found = 1;
            strncpy(msg.infos, pending_file_request->peer, INFOS_LEN - 1);
            msg.infos[INFOS_LEN - 1] = '\0';
            pending_file_request->type = NO_FILE_TRANSFER;
        } else {
            fprintf(stderr, "Please answer Y or N.\n");
            return 0;
        }
    }

    if (strcmp(buff, "/quit\n") == 0) {
        // QUIT logic here
        return -1;
    }

    if (strncmp(buff, "/nick ", 6) == 0) {
        char *new_nick = buff + 6;
        new_nick[strcspn(new_nick, "\n")] = '\0';
        if (verify_nickname(new_nick) == -1) {
            // We don't want the program to stop
            return 0;
        }
        msg.type = NICKNAME_NEW;
        command_found = 1;
        strncpy(msg.infos, new_nick, INFOS_LEN - 1); // Copy the new nickname into the message infos field
    } else {
        if (nick[0] == '\0' && msg.nick_sender[0] == '\0') {
            fprintf(stderr, "Nickname not set.\nUse /nick <nickname> to set it.\n");
            return 0;
        }

        if (strncmp(buff, "/who", 4) == 0) {
            // NICKNAME_LIST logic here
            msg.type = NICKNAME_LIST;
            command_found = 1;
        }

        if (strncmp(buff, "/whois ", 7) == 0) {
            char *whois_nick = buff + 7;
            whois_nick[strcspn(whois_nick, "\n")] = '\0';
            msg.type = NICKNAME_INFOS;
            command_found = 1;
            strncpy(msg.infos, whois_nick, INFOS_LEN - 1);
        }

        if (strncmp(buff, "/msgall ", 8) == 0) {
            char *msgall_payload = buff + 8;
            msgall_payload[strcspn(msgall_payload, "\n")] = '\0';
            msg.type = BROADCAST_SEND;
            command_found = 1;
            msg.pld_len = strlen(msgall_payload);
            payload = msgall_payload;
        }

        if (strncmp(buff, "/msg ", 5) == 0) {
            char *dest = buff + 5;
            dest[strcspn(dest, "\n")] = '\0'; // Replace \n with \0
            char *msg_payload = strchr(dest, ' '); // Find the first space separating the nickname and the message
            if (msg_payload == NULL) {
                fprintf(stderr, "Usage: /msg <nickname> <message>\n");
                return 0;
            }
            *msg_payload = '\0';
            msg_payload++;
            msg.type = UNICAST_SEND;
            command_found = 1;
            strncpy(msg.infos, dest, INFOS_LEN - 1);
            msg.pld_len = strlen(msg_payload);
            payload = msg_payload;
        }

        if (strncmp(buff, "/send ", 6) == 0) {
            char *request = buff + 6;
            request[strcspn(request, "\n")] = '\0';

            size_t separator = strcspn(request, " ");
            char *filename = request + separator;
            if (*filename == '\0') {
                fprintf(stderr, "Usage: /send <nickname> <filename>\n");
                return 0;
            }
            *filename++ = '\0';
 
            if (verify_nickname(request) == -1) {
                return 0;
            }
            if (*filename == '\0') {
                fprintf(stderr, "Usage: /send <nickname> <filename>\n");
                return 0;
            }

            if (pending_file_request->type != NO_FILE_TRANSFER ||
                fds[2].fd != -1 || fds[3].fd != -1) {
                fprintf(stderr, "A file transfer is already pending.\n");
                return 0;
            }
            size_t path_len = strlen(filename);
            if (path_len >= 2 && filename[0] == '"' && filename[path_len - 1] == '"') {
                filename[path_len - 1] = '\0';
                filename++;
            }
            const char *base = strrchr(filename, '/');
            base = base == NULL ? filename : base + 1;
            size_t name_len = strlen(base);
            if (!valid_filename(base)) {
                fprintf(stderr, "Invalid file name.\n");
                return 0;
            }
            int file_fd = open(filename, O_RDONLY);
            if (file_fd == -1) {
                perror("Could not open file");
                return 0;
            }
            pending_file_request->file_fd = file_fd;
            pending_file_request->type = FILE_REQUEST;
            strcpy(pending_file_request->filename, base);
            strncpy(pending_file_request->peer, request, NICK_LEN - 1);
            pending_file_request->peer[NICK_LEN - 1] = '\0';
            msg.type = FILE_REQUEST;
            command_found = 1;
            strncpy(msg.infos, request, INFOS_LEN - 1);
            msg.pld_len = (int)name_len;
            payload = pending_file_request->filename;
        }
    }

    if (!command_found) {
        if (buff[0] == '/') {
            fprintf(stderr, "Unknown command. Use /nick, /who, /whois, /msgall, /msg or /send.\n");
            return 0;
        }
        msg.type = ECHO_SEND;
        command_found = 1;
        msg.pld_len = strlen(buff);
        payload = buff;
    }

    if (send_protocol_message(sockfd, &msg, payload) == -1) {  
        if (msg.type == FILE_REQUEST && pending_file_request->file_fd != -1) {
            close(pending_file_request->file_fd);
            pending_file_request->file_fd = -1;
            pending_file_request->type = NO_FILE_TRANSFER;
        }
        if (msg.type == FILE_ACCEPT && fds[2].fd != -1) {
            close(fds[2].fd);
            fds[2].fd = -1;
            pending_file_request->type = NO_FILE_TRANSFER;
        }
        return -1;
    }
    printf("Send: %s %s\n", msg_type_str[msg.type], msg.infos);
    return 0;
}

static int connect_to_peer(char *endpoint, size_t length) {
    if (length == 0 || memchr(endpoint, '\0', length) != NULL) {
        fprintf(stderr, "Invalid file transfer address.\n");
        return -1;
    }

    char *port = strrchr(endpoint, ':');
    if (port == NULL || port == endpoint || port[1] == '\0') {
        fprintf(stderr, "Invalid file transfer address.\n");
        return -1;
    }
    *port++ = '\0';

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    if (inet_pton(AF_INET, endpoint, &address.sin_addr) != 1) {
        fprintf(stderr, "Invalid file transfer IP address.\n");
        return -1;
    }

    char *end;
    errno = 0;
    long port_number = strtol(port, &end, 10);
    if (*port < '0' || *port > '9' || *end != '\0' ||
        errno != 0 || port_number < 1 || port_number > 65535) {
        fprintf(stderr, "Invalid file transfer port.\n");
        return -1;
    }
    address.sin_port = htons((unsigned short)port_number);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        perror("File transfer socket");
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == -1) {
        perror("Could not connect to file receiver");
        close(fd);
        return -1;
    }
    return fd;
}

static int send_file(int fd, const char *nick, struct pending_file_request *state) {
    char chunk[PROTO_MAX_PAYLOAD];
    struct message msg = {0};
    msg.type = FILE_SEND;
    strncpy(msg.nick_sender, nick, NICK_LEN - 1);
    strncpy(msg.infos, state->filename, INFOS_LEN - 1);

    ssize_t count;
    while ((count = read(state->file_fd, chunk, sizeof(chunk))) != 0) {
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("Could not read file");
            return -1;
        }
        msg.pld_len = (int)count;
        if (send_protocol_message(fd, &msg, chunk) == -1) {
            return -1;
        }
    }

    msg.pld_len = 0;
    return send_protocol_message(fd, &msg, NULL);
}

static void discard_received_file(struct pending_file_request *state) {
    if (state->type == FILE_SEND && state->file_fd != -1) {
        close(state->file_fd);
        state->file_fd = -1;
        if (unlink(state->download_path) == -1) {
            perror("Could not remove incomplete file");
        }
    }
}

static void cleanup_transfer(struct pending_file_request *state, struct pollfd *fds) {
    discard_received_file(state);
    if (state->file_fd != -1) {
        close(state->file_fd);
        state->file_fd = -1;
    }
    if (fds[2].fd != -1) {
        close(fds[2].fd);
    }
    if (fds[3].fd != -1) {
        close(fds[3].fd);
    }
}

static int prepare_download(struct pending_file_request *state) {
    char directory[PATH_MAX];
    FILE *command = popen("xdg-user-dir DOWNLOAD", "r");
    if (command == NULL) {
        perror("Could not find download directory");
        return -1;
    }

    char *result = fgets(directory, sizeof(directory), command);
    int status = pclose(command);
    if (result == NULL || status != 0 || strchr(directory, '\n') == NULL) {
        fprintf(stderr, "Could not read XDG download directory.\n");
        return -1;
    }
    directory[strcspn(directory, "\n")] = '\0';
    if (directory[0] != '/') {
        fprintf(stderr, "Invalid XDG download directory.\n");
        return -1;
    }
    if (mkdir(directory, 0700) == -1 && errno != EEXIST) {
        perror("Could not create download directory");
        return -1;
    }
    int length = snprintf(state->download_path, sizeof(state->download_path),
                          "%s/%s", directory, state->filename);
    if (length < 0 || (size_t)length >= sizeof(state->download_path)) {
        fprintf(stderr, "Download path is too long.\n");
        return -1;
    }
    state->file_fd = open(state->download_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (state->file_fd == -1) {
        perror("Could not create received file");
        return -1;
    }
    return 0;
}

static int receive_file_message(int fd, const char *nick,
                                struct pending_file_request *state) {
    struct message msg;
    char chunk[PROTO_MAX_PAYLOAD + 1];
    if (recv_protocol_message(fd, &msg, chunk, sizeof(chunk)) == -1) {
        fprintf(stderr, "Incomplete file transfer.\n");
        return -1;
    }

    if (msg.type == FILE_ACK && state->type == FILE_ACK) {
        if (msg.status != OK || memchr(msg.infos, '\0', INFOS_LEN) == NULL ||
            strcmp(msg.infos, state->filename) != 0) {
            fprintf(stderr, "Invalid file acknowledgement.\n");
            return -1;
        }
        printf("%s received the file %s.\n", state->peer, state->filename);
        state->type = NO_FILE_TRANSFER;
        return 1;
    }

    if (msg.type != FILE_SEND || state->type != FILE_SEND ||
        memchr(msg.infos, '\0', INFOS_LEN) == NULL ||
        memchr(msg.nick_sender, '\0', NICK_LEN) == NULL ||
        strcmp(msg.infos, state->filename) != 0 ||
        strcmp(msg.nick_sender, state->peer) != 0) {
        fprintf(stderr, "Unexpected file transfer message.\n");
        return -1;
    }

    if (state->file_fd == -1 && prepare_download(state) == -1) {
        return -1;
    }

    if (msg.pld_len > 0) {
        if (send_message(state->file_fd, chunk, (size_t)msg.pld_len) == -1) {
            return -1;
        }
        return 0;
    }

    if (close(state->file_fd) == -1) {
        state->file_fd = -1;
        perror("Could not finish received file");
        if (unlink(state->download_path) == -1) {
            perror("Could not remove incomplete file");
        }
        return -1;
    }
    state->file_fd = -1;
    struct message ack = {0};
    ack.type = FILE_ACK;
    strncpy(ack.nick_sender, nick, NICK_LEN - 1);
    strncpy(ack.infos, state->filename, INFOS_LEN - 1);
    if (send_protocol_message(fd, &ack, NULL) == -1) {
        fprintf(stderr, "Could not acknowledge file transfer.\n");
        return -1;
    }
    printf("%s saved in %s\n", state->filename, state->download_path);
    state->type = NO_FILE_TRANSFER;
    return 1;
}

int reply_server(int sockfd, char *nick, struct pending_file_request *pending_file_request, struct pollfd *fds) {
    struct message msg;
    char payload[PROTO_MAX_PAYLOAD + 1];
    if (recv_protocol_message(sockfd, &msg, payload, sizeof(payload)) == -1) {
        printf("Server closed connection\n");
        return -1;
    }

    if (msg.type == NICKNAME_NEW) {
        if (msg.status == ERROR) {
            fprintf(stderr, "%s\n", msg.infos);
        }
        else {
            strncpy(nick, msg.infos, NICK_LEN - 1);
            nick[NICK_LEN - 1] = '\0';
            printf("[%s] %s\n", msg.nick_sender, payload);
        }
    }

    if (msg.type == NICKNAME_LIST) {
        if (msg.status == OK) {
            printf("[%s] Online users are:\n%s", msg.nick_sender, payload);
        }
    }

    if (msg.type == NICKNAME_INFOS) {
        printf("[%s] %s", msg.nick_sender, payload);
    }

    if (msg.type == ECHO_SEND) {
        printf("[%s] %s", msg.nick_sender, payload);
    }

    if (msg.type == BROADCAST_SEND) {
        printf("[%s] %s\n", msg.nick_sender, payload);
    }

    if (msg.type == UNICAST_SEND) {
        if (msg.status == ERROR) {
            fprintf(stderr, "%s\n", payload);
        } else {
            printf("[%s] (privé) %s\n", msg.nick_sender, payload);
        }
    }

    if (msg.type == FILE_REQUEST) {
        if (msg.status == ERROR) {
            fprintf(stderr, "%s\n", payload);
            if (pending_file_request->type == FILE_REQUEST &&
                pending_file_request->file_fd != -1 &&
                strcmp(msg.infos, pending_file_request->peer) == 0) {
                close(pending_file_request->file_fd);
                pending_file_request->file_fd = -1;
                pending_file_request->type = NO_FILE_TRANSFER;
            }
        } else {
            if (pending_file_request->type != NO_FILE_TRANSFER ||
                fds[2].fd != -1 || fds[3].fd != -1 ||
                memchr(payload, '\0', (size_t)msg.pld_len) != NULL ||
                !valid_filename(payload)) {
                struct message reject = {0};
                reject.type = FILE_REJECT;
                strncpy(reject.nick_sender, nick, NICK_LEN - 1);
                strncpy(reject.infos, msg.nick_sender, INFOS_LEN - 1);
                fprintf(stderr, "Busy or invalid file name; rejecting file request.\n");
                return send_protocol_message(sockfd, &reject, NULL);
            }
            strcpy(pending_file_request->filename, payload);
            printf("[%s] wants you to accept the transfer of the file \"%s\". Do you accept ? [Y/N]\n", msg.nick_sender, payload);
            pending_file_request->type = FILE_REQUEST;
            strncpy(pending_file_request->peer, msg.nick_sender, NICK_LEN - 1);
            pending_file_request->peer[NICK_LEN - 1] = '\0';
        }
    }

    if (msg.type == FILE_ACCEPT) {
        if (msg.status == ERROR) { // File transfer request was rejected or encountered an error
            fprintf(stderr, "%s\n", payload);
            if (pending_file_request->type == FILE_REQUEST &&
                pending_file_request->file_fd != -1 &&
                strcmp(msg.nick_sender, pending_file_request->peer) == 0) {
                close(pending_file_request->file_fd);
                pending_file_request->file_fd = -1;
                pending_file_request->type = NO_FILE_TRANSFER;
            }
            if (fds[2].fd != -1 &&
                strcmp(msg.infos, pending_file_request->peer) == 0) {
                close(fds[2].fd);
                fds[2].fd = -1;
                fds[2].revents = 0;
                pending_file_request->type = NO_FILE_TRANSFER;
            }
        } else {
            printf("[%s] accepted the file transfer. Address: %s\n", msg.nick_sender, payload);
            if (pending_file_request->type != FILE_REQUEST ||
                pending_file_request->file_fd == -1 ||
                strcmp(msg.nick_sender, pending_file_request->peer) != 0) {
                fprintf(stderr, "Unexpected file transfer acceptance.\n");
                return 0;
            }
            if (fds[2].fd != -1 || fds[3].fd != -1) {
                fprintf(stderr, "A file connection is already active.\n");
                return 0;
            }
            int peer_fd = connect_to_peer(payload, (size_t)msg.pld_len);
            if (peer_fd == -1) {
                close(pending_file_request->file_fd);
                pending_file_request->file_fd = -1;
                pending_file_request->type = NO_FILE_TRANSFER;
                return 0;
            }
            fds[3].fd = peer_fd;
            fds[3].events = POLLIN;
            fds[3].revents = 0;
            printf("Connected to %s for file transfer.\n", msg.nick_sender);
            int result = send_file(peer_fd, nick, pending_file_request);
            if (close(pending_file_request->file_fd) == -1) {
                perror("Could not close file");
                result = -1;
            }
            pending_file_request->file_fd = -1;
            if (result == -1) {
                fprintf(stderr, "File transfer failed.\n");
                close(fds[3].fd);
                fds[3].fd = -1;
                fds[3].revents = 0;
                pending_file_request->type = NO_FILE_TRANSFER;
            } else {
                pending_file_request->type = FILE_ACK;
            }
        }
    }

    if (msg.type == FILE_REJECT) {
        if (msg.status == ERROR) {
            fprintf(stderr, "%s\n", payload);
        } else {
            printf("[%s] rejected the file transfer.\n", msg.nick_sender);
        }
        if (pending_file_request->type == FILE_REQUEST &&
            pending_file_request->file_fd != -1 &&
            strcmp(msg.nick_sender, pending_file_request->peer) == 0) {
            close(pending_file_request->file_fd);
            pending_file_request->file_fd = -1;
            pending_file_request->type = NO_FILE_TRANSFER;
        }
    }
    return 0;
}

int handle_connect(char *host, char *port) {
	struct addrinfo hints, *result, *rp;
	int sfd;
	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, port, &hints, &result) != 0) {
		perror("getaddrinfo()");
		exit(EXIT_FAILURE);
	}
	for (rp = result; rp != NULL; rp = rp->ai_next) {
		sfd = socket(rp->ai_family, rp->ai_socktype,rp->ai_protocol);
		if (sfd == -1) {
			continue;
		}
		if (connect(sfd, rp->ai_addr, rp->ai_addrlen) != -1) {
			printf("Connected\n");
            break;
		}
		close(sfd);
	}
	if (rp == NULL) {
		fprintf(stderr, "Could not connect\n");
		exit(EXIT_FAILURE);
	}
	freeaddrinfo(result);
	return sfd;
}

int main(int argc, char *argv[]) {
	if (argc < 3) {
		fprintf(stderr, "Usage: %s <host> <port>\n", argv[0]);
		exit(EXIT_FAILURE);
	}

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal");
        return EXIT_FAILURE;
    }

    char *host = argv[1];
    char *port = argv[2];
	int sfd;
	sfd = handle_connect(host, port);

	struct pollfd fds[4] = {0}; // 2 more for file transfer sockets
    // Initialize pollfd for stdin and server socket
    fds[0].fd = 0;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    fds[1].fd = sfd;
    fds[1].events = POLLIN;
    fds[1].revents = 0;

    // Initialize pollfd for file transfer sockets
    fds[2].fd = -1;
    fds[2].events = POLLIN;
    fds[2].revents = 0;
    fds[3].fd = -1;
    fds[3].events = POLLIN;
    fds[3].revents = 0;

    char nick[NICK_LEN] = "";
    struct pending_file_request pending_file_request = {
        .type = NO_FILE_TRANSFER,
        .file_fd = -1
    };

	while (1) {
        int nfds = poll(fds, 4, -1);
        if (nfds < 0) {
            perror("poll");
            cleanup_transfer(&pending_file_request, fds);
			close(sfd);
            exit(EXIT_FAILURE);
        }
        if (fds[0].revents & POLLIN) {
            // Read from stdin
            if (echo_client(sfd, nick, &pending_file_request, fds) == -1) {
                cleanup_transfer(&pending_file_request, fds);
                close(sfd);
                return -1;
            }
            fds[0].revents = 0;
        }
        if (fds[1].revents & POLLIN) {
            // Read from server
			sleep(1);
            if (reply_server(sfd, nick, &pending_file_request, fds) == -1) {
                cleanup_transfer(&pending_file_request, fds);
                close(sfd);
                return -1;
            }
            fds[1].revents = 0;
        }
        if (fds[2].fd != -1 && (fds[2].revents & POLLIN)) {
            int peer_fd = accept(fds[2].fd, NULL, NULL);
            if (peer_fd == -1) {
                perror("Could not accept file sender");
            } else {
                close(fds[2].fd);
                fds[2].fd = -1;
                fds[2].revents = 0;
                fds[3].fd = peer_fd;
                fds[3].events = POLLIN;
                fds[3].revents = 0;
                pending_file_request.type = FILE_SEND;
                printf("File sender connected.\n");
            }
        }
        if (fds[3].fd != -1 && (fds[3].revents & POLLIN)) {
            int result = receive_file_message(fds[3].fd, nick, &pending_file_request);
            if (result != 0) {
                if (result == -1) {
                    discard_received_file(&pending_file_request);
                    pending_file_request.type = NO_FILE_TRANSFER;
                }
                close(fds[3].fd);
                fds[3].fd = -1;
                fds[3].revents = 0;
            }
        }
        if (fds[3].fd != -1 && !(fds[3].revents & POLLIN) &&
            (fds[3].revents & (POLLERR | POLLHUP | POLLNVAL))) {
            fprintf(stderr, "File transfer connection closed before completion.\n");
            discard_received_file(&pending_file_request);
            pending_file_request.type = NO_FILE_TRANSFER;
            close(fds[3].fd);
            fds[3].fd = -1;
            fds[3].revents = 0;
        }
    }
}

