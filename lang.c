#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/mman.h>

typedef unsigned char u8;
typedef unsigned int u32;

// Store information about current line and column and previous characters for better error messages.
u8 chHistory[128];
size_t chHistoryPos = 0;
int chLine = 1;
int chColumn = 0;
int chHistoryShowOnError = 0;

void fail(const char* format, ...) {
    fprintf(stderr, "ERROR: ");
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, "\n");
    if(chHistoryShowOnError) {
        fprintf(stderr, "Source line %d, column %d; previously read source:\n", chLine, chColumn);
        fprintf(stderr, "------------------------------------------------------------\n");

        for(size_t i = chHistoryPos; i < sizeof(chHistory); ++i) {
            if(chHistory[i] != 0) {
                fputc(chHistory[i], stderr);
            }
        }
        for(size_t i = 0; i < chHistoryPos; ++i) {
            if(chHistory[i] != 0) {
                fputc(chHistory[i], stderr);
            }
        }

        fprintf(stderr, "\n------------------------------------------------------------\n");
    }
    exit(1);
}

FILE* src;

// Character read from src using readRawCh or readCh. readRawCh reads the source character to ch
// or 0 if all characters have been read (0 is not allowed in the source; reading more characters
// after the end is an error). readCh calls readRawCh and repeats the calls until the read character
// is not a space or newline.
u8 ch = '?';

void readRawCh() {
    if(ch == 0) {
        fail("Read past the end of the source file.");
    }
    int val = fgetc(src);
    if(val == EOF) {
        ch = 0;
        if(ferror(src)) {
            fail("Reading the source file failed.");
        }
    } else {
        ch = (u8)val;
        chHistory[chHistoryPos++] = ch;
        if(chHistoryPos == sizeof(chHistory)) {
            chHistoryPos = 0;
        }
        if(val == '\n') {
            ++chLine;
            chColumn = 0;
        } else {
            ++chColumn;
        }
        chHistoryShowOnError = 1;
    }
}
void readCh() {
    do {
        readRawCh();
    } while(ch == ' ' || ch == '\n');
}

const size_t memSize = (size_t)1 << 22;
u8* memStart;
u8* memEnd;
u8* memPos;

void initMem() {
    memStart = mmap(NULL, memSize, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if(memStart == MAP_FAILED) {
        fail("Reserving an executable memory block of %zu bytes failed.", memSize);
    }

    memEnd = memStart + memSize;
    memPos = memStart;

    // Initialize memory with randomness to help catch bugs early.
    FILE* rng = fopen("/dev/urandom", "rb");
    if(rng == NULL || fread(memStart, 1, memSize, rng) != memSize || fclose(rng) != 0) {
        fail("Initializing memory from /dev/urandom failed.");
    }
}

void memWritePtr(void* ptr) {
    memcpy(memPos, &ptr, 4);
    memPos += 4;
}

void memWriteU32(u32 val) {
    memcpy(memPos, &val, 4);
    memPos += 4;
}

u8* compile(u8* outString) {
    // Write program code starting from memPos, which will be the entry point to the program.
    u8* entryPoint = memPos;

    // pushad: Save all registers to stack.
    *memPos++ = 0x60;

    // mov ebp, esp: Save the original stack pointer to ebp.
    *memPos++ = 0x89;
    *memPos++ = 0xE5;

    // mov esp, 'memEnd': Move stack pointer to the end of the memory block.
    *memPos++ = 0xBC;
    memWritePtr(memEnd);

    // mov eax, 'outString': Initialize eax to point to the beginning of the output string.
    *memPos++ = 0xb8;
    memWritePtr(outString);

    while(1) {
        readCh();
        if(ch == 0) {
            break;
        }

        // mov byte [eax], 'ch': Write character to the current write position eax in the output string.
        *memPos++ = 0xc6;
        *memPos++ = 0x00;
        *memPos++ = ch;

        // inc eax: Increment the current write position to the next byte.
        *memPos++ = 0x40;
    }

    // mov byte [eax], 10: Write newline character to the output string.
    *memPos++ = 0xc6;
    *memPos++ = 0x00;
    *memPos++ = 10;

    // inc eax: Increment the current write position to the next byte.
    *memPos++ = 0x40;

    // mov byte [eax], 0: Write the terminating zero byte to the output string.
    *memPos++ = 0xc6;
    *memPos++ = 0x00;
    *memPos++ = 0x00;

    // mov esp, ebp: Restore original stack pointer used in the C program from ebp.
    *memPos++ = 0x89;
    *memPos++ = 0xEC;

    // popad: Restore all registers from stack.
    *memPos++ = 0x61;

    // ret: Return to C program.
    *memPos++ = 0xC3;

    return entryPoint;
}

int main(int argc, char* argv[]) {
    if(argc != 2) {
        fail("Usage: ./lang <source>");
    }

    src = fopen(argv[1], "r");
    if(src == NULL) {
        fail("Opening the source file failed.");
    }

    initMem();

    // The program will write the zero-terminated output string to the center of the memory.
    u8* outString = memStart + memSize / 2;
    *outString = 0;

    u8* entryPoint = compile(outString);

    // Compilation done; do not show source information in subsequent error messages.
    chHistoryShowOnError = 0;

    if(fclose(src) != 0) {
        fail("Closing the source file failed.");
    }

    // Run the program.
    void (*entryPointFunc)() = (void(*)())entryPoint;
    __builtin___clear_cache(memStart, memEnd);
    entryPointFunc();

    // Print the output string of the program.
    while(*outString != 0) {
        putchar((int)*outString++);
    }

    return 0;
}
