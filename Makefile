ROCKSDB_VERSION=6.11.4
LIBUV_VERSION=1.38.1

.DELETE_ON_ERROR:

all: 3rdparty/rocksdb-$(ROCKSDB_VERSION)/librocksdb.a \
		3rdparty/libuv-$(LIBUV_VERSION)/build/lib/libuv.a
	@g++ -O2 -std=c++11 $(FLAGS) \
		-DROCKSDB_VERSION="\"$(ROCKSDB_VERSION)"\" \
		-DSERVER_VERSION="\"0.1.0"\" \
		-DLIBUV_VERSION="\"$(LIBUV_VERSION)"\" \
		-I3rdparty/rocksdb-$(ROCKSDB_VERSION)/include/ \
		-I3rdparty/libuv-$(LIBUV_VERSION)/build/include/ \
		-pthread -ldl -lbz2 -lz -lsnappy \
		-o rocksdb-server \
		src/server.cc src/client.cc src/exec.cc src/logging.cc src/match.cc src/util.cc \
		$^
clean:
	rm -f rocksdb-server
	rm -rf 3rdparty/libuv-*/
	rm -rf 3rdparty/rocksdb-*/
install: all
	cp rocksdb-server /usr/local/bin
uninstall: 
	rm -f /usr/local/bin/rocksdb-server

# libuv
libuv: 3rdparty/libuv-$(LIBUV_VERSION)/build/lib/libuv.a
3rdparty/libuv-$(LIBUV_VERSION).tar.gz:
	curl -sL https://github.com/libuv/libuv/archive/v$(LIBUV_VERSION).tar.gz -o $@
3rdparty/libuv-$(LIBUV_VERSION): 3rdparty/libuv-$(LIBUV_VERSION).tar.gz
	rm -rf $@
	tar xzf $< -C 3rdparty
3rdparty/libuv-$(LIBUV_VERSION)/build/lib/libuv.a: 3rdparty/libuv-$(LIBUV_VERSION)
	cd 3rdparty/libuv-$(LIBUV_VERSION) && sh autogen.sh
	mkdir -p 3rdparty/libuv-$(LIBUV_VERSION)/build
	cd 3rdparty/libuv-$(LIBUV_VERSION)/build && ../configure --prefix=$$(pwd)
	make -C 3rdparty/libuv-$(LIBUV_VERSION)/build install


# rocksdb
rocksdb: 3rdparty/rocksdb-$(ROCKSDB_VERSION)/librocksdb.a
3rdparty/rocksdb-$(ROCKSDB_VERSION)/librocksdb.a: 3rdparty/rocksdb-$(ROCKSDB_VERSION)
	DEBUG_LEVEL=0 make -C 3rdparty/rocksdb-$(ROCKSDB_VERSION) static_lib
3rdparty/rocksdb-$(ROCKSDB_VERSION): 3rdparty/rocksdb-$(ROCKSDB_VERSION).tar.gz
	rm -rf $@
	tar xzf $< -C 3rdparty
3rdparty/rocksdb-$(ROCKSDB_VERSION).tar.gz:
	curl -sL https://github.com/facebook/rocksdb/archive/v$(ROCKSDB_VERSION).tar.gz -o $@
