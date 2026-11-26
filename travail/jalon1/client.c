#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>

#define BUFFER_SIZE 10

int die(int ret_value, char *msg) {
    if (ret_value < 0) {
        perror(msg);
        exit(EXIT_FAILURE);
    }
    return 0;
}

int send_next_message_size(int sockfd, int msg_size) {
    int bytes_sent = 0;
    int ret;
    while (bytes_sent != sizeof(msg_size)) {
        ret = write(sockfd, ((char *)&msg_size) + bytes_sent, sizeof(msg_size) - bytes_sent);
        die(ret, "On writing message size");
        bytes_sent += ret;
    }
    return 0;
}

int send_message(int sockfd, char *msg, int msg_size) {
    int bytes_sent = 0;
    int ret;
    while (bytes_sent != msg_size) {
        ret = write(sockfd, msg + bytes_sent, msg_size - bytes_sent);
        die(ret, "On writing");
        bytes_sent += ret;
    }
    return 0;
}

int recv_next_message_size(int sockfd, int *msg_size) {
    int bytes_read = 0;
    int ret;
    while (bytes_read != sizeof(int)) {
        ret = read(sockfd, ((char *)msg_size) + bytes_read, sizeof(int) - bytes_read);
        die(ret, "On reading message size");
        if (ret == 0) {
            return -1;
        }
        bytes_read += ret;
    }
    return 0;
}

int recv_message(int sockfd, char *msg, int msg_size) {
    int bytes_read = 0;
    int ret;
    while (bytes_read != msg_size) {
        ret = read(sockfd, msg + bytes_read, msg_size - bytes_read);
        die(ret, "On reading");
        if (ret == 0) {
            return -1;
        }
        bytes_read += ret;
    }
    return 0;
}

int echo_client(int sockfd) {
	char buff[BUFFER_SIZE];		
    
    // Send
    memset(buff, 0, BUFFER_SIZE);
    read(STDIN_FILENO, buff, BUFFER_SIZE);
    int len = strlen(buff);
    send_next_message_size(sockfd, len);
    send_message(sockfd, buff, len);
    printf("Send: %s\n", buff);

    if (strcmp(buff, "/quit\n") == 0) {
        return -1;
    }

	return 0;
}

int reply_server(int sockfd) {
	int msg_size;
	if (recv_next_message_size(sockfd, &msg_size) < 0) {
		printf("Server closed connection\n");
		return -1;
	}
	if (msg_size > 0) {
		char *msg = malloc(msg_size);
		if (recv_message(sockfd, msg, msg_size) < 0) {
			printf("Server closed connection\n");
			free(msg);
			return -1;
		}
		printf("Received from server: %.*s\n", msg_size, msg);
		free(msg);
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

	while (1) {
        int nfds = poll(fds, 2, -1);
        if (nfds < 0) {
            perror("poll");
			close(sfd);
            exit(EXIT_FAILURE);
        }
        if (fds[0].revents & POLLIN) {
            // Read from stdin
            if (echo_client(sfd) == -1) {
                close(sfd);
                return 0;
            }
            fds[0].revents = 0;
        }
        if (fds[1].revents & POLLIN) {
            // Read from server
			sleep(1);
            if (reply_server(sfd) == -1) {
                close(sfd);
                return 0;
            }
            fds[1].revents = 0;
        }
    }
}

