PROG1	= resource_monitor
OBJS1	= main.c sampler.c store.c config.c prom.c sendout.c netconn.c alerts.c context.c ACAP.c cJSON.c
PROGS	= $(PROG1)

PKGS = glib-2.0 gio-2.0 axevent axparameter fcgi libcurl

CFLAGS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --cflags $(PKGS))
LDLIBS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs $(PKGS))
LDLIBS  += -s -lm -ldl -lpthread
CFLAGS += -Wall -Wno-format-overflow -O2

all:	$(PROGS)

$(PROG1): $(OBJS1)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

clean:
	rm -rf $(PROGS) *.o $(LIBDIR) *.eap* *_LICENSE.txt manifest.json package.conf* param.conf
