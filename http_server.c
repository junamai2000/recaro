#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/tcp.h>
#include "tkhttpd.h"
#include "kmemcached.h"

#define CRLF "\r\n"

#define HTTP_RESPONSE_200_DUMMY ""                 \
	"HTTP/1.1 200 OK" CRLF                         \
	"Server: " MODULE_NAME "/" MODULE_REV CRLF     \
	"Content-Type: text/plain" CRLF                \
	"Content-Length: 8" CRLF                       \
	"Connection: Close" CRLF                       \
	CRLF                                           \
	"200 OK" CRLF
#define HTTP_RESPONSE_200_KEEPALIVE_DUMMY ""       \
	"HTTP/1.1 200 OK" CRLF                         \
	"Server: " MODULE_NAME "/" MODULE_REV CRLF     \
	"Content-Type: text/plain" CRLF                \
	"Content-Length: 8" CRLF                       \
	"Connection: Keep-Alive" CRLF                  \
	CRLF                                           \
	"200 OK" CRLF
#define HTTP_RESPONSE_404 ""                       \
	"HTTP/1.1 404 Not Found" CRLF                  \
	"Server: " MODULE_NAME "/" MODULE_REV CRLF     \
	"Content-Type: text/plain" CRLF                \
	"Content-Length: 15" CRLF                      \
	"Connection: Close" CRLF                       \
	CRLF                                           \
	"404 Not Found" CRLF
#define HTTP_RESPONSE_404_KEEPALIVE ""             \
	"HTTP/1.1 404 Not Found" CRLF                  \
	"Server: " MODULE_NAME "/" MODULE_REV CRLF     \
	"Content-Type: text/plain" CRLF                \
	"Content-Length: 15" CRLF                      \
	"Connection: Keep-Alive" CRLF                  \
	CRLF                                           \
	"404 Not Found" CRLF
#define HTTP_RESPONSE_500 ""                       \
	"HTTP/1.1 500 Internal Server Error" CRLF      \
	"Server: " MODULE_NAME "/" MODULE_REV CRLF     \
	"Content-Type: text/plain" CRLF                \
	"Content-Length: 27" CRLF                      \
	"Connection: Close" CRLF                       \
	CRLF                                           \
	"500 Internal Server Error" CRLF
#define HTTP_RESPONSE_500_KEEPALIVE ""             \
	"HTTP/1.1 500 Internal Server Error" CRLF      \
	"Server: " MODULE_NAME "/" MODULE_REV CRLF     \
	"Content-Type: text/plain" CRLF                \
	"Content-Length: 27" CRLF                      \
	"Connection: Keep-Alive" CRLF                  \
	CRLF                                           \
	"500 Internal Server Error" CRLF
#define HTTP_RESPONSE_501 ""                       \
	"HTTP/1.1 501 Not Implemented" CRLF            \
	"Server: " MODULE_NAME "/" MODULE_REV CRLF     \
	"Content-Type: text/plain" CRLF                \
	"Content-Length: 21" CRLF                      \
	"Connection: Close" CRLF                       \
	CRLF                                           \
	"501 Not Implemented" CRLF
#define HTTP_RESPONSE_501_KEEPALIVE ""             \
	"HTTP/1.1 501 Not Implemented" CRLF            \
	"Server: " MODULE_NAME "/" MODULE_REV CRLF     \
	"Content-Type: text/plain" CRLF                \
	"Content-Length: 21" CRLF                      \
	"Connection: KeepAlive" CRLF                   \
	CRLF                                           \
	"501 Not Implemented" CRLF
#define HTTP_RESPONSE_505                          \
	"HTTP/1.1 505 HTTP Version Not Supported" CRLF \
	"Server: " MODULE_NAME "/" MODULE_REV CRLF     \
	"Content-Type: text/plain" CRLF                \
	"Content-Length: 32" CRLF                      \
	"Connection: Close" CRLF                       \
	CRLF                                           \
	"505 HTTP Version Not Supported" CRLF
#define HTTP_RESPONSE_505_KEEPALIVE                \
	"HTTP/1.1 505 HTTP Version Not Supported" CRLF \
	"Server: " MODULE_NAME "/" MODULE_REV CRLF     \
	"Content-Type: text/plain" CRLF                \
	"Content-Length: 32" CRLF                      \
	"Connection: KeepAlive" CRLF                   \
	CRLF                                           \
	"505 HTTP Version Not Supported" CRLF

#define CONTENT_TYPE "Content-Type: "
#define CONTENT_LENGTH "Content-Length: "

#define RECV_BUFFER_SIZE 4096

struct http_request {
	struct socket *socket;
	enum http_method method;
	char request_url[128];
	int complete;
};

static int
http_server_recv (struct socket *sock, char *buf, size_t size) {
	mm_segment_t oldfs;
	struct iovec iov = {
		.iov_base = (void *)buf,
		.iov_len = size
	};
	struct msghdr msg = {
		.msg_name = 0,
		.msg_namelen = 0,
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_control = NULL,
		.msg_controllen = 0,
		.msg_flags = 0
	};
	int length = 0;

	oldfs = get_fs();
	set_fs(KERNEL_DS);
	length = sock_recvmsg(sock, &msg, size, msg.msg_flags);
	set_fs(oldfs);
	return length;
}

static int
http_server_send (struct socket *sock, const char *buf, size_t size, int more) {
	mm_segment_t oldfs;
	struct iovec iov;
	struct msghdr msg = {
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_flags = more ? MSG_MORE : 0
	};
	int length, done = 0;

	oldfs = get_fs();
	set_fs(KERNEL_DS);
	while (done < size) {
		iov.iov_base = (void *)((char *)buf + done);
		iov.iov_len = size - done;
		length = sock_sendmsg(sock, &msg, iov.iov_len);
		if (length < 0) {
			printk(KERN_ERR MODULE_NAME ": write error: %d\n", length);
			break;
		}
		done += length;
	}
	set_fs(oldfs);
	return done;
}


static int
response_from_item(item_t *item, struct http_request *request, int *keep_alive) {
	char *start, *p, *q, *end;
	char buf[128] = "HTTP/1.1 200 OK\r\n";
	char *w = buf + strlen(buf);
	long nchunk;

	start = p = item->data;
	end = start + item->size;

	// 1行目: content_type
	q = p;
	while (*q != '\r') {
		q++;
		if (q == end) {
			printk("BAD CACHE: %s\n", request->request_url);
			return 503;
		}
	}
	strcpy(w, CONTENT_TYPE); w += strlen(CONTENT_TYPE);
	memcpy(w, p, q-p); w += q-p;
	*w++ = '\r';
	*w++ = '\n';
	p = q = q + 2; // skip \r\n

	// 2行目チャンク数.
	nchunk = simple_strtol(p, &q, 10); p = q = q + 2;

	if (nchunk <= 0) {
		printk("BUG: nchunk=%ld\n", nchunk);
		return 503;
	}
	else if (nchunk == 1) {
		// simple response.
		// chunkサイズは chunked encoding にならって 16進
		long size = simple_strtol(p, &q, 16); p = q = q+2;
		if (*keep_alive) {
			w += sprintf(w, "Connection: keep-alive\r\n");
		}
		w += sprintf(w, "Content-Length: %ld\r\n\r\n", size);
		http_server_send(request->socket, buf, w-buf, 1);
		http_server_send(request->socket, p, size, 0);
		return 0;
	}
	else {
		int cur;
		if (*keep_alive) {
			w += sprintf(w, "Connection: keep-alive\r\n");
		}
		w += sprintf(w, "Transfer-Encoding: chunked\r\n\r\n");
		http_server_send(request->socket, buf, w-buf, 1);
		//printk("start sending %ld chunks\n", nchunk);

		for (cur=0; cur<nchunk; ++cur) {
			long size = simple_strtol(p, &q, 16);
			//printk("chunk size: %ld\n", size);
			p = q = q + 2;
			w = buf + sprintf(buf, "%lx\r\n", size);
			http_server_send(request->socket, buf, w-buf, 1);
			http_server_send(request->socket, p, size, 1);
			http_server_send(request->socket, CRLF, 2, 1);
			p = q = q + size + 2;

			if (cur+1 < nchunk) {
				item_t *subitem;
				// TODO: SSIの入れ子に対応.
				while (*q != '\r') {
					q++;
				}
				subitem = get_item(p, q-p);
				strncpy(buf, p, q-p);
				buf[q-p] = '\0';
				//printk("loading '%s'\n", buf);
				p = q = q+2;
				if (subitem == NULL) {
					printk("can't load subitem.\n");
					continue;
				}
				//printk("ssi chunk size: %ld\n", subitem->size);
				size = sprintf(buf, "%lx\r\n", subitem->size);
				http_server_send(request->socket, buf, size, 1);
				http_server_send(request->socket, subitem->data, subitem->size, 1);
				http_server_send(request->socket, CRLF, 2, 1);
				release_item(subitem);
			}
		}
		http_server_send(request->socket, "0\r\n\r\n", 5, 0);
		return 0;
	}
}

static int
do_get (struct http_request *request, int *keep_alive) {
	item_t *item = get_item(request->request_url, strlen(request->request_url));
	if (item) {
		int status = response_from_item(item, request, keep_alive);
		release_item(item);
		return status;
	}
	// TODO: reverse proxy.
	return 404;
}

static int
do_post (struct http_request *request, int *keep_alive) {
	// TODO: reverse proxy.
	return 501;
}

static int
http_server_response (struct http_request *request, int keep_alive) {
	char *response;
	int status;

	switch (request->method) {
	case HTTP_GET:
		status = do_get(request, &keep_alive);
		break;
	case HTTP_POST:
		status = do_post(request, &keep_alive);
		break;
	default:
		// 405 Method Not Allowed
		status = 405;
	}
	if (status == 0)
		return 0; //response has sent already.

	//TODO: ret が 0 以外の場合は、そのステータスコードに応じたデフォルトのレスポンスを返す.
	printk(KERN_INFO MODULE_NAME ": request_url = %s\n", request->request_url);
	if (request->method != HTTP_GET) {
		response = keep_alive ? HTTP_RESPONSE_501_KEEPALIVE : HTTP_RESPONSE_501;
	} else {
		response = keep_alive ? HTTP_RESPONSE_200_KEEPALIVE_DUMMY : HTTP_RESPONSE_200_DUMMY;
	}
	http_server_send(request->socket, response, strlen(response), 0);
	return 0;
}

static int
http_parser_callback_message_begin (http_parser *parser) {
	struct http_request *request = parser->data;
	struct socket *socket = request->socket;
	memset(request, 0x00, sizeof(struct http_request));
	request->socket = socket;
	return 0;
}

static int
http_parser_callback_request_url (http_parser *parser, const char *p, size_t len) {
	struct http_request *request = parser->data;
	strncat(request->request_url, p, len);
	return 0;
}

static int
http_parser_callback_header_field (http_parser *parser, const char *p, size_t len) {
	return 0;
}

static int
http_parser_callback_header_value (http_parser *parser, const char *p, size_t len) {
	return 0;
}

static int
http_parser_callback_headers_complete (http_parser *parser) {
	struct http_request *request = parser->data;
	request->method = parser->method;	
	return 0;
}

static int
http_parser_callback_body (http_parser *parser, const char *p, size_t len) {
	return 0;
}

static int
http_parser_callback_message_complete (http_parser *parser) {
	struct http_request *request = parser->data;
	http_server_response(request, http_should_keep_alive(parser));
	request->complete = 1;
	return 0;
}

static int
http_server_worker (void *arg) {
	struct socket *socket;
	char *buf;
	int ret;
	struct http_parser parser;
	struct http_parser_settings setting = {
		.on_message_begin = http_parser_callback_message_begin,
		.on_url = http_parser_callback_request_url,
		.on_header_field = http_parser_callback_header_field,
		.on_header_value = http_parser_callback_header_value,
		.on_headers_complete = http_parser_callback_headers_complete,
		.on_body = http_parser_callback_body,
		.on_message_complete = http_parser_callback_message_complete
	};
	struct http_request request;

	socket = (struct socket *)arg;
	allow_signal(SIGKILL);
	allow_signal(SIGTERM);
	buf = kmalloc(RECV_BUFFER_SIZE, GFP_KERNEL);
	if (!buf) {
		printk(KERN_ERR MODULE_NAME ": can't allocate memory!\n");
		return -1;
	}
	request.socket = socket;
	http_parser_init(&parser, HTTP_REQUEST);
	parser.data = &request;
	while (!kthread_should_stop()) {
		ret = http_server_recv(socket, buf, RECV_BUFFER_SIZE - 1);
		if (ret <= 0) {
			if (ret) {
				printk(KERN_ERR MODULE_NAME ": recv error: %d\n", ret);
			}
			break;
		}
		http_parser_execute(&parser, &setting, buf, ret);
		if (request.complete && !http_should_keep_alive(&parser)) {
			break;
		}
	}
	kernel_sock_shutdown(socket, SHUT_RDWR);
	sock_release(socket);
	kfree(buf);
	return 0;
}

int
http_server_daemon (void *arg) {
	struct http_server_param *param;
	struct socket *socket;
	int err;
	struct task_struct *worker;

	param = (struct http_server_param *)arg;
	allow_signal(SIGKILL);
	allow_signal(SIGTERM);
	while (!kthread_should_stop()) {
		int yes=1;
		err = kernel_accept(param->listen_socket, &socket, 0);
		if (err < 0) {
			if (signal_pending(current)) {
				break;
			}
			printk(KERN_ERR MODULE_NAME ": kernel_accept() error: %d\n", err);
			continue;
		}
		kernel_setsockopt(socket, SOL_TCP, TCP_NODELAY, (void*)&yes, sizeof(yes));
		worker = kthread_run(http_server_worker, socket, MODULE_NAME);
		if (IS_ERR(worker)) {
			printk(KERN_ERR MODULE_NAME ": can't create more worker process\n");
			continue;
		}
	}
	return 0;
}

/* vim: set sw=8 ts=8 noexpandtab :*/
