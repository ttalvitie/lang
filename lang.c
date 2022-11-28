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
void writeU8(u8* dest, u8 val) {
    *dest = val;
}
void writeU32(u8* dest, u32 val) {
    memcpy(dest, &val, 4);
}

void emitPtr(void* ptr) {
    writePtr(memPos, ptr);
    memPos += 4;
}
void emitU8(u8 val) {
    writeU8(memPos, val);
    memPos += 1;
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
Name builtinNames[3];

// Compile the function body for function with given argCount arguments whose implementation source
// code ends in the character endCh (typically '}', but 0 for main the function as it ends in the
// end of the source code). baseSlot should be the location where the function can store its stack
// base pointer upon being called.
// Discards the current character; stops reading after reading the end character.
u8* compileFuncBody(u8* baseSlot, u32 argCount, u8 endCh) {
    // Start emitting the function implementation.
    u8* entryPoint = memPos;

    // cmp dword [esp+4], 'argCount': Compare the number of arguments from stack to 'argCount'.
    emitU8(0x81);
    emitU8(0x7C);
    emitU8(0x24);
    emitU8(0x04);
    emitU32(argCount);

    // x: jne x: If the number of arguments is not 'argCount', loop infinitely. (TODO: Better error handling)
    emitU8(0x75);
    emitU8(0xFE);

    // mov ['baseSlot'], esp: Save the base pointer (stack pointer esp) to the base slot.
    emitU8(0x89);
    emitU8(0x25);
    emitPtr(baseSlot);

    // mov esp, ['baseSlot']: Move the stack pointer to its original position, discarding local variables.
    emitU8(0x8B);
    emitU8(0x25);
    emitPtr(baseSlot);

    // pop eax: Pop the return address from the top of the stack to eax.
    emitU8(0x58);

    // add esp, '4 * (argCount + 1)': Discard the arguments and the number of arguments from the stack.
    emitU8(0x81);
    emitU8(0xC4);
    emitU32(4 * (argCount + 1));

    // jmp eax: Return from the function by jumping to the return address stored in eax.
    emitU8(0xFF);
    emitU8(0xE0);

    return entryPoint;
}

u8* compileMainFunc() {
    // Create a base pointer slot for the main function.
    u8* baseSlot = memPos;
    emitPtr(NULL);

    // Compile the main function as a function body that ends at the end of the file (character 0).
    return compileFuncBody(baseSlot, 0, 0);
}

// Emit the main program entry point code that can be called from the C program and will run the
// main function of the program. The entry point of the emitted code is memPos prior to calling
// this function. The return value is the pointer to the slot in the emitted code where the
// address of the main function should be written to.
u8* emitEntryPointGlue() {
    // pushad: Save all registers to stack.
    emitU8(0x60);

    // mov ebp, esp: Save the original stack pointer to ebp.
    emitU8(0x89);
    emitU8(0xE5);

    // mov esp, 'memEnd': Move the stack pointer to the end of the memory block.
    emitU8(0xBC);
    emitPtr(memEnd);

    // push 0: Push the length of the argument list for the main function (zero) to the stack.
    emitU8(0x6A);
    emitU8(0x00);

    // mov eax 'mainFunc': Write the address of the main function to eax.
    // (The mainFunc address will be filled in after compiling the main function.)
    emitU8(0xB8);
    u8* mainFuncCallAddr = memPos;
    emitPtr(NULL);

    // call eax: Call the main function.
    emitU8(0xFF);
    emitU8(0xD0);

    // mov esp, ebp: Restore original stack pointer used in the C program from ebp.
    emitU8(0x89);
    emitU8(0xEC);

    // popad: Restore all registers from stack.
    emitU8(0x61);

    // ret: Return to the C program.
    emitU8(0xC3);

    return mainFuncCallAddr;
}

// Emit the identity function (32-bit write) implementation; return the function pointer to it.
u8* emitIdentityFunction() {
    u8* entryPoint = memPos;

    // pop eax: Pop the return address from the stack to eax.
    emitU8(0x58);

    // pop ebx: Pop the number of arguments from the stack to ebx.
    emitU8(0x5B);

    // cmp ebx, 2: Compare the number of arguments to 2.
    emitU8(0x83);
    emitU8(0xFB);
    emitU8(0x02);

    // x: jne x: If the number of arguments is not 2, loop infinitely. (TODO: Better error handling)
    emitU8(0x75);
    emitU8(0xFE);

    // pop ecx: Read the second argument (the value) to ecx.
    emitU8(0x59);

    // pop ebx: Read the first argument (the output pointer) to ebx.
    emitU8(0x5B);

    // mov [ebx], ecx: Write the value to the output.
    emitU8(0x89);
    emitU8(0x0B);

    // jmp eax: Return from the function by jumping to the return address stored in eax.
    emitU8(0xFF);
    emitU8(0xE0);

    return entryPoint;
}

// Emits builtin variables/functions:
//   - "": The identity function (32-bit write).
//   - "brk": The program break (pointer to the end of allocated memory).
// Initializes the name trie under rootName to contain them.
// Returns a pointer to the brk variable where the correct value should be written once it is known.
u8* emitBuiltins() {
    // Emit identity function implementation.
    u8* identityFunc = emitIdentityFunction();

    // Emit the variables:

    // The identity function variable contains the address of the implementation.
    emitPtr(identityFunc);

    // Set brk to NULL for now; it will be overwritten by the caller later.
    u8* brkSlot = memPos;
    emitPtr(NULL);

    // Create a base pointer slot for builtin variables that contains its own address; thus we can
    // reference the variables "" and "brk" with offsets -8 and -4, respectively.
    u8* baseSlot = memPos;
    emitPtr(baseSlot);

    // Initialize the name trie.
    rootName.lastCh = 0;
    builtinNames[0].lastCh = 'b';
    builtinNames[1].lastCh = 'r';
    builtinNames[2].lastCh = 'k';

    rootName.hasVar = 1;
    builtinNames[0].hasVar = 0;
    builtinNames[1].hasVar = 0;
    builtinNames[2].hasVar = 1;

    rootName.varBaseSlot = baseSlot;
    rootName.varOffset = (u32)-8;
    builtinNames[2].varBaseSlot = baseSlot;
    builtinNames[2].varOffset = (u32)-4;

    rootName.successor = &builtinNames[0];
    builtinNames[0].successor = &builtinNames[1];
    builtinNames[1].successor = &builtinNames[2];
    builtinNames[2].successor = NULL;

    rootName.neighbor = NULL;
    builtinNames[0].neighbor = NULL;
    builtinNames[1].neighbor = NULL;
    builtinNames[2].neighbor = NULL;

    return brkSlot;
}

u8* compile() {
    // Emit glue code that will forward the call from the C program to our compiled main function.
    u8* entryPoint = memPos;
    u8* mainFuncCallAddr = emitEntryPointGlue();

    // Emit data and initialize name trie for builtin variables.
    u8* brkSlot = emitBuiltins();

    // Compile the source code as the main function (a function body that ends in 0, that is, the
    // end of the file).
    u8* mainFunc = compileMainFunc();

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

    u8* entryPoint = compile();

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
