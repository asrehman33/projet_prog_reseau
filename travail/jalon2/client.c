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
int echo_client(int sockfd, const char *nick) {
    char buff[BUFFER_SIZE];
    memset(buff, 0, BUFFER_SIZE);
    if (read(STDIN_FILENO, buff, BUFFER_SIZE - 1) <= 0) {
        return -1;
    }

    if (strcmp(buff, "/quit\n") == 0) {
        // QUIT logic here
        return -1;
    }

    struct message msg;
    memset(&msg, 0, sizeof(msg));
    strncpy(msg.nick_sender, nick, NICK_LEN - 1); // Copy the current nickname into the message sender field
    char *payload = "";

    if (strncmp(buff, "/nick ", 6) == 0) {
        char *new_nick = buff + 6;
        new_nick[strcspn(new_nick, "\n")] = '\0';
        if (verify_nickname(new_nick) == -1) {
            // We don't want the program to stop
            return 0;
        }
        msg.type = NICKNAME_NEW;
        strncpy(msg.infos, new_nick, INFOS_LEN - 1); // Copy the new nickname into the message infos field
    } else {
        if (nick[0] == '\0' && msg.nick_sender[0] == '\0') {
            fprintf(stderr, "Nickname not set.\nUse /nick <nickname> to set it.\n");
            return 0;
        }

        if (strncmp(buff, "/who", 4) == 0) {
            // NICKNAME_LIST logic here
            msg.type = NICKNAME_LIST;
        }

        if (strncmp(buff, "/whois ", 7) == 0) {
            char *whois_nick = buff + 7;
            whois_nick[strcspn(whois_nick, "\n")] = '\0';
            msg.type = NICKNAME_INFOS;
            strncpy(msg.infos, whois_nick, INFOS_LEN - 1);
        }

        if (strncmp(buff, "/msgall ", 8) == 0) {
            char *msgall_payload = buff + 8;
            msgall_payload[strcspn(msgall_payload, "\n")] = '\0';
            msg.type = BROADCAST_SEND;
            msg.pld_len = strlen(msgall_payload);
            payload = msgall_payload;
        }
    }

    if (send_protocol_message(sockfd, &msg, payload) == -1) {  
        return 0;
    }
    printf("Send: %s %s\n", msg_type_str[msg.type], msg.infos);
    return 0;
}

int reply_server(int sockfd, char *nick) {
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
            printf("[%s] %s%s", msg.nick_sender, msg.infos, payload);
        }
    }

    if (msg.type == NICKNAME_INFOS) {
        printf("[%s] %s", msg.nick_sender, payload);
    }

    if (msg.type == BROADCAST_SEND) {
        printf("[%s] %s", msg.nick_sender, payload);
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

	struct pollfd fds[2] = {0};
    fds[0].fd = 0;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    fds[1].fd = sfd;
    fds[1].events = POLLIN;
    fds[1].revents = 0;

    char nick[NICK_LEN] = "";

	while (1) {
        int nfds = poll(fds, 2, -1);
        if (nfds < 0) {
            perror("poll");
			close(sfd);
            exit(EXIT_FAILURE);
        }
        if (fds[0].revents & POLLIN) {
            // Read from stdin
            if (echo_client(sfd, nick) == -1) {
                close(sfd);
                return -1;
            }
            fds[0].revents = 0;
        }
        if (fds[1].revents & POLLIN) {
            // Read from server
			sleep(1);
            if (reply_server(sfd, nick) == -1) {
                close(sfd);
                return -1;
            }
            fds[1].revents = 0;
        }
    }
}

