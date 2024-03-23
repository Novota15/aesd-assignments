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

volatile sig_atomic_t keep_running = 1;
const char *output_filename = "/var/tmp/aesdsocketdata";

// Improved signal handler function to gracefully finish execution
void signal_handler(int sig) {
    keep_running = 0;
}

int main(int argc, char *argv[]) {
    struct sockaddr_in server_address = {0};
    int server_socket_fd, client_socket_fd;
    struct sockaddr_storage their_addr; // Connector's address information
    socklen_t addrlen = sizeof(their_addr);
    char *buffer;
    int buffer_size = 1024 * 1024 * 4; // 4MB
    FILE *output_file;
    int opt = 1;
    int flags;

    // Check for the '-d' argument to daemonize
    if (argc > 1 && strcmp(argv[1], "-d") == 0) {
        pid_t pid = fork();

        if (pid < 0) return -1; // Fork failed
        if (pid > 0) exit(EXIT_SUCCESS); // Parent exits

        // Child (daemon) continues
        setsid(); // Start a new session
    }

    // Register improved signal handlers for SIGINT and SIGTERM
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Open or create the file where data will be stored
    output_file = fopen(output_filename, "a+");
    if (!output_file) {
        handle_error("Failed to open output file");
    }

    // Create a socket
    server_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket_fd == -1) {
        handle_error("socket creation failed");
    }

    // Allow the port to be reused immediately after the program exits
    if (setsockopt(server_socket_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        handle_error("setsockopt failed");
    }

    // Set socket to non-blocking mode
    flags = fcntl(server_socket_fd, F_GETFL);
    if (flags == -1 || fcntl(server_socket_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        handle_error("setting socket to non-blocking failed");
    }

    // Bind the socket to a port
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = INADDR_ANY; // Automatically fill with my IP
    server_address.sin_port = htons(9000); // Port number
    if (bind(server_socket_fd, (struct sockaddr *)&server_address, sizeof(server_address)) < 0) {
        handle_error("bind failed");
    }

    // Listen for connections
    if (listen(server_socket_fd, 3) < 0) {
        handle_error("listen failed");
    }

    // Allocate memory for the data buffer
    buffer = calloc(buffer_size, sizeof(char));
    if (buffer == NULL) {
        handle_error("memory allocation failed");
    }

    // Open syslog for logging
    openlog("aesdsocket", LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL1);
    syslog(LOG_NOTICE, "Program started by User %d", getuid());

    // Main loop
    while (keep_running) {
        printf("Waiting for connections...\n");
        fflush(stdout);

        // Accept a connection (non-blocking)
        client_socket_fd = accept(server_socket_fd, (struct sockaddr *)&their_addr, &addrlen);
        if (client_socket_fd < 0) {
            if (errno == EWOULDBLOCK) {
                // No connection attempts, wait and retry
                usleep(100000); // 0.1 seconds
                continue;
            } else if (!keep_running) {
                // Exit signal received
                break;
            }
            handle_error("accept failed");
        }

        // Log accepted connection
        syslog(LOG_INFO, "Accepted connection from %s", inet_ntoa(((struct sockaddr_in*)&their_addr)->sin_addr));

        // Initialize a temporary buffer for reading data.
        char temp_buffer[buffer_size];
        ssize_t num_read;

        while((num_read = read(client_socket_fd, temp_buffer, sizeof(temp_buffer) - 1)) > 0) {
            temp_buffer[num_read] = '\0'; // Null-terminate the received chunk.
            char *token = strtok(temp_buffer, "\n");

            while(token != NULL) {
                // Process each token/packet.
                fputs(token, output_file); // Append the packet to the file.
                fputc('\n', output_file); // Ensure newline is preserved.
                token = strtok(NULL, "\n"); // Move to the next packet.
            }
            fflush(output_file); // Flush after all packets in the buffer are processed.

            // Send the file's content back to the client here, outside the inner loop.
            rewind(output_file); // Reset to the beginning of the file.
            while((num_read = fread(buffer, 1, buffer_size - 1, output_file)) > 0) {
                send(client_socket_fd, buffer, num_read, 0); // Send file content.
            }
            rewind(output_file); // Reset for the next operation.
        }

        fclose(output_file); // Close the file to ensure data is flushed

        if(num_read == -1 && errno != EWOULDBLOCK) {
            perror("read");
            close(client_socket_fd);
            continue; // Move to next client or exit if signal received.
        }
        

        // Reopen the file for reading before sending its contents
        output_file = fopen(output_filename, "r");
        if (!output_file) {
            handle_error("Failed to open output file for reading");
        }

        // Send back the file's content to the client
        fseek(output_file, 0, SEEK_SET); // Ensure we're at the start of the file
        while((num_read = fread(buffer, 1, buffer_size - 1, output_file)) > 0) {
            send(client_socket_fd, buffer, num_read, 0);
        }

        fclose(output_file); // Close the file after sending its content

        // Reopen the file for appending for the next write operation
        output_file = fopen(output_filename, "a+");
        if (!output_file) {
            handle_error("Failed to reopen output file for appending");
        }

        // Close client socket and log closure
        close(client_socket_fd);
        syslog(LOG_INFO, "Closed connection from %s", inet_ntoa(((struct sockaddr_in*)&their_addr)->sin_addr));
    } // While keep_running loop ends here

    // Clean up before exiting
    printf("Freeing allocated memory and closing file descriptors\n");
    free(buffer);
    fclose(output_file);
    close(server_socket_fd);

    // Delete the file as part of graceful shutdown
    if(remove(output_filename) == 0) {
        printf("Deleted the file: %s\n", output_filename);
    } else {
        perror("Failed to delete the file");
    }

    // Closing syslog
    closelog();

    return 0; 
}