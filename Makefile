# Build on the Beelink (FreeBSD 15.1 amd64) against a private static FFmpeg.
# This tree is edited on Linux. The Beelink compiles it. Do not compile it here.
#
# The link line assumes the private prefix below. Pin the compiler to a
# generic amd64 target. Do not pass -march=native.
#
# Private FFmpeg 7.1.5, installed under FFMPEG_PREFIX. FreeBSD base
# autodetects zlib, bzip2, lzma, and iconv; those must stay off or this
# static link fails. Pass --cc=cc. configure defaults to gcc, and the
# base compiler is clang. Use gmake for FFmpeg. Use make for this file.
#   ./configure --cc=cc --prefix=$(FFMPEG_PREFIX) \
#     --enable-static --disable-shared --disable-everything \
#     --disable-autodetect \
#     --disable-zlib --disable-bzlib --disable-lzma --disable-iconv \
#     --disable-doc --disable-programs \
#     --enable-network --enable-protocol=file --enable-protocol=tcp \
#     --enable-protocol=udp --enable-protocol=rtp \
#     --enable-demuxer=rtsp --enable-demuxer=rtp --enable-muxer=mpegts \
#     --enable-parser=h264 --enable-decoder=h264 --enable-bsf=h264_mp4toannexb \
#     --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample \
#     --disable-gpl --disable-nonfree --disable-x86asm --cpu=generic
# The H.264 decoder is for probing width and height. packwatch does not
# decode or scale. Record the FFmpeg version next to the installed binary.

FFMPEG_PREFIX ?= /usr/local/packwatch-ffmpeg

CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Wpedantic
CPPFLAGS += -I$(FFMPEG_PREFIX)/include
LIBS     = -L$(FFMPEG_PREFIX)/lib -Wl,--start-group -lavformat -lavcodec -lavutil -Wl,--end-group -lpthread -lm

.PHONY: all clean

all: packwatch

packwatch: src/packwatch.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -static -o $@ src/packwatch.c $(LIBS)

clean:
	rm -f packwatch
