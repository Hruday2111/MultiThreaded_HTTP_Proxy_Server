#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: %s <port>\n", argv[0]);
        exit(1);
    }
    int port = atoi(argv[1]);

    // 1. Create a socket -- this is just asking the OS for a "phone line" handle
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
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

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(1);
    }

    // 3. Start listening -- 5 is how many pending connections can queue up
    if (listen(server_fd, 5) < 0) {
        perror("listen failed");
        exit(1);
    }

    printf("Listening on port %d... waiting for ONE connection\n", port);

    // 4. Accept a connection -- this line BLOCKS (freezes) until a client connects
    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) {
        perror("accept failed");
        exit(1);
    }
    printf("Client connected!\n");

    // 5. Read whatever the client sent us (their raw HTTP request text)
    char buffer[4096] = {0};
    int bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    printf("----- Received %d bytes -----\n%s\n-----------------------------\n", bytes_read, buffer);

    // 6. Send back a fixed, hardcoded HTTP response (not the real website's content yet)
    char *response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 13\r\n"
        "Connection: close\r\n"
        "\r\n"
        "Hello, world!";
    send(client_fd, response, strlen(response), 0);

    // 7. Clean up both sockets
    close(client_fd);
    close(server_fd);
    printf("Done. Connection closed.\n");
    return 0;
}