#include "http_parser.h"
#include <stdio.h>
#include <string.h>

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
