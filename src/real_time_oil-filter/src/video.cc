
/*
 * Copy a YUV4MPEG stream to a v4l2 output device.
 * The stream is read from standard input.
 * The device can be specified as argument; it defaults to /dev/video0.
 *
 * Example using mplayer as a producer for the v4l2loopback driver:
 *
 * $ mkfifo /tmp/pipe
 * $ ./yuv4mpeg_to_v4l2 < /tmp/pipe &
 * $ mplayer movie.mp4 -vo yuv4mpeg:file=/tmp/pipe
 *
 * Copyright (C) 2011  Eric C. Cooper <ecc@cmu.edu>
 * Released under the GNU General Public License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include "filter.cuh"
#include "io_png.hh"

char *prog;

int dev_fd;

int frame_width;
int frame_height;
size_t frame_bytes;

void yuv420_to_rgb(unsigned char* yuv_buffer, unsigned char* rgb_buffer, int height, int width) {
	const int size = width * height;

	const size_t CbBase = size;

    const size_t CrBase = size + width*height/4;

    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j++) {
            int index = i * width * 3 + j * 3;
            int Y  = yuv_buffer[i * width + j] - 16;
            int Cr = yuv_buffer[CrBase + (i * width / 4) + (j / 2)]  - 128;
            int Cb = yuv_buffer[CbBase + (i * width / 4) + (j / 2)]  - 128;

            double R = 1.164*Y+1.596*Cr;
            double G = 1.164*Y-0.392*Cb-0.813*Cr;
            double B = 1.164*Y+2.017*Cb;

            rgb_buffer[index] = (R > 255) ? 255 : ((R < 0) ? 0 : R);
            rgb_buffer[index + 1] = (G > 255) ? 255 : ((G < 0) ? 0 : G);
            rgb_buffer[index + 2] = (B > 255) ? 255 : ((B < 0) ? 0 : B);
        }
    }
}

void rgb_to_yuv420(unsigned char* yuv_buffer, unsigned char* rgb_buffer, int height, int width) {
	const int size = width * height;

	const size_t CbBase = size;

    const size_t CrBase = size + width*height/4;

    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j++) {
            int index = i * width * 3 + j * 3;
			unsigned char r = rgb_buffer[index];
			unsigned char g = rgb_buffer[index + 1];
			unsigned char b = rgb_buffer[index + 2];

			int Y = 16 + 0.257 * r + 0.504 * g + 0.098 * b;
			int Cb = 128 - 0.148 * r - 0.291 * g + 0.439 * b;
			int Cr = 128 + 0.439 * r - 0.368 * g - 0.071 * b;

			yuv_buffer[i * width + j] = (Y > 235) ? 235 : ((Y < 16) ? 16 : Y);
			yuv_buffer[CrBase + (i * width / 4) + (j / 2)] = (Cr > 240) ? 240 : ((Cr < 16) ? 16 : Cr);
			yuv_buffer[CbBase + (i * width / 4) + (j / 2)] = (Cb > 240) ? 240 : ((Cb < 16) ? 16 : Cb);
        }
    }
}

void
usage(void)
{
	fprintf(stderr, (char*)"Usage: %s [/dev/videoN]\n", prog);
	exit(1);
}

void
sysfail(const char *msg)
{
	perror(msg);
	exit(1);
}

void
fail(char *msg)
{
	fprintf(stderr, "%s: %s\n", prog, msg);
	exit(1);
}

void
bad_header(char *kind)
{
	char msg[64];

	sprintf(msg, (char*)"malformed %s header", kind);
	fail(msg);
}

void
do_tag(char tag, char *value)
{
	switch (tag) {
	case 'W':
		frame_width = strtoul(value, NULL, 10);
		break;
	case 'H':
		frame_height = strtoul(value, NULL, 10);
		break;
	}
}

int
read_header(char *magic)
{
	char *p, *q, *p0;
	size_t n;
	int first, done;

	p0 = NULL;
	if (getline(&p0, &n, stdin) == -1) {
    free(p0);
    return 0;
  }

	q = p = p0;
	first = 1;
	done = 0;
	while (!done) {
		while (*q != ' ' && *q != '\n')
			if (*q++ == '\0') bad_header(magic);
		done = (*q == '\n');
		*q = '\0';
		if (first)
			if (strcmp(p, magic) == 0) first = 0;
			else bad_header(magic);
		else
			do_tag(*p, p + 1);
		p = ++q;
	}

  free(p0);
	return 1;
}

void
process_header(void)
{
	if (!read_header((char*)"YUV4MPEG2")) fail((char*)"missing YUV4MPEG2 header");
	frame_bytes = 3 * frame_width * frame_height / 2;
	if (frame_bytes == 0) fail((char*)"frame width or height is missing");
}

void copy_frames(void) {
	auto frame = (unsigned char*)malloc(frame_bytes);
	auto rgb_frame = (unsigned char*)malloc(frame_height * frame_width * 3 * sizeof(unsigned char));

    while (read_header((char*)"FRAME")) {
		if (fread(frame, 1, frame_bytes, stdin) != frame_bytes) {
    		free(frame);
			fail((char*)"malformed frame");
    	}
		yuv420_to_rgb(frame, rgb_frame, frame_height, frame_width);
        auto tmp_frame = oil_filter(rgb_frame, frame_height, frame_width);
		free(rgb_frame);
        rgb_frame = tmp_frame;
        rgb_to_yuv420(frame, rgb_frame, frame_height, frame_width);

        if (write(dev_fd, frame, frame_bytes) != (ssize_t)frame_bytes) {
    		free(frame);
			sysfail("write");
    	}
	}
	free(rgb_frame);
	free(frame);
}

#define vidioc(op, arg) \
	if (ioctl(dev_fd, VIDIOC_##op, arg) == -1) \
		sysfail(#op); \
	else {}

void
open_video(const char *device)
{
	struct v4l2_format v;

	dev_fd = open(device, O_RDWR);
	if (dev_fd == -1) sysfail(device);
	v.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	vidioc(G_FMT, &v);
	v.fmt.pix.width = frame_width;
	v.fmt.pix.height = frame_height;
	v.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
	v.fmt.pix.sizeimage = frame_bytes;
	vidioc(S_FMT, &v);
}

int
launch_webcam(const char *dev)
{
	process_header();
	open_video(dev);
	copy_frames();
	return 0;
}