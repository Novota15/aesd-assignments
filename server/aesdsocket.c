#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#define handle_error(msg) \
  do {                    \
    perror(msg);          \
    exit(EXIT_FAILURE);   \
  } while (0)

// Global flag to keep track of the application status
volatile int keep_running = 1;
const char *output_filename = "/var/tmp/aesdsocketdata";

// Mutex for task list and condition variable for queue management
pthread_mutex_t list_mutex;
pthread_cond_t queue_condition;

// Signal handler to gracefully shut down the server
void finish() {
  keep_running = 0;
  syslog(LOG_NOTICE, "Caught signal, exiting");
}

// Task structure to pass to worker threads
typedef struct Task {
  pthread_mutex_t *writing_output_mutex;
  FILE *output_file;
  int client_socket_fd;
  int id;
} Task;

// Thread structure for managing worker threads in a singly linked list
struct thread {
  pthread_t id;
  Task task;
  SLIST_ENTRY(thread) threads;
};

// Define the head for the singly linked list of threads
SLIST_HEAD(head_s, thread) head;

// Function to execute the task assigned to a worker thread
void execute_task(Task *task) {
  // Allocate a buffer for reading from the socket
  int thread_buffer_size = 10 * 1024 * 1024; // 10MB
  char *thread_buffer = calloc(thread_buffer_size, sizeof(char));
  if (thread_buffer == NULL) {
    handle_error("calloc");
  }

  // Read from the client socket
  ssize_t valread = read(task->client_socket_fd, thread_buffer, thread_buffer_size - 1);
  thread_buffer[valread] = '\0'; // Null-terminate the received data

  // Lock the mutex before writing to the shared output file
  pthread_mutex_lock(task->writing_output_mutex);

  // Move to the end of the file and write the received data
  fseek(task->output_file, 0, SEEK_END);
  char *tok = strtok(thread_buffer, "\n");
  while (tok != NULL) {
    fprintf(task->output_file, "%s\n", tok);
    tok = strtok(NULL, "\n");
  }

  // Reallocate the buffer if the file size is greater than the buffer size
  long file_size = ftell(task->output_file);
  if (file_size > thread_buffer_size) {
    thread_buffer_size = file_size * 2;
    thread_buffer = realloc(thread_buffer, thread_buffer_size);
  }

  // Read the entire file content into the buffer
  rewind(task->output_file);
  fread(thread_buffer, file_size, 1, task->output_file);
  thread_buffer[file_size] = '\0';
  fseek(task->output_file, 0, SEEK_END);

  // Unlock the mutex after writing
  pthread_mutex_unlock(task->writing_output_mutex);

  // Send the file content back to the client and close the socket
  send(task->client_socket_fd, thread_buffer, file_size, 0);
  close(task->client_socket_fd);

  // Clean up allocated resources
  free(thread_buffer);
}

// Worker thread starting function
void *start_thread(void *args) {
  struct thread *th = (struct thread *)args;
  execute_task(&th->task);
  return NULL;
}

// Timestamp thread function to add timestamps to the file every 10 seconds
void *timestamp_thread(void *args) {
  Task *task = (Task *)args;

  while (keep_running) {
    // Wait for 10 seconds
    for (int i = 0; i < 10 && keep_running == 1; i++) {
      sleep(1);
    }
    if (!keep_running) {
      break;
    }

    // Generate a timestamp string
    char s[100];
    time_t t = time(NULL);
    struct tm *tmp = localtime(&t);
    strftime(s, sizeof(s), "%Y-%m-%d %H:%M:%S", tmp);

    // Lock the mutex and write the timestamp to the file
    pthread_mutex_lock(task->writing_output_mutex);
    fprintf(task->output_file, "timestamp:%s\n", s);
    pthread_mutex_unlock(task->writing_output_mutex);
  }
  return NULL;
}

int main(int argc, char **argv) {
  int is_daemon = 0;
  for (int i = 1; i < argc; i++) {
    if (strncmp(argv[i], "-d", 2) == 0) {
      is_daemon = 1;
      break;
    }
  }

  // Setup signal handling for graceful shutdown
  signal(SIGINT, finish);
  signal(SIGTERM, finish);

  // Open or create the file where data will be appended
  FILE *output_file = fopen(output_filename, "a+");
  if (!output_file) {
    handle_error("Failed to open output file");
  }

  // Initialize server socket
  int server_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_socket_fd == -1) {
    handle_error("socket creation failed");
  }

  // Allow immediate reuse of the port
  int opt = 1;
  if (setsockopt(server_socket_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
    handle_error("setsockopt failed");
  }

  // Setup non-blocking mode for the socket
  int flags = fcntl(server_socket_fd, F_GETFL);
  if (flags == -1 || fcntl(server_socket_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
    handle_error("setting socket to non-blocking mode failed");
  }

  // Bind socket to port 9000 on any interface
  struct sockaddr_in server_address = {0};
  server_address.sin_family = AF_INET;
  server_address.sin_addr.s_addr = INADDR_ANY;
  server_address.sin_port = htons(9000);
  if (bind(server_socket_fd, (struct sockaddr *)&server_address, sizeof(server_address)) < 0) {
    handle_error("bind failed");
  }

  // Daemonize if required
  if (is_daemon) {
    daemonize(); // You should implement this function to handle the daemonization process as described in the previous code block
  }

  // Listen for incoming connections
  if (listen(server_socket_fd, 3) < 0) {
    handle_error("listen failed");
  }

  // Initialize logging
  openlog("ecen-5713", LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL1);
  syslog(LOG_NOTICE, "Program started by User %d", getuid());

  // Initialize the mutex used for file writing
  pthread_mutex_t writing_output_mutex;
  pthread_mutex_init(&writing_output_mutex, NULL);

  // Start the timestamping thread
  pthread_t timestamp_thread_id;
  Task timestamp_task = {.writing_output_mutex = &writing_output_mutex, .output_file = output_file};
  pthread_create(&timestamp_thread_id, NULL, timestamp_thread, &timestamp_task);

  // Main loop to accept connections and dispatch worker threads
  while (keep_running) {
    struct sockaddr_storage their_addr;
    socklen_t addrlen = sizeof(their_addr);
    int client_socket_fd = accept(server_socket_fd, (struct sockaddr *)&their_addr, &addrlen);

    if (client_socket_fd < 0) {
      if (errno == EWOULDBLOCK) {
        sleep(1); // No connection, sleep for a bit
        continue;
      } else if (!keep_running) {
        break; // Exit signal received
      } else {
        handle_error("accept failed");
      }
    }

    // Log the accepted connection
    syslog(LOG_INFO, "Accepted connection from %s", inet_ntoa(((struct sockaddr_in *)&their_addr)->sin_addr));

    // Create and dispatch a worker thread for the new connection
    pthread_t worker_thread_id;
    Task *new_task = malloc(sizeof(Task));
    *new_task = (Task){.client_socket_fd = client_socket_fd, .writing_output_mutex = &writing_output_mutex, .output_file = output_file};
    pthread_create(&worker_thread_id, NULL, handle_connection, new_task);
    pthread_detach(worker_thread_id); // The thread cleans up after itself
  }

  // Cleanup before exiting
  pthread_join(timestamp_thread_id, NULL); // Ensure the timestamp thread finishes
  pthread_mutex_destroy(&writing_output_mutex); // Clean up mutex
  fclose(output_file);
  close(server_socket_fd);
  unlink(output_filename); // Remove the output file
  closelog(); // Close the log
  return 0;
}