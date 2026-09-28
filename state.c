/*
 * state.c - state machine, whitelist, and log file
 *
 * Holds the file-scope state (all static, so private to this file), the
 * state handlers, and the command-mode helpers used by the menu in main.c.
 *
 * Scan flow:  IDLE -> SCANNING (repeated, one byte per call)
 *             -> VALIDATING -> LOGGING -> COOLDOWN -> IDLE
 * A full ring buffer sends SCANNING to ERROR, which resets and recovers.
 */

#include "tags.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Capacity of the whitelist (max tags), not the number in use.
 * If you change this, delete whitelist.dat so it is regenerated at the
 * new size.
 */
#define WHITELIST_SIZE 10
#define COOLDOWN_TICKS 2  /* same tag within this many ticks = duplicate */
/* Outcome flags packed into log_entry.statusbyte - one bit each, so flags
 * can be combined (currently exactly one is set per scan). Set with |=
 * (keeps other bits), test with &.
 */
#define FLAG_GRANTED   0x01
#define FLAG_DENIED    0x02
#define FLAG_DUPLICATE 0x04
#define FLAG_ERROR     0x08  /* reserved: not set anywhere yet */


/* State that must survive between calls to the handlers. Declared static so
 * only this file can touch it; other files go through functions.
 */
static ringBuf rb;
static uint8_t byte_count = 0;  /* bytes of the current UID received so far */
static tag current_tag;  /* the UID being assembled / just completed */
bool scan_ended = false;  /* not static: main.c reads it (extern in tags.h) */
static uint32_t system_tick = 0;  /* stand-in for time: +1 per scan (no RTC yet) */
static uint8_t last_tag_id[UID_BYTE] = {0};  /* debounce: the last accepted scan */
static uint32_t last_scan_tick = 0;  /* debounce: when that scan happened */
static log_entry pending_entry;  /* built by validating, written by logging */
static FILE *log_fp;  /* log file, opened once in append mode */
/* Tags currently in use. WHITELIST_SIZE is the capacity; this is the count.
 * NOTE: starts at 3 to match the defaults below and is not yet restored
 * from whitelist.dat on restart.
 */
static int whitelist_count = 3;

/* Default whitelist. Written to whitelist.dat on first run; after that the
 * file is the source of truth. UIDs are ASCII letters so they can be typed
 * on the keyboard while testing. Unused slots are zero-filled.
 */
tag whitelist[WHITELIST_SIZE] = {
    {{0x41, 0x42, 0x43, 0x44}, "Alice"},   // A B C D
    {{0x45, 0x46, 0x47, 0x48}, "Bob"},     // E F G H
    {{0x57, 0x58, 0x59, 0x5A}, "Charles"}  // W X Y Z
};

/* Fill the whitelist from whitelist.dat, creating the file from the
 * defaults above on first run.
 * TODO (hardening): fopen/fread results are unchecked, and
 * whitelist_count is not restored from the file.
 */
void load_whitelist(void)  {
FILE *fp = fopen("whitelist.dat", "rb");

/* file missing (first run): create it from the defaults, then reopen to read */
if (fp == NULL) {
    fp = fopen("whitelist.dat", "wb");
    fwrite(&whitelist, sizeof(whitelist), 1, fp);
    fclose(fp);
    fp = fopen("whitelist.dat", "rb");
}

fread(&whitelist, sizeof(whitelist), 1, fp);
fclose(fp);

}

/* One-time startup, called from main().
 * A log file that won't open is unrecoverable (nowhere to record scans), so
 * exit(1). A full ring buffer is different - see state_error().
 */
void system_init(void) {
    ringBuf_init(&rb);
    load_whitelist();
    /* "ab" = append + binary: history is kept across runs */
    log_fp = fopen("log.dat", "ab");

    if(log_fp == NULL) {
        printf("The log file didn't open. \n");
        exit(1);
    }
}

/* Check a UID against the whitelist. memcmp() is needed because arrays
 * can't be compared with ==.
 * KNOWN LIMITATION: this scans all WHITELIST_SIZE slots, including the
 * unused zero-filled ones, so a UID of 00 00 00 00 would be granted.
 * It should stop at whitelist_count (which must first be restored on load).
 */
bool is_authorized(uint8_t tag_id[UID_BYTE]) {
    for (int i = 0; i < WHITELIST_SIZE; i++) {
        if (memcmp(tag_id, whitelist[i].tag_id, UID_BYTE) == 0) {
            return true;
        }
    }
    return false;
}

/* Command mode: print every logged scan. Uses its own FILE* so it doesn't
 * disturb log_fp. fread() returns the number of complete entries read, so
 * the loop ends by itself at end of file - no entry count is needed.
 */
void dump_log(void) {
log_entry entry;
FILE *dump_fp;
dump_fp = fopen("log.dat", "rb");

while (fread(&entry, sizeof(log_entry), 1, dump_fp) == 1) {

    for (int j = 0; j < UID_BYTE; j++ ) {
        printf("%x\n", entry.tag_id[j]);
    }
    printf("%d", entry.timestamp);

    /* & isolates one bit, so each flag is tested on its own. (The else-if chain
     * in state_validating sets exactly one, but the format allows combinations.)
     */
    if(entry.statusbyte & FLAG_GRANTED) {
        printf("Granted.");
    }

    if(entry.statusbyte & FLAG_DENIED) {
        printf("Denied.");
    }

    if(entry.statusbyte & FLAG_DUPLICATE) {
        printf("Duplicate.");
    }


}

fclose(dump_fp);
}

/* Command mode: add a tag to the whitelist and re-save the file.
 * The UID is typed as 4 characters and stored as their ASCII codes.
 * Known gaps: EOF during input isn't handled, and the new tag isn't
 * checked against existing ones.
 */
void add_tag(void) {
    /* no free slot: refuse rather than write past the end of the array */
    if (whitelist_count >= WHITELIST_SIZE) {
        printf("You cannot add to the whitelist.\n");
        return;
    }

    /* 1. Read the 4 UID bytes straight from the keyboard */
    printf("Enter 4 characters for the UID: ");
    for (int j = 0; j < UID_BYTE; j++) {
        whitelist[whitelist_count].tag_id[j] = (uint8_t)getchar();
    }

    /* 2. Read the label (19 chars max, leaving room for '\0') */
    printf("Enter a label: ");
    scanf("%19s", whitelist[whitelist_count].id_label);

    /* 3. The slot is now filled, so count it */
    whitelist_count++;

    /* 4. Re-save the whole whitelist so the addition survives a restart */
    FILE *fp = fopen("whitelist.dat", "wb");
    if (fp == NULL) {
        printf("Could not save the whitelist.\n");
        return;
    }
    fwrite(&whitelist, sizeof(whitelist), 1, fp);
    fclose(fp);
}

/* Command mode: empty the log file. Safe here because the menu runs before
 * scanning starts, so log_fp is not mid-write.
 */
void clear_log(void) {
    /* "wb" truncates the file to zero bytes; nothing needs to be written */
    FILE *clear_fp = fopen("log.dat", "wb");
    if (clear_fp == NULL) {
        printf("Could not clear the log.\n");
        return;
    }
    fclose(clear_fp);
    printf("Log cleared.\n");
}

/* Quit from the menu. log_fp is static to this file, so the close has to
 * happen here. fclose() flushes any buffered writes to disk. (Ending the
 * scan loop instead simply returns from main(), which also flushes.)
 */
void close_program(void) {
fclose(log_fp);
exit(0);
}

/* ---- State handlers ----
 * Each one runs once per main-loop iteration (called through the sp[] table
 * at the bottom of this file) and returns the NEXT state.
 */
/* IDLE: nothing to prepare yet, so hand straight off to SCANNING. */
system_state state_idle (void) {
return STATE_SCANNING;
}

/* Called once per input BYTE, not once per UID, so byte_count and the ring
 * buffer carry progress between calls. This mirrors a real UART: bytes
 * trickle in one at a time and the receiver can't block waiting for the
 * rest of the UID. Once 4 bytes are queued they are popped as one tag.
 */
system_state state_scanning(void) {

    int c = scan_input();
    /* Input ended, or 'q' was typed as a test-harness quit key. Checked before
     * the byte reaches the ring buffer, so 'q' can never be part of a UID.
     */
    if (c == EOF|| c == 'q') {
        scan_ended = true;
        return STATE_IDLE;
    }

    /* buffer full: the byte was rejected, so this scan is unusable -> ERROR */
    if (ringBufWrite(&rb, (uint8_t)c) == false) {
        return STATE_ERROR;
    }

    byte_count++;

    /* a full UID has arrived (4 = UID_BYTE): pop it out of the ring buffer */
    if (byte_count == 4) {
        for (int i = 0; i < 4; i++) {
            current_tag.tag_id[i] = ringBufRead(&rb);
        }
        byte_count = 0;
        return STATE_VALIDATING;
    }

    return STATE_SCANNING;
}

/* Decide the outcome of one scan and fill pending_entry. Checked in order:
 * duplicate (same tag again within COOLDOWN_TICKS) -> authorized -> denied.
 * last_tag_id/last_scan_tick are updated only for non-duplicate scans;
 * otherwise a card resting on the reader would keep resetting its own
 * cooldown and it would never expire.
 */
system_state state_validating (void) {
    system_tick ++;
    /* pending_entry is static, so clear the previous scan's flags first */
    pending_entry.statusbyte = 0;

    if ((memcmp(current_tag.tag_id, last_tag_id, UID_BYTE) == 0) && ((system_tick - last_scan_tick) < COOLDOWN_TICKS)) {
        pending_entry.statusbyte |= FLAG_DUPLICATE;
    }

    else if (is_authorized(current_tag.tag_id)){
            pending_entry.statusbyte |= FLAG_GRANTED;
            memcpy(last_tag_id, current_tag.tag_id, UID_BYTE);
            last_scan_tick = system_tick;
    }

    else {
        pending_entry.statusbyte |= FLAG_DENIED;
        memcpy(last_tag_id, current_tag.tag_id, UID_BYTE);
        last_scan_tick = system_tick;
    }

    memcpy(pending_entry.tag_id, current_tag.tag_id, UID_BYTE);
    pending_entry.timestamp = system_tick;

    /* debug output - remove, or gate behind #ifdef DEBUG */
    printf("%d\n", pending_entry.statusbyte);
    return STATE_LOGGING;
}

/* Append the finished entry to the log as raw bytes (fwrite result not
 * checked yet).
 */
system_state state_logging (void) {
    fwrite(&pending_entry, sizeof(pending_entry), 1, log_fp);
    return STATE_COOLDOWN;
}

/* Placeholder: no real hold-off delay yet (no timer/RTC), so this passes
 * straight back to IDLE.
 */
system_state state_cooldown (void) {
    return STATE_IDLE;
}

/* Recovery, not shutdown: a full ring buffer ruins this one scan but the
 * system can carry on. Reset the buffer and byte counter so no stale bytes
 * leak into the next UID.
 */
system_state state_error (void) {
    ringBuf_init(&rb);
    byte_count =  0;
    return STATE_IDLE;
}

/* Dispatch table: function pointers indexed by system_state, so main() can
 * call sp[current_state]() instead of using a switch. The order must match
 * the enum in tags.h.
 */
system_state (*sp[6]) (void) = {state_idle, state_scanning, state_validating, state_logging, state_cooldown, state_error};
