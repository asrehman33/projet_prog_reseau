#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>

#define MSG_LEN 1024
#define MAX_CNN 20

void echo_server(int sockfd) {
	char buff[MSG_LEN];
	// Cleaning memory
	memset(buff, 0, MSG_LEN);
	// Receiving message
	recv(sockfd, buff, MSG_LEN, 0);

	printf("Received: %s", buff);
	// Sending message (ECHO)
	send(sockfd, buff, strlen(buff), 0);

	printf("Message sent!\n");
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

int handle_multiple_connection(int listen_df)
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
                        break;
                    }
                }
                
                fds[i].revents = 0;
            }
            else if ((fds[i].revents & POLLIN) && fds[i].fd != listen_df)
            {
                echo_server(fds[i].fd);
				fds[i].revents = 0;
            }
            else if ((fds[i].revents & POLLHUP) && fds[i].fd != listen_df)
            {
                printf("Connection closed by client 3\n");
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

	handle_multiple_connection(sfd);
	close(sfd);
	return EXIT_SUCCESS;
}

