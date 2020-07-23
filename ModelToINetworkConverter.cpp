//
// Copyright © 2017 Arm Ltd. All rights reserved.
// SPDX-License-Identifier: MIT
//

#define LOG_TAG "ArmnnDriver"

#include "ModelToINetworkConverter.hpp"
#include "Utils.hpp"

#include <log/log.h>
#include <type_traits>

namespace armnn_driver
{

template<typename HalPolicy>
ModelToINetworkConverter<HalPolicy>::ModelToINetworkConverter(const std::vector<armnn::BackendId>& backends,
    const HalModel& model,
    const std::set<unsigned int>& forcedUnsupportedOperations)
    : m_Data(backends)
    , m_Model(model)
    , m_ForcedUnsupportedOperations(forcedUnsupportedOperations)
    , m_ConversionResult(ConversionResult::Success)
{
    try
    {
        Convert();
    }
    catch (std::exception& e)
    {
        m_ConversionResult = ConversionResult::UnsupportedFeature;
        ALOGE("%s: Unexpected exception: %s", __func__, e.what());
        assert(false);
    }
}

template<typename HalPolicy>
void ModelToINetworkConverter<HalPolicy>::Convert()
{
    using HalModel       = typename HalPolicy::Model;
    using HalOperand     = typename HalPolicy::Operand;
    using HalOperandType = typename HalPolicy::OperandType;

    ALOGV("ModelToINetworkConverter::Convert(): %s", GetModelSummary<HalModel>(m_Model).c_str());

    // map the memory pool into shared pointers
    m_Data.m_MemPools.clear();
    if (!setRunTimePoolInfosFromHidlMemories(&m_Data.m_MemPools, m_Model.pools))
    {
        Fail("%s: Setting of run time pool infos from Hidl Memories has failed.", __func__);
        m_ConversionResult = ConversionResult::ErrorMappingPools;
        return;
    }

    uint32_t totalPoolSize = 0;
    for (auto&& pool : m_Model.pools)
    {
        totalPoolSize += pool.size();
    }

    using NetworkOptions = std::vector<armnn::BackendOptions>;
    NetworkOptions networkOptions;
    armnn::BackendOptions shapeInferenceMethodOption("ShapeInferenceMethod",
                                                    {
                                                            { "InferAndValidate", true }
                                                    });

    networkOptions.push_back(shapeInferenceMethodOption);

    // Create armnn::INetwork
    m_Data.m_Network = armnn::INetwork::Create(networkOptions);

    // add operations to it
    // track which layer outputs each operand
    ALOGV("ModelToINetworkConverter::Convert(): m_OutputSlotForOperand");
    m_Data.m_OutputSlotForOperand = std::vector<armnn::IOutputSlot*>(getMainModel(m_Model).operands.size(), nullptr);
    try
    {
        ALOGV("ModelToINetworkConverter::Convert(): for getMainModel(m_Model).inputIndexes.size()");
        for (uint32_t i = 0; i < getMainModel(m_Model).inputIndexes.size(); i++)
        {
            ALOGV("ModelToINetworkConverter::Convert(): getMainModel(m_Model).inputIndexes[i]");
            // inputs in android nn are represented by operands
            uint32_t inputIndex = getMainModel(m_Model).inputIndexes[i];
            ALOGV("ModelToINetworkConverter::Convert(): getMainModel(m_Model).operands[inputIndex];");
            const HalOperand& operand = getMainModel(m_Model).operands[inputIndex];
            ALOGV("ModelToINetworkConverter::Convert(): GetTensorInfoForOperand(operand)");
            const armnn::TensorInfo& tensor = GetTensorInfoForOperand(operand);
            ALOGV("ModelToINetworkConverter::Convert(): m_Data.m_Network->AddInputLayer(i)");
            armnn::IConnectableLayer* layer = m_Data.m_Network->AddInputLayer(i);

            ALOGV("ModelToINetworkConverter::Convert(): layer->GetOutputSlot(0)");
            armnn::IOutputSlot& outputSlot = layer->GetOutputSlot(0);
            ALOGV("ModelToINetworkConverter::Convert(): outputSlot.SetTensorInfo(GetTensorInfoForOperand(operand))");
            outputSlot.SetTensorInfo(GetTensorInfoForOperand(operand));

            ALOGV("ModelToINetworkConverter::Convert(): m_Data.m_OutputSlotForOperand[inputIndex] = &outputSlot");
            // store for later layers
            m_Data.m_OutputSlotForOperand[inputIndex] = &outputSlot;
        }
    }
    catch (UnsupportedOperand<HalOperandType>& e)
    {
        Fail("%s: Operand type %s not supported in ArmnnDriver", __func__, toString(e.m_type).c_str());
        m_ConversionResult = ConversionResult::UnsupportedFeature;
    }
    catch (const armnn::InvalidArgumentException& e)
    {
        Fail("%s: Failed to convert input operand to TensorShape: %s", __func__, e.what());
        m_ConversionResult = ConversionResult::UnsupportedFeature;
    }

    for (uint32_t operationIdx = 0; operationIdx < getMainModel(m_Model).operations.size(); operationIdx++)
    {
        const auto& operation = getMainModel(m_Model).operations[operationIdx];

        bool ok = true;
        if (m_ForcedUnsupportedOperations.find(operationIdx) != m_ForcedUnsupportedOperations.end())
        {
            Fail("%s: Operation at index %i has been forced to be unsupported.", __func__, operationIdx);
            ok = false;
        }

        if (ok)
        {
            try
            {
                ok = HalPolicy::ConvertOperation(operation, m_Model, m_Data);
            }
            catch (UnsupportedOperand<HalOperandType>& e)
            {
                Fail("%s: Operand type %s not supported in ArmnnDriver", __func__, toString(e.m_type).c_str());
                ok = false;
            }
            catch (const armnn::InvalidArgumentException& e)
            {
                Fail("%s: Failed to convert operation in %s", __func__, e.what());
                ok = false;
            }
        }

        // Store whether this operation was successfully converted.
        m_OperationSupported.emplace(operationIdx, ok);

        // Any single operation failing will fail the entire conversion.
        // We still need to continue and check the other ones.
        if (!ok)
        {
            m_ConversionResult = ConversionResult::UnsupportedFeature;
        }
    }
    try
    {
        if (m_ConversionResult == ConversionResult::Success)
        {
            for (uint32_t i = 0; i < getMainModel(m_Model).outputIndexes.size(); i++)
            {
                // outputs in android nn are represented by operands
                uint32_t outputIndex = getMainModel(m_Model).outputIndexes[i];
                const HalOperand& operand = getMainModel(m_Model).operands[outputIndex];
                const armnn::TensorInfo& tensor = GetTensorInfoForOperand(operand);
                armnn::IConnectableLayer* layer = m_Data.m_Network->AddOutputLayer(i);

                assert(m_Data.m_OutputSlotForOperand[outputIndex]);
                m_Data.m_OutputSlotForOperand[outputIndex]->Connect(layer->GetInputSlot(0));
            }
        }
    }
    catch (const armnn::InvalidArgumentException& e)
    {
        Fail("%s: Failed to convert output operand to TensorShape: %s", __func__, e.what());
        m_ConversionResult = ConversionResult::UnsupportedFeature;
    }
}

template<typename HalPolicy>
bool ModelToINetworkConverter<HalPolicy>::IsOperationSupported(uint32_t operationIndex) const
{
    std::map<uint32_t, bool>::const_iterator it = m_OperationSupported.find(operationIndex);
    assert(it != m_OperationSupported.end());
    return it->second;
}

///
/// Class template specializations
///

template class ModelToINetworkConverter<hal_1_0::HalPolicy>;

#ifdef ARMNN_ANDROID_NN_V1_1
template class ModelToINetworkConverter<hal_1_1::HalPolicy>;
#endif

#ifdef ARMNN_ANDROID_NN_V1_2
template class ModelToINetworkConverter<hal_1_1::HalPolicy>;
template class ModelToINetworkConverter<hal_1_2::HalPolicy>;
#endif

#ifdef ARMNN_ANDROID_NN_V1_3
template class ModelToINetworkConverter<hal_1_1::HalPolicy>;
template class ModelToINetworkConverter<hal_1_2::HalPolicy>;
template class ModelToINetworkConverter<hal_1_3::HalPolicy>;
#endif

} // armnn_driver
