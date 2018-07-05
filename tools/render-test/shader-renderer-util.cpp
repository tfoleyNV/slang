// shader-renderer-util.cpp

#include "shader-renderer-util.h"

namespace renderer_test {

using namespace Slang;

/* static */Result ShaderRendererUtil::generateTextureResource(const InputTextureDesc& inputDesc, int bindFlags, Renderer* renderer, RefPtr<TextureResource>& textureOut)
{
    TextureData texData;
    generateTextureData(texData, inputDesc);
    return createTextureResource(inputDesc, texData, bindFlags, renderer, textureOut);
}

/* static */Result ShaderRendererUtil::createTextureResource(const InputTextureDesc& inputDesc, const TextureData& texData, int bindFlags, Renderer* renderer, RefPtr<TextureResource>& textureOut)
{
    TextureResource::Desc textureResourceDesc;
    textureResourceDesc.init(Resource::Type::Unknown);

    textureResourceDesc.format = Format::RGBA_Unorm_UInt8;
    textureResourceDesc.numMipLevels = texData.mipLevels;
    textureResourceDesc.arraySize = inputDesc.arrayLength;
    textureResourceDesc.bindFlags = bindFlags;

    // It's the same size in all dimensions
    switch (inputDesc.dimension)
    {
        case 1:
        {
            textureResourceDesc.type = Resource::Type::Texture1D;
            textureResourceDesc.size.init(inputDesc.size);
            break;
        }
        case 2:
        {
            textureResourceDesc.type = inputDesc.isCube ? Resource::Type::TextureCube : Resource::Type::Texture2D;
            textureResourceDesc.size.init(inputDesc.size, inputDesc.size);
            break;
        }
        case 3:
        {
            textureResourceDesc.type = Resource::Type::Texture3D;
            textureResourceDesc.size.init(inputDesc.size, inputDesc.size, inputDesc.size);
            break;
        }
    }

    const int effectiveArraySize = textureResourceDesc.calcEffectiveArraySize();
    const int numSubResources = textureResourceDesc.calcNumSubResources();

    Resource::Usage initialUsage = Resource::Usage::GenericRead;
    TextureResource::Data initData;

    List<ptrdiff_t> mipRowStrides;
    mipRowStrides.SetSize(textureResourceDesc.numMipLevels);
    List<const void*> subResources;
    subResources.SetSize(numSubResources);

    // Set up the src row strides
    for (int i = 0; i < textureResourceDesc.numMipLevels; i++)
    {
        const int mipWidth = TextureResource::calcMipSize(textureResourceDesc.size.width, i);
        mipRowStrides[i] = mipWidth * sizeof(uint32_t);
    }

    // Set up pointers the the data
    {
        int subResourceIndex = 0;
        const int numGen = int(texData.dataBuffer.Count());
        for (int i = 0; i < numSubResources; i++)
        {
            subResources[i] = texData.dataBuffer[subResourceIndex].Buffer();
            // Wrap around
            subResourceIndex = (subResourceIndex + 1 >= numGen) ? 0 : (subResourceIndex + 1);
        }
    }

    initData.mipRowStrides = mipRowStrides.Buffer();
    initData.numMips = textureResourceDesc.numMipLevels;
    initData.numSubResources = numSubResources;
    initData.subResources = subResources.Buffer();

    textureOut = renderer->createTextureResource(Resource::Usage::GenericRead, textureResourceDesc, &initData);

    return textureOut ? SLANG_OK : SLANG_FAIL;
}

/* static */Result ShaderRendererUtil::createBufferResource(const InputBufferDesc& inputDesc, bool isOutput, size_t bufferSize, const void* initData, Renderer* renderer, Slang::RefPtr<BufferResource>& bufferOut)
{
    Resource::Usage initialUsage = Resource::Usage::GenericRead;

    BufferResource::Desc srcDesc;
    srcDesc.init(bufferSize);
    srcDesc.format = inputDesc.format;

    int bindFlags = 0;
    if (inputDesc.type == InputBufferType::ConstantBuffer)
    {
        bindFlags |= Resource::BindFlag::ConstantBuffer;
        srcDesc.cpuAccessFlags |= Resource::AccessFlag::Write;
        initialUsage = Resource::Usage::ConstantBuffer;
    }
    else
    {
        bindFlags |= Resource::BindFlag::UnorderedAccess | Resource::BindFlag::PixelShaderResource | Resource::BindFlag::NonPixelShaderResource;
        srcDesc.elementSize = inputDesc.stride;
        initialUsage = Resource::Usage::UnorderedAccess;
    }

    if (isOutput)
    {
        srcDesc.cpuAccessFlags |= Resource::AccessFlag::Read;
    }

    srcDesc.bindFlags = bindFlags;

    RefPtr<BufferResource> bufferResource = renderer->createBufferResource(initialUsage, srcDesc, initData);
    if (!bufferResource)
    {
        return SLANG_FAIL;
    }

    bufferOut = bufferResource;
    return SLANG_OK;
}

static SamplerState::Desc _calcSamplerDesc(const InputSamplerDesc& srcDesc)
{
    SamplerState::Desc dstDesc;
    if (srcDesc.isCompareSampler)
    {
        dstDesc.reductionOp = TextureReductionOp::Comparison;
        dstDesc.comparisonFunc = ComparisonFunc::Less;
    }
    return dstDesc;
}

static SamplerState* _createSamplerState(
    Renderer*               renderer,
    const InputSamplerDesc& srcDesc)
{
    return renderer->createSamplerState(_calcSamplerDesc(srcDesc));
}

/* static */BindingStateImpl::RegisterRange ShaderRendererUtil::calcRegisterRange(Renderer* renderer, const ShaderInputLayoutEntry& entry)
{
    typedef BindingStateImpl::RegisterRange RegisterRange;

    BindingStyle bindingStyle = RendererUtil::getBindingStyle(renderer->getRendererType());

    switch (bindingStyle)
    {
        case BindingStyle::DirectX:
        {
            return RegisterRange::makeSingle(entry.hlslBinding);
        }
        case BindingStyle::Vulkan:
        {
            // USe OpenGls for now
            // fallthru
        }
        case BindingStyle::OpenGl:
        {
            const int count = int(entry.glslBinding.Count());

            if (count <= 0)
            {
                break;
            }

            int baseIndex = entry.glslBinding[0];
            // Make sure they are contiguous
            for (int i = 1; i < int(entry.glslBinding.Count()); ++i)
            {
                if (baseIndex + i != entry.glslBinding[i])
                {
                    assert("Bindings must be contiguous");
                    break;
                }
            }
            return RegisterRange::makeRange(baseIndex, count);
        }
        /* case BindingStyle::Vulkan:
        {
        } */
        default: break;
    }
    // Return invalid
    return RegisterRange::makeInvalid();
}

/* static */Result ShaderRendererUtil::createBindingState(ShaderInputLayoutEntry* srcEntries, int numEntries, Renderer* renderer, BindingStateImpl** outBindingState)
{
    const int textureBindFlags = Resource::BindFlag::NonPixelShaderResource | Resource::BindFlag::PixelShaderResource;

    List<DescriptorSetLayout::SlotRangeDesc> slotRangeDescs;

    for (int i = 0; i < numEntries; i++)
    {
        const ShaderInputLayoutEntry& srcEntry = srcEntries[i];

        const BindingStateImpl::RegisterRange registerSet = calcRegisterRange(renderer, srcEntry);
        if (!registerSet.isValid())
        {
            assert(!"Couldn't find a binding");
            return SLANG_FAIL;
        }

        DescriptorSetLayout::SlotRangeDesc slotRangeDesc;

        switch (srcEntry.type)
        {
            case ShaderInputType::Buffer:
                {
                    const InputBufferDesc& srcBuffer = srcEntry.bufferDesc;

                    switch (srcBuffer.type)
                    {
                    case InputBufferType::ConstantBuffer:
                        slotRangeDesc.type = DescriptorSlotType::UniformBuffer;
                        break;

                    case InputBufferType::StorageBuffer:
                        slotRangeDesc.type = DescriptorSlotType::StorageBuffer;
                        break;
                    }
                }
                break;

            case ShaderInputType::CombinedTextureSampler:
                {
                    slotRangeDesc.type = DescriptorSlotType::CombinedImageSampler;
                }
                break;

            case ShaderInputType::Texture:
                {
                    if (srcEntry.textureDesc.isRWTexture)
                    {
                        slotRangeDesc.type = DescriptorSlotType::StorageImage;
                    }
                    else
                    {
                        slotRangeDesc.type = DescriptorSlotType::SampledImage;
                    }
                }
                break;

            case ShaderInputType::Sampler:
                slotRangeDesc.type = DescriptorSlotType::Sampler;
                break;

            default:
                assert(!"Unhandled type");
                return SLANG_FAIL;
        }
    }

    DescriptorSetLayout::Desc descriptorSetLayoutDesc;
    descriptorSetLayoutDesc.slotRangeCount = slotRangeDescs.Count();
    descriptorSetLayoutDesc.slotRanges = slotRangeDescs.Buffer();

    DescriptorSetLayout* descriptorSetLayout = renderer->createDescriptorSetLayout(descriptorSetLayoutDesc);
    if(!descriptorSetLayout) return SLANG_FAIL;

    DescriptorSet* descriptorSet = renderer->createDescriptorSet(descriptorSetLayout);
    if(!descriptorSet) return SLANG_FAIL;

    for (int i = 0; i < numEntries; i++)
    {
        const ShaderInputLayoutEntry& srcEntry = srcEntries[i];

        switch (srcEntry.type)
        {
            case ShaderInputType::Buffer:
                {
                    const InputBufferDesc& srcBuffer = srcEntry.bufferDesc;

                    const size_t bufferSize = srcEntry.bufferData.Count() * sizeof(uint32_t);

                    RefPtr<BufferResource> bufferResource;
                    SLANG_RETURN_ON_FAIL(createBufferResource(srcEntry.bufferDesc, srcEntry.isOutput, bufferSize, srcEntry.bufferData.Buffer(), renderer, bufferResource));

                    ResourceView::Desc viewDesc;
                    viewDesc.usage = Resource::Usage::PixelShaderResource;
                    auto bufferView = renderer->createBufferView(
                        bufferResource,
                        viewDesc);

                    descriptorSet->setResource(i, 0, bufferView);
                }
                break;

            case ShaderInputType::CombinedTextureSampler:
                {
                    RefPtr<TextureResource> texture;
                    SLANG_RETURN_ON_FAIL(generateTextureResource(srcEntry.textureDesc, textureBindFlags, renderer, texture));

                    auto sampler = _createSamplerState(renderer, srcEntry.samplerDesc);

                    ResourceView::Desc viewDesc;
                    viewDesc.usage = Resource::Usage::PixelShaderResource;
                    auto textureView = renderer->createTextureView(
                        texture,
                        viewDesc);

                    descriptorSet->setCombinedTextureSampler(i, 0, textureView, sampler);
                }
                break;

            case ShaderInputType::Texture:
                {
                    RefPtr<TextureResource> texture;
                    SLANG_RETURN_ON_FAIL(generateTextureResource(srcEntry.textureDesc, textureBindFlags, renderer, texture));

                    ResourceView::Desc viewDesc;
                    viewDesc.usage = Resource::Usage::PixelShaderResource;
                    auto textureView = renderer->createTextureView(
                        texture,
                        viewDesc);

                    descriptorSet->setResource(i, 0, textureView);
                }
                break;

            case ShaderInputType::Sampler:
                auto sampler = _createSamplerState(renderer, srcEntry.samplerDesc);
                descriptorSet->setSampler(i, 0, sampler);
                break;

            default:
                assert(!"Unhandled type");
                return SLANG_FAIL;
        }
    }


    return SLANG_OK;
}

/* static */Result ShaderRendererUtil::createBindingState(const ShaderInputLayout& layout, Renderer* renderer, BindingStateImpl** outBindingState)
{
    SLANG_RETURN_ON_FAIL(createBindingState(layout.entries.Buffer(), int(layout.entries.Count()), renderer, descOut));
    (*outBindingState)->m_numRenderTargets = layout.numRenderTargets;
    return SLANG_OK;
}

} // renderer_test
