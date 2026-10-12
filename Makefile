# Prince of Persia 2 port — build with `make` (needs Xcode CLT and SDL2 from Homebrew)
CC      ?= clang
SDL_CF  := $(shell sdl2-config --cflags)
SDL_LD  := $(shell sdl2-config --libs)
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Isrc -Ithird_party/musashi $(SDL_CF)
LDFLAGS += $(SDL_LD) -framework CoreText -framework CoreGraphics -framework CoreFoundation
BUILD   := build

MUSASHI := third_party/musashi
MUSASHI_SRC := $(MUSASHI)/m68kcpu.c $(MUSASHI)/m68kdasm.c $(BUILD)/m68kops.c $(MUSASHI)/softfloat/softfloat.c
GAME_SRC := $(wildcard src/game/*.c src/game/*/*.c)
SRC := $(wildcard src/*.c src/mac/*.c src/cpu/*.c src/fs/*.c src/platform/*.c) $(filter-out src/game/gen/%,$(GAME_SRC)) src/game/gen/glue.c
TRAPS_JSON ?= ../work/traps.json
OBJ := $(patsubst %.c,$(BUILD)/%.o,$(SRC)) $(BUILD)/m68kcpu.o $(BUILD)/m68kdasm.o $(BUILD)/m68kops.o $(BUILD)/softfloat.o

all: $(BUILD)/pop2

# Regenerate glue whenever ported sources or the function index change.
src/game/gen/glue.c src/game/gen/proto.h: tools/gen_port.py tools/fn_index.json $(wildcard tools/sig_overrides.json) $(filter-out src/game/gen/%,$(GAME_SRC))
	python3 tools/gen_port.py $(TRAPS_JSON)

$(BUILD)/m68kmake: $(MUSASHI)/m68kmake.c
	@mkdir -p $(BUILD)
	$(CC) -O1 -w -o $@ $<

$(BUILD)/m68kops.c: $(BUILD)/m68kmake $(MUSASHI)/m68k_in.c
	cd $(BUILD) && ./m68kmake . ../$(MUSASHI)/m68k_in.c

MCFLAGS := -O2 -g -w -I$(MUSASHI) -I$(BUILD) -DM68K_EMULATE_PMMU=M68K_OPT_OFF
ifeq ($(HOOK),1)
MCFLAGS += -DM68K_INSTRUCTION_HOOK=M68K_OPT_SPECIFY_HANDLER "-DM68K_INSTRUCTION_CALLBACK(pc)=cpu_instr_hook(pc)"
CFLAGS += -DPOP2_PC_HISTORY=1
endif
$(BUILD)/m68kcpu.o: $(MUSASHI)/m68kcpu.c $(BUILD)/m68kops.c $(MUSASHI)/m68kcpu.h
	$(CC) $(MCFLAGS) -c $< -o $@
$(BUILD)/m68kdasm.o: $(MUSASHI)/m68kdasm.c
	$(CC) $(MCFLAGS) -c $< -o $@
$(BUILD)/m68kops.o: $(BUILD)/m68kops.c
	$(CC) $(MCFLAGS) -c $< -o $@
$(BUILD)/softfloat.o: $(MUSASHI)/softfloat/softfloat.c
	$(CC) $(MCFLAGS) -c $< -o $@

$(BUILD)/%.o: %.c $(wildcard src/*/*.h src/*.h) src/game/gen/proto.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/pop2: $(BUILD)/m68kops.c $(OBJ)
	$(CC) $(OBJ) $(LDFLAGS) -o $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
