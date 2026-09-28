/*
 * tags.h - shared types, constants and function declarations
 *
 * RFID access logger: reads 4-byte tag IDs, checks them against a
 * whitelist, and records every scan (granted, denied or duplicate) in a
 * binary log file.
 *
 * Implemented in:  reader.c  (input + ring buffer)
 *                  state.c   (state machine, whitelist, log)
 *                  main.c    (menu + main loop)
 */

#ifndef TAGS_H_INCLUDED
#define TAGS_H_INCLUDED

#include <stdint.h>
#include <stdbool.h>
#define UID_BYTE 4  /* bytes in one tag UID (per reader datasheet) */
#define BUFFER_CAPACITY 8  /* ring buffer slots (~2 UIDs); independent of UID_BYTE */
#define RINGBUF_EMPTY -1  /* ringBufRead() result when empty; not a byte value */

/* An authorized card. The UID is raw bytes, not a string (0x00 is a valid
 * UID byte). Fixed-size arrays only - no pointers - so the whole struct can
 * be written to disk with fwrite() and read back with fread().
 */
typedef struct {
    uint8_t tag_id[UID_BYTE];
    char id_label[20];
} tag;

/* One scan event: which tag, when, and the outcome. "timestamp" is a tick
 * counter for now (no RTC). statusbyte holds the FLAG_* bits from state.c.
 */
typedef struct {
    uint8_t tag_id [UID_BYTE];
    uint32_t timestamp;
    uint8_t statusbyte;
} log_entry;

/* States of the scanning state machine. The order must match the dispatch
 * table sp[] in state.c. The STATE_ prefix avoids a clash with the ERROR
 * macro defined by some system headers.
 */
typedef enum {
STATE_IDLE,
STATE_SCANNING,
STATE_VALIDATING,
STATE_LOGGING,
STATE_COOLDOWN,
STATE_ERROR
} system_state;

/* Circular queue of raw bytes. rd_ptr = next byte to read, wr_ptr = next
 * slot to write. rd_ptr == wr_ptr means either empty or full, so the
 * "full" flag tells the two apart.
 */
typedef struct {
    uint8_t *rd_ptr;
    uint8_t *wr_ptr;
    uint8_t arr[BUFFER_CAPACITY];
    bool full;
} ringBuf;

/* ---- reader.c: input and ring buffer ---- */
int scan_input(void);
void ringBuf_init(ringBuf *rb);
bool ringBufWrite(ringBuf *rb, uint8_t byte);
int ringBufRead (ringBuf *rb);

/* ---- state.c: state handlers - each returns the NEXT state ---- */
system_state state_idle (void);
system_state state_scanning(void);
system_state state_validating (void);
system_state state_logging (void);
system_state state_cooldown (void);
system_state state_error (void);


/* One-time startup: ring buffer, whitelist file, log file. */
void system_init(void);


/* Dispatch table: function pointers indexed by system_state. */
extern system_state (*sp[6]) (void);
extern bool  scan_ended;  /* set on EOF or 'q'; ends the main loop */

/* Whitelist lookup. */
bool is_authorized(uint8_t tag_id[UID_BYTE]);

/* Command-mode operations, called from the menu in main.c. */
void dump_log(void);
void add_tag(void);
void clear_log(void);
void close_program(void);
#endif // TAGS_H_INCLUDED
