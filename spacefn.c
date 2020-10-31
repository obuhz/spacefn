/*
 * spacefn-evdev.c
 * James Laird-Wah (abrasive) 2018
 * This code is in the public domain.
 */

#include <fcntl.h>
#include <libconfig.h>
#include <libevdev/libevdev-uinput.h>
#include <libevdev/libevdev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int *keymap_k, *keymap_v, keymap_l = 0;

int *sfn_k, *sfn_v, sfn_l = 0;

int *shift_k, *shift_v, shift_l = 0;
int lshift_status = 0, rshift_status = 0;

// Ordered unique key buffer {{{1
#define MAX_BUFFER 8
unsigned int buffer[MAX_BUFFER];
unsigned int n_buffer = 0;

int interval = 200000;

// Spacefn remapping
unsigned int sfn_remap(unsigned int code) {
  for (int s = 0; s < sfn_l; s++) {
    if (code == sfn_k[s]) {
      return sfn_v[s];
    }
  }

  return 0;
}

// Global device handles {{{1
struct libevdev *idev;
struct libevdev_uinput *odev;
int fd;

static int buffer_contains(unsigned int code) {
  for (int i = 0; i < n_buffer; i++)
    if (buffer[i] == code) return 1;

  return 0;
}

static int buffer_remove(unsigned int code) {
  for (int i = 0; i < n_buffer; i++)
    if (buffer[i] == code) {
      memcpy(&buffer[i], &buffer[i + 1], (n_buffer - i - 1) * sizeof(*buffer));
      n_buffer--;
      return 1;
    }
  return 0;
}

static int buffer_append(unsigned int code) {
  if (n_buffer >= MAX_BUFFER) return 1;
  buffer[n_buffer++] = code;
  return 0;
}

#define V_RELEASE 0
#define V_PRESS 1
#define V_REPEAT 2
static void send_key(unsigned int code, int value) {
  libevdev_uinput_write_event(odev, EV_KEY, code, value);
  libevdev_uinput_write_event(odev, EV_SYN, SYN_REPORT, 0);
}

static void send_press(unsigned int code) { send_key(code, V_PRESS); }

static void send_release(unsigned int code) { send_key(code, V_RELEASE); }

static void send_repeat(unsigned int code) { send_key(code, V_REPEAT); }

static int shift_modifier(struct input_event *ev) {
  //某些key的shift值也要换，这里提供了一个机会

  if (ev->code == 42 && ev->value == V_PRESS) lshift_status = 1;
  if (ev->code == 54 && ev->value == V_PRESS) rshift_status = 1;
  if (ev->code == 42 && ev->value == V_RELEASE) lshift_status = 0;
  if (ev->code == 54 && ev->value == V_RELEASE) rshift_status = 0;

  if (lshift_status || rshift_status) {
    for (int i = 0; i < shift_l; i++) {
      if (ev->code == shift_k[i]) {
        ev->code = shift_v[i];
        return 1;
      }
    }
  }

  return 0;
}

static int read_one_key(struct input_event *ev) {
  int err = libevdev_next_event(
      idev, LIBEVDEV_READ_FLAG_NORMAL | LIBEVDEV_READ_FLAG_BLOCKING, ev);
  if (err) {
    fprintf(stderr, "Failed: (%d) %s\n", -err, strerror(err));
    exit(1);
  }

  if (ev->type != EV_KEY) {
    libevdev_uinput_write_event(odev, ev->type, ev->code, ev->value);
    return -1;
  }

  // key remap within every read_one_key()
  int s = keymap_l;
  for (int s = 0; s < keymap_l; s++) {
    if (keymap_k[s] == ev->code) {
      ev->code = keymap_v[s];
      break;
    }
  }
  // remap shift
  if (shift_l) {
    shift_modifier(ev);
  }
  return 0;
}

enum {
  IDLE,
  DECIDE,
  SHIFT,
} state = IDLE;

static void state_idle(void) {  
  struct input_event ev;
  for (;;) {
    while (read_one_key(&ev)) ;

    if (ev.code == KEY_SPACE && ev.value == V_PRESS) {
      state = DECIDE;
      return;
    }

    send_key(ev.code, ev.value);
  }
}

static void state_decide(void) {
  n_buffer = 0;
  struct input_event ev;
  struct timeval timeout;
  timeout.tv_sec = 0;
  timeout.tv_usec = interval;
  fd_set set;
  FD_ZERO(&set);

  while (timeout.tv_usec >= 0) {
    FD_SET(fd, &set);
    int nfds = select(fd + 1, &set, NULL, NULL, &timeout);
    if (!nfds) break;

    while (read_one_key(&ev))
      ;

    if (ev.value == V_PRESS) {
      buffer_append(ev.code);
      continue;
    }

    if (ev.code == KEY_SPACE && ev.value == V_RELEASE) {
      send_key(KEY_SPACE, V_PRESS);
      send_key(KEY_SPACE, V_RELEASE);
      for (int i = 0; i < n_buffer; i++) send_key(buffer[i], V_PRESS);
      state = IDLE;
      return;
    }

    if (ev.value == V_RELEASE && !buffer_contains(ev.code)) {
      send_key(ev.code, ev.value);
      continue;
    }

    if (ev.value == V_RELEASE && buffer_remove(ev.code)) {
      unsigned int code = sfn_remap(ev.code);
      send_key(code, V_PRESS);
      send_key(code, V_RELEASE);
      state = SHIFT;
      return;
    }
  }

  for (int i = 0; i < n_buffer; i++) {
    unsigned int code = sfn_remap(buffer[i]);
    if (code) buffer[i] = code;
    send_key(buffer[i], V_PRESS);
  }
  state = SHIFT;
}

static void state_shift(void) {
  //    n_buffer = 0;

  struct input_event ev;
  for (;;) {
    while (read_one_key(&ev))
      ;

    if (ev.code == KEY_SPACE && ev.value == V_RELEASE) {
      for (int i = 0; i < n_buffer; i++) send_key(buffer[i], V_RELEASE);
      state = IDLE;
      return;
    }
    if (ev.code == KEY_SPACE) continue;

    unsigned int code = sfn_remap(ev.code);
    if (code) {
      if (ev.value == V_PRESS)
        buffer_append(code);
      else if (ev.value == V_RELEASE)
        buffer_remove(code);

      send_key(code, ev.value);
    } else {
      send_key(ev.code, ev.value);
    }
  }
}

static void run_state_machine(void) {
  for (;;) {
    // printf("state %d\n", state);
    switch (state) {
      case IDLE:
        state_idle();
        break;
      case DECIDE:
        state_decide();
        break;
      case SHIFT:
        state_shift();
        break;
    }
  }
}

int main(int argc, char **argv) {  // {{{1
  if (argc < 2) {
    printf("usage: %s config.cfg...", argv[0]);
    return 1;
  }

  const config_setting_t *device, *lookup;
  config_t cfg, *cf;
  cf = &cfg;
  config_init(cf);

  if (!config_read_file(cf, argv[1])) {
    fprintf(stderr, "read config file error %s:%d %s\n", config_error_file(cf),
            config_error_line(cf), config_error_text(cf));
    config_destroy(cf);
    return 1;
  }

  lookup = config_lookup(cf, "interval");
  if (lookup) {
    interval = config_setting_get_int(lookup);
  }

  int len;
  lookup = config_lookup(cf, "remap.key");
  if (lookup) {
    len = config_setting_length(lookup);
    keymap_k = malloc(len * sizeof(int));
    for (int i = 0; i < len; i++) {
      keymap_k[i] = config_setting_get_int_elem(lookup, i);
    }
  }

  keymap_l = len;

  lookup = config_lookup(cf, "remap.value");
  if (lookup) {
    len = config_setting_length(lookup);
    keymap_v = malloc(len * sizeof(int));
    for (int i = 0; i < len; i++) {
      keymap_v[i] = config_setting_get_int_elem(lookup, i);
    }
  }

  lookup = config_lookup(cf, "shift.key");
  if (lookup) {
    shift_l = config_setting_length(lookup);
    shift_k = malloc(shift_l * sizeof(int));
    for (int i = 0; i < shift_l; i++) {
      shift_k[i] = config_setting_get_int_elem(lookup, i);
    }
    lookup = config_lookup(cf, "shift.value");
    shift_v = malloc(shift_l * sizeof(int));
    for (int i = 0; i < shift_l; i++) {
      shift_v[i] = config_setting_get_int_elem(lookup, i);
    }
  }

  lookup = config_lookup(cf, "spacefn.key");
  if (lookup) {
    len = config_setting_length(lookup);
    sfn_k = malloc(len * sizeof(int));
    for (int i = 0; i < len; i++) {
      sfn_k[i] = config_setting_get_int_elem(lookup, i);
    }
    sfn_l = len;
  }

  lookup = config_lookup(cf, "spacefn.value");
  if (lookup) {
    len = config_setting_length(lookup);
    sfn_v = malloc(len * sizeof(int));
    for (int i = 0; i < len; i++) {
      sfn_v[i] = config_setting_get_int_elem(lookup, i);
    }
  }

  device = config_lookup(cf, "device");

  fd = open(config_setting_get_string(device), O_RDONLY);
  if (fd < 0) {
    perror("Can't open device!");
    return 1;
  }

  config_destroy(cf);

  int err = libevdev_new_from_fd(fd, &idev);
  if (err) {
    fprintf(stderr, "Failed: (%d) %s\n", -err, strerror(err));
    return 1;
  }

  int uifd = open("/dev/uinput", O_RDWR);
  if (uifd < 0) {
    perror("open /dev/uinput");
    return 1;
  }

  err = libevdev_uinput_create_from_device(idev, uifd, &odev);
  if (err) {
    fprintf(stderr, "Failed: (%d) %s\n", -err, strerror(err));
    return 1;
  }

  usleep(200000);

  err = libevdev_grab(idev, LIBEVDEV_GRAB);
  if (err) {
    fprintf(stderr, "Failed: (%d) %s\n", -err, strerror(err));
    return 1;
  }

  run_state_machine();
}
