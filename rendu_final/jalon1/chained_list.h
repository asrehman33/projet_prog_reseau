#ifndef CHAINED_LIST_H
#define CHAINED_LIST_H

#include <netinet/in.h>

struct node {
    int fd;
    struct sockaddr_in addr;
    struct node *next;
};

struct list {
    struct node *head;
};

struct node *add_node(struct list *lst, int fd, struct sockaddr_in addr);
int len(struct list *lst);
void remove_by_fd(struct list *lst, int fd);
void free_list(struct list *lst);
void print_list(struct list *lst);
void print_node(struct node *node);

#endif
