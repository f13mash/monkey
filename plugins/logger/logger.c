/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Monkey HTTP Daemon
 *  ------------------
 *  Copyright (C) 2001-2012, Eduardo Silva Pereira <edsiper@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#define _GNU_SOURCE

/* System Headers */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

/* Monkey Plugin Interface */
#include "MKPlugin.h"

/* Local Headers */
#include "logger.h"
#include "pointers.h"

MONKEY_PLUGIN("logger", /* shortname */
              "Logger", /* name */
              VERSION, /* version */
              /* hooks */
              MK_PLUGIN_CORE_PRCTX | MK_PLUGIN_CORE_THCTX | MK_PLUGIN_STAGE_40);


struct status_response {
    int   i_status;
    char *s_status;
};

static struct status_response response_codes[] = {
    /* Common matches first */
    {200, "200"}, {404, "404"},

    {100, "100"}, {101, "101"},
    {201, "201"}, {202, "202"}, {203, "203"}, {204, "204"}, 
    {205, "205"}, {206, "206"},    
    {300, "300"}, {301, "301"}, {302, "302"}, {303, "303"}, {304, "304"},
    {305, "305"},
    {400, "400"}, {401, "401"}, {402, "402"}, {403, "403"},
    {405, "405"}, {406, "406"}, {407, "407"}, {408, "408"}, {409, "409"},
    {410, "410"}, {411, "411"}, {412, "412"}, {413, "413"}, {414, "414"},
    {415, "415"},
    {500, "500"}, {501, "501"}, {502, "502"}, {503, "503"}, {504, "504"},
    {505, "505"}, 
};


char *mk_logger_match_by_fd(int fd)
{
    struct mk_list *head;
    struct log_target *entry;

    mk_list_foreach(head, &targets_list) {
        entry = mk_list_entry(head, struct log_target, _head);

        if (entry->fd_access[0] == fd) {
            return entry->file_access;
        }
        if (entry->fd_error[0] == fd) {
            return entry->file_error;
        }
    }

    return NULL;
}

struct log_target *mk_logger_match_by_host(struct host *host)
{
    struct mk_list *head;
    struct log_target *entry;

    mk_list_foreach(head, &targets_list) {
        entry = mk_list_entry(head, struct log_target, _head);
        if (entry->host == host) {
            return entry;
        }
    }

    return NULL;
}

struct iov *mk_logger_get_cache()
{
    return pthread_getspecific(_mkp_data);
}

void *mk_logger_worker_init(void *args)
{
    int efd, max_events = mk_api->config->nhosts;
    int i, bytes, err;
    int flog;
    int clk;
    long slen;
    int timeout;
    char *target;
    struct mk_list *head;
    struct log_target *entry;

    /* pipe_size:
     * ---------- 
     * Linux set a pipe size usingto the PAGE_SIZE, 
     * check linux/include/pipe_fs_i.h for details:
     *
     *       #define PIPE_SIZE               PAGE_SIZE
     *
     * In the same header file we can found that every 
     * pipe has 16 pages, so our real memory allocation
     * is: (PAGE_SIZE*PIPE_BUFFERS)
     */
    long pipe_size;

    /* buffer_limit:
     * -------------
     * it means the maximum data that a monkey log pipe can contain.
     */
    long buffer_limit;

    mk_api->worker_rename("monkey: logger");

    /* Monkey allow just 75% of a pipe capacity */
    pipe_size = sysconf(_SC_PAGESIZE) * 16;
    buffer_limit = (pipe_size * MK_LOGGER_PIPE_LIMIT);

    /* Creating poll */
    efd = mk_api->epoll_create(max_events);

    /* Registering targets for virtualhosts */
    mk_list_foreach(head, &targets_list) {
        entry = mk_list_entry(head, struct log_target, _head);

        /* Add access log file */
        if (entry->fd_access[0] > 0) {
            mk_api->epoll_add(efd, entry->fd_access[0], 
                              MK_EPOLL_READ, MK_EPOLL_LEVEL_TRIGGERED);
        }
        /* Add error log file */
        if (entry->fd_error[0] > 0) {
            mk_api->epoll_add(efd, entry->fd_error[0], 
                              MK_EPOLL_READ, MK_EPOLL_LEVEL_TRIGGERED);
        }
    }

    /* Set initial timeout */
    timeout = time(NULL) + mk_logger_timeout;

    /* Reading pipe buffer */
    while (1) {
        usleep(1200);

        struct epoll_event events[max_events];
        int num_fds = epoll_wait(efd, events, max_events, -1);
        
        clk = mk_api->time_unix();

        for (i = 0; i < num_fds; i++) {
            target = mk_logger_match_by_fd(events[i].data.fd);

            if (!target) {
                mk_warn("Could not match host/epoll_fd");
                continue;
            }

            err = ioctl(events[i].data.fd, FIONREAD, &bytes);
            if (err == -1) {
                perror("ioctl");
            }

            if (bytes < buffer_limit && clk <= timeout) {
                continue;
            }

            timeout = clk + mk_logger_timeout;
        
            flog = open(target, O_WRONLY | O_CREAT, 0644);
            if (flog == -1) {
                mk_warn("Could not open logfile '%s'", target);
                
                if (bytes >= buffer_limit) {
                    /*
                     * if our buffer is full and we cannot open the logfile,
                     * we should consume the information in some way :(
                     */
                    mk_warn("Logger buffer is full, fake consuming");
                    int consumed = 0;
                    char buf[255];
                    do {
                        slen = read(events[i].data.fd, buf, 255);
                        if (slen > 0) { consumed += slen; }
                        else { break; }
                    } while (consumed < bytes);
                }

                continue;
            }
            
            lseek(flog, 0, SEEK_END);
            slen = splice(events[i].data.fd, NULL, flog,
                          NULL, bytes, SPLICE_F_MOVE);
            if (slen == -1) {
                mk_warn("Could not write to log file: splice() = %i", slen);
            }
            
            PLUGIN_TRACE("written %i bytes", bytes);
            close(flog);
        }
    }
}

int mk_logger_read_config(char *path)
{
    int timeout;
    unsigned long len;
    char *default_file = NULL;
    struct mk_config *conf;
    struct mk_config_section *section;

    mk_api->str_build(&default_file, &len, "%slogger.conf", path);
    conf = mk_api->config_create(default_file);
    section = mk_api->config_section_get(conf, "LOGGER");
    if (section) {

        /* FlushTimeout */
        timeout = (size_t) mk_api->config_section_getval(section,
                                                         "FlushTimeout",
                                                         MK_CONFIG_VAL_NUM);
        if (timeout <= 0) {
            mk_err("FlushTimeout does not have a proper value");
            exit(EXIT_FAILURE);
        }
        mk_logger_timeout = timeout;
        PLUGIN_TRACE("FlushTimeout %i seconds", mk_logger_timeout);

        /* MasterLog */
        mk_logger_master_path = mk_api->config_section_getval(section,
                                                              "MasterLog",
                                                              MK_CONFIG_VAL_STR);
        PLUGIN_TRACE("MasterLog '%s'", mk_logger_master_path);
    }

    mk_api->mem_free(default_file);

    return 0;
}

void mk_logger_print_details(void)
{
    time_t now;
    struct tm *current;

    now = time(NULL);
    current = localtime(&now);
    printf("[%i/%02i/%02i %02i:%02i:%02i] Monkey Started\n", 
           current->tm_year + 1900,
           current->tm_mon,
           current->tm_mday,
           current->tm_hour,
           current->tm_min,
           current->tm_sec);
    printf("   version          : %s\n", VERSION);
    printf("   server port      : %i\n", mk_api->config->serverport);
    printf("   number of workers: %i\n", mk_api->config->workers);
    fflush(stdout);
}

int _mkp_init(void **api, char *confdir)
{
    int fd;
    mk_api = *api;

    /* Specific thread key */
    pthread_key_create(&cache_content_length, NULL);
    pthread_key_create(&cache_status, NULL);
    pthread_key_create(&cache_ip_str, NULL);

    /* Global configuration */
    mk_logger_timeout = MK_LOGGER_TIMEOUT_DEFAULT;
    mk_logger_master_path = NULL;
    mk_logger_read_config(confdir);
    
    /* Check masterlog */
    if (mk_logger_master_path) {
        fd = open(mk_logger_master_path, O_WRONLY | O_CREAT, 0644);
        if (fd == -1) {
            mk_err("Could not open/create master logfile %s", mk_logger_master_path);
            exit(EXIT_FAILURE);

        }
        else {
            /* Close test FD for MasterLog */
            close(fd);
        }
    }

    /* Init mk_pointers */
    mk_logger_init_pointers();

    return 0;
}

void _mkp_exit()
{
}

void _mkp_core_prctx()
{
    struct log_target *new;
    struct host *host;
    struct mk_config_section *section;
    struct mk_config_entry *access_entry, *error_entry;

    /* Restore STDOUT if we are in background mode */
    if (mk_logger_master_path != NULL && mk_api->config->is_daemon == MK_TRUE) {
        mk_logger_master_stdout = freopen(mk_logger_master_path, "a+", stdout);
        mk_logger_master_stderr = freopen(mk_logger_master_path, "a+", stderr);
        mk_logger_print_details();
    }

    PLUGIN_TRACE("Reading virtual hosts");

    mk_list_init(&targets_list);

    host = mk_api->config->hosts;
    while (host) {
        /* Read logger section from virtual host configuration */
        section = mk_api->config_section_get(host->config, "LOGGER");
        if (section) {
            /* Read configuration entries */
            access_entry = mk_api->config_section_getval(section, "AccessLog", 
                                                         MK_CONFIG_VAL_STR);
            error_entry = mk_api->config_section_getval(section, "ErrorLog",
                                                        MK_CONFIG_VAL_STR);

            if (access_entry || error_entry) {
                new = mk_api->mem_alloc(sizeof(struct log_target));
                /* Set access pipe */
                if (access_entry) {
                    if (pipe(new->fd_access) < 0) {
                        mk_err("Could not create pipe");
                        exit(EXIT_FAILURE);
                    }
                    fcntl(new->fd_access[1], F_SETFL, O_NONBLOCK);
                    new->file_access = (char *) access_entry;
                }
                /* Set error pipe */
                if (error_entry) {
                    if (pipe(new->fd_error) < 0) {
                        mk_err("Could not create pipe");
                        exit(EXIT_FAILURE);
                    }
                    fcntl(new->fd_error[1], F_SETFL, O_NONBLOCK);
                    new->file_error = (char *) error_entry;
                }

                new->host = host;
                mk_list_add(&new->_head, &targets_list);
            }
        }
        host = host->next;
    }
    
    mk_api->worker_spawn((void *) mk_logger_worker_init);
}

void _mkp_core_thctx()
{
    struct mk_iov *iov_log;
    mk_pointer *content_length;
    mk_pointer *status;
    mk_pointer *ip_str;

    PLUGIN_TRACE("Creating thread cache");
    
    /* Cache iov log struct */
    iov_log = mk_api->iov_create(15, 0);
    pthread_setspecific(_mkp_data, (void *) iov_log);

    /* Cache content length */
    content_length = mk_api->mem_alloc_z(sizeof(mk_pointer));
    content_length->data = mk_api->mem_alloc_z(MK_UTILS_INT2MKP_BUFFER_LEN);
    content_length->len = -1;
    pthread_setspecific(cache_content_length, (void *) content_length);

    /* Cahe status */
    status = mk_api->mem_alloc_z(sizeof(mk_pointer));
    status->data = mk_api->mem_alloc_z(MK_UTILS_INT2MKP_BUFFER_LEN);
    status->len = -1;
    pthread_setspecific(cache_status, (void *) status);

    /* Cache IP address */
    ip_str = mk_api->mem_alloc_z(sizeof(mk_pointer));
    ip_str->data = mk_api->mem_alloc_z(INET6_ADDRSTRLEN + 1);
    ip_str->len  = -1;
    pthread_setspecific(cache_ip_str, (void *) ip_str);
}

int _mkp_stage_40(struct client_session *cs, struct session_request *sr)
{
    int i, http_status, ret;
    int array_len = ARRAY_SIZE(response_codes);
    struct log_target *target;
    struct mk_iov *iov;
    mk_pointer *date;
    mk_pointer *content_length;
    mk_pointer *ip_str;
    mk_pointer status;

    /* Set response status */
    http_status = sr->headers.status;

    /* Look for target log file */
    target = mk_logger_match_by_host(sr->host_conf);
    if (!target) {
        PLUGIN_TRACE("No target found");
        return 0;
    }

    /* Get iov cache struct and reset indexes */
    iov = (struct mk_iov *) mk_logger_get_cache();
    iov->iov_idx = 0;
    iov->buf_idx = 0;
    iov->total_len = 0;

    /* Format IP string */
    ip_str = pthread_getspecific(cache_ip_str);
    ret = mk_api->socket_ip_str(cs->socket, 
                                &ip_str->data, 
                                INET6_ADDRSTRLEN + 1,
                                &ip_str->len);
    /* 
     * If the socket is not longer available ip_str can be null, 
     * so we must check this condition and return
     */
    if (ret < 0) {
        return 0;
    }

    /* Add IP to IOV */
    mk_api->iov_add_entry(iov, ip_str->data, ip_str->len, mk_iov_none, 
                          MK_IOV_NOT_FREE_BUF);
    mk_api->iov_add_entry(iov, " - ", 3, mk_iov_none, MK_IOV_NOT_FREE_BUF);
    /* Date/time when object was requested */
    date = mk_api->time_human();
    mk_api->iov_add_entry(iov, date->data, date->len,
                          mk_logger_iov_space, MK_IOV_NOT_FREE_BUF);

    /* Access Log */
    if (http_status < 400){
        /* No access file defined */
        if (!target->file_access) {
            return 0;
        }

        /* HTTP Method */
        mk_api->iov_add_entry(iov, 
                              sr->method_p.data, 
                              sr->method_p.len, 
                              mk_logger_iov_space, MK_IOV_NOT_FREE_BUF);

        /* HTTP URI required */
        mk_api->iov_add_entry(iov, sr->uri.data, sr->uri.len,
                              mk_logger_iov_space, MK_IOV_NOT_FREE_BUF);

        /* HTTP Protocol */
        mk_api->iov_add_entry(iov, sr->protocol_p.data, sr->protocol_p.len,
                              mk_logger_iov_space, MK_IOV_NOT_FREE_BUF);

        /* HTTP Status code response */
        for (i=0; i < array_len; i++) {
            if (response_codes[i].i_status == http_status) {
                break;
            }
        }

        if (array_len == i) {
            mk_api->str_itop(http_status, &status);
            status.len -= 2;
        }
        else {
            status.data = response_codes[i].s_status;
            status.len  = 3;
        }
        mk_api->iov_add_entry(iov, 
                              status.data,
                              status.len,
                              mk_logger_iov_space, MK_IOV_NOT_FREE_BUF);

        /* Content Length */
        if (sr->method != HTTP_METHOD_HEAD) {
            /* Int to mk_pointer */
            content_length = pthread_getspecific(cache_content_length);
            mk_api->str_itop(sr->headers.content_length, content_length);

            mk_api->iov_add_entry(iov,
                                  content_length->data, content_length->len - 2, 
                                  mk_logger_iov_lf, MK_IOV_NOT_FREE_BUF);
        }
        else {
            mk_api->iov_add_entry(iov,
                                  mk_logger_iov_empty.data,
                                  mk_logger_iov_empty.len, 
                                  mk_logger_iov_lf, MK_IOV_NOT_FREE_BUF);
        }

        /* Write iov array to pipe */
        mk_api->iov_send(target->fd_access[1], iov);
    }
    else {
        if (!target->file_error) {
            return 0;
        }
        switch (http_status) {
        case MK_CLIENT_BAD_REQUEST:
            mk_api->iov_add_entry(iov,
                                  error_msg_400.data,
                                  error_msg_400.len,
                                  mk_logger_iov_lf, MK_IOV_NOT_FREE_BUF);
            break;
        case MK_CLIENT_FORBIDDEN:
            mk_api->iov_add_entry(iov,
                                  error_msg_403.data,
                                  error_msg_403.len,
                                  mk_logger_iov_space, MK_IOV_NOT_FREE_BUF);
            mk_api->iov_add_entry(iov,
                                  sr->uri.data,
                                  sr->uri.len,
                                  mk_logger_iov_lf, MK_IOV_NOT_FREE_BUF);
            break;
        case MK_CLIENT_NOT_FOUND:
            mk_api->iov_add_entry(iov,
                                  error_msg_404.data,
                                  error_msg_404.len,
                                  mk_logger_iov_space, MK_IOV_NOT_FREE_BUF);
            mk_api->iov_add_entry(iov,
                                  sr->uri.data,
                                  sr->uri.len,
                                  mk_logger_iov_lf, MK_IOV_NOT_FREE_BUF);
            break;
        case MK_CLIENT_METHOD_NOT_ALLOWED:
            mk_api->iov_add_entry(iov,
                                  error_msg_405.data,
                                  error_msg_405.len,
                                  mk_logger_iov_space, MK_IOV_NOT_FREE_BUF);
            mk_api->iov_add_entry(iov,
                                  sr->method_p.data,
                                  sr->method_p.len,
                                  mk_logger_iov_lf, MK_IOV_NOT_FREE_BUF);
            break;
        case MK_CLIENT_REQUEST_TIMEOUT:
            mk_api->iov_add_entry(iov,
                                  error_msg_408.data,
                                  error_msg_408.len,
                                  mk_logger_iov_lf, MK_IOV_NOT_FREE_BUF);
            break;
        case MK_CLIENT_LENGTH_REQUIRED:
            mk_api->iov_add_entry(iov,
                                  error_msg_411.data,
                                  error_msg_411.len,
                                  mk_logger_iov_lf, MK_IOV_NOT_FREE_BUF);
            break;
        case MK_CLIENT_REQUEST_ENTITY_TOO_LARGE:
            mk_api->iov_add_entry(iov,
                                  error_msg_413.data,
                                  error_msg_413.len,
                                  mk_logger_iov_lf, MK_IOV_NOT_FREE_BUF);
            break;
        case MK_SERVER_NOT_IMPLEMENTED:
            mk_api->iov_add_entry(iov,
                                  error_msg_501.data,
                                  error_msg_501.len,
                                  mk_logger_iov_space, MK_IOV_NOT_FREE_BUF);

            mk_api->iov_add_entry(iov,
                                  sr->method_p.data,
                                  sr->method_p.len,
                                  mk_logger_iov_lf, MK_IOV_NOT_FREE_BUF);
            break;
        case MK_SERVER_INTERNAL_ERROR:
            mk_api->iov_add_entry(iov,
                                  error_msg_500.data,
                                  error_msg_500.len,
                                  mk_logger_iov_space, MK_IOV_NOT_FREE_BUF);
            break;
        case MK_SERVER_HTTP_VERSION_UNSUP:
            mk_api->iov_add_entry(iov,
                                  error_msg_505.data,
                                  error_msg_505.len,
                                  mk_logger_iov_lf, MK_IOV_NOT_FREE_BUF);
            break;
        }
        /* Write iov array to pipe */
        mk_api->iov_send(target->fd_error[1], iov);
    }

    return 0;
}
