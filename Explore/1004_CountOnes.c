/*
 * File: count_set_bits_all_approaches.c
 * Description: Count number of set bits (1's) using 7 different approaches
 *
 * Approach 1: Brian Kernighan's Algorithm    - O(set bits)
 * Approach 2: Right Shift and Mask           - O(total bits)
 * Approach 3: Lookup Table                   - O(1)
 * Approach 4: Recursion                      - O(total bits)
 * Approach 5: Divide and Conquer (Hamming)   - O(1)
 * Approach 6: Modulo 2 (No bitwise)          - O(total bits)
 * Approach 7: String Conversion              - O(total bits)
 *
 * Sample Output 1:
 *   Enter a number: 13
 *   Approach 1 (Kernighan)       : 3
 *   Approach 2 (Right Shift)     : 3
 *   Approach 3 (Lookup Table)    : 3
 *   Approach 4 (Recursive)       : 3
 *   Approach 5 (Divide Conquer)  : 3
 *   Approach 6 (Modulo)          : 3
 *   Approach 7 (String Convert)  : 3
 *
 * Sample Output 2:
 *   Enter a number: 255
 *   Approach 1 (Kernighan)       : 8
 *   Approach 2 (Right Shift)     : 8
 *   Approach 3 (Lookup Table)    : 8
 *   Approach 4 (Recursive)       : 8
 *   Approach 5 (Divide Conquer)  : 8
 *   Approach 6 (Modulo)          : 8
 *   Approach 7 (String Convert)  : 8
 */

#include <stdio.h>
#include <stdint.h>

/*------------------------------------------------------------
 * Approach 1: Brian Kernighan's Algorithm
 * Logic: n & (n-1) clears the lowest set bit in each iteration
 * Time Complexity: O(number of set bits)
 *------------------------------------------------------------*/
int countBits_kernighan(int n) {
    int count = 0;
    while (n) {
        n = n & (n - 1);  /* clear the lowest set bit */
        count++;
    }
    return count;
}

/*------------------------------------------------------------
 * Approach 2: Right Shift and Mask
 * Logic: Check last bit using (n & 1), then right shift by 1
 * Time Complexity: O(total number of bits)
 *------------------------------------------------------------*/
int countBits_rightShift(int n) {
    int count = 0;
    while (n) {
        count += n & 1;  /* add 1 if last bit is set */
        n >>= 1;         /* shift right to check next bit */
    }
    return count;
}

/*------------------------------------------------------------
 * Approach 3: Lookup Table
 * Logic: Precomputed table for 0-255, split 32-bit into 4 bytes
 * Time Complexity: O(1) - preferred in embedded systems
 *------------------------------------------------------------*/
static const uint8_t lookup_table[256] = {
    0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4,
    1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5,
    1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5,
    2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,
    1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5,
    2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,
    2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,
    3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,
    1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5,
    2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,
    2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,
    3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,
    2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,
    3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,
    3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,
    4,5,5,6,5,6,6,7,5,6,6,7,6,7,7,8
};

int countBits_lookupTable(uint32_t n) {
    return lookup_table[n & 0xFF] +
           lookup_table[(n >> 8) & 0xFF] +
           lookup_table[(n >> 16) & 0xFF] +
           lookup_table[(n >> 24) & 0xFF];
}

/*------------------------------------------------------------
 * Approach 4: Recursion
 * Logic: Check last bit (n & 1), recurse on remaining (n >> 1)
 * Time Complexity: O(total number of bits)
 *------------------------------------------------------------*/
int countBits_recursive(int n) {
    if (n == 0)
        return 0;
    return (n & 1) + countBits_recursive(n >> 1);
}

/*------------------------------------------------------------
 * Approach 5: Divide and Conquer (Hamming Weight)
 * Logic: Parallel bit counting using magic constants
 * Time Complexity: O(1) - how __builtin_popcount() works
 *------------------------------------------------------------*/
int countBits_divideConquer(uint32_t n) {
    return (((n - ((n >> 1) & 0x55555555)) & 0x33333333) + (((n - ((n >> 1) & 0x55555555)) >> 2) & 0x33333333) + ((((n - ((n >> 1) & 0x55555555)) & 0x33333333) + (((n - ((n >> 1) & 0x55555555)) >> 2) & 0x33333333)) >> 4) & 0x0F0F0F0F) * 0x01010101 >> 24;
    /* pairs -> nibbles -> bytes -> sum all in one statement */

    /*
        n = n - ((n >> 1) & 0x55555555);                   /* count bits in pairs //
        n = (n & 0x33333333) + ((n >> 2) & 0x33333333);    // count bits in nibbles //
        n = (n + (n >> 4)) & 0x0F0F0F0F;                   // count bits in bytes //
        return (n * 0x01010101) >> 24;                     // sum all bytes //
    
    */
}

/*------------------------------------------------------------
 * Approach 6: Modulo 2 (No bitwise operators)
 * Logic: n % 2 gives last bit, n / 2 removes last bit
 * Time Complexity: O(total number of bits)
 *------------------------------------------------------------*/
int countBits_modulo(int n) {
    int count = 0;
    while (n) {
        count += n % 2;  /* last bit: 1 if odd, 0 if even */
        n = n / 2;       /* equivalent to right shift by 1 */
    }
    return count;
}

/*------------------------------------------------------------
 * Approach 7: String Conversion
 * Logic: Convert to binary string, then count '1' characters
 * Time Complexity: O(total number of bits)
 *------------------------------------------------------------*/
int countBits_stringConvert(int n) {
    char binary[33];
    int i = 0, count = 0;

    if (n == 0) return 0;

    while (n) {
        binary[i++] = (n % 2) + '0';
        n = n / 2;
    }

    for (int j = 0; j < i; j++) {
        if (binary[j] == '1')
            count++;
    }
    return count;
}

/*------------------------------------------------------------
 * Main: Run all 7 approaches and display results
 *------------------------------------------------------------*/
int main(void) {
    int n;
    printf("Enter a number: ");
    scanf("%d", &n);

    printf("Approach 1 (Kernighan)       : %d\n", countBits_kernighan(n));
    printf("Approach 2 (Right Shift)     : %d\n", countBits_rightShift(n));
    printf("Approach 3 (Lookup Table)    : %d\n", countBits_lookupTable((uint32_t)n));
    printf("Approach 4 (Recursive)       : %d\n", countBits_recursive(n));
    printf("Approach 5 (Divide Conquer)  : %d\n", countBits_divideConquer((uint32_t)n));
    printf("Approach 6 (Modulo)          : %d\n", countBits_modulo(n));
    printf("Approach 7 (String Convert)  : %d\n", countBits_stringConvert(n));

    return 0;
}

/*
 * ============================================================
 * EMBEDDED PERFORMANCE COMPARISON (Fastest to Slowest)
 * ============================================================
 *
 * Rank | Approach                  | Time         | Why
 * -----|---------------------------|--------------|--------------------------------------------
 *  1   | 5 - Divide & Conquer      | O(1)         | Fixed 5 instructions, no branches, no memory access
 *  2   | 3 - Lookup Table           | O(1)         | 4 table lookups + 3 additions, but needs 256 bytes RAM
 *  3   | 1 - Kernighan's           | O(set bits)  | Loops only for set bits, minimal instructions per loop
 *  4   | 2 - Right Shift           | O(total bits)| Loops through all 32 bits
 *  5   | 6 - Modulo                | O(total bits)| Division is expensive on embedded (no HW divider)
 *  6   | 4 - Recursive             | O(total bits)| Stack overhead per call, bad for limited stack
 *  7   | 7 - String Convert        | O(total bits)| Two passes + array storage, worst for embedded
 *
 * BEST CHOICE BASED ON CONSTRAINT:
 *
 * - Speed critical (Qualcomm DSP, ARM Cortex-M with fast multiply):
 *     Approach 5 (Divide & Conquer) - no branches, no memory dependency,
 *     fixed cycle count. This is what __builtin_popcount() compiles to.
 *
 * - Speed critical but no fast multiply:
 *     Approach 3 (Lookup Table) - O(1) but trades 256 bytes of flash/ROM for speed.
 *
 * - Memory constrained (small MCU, tight flash):
 *     Approach 1 (Kernighan's) - smallest code size, no extra memory,
 *     still very fast for sparse bits.
 *
 * AVOID ON EMBEDDED:
 *
 * - Approach 4 (Recursion)       - stack overflow risk on limited stack
 * - Approach 6 (Modulo)          - division is 10-30x slower than bitwise on most MCUs
 * - Approach 7 (String Convert)  - wastes RAM + two passes
 *
 * NOTE: In most Qualcomm/ARM codebases, you'll see either
 *       __builtin_popcount() (maps to Approach 5 or HW POPCNT instruction)
 *       or Approach 3 (lookup table) for guaranteed constant time.
 * ============================================================
 */
