#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef DEBUG
#define log(fmt, ...) \
    do { \
        fprintf(stderr, "[%s():%d] " fmt "\n", __func__, __LINE__, ##__VA_ARGS__); \
    } while(0)
#else
#define log(fmt, ...)
#endif

static void die(const char *msg)
{
    perror(msg);
    exit(EXIT_FAILURE);
}

static void print_error(const char *msg)
{
    perror(msg);
}

#define FRAME_WIDTH 720
#define FRAME_HEIGHT 480

#define VIDEO_FRAME_SIZE (FRAME_WIDTH * FRAME_HEIGHT * 3 / 2)

#define MMAP_SIZE 522496
#define MMAP_SIZE_2 695296

#define XWIN_SEGMENT_PIXELS 320
#define XWIN_BUF_SIZE (2 + XWIN_SEGMENT_PIXELS * 4) // 2 bytes (INDEX) + 320 pixels (BGRA)
#define XWIN_NUM_SEGMENTS (FRAME_WIDTH * FRAME_HEIGHT / XWIN_SEGMENT_PIXELS)
#define XWIN_FRAME_SIZE (FRAME_WIDTH * FRAME_HEIGHT * 4)

#define PORT_NOTIFY 5677
#define PORT_VIDEO 5678
#define PORT_XWIN 5679
#define PORT_EXECUTOR 5680
#define PORT_UDP_BROADCAST 5681

#define XWD_SKIP_BYTES 3179
#define DISCOVERY_PACKET_SIZE 32

#define APP_PATH "/opt/usr/apps/nx-remote-controller-mod"
//#define APP_PATH "/opt/storage/sdcard/remote"
#define POPUP_TIMEOUT_SH_COMMAND APP_PATH "/popup_timeout.sh"
#define LCD_CONTROL_SH_COMMAND APP_PATH "/lcd_control.sh"
#define NX_INPUT_INJECTOR_COMMAND "chroot " APP_PATH "/tools nx-input-injector"

#define CHROOT_COMMAND \
        "chroot " APP_PATH "/tools "
#define GET_DI_CAMERA_APP_WINDOW_ID_COMMAND \
        "\"$(" CHROOT_COMMAND "xdotool search --class di-camera-app)\""
#define XEV_NX_COMMAND \
        CHROOT_COMMAND "xev-nx -p -id " \
        GET_DI_CAMERA_APP_WINDOW_ID_COMMAND

#define HEVC_STATE_UNKNOWN (-1)
#define HEVC_STATE_OFF 0
#define HEVC_STATE_ON  1

#define PING_TIMEOUT_MS 5000

static off_t s_addrs[] = {
    0xbbaea500,
    0xbbb68e00,
    0xbbbe7700,
    0xbba6bc00,

//    0xa4a0d000, // MMAP_SIZE_2
//    0xa4ad2000, // MMAP_SIZE_2
//    0xaefd0000, // MMAP_SIZE_2
//    0xaf150000, // MMAP_SIZE_2

//    0x9f600000,
//    0x9f6fd000,
//    0x9f77b000,
//    0x9f8f7000,
};

#define S_ADDRS_SIZE sizeof(s_addrs)

typedef struct {
    int server_fd;
    int client_fd;
    int fps;
} StreamerData;

typedef void *(*OnConnect)(StreamerData *data);

typedef struct {
    int port;
    OnConnect on_connect;
    int *socket_connect_count;
} ListenSocketData;

static char *get_port_name(int port)
{
    switch (port) {
        case PORT_VIDEO:
            return "video";
        case PORT_XWIN:
            return "xwin";
        case PORT_NOTIFY:
            return "notify";
        case PORT_EXECUTOR:
            return "executor";
        case PORT_UDP_BROADCAST:
            return "discovery";
    }

    return "unknown";
}

static bool s_video_socket_closed_notify;
static bool s_xwin_socket_closed_notify;
static bool s_executor_socket_closed_notify;
static bool s_video_socket_close_request;

static void *start_notify(StreamerData *data)
{
    int client_fd = data->client_fd;
    FILE *xev_pipe = NULL;
    char buf[256];
    int xev_fd;
    int flags;
    size_t write_size;
    pid_t xev_pid = 0;
    FILE *hevc = NULL;
    int hevc_state = HEVC_STATE_UNKNOWN;
    char *line;
    int count = 0;

    free(data);

    hevc = fopen("/sys/kernel/debug/pmu/hevc/state", "r");
    if (hevc == NULL) {
        print_error("fopen() failed");
        goto error;
    }

    //log("xev-nx command = %s", XEV_NX_COMMAND);
    xev_pipe = popen(XEV_NX_COMMAND, "r");
    if (xev_pipe == NULL) {
        print_error("popen() failed!");
        goto error;
    }

    if (fgets(buf, sizeof(buf), xev_pipe) != NULL) {
        sscanf(buf, "%d\n", &xev_pid);
        log("xev-nx pid = %d", xev_pid);
    } else {
        log("failed get xev-nx pid.");
        goto error;
    }

    xev_fd = fileno(xev_pipe);
    flags = fcntl(xev_fd, F_GETFL, 0);
    flags |= O_NONBLOCK;
    fcntl(xev_fd, F_SETFL, flags);

    while (true) {
        // hevc check
        clearerr(hevc);
        rewind(hevc);
        memset(buf, 0, sizeof(buf));
        fread(buf, 1, sizeof(buf), hevc);
        if (ferror(hevc) != 0) {
            log("ferror()");
        } else if (feof(hevc) != 0) {
            //log("read_size = %d, buf = %s", read_size, buf);
            if (strncmp(buf, "on", 2) == 0) {
                if (hevc_state != HEVC_STATE_ON) {
                    hevc_state = HEVC_STATE_ON;
                    write_size = write(client_fd, "hevc=on\n", 8);
                    if (write_size == -1) {
                        print_error("write() failed!");
                        goto error;
                    }
                }
            } else if (strncmp(buf, "off", 3) == 0) {
                if (hevc_state != HEVC_STATE_OFF) {
                    hevc_state = HEVC_STATE_OFF;
                    write_size = write(client_fd, "hevc=off\n", 9);
                    if (write_size == -1) {
                        print_error("write() failed!");
                        goto error;
                    }
                }
            }
        }

        // xev-nx
        line = fgets(buf, sizeof(buf), xev_pipe);
        if (line == NULL) {
            if (errno == EWOULDBLOCK) {
                errno = 0;
                usleep(100*1000);
            } else {
                log("failed to read from xev_pipe.");
                break;
            }
        } else {
            write_size = write(client_fd, buf, strlen(buf));
            if (write_size == -1) {
                log("write() failed. write_size = %d", write_size);
                break;
            }
            //log("buf = %s", buf);
        }

        if (s_video_socket_closed_notify) {
            char msg[] = "socket_closed=video\n";
            write_size = write(client_fd, msg, strlen(msg));
            if (write_size == -1) {
                log("write() failed!");
                break;
            }
            s_video_socket_closed_notify = false;
        }

        if (s_xwin_socket_closed_notify) {
            char msg[] = "socket_closed=xwin\n";
            write_size = write(client_fd, msg, strlen(msg));
            if (write_size == -1) {
                log("write() failed!");
                break;
            }
            s_video_socket_closed_notify = false;
        }

        if (s_executor_socket_closed_notify) {
            char msg[] = "socket_closed=executor\n";
            write_size = write(client_fd, msg, strlen(msg));
            if (write_size == -1) {
                log("write() failed!");
                break;
            }
            s_executor_socket_closed_notify = false;
        }

        // send ping
        if (count % 10 == 0) {
            write_size = write(client_fd, "ping\n", 5);
            if (write_size == -1) {
                log("write() failed. ping failed");
                break;
            }
            count = 0;
        }
        count++;
    }

error:
    if (hevc != NULL && fclose(hevc)) {
        print_error("fclose() failed!");
    }
    if (xev_pid != 0 && kill(xev_pid, SIGKILL) == -1) {
        print_error("kill() failed!");
    }
    if (xev_pipe != NULL && pclose(xev_pipe) == -1) {
        //print_error("pclose() failed!");
    }

    log("notify finished.");

    return NULL;
}

static void *mmap_lcd(const int fd, const off_t offset)
{
    off_t pa_offset = offset & ~(sysconf(_SC_PAGE_SIZE) - 1);
    //log("offset = %llu, pa_offset = %llu", (unsigned long long)offset, (unsigned long long)pa_offset);
    void *p = mmap(NULL, MMAP_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, fd, pa_offset);
    if (p == MAP_FAILED) {
        die("mmap() failed");
    }

    return p + (offset - pa_offset);
}

static void munmap_lcd(void *addr, const off_t offset)
{
    off_t pa_offset = offset & ~(sysconf(_SC_PAGE_SIZE) - 1);
    if (munmap(addr - (offset - pa_offset), MMAP_SIZE) == -1) {
        die("munmap() failed");
    }
}

static long long get_current_time()
{
    struct timeval te; 
    gettimeofday(&te, NULL); // get current time
    long long milliseconds = te.tv_sec*1000LL + te.tv_usec/1000; // caculate milliseconds
    // printf("milliseconds: %lld\n", milliseconds);
    return milliseconds;
}

static int s_video_fps;
static void *start_video_capture(StreamerData *data)
{
    int client_fd = data->client_fd;
    s_video_fps = data->fps;
    int fd;
    void *addrs[S_ADDRS_SIZE];
    int hashs[S_ADDRS_SIZE] = {0,};
    int i, j, hash;
    long long start_time, end_time, time_diff;
    long long frame_time = 1000ll / (long)s_video_fps;
#ifdef DEBUG
    long long capture_start_time, capture_end_time;
#endif
    bool err = false;

    free(data);

    fd = open("/dev/mem", O_RDWR);
    if (fd == -1) {
        die("open() error");
    }

    for (i = 0; i < S_ADDRS_SIZE; i++) {
        addrs[i] = mmap_lcd(fd, s_addrs[i]);
    }

#ifdef DEBUG
    capture_start_time = get_current_time();
#endif
    s_video_socket_close_request = false;
    while (true) {
        start_time = get_current_time();

        if (s_video_socket_close_request) {
            s_video_socket_close_request = false;
            break;
        }

        for (i = 0; i < S_ADDRS_SIZE; i++) {
            const char *p = addrs[i];

            hash = 0;
            for (j = 0; j < 720*2; j++) {
                hash += p[j];
            }
            if (hashs[i] != 0 && hash != hashs[i]) {
                if (write(client_fd, p, VIDEO_FRAME_SIZE) == -1) {
                    log("write() failed!");
                    err = true;
                    break;
                }
                //log("[VideoCapture] %d, hash = %d (changed!)", i, hash);
            }

            hashs[i] = hash;
        }

        end_time = get_current_time();

        time_diff = end_time - start_time;
        if (time_diff < frame_time) {
            //log("sleep %lld ms", frame_time - time_diff);
            usleep((frame_time - time_diff) * 1000);
        }
        frame_time = 1000ll / (long)s_video_fps;

        if (err) {
            break;
        }
    }

#ifdef DEBUG
    capture_end_time = get_current_time();
    log("time = %f", (capture_end_time - capture_start_time) / 1000.0);
#endif

    for (i = 0; i < 4; i++) {
        munmap_lcd(addrs[i], s_addrs[i]);
    }

    if (close(fd) == -1) {
        print_error("close failed");
    }

    return NULL;
}

static int s_xwin_fps;
static void *start_xwin_capture(StreamerData *data)
{
    int client_fd = data->client_fd;
    s_xwin_fps = data->fps;

    long long start_time, end_time, time_diff;
    long long frame_time = 1000ll / (long)s_xwin_fps;
#ifdef DEBUG
    long long capture_start_time, capture_end_time;
#endif
    int count, skip_count;

    FILE *xwd_out;
    unsigned char buf[XWIN_BUF_SIZE];
    size_t skip_size, read_size, offset;
    ssize_t write_size;
    int hashs[XWIN_NUM_SEGMENTS] = {0,};
    int hash, hash_index;;

    bool err = false;

    free(data);

#ifdef DEBUG
    capture_start_time = get_current_time();
#endif
    count = 0;
    while (true) {
        start_time = get_current_time();
        hash = 0;
        err = false;

        xwd_out = popen("xwd -root", "r");
        if (xwd_out == NULL) {
            print_error("popen() failed");
            continue;
        }

        skip_size = XWD_SKIP_BYTES;
        do {
            read_size = fread(buf, 1, skip_size < XWIN_BUF_SIZE
                                        ? skip_size : XWIN_BUF_SIZE,
                              xwd_out);
            if (read_size == 0) {
                err = true;
                log("xwd read_size = 0");
                break;
            }
            skip_size -= read_size;
        } while (skip_size != 0);

        if (err) {
            pclose(xwd_out);
            continue;
        }

        if (skip_size == 0) {
            offset = 0;
            hash_index = 0;
            skip_count = 0;
            while (true) {
                read_size = fread(buf + 2, 1, XWIN_BUF_SIZE - 2, xwd_out); // first 2 bytes is index
                offset += read_size;
                if (read_size == 0) {
                    log("read_size == 0");
                    err = true;
                    break;
                } else if (read_size != XWIN_BUF_SIZE - 2) {
                    log("read_size != %d (XWIN_BUF_SIZE)", XWIN_BUF_SIZE);
                    err = true;
                    break;
                } else {
                    int i;
                    for (i = 2; i < read_size + 2; i += 4) {
                        hash += buf[i];
                        if (buf[i] > 0) {
                            hash += i;
                        }
                    }

                    if (hash_index != 0 && hashs[hash_index] == hash) {
                        skip_count++;
                    } else {
                        hashs[hash_index] = hash;
                        buf[0] = (hash_index >> 8) & 0xff;
                        buf[1] = hash_index & 0xff;

                        write_size = write(client_fd, buf, XWIN_BUF_SIZE);
                        if (write_size != XWIN_BUF_SIZE) {
                            log("write() failed");
                            err = true;
                            break;
                        }
                    }
                    if (offset == XWIN_FRAME_SIZE) {
                        // notify end of frame
                        buf[0] = 0x0f;
                        buf[1] = 0xff;
                        write_size = write(client_fd, buf, XWIN_BUF_SIZE);
                        if (write_size != XWIN_BUF_SIZE) {
                            log("write() failed");
                            err = true;
                            break;
                        }

                        if (skip_count != 1080 - 1) {
                            log("[XWinCapture] count = %d, skip_count = %d",
                                count, skip_count);
                        }
                        break;
                    }
                }
                hash_index++;
            }
        } else {
            log("skip_size = %d", skip_size);
            err = true;
        }

        if (pclose(xwd_out) == -1) {
            //print_error("pclose() failed");
            //err = true;
        }

        end_time = get_current_time();

        time_diff = end_time - start_time;
        if (time_diff < frame_time) {
            //log("sleep %lld ms", frame_time - time_diff);
            usleep((frame_time - time_diff) * 1000);
        }
        count++;
        frame_time = 1000ll / (long)s_xwin_fps;

        if (err) {
            break;
        }
    }
#ifdef DEBUG
    capture_end_time = get_current_time();
    log("time = %f", (capture_end_time - capture_start_time) / 1000.0);
#endif

    return NULL;
}

static void run_command(char *command_line)
{
    pid_t pid = fork();
    if (pid == 0) { // child
        log("execvp(), %s", command_line);
#define MAX_ARGS 63
        int argc = 0;
        char *argv[MAX_ARGS + 1];
        char *p = strtok(command_line, " ");
        while (p && argc < MAX_ARGS) {
            argv[argc++] = p;
            p = strtok(NULL, " ");
        }
        argv[argc] = NULL;
        execvp(argv[0], argv);
    } else if (pid > 0) {
        // parent. do nothing
    } else {
        print_error("fork() failed!");
    }
}

static void *start_executor(StreamerData *data)
{
    FILE *client_sock;
    int client_fd = data->client_fd;;
    char command_line[256];
    bool err = false;
    FILE *command_pipe = NULL;
    char buf[1024];
    size_t read_size;
    size_t write_size;
    unsigned long size;
    FILE *inject_input_pipe = NULL;
    long long last_ping_time;
    int flags;

    client_sock = fdopen(data->client_fd, "r");
    if (client_sock == NULL) {
        print_error("fdopen() failed");
        err = true;
    }

    free(data);

    if (err) {
        goto error;
    }

    log("executor started.");

    inject_input_pipe = popen(NX_INPUT_INJECTOR_COMMAND, "w");
    if (inject_input_pipe == NULL) {
        print_error("pope() failed");
        goto error;
    }

    flags = fcntl(client_fd, F_GETFL, 0);
    flags |= O_NONBLOCK;
    fcntl(client_fd, F_SETFL, flags);

    last_ping_time = get_current_time();
    while (true) {
        if (fgets(command_line, sizeof(command_line), client_sock) == NULL) {
            if (errno == EWOULDBLOCK) {
                errno = 0;
                usleep(50*1000);
            } else {
                break;
            }
        }
        if (command_line[strlen(command_line) -1] == '\n') {
            command_line[strlen(command_line) - 1] = '\0'; // strip '\n' at end
        } else {
            continue;
        }

        if (strlen(command_line) > 0 && command_line[0] == '@') {
            // run command in background and no output return
            run_command(command_line + 1);
        } else if (strlen(command_line) > 0 && command_line[0] == '$') {
            // run command in foreground and return output
            log("command = %s", command_line);

            command_pipe = popen(command_line + 1, "r");
            if (command_pipe == NULL) {
                print_error("popen() failed");
                continue;
            }

            while (feof(command_pipe) == 0 && ferror(command_pipe) == 0) {
                read_size = fread(buf, 1, sizeof(buf), command_pipe);
                if (read_size == 0) {
                    break;
                } else if (read_size < 0 || read_size > sizeof(buf)) {
                    fprintf(stderr, "read_size = %d\n", read_size);
                    break;
                }
                while (read_size > 0) {
                    size = htonl(read_size);
                    write_size = write(client_fd, (const void *)&size, 4);
                    if (write_size == -1) {
                        print_error("write() failed!");
                        goto error;
                    }
                    write_size = write(client_fd, buf, read_size);
                    if (write_size == -1) {
                        print_error("write() failed!");
                        goto error;
                    }
                    read_size -= write_size;
                }
            }
        } else if (strncmp("inject_input=", command_line, 13) == 0) {
            fprintf(inject_input_pipe, "%s\n", command_line + 13);
            fflush(inject_input_pipe);
        } else if (strncmp("vfps=", command_line, 5) == 0) {
            s_video_fps = atoi(command_line+5);
            fprintf(stderr, "video fps = %d\n", s_video_fps);
        } else if (strncmp("xfps=", command_line, 5) == 0) {
            s_xwin_fps = atoi(command_line+5);
            fprintf(stderr, "xwin fps = %d\n", s_xwin_fps);
        } else if (strncmp("lcd=on", command_line, 6) == 0) {
            system(LCD_CONTROL_SH_COMMAND " on");
        } else if (strncmp("lcd=off", command_line, 7) == 0) {
            system(LCD_CONTROL_SH_COMMAND " off");
        } else if (strncmp("lcd=video", command_line, 9) == 0) {
            system(LCD_CONTROL_SH_COMMAND " video");
        } else if (strncmp("lcd=osd", command_line, 7) == 0) {
            system(LCD_CONTROL_SH_COMMAND " osd");
        } else if (strncmp("ping", command_line, 4) == 0) {
            last_ping_time = get_current_time();
        }

        // EOF
        size = 0;
        write_size = write(client_fd, (const void *)&size, 4);
        if (write_size == -1) {
            print_error("write() failed!");
            goto error;
        }

        if (command_pipe != NULL && pclose(command_pipe) == -1) {
            //print_error("pclose() failed!");
            command_pipe = NULL;
        }

        if (last_ping_time < get_current_time() - PING_TIMEOUT_MS) {
            log("executor ping not reached.");
            goto error;
        }
    }

error:
    if (command_pipe != NULL) {
        if (pclose(command_pipe) == -1) {
            //print_error("pclose() failed!");
        }
    }
    if (inject_input_pipe != NULL) {
        if (pclose(inject_input_pipe) == -1) {
            //print_error("pclose() failed!");
        }
    }

    log("executor finished.");
    return NULL;
}

static void *listen_socket_func(void *thread_data)
{
    ListenSocketData *listen_socket_data = (ListenSocketData *)thread_data;
    int port = listen_socket_data->port;
    OnConnect on_connect = listen_socket_data->on_connect;
    int *socket_connect_count = listen_socket_data->socket_connect_count;
    struct sockaddr_in server_addr, client_addr;
    int server_fd, client_fd;
    socklen_t len;
    pthread_t thread;
    int on = 1;

    free(listen_socket_data);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        die("socket() failed");
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);

    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
               (const void *)&on, (socklen_t)sizeof(on));

    if (bind(server_fd, (struct sockaddr *)&server_addr,
             sizeof(server_addr)) == -1) {
        die("bind() failed");
    }

    if (listen(server_fd, 5) == -1) {
        die("listen() failed");
    }

    len = sizeof(client_addr);

    while (true) {
        StreamerData *data = (StreamerData *)malloc(sizeof(StreamerData));

        log("waiting client... port = %d (%s)", port, get_port_name(port));
        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &len);
        if (client_fd == -1) {
            die("accept() failed");
        }

        log("client connected. port = %d (%s)", port, get_port_name(port));

        data->server_fd = server_fd;
        data->client_fd = client_fd;
        data->fps = 5;

        (*socket_connect_count)++;
        if (pthread_create(&thread, NULL,
                           (void *(*)(void *))on_connect, data)) {
            die("ptherad_create() failed");
        }

        if (pthread_join(thread, NULL)) {
            die("pthread_join() failed");
        }

        if (*socket_connect_count > 0) {
            (*socket_connect_count)--;
        }

        close(client_fd);

        log("client closed. port = %d (%s)", port, get_port_name(port));
        log("connected socket count = %d", *socket_connect_count);
        if (port == PORT_VIDEO) {
            s_video_socket_closed_notify = true;
        } else if (port == PORT_XWIN) {
            s_xwin_socket_closed_notify = true;
        } else if (port == PORT_EXECUTOR) {
            s_executor_socket_closed_notify = true;
        } else if (port == PORT_NOTIFY) {
            s_video_socket_close_request = true;
        }
    }

    close(server_fd);

    return NULL;
}

static void listen_socket(const int port, const OnConnect on_connect, int *socket_connect_count)
{
    pthread_t thread;
    ListenSocketData *thread_data
        = (ListenSocketData *)malloc(sizeof(ListenSocketData));
    thread_data->port = port;
    thread_data->on_connect = on_connect;
    thread_data->socket_connect_count = socket_connect_count;

    if (pthread_create(&thread, NULL, listen_socket_func, thread_data)) {
        die("pthread_create() failed!");
    }

    if (pthread_detach(thread)) {
        die("pthread_detach() failed");
    }
}

static void broadcast_discovery_packet(const int port,
                                       const int *socket_connect_count)
{
    int sock;
    int broadcast_enable = 1;
    int ret;
    struct sockaddr_in sin;
    bool need_show_disconnected_msg = false;

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == -1) {
        die("socket() failed");
    }

    ret = setsockopt(sock, SOL_SOCKET, SO_BROADCAST,
                     &broadcast_enable, sizeof(broadcast_enable));
    if (ret == -1) {
        die("setsockopt() failed");
    }

    memset(&sin, 0, sizeof(struct sockaddr_in));
    sin.sin_family = AF_INET;
    sin.sin_port = (in_port_t)htons(port);
    sin.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    while (true) {
        if (*socket_connect_count == 0) {
            char msg[DISCOVERY_PACKET_SIZE] = {0,};
            char command_line[256];
            strncpy(command_line, POPUP_TIMEOUT_SH_COMMAND 
                    " 3 NXRemoteController disconnected.", 256);

            if (need_show_disconnected_msg) {
                run_command(command_line);
                need_show_disconnected_msg = false;
            }

            log("broadcasting discovery packet...");
            // TODO: get camera model
            strncpy(msg, "NX_REMOTE|1.0|NX500|", sizeof(msg)); // HEADER|VERSION|MODEL|

            if (sendto(sock, msg, sizeof(msg), 0, (struct sockaddr *)&sin,
                       sizeof(struct sockaddr_in)) == -1) {
                print_error("sendto() failed");
            }
        } else {
            need_show_disconnected_msg = true;
        }
        sleep(1);
    }

    if (close(sock) == -1) {
        print_error("close() failed");
    }
}

int main(int argc, char **argv)
{
    int socket_connect_count = 0;

    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    listen_socket(PORT_NOTIFY, start_notify, &socket_connect_count);
    listen_socket(PORT_VIDEO, start_video_capture, &socket_connect_count);
    listen_socket(PORT_XWIN, start_xwin_capture, &socket_connect_count);
    listen_socket(PORT_EXECUTOR, start_executor, &socket_connect_count);

    broadcast_discovery_packet(PORT_UDP_BROADCAST, &socket_connect_count);

    return 0;
}
