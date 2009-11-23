/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */

/*  Monkey HTTP Daemon
 *  ------------------
 *  Copyright (C) 2008, Eduardo Silva P.
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
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <string.h>

#include "monkey.h"
#include "conn_switch.h"
#include "signal.h"
#include "scheduler.h"
#include "memory.h"
#include "epoll.h"
#include "request.h"
#include "cache.h"
#include "config.h"
#include "clock.h"

/* Register thread information */
int mk_sched_register_thread(pthread_t tid, int efd)
{
        int i;
	struct sched_list_node *sr, *aux;

	sr = mk_mem_malloc_z(sizeof(struct sched_list_node)); 
	sr->tid = tid;
        sr->pid = -1;
	sr->epoll_fd = efd;
        sr->queue = mk_mem_malloc_z(sizeof(struct sched_connection)*
                                    config->worker_capacity);
	sr->request_handler = NULL;
	sr->next = NULL;

        for(i=0; i<config->worker_capacity; i++){
                sr->queue[i].status = MK_SCHEDULER_CONN_AVAILABLE;
        }

	if(!sched_list)
	{
		sr->idx = 1;
		sched_list = sr;
		return 0;
	}

	aux = sched_list;
	while(aux->next)
	{
		aux = aux->next;
	}
	sr->idx = aux->idx + 1;
	aux->next = sr;

	return 0;
}

/*
 * Create thread which will be listening 
 * for incomings file descriptors
 */
int mk_sched_launch_thread(int max_events)
{
	int efd;
	pthread_t tid;
	pthread_attr_t attr;
        sched_thread_conf *thconf;
	pthread_mutex_t mutex_wait_register;

	/* Creating epoll file descriptor */
	efd = mk_epoll_create(max_events);
	if(efd < 1)
	{
		return -1;
	}
	
	/* Thread stuff */
	pthread_mutex_init(&mutex_wait_register,(pthread_mutexattr_t *) NULL);
	pthread_mutex_lock(&mutex_wait_register);

	thconf = mk_mem_malloc(sizeof(sched_thread_conf));
	thconf->epoll_fd = efd;
	thconf->max_events = max_events;

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if(pthread_create(&tid, &attr, mk_sched_launch_epoll_loop,
				(void *) thconf)!=0)
	{
		perror("pthread_create");
		return -1;
	}

	/* Register working thread */
	mk_sched_register_thread(tid, efd);
	pthread_mutex_unlock(&mutex_wait_register);
	return 0;
}

/* created thread, all this calls are in the thread context */
void *mk_sched_launch_epoll_loop(void *thread_conf)
{
	sched_thread_conf *thconf;
        struct sched_list_node *thinfo;

        /* Avoid SIGPIPE signals */
        mk_signal_thread_sigpipe_safe();

	thconf = thread_conf;

        /* Init specific thread cache */
        mk_cache_thread_init();

	mk_epoll_calls *callers;
	callers = mk_epoll_set_callers((void *)mk_conn_switch,
			MK_CONN_SWITCH_READ, 
			MK_CONN_SWITCH_WRITE);
        
        /* Nasty way to export task id */
        usleep(1000);
        thinfo = mk_sched_get_thread_conf();
        while(!thinfo){
                thinfo = mk_sched_get_thread_conf();
        }
        
        /* Glibc doesn't export to user space the gettid() syscall */
        thinfo->pid = syscall(__NR_gettid);

	mk_sched_set_thread_poll(thconf->epoll_fd);
	mk_epoll_init(thconf->epoll_fd, callers, thconf->max_events);

	return 0;
}

struct request_idx *mk_sched_get_request_index()
{
	return (struct request_idx *) pthread_getspecific(request_index);
}

void mk_sched_set_request_index(struct request_idx *ri)
{
	pthread_setspecific(request_index, (void *)ri);
}

void mk_sched_set_thread_poll(int epoll)
{
	pthread_setspecific(epoll_fd, (void *) epoll);
}

int mk_sched_get_thread_poll()
{
	return (int) pthread_getspecific(epoll_fd);
}

struct sched_list_node *mk_sched_get_thread_conf()
{
        struct sched_list_node *node;
        pthread_t current;

        current = pthread_self();
        node = sched_list;
        while(node){
                if(pthread_equal(node->tid, current) != 0){
                        return node;
                }
                node = node->next;
        }

        return NULL;
}


void mk_sched_update_thread_status(int active, int closed)
{
        struct sched_list_node *thnode;

        thnode = mk_sched_get_thread_conf();

        switch(active){
                case MK_SCHEDULER_ACTIVE_UP:
                        thnode->active_requests++;
                        break;
                case MK_SCHEDULER_ACTIVE_DOWN:
                        thnode->active_requests--;
                        break;
        }

        switch(closed){
                case MK_SCHEDULER_CLOSED_UP:
                        thnode->closed_requests++;
                        break;
                case MK_SCHEDULER_CLOSED_DOWN:
                        thnode->closed_requests--;
                        break;
        }
}

int mk_sched_add_client(struct sched_list_node **sched, int remote_fd)
{
        int i;
        struct sched_list_node *l;

        l = (struct sched_list_node *) *sched;

        /* Look for an available slot */
        for(i=0; i<config->worker_capacity; i++){
                if(l->queue[i].status == MK_SCHEDULER_CONN_AVAILABLE){
                        l->queue[i].socket = remote_fd;
                        l->queue[i].status = MK_SCHEDULER_CONN_PENDING;
                        l->queue[i].arrive_time = log_current_utime;

                        mk_epoll_add_client(l->epoll_fd, remote_fd,
                                            MK_EPOLL_BEHAVIOR_TRIGGERED);
                        return 0;
                }
        }

        return -1;
}

int mk_sched_remove_client(struct sched_list_node **sched, int remote_fd)
{
        struct sched_connection *sc;

        sc = mk_sched_get_connection(sched, remote_fd);
        if(sc){
                close(remote_fd);
                sc->status = MK_SCHEDULER_CONN_AVAILABLE;
                return 0;
        }
        return -1;
}

struct sched_connection *mk_sched_get_connection(struct sched_list_node **sched, 
                                                 int remote_fd)
{
        int i;
        struct sched_list_node *l;

        if(!sched){
                l = mk_sched_get_thread_conf();
        }
        else{
                l = (struct sched_list_node *) *sched;
        }

        for(i=0; i<config->worker_capacity; i++){
                if(l->queue[i].socket == remote_fd){
                        return &l->queue[i];
                }
        }

        return NULL;
}

int mk_sched_check_timeouts(struct sched_list_node **sched)
{
        int i;
        struct request_idx *req_idx;
        struct client_request *req_cl;
        struct sched_list_node *l;

        l = (struct sched_list_node *) *sched;

        /* PENDING CONN TIMEOUT */
        for(i=0; i<config->worker_capacity; i++){
                if(l->queue[i].status == MK_SCHEDULER_CONN_PENDING){
                        if(l->queue[i].arrive_time + config->timeout <= log_current_utime){
                                mk_sched_remove_client(&l, l->queue[i].socket);
                        }
                }
        }

        /* PROCESSING CONN TIMEOUT */
        req_idx = mk_sched_get_request_index();
        req_cl = req_idx->first;
        
        while(req_cl){
                if(req_cl->status == MK_REQUEST_STATUS_INCOMPLETE){
                        if((req_cl->init_time + config->timeout) >= 
                           log_current_utime){
                                close(req_cl->socket);
                        }
                }
                req_cl = req_cl->next;
        }

        return 0;
}
