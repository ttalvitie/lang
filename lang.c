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
// is not a space or newline or part of a comment. If putBackCh is nonzero, readRawCh reads it and
// resets it to zero.
u8 ch = '?';
u8 putBackCh = 0;

// Convenience function for putting back character 'prevCh' that was read prior to the current
// character: checks that there is no already put back character and the current current character
// is not zero, and makes 'prevCh' the currently read character and the currently read character
// the next character to be read.
void setPutBackCh(char prevCh) {
    if(ch == 0) {
        fail("Expected character.");
    }
    if(putBackCh != 0) {
        fail("Internal compiler error: could not put back read character.");
    }
    putBackCh = ch;
    ch = prevCh;
}

void readRawCh() {
    if(putBackCh != 0) {
        ch = putBackCh;
        putBackCh = 0;
        return;
    }

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
        if(ch == '/') {
            // Ignore comments starting with "//" and ending in newline.
            readRawCh();
            if(ch != '/') {
                // Not a comment; put read character back.
                setPutBackCh('/');
            } else {
                // Comment; read until newline or end of file.
                while(ch != '\n' && ch != 0) {
                    readRawCh();
                }
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
Name builtinNameNodes[64];

// Addresses of builtin variables.
u8* builtinInitialProgramBreakPtr;
u8* builtinWrongNumberOfArgumentsHandlerPtr;

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
void compileExpression();
void compileStatementSequence(u8* varBaseSlot, u32 varOffset);

// True for characters that can end literals or variable names (e.g. statement and argument list
// separator characters, parentheses, characters occurring in binary operators and end-of-file
// special character 0).
int isStopCharacter(u8 character) {
    return
        character == '=' ||
        character == '!' ||
        character == '&' ||
        character == '|' ||
        character == '<' ||
        character == '>' ||
        character == '+' ||
        character == '*' ||
        character == '/' ||
        character == '%' ||
        character == '-' ||
        character == ';' ||
        character == '(' ||
        character == ')' ||
        character == '}' ||
        character == ',' ||
        character == 0;
}

// Helper function for expression compilation that checks whether *pIsLValue is nonzero and if it
// is, sets it to zero and emits code that dereferences eax and places the result back in eax.
void ensureNotLValue(int* pIsLValue) {
    if(*pIsLValue) {
        *pIsLValue = 0;

        // mov eax, [eax]: Dereference the pointer at eax and save the result back to eax.
        emitU8(0x8B);
        emitU8(0x00);
    }
}

// Compile a body of function literal (assumes that all argCount arguments have already been added
// to the name trie). Emits a jump that bypasses the function implementation and writes the
// function pointer to eax.
void compileFuncLiteral(u8* baseSlot, u32 argCount) {
    // mov ebx, 'endPtr': Write the address to the code after the function implementation to ebx
    // (to be filled in after emitting the string).
    emitU8(0xBB);
    u8* endPtrSlot = memPos;
    emitPtr(NULL);

    // jmp ebx: Jump to the code after the function implementation.
    emitU8(0xFF);
    emitU8(0xE3);

    u8* entryPoint = memPos;

    // mov ebx, 'argCountCorrectJumpPoint': Write the jump point for the case where the number of arguments is correct to ebx. ('argCountCorrectJumpPoint' will be filled in later.)
    emitU8(0xBB);
    u8* argCountCorrectJumpPointSlot = memPos;
    emitPtr(NULL);

    // cmp eax, 'argCount': Compare the number of arguments (in eax) to 'argCount'.
    emitU8(0x3D);
    emitU32(argCount);

    // "je correct ; jmp incorrect ; correct: jmp ebx ; incorrect:": If the number of arguments is correct, jump to 'argCountCorrectJumpPoint' stored in ebx; otherwise continue.
    emitU8(0x74);
    emitU8(0x02);
    emitU8(0xEB);
    emitU8(0x02);
    emitU8(0xFF);
    emitU8(0xE3);

    // If control reaches here, the number of arguments (stored in eax) is wrong. Let us call the
    // handler function and then go to infinite loop.

    // push eax: Push the number of arguments as the fourth argument.
    emitU8(0x50);

    // push 'argCount': Push the correct number of arguments as the third argument.
    emitU8(0x68);
    emitU32(argCount);

    // push 'chColumn': Push the current column as the second argument.
    emitU8(0x68);
    emitU32((u32)chColumn);

    // push 'chLine': Push the current line as the first argument.
    emitU8(0x68);
    emitU32((u32)chLine);

    // mov eax, 4: Write the number of arguments (4) to eax.
    emitU8(0xB8);
    emitU32(4);

    // mov ebx, ['builtinWrongNumberOfArgumentsHandlerPtr']: Read the handler function pointer to ebx.
    emitU8(0x8B);
    emitU8(0x1D);
    emitPtr(builtinWrongNumberOfArgumentsHandlerPtr);

    // call ebx: Call the handler function.
    emitU8(0xFF);
    emitU8(0xD3);

    // x: jmp x: Loop infinitely.
    emitU8(0xEB);
    emitU8(0xFE);

    // Fill in argCountCorrectJumpPoint.
    writePtr(argCountCorrectJumpPointSlot, memPos);

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

    // Compile the function body. It will write the return value to eax (should not be clobbered).
    compileStatementSequence(baseSlot, 0);
    if(ch != '}') {
        fail("Expected '}'.");
    }
    readCh();

    // mov esp, ['baseSlot']: Move the stack pointer to the base pointer position, discarding local variables.
    emitU8(0x8B);
    emitU8(0x25);
    emitPtr(baseSlot);

    // pop dword ['baseSlot']: Restore the base pointer of the upper recursive call of this function from the stack.
    emitU8(0x8F);
    emitU8(0x05);
    emitPtr(baseSlot);

    // pop ebx: Pop the return address to ebx.
    emitU8(0x5B);

    // add esp, '4 * argCount': Discard the arguments from the stack.
    emitU8(0x81);
    emitU8(0xC4);
    emitU32(4 * argCount);

    // jmp ebx: Return from the function by jumping to the return address stored in ebx.
    emitU8(0xFF);
    emitU8(0xE3);

    // Fill in the address of the code after the function implementation to the slot that was
    // reserved earlier.
    writePtr(endPtrSlot, memPos);

    // mov eax, 'entryPoint': Write the function pointer to eax.
    emitU8(0xB8);
    emitPtr(entryPoint);
}

// Read argument list of a function literal, defining the argument variables to the trie, compiling
// the function body using compileFuncLiteral and then undefining the argument variables. Recursive
// implementation; 'name' is the currently read name prefix, 'argCount' is the number of fully read
// argument names so far.
void compileFuncLiteralWithArgumentList(Name* name, u8* baseSlot, u32 argCount) {
    while(1) {
        readCh();
        if(ch == ',' || ch == ']') {
            // Name completely read.

            if(name->hasVar) {
                fail("A variable with given name already exists.");
            }

            // Add the variable.
            name->hasVar = 1;
            name->varBaseSlot = baseSlot;
            name->varOffset = 4 * (argCount + 2); // Skip old base pointer and return address; see compileFuncLiteral implementation.
            ++argCount;

            // Proceed either recursively to the rest of the argument list or to compiling the
            // function body.
            if(ch == ']') {
                readCh();
                if(ch != '{') {
                    fail("Expected '{'.");
                }
                compileFuncLiteral(baseSlot, argCount);
            } else {
                compileFuncLiteralWithArgumentList(&rootName, baseSlot, argCount);
            }

            // At this point, the whole function literal is compiled and the variable is out of
            // scope. Thus we remove it and return.
            name->hasVar = 0;
            return;
        } else {
            // Extend the name by the read character, possibly adding a new node to the trie.
            Name newNameNode;
            Name** nameLink;
            if(!extendName(ch, &name, &newNameNode, &nameLink)) {
                // 'newNameNode' is now part of the trie, so we need to recurse to persist it.
                compileFuncLiteralWithArgumentList(name, baseSlot, argCount);

                // At this point, the whole function literal is compiled and all the variables in
                // the subtree of 'newNameNode' are out of scope, so we can remove the node from
                // the trie and retun.
                *nameLink = NULL;
                return;
            }
        }
    }
}

// Emit code that logically negates the value in eax.
void emitEaxLogicalNegation() {
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

    if(ch == '#') {
        // Hexadecimal literal.
        u32 val = 0;

        readCh();
        while(!isStopCharacter(ch)) {
            val *= 16;
            if(ch >= '0' && ch <= '9') {
                val += ch - '0';
            } else if(ch >= 'A' && ch <= 'F') {
                val += 10;
                val += ch - 'A';
            } else {
                fail("Expected hexadecimal digit.");
            }
            readCh();
        }

        // mov eax, 'val': Set the expression value eax to 'val'.
        emitU8(0xB8);
        emitU32(val);
    } else if(ch >= '0' && ch <= '9') {
        // Decimal literal.
        u32 val = 0;
        while(!isStopCharacter(ch)) {
            if(!(ch >= '0' && ch <= '9')) {
                fail("Expected decimal digit.");
            }
            val *= 10;
            val += ch - '0';
            readCh();
        }

        // mov eax, 'val': Set the expression value eax to 'val'.
        emitU8(0xB8);
        emitU32(val);
    } else if(ch == '"' || ch == '$') {
        // (Binary) string literal.

        // mov eax, 'endPtr': Copy the address of the code after the binary string literal to eax (to be filled in after emitting the string).
        emitU8(0xB8);
        u8* endPtrSlot = memPos;
        emitPtr(NULL);

        // jmp eax: Jump to the code after the binary string.
        emitU8(0xFF);
        emitU8(0xE0);

        // Save the pointer to the start of the binary string.
        u8* stringPtr = memPos;

        // Read and emit the bytes.
        if(ch == '"') {
            // String literal (null-terminated).
            readRawCh();
            while(ch != '"') {
                if(ch == '\\') {
                    readCh();
                }
                emitU8(ch);
                readRawCh();
            }
            readCh();
            emitU8(0x00);
        } else {
            // Binary string literal (in hexadecimal).
            readCh();
            while(!isStopCharacter(ch)) {
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
        }

        // Fill in the address of the code after the binary string to the slot that was reserved
        // earlier.
        writePtr(endPtrSlot, memPos);

        // mov eax, 'stringPtr': Copy the pointer to the start of the binary string to ebx.
        emitU8(0xB8);
        emitPtr(stringPtr);
    } else if(ch == '[' || ch == '{') {
        // Function literal.

        // "jmp x ; dd 'NULL' ; x:": Create a base pointer slot for the function and jump over it.
        emitU8(0xEB);
        emitU8(0x04);
        u8* baseSlot = memPos;
        emitPtr(NULL);

        if(ch == '[') {
            // Argument list.
            compileFuncLiteralWithArgumentList(&rootName, baseSlot, 0);
        } else {
            // No argument list.
            compileFuncLiteral(baseSlot, 0);
        }
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
            fail("Variable with given name does not exist.");
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

    // Allow calling the expression value as a function pointer (possibly multiple times).
    while(ch == '(') {
        ensureNotLValue(&isLValue);

        // sub esp, 'argListSize': Make space for the argument list in the stack. ('argListSize' will be filled in later.)
        emitU8(0x81);
        emitU8(0xEC);
        u8* argListSizeSlot = memPos;
        emitU32(0);

        // push eax: Save the function pointer to stack.
        emitU8(0x50);

        u32 argCount = 0;

        // Read the argument list.
        readCh();
        if(ch != ')') {
            while(1) {
                compileExpression();
                if(ch != ',' && ch != ')') {
                    fail("Expected ',' or ')'.");
                }

                ++argCount;

                // mov [esp+'4 * argCount'], eax: Write the expression value to the its position in the argument list.
                emitU8(0x89);
                emitU8(0x84);
                emitU8(0x24);
                emitU32(4 * argCount);

                if(ch == ')') {
                    break;
                }
                readCh();
            }
        }
        readCh();

        // Fill in the argument list size we reserved earlier.
        u32 argListSize = 4 * argCount;
        writeU32(argListSizeSlot, argListSize);

        // mov eax, 'argCount': Set the number of arguments for the call to 'argCount'.
        emitU8(0xB8);
        emitU32(argCount);

        // pop ebx: Pop the function pointer from the stack to eax.
        emitU8(0x5B);

        // call ebx: Call the function pointer in ebx.
        emitU8(0xFF);
        emitU8(0xD3);

        // The function return value is now stored in eax.
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
    if(logNegCount) {
        // Logical negation destroys the lvalueness of the result.
        ensureNotLValue(&isLValue);
    }
    while(logNegCount) {
        emitEaxLogicalNegation();
        --logNegCount;
    }

    return isLValue;
}

// Stack of compileExpression helper functions ordered by precedence. (The ones returning int
// return lvalueness similarly to createAtomicExpressions; others always set eax to the expression
// value like compileExpression.)
int compileExpressionImpl6() {
    return compileAtomicExpression();
}
int compileExpressionImpl5() {
    // Multiplication and division/remainder operator handling.

    int isLValue = compileExpressionImpl6();
    while(ch == '*' || ch == '/' || ch == '%') {
        u8 opCh = ch;

        ensureNotLValue(&isLValue);

        // push eax: Push the current value to the stack.
        emitU8(0x50);

        // Evaluate the next factor/divisor to eax.
        readCh();
        isLValue = compileExpressionImpl6();
        ensureNotLValue(&isLValue);

        // pop ebx: Pop the previous value to ebx.
        emitU8(0x5B);

        if(opCh == '*') {
            // mul ebx: Multiply eax by ebx, saving the low bits of the result to eax (and high bits to edx).
            emitU8(0xF7);
            emitU8(0xE3);
        } else {
            // mov ecx, eax: Copy divisor from eax to ecx.
            emitU8(0x89);
            emitU8(0xC1);

            // mov eax, ebx: Copy dividend from ebx to eax.
            emitU8(0x89);
            emitU8(0xD8);

            // mov edx, 0: Initialize edx to zero.
            emitU8(0xBA);
            emitU32(0);

            // div ecx: Divide dividend with low bits in eax and high bits set to zero in edx by divisor in ecx, saving quotient to eax and remainder to edx.
            emitU8(0xF7);
            emitU8(0xF1);

            if(opCh == '%') {
                // mov eax, edx: Copy remainder from edx to eax.
                emitU8(0x89);
                emitU8(0xD0);
            }
        }
    }
    return isLValue;
}
int compileExpressionImpl4() {
    // Addition and subtraction operator handling.

    int isLValue = compileExpressionImpl5();
    while(ch == '+' || ch == '-') {
        int isSubtraction = ch == '-';

        ensureNotLValue(&isLValue);

        // push eax: Push the current value to the stack.
        emitU8(0x50);

        // Evaluate the next term to eax.
        readCh();
        isLValue = compileExpressionImpl5();
        ensureNotLValue(&isLValue);

        if(isSubtraction) {
            // neg eax: Negate the term.
            emitU8(0xF7);
            emitU8(0xD8);
        }

        // pop ebx: Pop the previous value to ebx.
        emitU8(0x5B);

        // add eax, ebx: Combine the results, saving the total to eax.
        emitU8(0x01);
        emitU8(0xD8);
    }
    return isLValue;
}
int compileExpressionImpl3() {
    // Comparison operator handling.

    int isLValue = compileExpressionImpl4();
    if(ch == '=' || ch == '!' || ch == '<' || ch == '>') {
        if(ch == '=') {
            readCh();
            if(ch != '=') {
                // Only a single '=' sign, so this is an assignment.
                setPutBackCh('=');
                return isLValue;
            }
            setPutBackCh('=');
        }

        // We have a comparison expression; parse it as a comparison chain with short-circuiting.

        // push ['falseJumpPoint']: Push to the stack the address to jump to when comparison fails. ('falseJumpPoint' will be filled in later.)
        emitU8(0x68);
        u8* falseJumpPointSlot = memPos;
        emitPtr(NULL);

        while(ch == '=' || ch == '!' || ch == '<' || ch == '>') {
            u8 firstCh = ch;
            readCh();

            u8 cmpInstruction;
            if(ch == '=') {
                if(firstCh == '=') {
                    cmpInstruction = 0x74; // Equality comparison je.
                } else if(firstCh == '!') {
                    cmpInstruction = 0x75; // Non-equality comparison jne.
                } else if(firstCh == '<') {
                    cmpInstruction = 0x76; // Unsigned less-than-or-equal comparison jbe.
                } else {
                    cmpInstruction = 0x73; // Unsigned greater-than-or-equal comparison jbe.
                }
            } else {
                // 'ch' is not part of the comparison operator, so we need to put it back.
                setPutBackCh(firstCh);

                if(firstCh == '<') {
                    cmpInstruction = 0x72; // Unsigned less-than comparison jb.
                } else if(firstCh == '>') {
                    cmpInstruction = 0x77; // Unsigned greater-than comparison ja.
                } else {
                    fail("Expected '='.");
                }
            }

            ensureNotLValue(&isLValue);

            // push eax: Push the previous value to the stack.
            emitU8(0x50);

            // Evaluate the latest value to eax.
            readCh();
            isLValue = compileExpressionImpl4();
            ensureNotLValue(&isLValue);

            // pop ebx: Pop the previous value to ebx.
            emitU8(0x5B);

            // cmp ebx, eax: Compare the previous value to the latest value.
            emitU8(0x39);
            emitU8(0xC3);

            // "JCC true ; jmp [esp] ; true:": If the comparison result is not true (using conditional jump instruction JCC), jump to the false case jump address stored in stack.
            emitU8(cmpInstruction);
            emitU8(0x03);
            emitU8(0xFF);
            emitU8(0x24);
            emitU8(0x24);

            // At this point, the latest value is in eax, ready to be compared to the next value.
        }

        // If control gets here, the result for the whole comparison expression is true.

        // mov eax, 1: Set the comparison result to 1 (true).
        emitU8(0xB8);
        emitU32(1);

        // mov ebx, ['endJumpPoint']: Write the address to the end to ebx. ('endJumpPoint' will be filled in later.)
        emitU8(0xBB);
        u8* endJumpPointSlot = memPos;
        emitPtr(NULL);

        // jmp ebx: Jump to end.
        emitU8(0xFF);
        emitU8(0xE3);

        // Start the code for the false case; fill in its address to the slot that was reserved earlier.
        writePtr(falseJumpPointSlot, memPos);

        // mov eax, 0: Set the comparison result to 0 (false).
        emitU8(0xB8);
        emitU32(0);

        // Start the end code for the comparison; fill in its address to the slot that was reserved earlier.
        writePtr(endJumpPointSlot, memPos);

        // add esp, 4: Pop the false jump point from the stack.
        emitU8(0x83);
        emitU8(0xC4);
        emitU8(0x04);

        return 0;
    } else {
        // Not a comparison expression; return the value unmodified.
        return isLValue;
    }
}
int compileExpressionImpl2() {
    // Logical operator handling.

    int isLValue = compileExpressionImpl3();
    if(ch == '&' || ch == '|') {
        // We have a logical and/or expression; parse it as an and/or chain with short-circuiting.

        // Only allow cmpCh operators (i.e. no mixing && and ||) for simplicity.
        u8 cmpCh = ch;

        // Parse this as a '&&' expression, doing negations as required for the '||' case.

        // push ['falseJumpPoint']: Push to the stack the address to jump to when false is encountered. ('falseJumpPoint' will be filled in later.)
        emitU8(0x68);
        u8* falseJumpPointSlot = memPos;
        emitPtr(NULL);

        while(1) {
            ensureNotLValue(&isLValue);

            // Logically negate the value if we are compiling a '||' expression.
            if(cmpCh == '|') {
                emitEaxLogicalNegation();
            }

            // cmp eax, 0: Compare the value to 0.
            emitU8(0x83);
            emitU8(0xF8);
            emitU8(0x00);

            // "jne true ; jmp [esp] ; true:": If the value is zero, jump to the false case jump address stored in stack.
            emitU8(0x75);
            emitU8(0x03);
            emitU8(0xFF);
            emitU8(0x24);
            emitU8(0x24);

            if(ch != '&' && ch != '|') {
                // End of the chain.
                break;
            }
            if(ch != cmpCh) {
                fail("Expected '%c'", (char)cmpCh);
            }
            readCh();
            if(ch != cmpCh) {
                fail("Expected '%c'", (char)cmpCh);
            }

            // Evaluate the latest value to eax.
            readCh();
            isLValue = compileExpressionImpl3();
        }

        // If control gets here, the result for the whole expression is true.

        // mov eax, 1: Set the result to 1 (true).
        emitU8(0xB8);
        emitU32(1);

        // mov ebx, ['endJumpPoint']: Write the address to the end to ebx. ('endJumpPoint' will be filled in later.)
        emitU8(0xBB);
        u8* endJumpPointSlot = memPos;
        emitPtr(NULL);

        // jmp ebx: Jump to end.
        emitU8(0xFF);
        emitU8(0xE3);

        // Start the code for the false case; fill in its address to the slot that was reserved earlier.
        writePtr(falseJumpPointSlot, memPos);

        // mov eax, 0: Set the result to 0 (false).
        emitU8(0xB8);
        emitU32(0);

        // Start the end code for the expression; fill in its address to the slot that was reserved earlier.
        writePtr(endJumpPointSlot, memPos);

        // add esp, 4: Pop the false jump point from the stack.
        emitU8(0x83);
        emitU8(0xC4);
        emitU8(0x04);

        // Logically negate the value if we are compiling a '||' expression.
        if(cmpCh == '|') {
            emitEaxLogicalNegation();
        }

        return 0;
    } else {
        // Not a logical and/or expression; return the value unmodified.
        return isLValue;
    }
}

void compileExpressionImpl1() {
    // Assignment operator handling.

    int isLValue = compileExpressionImpl2();
    if(ch == '=') {
        if(!isLValue) {
            fail("Left side of assignment is not assignable.");
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
        ensureNotLValue(&isLValue);
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
                fail("Variable with given name already exists.");
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
                fail("Expected ';'.");
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

// Emit code for the default implementation for __builtin_wrongNumberOfArgumentsHandler which
// does nothing.
void emitDefaultWrongNumberOfArgumentsHandler() {
    // pop ebp: Pop the return address from the stack.
    emitU8(0x5D);

    // shl eax, 2: Multiply the number of arguments stored in eax by 4.
    emitU8(0xC1);
    emitU8(0xE0);
    emitU8(0x02);

    // add esp, eax: Discard the arguments from the stack.
    emitU8(0x01);
    emitU8(0xC4);

    // mov eax, 0: Set return value eax to zero.
    emitU8(0xB8);
    emitU32(0);

    // jmp ebp: Return by jumping to the return address.
    emitU8(0xFF);
    emitU8(0xE5);
}

// Emit a 4-byte builtin variable with given value, and add it to the name trie with given name
// relative to given base slot that must contain pointer to itself. Allocates trie nodes from
// builtinNameNodes starting from *pNextNodeIdx.
void initBuiltinVariable(const char* nameStr, u8* value, u8* baseSlot, size_t* pNextNodeIdx) {
    Name* name = &rootName;
    while(*nameStr != '\0') {
        if(*pNextNodeIdx >= sizeof(builtinNameNodes) / sizeof(builtinNameNodes[0])) {
            fail("Internal compiler error: the builtinNameNodes array is too small.");
        }
        Name** nameLink;
        if(!extendName((u8)*nameStr++, &name, &builtinNameNodes[*pNextNodeIdx], &nameLink)) {
            // New node allocated, advance node index.
            ++*pNextNodeIdx;
        }
    }

    if(name->hasVar) {
        fail("Internal compiler error: duplicate builtin name.");
    }
    name->hasVar = 1;
    name->varBaseSlot = baseSlot;
    name->varOffset = (u32)(memPos - baseSlot);

    emitPtr(value);
}

// Emits builtin variables/functions, initializing the name trie under rootName and the the global
// builtin*Ptr variables.
void initBuiltins() {
    // Emit builtin function implementations:
    u8* defaultWrongNumberOfArgumentsHandlerEntryPoint = memPos;
    emitDefaultWrongNumberOfArgumentsHandler();

    // Create a base pointer slot for builtin variables that contains its own address; thus we can
    // reference the variables with offsets 4, 8, ...
    u8* baseSlot = memPos;
    emitPtr(baseSlot);

    // Initialize the name trie.
    rootName.lastCh = 0;
    rootName.hasVar = 0;
    rootName.successor = NULL;
    rootName.neighbor = NULL;

    // Start adding builtin variables.
    size_t nextNodeIdx = 0;

    // Initial program break (where the program can start allocating memory). Will be filled in later.
    builtinInitialProgramBreakPtr = memPos;
    initBuiltinVariable("__builtin_initialProgramBreak", NULL, baseSlot, &nextNodeIdx);

    // Function called by all function literal implementations if the number of arguments is wrong
    // before going to infinite loop. Takes four arguments: the line and column where the function
    // is defined, the correct number of arguments and the given number of arguments. The default
    // implementation does nothing; the program can override it to e.g. print an error message or
    // exit the program.
    builtinWrongNumberOfArgumentsHandlerPtr = memPos;
    initBuiltinVariable("__builtin_wrongNumberOfArgumentsHandler", defaultWrongNumberOfArgumentsHandlerEntryPoint, baseSlot, &nextNodeIdx);
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
        fail("Unexpected character.");
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

    // mov eax, 0: Write the number of arguments argument (zero) to eax.
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
    // Emit glue code that will forward the call from the C program to our compiled main function.
    u8* entryPoint = memPos;
    u8* mainFuncPtrSlot = emitEntryPointGlue();

    // Initialize the name trie and the builtin variables.
    initBuiltins();

    // Compile the source code as the main function.
    u8* mainFunc = compileMainFunc();

    // Fill in the address to the main function call in the entry point glue code.
    writePtr(mainFuncPtrSlot, mainFunc);

    // Fill in the initial program break position.
    writePtr(builtinInitialProgramBreakPtr, memPos);

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
