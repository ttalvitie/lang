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

void writePtr(u8* dest, void* ptr) {
    memcpy(dest, &ptr, 4);
}

void writeU32(u8* dest, u32 val) {
    memcpy(dest, &val, 4);
}

void emitPtr(void* ptr) {
    writePtr(memPos, ptr);
    memPos += 4;
}

void emitU32(u32 val) {
    writeU32(memPos, val);
    memPos += 4;
}

typedef struct Name Name;
struct Name {
    u8 lastCh;
    u8 hasVar;
    u8* varBaseSlot;
    u32 varOffset;
    Name* successor;
    Name* neighbor;
};
Name rootName;

// Compile TODO
u8* compileFuncBody(u8 endCh) {
    u8* entryPoint = memPos;

    // TEST: call identity function

    // mov eax, ['rootName.varBaseSlot']
    *memPos++ = 0xA1;
    emitPtr(rootName.varBaseSlot);

    // mov eax, [eax-'rootName.varOffset']
    *memPos++ = 0x8B;
    *memPos++ = 0x80;
    emitU32(-rootName.varOffset);

    // push 'memStart + 10000'
    *memPos++ = 0x68;
    emitPtr(memStart + 10000);

    // push 0x13371337
    *memPos++ = 0x68;
    emitU32(0x13371337);

    // push 2
    *memPos++ = 0x6A;
    *memPos++ = 0x02;

    // call eax
    *memPos++ = 0xFF;
    *memPos++ = 0xD0;

    // ret: Return from the function.
    *memPos++ = 0xC3;

    return entryPoint;
}

// Emit the main program entry point code that can be called from the C program and will run the
// main function of the program. The entry point of the emitted code is memPos prior to calling
// this function. The return value is the pointer to the slot in the emitted code where the
// address of the main function should be written to.
u8* emitEntryPointGlue() {
    // pushad: Save all registers to stack.
    *memPos++ = 0x60;

    // mov ebp, esp: Save the original stack pointer to ebp.
    *memPos++ = 0x89;
    *memPos++ = 0xE5;

    // mov esp, 'memEnd': Move the stack pointer to the end of the memory block.
    *memPos++ = 0xBC;
    emitPtr(memEnd);

    // push 0: Push the length of the argument list for the main function (zero) to the stack.
    *memPos++ = 0x6A;
    *memPos++ = 0x00;

    // mov eax 'mainFunc': Write the address of the main function to eax.
    // (The mainFunc address will be filled in after compiling the main function.)
    *memPos++ = 0xB8;
    u8* mainFuncCallAddr = memPos;
    memPos += 4;

    // call eax: Call the main function.
    *memPos++ = 0xFF;
    *memPos++ = 0xD0;

    // mov esp, ebp: Restore original stack pointer used in the C program from ebp.
    *memPos++ = 0x89;
    *memPos++ = 0xEC;

    // popad: Restore all registers from stack.
    *memPos++ = 0x61;

    // ret: Return to the C program.
    *memPos++ = 0xC3;

    return mainFuncCallAddr;
}

// Emit the identity function (32-bit write) implementation; return the function pointer to it.
u8* emitIdentityFunction() {
    u8* entryPoint = memPos;

    // pop eax: Pop the return address from the stack to eax.
    *memPos++ = 0x58;

    // pop ebx: Pop the number of arguments from the stack to ebx.
    *memPos++ = 0x5B;

    // cmp ebx, 2: Compare the number of arguments to 2.
    *memPos++ = 0x83;
    *memPos++ = 0xFB;
    *memPos++ = 0x02;

    // x: jne x: If the number of arguments is not 2, loop infinitely. (TODO: Better error handling)
    *memPos++ = 0x75;
    *memPos++ = 0xFE;

    // pop ecx: Read the second argument (the value) to ecx.
    *memPos++ = 0x59;

    // pop ebx: Read the first argument (the output pointer) to ebx.
    *memPos++ = 0x5B;

    // mov [ebx], ecx: Write the value to the output.
    *memPos++ = 0x89;
    *memPos++ = 0x0B;

    // jmp eax: Return from the function.
    *memPos++ = 0xFF;
    *memPos++ = 0xE0;

    return entryPoint;
}

u8* compile(u8* outString) {
    // Emit glue code that will forward the call from the C program to our compiled main function.
    u8* entryPoint = memPos;
    u8* mainFuncCallAddr = emitEntryPointGlue();

    // Initialize the name trie with two names:
    //   - "": The identity function (32-bit write).
    //   - "brk": The program break (pointer to the end of allocated memory).
    emitPtr(emitIdentityFunction());
    u8* brkSlot = memPos;
    memPos += 4;
    u8* baseSlot = memPos;
    emitPtr(baseSlot);

    Name name1, name2, brkName;

    rootName.lastCh = 0;
    name1.lastCh = 'b';
    name2.lastCh = 'r';
    brkName.lastCh = 'k';

    rootName.hasVar = 1;
    rootName.varBaseSlot = baseSlot;
    rootName.varOffset = 8;
    brkName.hasVar = 1;
    brkName.varBaseSlot = baseSlot;
    brkName.varOffset = 4;

    name1.hasVar = 0;
    name2.hasVar = 0;

    rootName.successor = &name1;
    name1.successor = &name2;
    name2.successor = &brkName;
    brkName.successor = NULL;

    rootName.neighbor = NULL;
    name1.neighbor = NULL;
    name2.neighbor = NULL;
    brkName.neighbor = NULL;

    // Compile the source code as the main function (a function body that ends in 0, that is, the
    // end of the file).
    readCh();
    u8* mainFunc = compileFuncBody(0);

    // Fill in the address to the main function call in the entry point glue code.
    writePtr(mainFuncCallAddr, mainFunc);

    // Fill in the initial program break position.
    writePtr(brkSlot, memPos);

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

    // TEST
    printf("%x\n", *(u32*)(memStart + 10000));

    return 0;
}
