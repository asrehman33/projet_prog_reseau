#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>
#include "chained_list.h"

#define BUFFER_SIZE 1024
#define MAX_CNN 20

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

int echo_server(int sockfd, int client_index) {
	char buff[BUFFER_SIZE];
	int msg_size;
	// Recv
	if (recv_next_message_size(sockfd, &msg_size) == -1) {
		return -1;
	}
	if (recv_message(sockfd, buff, msg_size) == -1) {
		return -1;
	}
	printf("Received: %.*s\n", msg_size, buff);

	if (strcmp(buff, "/quit\n") == 0) {
		return -1;
	}

	// Send (ECHO)
	send_next_message_size(sockfd, msg_size);
	send_message(sockfd, buff, msg_size);

	printf("Sent to client %d: %.*s\n", client_index, msg_size, buff);
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
		sfd = socket(rp->ai_family, rp->ai_socktype,
		rp->ai_protocol);

		// Allows the socket to be reused immediately after the program terminates
		if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) == -1) {
			perror("setsockopt - in main");
			exit(1);
		}

		if (sfd == -1) {
			continue;
		}
		if (bind(sfd, rp->ai_addr, rp->ai_addrlen) == 0) {
			break;
		}
		close(sfd);
	}
	if (rp == NULL) {
		perror("Could not bind");
		exit(EXIT_FAILURE);
	}
	freeaddrinfo(result);
	return sfd;
}

int handle_multiple_connection(int listen_df, struct list *lst)
{
	struct sockaddr_in client_addr;
    socklen_t size_addr = sizeof(client_addr);

	struct pollfd fds[MAX_CNN] = {0};

    fds[0].fd = listen_df;
    fds[0].events = POLLIN;
    fds[0].revents = 0;

    for (int i = 1; i < MAX_CNN; i++)
    {
        fds[i].fd = -1;
        fds[i].events = 0;
        fds[i].revents = 0;
    }

	while(1)
    {
        poll(fds, MAX_CNN, -1);

        for (int i = 0; i < MAX_CNN; i++)
        {
            if ((fds[i].revents & POLLIN) && fds[i].fd == listen_df)
            {
                int new_fd = accept(listen_df, (struct sockaddr*)&client_addr, &size_addr);
                // die(new_fd, "On accepting");
                printf("New connection accepted\n");

                for (int j = 1; j < MAX_CNN; j++)
                {
                    if (fds[j].fd == -1)
                    {
                        fds[j].fd = new_fd;
                        fds[j].events = POLLIN;
                        fds[j].revents = 0;
                        struct node *node = add_node(lst, new_fd, client_addr);
						print_node(node);
                        break;
                    }
                }
				print_list(lst);
                
                fds[i].revents = 0;
            }
            else if ((fds[i].revents & POLLIN) && fds[i].fd != listen_df)
            {
                if (echo_server(fds[i].fd, i) == -1) {
                    close(fds[i].fd);
                    remove_by_fd(lst, fds[i].fd);
					print_list(lst);
                    fds[i].fd = -1;
                    fds[i].events = 0;
                }
				fds[i].revents = 0;
            }
            else if ((fds[i].revents & POLLHUP) && fds[i].fd != listen_df)
            {
				// Client disconnected, remove from list and close socket
                printf("Connection closed by client 3\n");
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
	int sfd;
	sfd = handle_bind(port);
	if ((listen(sfd, SOMAXCONN)) != 0) {
		perror("listen()\n");
		exit(EXIT_FAILURE);
	}

	struct list lst = {0};

	handle_multiple_connection(sfd, &lst);
	close(sfd);
	return EXIT_SUCCESS;
}

