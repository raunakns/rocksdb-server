#include "server.h"

client *client_new(){
	client *c = (client*)calloc(1, sizeof(client));
	if (!c){
		err(1, "malloc");
	}
	c->worker.data = c; // self reference
	return c;
}

void client_free(client *c){
	if (!c){
		return;
	}
	if (c->buf){
		free(c->buf);
	}
	if (c->args){
		free(c->args);
	}
	if (c->args_size){
		free(c->args_size);
	}
	if (c->output){
		free(c->output);
	}
	if (c->tmp_err){
		free(c->tmp_err);
	}
	free(c);
}

void on_close(uv_handle_t *stream){
	client *c = (client *)stream;
	log('.', "%s: closed connection", c->peer);
	client_free((client*)stream);
}

void client_close(client *c){
	uv_close((uv_handle_t *)&c->tcp, on_close);
}

inline void client_output_require(client *c, size_t siz){
	if (c->output_cap < siz){
		while (c->output_cap < siz){
			if (c->output_cap == 0){
				c->output_cap = 1;
			}else{
				c->output_cap *= 2;
			}
		}
		c->output = (char*)realloc(c->output, c->output_cap);
		if (!c->output){
			err(1, "malloc");
		}
	}
}

void client_write(client *c, const char *data, int n){
	client_output_require(c, c->output_len+n);
	memcpy(c->output+c->output_len, data, n);	
	c->output_len+=n;
}

void client_clear(client *c){
	c->output_len = 0;
	c->output_offset = 0;
}

void client_write_byte(client *c, char b){
	client_output_require(c, c->output_len+1);
	c->output[c->output_len++] = b;
}

void client_write_bulk(client *c, const char *data, int n){
	char h[32];
	sprintf(h, "$%d\r\n", n);
	client_write(c, h, strlen(h));
	client_write(c, data, n);
	client_write_byte(c, '\r');
	client_write_byte(c, '\n');
}

void client_write_multibulk(client *c, int n){
	char h[32];
	sprintf(h, "*%d\r\n", n);
	client_write(c, h, strlen(h));
}

void client_write_int(client *c, int n){
	char h[32];
	sprintf(h, ":%d\r\n", n);
	client_write(c, h, strlen(h));
}

void client_write_error(client *c, error err){
	client_write(c, "-ERR ", 5);
	client_write(c, err, strlen(err));
	client_write_byte(c, '\r');
	client_write_byte(c, '\n');
}


static void on_write_done(uv_write_t *req, int status){
	uv_stream_t *const stream = req->handle;
	client *const c = (client*)stream;
	bool keepalive = true;
	if (status < 0){
		// Error writing, such as closed remote end.
		keepalive = false;
	}else{
		// Finished writing response, at this point, reads are still
		// disabled. The client may have pipelined requests, so
		// process any remaining bytes in the input buffer. If there
		// are insufficient number, reads will be re-enabled.
		keepalive = client_process_command(c);
	}

	if (!keepalive){
		client_close(c);
	}
}

static void on_last_write_done(uv_write_t *req, int status){
	uv_stream_t *const stream = req->handle;
	client *const c = (client*)stream;
	client_close(c);
}

// Returns:
//   true - data was flushed
//   false - no data was flushed
static bool client_flush_impl(client *c, int offset, uv_write_cb cb){
	if (c->output_len-offset <= 0){
		return false;
	}
	uv_buf_t buf = uv_buf_init(c->output+offset, c->output_len-offset);
	c->output_offset = 0;
	c->output_len = 0;
	uv_write(&c->req, (uv_stream_t *)&c->tcp, &buf, 1, cb);
	return true;
}

void client_flush_offset(client *c, int offset){
	client_flush_impl(c, offset, &on_write_done);
}


void client_flush(client *c){
	client_flush_impl(c, c->output_offset, &on_write_done);
}

void client_err_alloc(client *c, int n){
	if (c->tmp_err){
		free(c->tmp_err);
	}
	c->tmp_err = (char*)malloc(n);
	if (!c->tmp_err){
		err(1, "malloc");
	}
	memset(c->tmp_err, 0, n);
}

error client_err_expected_got(client *c, char c1, char c2){
	client_err_alloc(c, 64);
	sprintf(c->tmp_err, "Protocol error: expected '%c', got '%c'", c1, c2);
	return c->tmp_err;
}

error client_err_unknown_command(client *c, const char *name, int count){
	client_err_alloc(c, count+64);
	c->tmp_err[0] = 0;
	strcat(c->tmp_err, "unknown command '");
	strncat(c->tmp_err, name, count);
	strcat(c->tmp_err, "'");
	return c->tmp_err;
}

void client_append_arg(client *c, const char *data, int nbyte){
	if (c->args_cap==c->args_len){
		if (c->args_cap==0){
			c->args_cap=1;
		}else{
			c->args_cap*=2;
		}
		c->args = (const char**)realloc(c->args, c->args_cap*sizeof(const char *));
		if (!c->args){
			err(1, "malloc");
		}
		c->args_size = (int*)realloc(c->args_size, c->args_cap*sizeof(int));
		if (!c->args_size){
			err(1, "malloc");
		}
	}
	c->args[c->args_len] = data;
	c->args_size[c->args_len] = nbyte;
	c->args_len++;
}

// Parse and consume a command formatted using the telnet protocol.
// Returns:
//   > 0: Consumed a whole command (stored in client)
//   < 0: Encountered fatal parsing error
//   = 0: Consumed part of a command (in client), but need more data
static int client_parse_telnet_command(client *c){
	size_t i = c->buf_idx;
	size_t z = c->buf_len+c->buf_idx;
	if (i >= z){
		return 0;
	}
	c->args_len = 0;
	size_t s = i;
	bool first = true;
	for (;i<z;i++){
		if (c->buf[i]=='\'' || c->buf[i]=='\"'){
			if (!first){
				client_write_error(c, "Protocol error: unbalanced quotes in request");
				return -1;
			}
			char b = c->buf[i];
			i++;
			s = i;
			for (;i<z;i++){
				if (c->buf[i] == b){
					if (i+1>=z||c->buf[i+1]==' '||c->buf[i+1]=='\r'||c->buf[i+1]=='\n'){
						client_append_arg(c, c->buf+s, i-s);
						i--;
					}else{
						client_write_error(c, "Protocol error: unbalanced quotes in request");
						return -1;
					}
					break;
				}
			}
			i++;
			continue;
		}
		if (c->buf[i] == '\n'){
			if (!first){
				size_t e;
				if (i>s && c->buf[i-1] == '\r'){
					e = i-1;
				}else{
					e = i;
				}
				client_append_arg(c, c->buf+s, e-s);
			}
			i++;
			c->buf_len -= i-c->buf_idx;
			if (c->buf_len == 0){
				c->buf_idx = 0;
			}else{
				c->buf_idx = i;
			}
			return 1;
		}
		if (c->buf[i] == ' '){
			if (!first){
				client_append_arg(c, c->buf+s, i-s);
				first = true;
			}
		}else{
			if (first){
				s = i;
				first = false;
			}
		}
	}
	return 0;
}

// Parse and consume a command in the bytes received.
// Returns:
//   > 0: Consumed a whole command (stored in client)
//   < 0: Encountered fatal parsing error
//   = 0: Consumed part of a command (in client), but need more data
static int client_parse_command(client *c){
	c->args_len = 0;
	size_t i = c->buf_idx;
	size_t z = c->buf_idx+c->buf_len;
	if (i >= z){
		return 0;
	}
	if (c->buf[i] != '*'){
		return client_parse_telnet_command(c);
	}
	i++;
	int args_len = 0;
	size_t s = i;
	for (;i < z;i++){
		if (c->buf[i]=='\n'){
			if (c->buf[i-1] !='\r'){
				client_write_error(c, "Protocol error: invalid multibulk length");
				return -1;
			}
			c->buf[i-1] = 0;
			args_len = atoi(c->buf+s);
			c->buf[i-1] = '\r';
			if (args_len <= 0){
				if (args_len < 0 || i-s != 2){
					client_write_error(c, "Protocol error: invalid multibulk length");
					return -1;
				}
			}
			i++;
			break;
		}
	}
	if (i >= z){
		return 0;
	}
	for (int j=0;j<args_len;j++){
		if (i >= z){
			return 0;
		}
		if (c->buf[i] != '$'){
			client_write_error(c, client_err_expected_got(c, '$', c->buf[i]));
			return -1;
		}
		i++;
		int nsiz = 0;
		size_t s = i;
		for (;i < z;i++){
			if (c->buf[i]=='\n'){
				if (c->buf[i-1] !='\r'){
					client_write_error(c, "Protocol error: invalid bulk length");
					return -1;
				}
				c->buf[i-1] = 0;
				nsiz = atoi(c->buf+s);
				c->buf[i-1] = '\r';
				if (nsiz <= 0){
					if (nsiz < 0 || i-s != 2){
						client_write_error(c, "Protocol error: invalid bulk length");
						return -1;
					}
				}
				i++;
				if (z-i < nsiz+2){
					return 0;
				}
				s = i;
				if (c->buf[s+nsiz] != '\r'){
					client_write_error(c, "Protocol error: invalid bulk data");
					return -1;
				}
				if (c->buf[s+nsiz+1] != '\n'){
					client_write_error(c, "Protocol error: invalid bulk data");
					return -1;
				}
				client_append_arg(c, c->buf+s, nsiz);
				i += nsiz+2;
				break;
			}
		}
	}
	c->buf_len -= i-c->buf_idx;
	if (c->buf_len == 0){
		c->buf_idx = 0;
	}else{
		c->buf_idx = i;
	}

	return 1;
}

bool client_process_command(client *c){
	int r = client_parse_command(c);
	bool keepalive = true;

	if (r > 0){
		// A dispatched command run in a worker pool. To prevent more
		// commands from being processed while this one is outstanding,
		// disable reads from the stream. It will be re-enabled when
		// the response is written back out.
		uv_read_stop((uv_stream_t *)&c->tcp);
		client_clear(c);
		exec_command(c);
	}else if (r == 0){
		// Need more bytes to finish parsing the command.
		if ((r = server_enable_reads(c)) < 0){
			log('.', "%s: error enabling reads: %s", c->peer, uv_strerror(r));
			keepalive = false;
		}
	}else{
		// Errors related to the structure of the message are hard (if not
		// impossible) to recover from, since message boundaries have been
		// lost. Stop reading so we don't spin our wheels consuming
		// data, flush any error messages written to the socket,
		// and mark the connection for closing (which will be closed
		// when write finishes).
		log('.', "%s: encountered malformed request", c->peer);
		uv_read_stop((uv_stream_t *)&c->tcp);
		keepalive = client_flush_impl(c, c->output_offset, &on_last_write_done);
	}
	return keepalive;
}

static void on_command(uv_work_t *req){
	// Execute the command (in a worker pool)
	client *c = (client *)req->data;
	const command_t command = c->on_command;
	c->on_command = NULL;
	return command(c);
}

static void on_command_done(uv_work_t *req, int status){
	// Called in the main loop when the command is done executing.
	// At this point, it's safe to flush any data queued up to write to
	// the client.
	client *c = (client *)req->data;
	if (status!=0){
		client_close(c);
	}else{
		client_flush(c);
	}
}

void client_dispatch_command(client *c, command_t command){
	c->on_command = command;
	if (inmem){
		// Everything is in memory, so there is no blocking I/O
		// to worry about; handle the request in the main loop.
		on_command(&c->worker);
		on_command_done(&c->worker, 0);
	}else{
		// Commands are executed in a worker pool to free the main loop
		// to process other connections.
		uv_loop_t *loop = c->tcp.loop;
		uv_queue_work(loop, &c->worker, on_command, on_command_done);
	}
}

void client_print_args(client *c){
	printf("args[%d]:", c->args_len);
	for (int i=0;i<c->args_len;i++){
		printf(" [");
		for (int j=0;j<c->args_size[i];j++){
			printf("%c", c->args[i][j]);
		}
		printf("]");
	}
	printf("\n");
}
