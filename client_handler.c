#include "client_handler.h"
#include "http_parser.h"
#include "remote_fetch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <semaphore.h>

static sem_t client_limit;

// Initialize the limit that controls how many client threads may actively run.
int init_client_handler(unsigned int max_clients){
    return sem_init(&client_limit, 0, max_clients);
}

// Send a real HTTP error instead of leaving an unsupported client hanging.
void send_unsupported_response(int client_fd){
    const char *response =
        "HTTP/1.1 501 Not Implemented\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n";

    if(send(client_fd, response, strlen(response), 0) < 0){
        fprintf(stderr, "[client fd=%d] failed to send 501 response\n", client_fd);
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

    printf("[client fd=%d] worker started\n", client_fd);

    // Read the request in chunks until the complete HTTP header arrives.
    // One recv() call is not guaranteed to contain the complete request.
    size_t buffer_capacity = 4096;
    size_t total_bytes_read = 0;
    char *buffer = malloc(buffer_capacity);
    int headers_complete = 0;

    if(buffer == NULL){
        fprintf(stderr, "[client fd=%d] request buffer allocation failed\n", client_fd);
        close(client_fd);
        sem_post(&client_limit);
        return NULL;
    }

    while(!headers_complete){
        if(total_bytes_read + 1 >= buffer_capacity){
            size_t new_capacity = buffer_capacity * 2;
            char *larger_buffer = realloc(buffer, new_capacity);

            if(larger_buffer == NULL){
                fprintf(stderr, "[client fd=%d] request buffer resize failed\n", client_fd);
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
            fprintf(stderr, "[client fd=%d] failed while reading request\n", client_fd);
            break;
        }

        if(bytes_read == 0){
            printf("[client fd=%d] disconnected before completing the request\n", client_fd);
            break;
        }

        total_bytes_read += (size_t)bytes_read;
        buffer[total_bytes_read] = '\0';

        if(strstr(buffer, "\r\n\r\n") != NULL){
            headers_complete = 1;
        }
    }

    if(buffer != NULL && headers_complete){
        printf("[client fd=%d] request received (%zu bytes)\n",
               client_fd, total_bytes_read);
        printf("[client fd=%d] ----- request begin -----\n%s",
               client_fd, buffer);
        printf("[client fd=%d] ----- request end -----\n", client_fd);

        struct request_target target;
        if(extract_request_target(buffer, &target) < 0){
            printf(
                "[parser fd=%d] unsupported request; only GET http://host/path is supported\n",
                client_fd
            );
            send_unsupported_response(client_fd);
        }else{
            printf(
                "[parser fd=%d] destination host=%s path=%s\n",
                client_fd,
                target.host,
                target.path
            );
            if(fetch_remote_response(target.host, target.path, client_fd) < 0){
                printf("[client fd=%d] remote fetch failed\n", client_fd);
            }else{
                printf("[client fd=%d] remote response relayed successfully\n", client_fd);
            }
        }
    }

    // Keep the test delay so the semaphore limit remains easy to observe.
    sleep(5);

    // Return the semaphore token before this detached thread exits.
    free(buffer);
    close(client_fd);
    sem_post(&client_limit);
    printf("[client fd=%d] connection closed; worker finished\n", client_fd);
    return NULL;
}
