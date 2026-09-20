/*
 * Responsibility: resolve an HTTP destination, fetch its response, relay the
 * bytes to the client, and return a copy that the cache can store.
 */

#include "remote_fetch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netdb.h>

// Connect to the destination web server, send one request, and relay its
// response directly to the original client connection.
// The response is also accumulated so a successful miss can populate the cache.
int fetch_remote_response(
    const char *host,
    const char *path,
    int client_fd,
    char **response_data,
    size_t *response_size
){
    *response_data = NULL;
    *response_size = 0;

    // Resolve the hostname into an IP address using DNS.
    struct hostent *server = gethostbyname(host);
    int remote_fd = -1;

    if(server == NULL){
        fprintf(stderr, "[remote client fd=%d] DNS lookup failed for host=%s\n", client_fd, host);
        return -1;
    }

    // This socket is separate from client_fd: it connects outward from the
    // proxy to the destination web server.
    remote_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(remote_fd < 0){
        fprintf(stderr, "[remote client fd=%d] could not create remote socket\n", client_fd);
        return -1;
    }

    // Bound the remote read so one unresponsive origin cannot hold a worker
    // semaphore slot forever and poison later benchmark requests.
    struct timeval remote_timeout;
    remote_timeout.tv_sec = 5;
    remote_timeout.tv_usec = 0;
    setsockopt(
        remote_fd,
        SOL_SOCKET,
        SO_RCVTIMEO,
        &remote_timeout,
        sizeof(remote_timeout)
    );

    // Build the destination address: IPv4, resolved IP, and port 80.
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
        fprintf(
            stderr,
            "[remote client fd=%d remote fd=%d] connection failed to host=%s port=80\n",
            client_fd,
            remote_fd,
            host
        );
        close(remote_fd);
        return -1;
    }

    printf(
        "[remote %d -> %d] connected host=%s port=80\n",
        client_fd,
        remote_fd,
        host
    );

    // Convert the client's proxy-style absolute URL into the origin-form
    // request expected by the destination server.
    char remote_request[4096];
    int request_length = snprintf(
        remote_request,
        sizeof(remote_request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        path,
        host
    );

    if(request_length < 0 || (size_t)request_length >= sizeof(remote_request)){
        fprintf(stderr, "[remote client fd=%d] request is too large\n", client_fd);
        close(remote_fd);
        return -1;
    }

    if(send(remote_fd, remote_request, (size_t)request_length, 0) < 0){
        fprintf(
            stderr,
            "[remote client fd=%d remote fd=%d] failed to send request\n",
            client_fd,
            remote_fd
        );
        close(remote_fd);
        return -1;
    }

    // Relay each response chunk immediately instead of buffering the entire
    // response in memory. Connection: close makes recv() eventually return 0.
    char relay_buf[4096];
    ssize_t bytes_read;
    size_t total_bytes_relayed = 0;
    while((bytes_read = recv(remote_fd, relay_buf, sizeof(relay_buf), 0)) > 0){
        char *larger_response = (char *)realloc(
            *response_data,
            total_bytes_relayed + (size_t)bytes_read
        );
        if(larger_response == NULL){
            fprintf(stderr, "[remote client fd=%d] response buffer allocation failed\n", client_fd);
            free(*response_data);
            *response_data = NULL;
            close(remote_fd);
            return -1;
        }
        *response_data = larger_response;
        memcpy(
            *response_data + total_bytes_relayed,
            relay_buf,
            (size_t)bytes_read
        );

        if(send(client_fd, relay_buf, (size_t)bytes_read, 0) < 0){
            fprintf(
                stderr,
                "[relay client fd=%d remote fd=%d] failed to send response chunk\n",
                client_fd,
                remote_fd
            );
            free(*response_data);
            *response_data = NULL;
            close(remote_fd);
            return -1;
        }
        total_bytes_relayed += (size_t)bytes_read;
    }

    if(bytes_read < 0){
        fprintf(
            stderr,
            "[remote client fd=%d remote fd=%d] failed while reading response\n",
            client_fd,
            remote_fd
        );
        free(*response_data);
        *response_data = NULL;
        close(remote_fd);
        return -1;
    }

    printf(
        "[relay %d -> %d] bytes=%zu\n",
        client_fd,
        remote_fd,
        total_bytes_relayed
    );
    *response_size = total_bytes_relayed;
    close(remote_fd);
    return 0;
}
