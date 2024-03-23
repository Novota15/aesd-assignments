#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#define handle_error(msg) \
  do { perror(msg); exit(EXIT_FAILURE); } while (0)

int keep_running = 1;
const char *output_filename = "/var/tmp/aesdsocketdata";

// Signal handler function to gracefully finish execution
void finish() {
  printf("Finishing...\n");
  keep_running = 0;
  syslog(LOG_NOTICE, "Caught signal, exiting");
}

int main(void) {
  // Register signal handlers for SIGINT and SIGTERM
  signal(SIGINT, finish);
  signal(SIGTERM, finish);

  // Open or create the file where data will be stored
  FILE *output_file = fopen(output_filename, "a+");
  if (!output_file) {
    handle_error("Failed to open output file");
  }

  // Set up hints for getaddrinfo
  struct addrinfo hints;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC; // Allow IPv4 or IPv6
  hints.ai_socktype = SOCK_STREAM; // TCP stream sockets
  hints.ai_flags = AI_PASSIVE; // Use my IP address

  // Create a socket
  int server_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_socket_fd == -1) {
    handle_error("socket creation failed");
  }

  // Allow the port to be reused immediately after the program exits
  int opt = 1;
  if (setsockopt(server_socket_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT,
                 &opt, sizeof(opt))) {
    handle_error("setsockopt failed");
  }

  // Set socket to non-blocking mode
  int flags = fcntl(server_socket_fd, F_GETFL);
  if (flags == -1 || fcntl(server_socket_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
    handle_error("setting socket to non-blocking failed");
  }

  // Bind the socket to a port
  struct sockaddr_in server_address = {0};
  server_address.sin_family = AF_INET;
  server_address.sin_addr.s_addr = INADDR_ANY; // Automatically fill with my IP
  server_address.sin_port = htons(9000); // Port number
  if (bind(server_socket_fd, (struct sockaddr *)&server_address,
           sizeof(server_address)) < 0) {
    handle_error("bind failed");
  }

  // Listen for connections
  if (listen(server_socket_fd, 3) < 0) {
    handle_error("listen failed");
  }

  // Allocate memory for the data buffer
  int buffer_size = 1024 * 1024 * 4; // 4MB
  char *buffer = calloc(buffer_size, sizeof(char));
  if (buffer == NULL) {
    handle_error("memory allocation failed");
  }

  // Open syslog for logging
  openlog("ecen-5713", LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL1);
  syslog(LOG_NOTICE, "Program started by User %d", getuid());

  // Main loop
  while (keep_running) {
    printf("Waiting for connections...\n");
    fflush(stdout);

    struct sockaddr_storage their_addr; // Connector's address information
    socklen_t addrlen = sizeof(their_addr);

    // Accept a connection (non-blocking)
    int client_socket_fd = accept(server_socket_fd, (struct sockaddr *)&their_addr, &addrlen);
    if (client_socket_fd < 0) {
      if (errno == EWOULDBLOCK) {
        // No connection attempts, wait and retry
        usleep(500000); // 0.5 seconds
        continue;
      } else if (!keep_running) {
        // Exit signal received
        break;
      }
      handle_error("accept failed");
    }

    // Log accepted connection
    syslog(LOG_INFO, "Accepted connection from %s",
           inet_ntoa(((struct sockaddr_in *)&their_addr)->sin_addr));

    // Read data from client
    ssize_t valread = read(client_socket_fd, buffer, buffer_size - 1); // Leave space for null terminator
    printf("Read %ld bytes\n", valread);
    buffer[valread] = '\0'; // Ensure string is null-terminated

    // Write received data to file, one line at a time
    fseek(output_file, 0, SEEK_END);
    char *tok = strtok(buffer, "\n");
