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
#include <time.h>

#define USE_AESD_CHAR_DEVICE 1
#if (USE_AESD_CHAR_DEVICE)
#define FILE_NAME	"/dev/aesdchar"
#else
#define FILE_NAME	"/var/tmp/aesdsocketdata"
#endif

#define handle_error(msg) \
  do {                    \
    perror(msg);          \
    exit(EXIT_FAILURE);   \
  } while (0)

int keep_running = 1;

// Mutexes and condition for thread synchronization
pthread_mutex_t file_mutex;
pthread_mutex_t list_mutex;

// Signal handler to safely shut down the server
void finish() {
  keep_running = 0;
  syslog(LOG_NOTICE, "Caught signal, exiting");
}

// Task structure for handling client requests
typedef struct Task {
  int client_socket_fd;
  int id;
} Task;

struct thread {
  pthread_t id;
  Task task;
  SLIST_ENTRY(thread) threads;
};

// Initialize the head for the thread list
SLIST_HEAD(head_s, thread) head;

#if (USE_AESD_CHAR_DEVICE)
// Function to process client requests for character device
void execute_task(Task *task) {
  int buffer_size = 1024;
  char *read_buffer = malloc(buffer_size);
  char *write_buffer = malloc(buffer_size);
  if (read_buffer == NULL || write_buffer == NULL) {
    handle_error("malloc");
  }

  int write_pos = 0;
  ssize_t valread;
  
  // Read data from client until we get a newline
  while ((valread = read(task->client_socket_fd, read_buffer, buffer_size - 1)) > 0) {
    // Check if we need to expand write buffer
    if (write_pos + valread >= buffer_size) {
      buffer_size *= 2;
      write_buffer = realloc(write_buffer, buffer_size);
      if (write_buffer == NULL) {
        handle_error("realloc");
      }
    }
    
    memcpy(write_buffer + write_pos, read_buffer, valread);
    write_pos += valread;
    
    // Check if we have a newline
    if (memchr(read_buffer, '\n', valread) != NULL) {
      break;
    }
  }

  if (valread < 0) {
    perror("read from socket");
    goto cleanup;
  }

  // Lock the mutex before accessing the device
  pthread_mutex_lock(&file_mutex);

  // Open the device, write data, then read it back
  int fd = open(FILE_NAME, O_RDWR);
  if (fd < 0) {
    perror("open device");
    pthread_mutex_unlock(&file_mutex);
    goto cleanup;
  }

  // Write the data to the device
  ssize_t written = write(fd, write_buffer, write_pos);
  if (written < 0) {
    perror("write to device");
    close(fd);
    pthread_mutex_unlock(&file_mutex);
    goto cleanup;
  }

  // Now read all data from the device to send back to client
  lseek(fd, 0, SEEK_SET);
  
  int total_read = 0;
  int send_buffer_size = 4096;
  char *send_buffer = malloc(send_buffer_size);
  if (send_buffer == NULL) {
    close(fd);
    pthread_mutex_unlock(&file_mutex);
    handle_error("malloc");
  }

  while (1) {
    if (total_read + 1024 >= send_buffer_size) {
      send_buffer_size *= 2;
      send_buffer = realloc(send_buffer, send_buffer_size);
      if (send_buffer == NULL) {
        close(fd);
        pthread_mutex_unlock(&file_mutex);
        handle_error("realloc");
      }
    }
    
    ssize_t bytes_read = read(fd, send_buffer + total_read, 1024);
    if (bytes_read <= 0) {
      break;
    }
    total_read += bytes_read;
  }

  close(fd);
  pthread_mutex_unlock(&file_mutex);

  // Send the data back to the client
  if (total_read > 0) {
    send(task->client_socket_fd, send_buffer, total_read, 0);
  }

  free(send_buffer);

cleanup:
  close(task->client_socket_fd);
  free(read_buffer);
  free(write_buffer);
}

#else
// Function to process client requests for file-based storage
void execute_task(Task *task) {
  int thread_buffer_size = 10 * 1024 * 1024;
  char *thread_buffer = calloc(thread_buffer_size, sizeof(char));
  if (thread_buffer == NULL) {
    handle_error("calloc");
  }

  // Read data from client
  ssize_t valread =
      read(task->client_socket_fd, thread_buffer, thread_buffer_size - 1);
  thread_buffer[valread] = 0;

  // Lock the mutex before writing to the shared output file
  pthread_mutex_lock(&file_mutex);

  FILE *output_file = fopen(FILE_NAME, "a+");
  if (!output_file) {
    pthread_mutex_unlock(&file_mutex);
    handle_error("Failed to open output file");
  }

  // Append received data to the file
  fseek(output_file, 0, SEEK_END);
  char *tok = strtok(thread_buffer, "\n");
  while (tok != NULL) {
    fprintf(output_file, "%s\n", tok);
    tok = strtok(NULL, "\n");
  }

  // Determine the file size to reallocate buffer if necessary
  long file_size = ftell(output_file);

  if (file_size > thread_buffer_size) {
    thread_buffer_size = file_size * 2;
    thread_buffer = realloc(thread_buffer, thread_buffer_size);
  }

  // Read the entire file content to send it back to the client
  rewind(output_file);
  fread(thread_buffer, file_size, 1, output_file);
  thread_buffer[file_size] = 0;

  fclose(output_file);
  
  // Unlock the mutex after writing is done
  pthread_mutex_unlock(&file_mutex);

  // Send the updated file content to the client
  send(task->client_socket_fd, thread_buffer, file_size, 0);
  
  // Clean up
  close(task->client_socket_fd);
  free(thread_buffer);
}

// Thread function for appending timestamp to the file every 10 seconds
void *timestamp_thread(void *args) {
  (void)args;  // unused

  while (keep_running) {
    for (int i = 0; i < 10 && keep_running == 1; i++) {
      sleep(1);
    }
    if (!keep_running) {
      break;
    }
    char s[100];
    time_t t = time(NULL);
    struct tm *tmp;
    tmp = localtime(&t);
    strftime(s, sizeof(s), "%Y-%m-%d %H:%M:%S", tmp);

    pthread_mutex_lock(&file_mutex);
    FILE *output_file = fopen(FILE_NAME, "a");
    if (output_file) {
      fprintf(output_file, "timestamp:%s\n", s);
      fclose(output_file);
    }
    pthread_mutex_unlock(&file_mutex);
  }
  return NULL;
}
#endif

// Thread function to handle connections
void *start_thread(void *args) {
  struct thread *th = (struct thread *)args;
  execute_task(&th->task);
  return NULL;
}

int main(int argc, char **argv) {
  int is_daemon = 0;

  // Checks command line args for the daemon mode flag
  for (int i = 1; i < argc; i++) {
    if (strncmp(argv[i], "-d", 2) == 0) {
      is_daemon = 1;
      break;
    }
  }

  // Set up signal handlers for graceful shutdown
  signal(SIGINT, finish);
  signal(SIGTERM, finish);

#if (!USE_AESD_CHAR_DEVICE)
  // Open or create the file where data will be stored, and immediately clear it
  FILE *output_file = fopen(FILE_NAME, "w");
  if (!output_file) {
      handle_error("Failed to open output file");
  }
  fclose(output_file);
#endif

  struct addrinfo hints;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;  // use my IP

  int server_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_socket_fd == -1) {
    handle_error("socket");
  }

  // Set socket options to reuse the address and port
  int opt = 1;
  if (setsockopt(server_socket_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT,
                 &opt, sizeof(opt))) {
    handle_error("setsockopt");
  }

  int flags = fcntl(server_socket_fd, F_GETFL);
  if (flags < 0) {
    handle_error("fcntl");
  }
  flags = fcntl(server_socket_fd, F_SETFL, flags | O_NONBLOCK);
  if (flags < 0) {
    handle_error("fcntl");
  }

  // Prepare server socket address structure
  struct sockaddr_in server_address;
  server_address.sin_family = AF_INET;
  server_address.sin_addr.s_addr = INADDR_ANY;
  server_address.sin_port = htons(9000);

  // Bind to the server address
  if (bind(server_socket_fd, (struct sockaddr *)&server_address,
           sizeof(server_address)) < 0) {
    handle_error("bind");
  }

  // Daemonize if requested
  if (is_daemon) {
    pid_t pid = fork();
    if (pid < 0) {
      perror("Fail to fork");
      exit(EXIT_FAILURE);
    }
    if (pid > 0) {
      // parent process
      exit(EXIT_SUCCESS);
    }

    if (setsid() < 0) {
      exit(EXIT_FAILURE);
    }

    signal(SIGCHLD, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    pid = fork();
    if (pid < 0) {
      perror("Fail to fork (second time)");
      exit(EXIT_FAILURE);
    }
    if (pid > 0) {
      // parent process
      exit(EXIT_SUCCESS);
    }
    umask(0);
    chdir("/");
  }

  if (listen(server_socket_fd, 3) < 0) {
    handle_error("listen");
  }

  openlog("aesdsocket", LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL1);
  syslog(LOG_NOTICE, "Program started by User %d", getuid());

  // Initialize thread management
  pthread_mutex_init(&file_mutex, NULL);
  pthread_mutex_init(&list_mutex, NULL);

  SLIST_INIT(&head);

#if (!USE_AESD_CHAR_DEVICE)
  // Start the timestamp appending thread only for file-based mode
  pthread_t timestamp_thread_id;
  pthread_create(&timestamp_thread_id, NULL, timestamp_thread, NULL);
#endif

  struct thread *e = NULL;
  int counter = 0;
  
  // Main server loop for accepting and handling connections
  while (keep_running) {
    int client_socket_fd;
    struct sockaddr_storage their_addr;  // connector's address information
    socklen_t addrlen = sizeof(their_addr);

    if ((client_socket_fd = accept(
             server_socket_fd, (struct sockaddr *)&their_addr, &addrlen)) < 0) {
      if (keep_running == 0) {
        break;
      } else if (errno == EWOULDBLOCK) {
        usleep(1000);
        continue;
      }
      handle_error("accept");
    }

    counter++;

    syslog(LOG_INFO, "Accepted connection from %s",
           inet_ntoa(((struct sockaddr_in *)&their_addr)->sin_addr));

    pthread_mutex_lock(&list_mutex);

    e = malloc(sizeof(struct thread));
    if (e == NULL) {
      handle_error("malloc");
    }

    Task task = {.client_socket_fd = client_socket_fd,
                 .id = counter};
    e->task = task;

    if (pthread_create(&e->id, NULL, start_thread, e) != 0) {
      handle_error("pthread_create");
    }
    SLIST_INSERT_HEAD(&head, e, threads);
    e = NULL;
    pthread_mutex_unlock(&list_mutex);
  }

  // Wait for all threads to complete
  SLIST_FOREACH(e, &head, threads) {
    if (pthread_join(e->id, NULL) != 0) {
      handle_error("pthread_join");
    }
  }
  
#if (!USE_AESD_CHAR_DEVICE)
  pthread_join(timestamp_thread_id, NULL);
#endif

  // Clean up
  pthread_mutex_destroy(&list_mutex);
  pthread_mutex_destroy(&file_mutex);
  close(server_socket_fd);
  
#if (!USE_AESD_CHAR_DEVICE)
  unlink(FILE_NAME);
#endif

  closelog();
  return 0;
}
