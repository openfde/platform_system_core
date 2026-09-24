#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/inotify.h>
#include <sys/limits.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <linux/input.h>
#include <err.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

struct label {
    const char *name;
    int value;
};

#define LABEL(constant) { #constant, constant }
#define LABEL_END { NULL, -1 }

static struct label key_value_labels[] = {
        { "UP", 0 },
        { "DOWN", 1 },
        { "REPEAT", 2 },
        LABEL_END,
};

#include "input.h-labels.h"

#undef LABEL
#undef LABEL_END

static struct pollfd *ufds;
static char **device_names;
struct device_state {
    int is_fifo;
    size_t fifo_pending;
    unsigned char fifo_data[sizeof(struct input_event)];
};
static struct device_state *device_states;
static int nfds;

static const char *openfde_fifo_paths[] = {
    "/dev/input/wl_touch_events",
    "/dev/input/wl_keyboard_events",
    "/dev/input/wl_pointer_events",
    "/dev/input/wl_tablet_events",
};

static int is_openfde_fifo_name(const char *name) {
    return !strcmp(name, "wl_touch_events") || !strcmp(name, "wl_keyboard_events") ||
            !strcmp(name, "wl_pointer_events") || !strcmp(name, "wl_tablet_events");
}

enum {
    PRINT_DEVICE_ERRORS     = 1U << 0,
    PRINT_DEVICE            = 1U << 1,
    PRINT_DEVICE_NAME       = 1U << 2,
    PRINT_DEVICE_INFO       = 1U << 3,
    PRINT_VERSION           = 1U << 4,
    PRINT_POSSIBLE_EVENTS   = 1U << 5,
    PRINT_INPUT_PROPS       = 1U << 6,
    PRINT_HID_DESCRIPTOR    = 1U << 7,

    PRINT_ALL_INFO          = (1U << 8) - 1,

    PRINT_LABELS            = 1U << 16,
};

static const char *get_label(const struct label *labels, int value)
{
    while(labels->name && value != labels->value) {
        labels++;
    }
    return labels->name;
}

static int print_input_props(int fd)
{
    uint8_t bits[INPUT_PROP_CNT / 8];
    int i, j;
    int res;
    int count;
    const char *bit_label;

    printf("  input props:\n");
    res = ioctl(fd, EVIOCGPROP(sizeof(bits)), bits);
    if(res < 0) {
        printf("    <not available\n");
        return 1;
    }
    count = 0;
    for(i = 0; i < res; i++) {
        for(j = 0; j < 8; j++) {
            if (bits[i] & 1 << j) {
                bit_label = get_label(input_prop_labels, i * 8 + j);
                if(bit_label)
                    printf("    %s\n", bit_label);
                else
                    printf("    %04x\n", i * 8 + j);
                count++;
            }
        }
    }
    if (!count)
        printf("    <none>\n");
    return 0;
}

static int print_possible_events(int fd, int print_flags)
{
    uint8_t *bits = NULL;
    ssize_t bits_size = 0;
    const char* label;
    int i, j, k;
    int res, res2;
    struct label* bit_labels;
    const char *bit_label;

    printf("  events:\n");
    for(i = EV_KEY; i <= EV_MAX; i++) { // skip EV_SYN since we cannot query its available codes
        int count = 0;
        while(1) {
            res = ioctl(fd, EVIOCGBIT(i, bits_size), bits);
            if(res < bits_size)
                break;
            bits_size = res + 16;
            bits = realloc(bits, bits_size * 2);
            if (bits == NULL) err(1, "failed to allocate buffer of size %zd", bits_size);
        }
        res2 = 0;
        switch(i) {
            case EV_KEY:
                res2 = ioctl(fd, EVIOCGKEY(res), bits + bits_size);
                label = "KEY";
                bit_labels = key_labels;
                break;
            case EV_REL:
                label = "REL";
                bit_labels = rel_labels;
                break;
            case EV_ABS:
                label = "ABS";
                bit_labels = abs_labels;
                break;
            case EV_MSC:
                label = "MSC";
                bit_labels = msc_labels;
                break;
            case EV_LED:
                res2 = ioctl(fd, EVIOCGLED(res), bits + bits_size);
                label = "LED";
                bit_labels = led_labels;
                break;
            case EV_SND:
                res2 = ioctl(fd, EVIOCGSND(res), bits + bits_size);
                label = "SND";
                bit_labels = snd_labels;
                break;
            case EV_SW:
                res2 = ioctl(fd, EVIOCGSW(bits_size), bits + bits_size);
                label = "SW ";
                bit_labels = sw_labels;
                break;
            case EV_REP:
                label = "REP";
                bit_labels = rep_labels;
                break;
            case EV_FF:
                label = "FF ";
                bit_labels = ff_labels;
                break;
            case EV_PWR:
                label = "PWR";
                bit_labels = NULL;
                break;
            case EV_FF_STATUS:
                label = "FFS";
                bit_labels = ff_status_labels;
                break;
            default:
                res2 = 0;
                label = "???";
                bit_labels = NULL;
        }
        for(j = 0; j < res; j++) {
            for(k = 0; k < 8; k++)
                if(bits[j] & 1 << k) {
                    char down;
                    if(j < res2 && (bits[j + bits_size] & 1 << k))
                        down = '*';
                    else
                        down = ' ';
                    if(count == 0)
                        printf("    %s (%04x):", label, i);
                    else if((count & (print_flags & PRINT_LABELS ? 0x3 : 0x7)) == 0 || i == EV_ABS)
                        printf("\n               ");
                    if(bit_labels && (print_flags & PRINT_LABELS)) {
                        bit_label = get_label(bit_labels, j * 8 + k);
                        if(bit_label)
                            printf(" %.20s%c%*s", bit_label, down, (int) (20 - strlen(bit_label)), "");
                        else
                            printf(" %04x%c                ", j * 8 + k, down);
                    } else {
                        printf(" %04x%c", j * 8 + k, down);
                    }
                    if(i == EV_ABS) {
                        struct input_absinfo abs;
                        if(ioctl(fd, EVIOCGABS(j * 8 + k), &abs) == 0) {
                            printf(" : value %d, min %d, max %d, fuzz %d, flat %d, resolution %d",
                                abs.value, abs.minimum, abs.maximum, abs.fuzz, abs.flat,
                                abs.resolution);
                        }
                    }
                    count++;
                }
        }
        if(count)
            printf("\n");
    }
    free(bits);
    return 0;
}

static void print_event(int type, int code, int value, int print_flags)
{
    const char *type_label, *code_label, *value_label;

    if (print_flags & PRINT_LABELS) {
        type_label = get_label(ev_labels, type);
        code_label = NULL;
        value_label = NULL;

        switch(type) {
            case EV_SYN:
                code_label = get_label(syn_labels, code);
                break;
            case EV_KEY:
                code_label = get_label(key_labels, code);
                value_label = get_label(key_value_labels, value);
                break;
            case EV_REL:
                code_label = get_label(rel_labels, code);
                break;
            case EV_ABS:
                code_label = get_label(abs_labels, code);
                switch(code) {
                    case ABS_MT_TOOL_TYPE:
                        value_label = get_label(mt_tool_labels, value);
                }
                break;
            case EV_MSC:
                code_label = get_label(msc_labels, code);
                break;
            case EV_LED:
                code_label = get_label(led_labels, code);
                break;
            case EV_SND:
                code_label = get_label(snd_labels, code);
                break;
            case EV_SW:
                code_label = get_label(sw_labels, code);
                break;
            case EV_REP:
                code_label = get_label(rep_labels, code);
                break;
            case EV_FF:
                code_label = get_label(ff_labels, code);
                break;
            case EV_FF_STATUS:
                code_label = get_label(ff_status_labels, code);
                break;
        }

        if (type_label)
            printf("%-12.12s", type_label);
        else
            printf("%04x        ", type);
        if (code_label)
            printf(" %-20.20s", code_label);
        else
            printf(" %04x                ", code);
        if (value_label)
            printf(" %-20.20s", value_label);
        else
            printf(" %08x            ", value);
    } else {
        printf("%04x %04x %08x", type, code, value);
    }
}

static void print_hid_descriptor(int bus, int vendor, int product)
{
    const char *dirname = "/sys/kernel/debug/hid";
    char prefix[16];
    DIR *dir;
    struct dirent *de;
    char filename[PATH_MAX];
    FILE *file;
    char line[2048];

    snprintf(prefix, sizeof(prefix), "%04X:%04X:%04X.", bus, vendor, product);

    dir = opendir(dirname);
    if(dir == NULL)
        return;
    while((de = readdir(dir))) {
        if (strstr(de->d_name, prefix) == de->d_name) {
            snprintf(filename, sizeof(filename), "%s/%s/rdesc", dirname, de->d_name);

            file = fopen(filename, "r");
            if (file) {
                printf("  HID descriptor: %s\n\n", de->d_name);
                while (fgets(line, sizeof(line), file)) {
                    fputs("    ", stdout);
                    fputs(line, stdout);
                }
                fclose(file);
                puts("");
            }
        }
    }
    closedir(dir);
}

static int open_device(const char *device, int print_flags)
{
    int version;
    int fd;
    int open_flags = O_RDONLY | O_CLOEXEC;
    int clkid = CLOCK_MONOTONIC;
    struct pollfd *new_ufds;
    char **new_device_names;
    struct device_state *new_device_states;
    char name[80];
    char location[80];
    char idstr[80];
    struct input_id id;
    struct stat st;
    int is_fifo = 0;

    if (stat(device, &st) == 0 && S_ISFIFO(st.st_mode)) {
        is_fifo = 1;
        open_flags = O_RDWR | O_NONBLOCK | O_CLOEXEC;
    }
    snprintf(name, sizeof(name), "%s", device);

    fd = open(device, open_flags);
    if(fd < 0) {
        if(print_flags & PRINT_DEVICE_ERRORS)
            fprintf(stderr, "could not open %s, %s\n", device, strerror(errno));
        return -1;
    }

    if (!is_fifo) {
        if(ioctl(fd, EVIOCGVERSION, &version)) {
            if(print_flags & PRINT_DEVICE_ERRORS)
                fprintf(stderr, "could not get driver version for %s, %s\n", device, strerror(errno));
            close(fd);
            return -1;
        }
        if(ioctl(fd, EVIOCGID, &id)) {
            if(print_flags & PRINT_DEVICE_ERRORS)
                fprintf(stderr, "could not get driver id for %s, %s\n", device, strerror(errno));
            close(fd);
            return -1;
        }
        name[sizeof(name) - 1] = '\0';
        location[sizeof(location) - 1] = '\0';
        idstr[sizeof(idstr) - 1] = '\0';
        if(ioctl(fd, EVIOCGNAME(sizeof(name) - 1), &name) < 1) {
            //fprintf(stderr, "could not get device name for %s, %s\n", device, strerror(errno));
            name[0] = '\0';
        }
        if(ioctl(fd, EVIOCGPHYS(sizeof(location) - 1), &location) < 1) {
            //fprintf(stderr, "could not get location for %s, %s\n", device, strerror(errno));
            location[0] = '\0';
        }
        if(ioctl(fd, EVIOCGUNIQ(sizeof(idstr) - 1), &idstr) < 1) {
            //fprintf(stderr, "could not get idstring for %s, %s\n", device, strerror(errno));
            idstr[0] = '\0';
        }

        if (ioctl(fd, EVIOCSCLOCKID, &clkid) != 0) {
            fprintf(stderr, "Can't enable monotonic clock reporting: %s\n", strerror(errno));
            // a non-fatal error
        }
    }

    new_ufds = realloc(ufds, sizeof(ufds[0]) * (nfds + 1));
    if(new_ufds == NULL) {
        fprintf(stderr, "out of memory\n");
        close(fd);
        return -1;
    }
    ufds = new_ufds;
    new_device_names = realloc(device_names, sizeof(device_names[0]) * (nfds + 1));
    if(new_device_names == NULL) {
        fprintf(stderr, "out of memory\n");
        close(fd);
        return -1;
    }
    device_names = new_device_names;
    new_device_states = realloc(device_states, sizeof(device_states[0]) * (nfds + 1));
    if(new_device_states == NULL) {
        fprintf(stderr, "out of memory\n");
        close(fd);
        return -1;
    }
    device_states = new_device_states;
    ufds[nfds].fd = -1;
    ufds[nfds].events = POLLIN;
    device_names[nfds] = NULL;
    memset(&device_states[nfds], 0, sizeof(device_states[0]));

    if(print_flags & PRINT_DEVICE)
        printf("add device %d: %s\n", nfds, device);
    if((print_flags & PRINT_DEVICE_INFO) && !is_fifo)
        printf("  bus:      %04x\n"
               "  vendor    %04x\n"
               "  product   %04x\n"
               "  version   %04x\n",
               id.bustype, id.vendor, id.product, id.version);
    if(print_flags & PRINT_DEVICE_NAME)
        printf("  name:     \"%s\"\n", name);
    if((print_flags & PRINT_DEVICE_INFO) && !is_fifo)
        printf("  location: \"%s\"\n"
               "  id:       \"%s\"\n", location, idstr);
    if((print_flags & PRINT_VERSION) && !is_fifo)
        printf("  version:  %d.%d.%d\n",
               version >> 16, (version >> 8) & 0xff, version & 0xff);

    if((print_flags & PRINT_POSSIBLE_EVENTS) && !is_fifo) {
        print_possible_events(fd, print_flags);
    }

    if((print_flags & PRINT_INPUT_PROPS) && !is_fifo) {
        print_input_props(fd);
    }
    if((print_flags & PRINT_HID_DESCRIPTOR) && !is_fifo) {
        print_hid_descriptor(id.bustype, id.vendor, id.product);
    }

    device_names[nfds] = strdup(device);
    if (device_names[nfds] == NULL) {
        fprintf(stderr, "out of memory\n");
        close(fd);
        return -1;
    }
    ufds[nfds].fd = fd;
    device_states[nfds].is_fifo = is_fifo;
    device_states[nfds].fifo_pending = 0;
    nfds++;

    return 0;
}

int close_device(const char *device, int print_flags)
{
    int i;
    for(i = 1; i < nfds; i++) {
        if(strcmp(device_names[i], device) == 0) {
            int count = nfds - i - 1;
            if(print_flags & PRINT_DEVICE)
                printf("remove device %d: %s\n", i, device);
            close(ufds[i].fd);
            free(device_names[i]);
            memmove(device_names + i, device_names + i + 1, sizeof(device_names[0]) * count);
            memmove(ufds + i, ufds + i + 1, sizeof(ufds[0]) * count);
            memmove(device_states + i, device_states + i + 1, sizeof(device_states[0]) * count);
            nfds--;
            return 0;
        }
    }
    if(print_flags & PRINT_DEVICE_ERRORS)
        fprintf(stderr, "remote device: %s not found\n", device);
    return -1;
}

static int read_notify(const char *dirname, int nfd, int print_flags)
{
    int res;
    char devname[PATH_MAX];
    char *filename;
    char event_buf[512];
    int event_size;
    int event_pos = 0;
    struct inotify_event *event;

    res = read(nfd, event_buf, sizeof(event_buf));
    if(res < (int)sizeof(*event)) {
        if(errno == EINTR)
            return 0;
        fprintf(stderr, "could not get inotify events, %s\n", strerror(errno));
        return 1;
    }
    //printf("got %d bytes of event information\n", res);

    strcpy(devname, dirname);
    filename = devname + strlen(devname);
    *filename++ = '/';

    while(res >= (int)sizeof(*event)) {
        event = (struct inotify_event *)(event_buf + event_pos);
        //printf("%d: %08x \"%s\"\n", event->wd, event->mask, event->len ? event->name : "");
        if(event->len) {
            if (is_openfde_fifo_name(event->name)) {
                event_size = sizeof(*event) + event->len;
                res -= event_size;
                event_pos += event_size;
                continue;
            }
            strcpy(filename, event->name);
            if(event->mask & IN_CREATE) {
                open_device(devname, print_flags);
            }
            else {
                close_device(devname, print_flags);
            }
        }
        event_size = sizeof(*event) + event->len;
        res -= event_size;
        event_pos += event_size;
    }
    return 0;
}

static int scan_dir(const char *dirname, int print_flags)
{
    char devname[PATH_MAX];
    char *filename;
    DIR *dir;
    struct dirent *de;
    dir = opendir(dirname);
    if(dir == NULL)
        return -1;
    strcpy(devname, dirname);
    filename = devname + strlen(devname);
    *filename++ = '/';
    while((de = readdir(dir))) {
        if(de->d_name[0] == '.' &&
           (de->d_name[1] == '\0' ||
            (de->d_name[1] == '.' && de->d_name[2] == '\0')))
            continue;
        if (is_openfde_fifo_name(de->d_name))
            continue;
        strcpy(filename, de->d_name);
        open_device(devname, print_flags);
    }
    closedir(dir);
    return 0;
}

static int print_input_event(const struct input_event *event, int get_time, int print_device,
                             int print_flags, int sync_rate, int64_t *last_sync_time,
                             int *event_count, const char *newline, int index) {
    if(get_time) {
        printf("[%8ld.%06ld] ", event->time.tv_sec, event->time.tv_usec);
    }
    if(print_device)
        printf("%s: ", device_names[index]);
    print_event(event->type, event->code, event->value, print_flags);
    if(sync_rate && event->type == 0 && event->code == 0) {
        int64_t now = event->time.tv_sec * 1000000LL + event->time.tv_usec;
        if(*last_sync_time)
            printf(" rate %lld", 1000000LL / (now - *last_sync_time));
        *last_sync_time = now;
    }
    printf("%s", newline);
    if(*event_count && --(*event_count) == 0)
        return 1;
    return 0;
}

static int read_fifo_events(int index, int get_time, int print_device, int print_flags,
                            int sync_rate, int64_t *last_sync_time, int *event_count,
                            const char *newline, short revents) {
    unsigned char read_buf[sizeof(struct input_event) * 64];
    struct device_state *state = &device_states[index];
    while (1) {
        ssize_t res = read(ufds[index].fd, read_buf, sizeof(read_buf));
        if (res < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            fprintf(stderr, "could not get fifo event for %s, %s\n", device_names[index], strerror(errno));
            return -1;
        }
        if (res == 0) {
            if (revents & (POLLHUP | POLLERR)) {
                int new_fd = open(device_names[index], O_RDWR | O_NONBLOCK | O_CLOEXEC);
                if (new_fd < 0) {
                    fprintf(stderr, "could not reopen fifo %s, %s\n",
                            device_names[index], strerror(errno));
                    return -1;
                }
                close(ufds[index].fd);
                ufds[index].fd = new_fd;
            }
            break;
        }

        size_t offset = 0;
        if (state->fifo_pending) {
            size_t needed = sizeof(struct input_event) - state->fifo_pending;
            if ((size_t)res < needed) {
                memcpy(state->fifo_data + state->fifo_pending, read_buf, res);
                state->fifo_pending += res;
                continue;
            }
            memcpy(state->fifo_data + state->fifo_pending, read_buf, needed);
            struct input_event event;
            memcpy(&event, state->fifo_data, sizeof(event));
            if (print_input_event(&event, get_time, print_device, print_flags, sync_rate,
                                  last_sync_time, event_count, newline, index)) {
                return 1;
            }
            state->fifo_pending = 0;
            offset += needed;
        }
        while ((size_t)(res - offset) >= sizeof(struct input_event)) {
            struct input_event event;
            memcpy(&event, read_buf + offset, sizeof(event));
            if (print_input_event(&event, get_time, print_device, print_flags, sync_rate,
                                  last_sync_time, event_count, newline, index)) {
                return 1;
            }
            offset += sizeof(struct input_event);
        }
        if ((size_t)res > offset) {
            state->fifo_pending = res - offset;
            memcpy(state->fifo_data, read_buf + offset, state->fifo_pending);
        }
    }
    return 0;
}

static void cleanup_fds(void) {
    int i;
    for (i = 0; i < nfds; i++) {
        if (ufds && ufds[i].fd >= 0) {
            close(ufds[i].fd);
        }
    }
    for (i = 0; i < nfds; i++) {
        free(device_names ? device_names[i] : NULL);
    }
    free(ufds);
    free(device_names);
    free(device_states);
}

static void open_default_fifo_devices(int print_flags) {
    for (size_t i = 0; i < sizeof(openfde_fifo_paths) / sizeof(openfde_fifo_paths[0]); i++) {
        open_device(openfde_fifo_paths[i], print_flags & ~PRINT_DEVICE_ERRORS);
    }
}

static void usage(char *name)
{
    fprintf(stderr, "Usage: %s [-t] [-n] [-s switchmask] [-S] [-v [mask]] [-d] [-p] [-i] [-l] [-q] [-c count] [-r] [device]\n", name);
    fprintf(stderr, "    -t: show time stamps\n");
    fprintf(stderr, "    -n: don't print newlines\n");
    fprintf(stderr, "    -s: print switch states for given bits\n");
    fprintf(stderr, "    -S: print all switch states\n");
    fprintf(stderr, "    -v: verbosity mask (errs=1, dev=2, name=4, info=8, vers=16, pos. events=32, props=64)\n");
    fprintf(stderr, "    -d: show HID descriptor, if available\n");
    fprintf(stderr, "    -p: show possible events (errs, dev, name, pos. events)\n");
    fprintf(stderr, "    -i: show all device info and possible events\n");
    fprintf(stderr, "    -l: label event types and names in plain text\n");
    fprintf(stderr, "    -q: quiet (clear verbosity mask)\n");
    fprintf(stderr, "    -c: print given number of events then exit\n");
    fprintf(stderr, "    -r: print rate events are received\n");
    fprintf(stderr, "When no [device] is given, getevent monitors /dev/input evdev devices and OpenFDE FIFOs\n");
}

int getevent_main(int argc, char *argv[])
{
    int c;
    int i;
    int res;
    int ret = 0;
    int get_time = 0;
    int print_device = 0;
    char *newline = "\n";
    uint16_t get_switch = 0;
    struct input_event event;
    int print_flags = 0;
    int print_flags_set = 0;
    int dont_block = -1;
    int event_count = 0;
    int sync_rate = 0;
    int64_t last_sync_time = 0;
    const char *device = NULL;
    const char *device_path = "/dev/input";

    /* disable buffering on stdout */
    setbuf(stdout, NULL);

    opterr = 0;
    do {
        c = getopt(argc, argv, "tns:Sv::dpilqc:rh");
        if (c == EOF)
            break;
        switch (c) {
        case 't':
            get_time = 1;
            break;
        case 'n':
            newline = "";
            break;
        case 's':
            get_switch = strtoul(optarg, NULL, 0);
            if(dont_block == -1)
                dont_block = 1;
            break;
        case 'S':
            get_switch = ~0;
            if(dont_block == -1)
                dont_block = 1;
            break;
        case 'v':
            if(optarg)
                print_flags |= strtoul(optarg, NULL, 0);
            else
                print_flags |= PRINT_DEVICE | PRINT_DEVICE_NAME | PRINT_DEVICE_INFO | PRINT_VERSION;
            print_flags_set = 1;
            break;
        case 'd':
            print_flags |= PRINT_HID_DESCRIPTOR;
            break;
        case 'p':
            print_flags |= PRINT_DEVICE_ERRORS | PRINT_DEVICE
                    | PRINT_DEVICE_NAME | PRINT_POSSIBLE_EVENTS | PRINT_INPUT_PROPS;
            print_flags_set = 1;
            if(dont_block == -1)
                dont_block = 1;
            break;
        case 'i':
            print_flags |= PRINT_ALL_INFO;
            print_flags_set = 1;
            if(dont_block == -1)
                dont_block = 1;
            break;
        case 'l':
            print_flags |= PRINT_LABELS;
            break;
        case 'q':
            print_flags_set = 1;
            break;
        case 'c':
            event_count = atoi(optarg);
            dont_block = 0;
            break;
        case 'r':
            sync_rate = 1;
            break;
        case '?':
            fprintf(stderr, "%s: invalid option -%c\n",
                argv[0], optopt);
        case 'h':
            usage(argv[0]);
            exit(1);
        }
    } while (1);
    if(dont_block == -1)
        dont_block = 0;

    if (optind + 1 == argc) {
        device = argv[optind];
        optind++;
    }
    if (optind != argc) {
        usage(argv[0]);
        exit(1);
    }
    nfds = 1;
    ufds = calloc(1, sizeof(ufds[0]));
    device_names = calloc(1, sizeof(device_names[0]));
    device_states = calloc(1, sizeof(device_states[0]));
    if (!ufds || !device_names || !device_states) {
        fprintf(stderr, "out of memory\n");
        ret = 1;
        goto done;
    }
    ufds[0].fd = inotify_init();
    ufds[0].events = POLLIN;
    if (ufds[0].fd < 0) {
        fprintf(stderr, "could not initialize inotify, %s\n", strerror(errno));
        ret = 1;
        goto done;
    }
    if(device) {
        if(!print_flags_set)
            print_flags |= PRINT_DEVICE_ERRORS;
        res = open_device(device, print_flags);
        if(res < 0) {
            ret = 1;
            goto done;
        }
    } else {
        if(!print_flags_set)
            print_flags |= PRINT_DEVICE_ERRORS | PRINT_DEVICE | PRINT_DEVICE_NAME;
        print_device = 1;
        open_default_fifo_devices(print_flags);
        res = inotify_add_watch(ufds[0].fd, device_path, IN_DELETE | IN_CREATE);
        if(res < 0) {
            fprintf(stderr, "could not add watch for %s, %s\n", device_path, strerror(errno));
            ret = 1;
            goto done;
        }
        res = scan_dir(device_path, print_flags);
        if(res < 0) {
            fprintf(stderr, "scan dir failed for %s\n", device_path);
            ret = 1;
            goto done;
        }
    }

    if(get_switch) {
        for(i = 1; i < nfds; i++) {
            if (device_states[i].is_fifo)
                continue;
            uint16_t sw;
            res = ioctl(ufds[i].fd, EVIOCGSW(1), &sw);
            if(res < 0) {
                fprintf(stderr, "could not get switch state, %s\n", strerror(errno));
                ret = 1;
                goto done;
            }
            sw &= get_switch;
            printf("%04x%s", sw, newline);
        }
    }

    if(dont_block)
        goto done;

    while(1) {
        do {
            res = poll(ufds, nfds, -1);
        } while (res < 0 && errno == EINTR);
        if (res < 0) {
            fprintf(stderr, "poll failed, %s\n", strerror(errno));
            ret = 1;
            goto done;
        }
        if(ufds[0].revents & POLLIN) {
            read_notify(device_path, ufds[0].fd, print_flags);
        }
        for(i = 1; i < nfds; i++) {
            if(ufds[i].revents) {
                if (device_states[i].is_fifo) {
                    if (ufds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                        res = read_fifo_events(i, get_time, print_device, print_flags, sync_rate,
                                               &last_sync_time, &event_count, newline, ufds[i].revents);
                        if (res < 0) {
                            ret = 1;
                            goto done;
                        }
                        if (res > 0)
                            goto done;
                    }
                } else if(ufds[i].revents & POLLIN) {
                    do {
                        res = read(ufds[i].fd, &event, sizeof(event));
                    } while (res < 0 && errno == EINTR);
                    if(res < (int)sizeof(event)) {
                        fprintf(stderr, "could not get evdev event, %s\n", strerror(errno));
                        ret = 1;
                        goto done;
                    }
                    if (print_input_event(&event, get_time, print_device, print_flags, sync_rate,
                                          &last_sync_time, &event_count, newline, i))
                        goto done;
                }
            }
        }
    }

done:
    cleanup_fds();
    return ret;
}
