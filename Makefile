CC=gcc
CFLAGS=-Wall -O3
#CFLAGS=-g
INCLUDES=
LDFLAGS=
#LIBS=-pthread

SRCS=main.c lru_cache.c config_parser.c miss_filter.c miss_table.c static_buffer.c ghost_cache.c
OBJS=$(SRCS:.c=.o)
PROG=bouncer

all: $(PROG)

.c.o:
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(PROG): $(OBJS)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(OBJS) $(LDFLAGS) $(LIBS)

clean:
	$(RM) *.o *~ $(PROG)
