#define _GNU_SOURCE
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <poll.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/signalfd.h>
#include <fcntl.h>
#include <time.h>
#include <stdarg.h>

#include <sys/fanotify.h>

void debug_printf(char *format, ...)
{
#ifdef DEBUG
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
#endif
    return;
}

/* Structure to keep track of monitored directories */
typedef struct {
  /* Path of the directory */
  char *path;
} monitored_t;

/* Size of buffer to use when reading fanotify-cmdline events */
#define FANOTIFY_BUFFER_SIZE 8192

/* Enumerate list of FDs to poll */
enum {
  FD_POLL_SIGNAL = 0,
  FD_POLL_FANOTIFY,
  FD_POLL_MAX
};

/* Setup fanotify notifications (FAN) mask. All these defined in fanotify.h. */
static uint64_t event_mask =
    (FAN_ACCESS |         /* File accessed */
        FAN_MODIFY |         /* File modified */
        FAN_CLOSE_WRITE |    /* Writtable file closed */
        FAN_CLOSE_NOWRITE |  /* Unwrittable file closed */
        FAN_OPEN |           /* File was opened */
        FAN_ONDIR |          /* We want to be reported of events in the directory */
        FAN_EVENT_ON_CHILD); /* We want to be reported of events in files of the directory */

/* Array of directories being monitored */
static monitored_t *monitors;
static int n_monitors;

static char *get_program_cmdline_from_pid(int pid, char *buffer, size_t buffer_size) {
    int i;
    int fd;
    ssize_t len;

    /* Try to get program name by PID */
    sprintf(buffer, "/proc/%d/cmdline", pid);
    if ((fd = open(buffer, O_RDONLY)) < 0)
        return NULL;

    /* Read file contents into buffer */
    if ((len = read(fd, buffer, buffer_size - 1)) <= 0) {
        close(fd);
        return NULL;
    }
    close(fd);

    for (i = 0; i < len; i++) {
        if (buffer[i] == '\0') {
            buffer[i] = ' ';
        }
    }

    buffer[len] = '\0';

    return buffer;
}

static char *get_file_path_from_fd(int fd, char *buffer, size_t buffer_size) {
    ssize_t len;
    char fdpath[PATH_MAX];

    if (fd <= 0)
        return NULL;

    sprintf(fdpath, "/proc/self/fd/%d", fd);
    if ((len = readlink(fdpath, buffer, buffer_size - 1)) < 0)
        return NULL;

    buffer[len] = '\0';
    return buffer;
}

static void event_process(struct fanotify_event_metadata *event) {
    char path[PATH_MAX];
    time_t current_time;
    char *c_time_string;

    current_time = time(NULL);
    c_time_string = ctime(&current_time);

    printf("%s [%d] Event on '%s':\n",
           strtok(c_time_string, "\n"),
           event->pid,
           get_file_path_from_fd(event->fd, path, PATH_MAX) ? path : "unknown");

    printf("%s [%d] Event: ", strtok(c_time_string, "\n"), event->pid);
    if (event->mask & FAN_OPEN)
        printf("FAN_OPEN ");
    if (event->mask & FAN_ACCESS)
        printf("FAN_ACCESS ");
    if (event->mask & FAN_MODIFY)
        printf("FAN_MODIFY ");
    if (event->mask & FAN_CLOSE_WRITE)
        printf("FAN_CLOSE_WRITE ");
    if (event->mask & FAN_CLOSE_NOWRITE)
        printf("FAN_CLOSE_NOWRITE ");
    printf("\n");

    printf("%s [%d] Cmdline: %s\n\n",
           strtok(c_time_string, "\n"),
           event->pid,
           (get_program_cmdline_from_pid(event->pid, path, PATH_MAX) ? path : "unknown"));

    fflush(stdout);

    close(event->fd);
}

static void shutdown_fanotify(int fanotify_fd) {
    int i;

    for (i = 0; i < n_monitors; ++i) {
        /* Remove the mark, using same event mask as when creating it */
        fanotify_mark(fanotify_fd,
                      FAN_MARK_REMOVE,
                      event_mask,
                      AT_FDCWD,
                      monitors[i].path);
        free(monitors[i].path);
    }
    free(monitors);
    close(fanotify_fd);
}

static int initialize_fanotify(int argc, const char **argv) {
    int i;
    int fanotify_fd;

    /* Create new fanotify-cmdline device */
    if ((fanotify_fd = fanotify_init(FAN_CLOEXEC,
                                     O_RDONLY | O_CLOEXEC | O_LARGEFILE)) < 0) {
        fprintf(stderr,
                "Couldn't setup new fanotify-cmdline device: %s\n",
                strerror(errno));
        return -1;
    }

    /* Allocate array of monitor setups */
    n_monitors = argc - 1;
    monitors = malloc(n_monitors * sizeof(monitored_t));

    /* Loop all input directories, setting up marks */
    for (i = 0; i < n_monitors; ++i) {
        monitors[i].path = strdup(argv[i + 1]);
        /* Add new fanotify-cmdline mark */
        if (fanotify_mark(fanotify_fd,
                          FAN_MARK_ADD,
                          event_mask,
                          AT_FDCWD,
                          monitors[i].path) < 0) {
            fprintf(stderr,
                    "Couldn't add monitor in directory '%s': '%s'\n",
                    monitors[i].path,
                    strerror(errno));
            return -1;
        }

        printf("Started monitoring '%s'...\n",
               monitors[i].path);
    }

    return fanotify_fd;
}

static void shutdown_signals(int signal_fd) {
    close(signal_fd);
}

static int initialize_signals(void) {
    int signal_fd;
    sigset_t sigmask;

    /* We want to handle SIGINT and SIGTERM in the signal_fd, so we block them. */
    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGINT);
    sigaddset(&sigmask, SIGTERM);

    if (sigprocmask(SIG_BLOCK, &sigmask, NULL) < 0) {
        fprintf(stderr,
                "Couldn't block signals: '%s'\n",
                strerror(errno));
        return -1;
    }

    /* Get new FD to read signals from it */
    if ((signal_fd = signalfd(-1, &sigmask, 0)) < 0) {
        fprintf(stderr,
                "Couldn't setup signal FD: '%s'\n",
                strerror(errno));
        return -1;
    }

    return signal_fd;
}

uint64_t getmask(const char *name) {
    const char *prefix = "FAN_";
    size_t len = strlen(prefix);
    struct {
        const char*    name;
        const uint64_t value;
    } fanmask[] = {
        {"ACCESS",         FAN_ACCESS},
        {"MODIFY",         FAN_MODIFY},
        {"CLOSE_WRITE",    FAN_CLOSE_WRITE},
        {"CLOSE_NOWRITE",  FAN_CLOSE_NOWRITE},
        {"OPEN",           FAN_OPEN},
        {"ONDIR",          FAN_ONDIR},
        {"EVENT_ON_CHILD", FAN_EVENT_ON_CHILD},
        {NULL,             0}
    };
    char *body = strcasestr(name, prefix);
    if (body == NULL) {
        body = (char*)name;
    } else {
        body += len;
    }
    debug_printf("body = %s\n", body);
    int i = 0;
    do {
        if (strcasecmp(body, fanmask[i].name) == 0) return fanmask[i].value;
    } while (fanmask[++i].name != NULL);
    return 0;
}

int main(int argc,
         const char **argv) {
    int signal_fd;
    int fanotify_fd;
    struct pollfd fds[FD_POLL_MAX];
    uint64_t mask;
    int i = 0;

    /* Input arguments... */
    debug_printf("argc = %d\n", argc);
    if (argc < 2) {
        fprintf(stderr, "Usage: %s [-e mask | +e mask]... directory1 [directory2 ...]\n", argv[0]);
        fprintf(stderr, "mask: ACCESS, MODIFY, CLOSE_WRITE, CLOSE_NOWRITE, OPEN, ONDIR, EVENT_ON_CHILD\n");
        exit(EXIT_FAILURE);
    }

    /* Parse options */
    if (strcmp(argv[1], "+e") == 0) event_mask = 0;
    debug_printf("event_mask = 0x%08lx\n", event_mask);
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "+e") == 0) {
            if (++i < argc) {
                mask = getmask(argv[i]);
                if (mask == 0) {
                    debug_printf("+set invalid(%s)\n", argv[i]);
                } else {
                    debug_printf("+set 0x%08lx\n", mask);
                    event_mask |= mask;
                }
            } else {
                break;
            }
        } else if (strcmp(argv[i], "-e") == 0) {
            if (++i < argc) {
                mask = getmask(argv[i]);
                if (mask == 0) {
                    debug_printf("-reset invalid(%s)\n", argv[i]);
                } else {
                    debug_printf("-reset 0x%08lx\n", mask);
                    event_mask &= ~mask;
                }
            } else {
                break;
            }
        } else {
            break;
        }
    }
    debug_printf("event_mask = 0x%08lx\n", event_mask);
    debug_printf("i = %d\n", i);
    debug_printf("argv[%d] = %s\n", i, argv[i]);
    if (argc <= i) {
        fprintf(stderr, "Usage: %s [-e mask | +e mask]... directory1 [directory2 ...]\n", argv[0]);
        fprintf(stderr, "mask: ACCESS, MODIFY, CLOSE_WRITE, CLOSE_NOWRITE, OPEN, ONDIR, EVENT_ON_CHILD\n");
        exit(EXIT_FAILURE);
    }

    /* Initialize signals FD */
    if ((signal_fd = initialize_signals()) < 0) {
        fprintf(stderr, "Couldn't initialize signals\n");
        exit(EXIT_FAILURE);
    }

    /* Initialize fanotify-cmdline FD and the marks */
    --i;
    if ((fanotify_fd = initialize_fanotify(argc - i, &argv[i])) < 0) {
        fprintf(stderr, "Couldn't initialize fanotify-cmdline\n");
        exit(EXIT_FAILURE);
    }

    /* Setup polling */
    fds[FD_POLL_SIGNAL].fd = signal_fd;
    fds[FD_POLL_SIGNAL].events = POLLIN;
    fds[FD_POLL_FANOTIFY].fd = fanotify_fd;
    fds[FD_POLL_FANOTIFY].events = POLLIN;

    /* Now loop */
    for (;;) {
        /* Block until there is something to be read */
        if (poll(fds, FD_POLL_MAX, -1) < 0) {
            fprintf(stderr,
                    "Couldn't poll(): '%s'\n",
                    strerror(errno));
            exit(EXIT_FAILURE);
        }

        /* Signal received? */
        if (fds[FD_POLL_SIGNAL].revents & POLLIN) {
            struct signalfd_siginfo fdsi;

            if (read(fds[FD_POLL_SIGNAL].fd,
                     &fdsi,
                     sizeof(fdsi)) != sizeof(fdsi)) {
                fprintf(stderr,
                        "Couldn't read signal, wrong size read\n");
                exit(EXIT_FAILURE);
            }

            /* Break loop if we got the expected signal */
            if (fdsi.ssi_signo == SIGINT ||
                fdsi.ssi_signo == SIGTERM) {
                break;
            }

            fprintf(stderr,
                    "Received unexpected signal\n");
        }

        /* fanotify-cmdline event received? */
        if (fds[FD_POLL_FANOTIFY].revents & POLLIN) {
            char buffer[FANOTIFY_BUFFER_SIZE];
            ssize_t length;

            /* Read from the FD. It will read all events available up to
             * the given buffer size. */
            if ((length = read(fds[FD_POLL_FANOTIFY].fd,
                               buffer,
                               FANOTIFY_BUFFER_SIZE)) > 0) {
                struct fanotify_event_metadata *metadata;

                metadata = (struct fanotify_event_metadata *) buffer;
                while (FAN_EVENT_OK (metadata, length)) {
                    event_process(metadata);
                    if (metadata->fd > 0)
                        close(metadata->fd);
                    metadata = FAN_EVENT_NEXT (metadata, length);
                }
            }
        }
    }

    /* Clean exit */
    shutdown_fanotify(fanotify_fd);
    shutdown_signals(signal_fd);

    printf("Exiting fanotify-cmdline example...\n");

    return EXIT_SUCCESS;
}
