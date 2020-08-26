#include <err.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <uv.h>
#include <unistd.h>


#define MAX_MESSAGE_SIZE 512

typedef struct log_event log_event_t;
struct log_event {
	uv_write_t req;                 // Must be first
	struct timespec timestamp;
	log_event_t *next;
	char timestamp_buf[25];         // YYYY-MM-DDTHH:MM:SS.sss
	char cc_buf[12];
	char msg_buf[MAX_MESSAGE_SIZE];
};

typedef struct logger logger_t;
struct logger{
	uv_async_t async;     // Async operation to format events ond flush to stream
	uv_pipe_t pipe;       // Stream to write logs to
	log_event_t *head;    // Head of the linked list of log entries.
	log_event_t **next;   // The pointer to update with the next log event
	bool f_color;
};


static logger_t root_logger;

static log_event_t *log_event_alloc(void){
	log_event_t *result = (log_event_t*)calloc(1, sizeof(*result));
	if (result == NULL) {
		err(1, "Memory allocation failed!");
	}
	return result;
}

static void log_event_free(log_event_t *event){
	memset(event, 0, sizeof(*event));
	free(event);
}

static void logger_format_event(logger_t *logger, log_event_t *event){
	const struct timespec *timestamp = &event->timestamp;
	struct tm tm_info;
	int r;

	localtime_r(&timestamp->tv_sec, &tm_info) ;
	r = strftime(event->timestamp_buf, sizeof(event->timestamp_buf), "%Y-%m-%dT%H:%M:%S", &tm_info);
	snprintf(event->timestamp_buf+r, sizeof(event->timestamp_buf)-r, ".%03d", (int)(timestamp->tv_nsec/1000000)%1000);

	if (logger->f_color){
		switch (event->cc_buf[0]) {
		case '.': strncpy(event->cc_buf, "\x1b[35m.\x1b[0m", sizeof(event->cc_buf)); break;
		case '*': strncpy(event->cc_buf, "\x1b[1m*\x1b[0m", sizeof(event->cc_buf)); break;
		case '#': strncpy(event->cc_buf, "\x1b[33m#\x1b[0m", sizeof(event->cc_buf)); break;
		}
		// strncpy might not leave cc_buf NUL terminated.
		event->cc_buf[sizeof(event->cc_buf)-1] = '\0';
	}
}

static void on_write_log_done(uv_write_t *req, int status){
	log_event_t *event = (log_event_t *)req;
	log_event_free(event);
}

static void logger_flush_entry(logger_t *logger, log_event_t *event){
	static char space[] = { ' ' };
	static char eol[] = { '\n' };

	logger_format_event(logger, event);
	uv_buf_t bufs[] = {
		uv_buf_init(event->timestamp_buf, strlen(event->timestamp_buf)),
		uv_buf_init(space, sizeof(space)),
		uv_buf_init(event->cc_buf, strlen(event->cc_buf)),
		uv_buf_init(space, sizeof(space)),
		uv_buf_init(event->msg_buf, strlen(event->msg_buf)),
		uv_buf_init(eol, sizeof(eol)),
	};
	if (uv_write(&event->req, (uv_stream_t*)&logger->pipe, bufs, sizeof(bufs)/sizeof(*bufs), &on_write_log_done) != 0){
		log_event_free(event);
	}
}

static void logger_flush(uv_async_t *handle){
	logger_t *logger = (logger_t *)handle->data;

	// Flush all entries from the linked list without locking.
	// This is equivalent to:
	//     entries = logger->head;
	//     logger->head = NULL;
	//     logger->tail = &logger->head
	log_event *head = __atomic_exchange_n(&logger->head, NULL, __ATOMIC_ACQ_REL);
	__atomic_store_n(&logger->next, &logger->head, __ATOMIC_RELEASE);

	for (log_event_t *e = head; e != NULL; e = e->next){
		logger_flush_entry(logger, e);
	}
}


int log_init(uv_loop_t *loop, int fd)
{
	logger_t *logger = &root_logger;
	int r;

	if ((r = uv_pipe_init(loop, &logger->pipe, 0)) != 0){
		return r;
	}
	if ((r = uv_pipe_open(&logger->pipe, fd)) != 0){
		return r;
	}
	root_logger.pipe.data = logger;

	if ((r = uv_async_init(loop, &logger->async, &logger_flush)) != 0){
		return r;
	}
	logger->async.data = logger;

	logger->head = NULL;
	logger->next = &logger->head;
	logger->f_color = isatty(fd);

	return 0;
}

void log(char c, const char *format, ...){

	logger_t *const logger = &root_logger;
	va_list args;
	log_event_t *event = log_event_alloc();

	clock_gettime(CLOCK_REALTIME, &event->timestamp);

	event->cc_buf[0] = c;
	event->cc_buf[1] = '\0';

	va_start(args, format);
	vsnprintf(event->msg_buf, sizeof(event->msg_buf), format, args);
	va_end(args);

	event->next = NULL;

	// Add this event to the end of the linked list without locking.
	// This is equivalent to:
	//     (*logger->next) = event;     // Add this event to the end of the list
	//     logger->next = &event-next;  // Make subsequent entries follow this event
	while (1){
		log_event_t **old_next = __atomic_load_n(&logger->next, __ATOMIC_CONSUME);
		log_event_t *expected = NULL;

		if (!__atomic_compare_exchange_n(logger->next, &expected, event, true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)){
			// Some other thread added an event to the end of the list.
			// Try again with the new "next" pointer.
			continue;
		}


		// Success!
		// The link list now has a new last pointer but the "next" pointer
		// hasn't been updated to add the next event after the one just added.
		// If the "next" pointer has been changed, it must have been set to the
		// empty list (to flush the log entries to disk), in which case it's
		// it shouldn't be updated.
		__atomic_compare_exchange_n(&logger->next, &old_next, &event->next, true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
		break;
	}

	uv_async_send(&logger->async);
}
