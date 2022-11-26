#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/mman.h>

typedef unsigned char u8;
typedef unsigned int u32;

void fail(const char* format, ...) {
    fprintf(stderr, "ERROR: ");
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, "\n");
    exit(1);
}

FILE* src;

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

int main(int argc, char* argv[]) {
    if(argc != 2) {
        fail("Usage: ./lang <source>");
    }

    src = fopen(argv[1], "r");
    if(src == NULL) {
        fail("Opening the source file failed.");
    }

    initMem();

    if(fclose(src) != 0) {
        fail("Closing the source file failed.");
    }

    // pushad: Save all registers to stack.
    *memPos++ = 0x60;

    // mov ebp, esp: Save original stack pointer to ebp.
    *memPos++ = 0x89;
    *memPos++ = 0xE5;

    // mov esp, 'memEnd': Move stack pointer to the end of the memory block.
    *memPos++ = 0xBC;
    memWritePtr(memEnd);

    const u32 data = 0x1337CAFE;
    // push 'data': Write data to stack.
    *memPos++ = 0x68;
    memWriteU32(data);

    // mov esp, ebp: Restore original stack pointer used in the C program from ebp.
    *memPos++ = 0x89;
    *memPos++ = 0xEC;

    // popad: Restore all registers from stack.
    *memPos++ = 0x61;

    // ret: Return to C program.
    *memPos++ = 0xC3;

    void (*func)() = (void(*)())memStart;
    __builtin___clear_cache(memStart, memEnd);
    func();

    u32 data2;
    memcpy(&data2, memEnd - 4, 4);
    printf("Stack data: %x\n", data2);

    return 0;
}
