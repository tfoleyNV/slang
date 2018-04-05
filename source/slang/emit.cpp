// emit.cpp
#include "emit.h"

#include "ir-insts.h"
#include "ir-ssa.h"
#include "ir-validate.h"
#include "legalize-types.h"
#include "lower-to-ir.h"
#include "mangle.h"
#include "name.h"
#include "syntax.h"
#include "type-layout.h"
#include "visitor.h"

#include <assert.h>

// Note: using C++ stdio just to get a locale-independent
// way to format floating-point values.
//
// TODO: Go ahead and implement teh Dragon4 algorithm so
// that we can print floating-point values to arbitrary
// precision as needed.
#include <sstream>

#ifdef _WIN32
#include <d3dcompiler.h>
#pragma warning(disable:4996)
#endif

namespace Slang {

struct ExtensionUsageTracker
{
    // Record the GLSL extnsions we have already emitted a `#extension` for
    HashSet<String> glslExtensionsRequired;
    StringBuilder glslExtensionRequireLines;

    ProfileVersion profileVersion = ProfileVersion::GLSL_110;
};

void requireGLSLExtension(
    ExtensionUsageTracker*  tracker,
    String const&           name)
{
    if (tracker->glslExtensionsRequired.Contains(name))
        return;

    StringBuilder& sb = tracker->glslExtensionRequireLines;

    sb.append("#extension ");
    sb.append(name);
    sb.append(" : require\n");

    tracker->glslExtensionsRequired.Add(name);
}

void requireGLSLVersionImpl(
    ExtensionUsageTracker*  tracker,
    ProfileVersion          version)
{
    // Check if this profile is newer
    if ((UInt)version > (UInt)tracker->profileVersion)
    {
        tracker->profileVersion = version;
    }
}


// Shared state for an entire emit session
struct SharedEmitContext
{
    // The entry point we are being asked to compile
    EntryPointRequest* entryPoint;

    // The layout for the entry point
    EntryPointLayout*   entryPointLayout;

    // The target language we want to generate code for
    CodeGenTarget target;

    // The final code generation target
    //
    // For example, `target` might be `GLSL`, while `finalTarget` might be `SPIRV`
    CodeGenTarget finalTarget;

    // The string of code we've built so far
    StringBuilder sb;

    // Current source position for tracking purposes...
    HumaneSourceLoc loc;
    HumaneSourceLoc nextSourceLocation;
    bool needToUpdateSourceLocation;

    // For GLSL output, we can't emit traidtional `#line` directives
    // with a file path in them, so we maintain a map that associates
    // each path with a unique integer, and then we output those
    // instead.
    Dictionary<String, int> mapGLSLSourcePathToID;
    int glslSourceIDCount = 0;

    // We only want to emit each `import`ed module one time, so
    // we maintain a set of already-emitted modules.
    HashSet<ModuleDecl*> modulesAlreadyEmitted;

    // We track the original global-scope layout so that we can
    // find layout information for `import`ed parameters.
    //
    // TODO: This will probably change if we represent imports
    // explicitly in the layout data.
    StructTypeLayout*   globalStructLayout;

    ProgramLayout*      programLayout;

    ModuleDecl*  program;

    bool                needHackSamplerForTexelFetch = false;

    ExtensionUsageTracker extensionUsageTracker;

    UInt uniqueIDCounter = 1;
    Dictionary<IRInst*, UInt> mapIRValueToID;
    Dictionary<Decl*, UInt> mapDeclToID;

    HashSet<String> irDeclsVisited;

    HashSet<String> irTupleTypes;

    // The "effective" profile that is being used to emit code,
    // combining information from the target and entry point.
    Profile effectiveProfile;


    // Are we at the start of a line, so that we should indent
    // before writing any other text?
    bool isAtStartOfLine = true;

    // How far are we indented?
    Int indentLevel = 0;
};

struct EmitContext
{
    // The shared context that is in effect
    SharedEmitContext* shared;
};

//

enum EPrecedence
{
#define LEFT(NAME)                      \
    kEPrecedence_##NAME##_Left,     \
    kEPrecedence_##NAME##_Right

#define RIGHT(NAME)                     \
    kEPrecedence_##NAME##_Right,    \
    kEPrecedence_##NAME##_Left

#define NONASSOC(NAME)                  \
    kEPrecedence_##NAME##_Left,     \
    kEPrecedence_##NAME##_Right = kEPrecedence_##NAME##_Left

    NONASSOC(None),
    LEFT(Comma),

    NONASSOC(General),

    RIGHT(Assign),

    RIGHT(Conditional),

    LEFT(Or),
    LEFT(And),
    LEFT(BitOr),
    LEFT(BitXor),
    LEFT(BitAnd),

    LEFT(Equality),
    LEFT(Relational),
    LEFT(Shift),
    LEFT(Additive),
    LEFT(Multiplicative),
    RIGHT(Prefix),
    LEFT(Postfix),
    NONASSOC(Atomic),
};

// Info on an op for emit purposes
struct EOpInfo
{
    char const* op;
    EPrecedence leftPrecedence;
    EPrecedence rightPrecedence;
};

#define OP(NAME, TEXT, PREC) \
static const EOpInfo kEOp_##NAME = { TEXT, kEPrecedence_##PREC##_Left, kEPrecedence_##PREC##_Right, }

OP(None,        "",     None);

OP(Comma,       ",",    Comma);

OP(General,     "",     General);

OP(Assign,      "=",    Assign);
OP(AddAssign,   "+=",   Assign);
OP(SubAssign,   "-=",   Assign);
OP(MulAssign,   "*=",   Assign);
OP(DivAssign,   "/=",   Assign);
OP(ModAssign,   "%=",   Assign);
OP(LshAssign,   "<<=",  Assign);
OP(RshAssign,   ">>=",  Assign);
OP(OrAssign,    "|=",   Assign);
OP(AndAssign,   "&=",   Assign);
OP(XorAssign,   "^=",   Assign);

OP(Conditional, "?:",   Conditional);

OP(Or,          "||",   Or);
OP(And,         "&&",   And);
OP(BitOr,       "|",    BitOr);
OP(BitXor,      "^",    BitXor);
OP(BitAnd,      "&",    BitAnd);

OP(Eql,         "==",   Equality);
OP(Neq,         "!=",   Equality);

OP(Less,        "<",    Relational);
OP(Greater,     ">",    Relational);
OP(Leq,         "<=",   Relational);
OP(Geq,         ">=",   Relational);

OP(Lsh,         "<<",   Shift);
OP(Rsh,         ">>",   Shift);

OP(Add,         "+",    Additive);
OP(Sub,         "-",    Additive);

OP(Mul,         "*",    Multiplicative);
OP(Div,         "/",    Multiplicative);
OP(Mod,         "%",    Multiplicative);

OP(Prefix,      "",     Prefix);
OP(Postfix,     "",     Postfix);
OP(Atomic,      "",     Atomic);

#undef OP

// Table to allow data-driven lookup of an op based on its
// name (to assist when outputting unchecked operator calls)
static EOpInfo const* const kInfixOpInfos[] =
{
    &kEOp_Comma,
    &kEOp_Assign,
    &kEOp_AddAssign,
    &kEOp_SubAssign,
    &kEOp_MulAssign,
    &kEOp_DivAssign,
    &kEOp_ModAssign,
    &kEOp_LshAssign,
    &kEOp_RshAssign,
    &kEOp_OrAssign,
    &kEOp_AndAssign,
    &kEOp_XorAssign,
    &kEOp_Or,
    &kEOp_And,
    &kEOp_BitOr,
    &kEOp_BitXor,
    &kEOp_BitAnd,
    &kEOp_Eql,
    &kEOp_Neq,
    &kEOp_Less,
    &kEOp_Greater,
    &kEOp_Leq,
    &kEOp_Geq,
    &kEOp_Lsh,
    &kEOp_Rsh,
    &kEOp_Add,
    &kEOp_Sub,
    &kEOp_Mul,
    &kEOp_Div,
    &kEOp_Mod,
};


//

// represents a declarator for use in emitting types
struct EDeclarator
{
    enum class Flavor
    {
        name,
        Array,
        UnsizedArray,
    };
    Flavor flavor;
    EDeclarator* next = nullptr;

    // Used for `Flavor::name`
    Name*       name;
    SourceLoc   loc;

    // Used for `Flavor::Array`
    IntVal* elementCount;
};

struct TypeEmitArg
{
    EDeclarator* declarator;
};

struct EmitVisitor
    : TypeVisitorWithArg<EmitVisitor, TypeEmitArg>
{
    EmitContext* context;
    EmitVisitor(EmitContext* context)
        : context(context)
    {}

    Session* getSession()
    {
        return context->shared->entryPoint->compileRequest->mSession;
    }

    // Low-level emit logic

    void emitRawTextSpan(char const* textBegin, char const* textEnd)
    {
        // TODO(tfoley): Need to make "corelib" not use `int` for pointer-sized things...
        auto len = textEnd - textBegin;
        context->shared->sb.Append(textBegin, len);
    }

    void emitRawText(char const* text)
    {
        emitRawTextSpan(text, text + strlen(text));
    }

    void emitTextSpan(char const* textBegin, char const* textEnd)
    {
        // Don't change anything given an empty string
        if(textBegin == textEnd)
            return;

        // If the source location has changed in a way that required update,
        // do it now!
        flushSourceLocationChange();

        // Note: we don't want to emit indentation on a line that is empty.
        // The logic in `Emit(textBegin, textEnd)` below will have broken
        // the text into lines, so we can simply check if a line consists
        // of just a newline.
        if(context->shared->isAtStartOfLine && *textBegin != '\n')
        {
            // We are about to emit text (other than a newline)
            // at the start of a line, so we will emit the proper
            // amount of indentation to keep things looking nice.
            context->shared->isAtStartOfLine = false;
            for(Int ii = 0; ii < context->shared->indentLevel; ++ii)
            {
                char const* indentString = "    ";
                size_t indentStringSize = strlen(indentString);
                emitRawTextSpan(indentString, indentString + indentStringSize);

                // We will also update our tracking location, just in
                // case other logic needs it.
                //
                // TODO: We may need to have a switch that controls whether
                // we are in "pretty-printing" mode or "follow the locations
                // in the original code" mode.
                context->shared->loc.column += indentStringSize;
            }
        }

        // Emit the raw text
        emitRawTextSpan(textBegin, textEnd);

        // Update our logical position
        auto len = int(textEnd - textBegin);
        context->shared->loc.column += len;
    }

    void indent()
    {
        context->shared->indentLevel++;
    }

    void dedent()
    {
        context->shared->indentLevel--;
    }

    void Emit(char const* textBegin, char const* textEnd)
    {
        char const* spanBegin = textBegin;
        char const* spanEnd = spanBegin;
        for(;;)
        {
            if(spanEnd == textEnd)
            {
                // We have a whole range of text waiting to be flushed
                emitTextSpan(spanBegin, spanEnd);
                return;
            }

            auto c = *spanEnd++;

            if( c == '\n' )
            {
                // At the end of a line, we need to update our tracking
                // information on code positions
                emitTextSpan(spanBegin, spanEnd);
                context->shared->loc.line++;
                context->shared->loc.column = 1;
                context->shared->isAtStartOfLine = true;

                // Start a new span for emit purposes
                spanBegin = spanEnd;
            }
        }
    }

    void Emit(char const* text)
    {
        Emit(text, text + strlen(text));
    }

    void emit(String const& text)
    {
        Emit(text.begin(), text.end());
    }

    void emit(UnownedStringSlice const& text)
    {
        Emit(text.begin(), text.end());
    }


    void emit(Name* name)
    {
        emit(getText(name));
    }

    void emit(NameLoc const& nameAndLoc)
    {
        advanceToSourceLocation(nameAndLoc.loc);
        emit(getText(nameAndLoc.name));
    }

    void emitName(
        Name*               name,
        SourceLoc const&    loc)
    {
        advanceToSourceLocation(loc);
        emit(name);
    }

    void emitName(NameLoc const& nameAndLoc)
    {
        emitName(nameAndLoc.name, nameAndLoc.loc);
    }

    void emitName(Name* name)
    {
        emitName(name, SourceLoc());
    }

    void emitName(
        Decl*               decl,
        SourceLoc const&    loc)
    {
        if(auto name = decl->getName())
            emitName(name, loc);

        Emit("_S");
        Emit(getID(decl));
    }

    void emitName(
        Decl*               decl)
    {
        emitName(decl, SourceLoc());
    }

    void Emit(IntegerLiteralValue value)
    {
        char buffer[32];
        sprintf(buffer, "%lld", (long long int)value);
        Emit(buffer);
    }


    void Emit(UInt value)
    {
        char buffer[32];
        sprintf(buffer, "%llu", (unsigned long long)(value));
        Emit(buffer);
    }

    void Emit(int value)
    {
        char buffer[16];
        sprintf(buffer, "%d", value);
        Emit(buffer);
    }

    void Emit(double value)
    {
        // There are a few different requirements here that we need to deal with:
        //
        // 1) We need to print somethign that is valid syntax in the target language
        //    (this means that hex floats are off the table for now)
        //
        // 2) We need our printing to be independent of the current global locale in C,
        //    so that we don't depend on the application leaving it as the default,
        //    and we also don't revert any changes they make.
        //    (this means that `sprintf` and friends are off the table)
        //
        // 3) We need to be sure that floating-point literals specified by the user will
        //    "round-trip" and turn into the same value when parsed back in. This means
        //    that we need to print a reasonable number of digits of precision.
        //
        // For right now, the easiest option that can balance these is to use
        // the C++ standard library `iostream`s, because they support an explicit locale,
        // and can (hopefully) print floating-point numbers accurately.
        //
        // Eventually, the right move here would be to implement proper floating-point
        // number formatting ourselves, but that would require extensive testing to
        // make sure we get it right.

        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream.setf(std::ios::fixed,std::ios::floatfield);
        stream.precision(20);
        stream << value;

        Emit(stream.str().c_str());
    }


    // Emit a `#line` directive to the output.
    // Doesn't udpate state of source-location tracking.
    void emitLineDirective(
        HumaneSourceLoc const& sourceLocation)
    {
        emitRawText("\n#line ");

        char buffer[16];
        sprintf(buffer, "%llu", (unsigned long long)sourceLocation.line);
        emitRawText(buffer);

        emitRawText(" ");

        bool shouldUseGLSLStyleLineDirective = false;

        auto mode = context->shared->entryPoint->compileRequest->lineDirectiveMode;
        switch (mode)
        {
        case LineDirectiveMode::None:
        case LineDirectiveMode::Default:
            SLANG_UNEXPECTED("should not be trying to emit '#line' directive");
            return;

        default:
            // To try to make the default behavior reasonable, we will
            // always use C-style line directives (to give the user
            // good source locations on error messages from downstream
            // compilers) *unless* they requested raw GLSL as the
            // output (in which case we want to maximize compatibility
            // with downstream tools).
            if (context->shared->finalTarget == CodeGenTarget::GLSL)
            {
                shouldUseGLSLStyleLineDirective = true;
            }
            break;

        case LineDirectiveMode::Standard:
            break;

        case LineDirectiveMode::GLSL:
            shouldUseGLSLStyleLineDirective = true;
            break;
        }

        if(shouldUseGLSLStyleLineDirective)
        {
            auto path = sourceLocation.getPath();

            // GLSL doesn't support the traditional form of a `#line` directive without
            // an extension. Rather than depend on that extension we will output
            // a directive in the traditional GLSL fashion.
            //
            // TODO: Add some kind of configuration where we require the appropriate
            // extension and then emit a traditional line directive.

            int id = 0;
            if(!context->shared->mapGLSLSourcePathToID.TryGetValue(path, id))
            {
                id = context->shared->glslSourceIDCount++;
                context->shared->mapGLSLSourcePathToID.Add(path, id);
            }

            sprintf(buffer, "%d", id);
            emitRawText(buffer);
        }
        else
        {
            // The simple case is to emit the path for the current source
            // location. We need to be a little bit careful with this,
            // because the path might include backslash characters if we
            // are on Windows, and we want to canonicalize those over
            // to forward slashes.
            //
            // TODO: Canonicalization like this should be done centrally
            // in a module that tracks source files.

            emitRawText("\"");
            for(auto c : sourceLocation.getPath())
            {
                char charBuffer[] = { c, 0 };
                switch(c)
                {
                default:
                    emitRawText(charBuffer);
                    break;

                // The incoming file path might use `/` and/or `\\` as
                // a directory separator. We want to canonicalize this.
                //
                // TODO: should probably canonicalize paths to not use backslash somewhere else
                // in the compilation pipeline...
                case '\\':
                    emitRawText("/");
                    break;
                }
            }
            emitRawText("\"");
        }

        emitRawText("\n");
    }

    // Emit a `#line` directive to the output, and also
    // ensure that source location tracking information
    // is correct based on the directive we just output.
    void emitLineDirectiveAndUpdateSourceLocation(
        HumaneSourceLoc const& sourceLocation)
    {
        emitLineDirective(sourceLocation);

        HumaneSourceLoc newLoc = sourceLocation;
        newLoc.column = 1;

        context->shared->loc = newLoc;
    }

    void emitLineDirectiveIfNeeded(
        HumaneSourceLoc const& sourceLocation)
    {
        // Don't do any of this work if the user has requested that we
        // not emit line directives.
        auto mode = context->shared->entryPoint->compileRequest->lineDirectiveMode;
        switch(mode)
        {
        case LineDirectiveMode::None:
        case LineDirectiveMode::Default:
            // Default behavior is to not emit line directives, since they
            // don't help readability much for IR-based output.
            return;

        default:
            break;
        }

        // Ignore invalid source locations
        if(sourceLocation.line <= 0)
            return;

        // If we are currently emitting code at a source location with
        // a differnet file or line, *or* if the source location is
        // somehow later on the line than what we want to emit,
        // then we need to emit a new `#line` directive.
        if(sourceLocation.path != context->shared->loc.path
            || sourceLocation.line != context->shared->loc.line
            || sourceLocation.column < context->shared->loc.column)
        {
            // Special case: if we are in the same file, and within a small number
            // of lines of the target location, then go ahead and output newlines
            // to get us caught up.
            enum { kSmallLineCount = 3 };
            auto lineDiff = sourceLocation.line - context->shared->loc.line;
            if(sourceLocation.path == context->shared->loc.path
                && sourceLocation.line > context->shared->loc.line
                && lineDiff <= kSmallLineCount)
            {
                for(int ii = 0; ii < lineDiff; ++ii )
                {
                    Emit("\n");
                }
                SLANG_RELEASE_ASSERT(sourceLocation.line == context->shared->loc.line);
            }
            else
            {
                // Go ahead and output a `#line` directive to get us caught up
                emitLineDirectiveAndUpdateSourceLocation(sourceLocation);
            }
        }
    }

    void advanceToSourceLocation(
        HumaneSourceLoc const& sourceLocation)
    {
        // Skip invalid locations
        if(sourceLocation.line <= 0)
            return;

        context->shared->needToUpdateSourceLocation = true;
        context->shared->nextSourceLocation = sourceLocation;
    }

    SourceManager* getSourceManager()
    {
        return context->shared->entryPoint->compileRequest->getSourceManager();
    }

    void advanceToSourceLocation(
        SourceLoc const& sourceLocation)
    {
        advanceToSourceLocation(getSourceManager()->getHumaneLoc(sourceLocation));
    }

    void flushSourceLocationChange()
    {
        if(!context->shared->needToUpdateSourceLocation)
            return;

        // Note: the order matters here, because trying to update
        // the source location may involve outputting text that
        // advances the location, and outputting text is what
        // triggers this flush operation.
        context->shared->needToUpdateSourceLocation = false;
        emitLineDirectiveIfNeeded(context->shared->nextSourceLocation);
    }

    DiagnosticSink* getSink()
    {
        return &context->shared->entryPoint->compileRequest->mSink;
    }

    //
    // Types
    //

    void Emit(RefPtr<IntVal> val)
    {
        if(auto constantIntVal = val.As<ConstantIntVal>())
        {
            Emit(constantIntVal->value);
        }
        else if(auto varRefVal = val.As<GenericParamIntVal>())
        {
            EmitDeclRef(varRefVal->declRef);
        }
        else
        {
            SLANG_DIAGNOSE_UNEXPECTED(getSink(), SourceLoc(), "unknown type of integer constant value");
        }
    }

    void EmitDeclarator(EDeclarator* declarator)
    {
        if (!declarator) return;

        Emit(" ");

        switch (declarator->flavor)
        {
        case EDeclarator::Flavor::name:
            emitName(declarator->name, declarator->loc);
            break;

        case EDeclarator::Flavor::Array:
            EmitDeclarator(declarator->next);
            Emit("[");
            if(auto elementCount = declarator->elementCount)
            {
                Emit(elementCount);
            }
            Emit("]");
            break;

        case EDeclarator::Flavor::UnsizedArray:
            EmitDeclarator(declarator->next);
            Emit("[]");
            break;

        default:
            SLANG_DIAGNOSE_UNEXPECTED(getSink(), SourceLoc(), "unknown declarator flavor");
            break;
        }
    }

    void emitGLSLTypePrefix(
        RefPtr<Type>  type)
    {
        if(auto basicElementType = type->As<BasicExpressionType>())
        {
            switch (basicElementType->baseType)
            {
            case BaseType::Float:
                // no prefix
                break;

            case BaseType::Int:		Emit("i");		break;
            case BaseType::UInt:	Emit("u");		break;
            case BaseType::Bool:	Emit("b");		break;
            case BaseType::Double:	Emit("d");		break;
            default:
                SLANG_DIAGNOSE_UNEXPECTED(getSink(), SourceLoc(), "unhandled GLSL type prefix");
                break;
            }
        }
        else if(auto vectorType = type->As<VectorExpressionType>())
        {
            emitGLSLTypePrefix(vectorType->elementType);
        }
        else if(auto matrixType = type->As<MatrixExpressionType>())
        {
            emitGLSLTypePrefix(matrixType->getElementType());
        }
        else
        {
            SLANG_DIAGNOSE_UNEXPECTED(getSink(), SourceLoc(), "unhandled GLSL type prefix");
        }
    }

    void emitHLSLTextureType(
        RefPtr<TextureTypeBase> texType)
    {
        switch(texType->getAccess())
        {
        case SLANG_RESOURCE_ACCESS_READ:
            break;

        case SLANG_RESOURCE_ACCESS_READ_WRITE:
            Emit("RW");
            break;

        case SLANG_RESOURCE_ACCESS_RASTER_ORDERED:
            Emit("RasterizerOrdered");
            break;

        case SLANG_RESOURCE_ACCESS_APPEND:
            Emit("Append");
            break;

        case SLANG_RESOURCE_ACCESS_CONSUME:
            Emit("Consume");
            break;

        default:
            SLANG_DIAGNOSE_UNEXPECTED(getSink(), SourceLoc(), "unhandled resource access mode");
            break;
        }

        switch (texType->GetBaseShape())
        {
        case TextureFlavor::Shape::Shape1D:		Emit("Texture1D");		break;
        case TextureFlavor::Shape::Shape2D:		Emit("Texture2D");		break;
        case TextureFlavor::Shape::Shape3D:		Emit("Texture3D");		break;
        case TextureFlavor::Shape::ShapeCube:	Emit("TextureCube");	break;
        case TextureFlavor::Shape::ShapeBuffer:  Emit("Buffer");         break;
        default:
            SLANG_DIAGNOSE_UNEXPECTED(getSink(), SourceLoc(), "unhandled resource shape");
            break;
        }

        if (texType->isMultisample())
        {
            Emit("MS");
        }
        if (texType->isArray())
        {
            Emit("Array");
        }
        Emit("<");
        EmitType(texType->elementType);
        Emit(" >");
    }

    void emitGLSLTextureOrTextureSamplerType(
        RefPtr<TextureTypeBase> type,
        char const*             baseName)
    {
        emitGLSLTypePrefix(type->elementType);

        Emit(baseName);
        switch (type->GetBaseShape())
        {
        case TextureFlavor::Shape::Shape1D:		Emit("1D");		break;
        case TextureFlavor::Shape::Shape2D:		Emit("2D");		break;
        case TextureFlavor::Shape::Shape3D:		Emit("3D");		break;
        case TextureFlavor::Shape::ShapeCube:	Emit("Cube");	break;
        case TextureFlavor::Shape::ShapeBuffer:	Emit("Buffer");	break;
        default:
            SLANG_DIAGNOSE_UNEXPECTED(getSink(), SourceLoc(), "unhandled resource shape");
            break;
        }

        if (type->isMultisample())
        {
            Emit("MS");
        }
        if (type->isArray())
        {
            Emit("Array");
        }
    }

    void emitGLSLTextureType(
        RefPtr<TextureType> texType)
    {
        switch(texType->getAccess())
        {
        case SLANG_RESOURCE_ACCESS_READ_WRITE:
        case SLANG_RESOURCE_ACCESS_RASTER_ORDERED:
            emitGLSLTextureOrTextureSamplerType(texType, "image");
            break;

        default:
            emitGLSLTextureOrTextureSamplerType(texType, "texture");
            break;
        }
    }

    void emitGLSLTextureSamplerType(
        RefPtr<TextureSamplerType>  type)
    {
        emitGLSLTextureOrTextureSamplerType(type, "sampler");
    }

    void emitGLSLImageType(
        RefPtr<GLSLImageType>   type)
    {
        emitGLSLTextureOrTextureSamplerType(type, "image");
    }

    void emitTextureType(
        RefPtr<TextureType> texType)
    {
        switch(context->shared->target)
        {
        case CodeGenTarget::HLSL:
            emitHLSLTextureType(texType);
            break;

        case CodeGenTarget::GLSL:
            emitGLSLTextureType(texType);
            break;

        default:
            SLANG_DIAGNOSE_UNEXPECTED(getSink(), SourceLoc(), "unhandled code generation target");
            break;
        }
    }

    void emitTextureSamplerType(
        RefPtr<TextureSamplerType>  type)
    {
        switch(context->shared->target)
        {
        case CodeGenTarget::GLSL:
            emitGLSLTextureSamplerType(type);
            break;

        default:
            SLANG_DIAGNOSE_UNEXPECTED(getSink(), SourceLoc(), "this target should see combined texture-sampler types");
            break;
        }
    }

    void emitImageType(
        RefPtr<GLSLImageType>   type)
    {
        switch(context->shared->target)
        {
        case CodeGenTarget::HLSL:
            emitHLSLTextureType(type);
            break;

        case CodeGenTarget::GLSL:
            emitGLSLImageType(type);
            break;

        default:
            SLANG_DIAGNOSE_UNEXPECTED(getSink(), SourceLoc(), "this target should see GLSL image types");
            break;
        }
    }

    void emitTypeImpl(RefPtr<Type> type, EDeclarator* declarator)
    {
        TypeEmitArg arg;
        arg.declarator = declarator;

        TypeVisitorWithArg::dispatch(type, arg);
    }

#define UNEXPECTED(NAME) \
    void visit##NAME(NAME*, TypeEmitArg const& arg) \
    { Emit(#NAME); EmitDeclarator(arg.declarator); }

    UNEXPECTED(ErrorType);
    UNEXPECTED(OverloadGroupType);
    UNEXPECTED(FuncType);
    UNEXPECTED(TypeType);
    UNEXPECTED(GenericDeclRefType);
    UNEXPECTED(InitializerListType);

    UNEXPECTED(IRBasicBlockType);
    UNEXPECTED(PtrType);

#undef UNEXPECTED

    void visitNamedExpressionType(NamedExpressionType* type, TypeEmitArg const& arg)
    {
        // We will always emit the actual type referenced by
        // a named type declaration, rather than try to produce
        // equivalent `typedef` declarations in the output.

        emitTypeImpl(GetType(type->declRef), arg.declarator);
    }

    void visitBasicExpressionType(BasicExpressionType* basicType, TypeEmitArg const& arg)
    {
        auto declarator = arg.declarator;
        switch (basicType->baseType)
        {
        case BaseType::Void:	Emit("void");		break;
        case BaseType::Int:		Emit("int");		break;
        case BaseType::Float:	Emit("float");		break;
        case BaseType::UInt:	Emit("uint");		break;
        case BaseType::Bool:	Emit("bool");		break;
        case BaseType::Double:	Emit("double");		break;
        default:
            SLANG_DIAGNOSE_UNEXPECTED(getSink(), SourceLoc(), "unhandled scalar type");
            break;
        }

        EmitDeclarator(declarator);
    }

    void visitVectorExpressionType(VectorExpressionType* vecType, TypeEmitArg const& arg)
    {
        auto declarator = arg.declarator;
        switch(context->shared->target)
        {
        case CodeGenTarget::GLSL:
        case CodeGenTarget::GLSL_Vulkan:
        case CodeGenTarget::GLSL_Vulkan_OneDesc:
            {
                emitGLSLTypePrefix(vecType->elementType);
                Emit("vec");
                Emit(vecType->elementCount);
            }
            break;

        case CodeGenTarget::HLSL:
            // TODO(tfoley): should really emit these with sugar
            Emit("vector<");
            EmitType(vecType->elementType);
            Emit(",");
            Emit(vecType->elementCount);
            Emit(">");
            break;

        default:
            SLANG_DIAGNOSE_UNEXPECTED(getSink(), SourceLoc(), "unhandled code generation target");
            break;
        }

        EmitDeclarator(declarator);
    }

    void visitMatrixExpressionType(MatrixExpressionType* matType, TypeEmitArg const& arg)
    {
        auto declarator = arg.declarator;
        switch(context->shared->target)
        {
        case CodeGenTarget::GLSL:
        case CodeGenTarget::GLSL_Vulkan:
        case CodeGenTarget::GLSL_Vulkan_OneDesc:
            {
                emitGLSLTypePrefix(matType->getElementType());
                Emit("mat");
                Emit(matType->getRowCount());
                // TODO(tfoley): only emit the next bit
                // for non-square matrix
                Emit("x");
                Emit(matType->getColumnCount());
            }
            break;

        case CodeGenTarget::HLSL:
            // TODO(tfoley): should really emit these with sugar
            Emit("matrix<");
            EmitType(matType->getElementType());
            Emit(",");
            Emit(matType->getRowCount());
            Emit(",");
            Emit(matType->getColumnCount());
            Emit("> ");
            break;

        default:
            SLANG_DIAGNOSE_UNEXPECTED(getSink(), SourceLoc(), "unhandled code generation target");
            break;
        }

        EmitDeclarator(declarator);
    }

    void visitTextureType(TextureType* texType, TypeEmitArg const& arg)
    {
        auto declarator = arg.declarator;
        emitTextureType(texType);
        EmitDeclarator(declarator);
    }

    void visitTextureSamplerType(TextureSamplerType* textureSamplerType, TypeEmitArg const& arg)
    {
        auto declarator = arg.declarator;
        emitTextureSamplerType(textureSamplerType);
        EmitDeclarator(declarator);
    }

    void visitGLSLImageType(GLSLImageType* imageType, TypeEmitArg const& arg)
    {
        auto declarator = arg.declarator;
        emitImageType(imageType);
        EmitDeclarator(declarator);
    }

    void visitSamplerStateType(SamplerStateType* samplerStateType, TypeEmitArg const& arg)
    {
        auto declarator = arg.declarator;
        switch(context->shared->target)
        {
        case CodeGenTarget::HLSL:
        default:
            switch (samplerStateType->flavor)
            {
            case SamplerStateFlavor::SamplerState:			Emit("SamplerState");			break;
            case SamplerStateFlavor::SamplerComparisonState:	Emit("SamplerComparisonState");	break;
            default:
                SLANG_DIAGNOSE_UNEXPECTED(getSink(), SourceLoc(), "unhandled sampler state flavor");
                break;
            }
            break;

        case CodeGenTarget::GLSL:
            switch (samplerStateType->flavor)
            {
            case SamplerStateFlavor::SamplerState:			Emit("sampler");		break;
            case SamplerStateFlavor::SamplerComparisonState:	Emit("samplerShadow");	break;
            default:
                SLANG_DIAGNOSE_UNEXPECTED(getSink(), SourceLoc(), "unhandled sampler state flavor");
                break;
            }
            break;
            break;
        }

        EmitDeclarator(declarator);
    }

    void visitDeclRefType(DeclRefType* declRefType, TypeEmitArg const& arg)
    {
        auto declarator = arg.declarator;
        EmitDeclRef(declRefType->declRef);
        EmitDeclarator(declarator);
    }

    void visitArrayExpressionType(ArrayExpressionType* arrayType, TypeEmitArg const& arg)
    {
        auto declarator = arg.declarator;

        EDeclarator arrayDeclarator;
        arrayDeclarator.next = declarator;

        if(arrayType->ArrayLength)
        {
            arrayDeclarator.flavor = EDeclarator::Flavor::Array;
            arrayDeclarator.elementCount = arrayType->ArrayLength.Ptr();
        }
        else
        {
            arrayDeclarator.flavor = EDeclarator::Flavor::UnsizedArray;
        }


        emitTypeImpl(arrayType->baseType, &arrayDeclarator);
    }

    void visitRateQualifiedType(RateQualifiedType* type, TypeEmitArg const& arg)
    {
        emitTypeImpl(type->valueType, arg.declarator);
    }

    void visitConstExprRate(ConstExprRate* /*rate*/, TypeEmitArg const& /*arg*/)
    {
        // This should never appear as a data type
        SLANG_UNEXPECTED("Rates not expected during emit");
    }

    void visitGroupSharedType(GroupSharedType* type, TypeEmitArg const& arg)
    {
        switch(getTarget(context))
        {
        case CodeGenTarget::HLSL:
            Emit("groupshared ");
            break;

        case CodeGenTarget::GLSL:
            Emit("shared ");
            break;

        default:
            break;
        }
        emitTypeImpl(type->valueType, arg.declarator);
    }

    void EmitType(
        RefPtr<Type>        type,
        SourceLoc const&    typeLoc,
        Name*               name,
        SourceLoc const&    nameLoc)
    {
        advanceToSourceLocation(typeLoc);

        EDeclarator nameDeclarator;
        nameDeclarator.flavor = EDeclarator::Flavor::name;
        nameDeclarator.name = name;
        nameDeclarator.loc = nameLoc;
        emitTypeImpl(type, &nameDeclarator);
    }

    void EmitType(RefPtr<Type> type, Name* name)
    {
        EmitType(type, SourceLoc(), name, SourceLoc());
    }

    void EmitType(RefPtr<Type> type, String const& name)
    {
        // HACK: the rest of the code wants a `Name`,
        // so we'll create one for a bit...
        Name tempName;
        tempName.text = name;

        EmitType(type, SourceLoc(), &tempName, SourceLoc());
    }


    void EmitType(RefPtr<Type> type)
    {
        emitTypeImpl(type, nullptr);
    }

    //
    // Expressions
    //

    bool MaybeEmitParens(EOpInfo& outerPrec, EOpInfo prec)
    {
        bool needParens = (prec.leftPrecedence <= outerPrec.leftPrecedence)
            || (prec.rightPrecedence <= outerPrec.rightPrecedence);

        if (needParens)
        {
            Emit("(");

            outerPrec = kEOp_None;
        }
        return needParens;
    }

    bool isTargetIntrinsicModifierApplicable(
        String const& targetName)
    {
        switch(context->shared->target)
        {
        default:
            SLANG_DIAGNOSE_UNEXPECTED(getSink(), SourceLoc(), "unhandled code generation target");
            return false;

        case CodeGenTarget::GLSL: return targetName == "glsl";
        case CodeGenTarget::HLSL: return targetName == "hlsl";
        }
    }

    bool isTargetIntrinsicModifierApplicable(
        IRTargetIntrinsicDecoration*    decoration)
    {
        auto targetName = decoration->targetName;

        // If no target name was specified, then the modifier implicitly
        // applies to all targets.
        if(targetName.Length() == 0)
            return true;

        return isTargetIntrinsicModifierApplicable(targetName);
    }

    void emitStringLiteral(
        String const&   value)
    {
        emit("\"");
        for (auto c : value)
        {
            // TODO: This needs a more complete implementation,
            // especially if we want to support Unicode.

            char buffer[] = { c, 0 };
            switch (c)
            {
            default:
                emit(buffer);
                break;

            case '\"': emit("\\\"");
            case '\'': emit("\\\'");
            case '\\': emit("\\\\");
            case '\n': emit("\\n");
            case '\r': emit("\\r");
            case '\t': emit("\\t");
            }
        }
        emit("\"");
    }

    EOpInfo leftSide(EOpInfo const& outerPrec, EOpInfo const& prec)
    {
        EOpInfo result;
        result.leftPrecedence = outerPrec.leftPrecedence;
        result.rightPrecedence = prec.leftPrecedence;
        return result;
    }

    EOpInfo rightSide(EOpInfo const& prec, EOpInfo const& outerPrec)
    {
        EOpInfo result;
        result.leftPrecedence = prec.rightPrecedence;
        result.rightPrecedence = outerPrec.rightPrecedence;
        return result;
    }

    void requireGLSLExtension(String const& name)
    {
        Slang::requireGLSLExtension(&context->shared->extensionUsageTracker, name);
    }

    void requireGLSLVersion(ProfileVersion version)
    {
        if (context->shared->target != CodeGenTarget::GLSL)
            return;

        Slang::requireGLSLVersionImpl(&context->shared->extensionUsageTracker, version);
    }

    void requireGLSLVersion(int version)
    {
        switch (version)
        {
    #define CASE(NUMBER) \
        case NUMBER: requireGLSLVersion(ProfileVersion::GLSL_##NUMBER); break

        CASE(110);
        CASE(120);
        CASE(130);
        CASE(140);
        CASE(150);
        CASE(330);
        CASE(400);
        CASE(410);
        CASE(420);
        CASE(430);
        CASE(440);
        CASE(450);

    #undef CASE
        }
    }

    void setSampleRateFlag()
    {
        context->shared->entryPointLayout->flags |= EntryPointLayout::Flag::usesAnySampleRateInput;
    }

    void doSampleRateInputCheck(Name* name)
    {
        auto text = getText(name);
        if (text == "gl_SampleID")
        {
            setSampleRateFlag();
        }
    }

    //
    // Declaration References
    //

    void EmitVal(RefPtr<Val> val)
    {
        if (auto type = val.As<Type>())
        {
            EmitType(type);
        }
        else if (auto intVal = val.As<IntVal>())
        {
            Emit(intVal);
        }
        else
        {
            // Note(tfoley): ignore unhandled cases for semantics for now...
    //		assert(!"unimplemented");
        }
    }

    bool isBuiltinDecl(Decl* decl)
    {
        for (auto dd = decl; dd; dd = dd->ParentDecl)
        {
            if (dd->FindModifier<FromStdLibModifier>())
                return true;
        }
        return false;
    }

    void EmitDeclRef(DeclRef<Decl> declRef)
    {
        // When refering to anything other than a builtin, use its IR-facing name
        if (!isBuiltinDecl(declRef.getDecl()))
        {
            emit(getIRName(declRef));
            return;
        }


        // TODO: need to qualify a declaration name based on parent scopes/declarations

        // Emit the name for the declaration itself
        emitName(declRef.GetName());

        // If the declaration is nested directly in a generic, then
        // we need to output the generic arguments here
        auto parentDeclRef = declRef.GetParent();
        if (auto genericDeclRef = parentDeclRef.As<GenericDecl>())
        {
            // Only do this for declarations of appropriate flavors
            if(auto funcDeclRef = declRef.As<FunctionDeclBase>())
            {
                // Don't emit generic arguments for functions, because HLSL doesn't allow them
                return;
            }

            GenericSubstitution* subst = declRef.substitutions.genericSubstitutions;
            if (!subst)
                return;

            Emit("<");
            UInt argCount = subst->args.Count();
            for (UInt aa = 0; aa < argCount; ++aa)
            {
                if (aa != 0) Emit(",");
                EmitVal(subst->args[aa]);
            }
            Emit(" >");
        }

    }

    typedef unsigned int ESemanticMask;
    enum
    {
        kESemanticMask_None = 0,

        kESemanticMask_NoPackOffset = 1 << 0,

        kESemanticMask_Default = kESemanticMask_NoPackOffset,
    };

    void EmitSemantic(RefPtr<HLSLSemantic> semantic, ESemanticMask /*mask*/)
    {
        if (auto simple = semantic.As<HLSLSimpleSemantic>())
        {
            Emit(" : ");
            emit(simple->name.Content);
        }
        else if(auto registerSemantic = semantic.As<HLSLRegisterSemantic>())
        {
            // Don't print out semantic from the user, since we are going to print the same thing our own way...
        }
        else if(auto packOffsetSemantic = semantic.As<HLSLPackOffsetSemantic>())
        {
            // Don't print out semantic from the user, since we are going to print the same thing our own way...
        }
        else
        {
            SLANG_DIAGNOSE_UNEXPECTED(getSink(), semantic->loc, "unhandled kind of semantic");
        }
    }


    void EmitSemantics(RefPtr<Decl> decl, ESemanticMask mask = kESemanticMask_Default )
    {
        // Don't emit semantics if we aren't translating down to HLSL
        switch (context->shared->target)
        {
        case CodeGenTarget::HLSL:
            break;

        default:
            return;
        }

        for (auto mod = decl->modifiers.first; mod; mod = mod->next)
        {
            auto semantic = mod.As<HLSLSemantic>();
            if (!semantic)
                continue;

            EmitSemantic(semantic, mask);
        }
    }

    // A chain of variables to use for emitting semantic/layout info
    struct EmitVarChain
    {
        VarLayout*      varLayout;
        EmitVarChain*   next;

        EmitVarChain()
            : varLayout(0)
            , next(0)
        {}

        EmitVarChain(VarLayout* varLayout)
            : varLayout(varLayout)
            , next(0)
        {}

        EmitVarChain(VarLayout* varLayout, EmitVarChain* next)
            : varLayout(varLayout)
            , next(next)
        {}
    };

    UInt getBindingOffset(EmitVarChain* chain, LayoutResourceKind kind)
    {
        UInt offset = 0;
        for(auto cc = chain; cc; cc = cc->next)
        {
            if(auto resInfo = cc->varLayout->FindResourceInfo(kind))
            {
                offset += resInfo->index;
            }
        }
        return offset;
    }

    UInt getBindingSpace(EmitVarChain* chain, LayoutResourceKind kind)
    {
        UInt space = 0;
        for(auto cc = chain; cc; cc = cc->next)
        {
            auto varLayout = cc->varLayout;
            if(auto resInfo = varLayout->FindResourceInfo(kind))
            {
                space += resInfo->space;
            }
            if(auto resInfo = varLayout->FindResourceInfo(LayoutResourceKind::RegisterSpace))
            {
                space += resInfo->index;
            }
        }
        return space;
    }

    // Emit a single `regsiter` semantic, as appropriate for a given resource-type-specific layout info
    void emitHLSLRegisterSemantic(
        LayoutResourceKind  kind,
        EmitVarChain*       chain,
        // Keyword to use in the uniform case (`register` for globals, `packoffset` inside a `cbuffer`)
        char const* uniformSemanticSpelling = "register")
    {
        if(!chain)
            return;
        if(!chain->varLayout->FindResourceInfo(kind))
            return;

        UInt index = getBindingOffset(chain, kind);
        UInt space = getBindingSpace(chain, kind);

        switch(kind)
        {
        case LayoutResourceKind::Uniform:
            {
                UInt offset = index;

                // The HLSL `c` register space is logically grouped in 16-byte registers,
                // while we try to traffic in byte offsets. That means we need to pick
                // a register number, based on the starting offset in 16-byte register
                // units, and then a "component" within that register, based on 4-byte
                // offsets from there. We cannot support more fine-grained offsets than that.

                Emit(" : ");
                Emit(uniformSemanticSpelling);
                Emit("(c");

                // Size of a logical `c` register in bytes
                auto registerSize = 16;

                // Size of each component of a logical `c` register, in bytes
                auto componentSize = 4;

                size_t startRegister = offset / registerSize;
                Emit(int(startRegister));

                size_t byteOffsetInRegister = offset % registerSize;

                // If this field doesn't start on an even register boundary,
                // then we need to emit additional information to pick the
                // right component to start from
                if (byteOffsetInRegister != 0)
                {
                    // The value had better occupy a whole number of components.
                    SLANG_RELEASE_ASSERT(byteOffsetInRegister % componentSize == 0);

                    size_t startComponent = byteOffsetInRegister / componentSize;

                    static const char* kComponentNames[] = {"x", "y", "z", "w"};
                    Emit(".");
                    Emit(kComponentNames[startComponent]);
                }
                Emit(")");
            }
            break;

        case LayoutResourceKind::RegisterSpace:
        case LayoutResourceKind::GenericResource:
            // ignore
            break;
        default:
            {
                Emit(" : register(");
                switch( kind )
                {
                case LayoutResourceKind::ConstantBuffer:
                    Emit("b");
                    break;
                case LayoutResourceKind::ShaderResource:
                    Emit("t");
                    break;
                case LayoutResourceKind::UnorderedAccess:
                    Emit("u");
                    break;
                case LayoutResourceKind::SamplerState:
                    Emit("s");
                    break;
                default:
                    SLANG_DIAGNOSE_UNEXPECTED(getSink(), SourceLoc(), "unhandled HLSL register type");
                    break;
                }
                Emit(index);
                if(space)
                {
                    Emit(", space");
                    Emit(space);
                }
                Emit(")");
            }
        }
    }

    // Emit all the `register` semantics that are appropriate for a particular variable layout
    void emitHLSLRegisterSemantics(
        EmitVarChain*       chain,
        char const*         uniformSemanticSpelling = "register")
    {
        if (!chain) return;

        auto layout = chain->varLayout;

        switch( context->shared->target )
        {
        default:
            return;

        case CodeGenTarget::HLSL:
            break;
        }

        for( auto rr : layout->resourceInfos )
        {
            emitHLSLRegisterSemantic(rr.kind, chain, uniformSemanticSpelling);
        }
    }

    void emitHLSLRegisterSemantics(
        VarLayout*  varLayout,
        char const* uniformSemanticSpelling = "register")
    {
        if(!varLayout)
            return;

        EmitVarChain chain(varLayout);
        emitHLSLRegisterSemantics(&chain, uniformSemanticSpelling);
    }

    void emitHLSLParameterGroupFieldLayoutSemantics(
        EmitVarChain*       chain)
    {
        if(!chain)
            return;

        auto layout = chain->varLayout;
        for( auto rr : layout->resourceInfos )
        {
            emitHLSLRegisterSemantic(rr.kind, chain, "packoffset");
        }
    }


    void emitHLSLParameterGroupFieldLayoutSemantics(
        RefPtr<VarLayout>   fieldLayout,
        EmitVarChain*       inChain)
    {
        EmitVarChain chain(fieldLayout, inChain);
        emitHLSLParameterGroupFieldLayoutSemantics(&chain);
    }

    void emitGLSLLayoutQualifier(
        LayoutResourceKind  kind,
        EmitVarChain*       chain)
    {
        if(!chain)
            return;
        if(!chain->varLayout->FindResourceInfo(kind))
            return;

        UInt index = getBindingOffset(chain, kind);
        UInt space = getBindingSpace(chain, kind);
        switch(kind)
        {
        case LayoutResourceKind::Uniform:
            {
                // Explicit offsets require a GLSL extension (which
                // is not universally supported, it seems) or a new
                // enough GLSL version (which we don't want to
                // universall require), so for right now we
                // won't actually output explicit offsets for uniform
                // shader parameters.
                //
                // TODO: We should fix this so that we skip any
                // extra work for parameters that are laid out as
                // expected by the default rules, but do *something*
                // for parameters that need non-default layout.
                //
                // Using the `GL_ARB_enhanced_layouts` feature is one
                // option, but we should also be able to do some
                // things by introducing padding into the declaration
                // (padding insertion would probably be best done at
                // the IR level).
                bool useExplicitOffsets = false;
                if (useExplicitOffsets)
                {
                    requireGLSLExtension("GL_ARB_enhanced_layouts");

                    Emit("layout(offset = ");
                    Emit(index);
                    Emit(")\n");
                }
            }
            break;

        case LayoutResourceKind::VertexInput:
        case LayoutResourceKind::FragmentOutput:
            Emit("layout(location = ");
            Emit(index);
            Emit(")\n");
            break;

        case LayoutResourceKind::SpecializationConstant:
            Emit("layout(constant_id = ");
            Emit(index);
            Emit(")\n");
            break;

        case LayoutResourceKind::ConstantBuffer:
        case LayoutResourceKind::ShaderResource:
        case LayoutResourceKind::UnorderedAccess:
        case LayoutResourceKind::SamplerState:
        case LayoutResourceKind::DescriptorTableSlot:
            Emit("layout(binding = ");
            Emit(index);
            if(space)
            {
                Emit(", set = ");
                Emit(space);
            }
            Emit(")\n");
            break;

        case LayoutResourceKind::PushConstantBuffer:
            Emit("layout(push_constant)\n");
            break;

        }
    }

    void emitGLSLLayoutQualifiers(
        RefPtr<VarLayout>               layout,
        EmitVarChain*                   inChain,
        LayoutResourceKind              filter = LayoutResourceKind::None)
    {
        if(!layout) return;

        switch( context->shared->target )
        {
        default:
            return;

        case CodeGenTarget::GLSL:
            break;
        }

        EmitVarChain chain(layout, inChain);

        for( auto info : layout->resourceInfos )
        {
            // Skip info that doesn't match our filter
            if (filter != LayoutResourceKind::None
                && filter != info.kind)
            {
                continue;
            }

            emitGLSLLayoutQualifier(info.kind, &chain);
        }
    }


    void emitGLSLVersionDirective(
        ModuleDecl*  /*program*/)
    {
        auto effectiveProfile = context->shared->effectiveProfile;
        if(effectiveProfile.getFamily() == ProfileFamily::GLSL)
        {
            requireGLSLVersion(effectiveProfile.GetVersion());
        }

        // HACK: We aren't picking GLSL versions carefully right now,
        // and so we might end up only requiring the initial 1.10 version,
        // even though even basic functionality needs a higher version.
        //
        // For now, we'll work around this by just setting the minimum required
        // version to a high one:
        //
        // TODO: Either correctly compute a minimum required version, or require
        // the user to specify a version as part of the target.
        requireGLSLVersionImpl(&context->shared->extensionUsageTracker, ProfileVersion::GLSL_450);

        auto requiredProfileVersion = context->shared->extensionUsageTracker.profileVersion;
        switch (requiredProfileVersion)
        {
#define CASE(TAG, VALUE)    \
        case ProfileVersion::TAG: Emit("#version " #VALUE "\n"); return

        CASE(GLSL_110, 110);
        CASE(GLSL_120, 120);
        CASE(GLSL_130, 130);
        CASE(GLSL_140, 140);
        CASE(GLSL_150, 150);
        CASE(GLSL_330, 330);
        CASE(GLSL_400, 400);
        CASE(GLSL_410, 410);
        CASE(GLSL_420, 420);
        CASE(GLSL_430, 430);
        CASE(GLSL_440, 440);
        CASE(GLSL_450, 450);
#undef CASE

        default:
            break;
        }

        // No information is available for us to guess a profile,
        // so it seems like we need to pick one out of thin air.
        //
        // Ideally we should infer a minimum required version based
        // on the constructs we have seen used in the user's code
        //
        // For now we just fall back to a reasonably recent version.

        Emit("#version 420\n");
    }

    void emitGLSLPreprocessorDirectives(
        RefPtr<ModuleDecl>   program)
    {
        switch(context->shared->target)
        {
        // Don't emit this stuff unless we are targetting GLSL
        default:
            return;

        case CodeGenTarget::GLSL:
            break;
        }

        emitGLSLVersionDirective(program);


        // TODO: when cross-compiling we may need to output additional `#extension` directives
        // based on the features that we have used.

        for( auto extensionDirective :  program->GetModifiersOfType<GLSLExtensionDirective>() )
        {
            // TODO(tfoley): Emit an appropriate `#line` directive...

            Emit("#extension ");
            emit(extensionDirective->extensionNameToken.Content);
            Emit(" : ");
            emit(extensionDirective->dispositionToken.Content);
            Emit("\n");
        }

        // TODO: handle other cases...
    }

    // Utility code for generating unique IDs as needed
    // during the emit process (e.g., for declarations
    // that didn't origianlly have names, but now need to).

    UInt allocateUniqueID()
    {
        return context->shared->uniqueIDCounter++;
    }

    UInt getID(Decl* decl)
    {
        auto& mapDeclToID = context->shared->mapDeclToID;

        UInt id = 0;
        if(mapDeclToID.TryGetValue(decl, id))
            return id;

        id = allocateUniqueID();
        mapDeclToID.Add(decl, id);
        return id;
    }

    // IR-level emit logc

    UInt getID(IRInst* value)
    {
        auto& mapIRValueToID = context->shared->mapIRValueToID;

        UInt id = 0;
        if (mapIRValueToID.TryGetValue(value, id))
            return id;

        id = allocateUniqueID();
        mapIRValueToID.Add(value, id);
        return id;
    }

    String getIRName(Decl* decl)
    {
        // TODO: need a flag to get rid of the step that adds
        // a prefix here, so that we can get "clean" output
        // when needed.
        //

        String name;
        if (!(context->shared->entryPoint->compileRequest->compileFlags & SLANG_COMPILE_FLAG_NO_MANGLING))
        {
            name.append("_s");
        }
        name.append(getText(decl->getName()));
        return name;
    }

    String getIRName(DeclRefBase const& declRef)
    {
        // In general, when referring to a declaration that has been lowered
        // via the IR, we want to use its mangled name.
        //
        // There are two main exceptions to this:
        //
        // 1. For debugging, we accept the `-no-mangle` flag which basically
        //    instructs us to try to use the original name of all declarations,
        //    to make the output more like what is expected to come out of
        //    fxc pass-through. This case should get deprecated some day.
        //
        // 2. It is really annoying to have the fields of a `struct` type
        //    get ridiculously lengthy mangled names, and this also messes
        //    up stuff like specialization (since the mangled name of a field
        //    would then include the mangled name of the outer type).
        //

        String name;
        if (context->shared->entryPoint->compileRequest->compileFlags & SLANG_COMPILE_FLAG_NO_MANGLING)
        {
            // Special case (1):
            name.append(getText(declRef.GetName()));
            return name;
        }

        // Special case (2)
        if (declRef.GetParent().decl->As<AggTypeDecl>())
        {
            name.append(declRef.decl->nameAndLoc.name->text);
            return name;
        }
        // General case:
        name.append(getMangledName(declRef));
        return name;
    }

    String getIRName(
        IRInst*        inst)
    {
        switch(inst->op)
        {
        case kIROp_decl_ref:
            {
                auto irDeclRef = (IRDeclRef*) inst;
                return getIRName(irDeclRef->declRef);
            }
            break;

        default:
            break;
        }

        if(auto decoration = inst->findDecoration<IRHighLevelDeclDecoration>())
        {
            auto decl = decoration->decl;
            if (auto reflectionNameMod = decl->FindModifier<ParameterGroupReflectionName>())
            {
                return getText(reflectionNameMod->nameAndLoc.name);
            }

            if ((context->shared->entryPoint->compileRequest->compileFlags & SLANG_COMPILE_FLAG_NO_MANGLING))
            {
                return getIRName(decl);
            }
        }

        switch (inst->op)
        {
        case kIROp_global_var:
        case kIROp_global_constant:
        case kIROp_Func:
            {
                auto& mangledName = ((IRGlobalValue*)inst)->mangledName;
                if(getText(mangledName).Length() != 0)
                    return getText(mangledName);
            }
            break;

        default:
            break;
        }

        StringBuilder sb;
        sb << "_S";
        sb << getID(inst);
        return sb.ProduceString();
    }

    struct IRDeclaratorInfo
    {
        enum class Flavor
        {
            Simple,
            Ptr,
            Array,
        };

        Flavor flavor;
        IRDeclaratorInfo*   next;
        union
        {
            String const* name;
            IRInst* elementCount;
        };
    };

    void emitDeclarator(
        EmitContext*    ctx,
        IRDeclaratorInfo*   declarator)
    {
        if(!declarator)
            return;

        switch( declarator->flavor )
        {
        case IRDeclaratorInfo::Flavor::Simple:
            emit(" ");
            emit(*declarator->name);
            break;

        case IRDeclaratorInfo::Flavor::Ptr:
            emit("*");
            emitDeclarator(ctx, declarator->next);
            break;

        case IRDeclaratorInfo::Flavor::Array:
            emitDeclarator(ctx, declarator->next);
            emit("[");
            emitIROperand(ctx, declarator->elementCount, IREmitMode::Default);
            emit("]");
            break;
        }
    }

    void emitIRSimpleValue(
        EmitContext*    /*context*/,
        IRInst*         inst)
    {
        switch(inst->op)
        {
        case kIROp_IntLit:
            emit(((IRConstant*) inst)->u.intVal);
            break;

        case kIROp_FloatLit:
            Emit(((IRConstant*) inst)->u.floatVal);
            break;

        case kIROp_boolConst:
            {
                bool val = ((IRConstant*)inst)->u.intVal != 0;
                emit(val ? "true" : "false");
            }
            break;

        default:
            SLANG_UNIMPLEMENTED_X("val case for emit");
            break;
        }

    }

    CodeGenTarget getTarget(EmitContext* ctx)
    {
        return ctx->shared->target;
    }

    // Hack to allow IR emit for global constant to override behavior
    enum class IREmitMode
    {
        Default,
        GlobalConstant,
    };

    bool shouldFoldIRInstIntoUseSites(
        EmitContext*    ctx,
        IRInst*        inst,
        IREmitMode      mode)
    {
        // Certain opcodes should always be folded in
        switch( inst->op )
        {
        default:
            break;

        case kIROp_Var:
        case kIROp_global_var:
        case kIROp_global_constant:
        case kIROp_Param:
            return false;

        case kIROp_IntLit:
        case kIROp_FloatLit:
        case kIROp_boolConst:
        case kIROp_FieldAddress:
        case kIROp_getElementPtr:
        case kIROp_specialize:
        case kIROp_BufferElementRef:
            return true;
        }

        // Always fold when we are inside a global constant initializer
        if (mode == IREmitMode::GlobalConstant)
            return true;

        // Certain *types* will usually want to be folded in,
        // because they aren't allowed as types for temporary
        // variables.
        auto type = inst->getDataType();

        while (auto ptrType = type->As<PtrTypeBase>())
        {
            type = ptrType->getValueType();
        }

        if(type->As<UniformParameterGroupType>())
        {
            // TODO: we need to be careful here, because
            // HLSL shader model 6 allows these as explicit
            // types.
            return true;
        }
        else if (type->As<HLSLStreamOutputType>())
        {
            return true;
        }
        else if (type->As<HLSLPatchType>())
        {
            return true;
        }


        // GLSL doesn't allow texture/resource types to
        // be used as first-class values, so we need
        // to fold them into their use sites in all cases
        if (getTarget(ctx) == CodeGenTarget::GLSL)
        {
            if(type->As<ResourceTypeBase>())
            {
                return true;
            }
            else if(type->As<HLSLStructuredBufferTypeBase>())
            {
                return true;
            }
            else if(type->As<SamplerStateType>())
            {
                return true;
            }
        }

        // By default we will *not* fold things into their use sites.
        return false;
    }

    bool isDerefBaseImplicit(
        EmitContext*    /*context*/,
        IRInst*        inst)
    {
        auto type = inst->getDataType();

        if(type->As<UniformParameterGroupType>() && !type->As<ParameterBlockType>())
        {
            // TODO: we need to be careful here, because
            // HLSL shader model 6 allows these as explicit
            // types.
            return true;
        }

        return false;
    }



    void emitIROperand(
        EmitContext*    ctx,
        IRInst*         inst,
        IREmitMode      mode)
    {
        if( shouldFoldIRInstIntoUseSites(ctx, inst, mode) )
        {
            emit("(");
            emitIRInstExpr(ctx, inst, mode);
            emit(")");
            return;
        }

        switch(inst->op)
        {
        case 0: // nothing yet
        default:
            emit(getIRName(inst));
            break;
        }
    }

    void emitIRArgs(
        EmitContext*    ctx,
        IRInst*         inst,
        IREmitMode      mode)
    {
        UInt argCount = inst->getOperandCount();
        IRUse* args = inst->getOperands();

        emit("(");
        for(UInt aa = 0; aa < argCount; ++aa)
        {
            if(aa != 0) emit(", ");
            emitIROperand(ctx, args[aa].get(), mode);
        }
        emit(")");
    }

    void emitIRType(
        EmitContext*    /*context*/,
        IRType*         type,
        String const&   name)
    {
        EmitType(type, name);
    }

    void emitIRType(
        EmitContext*    /*context*/,
        IRType*         type,
        Name*           name)
    {
        EmitType(type, name);
    }

    void emitIRType(
        EmitContext*    /*context*/,
        IRType*         type)
    {
        EmitType(type);
    }

    void emitIRRateQualifiers(
        EmitContext*    ctx,
        Type*           rate)
    {
        if(!rate) return;

        if( auto constExprRate = rate->As<ConstExprRate>() )
        {
            switch( getTarget(ctx) )
            {
            case CodeGenTarget::GLSL:
                emit("const ");
                break;

            default:
                break;
            }
        }
    }

    void emitIRRateQualifiers(
        EmitContext*    ctx,
        IRInst*         value)
    {
        emitIRRateQualifiers(ctx, value->getRate());
    }


    void emitIRInstResultDecl(
        EmitContext*    ctx,
        IRInst*         inst)
    {
        auto type = inst->getDataType();
        if(!type)
            return;

        if (type->Equals(getSession()->getVoidType()))
            return;

        emitIRRateQualifiers(ctx, inst);

        emitIRType(ctx, type, getIRName(inst));
        emit(" = ");
    }

    class UnmangleContext
    {
    private:
        char const* cursor_  = nullptr;
        char const* begin_   = nullptr;
        char const* end_     = nullptr;

        bool isDigit(char c)
        {
            return (c >= '0') && (c <= '9');
        }

        char peek()
        {
            return *cursor_;
        }

        char get()
        {
            return *cursor_++;
        }

        void expect(char c)
        {
            if(peek() == c)
            {
                get();
            }
            else
            {
                // ERROR!
                SLANG_UNEXPECTED("mangled name error");
            }
        }

        void expect(char const* str)
        {
            while(char c = *str++)
                expect(c);
        }

    public:
        UnmangleContext()
        {}

        UnmangleContext(String const& str)
            : cursor_(str.begin())
            , begin_(str.begin())
            , end_(str.end())
        {}

        // Call at the beginning of a mangled name,
        // to strip off the main prefix
        void startUnmangling()
        {
            expect("_S");
        }

        UInt readCount()
        {
            int c = peek();
            if(!isDigit((char)c))
            {
                SLANG_UNEXPECTED("bad name mangling");
                UNREACHABLE_RETURN(0);
            }
            get();

            if(c == '0')
                return 0;

            UInt count = 0;
            for(;;)
            {
                count = count*10 + c - '0';
                c = peek();
                if(!isDigit((char)c))
                    return count;

                get();
            }
        }

        void readGenericParam()
        {
            switch(peek())
            {
            case 'T':
            case 'C':
                get();
                break;

            case 'v':
                get();
                readType();
                break;

            default:
                SLANG_UNEXPECTED("bad name mangling");
                break;
            }
        }

        void readGenericParams()
        {
            expect("g");
            UInt paramCount = readCount();
            for(UInt pp = 0; pp < paramCount; pp++)
            {
                readGenericParam();
            }
        }

        void readSimpleIntVal()
        {
            int c = peek();
            if(isDigit((char)c))
            {
                get();
            }
            else
            {
                readVal();
            }
        }

        void readType()
        {
            int c = peek();
            switch(c)
            {
            case 'V':
            case 'b':
            case 'i':
            case 'u':
            case 'U':
            case 'h':
            case 'f':
            case 'd':
                get();
                break;

            case 'v':
                get();
                readSimpleIntVal();
                readType();
                break;

            default:
                // TODO: need to read a named type
                // here...
                break;
            }
        }

        void readVal()
        {
            // TODO: handle other cases here
            readType();
        }

        void readGenericArg()
        {
            readVal();
        }

        void readGenericArgs()
        {
            expect("G");
            UInt argCount = readCount();
            for(UInt aa = 0; aa < argCount; aa++)
            {
                readGenericArg();
            }
        }

        UnownedStringSlice readSimpleName()
        {
            UnownedStringSlice result;
            for(;;)
            {
                int c = peek();

                if(c == 'g')
                {
                    readGenericParams();
                    continue;
                }
                else if(c == 'G')
                {
                    readGenericArgs();
                    continue;
                }

                if(!isDigit((char)c))
                    return result;

                // Read the length part
                UInt count = readCount();
                if(count > UInt(end_ - cursor_))
                {
                    SLANG_UNEXPECTED("bad name mangling");
                    UNREACHABLE_RETURN(result);
                }

                result = UnownedStringSlice(cursor_, cursor_ + count);
                cursor_ += count;
            }
        }

        UInt readParamCount()
        {
            expect("p");
            UInt count = readCount();
            expect("p");
            return count;
        }
    };

    IRTargetIntrinsicDecoration* findTargetIntrinsicDecoration(
        EmitContext*    /* ctx */,
        IRFunc*         func)
    {
        for (auto dd = func->firstDecoration; dd; dd = dd->next)
        {
            if (dd->op != kIRDecorationOp_TargetIntrinsic)
                continue;

            auto targetIntrinsic = (IRTargetIntrinsicDecoration*)dd;
            if (isTargetIntrinsicModifierApplicable(targetIntrinsic))
                return targetIntrinsic;
        }

        return nullptr;
    }

    // Check if the string being used to define a target intrinsic
    // is an "ordinary" name, such that we can simply emit a call
    // to the new name with the arguments of the old operation.
    bool isOrdinaryName(String const& name)
    {
        char const* cursor = name.begin();
        char const* end = name.end();

        while(cursor != end)
        {
            int c = *cursor++;
            if( (c >= 'a') && (c <= 'z') ) continue;
            if( (c >= 'A') && (c <= 'Z') ) continue;
            if( c == '_' ) continue;

            return false;
        }
        return true;
    }

    void emitTargetIntrinsicCallExpr(
        EmitContext*                    ctx,
        IRCall*                         inst,
        IRFunc*                         /* func */,
        IRTargetIntrinsicDecoration*    targetIntrinsic,
        IREmitMode                      mode)
    {
        IRUse* args = inst->getOperands();
        UInt argCount = inst->getOperandCount();

        // First operand was the function to be called
        args++;
        argCount--;

        auto name = targetIntrinsic->definition;


        if(isOrdinaryName(name))
        {
            // Simple case: it is just an ordinary name, so we call it like a builtin.

            emit(name);
            Emit("(");
            for (UInt aa = 0; aa < argCount; ++aa)
            {
                if (aa != 0) Emit(", ");
                emitIROperand(ctx, args[aa].get(), mode);
            }
            Emit(")");
            return;
        }
        else
        {
            // General case: we are going to emit some more complex text.

            Emit("(");

            char const* cursor = name.begin();
            char const* end = name.end();
            while(cursor != end)
            {
                char c = *cursor++;
                if( c != '$' )
                {
                    // Not an escape sequence
                    emitRawTextSpan(&c, &c+1);
                    continue;
                }

                SLANG_RELEASE_ASSERT(cursor != end);

                char d = *cursor++;

                switch (d)
                {
                case '0': case '1': case '2': case '3': case '4':
                case '5': case '6': case '7': case '8': case '9':
                    {
                        // Simple case: emit one of the direct arguments to the call
                        UInt argIndex = d - '0';
                        SLANG_RELEASE_ASSERT((0 <= argIndex) && (argIndex < argCount));
                        Emit("(");
                        emitIROperand(ctx, args[argIndex].get(), mode);
                        Emit(")");
                    }
                    break;

                case 'p':
                    {
                        // If we are calling a D3D texturing operation in the form t.Foo(s, ...),
                        // then this form will pair up the t and s arguments as needed for a GLSL
                        // texturing operation.
                        SLANG_RELEASE_ASSERT(argCount >= 2);

                        auto textureArg = args[0].get();
                        auto samplerArg = args[1].get();

                        if (auto baseTextureType = textureArg->type->As<TextureType>())
                        {
                            emitGLSLTextureOrTextureSamplerType(baseTextureType, "sampler");

                            if (auto samplerType = samplerArg->type->As<SamplerStateType>())
                            {
                                if (samplerType->flavor == SamplerStateFlavor::SamplerComparisonState)
                                {
                                    Emit("Shadow");
                                }
                            }

                            Emit("(");
                            emitIROperand(ctx, textureArg, mode);
                            Emit(",");
                            emitIROperand(ctx, samplerArg, mode);
                            Emit(")");
                        }
                        else
                        {
                            SLANG_UNEXPECTED("bad format in intrinsic definition");
                        }
                    }
                    break;

                case 'P':
                    {
                        // Okay, we need a collosal hack to deal with the fact that GLSL `texelFetch()`
                        // for Vulkan seems to be completely broken by design. It's signature wants
                        // a `sampler2D` for consistency with its peers, but the actual SPIR-V operation
                        // ignores the sampler paart of it, and just used the `texture2D` part.
                        //
                        // The HLSL equivalent (e.g., `Texture2D.Load()`) doesn't provide a sampler
                        // argument, so we seemingly need to conjure one out of thin air. :(
                        //
                        // We are going to hack this *hard* for now.

                        auto textureArg = args[0].get();
                        if (auto baseTextureType = textureArg->type->As<TextureType>())
                        {
                            emitGLSLTextureOrTextureSamplerType(baseTextureType, "sampler");
                            Emit("(");
                            emitIROperand(ctx, textureArg, mode);
                            Emit(",");
                            Emit("SLANG_hack_samplerForTexelFetch");
                            context->shared->needHackSamplerForTexelFetch = true;
                            Emit(")");
                        }
                        else
                        {
                            SLANG_UNEXPECTED("bad format in intrinsic definition");
                        }
                    }
                    break;

                case 'z':
                    {
                        // If we are calling a D3D texturing operation in the form t.Foo(s, ...),
                        // where `t` is a `Texture*<T>`, then this is the step where we try to
                        // properly swizzle the output of the equivalent GLSL call into the right
                        // shape.
                        SLANG_RELEASE_ASSERT(argCount >= 1);

                        auto textureArg = args[0].get();
                        if (auto baseTextureType = textureArg->type->As<TextureType>())
                        {
                            auto elementType = baseTextureType->elementType;
                            if (auto basicType = elementType->As<BasicExpressionType>())
                            {
                                // A scalar result is expected
                                Emit(".x");
                            }
                            else if (auto vectorType = elementType->As<VectorExpressionType>())
                            {
                                // A vector result is expected
                                auto elementCount = GetIntVal(vectorType->elementCount);

                                if (elementCount < 4)
                                {
                                    char const* swiz[] = { "", ".x", ".xy", ".xyz", "" };
                                    Emit(swiz[elementCount]);
                                }
                            }
                            else
                            {
                                // What other cases are possible?
                            }
                        }
                        else
                        {
                            SLANG_UNEXPECTED("bad format in intrinsic definition");
                        }
                    }
                    break;

                case 'N':
                    {
                        // Extract the element count from a vector argument so that
                        // we can use it in the constructed expression.

                        SLANG_RELEASE_ASSERT(*cursor >= '0' && *cursor <= '9');
                        UInt argIndex = (*cursor++) - '0';
                        SLANG_RELEASE_ASSERT(argCount > argIndex);

                        auto vectorArg = args[argIndex].get();
                        if (auto vectorType = vectorArg->type->As<VectorExpressionType>())
                        {
                            auto elementCount = GetIntVal(vectorType->elementCount);
                            Emit(elementCount);
                        }
                        else
                        {
                            SLANG_UNEXPECTED("bad format in intrinsic definition");
                        }
                    }
                    break;


                default:
                    SLANG_UNEXPECTED("bad format in intrinsic definition");
                    break;
                }
            }

            Emit(")");
        }
    }

    void emitIntrinsicCallExpr(
        EmitContext*    ctx,
        IRCall*         inst,
        IRFunc*         func,
        IREmitMode      mode)
    {
        // For a call with N arguments, the instruction will
        // have N+1 operands. We will start consuming operands
        // starting at the index 1.
        UInt operandCount = inst->getOperandCount();
        UInt argCount = operandCount - 1;
        UInt operandIndex = 1;


        // 
        if (auto targetIntrinsicDecoration = findTargetIntrinsicDecoration(ctx, func))
        {
            emitTargetIntrinsicCallExpr(
                ctx,
                inst,
                func,
                targetIntrinsicDecoration,
                mode);
            return;
        }

        // Our current strategy for dealing with intrinsic
        // calls is to "un-mangle" the mangled name, in
        // order to figure out what the user was originally
        // calling. This is a bit messy, and there might
        // be better strategies (including just stuffing
        // a pointer to the original decl onto the callee).

        UnmangleContext um(getText(func->mangledName));
        um.startUnmangling();

        // We'll read through the qualified name of the
        // symbol (e.g., `Texture2D<T>.Sample`) and then
        // only keep the last segment of the name (e.g.,
        // the `Sample` part).
        auto name = um.readSimpleName();

        // We will special-case some names here, that
        // represent callable declarations that aren't
        // ordinary functions, and thus may use different
        // syntax.
        if(name == "operator[]")
        {
            // The user is invoking a built-in subscript operator
            emit("(");
            emitIROperand(ctx, inst->getOperand(operandIndex++), mode);
            emit(")[");
            emitIROperand(ctx, inst->getOperand(operandIndex++), mode);
            emit("]");

            if(operandIndex < operandCount)
            {
                emit(" = ");
                emitIROperand(ctx, inst->getOperand(operandIndex++), mode);
            }
            return;
        }

        // The mangled function name currently records
        // the number of explicit parameters, and thus
        // doesn't include the implicit `this` parameter.
        // We can compare the argument and parameter counts
        // to figure out whether we have a member function call.
        UInt paramCount = um.readParamCount();

        if(argCount != paramCount)
        {
            // Looks like a member function call
            emit("(");
            emitIROperand(ctx, inst->getOperand(operandIndex), mode);
            emit(").");

            operandIndex++;
        }

        emit(name);
        emit("(");
        bool first = true;
        for(; operandIndex < operandCount; ++operandIndex )
        {
            if(!first) emit(", ");
            emitIROperand(ctx, inst->getOperand(operandIndex), mode);
            first = false;
        }
        emit(")");
    }

    void emitIRCallExpr(
        EmitContext*    ctx,
        IRCall*         inst,
        IREmitMode      mode)
    {
        // We want to detect any call to an intrinsic operation,
        // that we can emit it directly without mangling, etc.
        auto funcValue = inst->getOperand(0);
        if(auto irFunc = asTargetIntrinsic(ctx, funcValue))
        {
            emitIntrinsicCallExpr(ctx, inst, irFunc, mode);
        }
        else
        {
            emitIROperand(ctx, funcValue, mode);
            emit("(");
            UInt argCount = inst->getOperandCount();
            for( UInt aa = 1; aa < argCount; ++aa )
            {
                if(aa != 1) emit(", ");
                emitIROperand(ctx, inst->getOperand(aa), mode);
            }
            emit(")");
        }
    }

    void emitIRInstExpr(
        EmitContext*    ctx,
        IRInst*         inst,
        IREmitMode      mode)
    {
        advanceToSourceLocation(inst->sourceLoc);

        switch(inst->op)
        {
        case kIROp_IntLit:
        case kIROp_FloatLit:
        case kIROp_boolConst:
            emitIRSimpleValue(ctx, inst);
            break;

        case kIROp_Construct:
        case kIROp_makeVector:
        case kIROp_makeMatrix:
            // Simple constructor call
            if( inst->getOperandCount() == 1 && getTarget(ctx) == CodeGenTarget::HLSL)
            {
                // Need to emit as cast for HLSL
                emit("(");
                emitIRType(ctx, inst->getDataType());
                emit(") ");
                emitIROperand(ctx, inst->getOperand(0), mode);
            }
            else
            {
                emitIRType(ctx, inst->getDataType());
                emitIRArgs(ctx, inst, mode);
            }
            break;

        case kIROp_constructVectorFromScalar:
            // Simple constructor call
            if( getTarget(ctx) == CodeGenTarget::HLSL )
            {
                emit("(");
                emitIRType(ctx, inst->getDataType());
                emit(")");
            }
            else
            {
                emitIRType(ctx, inst->getDataType());
            }
            emit("(");
            emitIROperand(ctx, inst->getOperand(0), mode);
            emit(")");
            break;

        case kIROp_FieldExtract:
            {
                // Extract field from aggregate

                IRFieldExtract* fieldExtract = (IRFieldExtract*) inst;

                if (!isDerefBaseImplicit(ctx, fieldExtract->getBase()))
                {
                    emitIROperand(ctx, fieldExtract->getBase(), mode);
                    emit(".");
                }
                emit(getIRName(fieldExtract->getField()));
            }
            break;

        case kIROp_FieldAddress:
            {
                // Extract field "address" from aggregate

                IRFieldAddress* ii = (IRFieldAddress*) inst;

                if (!isDerefBaseImplicit(ctx, ii->getBase()))
                {
                    emitIROperand(ctx, ii->getBase(), mode);
                    emit(".");
                }

                emit(getIRName(ii->getField()));
            }
            break;

#define CASE(OPCODE, OP)                                    \
        case OPCODE:                                        \
            emitIROperand(ctx, inst->getOperand(0), mode);  \
            emit(" " #OP " ");                              \
            emitIROperand(ctx, inst->getOperand(1), mode);  \
            break

        CASE(kIROp_Add, +);
        CASE(kIROp_Sub, -);
        CASE(kIROp_Div, /);
        CASE(kIROp_Mod, %);

        CASE(kIROp_Lsh, <<);
        CASE(kIROp_Rsh, >>);

        // TODO: Need to pull out component-wise
        // comparison cases for matrices/vectors
        CASE(kIROp_Eql, ==);
        CASE(kIROp_Neq, !=);
        CASE(kIROp_Greater, >);
        CASE(kIROp_Less, <);
        CASE(kIROp_Geq, >=);
        CASE(kIROp_Leq, <=);

        CASE(kIROp_BitAnd, &);
        CASE(kIROp_BitXor, ^);
        CASE(kIROp_BitOr, |);

        CASE(kIROp_And, &&);
        CASE(kIROp_Or, ||);

#undef CASE

        // Component-wise ultiplication needs to be special cased,
        // because GLSL uses infix `*` to express inner product
        // when working with matrices.
        case kIROp_Mul:
            // Are we targetting GLSL, and is this a matrix product?
            if(getTarget(ctx) == CodeGenTarget::GLSL
                && inst->type->As<MatrixExpressionType>())
            {
                emit("matrixCompMult(");
                emitIROperand(ctx, inst->getOperand(0), mode);
                emit(", ");
                emitIROperand(ctx, inst->getOperand(1), mode);
                emit(")");
            }
            else
            {
                // Default handling is to just rely on infix
                // `operator*`.
                emitIROperand(ctx, inst->getOperand(0), mode);
                emit(" * ");
                emitIROperand(ctx, inst->getOperand(1), mode);
            }
            break;

        case kIROp_Not:
            {
                if (inst->getDataType()->Equals(getSession()->getBoolType()))
                {
                    emit("!");
                }
                else
                {
                    emit("~");
                }
                emitIROperand(ctx, inst->getOperand(0), mode);
            }
            break;

        case kIROp_Neg:
            {
                emit("-");
                emitIROperand(ctx, inst->getOperand(0), mode);
            }
            break;
        case kIROp_BitNot:
            {
                emit("~");
                emitIROperand(ctx, inst->getOperand(0), mode);
            }
            break;
        case kIROp_Sample:
            emitIROperand(ctx, inst->getOperand(0), mode);
            emit(".Sample(");
            emitIROperand(ctx, inst->getOperand(1), mode);
            emit(", ");
            emitIROperand(ctx, inst->getOperand(2), mode);
            emit(")");
            break;

        case kIROp_SampleGrad:
            // argument 0 is the instruction's type
            emitIROperand(ctx, inst->getOperand(0), mode);
            emit(".SampleGrad(");
            emitIROperand(ctx, inst->getOperand(1), mode);
            emit(", ");
            emitIROperand(ctx, inst->getOperand(2), mode);
            emit(", ");
            emitIROperand(ctx, inst->getOperand(3), mode);
            emit(", ");
            emitIROperand(ctx, inst->getOperand(4), mode);
            emit(")");
            break;

        case kIROp_Load:
            // TODO: this logic will really only work for a simple variable reference...
            emitIROperand(ctx, inst->getOperand(0), mode);
            break;

        case kIROp_Store:
            // TODO: this logic will really only work for a simple variable reference...
            emitIROperand(ctx, inst->getOperand(0), mode);
            emit(" = ");
            emitIROperand(ctx, inst->getOperand(1), mode);
            break;

        case kIROp_Call:
            {
                emitIRCallExpr(ctx, (IRCall*)inst, mode);
            }
            break;

        case kIROp_BufferLoad:
        case kIROp_BufferElementRef:
            emitIROperand(ctx, inst->getOperand(0), mode);
            emit("[");
            emitIROperand(ctx, inst->getOperand(1), mode);
            emit("]");
            break;

        case kIROp_BufferStore:
            emitIROperand(ctx, inst->getOperand(0), mode);
            emit("[");
            emitIROperand(ctx, inst->getOperand(1), mode);
            emit("] = ");
            emitIROperand(ctx, inst->getOperand(2), mode);
            break;

        case kIROp_GroupMemoryBarrierWithGroupSync:
            emit("GroupMemoryBarrierWithGroupSync()");
            break;

        case kIROp_getElement:
        case kIROp_getElementPtr:
            emitIROperand(ctx, inst->getOperand(0), mode);
            emit("[");
            emitIROperand(ctx, inst->getOperand(1), mode);
            emit("]");
            break;

        case kIROp_Mul_Vector_Matrix:
        case kIROp_Mul_Matrix_Vector:
        case kIROp_Mul_Matrix_Matrix:
            if(getTarget(ctx) == CodeGenTarget::GLSL)
            {
                // GLSL expresses inner-product multiplications
                // with the ordinary infix `*` operator.
                //
                // Note that the order of the operands is reversed
                // compared to HLSL (and Slang's internal representation)
                // because the notion of what is a "row" vs. a "column"
                // is reversed between HLSL/Slang and GLSL.
                //
                emitIROperand(ctx, inst->getOperand(1), mode);
                emit(" * ");
                emitIROperand(ctx, inst->getOperand(0), mode);
            }
            else
            {
                emit("mul(");
                emitIROperand(ctx, inst->getOperand(0), mode);
                emit(", ");
                emitIROperand(ctx, inst->getOperand(1), mode);
                emit(")");
            }
            break;

        case kIROp_swizzle:
            {
                auto ii = (IRSwizzle*)inst;
                emitIROperand(ctx, ii->getBase(), mode);
                emit(".");
                UInt elementCount = ii->getElementCount();
                for (UInt ee = 0; ee < elementCount; ++ee)
                {
                    IRInst* irElementIndex = ii->getElementIndex(ee);
                    assert(irElementIndex->op == kIROp_IntLit);
                    IRConstant* irConst = (IRConstant*)irElementIndex;

                    UInt elementIndex = (UInt)irConst->u.intVal;
                    assert(elementIndex < 4);

                    char const* kComponents[] = { "x", "y", "z", "w" };
                    emit(kComponents[elementIndex]);
                }
            }
            break;

        case kIROp_specialize:
            {
                emitIROperand(ctx, inst->getOperand(0), mode);
            }
            break;

        case kIROp_Select:
            {
                emitIROperand(ctx, inst->getOperand(0), mode);
                emit(" ? ");
                emitIROperand(ctx, inst->getOperand(1), mode);
                emit(" : ");
                emitIROperand(ctx, inst->getOperand(2), mode);
            }
            break;

        case kIROp_Param:
            emit(getIRName(inst));
            break;

        case kIROp_makeArray:
        case kIROp_makeStruct:
            {
                // TODO: initializer-list syntax may not always
                // be appropriate, depending on the context
                // of the expression.

                emit("{ ");
                UInt argCount = inst->getOperandCount();
                for (UInt aa = 0; aa < argCount; ++aa)
                {
                    if (aa != 0) emit(", ");
                    emitIROperand(ctx, inst->getOperand(aa), mode);
                }
                emit(" }");
            }
            break;

        default:
            emit("/* unhandled */");
            break;
        }
    }

    void emitIRInst(
        EmitContext*    ctx,
        IRInst*         inst,
        IREmitMode      mode)
    {
        if (shouldFoldIRInstIntoUseSites(ctx, inst, mode))
        {
            return;
        }

        advanceToSourceLocation(inst->sourceLoc);

        switch(inst->op)
        {
        default:
            emitIRInstResultDecl(ctx, inst);
            emitIRInstExpr(ctx, inst, mode);
            emit(";\n");
            break;

        case kIROp_undefined:
            {
                auto type = inst->getDataType();
                emitIRType(ctx, type, getIRName(inst));
                emit(";\n");
            }
            break;

        case kIROp_Var:
            {
                auto ptrType = inst->getDataType();
                auto valType = ((PtrType*)ptrType)->getValueType();

                auto name = getIRName(inst);
                emitIRType(ctx, valType, name);
                emit(";\n");
            }
            break;

        case kIROp_Param:
            // Don't emit parameters, since they are declared as part of the function.
            break;

        case kIROp_FieldAddress:
            // skip during code emit, since it should be
            // folded into use site(s)
            break;

        case kIROp_ReturnVoid:
            emit("return;\n");
            break;

        case kIROp_ReturnVal:
            emit("return ");
            emitIROperand(ctx, ((IRReturnVal*) inst)->getVal(), mode);
            emit(";\n");
            break;

        case kIROp_discard:
            emit("discard;\n");
            break;

        case kIROp_swizzleSet:
            {
                auto ii = (IRSwizzleSet*)inst;
                emitIRInstResultDecl(ctx, inst);
                emitIROperand(ctx, inst->getOperand(0), mode);
                emit(";\n");
                emitIROperand(ctx, inst, mode);
                emit(".");
                UInt elementCount = ii->getElementCount();
                for (UInt ee = 0; ee < elementCount; ++ee)
                {
                    IRInst* irElementIndex = ii->getElementIndex(ee);
                    assert(irElementIndex->op == kIROp_IntLit);
                    IRConstant* irConst = (IRConstant*)irElementIndex;

                    UInt elementIndex = (UInt)irConst->u.intVal;
                    assert(elementIndex < 4);

                    char const* kComponents[] = { "x", "y", "z", "w" };
                    emit(kComponents[elementIndex]);
                }
                emit(" = ");
                emitIROperand(ctx, inst->getOperand(1), mode);
                emit(";\n");
            }
            break;
        }
    }

    void emitIRSemantics(
        EmitContext*    ctx,
        IRInst*         inst)
    {
        // Don't emit semantics if we aren't translating down to HLSL
        switch (ctx->shared->target)
        {
        case CodeGenTarget::HLSL:
            break;

        default:
            return;
        }

        if(auto layoutDecoration = inst->findDecoration<IRLayoutDecoration>())
        {
            if(auto varLayout = layoutDecoration->layout.As<VarLayout>())
            {
                if(varLayout->flags & VarLayoutFlag::HasSemantic)
                {
                    Emit(" : ");
                    emit(varLayout->semanticName);
                    if(varLayout->semanticIndex)
                    {
                        Emit(varLayout->semanticIndex);
                    }

                    return;
                }
            }
        }

        // TODO(tfoley): should we ever need to use the high-level declaration
        // for this? It seems like the wrong approach...

        auto decoration = inst->findDecoration<IRHighLevelDeclDecoration>();
        if( decoration )
        {
            EmitSemantics(decoration->decl);
        }
    }

    VarLayout* getVarLayout(
        EmitContext*    /*context*/,
        IRInst*         var)
    {
        auto decoration = var->findDecoration<IRLayoutDecoration>();
        if (!decoration)
            return nullptr;

        return (VarLayout*) decoration->layout.Ptr();
    }

    void emitIRLayoutSemantics(
        EmitContext*    ctx,
        IRInst*         inst,
        char const*     uniformSemanticSpelling = "register")
    {
        auto layout = getVarLayout(ctx, inst);
        if (layout)
        {
            emitHLSLRegisterSemantics(layout, uniformSemanticSpelling);
        }
    }

    // When we are about to traverse an edge from one block to another,
    // we need to emit the assignments that conceptually occur "along"
    // the edge. In traditional SSA these are the phi nodes in the
    // target block, while in our representation these use the arguments
    // to the branch instruction to fill in the parameters of the target.
    void emitPhiVarAssignments(
        EmitContext*    ctx,
        UInt            argCount,
        IRUse*          args,
        IRBlock*        targetBlock)
    {
        UInt argCounter = 0;
        for (auto pp = targetBlock->getFirstParam(); pp; pp = pp->getNextParam())
        {
            UInt argIndex = argCounter++;

            if (argIndex >= argCount)
            {
                assert(!"not enough arguments for branch");
                break;
            }

            IRInst* arg = args[argIndex].get();

            emitIROperand(ctx, pp, IREmitMode::Default);
            emit(" = ");
            emitIROperand(ctx, arg, IREmitMode::Default);
            emit(";\n");
        }
    }

    enum LabelOp
    {
        kLabelOp_break,
        kLabelOp_continue,

        kLabelOpCount,
    };

    struct LabelStack
    {
        LabelStack* parent;
        IRBlock*    block;
        LabelOp     op;
    };

    // We want to emit a range of code in the IR, represented
    // by the blocks that are logically in the interval [begin, end)
    // which we consider as a single-entry multiple-exit region.
    //
    // Note: because there are multiple exists, control flow
    // may exit this region with operations that do *not* branch
    // to `end`, but such non-local control flow will hopefully
    // be captured.
    // 
    void emitIRStmtsForBlocks(
        EmitContext*    ctx,
        IRBlock*        begin,
        IRBlock*        end,

        // Labels to use at the start
        LabelStack*     initialLabels,

        // Labels to switch to after emitting first basic block
        LabelStack*     labels = nullptr)
    {
        if(!labels)
            labels = initialLabels;

        auto useLabels = initialLabels;

        IRBlock* block = begin;
        while(block != end)
        {
            // If the block we are trying to emit has been registered as a
            // destination label (e.g. for a loop or `switch`) then we
            // may need to emit a `break` or `continue` as needed.
            //
            // TODO: we eventually need to handle the possibility of
            // multi-level break/continue targets, which could be challenging.

            // First, figure out which block has been registered as
            // the current `break` and `continue` target.
            IRBlock* registeredBlock[kLabelOpCount] = {};
            for( auto ll = labels; ll; ll = ll->parent )
            {
                if(!registeredBlock[ll->op])
                {
                    registeredBlock[ll->op] = ll->block;
                }
            }

            // Next, search in the active labels we are allowed to use,
            // and see if the block we are trying to branch to is an
            // available break/continue target.
            for(auto ll = useLabels; ll; ll = ll->parent)
            {
                if(ll->block == block)
                {
                    // We are trying to go to a block that has been regsitered as a label.

                    if(block != registeredBlock[ll->op])
                    {
                        // ERROR: need support for multi-level break/continue to pull this one off!
                    }

                    switch(ll->op)
                    {
                    case kLabelOp_break:
                        emit("break;\n");
                        break;

                    case kLabelOp_continue:
                        emit("continue;\n");
                        break;
                    }

                    return;
                }
            }

            // Start by emitting the non-terminator instructions in the block.
            auto terminator = block->getLastInst();
            assert(as<IRTerminatorInst>(terminator));
            for (auto inst = block->getFirstInst(); inst != terminator; inst = inst->getNextInst())
            {
                emitIRInst(ctx, inst, IREmitMode::Default);
            }

            // Now look at the terminator instruction, which will tell us what we need to emit next.

            advanceToSourceLocation(terminator->sourceLoc);

            switch (terminator->op)
            {
            default:
                SLANG_UNEXPECTED("terminator inst");
                return;

            case kIROp_unreachable:
                return;

            case kIROp_ReturnVal:
            case kIROp_ReturnVoid:
            case kIROp_discard:
                emitIRInst(ctx, terminator, IREmitMode::Default);
                return;

            case kIROp_ifElse:
                {
                    // Two-sided `if` statement
                    auto t = (IRIfElse*)terminator;

                    auto trueBlock = t->getTrueBlock();
                    auto falseBlock = t->getFalseBlock();
                    auto afterBlock = t->getAfterBlock();

                    // TODO: consider simplifying the code in
                    // the case where `trueBlock == afterBlock`
                    // so that we output `if(!cond) { falseBlock }`
                    // instead of the current `if(cond) {} else {falseBlock}`

                    emit("if(");
                    emitIROperand(ctx, t->getCondition(), IREmitMode::Default);
                    emit(")\n{\n");
                    indent();
                    emitIRStmtsForBlocks(
                        ctx,
                        trueBlock,
                        afterBlock,
                        labels);
                    dedent();
                    emit("}\n");
                    // Don't emit the false block if it would be empty
                    if(falseBlock != afterBlock)
                    {
                        emit("else\n{\n");
                        indent();
                        emitIRStmtsForBlocks(
                            ctx,
                            falseBlock,
                            afterBlock,
                            labels);
                        dedent();
                        emit("}\n");
                    }

                    // Continue with the block after the `if`
                    block = afterBlock;
                }
                break;

            case kIROp_loop:
                {
                    // Header for a `while` or `for` loop
                    auto t = (IRLoop*)terminator;

                    auto targetBlock = t->getTargetBlock();
                    auto breakBlock = t->getBreakBlock();

                    UInt argCount = t->getOperandCount();
                    static const UInt kFixedArgCount = 3;
                    emitPhiVarAssignments(
                        ctx,
                        argCount - kFixedArgCount,
                        t->getOperands() + kFixedArgCount,
                        targetBlock);

                    // Set up entries on our label stack for break/continue

                    LabelStack subBreakLabel;
                    subBreakLabel.parent = labels;
                    subBreakLabel.block = breakBlock;
                    subBreakLabel.op = kLabelOp_break;

                    // Note: when forming the `continue` label, we don't
                    // actually point at the "continue block" from the loop
                    // statement, because we aren't actually going to
                    // generate an ordinary continue caluse in a `for` loop.
                    //
                    // Instead, our `continue` label will always be the
                    // loop header.
                    LabelStack subContinueLabel;
                    subContinueLabel.parent = &subBreakLabel;
                    subContinueLabel.block = targetBlock;
                    subContinueLabel.op = kLabelOp_continue;

                    if (auto loopControlDecoration = t->findDecoration<IRLoopControlDecoration>())
                    {
                        switch (loopControlDecoration->mode)
                        {
                        case kIRLoopControl_Unroll:
                            // Note: loop unrolling control is only available in HLSL, not GLSL
                            if(getTarget(ctx) == CodeGenTarget::HLSL)
                            {
                                emit("[unroll]\n");
                            }
                            break;

                        default:
                            break;
                        }
                    }

                    emit("for(;;)\n{\n");
                    indent();
                    emitIRStmtsForBlocks(
                        ctx,
                        targetBlock,
                        nullptr,
                        // For the first block, we only want the `break` label active
                        &subBreakLabel,
                        // After the first block, we can safely use the `continue` label too
                        &subContinueLabel);
                    dedent();
                    emit("}\n");

                    // Continue with the block after the loop
                    block = breakBlock;
                }
                break;

#if 0
            case kIROp_break:
                {
                    auto t = (IRBreak*)terminator;
                    auto targetBlock = t->getTargetBlock();

                    UInt argCount = t->getArgCount();
                    static const UInt kFixedArgCount = 1;
                    emitPhiVarAssignments(
                        ctx,
                        argCount - kFixedArgCount,
                        t->getArgs() + kFixedArgCount,
                        targetBlock);

                    emit("break;\n");
                }
                return;

            case kIROp_continue:
                // With out current strategy for outputting loops,
                // just outputting an AST-level `continue` here won't
                // actually execute the statements in the continue block.
                //
                // Instead, we have to manually output those statements
                // directly here, and *then* do an AST-level `continue`.
                //
                // This leads to code duplication when we have multiple
                // `continue` sites in the original program, but it avoids
                // introducing additional temporaries for control flow.
                {
                    auto continueInst = (IRContinue*) terminator;
                    auto targetBlock = continueInst->getTargetBlock();

                    UInt argCount = continueInst->getArgCount();
                    static const UInt kFixedArgCount = 1;
                    emitPhiVarAssignments(
                        ctx,
                        argCount - kFixedArgCount,
                        continueInst->getArgs() + kFixedArgCount,
                        targetBlock);

                    IRBlock* loopHead = nullptr;
                    ctx->shared->irMapContinueTargetToLoopHead.TryGetValue(targetBlock, loopHead);
                    SLANG_ASSERT(loopHead);
                    emitIRStmtsForBlocks(
                        ctx,
                        targetBlock,
                        loopHead);
                    emit("continue;\n");
                }
                return;

            case kIROp_loopTest:
                {
                    // Loop condition being tested
                    auto t = (IRLoopTest*)terminator;

                    auto afterBlock = t->getTrueBlock();
                    auto exitBlock = t->getFalseBlock();

                    emit("if(");
                    emitIROperand(ctx, t->getCondition());
                    emit(")\n{} else {\n");
                    emitIRStmtsForBlocks(
                        ctx,
                        exitBlock,
                        nullptr);
                    emit("}\n");

                    // Continue with the block after the test
                    block = afterBlock;
                }
                break;
#endif

            case kIROp_unconditionalBranch:
                {
                    // Unconditional branch as part of normal
                    // control flow. This is either a forward
                    // edge to the "next" block in an ordinary
                    // block, or a backward edge to the top
                    // of a loop.
                    auto t = (IRUnconditionalBranch*)terminator;
                    auto targetBlock = t->getTargetBlock();

                    UInt argCount = t->getOperandCount();
                    static const UInt kFixedArgCount = 1;
                    emitPhiVarAssignments(
                        ctx,
                        argCount - kFixedArgCount,
                        t->getOperands() + kFixedArgCount,
                        targetBlock);

                    block = t->getTargetBlock();
                }
                break;

            case kIROp_conditionalBranch:
                // Note: We currently do not generate any plain
                // `conditionalBranch` instructions when lowering
                // to IR, because these would not have the annotations
                // needed to be able to emit high-level control
                // flow from them.
                SLANG_UNEXPECTED("terminator inst");
                return;


            case kIROp_switch:
                {
                    // A `switch` instruction will always translate
                    // to a `switch` statement, but we need to
                    // take some care to emit the `case`s in ways
                    // that avoid code duplication.

                    // TODO: Eventually, the "right" way to handle `switch`
                    // statements while being more robust about Duff's Device, etc.
                    // would be to register each of the case labels in a lookup
                    // table, and then walk the blocks in the region between
                    // the `switch` and the `break` and then whenever we see a block
                    // that matches one of the registered labels, emit the appropriate
                    // `case ...:` or `default:` label.

                    auto t = (IRSwitch*) terminator;

                    // Extract the fixed arguments.
                    auto conditionVal = t->getCondition();
                    auto breakLabel = t->getBreakLabel();
                    auto defaultLabel = t->getDefaultLabel();

                    // Register the block to be used for our `break` target
                    LabelStack subLabels;
                    subLabels.parent = labels;
                    subLabels.op = kLabelOp_break;
                    subLabels.block = breakLabel;

                    // We need to track whether we've dealt with
                    // the `default` case already.
                    bool defaultLabelHandled = false;

                    // If the `default` case just branches to
                    // the join point, then we don't need to
                    // do anything with it.
                    if(defaultLabel == breakLabel)
                        defaultLabelHandled = true;

                    // Emit the start of our statement.
                    emit("switch(");
                    emitIROperand(ctx, conditionVal, IREmitMode::Default);
                    emit(")\n{\n");

                    // Now iterate over the `case`s of the branch
                    UInt caseIndex = 0;
                    UInt caseCount = t->getCaseCount();
                    while(caseIndex < caseCount)
                    {
                        // We are going to extract one case here,
                        // but we might need to fold additional
                        // cases into it, if they share the
                        // same label.
                        //
                        // Note: this makes assumptions that the
                        // IR code generator orders cases such
                        // that: (1) cases with the same label
                        // are consecutive, and (2) any case
                        // that "falls through" to another must
                        // come right before it in the list.
                        auto caseVal = t->getCaseValue(caseIndex);
                        auto caseLabel = t->getCaseLabel(caseIndex);
                        caseIndex++;

                        // Emit the `case ...:` for this case, and any
                        // others that share the same label
                        for(;;)
                        {
                            emit("case ");
                            emitIROperand(ctx, caseVal, IREmitMode::Default);
                            emit(":\n");

                            if(caseIndex >= caseCount)
                                break;

                            auto nextCaseLabel = t->getCaseLabel(caseIndex);
                            if(nextCaseLabel != caseLabel)
                                break;

                            caseVal = t->getCaseValue(caseIndex);
                            caseIndex++;
                        }

                        // The label for the current `case` might also
                        // be the label used by the `default` case, so
                        // check for that here.
                        if(caseLabel == defaultLabel)
                        {
                            emit("default:\n");
                            defaultLabelHandled = true;
                        }

                        // Now we need to emit the statements that make
                        // up this case. The 99% case will be that it
                        // will terminate with a `break` (or a `return`,
                        // `continue`, etc.) and so we can pass in
                        // `nullptr` for the ending block.
                        IRBlock* caseEndLabel = nullptr;

                        // However, there is also the possibility that
                        // this case will fall through to the next, and
                        // so we need to prepare for that possibility here.
                        //
                        // If there is a next case, then we will set its
                        // label up as the "end" label when emitting
                        // the statements inside the block.
                        if(caseIndex < caseCount)
                        {
                            caseEndLabel = t->getCaseLabel(caseIndex);
                        }

                        // Now emit the statements for this case.
                        indent();
                        emit("{\n");
                        indent();
                        emitIRStmtsForBlocks(ctx, caseLabel, caseEndLabel, &subLabels);
                        dedent();
                        emit("}\n");
                        dedent();
                    }

                    // If we've gone through all the cases and haven't
                    // managed to encounter the `default:` label,
                    // then assume it is a distinct case and handle it here.
                    if(!defaultLabelHandled)
                    {
                        emit("default:\n");
                        indent();
                        emit("{\n");
                        indent();
                        emitIRStmtsForBlocks(ctx, defaultLabel, breakLabel, &subLabels);
                        emit("break;\n");
                        dedent();
                        emit("}\n");
                        dedent();
                    }

                    emit("}\n");
                    block = breakLabel;

                }
                break;
            }

            // After we've emitted the first block, we are safe from accidental
            // cases where we'd emit an entire loop body as a single `continue`,
            // so we can safely switch in whatever labels are intended to be used.
            useLabels = labels;

            // If we reach this point, then we've emitted
            // one block, and we have a new block where
            // control flow continues.
            //
            // We need to handle a special case here,
            // when control flow jumps back to the
            // starting block of the range we were
            // asked to work with:
            if (block == begin) return;
        }
    }

    // Is an IR function a definition? (otherwise it is a declaration)
    bool isDefinition(IRFunc* func)
    {
        // For now, we use a simple approach: a function is
        // a definition if it has any blocks, and a declaration otherwise.
        return func->getFirstBlock() != nullptr;
    }

    String getIRFuncName(
        IRFunc* func)
    {
        if (auto entryPointLayout = asEntryPoint(func))
        {
            // GLSL will always need to use `main` as the
            // name for an entry-point function, but other
            // targets should try to use the original name.
            //
            // TODO: always use `main`, and have any code
            // that wraps this know to use `main` instead
            // of the original entry-point name...
            //
            if (getTarget(context) != CodeGenTarget::GLSL)
            {
                return getText(entryPointLayout->entryPoint->getName());
            }

            // 

            return "main";
        }
        else
        {
            return getIRName(func);
        }
    }

    void emitIREntryPointAttributes_HLSL(
        EmitContext*        ctx,
        EntryPointLayout*   entryPointLayout)
    {
        auto profile = ctx->shared->effectiveProfile;
        auto stage = profile.GetStage();

        if(profile.getFamily() == ProfileFamily::DX)
        {
            if(profile.GetVersion() >= ProfileVersion::DX_6_1 )
            {
                char const* stageName = nullptr;
                switch(stage)
                {
            #define PROFILE_STAGE(ID, NAME, ENUM) \
                case Stage::ID: stageName = #NAME; break;

            #include "profile-defs.h"

                default:
                    break;
                }

                if(stageName)
                {
                    emit("[shader(\"");
                    emit(stageName);
                    emit("\")]");
                }
            }
        }

        switch (stage)
        {
        case Stage::Compute:
            {
                static const UInt kAxisCount = 3;
                UInt sizeAlongAxis[kAxisCount];

                // TODO: this is kind of gross because we are using a public
                // reflection API function, rather than some kind of internal
                // utility it forwards to...
                spReflectionEntryPoint_getComputeThreadGroupSize(
                    (SlangReflectionEntryPoint*)entryPointLayout,
                    kAxisCount,
                    &sizeAlongAxis[0]);

                emit("[numthreads(");
                for (int ii = 0; ii < 3; ++ii)
                {
                    if (ii != 0) emit(", ");
                    Emit(sizeAlongAxis[ii]);
                }
                emit(")]\n");
            }
            break;
        case Stage::Geometry:
        {
            if (auto attrib = entryPointLayout->entryPoint->FindModifier<MaxVertexCountAttribute>())
            {
                emit("[maxvertexcount(");
                Emit(attrib->value);
                emit(")]\n");
            }
            if (auto attrib = entryPointLayout->entryPoint->FindModifier<InstanceAttribute>())
            {
                emit("[instance(");
                Emit(attrib->value);
                emit(")]\n");
            }
        }
        break;
        // TODO: There are other stages that will need this kind of handling.
        default:
            break;
        }
    }

    void emitIREntryPointAttributes_GLSL(
        EmitContext*        /*ctx*/,
        EntryPointLayout*   entryPointLayout)
    {
        auto profile = entryPointLayout->profile;
        auto stage = profile.GetStage();

        switch (stage)
        {
        case Stage::Compute:
            {
                static const UInt kAxisCount = 3;
                UInt sizeAlongAxis[kAxisCount];

                // TODO: this is kind of gross because we are using a public
                // reflection API function, rather than some kind of internal
                // utility it forwards to...
                spReflectionEntryPoint_getComputeThreadGroupSize(
                    (SlangReflectionEntryPoint*)entryPointLayout,
                    kAxisCount,
                    &sizeAlongAxis[0]);

                emit("layout(");
                char const* axes[] = { "x", "y", "z" };
                for (int ii = 0; ii < 3; ++ii)
                {
                    if (ii != 0) emit(", ");
                    emit("local_size_");
                    emit(axes[ii]);
                    emit(" = ");
                    Emit(sizeAlongAxis[ii]);
                }
                emit(") in;");
            }
            break;
        case Stage::Geometry:
        {
            if (auto attrib = entryPointLayout->entryPoint->FindModifier<MaxVertexCountAttribute>())
            {
                emit("layout(max_vertices = ");
                Emit(attrib->value);
                emit(") out;\n");
            }
            if (auto attrib = entryPointLayout->entryPoint->FindModifier<InstanceAttribute>())
            {
                emit("layout(invocations = ");
                Emit(attrib->value);
                emit(") in;\n");
            }

            for(auto pp : entryPointLayout->entryPoint->GetParameters())
            {
                if(auto inputPrimitiveTypeModifier = pp->FindModifier<HLSLGeometryShaderInputPrimitiveTypeModifier>())
                {
                    if(inputPrimitiveTypeModifier->As<HLSLTriangleModifier>())
                    {
                        emit("layout(triangles) in;\n");
                    }
                    else if(inputPrimitiveTypeModifier->As<HLSLLineModifier>())
                    {
                        emit("layout(lines) in;\n");
                    }
                    else if(inputPrimitiveTypeModifier->As<HLSLLineAdjModifier>())
                    {
                        emit("layout(lines_adjacency) in;\n");
                    }
                    else if(inputPrimitiveTypeModifier->As<HLSLPointModifier>())
                    {
                        emit("layout(points) in;\n");
                    }
                    else if(inputPrimitiveTypeModifier->As<HLSLTriangleAdjModifier>())
                    {
                        emit("layout(triangles_adjacency) in;\n");
                    }
                }

                if(auto outputStreamType = pp->type->As<HLSLStreamOutputType>())
                {
                    if(outputStreamType->As<HLSLTriangleStreamType>())
                    {
                        emit("layout(triangle_strip) out;\n");
                    }
                    else if(outputStreamType->As<HLSLLineStreamType>())
                    {
                        emit("layout(line_strip) out;\n");
                    }
                    else if(outputStreamType->As<HLSLPointStreamType>())
                    {
                        emit("layout(points) out;\n");
                    }
                }
            }


        }
        break;
        // TODO: There are other stages that will need this kind of handling.
        default:
            break;
        }
    }

    void emitIREntryPointAttributes(
        EmitContext*        ctx,
        EntryPointLayout*   entryPointLayout)
    {
        switch(getTarget(ctx))
        {
        case CodeGenTarget::HLSL:
            emitIREntryPointAttributes_HLSL(ctx, entryPointLayout);
            break;

        case CodeGenTarget::GLSL:
            emitIREntryPointAttributes_GLSL(ctx, entryPointLayout);
            break;
        }
    }

    void emitPhiVarDecls(
        EmitContext*    ctx,
        IRFunc*         func)
    {
        // We will skip the first block, since its parameters are
        // the parameters of the whole function.
        auto bb = func->getFirstBlock();
        if (!bb)
            return;
        bb = bb->getNextBlock();

        for (; bb; bb = bb->getNextBlock())
        {
            for (auto pp = bb->getFirstParam(); pp; pp = pp->getNextParam())
            {
                emitIRType(ctx, pp->getDataType(), getIRName(pp));
                emit(";\n");
            }
        }
    }

    void emitIRSimpleFunc(
        EmitContext*    ctx,
        IRFunc*         func)
    {
        auto resultType = func->getResultType();

        // Put a newline before the function so that
        // the output will be more readable.
        emit("\n");

        // Deal with decorations that need
        // to be emitted as attributes
        auto entryPointLayout = asEntryPoint(func);
        if (entryPointLayout)
        {
            emitIREntryPointAttributes(ctx, entryPointLayout);
        }

        auto name = getIRFuncName(func);

        emitIRType(ctx, resultType, name);

        emit("(");
        auto firstParam = func->getFirstParam();
        for( auto pp = firstParam; pp; pp = pp->getNextParam() )
        {
            if(pp != firstParam)
                emit(", ");

            auto paramName = getIRName(pp);
            auto paramType = pp->getDataType();
            if (auto decor = pp->findDecoration<IRHighLevelDeclDecoration>())
            {
                if (decor->decl)
                {
                    auto primType = decor->decl->FindModifier<HLSLGeometryShaderInputPrimitiveTypeModifier>();
                    if (dynamic_cast<HLSLTriangleModifier*>(primType))
                        emit("triangle ");
                    else if (dynamic_cast<HLSLPointModifier*>(primType))
                        emit("point ");
                    else if (dynamic_cast<HLSLLineModifier*>(primType))
                        emit("line ");
                    else if (dynamic_cast<HLSLLineAdjModifier*>(primType))
                        emit("lineadj ");
                    else if (dynamic_cast<HLSLTriangleAdjModifier*>(primType))
                        emit("triangleadj ");
                }
            }
            emitIRParamType(ctx, paramType, paramName);

            emitIRSemantics(ctx, pp);
        }
        emit(")");


        emitIRSemantics(ctx, func);

        // TODO: encode declaration vs. definition
        if(isDefinition(func))
        {
            emit("\n{\n");
            indent();

            // HACK: forward-declare all the local variables needed for the
            // prameters of non-entry blocks.
            emitPhiVarDecls(ctx, func);

            // Need to emit the operations in the blocks of the function

            emitIRStmtsForBlocks(ctx, func->getFirstBlock(), nullptr, nullptr);

            dedent();
            emit("}\n");
        }
        else
        {
            emit(";\n");
        }
    }

    void emitIRParamType(
        EmitContext*    ctx,
        Type*           type,
        String const&   name)
    {
        // An `out` or `inout` parameter will have been
        // encoded as a parameter of pointer type, so
        // we need to decode that here.
        //
        if( auto outType = type->As<OutType>() )
        {
            emit("out ");
            type = outType->getValueType();
        }
        else if( auto inOutType = type->As<InOutType>() )
        {
            emit("inout ");
            type = inOutType->getValueType();
        }

        emitIRType(ctx, type, name);
    }

    void emitIRFuncDecl(
        EmitContext*    ctx,
        IRFunc*         func)
    {
        // We don't want to declare generic functions,
        // because none of our targets actually support them.
        if(func->getGenericDecl())
            return;

        // We also don't want to emit declarations for operations
        // that only appear in the IR as stand-ins for built-in
        // operations on that target.
        if (isTargetIntrinsic(ctx, func))
            return;

        // Finally, don't emit a declaration for an entry point,
        // because it might need meta-data attributes attached
        // to it, and the HLSL compiler will get upset if the
        // forward declaration doesn't *also* have those
        // attributes.
        if(asEntryPoint(func))
            return;


        // A function declaration doesn't have any IR basic blocks,
        // and as a result it *also* doesn't have the IR `param` instructions,
        // so we need to emit a declaration entirely from the type.

        auto funcType = func->getType();
        auto resultType = func->getResultType();

        auto name = getIRFuncName(func);

        emitIRType(ctx, resultType, name);

        emit("(");
        auto paramCount = funcType->getParamCount();
        for(UInt pp = 0; pp < paramCount; ++pp)
        {
            if(pp != 0)
                emit(", ");

            String paramName;
            paramName.append("_");
            paramName.append(pp);
            auto paramType = funcType->getParamType(pp);

            emitIRParamType(ctx, paramType, paramName);
        }
        emit(");\n");
    }

    EntryPointLayout* getEntryPointLayout(
        EmitContext*    /*context*/,
        IRFunc*         func)
    {
        if( auto layoutDecoration = func->findDecoration<IRLayoutDecoration>() )
        {
            return layoutDecoration->layout.As<EntryPointLayout>();
        }
        return nullptr;
    }

    EntryPointLayout* asEntryPoint(IRFunc* func)
    {
        if (auto layoutDecoration = func->findDecoration<IRLayoutDecoration>())
        {
            if (auto entryPointLayout = layoutDecoration->layout.As<EntryPointLayout>())
            {
                return entryPointLayout;
            }
        }

        return nullptr;
    }

    // Detect if the given IR function represents a
    // declaration of an intrinsic/builtin for the
    // current code-generation target.
    bool isTargetIntrinsic(
        EmitContext*    /*ctxt*/,
        IRFunc*         func)
    {
        // For now we do this in an overly simplistic
        // fashion: we say that *any* function declaration
        // (rather then definition) must be an intrinsic:
        return !isDefinition(func);
    }

    // Check whether a given value names a target intrinsic,
    // and return the IR function representing the instrinsic
    // if it does.
    IRFunc* asTargetIntrinsic(
        EmitContext*    ctxt,
        IRInst*         value)
    {
        if(!value)
            return nullptr;

        if(value->op == kIROp_specialize)
        {
            value = ((IRSpecialize*) value)->genericVal.get();
        }

        if(value->op != kIROp_Func)
            return nullptr;

        IRFunc* func = (IRFunc*) value;
        if(!isTargetIntrinsic(ctxt, func))
            return nullptr;

        return func;
    }

    void emitIRFunc(
        EmitContext*    ctx,
        IRFunc*         func)
    {
        if(func->getGenericDecl())
        {
            return;
        }

        if(!isDefinition(func))
        {
            // This is just a function declaration,
            // and so we want to emit it as such.
            // (Or maybe not emit it at all).

            // We do not emit the declaration for
            // functions that appear to be intrinsics/builtins
            // in the target langugae.
            if (isTargetIntrinsic(ctx, func))
                return;

            emitIRFuncDecl(ctx, func);
        }
        else
        {
            // The common case is that what we
            // have is just an ordinary function,
            // and we can emit it as such.
            emitIRSimpleFunc(ctx, func);
        }
    }

#if 0
    void emitIRStruct(
        EmitContext*    context,
        IRStructDecl*   structType)
    {
        emit("struct ");
        emit(getName(structType));
        emit("\n{\n");

        for(auto ff = structType->getFirstField(); ff; ff = ff->getNextField())
        {
            auto fieldType = ff->getFieldType();
            emitIRType(context, fieldType, getName(ff));

            emitIRSemantics(context, ff);

            emit(";\n");
        }
        emit("};\n");
    }
#endif

    void emitIRMatrixLayoutModifiers(
        EmitContext*    ctx,
        VarLayout*      layout)
    {
        // We need to handle the case where the variable has
        // a matrix type, and has been given a non-standard
        // layout attribute (for HLSL, `row_major` is the
        // non-standard layout).
        //
        if (auto matrixTypeLayout = layout->typeLayout.As<MatrixTypeLayout>())
        {
            auto target = ctx->shared->target;

            switch (target)
            {
            case CodeGenTarget::HLSL:
                switch (matrixTypeLayout->mode)
                {
                case kMatrixLayoutMode_ColumnMajor:
                    if(target == CodeGenTarget::GLSL)
                    emit("column_major ");
                    break;

                case kMatrixLayoutMode_RowMajor:
                    emit("row_major ");
                    break;
                }
                break;

            case CodeGenTarget::GLSL:
                // Reminder: the meaning of row/column major layout
                // in our semantics is the *opposite* of what GLSL
                // calls them, because what they call "columns"
                // are what we call "rows."
                //
                switch (matrixTypeLayout->mode)
                {
                case kMatrixLayoutMode_ColumnMajor:
                    if(target == CodeGenTarget::GLSL)
                    emit("layout(row_major)\n");
                    break;

                case kMatrixLayoutMode_RowMajor:
                    emit("layout(column_major)\n");
                    break;
                }
                break;

            default:
                break;
            }
            
        }

    }

    // Emit the `flat` qualifier if the underlying type
    // of the variable is an integer type.
    void maybeEmitGLSLFlatModifier(
        EmitContext*,
        Type*           valueType)
    {
        auto tt = valueType;
        if(auto vecType = tt->As<VectorExpressionType>())
            tt = vecType->elementType;
        if(auto vecType = tt->As<MatrixExpressionType>())
            tt = vecType->getElementType();

        auto baseType = tt->As<BasicExpressionType>();
        if(!baseType)
            return;

        switch(baseType->baseType)
        {
        default:
            break;

        case BaseType::Int:
        case BaseType::UInt:
        case BaseType::UInt64:
            Emit("flat ");
            break;
        }
    }

    void emitInterpolationModifiers(
        EmitContext*    ctx,
        VarDeclBase*    decl,
        Type*           valueType)
    {
        bool isGLSL = (ctx->shared->target == CodeGenTarget::GLSL);
        bool anyModifiers = false;

        if(decl->FindModifier<HLSLNoInterpolationModifier>())
        {
            anyModifiers = true;
            Emit(isGLSL ? "flat " : "nointerpolation ");
        }
        else if(decl->FindModifier<HLSLNoPerspectiveModifier>())
        {
            anyModifiers = true;
            Emit("noperspective ");
        }
        else if(decl->FindModifier<HLSLLinearModifier>())
        {
            anyModifiers = true;
            Emit(isGLSL ? "smooth " : "linear ");
        }
        else if(decl->FindModifier<HLSLSampleModifier>())
        {
            anyModifiers = true;
            Emit("sample ");
        }
        else if(decl->FindModifier<HLSLCentroidModifier>())
        {
            anyModifiers = true;
            Emit("centroid ");
        }

        // If the user didn't explicitly qualify a varying
        // with integer type, then we need to explicitly
        // add the `flat` modifier for GLSL.
        if(!anyModifiers && isGLSL)
        {
            maybeEmitGLSLFlatModifier(ctx, valueType);
        }
    }

    void emitInterpolationModifiers(
        EmitContext*    ctx,
        VarLayout*      layout,
        Type*           valueType)
    {
        emitInterpolationModifiers(ctx, layout->varDecl, valueType);
    }

    void emitIRVarModifiers(
        EmitContext*    ctx,
        VarLayout*      layout,
        Type*           valueType)
    {
        if (!layout)
            return;

        emitIRMatrixLayoutModifiers(ctx, layout);

        // As a special case, if we are emitting a GLSL declaration
        // for an HLSL `RWTexture*` then we need to emit a `format` layout qualifier.
        if(getTarget(context) == CodeGenTarget::GLSL)
        {
            if(auto resourceType = unwrapArray(valueType).As<TextureType>())
            {
                switch(resourceType->getAccess())
                {
                case SLANG_RESOURCE_ACCESS_READ_WRITE:
                case SLANG_RESOURCE_ACCESS_RASTER_ORDERED:
                    {
                        // TODO: at this point we need to look at the element
                        // type and figure out what format we want.
                        //
                        // For now just hack it and assume a fixed format.
                        Emit("layout(rgba32f)");

                        // TODO: we also need a way for users to specify what
                        // the format should be explicitly, to avoid having
                        // to have us infer things...
                    }
                    break;

                default:
                    break;
                }
            }
        }

        if (ctx->shared->target == CodeGenTarget::GLSL)
        {
            // Layout-related modifiers need to come before the declaration,
            // so deal with them here.
            emitGLSLLayoutQualifiers(layout, nullptr);

            // try to emit an appropriate leading qualifier
            for (auto rr : layout->resourceInfos)
            {
                switch (rr.kind)
                {
                case LayoutResourceKind::Uniform:
                case LayoutResourceKind::ShaderResource:
                case LayoutResourceKind::DescriptorTableSlot:
                    emit("uniform ");
                    break;

                case LayoutResourceKind::VaryingInput:
                    {
                        emit("in ");
                        if(layout->stage == Stage::Fragment)
                        {
                            maybeEmitGLSLFlatModifier(ctx, valueType);
                        }
                    }
                    break;

                case LayoutResourceKind::FragmentOutput:
                    {
                        emit("out ");
                        if(layout->stage != Stage::Fragment)
                        {
                            maybeEmitGLSLFlatModifier(ctx, valueType);
                        }
                    }
                    break;

                default:
                    continue;
                }

                break;
            }
        }
    }

    void emitHLSLParameterBlock(
        EmitContext*        ctx,
        IRGlobalVar*        varDecl,
        ParameterBlockType* type)
    {
        emit("cbuffer ");

        // Generate a dummy name for the block
        emit("_S");
        Emit(ctx->shared->uniqueIDCounter++);

        auto varLayout = getVarLayout(ctx, varDecl);
        assert(varLayout);

        EmitVarChain blockChain(varLayout);

        EmitVarChain containerChain = blockChain;
        EmitVarChain elementChain = blockChain;

        auto typeLayout = varLayout->typeLayout;
        if( auto parameterGroupTypeLayout = typeLayout.As<ParameterGroupTypeLayout>() )
        {
            containerChain = EmitVarChain(parameterGroupTypeLayout->containerVarLayout, &blockChain);
            elementChain = EmitVarChain(parameterGroupTypeLayout->elementVarLayout, &blockChain);

            typeLayout = parameterGroupTypeLayout->elementVarLayout->getTypeLayout();
        }

        emitHLSLRegisterSemantic(LayoutResourceKind::ConstantBuffer, &containerChain);

        emit("\n{\n");
        indent();

        auto elementType = type->getElementType();


        emitIRType(ctx, elementType, getIRName(varDecl));

        emitHLSLParameterGroupFieldLayoutSemantics(&elementChain);
        emit(";\n");

        dedent();
        emit("}\n");
    }

    void emitHLSLParameterGroup(
        EmitContext*                ctx,
        IRGlobalVar*                varDecl,
        UniformParameterGroupType*  type)
    {
        if(auto parameterBlockType = type->As<ParameterBlockType>())
        {
            emitHLSLParameterBlock(ctx, varDecl, parameterBlockType);
            return;
        }

        emit("cbuffer ");
        emit(getIRName(varDecl));

        auto varLayout = getVarLayout(ctx, varDecl);
        assert(varLayout);

        EmitVarChain blockChain(varLayout);

        EmitVarChain containerChain = blockChain;
        EmitVarChain elementChain = blockChain;

        auto typeLayout = varLayout->typeLayout;
        if( auto parameterGroupTypeLayout = typeLayout.As<ParameterGroupTypeLayout>() )
        {
            containerChain = EmitVarChain(parameterGroupTypeLayout->containerVarLayout, &blockChain);
            elementChain = EmitVarChain(parameterGroupTypeLayout->elementVarLayout, &blockChain);

            typeLayout = parameterGroupTypeLayout->elementVarLayout->typeLayout;
        }

        emitHLSLRegisterSemantic(LayoutResourceKind::ConstantBuffer, &containerChain);

        emit("\n{\n");
        indent();

        auto elementType = type->getElementType();


        if(auto declRefType = elementType->As<DeclRefType>())
        {
            if(auto structDeclRef = declRefType->declRef.As<StructDecl>())
            {
                auto structTypeLayout = typeLayout.As<StructTypeLayout>();
                assert(structTypeLayout);

                UInt fieldIndex = 0;
                for(auto ff : GetFields(structDeclRef))
                {
                    // TODO: need a plan to deal with the case where the IR-level
                    // `struct` type might not match the high-level type, so that
                    // the numbering of fields is different.
                    //
                    // The right plan is probably to require that the lowering pass
                    // create a fresh layout for any type/variable that it splits
                    // in this fashion, so that the layout information it attaches
                    // can always be assumed to apply to the actual instruciton.
                    //

                    auto fieldLayout = structTypeLayout->fields[fieldIndex++];

                    auto fieldType = GetType(ff);
                    if(fieldType->Equals(getSession()->getVoidType()))
                        continue;

                    emitIRVarModifiers(ctx, fieldLayout, fieldType);

                    emitIRType(ctx, fieldType, getIRName(ff));

                    emitHLSLParameterGroupFieldLayoutSemantics(fieldLayout, &elementChain);

                    emit(";\n");
                }
            }
        }
        else
        {
            emit("/* unexpected */");
        }

        dedent();
        emit("}\n");
    }

    void emitGLSLParameterBlock(
        EmitContext*        ctx,
        IRGlobalVar*        varDecl,
        ParameterBlockType* type)
    {
        auto varLayout = getVarLayout(ctx, varDecl);
        assert(varLayout);

        EmitVarChain blockChain(varLayout);

        EmitVarChain containerChain = blockChain;
        EmitVarChain elementChain = blockChain;

        auto typeLayout = varLayout->typeLayout;
        if( auto parameterGroupTypeLayout = typeLayout.As<ParameterGroupTypeLayout>() )
        {
            containerChain = EmitVarChain(parameterGroupTypeLayout->containerVarLayout, &blockChain);
            elementChain = EmitVarChain(parameterGroupTypeLayout->elementVarLayout, &blockChain);

            typeLayout = parameterGroupTypeLayout->elementVarLayout->getTypeLayout();
        }

        emitGLSLLayoutQualifier(LayoutResourceKind::DescriptorTableSlot, &containerChain);
        emit("layout(std140) uniform ");

        // Generate a dummy name for the block
        emit("_S");
        Emit(ctx->shared->uniqueIDCounter++);

        emit("\n{\n");
        indent();

        auto elementType = type->getElementType();

        emitIRType(ctx, elementType, getIRName(varDecl));
        emit(";\n");

        dedent();
        emit("};\n");
    }

    void emitGLSLParameterGroup(
        EmitContext*                ctx,
        IRGlobalVar*                varDecl,
        UniformParameterGroupType*  type)
    {
        if(auto parameterBlockType = type->As<ParameterBlockType>())
        {
            emitGLSLParameterBlock(ctx, varDecl, parameterBlockType);
            return;
        }

        auto varLayout = getVarLayout(ctx, varDecl);
        assert(varLayout);

        EmitVarChain blockChain(varLayout);

        EmitVarChain containerChain = blockChain;
        EmitVarChain elementChain = blockChain;

        auto typeLayout = varLayout->typeLayout;
        if( auto parameterGroupTypeLayout = typeLayout.As<ParameterGroupTypeLayout>() )
        {
            containerChain = EmitVarChain(parameterGroupTypeLayout->containerVarLayout, &blockChain);
            elementChain = EmitVarChain(parameterGroupTypeLayout->elementVarLayout, &blockChain);

            typeLayout = parameterGroupTypeLayout->elementVarLayout->typeLayout;
        }

        emitGLSLLayoutQualifier(LayoutResourceKind::DescriptorTableSlot, &containerChain);

        if(type->As<GLSLShaderStorageBufferType>())
        {
            emit("layout(std430) buffer ");
        }
        // TODO: what to do with HLSL `tbuffer` style buffers?
        else
        {
            emit("layout(std140) uniform ");
        }

        emit(getIRName(varDecl));

        emit("\n{\n");
        indent();

        auto elementType = type->getElementType();

        if(auto declRefType = elementType->As<DeclRefType>())
        {
            if(auto structDeclRef = declRefType->declRef.As<StructDecl>())
            {
                auto structTypeLayout = typeLayout.As<StructTypeLayout>();
                assert(structTypeLayout);

                UInt fieldIndex = 0;
                for(auto ff : GetFields(structDeclRef))
                {
                    // TODO: need a plan to deal with the case where the IR-level
                    // `struct` type might not match the high-level type, so that
                    // the numbering of fields is different.
                    //
                    // The right plan is probably to require that the lowering pass
                    // create a fresh layout for any type/variable that it splits
                    // in this fashion, so that the layout information it attaches
                    // can always be assumed to apply to the actual instruciton.
                    //

                    auto fieldLayout = structTypeLayout->fields[fieldIndex++];

                    auto fieldType = GetType(ff);
                    if(fieldType->Equals(getSession()->getVoidType()))
                        continue;

                    // Note: we will emit matrix-layout modifiers here, but
                    // we will refrain from emitting other modifiers that
                    // might not be appropriate to the context (e.g., we
                    // shouldn't go emitting `uniform` just because these
                    // things are uniform...).
                    //
                    // TODO: we need a more refined set of modifiers that
                    // we should allow on fields, because we might end
                    // up supporting layout that isn't the default for
                    // the given block type (e.g., something other than
                    // `std140` for a uniform block).
                    //
                    emitIRMatrixLayoutModifiers(ctx, fieldLayout);

                    emitIRType(ctx, fieldType, getIRName(ff));

//                    emitHLSLParameterGroupFieldLayoutSemantics(layout, fieldLayout);

                    emit(";\n");
                }
            }
        }
        else
        {
            emit("/* unexpected */");
        }

        // TODO: we should consider always giving parameter blocks
        // names when outputting GLSL, since that shouldn't affect
        // the semantics of things, and will reduce the risk of
        // collisions in the global namespace...

        dedent();
        emit("};\n");
    }

    void emitIRParameterGroup(
        EmitContext*                ctx,
        IRGlobalVar*                varDecl,
        UniformParameterGroupType*  type)
    {
        switch (ctx->shared->target)
        {
        case CodeGenTarget::HLSL:
            emitHLSLParameterGroup(ctx, varDecl, type);
            break;

        case CodeGenTarget::GLSL:
            emitGLSLParameterGroup(ctx, varDecl, type);
            break;
        }
    }

    void emitIRVar(
        EmitContext*    ctx,
        IRVar*          varDecl)
    {
        auto allocatedType = varDecl->getDataType();
        auto varType = allocatedType->getValueType();
//        auto addressSpace = allocatedType->getAddressSpace();

#if 0
        switch( varType->op )
        {
        case kIROp_ConstantBufferType:
        case kIROp_TextureBufferType:
            emitIRParameterGroup(ctx, varDecl, (IRUniformBufferType*) varType);
            return;

        default:
            break;
        }
#endif

        // Need to emit appropriate modifiers here.

        auto layout = getVarLayout(ctx, varDecl);
        
        emitIRVarModifiers(ctx, layout, varType);

#if 0
        switch (addressSpace)
        {
        default:
            break;

        case kIRAddressSpace_GroupShared:
            emit("groupshared ");
            break;
        }
#endif
        emitIRRateQualifiers(ctx, varDecl);

        emitIRType(ctx, varType, getIRName(varDecl));

        emitIRSemantics(ctx, varDecl);

        emitIRLayoutSemantics(ctx, varDecl);

        emit(";\n");
    }

    RefPtr<Type> unwrapArray(Type* type)
    {
        Type* t = type;
        while( auto arrayType = t->As<ArrayExpressionType>() )
        {
            t = arrayType->baseType;
        }
        return t;
    }

    void emitIRStructuredBuffer_GLSL(
        EmitContext*                    ctx,
        IRGlobalVar*                    varDecl,
        HLSLStructuredBufferTypeBase*   structuredBufferType)
    {
        // Shader storage buffer is an OpenGL 430 feature
        //
        // TODO: we should require either the extension or the version...
        requireGLSLVersion(430);

        emit("layout(std430) buffer ");

        // Generate a dummy name for the block
        emit("_S");
        Emit(ctx->shared->uniqueIDCounter++);

        emit(" {\n");
        indent();


        auto elementType = structuredBufferType->getElementType();
        emitIRType(ctx, elementType, getIRName(varDecl) + "[]");
        emit(";\n");

        dedent();
        emit("}");

        // TODO: we need to consider the case where the type of the variable is
        // an *array* of structured buffers, in which case we need to declare
        // the block as an array too.
        //
        // The main challenge here is that then the block will have a name,
        // and also the field inside the block will have a name, so that when
        // the user had written `a[i][j]` we now need to emit `a[i].someName[j]`.

        emit(";\n");
    }

    void emitIRGlobalVar(
        EmitContext*    ctx,
        IRGlobalVar*    varDecl)
    {
        auto allocatedType = varDecl->getDataType();
        auto varType = allocatedType->getValueType();

        String initFuncName;
        if (varDecl->getFirstBlock())
        {
            // A global variable with code means it has an initializer
            // associated with it. Eventually we'd like to emit that
            // initializer directly as an expression here, but for
            // now we'll emit it as a separate function.

            initFuncName = getIRName(varDecl);
            initFuncName.append("_init");

            emit("\n");
            emitIRType(ctx, varType, initFuncName);
            Emit("()\n{\n");
            indent();
            emitIRStmtsForBlocks(ctx, varDecl->getFirstBlock(), nullptr, nullptr);
            dedent();
            Emit("}\n");
        }

        // Emit a blank line so that the formatting is nicer.
        emit("\n");

        if (auto paramBlockType = varType->As<UniformParameterGroupType>())
        {
            emitIRParameterGroup(
                ctx,
                varDecl,
                paramBlockType);
            return;
        }

        if(getTarget(ctx) == CodeGenTarget::GLSL)
        {
            // When outputting GLSL, we need to transform any declaration of
            // a `*StructuredBuffer<T>` into an ordinary `buffer` declaration.
            if( auto structuredBufferType = unwrapArray(varType)->As<HLSLStructuredBufferTypeBase>() )
            {
                emitIRStructuredBuffer_GLSL(
                    ctx,
                    varDecl,
                    structuredBufferType);
                return;
            }

            // We want to skip the declaration of any system-value variables
            // when outputting GLSL (well, except in the case where they
            // actually *require* redeclaration...).
            //
            // TODO: can we detect this more robustly?
            if(getText(varDecl->mangledName).StartsWith("gl_"))
            {
                // The variable represents an OpenGL system value,
                // so we will assume that it doesn't need to be declared.
                //
                // TODO: handle case where we *should* declare the variable.
                return;
            }
        }

        // Need to emit appropriate modifiers here.

        auto layout = getVarLayout(ctx, varDecl);

        if (!layout)
        {
            // A global variable without a layout is just an
            // ordinary global variable, and may need special
            // modifiers to indicate it as such.
            switch (getTarget(ctx))
            {
            case CodeGenTarget::HLSL:
                // HLSL requires the `static` modifier on any
                // global variables; otherwise they are assumed
                // to be uniforms.
                Emit("static ");
                break;

            default:
                break;
            }
        }

        emitIRVarModifiers(ctx, layout, varType);

        emitIRType(ctx, varType, getIRName(varDecl));

        emitIRSemantics(ctx, varDecl);

        emitIRLayoutSemantics(ctx, varDecl);

        if (varDecl->getFirstBlock())
        {
            Emit(" = ");
            emit(initFuncName);
            Emit("()");
        }

        emit(";\n");
    }

    void emitIRGlobalConstantInitializer(
        EmitContext*        ctx,
        IRGlobalConstant*   valDecl)
    {
        // We expect to see only a single block
        auto block = valDecl->getFirstBlock();
        assert(block);
        assert(!block->getNextBlock());

        // We expect the terminator to be a `return`
        // instruction with a value.
        auto returnInst = (IRReturnVal*) block->getLastInst();
        assert(returnInst->op == kIROp_ReturnVal);

        // Now we want to emit the expression form of
        // the value being returned, and force any
        // sub-expressions to be included.
        emitIRInstExpr(ctx, returnInst->getVal(), IREmitMode::GlobalConstant);
    }

    void emitIRGlobalConstant(
        EmitContext*        ctx,
        IRGlobalConstant*   valDecl)
    {
        auto valType = valDecl->getDataType();

        if( ctx->shared->target != CodeGenTarget::GLSL )
        {
            emit("static ");
        }
        emit("const ");
        emitIRType(ctx, valType, getIRName(valDecl));

        if (valDecl->getFirstBlock())
        {
            // There is an initializer (which we expect for
            // any global constant...).

            emit(" = ");

            // We need to emit the entire initializer as
            // a single expression.
            emitIRGlobalConstantInitializer(ctx, valDecl);
        }


        emit(";\n");
    }

    void emitIRGlobalInst(
        EmitContext*    ctx,
        IRInst*         inst)
    {
        switch(inst->op)
        {
        case kIROp_Func:
            emitIRFunc(ctx, (IRFunc*) inst);
            break;

        case kIROp_global_var:
            emitIRGlobalVar(ctx, (IRGlobalVar*) inst);
            break;

        case kIROp_global_constant:
            emitIRGlobalConstant(ctx, (IRGlobalConstant*) inst);
            break;

        case kIROp_Var:
            emitIRVar(ctx, (IRVar*) inst);
            break;

        default:
            break;
        }
    }

    void ensureStructDecl(
        EmitContext*        ctx,
        DeclRef<StructDecl> declRef)
    {
        auto mangledName = getMangledName(declRef);
        if(ctx->shared->irDeclsVisited.Contains(mangledName))
            return;

        ctx->shared->irDeclsVisited.Add(mangledName);

        // First emit any types used by fields of this type
        for( auto ff : GetFields(declRef) )
        {
            if(ff.getDecl()->HasModifier<HLSLStaticModifier>())
                continue;

            auto fieldType = GetType(ff);
            emitIRUsedType(ctx, fieldType);
        }

        // Don't emit declarations for types that should be built-in on the target.
        //
        // TODO: This should really be checking if the type is a target intrinsic
        // for the chosen target, and not just whether it is globally declared
        // as a builtin (so that we can have types that are builtin in some cases,
        // but not others).
        if(declRef.getDecl()->HasModifier<BuiltinModifier>())
            return;

        Emit("\nstruct ");
        EmitDeclRef(declRef);
        Emit("\n{\n");
        indent();
        for( auto ff : GetFields(declRef) )
        {
            if(ff.getDecl()->HasModifier<HLSLStaticModifier>())
                continue;

            auto fieldType = GetType(ff);

            // Skip `void` fields that might have been created by legalization.
            if(fieldType->Equals(getSession()->getVoidType()))
                continue;

            // Note: GLSL doesn't support interpolation modifiers on `struct` fields
            if( ctx->shared->target != CodeGenTarget::GLSL )
            {
                emitInterpolationModifiers(ctx, ff.getDecl(), fieldType);
            }
            emitIRType(ctx, fieldType, getIRName(ff));

            EmitSemantics(ff.getDecl());

            emit(";\n");
        }
        dedent();
        Emit("};\n");
    }

    void emitIRUsedDeclRef(
        EmitContext*    ctx,
        DeclRef<Decl>   declRef)
    {
        auto decl = declRef.getDecl();

        if(decl->HasModifier<BuiltinTypeModifier>()
            || decl->HasModifier<MagicTypeModifier>())
        {
            return;
        }

        if( auto structDeclRef = declRef.As<StructDecl>() )
        {
            //
            ensureStructDecl(ctx, structDeclRef);
        }
    }

    // A type is going to be used by the IR, so
    // make sure that we have emitted whatever
    // it needs.
    void emitIRUsedType(
        EmitContext*    ctx,
        Type*           type)
    {
        if(type->As<BasicExpressionType>())
        {}
        else if(type->As<VectorExpressionType>())
        {}
        else if(type->As<MatrixExpressionType>())
        {}
        else if(auto arrayType = type->As<ArrayExpressionType>())
        {
            emitIRUsedType(ctx, arrayType->baseType);
        }
        else if( auto textureType = type->As<TextureTypeBase>() )
        {
            emitIRUsedType(ctx, textureType->elementType);
        }
        else if( auto genericType = type->As<BuiltinGenericType>() )
        {
            emitIRUsedType(ctx, genericType->elementType);
        }
        else if( auto ptrType = type->As<PtrTypeBase>() )
        {
            emitIRUsedType(ctx, ptrType->getValueType());
        }
        else if(type->As<SamplerStateType>() )
        {
        }
        else if( auto declRefType = type->As<DeclRefType>() )
        {
            auto declRef = declRefType->declRef;
            emitIRUsedDeclRef(ctx, declRef);
        }
        else
        {}
    }

    void emitIRUsedTypesForGlobalValueWithCode(
        EmitContext*            ctx,
        IRGlobalValueWithCode*  value)
    {
        for( auto bb = value->getFirstBlock(); bb; bb = bb->getNextBlock() )
        {
            for( auto pp = bb->getFirstParam(); pp; pp = pp->getNextParam() )
            {
                emitIRUsedTypesForValue(ctx, pp);
            }

            for( auto ii = bb->getFirstInst(); ii; ii = ii->getNextInst() )
            {
                emitIRUsedTypesForValue(ctx, ii);
            }
        }
    }

    void emitIRUsedTypesForValue(
        EmitContext*    ctx,
        IRInst*         value)
    {
        if(!value) return;
        switch( value->op )
        {
        case kIROp_Func:
            {
                auto irFunc = (IRFunc*) value;

                // Don't emit anything for a generic function,
                // since we only care about the types used by
                // the actual specializations.
                if (irFunc->getGenericDecl())
                    return;

                emitIRUsedType(ctx, irFunc->getResultType());

                emitIRUsedTypesForGlobalValueWithCode(ctx, irFunc);
            }
            break;

        case kIROp_global_var:
            {
                auto irGlobal = (IRGlobalVar*) value;
                emitIRUsedType(ctx, irGlobal->type);
                emitIRUsedTypesForGlobalValueWithCode(ctx, irGlobal);
            }
            break;

        case kIROp_global_constant:
            {
                auto irGlobal = (IRGlobalConstant*) value;
                emitIRUsedType(ctx, irGlobal->type);
                emitIRUsedTypesForGlobalValueWithCode(ctx, irGlobal);
            }
            break;

        default:
            {
                emitIRUsedType(ctx, value->type);
            }
            break;
        }
    }

    void emitIRUsedTypesForModule(
        EmitContext*    ctx,
        IRModule*       module)
    {
        for(auto ii : module->getGlobalInsts())
        {
            emitIRUsedTypesForValue(ctx, ii);
        }
    }

    void emitIRModule(
        EmitContext*    ctx,
        IRModule*       module)
    {
        emitIRUsedTypesForModule(ctx, module);

        // Before we emit code, we need to forward-declare
        // all of our functions so that we don't have to
        // sort them by dependencies.
        for(auto ii : module->getGlobalInsts())
        {
            if(ii->op != kIROp_Func)
                continue;

            auto func = (IRFunc*) ii;
            emitIRFuncDecl(ctx, func);
        }

        for(auto ii : module->getGlobalInsts())
        {
            emitIRGlobalInst(ctx, ii);
        }
    }


};

//


//

EntryPointLayout* findEntryPointLayout(
    ProgramLayout*      programLayout,
    EntryPointRequest*  entryPointRequest)
{
    for( auto entryPointLayout : programLayout->entryPoints )
    {
        if(entryPointLayout->entryPoint->getName() != entryPointRequest->name)
            continue;

        // TODO: We need to be careful about this check, since it relies on
        // the profile information in the layout matching that in the request.
        //
        // What we really seem to want here is some dictionary mapping the
        // `EntryPointRequest` directly to the `EntryPointLayout`, and maybe
        // that is precisely what we should build...
        //
        if(entryPointLayout->profile != entryPointRequest->profile)
            continue;

        // TODO: can't easily filter on translation unit here...
        // Ideally the `EntryPointRequest` should get filled in with a pointer
        // the specific function declaration that represents the entry point.

        return entryPointLayout.Ptr();
    }

    return nullptr;
}

// Given a layout computed for a whole program, find
// the corresponding layout to use when looking up
// variables at the global scope.
//
// It might be that the global scope was logically
// mapped to a constant buffer, so that we need
// to "unwrap" that declaration to get at the
// actual struct type inside.
StructTypeLayout* getGlobalStructLayout(
    ProgramLayout*  programLayout)
{
    auto globalScopeLayout = programLayout->globalScopeLayout->typeLayout;
    if( auto gs = globalScopeLayout.As<StructTypeLayout>() )
    {
        return gs.Ptr();
    }
    else if( auto globalConstantBufferLayout = globalScopeLayout.As<ParameterGroupTypeLayout>() )
    {
        // TODO: the `cbuffer` case really needs to be emitted very
        // carefully, but that is beyond the scope of what a simple rewriter
        // can easily do (without semantic analysis, etc.).
        //
        // The crux of the problem is that we need to collect all the
        // global-scope uniforms (but not declarations that don't involve
        // uniform storage...) and put them in a single `cbuffer` declaration,
        // so that we can give it an explicit location. The fields in that
        // declaration might use various type declarations, so we'd really
        // need to emit all the type declarations first, and that involves
        // some large scale reorderings.
        //
        // For now we will punt and just emit the declarations normally,
        // and hope that the global-scope block (`$Globals`) gets auto-assigned
        // the same location that we manually asigned it.

        auto elementTypeLayout = globalConstantBufferLayout->offsetElementTypeLayout;
        auto elementTypeStructLayout = elementTypeLayout.As<StructTypeLayout>();

        // We expect all constant buffers to contain `struct` types for now
        SLANG_RELEASE_ASSERT(elementTypeStructLayout);

        return elementTypeStructLayout.Ptr();
    }
    else
    {
        SLANG_UNEXPECTED("uhandled global-scope binding layout");
        return nullptr;
    }
}

void legalizeTypes(
    TypeLegalizationContext*    context,
    IRModule*                   module);

String emitEntryPoint(
    EntryPointRequest*  entryPoint,
    ProgramLayout*      programLayout,
    CodeGenTarget       target,
    TargetRequest*      targetRequest)
{
    auto translationUnit = entryPoint->getTranslationUnit();
   
    SharedEmitContext sharedContext;
    sharedContext.target = target;
    sharedContext.finalTarget = targetRequest->target;
    sharedContext.entryPoint = entryPoint;
    sharedContext.effectiveProfile = getEffectiveProfile(entryPoint, targetRequest);

    if (entryPoint)
    {
        sharedContext.entryPointLayout = findEntryPointLayout(
            programLayout,
            entryPoint);
    }

    sharedContext.programLayout = programLayout;

    // Layout information for the global scope is either an ordinary
    // `struct` in the common case, or a constant buffer in the case
    // where there were global-scope uniforms.
    StructTypeLayout* globalStructLayout = getGlobalStructLayout(programLayout);
    sharedContext.globalStructLayout = globalStructLayout;

    auto translationUnitSyntax = translationUnit->SyntaxNode.Ptr();

    EmitContext context;
    context.shared = &sharedContext;

    EmitVisitor visitor(&context);

    // We are going to create a fresh IR module that we will use to
    // clone any code needed by the user's entry point.
    IRSpecializationState* irSpecializationState = createIRSpecializationState(
        entryPoint,
        programLayout,
        target,
        targetRequest);
    {
        TypeLegalizationContext typeLegalizationContext;
        typeLegalizationContext.session = entryPoint->compileRequest->mSession;

        IRModule* irModule = getIRModule(irSpecializationState);
        auto compileRequest = translationUnit->compileRequest;

        typeLegalizationContext.irModule = irModule;

        specializeIRForEntryPoint(
            irSpecializationState,
            entryPoint,
            &sharedContext.extensionUsageTracker);

        validateIRModuleIfEnabled(compileRequest, irModule);

        // If the user specified the flag that they want us to dump
        // IR, then do it here, for the target-specific, but
        // un-specialized IR.
        if (translationUnit->compileRequest->shouldDumpIR)
        {
            dumpIR(irModule);
        }

        // Next, we need to ensure that the code we emit for
        // the target doesn't contain any operations that would
        // be illegal on the target platform. For example,
        // none of our target supports generics, or interfaces,
        // so we need to specialize those away.
        //
        specializeGenerics(irModule, sharedContext.target);

        // Debugging code for IR transformations...
#if 0
        fprintf(stderr, "### SPECIALIZED:\n");
        dumpIR(lowered);
        fprintf(stderr, "###\n");
#endif

        // After we've fully specialized all generics, and
        // "devirtualized" all the calls through interfaces,
        // we need to ensure that the code only uses types
        // that are legal on the chosen target.
        // 
        legalizeTypes(
            &typeLegalizationContext,
            irModule);

        //  Debugging output of legalization
#if 0
        fprintf(stderr, "### LEGALIZED:\n");
        dumpIR(lowered);
        fprintf(stderr, "###\n");
#endif

        // Once specialization and type legalization have been performed,
        // we should perform some of our basic optimization steps again,
        // to see if we can clean up any temporaries created by legalization.
        // (e.g., things that used to be aggregated might now be split up,
        // so that we can work with the individual fields).
        constructSSA(irModule);

        validateIRModuleIfEnabled(compileRequest, irModule);

        // After all of the required optimization and legalization
        // passes have been performed, we can emit target code from
        // the IR module.
        //
        // TODO: do we want to emit directly from IR, or translate the
        // IR back into AST for emission?
        visitor.emitIRModule(&context, irModule);
        
        // retain the specialized ir module, because the current
        // GlobalGenericParamSubstitution implementation may reference ir objects 
        targetRequest->compileRequest->compiledModules.Add(irModule);
    }
    destroyIRSpecializationState(irSpecializationState);

    String code = sharedContext.sb.ProduceString();
    sharedContext.sb.Clear();

    // Now that we've emitted the code for all the declaratiosn in the file,
    // it is time to stich together the final output.

    // There may be global-scope modifiers that we should emit now
    visitor.emitGLSLPreprocessorDirectives(translationUnitSyntax);
    String prefix = sharedContext.sb.ProduceString();


    StringBuilder finalResultBuilder;
    finalResultBuilder << prefix;

    finalResultBuilder << sharedContext.extensionUsageTracker.glslExtensionRequireLines.ProduceString();

    if (sharedContext.needHackSamplerForTexelFetch)
    {
        finalResultBuilder
            << "layout(set = 0, binding = "
            << programLayout->bindingForHackSampler
            << ") uniform sampler SLANG_hack_samplerForTexelFetch;\n";
    }

    finalResultBuilder << code;

    String finalResult = finalResultBuilder.ProduceString();
    
    return finalResult;
}

} // namespace Slang
