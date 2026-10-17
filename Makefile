# Prince of Persia 2 port — build with `make` (needs Xcode CLT and SDL2 from Homebrew)
CC      ?= clang
SDL_CF  := $(shell sdl2-config --cflags)
SDL_LD  := $(shell sdl2-config --libs)
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Isrc -Ithird_party/musashi $(SDL_CF)
LDFLAGS += $(SDL_LD) -lz -framework CoreText -framework CoreGraphics -framework CoreFoundation -framework Cocoa
BUILD   := build

MUSASHI := third_party/musashi
MUSASHI_SRC := $(MUSASHI)/m68kcpu.c $(MUSASHI)/m68kdasm.c $(BUILD)/m68kops.c $(MUSASHI)/softfloat/softfloat.c
GAME_SRC := $(wildcard src/game/*.c src/game/*/*.c)
SRC := $(wildcard src/*.c src/mac/*.c src/cpu/*.c src/fs/*.c src/platform/*.c) $(filter-out src/game/gen/%,$(GAME_SRC)) src/game/gen/glue.c
OBJC_SRC := $(wildcard src/platform/*.m)
TRAPS_JSON ?= $(CURDIR)/../work/traps.json
# CORE=mini: release build without the 68k emulator (src/cpu/minicore.c runs only the
# dispatch instructions left once every function is ported); default: the Musashi emulator.
ifeq ($(CORE),mini)
CFLAGS += -DPOP2_MINICORE -I$(MUSASHI)
# (src/cpu/minicore.c is part of src/cpu/*.c)
CORE_OBJ :=
CORE_DEP :=
else
SRC := $(filter-out src/cpu/minicore.c,$(SRC))
CORE_OBJ := $(BUILD)/m68kcpu.o $(BUILD)/m68kdasm.o $(BUILD)/m68kops.o $(BUILD)/softfloat.o
CORE_DEP := $(BUILD)/m68kops.c
endif
OBJ := $(patsubst %.c,$(BUILD)/%.o,$(SRC)) $(patsubst %.m,$(BUILD)/%.o,$(OBJC_SRC)) $(CORE_OBJ)

all: $(BUILD)/pop2

# Regenerate glue whenever ported sources or the function index change.
src/game/gen/glue.c src/game/gen/proto.h: tools/gen_port.py tools/fn_index.json $(wildcard tools/sig_overrides/*.json) $(wildcard tools/coderes/*.json) $(filter-out src/game/gen/%,$(GAME_SRC))
	python3 tools/gen_port.py $(TRAPS_JSON)

$(BUILD)/m68kmake: $(MUSASHI)/m68kmake.c
	@mkdir -p $(BUILD)
	$(CC) -O1 -w -o $@ $<

$(BUILD)/m68kops.c: $(BUILD)/m68kmake $(MUSASHI)/m68k_in.c
	cd $(BUILD) && ./m68kmake . $(CURDIR)/$(MUSASHI)/m68k_in.c

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

$(BUILD)/%.o: %.m
	@mkdir -p $(dir $@)
	$(CC) -O2 -g -fobjc-arc -Wall -Isrc -c $< -o $@

# The game sources are compiled from a rewritten copy (tools/mkcheck.py): every call to a
# ported function goes through its nat_ wrapper, which pushes the arguments on the emulated
# stack and enters the function through its glue, exactly as an original 68k caller would.
PORTED_SRC := $(filter-out src/game/gen/%,$(wildcard src/game/*.c))
NAT_GAME   := $(patsubst src/game/%.c,$(BUILD)/nat/%.o,$(PORTED_SRC))
NAT_OBJ    := $(filter-out $(patsubst %.c,$(BUILD)/%.o,$(PORTED_SRC)),$(OBJ)) $(NAT_GAME) $(BUILD)/src/game/gen/chk.o
$(BUILD)/nat/.stamp: tools/mkcheck.py $(PORTED_SRC) src/game/gen/glue.c
	@mkdir -p $(BUILD)/nat
	python3 tools/mkcheck.py $(BUILD)/nat nat_
	@touch $@
$(BUILD)/nat/%.c: $(BUILD)/nat/.stamp ;
$(BUILD)/nat/%.o: $(BUILD)/nat/%.c $(wildcard src/*/*.h src/*.h) src/game/gen/proto.h src/game/gen/proto_chk.h
	$(CC) $(CFLAGS) -DPORT_CHECK_BUILD -Isrc/game -c $< -o $@

$(BUILD)/pop2: $(CORE_DEP) $(NAT_OBJ)
	$(CC) $(NAT_OBJ) $(LDFLAGS) -o $@

# ---- check build: calls between ported functions go through their 68k entry (see tools/mkcheck.py)
CHK_GAME   := $(patsubst src/game/%.c,$(BUILD)/chk/%.o,$(PORTED_SRC))
CHK_OBJ    := $(filter-out $(patsubst %.c,$(BUILD)/%.o,$(PORTED_SRC)),$(OBJ)) $(CHK_GAME) $(BUILD)/src/game/gen/chk.o
src/game/gen/chk.c src/game/gen/proto_chk.h: src/game/gen/glue.c
$(BUILD)/chk/.stamp: tools/mkcheck.py $(PORTED_SRC) src/game/gen/glue.c
	@mkdir -p $(BUILD)/chk
	python3 tools/mkcheck.py $(BUILD)/chk
	@touch $@
$(BUILD)/chk/%.c: $(BUILD)/chk/.stamp ;
$(BUILD)/chk/%.o: $(BUILD)/chk/%.c $(wildcard src/*/*.h src/*.h) src/game/gen/proto.h src/game/gen/proto_chk.h
	$(CC) $(CFLAGS) -DPORT_CHECK_BUILD -Isrc/game -c $< -o $@
$(BUILD)/pop2check: $(CORE_DEP) $(CHK_OBJ)
	$(CC) $(CHK_OBJ) $(LDFLAGS) -o $@
check: $(BUILD)/pop2check

# ---- macOS app bundle (no game data inside: it asks for the user's own disk image)
#   make app BUILD=build/release CORE=mini   (release build: no 68k emulator, no debug hook)
APP := $(BUILD)/PoP2 Port.app
SDL3_LIBDIR ?= $(shell (command -v pkg-config >/dev/null && pkg-config --variable=libdir sdl3) 2>/dev/null || echo /opt/homebrew/opt/sdl3/lib)
app: $(BUILD)/pop2 packaging/Info.plist
	rm -rf "$(APP)"
	mkdir -p "$(APP)/Contents/MacOS" "$(APP)/Contents/Frameworks" "$(APP)/Contents/Resources"
	cp $(BUILD)/pop2 "$(APP)/Contents/MacOS/pop2"
	strip -S "$(APP)/Contents/MacOS/pop2"   # debug map: object paths of the build machine
	cp packaging/Info.plist "$(APP)/Contents/Info.plist"
	SDL2=$$(otool -L $(BUILD)/pop2 | awk '/libSDL2/{print $$1}'); \
	  cp "$$SDL2" "$(APP)/Contents/Frameworks/"; chmod u+w "$(APP)/Contents/Frameworks/"*; \
	  install_name_tool -change "$$SDL2" "@executable_path/../Frameworks/$$(basename $$SDL2)" "$(APP)/Contents/MacOS/pop2"; \
	  install_name_tool -id "@executable_path/../Frameworks/$$(basename $$SDL2)" "$(APP)/Contents/Frameworks/$$(basename $$SDL2)"; \
	  if strings "$$SDL2" | grep -q libSDL3; then \
	    SDL3="$(SDL3_LIBDIR)/libSDL3.dylib"; [ -f "$$SDL3" ] || { echo "sdl2-compat needs SDL3: $$SDL3 not found (set SDL3_LIBDIR)"; exit 1; }; \
	    cp -L "$$SDL3" "$(APP)/Contents/Frameworks/libSDL3.dylib"; chmod u+w "$(APP)/Contents/Frameworks/libSDL3.dylib"; \
	    install_name_tool -id "@loader_path/libSDL3.dylib" "$(APP)/Contents/Frameworks/libSDL3.dylib"; fi
	codesign --force --deep -s - "$(APP)"
	@echo "built $(APP)"

clean:
	rm -rf $(BUILD)

.PHONY: all clean check app

# keep the rewritten sources (for reading compiler errors and debugging)
.SECONDARY: $(patsubst src/game/%.c,$(BUILD)/nat/%.c,$(PORTED_SRC)) $(patsubst src/game/%.c,$(BUILD)/chk/%.c,$(PORTED_SRC))
