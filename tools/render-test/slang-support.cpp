// slang-support.cpp

#include "slang-support.h"

#include <assert.h>
#include <stdio.h>

namespace renderer_test {

struct SlangShaderCompilerWrapper : public ShaderCompiler
{
    ShaderCompiler*     innerCompiler;
    SlangCompileTarget  target;
    SlangSourceLanguage sourceLanguage;

	virtual ShaderProgram* compileProgram(ShaderCompileRequest const& request) override
	{
		SlangSession* slangSession = spCreateSession(NULL);
		SlangCompileRequest* slangRequest = spCreateCompileRequest(slangSession);

		spSetCodeGenTarget(slangRequest, target);

		// Define a macro so that shader code in a test can detect what language we
		// are nominally working with.
		char const* langDefine = nullptr;
		switch (sourceLanguage)
		{
		case SLANG_SOURCE_LANGUAGE_GLSL:    langDefine = "__GLSL__";    break;
		case SLANG_SOURCE_LANGUAGE_HLSL:    langDefine = "__HLSL__";    break;
		case SLANG_SOURCE_LANGUAGE_SLANG:   langDefine = "__SLANG__";   break;
		default:
			assert(!"unexpected");
			break;
		}
		spAddPreprocessorDefine(slangRequest, langDefine, "1");

        // If we aren't dealing with true Slang input, then don't enable checking.
        //
        // Note: do this before using command-line arguments to set flags, so
        // that we don't accidentally clobber other flags.
        if (sourceLanguage != SLANG_SOURCE_LANGUAGE_SLANG)
        {
            spSetCompileFlags(slangRequest, SLANG_COMPILE_FLAG_NO_CHECKING);
        }

        // Preocess any additional command-line options specified for Slang using
        // the `-xslang <arg>` option to `render-test`.
		spProcessCommandLineArguments(slangRequest, &gOptions.slangArgs[0], gOptions.slangArgCount);

		int computeTranslationUnit = 0;
		int vertexTranslationUnit = 0;
		int fragmentTranslationUnit = 0;
		char const* vertexEntryPointName = request.vertexShader.name;
		char const* fragmentEntryPointName = request.fragmentShader.name;
		char const* computeEntryPointName = request.computeShader.name;

		if (sourceLanguage == SLANG_SOURCE_LANGUAGE_GLSL)
		{
			// GLSL presents unique challenges because, frankly, it got the whole
			// compilation model wrong. One aspect of working around this is that
			// we will compile the same source file multiple times: once per
			// entry point, and we will have different preprocessor definitions
			// active in each case.

			vertexTranslationUnit = spAddTranslationUnit(slangRequest, sourceLanguage, nullptr);
			spAddTranslationUnitSourceString(slangRequest, vertexTranslationUnit, request.source.path, request.source.text);
			spTranslationUnit_addPreprocessorDefine(slangRequest, vertexTranslationUnit, "__GLSL_VERTEX__", "1");
			vertexEntryPointName = "main";

			fragmentTranslationUnit = spAddTranslationUnit(slangRequest, sourceLanguage, nullptr);
			spAddTranslationUnitSourceString(slangRequest, fragmentTranslationUnit, request.source.path, request.source.text);
			spTranslationUnit_addPreprocessorDefine(slangRequest, fragmentTranslationUnit, "__GLSL_FRAGMENT__", "1");
			fragmentEntryPointName = "main";

			computeTranslationUnit = spAddTranslationUnit(slangRequest, sourceLanguage, nullptr);
			spAddTranslationUnitSourceString(slangRequest, computeTranslationUnit, request.source.path, request.source.text);
			spTranslationUnit_addPreprocessorDefine(slangRequest, computeTranslationUnit, "__GLSL_COMPUTE__", "1");
			computeEntryPointName = "main";
		}
		else
		{
			int translationUnit = spAddTranslationUnit(slangRequest, sourceLanguage, nullptr);
			spAddTranslationUnitSourceString(slangRequest, translationUnit, request.source.path, request.source.text);

			vertexTranslationUnit = translationUnit;
			fragmentTranslationUnit = translationUnit;
			computeTranslationUnit = translationUnit;
		}


		ShaderProgram * result = nullptr;
        Slang::List<const char*> rawTypeNames;
        for (auto typeName : request.entryPointTypeArguments)
            rawTypeNames.Add(typeName.Buffer());
		if (request.computeShader.name)
		{
		    int computeEntryPoint = spAddEntryPointEx(slangRequest, computeTranslationUnit, 
                computeEntryPointName, 
                spFindProfile(slangSession, request.computeShader.profile),
                (int)rawTypeNames.Count(),
                rawTypeNames.Buffer());
			int compileErr = spCompile(slangRequest);
			if (auto diagnostics = spGetDiagnosticOutput(slangRequest))
			{
				fprintf(stderr, "%s", diagnostics);
			}
			if (!compileErr)
			{
				ShaderCompileRequest innerRequest = request;
				char const* computeCode = spGetEntryPointSource(slangRequest, computeEntryPoint);
				innerRequest.computeShader.source.text = computeCode;
				result = innerCompiler->compileProgram(innerRequest);
			}
		}
		else
		{
			int vertexEntryPoint = spAddEntryPointEx(slangRequest, vertexTranslationUnit, vertexEntryPointName, spFindProfile(slangSession, request.vertexShader.profile), (int)rawTypeNames.Count(), rawTypeNames.Buffer());
			int fragmentEntryPoint = spAddEntryPointEx(slangRequest, fragmentTranslationUnit, fragmentEntryPointName, spFindProfile(slangSession, request.fragmentShader.profile), (int)rawTypeNames.Count(), rawTypeNames.Buffer());

			int compileErr = spCompile(slangRequest);
			if (auto diagnostics = spGetDiagnosticOutput(slangRequest))
			{
				// TODO(tfoley): re-enable when I get a logging solution in place
	//            OutputDebugStringA(diagnostics);
				fprintf(stderr, "%s", diagnostics);
			}
			if (!compileErr)
			{
				ShaderCompileRequest innerRequest = request;

				char const* vertexCode = spGetEntryPointSource(slangRequest, vertexEntryPoint);
				char const* fragmentCode = spGetEntryPointSource(slangRequest, fragmentEntryPoint);

				innerRequest.vertexShader.source.text = vertexCode;
				innerRequest.fragmentShader.source.text = fragmentCode;

				result = innerCompiler->compileProgram(innerRequest);
			}
		}
        // We clean up the Slang compilation context and result *after*
        // we have run the downstream compiler, because Slang
        // owns the memory allocation for the generated text, and will
        // free it when we destroy the compilation result.
        spDestroyCompileRequest(slangRequest);
        spDestroySession(slangSession);

        return result;
    }
};

ShaderCompiler* createSlangShaderCompiler(
    ShaderCompiler*     innerCompiler,
    SlangSourceLanguage sourceLanguage,
    SlangCompileTarget  target)
{
    auto result = new SlangShaderCompilerWrapper();
    result->innerCompiler = innerCompiler;
    result->sourceLanguage = sourceLanguage;
    result->target = target;

    return result;

}


} // renderer_test
