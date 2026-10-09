# Standalone build for the AHX core library and note demo.

CC      ?= cc
AR      ?= ar
CFLAGS  ?= -std=c99 -Wall -Wextra -Wpedantic -O2

# Select the fixed-point device voice or the floating-point reference table.
VOICE   ?= ahx_voice_fixed.c

# Shared reader, voice state, filter and player sources.
CORE    := ahx_read.c ahx_fixed.c ahx_voice.c ahx_player.c ahx_module.c
HEADERS := ahx.h ahx_config.h ahx_fixed.h ahx_libc.h ahx_module.h ahx_note.h \
           ahx_player.h ahx_voice.h

all: libahx-core.a ahx_note_demo ahx_audition_check

# Build the library from the shared sources and selected voice implementation.
libahx-core.a: $(CORE:.c=.o) $(patsubst %.c,%.o,$(VOICE))
	$(AR) rcs $@ $^

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c -o $@ $<

# Build the standalone note demo with a caller-owned envelope.
ahx_note_demo: host/ahx_note_demo.c ahx_note.h ahx_fixed.c ahx_fixed.h ahx_voice_fixed.c $(HEADERS)
	$(CC) $(CFLAGS) -I. -o $@ host/ahx_note_demo.c ahx_fixed.c ahx_voice_fixed.c -lm

# Build the audition check over the host's half of the voice. It compares two paths through the
# player - a pattern row and a note - so either half of the voice would do; the host's is the one
# that comes with ahx_waves_generate(), which is what hands the player its tables.
ahx_audition_check: host/ahx_audition_check.c $(CORE) ahx_tables.c $(HEADERS)
	$(CC) $(CFLAGS) -I. -o $@ host/ahx_audition_check.c $(CORE) ahx_tables.c -lm

# Render a short sequence and reject silent output.
check: ahx_note_demo ahx_audition_check
	@out=$$(./ahx_note_demo -o /dev/null c3 e3 g3 c4) || exit 1; \
	 echo "$$out"; \
	 case "$$out" in *"peak=0"*) echo "ahx-core: the demo rendered silence" >&2; exit 1;; esac
	@./ahx_audition_check

clean:
	rm -f *.o libahx-core.a ahx_note_demo ahx_audition_check

.PHONY: all check clean
