#---------------------------------------------------------------------------------
# Gemi3DS - cliente de Gemini AI para Nintendo 3DS
# Requiere devkitPro (devkitARM + libctru + citro2d/citro3d) y los portlibs:
#   dkp-pacman -S 3ds-dev 3ds-curl 3ds-mbedtls
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Configura DEVKITARM en tu entorno. export DEVKITARM=<ruta>/devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

TARGET		:=	Gemi3DS
BUILD		:=	build
SOURCES		:=	source lib
DATA		:=	data
INCLUDES	:=	source lib
ROMFS		:=	romfs

APP_TITLE	:=	Gemi3DS
APP_DESCRIPTION	:=	Cliente de Gemini AI para 3DS
APP_AUTHOR	:=	VENOMSHiNE
ICON		:=	icon.png

#---------------------------------------------------------------------------------
ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS	:=	-g -Wall -O2 -mword-relocations -ffunction-sections -fdata-sections \
			-Wno-unused-function -Wno-unused-variable -Wno-format-truncation \
			$(ARCH)
CFLAGS	+=	$(INCLUDE) -D__3DS__ -DCJSON_NESTING_LIMIT=256

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11
ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map) -Wl,--gc-sections -Wl,--wrap=fontGlyphIndexFromCodePoint

LIBS	:= -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lcitro2d -lcitro3d -lctru -lm $(EXTRA_LIBS)
LIBDIRS	:= $(PORTLIBS) $(CTRULIB)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------
.DEFAULT_GOAL := all

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)
export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export LD	:=	$(CC)
export OFILES	:=	$(CFILES:.c=.o) $(SFILES:.s=.o)
export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)
export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export APP_ICON := $(TOPDIR)/$(ICON)
export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS)

.PHONY: all clean cia

#---------------------------------------------------------------------------------
# .cia instalable (requiere bannertool y makerom en el PATH)
#---------------------------------------------------------------------------------
cia: all
	@bannertool makebanner -i cia/banner.png -a cia/banner.wav -o $(BUILD)/banner.bnr
	@bannertool makesmdh -s "$(APP_TITLE)" -l "$(APP_DESCRIPTION)" -p "$(APP_AUTHOR)" -i $(ICON) -o $(BUILD)/icon.icn
	@makerom -f cia -o $(TARGET).cia -elf $(TARGET).elf -rsf cia/gemi3ds.rsf -icon $(BUILD)/icon.icn \
		-banner $(BUILD)/banner.bnr -exefslogo -target t -DAPP_VERSION_MAJOR=1 -ver 1024
	@echo built ... $(TARGET).cia

all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(TARGET).cia $(OUTPUT).smdh $(TARGET).elf

#---------------------------------------------------------------------------------
else

$(OUTPUT).3dsx	:	$(OUTPUT).elf $(OUTPUT).smdh
$(OUTPUT).elf	:	$(OFILES)

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
