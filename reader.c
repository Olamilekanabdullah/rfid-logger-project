/*
 * reader.c - byte input and the receive ring buffer
 *
 * scan_input() stands in for a UART receive on real hardware. Incoming
 * bytes are queued in the ring buffer one at a time; state.c pops them
 * once a full UID has arrived.
 */

#include "tags.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

/* Returns one byte (0-255) from stdin, or EOF when input ends.
 * The return type is int, not uint8_t, so that EOF (-1) can never be
 * mistaken for a real byte value. Unlike a real UART poll, getchar()
 * blocks until a key is pressed.
 */
int scan_input(void) {
    int c;
    printf("Enter the byte:\n");
    c = getchar();
    return c;
}
/* Reset the buffer to empty: both pointers at slot 0, not full. The caller
 * owns the struct (no malloc), since its size is fixed at compile time.
 */
void ringBuf_init(ringBuf *rb) {
    rb->rd_ptr = &(rb->arr[0]);
    rb->wr_ptr = &(rb->arr[0]);
    rb->full = false;
    memset(rb->arr, 0, BUFFER_CAPACITY);
}
/* Queue one byte. Returns false if the buffer is full.
 * Policy is reject-new, not overwrite-oldest: every byte belongs to a UID,
 * so silently discarding one would corrupt a tag ID. The caller decides
 * what to do about the failure (state.c goes to STATE_ERROR).
 */
bool ringBufWrite(ringBuf *rb, uint8_t byte){
    if (rb->full == true) {
        printf("Buffer is currently full.\n");
        return false;
        }

    *(rb->wr_ptr) = byte;
    rb->wr_ptr += 1;

    /* stepped past the last slot: wrap back to slot 0 (this is the "ring") */
    if (rb->wr_ptr > &(rb->arr[BUFFER_CAPACITY - 1])) {
            rb->wr_ptr = &(rb->arr[0]);
        }
    /* write pointer caught the read pointer: every slot holds unread data */
    if(rb->wr_ptr == rb->rd_ptr){
        rb->full = true;
    }
    return true;
}

/* Remove and return the oldest byte, or RINGBUF_EMPTY if nothing is queued.
 * int (not uint8_t) so that -1 can't be confused with a real byte.
 */
int ringBufRead (ringBuf *rb) {
int byte = RINGBUF_EMPTY;
/* data is available unless the pointers are equal AND the buffer isn't full */
if (rb->rd_ptr != rb->wr_ptr || rb->full == true) {
    byte = *rb->rd_ptr;
    rb->rd_ptr += 1;
    /* a slot was just freed, so the buffer can no longer be full */
    rb->full = false;

     /* same wrap-around as on the write side */
     if (rb->rd_ptr > &(rb->arr[BUFFER_CAPACITY - 1])) {
            rb->rd_ptr = &(rb->arr[0]);
        }
}

return byte;
}
