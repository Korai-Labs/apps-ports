#pragma GCC optimize("O3")
#pragma GCC optimize("-funroll-all-loops")

// TODO: Handle back button correctly
// TODO: Add keys to top of the user dictionary, not the bottom

#include <furi.h>
#include <furi_hal.h>
#include "time.h"
#include <gui/gui.h>
#include <gui/elements.h>
#include <input/input.h>
#include <stdlib.h>
#include "mfkey32_icons.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <storage/storage.h>
#include <lib/nfc/helpers/mf_classic_dict.h>
#include <lib/toolbox/args.h>
#include <lib/flipper_format/flipper_format.h>
#include <dolphin/dolphin.h>

#define MF_CLASSIC_DICT_FLIPPER_PATH EXT_PATH("nfc/assets/mf_classic_dict.nfc")
#define MF_CLASSIC_DICT_USER_PATH EXT_PATH("nfc/assets/mf_classic_dict_user.nfc")
#define MF_CLASSIC_NONCE_PATH EXT_PATH("nfc/.mfkey32.log")
#define TAG "Mfkey32"
#define NFC_MF_CLASSIC_KEY_LEN (13)

// MSB_LIMIT: Chunk size (out of 256)
#define MSB_LIMIT 16
#define MIN_RAM 114500
#define LF_POLY_ODD (0x29CE5C)
#define LF_POLY_EVEN (0x870804)
#define CONST_M1_1 (LF_POLY_EVEN << 1 | 1)
#define CONST_M2_1 (LF_POLY_ODD << 1)
#define CONST_M1_2 (LF_POLY_ODD)
#define CONST_M2_2 (LF_POLY_EVEN << 1 | 1)
#define BIT(x, n) ((x) >> (n)&1)
#define BEBIT(x, n) BIT(x, (n) ^ 24)
#define SWAPENDIAN(x) (x = (x >> 8 & 0xff00ff) | (x & 0xff00ff) << 8, x = x >> 16 | x << 16)
//#define SIZEOF(arr) sizeof(arr) / sizeof(*arr)

struct Crypto1State {
    uint32_t odd, even;
};
struct Crypto1Params {
    uint64_t key;
    uint32_t nr0_enc, uid_xor_nt0, uid_xor_nt1, nr1_enc, p64b, ar1_enc;
};
struct Msb {
    int tail;
    uint32_t states[768];
};

typedef enum {
    EventTypeTick,
    EventTypeKey,
} EventType;

typedef struct {
    EventType type;
    InputEvent input;
} PluginEvent;

typedef enum {
    MissingNonces,
    ZeroNonces,
    OutOfMemory,
} MfkeyError;

typedef enum {
    Ready,
    Initializing,
    DictionaryAttack,
    MfkeyAttack,
    Complete,
    Error,
    Help,
} MfkeyState;

typedef struct {
    FuriMutex* mutex;
    MfkeyError err;
    MfkeyState mfkey_state;
    int cracked;
    int unique_cracked;
    int total;
    int dict_count;
    int search;
    bool is_thread_running;
    bool close_thread_please;
    FuriThread* mfkeythread;
} ProgramState;

// TODO: Merge this with Crypto1Params?
typedef struct {
    uint32_t uid; // serial number
    uint32_t nt0; // tag challenge first
    uint32_t nt1; // tag challenge second
    uint32_t nr0_enc; // first encrypted reader challenge
    uint32_t ar0_enc; // first encrypted reader response
    uint32_t nr1_enc; // second encrypted reader challenge
    uint32_t ar1_enc; // second encrypted reader response
} MfClassicNonce;

typedef struct {
    Stream* stream;
    uint32_t total_nonces;
    MfClassicNonce* remaining_nonce_array;
    size_t remaining_nonces;
} MfClassicNonceArray;

struct MfClassicDict {
    Stream* stream;
    uint32_t total_keys;
};

static const uint8_t table[256] = {
    0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3,
    4, 4, 5, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4,
    4, 5, 4, 5, 5, 6, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4,
    5, 3, 4, 4, 5, 4, 5, 5, 6, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5,
    4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2,
    3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5,
    5, 6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4,
    5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 3, 4, 4, 5, 4, 5, 5, 6,
    4, 5, 5, 6, 5, 6, 6, 7, 4, 5, 5, 6, 5, 6, 6, 7, 5, 6, 6, 7, 6, 7, 7, 8};
static const uint8_t lookup1[256] = {
    0, 0,  16, 16, 0,  16, 0,  0,  0, 16, 0,  0,  16, 16, 16, 16, 0, 0,  16, 16, 0,  16, 0,  0,
    0, 16, 0,  0,  16, 16, 16, 16, 0, 0,  16, 16, 0,  16, 0,  0,  0, 16, 0,  0,  16, 16, 16, 16,
    8, 8,  24, 24, 8,  24, 8,  8,  8, 24, 8,  8,  24, 24, 24, 24, 8, 8,  24, 24, 8,  24, 8,  8,
    8, 24, 8,  8,  24, 24, 24, 24, 8, 8,  24, 24, 8,  24, 8,  8,  8, 24, 8,  8,  24, 24, 24, 24,
    0, 0,  16, 16, 0,  16, 0,  0,  0, 16, 0,  0,  16, 16, 16, 16, 0, 0,  16, 16, 0,  16, 0,  0,
    0, 16, 0,  0,  16, 16, 16, 16, 8, 8,  24, 24, 8,  24, 8,  8,  8, 24, 8,  8,  24, 24, 24, 24,
    0, 0,  16, 16, 0,  16, 0,  0,  0, 16, 0,  0,  16, 16, 16, 16, 0, 0,  16, 16, 0,  16, 0,  0,
    0, 16, 0,  0,  16, 16, 16, 16, 8, 8,  24, 24, 8,  24, 8,  8,  8, 24, 8,  8,  24, 24, 24, 24,
    8, 8,  24, 24, 8,  24, 8,  8,  8, 24, 8,  8,  24, 24, 24, 24, 0, 0,  16, 16, 0,  16, 0,  0,
    0, 16, 0,  0,  16, 16, 16, 16, 8, 8,  24, 24, 8,  24, 8,  8,  8, 24, 8,  8,  24, 24, 24, 24,
    8, 8,  24, 24, 8,  24, 8,  8,  8, 24, 8,  8,  24, 24, 24, 24};
static const uint8_t lookup2[256] = {
    0, 0, 4, 4, 0, 4, 0, 0, 0, 4, 0, 0, 4, 4, 4, 4, 0, 0, 4, 4, 0, 4, 0, 0, 0, 4, 0, 0, 4,
    4, 4, 4, 2, 2, 6, 6, 2, 6, 2, 2, 2, 6, 2, 2, 6, 6, 6, 6, 2, 2, 6, 6, 2, 6, 2, 2, 2, 6,
    2, 2, 6, 6, 6, 6, 0, 0, 4, 4, 0, 4, 0, 0, 0, 4, 0, 0, 4, 4, 4, 4, 2, 2, 6, 6, 2, 6, 2,
    2, 2, 6, 2, 2, 6, 6, 6, 6, 0, 0, 4, 4, 0, 4, 0, 0, 0, 4, 0, 0, 4, 4, 4, 4, 0, 0, 4, 4,
    0, 4, 0, 0, 0, 4, 0, 0, 4, 4, 4, 4, 0, 0, 4, 4, 0, 4, 0, 0, 0, 4, 0, 0, 4, 4, 4, 4, 2,
    2, 6, 6, 2, 6, 2, 2, 2, 6, 2, 2, 6, 6, 6, 6, 0, 0, 4, 4, 0, 4, 0, 0, 0, 4, 0, 0, 4, 4,
    4, 4, 0, 0, 4, 4, 0, 4, 0, 0, 0, 4, 0, 0, 4, 4, 4, 4, 2, 2, 6, 6, 2, 6, 2, 2, 2, 6, 2,
    2, 6, 6, 6, 6, 2, 2, 6, 6, 2, 6, 2, 2, 2, 6, 2, 2, 6, 6, 6, 6, 2, 2, 6, 6, 2, 6, 2, 2,
    2, 6, 2, 2, 6, 6, 6, 6, 2, 2, 6, 6, 2, 6, 2, 2, 2, 6, 2, 2, 6, 6, 6, 6};

uint32_t prng_successor(uint32_t x, uint32_t n) {
    SWAPENDIAN(x);
    while(n--) x = x >> 1 | (x >> 16 ^ x >> 18 ^ x >> 19 ^ x >> 21) << 31;
    return SWAPENDIAN(x);
}

static inline int filter(uint32_t const x) {
    uint32_t f;
    f = lookup1[x & 0xff] | lookup2[(x >> 8) & 0xff];
    f |= 0x0d938 >> (x >> 16 & 0xf) & 1;
    return BIT(0xEC57E80A, f);
}

static inline uint8_t evenparity32(uint32_t x) {
    if((table[x & 0xff] + table[(x >> 8) & 0xff] + table[(x >> 16) & 0xff] + table[x >> 24]) % 2 ==
       0) {
        return 0;
    } else {
        return 1;
    }
    //return ((table[x & 0xff] + table[(x >> 8) & 0xff] + table[(x >> 16) & 0xff] + table[x >> 24]) % 2) & 0xFF;
}

static inline void update_contribution(unsigned int data[], int item, int mask1, int mask2) {
    int p = data[item] >> 25;
    p = p << 1 | evenparity32(data[item] & mask1);
    p = p << 1 | evenparity32(data[item] & mask2);
    data[item] = p << 24 | (data[item] & 0xffffff);
}

void crypto1_get_lfsr(struct Crypto1State* state, uint64_t* lfsr) {
    int i;
    for(*lfsr = 0, i = 23; i >= 0; --i) {
        *lfsr = *lfsr << 1 | BIT(state->odd, i ^ 3);
        *lfsr = *lfsr << 1 | BIT(state->even, i ^ 3);
    }
}

static inline uint32_t crypt_word(struct Crypto1State* s) {
    // "in" and "x" are always 0 (last iteration)
    uint32_t res_ret = 0;
    uint32_t feedin, t;
    for(int i = 0; i <= 31; i++) {
        res_ret |= (filter(s->odd) << (24 ^ i));
        feedin = LF_POLY_EVEN & s->even;
        feedin ^= LF_POLY_ODD & s->odd;
        s->even = s->even << 1 | (evenparity32(feedin));
        t = s->odd, s->odd = s->even, s->even = t;
    }
    return res_ret;
}

static inline void crypt_word_noret(struct Crypto1State* s, uint32_t in, int x) {
    uint8_t ret;
    uint32_t feedin, t, next_in;
    for(int i = 0; i <= 31; i++) {
        next_in = BEBIT(in, i);
        ret = filter(s->odd);
        feedin = ret & (!!x);
        feedin ^= LF_POLY_EVEN & s->even;
        feedin ^= LF_POLY_ODD & s->odd;
        feedin ^= !!next_in;
        s->even = s->even << 1 | (evenparity32(feedin));
        t = s->odd, s->odd = s->even, s->even = t;
    }
    return;
}

static inline void rollback_word_noret(struct Crypto1State* s, uint32_t in, int x) {
    uint8_t ret;
    uint32_t feedin, t, next_in;
    for(int i = 31; i >= 0; i--) {
        next_in = BEBIT(in, i);
        s->odd &= 0xffffff;
        t = s->odd, s->odd = s->even, s->even = t;
        ret = filter(s->odd);
        feedin = ret & (!!x);
        feedin ^= s->even & 1;
        feedin ^= LF_POLY_EVEN & (s->even >>= 1);
        feedin ^= LF_POLY_ODD & s->odd;
        feedin ^= !!next_in;
        s->even |= (evenparity32(feedin)) << 23;
    }
    return;
}

int key_already_found_for_nonce(
    uint64_t* keyarray,
    int keyarray_size,
    uint32_t uid_xor_nt1,
    uint32_t nr1_enc,
    uint32_t p64b,
    uint32_t ar1_enc) {
    for(int k = 0; k < keyarray_size; k++) {
        struct Crypto1State temp = {0, 0};

        for(int i = 0; i < 24; i++) {
            (&temp)->odd |= (BIT(keyarray[k], 2 * i + 1) << (i ^ 3));
            (&temp)->even |= (BIT(keyarray[k], 2 * i) << (i ^ 3));
        }

        crypt_word_noret(&temp, uid_xor_nt1, 0);
        crypt_word_noret(&temp, nr1_enc, 1);

        if(ar1_enc == (crypt_word(&temp) ^ p64b)) {
            return 1;
        }
    }
    return 0;
}

int check_state(struct Crypto1State* t, struct Crypto1Params* p) {
    if(!(t->odd | t->even)) return 0;
    rollback_word_noret(t, 0, 0);
    rollback_word_noret(t, p->nr0_enc, 1);
    rollback_word_noret(t, p->uid_xor_nt0, 0);
    struct Crypto1State temp = {t->odd, t->even};
    crypt_word_noret(t, p->uid_xor_nt1, 0);
    crypt_word_noret(t, p->nr1_enc, 1);
    if(p->ar1_enc == (crypt_word(t) ^ p->p64b)) {
        crypto1_get_lfsr(&temp, &(p->key));
        return 1;
    }
    return 0;
}

static inline int state_loop(unsigned int* states_buffer, int xks, int m1, int m2) {
    int states_tail = 0;
    int round = 0, s = 0, xks_bit = 0;

    for(round = 1; round <= 12; round++) {
        xks_bit = BIT(xks, round);

        for(s = 0; s <= states_tail; s++) {
            states_buffer[s] <<= 1;

            if((filter(states_buffer[s]) ^ filter(states_buffer[s] | 1)) != 0) {
                states_buffer[s] |= filter(states_buffer[s]) ^ xks_bit;
                if(round > 4) {
                    update_contribution(states_buffer, s, m1, m2);
                }
            } else if(filter(states_buffer[s]) == xks_bit) {
                // TODO: Refactor
                if(round > 4) {
                    states_buffer[++states_tail] = states_buffer[s + 1];
                    states_buffer[s + 1] = states_buffer[s] | 1;
                    update_contribution(states_buffer, s, m1, m2);
                    s++;
                    update_contribution(states_buffer, s, m1, m2);
                } else {
                    states_buffer[++states_tail] = states_buffer[++s];
                    states_buffer[s] = states_buffer[s - 1] | 1;
                }
            } else {
                states_buffer[s--] = states_buffer[states_tail--];
            }
        }
    }

    return states_tail;
}

int binsearch(unsigned int data[], int start, int stop) {
    int mid, val = data[stop] & 0xff000000;
    while(start != stop) {
        mid = (stop - start) >> 1;
        if((data[start + mid] ^ 0x80000000) > (val ^ 0x80000000))
            stop = start + mid;
        else
            start += mid + 1;
    }
    return start;
}
void quicksort(unsigned int array[], int low, int high) {
    //if (SIZEOF(array) == 0)
    //    return;
    if(low >= high) return;
    int middle = low + (high - low) / 2;
    unsigned int pivot = array[middle];
    int i = low, j = high;
    while(i <= j) {
        while(array[i] < pivot) {
            i++;
        }
        while(array[j] > pivot) {
            j--;
        }
        if(i <= j) { // swap
            int temp = array[i];
            array[i] = array[j];
            array[j] = temp;
            i++;
            j--;
        }
    }
    if(low < j) {
        quicksort(array, low, j);
    }
    if(high > i) {
        quicksort(array, i, high);
    }
}
int extend_table(unsigned int data[], int tbl, int end, int bit, int m1, int m2) {
    for(data[tbl] <<= 1; tbl <= end; data[++tbl] <<= 1) {
        if((filter(data[tbl]) ^ filter(data[tbl] | 1)) != 0) {
            data[tbl] |= filter(data[tbl]) ^ bit;
            update_contribution(data, tbl, m1, m2);
        } else if(filter(data[tbl]) == bit) {
            data[++end] = data[tbl + 1];
            data[tbl + 1] = data[tbl] | 1;
            update_contribution(data, tbl, m1, m2);
            tbl++;
            update_contribution(data, tbl, m1, m2);
        } else {
            data[tbl--] = data[end--];
        }
    }
    return end;
}

int old_recover(
    unsigned int odd[],
    int o_head,
    int o_tail,
    int oks,
    unsigned int even[],
    int e_head,
    int e_tail,
    int eks,
    int rem,
    int s,
    struct Crypto1Params* p,
    int first_run) {
    int o, e, i;
    if(rem == -1) {
        for(e = e_head; e <= e_tail; ++e) {
            even[e] = (even[e] << 1) ^ evenparity32(even[e] & LF_POLY_EVEN);
            for(o = o_head; o <= o_tail; ++o, ++s) {
                struct Crypto1State temp = {0, 0};
                temp.even = odd[o];
                temp.odd = even[e] ^ evenparity32(odd[o] & LF_POLY_ODD);
                if(check_state(&temp, p)) {
                    return -1;
                }
            }
        }
        return s;
    }
    if(first_run == 0) {
        for(i = 0; (i < 4) && (rem-- != 0); i++) {
            oks >>= 1;
            eks >>= 1;
            o_tail = extend_table(
                odd, o_head, o_tail, oks & 1, LF_POLY_EVEN << 1 | 1, LF_POLY_ODD << 1);
            if(o_head > o_tail) return s;
            e_tail =
                extend_table(even, e_head, e_tail, eks & 1, LF_POLY_ODD, LF_POLY_EVEN << 1 | 1);
            if(e_head > e_tail) return s;
        }
    }
    first_run = 0;
    quicksort(odd, o_head, o_tail);
    quicksort(even, e_head, e_tail);
    while(o_tail >= o_head && e_tail >= e_head) {
        if(((odd[o_tail] ^ even[e_tail]) >> 24) == 0) {
            o_tail = binsearch(odd, o_head, o = o_tail);
            e_tail = binsearch(even, e_head, e = e_tail);
            s = old_recover(odd, o_tail--, o, oks, even, e_tail--, e, eks, rem, s, p, first_run);
            if(s == -1) {
                break;
            }
        } else if((odd[o_tail] ^ 0x80000000) > (even[e_tail] ^ 0x80000000)) {
            o_tail = binsearch(odd, o_head, o_tail) - 1;
        } else {
            e_tail = binsearch(even, e_head, e_tail) - 1;
        }
    }
    return s;
}

int calculate_msb_tables(
    int oks,
    int eks,
    int msb_round,
    struct Crypto1Params* p,
    unsigned int* states_buffer,
    struct Msb* odd_msbs,
    struct Msb* even_msbs,
    unsigned int* temp_states_odd,
    unsigned int* temp_states_even) {
    //FURI_LOG_I(TAG, "MSB GO %i", msb_iter); // DEBUG
    unsigned int msb_head = (MSB_LIMIT * msb_round); // msb_iter ranges from 0 to (256/MSB_LIMIT)-1
    unsigned int msb_tail = (MSB_LIMIT * (msb_round + 1));
    int states_tail = 0, tail = 0;
    int i = 0, j = 0, semi_state = 0, found = 0;
    unsigned int msb = 0;
    // TODO: Why is this necessary?
    memset(odd_msbs, 0, MSB_LIMIT * sizeof(struct Msb));
    memset(even_msbs, 0, MSB_LIMIT * sizeof(struct Msb));

    for(semi_state = 1 << 20; semi_state >= 0; semi_state--) {
        //if (main_iter % 2048 == 0) {
        //    FURI_LOG_I(TAG, "On main_iter %i", main_iter); // DEBUG
        //}
        if(filter(semi_state) == (oks & 1)) {
            states_buffer[0] = semi_state;
            states_tail = state_loop(states_buffer, oks, CONST_M1_1, CONST_M2_1);

            for(i = states_tail; i >= 0; i--) {
                msb = states_buffer[i] >> 24;
                if((msb >= msb_head) && (msb < msb_tail)) {
                    found = 0;
                    for(j = 0; j < odd_msbs[msb - msb_head].tail - 1; j++) {
                        if(odd_msbs[msb - msb_head].states[j] == states_buffer[i]) {
                            found = 1;
                            break;
                        }
                    }

                    if(!found) {
                        tail = odd_msbs[msb - msb_head].tail++;
                        odd_msbs[msb - msb_head].states[tail] = states_buffer[i];
                    }
                }
            }
        }

        if(filter(semi_state) == (eks & 1)) {
            states_buffer[0] = semi_state;
            states_tail = state_loop(states_buffer, eks, CONST_M1_2, CONST_M2_2);

            for(i = 0; i <= states_tail; i++) {
                msb = states_buffer[i] >> 24;
                if((msb >= msb_head) && (msb < msb_tail)) {
                    found = 0;

                    for(j = 0; j < even_msbs[msb - msb_head].tail; j++) {
                        if(even_msbs[msb - msb_head].states[j] == states_buffer[i]) {
                            found = 1;
                            break;
                        }
                    }

                    if(!found) {
                        tail = even_msbs[msb - msb_head].tail++;
                        even_msbs[msb - msb_head].states[tail] = states_buffer[i];
                    }
                }
            }
        }
    }

    oks >>= 12;
    eks >>= 12;

    for(i = 0; i < MSB_LIMIT; i++) {
        memcpy(temp_states_odd, odd_msbs[i].states, odd_msbs[i].tail * sizeof(unsigned int));
        memcpy(temp_states_even, even_msbs[i].states, even_msbs[i].tail * sizeof(unsigned int));
        int res = old_recover(
            temp_states_odd,
            0,
            odd_msbs[i].tail,
            oks,
            temp_states_even,
            0,
            even_msbs[i].tail,
            eks,
            3,
            0,
            p,
            1);
        if(res == -1) {
            return 1;
        }
        //odd_msbs[i].tail = 0;
        //even_msbs[i].tail = 0;
    }

    return 0;
}

int recover(struct Crypto1Params* p, int ks2, ProgramState* const program_state) {
    unsigned int* states_buffer = malloc(sizeof(unsigned int) * (2 << 9));
    struct Msb* odd_msbs = (struct Msb*)malloc(MSB_LIMIT * sizeof(struct Msb));
    struct Msb* even_msbs = (struct Msb*)malloc(MSB_LIMIT * sizeof(struct Msb));
    unsigned int* temp_states_odd = malloc(sizeof(unsigned int) * (1280));
    unsigned int* temp_states_even = malloc(sizeof(unsigned int) * (1280));
    int oks = 0, eks = 0;
    int i = 0, msb = 0;
    for(i = 31; i >= 0; i -= 2) {
        oks = oks << 1 | BEBIT(ks2, i);
    }
    for(i = 30; i >= 0; i -= 2) {
        eks = eks << 1 | BEBIT(ks2, i);
    }
    int bench_start = furi_hal_rtc_get_timestamp();
    for(msb = 0; msb <= ((256 / MSB_LIMIT) - 1); msb++) {
        //printf("MSB: %i\n", msb);
        program_state->search = msb;
        if(calculate_msb_tables(
               oks,
               eks,
               msb,
               p,
               states_buffer,
               odd_msbs,
               even_msbs,
               temp_states_odd,
               temp_states_even)) {
            int bench_stop = furi_hal_rtc_get_timestamp();
            FURI_LOG_I(TAG, "Cracked in %i seconds", bench_stop - bench_start);
            free(states_buffer);
            free(odd_msbs);
            free(even_msbs);
            free(temp_states_odd);
            free(temp_states_even);
            return 1;
        }
    }
    return 0;
}

bool napi_mf_classic_dict_check_presence(MfClassicDictType dict_type) {
    Storage* storage = furi_record_open(RECORD_STORAGE);

    bool dict_present = false;
    if(dict_type == MfClassicDictTypeSystem) {
        dict_present = storage_common_stat(storage, MF_CLASSIC_DICT_FLIPPER_PATH, NULL) == FSE_OK;
    } else if(dict_type == MfClassicDictTypeUser) {
        dict_present = storage_common_stat(storage, MF_CLASSIC_DICT_USER_PATH, NULL) == FSE_OK;
    }

    furi_record_close(RECORD_STORAGE);

    return dict_present;
}

MfClassicDict* napi_mf_classic_dict_alloc(MfClassicDictType dict_type) {
    MfClassicDict* dict = malloc(sizeof(MfClassicDict));
    Storage* storage = furi_record_open(RECORD_STORAGE);
    dict->stream = buffered_file_stream_alloc(storage);
    furi_record_close(RECORD_STORAGE);

    bool dict_loaded = false;
    do {
        if(dict_type == MfClassicDictTypeSystem) {
            if(!buffered_file_stream_open(
                   dict->stream,
                   MF_CLASSIC_DICT_FLIPPER_PATH,
                   FSAM_READ_WRITE,
                   FSOM_OPEN_EXISTING)) {
                buffered_file_stream_close(dict->stream);
                break;
            }
        } else if(dict_type == MfClassicDictTypeUser) {
            if(!buffered_file_stream_open(
                   dict->stream, MF_CLASSIC_DICT_USER_PATH, FSAM_READ_WRITE, FSOM_OPEN_ALWAYS)) {
                buffered_file_stream_close(dict->stream);
                break;
            }
        }

        // Check for newline ending
        if(!stream_eof(dict->stream)) {
            if(!stream_seek(dict->stream, -1, StreamOffsetFromEnd)) break;
            uint8_t last_char = 0;
            if(stream_read(dict->stream, &last_char, 1) != 1) break;
            if(last_char != '\n') {
                FURI_LOG_D(TAG, "Adding new line ending");
                if(stream_write_char(dict->stream, '\n') != 1) break;
            }
            if(!stream_rewind(dict->stream)) break;
        }

        // Read total amount of keys
        FuriString* next_line;
        next_line = furi_string_alloc();
        while(true) {
            if(!stream_read_line(dict->stream, next_line)) {
                FURI_LOG_T(TAG, "No keys left in dict");
                break;
            }
            FURI_LOG_T(
                TAG,
                "Read line: %s, len: %zu",
                furi_string_get_cstr(next_line),
                furi_string_size(next_line));
            if(furi_string_get_char(next_line, 0) == '#') continue;
            if(furi_string_size(next_line) != NFC_MF_CLASSIC_KEY_LEN) continue;
            dict->total_keys++;
        }
        furi_string_free(next_line);
        stream_rewind(dict->stream);

        dict_loaded = true;
        FURI_LOG_I(TAG, "Loaded dictionary with %lu keys", dict->total_keys);
    } while(false);

    if(!dict_loaded) {
        buffered_file_stream_close(dict->stream);
        free(dict);
        dict = NULL;
    }

    return dict;
}

bool napi_mf_classic_dict_add_key_str(MfClassicDict* dict, FuriString* key) {
    furi_assert(dict);
    furi_assert(dict->stream);
    FURI_LOG_I(TAG, "Saving key: %s", furi_string_get_cstr(key));

    furi_string_cat_printf(key, "\n");

    bool key_added = false;
    do {
        if(!stream_seek(dict->stream, 0, StreamOffsetFromEnd)) break;
        if(!stream_insert_string(dict->stream, key)) break;
        dict->total_keys++;
        key_added = true;
    } while(false);

    furi_string_left(key, 12);
    return key_added;
}

void napi_mf_classic_dict_free(MfClassicDict* dict) {
    furi_assert(dict);
    furi_assert(dict->stream);

    buffered_file_stream_close(dict->stream);
    stream_free(dict->stream);
    free(dict);
}

static void napi_mf_classic_dict_int_to_str(uint8_t* key_int, FuriString* key_str) {
    furi_string_reset(key_str);
    for(size_t i = 0; i < 6; i++) {
        furi_string_cat_printf(key_str, "%02X", key_int[i]);
    }
}

static void napi_mf_classic_dict_str_to_int(FuriString* key_str, uint64_t* key_int) {
    uint8_t key_byte_tmp;

    *key_int = 0ULL;
    for(uint8_t i = 0; i < 12; i += 2) {
        args_char_to_hex(
            furi_string_get_char(key_str, i), furi_string_get_char(key_str, i + 1), &key_byte_tmp);
        *key_int |= (uint64_t)key_byte_tmp << (8 * (5 - i / 2));
    }
}

uint32_t napi_mf_classic_dict_get_total_keys(MfClassicDict* dict) {
    furi_assert(dict);

    return dict->total_keys;
}

bool napi_mf_classic_dict_rewind(MfClassicDict* dict) {
    furi_assert(dict);
    furi_assert(dict->stream);

    return stream_rewind(dict->stream);
}

bool napi_mf_classic_dict_get_next_key_str(MfClassicDict* dict, FuriString* key) {
    furi_assert(dict);
    furi_assert(dict->stream);

    bool key_read = false;
    furi_string_reset(key);
    while(!key_read) {
        if(!stream_read_line(dict->stream, key)) break;
        if(furi_string_get_char(key, 0) == '#') continue;
        if(furi_string_size(key) != NFC_MF_CLASSIC_KEY_LEN) continue;
        furi_string_left(key, 12);
        key_read = true;
    }

    return key_read;
}

bool napi_mf_classic_dict_get_next_key(MfClassicDict* dict, uint64_t* key) {
    furi_assert(dict);
    furi_assert(dict->stream);

    FuriString* temp_key;
    temp_key = furi_string_alloc();
    bool key_read = napi_mf_classic_dict_get_next_key_str(dict, temp_key);
    if(key_read) {
        napi_mf_classic_dict_str_to_int(temp_key, key);
    }
    furi_string_free(temp_key);
    return key_read;
}

bool napi_mf_classic_dict_is_key_present_str(MfClassicDict* dict, FuriString* key) {
    furi_assert(dict);
    furi_assert(dict->stream);

    FuriString* next_line;
    next_line = furi_string_alloc();

    bool key_found = false;
    stream_rewind(dict->stream);
    while(!key_found) { //-V654
        if(!stream_read_line(dict->stream, next_line)) break;
        if(furi_string_get_char(next_line, 0) == '#') continue;
        if(furi_string_size(next_line) != NFC_MF_CLASSIC_KEY_LEN) continue;
        furi_string_left(next_line, 12);
        if(!furi_string_equal(key, next_line)) continue;
        key_found = true;
    }

    furi_string_free(next_line);
    return key_found;
}

bool napi_mf_classic_dict_is_key_present(MfClassicDict* dict, uint8_t* key) {
    FuriString* temp_key;

    temp_key = furi_string_alloc();
    napi_mf_classic_dict_int_to_str(key, temp_key);
    bool key_found = napi_mf_classic_dict_is_key_present_str(dict, temp_key);
    furi_string_free(temp_key);
    return key_found;
}

bool napi_key_already_found_for_nonce(
    MfClassicDict* dict,
    uint32_t uid_xor_nt1,
    uint32_t nr1_enc,
    uint32_t p64b,
    uint32_t ar1_enc) {
    bool found = false;
    uint64_t k = 0;
    napi_mf_classic_dict_rewind(dict);
    while(napi_mf_classic_dict_get_next_key(dict, &k)) {
        struct Crypto1State temp = {0, 0};
        int i;
        for(i = 0; i < 24; i++) {
            (&temp)->odd |= (BIT(k, 2 * i + 1) << (i ^ 3));
            (&temp)->even |= (BIT(k, 2 * i) << (i ^ 3));
        }
        crypt_word_noret(&temp, uid_xor_nt1, 0);
        crypt_word_noret(&temp, nr1_enc, 1);
        if(ar1_enc == (crypt_word(&temp) ^ p64b)) {
            found = true;
            break;
        }
    }
    return found;
}

bool napi_mf_classic_nonces_check_presence() {
    Storage* storage = furi_record_open(RECORD_STORAGE);

    bool nonces_present = storage_common_stat(storage, MF_CLASSIC_NONCE_PATH, NULL) == FSE_OK;

    furi_record_close(RECORD_STORAGE);

    return nonces_present;
}

MfClassicNonceArray* napi_mf_classic_nonce_array_alloc(
    MfClassicDict* system_dict,
    bool system_dict_exists,
    MfClassicDict* user_dict,
    bool user_dict_exists,
    ProgramState* const program_state) {
    MfClassicNonceArray* nonce_array = malloc(sizeof(MfClassicNonceArray));
    MfClassicNonce* remaining_nonce_array_init = malloc(sizeof(MfClassicNonce) * 1);
    nonce_array->remaining_nonce_array = remaining_nonce_array_init;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    nonce_array->stream = buffered_file_stream_alloc(storage);
    furi_record_close(RECORD_STORAGE);

    bool array_loaded = false;
    do {
        // https://github.com/flipperdevices/flipperzero-firmware/blob/5134f44c09d39344a8747655c0d59864bb574b96/applications/services/storage/filesystem_api_defines.h#L8-L22
        if(!buffered_file_stream_open(
               nonce_array->stream, MF_CLASSIC_NONCE_PATH, FSAM_READ_WRITE, FSOM_OPEN_EXISTING)) {
            buffered_file_stream_close(nonce_array->stream);
            break;
        }

        // Check for newline ending
        if(!stream_eof(nonce_array->stream)) {
            if(!stream_seek(nonce_array->stream, -1, StreamOffsetFromEnd)) break;
            uint8_t last_char = 0;
            if(stream_read(nonce_array->stream, &last_char, 1) != 1) break;
            if(last_char != '\n') {
                FURI_LOG_D(TAG, "Adding new line ending");
                if(stream_write_char(nonce_array->stream, '\n') != 1) break;
            }
            if(!stream_rewind(nonce_array->stream)) break;
        }

        // Read total amount of nonces
        FuriString* next_line;
        next_line = furi_string_alloc();
        while(true) {
            if(!stream_read_line(nonce_array->stream, next_line)) {
                FURI_LOG_T(TAG, "No nonces left");
                break;
            }
            FURI_LOG_T(
                TAG,
                "Read line: %s, len: %zu",
                furi_string_get_cstr(next_line),
                furi_string_size(next_line));
            if(!furi_string_start_with_str(next_line, "Sec")) continue;
            const char* next_line_cstr = furi_string_get_cstr(next_line);
            MfClassicNonce res = {0};
            char token[20];
            int i = 0;
            const char* ptr = next_line_cstr;
            while(sscanf(ptr, "%s", token) == 1) {
                switch(i) {
                case 5:
                    sscanf(token, "%lx", &res.uid);
                    break;
                case 7:
                    sscanf(token, "%lx", &res.nt0);
                    break;
                case 9:
                    sscanf(token, "%lx", &res.nr0_enc);
                    break;
                case 11:
                    sscanf(token, "%lx", &res.ar0_enc);
                    break;
                case 13:
                    sscanf(token, "%lx", &res.nt1);
                    break;
                case 15:
                    sscanf(token, "%lx", &res.nr1_enc);
                    break;
                case 17:
                    sscanf(token, "%lx", &res.ar1_enc);
                    break;
                default:
                    break; // Do nothing
                }
                i++;
                ptr = strchr(ptr, ' ');
                if(!ptr) {
                    break;
                }
                ptr++;
            }
            (program_state->total)++;
            uint32_t p64b = prng_successor(res.nt1, 64);
            if((system_dict_exists &&
                napi_key_already_found_for_nonce(
                    system_dict, res.uid ^ res.nt1, res.nr1_enc, p64b, res.ar1_enc)) ||
               (user_dict_exists &&
                napi_key_already_found_for_nonce(
                    user_dict, res.uid ^ res.nt1, res.nr1_enc, p64b, res.ar1_enc))) {
                (program_state->cracked)++;
                continue;
            }
            FURI_LOG_I(TAG, "No key found for %lx %lx", res.uid, res.ar1_enc);
            // TODO: Refactor
            nonce_array->remaining_nonce_array = realloc(
                nonce_array->remaining_nonce_array,
                sizeof(MfClassicNonce) * ((nonce_array->remaining_nonces) + 1));
            nonce_array->remaining_nonces++;
            nonce_array->remaining_nonce_array[(nonce_array->remaining_nonces) - 1] = res;
            nonce_array->total_nonces++;
        }
        furi_string_free(next_line);
        stream_rewind(nonce_array->stream);

        array_loaded = true;
        FURI_LOG_I(TAG, "Loaded %lu nonces", nonce_array->total_nonces);
    } while(false);

    if(!array_loaded) {
        buffered_file_stream_close(nonce_array->stream);
        free(nonce_array);
        nonce_array = NULL;
    }

    return nonce_array;
}

void napi_mf_classic_nonce_array_free(MfClassicNonceArray* nonce_array) {
    furi_assert(nonce_array);
    furi_assert(nonce_array->stream);

    buffered_file_stream_close(nonce_array->stream);
    stream_free(nonce_array->stream);
    free(nonce_array);
}

static void finished_beep() {
    // Beep to indicate completion if the speaker is available
    if(furi_hal_speaker_acquire(1000)) { // Wait up to a second for the speaker
        float freq = 3000;
        float volume = 1.0f; // 100% volume
        furi_hal_speaker_start(freq, volume);
        furi_delay_ms(75);
        furi_hal_speaker_stop();
        furi_hal_speaker_release();
    }
}

void mfkey32(ProgramState* const program_state) {
    uint64_t found_key; // recovered key
    size_t keyarray_size = 0;
    uint64_t* keyarray = malloc(sizeof(uint64_t) * 1);
    uint32_t i = 0;
    // Check for nonces
    if(!napi_mf_classic_nonces_check_presence()) {
        program_state->err = MissingNonces;
        program_state->mfkey_state = Error;
        return;
    }
    // Read dictionaries (optional)
    MfClassicDict* system_dict = {0};
    bool system_dict_exists = napi_mf_classic_dict_check_presence(MfClassicDictTypeSystem);
    MfClassicDict* user_dict = {0};
    bool user_dict_exists = napi_mf_classic_dict_check_presence(MfClassicDictTypeUser);
    uint32_t total_dict_keys = 0;
    if(system_dict_exists) {
        system_dict = napi_mf_classic_dict_alloc(MfClassicDictTypeSystem);
        total_dict_keys += napi_mf_classic_dict_get_total_keys(system_dict);
    }
    user_dict = napi_mf_classic_dict_alloc(MfClassicDictTypeUser);
    if(user_dict_exists) {
        total_dict_keys += napi_mf_classic_dict_get_total_keys(user_dict);
    }
    user_dict_exists = true;
    program_state->dict_count = total_dict_keys;
    program_state->mfkey_state = DictionaryAttack;
    // Read nonces
    MfClassicNonceArray* nonce_arr;
    nonce_arr = napi_mf_classic_nonce_array_alloc(
        system_dict, system_dict_exists, user_dict, user_dict_exists, program_state);
    if(system_dict_exists) {
        napi_mf_classic_dict_free(system_dict);
    }
    if(nonce_arr->total_nonces == 0) {
        // Nothing to crack
        program_state->err = ZeroNonces;
        program_state->mfkey_state = Error;
        napi_mf_classic_nonce_array_free(nonce_arr);
        napi_mf_classic_dict_free(user_dict);
        free(keyarray);
        return;
    }
    if(memmgr_get_free_heap() < MIN_RAM) {
        // Insufficient RAM
        program_state->err = OutOfMemory;
        program_state->mfkey_state = Error;
        napi_mf_classic_nonce_array_free(nonce_arr);
        napi_mf_classic_dict_free(user_dict);
        free(keyarray);
        return;
    }
    program_state->mfkey_state = MfkeyAttack;
    for(i = 0; i < nonce_arr->total_nonces; i++) {
        MfClassicNonce next_nonce = nonce_arr->remaining_nonce_array[i];
        uint32_t p64 = prng_successor(next_nonce.nt0, 64);
        uint32_t p64b = prng_successor(next_nonce.nt1, 64);
        if(key_already_found_for_nonce(
               keyarray,
               keyarray_size,
               next_nonce.uid ^ next_nonce.nt1,
               next_nonce.nr1_enc,
               p64b,
               next_nonce.ar1_enc)) {
            nonce_arr->remaining_nonces--;
            (program_state->cracked)++;
            continue;
        }
        FURI_LOG_I(TAG, "Cracking %lx %lx", next_nonce.uid, next_nonce.ar1_enc);
        struct Crypto1Params p = {
            0,
            next_nonce.nr0_enc,
            next_nonce.uid ^ next_nonce.nt0,
            next_nonce.uid ^ next_nonce.nt1,
            next_nonce.nr1_enc,
            p64b,
            next_nonce.ar1_enc};
        if(recover(&p, next_nonce.ar0_enc ^ p64, program_state) == 0) {
            // No key found in recover()
            continue;
        }
        found_key = p.key;
        bool already_found = false;
        for(i = 0; i < keyarray_size; i++) {
            if(keyarray[i] == found_key) {
                already_found = true;
                break;
            }
        }
        if(already_found == false) {
            // New key
            keyarray = realloc(keyarray, sizeof(uint64_t) * (keyarray_size + 1));
            keyarray_size += 1;
            keyarray[keyarray_size - 1] = found_key;
            (program_state->cracked)++;
            (program_state->unique_cracked)++;
        }
    }
    // TODO: Update display to show all keys were found
    // TODO: Prepend found key(s) to user dictionary file
    //FURI_LOG_I(TAG, "Unique keys found:");
    for(i = 0; i < keyarray_size; i++) {
        //FURI_LOG_I(TAG, "%012" PRIx64, keyarray[i]);
        FuriString* temp_key = furi_string_alloc();
        furi_string_cat_printf(temp_key, "%012" PRIX64, keyarray[i]);
        napi_mf_classic_dict_add_key_str(user_dict, temp_key);
        furi_string_free(temp_key);
    }
    napi_mf_classic_nonce_array_free(nonce_arr);
    if(user_dict_exists) {
        napi_mf_classic_dict_free(user_dict);
    }
    free(keyarray);
    //FURI_LOG_I(TAG, "mfkey32 function completed normally"); // DEBUG
    program_state->mfkey_state = Complete;
    finished_beep();
    return;
}

// Screen is 128x64 px
static void render_callback(Canvas* const canvas, void* ctx) {
    furi_assert(ctx);
    const ProgramState* program_state = ctx;
    furi_mutex_acquire(program_state->mutex, FuriWaitForever);
    char draw_str[32] = {};
    canvas_clear(canvas);
    canvas_draw_frame(canvas, 0, 0, 128, 64);
    canvas_draw_frame(canvas, 0, 15, 128, 64);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 5, 4, AlignLeft, AlignTop, "Mfkey32");
    canvas_draw_icon(canvas, 114, 4, &I_mfkey);
    if(program_state->is_thread_running && program_state->mfkey_state == MfkeyAttack) {
        float progress = (float)program_state->cracked / (float)program_state->total;
        elements_progress_bar_with_text(canvas, 5, 18, 118, progress, draw_str);
        canvas_set_font(canvas, FontSecondary);
        snprintf(
            draw_str,
            sizeof(draw_str),
            "Keys found: %d/%d (in prog.)",
            program_state->cracked,
            program_state->total);
        canvas_draw_str_aligned(canvas, 5, 31, AlignLeft, AlignTop, draw_str);
        snprintf(
            draw_str, sizeof(draw_str), "Search: %d/%d", program_state->search, 256 / MSB_LIMIT);
        canvas_draw_str_aligned(canvas, 26, 41, AlignLeft, AlignTop, draw_str);
    } else if(program_state->is_thread_running && program_state->mfkey_state == DictionaryAttack) {
        elements_progress_bar_with_text(canvas, 5, 18, 118, 0, draw_str);
        canvas_set_font(canvas, FontSecondary);
        snprintf(
            draw_str, sizeof(draw_str), "Dict solves: %d (in progress)", program_state->cracked);
        canvas_draw_str_aligned(canvas, 10, 31, AlignLeft, AlignTop, draw_str);
        snprintf(draw_str, sizeof(draw_str), "Keys in dict: %d", program_state->dict_count);
        canvas_draw_str_aligned(canvas, 26, 41, AlignLeft, AlignTop, draw_str);
    } else if(program_state->mfkey_state == Complete) {
        // TODO: Scrollable list view to see cracked keys if user presses down
        elements_progress_bar_with_text(canvas, 5, 18, 118, 1, draw_str);
        canvas_set_font(canvas, FontSecondary);
        snprintf(draw_str, sizeof(draw_str), "Complete");
        canvas_draw_str_aligned(canvas, 40, 31, AlignLeft, AlignTop, draw_str);
        snprintf(
            draw_str,
            sizeof(draw_str),
            "Keys added to user dict: %d",
            program_state->unique_cracked);
        canvas_draw_str_aligned(canvas, 10, 41, AlignLeft, AlignTop, draw_str);
    } else if(program_state->mfkey_state == Ready) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 50, 30, AlignLeft, AlignTop, "Ready");
        elements_button_center(canvas, "Start");
        elements_button_right(canvas, "Help");
    } else if(program_state->mfkey_state == Help) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 7, 20, AlignLeft, AlignTop, "Collect nonces using");
        canvas_draw_str_aligned(canvas, 7, 30, AlignLeft, AlignTop, "Detect Reader.");
        canvas_draw_str_aligned(canvas, 7, 40, AlignLeft, AlignTop, "Developers: noproto, AG");
        canvas_draw_str_aligned(canvas, 7, 50, AlignLeft, AlignTop, "Thanks: bettse");
    } else if(program_state->mfkey_state == Error) {
        canvas_draw_str_aligned(canvas, 50, 25, AlignLeft, AlignTop, "Error");
        canvas_set_font(canvas, FontSecondary);
        if(program_state->err == MissingNonces) {
            canvas_draw_str_aligned(canvas, 25, 36, AlignLeft, AlignTop, "No nonces found");
        } else if(program_state->err == ZeroNonces) {
            canvas_draw_str_aligned(canvas, 25, 36, AlignLeft, AlignTop, "No nonces to crack");
        } else if(program_state->err == OutOfMemory) {
            canvas_draw_str_aligned(canvas, 25, 36, AlignLeft, AlignTop, "Insufficient memory");
        } else {
            // Unhandled error
        }
    } else {
        // Unhandled program state
    }
    furi_mutex_release(program_state->mutex);
}

static void input_callback(InputEvent* input_event, FuriMessageQueue* event_queue) {
    furi_assert(event_queue);

    PluginEvent event = {.type = EventTypeKey, .input = *input_event};
    furi_message_queue_put(event_queue, &event, FuriWaitForever);
}

static void mfkey32_state_init(ProgramState* const program_state) {
    program_state->is_thread_running = false;
    program_state->mfkey_state = Ready;
    program_state->cracked = 0;
    program_state->unique_cracked = 0;
    program_state->total = 0;
    program_state->dict_count = 0;
}

// Entrypoint for worker thread
static int32_t mfkey32_worker_thread(void* ctx) {
    ProgramState* program_state = ctx;
    program_state->is_thread_running = true;
    program_state->mfkey_state = Initializing;
    //FURI_LOG_I(TAG, "Hello from the mfkey32 worker thread"); // DEBUG
    mfkey32(program_state);
    program_state->is_thread_running = false;
    return 0;
}

void start_mfkey32_thread(ProgramState* program_state) {
    if(!program_state->is_thread_running) {
        furi_thread_start(program_state->mfkeythread);
    }
}

int32_t mfkey32_main() {
    FuriMessageQueue* event_queue = furi_message_queue_alloc(8, sizeof(PluginEvent));

    DOLPHIN_DEED(DolphinDeedPluginStart);
    ProgramState* program_state = malloc(sizeof(ProgramState));

    mfkey32_state_init(program_state);

    program_state->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    if(!program_state->mutex) {
        FURI_LOG_E(TAG, "cannot create mutex\r\n");
        free(program_state);
        return 255;
    }

    // Set system callbacks
    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, render_callback, program_state);
    view_port_input_callback_set(view_port, input_callback, event_queue);

    // Open GUI and register view_port
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    program_state->mfkeythread = furi_thread_alloc();
    furi_thread_set_name(program_state->mfkeythread, "Mfkey32 Worker");
    furi_thread_set_stack_size(program_state->mfkeythread, 2048);
    furi_thread_set_context(program_state->mfkeythread, program_state);
    furi_thread_set_callback(program_state->mfkeythread, mfkey32_worker_thread);

    PluginEvent event;
    for(bool main_loop = true; main_loop;) {
        FuriStatus event_status = furi_message_queue_get(event_queue, &event, 100);

        furi_mutex_acquire(program_state->mutex, FuriWaitForever);

        if(event_status == FuriStatusOk) {
            // press events
            if(event.type == EventTypeKey) {
                if(event.input.type == InputTypePress) {
                    switch(event.input.key) {
                    case InputKeyUp:
                        break;
                    case InputKeyDown:
                        break;
                    case InputKeyRight:
                        if(!program_state->is_thread_running &&
                           program_state->mfkey_state == Ready) {
                            program_state->mfkey_state = Help;
                            view_port_update(view_port);
                        }
                        break;
                    case InputKeyLeft:
                        break;
                    case InputKeyOk:
                        if(!program_state->is_thread_running &&
                           program_state->mfkey_state == Ready) {
                            start_mfkey32_thread(program_state);
                            view_port_update(view_port);
                        }
                        break;
                    case InputKeyBack:
                        if(!program_state->is_thread_running &&
                           program_state->mfkey_state == Help) {
                            program_state->mfkey_state = Ready;
                            view_port_update(view_port);
                        } else {
                            program_state->close_thread_please = true;
                            if(program_state->is_thread_running && program_state->mfkeythread) {
                                // Wait until thread is finished
                                furi_thread_join(program_state->mfkeythread);
                            }
                            program_state->close_thread_please = false;
                            main_loop = false;
                        }
                        break;
                    default:
                        break;
                    }
                }
            }
        }

        view_port_update(view_port);
        furi_mutex_release(program_state->mutex);
    }

    furi_thread_free(program_state->mfkeythread);
    view_port_enabled_set(view_port, false);
    gui_remove_view_port(gui, view_port);
    furi_record_close("gui");
    view_port_free(view_port);
    furi_message_queue_free(event_queue);
    furi_mutex_free(program_state->mutex);
    free(program_state);

    return 0;
}
