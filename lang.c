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
// is not a space or newline or part of a comment.
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
        if(ch == '%') {
            while(ch != '\n' && ch != 0) {
                readRawCh();
            }
        }
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

// Forward declarations needed for cyclic recursion.
void compileStatementSequence(u8* varBaseSlot, u32 varOffset);

// True for characters that can end literals or variable names (e.g. statement and argument list
// separator characters, parentheses, characters occurring in binary operators and end-of-file
// special character 0).
int isStopCharacter(u8 character) {
    return character == '=' || character == ';' || character == '(' || character == ')' || character == '}' || character == 0;
}

// Compile atomic expression (that is, expressions that remain after splitting the code at binary
// operators). Reads up to the first character that is not a part of the atomic expression.
// If the expression is an lvalue (can occur on the left side of an assignment operator), returns 1
// and emits code at the end of which eax is the address of the expression value. Otherwise,
// returns 0 and emits code at the end of which eax is the expression value.
int compileAtomicExpression() {
    // Count the number of exclamation points in the beginning of the expression to count the number
    // of times we need to do logical negation to the result.
    int logNegCount = 0;
    while(ch == '!') {
        ++logNegCount;
        readCh();
    }

    // Count the number of stars in the beginning of the expression to count the number of times we
    // need to dereference the final result.
    int derefCount = 0;
    while(ch == '*') {
        ++derefCount;
        readCh();
    }

    int isLValue = 0;

    if(ch >= '0' && ch <= '9') {
        // Decimal literal.
        u32 val = 0;
        while(!isStopCharacter(ch)) {
            if(!(ch >= '0' && ch <= '9')) {
                fail("Expected decimal digit");
            }
            val *= 10;
            val += ch - '0';
            readCh();
        }

        // mov eax, 'val': Set the expression value eax to 'val'.
        emitU8(0xB8);
        emitU32(val);
    } else {
        // Variable or address of variable.

        // Detect whether we want the value of the address of the variable.
        int isAddr = 0;
        if(ch == '&') {
            isAddr = 1;
            readCh();
        }

        Name* name = &rootName;
        while(!isStopCharacter(ch)) {
            if(name != NULL && !extendName(ch, &name, NULL, NULL)) {
                name = NULL;
            }
            readCh();
        }
        if(name == NULL || !name->hasVar) {
            fail("Variable with given name does not exist");
        }

        // mov eax, ['name->varBaseSlot']: Write the base pointer of the function variable to eax.
        emitU8(0xA1);
        emitPtr(name->varBaseSlot);

        // add eax, 'name->varOffset': Set eax to the address of the variable.
        emitU8(0x05);
        emitU32(name->varOffset);

        if(!isAddr) {
            // If we are not taking the address of the variable, we have an lvalue and eax already
            // points to its address.
            isLValue = 1;
        }
    }

    // Dereference the result the requested number of times.
    if(!isLValue && derefCount) {
        // Dereference makes a non-lvalue expression value the address for an lvalue.
        --derefCount;
        isLValue = 1;
    }
    while(derefCount) {
        // mov eax, [eax]: Dereference the pointer at eax and save the result back to eax.
        emitU8(0x8B);
        emitU8(0x00);

        --derefCount;
    }

    // Logically negate the result the requested number of times.
    if(logNegCount && isLValue) {
        // Logical negation destroys the lvalueness of the result.
        isLValue = 0;

        // mov eax, [eax]: Dereference the pointer at eax and save the result back to eax.
        emitU8(0x8B);
        emitU8(0x00);
    }
    while(logNegCount) {
        // cmp eax, 0: Compare the result in eax to zero.
        emitU8(0x83);
        emitU8(0xF8);
        emitU8(0x00);

        // mov eax, 0: Initialize the result in eax to zero.
        emitU8(0xB8);
        emitU32(0);

        // sete al: Set the lowest byte of eax to 1 if eax was nonzero in the comparison above.
        emitU8(0x0F);
        emitU8(0x94);
        emitU8(0xC0);

        --logNegCount;
    }

    return isLValue;
}

// Stack of compileExpression helper functions ordered by precedence. (The ones returning int
// return lvalueness similarly to createAtomicExpressions; others always set eax to the expression
// value like compileExpression.)
void compileExpressionImpl1() {
    // Assignment operator handling.

    int isLValue = compileAtomicExpression();
    if(ch == '=') {
        // Assignment operator.

        if(!isLValue) {
            fail("Left side of assignment is not assignable");
        }

        // push eax: Push the assignment target address in eax to stack.
        emitU8(0x50);

        // Compile the rest of the expression (may contain other assignments).
        readCh();
        compileExpressionImpl1();

        // pop ebx: Pop the assignment target address from the stack to ebx.
        emitU8(0x5B);

        // mov [ebx], eax: Write the expression value to be assigned to the assignment target address.
        emitU8(0x89);
        emitU8(0x03);
    } else {
        // No assignment; make sure that the eax contains the expression value.
        if(isLValue) {
            // mov eax, [eax]: Dereference the pointer at eax and save the result back to eax.
            emitU8(0x8B);
            emitU8(0x00);
        }
    }
}

// Compile expression. Reads up to the first character that is not a part of the expression.
// At the end of the emitted code, the expression value is stored in eax.
void compileExpression() {
    compileExpressionImpl1();
}

// Compile a single variable definition and then proceed to compile the rest of the statement
// sequence using compileStatementSequence. 'name' is the already read prefix of the variable name.
// The 'var*' arguments specify the base pointer slot and offset for creating new variables.
void compileVariableDefinition(Name* name, u8* varBaseSlot, u32 varOffset) {
    while(1) {
        readCh();
        if(ch == '=') {
            // Assignment sign reached.

            if(name->hasVar) {
                fail("Variable with given name already exists");
            }

            // Add the variable to the stack frame and bring it to scope. We do this before
            // compiling the expression to facilitate creating e.g. recursive functions.
            name->hasVar = 1;
            name->varBaseSlot = varBaseSlot;
            varOffset -= 4;
            name->varOffset = varOffset;

            // push 0: Reserve space for the variable in the stack and initialize it to 0.
            emitU8(0x6A);
            emitU8(0x00);

            // Compile the expression that computes the initial value for the variable to eax.
            readCh();
            compileExpression();

            if(ch != ';') {
                fail("Expected ';'");
            }

            // mov ebx, ['name->varBaseSlot']: Write the base pointer to ebx.
            emitU8(0x8B);
            emitU8(0x1D);
            emitPtr(name->varBaseSlot);

            // mov [ebx+'name->varOffset'], eax: Write the initial value in eax to the variable.
            emitU8(0x89);
            emitU8(0x83);
            emitU32(name->varOffset);

            // Compile the rest of the statements (they may use the created variable).
            compileStatementSequence(varBaseSlot, varOffset);

            // Now the variable is out of scope and should be removed.
            name->hasVar = 0;

            return;
        } else {
            // Extend the name by the read character, possibly adding a new node to the trie.
            Name newNameNode;
            Name** nameLink;
            if(!extendName(ch, &name, &newNameNode, &nameLink)) {
                // 'newNameNode' is now part of the trie, so we need to recurse to persist it.
                compileVariableDefinition(name, varBaseSlot, varOffset);

                // All variables created in the subtree of 'newNameNode' are now out of scope, so
                // we can remove the node from the trie.
                *nameLink = NULL;

                return;
            }
        }
    }
}

// Compile a sequence of one or more statements. Statements are either expressions or variable
// definitions (at sign, variable name, equals sign and an expression). The last statement must be
// an expression and its value will be stored in eax at the end of the emitted code.
// The 'var*' arguments specify the base pointer slot and offset for creating new variables.
void compileStatementSequence(u8* varBaseSlot, u32 varOffset) {
    while(1) {
        readCh();

        if(ch == '@') {
            compileVariableDefinition(&rootName, varBaseSlot, varOffset);

            // compileVariableDefinition calls this function after defining the variable, so at
            // this point the whole statement sequence is already compiled and we need to just
            // return here.
            return;
        } else {
            compileExpression();

            if(ch != ';') {
                // End of statement sequence (or a character that could not be parsed as a part of
                // the expression; the caller will catch the error). The expression value is stored
                // in eax.
                return;
            }
        }
    }
}

// Compile the main function and return its entry point.
u8* compileMainFunc() {
    // Create a base pointer slot for the main function.
    u8* baseSlot = memPos;
    emitPtr(NULL);

    u8* entryPoint = memPos;

    // mov ['baseSlot'], esp: Save the base pointer (stack pointer esp) to the base slot.
    emitU8(0x89);
    emitU8(0x25);
    emitPtr(baseSlot);

    compileStatementSequence(baseSlot, 0);

    // mov esp, ['baseSlot']: Move the stack pointer to the base pointer position, discarding local variables.
    emitU8(0x8B);
    emitU8(0x25);
    emitPtr(baseSlot);

    if(ch != 0) {
        fail("Unexpected character");
    }

    // ret: Return.
    emitU8(0xC3);

    return entryPoint;
}

// Emit the main program entry point code that can be called from the C program and will run the
// main function of the program. The entry point of the emitted code is memPos prior to calling
// this function. The return value is the pointer to the slot in the emitted code where the
// address of the main function should be written to.
u8* emitEntryPointGlue() {
    // push 0: Make space in the stack for the exit status of the program.
    emitU8(0x6A);
    emitU8(0x00);

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

    // mov ebx, 'mainFunc': Write the address of the main function to ebx.
    // (The mainFunc address will be filled in after compiling the main function.)
    emitU8(0xBB);
    u8* mainFuncPtrSlot = memPos;
    emitPtr(NULL);

    // mov eax, 0: Write the length of the argument list (zero) to eax.
    emitU8(0xB8);
    emitU32(0);

    // call ebx: Call the main function.
    emitU8(0xFF);
    emitU8(0xD3);

    // mov esp, ['origStackPtrSlot']: Restore the original stack pointer used in the C program
    // from origStackPtrSlot.
    // (The origStackPtrSlot address will be filled in after it is created after this glue.)
    emitU8(0x8B);
    emitU8(0x25);
    u8* origStackPtrSlot2 = memPos;
    emitPtr(NULL);

    // mov [esp+32], eax: Copy the exit status (the return value of the main function) stored in eax to the stack position we reserved for it.
    emitU8(0x89);
    emitU8(0x44);
    emitU8(0x24);
    emitU8(0x20);

    // popad: Restore all registers from stack.
    emitU8(0x61);

    // pop eax: Pop the exit status from the stack to eax.
    emitU8(0x58);

    // ret: Return to the C program.
    emitU8(0xC3);

    // Create a slot for storing the original stack pointer and fill in its address to the
    // instructions using it.
    writePtr(origStackPtrSlot1, memPos);
    writePtr(origStackPtrSlot2, memPos);
    emitPtr(NULL);

    return mainFuncPtrSlot;
}

u8* compile() {
    // Initialize the name trie.
    rootName.hasVar = 0;
    rootName.successor = NULL;
    rootName.neighbor = NULL;

    // Emit glue code that will forward the call from the C program to our compiled main function.
    u8* entryPoint = memPos;
    u8* mainFuncPtrSlot = emitEntryPointGlue();

    // Compile the source code as the main function.
    u8* mainFunc = compileMainFunc();

    // Fill in the address to the main function call in the entry point glue code.
    writePtr(mainFuncPtrSlot, mainFunc);

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
    int (*entryPointFunc)() = (int(*)())entryPoint;
    __builtin___clear_cache(memStart, memEnd);
    return entryPointFunc();
}
