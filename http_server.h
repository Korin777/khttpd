#ifndef KHTTPD_HTTP_SERVER_H
#define KHTTPD_HTTP_SERVER_H

#include <net/sock.h>

struct http_server_param {
    struct socket *listen_socket;
};

// 每個連線都會建立一個此結構
struct khttpd {
    struct socket *sock;
    struct list_head list;
    struct work_struct khttpd_work;
    struct timer_list timer;
};

struct http_service {
    bool is_stopped;
    struct list_head worker;
};


extern int http_server_daemon(void *arg);

#endif
