## Haiku Generic Build Makefile ##
NAME = ROneSeg
TYPE = APP
APP_MIME_SIG = application/x-vnd.ROneSeg

SRCS = \
	src/main.cpp \
	src/MainWindow.cpp \
	src/VideoView.cpp \
	src/Player.cpp \
	src/TunerAdapterIO.cpp \
	src/SiParser.cpp \
	src/AribText.cpp \
	src/ChannelTable.cpp \
	src/FileTuner.cpp \
	src/UsbTuner.cpp

RDEFS = src/app.rdef
RSRCS =

# be:     BApplication/BWindow/interface kit
# media:  BMediaFile / BMediaTrack / BSoundPlayer
# device: the USB Kit (BUSBRoster/BUSBDevice/BUSBEndpoint). This is the whole
#         reason no kernel driver is needed - userland can do the control and
#         bulk transfers a demodulator module needs.
# iconv:  AribText converts service names through EUC-JP rather than carrying
#         a JIS X 0208 table of its own. Haiku's iconv.h macro-renames the
#         functions to libiconv_*, so this is a real link dependency and not
#         something libroot already provides.
# stdc++/supc++: makefile-engine does not link these for TYPE=APP.
#
# The 32-bit x86 build this machine needs is the gcc2 hybrid, where plain
# -lstdc++ resolves to the *modern* x86 libstdc++ (wrong ABI for gcc2
# objects) and -lsupc++ does not exist at all (GCC 2.95 predates the
# libstdc++/libsupc++ split). Same treatment R World Radio's Makefile gives
# it. In practice you want the modern compiler anyway - build with
# `setarch x86 make`.
GCC_VERSION := $(shell g++ -dumpversion)
ifeq ($(filter 2.%,$(GCC_VERSION)),)
LIBS = be media device iconv stdc++ supc++ \
	/boot/system/develop/lib/libshared.a
else
LIBS = be media device iconv \
	/boot/system/develop/lib/libstdc++.r4.so \
	/boot/system/develop/lib/libshared.a
endif

LIBPATHS =
# private/media/experimental has BAdapterIO/BMediaIO, the experimental Media
# Kit class Haiku's own http_streamer add-on is built on and the one
# TunerAdapterIO derives from. private/shared has RWLocker.h, which
# AdapterIO.h itself includes.
SYSTEM_INCLUDE_PATHS = \
	/boot/system/develop/headers/private/media/experimental \
	/boot/system/develop/headers/private/shared
LOCAL_INCLUDE_PATHS =
OPTIMIZE := FULL
LOCALES =
DEFINES =
WARNINGS = ALL
SYMBOLS =
DEBUGGER =
COMPILER_FLAGS = -D_GLIBCXX_USE_CXX11_ABI=1
LINKER_FLAGS =
APP_VERSION =
DRIVER_PATH =

## Include the Makefile-Engine that ships with the Haiku SDK.
include /boot/system/develop/etc/makefile-engine
