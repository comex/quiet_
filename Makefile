HOST_CXX := c++
HOST_CXXFLAGS :=
HOST_LDFLAGS :=
# for uitest only
HOST_SDL :=

CXX :=
CXXFLAGS :=
BINUTILS := powerpc-elf-
LD := $(BINUTILS)ld
LDFLAGS :=
OBJCOPY := $(BINUTILS)objcopy
NM := $(BINUTILS)nm

#if PRIVATE
CXX := /opt/mllvm/bin/clang
HOST_SDL := /usr/local/opt/sdl2
MAINTAINER_MODE := 1
BONUS_FEATURES := 1
#endif

all: obj/mod.elf

ifneq "$(findstring clang,$(shell $(CXX) --version))" ""
CXX_IS_CLANG := 1
endif

ifneq "$(findstring clang,$(shell $(HOST_CXX) --version))" ""
HOST_CXX_IS_CLANG := 1
endif

# only needed if LTO_MODE = llvm-hacky
LLVM_LINK := $(dir $(CXX))/llvm-link

# python3 works too
PYTHON := python

# options: none | normal | llvm-hacky
LTO_MODE := none

ifeq "$(CXX_IS_CLANG)" "1"
  USE_CC_TO_LINK := 0
else
  USE_CC_TO_LINK := 1
endif

ifeq "$(USE_CC_TO_LINK)" "1"
  LD_OR_CC := $(CXX)
  Wl := -Wl,
else
  LD_OR_CC := $(LD)
  Wl :=
endif

settings := HOST_CXX=$(HOST_CXX) HOST_CXXFLAGS=$(HOST_CXXFLAGS) HOST_LDFLAGS=$(HOST_LDFLAGS) HOST_SDL=$(HOST_SDL) CXX=$(CXX) CXXFLAGS=$(CXXFLAGS) BINUTILS=$(BINUTILS) LD=$(LD) LDFLAGS=$(LDFLAGS) OBJCOPY=$(OBJCOPY) NM=$(NM) LTO_MODE=$(LTO_MODE) LLVM_LINK=$(LLVM_LINK)

CXXFLAGS_MOD :=
define check_bool # (name,default)
  ifeq "$$($(1))" ""
    $(1) := $(2)
  endif
  CXXFLAGS_MOD += -D$(1)=$$($(1))
  settings += $(1)=$$($(1))
endef
$(eval $(call check_bool,ENABLE_USBDUCKS,1))
$(eval $(call check_bool,ENABLE_GDBSTUB,1))
$(eval $(call check_bool,ENABLE_QMOD,1))

$(eval $(call check_bool,ENABLE_SWIFTCALL,1))

-include obj/settings.mk
ifneq "$(old_settings)" "$(settings)"
  $(warning need full rebuild since parameters changed; running rm -rf obj)
  .PHONY: always
  obj/settings.mk: always
endif
$(decl)
obj/settings.mk: Makefile
	@ \
		rm -rf obj; \
		mkdir -p obj; \
		$(CXX) --version 2>&1 | grep -q clang; CXX_IS_CLANG=$$((1-$$?)); \
		$(HOST_CXX) --version 2>&1 | grep -q clang; HOST_CXX_IS_CLANG=$$((1-$$?)); \
		(echo 'old_settings := $(subst ','"'"',$(settings))'; \
		 echo "CXX_IS_CLANG := $$CXX_IS_CLANG"; \
		 echo "HOST_CXX_IS_CLANG := $$HOST_CXX_IS_CLANG"; \
		) > obj/settings.mk

MAINTAINER_MODE ?= 0
ENABLE_MODULES := 1
CXXFLAGS_CLANG := -Werror=thread-safety
ifeq "$(ENABLE_MODULES)" "1"
  CXXFLAGS_CLANG += -fmodules -fcxx-modules -fmodule-map-file=module.modulemap -fmodules-cache-path=obj/modules_cache -DENABLE_MODULES=1
endif
CXXFLAGS_MOD_AND_DUMMY := -std=gnu++17 -Wuninitialized -Wunused -Wconversion -Wimplicit-fallthrough -Wno-multichar -Wno-gnu-string-literal-operator-template -Wmissing-declarations -Icommon -Iqmod -Igdbstub -Iloader -Iusbducks -Iobj -MD -g3 -fno-exceptions
#if PRIVATE
ifeq "$(BONUS_FEATURES)" "1"
    CXXFLAGS_MOD_AND_DUMMY += -Iloud -DBONUS_FEATURES=1
endif
#endif
ifeq "$(MAINTAINER_MODE)" "1"
    CXXFLAGS_MOD_AND_DUMMY += -Werror
endif
SANITIZE_FLAGS_DUMMY := -fsanitize=address,undefined -fno-sanitize=function
CXXFLAGS_DUMMY := $(CXXFLAGS_MOD_AND_DUMMY) -O0 -DDUMMY=1 -DPROJECT_NAME='"dummy"' -DFIXED_BUILD_ID='"dummy"' $(SANITIZE_FLAGS_DUMMY)
LDFLAGS_DUMMY := $(SANITIZE_FLAGS_DUMMY)
CXXFLAGS_MOD += $(CXXFLAGS_MOD_AND_DUMMY) -mcpu=750 -ffreestanding -nostdinc -DDUMMY=0 -ffunction-sections -fdata-sections -fvisibility=hidden -fno-rtti
LDFLAGS_FAKESO := -shared -m elf32ppc -nostdlib
LDFLAGS_MOD += $(Wl)-m $(Wl)elf32ppc $(Wl)--gc-sections $(LDFLAGS) -nostdlib -z max-page-size=4096 -z common-page-size=8 -z notext -g $(Wl)--unresolved-symbols=report-all -Bsymbolic -Lobj $(foreach rpl,$(RPLS),-l$(rpl:obj/lib%.so=%)) $(Wl)--as-needed -z nocopyreloc $(Wl)-n $(Wl)-call_shared -shared -T loader/mod.ld

ifeq "$(LTO_MODE)" "normal"
  CXXFLAGS_MOD += -flto
  LDFLAGS_MOD += -flto
else
  ifeq "$(LTO_MODE)" "llvm-hacky"
    CXXFLAGS_MOD += -emit-llvm
  else
    ifneq "$(LTO_MODE)" "none"
        $(error "unexpected LTO_MODE '$(LTO_MODE)'"))
    endif
  endif
endif

ifeq "$(CXX_IS_CLANG)" "1"
CXXFLAGS_MOD += -target powerpc-none-elf -fdouble-square-bracket-attributes -Wno-gnu-alignof-expression -Wno-undefined-var-template -Wno-undefined-internal -Oz $(CXXFLAGS_CLANG)
else
CXXFLAGS_MOD += -Os -Wno-maybe-uninitialized -Wno-conversion -Wno-unused-function -mno-eabi -mno-sdata -fpic
LDFLAGS_MOD_POST := -lgcc
endif

CXXFLAGS_MOD += $(CXXFLAGS)

ifeq "$(HOST_CXX_IS_CLANG)" "1"
CXXFLAGS_DUMMY += $(CXXFLAGS_CLANG)
endif


RPLS := obj/libcoreinit.so obj/libsnd_core.so obj/libvpad.so obj/libnsysnet.so obj/libgx2.so obj/libsysapp.so obj/libtcl.so obj/libnsysuhs.so obj/libnn_save.so obj/libnlibcurl.so obj/libnn_boss.so

EXTRA_DEPS += obj/sinvals.h obj/font.h Makefile
ifeq "$(ENABLE_MODULES)" "1"
  EXTRA_DEPS += module.modulemap
endif

define host_prog # (prog,define)
OBJECTS_$(1) := $(patsubst %.h,obj/_$(1)/%.o,$(patsubst %.cpp,obj/_$(1)/%.o,$(SOURCES_$(1))))
DEPFILES_$(1) := $$(patsubst %.o,%.d,$$(OBJECTS_$(1)))
-include $$(DEPFILES_$(1))

obj/_$(1)/%.o: %.cpp $(EXTRA_DEPS) $(DEPS_$(1))
	@mkdir -p $$(dir $$@)
	$(HOST_CXX) $(CXXFLAGS_DUMMY) $(CXXFLAGS_$(1)) -D$(2) $(HOST_CXXFLAGS) -c -o $$@ $$<
obj/$(1): $$(OBJECTS_$(1)) $(EXTRA_DEPS)
	@mkdir -p $$(dir $$@)
	$(HOST_CXX) $(LDFLAGS_DUMMY) $(LDFLAGS_$(1)) -o $$@ $$(OBJECTS_$(1)) $(HOST_LDFLAGS)
endef

SOURCES := loader/loader.cpp common/logging.cpp common/ssprintf.cpp common/ssscanf.cpp common/misc.cpp common/kern_garbage.cpp
ifneq "$(ENABLE_USBDUCKS)" "0"
  SOURCES += usbducks/usbducks.cpp usbducks/usbducks_backend_wiiu.cpp
endif

ifneq "$(ENABLE_GDBSTUB)" "0"
  SOURCES += gdbstub/gdbstub.cpp gdbstub/trace.cpp common/lz4.cpp
ifeq "$(CXX_IS_CLANG)" "1"
  SOURCES += common/compiler-rt.cpp
endif
endif

ifneq "$(ENABLE_QMOD)" "0"
  SOURCES += qmod/qmod.cpp qmod/mmhooks.cpp qmod/graphics.cpp qmod/uifront.cpp qmod/uiback.cpp common/containers.cpp
endif

#if PRIVATE
ifeq "$(BONUS_FEATURES)" "1"
SOURCES += loud/loud.cpp
obj/loud/loud.o: obj/patchcode.h
obj/patchcode.elf: loud/patchcode.c Makefile
	/opt/arm-none-eabi/bin/arm-none-eabi-gcc -Os -mcpu=arm926ej-s -nostdinc -nostdlib -Wall -Wextra -Werror -mthumb -mbig-endian -ffreestanding -fno-ident -o obj/patchcode.elf loud/patchcode.c -Ttext=0x051107A0 -Wl,-esyslog_stage2
obj/patchcode.bin: obj/patchcode.elf Makefile
	/opt/arm-none-eabi/bin/arm-none-eabi-objcopy -O binary $< $@
obj/patchcode.h: obj/patchcode.bin Makefile
	xxd -i < $< > $@
endif
#endif

obj/dummy.o: loader/dummy.cpp $(EXTRA_DEPS)
	@mkdir -p $(dir $@)
	$(CXX) $(filter-out -flto,$(filter-out -emit-llvm,$(CXXFLAGS_MOD))) -c -o $@ $<

obj/%.o: %.cpp $(EXTRA_DEPS)
	@mkdir -p $(dir $@)
	$(CXX) $(STD_CXX) $(CXXFLAGS_MOD) -MD -c -o $@ $<
obj/%.s: %.cpp $(EXTRA_DEPS)
	@mkdir -p $(dir $@)
	$(CXX) $(STD_CXX) $(CXXFLAGS_MOD) -S -o $@ $<

OBJECTS := $(patsubst %.cpp,obj/%.o,$(SOURCES))
DEPFILES := $(patsubst %.o,%.d,$(OBJECTS))
-include $(DEPFILES)
ifeq "$(LTO_MODE)" "llvm-hacky"
obj/mod.bc: $(OBJECTS) $(EXTRA_DEPS)
	@mkdir -p $(dir $@)
	$(LLVM_LINK) -o $@ $(OBJECTS)
obj/mod.o: obj/mod.bc $(EXTRA_DEPS)
	$(CXX) $(filter-out -emit-llvm,$(CXXFLAGS_MOD)) -c -o $@ $< -Wno-unused-command-line-argument
OBJECTS_FINAL := obj/mod.o
else
OBJECTS_FINAL := $(OBJECTS)
endif
obj/mod.elf.dbg: $(OBJECTS_FINAL) $(EXTRA_DEPS) $(RPLS) loader/mod.ld
	@mkdir -p $(dir $@)
	$(LD_OR_CC) $(LDFLAGS_MOD) -o $@ $(OBJECTS_FINAL) $(LDFLAGS_MOD_POST)
obj/mod.elf.strip: obj/mod.elf.dbg Makefile
	$(OBJCOPY) -S $< $@
obj/mod.elf: obj/mod.elf.strip obj/patch-elf $(EXTRA_DEPS)
	obj/patch-elf $< $@

obj/lib%.so: common/%.names common/dummy.map obj/x $(EXTRA_DEPS) obj/dummy.o
	(echo 'SECTIONS { .text : { _DYNAMIC = .; ' && sed 's/^\([^#].*\)/\1 = dummy;/' $< && echo '} }') > obj/$*.ld
	$(LD) $(LDFLAGS_FAKESO) -o $@ -T obj/$*.ld -soname $*.rpl --version-script=common/dummy.map obj/dummy.o

SOURCES_loadertest := loader/loader.cpp common/logging.cpp common/misc.cpp
$(eval $(call host_prog,loadertest,LOADERTEST))

DEPS_uitest := obj/font.h
SOURCES_uitest := qmod/uifront.cpp qmod/uiback.cpp qmod/graphics.cpp common/containers.cpp common/logging.cpp
CXXFLAGS_uitest := -isystem $(HOST_SDL)/include/SDL2
LDFLAGS_uitest := -L$(HOST_SDL)/lib -lSDL2
$(eval $(call host_prog,uitest,UITEST))

DEPS_settingstest := obj/font.h
SOURCES_settingstest := qmod/settings.h
$(eval $(call host_prog,settingstest,SETTINGSTEST))

SOURCES_ssscanftest := common/ssscanf.h
$(eval $(call host_prog,ssscanftest,SSSCANFTEST))

SOURCES_ssprintftest := common/ssprintf.cpp common/logging.cpp common/misc.cpp
$(eval $(call host_prog,ssprintftest,SSPRINTFTEST))

SOURCES_ssprintf_compile_bench := common/ssprintf.cpp
$(eval $(call host_prog,ssprintf_compile_bench, SSPRINTF_COMPILE_BENCH))

SOURCES_identifytest := qmod/identify.cpp
$(eval $(call host_prog,identifytest,IDENTIFYTEST))

SOURCES_logtest := common/logging.cpp
$(eval $(call host_prog,logtest,LOGTEST))

SOURCES_containertest := common/containers.cpp common/logging.cpp
$(eval $(call host_prog,containertest,CONTAINERTEST))

SOURCES_tracetest := gdbstub/trace.cpp common/containers.cpp common/logging.cpp common/ssprintf.cpp
$(eval $(call host_prog,tracetest,TRACETEST))

SOURCES_gdbstubtest := gdbstub/gdbstub.cpp gdbstub/trace.cpp common/lz4.cpp common/containers.cpp common/logging.cpp common/ssprintf.cpp common/ssscanf.cpp common/misc.cpp
$(eval $(call host_prog,gdbstubtest,GDBSTUBTEST))

SOURCES_usbducksd := usbducks/usbducks_backend_libusb.cpp usbducks/usbducks.cpp usbducks/usbducksd.cpp common/logging.cpp common/misc.cpp common/ssprintf.cpp
CXXFLAGS_usbducksd := -O
LDFLAGS_usbducksd := -lusb-1.0
$(eval $(call host_prog,usbducksd,USBDUCKSD))

SOURCES_patch-elf := loader/patch-elf.cpp common/logging.cpp common/misc.cpp common/ssprintf.cpp
CXXFLAGS_patch-elf := -g3
DEPS_patch-elf := loader/loader.cpp
$(eval $(call host_prog,patch-elf,PATCH_ELF))

obj/%/x:
	mkdir -p $@
obj/x:
	mkdir -p $@

obj/font.h: obj/x common/gen_font_h.py font_pngs/*.png
	$(PYTHON) common/gen_font_h.py font_pngs obj/font.h
obj/sinvals.h: obj/x common/gen_sinvals_h.py
	$(PYTHON) common/gen_sinvals_h.py obj/sinvals.h

module.modulemap: make-modulemap.sh
	bash make-modulemap.sh

ifneq "$(DECAF_EMU_LATTE_ASSEMBLER)" ""
qmod/shader%.gfd: qmod/shader%.vsh qmod/shader%.psh $(DECAF_EMU_LATTE_ASSEMBLER) Makefile
	$(DECAF_EMU_LATTE_ASSEMBLER) compile $@ --vsh=qmod/shader$*.vsh --psh=qmod/shader$*.psh
qmod/shader%.h: qmod/shader%.gfd common/gfd2h.py Makefile
	$(PYTHON) common/gfd2h.py qmod/shader$*.gfd shader$* qmod/shader$*.h
endif

clean:
	rm -rf obj module.modulemap
.PRECIOUS: obj/%.o obj/%.S obj/lib%.so obj/%.elf.dbg obj/%.elf qmod/shader%.gfd qmod/shader%.h obj/%.elf.strip
