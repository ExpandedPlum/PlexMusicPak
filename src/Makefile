###########################################################
# Supported platforms
SUPPORTED_PLATFORMS = tg5040
DEFAULT_PLATFORM = tg5040

# Set platform from environment or default
ifeq (,$(PLATFORM))
PLATFORM=$(UNION_PLATFORM)
endif

ifeq (,$(PLATFORM))
PLATFORM=$(DEFAULT_PLATFORM)
endif

# Validate platform
ifeq (,$(filter $(PLATFORM),$(SUPPORTED_PLATFORMS)))
$(error Invalid PLATFORM '$(PLATFORM)'. Supported: $(SUPPORTED_PLATFORMS))
endif

# Validate cross-compiler
ifeq (,$(CROSS_COMPILE))
$(error Missing CROSS_COMPILE for this toolchain)
endif

###########################################################

TARGET = plexmusic
PRODUCT = ../bin/$(PLATFORM)/$(TARGET).elf

# Common headers path (NextUI workspace)
NEXTUI_COMMON = ../../../NextUI/workspace/all/common
NEXTUI_PLATFORM = ../../../NextUI/workspace/$(PLATFORM)/platform

INCDIR  = -I.
INCDIR += -I./include
INCDIR += -I./include/parson
INCDIR += -I$(NEXTUI_COMMON)
INCDIR += -I$(NEXTUI_PLATFORM)
INCDIR += -I../../../NextUI/workspace/$(PLATFORM)/libmsettings
INCDIR += -I../../../NextUI/workspace/all/minarch/libretro-common/include

CC = $(CROSS_COMPILE)gcc

MY_CFLAGS  = -O2 -fomit-frame-pointer -std=gnu99
MY_CFLAGS += $(INCDIR)
MY_CFLAGS += -DPLATFORM=\"$(PLATFORM)\"
MY_CFLAGS += -DUSE_SDL2 -DUSE_GLES -DGL_GLEXT_PROTOTYPES
MY_CFLAGS += -I$(PREFIX)/include -I$(PREFIX_LOCAL)/include
# Force-include defines.h so BTN_ID_COUNT and friends are available to all TUs
MY_CFLAGS += -include $(NEXTUI_COMMON)/defines.h

MY_LDFLAGS  = -L$(PREFIX)/lib -L$(PREFIX_LOCAL)/lib
MY_LDFLAGS += -L$(PREFIX)/lib/$(CROSS_TRIPLE)
MY_LDFLAGS += -L../../../NextUI/workspace/$(PLATFORM)/libmsettings
MY_LDFLAGS += $(shell pkg-config --libs sdl2 glesv2 2>/dev/null || echo "-lSDL2 -lGLESv2")
MY_LDFLAGS += -lSDL2_image -lSDL2_ttf
MY_LDFLAGS += -lmsettings -lasound
MY_LDFLAGS += -lzip -lpthread -ldl -lz -lm

# NextUI common source files
NEXTUI_COMMON_SRC = \
    $(NEXTUI_COMMON)/utils.c \
    $(NEXTUI_COMMON)/api.c \
    $(NEXTUI_COMMON)/config.c \
    $(NEXTUI_COMMON)/scaler.c

NEXTUI_PLATFORM_SRC = $(NEXTUI_PLATFORM)/platform.c

# Main source files (explicit list to avoid picking up stub .c files for modules
# we intentionally exclude, and to control link order)
SOURCE = main.c \
         plex_log.c \
         module_auth.c \
         module_browse.c \
         module_player.c \
         module_settings.c \
         plex_queue.c \
         plex_api.c \
         plex_art.c \
         plex_auth.c \
         plex_config.c \
         plex_downloads.c \
         plex_favorites.c \
         plex_net.c \
         file_util.c \
         plexd_client.c \
         audio_out.c \
         background.c \
         settings.c \
         ui_fonts.c \
         ui_icons.c \
         ui_utils.c \
         ui_keyboard.c \
         module_common.c \
         include/parson/parson.c \
         $(NEXTUI_COMMON_SRC) \
         $(NEXTUI_PLATFORM_SRC)

all:
	@mkdir -p ../bin/$(PLATFORM)
	$(CC) $(SOURCE) -o $(PRODUCT) $(MY_CFLAGS) $(MY_LDFLAGS)

clean:
	rm -f ../bin/tg5040/$(TARGET).elf
	rm -f *.o
