#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <semaphore.h>
#include <netdb.h>

sem_t client_limit;
// Maximum number of clients allowed to run concurrently.
static const unsigned int MAX_CONCURRENT_CLIENTS = 3;

// The small subset of the request destination that this proxy currently needs.
struct request_target{
    char host[256];
    char path[2048];
};

// Extract an absolute HTTP URL and the Host header from the raw client request.
// This is intentionally simple for now; a complete proxy parser will replace it.
int extract_request_target(const char *request, struct request_target *target){
    char method[16];
    char url[2048];
    char version[16];
    const char *host_header;
    const char *url_path;

    if(sscanf(request, "%15s %2047s %15s", method, url, version) != 3){
        return -1;
    }

    // Only support GET requests using the absolute URL form expected by proxies.
    if(strcmp(method, "GET") != 0 || strncmp(url, "http://", 7) != 0){
        return -1;
    }

    // The path begins at the first slash after "http://hostname".
    url_path = strchr(url + 7, '/');
    if(url_path == NULL){
        strcpy(target->path, "/");
    }else{
        if(strlen(url_path) >= sizeof(target->path)){
            return -1;
        }
        strcpy(target->path, url_path);
    }

    // Find the hostname that the client supplied in its Host header.
    host_header = strstr(request, "Host:");
    if(host_header == NULL || sscanf(host_header, "Host: %255[^\r\n]", target->host) != 1){
        return -1;
    }

    return 0;
}

// Send a real HTTP error instead of leaving an unsupported client hanging.
void send_unsupported_response(int client_fd){
    const char *response =
        "HTTP/1.1 501 Not Implemented\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n";

    if(send(client_fd, response, strlen(response), 0) < 0){
        perror("send error response failed");
    }
}

void *handle_client(void *arg){
    // A thread may exist while waiting, but only a limited number may actively
    // handle clients at the same time.
    if(sem_wait(&client_limit) < 0){
        perror("sem_wait failed");
        return NULL;
    }

    // The main thread allocated this integer for pthread_create().
    int client_fd = *(int *)arg;
    free(arg);

    printf("Client connected!\n");

    // Read the request in chunks until the complete HTTP header arrives.
    // One recv() call is not guaranteed to contain the complete request.
    size_t buffer_capacity = 4096;
    size_t total_bytes_read = 0;
    char *buffer = malloc(buffer_capacity);
    int headers_complete = 0;

    if(buffer == NULL){
        perror("malloc failed");
        close(client_fd);
        sem_post(&client_limit);
        return NULL;
    }

    while (!headers_complete){
        if(total_bytes_read + 1 >= buffer_capacity){
            size_t new_capacity = buffer_capacity * 2;
            char *larger_buffer = realloc(buffer, new_capacity);

            if(larger_buffer == NULL){
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

        if(bytes_read < 0){
            perror("recv failed");
            break;
        }

        if(bytes_read == 0){
            printf("Client disconnected before the complete HTTP header arrived.\n");
            break;
        }

        total_bytes_read += (size_t) bytes_read;
        buffer[total_bytes_read] = '\0';

        if(strstr(buffer, "\r\n\r\n") != NULL){
            headers_complete = 1;
        }
    }

    if(buffer != NULL && headers_complete){
        printf("----- Received %zu bytes -----\n%s\n-----------------------------\n",
               total_bytes_read, buffer);

        struct request_target target;
        if(extract_request_target(buffer, &target) < 0){
            printf("Unsupported request: expected GET http://host/path HTTP/1.1\n");
            send_unsupported_response(client_fd);
        }else{
            printf("Destination host: %s\n", target.host);
            printf("Destination path: %s\n", target.path);

            // Resolve the hostname into an IP address using DNS.
            struct hostent *server = gethostbyname(target.host);
            int remote_fd = -1;

            if(server == NULL){
                herror("gethostbyname failed");
            }else{
                // This socket is separate from client_fd: it connects outward
                // from the proxy to the destination web server.
                remote_fd = socket(AF_INET, SOCK_STREAM, 0);
                if(remote_fd < 0){
                    perror("remote socket failed");
                }else{
                    // Build the destination address: IPv4, resolved IP, port 80.
                    struct sockaddr_in remote_address;
                    memset(&remote_address, 0, sizeof(remote_address));
                    remote_address.sin_family = AF_INET;
                    remote_address.sin_port = htons(80);
                    memcpy(
                        &remote_address.sin_addr.s_addr,
                        server->h_addr,
                        server->h_length
                    );

                    // Unlike the listening socket, this socket only needs connect().
                    if(connect(
                        remote_fd,
                        (struct sockaddr *)&remote_address,
                        sizeof(remote_address)
                    ) < 0){
                        perror("connect to remote server failed");
                        close(remote_fd);
                        remote_fd = -1;
                    }else{
                        printf("Connected to %s on port 80.\n", target.host);

                        // Convert the client's proxy-style absolute URL into the
                        // origin-form request expected by the destination server.
                        char remote_request[4096];
                        int request_length = snprintf(
                            remote_request,
                            sizeof(remote_request),
                            "GET %s HTTP/1.1\r\n"
                            "Host: %s\r\n"
                            "Connection: close\r\n"
                            "\r\n",
                            target.path,
                            target.host
                        );

                        if(request_length < 0 || (size_t)request_length >= sizeof(remote_request)){
                            fprintf(stderr, "Remote request is too large\n");
                        }else if(send(
                            remote_fd,
                            remote_request,
                            (size_t)request_length,
                            0
                        ) < 0){
                            perror("send to remote server failed");
                        }else{
                            // Relay each response chunk immediately instead of
                            // buffering the entire response in memory.
                            char relay_buf[4096];
                            ssize_t bytes_read;

                            while((bytes_read = recv(
                                remote_fd,
                                relay_buf,
                                sizeof(relay_buf),
                                0
                            )) > 0){
                                if(send(
                                    client_fd,
                                    relay_buf,
                                    (size_t)bytes_read,
                                    0
                                ) < 0){
                                    perror("send to client failed");
                                    break;
                                }
                            }

                            if(bytes_read < 0){
                                perror("recv from remote server failed");
                            }
                        }
                    }
                }
            }

            if(remote_fd >= 0){
                close(remote_fd);
            }
        }
    }
    sleep(5);
    // Return the semaphore token before this detached thread exits.
    free(buffer);
    close(client_fd);
    sem_post(&client_limit);
    printf("Connection closed. Waiting for the next client...\n");
    return NULL;
}

int main(int argc, char *argv[]){
    if(argc != 2){
        printf("Usage: %s <port>\n", argv[0]);
        exit(1);
    }
    int port = atoi(argv[1]);

    // 1. Create a socket -- this is just asking the OS for a "phone line" handle
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(server_fd < 0){
        perror("socket failed");
        exit(1);
    }

    if(sem_init(&client_limit, 0, MAX_CONCURRENT_CLIENTS) < 0){
        perror("sem_init failed");
        close(server_fd);
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

    if(bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0){
        perror("bind failed");
        exit(1);
    }

    // 3. Start listening -- 5 is how many pending connections can queue up
    if(listen(server_fd, 5) < 0){
        perror("listen failed");
        exit(1);
    }

    printf("Listening on port %d... waiting for connections\n", port);

    // 4. Keep accepting clients. accept() blocks until a client connects.
    //    Each client is handled by its own detached thread.
    while (1){
        int client_fd = accept(server_fd, NULL, NULL);
        if(client_fd < 0){
            perror("accept failed");
            continue;
        }

        int *client_arg = malloc(sizeof(*client_arg));
        if(client_arg == NULL){
            perror("malloc failed");
            close(client_fd);
            continue;
        }
        *client_arg = client_fd;

        pthread_t tid;
        int thread_result = pthread_create(&tid, NULL, handle_client, client_arg);
        if(thread_result != 0){
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
