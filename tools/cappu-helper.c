// SPDX-License-Identifier: GPL-2.0-only
/*
 * Mi Pad 3 spike init helper, built static for the initramfs.
 *
 *   /init (or cappu-helper run as PID 1)
 *       The whole of init, with no shell involved: mount the pseudo filesystems, start
 *       the dead-man timer, enable charging, then serve the USB serial gadget. toybox sh
 *       as PID 1 re-ran its script from the top every 80 ms (2026-11-22).
 *   cappu-helper deadman SECONDS FLAGFILE
 *       Restart (see below) after SECONDS unless FLAGFILE exists by then.
 *   cappu-helper restart
 *       sync, then a clean reboot(2): the kernel offlines the secondary CPUs first
 *       (which powers their MTCMOS domains down). Falls back to sysrq 'b'.
 *   cappu-helper dump ADDR LEN
 *       Hex dump of LEN bytes of physical (register) space at ADDR, read as 32-bit
 *       words through /dev/mem.
 *   cappu-helper serve TTY
 *       Command server for the host (spike-shell.py): read lines from TTY (raw mode, no
 *       echo) and run each with /bin/sh -c, output to TTY. No interactive shell, no job
 *       control. I/O errors are logged and the tty is reopened.
 *   cappu-helper console TTY PROGRAM [ARG...]
 *       Forever: open TTY as the controlling terminal of a new session, make it
 *       fds 0-2, run PROGRAM; reopen when it exits.
 *
 * Every failure is logged with its errno to /dev/kmsg, which reaches ramoops, so a
 * broken console or reboot path shows up in the next log.
 */
#include <dirent.h>
#include <errno.h>
#include <drm/drm.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static void klog(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int fd, n;

	n = snprintf(buf, sizeof(buf), "cappu-helper[%d]: ", getpid());
	va_start(ap, fmt);
	n += vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
	va_end(ap);
	if (n >= (int)sizeof(buf) - 1)
		n = sizeof(buf) - 2;
	buf[n++] = '\n';
	fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	if (fd >= 0) {
		if (write(fd, buf, n) < 0)
			dprintf(2, "kmsg write: %s\n", strerror(errno));
		close(fd);
	}
}

static int restart(void)
{
	int fd;

	sync();
	klog("restart: clean reboot");
	reboot(RB_AUTOBOOT);
	klog("restart: reboot(2) failed: %s; trying sysrq", strerror(errno));
	fd = open("/proc/sys/kernel/sysrq", O_WRONLY);
	if (fd >= 0) {
		if (write(fd, "1", 1) != 1)
			klog("restart: enabling sysrq failed: %s", strerror(errno));
		close(fd);
	}
	fd = open("/proc/sysrq-trigger", O_WRONLY);
	if (fd < 0) {
		klog("restart: open sysrq-trigger: %s", strerror(errno));
		return 1;
	}
	if (write(fd, "b", 1) != 1)
		klog("restart: write sysrq-trigger: %s", strerror(errno));
	close(fd);
	return 1;
}

static int dump(unsigned long base, unsigned long len)
{
	unsigned long ps = sysconf(_SC_PAGESIZE);
	unsigned long start = base & ~(ps - 1), off = base - start;
	unsigned long maplen = (off + len + ps - 1) & ~(ps - 1), i;
	volatile uint32_t *p;
	int fd, j;

	fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0) {
		dprintf(2, "open /dev/mem: %s\n", strerror(errno));
		return 1;
	}
	p = mmap(NULL, maplen, PROT_READ, MAP_SHARED, fd, start);
	if (p == MAP_FAILED) {
		dprintf(2, "mmap %#lx: %s\n", start, strerror(errno));
		return 1;
	}
	for (i = 0; i < len; i += 16) {
		printf("%08lx:", base + i);
		for (j = 0; j < 4 && i + 4 * j < len; j++)
			printf(" %08x", p[(off + i) / 4 + j]);
		printf("\n");
	}
	return 0;
}

static int deadman(int seconds, const char *flag)
{
	klog("deadman: reboot in %d s unless %s exists", seconds, flag);
	sleep(seconds);
	if (access(flag, F_OK) == 0) {
		klog("deadman: %s present, host attached, standing down", flag);
		return 0;
	}
	klog("deadman: no host session, restarting (lands in fastboot)");
	return restart();
}

static void run_line(int tty, char *line)
{
	pid_t pid = fork();
	int status;

	if (pid < 0) {
		klog("serve: fork: %s", strerror(errno));
		return;
	}
	if (pid == 0) {
		int nul = open("/dev/null", O_RDONLY);

		dup2(nul, 0);
		dup2(tty, 1);
		dup2(tty, 2);
		if (nul > 2)
			close(nul);
		if (tty > 2)
			close(tty);
		execl("/bin/sh", "sh", "-c", line, (char *)NULL);
		klog("serve: exec /bin/sh: %s", strerror(errno));
		_exit(127);
	}
	waitpid(pid, &status, 0);
	while (waitpid(-1, NULL, WNOHANG) > 0)
		;
}

/*
 * "@recv SIZE TARGET" on the command channel: take SIZE raw bytes from the host and
 * write them to TARGET, so that a new image never has to go through the bootloader's
 * fastboot download (which sometimes hangs for good in the middle of a transfer).
 * TARGET is "recovery" (the eMMC partition of that name, found by its GPT label) or a
 * file under /tmp; nothing else is accepted. Replies "READY", then after the data one
 * line with the CRC-32 of what was received and of what reads back from the target.
 */
static uint32_t crc32_update(uint32_t crc, const unsigned char *p, size_t n)
{
	static uint32_t table[256];
	size_t i;
	int k;

	if (!table[1])
		for (i = 0; i < 256; i++) {
			uint32_t c = i;

			for (k = 0; k < 8; k++)
				c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
			table[i] = c;
		}
	crc = ~crc;
	for (i = 0; i < n; i++)
		crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
	return ~crc;
}

static int partition_by_name(const char *name, char *dev, size_t len)
{
	DIR *d = opendir("/sys/class/block");
	struct dirent *e;
	int found = 0;

	if (!d)
		return 0;
	while (!found && (e = readdir(d))) {
		char path[300], line[128], want[80];
		FILE *f;

		if (strncmp(e->d_name, "mmcblk0p", 8))
			continue;
		snprintf(path, sizeof(path), "/sys/class/block/%s/uevent", e->d_name);
		snprintf(want, sizeof(want), "PARTNAME=%s\n", name);
		f = fopen(path, "r");
		if (!f)
			continue;
		while (fgets(line, sizeof(line), f))
			if (!strcmp(line, want)) {
				snprintf(dev, len, "/dev/%s", e->d_name);
				found = 1;
			}
		fclose(f);
	}
	closedir(d);
	return found;
}

static void recv_file(int tty, const char *args)
{
	static unsigned char buf[65536];
	char target[64] = "", dev[300] = "";
	unsigned long size = 0, got = 0;
	uint32_t crc = 0, crc_back = 0;
	int out;

	if (sscanf(args, "%lu %63s", &size, target) != 2 || !size || size > (64ul << 20)) {
		dprintf(tty, "recv: usage: @recv SIZE recovery|/tmp/FILE (at most 64 MiB)\n");
		return;
	}
	if (!strcmp(target, "recovery")) {
		if (!partition_by_name("recovery", dev, sizeof(dev))) {
			dprintf(tty, "recv: no partition named recovery\n");
			return;
		}
	} else if (!strncmp(target, "/tmp/", 5) && !strstr(target, "..")) {
		snprintf(dev, sizeof(dev), "%s", target);
	} else {
		dprintf(tty, "recv: target %s not allowed\n", target);
		return;
	}
	out = open(dev, O_WRONLY | O_CREAT, 0600);
	if (out < 0) {
		dprintf(tty, "recv: open %s: %s\n", dev, strerror(errno));
		return;
	}
	klog("recv: %lu bytes to %s", size, dev);
	dprintf(tty, "READY\n");

	while (got < size) {
		size_t want = size - got < sizeof(buf) ? size - got : sizeof(buf);
		ssize_t n = read(tty, buf, want), w = 0;

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			klog("recv: read: %s after %lu bytes", n < 0 ? strerror(errno) : "EOF", got);
			close(out);
			return;
		}
		crc = crc32_update(crc, buf, n);
		while (w < n) {
			ssize_t x = write(out, buf + w, n - w);

			if (x <= 0) {
				dprintf(tty, "recv: write %s: %s\n", dev, strerror(errno));
				close(out);
				return;
			}
			w += x;
		}
		got += n;
	}
	fsync(out);
	close(out);

	/* read it back through a fresh descriptor, past the page cache where possible */
	out = open(dev, O_RDONLY);
	if (out >= 0) {
		posix_fadvise(out, 0, 0, POSIX_FADV_DONTNEED);
		for (got = 0; got < size; ) {
			size_t want = size - got < sizeof(buf) ? size - got : sizeof(buf);
			ssize_t n = read(out, buf, want);

			if (n <= 0)
				break;
			crc_back = crc32_update(crc_back, buf, n);
			got += n;
		}
		close(out);
	}
	klog("recv: done, crc %08x, read back %08x", crc, crc_back);
	dprintf(tty, "recv: %lu bytes to %s, crc32 %08x, read back %08x\n", size, dev, crc,
		crc_back);
}

static int serve(const char *tty)
{
	static char line[8192];

	for (;;) {
		struct termios tio;
		size_t len = 0;
		unsigned long lines = 0;
		int fd = open(tty, O_RDWR | O_NOCTTY);

		if (fd < 0) {
			klog("serve: open %s: %s", tty, strerror(errno));
			sleep(1);
			continue;
		}
		if (tcgetattr(fd, &tio) == 0) {
			cfmakeraw(&tio);
			tio.c_cc[VMIN] = 1;
			tio.c_cc[VTIME] = 0;
			if (tcsetattr(fd, TCSANOW, &tio) < 0)
				klog("serve: tcsetattr: %s", strerror(errno));
		} else {
			klog("serve: tcgetattr: %s", strerror(errno));
		}
		klog("serve: %s open, waiting for commands", tty);
		for (;;) {
			char c;
			ssize_t n = read(fd, &c, 1);

			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0) {
				klog("serve: read %s: %s (after %lu commands)", tty,
				     n < 0 ? strerror(errno) : "end of file", lines);
				break;
			}
			if (c == '\r')
				continue;
			if (c != '\n') {
				if (len < sizeof(line) - 1)
					line[len++] = c;
				continue;
			}
			line[len] = 0;
			len = 0;
			if (!line[0])
				continue;
			if (!lines++)
				klog("serve: first command received");
			if (!strncmp(line, "@recv ", 6))
				recv_file(fd, line + 6);
			else
				run_line(fd, line);
		}
		close(fd);
		sleep(1);
	}
	return 0;
}

static int console(const char *tty, char **argv)
{
	for (;;) {
		pid_t pid = fork();

		if (pid < 0) {
			klog("console: fork: %s", strerror(errno));
			sleep(1);
			continue;
		}
		if (pid == 0) {
			int fd;

			if (setsid() < 0)
				klog("console: setsid: %s", strerror(errno));
			fd = open(tty, O_RDWR | O_NOCTTY);
			if (fd < 0) {
				klog("console: open %s: %s", tty, strerror(errno));
				_exit(1);
			}
			if (ioctl(fd, TIOCSCTTY, 1) < 0)
				klog("console: TIOCSCTTY %s: %s", tty, strerror(errno));
			dup2(fd, 0);
			dup2(fd, 1);
			dup2(fd, 2);
			if (fd > 2)
				close(fd);
			if (write(1, "\r\ncappu spike shell\r\n", 21) < 0)
				klog("console: write %s: %s", tty, strerror(errno));
			execv(argv[0], argv);
			klog("console: exec %s: %s", argv[0], strerror(errno));
			_exit(1);
		}
		{
			int status = 0;

			waitpid(pid, &status, 0);
			klog("console: %s session ended (status %#x), reopening", tty, status);
		}
		sleep(1);
	}
}

/*
 * USB-C. Nothing in this kernel drives the TUSB320 (i2c@11013000, address 0x47), and a
 * USB-C host port only supplies VBUS once it sees the tablet's Rd on a CC line.
 *
 * The stock (LineageOS) driver can leave the chip unusable for whoever boots next: its
 * shutdown hook reselects UFP, the detach interrupt that causes runs the driver's
 * "back to DRP" sequence in the IRQ thread, and that sequence sets DISABLE_RD_RP
 * (register 0x45 bit 2), sleeps 100 ms and only then clears it. The restart wins the
 * race, the chip stays powered through the reset and keeps Rd/Rp off: the host sees
 * nothing on CC and the tablet runs on battery (found 2026-11-22: reg45 = 0x04, both CC
 * lines reported as an audio accessory, no VBUS; clearing the bit and restarting
 * detection brought VBUS back at once).
 *
 * So: re-enable the terminations, select UFP (this board cannot source VBUS anyway) and
 * restart attach detection. The mode only changes while unattached, so drop the CC
 * terminations around the write with DISABLE_TERM (the stock sequence for this chip).
 *   reg 0x08 bits 5:4  current advertised by the host: 0 default, 1 1.5 A, 3 3 A
 *   reg 0x09 bits 7:6  attached state: 0 none, 1 source, 2 sink, 3 accessory;
 *            bit 4     interrupt, write 1 to clear
 *   reg 0x0a bits 5:4  MODE_SELECT: 0 PORT pin, 1 UFP, 2 DFP, 3 DRP; bit 0 DISABLE_TERM
 *   reg 0x45 bit 2     DISABLE_RD_RP
 */
#define TUSB320_ADDR	0x47
#define CHARGER		"/sys/class/power_supply/bq24190-charger/"
#define GAUGE		"/sys/class/power_supply/bq27520-0/"

static int i2c_bus_for(const char *of_suffix)
{
	DIR *d = opendir("/sys/bus/i2c/devices");
	size_t want = strlen(of_suffix);
	struct dirent *e;
	int bus = -1;

	if (!d)
		return -1;
	while ((e = readdir(d))) {
		char path[300], link[300];
		ssize_t n;

		if (strncmp(e->d_name, "i2c-", 4))
			continue;
		snprintf(path, sizeof(path), "/sys/bus/i2c/devices/%s/of_node", e->d_name);
		n = readlink(path, link, sizeof(link) - 1);
		if (n < (ssize_t)want)
			continue;
		link[n] = 0;
		if (!strcmp(link + n - want, of_suffix))
			bus = atoi(e->d_name + 4);
	}
	closedir(d);
	return bus;
}

static int i2c_rd(int fd, int addr, int reg)
{
	unsigned char r = reg, v = 0;
	struct i2c_msg m[2] = {
		{ .addr = addr, .len = 1, .buf = &r },
		{ .addr = addr, .flags = I2C_M_RD, .len = 1, .buf = &v },
	};
	struct i2c_rdwr_ioctl_data x = { .msgs = m, .nmsgs = 2 };

	return ioctl(fd, I2C_RDWR, &x) < 0 ? -errno : v;
}

static int i2c_wr(int fd, int addr, int reg, int val)
{
	unsigned char b[2] = { reg, val };
	struct i2c_msg m = { .addr = addr, .len = 2, .buf = b };
	struct i2c_rdwr_ioctl_data x = { .msgs = &m, .nmsgs = 1 };

	return ioctl(fd, I2C_RDWR, &x) < 0 ? -errno : 0;
}

static int read_int_file(const char *path)
{
	char buf[16] = "";
	int fd = open(path, O_RDONLY);

	if (fd < 0)
		return -1;
	if (read(fd, buf, sizeof(buf) - 1) < 0)
		buf[0] = 0;
	close(fd);
	return buf[0] ? atoi(buf) : -1;
}

static void write_file(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY);

	if (fd < 0 || write(fd, val, strlen(val)) < 0)
		klog("init: write %s to %s: %s", val, path, strerror(errno));
	if (fd >= 0)
		close(fd);
}

/* Returns the current the host advertises (reg 0x08 bits 5:4) once attached as a sink,
 * or -1 if the tablet is not a sink on a live port. */
static int usbc_sink(void)
{
	int bus = i2c_bus_for("/i2c@11013000");
	int r8, r9, ra, r45, fd, i;
	char dev[32];

	if (bus < 0) {
		klog("usbc: no i2c adapter for i2c@11013000");
		return -1;
	}
	snprintf(dev, sizeof(dev), "/dev/i2c-%d", bus);
	fd = open(dev, O_RDWR);
	if (fd < 0) {
		klog("usbc: open %s: %s", dev, strerror(errno));
		return -1;
	}
	r8 = i2c_rd(fd, TUSB320_ADDR, 0x08);
	r9 = i2c_rd(fd, TUSB320_ADDR, 0x09);
	ra = i2c_rd(fd, TUSB320_ADDR, 0x0a);
	r45 = i2c_rd(fd, TUSB320_ADDR, 0x45);
	if (r8 < 0 || r9 < 0 || ra < 0 || r45 < 0) {
		klog("usbc: tusb320 not answering on %s: %s", dev, strerror(-ra));
		close(fd);
		return -1;
	}
	klog("usbc: found reg8=%#04x reg9=%#04x regA=%#04x reg45=%#04x, charger pg_stat=%d%s",
	     r8, r9, ra, r45, read_int_file(CHARGER "f_pg_stat"),
	     r45 & 4 ? " -- CC terminations disabled (stock driver shutdown race)" : "");

	if ((r45 & 4) || ((ra >> 4) & 3) != 1 || (r9 >> 6) != 2) {
		i2c_wr(fd, TUSB320_ADDR, 0x45, r45 & ~4);
		i2c_wr(fd, TUSB320_ADDR, 0x0a, (ra & 0xc0) | 0x11);
		usleep(300000);
		i2c_wr(fd, TUSB320_ADDR, 0x0a, (ra & 0xc0) | 0x10);
		for (i = 0; i < 30; i++) {
			usleep(100000);
			r9 = i2c_rd(fd, TUSB320_ADDR, 0x09);
			if (r9 >= 0 && (r9 >> 6) == 2)
				break;
		}
		usleep(300000);		/* VBUS and the charger's input detection */
	}
	if (r9 >= 0)
		i2c_wr(fd, TUSB320_ADDR, 0x09, (r9 & 0x07) | 0x10);
	/*
	 * The advertised current is only valid some time after the attach (it read 0,
	 * "default", right after one on 2026-11-22 and 3 A a moment later): give it up
	 * to a second to show more than the default.
	 */
	for (i = 0; i < 10; i++) {
		r8 = i2c_rd(fd, TUSB320_ADDR, 0x08);
		if (r8 < 0 || ((r8 >> 4) & 3))
			break;
		usleep(100000);
	}
	klog("usbc: now   reg8=%#04x reg9=%#04x regA=%#04x reg45=%#04x, charger pg_stat=%d",
	     r8, r9, i2c_rd(fd, TUSB320_ADDR, 0x0a), i2c_rd(fd, TUSB320_ADDR, 0x45),
	     read_int_file(CHARGER "f_pg_stat"));
	close(fd);
	return r8 >= 0 && r9 >= 0 && (r9 >> 6) == 2 ? (r8 >> 4) & 3 : -1;
}

/*
 * Charging. LK hands over with charging off and 512 mA / 500 mA limits, and the charger
 * is not reset (see disable-reset in the DTS), so switch it on and take what the port
 * offers on CC: the charger only knows USB 2.0 detection, which says 500 mA on a host.
 */
static void charger_setup(int advertised)
{
	const char *iin = NULL, *ichg = NULL;

	write_file(CHARGER "charge_type", "Fast");
	if (advertised == 3) {			/* 3 A port */
		iin = "2000000";
		ichg = "2048000";
	} else if (advertised == 1) {		/* 1.5 A port */
		iin = "1500000";
		ichg = "1536000";
	}
	if (iin) {
		write_file(CHARGER "input_current_limit", iin);
		write_file(CHARGER "constant_charge_current", ichg);
	}
	klog("charger: port advertises %s; input limit %d uA, charge current %d uA",
	     advertised == 3 ? "3 A" : advertised == 1 ? "1.5 A" :
	     advertised < 0 ? "nothing (not attached as a sink)" : "default USB current",
	     read_int_file(CHARGER "input_current_limit"),
	     read_int_file(CHARGER "constant_charge_current"));
}

/*
 * Nothing else here watches the battery temperature (the stock kernel does it in
 * software). Stop charging outside 0..45 C by the gauge, resume inside 2..42 C.
 */
static int charge_guard(void)
{
	int stopped = 0;

	for (;;) {
		int t = read_int_file(GAUGE "temp");	/* tenths of a degree C */

		if (t != -1 && !stopped && (t > 450 || t < 0)) {
			write_file(CHARGER "charge_type", "None");
			klog("charger: battery at %d.%d C, charging stopped", t / 10, abs(t % 10));
			stopped = 1;
		} else if (t != -1 && stopped && t < 420 && t > 20) {
			write_file(CHARGER "charge_type", "Fast");
			klog("charger: battery at %d.%d C, charging resumed", t / 10, t % 10);
			stopped = 0;
		}
		sleep(15);
	}
	return 0;
}

/*
 * Display checks that do not need anyone to look at the panel.
 *
 * vblank: time N vertical blanks on the mediatek-drm CRTC. The DSI host is the timing
 * master of the whole pipeline, so vblanks arriving at a steady rate mean frames are
 * leaving both links; the period is the real refresh rate.
 */
static int vblank(int frames)
{
	struct timespec t0, t1;
	char dev[32], name[32];
	int fd = -1, i;
	double dt;

	for (i = 0; i < 4 && fd < 0; i++) {
		struct drm_version v = { .name = name, .name_len = sizeof(name) - 1 };

		snprintf(dev, sizeof(dev), "/dev/dri/card%d", i);
		fd = open(dev, O_RDWR);
		if (fd < 0)
			continue;
		memset(name, 0, sizeof(name));
		if (ioctl(fd, DRM_IOCTL_VERSION, &v) < 0 || strcmp(name, "mediatek")) {
			close(fd);
			fd = -1;
		}
	}
	if (fd < 0) {
		printf("vblank: no mediatek DRM device\n");
		return 1;
	}

	for (i = 0; i <= frames; i++) {
		union drm_wait_vblank w = {
			.request = { .type = _DRM_VBLANK_RELATIVE, .sequence = 1 },
		};

		if (ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &w) < 0) {
			printf("vblank: wait %d on %s: %s\n", i, dev, strerror(errno));
			return 1;
		}
		if (i == 0)
			clock_gettime(CLOCK_MONOTONIC, &t0);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
	printf("vblank: %d frames in %.4f s on %s = %.3f Hz (%.2f us per frame)\n",
	       frames, dt, dev, frames / dt, dt / frames * 1e6);
	close(fd);
	return 0;
}

/*
 * pattern: a picture for one look at the glass. Left half blue, right half green (so
 * swapped links show), a white frame one pixel inside the edge (so a shifted or cropped
 * half shows), a white diagonal from top left to bottom right (so the seam between the
 * two halves shows), and red, green, blue and white squares in the top left corner.
 */
static int pattern(const char *dev)
{
	struct fb_var_screeninfo v;
	struct fb_fix_screeninfo f;
	uint32_t red, green, blue, white, *row;
	unsigned int x, y;
	uint8_t *map;
	int fd = open(dev, O_RDWR);

	if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &v) < 0 ||
	    ioctl(fd, FBIOGET_FSCREENINFO, &f) < 0) {
		printf("pattern: %s: %s\n", dev, strerror(errno));
		return 1;
	}
	if (v.bits_per_pixel != 32) {
		printf("pattern: %s is %u bpp, need 32\n", dev, v.bits_per_pixel);
		return 1;
	}
	map = mmap(NULL, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		printf("pattern: mmap %s: %s\n", dev, strerror(errno));
		return 1;
	}
	red = ((1u << v.red.length) - 1) << v.red.offset;
	green = ((1u << v.green.length) - 1) << v.green.offset;
	blue = ((1u << v.blue.length) - 1) << v.blue.offset;
	white = red | green | blue;

	for (y = 0; y < v.yres; y++) {
		row = (uint32_t *)(map + (size_t)(y + v.yoffset) * f.line_length);
		for (x = 0; x < v.xres; x++) {
			uint32_t c = x < v.xres / 2 ? blue >> 1 & blue : green >> 1 & green;
			unsigned int d = (unsigned int)((uint64_t)y * v.xres / v.yres);

			if (x == 1 || y == 1 || x == v.xres - 2 || y == v.yres - 2)
				c = white;
			else if (x + 3 > d && x < d + 3)
				c = white;
			else if (y >= 64 && y < 192 && x >= 64 && x < 576)
				c = x < 192 ? red : x < 320 ? green : x < 448 ? blue : white;
			row[x] = c;
		}
	}
	printf("pattern: %ux%u on %s (%s), line %u bytes\n", v.xres, v.yres, dev, f.id,
	       f.line_length);
	munmap(map, f.smem_len);
	close(fd);
	return 0;
}

/*
 * disptrace: follow the DSI0 video state machine for a while and print one line per
 * change of vertical period (sync, back porch, active, front porch), with the DSI line
 * counter and the number of pixels the SPLIT has taken in. Shows the real DSI frame
 * period and how much of a source frame each DSI frame consumes.
 */
static int disptrace(int ms)
{
	static const char *const name[16] = {
		[0] = "-", [1] = "VFP", [2] = "VACT", [4] = "VBP", [8] = "VSA",
	};
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	volatile uint32_t *dsi, *dsi1, *split, *ovl, *rdma;
	struct timespec t0, t;
	unsigned int last = 0xff, n = 0, last1 = 0xff;
	double vsa1 = -1;
	uint32_t split_last = 0;
	double last_vsa = -1;

	if (fd < 0) {
		printf("disptrace: /dev/mem: %s\n", strerror(errno));
		return 1;
	}
	dsi = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, fd, 0x1401b000);
	split = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, fd, 0x14019000);
	ovl = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x1400c000);
	rdma = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x1400e000);
	dsi1 = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, fd, 0x1401c000);
	if (dsi == MAP_FAILED || split == MAP_FAILED || ovl == MAP_FAILED || rdma == MAP_FAILED ||
	    dsi1 == MAP_FAILED) {
		printf("disptrace: mmap: %s\n", strerror(errno));
		return 1;
	}
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		unsigned int period = (dsi[0x164 / 4] >> 12) & 0xf;
		unsigned int period1 = (dsi1[0x164 / 4] >> 12) & 0xf;
		double us;

		clock_gettime(CLOCK_MONOTONIC, &t);
		us = (t.tv_sec - t0.tv_sec) * 1e6 + (t.tv_nsec - t0.tv_nsec) / 1e3;
		if (us > ms * 1000.0 || n > 200)
			break;
		/* the second link: when its frame starts, relative to the first one's */
		if (period1 != last1) {
			last1 = period1;
			if (period1 == 8)
				vsa1 = us;
		}
		if (period == last)
			continue;
		last = period;
		n++;
		{
			uint32_t sp = split[0x08 / 4] & 0xffffff;

			printf("%9.0f us  %-4s line %7u  split in-pixels %8u (+%u)", us,
			       name[period] ? name[period] : "?", dsi[0x16c / 4] & 0x3fffff,
			       sp, (sp - split_last) & 0xffffff);
			split_last = sp;
		}
		if (period == 8) {
			if (last_vsa >= 0)
				printf("   frame %.0f us", us - last_vsa);
			last_vsa = us;
			printf("  dsi1 now %s, its last VSA %.0f us ago",
			       name[period1] ? name[period1] : "?", vsa1 < 0 ? -1.0 : us - vsa1);
			/* what the overlay and the read-DMA did during that frame */
			printf("  ovl sta %#x intsta %#x  rdma intsta %#x", ovl[0], ovl[0x08 / 4],
			       rdma[0x04 / 4]);
			ovl[0x08 / 4] = 0;
			rdma[0x04 / 4] = 0;
		}
		printf("\n");
	}
	return 0;
}

/*
 * dispcurve: sample the pipeline every STEP microseconds for MS milliseconds: where the
 * DSI is in its frame, which line the overlay and the overdrive block are on, and how
 * many pixels the SPLIT has taken in. Shows when a source frame starts and ends
 * relative to the DSI frame, and where the stream stalls.
 */
static int dispcurve(int ms, int step_us, int edges_only)
{
	double vact_since = -1;
	unsigned int prev_period = 0xff;
	static const char *const name[16] = {
		[0] = "-", [1] = "VFP", [2] = "VACT", [4] = "VBP", [8] = "VSA",
	};
	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	volatile uint32_t *dsi, *split, *ovl, *od, *rdma;
	struct timespec t0, t;
	double next = 0;

	if (fd < 0)
		return 1;
	dsi = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, fd, 0x1401b000);
	split = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, fd, 0x14019000);
	ovl = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, fd, 0x1400c000);
	od = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, fd, 0x14023000);
	rdma = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, fd, 0x1400e000);
	if (dsi == MAP_FAILED || split == MAP_FAILED || ovl == MAP_FAILED ||
	    od == MAP_FAILED || rdma == MAP_FAILED)
		return 1;
	printf("   t(us) dsi   ovl y,x     od-in y,x    od-out y,x   split-in px (lines)  rdma-int ovl-flow\n");
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		double us;

		clock_gettime(CLOCK_MONOTONIC, &t);
		us = (t.tv_sec - t0.tv_sec) * 1e6 + (t.tv_nsec - t0.tv_nsec) / 1e3;
		if (us > ms * 1000.0)
			break;
		if (us < next)
			continue;
		next += step_us;
		{
			unsigned int period = (dsi[0x164 / 4] >> 12) & 0xf;
			uint32_t o = ovl[0x244 / 4], i = od[0x24 / 4], q = od[0x28 / 4];
			uint32_t sp = split[0x08 / 4] & 0xffffff;

			if (period == 2 && prev_period != 2)
				vact_since = us;
			prev_period = period;
			/* edges only: the vertical blanking and the first 1.2 ms after it */
			if (edges_only && period == 2 && us - vact_since > 1200)
				continue;

			printf("%8.0f %-4s %5u,%-5u %5u,%-5u %5u,%-5u %8u (%6.1f)  %#04x  %08x\n", us,
			       name[period] ? name[period] : "?",
			       (o >> 16) & 0x1fff, o & 0x1fff, (i >> 16) & 0x1fff, i & 0x1fff,
			       (q >> 16) & 0x1fff, q & 0x1fff, sp, sp / 1536.0,
			       rdma[0x04 / 4], ovl[0x240 / 4]);
		}
	}
	return 0;
}

/*
 * dsirestart: stop the two DSI links and start them again with one of several
 * sequences, at register-write speed, to find out what the pairing needs.
 * FLAGS: 1 reset both engines before pairing, 2 drop and set DUAL_EN again,
 *        4 set the second link's start bit as well, 8 second link's mode first,
 *        16 wait DELAY_US after every step instead of only before the start.
 */
static int dsirestart(int flags, int delay_us)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	volatile uint32_t *m, *s;
	int i;

	if (fd < 0)
		return 1;
	m = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x1401b000);
	s = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x1401c000);
	if (m == MAP_FAILED || s == MAP_FAILED)
		return 1;
#define STEP() do { if (flags & 16) usleep(delay_us); } while (0)
	/* video off: command mode, wait until both engines are idle */
	m[0x14 / 4] = 0;
	s[0x14 / 4] = 0;
	for (i = 0; i < 100000 && ((m[0x0c / 4] | s[0x0c / 4]) & 0x80000000); i++)
		usleep(10);
	m[0x00] = 0;
	s[0x00] = 0;
	STEP();
	if (flags & 2) {
		m[0x10 / 4] &= ~0x10;
		s[0x10 / 4] &= ~0x10;
		STEP();
	}
	if (flags & 1) {
		m[0x10 / 4] |= 1;
		m[0x10 / 4] &= ~1;
		s[0x10 / 4] |= 1;
		s[0x10 / 4] &= ~1;
		STEP();
	}
	m[0x10 / 4] |= 0x10;
	s[0x10 / 4] |= 0x10;
	STEP();
	if (flags & 8) {
		s[0x14 / 4] = 3;
		m[0x14 / 4] = 3;
	} else {
		m[0x14 / 4] = 3;
		s[0x14 / 4] = 3;
	}
	usleep(delay_us);
	m[0x00] = 1;
	if (flags & 4)
		s[0x00] = 1;
#undef STEP
	printf("dsirestart flags %d delay %d us: idle after %d polls\n", flags, delay_us, i);
	return 0;
}

static int init_main(void)
{
	static const struct { const char *src, *dir, *type; } fs[] = {
		{ "devtmpfs", "/dev", "devtmpfs" }, { "proc", "/proc", "proc" },
		{ "sysfs", "/sys", "sysfs" }, { "pstore", "/sys/fs/pstore", "pstore" },
		{ "debugfs", "/sys/kernel/debug", "debugfs" }, { "tmpfs", "/tmp", "tmpfs" },
	};
	unsigned int i;
	pid_t pid;

	for (i = 0; i < sizeof(fs) / sizeof(fs[0]); i++)
		if (mount(fs[i].src, fs[i].dir, fs[i].type, 0, NULL) < 0)
			klog("init: mount %s: %s", fs[i].dir, strerror(errno));
	setenv("PATH", "/bin:/usr/bin", 1);
	write_file("/proc/sys/kernel/sysrq", "1");

	/* restart, into fastboot, unless a host session touches the flag file in time */
	pid = fork();
	if (pid == 0)
		_exit(deadman(120, "/tmp/host-attached"));

	charger_setup(usbc_sink());
	pid = fork();
	if (pid == 0)
		_exit(charge_guard());
	klog("init: userspace up, serving /dev/ttyGS0");
	return serve("/dev/ttyGS0");
}

int main(int argc, char **argv)
{
	if (getpid() == 1)
		return init_main();
	if (argc == 4 && !strcmp(argv[1], "deadman"))
		return deadman(atoi(argv[2]), argv[3]);
	if (argc == 4 && !strcmp(argv[1], "dump"))
		return dump(strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 0));
	if (argc >= 2 && !strcmp(argv[1], "vblank"))
		return vblank(argc > 2 ? atoi(argv[2]) : 120);
	if (argc >= 4 && !strcmp(argv[1], "dsirestart"))
		return dsirestart(atoi(argv[2]), atoi(argv[3]));
	if (argc >= 2 && !strcmp(argv[1], "dispcurve"))
		return dispcurve(argc > 2 ? atoi(argv[2]) : 36, argc > 3 ? atoi(argv[3]) : 500,
				 argc > 4);
	if (argc >= 2 && !strcmp(argv[1], "disptrace"))
		return disptrace(argc > 2 ? atoi(argv[2]) : 150);
	if (argc >= 2 && !strcmp(argv[1], "pattern"))
		return pattern(argc > 2 ? argv[2] : "/dev/fb0");
	if (argc == 2 && !strcmp(argv[1], "usbc")) {
		charger_setup(usbc_sink());
		return 0;
	}
	if (argc == 2 && !strcmp(argv[1], "restart"))
		return restart();
	if (argc == 3 && !strcmp(argv[1], "serve"))
		return serve(argv[2]);
	if (argc >= 4 && !strcmp(argv[1], "console"))
		return console(argv[2], &argv[3]);
	dprintf(2, "usage: %s deadman SECONDS FLAGFILE | restart | usbc | vblank [FRAMES] | disptrace [MS] | dsirestart FLAGS DELAY_US | dispcurve [MS [STEP_US [edges]]] | pattern [FBDEV] | dump ADDR LEN | serve TTY | console TTY PROGRAM [ARG...]\n", argv[0]);
	return 2;
}
