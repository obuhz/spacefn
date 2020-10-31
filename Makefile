spacefn: spacefn.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

CFLAGS := `pkg-config --cflags libevdev libconfig`
LDFLAGS := `pkg-config --libs libevdev libconfig`
