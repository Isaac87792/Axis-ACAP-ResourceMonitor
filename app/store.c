/* History ring files on the SD card; see store.h */
#define _GNU_SOURCE
#include "store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define MAGIC   "RMHIST01"
#define VERSION 1u
#define HEAD    64                      /* bytes before the first point */

typedef struct { char magic[8]; uint32_t version, step, cap, recSize, layout, head, count; } Header;

long long ring_file_bytes(uint32_t cap, uint32_t recSize) { return HEAD + (long long)cap * recSize; }

static int write_all(int fd, const void* buf, size_t n, off_t at) {
	const unsigned char* p = buf;
	while (n) {
		ssize_t w = pwrite(fd, p, n, at);
		if (w < 0) { if (errno == EINTR) continue; return 0; }
		p += w; n -= (size_t)w; at += w;
	}
	return 1;
}
static int read_all(int fd, void* buf, size_t n, off_t at) {
	unsigned char* p = buf;
	while (n) {
		ssize_t g = pread(fd, p, n, at);
		if (g < 0) { if (errno == EINTR) continue; return 0; }
		if (g == 0) return 0;
		p += g; n -= (size_t)g; at += g;
	}
	return 1;
}
static int write_header(Ring* r) {
	unsigned char b[HEAD];
	memset(b, 0, sizeof(b));
	Header h;
	memcpy(h.magic, MAGIC, 8);
	h.version = VERSION; h.step = r->step; h.cap = r->cap; h.recSize = r->recSize; h.layout = r->layout; h.head = r->head; h.count = r->count;
	memcpy(b, &h, sizeof(h));
	return write_all(r->fd, b, sizeof(b), 0);
}
/* Make the file in full: header, then every point's place filled with zeros, so it never grows */
static int make_file(Ring* r) {
	int fd = open(r->path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0) return 0;
	r->fd = fd; r->head = 0; r->count = 0;
	int ok = write_header(r);
	static unsigned char zero[65536];
	long long left = (long long)r->cap * r->recSize;
	off_t at = HEAD;
	while (ok && left > 0) {
		size_t n = left > (long long)sizeof(zero) ? sizeof(zero) : (size_t)left;
		ok = write_all(fd, zero, n, at);
		at += (off_t)n; left -= (long long)n;
	}
	if (ok) ok = fsync(fd) == 0;
	if (!ok) { int e = errno; close(fd); unlink(r->path); r->fd = -1; errno = e; return 0; }
	return 1;
}

int ring_open(Ring* r, const char* path, uint32_t step, uint32_t cap, uint32_t recSize, uint32_t layout, int pendCap, int* made) {
	memset(r, 0, sizeof(*r));
	r->fd = -1;
	pthread_mutex_init(&r->mu, NULL);
	snprintf(r->path, sizeof(r->path), "%s", path);
	r->step = step; r->cap = cap; r->recSize = recSize; r->layout = layout;
	r->pendCap = pendCap < 1 ? 1 : pendCap;
	r->pend = malloc((size_t)r->pendCap * recSize);
	if (made) *made = 0;
	if (!r->pend) return 0;
	int fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd >= 0) {
		Header h;
		struct stat st;
		int good = read_all(fd, &h, sizeof(h), 0) && fstat(fd, &st) == 0 && !memcmp(h.magic, MAGIC, 8) && h.version == VERSION &&
			h.step == step && h.cap == cap && h.recSize == recSize && h.layout == layout && h.head < cap && h.count <= cap &&
			st.st_size == ring_file_bytes(cap, recSize);
		if (good) { r->fd = fd; r->head = h.head; r->count = h.count; return 1; }
		close(fd);
	}
	if (!make_file(r)) { free(r->pend); r->pend = NULL; return 0; }
	if (made) *made = 1;
	return 1;
}

/* lock held */
static int flush_locked(Ring* r) {
	if (r->fd < 0) return 0;
	int ok = 1;
	for (int i = 0; i < r->pendN && ok; i++) {
		ok = write_all(r->fd, r->pend + (size_t)i * r->recSize, r->recSize, HEAD + (off_t)r->head * r->recSize);
		if (ok) { r->head = (r->head + 1) % r->cap; if (r->count < r->cap) r->count++; }
	}
	if (ok) ok = write_header(r);
	r->pendN = 0;                       /* a point that could not be written is dropped, never kept growing */
	return ok;
}

int ring_add(Ring* r, const void* rec) {
	pthread_mutex_lock(&r->mu);
	int ok = 1;
	if (r->fd >= 0) {
		memcpy(r->pend + (size_t)r->pendN * r->recSize, rec, r->recSize);
		if (++r->pendN >= r->pendCap) ok = flush_locked(r);
	} else ok = 0;
	pthread_mutex_unlock(&r->mu);
	return ok;
}

int ring_flush(Ring* r) {
	pthread_mutex_lock(&r->mu);
	int ok = r->pendN ? flush_locked(r) : r->fd >= 0;
	pthread_mutex_unlock(&r->mu);
	return ok;
}

uint32_t ring_count(Ring* r) {
	pthread_mutex_lock(&r->mu);
	uint32_t n = r->fd >= 0 ? r->count + (uint32_t)r->pendN : 0;
	if (n > r->cap) n = r->cap;
	pthread_mutex_unlock(&r->mu);
	return n;
}

uint32_t ring_read(Ring* r, uint32_t want, void (*cb)(const void* rec, void* u), void* u) {
	pthread_mutex_lock(&r->mu);
	if (r->fd < 0) { pthread_mutex_unlock(&r->mu); return 0; }
	uint32_t pend = (uint32_t)r->pendN, onDisk = r->count, total = onDisk + pend;
	if (want > total) want = total;
	if (want > r->cap) want = r->cap;
	uint32_t fromDisk = want > pend ? want - pend : 0, skipPend = want < pend ? pend - want : 0;
	if (fromDisk > onDisk) fromDisk = onDisk;
	/* Read in blocks, so a 30 day file is never all in memory at once */
	enum { BLOCK = 256 };
	unsigned char* buf = malloc((size_t)BLOCK * r->recSize);
	uint32_t done = 0;
	uint32_t slot = (r->head + r->cap - fromDisk) % r->cap;
	while (buf && done < fromDisk) {
		uint32_t run = fromDisk - done;
		if (run > BLOCK) run = BLOCK;
		if (slot + run > r->cap) run = r->cap - slot;      /* to the end of the file, then from the start */
		if (!read_all(r->fd, buf, (size_t)run * r->recSize, HEAD + (off_t)slot * r->recSize)) break;
		for (uint32_t i = 0; i < run; i++) cb(buf + (size_t)i * r->recSize, u);
		done += run;
		slot = (slot + run) % r->cap;
	}
	free(buf);
	for (uint32_t i = skipPend; i < pend; i++) { cb(r->pend + (size_t)i * r->recSize, u); done++; }
	pthread_mutex_unlock(&r->mu);
	return done;
}

int ring_clear(Ring* r) {
	pthread_mutex_lock(&r->mu);
	r->pendN = 0; r->head = 0; r->count = 0;
	int ok = r->fd >= 0 && write_header(r);
	pthread_mutex_unlock(&r->mu);
	return ok;
}

void ring_close(Ring* r) {
	pthread_mutex_lock(&r->mu);
	if (r->fd >= 0) {
		if (r->pendN) flush_locked(r);
		fsync(r->fd);
		close(r->fd);
		r->fd = -1;
	}
	free(r->pend);
	r->pend = NULL;
	r->pendN = 0;
	pthread_mutex_unlock(&r->mu);
}
