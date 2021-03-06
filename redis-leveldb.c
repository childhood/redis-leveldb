#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <time.h>
#include <fcntl.h>

#include <sys/socket.h>
#include <netinet/tcp.h> /* TCP_NODELAY */
#include <netinet/in.h>  /* inet_ntoa */
#include <arpa/inet.h>   /* inet_ntoa */

#include <ev.h>

#include <leveldb/c.h>

#define MAX_CONNECTIONS 1024
#define READ_BUFFER 8192

typedef struct rl_server {
  struct ev_loop* loop;
  int fd;
  ev_io connection_watcher;
  leveldb_options_t* options;
  leveldb_readoptions_t* read_options;
  leveldb_writeoptions_t* write_options;
  leveldb_t* db;

} rl_server;

typedef struct rl_connection {
  int fd;                      /* ro */
  struct sockaddr_in sockaddr; /* ro */
  socklen_t socklen;           /* ro */ 
  rl_server *server;          /* ro */
  char *ip;                    /* ro */
  unsigned open:1;             /* ro */

  int buffered_data;
  char read_buffer[READ_BUFFER];

  ev_io write_watcher;         /* private */
  ev_io read_watcher;          /* private */
  ev_timer timeout_watcher;    /* private */
  ev_timer goodbye_watcher;    /* private */
} rl_connection;

static void error(const char* s) {
  puts(s);
}

static size_t get_int(char** i) {
  char* b = *i;

  size_t val = 0, pos = 1;

  while(*b != '\r') {
    val *= pos;
    val += (*b++ - '0');
    pos *= 10;
  }

  b += 2;

  *i = b;

  return val;
}

static int get(rl_connection* c, char* b) {
  if(*b++ != '$') return -1;

  size_t size = get_int(&b);

  size_t out_size = 0;
  char* err = 0;

  char* out = leveldb_get(c->server->db, c->server->read_options,
                          b, size, &out_size, &err);

  if(err) {
    error(err);
    out = 0;
  }

  if(!out) {
    write(c->fd, "$-1\r\n", 5);
  } else {
    char buf[256];
    buf[0] = '$';
    int count = sprintf(buf + 1, "%ld", out_size);

    write(c->fd, buf, count + 1);
    write(c->fd, "\r\n", 2);
    write(c->fd, out, out_size);
    write(c->fd, "\r\n", 2);

    free(out);
  }

  return 1;
}

static void write_error(int fd, const char* msg) {
  write(fd, "-", 1);
  write(fd, msg, strlen(msg));
  write(fd, "\r\n", 2);
}

static void write_status(int fd, const char* msg) {
  write(fd, "+", 1);
  write(fd, msg, strlen(msg));
  write(fd, "\r\n", 2);
}

static int inc(rl_connection* c, char* b) {
  if(*b++ != '$') return -1;

  size_t size = get_int(&b);

  size_t out_size = 0;
  char* err = 0;

  char* out = leveldb_get(c->server->db, c->server->read_options,
                          b, size, &out_size, &err);

  if(err) {
    error(err);
    out = 0;
  }

  if(!out) out = strdup("0");

  int done = 0;

  for(int i = out_size - 1; i >= 0; i--) {
    switch(out[i]) {
      case '0':
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7':
      case '8':
        out[i]++;
        done = 1;
        break;
      case '9':
        out[i] = '0';
        break;
      default:
        write_error(c->fd, "bad key type");
        return 1;
    }

    if(done) break;
  }

  if(!done) {
    realloc(out, out_size + 1);
    memmove(out + 1, out, out_size);
    out[0] = '1';

    out_size++;
  }

  leveldb_put(c->server->db, c->server->write_options,
                          b, size,
                          out, out_size, &err);

  if(err) {
    free(out);
    write_error(c->fd, err);
    return 1;
  }

  write(c->fd, ":", 1);
  write(c->fd, out, out_size);
  write(c->fd, "\r\n", 2);

  free(out);

  return 1;
}

static int set(rl_connection* c, char* b) {
  if(*b++ != '$') return -1;

  size_t key_size = get_int(&b);
  char* key = b;

  b += key_size;
  b += 2;

  if(*b++ != '$') return -1;

  size_t val_size = get_int(&b);
  char* val = b;

  char* err = 0;

  key[key_size] = 0;
  val[val_size] = 0;

  leveldb_put(c->server->db, c->server->write_options,
                          key, key_size,
                          val, val_size, &err);

  if(err) error(err);

  write_status(c->fd, "OK");

  return 1;
}

static int handle(rl_connection* c) {
  char* b = c->read_buffer;

  if(*b++ != '*') return -1;

  size_t count = get_int(&b);

  switch(count) {
  case 2: {
    if(*b++ != '$') return -1;

    size_t size = get_int(&b);

    if(size == 3) {
      if(memcmp(b, "get", 3) == 0) {
        return get(c, b + 5);
      }
    } else if(size == 4) {
      if(memcmp(b, "incr", 4) == 0) {
        return inc(c, b + 6);
      }
    }

    write_error(c->fd, "unknown command");
    break;
  }

  case 3: {
    if(*b++ != '$') return -1;

    size_t size = get_int(&b);

    if(size == 3 && memcmp(b, "set", 3) == 0) {
      b += size;
      b += 2;

      return set(c, b);
    } else {
      write_error(c->fd, "unknown command");
    }
    break;
  }
  case 4:
    write_error(c->fd, "unknown command");
    break;
  }

  return 1;
}

static void 
on_readable(struct ev_loop *loop, ev_io *watcher, int revents)
{
  rl_connection *connection = watcher->data;
  size_t offset = connection->buffered_data;
  int left = READ_BUFFER - offset;
  char* recv_buffer = connection->read_buffer + offset;
  ssize_t recved;

  /* printf("on_readable\n"); */
  // TODO -- why is this broken?
  //assert(ev_is_active(&connection->timeout_watcher));
  assert(watcher == &connection->read_watcher);

  // No more buffer space.
  if(left == 0) return;

  if(EV_ERROR & revents) {
    error("on_readable() got error event, closing connection.");
    return;
  }

  recved = recv(connection->fd, recv_buffer, left, 0);

  if(recved == 0) {
    ev_io_stop(loop, &connection->read_watcher);
    close(connection->fd);
    return;
  }

  if(recved <= 0) return;

  recv_buffer[recved] = 0;

  connection->buffered_data += recved;

  int ret = handle(connection);
  switch(ret) {
  case -1:
    error("bad protocol error");
    // fallthrough
  case 1:
    connection->buffered_data = 0;
    break;
  case 0:
    // more data needed, leave the buffer.
    break;
  default:
    error("unknown return error");
    break;
  }

  /* rl_connection_reset_timeout(connection); */

  return;

/* error: */
  /* rl_connection_schedule_close(connection); */
}

rl_connection* new_connection(rl_server* s, int fd) {
  rl_connection* connection = malloc(sizeof(rl_connection));

  connection->fd = fd;
  connection->server = s;
  connection->ip = NULL;
  connection->open = 0;

  /* ev_init(&connection->write_watcher, on_writable); */
  /* connection->write_watcher.data = connection; */

  ev_init(&connection->read_watcher, on_readable);
  connection->read_watcher.data = connection;

  /* ev_timer_init(&connection->goodbye_watcher, on_goodbye, 0., 0.); */
  /* connection->goodbye_watcher.data = connection;   */

  /* ev_timer_init(&connection->timeout_watcher, on_timeout, 0., EBB_DEFAULT_TIMEOUT); */
  connection->timeout_watcher.data = connection;

  return connection;
}

int server_listen_on_fd(rl_server* s, int fd);

int make_server(rl_server* s, const int port) {
  int fd = -1;
  struct linger ling = {0, 0};
  struct sockaddr_in addr;
  int flags = 1;
  
  if((fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
    perror("socket()");
    goto error;
  }
  
  flags = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags));
  setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (void *)&flags, sizeof(flags));
  setsockopt(fd, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling));

  /* XXX: Sending single byte chunks in a response body? Perhaps there is a
   * need to enable the Nagel algorithm dynamically. For now disabling.
   */
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (void *)&flags, sizeof(flags));
  
  /* the memset call clears nonstandard fields in some impementations that
   * otherwise mess things up.
   */
  memset(&addr, 0, sizeof(addr));
  
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  
  if(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("bind()");
    goto error;
  }
  
  return server_listen_on_fd(s, fd);

error:
  if(fd > 0) close(fd);
  return -1;
}

static void set_nonblock (int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  int r = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  assert(0 <= r && "Setting socket non-block failed!");
}

int server_listen_on_fd(rl_server* s, int fd) {
  s->fd = fd;

  if(listen(fd, MAX_CONNECTIONS) < 0) {
    perror("listen()");
    return -1;
  }
  
  set_nonblock(fd);
  
  ev_io_set(&s->connection_watcher, s->fd, EV_READ);
  ev_io_start(s->loop, &s->connection_watcher);
  
  return s->fd;
}

static void 
on_connection(struct ev_loop *loop, ev_io *watcher, int revents) {
  rl_server* s = watcher->data;

  //printf("on connection!\n");

  if(EV_ERROR & revents) {
    error("on_connection() got error event, closing server.");
    return;
  }
  
  struct sockaddr_in addr; // connector's address information
  socklen_t addr_len = sizeof(addr); 
  int fd = accept(s->fd, (struct sockaddr*)&addr, &addr_len);

  if(fd < 0) {
    perror("accept()");
    return;
  }

  rl_connection *connection = new_connection(s, fd);

  if(connection == NULL) {
    close(fd);
    return;
  } 
  
  set_nonblock(fd);

  connection->fd = fd;
  connection->open = 1;
  connection->server = s;
  memcpy(&connection->sockaddr, &addr, addr_len);
  connection->ip = inet_ntoa(connection->sockaddr.sin_addr);  

  ev_io_set(&connection->write_watcher, connection->fd, EV_WRITE);
  ev_io_set(&connection->read_watcher, connection->fd, EV_READ);

  /* ev_timer_again(loop, &connection->timeout_watcher); */

  ev_io_start(s->loop, &connection->read_watcher);
}

int main(int argc, char** argv) {
  rl_server s;

  s.options = leveldb_options_create();
  leveldb_options_set_create_if_missing(s.options, 1);

  s.read_options = leveldb_readoptions_create();
  s.write_options = leveldb_writeoptions_create();

  char* err = 0;

  s.db = leveldb_open(s.options, "redis.db", &err);
  if(err) {
    puts(err);
    return 1;
  }

  s.loop = ev_default_loop(0);
  s.connection_watcher.data = &s;

  ev_init(&s.connection_watcher, on_connection);

  make_server(&s, 8323);

  ev_run(s.loop, 0);

  return 0;
}
