#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment.")
endif

include $(DEVKITARM)/3ds_rules

TARGET      :=  EBookReaderFor3DS
BUILD       :=  build
SOURCES     :=  source source/lib source/libjpeg
INCLUDES    :=  include include/freetype2 include/utf8 include/pugi include/zip source/libjpeg
DATA        :=  data

APP_TITLE       :=  EBook Reader 3DS
APP_DESCRIPTION :=  EPUB reader for Nintendo 3DS based on the DS version
APP_AUTHOR      :=  rena
APP_ICON        :=  data/icon.png

ARCH        :=  -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS      :=  -g -Wall -O3 -ffast-math -ffunction-sections -fdata-sections $(ARCH) $(INCLUDE) -D_3DS
CXXFLAGS    :=  $(CFLAGS) -fno-rtti -fno-exceptions
ASFLAGS     :=  -g $(ARCH)
LDFLAGS     :=  -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map),--gc-sections

LIBS        :=  -lctru -lfreetype -lz -lm
LIBDIRS     :=  $(CTRULIB) $(CURDIR)/lib

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT    :=  $(CURDIR)/$(TARGET)
export TOPDIR    :=  $(CURDIR)
export DEPSDIR   :=  $(CURDIR)/$(BUILD)

export VPATH     :=  $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                      $(foreach dir,$(DATA),$(CURDIR)/$(dir))

CFILES      :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES    :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES      :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES    :=  $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
	export LD := $(CC)
else
	export LD := $(CXX)
endif

export OFILES     := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export INCLUDE    := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                     $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                     -I$(CURDIR)/$(BUILD)
export LIBPATHS   := $(foreach dir,$(LIBDIRS),-L$(dir)/lib) -L$(CURDIR)/lib

.PHONY: all clean $(BUILD)

all: $(TARGET).3dsx $(TARGET).smdh

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(TARGET).3dsx $(TARGET).smdh: $(BUILD)

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(TARGET).smdh $(TARGET).elf $(TARGET).lst

else

DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).elf: $(OFILES)

%.bin.o %_bin.h : %.bin
	@echo $(notdir $<)
	$(bin2o)

-include $(DEPENDS)

endif
