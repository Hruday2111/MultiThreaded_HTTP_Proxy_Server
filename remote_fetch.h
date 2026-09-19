#ifndef REMOTE_FETCH_H
#define REMOTE_FETCH_H

#include <stddef.h>

int fetch_remote_response(
    const char *host,
    const char *path,
    int client_fd,
    char **response_data,
    size_t *response_size
);

#endif
