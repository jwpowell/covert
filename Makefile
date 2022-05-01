
TOPDIR   := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
SRCDIR   := $(TOPDIR)/src
INCDIR   := $(TOPDIR)/include

CC       := gcc
CPPFLAGS := -I$(INCDIR) -D_GNU_SOURCE
CFLAGS   := -g -std=c11 -O2 -Wall -Wextra -Werror=pedantic -pipe -pthread
LDFLAGS  := 
LDLIBS   :=

SRCS     := $(shell find $(SRCDIR) -type f -name "*.c")
OBJS     := $(patsubst %.c,%.o,$(SRCS))
DEPS     := $(patsubst %.c,%.d,$(SRCS))

TARGET   := covert

.PHONY: all clean

all: $(TARGET)

clean:
	$(RM) $(TARGET) $(OBJS) $(DEPS) 

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

-include $(DEPS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -o $@ -c $<