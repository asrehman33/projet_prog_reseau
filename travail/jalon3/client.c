#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>
#include "msg_struct.h"
#include <ctype.h>

#define BUFFER_SIZE 1024

struct pending_file_request {
    int is_pending;
    char dest[INFOS_LEN];
    int listener_fd;
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

int echo_client(int sockfd, const char *nick, struct pending_file_request *pending_file_request) {
    char buff[BUFFER_SIZE];
    
    memset(buff, 0, BUFFER_SIZE);
    if (read(STDIN_FILENO, buff, BUFFER_SIZE - 1) <= 0) {
        return -1;
    }

    struct message msg;
    memset(&msg, 0, sizeof(msg));
    strncpy(msg.nick_sender, nick, NICK_LEN - 1); // Copy the current nickname into the message sender field
    char *payload = "";
    int command_found = 0;

    if (pending_file_request->is_pending == 1) {
        // Handle pending file request confirmation (Y/N)
        if (strncmp(buff, "Y\n", 2) == 0 || strncmp(buff, "y\n", 2) == 0) {
            char endpoint[INET_ADDRSTRLEN + 6];
            msg.type = FILE_ACCEPT;
            command_found = 1;
            strncpy(msg.infos, pending_file_request->dest, INFOS_LEN - 1);
            msg.infos[INFOS_LEN - 1] = '\0';
            if (pending_file_request->listener_fd != -1) {
                fprintf(stderr, "A file transfer is already awaiting a connection.\n");
                return 0;
            }
            int listener_fd = handle_bind_for_file_transfert("8081"); // Bind to any available port for file transfer
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
            pending_file_request->listener_fd = listener_fd;
        } else if (strncmp(buff, "N\n", 2) == 0 || strncmp(buff, "n\n", 2) == 0) {
            msg.type = FILE_REJECT;
            command_found = 1;
            strncpy(msg.infos, pending_file_request->dest, INFOS_LEN - 1);
            msg.infos[INFOS_LEN - 1] = '\0';
        } else {
            fprintf(stderr, "Please answer Y or N.\n");
            return 0;
        }
        pending_file_request->is_pending = 0;
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

            msg.type = FILE_REQUEST;
            command_found = 1;
            strncpy(msg.infos, request, INFOS_LEN - 1);
            msg.pld_len = strlen(filename);
            payload = filename;
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
        return 0;
    }
    printf("Send: %s %s\n", msg_type_str[msg.type], msg.infos);
    return 0;
}

int reply_server(int sockfd, char *nick, struct pending_file_request *pending_file_request) {
    struct message msg;
    char payload[BUFFER_SIZE];
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
        } else {
            if (pending_file_request->is_pending ||
                pending_file_request->listener_fd != -1) { // Already handling a file request, cannot accept another one simultaneously
                struct message reject = {0};
                reject.type = FILE_REJECT;
                strncpy(reject.nick_sender, nick, NICK_LEN - 1);
                strncpy(reject.infos, msg.nick_sender, INFOS_LEN - 1);
                fprintf(stderr, "Already handling a file request; rejecting another.\n");
                return send_protocol_message(sockfd, &reject, NULL);
            }
            printf("[%s] wants you to accept the transfer of the file \"%s\". Do you accept ? [Y/N]\n", msg.nick_sender, payload);
            pending_file_request->is_pending = 1;
            strncpy(pending_file_request->dest, msg.nick_sender, INFOS_LEN - 1);
            pending_file_request->dest[INFOS_LEN - 1] = '\0';
        }
    }

    if (msg.type == FILE_ACCEPT) {
        if (msg.status == ERROR) { // File transfer request was rejected or encountered an error
            fprintf(stderr, "%s\n", payload);
            if (pending_file_request->listener_fd != -1 &&
                strcmp(msg.infos, pending_file_request->dest) == 0) {
                close(pending_file_request->listener_fd);
                pending_file_request->listener_fd = -1;
            }
        } else {
            printf("[%s] accepted the file transfer. Address: %s\n", msg.nick_sender, payload);
        }
    }

    if (msg.type == FILE_REJECT) {
        if (msg.status == ERROR) {
            fprintf(stderr, "%s\n", payload);
        } else {
            printf("[%s] rejected the file transfer.\n", msg.nick_sender);
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
    struct pending_file_request pending_file_request = {.listener_fd = -1};

	while (1) {
        int nfds = poll(fds, 2, -1);
        if (nfds < 0) {
            perror("poll");
            if (pending_file_request.listener_fd != -1) {
                close(pending_file_request.listener_fd);
            }
			close(sfd);
            exit(EXIT_FAILURE);
        }
        if (fds[0].revents & POLLIN) {
            // Read from stdin
            if (echo_client(sfd, nick, &pending_file_request) == -1) {
                if (pending_file_request.listener_fd != -1) {
                    close(pending_file_request.listener_fd);
                }
                close(sfd);
                return -1;
            }
            fds[0].revents = 0;
        }
        if (fds[1].revents & POLLIN) {
            // Read from server
			sleep(1);
            if (reply_server(sfd, nick, &pending_file_request) == -1) {
                if (pending_file_request.listener_fd != -1) {
                    close(pending_file_request.listener_fd);
                }
                close(sfd);
                return -1;
            }
            fds[1].revents = 0;
        }
    }
}

