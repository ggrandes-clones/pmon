/*
 * improved pmon3
 * Name: pmon4.c
 * Compile: gcc pmon4.c -o pmon4
 * License: GNU GPL v2 (see LICENSE)
 */
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/connector.h>
#include <linux/cn_proc.h>
#include <signal.h>
#include <errno.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <getopt.h>

/*
 * module variables
 */
static volatile bool need_exit = false;
static bool doDebug = false;
static bool doTimeStamp = true;
static time_t last_now = 0;
static time_t now;
static char timestamp_buffer[64];
static bool exitIfParentDies = false;
static pid_t saved_ppid = 0;

/*
 * connect to netlink
 * returns netlink socket, or -1 on error
 */
static int nl_connect() {
    int rc;
    int nl_sock;
    struct sockaddr_nl sa_nl;

    nl_sock = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_CONNECTOR);
    if (nl_sock == -1) {
        perror("error: socket");
        return -1;
    }

    struct timeval timeout;      
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;

    rc = setsockopt(nl_sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));
    if (rc == -1) {
        perror("error: setsockopt");
        close(nl_sock);
        return -1;
    }

    sa_nl.nl_family = AF_NETLINK;
    sa_nl.nl_groups = CN_IDX_PROC;
    sa_nl.nl_pid = getpid();

    rc = bind(nl_sock, (struct sockaddr *)&sa_nl, sizeof(sa_nl));
    if (rc == -1) {
        perror("error: bind");
        close(nl_sock);
        return -1;
    }

    return nl_sock;
}

/*
 * subscribe on proc events (process notifications)
 */
static int set_proc_ev_listen(int nl_sock, bool enable) {
    int rc;
    struct __attribute__ ((aligned(NLMSG_ALIGNTO))) {
        struct nlmsghdr nl_hdr;
        struct __attribute__ ((__packed__)) {
            struct cn_msg cn_msg;
            enum proc_cn_mcast_op cn_mcast;
        };
    } nlcn_msg;

    memset(&nlcn_msg, 0, sizeof(nlcn_msg));
    nlcn_msg.nl_hdr.nlmsg_len = sizeof(nlcn_msg);
    nlcn_msg.nl_hdr.nlmsg_pid = getpid();
    nlcn_msg.nl_hdr.nlmsg_type = NLMSG_DONE;

    nlcn_msg.cn_msg.id.idx = CN_IDX_PROC;
    nlcn_msg.cn_msg.id.val = CN_VAL_PROC;
    nlcn_msg.cn_msg.len = sizeof(enum proc_cn_mcast_op);

    nlcn_msg.cn_mcast = (enable ? PROC_CN_MCAST_LISTEN : PROC_CN_MCAST_IGNORE);

    rc = send(nl_sock, &nlcn_msg, sizeof(nlcn_msg), 0);
    if (rc == -1) {
        perror("error: netlink send");
        return -1;
    }

    return 0;
}

/*
 * fill timetamp buffer
 */
static int fillTimeStamp(void) {
    struct tm *tmp;

    tmp = localtime(&now);
    if (tmp == NULL) {
        perror("error: localtime");
        return -1;
    }

    // "%Y-%m-%d %H:%M:%S " "%F %T "
    // "%F %T %s " => "2013-10-27 18:48:11 1382896091 "
    if (strftime(timestamp_buffer, sizeof(timestamp_buffer), "%F %T %s ", tmp) == 0) {
        perror("error: strftime");
        return -1;
    }
    return 0;
}

/*
 * print timetamp of event
 */
static int printTimeStamp(void) {
    fprintf(stdout, "%s", timestamp_buffer);
    return 0;
}

/*
 * check if parent dies
 */
static void checkParentAlive(void) {
    pid_t current_ppid = getppid();
    if (saved_ppid == 0) {
        saved_ppid = current_ppid;
    }
    if (saved_ppid != current_ppid) {
        if (doDebug) {
            if (doTimeStamp)
                printTimeStamp();
            fprintf(stdout, "event=debug saved_ppid=%d current_ppid=%d\n", saved_ppid, current_ppid);
        }
        need_exit = true;
    }
}
static void handle_task(void) {
    fillTimeStamp();
    if (doDebug) {
        if (doTimeStamp)
            printTimeStamp();
        fprintf(stdout, "event=debug text=handle_task\n");
    }
    if (exitIfParentDies) {
        checkParentAlive();
    }
}

/*
 * handle a single process event
 */
static int handle_proc_ev(int nl_sock) {
    int rc;
    struct __attribute__ ((aligned(NLMSG_ALIGNTO))) {
        struct nlmsghdr nl_hdr;
        struct __attribute__ ((__packed__)) {
            struct cn_msg cn_msg;
            struct proc_event proc_ev;
        };
    } nlcn_msg;

    while (!need_exit) {
        now = time(NULL);
        if (now > last_now) {
            handle_task();
            last_now = now;
        }
        rc = recv(nl_sock, &nlcn_msg, sizeof(nlcn_msg), 0);
        if (rc == 0) {
            /* shutdown? */
            return 0;
        }
        else if (rc == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EWOULDBLOCK) {
                continue;
            }
            perror("error: netlink recv");
            if (errno == ENOBUFS) {
                continue;
            }
            return -1;
        }
        switch (nlcn_msg.proc_ev.what) {
            case PROC_EVENT_NONE: {
                if (doTimeStamp)
                    printTimeStamp();
                fprintf(stdout, "event=nop\n");
                break;
            }
            case PROC_EVENT_FORK: {
                if (nlcn_msg.proc_ev.event_data.fork.child_pid != 
                    nlcn_msg.proc_ev.event_data.fork.child_tgid)
                    continue;
                if (doTimeStamp)
                    printTimeStamp();
                fprintf(stdout, "event=fork parent.pid=%d child.pid=%d\n",
                nlcn_msg.proc_ev.event_data.fork.parent_pid,
                nlcn_msg.proc_ev.event_data.fork.child_pid);
                break;
            }
            case PROC_EVENT_EXIT: {
                if (nlcn_msg.proc_ev.event_data.exit.process_pid != 
                    nlcn_msg.proc_ev.event_data.exit.process_tgid)
                    continue;
                if (doTimeStamp)
                    printTimeStamp();
                fprintf(stdout, "event=exit process.pid=%d exit.code=%d\n",
                nlcn_msg.proc_ev.event_data.exit.process_pid,
                nlcn_msg.proc_ev.event_data.exit.exit_code);
                break;
            }
            default: {
                continue;
            }
        }
        fflush(stdout);
    }

    return 0;
}

static void on_sigint(int unused) {
    need_exit = true;
}

static void usage(char *p) {
    printf(
        "Usage: %s [options]\n"
        "\n"
        "\t-h\tthis help screen\n"
        "\t-t\tdisable timestamp\n"
        "\t-p\texit if parent dies\n"
        "\t-d\tprint debugging information\n"
        "\n"
        , p
    );
}

int main(int argc, char *argv[]) {
    int nl_sock;
    int rc = EXIT_SUCCESS;
    int s;

    while ((s = getopt(argc, argv, "hpdt")) != -1) {
        switch (s) {
            case 't': {
                doTimeStamp = false;
                break;
            }
            case 'p': {
                exitIfParentDies = true;
                break;
            }
            case 'd': {
                doDebug = true;
                break;
            }
            case 'h': {
                usage(argv[0]);
                return 0;
            }
            default: {
                /* getopt() outputs an error for us */
                usage(argv[0]);
                return 1;
            }
        }
    }

    if (geteuid() != 0) {
        printf("need root permissions\n");
        exit(EXIT_FAILURE);
    }

    memset(&timestamp_buffer, 0, sizeof(timestamp_buffer));
    signal(SIGINT, &on_sigint);
    siginterrupt(SIGINT, true);
    checkParentAlive();

    nl_sock = nl_connect();
    if (nl_sock == -1) {
        exit(EXIT_FAILURE);
    }
    rc = set_proc_ev_listen(nl_sock, true);
    if (rc == -1) {
        rc = EXIT_FAILURE;
        goto out;
    }

    rc = handle_proc_ev(nl_sock);
    if (rc == -1) {
        rc = EXIT_FAILURE;
        goto out;
    }

    set_proc_ev_listen(nl_sock, false);

out:
    close(nl_sock);
    exit(rc);
}
