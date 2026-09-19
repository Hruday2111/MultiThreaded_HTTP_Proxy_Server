#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

struct request_target{
    char host[256];
    char path[2048];
};

int extract_request_target(const char *request, struct request_target *target);

#endif
