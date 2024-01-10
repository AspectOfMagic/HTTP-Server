#include "connection.h"
#include "debug.h"
#include "helper_funcs.h"
#include "queue.h"
#include "request.h"
#include "response.h"
#include "rwlock.h"

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct uri_rwlock {
  char *uri;
  rwlock_t *lock;
  int threads_in_use;
};

typedef struct uri_rwlock uri_rwlock_t;

void *worker_thread(void *args);
uri_rwlock_t **uri_rwlocks_create(void);
void uri_rwlocks_destroy(uri_rwlock_t ***rwlocks);
uri_rwlock_t *find_uri_lock(char *uri);
uri_rwlock_t *get_uri_lock(char *uri);
void handle_connection(int connfd);
void handle_get(conn_t *conn);
void handle_put(conn_t *conn);
void handle_unsupported(conn_t *);

int num_workers = 4;
queue_t *client_queue;
uri_rwlock_t **uri_locks;
pthread_mutex_t access_uri_locks;

int main(int argc, char **argv) {
  if (argc < 2) {
    warnx("wrong arguments: %s port_num", argv[0]);
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    return EXIT_FAILURE;
  }
  int rc;
  int opt_val;
  char *endptr_1 = NULL;
  char *endptr_2 = NULL;
  while ((opt_val = getopt(argc, argv, "t:")) != -1) {
    if (opt_val == 't') {
      num_workers = (int)strtoull(optarg, &endptr_1, 10);
    }
  }
  if (num_workers <= 0 || (endptr_1 && *endptr_1 != '\0')) {
    warnx("Invalid number of worker threads: %s", optarg);
    return EXIT_FAILURE;
  }
  if (optind == argc) {
    warnx("port number argument not found");
    return EXIT_FAILURE;
  }
  size_t port = (size_t)strtoull(argv[optind], &endptr_2, 10);
  if (endptr_2 && *endptr_2 != '\0') {
    warnx("invalid port number: %s", argv[optind]);
    return EXIT_FAILURE;
  }
  rc = pthread_mutex_init(&access_uri_locks, NULL);
  assert(!rc);
  pthread_t threads[num_workers];
  client_queue = queue_new(num_workers);
  uri_locks = uri_rwlocks_create();
  for (int i = 0; i < num_workers; i++) {
    pthread_create(threads + i, NULL, worker_thread, NULL);
  }
  signal(SIGPIPE, SIG_IGN);
  Listener_Socket sock;
  listener_init(&sock, port);
  while (1) {
    intptr_t connfd = listener_accept(&sock);
    if (connfd < 0) {
      continue;
    }
    queue_push(client_queue, (void *)connfd);
  }
  for (int i = 0; i < num_workers; i++) {
    queue_push(client_queue, (void *)-1);
  }
  for (int i = 0; i < num_workers; i++) {
    pthread_join(threads[i], NULL);
  }
  queue_delete(&client_queue);
  uri_rwlocks_destroy(&uri_locks);
  rc = pthread_mutex_destroy(&access_uri_locks);
  assert(!rc);
  return EXIT_SUCCESS;
}

void *worker_thread(void *args) {
  intptr_t connfd;
  while (1) {
    queue_pop(client_queue, (void **)&connfd);
    if (connfd < 0) {
      return NULL;
    }
    handle_connection(connfd);
    close(connfd);
  }
  (void)args;
}

uri_rwlock_t **uri_rwlocks_create(void) {
  uri_rwlock_t **rwlocks =
      (uri_rwlock_t **)calloc(num_workers, sizeof(uri_rwlock_t *));
  for (int i = 0; i < num_workers; i++) {
    rwlocks[i] = (uri_rwlock_t *)calloc(1, sizeof(uri_rwlock_t));
    rwlocks[i]->uri = (char *)calloc(65, sizeof(char));
    rwlocks[i]->lock = rwlock_new(N_WAY, 3);
    rwlocks[i]->threads_in_use = 0;
  }
  return rwlocks;
}

void uri_rwlocks_destroy(uri_rwlock_t ***rwlocks) {
  for (int i = 0; i < num_workers; i++) {
    free((*rwlocks[i])->uri);
    rwlock_delete(&((*rwlocks[i])->lock));
    free(*rwlocks[i]);
  }
  free(*rwlocks);
  *rwlocks = NULL;
}

uri_rwlock_t *find_uri_lock(char *uri) {
  for (int i = 0; i < num_workers; i++) {
    if (strcmp(uri_locks[i]->uri, uri) == 0) {
      uri_locks[i]->threads_in_use++;
      return uri_locks[i];
    }
  }
  return NULL;
}

uri_rwlock_t *get_uri_lock(char *uri) {
  for (int i = 0; i < num_workers; i++) {
    if (uri_locks[i]->threads_in_use == 0) {
      strcpy(uri_locks[i]->uri, uri);
      uri_locks[i]->threads_in_use++;
      return uri_locks[i];
    }
  }
  return NULL;
}

void handle_connection(int connfd) {
  conn_t *conn = conn_new(connfd);
  const Response_t *res = conn_parse(conn);
  if (res != NULL) {
    conn_send_response(conn, res);
  } else {
    // debug("%s", conn_str(conn));
    const Request_t *req = conn_get_request(conn);
    if (req == &REQUEST_GET) {
      handle_get(conn);
    } else if (req == &REQUEST_PUT) {
      handle_put(conn);
    } else {
      handle_unsupported(conn);
    }
  }
  conn_delete(&conn);
}

void handle_get(conn_t *conn) {
  char *uri = conn_get_uri(conn);
  // debug("GET request not implemented. But, we want to get %s", uri);
  char *req_id = conn_get_header(conn, "Request-Id");
  if (req_id == NULL) {
    req_id = "0";
  }
  const Response_t *res = NULL;
  struct stat sb;
  int count;
  pthread_mutex_lock(&access_uri_locks);
  uri_rwlock_t *uri_lock = find_uri_lock(uri);
  if (uri_lock == NULL) {
    uri_lock = get_uri_lock(uri);
  }
  pthread_mutex_unlock(&access_uri_locks);
  reader_lock(uri_lock->lock);
  int fd = open(uri, O_RDONLY);
  if (fd < 0) {
    // debug("%s: %d", uri, errno);
    if (errno == ENOENT) {
      res = &RESPONSE_NOT_FOUND;
      goto out;
    } else if (errno == EACCES) {
      res = &RESPONSE_FORBIDDEN;
      goto out;
    } else {
      res = &RESPONSE_INTERNAL_SERVER_ERROR;
      goto out;
    }
  }
  int rc = fstat(fd, &sb);
  if (rc < 0) {
    res = &RESPONSE_INTERNAL_SERVER_ERROR;
    close(fd);
    goto out;
  } else if (sb.st_mode == S_IFDIR) {
    res = &RESPONSE_BAD_REQUEST;
    close(fd);
    goto out;
  }
  char *max_length = conn_get_header(conn, "Content-Length");
  if (max_length == NULL || sb.st_size <= atoi(max_length)) {
    count = sb.st_size;
  } else {
    count = atoi(max_length);
  }
  res = conn_send_file(conn, fd, count);
  if (res == NULL) {
    res = &RESPONSE_OK;
    fprintf(stderr, "GET,%s,%hu,%s\n", uri, response_get_code(res), req_id);
  } else {
    res = &RESPONSE_INTERNAL_SERVER_ERROR;
  }
  close(fd);
out:
  if (res != &RESPONSE_OK) {
    conn_send_response(conn, res);
    fprintf(stderr, "GET,%s,%hu,%s\n", uri, response_get_code(res), req_id);
  }
  pthread_mutex_lock(&access_uri_locks);
  uri_lock->threads_in_use--;
  pthread_mutex_unlock(&access_uri_locks);
  reader_unlock(uri_lock->lock);
}

void handle_unsupported(conn_t *conn) {
  // debug("handling unsupported request");
  //  send responses
  conn_send_response(conn, &RESPONSE_NOT_IMPLEMENTED);
}

void handle_put(conn_t *conn) {
  char *uri = conn_get_uri(conn);
  char *req_id = conn_get_header(conn, "Request-Id");
  if (req_id == NULL) {
    req_id = "0";
  }
  const Response_t *res = NULL;
  // debug("handling put request for %s", uri);
  //  Check if file already exists before opening it.
  pthread_mutex_lock(&access_uri_locks);
  uri_rwlock_t *uri_lock = find_uri_lock(uri);
  if (uri_lock == NULL) {
    uri_lock = get_uri_lock(uri);
  }
  pthread_mutex_unlock(&access_uri_locks);
  writer_lock(uri_lock->lock);
  bool existed = access(uri, F_OK) == 0;
  // debug("%s existed? %d", uri, existed);
  //  Open the file..
  int fd = open(uri, O_CREAT | O_TRUNC | O_WRONLY, 0600);
  if (fd < 0) {
    // debug("%s: %d", uri, errno);
    if (errno == EACCES || errno == EISDIR || errno == ENOENT) {
      res = &RESPONSE_FORBIDDEN;
      goto out;
    } else {
      res = &RESPONSE_INTERNAL_SERVER_ERROR;
      goto out;
    }
  }
  res = conn_recv_file(conn, fd);
  if (res == NULL && existed) {
    res = &RESPONSE_OK;
  } else if (res == NULL && !existed) {
    res = &RESPONSE_CREATED;
  } else {
    res = &RESPONSE_INTERNAL_SERVER_ERROR;
  }
  close(fd);
out:
  conn_send_response(conn, res);
  fprintf(stderr, "PUT,%s,%hu,%s\n", uri, response_get_code(res), req_id);
  pthread_mutex_lock(&access_uri_locks);
  uri_lock->threads_in_use--;
  pthread_mutex_unlock(&access_uri_locks);
  writer_unlock(uri_lock->lock);
}
