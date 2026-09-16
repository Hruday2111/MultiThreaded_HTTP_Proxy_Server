#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>

void *handle_client(void *arg){
    sleep(10);
    int client_fd = *(int *)arg;
    free(arg);

    printf("Client connected!\n");

    // Read the request in chunks until the complete HTTP header arrives.
    // One recv() call is not guaranteed to contain the complete request.
    size_t buffer_capacity = 4096;
    size_t total_bytes_read = 0;
    char *buffer = malloc(buffer_capacity);
    int headers_complete = 0;

    if (buffer == NULL){
        perror("malloc failed");
        close(client_fd);
        return NULL;
    }

    while (!headers_complete){
        if (total_bytes_read + 1 >= buffer_capacity){
            size_t new_capacity = buffer_capacity * 2;
            char *larger_buffer = realloc(buffer, new_capacity);

            if (larger_buffer == NULL){
                perror("realloc failed");
                free(buffer);
                buffer = NULL;
                break;
            }

            buffer = larger_buffer;
            buffer_capacity = new_capacity;
        }

        ssize_t bytes_read = recv(
            client_fd,
            buffer + total_bytes_read,
            buffer_capacity - total_bytes_read - 1,
            0
        );

        if (bytes_read < 0){
            perror("recv failed");
            break;
        }

        if (bytes_read == 0){
            printf("Client disconnected before the complete HTTP header arrived.\n");
            break;
        }

        total_bytes_read += (size_t) bytes_read;
        buffer[total_bytes_read] = '\0';

        if (strstr(buffer, "\r\n\r\n") != NULL){
            headers_complete = 1;
        }
    }

    if (buffer != NULL && headers_complete){
        printf("----- Received %zu bytes -----\n%s\n-----------------------------\n",
               total_bytes_read, buffer);

        // Send back a fixed, hardcoded HTTP response.
        const char *response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 13\r\n"
            "Connection: close\r\n"
            "\r\n"
            "Hello, world!";

        if (send(client_fd, response, strlen(response), 0) < 0){
            perror("send failed");
        }
    }

    free(buffer);
    close(client_fd);
    printf("Connection closed. Waiting for the next client...\n");
    return NULL;
}

int main(int argc, char *argv[]){
    if (argc != 2){
        printf("Usage: %s <port>\n", argv[0]);
        exit(1);
    }
    int port = atoi(argv[1]);

    // 1. Create a socket -- this is just asking the OS for a "phone line" handle
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0){
        perror("socket failed");
        exit(1);
    }

    // Lets us restart the server quickly without "port already in use" errors
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 2. Bind it to a specific port on this machine
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;  // accept on any local network interface
    address.sin_port = htons(port);        // htons = convert port into network byte order

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0){
        perror("bind failed");
        exit(1);
    }

    // 3. Start listening -- 5 is how many pending connections can queue up
    if (listen(server_fd, 5) < 0){
        perror("listen failed");
        exit(1);
    }

    printf("Listening on port %d... waiting for connections\n", port);

    // 4. Keep accepting clients. accept() blocks until a client connects.
    //    Each client is handled by its own detached thread.
    while (1){
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0){
            perror("accept failed");
            continue;
        }

        int *client_arg = malloc(sizeof(*client_arg));
        if (client_arg == NULL){
            perror("malloc failed");
            close(client_fd);
            continue;
        }
        *client_arg = client_fd;

        pthread_t tid;
        int thread_result = pthread_create(&tid, NULL, handle_client, client_arg);
        if (thread_result != 0){
            fprintf(stderr, "pthread_create failed: %s\n", strerror(thread_result));
            free(client_arg);
            close(client_fd);
            continue;
        }

        pthread_detach(tid);
    }

    // The loop currently runs until the process is stopped.
    close(server_fd);
    return 0;
}
