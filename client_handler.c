/*
 * Responsibility: own the per-client lifecycle, including request buffering,
 * cache lookup, remote fetching, error responses, and semaphore accounting.
 */

#include "client_handler.h"
#include "cache.h"
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

// Read one complete request, serve a cache hit, or coordinate a remote fetch.
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
        printf("[client fd=%d] request received: %zu bytes\n",
               client_fd, total_bytes_read);

        struct request_target target;
        if(extract_request_target(buffer, &target) < 0){
            printf(
                "[client fd=%d] unsupported request; only GET http://host/path is supported\n",
                client_fd
            );
            send_unsupported_response(client_fd);
        }else{
            printf(
                "[request fd=%d] host=%s path=%s\n",
                client_fd,
                target.host,
                target.path
            );

            char cache_key[4096];
            // NOTE: query parameters are part of the key, so cache-busting
            // parameters correctly produce distinct, uncacheable entries.
            int key_length = snprintf(
                cache_key,
                sizeof(cache_key),
                "http://%s%s",
                target.host,
                target.path
            );

            if(key_length < 0 || (size_t)key_length >= sizeof(cache_key)){
                printf("[cache fd=%d] request key is too large; skipping cache\n", client_fd);
            }else{
                char *cached_data = NULL;
                size_t cached_size = 0;

                // NOTE: concurrent misses for the same key are not coalesced;
                // cache-stampede protection is a future improvement.
                if(cache_get(cache_key, &cached_data, &cached_size)){
                    printf(
                        "[cache fd=%d] HIT key=%s bytes=%zu\n",
                        client_fd,
                        cache_key,
                        cached_size
                    );
                    if(send(client_fd, cached_data, cached_size, 0) < 0){
                        fprintf(stderr, "[cache fd=%d] failed to send cached response\n", client_fd);
                    }
                    free(cached_data);
                }else{
                    printf("[cache fd=%d] MISS key=%s\n", client_fd, cache_key);

                    char *response_data = NULL;
                    size_t response_size = 0;
                    if(fetch_remote_response(
                        target.host,
                        target.path,
                        client_fd,
                        &response_data,
                        &response_size
                    ) < 0){
                        printf("[client fd=%d] remote fetch failed\n", client_fd);
                    }else{
                        cache_put(cache_key, response_data, response_size);
                        printf(
                            "[cache fd=%d] STORED key=%s bytes=%zu\n",
                            client_fd,
                            cache_key,
                            response_size
                        );
                        free(response_data);
                    }
                }
            }
        }
    }

    // Return the semaphore token before this detached thread exits.
    free(buffer);
    close(client_fd);
    sem_post(&client_limit);
    printf("[client fd=%d] done\n", client_fd);
    return NULL;
}
