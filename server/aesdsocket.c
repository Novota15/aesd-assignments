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
const char *output_filename = "/var/tmp/aesdsocketdata";

// Mutexes and condition for thread synchronization
pthread_mutex_t list_mutex;
pthread_cond_t queue_condition;

// Signal handler to safely shut down the server
void finish() {
  keep_running = 0;
  syslog(LOG_NOTICE, "Caught signal, exiting");
}

// Task structure for handling client requests
typedef struct Task {
  pthread_mutex_t *writing_output_mutex;
  FILE *output_file;
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

// Function to process client requests
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
  pthread_mutex_lock(task->writing_output_mutex);

  // Append received data to the file
  fseek(task->output_file, 0, SEEK_END);
  char *tok = strtok(thread_buffer, "\n");
  while (tok != NULL) {
    fprintf(task->output_file, "%s\n", tok);
    tok = strtok(NULL, "\n");
  }

  // Determine the file size to reallocate buffer if necessary
  long file_size = ftell(task->output_file);

  if (file_size > thread_buffer_size) {
    thread_buffer_size = file_size * 2;
    thread_buffer = realloc(thread_buffer, thread_buffer_size);
  }

  // Read the entire file content to send it back to the client
  rewind(task->output_file);
  fread(thread_buffer, file_size, 1, task->output_file);
  thread_buffer[file_size] = 0;
  fseek(task->output_file, 0, SEEK_END);

  // Unlock the mutex after writing is done
  pthread_mutex_unlock(task->writing_output_mutex);

  // Send the updated file content to the client
  send(task->client_socket_fd, thread_buffer, file_size, 0);
  
  // Clean up
  close(task->client_socket_fd);
  free(thread_buffer);
}

// Thread function to handle connections
void *start_thread(void *args) {
  struct thread *th = (struct thread *)args;
  execute_task(&th->task);
  return NULL;
}

// Thread function for appending timestamp to the file every 10 seconds
void *timestamp_thread(void *args) {
  Task *task = (Task *)args;

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

    pthread_mutex_lock(task->writing_output_mutex);
    fprintf(task->output_file, "timestamp:%s\n", s);
    pthread_mutex_unlock(task->writing_output_mutex);
  }
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

  // Open or create the file where data will be stored, and immediately clear it
  FILE *output_file = fopen(output_filename, "w");
  if (!output_file) {
      handle_error("Failed to open output file");
  }
  // Close the file after clearing it
  fclose(output_file);
  
  // Reopen for append
  output_file = fopen(output_filename, "a+");

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
  pthread_t timestamp_thread_id;
  pthread_mutex_t writing_output_mutex;
  int counter = 0;

  pthread_mutex_init(&writing_output_mutex, NULL);
  pthread_mutex_init(&list_mutex, NULL);
  pthread_cond_init(&queue_condition, NULL);

  SLIST_INIT(&head);

  // Start the timestamp appending thread
  Task timestamp_task = {.writing_output_mutex = &writing_output_mutex,
                         .output_file = output_file};
  pthread_create(&timestamp_thread_id, NULL, timestamp_thread, &timestamp_task);

  struct thread *e = NULL;
  
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

    Task task = {.writing_output_mutex = &writing_output_mutex,
                 .client_socket_fd = client_socket_fd,
                 .output_file = output_file,
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
  pthread_join(timestamp_thread_id, NULL);

  // Clean up
  pthread_mutex_destroy(&list_mutex);
  pthread_mutex_destroy(&writing_output_mutex);
  fclose(output_file);
  close(server_socket_fd);
  unlink(output_filename);
  closelog();
  return 0;
}