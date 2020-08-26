#include "server.h"

rocksdb::DB* db = NULL;
bool nosync = true;
int nprocs = 1;
int tcp_keepalive = 60;
uv_loop_t *loop = NULL;
bool inmem = false;
bool readonly = false;
const char *dir = "data";


static void get_peer_name(char *buf, size_t buf_len, uv_tcp_t *tcp){
	struct sockaddr_storage addr;
	int addr_len = sizeof(addr);
	int ofs = 0;

	uv_tcp_getpeername(tcp, (struct sockaddr*)&addr, &addr_len);
	switch (addr.ss_family){
	case AF_INET:
		uv_inet_ntop(AF_INET, &((struct sockaddr_in *)&addr)->sin_addr, buf+ofs, buf_len-ofs);
		ofs += strlen(buf);
		snprintf(buf+ofs, buf_len-ofs, ":%hu", ntohs(((struct sockaddr_in *)&addr)->sin_port));
		return;
	case AF_INET6:
		if (buf_len-ofs < 4){
			// Can't even fit in "[0]"
			break;
		}
		buf[ofs++] = '[';
		uv_inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&addr)->sin6_addr, buf+ofs, buf_len-ofs-1);
		ofs += strlen(buf);
		snprintf(buf+ofs, buf_len-ofs, "]:%hu", ntohs(((struct sockaddr_in6 *)&addr)->sin6_port));
		return;
	}
	snprintf(buf, buf_len, "[unknown]");
}

void get_buffer(uv_handle_t *handle, size_t size, uv_buf_t *buf){
	client *c = (client*)handle;
	if (c->buf_cap-c->buf_idx-c->buf_len < size){
		while (c->buf_cap-c->buf_idx-c->buf_len < size){
			if (c->buf_cap==0){
				c->buf_cap=1;
			}else{
				c->buf_cap*=2;
			}
		}
		c->buf = (char*)realloc(c->buf, c->buf_cap);
		if (!c->buf){
			err(1, "malloc");
		}
	}
	buf->base = c->buf+c->buf_idx+c->buf_len;
	buf->len = size;
}

void on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf){
	client *c = (client*)stream;
	bool keepalive = true;
	if (nread < 0) {
		if (nread != UV_EOF){
			log('.', "%s: error reading: %s", c->peer, uv_strerror(nread));
		}
		keepalive = false;
	}else if (nread > 0){
		c->buf_len += nread;
		keepalive = client_process_command(c);
	}

	if (!keepalive){
		client_close(c);
	}
}

int server_enable_reads(client *c) {
	return uv_read_start((uv_stream_t *)&c->tcp, get_buffer, on_read);
}

void on_accept(uv_stream_t *server, int status) {
	if (status < 0) {
		log('.', "error accepting connection: %s", uv_strerror(status));
		return;
	}

	client *c = client_new();
	c->server = server;

	uv_tcp_init(server->loop, &c->tcp);
	if ((status = uv_accept(c->server, (uv_stream_t *)&c->tcp)) < 0){
		log('.', "error accepting connection: %s", uv_strerror(status));
		client_free(c);
		return;
	}
	get_peer_name(c->peer, sizeof(c->peer), &c->tcp);

	if ((status = uv_tcp_keepalive(&c->tcp, tcp_keepalive > 0, tcp_keepalive)) < 0){
		log('.', "%s: error setting keepalive: %s", c->peer, uv_strerror(status));
		client_close(c);
		return;
	}

	log('.', "%s: accepted new connection", c->peer);
	if ((status = server_enable_reads(c)) < 0){
		log('.', "%s: error enabling reads: %s", c->peer, uv_strerror(status));
		client_close(c);
		return;
	}
}

void opendb(){
	rocksdb::Options options;
	options.create_if_missing = true;
	if (inmem){
		options.env = rocksdb::NewMemEnv(rocksdb::Env::Default());
	}

	rocksdb::Status s = readonly ? rocksdb::DB::OpenForReadOnly(options, dir, &db) : rocksdb::DB::Open(options, dir, &db);
	if (!s.ok()){
		err(1, "%s", s.ToString().c_str());
	}
}

void flushdb(){
	delete db;
	if (remove_directory(dir, false)){
		err(1, "remove_directory");
	}
	opendb();
}

int main(int argc, char **argv) {
	int tcp_port = 5555;
	bool tcp_port_provided = false;
	for (int i=1;i<argc;i++){
		if (strcmp(argv[i], "-h")==0||
			strcmp(argv[i], "--help")==0||
			strcmp(argv[i], "-?")==0){
			fprintf(stdout, "RocksDB version " ROCKSDB_VERSION ", Libuv version " LIBUV_VERSION ", Server version " SERVER_VERSION "\n");
			fprintf(stdout, "usage: %s [-d data_path] [-p tcp_port] [--sync] [--readonly] [--inmem] [--keepalive seconds]\n", argv[0]);
			return 0;
		}else if (strcmp(argv[i], "--version")==0){
			fprintf(stdout, "RocksDB version " ROCKSDB_VERSION ", Libuv version " LIBUV_VERSION ", Server version " SERVER_VERSION "\n");
			return 0;
		}else if (strcmp(argv[i], "-d")==0){
			if (i+1 == argc){
				fprintf(stderr, "argument missing after: \"%s\"\n", argv[i]);
				return 1;
			}
			dir = argv[++i];
		}else if (strcmp(argv[i], "--sync")==0){
			nosync = false;
		}else if (strcmp(argv[i], "--inmem")==0){
			inmem = true;
		}else if (strcmp(argv[i], "--readonly")==0){
			readonly = true;
		}else if (strcmp(argv[i], "-p")==0){
			if (i+1 == argc){
				fprintf(stderr, "argument missing after: \"%s\"\n", argv[i]);
				return 1;
			}
			tcp_port = atoi(argv[i+1]);
			if (!tcp_port){
				fprintf(stderr, "invalid option '%s' for argument: \"%s\"\n", argv[i+1], argv[i]);
				return 1;
			}
			i++;
			tcp_port_provided = true;
		}else if (strcmp(argv[i], "--keepalive")==0){
			if (i+1 == argc){
				fprintf(stderr, "argument missing after %s\n", argv[i]);
				return 1;
			}
			char *endp;
			long val;

			errno = 0;
			val = strtol(argv[i+1], &endp, 0);
			if (errno != 0 || *endp != '\0'){
				fprintf(stderr, "invalid option for %s: \"%s\" not a number\n", argv[i], argv[i+1]);
				return 1;

			}
			if (val < 0 || val > INT_MAX){
				fprintf(stderr, "invalid option for %s: %ld out of range\n", argv[i], val);
				return 1;
			}

			tcp_keepalive = (int)val;
			i++;
		}else{
			fprintf(stderr, "unknown option argument: \"%s\"\n", argv[i]);
			return 1;
		}
	}
	loop = uv_default_loop();
	log_init(loop, STDERR_FILENO);

	log('#', "Server started, RocksDB version " ROCKSDB_VERSION ", Libuv version " LIBUV_VERSION ", Server version " SERVER_VERSION);
	opendb();

	uv_tcp_t server;

	struct sockaddr_in addr;
	uv_ip4_addr("0.0.0.0", tcp_port, &addr);

	uv_tcp_init(loop, &server);
	uv_tcp_bind(&server, (const struct sockaddr*)&addr, 0);

	int r = uv_listen((uv_stream_t *)&server, -1, on_accept);
	if (r) {
		err(1, "uv_listen");
	}
	log('*', "The server is now ready to accept connections on port %d", tcp_port);
	return uv_run(loop, UV_RUN_DEFAULT);
}
