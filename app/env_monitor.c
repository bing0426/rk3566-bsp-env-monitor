#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "env_monitor_uapi.h"

#define DEFAULT_SENSOR_DEV "/dev/bme280_env"
#define DEFAULT_OLED_DEV "/dev/ssd1315_oled"
#define DEFAULT_KEY_DEV "/dev/env_key"
#define DEFAULT_INTERVAL_MS 600000U

/*bmp280 data struct */
struct env_sample {
	int temp_mC;
	unsigned int pressure_Pa;
	unsigned int humidity_mpermille;
};

struct glyph5x7 {
	char ch;
	uint8_t col[5];
};

static volatile sig_atomic_t g_running = 1;

static const struct glyph5x7 font5x7[] = {
	{ ' ', { 0x00, 0x00, 0x00, 0x00, 0x00 } },
	{ '.', { 0x00, 0x60, 0x60, 0x00, 0x00 } },
	{ ':', { 0x00, 0x36, 0x36, 0x00, 0x00 } },
	{ '-', { 0x08, 0x08, 0x08, 0x08, 0x08 } },
	{ '/', { 0x20, 0x10, 0x08, 0x04, 0x02 } },
	{ '%', { 0x62, 0x64, 0x08, 0x13, 0x23 } },
	{ '0', { 0x3E, 0x51, 0x49, 0x45, 0x3E } },
	{ '1', { 0x00, 0x42, 0x7F, 0x40, 0x00 } },
	{ '2', { 0x42, 0x61, 0x51, 0x49, 0x46 } },
	{ '3', { 0x21, 0x41, 0x45, 0x4B, 0x31 } },
	{ '4', { 0x18, 0x14, 0x12, 0x7F, 0x10 } },
	{ '5', { 0x27, 0x45, 0x45, 0x45, 0x39 } },
	{ '6', { 0x3C, 0x4A, 0x49, 0x49, 0x30 } },
	{ '7', { 0x01, 0x71, 0x09, 0x05, 0x03 } },
	{ '8', { 0x36, 0x49, 0x49, 0x49, 0x36 } },
	{ '9', { 0x06, 0x49, 0x49, 0x29, 0x1E } },
	{ 'A', { 0x7E, 0x11, 0x11, 0x11, 0x7E } },
	{ 'B', { 0x7F, 0x49, 0x49, 0x49, 0x36 } },
	{ 'C', { 0x3E, 0x41, 0x41, 0x41, 0x22 } },
	{ 'D', { 0x7F, 0x41, 0x41, 0x22, 0x1C } },
	{ 'E', { 0x7F, 0x49, 0x49, 0x49, 0x41 } },
	{ 'F', { 0x7F, 0x09, 0x09, 0x09, 0x01 } },
	{ 'G', { 0x3E, 0x41, 0x49, 0x49, 0x7A } },
	{ 'H', { 0x7F, 0x08, 0x08, 0x08, 0x7F } },
	{ 'I', { 0x00, 0x41, 0x7F, 0x41, 0x00 } },
	{ 'J', { 0x20, 0x40, 0x41, 0x3F, 0x01 } },
	{ 'K', { 0x7F, 0x08, 0x14, 0x22, 0x41 } },
	{ 'L', { 0x7F, 0x40, 0x40, 0x40, 0x40 } },
	{ 'M', { 0x7F, 0x02, 0x0C, 0x02, 0x7F } },
	{ 'N', { 0x7F, 0x04, 0x08, 0x10, 0x7F } },
	{ 'O', { 0x3E, 0x41, 0x41, 0x41, 0x3E } },
	{ 'P', { 0x7F, 0x09, 0x09, 0x09, 0x06 } },
	{ 'Q', { 0x3E, 0x41, 0x51, 0x21, 0x5E } },
	{ 'R', { 0x7F, 0x09, 0x19, 0x29, 0x46 } },
	{ 'S', { 0x46, 0x49, 0x49, 0x49, 0x31 } },
	{ 'T', { 0x01, 0x01, 0x7F, 0x01, 0x01 } },
	{ 'U', { 0x3F, 0x40, 0x40, 0x40, 0x3F } },
	{ 'V', { 0x1F, 0x20, 0x40, 0x20, 0x1F } },
	{ 'W', { 0x3F, 0x40, 0x38, 0x40, 0x3F } },
	{ 'X', { 0x63, 0x14, 0x08, 0x14, 0x63 } },
	{ 'Y', { 0x07, 0x08, 0x70, 0x08, 0x07 } },
	{ 'Z', { 0x61, 0x51, 0x49, 0x45, 0x43 } },
	{ '?', { 0x02, 0x01, 0x51, 0x09, 0x06 } },
};

static void on_signal(int signo)
{
	(void)signo;
	g_running = 0;
}

static long long monotonic_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static const uint8_t *lookup_glyph(char ch)
{
	size_t i;

	if (ch >= 'a' && ch <= 'z')
		ch = (char)(ch - 'a' + 'A');

	for (i = 0; i < sizeof(font5x7) / sizeof(font5x7[0]); i++) {
		if (font5x7[i].ch == ch)
			return font5x7[i].col;
	}

	return lookup_glyph('?');
}

static void fb_set_pixel(uint8_t fb[SSD1315_FB_SIZE], int x, int y)
{
	if (x < 0 || x >= SSD1315_WIDTH || y < 0 || y >= SSD1315_HEIGHT)
		return;

	fb[(y / 8) * SSD1315_WIDTH + x] |= (uint8_t)(1U << (y & 7));
}

static void fb_draw_char(uint8_t fb[SSD1315_FB_SIZE], int x, int y, char ch)
{
	const uint8_t *glyph = lookup_glyph(ch);
	int col;
	int row;

	for (col = 0; col < 5; col++) {
		for (row = 0; row < 7; row++) {
			if (glyph[col] & (1U << row))
				fb_set_pixel(fb, x + col, y + row);
		}
	}
}

static void fb_draw_text(uint8_t fb[SSD1315_FB_SIZE], int x, int y,
			 const char *text)
{
	while (*text && x < SSD1315_WIDTH - 5) {
		fb_draw_char(fb, x, y, *text++);
		x += 6;
	}
}

static void format_temp(char *buf, size_t len, int temp_mC)
{
	int neg = temp_mC < 0;
	unsigned int abs_mC = neg ? (unsigned int)(-temp_mC) :
				    (unsigned int)temp_mC;

	snprintf(buf, len, "%s%u.%02u C", neg ? "-" : "",
		 abs_mC / 1000, (abs_mC % 1000) / 10);
}

static void format_milli_percent(char *buf, size_t len,
				 unsigned int value_milli)
{
	snprintf(buf, len, "%u.%02u %%", value_milli / 1000,
		 (value_milli % 1000) / 10);
}

static void format_pressure_hpa(char *buf, size_t len, unsigned int pressure_Pa)
{
	snprintf(buf, len, "%u.%02u HPA", pressure_Pa / 100,
		 pressure_Pa % 100);
}

static int read_sample(const char *sensor_dev, struct env_sample *sample)
{
	char line[128];
	ssize_t n;
	int fd;
	int ret;

	fd = open(sensor_dev, O_RDONLY);
	if (fd < 0) {
		perror(sensor_dev);
		return -1;
	}

	n = read(fd, line, sizeof(line) - 1);
	if (n < 0) {
		perror("read sensor");
		close(fd);
		return -1;
	}

	close(fd);
	line[n] = '\0';

	ret = sscanf(line, "temp_mC=%d pressure_Pa=%u humidity_mpermille=%u",
		     &sample->temp_mC, &sample->pressure_Pa,
		     &sample->humidity_mpermille);
	if (ret != 3) {
		fprintf(stderr, "unexpected sensor line: %s\n", line);
		return -1;
	}

	return 0;
}

static int render_and_flush(int oled_fd, const struct env_sample *sample,
			    const char *source)
{
	uint8_t fb[SSD1315_FB_SIZE];
	char temp[24];
	char hum[24];
	char press[24];
	char line[32];
	ssize_t n;

	memset(fb, 0, sizeof(fb));
	format_temp(temp, sizeof(temp), sample->temp_mC);
	format_milli_percent(hum, sizeof(hum), sample->humidity_mpermille);
	format_pressure_hpa(press, sizeof(press), sample->pressure_Pa);

	fb_draw_text(fb, 0, 0, "BME280 ENV");
	snprintf(line, sizeof(line), "T:%s", temp);
	fb_draw_text(fb, 0, 12, line);
	snprintf(line, sizeof(line), "H:%s", hum);
	fb_draw_text(fb, 0, 24, line);
	snprintf(line, sizeof(line), "P:%s", press);
	fb_draw_text(fb, 0, 36, line);
	snprintf(line, sizeof(line), "SRC:%s", source);
	fb_draw_text(fb, 0, 52, line);

	n = write(oled_fd, fb, sizeof(fb));
	if (n < 0) {
		perror("write oled");
		return -1;
	}
	if (n != (ssize_t)sizeof(fb)) {
		fprintf(stderr, "short oled write: %zd/%zu\n", n, sizeof(fb));
		return -1;
	}

	return 0;
}

static int update_display(const char *sensor_dev, int oled_fd,
			  const char *source)
{
	struct env_sample sample;

	if (read_sample(sensor_dev, &sample) < 0)
		return -1;

	if (render_and_flush(oled_fd, &sample, source) < 0)
		return -1;

	printf("updated by %-5s T=%d mC H=%u milli%% P=%u Pa\n",
	       source, sample.temp_mC, sample.humidity_mpermille,
	       sample.pressure_Pa);
	fflush(stdout);

	return 0;
}

static void drain_key_events(int key_fd)
{
	char buf[32];

	while (read(key_fd, buf, sizeof(buf)) > 0);

	if (errno != EAGAIN && errno != EWOULDBLOCK)
		perror("read key");
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [-i interval_ms] [-s sensor_dev] [-o oled_dev] [-k key_dev]\n"
		"Defaults: -i %u -s %s -o %s -k %s\n",
		prog, DEFAULT_INTERVAL_MS, DEFAULT_SENSOR_DEV, DEFAULT_OLED_DEV,
		DEFAULT_KEY_DEV);
}

int main(int argc, char **argv)
{
	const char *sensor_dev = DEFAULT_SENSOR_DEV;
	const char *oled_dev = DEFAULT_OLED_DEV;
	const char *key_dev = DEFAULT_KEY_DEV;
	unsigned int interval_ms = DEFAULT_INTERVAL_MS;
	struct pollfd pfd;
	long long next_due;
	int oled_fd;
	int key_fd;
	int opt;

	while ((opt = getopt(argc, argv, "i:s:o:k:h")) != -1) {
		switch (opt) {
		case 'i':
			interval_ms = (unsigned int)strtoul(optarg, NULL, 0);
			break;
		case 's':
			sensor_dev = optarg;
			break;
		case 'o':
			oled_dev = optarg;
			break;
		case 'k':
			key_dev = optarg;
			break;
		case 'h':
		default:
			usage(argv[0]);
			return opt == 'h' ? 0 : 1;
		}
	}

	if (!interval_ms) {
		fprintf(stderr, "interval_ms must be > 0\n");
		return 1;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	oled_fd = open(oled_dev, O_WRONLY);
	if (oled_fd < 0) {
		perror(oled_dev);
		return 1;
	}

	key_fd = open(key_dev, O_RDONLY | O_NONBLOCK);
	if (key_fd < 0)
		perror(key_dev);

	ioctl(oled_fd, SSD1315_IOC_CLEAR);

	if (update_display(sensor_dev, oled_fd, "BOOT") < 0) {
		close(oled_fd);
		if (key_fd >= 0)
			close(key_fd);
		return 1;
	}

	next_due = monotonic_ms() + interval_ms;

	while (g_running) {
		long long now = monotonic_ms();
		int timeout_ms;
		int ret;

		if (now >= next_due)
			timeout_ms = 0;
		else if (next_due - now > INT32_MAX)
			timeout_ms = INT32_MAX;
		else
			timeout_ms = (int)(next_due - now);

		if (key_fd >= 0) {
			pfd.fd = key_fd;
			pfd.events = POLLIN;
			pfd.revents = 0;
			ret = poll(&pfd, 1, timeout_ms);/*睡眠等待*/
		} else {
			ret = poll(NULL, 0, timeout_ms);
		}

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			break;
		}

		if (ret > 0 && key_fd >= 0 && (pfd.revents & POLLIN)) {
			drain_key_events(key_fd);
			update_display(sensor_dev, oled_fd, "KEY");
			next_due = monotonic_ms() + interval_ms;
			continue;
		}

		if (ret == 0 || monotonic_ms() >= next_due) {
			update_display(sensor_dev, oled_fd, "TIMER");
			next_due = monotonic_ms() + interval_ms;
		}
	}

	ioctl(oled_fd, SSD1315_IOC_DISPLAY_OFF);

	if (key_fd >= 0)
		close(key_fd);
	close(oled_fd);

	return 0;
}
