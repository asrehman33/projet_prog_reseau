#include <netinet/in.h>
#include <stdlib.h>
#include <stdio.h>
#include <arpa/inet.h>
#include "chained_list.h"

struct node *add_node(struct list *lst, int fd, int index, struct sockaddr_in addr) {
    struct node *new_node = malloc(sizeof(struct node));
    new_node->fd = fd;
    new_node->index = index;
    new_node->addr = addr;
    new_node->next = NULL;
    new_node->nickname[0] = '\0';

    if (lst->head == NULL) {
        lst->head = new_node;
    } else {
        struct node *current = lst->head;
        while (current->next != NULL) {
            current = current->next;
        }
        current->next = new_node;
    }
    return new_node;
}

int len(struct list *lst) {
    int count = 0;
    struct node *current = lst->head;
    while (current != NULL) {
        count++;
        current = current->next;
    }
    return count;
}

void remove_by_fd(struct list *lst, int fd) {
    struct node *current = lst->head;
    struct node *prev = NULL;
    while (current != NULL) {
        if (current->fd == fd) {
            if (prev == NULL) {
                lst->head = current->next;
            } else {
                prev->next = current->next;
            }
            free(current);
            return;
        }
        prev = current;
        current = current->next;
    }
}

void free_list(struct list *lst) {
    struct node *current = lst->head;
    while (current != NULL) {
        struct node *next = current->next;
        free(current);
        current = next;
    }
    lst->head = NULL;
}

void print_list(struct list *lst) {
    struct node *current = lst->head;
    while (current != NULL) {
        printf("fd%d->", current->fd);
        current = current->next;
    }
    printf("NULL\n");
}

void print_node(struct node *node) {
    if (node == NULL) {
        return;
    }
    printf("fd: %d | %s:%d\n", node->fd, inet_ntoa(node->addr.sin_addr), ntohs(node->addr.sin_port));
}

struct node *get_node_by_index(struct list *lst, int index) {
    struct node *current = lst->head;

    while (current != NULL) {
        if (current->index == index) {
            return current;
        }
        current = current->next;
    }

    return NULL;
}