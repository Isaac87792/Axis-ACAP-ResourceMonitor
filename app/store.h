/* History kept on the SD card: one fixed-size ring file per tier (10 s, 1 min, 5 min points).
 * Each file is set aside in full when it is made, so it never grows; points are written in batches
 * to spare the card. Plain C, so it can be tested on any Linux machine. */
#ifndef STORE_H
#define STORE_H
#include <stdint.h>
#include <pthread.h>

typedef struct {
	int fd;                     /* -1 when closed */
	pthread_mutex_t mu;
	uint32_t step, cap, recSize, layout, head, count;
	unsigned char* pend;        /* points not yet written, oldest first */
	int pendN, pendCap;
	char path[256];
} Ring;

/* Bytes a ring file takes on the card */
long long ring_file_bytes(uint32_t cap, uint32_t recSize);
/* Open the ring at path, or make it (in full) when it is missing or was made for another layout.
 * Returns 1 when open (made: set to 1 when it was made new), 0 on failure (errno kept). */
int ring_open(Ring* r, const char* path, uint32_t step, uint32_t cap, uint32_t recSize, uint32_t layout, int pendCap, int* made);
/* Add a point (recSize bytes, its first 4 bytes the unix time); written when the batch is full */
int ring_add(Ring* r, const void* rec);
/* Write what is waiting. Returns 1 when all is on the card. */
int ring_flush(Ring* r);
/* The last `want` points (written and waiting), oldest first, each given to cb. Returns how many. */
uint32_t ring_read(Ring* r, uint32_t want, void (*cb)(const void* rec, void* u), void* u);
/* Points kept */
uint32_t ring_count(Ring* r);
/* Write what is waiting and close */
void ring_close(Ring* r);
/* Forget every point (the file keeps its size) */
int ring_clear(Ring* r);
#endif
