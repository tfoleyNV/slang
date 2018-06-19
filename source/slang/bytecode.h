// bytecode.h
#ifndef SLANG_BYTECODE_H_INCLUDED
#define SLANG_BYTECODE_H_INCLUDED

// The file defines services for working with the Slang "bytecode" format,
// which is defined in the public `slang-bytecode.h` header.
//

#include "../core/basic.h"

#include "../../slang-bytecode.h"

namespace Slang
{

// The `slang-bytecode.h` file is intended to be used by clients who want
// to parse/inspect a compiled module without needing to depend on the
// Slang compiler implementation. As such, all of its definitions are
// plain C types without namespaces, etc.
//
// For convenience inside of the Slang compiler codebase, we will map
// the external names over to internal `typedef`s that drop the `Slang`
// prefix.
//

typedef SlangBCFileHeader               BCFileHeader;
typedef SlangBCSectionTableEntry        BCSectionTableEntry;
typedef SlangBCStringTableSectionHeader BCStringTableSectionHeader;
typedef SlangBCStringTableEntry         BCStringTableEntry;

typedef SlangBCReflectionSectonHeader   BCReflectionSectionHeader;
typedef SlangBCReflectionEntry          BCReflectionEntry;
typedef SlangBCReflectionNode           BCReflectionNode;
typedef SlangBCCode                     BCOp;
typedef SlangBCFunc                     BCFunc;
typedef SlangBCRegister                 BCReg;
typedef SlangBCConstant                 BCConst;
typedef SlangBCBlock                    BCBlock;
typedef SlangBCModule                   BCModule;

class CompileRequest;
void generateBytecodeForCompileRequest(
    CompileRequest* compileReq);

}


#endif
