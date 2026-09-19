#include "client_handler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>

// Maximum number of clients allowed to run concurrently.
static const unsigned int MAX_CONCURRENT_CLIENTS = 3;

int main(int argc, char *argv[]){
    if(argc != 2){
        printf("Usage: %s <port>\n", argv[0]);
        exit(1);
    }
    int port = atoi(argv[1]);

    // Create the listening socket that accepts connections from browsers.
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(server_fd < 0){
        perror("socket failed");
        exit(1);
    }

    if(init_client_handler(MAX_CONCURRENT_CLIENTS) < 0){
        perror("sem_init failed");
        close(server_fd);
        exit(1);
    }

    // Lets us restart the server quickly without "port already in use" errors.
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Bind the listening socket to a port on this machine.
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if(bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0){
        perror("bind failed");
        exit(1);
    }

    // Start listening; 5 is the pending-connection queue length.
    if(listen(server_fd, 5) < 0){
        perror("listen failed");
        exit(1);
    }

    printf("[main] listening on port %d\n", port);

    // Accept clients forever and hand each one to a detached worker thread.
    while(1){
        int client_fd = accept(server_fd, NULL, NULL);
        if(client_fd < 0){
            fprintf(stderr, "[main] accept failed\n");
            continue;
        }

        printf("[main] accepted client fd=%d; starting worker\n", client_fd);

        // pthread_create receives one void* argument, so allocate the fd and
        // let handle_client copy and free it inside the worker thread.
        int *client_arg = malloc(sizeof(*client_arg));
        if(client_arg == NULL){
            fprintf(stderr, "[main] could not allocate client argument\n");
            close(client_fd);
            continue;
        }
        *client_arg = client_fd;

        pthread_t tid;
        int thread_result = pthread_create(&tid, NULL, handle_client, client_arg);
        if(thread_result != 0){
            fprintf(stderr, "[main] pthread_create failed: %s\n", strerror(thread_result));
            free(client_arg);
            close(client_fd);
            continue;
        }

        // The server runs continuously, so workers clean up themselves.
        pthread_detach(tid);
    }

    close(server_fd);
    return 0;
}
