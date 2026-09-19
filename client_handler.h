#ifndef CLIENT_HANDLER_H
#define CLIENT_HANDLER_H

int init_client_handler(unsigned int max_clients);
void *handle_client(void *arg);

#endif
