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

// Trie structure containing variable names.
typedef struct Name Name;
struct Name {
    // Last character in the name so far.
    u8 lastCh;

    // If this node contains a variable, 'hasVar' is nonzero, and the variable is located in address
    // *varBaseSlot + varOffset.
    u8 hasVar;
    u8* varBaseSlot;
    u32 varOffset;

    // The trie structure: 'successor' is a child node (if not NULL), and the children form a singly
    // linked list using the 'neighbor' fields as links (NULL marks the end of the list).
    Name* successor;
    Name* neighbor;
};
Name rootName;
Name builtinNames[3];

// Advance *name in the name trie by appending 'newCh' to the end of the name. If this can be done
// using the existing nodes, updates *name and returns 1. Otherwise, returns 0 and if 'newNode' is
// not NULL, adds it to the trie and points *name to it, and sets *link to point to the
// successor/neighbor pointer in the trie that points to the added node (the node can be removed
// by setting **link to NULL; this should only be done after all nodes that have been added after
// adding this node have already been removed). If newNode is added to the trie, it is initialized
// to contain no variable.
int extendName(u8 newCh, Name** name, Name* newNode, Name*** link) {
    Name** linkCand = &(*name)->successor;
    while(*linkCand != NULL && (*linkCand)->lastCh != newCh) {
        linkCand = &(*linkCand)->neighbor;
    }
    if(*linkCand != NULL && (*linkCand)->lastCh == newCh) {
        *name = *linkCand;
        return 1;
    } else {
        if(newNode != NULL) {
            *link = linkCand;
            *linkCand = newNode;
            *name = newNode;
            newNode->lastCh = newCh;
            newNode->hasVar = 0;
            newNode->successor = NULL;
            newNode->neighbor = NULL;
        }
        return 0;
    }
}

void compileFuncBody(u8* baseSlot, u32 argCount, u8 endCh);
void compileFuncBodyImpl(Name* name, int isAssignment, u8* varBaseSlot, u32 varOffset, u8 endCh);

// Compiles a function literal with an argument list. Recursive implementation; argName is the
// prefix of the current argument name read so far, and argCount is the number of complete arguments
// read. 'baseSlot' is the slot containing the current base pointer of the function.
// Discards the current character; assumes that the square bracket starting the argument list has
// already been read. After reading the argument list, proceeds to compileFuncBody.
void compileFuncLiteralWithArgumentList(Name* argName, u32 argCount, u8* baseSlot) {
    readCh();

    if(ch == ']' || ch == ',') {
        // Argument name read; add the variable.
        if(argName->hasVar) {
            fail("Variable with given name already exists");
        }

        argName->hasVar = 1;
        argName->varBaseSlot = baseSlot;

        // Offset of the first argument is 12 to skip old base pointer, return address and the
        // number of arguments.
        argName->varOffset = 12 + 4 * argCount;

        ++argCount;

        if(ch == ']') {
            // End of argument list; proceed to compiling the function body.
            readCh();
            if(ch != '{') {
                fail("Expected '{'");
            }
            compileFuncBody(baseSlot, argCount, '}');
        } else {
            // ch == ',': read the rest of the argument list.
            compileFuncLiteralWithArgumentList(&rootName, argCount, baseSlot);
        }

        // Function body compiled, so the variable is out of scope; remove it.
        argName->hasVar = 0;

        return;
    }

    // Extend the name prefix and continue.
    Name newNameNode;
    Name** nameLink;
    if(extendName(ch, &argName, &newNameNode, &nameLink)) {
        compileFuncLiteralWithArgumentList(argName, argCount, baseSlot);
        return;
    } else {
        // newNameNode is now part of the trie, so to persist it we need to recurse.
        compileFuncLiteralWithArgumentList(argName, argCount, baseSlot);

        // After the recursive call, the function body has been compiled, and all local
        // variables are out of scope, so we can remove the node from the trie.
        *nameLink = NULL;

        return;
    }
}

// Compile function call argument. The emitted implementation writes the value to ebx.
// Uses the current character in 'ch' as the first character, and reads up to the character that
// ends the argument (',' or ')').
void compileCallArgument() {
    if(ch == '#') {
        // Hexadecimal literal.
        u32 val = 0;
        readCh();
        while(ch != ',' && ch != ')') {
            val *= 16;
            if(ch >= '0' && ch <= '9') {
                val += ch - '0';
            } else if(ch >= 'A' && ch <= 'F') {
                val += 10 + (ch - 'A');
            } else {
                fail("Expected hexadecimal digit");
            }
            readCh();
        }

        // mov ebx, 'val': Copy the argument value to ebx.
        emitU8(0xBB);
        emitU32(val);
    } else if(ch >= '0' && ch <= '9') {
        // Decimal literal.
        u32 val = 0;
        while(ch != ',' && ch != ')') {
            if(ch < '0' || ch > '9') {
                fail("Expected decimal digit");
            }
            val *= 10;
            val += ch - '0';
            readCh();
        }

        // mov ebx, 'val': Copy the argument value to ebx.
        emitU8(0xBB);
        emitU32(val);
    } else if(ch == '$') {
        // Binary string literal (in hexadecimal).

        // mov ebx, 'endPtr': Copy the address of the code after the binary string literal to ebx
        // (to be filled in after emitting the string).
        emitU8(0xBB);
        u8* endPtrSlot = memPos;
        emitPtr(NULL);

        // jmp ebx: Jump to the code after the binary string.
        emitU8(0xFF);
        emitU8(0xE3);

        // Save the pointer to the start of the binary string.
        u8* stringPtr = memPos;

        // Read and emit the bytes.
        readCh();
        while(ch != ',' && ch != ')') {
            u8 byte = 0;
            for(int i = 0; i < 2; ++i) {
                byte *= 16;
                if(ch >= '0' && ch <= '9') {
                    byte += ch - '0';
                } else if(ch >= 'A' && ch <= 'F') {
                    byte += 10 + (ch - 'A');
                } else {
                    fail("Expected hexadecimal digit");
                }
                readCh();
            }
            emitU8(byte);
        }

        // Fill in the address of the code after the binary string to the slot that was reserved
        // earlier.
        writePtr(endPtrSlot, memPos);

        // mov ebx, 'stringPtr': Copy the pointer to the start of the binary string to ebx.
        emitU8(0xBB);
        emitPtr(stringPtr);
    } else if(ch == '[' || ch == '{') {
        // Function literal.

        // mov ebx, 'endPtr': Copy the address to the code after the function implementation to ebx
        // (to be filled in after emitting the string).
        emitU8(0xBB);
        u8* endPtrSlot = memPos;
        emitPtr(NULL);

        // jmp ebx: Jump to the code after the function implementation.
        emitU8(0xFF);
        emitU8(0xE3);

        // Create a base pointer slot for the function.
        u8* baseSlot = memPos;
        emitPtr(NULL);

        // Save the entry point of the function.
        u8* entryPoint = memPos;

        if(ch == '[') {
            // Create a variable for each argument and then compile function body.
            compileFuncLiteralWithArgumentList(&rootName, 0, baseSlot);
        } else {
            // No arguments; proceed to compiling the function body directly.
            compileFuncBody(baseSlot, 0, '}');
        }
        readCh();

        // Fill in the address of the code after the function implementation to the slot that was
        // reserved earlier.
        writePtr(endPtrSlot, memPos);

        // mov ebx, 'entryPoint': Copy the pointer to the entry point of the function to ebx.
        emitU8(0xBB);
        emitPtr(entryPoint);
    } else {
        // Variable name, address or function call.
        int addr = 0;
        if(ch == '&') {
            addr = 1;
            readCh();
        }

        Name* varName = &rootName;
        while(ch != ',' && ch != ')' && ch != '(') {
            if(!extendName(ch, &varName, NULL, NULL)) {
                varName = NULL;
                break;
            }
            readCh();
        }

        if(varName == NULL || !varName->hasVar) {
            fail("Variable with given name does not exist");
        }

        if(ch == '(') {
            // Function call.

            if(addr) {
                fail("Cannot take address of function call");
            }

            // push 0: Make a slot in stack for the return value of the call.
            emitU8(0x6A);
            emitU8(0x00);

            // mov eax, esp: Point eax to the return value slot.
            emitU8(0x89);
            emitU8(0xE0);

            // Compile the call as a single function call with assignment.
            compileFuncBodyImpl(varName, 1, NULL, 0, 255);
            readCh();

            // pop ebx: Pop the return value from the return value slot in the stack to ebx.
            emitU8(0x5B);
        } else {
            // Variable name or address.

            // mov ebx, ['varName->varBaseSlot']: Copy the base pointer of the argument variable to ebx.
            emitU8(0x8B);
            emitU8(0x1D);
            emitPtr(varName->varBaseSlot);

            if(addr) {
                // add ebx, 'varName->varOffset': Compute the variable address to ebx.
                emitU8(0x81);
                emitU8(0xC3);
                emitU32(varName->varOffset);
            } else {
                // mov ebx, [ebx+'varName->varOffset']: Copy the variable value to ebx.
                emitU8(0x8B);
                emitU8(0x9B);
                emitU32(varName->varOffset);
            }
        }
    }
}

// Implementation of the actual function body compilation in compileFuncBody; tracks compilation
// state in the arguments and uses recursion for compilation of sub-units (and stack allocation
// of name trie nodes).
//
// If endCh has the special value 255, the function compiles a single function call.
// Otherwise, it compiles a sequence of function calls, possibly with results assigned to variables
// either to an existing variable (with the '=' operator) or a new variable (with the ':' operator).
// In this case, function calls/assignments are read until the endCh character is read (if endCh
// is 0, it reads to the end of the source file).
//
// The 'name' argument specifies a prefix for the name to be read from the source (the called
// function or the assignment target). If 'isAssignment' is nonzero, the generated code for the
// first function call (which must exist and follow immediately without further assignments)
// assigns the function result to the address in register eax (that is, passes the value in eax as
// the first argument).
//
// The 'var*' arguments specify the base pointer slot and offset for creating new variables. They
// are ignored if endCh is 255 (as new variables cannot be created then).
//
// Uses the current character in 'ch' as the first character. After reading the last character (if
// endCh is 255, the closing parenthesis and otherwise the occurence of endCh), no further reads
// are made prior to returning.
void compileFuncBodyImpl(Name* name, int isAssignment, u8* varBaseSlot, u32 varOffset, u8 endCh)
{
    if(endCh != 255 && ch == endCh && !isAssignment && name == &rootName) {
        return;
    }

    // Allow comments starting with % and ending at newline for actual function bodies.
    if(name == &rootName && !isAssignment && endCh != 255 && ch == '%') {
        while(ch != '\n' && ch != 0) {
            readRawCh();
        }
        if(ch != 0) {
            readCh();
        }
        compileFuncBodyImpl(name, isAssignment, varBaseSlot, varOffset, endCh);
        return;
    }

    if(ch != '(' && ch != ':' && ch != '=') {
        // Not a special character; extend the name prefix and continue.
        Name newNameNode;
        Name** nameLink;
        if(extendName(ch, &name, &newNameNode, &nameLink)) {
            readCh();
            compileFuncBodyImpl(name, isAssignment, varBaseSlot, varOffset, endCh);
            return;
        } else {
            // newNameNode is now part of the trie, so to persist it we need to recurse.
            readCh();
            compileFuncBodyImpl(name, isAssignment, varBaseSlot, varOffset, endCh);

            // After the recursive call, the function body has been compiled, and all local
            // variables are out of scope, so we can remove the node from the trie.
            *nameLink = NULL;

            return;
        }
    }

    if(ch != '(' && (isAssignment || endCh == 255)) {
        fail("Unexpected assignment");
    }

    if(ch == ':') {
        // Variable creation ':'.
        if(name->hasVar) {
            fail("A variable with the same name already exists");
        }

        // Add information about the new variable to the trie.
        name->hasVar = 1;
        name->varBaseSlot = varBaseSlot;
        varOffset -= 4;
        name->varOffset = varOffset;

        // push 0: Reserve space for the variable in the stack and initialize it to 0.
        emitU8(0x6A);
        emitU8(0x00);

        // mov eax, esp: Save the address of the new variable to eax.
        emitU8(0x89);
        emitU8(0xE0);

        // Compile the function call and the remainder of the body in a recursive call, using
        // isAssignment = 1 to enable the assignment.
        readCh();
        compileFuncBodyImpl(&rootName, 1, varBaseSlot, varOffset, endCh);

        // After the recursive call, the function body has been compiled, and all local
        // variables are out of scope, so we can remove the created variable.
        name->hasVar = 0;

        return;
    }

    // Variable assignment '=' or function call '('.
    if(!name->hasVar) {
        fail("Variable with given name does not exist");
    }

    if(ch == '=') {
        // Variable assignment '='.

        // mov eax, ['name->varBaseSlot']: Copy the base pointer of the variable to eax.
        emitU8(0xA1);
        emitPtr(name->varBaseSlot);

        // add eax, 'name->varOffset': Apply the variable offset to eax so that eax points to the variable.
        emitU8(0x05);
        emitU32(name->varOffset);

        // Compile the function call and the remainder of the body, using isAssignment = 1 to
        // enable the assignment.
        name = &rootName;
        isAssignment = 1;
        readCh();
        compileFuncBodyImpl(name, isAssignment, varBaseSlot, varOffset, endCh);
        return;
    }

    // sub esp, 'callSize': Make space for the arguments and the number of arguments in the stack.
    // 'callSize' will be filled in after reading the arguments.
    emitU8(0x81);
    emitU8(0xEC);
    u8* callSizeSlot = memPos;
    emitU32(0);

    u32 argCount = 0;
    if(isAssignment) {
        // This is an assignment; use eax as the first argument.
        argCount = 1;

        // mov [esp+4], eax: Copy eax to the first argument.
        emitU8(0x89);
        emitU8(0x44);
        emitU8(0x24);
        emitU8(0x04);
    }

    readCh();
    if(ch != ')') {
        while(1) {
            // Compile the argument; the value is stored in ebx.
            compileCallArgument();

            ++argCount;

            // mov [esp+'4 * argCount'], ebx: Copy the argument value to the argument slot in the stack.
            emitU8(0x89);
            emitU8(0x9C);
            emitU8(0x24);
            emitU32(4 * argCount);

            if(ch == ')') {
                break;
            }
            readCh();
        }
    }

    // mov dword [esp], 'argCount': Write the number of arguments to the top of the stack.
    emitU8(0xC7);
    emitU8(0x04);
    emitU8(0x24);
    emitU32(argCount);

    // Fill in the space needed for the argument list to the slot that was reserved earlier.
    writeU32(callSizeSlot, 4 * (argCount + 1));

    // mov eax, ['name->varBaseSlot']: Copy the base pointer of the function variable to eax.
    emitU8(0xA1);
    emitPtr(name->varBaseSlot);

    // call [eax+'name->varOffset']: Call the function.
    emitU8(0xFF);
    emitU8(0x90);
    emitU32(name->varOffset);

    if(endCh != 255) {
        // Compile the rest of the function calls.
        name = &rootName;
        isAssignment = 0;
        readCh();
        compileFuncBodyImpl(name, isAssignment, varBaseSlot, varOffset, endCh);
    }
}

// Compile the function body for function with given argCount arguments whose implementation source
// code ends in the character endCh (typically '}', but 0 for main the function as it ends in the
// end of the source code). baseSlot should be the location where the function can store its stack
// base pointer upon being called.
// The entry point of the function is memPos before the call.
// Discards the current character; stops reading after reading the end character.
void compileFuncBody(u8* baseSlot, u32 argCount, u8 endCh) {
    // cmp dword [esp+4], 'argCount': Compare the number of arguments from stack to 'argCount'.
    emitU8(0x81);
    emitU8(0x7C);
    emitU8(0x24);
    emitU8(0x04);
    emitU32(argCount);

    // x: jne x: If the number of arguments is not 'argCount', loop infinitely. (TODO: Better error handling)
    emitU8(0x75);
    emitU8(0xFE);

    // push dword ['baseSlot']: Push the base pointer of the upper recursive call of this function to the stack.
    emitU8(0xFF);
    emitU8(0x35);
    emitPtr(baseSlot);

    // mov ['baseSlot'], esp: Save the base pointer (stack pointer esp) to the base slot.
    emitU8(0x89);
    emitU8(0x25);
    emitPtr(baseSlot);

    // Compile the actual function content.
    readCh();
    compileFuncBodyImpl(&rootName, 0, baseSlot, 0, endCh);

    // mov esp, ['baseSlot']: Move the stack pointer to the base pointer position, discarding local variables.
    emitU8(0x8B);
    emitU8(0x25);
    emitPtr(baseSlot);

    // pop dword ['baseSlot']: Restore the base pointer of the upper recursive call of this function from the stack.
    emitU8(0x8F);
    emitU8(0x05);
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
}

u8* compileMainFunc() {
    // Create a base pointer slot for the main function.
    u8* baseSlot = memPos;
    emitPtr(NULL);

    u8* entryPoint = memPos;

    // Compile the main function as a function body that ends at the end of the file (character 0).
    compileFuncBody(baseSlot, 0, 0);

    return entryPoint;
}

// Emit the main program entry point code that can be called from the C program and will run the
// main function of the program. The entry point of the emitted code is memPos prior to calling
// this function. The return value is the pointer to the slot in the emitted code where the
// address of the main function should be written to.
u8* emitEntryPointGlue() {
    // pushad: Save all registers to stack.
    emitU8(0x60);

    // mov ['origStackPtrSlot'], esp: Save the original stack pointer to origStackPtrSlot.
    // (The origStackPtrSlot address will be filled in after it is created after this glue.)
    emitU8(0x89);
    emitU8(0x25);
    u8* origStackPtrSlot1 = memPos;
    emitPtr(NULL);

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

    // mov esp, ['origStackPtrSlot']: Restore the original stack pointer used in the C program
    // from origStackPtrSlot.
    // (The origStackPtrSlot address will be filled in after it is created after this glue.)
    emitU8(0x8B);
    emitU8(0x25);
    u8* origStackPtrSlot2 = memPos;
    emitPtr(NULL);

    // popad: Restore all registers from stack.
    emitU8(0x61);

    // ret: Return to the C program.
    emitU8(0xC3);

    // Create a slot for storing the original stack pointer and fill in its address to the
    // instructions using it.
    writePtr(origStackPtrSlot1, memPos);
    writePtr(origStackPtrSlot2, memPos);
    emitPtr(NULL);


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

    // pop ebx: Read the first argument (the output pointer) to ebx.
    emitU8(0x5B);

    // pop ecx: Read the second argument (the value) to ecx.
    emitU8(0x59);

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

    return 0;
}
