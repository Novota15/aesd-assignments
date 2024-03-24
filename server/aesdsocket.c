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
#include <pthread.h>

#define handle_error(msg) \
  do { perror(msg); exit(EXIT_FAILURE); } while (0)

volatile sig_atomic_t keep_running = 1;
const char *output_filename = "/var/tmp/aesdsocketdata";
pthread_mutex_t write_mutex = PTHREAD_MUTEX_INITIALIZER;


// Improved signal handler function to gracefully finish execution
void signal_handler(int sig) {
    keep_running = 0;
}

void* handle_connection(void* arg) {
    int client_socket_fd = *((int*)arg);
    free(arg); // Free the dynamically allocated memory for the file descriptor.

    char *buffer = calloc(1024 * 1024, sizeof(char)); // 1MB buffer.
    if (buffer == NULL) {
        handle_error("memory allocation failed");
    }

    FILE *output_file = fopen(output_filename, "a+");
    if (!output_file) {
        handle_error("Failed to open output file");
    }

    ssize_t num_read;
    while((num_read = read(client_socket_fd, buffer, 1024 * 1024 - 1)) > 0) {
        buffer[num_read] = '\0';

        pthread_mutex_lock(&write_mutex);
        fputs(buffer, output_file);
        fflush(output_file);
        pthread_mutex_unlock(&write_mutex);
    }

    fclose(output_file);

    close(client_socket_fd);
    free(buffer);
    return NULL;
}

int main(int argc, char *argv[]) {
    struct sockaddr_in server_address = {0};
    int server_socket_fd;
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

        int* client_socket_fd = malloc(sizeof(int));
        *client_socket_fd = accept(server_socket_fd, (struct sockaddr *)&their_addr, &addrlen);
        if (*client_socket_fd < 0) {
            free(client_socket_fd);
            if (errno == EWOULDBLOCK) {
                usleep(100000);
                continue;
            } else if (!keep_running) {
                break;
            }
            handle_error("accept failed");
        }

        pthread_t thread_id;
        if(pthread_create(&thread_id, NULL, handle_connection, client_socket_fd) != 0) {
            handle_error("Failed to create thread");
        }
        pthread_detach(thread_id); // Threads will clean up themselves.
    }

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