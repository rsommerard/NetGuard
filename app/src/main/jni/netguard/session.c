/*
    This file is part of NetGuard.

    NetGuard is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    NetGuard is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with NetGuard.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2015-2017 by Marcel Bokhorst (M66B)
*/

#include "netguard.h"

extern JavaVM *jvm;
extern int pipefds[2];
extern pthread_t thread_id;
extern pthread_mutex_t lock;

struct ng_session *ng_session = NULL;

void init(const struct arguments *args) {
    ng_session = NULL;
}

void clear() {
    struct ng_session *s = ng_session;

    while (s != NULL) {
        if (s->socket >= 0 && close(s->socket)) {
            log_android(ANDROID_LOG_ERROR, "close %d error %d: %s", s->socket, errno, strerror(errno));
        }

        if (s->protocol == IPPROTO_TCP) {
            clear_tcp_data(&s->tcp);
        }

        struct ng_session *p = s;

        s = s->next;

        free(p);
    }

    ng_session = NULL;
}

void *handle_events(void *a) {
    struct arguments *args = (struct arguments *) a;
    // log_android(ANDROID_LOG_WARN, "Start events tun=%d thread %x", args->tun, thread_id);

    // Attach to Java
    JNIEnv *env;
    jint rs = (*jvm)->AttachCurrentThread(jvm, &env, NULL);

    if (rs != JNI_OK) {
        log_android(ANDROID_LOG_ERROR, "AttachCurrentThread failed");
        return NULL;
    }

    args->env = env;

    // Get max number of sessions
    int maxsessions = SESSION_MAX;
    struct rlimit rlim;

    if (getrlimit(RLIMIT_NOFILE, &rlim)) {
        // log_android(ANDROID_LOG_WARN, "getrlimit error %d: %s", errno, strerror(errno));
    } else {
        maxsessions = (int) (rlim.rlim_cur * SESSION_LIMIT / 100);

        if (maxsessions > SESSION_MAX) {
            maxsessions = SESSION_MAX;
        }

        log_android(ANDROID_LOG_WARN, "getrlimit soft %d hard %d max sessions %d", rlim.rlim_cur, rlim.rlim_max, maxsessions);
    }

    int stopping = 0;

    // Open epoll file
    int epoll_fd = epoll_create(1);

    if (epoll_fd < 0) {
        log_android(ANDROID_LOG_ERROR, "epoll create error %d: %s", errno, strerror(errno));
        report_exit(args, "epoll create error %d: %s", errno, strerror(errno));
        stopping = 1;
    }

    // Monitor stop events
    struct epoll_event ev_pipe;
    memset(&ev_pipe, 0, sizeof(struct epoll_event));
    ev_pipe.events = EPOLLIN | EPOLLERR;
    ev_pipe.data.ptr = &ev_pipe;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, pipefds[0], &ev_pipe)) {
        log_android(ANDROID_LOG_ERROR, "epoll add pipe error %d: %s", errno, strerror(errno));
        report_exit(args, "epoll add pipe error %d: %s", errno, strerror(errno));
        stopping = 1;
    }

    // Monitor tun events
    struct epoll_event ev_tun;
    memset(&ev_tun, 0, sizeof(struct epoll_event));
    ev_tun.events = EPOLLIN | EPOLLERR;
    ev_tun.data.ptr = NULL;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, args->tun, &ev_tun)) {
        log_android(ANDROID_LOG_ERROR, "epoll add tun error %d: %s", errno, strerror(errno));
        report_exit(args, "epoll add tun error %d: %s", errno, strerror(errno));
        stopping = 1;
    }

    // Loop
    long long last_check = 0;

    while (!stopping) {
        // log_android(ANDROID_LOG_DEBUG, "Loop thread %x", thread_id);

        int recheck = 0;
        int timeout = EPOLL_TIMEOUT;

        // Count sessions
        int isessions = 0;
        int usessions = 0;
        int tsessions = 0;
        struct ng_session *s = ng_session;

        while (s != NULL) {
            if (s->protocol == IPPROTO_ICMP || s->protocol == IPPROTO_ICMPV6) {
                if (!s->icmp.stop) {
                    isessions++;
                }
            } else if (s->protocol == IPPROTO_UDP) {
                if (s->udp.state == UDP_ACTIVE) {
                    usessions++;
                }
            } else if (s->protocol == IPPROTO_TCP) {
                if (s->tcp.state != TCP_CLOSING && s->tcp.state != TCP_CLOSE) {
                    tsessions++;
                }

                if (s->socket >= 0) {
                    recheck = recheck | monitor_tcp_session(args, s, epoll_fd);
                }
            }

            s = s->next;
        }

        int sessions = isessions + usessions + tsessions;

        // Check sessions
        long long ms = get_ms();

        if (ms - last_check > EPOLL_MIN_CHECK) {
            last_check = ms;

            time_t now = time(NULL);
            struct ng_session *sl = NULL;
            s = ng_session;

            while (s != NULL) {
                int del = 0;

                if (s->protocol == IPPROTO_ICMP || s->protocol == IPPROTO_ICMPV6) {
                    del = check_icmp_session(args, s, sessions, maxsessions);

                    if (!s->icmp.stop && !del) {
                        int stimeout = s->icmp.time + get_icmp_timeout(&s->icmp, sessions, maxsessions) - now + 1;

                        if (stimeout > 0 && stimeout < timeout) {
                            timeout = stimeout;
                        }
                    }
                } else if (s->protocol == IPPROTO_UDP) {
                    del = check_udp_session(args, s, sessions, maxsessions);

                    if (s->udp.state == UDP_ACTIVE && !del) {
                        int stimeout = s->udp.time + get_udp_timeout(&s->udp, sessions, maxsessions) - now + 1;

                        if (stimeout > 0 && stimeout < timeout) {
                            timeout = stimeout;
                        }
                    }
                } else if (s->protocol == IPPROTO_TCP) {
                    del = check_tcp_session(args, s, sessions, maxsessions);

                    if (s->tcp.state != TCP_CLOSING && s->tcp.state != TCP_CLOSE && !del) {
                        int stimeout = s->tcp.time +
                                       get_tcp_timeout(&s->tcp, sessions, maxsessions) - now + 1;

                        if (stimeout > 0 && stimeout < timeout) {
                            timeout = stimeout;
                        }
                    }
                }

                if (del) {
                    if (sl == NULL) {
                        ng_session = s->next;
                    } else {
                        sl->next = s->next;
                    }

                    struct ng_session *c = s;

                    s = s->next;

                    if (c->protocol == IPPROTO_TCP) {
                        clear_tcp_data(&c->tcp);
                    }

                    free(c);
                } else {
                    sl = s;
                    s = s->next;
                }
            }
        } else {
            recheck = 1;
            // log_android(ANDROID_LOG_DEBUG, "Skipped session checks");
        }

        // log_android(ANDROID_LOG_DEBUG, "sessions ICMP %d UDP %d TCP %d max %d/%d timeout %d recheck %d", isessions, usessions, tsessions, sessions, maxsessions, timeout, recheck);

        // Poll
        struct epoll_event ev[EPOLL_EVENTS];
        int ready = epoll_wait(epoll_fd, ev, EPOLL_EVENTS,
                               recheck ? EPOLL_MIN_CHECK : timeout * 1000);

        if (ready < 0) {
            if (errno == EINTR) {
                // log_android(ANDROID_LOG_DEBUG, "epoll interrupted tun %d thread %x", args->tun, thread_id);
                continue;
            } else {
                log_android(ANDROID_LOG_ERROR, "epoll tun %d thread %x error %d: %s", args->tun, thread_id, errno, strerror(errno));
                report_exit(args, "epoll tun %d thread %x error %d: %s", args->tun, thread_id, errno, strerror(errno));
                break;
            }
        }

        if (ready == 0) {
            // log_android(ANDROID_LOG_DEBUG, "epoll timeout");
        } else {

            if (pthread_mutex_lock(&lock)) {
                log_android(ANDROID_LOG_ERROR, "pthread_mutex_lock failed");
            }

            int error = 0;

            for (int i = 0; i < ready; i++) {
                if (ev[i].data.ptr == &ev_pipe) {
                    // Check pipe
                    stopping = 1;
                    uint8_t buffer[1];

                    if (read(pipefds[0], buffer, 1) < 0) {
                        // log_android(ANDROID_LOG_WARN, "Read pipe error %d: %s", errno, strerror(errno));
                    } else {
                        // log_android(ANDROID_LOG_WARN, "Read pipe");
                    }

                    break;

                } else if (ev[i].data.ptr == NULL) {
                    // Check upstream
                    // log_android(ANDROID_LOG_DEBUG, "epoll ready %d/%d in %d out %d err %d hup %d", i, ready, (ev[i].events & EPOLLIN) != 0, (ev[i].events & EPOLLOUT) != 0, (ev[i].events & EPOLLERR) != 0, (ev[i].events & EPOLLHUP) != 0);

                    while (!error && is_readable(args->tun))
                        if (check_tun(args, &ev[i], epoll_fd, sessions, maxsessions) < 0) {
                            error = 1;
                        }

                } else {
                    // Check downstream
                    // log_android(ANDROID_LOG_DEBUG, "epoll ready %d/%d in %d out %d err %d hup %d prot %d sock %d", i, ready, (ev[i].events & EPOLLIN) != 0, (ev[i].events & EPOLLOUT) != 0, (ev[i].events & EPOLLERR) != 0, (ev[i].events & EPOLLHUP) != 0, ((struct ng_session *) ev[i].data.ptr)->protocol, ((struct ng_session *) ev[i].data.ptr)->socket);

                    struct ng_session *session = (struct ng_session *) ev[i].data.ptr;

                    if (session->protocol == IPPROTO_ICMP || session->protocol == IPPROTO_ICMPV6) {
                        check_icmp_socket(args, &ev[i]);
                    } else if (session->protocol == IPPROTO_UDP) {
                        while (!(ev[i].events & EPOLLERR) && (ev[i].events & EPOLLIN) && is_readable(session->socket)) {
                            check_udp_socket(args, &ev[i]);
                        }
                    } else if (session->protocol == IPPROTO_TCP) {
                        check_tcp_socket(args, &ev[i], epoll_fd);
                    }
                }

                if (error) {
                    break;
                }
            }

            if (pthread_mutex_unlock(&lock)) {
                log_android(ANDROID_LOG_ERROR, "pthread_mutex_unlock failed");
            }

            if (error) {
                break;
            }
        }
    }

    // Close epoll file
    if (epoll_fd >= 0 && close(epoll_fd)) {
        log_android(ANDROID_LOG_ERROR, "epoll close error %d: %s", errno, strerror(errno));
    }

    (*env)->DeleteGlobalRef(env, args->instance);

    // Detach from Java
    rs = (*jvm)->DetachCurrentThread(jvm);

    if (rs != JNI_OK) {
        log_android(ANDROID_LOG_ERROR, "DetachCurrentThread failed");
    }

    // Cleanup
    free(args);

    // log_android(ANDROID_LOG_WARN, "Stopped events tun=%d thread %x", args->tun, thread_id);
    thread_id = 0;
    return NULL;
}
