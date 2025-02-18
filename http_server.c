#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kthread.h>
#include <linux/sched/signal.h>
#include <linux/tcp.h>
#include <linux/timer.h>

#include "http_parser.h"
#include "http_server.h"

#define SEND_BUFFER_LEN 1024

#define CRLF "\r\n"

#define HTTP_RESPONSE_200_DUMMY                               \
    ""                                                        \
    "HTTP/1.1 200 OK" CRLF "Server: " KBUILD_MODNAME CRLF     \
    "Content-Type: text/plain" CRLF "Content-Length: 12" CRLF \
    "Connection: Close" CRLF CRLF "Hello World!" CRLF
#define HTTP_RESPONSE_200_KEEPALIVE_DUMMY                     \
    ""                                                        \
    "HTTP/1.1 200 OK" CRLF "Server: " KBUILD_MODNAME CRLF     \
    "Content-Type: text/plain" CRLF "Content-Length: 12" CRLF \
    "Connection: Keep-Alive" CRLF CRLF "Hello World!" CRLF
#define HTTP_RESPONSE_200_HTML                            \
    ""                                                    \
    "HTTP/1.1 200 OK" CRLF "Server: " KBUILD_MODNAME CRLF \
    "Content-Type: text/html" CRLF "Connection: Close" CRLF CRLF
#define HTML_TEMPLATE                                    \
    ""                                                   \
    "<html><head><style>\r\n"                            \
    "body{font-family: monospace; font-size: 15px;}\r\n" \
    "td {padding: 1.5px 6px;}\r\n"                       \
    "</style></head><body><table>\r\n"
#define HTTP_RESPONSE_501                                              \
    ""                                                                 \
    "HTTP/1.1 501 Not Implemented" CRLF "Server: " KBUILD_MODNAME CRLF \
    "Content-Type: text/plain" CRLF "Content-Length: 21" CRLF          \
    "Connection: Close" CRLF CRLF "501 Not Implemented" CRLF
#define HTTP_RESPONSE_501_KEEPALIVE                                    \
    ""                                                                 \
    "HTTP/1.1 501 Not Implemented" CRLF "Server: " KBUILD_MODNAME CRLF \
    "Content-Type: text/plain" CRLF "Content-Length: 21" CRLF          \
    "Connection: KeepAlive" CRLF CRLF "501 Not Implemented" CRLF

#define RECV_BUFFER_SIZE 4096

struct http_service daemon = {.is_stopped = false};
extern struct workqueue_struct *khttpd_wq;

struct http_request {
    struct socket *socket;
    enum http_method method;
    char request_url[128];
    struct dir_context dir_context;
    int complete;
};


static int http_server_recv(struct socket *sock, char *buf, size_t size)
{
    struct kvec iov = {.iov_base = (void *) buf, .iov_len = size};
    struct msghdr msg = {.msg_name = 0,
                         .msg_namelen = 0,
                         .msg_control = NULL,
                         .msg_controllen = 0,
                         .msg_flags = 0};
    return kernel_recvmsg(sock, &msg, &iov, 1, size, msg.msg_flags);
}

static int http_server_send(struct socket *sock, const char *buf, size_t size)
{
    struct msghdr msg = {.msg_name = NULL,
                         .msg_namelen = 0,
                         .msg_control = NULL,
                         .msg_controllen = 0,
                         .msg_flags = 0};
    int done = 0;
    while (done < size) {
        struct kvec iov = {
            .iov_base = (void *) ((char *) buf + done),
            .iov_len = size - done,
        };
        int length = kernel_sendmsg(sock, &msg, &iov, 1, iov.iov_len);
        if (length < 0) {
            pr_err("write error: %d\n", length);
            break;
        }
        done += length;
    }
    return done;
}

static int trace_dir(struct dir_context *dir_context,
                     const char *name,
                     int namelen,
                     loff_t offset,
                     u64 ino,
                     unsigned int d_type)
{
    if (strcmp(name, ".")) {
        struct http_request *request =
            container_of(dir_context, struct http_request, dir_context);
        char buf[SEND_BUFFER_LEN] = {0};
        snprintf(buf, SEND_BUFFER_LEN,
                 "<tr><td><a href=\"%s/%s\">%s</a></td></tr>\r\n",
                 request->request_url, name, name);
        http_server_send(request->socket, buf, strlen(buf));
    }
    return 0;
}

static bool traverse_directory(struct http_request *request)
{
    char buf[SEND_BUFFER_LEN] = {0};

    if (request->method != HTTP_GET) {
        snprintf(buf, SEND_BUFFER_LEN, HTTP_RESPONSE_501);
        http_server_send(request->socket, buf, strlen(buf));
        return false;
    }

    snprintf(buf, SEND_BUFFER_LEN, HTTP_RESPONSE_200_HTML);
    http_server_send(request->socket, buf, strlen(buf));
    snprintf(buf, SEND_BUFFER_LEN, HTML_TEMPLATE);
    http_server_send(request->socket, buf, strlen(buf));

    struct file *fp;
    fp = filp_open(request->request_url, O_RDONLY | O_DIRECTORY, 0);
    if (IS_ERR(fp)) {
        pr_info("open file failed");
        return false;
    }
    request->dir_context.actor = trace_dir;
    iterate_dir(fp, &request->dir_context);
    snprintf(buf, SEND_BUFFER_LEN, "</table></body></html>\r\n");
    http_server_send(request->socket, buf, strlen(buf));
    filp_close(fp, NULL);
    return true;
}

static int http_server_response(struct http_request *request, int keep_alive)
{
    // char *response;

    // if (request->method != HTTP_GET)
    //     response = keep_alive ? HTTP_RESPONSE_501_KEEPALIVE :
    //     HTTP_RESPONSE_501;
    // else
    //     response = keep_alive ? HTTP_RESPONSE_200_KEEPALIVE_DUMMY
    //                           : HTTP_RESPONSE_200_DUMMY;
    // http_server_send(request->socket, response, strlen(response));
    traverse_directory(request);

    return 0;
}

// 初始話 request
static int http_parser_callback_message_begin(http_parser *parser)
{
    struct http_request *request = parser->data;
    struct socket *socket = request->socket;
    memset(request, 0x00, sizeof(struct http_request));
    request->socket = socket;
    return 0;
}

// 解析 url
static int http_parser_callback_request_url(http_parser *parser,
                                            const char *p,
                                            size_t len)
{
    struct http_request *request = parser->data;
    if (p[len - 1] == '/')
        --len;
    strncat(request->request_url, p, len);
    pr_info("request url : %s/\n", request->request_url);
    return 0;
}

static int http_parser_callback_header_field(http_parser *parser,
                                             const char *p,
                                             size_t len)
{
    return 0;
}

static int http_parser_callback_header_value(http_parser *parser,
                                             const char *p,
                                             size_t len)
{
    return 0;
}

// 解析 method ex: get, post
static int http_parser_callback_headers_complete(http_parser *parser)
{
    struct http_request *request = parser->data;
    request->method = parser->method;
    return 0;
}

static int http_parser_callback_body(http_parser *parser,
                                     const char *p,
                                     size_t len)
{
    return 0;
}

static int http_parser_callback_message_complete(http_parser *parser)
{
    struct http_request *request = parser->data;
    http_server_response(request, http_should_keep_alive(parser));
    request->complete = 1;
    return 0;
}

static void free_work(void)
{
    struct khttpd *l, *tar;
    /* cppcheck-suppress uninitvar */

    list_for_each_entry_safe (tar, l, &daemon.worker, list) {
        kernel_sock_shutdown(tar->sock, SHUT_RDWR);
        flush_work(&tar->khttpd_work);
        sock_release(tar->sock);
        kfree(tar);
    }
}

void timer_callback(struct timer_list *arg)
{
    struct khttpd *worker = container_of(arg, struct khttpd, timer);
    kernel_sock_shutdown(worker->sock, SHUT_RDWR);
}


// work item
static void http_server_worker(struct work_struct *work)
{
    pr_info("connect\n");
    struct khttpd *worker = container_of(work, struct khttpd, khttpd_work);
    timer_setup(&worker->timer, timer_callback, 0);

    char *buf;
    struct http_parser parser;
    struct http_parser_settings setting = {
        .on_message_begin = http_parser_callback_message_begin,
        .on_url = http_parser_callback_request_url,
        .on_header_field = http_parser_callback_header_field,
        .on_header_value = http_parser_callback_header_value,
        .on_headers_complete = http_parser_callback_headers_complete,
        .on_body = http_parser_callback_body,
        .on_message_complete = http_parser_callback_message_complete};
    struct http_request request;

    allow_signal(SIGKILL);
    allow_signal(SIGTERM);

    buf = kzalloc(RECV_BUFFER_SIZE, GFP_KERNEL);
    if (!buf) {
        pr_err("can't allocate memory!\n");
        return;
    }

    request.socket = worker->sock;
    http_parser_init(&parser, HTTP_REQUEST);
    parser.data = &request;
    while (!daemon.is_stopped) {
        int ret = http_server_recv(worker->sock, buf, RECV_BUFFER_SIZE - 1);
        pr_info("ret %d\n", ret);
        if (ret <= 0) {
            if (ret)
                pr_err("recv error: %d\n", ret);
            break;
        }
        http_parser_execute(&parser, &setting, buf, ret);
        if (request.complete && !http_should_keep_alive(&parser)) {
            break;
        }
        // timer update
        mod_timer(&worker->timer, jiffies + msecs_to_jiffies(5000));
        memset(buf, 0, RECV_BUFFER_SIZE);
    }
    del_timer_sync(&worker->timer);
    kernel_sock_shutdown(worker->sock, SHUT_RDWR);
    kfree(buf);
    pr_info("close\n");
    return;
}

// create work item
static struct work_struct *create_work(struct socket *sk)
{
    struct khttpd *work;

    if (!(work = kmalloc(sizeof(*work), GFP_KERNEL)))
        return NULL;

    work->sock = sk;

    // set up a work item pointing to that function
    INIT_WORK(&work->khttpd_work, http_server_worker);

    list_add(&work->list, &daemon.worker);

    return &work->khttpd_work;
}

int http_server_daemon(void *arg)
{
    struct socket *socket;
    // struct task_struct *worker;
    struct work_struct *work;
    struct http_server_param *param = (struct http_server_param *) arg;

    allow_signal(SIGKILL);
    allow_signal(SIGTERM);

    INIT_LIST_HEAD(&daemon.worker);

    while (!kthread_should_stop()) {
        int err = kernel_accept(param->listen_socket, &socket, 0);
        if (err < 0) {
            if (signal_pending(current))
                break;
            pr_err("kernel_accept() error: %d\n", err);
            continue;
        }
        if (unlikely(!(work = create_work(socket)))) {
            pr_err("can't create more worker process, connection closed\n");
            kernel_sock_shutdown(socket, SHUT_RDWR);
            sock_release(socket);
            continue;
        }
        // queue work item on a workqueue
        queue_work(khttpd_wq, work);
    }

    daemon.is_stopped = true;
    free_work();

    return 0;
}
