/*
 * Copyright (c) 2018, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "builtin_op_importers.hpp"
#include "onnx2trt_utils.hpp"
#include "plugin.hpp"
#include "FancyActivation.hpp"
#include "ResizeNearest.hpp"
#include "Split.hpp"
#include "MyElementWise.hpp"
#include "ConstantOfShape.hpp"
#include "Expand.hpp"
#include "Print.hpp"
//#include "MyLocalCast.hpp"
#include "MyCast.hpp"
#include "NonZero.hpp"
#include "SkipLayerNormPlugin.hpp"
#include "QKVToContextPlugin.hpp"
#include "EmbedPosition.hpp"

#include "InstanceNormalization.hpp"


#include <numeric> // For std::iota
#include <iostream>

namespace onnx2trt {

namespace {

enum { BATCH_DIM = 0 };
bool debug_on = false;

//sds-temp, add print plugin

nvinfer1::ITensor* add_print_plugin(IImporterContext* ctx, ::ONNX_NAMESPACE::NodeProto const& node, nvinfer1::ITensor* tensor)
{

    std::vector<int> names;
    for( std::string name: node.output()) 
    {
        int num = atoi(name.c_str());
        names.push_back(num);
      
    }

    nvinfer1::IPluginV2Layer* layer = nullptr;

    if( tensor != nullptr) 
    {
        layer = ctx->addPluginV2(new PrintPlugin(names),{tensor});
    }
    return layer->getOutput(0);
}



// Returns false if the transpose does not require any data movement (i.e., it's equivalent to a reshape)
bool is_transpose_required(nvinfer1::Dims const& shape,
                           nvinfer1::Permutation const& perm) {
  int ndim = shape.nbDims;
  int prev_significant_dim = 0;
  for( int dst_i=0; dst_i<ndim; ++dst_i ) {
    int src_i = perm.order[dst_i];
    if( shape.d[src_i] != 1 ) {
      if( src_i < prev_significant_dim ) {
        return false;
      }
      prev_significant_dim = src_i;
    }
  }
  return true;
}

// Note: perm should not include the batch dim
nvinfer1::ITensor*
transpose_tensor(IImporterContext* ctx,
                 nvinfer1::ITensor& tensor,
                 nvinfer1::Permutation const& perm,
                 bool permute_dim_types=true) {
  nvinfer1::IShuffleLayer* layer = ctx->network()->addShuffle(tensor);
  if( !layer ) {
    return nullptr;
  }
  nvinfer1::Dims shape = tensor.getDimensions();
  // Check if we need to transpose the data
  if( !is_transpose_required(shape, perm) ) {
    layer->setFirstTranspose(perm);
  }
  // Transpose can be simplified to be a reshape if no data re-ordering is required.
  else
  {
    nvinfer1::Dims new_shape;
    new_shape.nbDims = shape.nbDims;
    for (int i = 0; i < new_shape.nbDims; i++)
    {
      new_shape.d[i] = shape.d[perm.order[i]];
    }
    layer->setReshapeDimensions(new_shape);
  }
  return layer->getOutput(0);
}

nvinfer1::ITensor*
move_tensor_dimension(IImporterContext* ctx,
                      nvinfer1::ITensor& tensor,
                      int from, int to) {
  int ndim = tensor.getDimensions().nbDims;
  if( !(0 <= from && from < ndim) ) { return nullptr; }
  if( !(0 <= to   && to   < ndim) ) { return nullptr; }
  std::vector<int> vperm;
  vperm.reserve(ndim);
  for( int i=0; i<ndim; ++i ) {
    vperm.push_back(i);
  }
  vperm.erase(vperm.begin() + from);
  vperm.insert(vperm.begin() + to, from);
  nvinfer1::Permutation perm;
  std::copy(vperm.begin(), vperm.end(), perm.order);
  return transpose_tensor(ctx, tensor, perm, false);
}

nvinfer1::ITensor*
reshape_tensor(IImporterContext* ctx,
               nvinfer1::ITensor& tensor,
               nvinfer1::Dims shape) {
  if( shape == tensor.getDimensions() ) {
    return &tensor;
  }
  nvinfer1::IShuffleLayer* layer = ctx->network()->addShuffle(tensor);
  if( !layer ) {
    return nullptr;
  }
  layer->setReshapeDimensions(shape);
  return layer->getOutput(0);
}

nvinfer1::ITensor*
flatten_tensor(IImporterContext* ctx,
               nvinfer1::ITensor& tensor,
               int axis=0) {
  nvinfer1::Dims shape = tensor.getDimensions();
  nvinfer1::Dims new_shape = shape;
  for( int i=axis+1; i<shape.nbDims; ++i ) {
    new_shape.d[axis] *= shape.d[i];
    new_shape.d[i] = 1;
  }
  return reshape_tensor(ctx, tensor, new_shape);
}

NodeImportResult unaryHelper(IImporterContext* ctx,
    const ::ONNX_NAMESPACE::NodeProto& node, std::vector<TensorOrWeights>& inputs, nvinfer1::UnaryOperation op)
{
    nvinfer1::ITensor& input = convertToTensor(inputs.at(0), ctx);
    nvinfer1::IUnaryLayer* layer = ctx->network()->addUnary(input, op);
    return {{layer->getOutput(0)}};
}

NodeImportResult activationHelper(IImporterContext* ctx,
    const ::ONNX_NAMESPACE::NodeProto& node, std::vector<TensorOrWeights>& inputs, nvinfer1::ActivationType op, float* alpha = nullptr, float* beta = nullptr)
{
    nvinfer1::ITensor& input = convertToTensor(inputs.at(0), ctx);
    nvinfer1::IActivationLayer* layer = ctx->network()->addActivation(input, op);
    if (alpha)
    {
        layer->setAlpha(*alpha);
    }
    if (beta)
    {
        layer->setBeta(*beta);
    }

    return {{layer->getOutput(0)}};
}

// Adds a constant scalar to the network in the form of a constant layer.
template <typename ScalarType>
nvinfer1::IConstantLayer* addConstantScalar(IImporterContext* ctx, ScalarType scalar, ShapedWeights::DataType type)
{
    ShapedWeights scalarWeights = ctx->createTempWeights(type, nvinfer1::Dims{0});
    static_cast<ScalarType*>(scalarWeights.values)[0] = scalar;

    #if 0
    //sds-temp转成float
    ShapedWeights::DataType _type = (int32_t)::ONNX_NAMESPACE::TensorProto::FLOAT;
    
    auto newW = ctx->createTempWeights(_type, scalarWeights.shape);
    bool ret = onnx2trt::getNewWeights(scalarWeights, newW);
    
    return ctx->network()->addConstant(newW.shape, newW);
	#endif
	return ctx->network()->addConstant(scalarWeights.shape, scalarWeights);
}

NodeImportResult
addScale(IImporterContext*   ctx,
         nvinfer1::ITensor&  tensor_,
         nvinfer1::ScaleMode mode,
         nvinfer1::Weights   shift,
         nvinfer1::Weights   scale,
         nvinfer1::Weights   power) {
  nvinfer1::ITensor* tensor_ptr = &tensor_;
  nvinfer1::Dims dims = tensor_ptr->getDimensions();
#if NV_TENSORRT_MAJOR >= 4
  bool need_to_expand_dims = (dims.nbDims != 3);
  nvinfer1::Dims orig_shape = dims;
  if( need_to_expand_dims ) {
    // Expand or squash dims to 3D
    nvinfer1::Dims new_shape = dims;
    while( new_shape.nbDims < 3 ) {
      new_shape.d[new_shape.nbDims++] = 1;
    }
    while( new_shape.nbDims > 3 ) {
      new_shape.d[2] *= new_shape.d[--new_shape.nbDims];
    }
    tensor_ptr = reshape_tensor(ctx, *tensor_ptr, new_shape);
    ASSERT(tensor_ptr, ErrorCode::kUNSUPPORTED_NODE);
    dims = tensor_ptr->getDimensions();
  }
#endif // NV_TENSORRT_MAJOR >= 4

  ASSERT(dims.nbDims == 3, ErrorCode::kUNSUPPORTED_NODE);
  // Fill in dtype for any unused (dummy) weights
  nvinfer1::DataType* dtype_ptr = nullptr;
  if( shift.count ) {
    dtype_ptr = &shift.type;
  }
  if( scale.count ) {
    ASSERT(!dtype_ptr || *dtype_ptr == scale.type,
           ErrorCode::kUNSUPPORTED_NODE);
    dtype_ptr = &scale.type;
  }
  if( power.count ) {
    ASSERT(!dtype_ptr || *dtype_ptr == power.type,
           ErrorCode::kUNSUPPORTED_NODE);
    dtype_ptr = &power.type;
  }
  ASSERT(dtype_ptr, ErrorCode::kINTERNAL_ERROR);
  shift.type = *dtype_ptr;
  scale.type = *dtype_ptr;
  power.type = *dtype_ptr;
  auto* layer = ctx->network()->addScale(
      *tensor_ptr, mode, shift, scale, power);
  ASSERT(layer, ErrorCode::kUNSUPPORTED_NODE);
  tensor_ptr = layer->getOutput(0);

#if NV_TENSORRT_MAJOR >= 4
  if( need_to_expand_dims ) {
    // Un-expand spatial dims back to 1D
    tensor_ptr = reshape_tensor(ctx, *tensor_ptr, orig_shape);
    ASSERT(tensor_ptr, ErrorCode::kUNSUPPORTED_NODE);
  }
#endif // NV_TENSORRT_MAJOR >= 4
  return {{tensor_ptr}};
}

// Explicit broadcasting for ONNX opset < 7
// This function adds extra dimensions to the end of rhs's shape in order to
// line up the dimensions based on the specified broadcasting axis.
Status applyLegacyBinaryOpBroadcasting(IImporterContext* ctx,
                                       ::ONNX_NAMESPACE::NodeProto const& node,
                                       TensorOrWeights& lhs,
                                       TensorOrWeights& rhs) {
  int lhs_ndim = lhs.shape().nbDims;
  int rhs_ndim = rhs.shape().nbDims;
  OnnxAttrs attrs(node);
  bool broadcasting_on = (attrs.count("axis") && attrs.count("broadcast") &&
                          attrs.get<int>("broadcast"));
  if (rhs_ndim >= lhs_ndim || !broadcasting_on) {
    return Status::success();
  }
  int axis = attrs.get<int>("axis");
  if( axis < 0 ) {
    axis += 1 + lhs_ndim; // Support negative indexing
  }
  // Note: axis=0 still means the batch dim here
  if( rhs.is_tensor() ) {
    // Batch dims of tensors must be aligned
    ASSERT(axis == BATCH_DIM, ErrorCode::kUNSUPPORTED_NODE);
  } else { // rhs is weights
    if( axis == BATCH_DIM ) {
      // Weights must broadcast across the batch dim
      ASSERT(rhs.shape().d[0] == 1, ErrorCode::kUNSUPPORTED_NODE);
    }
    axis -= 1; // Shift batch dim to align with tensors
  }
  int num_dims_to_add = lhs_ndim - (axis + rhs_ndim);
  ASSERT(num_dims_to_add >= 0, ErrorCode::kINVALID_NODE);
  if (num_dims_to_add == 0) {
    return Status::success();
  }
  nvinfer1::Dims new_shape = rhs.shape();
  for (int i=0; i<num_dims_to_add; ++i) {
    new_shape.d[new_shape.nbDims++] = 1;
  }
  if (rhs.is_weights()) {
    rhs.weights().shape = new_shape;
  } else {
    ASSERT(rhs.reset_tensor(reshape_tensor(ctx, rhs.tensor(), new_shape)),
           ErrorCode::kUNSUPPORTED_NODE);
  }
  return Status::success();
}

NodeImportResult
combineTensorsElementwise(IImporterContext* ctx,
                          ::ONNX_NAMESPACE::NodeProto const& node,
                          std::vector<TensorOrWeights>& inputs,
                          nvinfer1::ElementWiseOperation binary_op,
                          bool legacy_binary_op_broadcasting=false) {
  ASSERT(!inputs.empty(), ErrorCode::kINVALID_NODE);
  if (ctx->getOpsetVersion() < 7 && legacy_binary_op_broadcasting) {
    ASSERT(inputs.size() == 2, ErrorCode::kINTERNAL_ERROR);
    TRT_CHECK(applyLegacyBinaryOpBroadcasting(ctx, node, inputs[0], inputs[1]));
  }
  std::vector<nvinfer1::ITensor*> input_tensors;
  int ndim_max = -1;
  int tensors_ndim_max = -1;
  for( auto input : inputs ) {
    ndim_max = std::max(ndim_max, input.shape().nbDims);
    // Note: Tensor dims always exclude the batch dim, but weights may not
    if( input.is_tensor() ) {
      tensors_ndim_max = std::max(tensors_ndim_max, input.shape().nbDims);
    }
  }
  for( auto input : inputs ) {
    nvinfer1::ITensor* tensor_ptr;
#if NV_TENSORRT_MAJOR < 4
    ASSERT(input.is_tensor(), ErrorCode::kUNSUPPORTED_NODE);
    tensor_ptr = &input.tensor();
#else
    if( input.is_weights() ) {
      auto weights = input.weights();
      // Note: TRT supports broadcasting, but ranks must match
      if( input.shape().nbDims < ndim_max ) {
        weights.shape = expand_dims(weights.shape, ndim_max);
      }
      if (weights.shape.nbDims == tensors_ndim_max + 1) {
        // The weights contain a batch dim, which must be removed
        // Note: TRT Constant layer has implicit batch dim of 1
           ASSERT(weights.shape.d[BATCH_DIM] == 1, ErrorCode::kUNSUPPORTED_NODE);
        weights.shape = remove_dim(weights.shape, BATCH_DIM);
      }
#if 0
      //sds-temp转成float
      ShapedWeights::DataType _type = (int32_t)::ONNX_NAMESPACE::TensorProto::FLOAT;

      auto newW = ctx->createTempWeights(_type, weights.shape);
      bool ret = onnx2trt::getNewWeights(weights, newW);

      
      auto* layer = ctx->network()->addConstant(newW.shape, newW);
#endif
      auto* layer = ctx->network()->addConstant(weights.shape, weights);
      ASSERT(layer, ErrorCode::kUNSUPPORTED_NODE);
      tensor_ptr = layer->getOutput(0);
    } else {
      tensor_ptr = &input.tensor();
      // Support broadcasting for tensor inputs by expanding dimensions.
      if (tensor_ptr->getDimensions().nbDims != tensors_ndim_max)
      {
        nvinfer1::Dims new_dims = expand_dims(tensor_ptr->getDimensions(), tensors_ndim_max);
        tensor_ptr = reshape_tensor(ctx, *tensor_ptr, new_dims);
      }
      ASSERT(tensor_ptr->getDimensions().nbDims == tensors_ndim_max,
             ErrorCode::kUNSUPPORTED_NODE);
     }
#endif
    input_tensors.push_back(tensor_ptr);
  }
#if 1
  //转成float类型，elementwise不支持int32
  nvinfer1::DataType  dtype= nvinfer1::DataType::kFLOAT;
  std::vector<nvinfer1::ITensor*> input_tensors2;
  for (int i = 0; i < input_tensors.size(); i++)
  {
    nvinfer1::ITensor* tensor = input_tensors.at(i);
    if(tensor->getType() != nvinfer1::DataType::kINT32)
    {
        input_tensors2.push_back(tensor);
        continue;
    }
    //tensor->setType(nvinfer1::DataType::kFLOAT);
    nvinfer1::ILayer* layer_ptr =  ctx->addPluginV2Ext(
        new MyCastPlugin(nvinfer1::DataType::kINT32, nvinfer1::DataType::kFLOAT),
        {tensor});
    nvinfer1::ITensor* cast_tensor = layer_ptr->getOutput(0);
    input_tensors2.push_back(cast_tensor);
  }
#endif
    
    
  nvinfer1::ITensor* combined = input_tensors2.at(0);
  if( input_tensors2.size() == 1 ) {
    // Note: Single input must be wrapped in identity to avoid messing up network outputs
    return {{identity(ctx, combined)}};
  }
  for( size_t i=1; i<input_tensors2.size(); ++i ) {
    nvinfer1::ITensor* tensor = input_tensors2.at(i);
    ASSERT(tensor->getDimensions().nbDims == combined->getDimensions().nbDims,
           ErrorCode::kUNSUPPORTED_NODE);
    auto* layer = ctx->network()->addElementWise(
      *combined, *tensor, binary_op);
    ASSERT(layer, ErrorCode::kUNSUPPORTED_NODE);
    if(debug_on)
        combined = add_print_plugin(ctx, node,layer->getOutput(0));
    else
        combined = layer->getOutput(0);
  }
  return {{combined}};
}

//Begin added by shendasai for add op 'Equal', 'Less', 'Where', 20190815
NodeImportResult
combineTensorsMyElementwise(IImporterContext* ctx,
                          ::ONNX_NAMESPACE::NodeProto const& node,
                          std::vector<TensorOrWeights>& inputs,
                          MyElementWiseType my_op_type,
                          bool legacy_binary_op_broadcasting=false) {
  ASSERT(!inputs.empty(), ErrorCode::kINVALID_NODE);
   ASSERT(my_op_type <= MyElementWiseType::MAX_VALUE,  ErrorCode::kINVALID_NODE);
  if (ctx->getOpsetVersion() < 7 && legacy_binary_op_broadcasting) {
    ASSERT(inputs.size() == 2, ErrorCode::kINTERNAL_ERROR);
    TRT_CHECK(applyLegacyBinaryOpBroadcasting(ctx, node, inputs[0], inputs[1]));
  }
  std::vector<nvinfer1::ITensor*> input_tensors;
  int ndim_max = -1;
  int tensors_ndim_max = -1;
  for( auto input : inputs ) {
    ndim_max = std::max(ndim_max, input.shape().nbDims);
    // Note: Tensor dims always exclude the batch dim, but weights may not
    if( input.is_tensor() ) {
      tensors_ndim_max = std::max(tensors_ndim_max, input.shape().nbDims);
    }
  }

  for( auto input : inputs ) {
    nvinfer1::ITensor* tensor_ptr;
#if NV_TENSORRT_MAJOR < 4
    ASSERT(input.is_tensor(), ErrorCode::kUNSUPPORTED_NODE);
    tensor_ptr = &input.tensor();
#else
    if( input.is_weights() ) {
      //sds:如果是weights类型，必须用constant创建一个只有ouput的layer
      //    pytorch不区分weights和tensor，都是input
      //    tensorrt是区分input和tensor的。input必须是上面层的输出,weigths则是独立的。
      //    所以，要么把weights转为constant layer,要么就直接传给plugin，但需要单独处理(serialize等)
      auto weights = input.weights();
      // Note: TRT supports broadcasting, but ranks must match
      //sds:支持广播，但是rank一定要一致
      if( input.shape().nbDims < ndim_max ) {
        weights.shape = expand_dims(weights.shape, ndim_max);
      }
      //进入这个分支说明输入时tensor+weights，且wieghts 的rank多。这时候就要降维了
      if (weights.shape.nbDims == tensors_ndim_max + 1) {
        // The weights contain a batch dim, which must be removed
        // Note: TRT Constant layer has implicit batch dim of 1
        ASSERT(weights.shape.d[BATCH_DIM] == 1, ErrorCode::kUNSUPPORTED_NODE);
        weights.shape = remove_dim(weights.shape, BATCH_DIM);
      }
#if 0
      //sds-temp转成float
      ShapedWeights::DataType _type = (int32_t)::ONNX_NAMESPACE::TensorProto::FLOAT;

      auto newW = ctx->createTempWeights(_type, weights.shape);
      bool ret = onnx2trt::getNewWeights(weights, newW);

      
      auto* layer = ctx->network()->addConstant(newW.shape, newW);
#endif
      auto* layer = ctx->network()->addConstant(weights.shape, weights);
      ASSERT(layer, ErrorCode::kUNSUPPORTED_NODE);
      tensor_ptr = layer->getOutput(0);
    } else {
      tensor_ptr = &input.tensor();
      // Support broadcasting for tensor inputs by expanding dimensions.
      if (tensor_ptr->getDimensions().nbDims != tensors_ndim_max)
      {
        nvinfer1::Dims new_dims = expand_dims(tensor_ptr->getDimensions(), tensors_ndim_max);
        tensor_ptr = reshape_tensor(ctx, *tensor_ptr, new_dims);
      }
      ASSERT(tensor_ptr->getDimensions().nbDims == tensors_ndim_max,
             ErrorCode::kUNSUPPORTED_NODE);
     }
#endif
    input_tensors.push_back(tensor_ptr);
  }



#if 1
    //转成float类型，elementwise不支持int32
    nvinfer1::DataType  dtype= nvinfer1::DataType::kFLOAT;
    std::vector<nvinfer1::ITensor*> input_tensors2;
    for (int i = 0; i < input_tensors.size(); i++)
    {
      nvinfer1::ITensor* tensor = input_tensors.at(i);
      if(tensor->getType() != nvinfer1::DataType::kINT32)
      {
          input_tensors2.push_back(tensor);
          continue;
      }
      //tensor->setType(nvinfer1::DataType::kFLOAT);
      //sds-temp,只有tensor 且int32会走进来   
      nvinfer1::ILayer* layer_ptr =  ctx->addPluginV2Ext(
          new MyCastPlugin(nvinfer1::DataType::kINT32, nvinfer1::DataType::kFLOAT),
          {tensor});
      nvinfer1::ITensor* cast_tensor = layer_ptr->getOutput(0);
      input_tensors2.push_back(cast_tensor);
    }
#endif





  
  nvinfer1::ITensor* combined = input_tensors2.at(0);

  nvinfer1::IPluginV2Layer* layer2 = nullptr;

  if( input_tensors2.size() == 1 ) {
    // Note: Single input must be wrapped in identity to avoid messing up network outputs
    //return {{identity(ctx, combined)}};
    layer2 = ctx->addPluginV2(new MyElementWisePlugin(my_op_type),{combined});
    
  }
  
  // 'Equal' or 'Less'
  if(input_tensors2.size()==2) 
  {
    nvinfer1::ITensor* tensor = input_tensors2.at(1);
    ASSERT(tensor->getDimensions().nbDims == combined->getDimensions().nbDims,
           ErrorCode::kUNSUPPORTED_NODE);
    //add plugin for this op
    //PluginV2& plugin = getMyPluginMap()[my_op_type].second();
    //sds,可扩展性，后续改成函数指针map->创建plugin。
    layer2 = ctx->addPluginV2(new MyElementWisePlugin(my_op_type),{combined, tensor});
    
  }

  //'where'
  if(input_tensors2.size() ==3)
  {
      nvinfer1::ITensor* tensor1 = input_tensors2.at(1);
      nvinfer1::ITensor* tensor2 = input_tensors2.at(2);
      ASSERT(tensor1->getDimensions().nbDims == combined->getDimensions().nbDims,
             ErrorCode::kUNSUPPORTED_NODE);
      ASSERT(tensor2->getDimensions().nbDims == combined->getDimensions().nbDims,
             ErrorCode::kUNSUPPORTED_NODE);
      //add plugin for this op
      //PluginV2& plugin = getMyPluginMap()[my_op_type].second();
  
      //sds,可扩展性，后续改成函数指针map->创建plugin。

      {
          //layer = ctx->addPluginV2(new EqualPlugin(*combined, *tensor),
          layer2 = ctx->addPluginV2(new MyElementWisePlugin(my_op_type),{combined, tensor1, tensor2});
      }
  }

    //get output
    ASSERT(layer2, ErrorCode::kUNSUPPORTED_NODE);
    int noutput =1;//just one output
    ASSERT(layer2->getNbOutputs() == noutput, ErrorCode::kINTERNAL_ERROR);
    std::vector<TensorOrWeights> outputs;

    //add_print_plugin(ctx, node, layer2->getOutput(0));
    
    for( int i=0; i<noutput; ++i ) 
    {
    if(debug_on)
        outputs.push_back(add_print_plugin(ctx, node,layer2->getOutput(i)));
    else
        outputs.push_back(layer2->getOutput(i));
    }
    return outputs;
}
//End added by shendasai, 20190815


// Note: As of TRT 4, ElementWise + Constant is preferred over Scale layer
#if NV_TENSORRT_MAJOR < 4
Status check_broadcast_attrs(IImporterContext* ctx, OnnxAttrs const& attrs,
                             nvinfer1::Dims const& dims) {
  if (ctx->getOpsetVersion() < 7) {
    ASSERT(attrs.count("broadcast"), ErrorCode::kUNSUPPORTED_NODE);
    bool broadcast = attrs.get<int>("broadcast");
    ASSERT(broadcast || dims.nbDims == 1, ErrorCode::kINVALID_NODE);
    int axis = attrs.get<int>("axis", -1);
    TRT_CHECK(convert_axis(axis, dims.nbDims));
    ASSERT(axis == 0, ErrorCode::kUNSUPPORTED_NODE);
  }
  return Status::success();
}

enum ScaleOp {
  kSHIFT,
  kSCALE,
  kPOWER,
};

NodeImportResult importScaleOp(IImporterContext* ctx,
                               ::ONNX_NAMESPACE::NodeProto const& node,
                               TensorOrWeights& input0,
                               TensorOrWeights& input1,
                               ScaleOp op) {
  auto* tensor_ptr = (input0.is_tensor() ?
                      &input0.tensor() :
                      &input1.tensor());
  auto weights = (input0.is_weights() ?
                        input0.weights() :
                        input1.weights());
  nvinfer1::Dims dims = tensor_ptr->getDimensions();
  // Note: ONNX opset >= 7 uses Numpy-style broadcasting, so dims are padded
  // at the end with ones for broadcasting.
  weights.shape = squeeze_trailing_dims(weights.shape);
  nvinfer1::ScaleMode mode = get_scale_mode(weights.shape);
  if( mode == nvinfer1::ScaleMode::kELEMENTWISE ) {
    // TODO: TRT doesn't support including the batch dim in elementwise,
    //       but we can't do a more specific assertion here yet because
    //       the input tensor's shape may have been padded to WAR TRT's
    //       shape issues.
    ASSERT(get_shape_size(weights.shape) == get_shape_size(dims),
           ErrorCode::kUNSUPPORTED_NODE);
  } else if( mode == nvinfer1::ScaleMode::kCHANNEL ) {
    OnnxAttrs attrs(node);
    // TRT does not currently support full broadcasting
    TRT_CHECK(check_broadcast_attrs(ctx, attrs, dims));
    ASSERT(weights.shape.d[0] == dims.d[0],
           ErrorCode::kUNSUPPORTED_NODE);
  }
  nvinfer1::Weights shift_weights = {};
  nvinfer1::Weights scale_weights = {};
  nvinfer1::Weights power_weights = {};
  switch( op ) {
  case kSHIFT: shift_weights = weights; break;
  case kSCALE: scale_weights = weights; break;
  case kPOWER: power_weights = weights; break;
  }
  return addScale(
      ctx, *tensor_ptr, mode, shift_weights, scale_weights, power_weights);
}
#endif // NV_TENSORRT_MAJOR < 4

} // namespace

string_map<NodeImporter>& getBuiltinOpImporterMap() {
  static string_map<NodeImporter> builtin_op_importers;
  return builtin_op_importers;
}



namespace {

bool registerBuiltinOpImporter(std::string op,
                               NodeImporter const& importer) {
  bool inserted = getBuiltinOpImporterMap().insert({op, importer}).second;
  assert(inserted);
  return inserted;
}



#define IGNORE_UNUSED_GLOBAL(x) \
  static void _ignore_unused2_##x(); \
  static void _ignore_unused1_##x() { (void)_ignore_unused2_##x; (void)x; } \
  static void _ignore_unused2_##x() { (void)_ignore_unused1_##x; } \
  struct SwallowSemicolon##x {}

#define DECLARE_BUILTIN_OP_IMPORTER(op) \
  NodeImportResult import##op(IImporterContext* ctx, \
                              ::ONNX_NAMESPACE::NodeProto const& node, \
                              std::vector<TensorOrWeights>& inputs)

#define DEFINE_BUILTIN_OP_IMPORTER(op) \
  NodeImportResult import##op(IImporterContext* ctx, \
                          ::ONNX_NAMESPACE::NodeProto const& node, \
                          std::vector<TensorOrWeights>& inputs); \
  static const bool op##_registered_builtin_op = \
      registerBuiltinOpImporter(#op, import##op); \
  IGNORE_UNUSED_GLOBAL(op##_registered_builtin_op); \
  NodeImportResult import##op(IImporterContext* ctx, \
                              ::ONNX_NAMESPACE::NodeProto const& node, \
                              std::vector<TensorOrWeights>& inputs)

#define RETURN_FIRST_OUTPUT(layer) do { \
  nvinfer1::ILayer* _layer_ptr_tmp = layer; \
  ASSERT(_layer_ptr_tmp, ErrorCode::kUNSUPPORTED_NODE); \
  return {{_layer_ptr_tmp->getOutput(0)}}; \
} while(0)

#define RETURN_IDENTITY(input) do { \
  TensorOrWeights output = identity(ctx, input); \
  ASSERT(output, ErrorCode::kUNSUPPORTED_NODE); \
  return {{output}}; \
} while(0)

#if NV_TENSORRT_MAJOR >= 4
// Helper for ArgMax/ArgMin
NodeImportResult argMinMaxHelper(IImporterContext* ctx,
    const ::ONNX_NAMESPACE::NodeProto& node, std::vector<TensorOrWeights>& inputs, nvinfer1::TopKOperation op)
{
    ASSERT(inputs.at(0).is_tensor(), ErrorCode::kUNSUPPORTED_NODE);
    nvinfer1::ITensor& tensor = inputs.at(0).tensor();
    ASSERT(tensor.getType() != nvinfer1::DataType::kINT32, ErrorCode::kUNSUPPORTED_NODE);
    // Get attributes.
    OnnxAttrs attrs(node);
    int keepdims = attrs.get("keepdims", 1);
    int axis = attrs.get("axis", 0);
    int nbDims = tensor.getDimensions().nbDims;
    // Adjust axis to TensorRT format
    TRT_CHECK(convert_axis(axis, nbDims));

    uint32_t axisMask = 1 << axis;
    // Insert a TopK layer with k set to 1.
    nvinfer1::ITopKLayer* layer = ctx->network()->addTopK(tensor, op, 1, axisMask);
    ASSERT(layer, ErrorCode::kUNSUPPORTED_NODE);
    // We don't care about the TopK values, just the indices.
    nvinfer1::ITensor* indices = layer->getOutput(1);
    indices->setType(nvinfer1::DataType::kINT32);
    if (keepdims)
    {
        // The default behavior of the TopK layer is to keepdims.
        return {{indices}};
    }
    else
    {
        // Otherwise, we need to squeeze the axis dimension - we achieve this by reshaping.
        // The new dimensions are just the old dimensions with all values after axis shifted over.
        nvinfer1::Dims reshapeDims = indices->getDimensions();
        --reshapeDims.nbDims;
        // The axis dimension should be reduced to size 1 after performing the reduction.
        ASSERT(reshapeDims.d[axis] == 1, ErrorCode::kINVALID_VALUE);
        for (int i = axis; i < reshapeDims.nbDims; ++i)
        {
            reshapeDims.d[i] = reshapeDims.d[i + 1];
        }
        nvinfer1::IShuffleLayer* squeezeLayer = ctx->network()->addShuffle(*indices);
        squeezeLayer->setReshapeDimensions(reshapeDims);
        return {{squeezeLayer->getOutput(0)}};
    }
}
#endif // #if NV_TENSORRT_MAJOR >= 4

DEFINE_BUILTIN_OP_IMPORTER(Abs) {
  return apply_unary_function(ctx, inputs.at(0), nvinfer1::UnaryOperation::kABS);
}

DEFINE_BUILTIN_OP_IMPORTER(Acos)
{
    return unaryHelper(ctx, node, inputs, nvinfer1::UnaryOperation::kACOS);
}

DEFINE_BUILTIN_OP_IMPORTER(Acosh)
{
    return unaryHelper(ctx, node, inputs, nvinfer1::UnaryOperation::kACOSH);
}
#if 1
DEFINE_BUILTIN_OP_IMPORTER(Add) {
  ASSERT(inputs.size() == 2, ErrorCode::kINVALID_NODE);
  return combineTensorsElementwise(
      ctx, node, inputs, nvinfer1::ElementWiseOperation::kSUM, true);
}
#endif
#if NV_TENSORRT_MAJOR >= 4
DEFINE_BUILTIN_OP_IMPORTER(ArgMax)
{
    return argMinMaxHelper(ctx, node, inputs, nvinfer1::TopKOperation::kMAX);
}

DEFINE_BUILTIN_OP_IMPORTER(ArgMin)
{
    return argMinMaxHelper(ctx, node, inputs, nvinfer1::TopKOperation::kMIN);
}
#endif // #if NV_TENSORRT_MAJOR >= 4


DEFINE_BUILTIN_OP_IMPORTER(Asin)
{
    return unaryHelper(ctx, node, inputs, nvinfer1::UnaryOperation::kASIN);
}

DEFINE_BUILTIN_OP_IMPORTER(Asinh)
{
    return unaryHelper(ctx, node, inputs, nvinfer1::UnaryOperation::kASINH);
}

DEFINE_BUILTIN_OP_IMPORTER(Atan)
{
    return unaryHelper(ctx, node, inputs, nvinfer1::UnaryOperation::kATAN);
}

DEFINE_BUILTIN_OP_IMPORTER(Atanh)
{
    return unaryHelper(ctx, node, inputs, nvinfer1::UnaryOperation::kATANH);
}

DEFINE_BUILTIN_OP_IMPORTER(AveragePool) {
  // TensorRT 5.1 only supports up to opset 9.
  ASSERT(ctx->getOpsetVersion() < 10, ErrorCode::kUNSUPPORTED_NODE);
  ASSERT(inputs.at(0).is_tensor(), ErrorCode::kUNSUPPORTED_NODE);
  nvinfer1::ITensor* tensor_ptr = &inputs.at(0).tensor();
  nvinfer1::Dims dims = tensor_ptr->getDimensions();
#if NV_TENSORRT_MAJOR >= 4
  bool need_to_expand_dims = (dims.nbDims == 2);
  if( need_to_expand_dims ) {
    // Expand spatial dims from 1D to 2D
    nvinfer1::DimsCHW new_shape(dims.d[0], dims.d[1], 1);
    tensor_ptr = reshape_tensor(ctx, *tensor_ptr, new_shape);
    ASSERT(tensor_ptr, ErrorCode::kUNSUPPORTED_NODE);
    dims = tensor_ptr->getDimensions();
  }
#endif // NV_TENSORRT_MAJOR >= 4
  ASSERT(dims.nbDims == 3, ErrorCode::kUNSUPPORTED_NODE);
  nvinfer1::DimsHW kernel_size(1, 1), strides(1, 1), beg_padding(0, 0), end_padding(0, 0);
  nvinfer1::PaddingMode paddingMode;
  get_kernel_params(node, get_DimsHW_from_CHW(dims),
                    &kernel_size, &strides, &beg_padding, &end_padding, paddingMode);
  nvinfer1::IPoolingLayer* pooling_layer = ctx->network()->addPooling(
    *tensor_ptr, nvinfer1::PoolingType::kAVERAGE, kernel_size);
  nvinfer1::ILayer* layer = pooling_layer;
  ASSERT(layer, ErrorCode::kUNSUPPORTED_NODE);
  pooling_layer->setStride(strides);
  // Note: Average pooling requires special care with asymmetric padding
  //       because the need to exclude padding pixels from the average
  //       means we can't just use a pre-padding layer.
  nvinfer1::DimsHW pre_crop(0, 0), post_crop(0, 0);
  for( int d=0; d<2; ++d ) {
    if( end_padding.d[d] == beg_padding.d[d] ) {
      // Symmetric padding, nothing special needed
    } else if( end_padding.d[d] == beg_padding.d[d] + 1 ) {
      // Pad symmetrically such that we get one more output element at
      // the beginning, and then crop it off after the pooling operation.
      beg_padding.d[d] += strides.d[d];
      pre_crop.d[d] = 1;
    } else {
      bool supported_form_of_asymmetric_padding_for_AveragePool = false;
      ASSERT(supported_form_of_asymmetric_padding_for_AveragePool,
             ErrorCode::kUNSUPPORTED_NODE);
    }
  }
  pooling_layer->setPadding(beg_padding);
  if( pre_crop  != nvinfer1::DimsHW(0, 0) ||
      post_crop != nvinfer1::DimsHW(0, 0) ) {
    layer = ctx->network()->addPadding(*pooling_layer->getOutput(0),
                                       -pre_crop, -post_crop);
  }
  tensor_ptr = layer->getOutput(0);
  dims = tensor_ptr->getDimensions();
#if NV_TENSORRT_MAJOR >= 4
  if( need_to_expand_dims ) {
    // Un-expand spatial dims back to 1D
    nvinfer1::Dims new_shape{2, {dims.d[0], dims.d[1]}};
    tensor_ptr = reshape_tensor(ctx, *tensor_ptr, new_shape);
    ASSERT(tensor_ptr, ErrorCode::kUNSUPPORTED_NODE);
  }
#endif // NV_TENSORRT_MAJOR >= 4
  return {{tensor_ptr}};
}

DEFINE_BUILTIN_OP_IMPORTER(BatchNormalization) {
  ASSERT(inputs.at(0).is_tensor(),  ErrorCode::kUNSUPPORTED_NODE);
  ASSERT(inputs.at(1).is_weights(), ErrorCode::kUNSUPPORTED_NODE);
  ASSERT(inputs.at(2).is_weights(), ErrorCode::kUNSUPPORTED_NODE);
  ASSERT(inputs.at(3).is_weights(), ErrorCode::kUNSUPPORTED_NODE);
  ASSERT(inputs.at(4).is_weights(), ErrorCode::kUNSUPPORTED_NODE);
  nvinfer1::ITensor& tensor = inputs.at(0).tensor();
  auto scale_weights    = inputs.at(1).weights();
  auto bias_weights     = inputs.at(2).weights();
  auto mean_weights     = inputs.at(3).weights();
  auto variance_weights = inputs.at(4).weights();
  OnnxAttrs attrs(node);
  float eps = attrs.get<float>("epsilon", 1e-5f);
  // TODO: Check if ONNX "spatial" attribute is important (maybe changes mean and variance broadcasting?)
  ASSERT(scale_weights.type    == ::ONNX_NAMESPACE::TensorProto::FLOAT &&
         bias_weights.type     == ::ONNX_NAMESPACE::TensorProto::FLOAT &&
         mean_weights.type     == ::ONNX_NAMESPACE::TensorProto::FLOAT &&
         variance_weights.type == ::ONNX_NAMESPACE::TensorProto::FLOAT,
         ErrorCode::kUNSUPPORTED_NODE);
  nvinfer1::Dims dims = tensor.getDimensions();
  int nchan = dims.d[0];
  nvinfer1::Dims weights_shape{1, {nchan}};
  ASSERT(scale_weights.shape    == weights_shape, ErrorCode::kINVALID_NODE);
  ASSERT(bias_weights.shape     == weights_shape, ErrorCode::kINVALID_NODE);
  ASSERT(mean_weights.shape     == weights_shape, ErrorCode::kINVALID_NODE);
  ASSERT(variance_weights.shape == weights_shape, ErrorCode::kINVALID_NODE);
  auto combined_scale_weights = ctx->createTempWeights(scale_weights.type, scale_weights.shape);
  auto combined_bias_weights  = ctx->createTempWeights(bias_weights.type,  bias_weights.shape);
  size_t nweight = nchan;
  // Fold the weights together into a single bias and scale
  for( size_t i=0; i<nweight; ++i ) {
    float scale    = (static_cast<float const*>(scale_weights.values))[i];
    float bias     = (static_cast<float const*>(bias_weights.values))[i];
    float mean     = (static_cast<float const*>(mean_weights.values))[i];
    float variance = (static_cast<float const*>(variance_weights.values))[i];
    float& combined_scale_ref = const_cast<float*>(
        static_cast<float const*>(combined_scale_weights.values))[i];
    float& combined_bias_ref  = const_cast<float*>(
        static_cast<float const*>(combined_bias_weights.values))[i];
    combined_scale_ref = scale / sqrtf(variance + eps);
    combined_bias_ref  = bias - mean * combined_scale_ref;
  }
  return addScale(ctx, tensor, nvinfer1::ScaleMode::kCHANNEL,
                  combined_bias_weights, combined_scale_weights, {});
}

DEFINE_BUILTIN_OP_IMPORTER(Ceil) {
   return unaryHelper(ctx, node, inputs, nvinfer1::UnaryOperation::kCEIL);
}

//sds-check
//faiseq 用了一次，转Int32
//很奇怪，这个实现并不能使得输入输出的数据类型改变。而是只选择了精度  
DEFINE_BUILTIN_OP_IMPORTER(Cast) {
//sds-temp, do nothing
#if 0
    // Get input node.
    ASSERT(inputs.at(0).is_tensor(), ErrorCode::kUNSUPPORTED_NODE);
    OnnxAttrs attrs(node);
    // Check if datatype casted to is valid.
    nvinfer1::DataType dtype = nvinfer1::DataType::kFLOAT;
    ASSERT(convert_dtype(attrs.get<int32_t>("to"), &dtype), ErrorCode::kUNSUPPORTED_NODE);
    //sds,不支持INT32操作，转成16
    //sds-temp,目前只在nonzero后面会走到� 0~127nt16够用了。
    if(dtype == nvinfer1::DataType::kINT32)
        dtype = nvinfer1::DataType::kFLOAT;
    // Add the layer.
    nvinfer1::IIdentityLayer* layer = ctx->network()->addIdentity(inputs.at(0).tensor());
    layer->setPrecision(dtype);

    //layer->setOutputType(0, nvinfer1::DataType::kINT8);
    RETURN_FIRST_OUTPUT(layer);
#endif
    return inputs;
}

DEFINE_BUILTIN_OP_IMPORTER(Clip) {
  ASSERT(inputs.at(0).is_tensor(), ErrorCode::kUNSUPPORTED_NODE);
  OnnxAttrs attrs(node);
  // beta is the upper bound.
  float alpha = attrs.get("min", std::numeric_limits<float>::lowest());
  float beta = attrs.get("max", std::numeric_limits<float>::max());
  return activationHelper(ctx, node, inputs, nvinfer1::ActivationType::kCLIP, &alpha, &beta);
}

bool  concat_scalar(IImporterContext* ctx, std::vector<TensorOrWeights>& inputs, ShapedWeights& newWeights)
{

    std::vector<int32_t> temp_value;
    for( auto& input : inputs )
    {
        //必须是weights
        if(!input.is_weights()) return false;

        //必须是常量
        auto weights = input.weights();
        nvinfer1::Dims dims = weights.shape;
        if(dims.nbDims !=1 || dims.d[0] != 1) return false;
    
        int32_t temp = static_cast<int32_t*>(weights.values)[0];
        temp_value.push_back(temp);
    }

    int _size = temp_value.size();
    int ndim = 1;
    auto scale_dtype = ::ONNX_NAMESPACE::TensorProto::INT32;
    auto scale_shape = nvinfer1::Dims{ndim, {1, 1, 1, 1, 1, 1, 1, 1}};
    scale_shape.d[0]=_size;
    newWeights = ctx->createTempWeights(scale_dtype, scale_shape);
    //static_cast<float*>(newWeights.values)[0] = scale_value;
    std::copy(temp_value.begin(), temp_value.end(), static_cast<int32_t *>(newWeights.values));


    return true;
}
DEFINE_BUILTIN_OP_IMPORTER(Concat) {
  bool isScalar =false;

  //Begin:added by shendasai ,2019-8-23
  ShapedWeights newWeights;
  isScalar = concat_scalar(ctx, inputs, newWeights);

  if(isScalar)
  {
      return {{newWeights}};
  }
  //End:added by shendasai ,2019-8-23

  
  std::vector<nvinfer1::ITensor*> tensors;
  for( auto& input : inputs ) {
    ASSERT(input.is_tensor(), ErrorCode::kUNSUPPORTED_NODE);
#if NV_TENSORRT_MAJOR >= 4
    ASSERT(input.tensor().getType() != nvinfer1::DataType::kINT32,
           ErrorCode::kUNSUPPORTED_NODE);
#endif // NV_TENSORRT_MAJOR >= 4
    tensors.push_back(&input.tensor());
  }

  
  OnnxAttrs attrs(node);
  int nbDims = inputs.at(0).shape().nbDims;
  int axis = attrs.get<int>("axis");
  TRT_CHECK(convert_axis(axis, nbDims));
  auto* layer = ctx->network()->addConcatenation(tensors.data(), tensors.size());
  ASSERT(layer, ErrorCode::kUNSUPPORTED_NODE);
  layer->setAxis(axis);
  if(debug_on)
    return {{add_print_plugin(ctx, node,layer->getOutput(0))}};
  else
    RETURN_FIRST_OUTPUT(layer);
}
//sds,并没有调用tensorrt的constant layer，而是直接返回了一个TensorOrWeight
DEFINE_BUILTIN_OP_IMPORTER(Constant) {
  // TODO: This silently fails if the dtype is not supported
  OnnxAttrs attrs(node);
  return {{attrs.get<ShapedWeights>("value")}};
}

DEFINE_BUILTIN_OP_IMPORTER(ConstantOfShape) {
  // TODO: This silently fails if the dtype is not supported
  OnnxAttrs attrs(node);
  //return {{attrs.get<ShapedWeights>("value")}};
  nvinfer1::Weights w = nvinfer1::Weights(attrs.get<ShapedWeights>("value"));
  float value = *((float*)(w.values));


  ASSERT(inputs.size() == 1, ErrorCode::kINVALID_NODE);
  
    ASSERT(inputs.at(0).is_weights(), ErrorCode::kINVALID_NODE);

    auto weights = inputs.at(0).weights();
    nvinfer1::Dims dims = weights.shape;
    ASSERT(dims.nbDims ==1, ErrorCode::kINVALID_NODE);//必须是1-d的shape

    nvinfer1::Dims  new_shape;

    int len = dims.d[0]; 
    new_shape.nbDims = len;
    int32_t* pTemp_ = static_cast<int32_t*>(weights.values);
    std::copy(pTemp_, pTemp_ + len, new_shape.d);

    new_shape = set_dims_CHW(new_shape);

  //sds-temp,未使用输入，这个层可以直接返回weights，不用plugin layer.
  std::vector<nvinfer1::ITensor*> inputs_;

  RETURN_FIRST_OUTPUT(
      ctx->addPluginV2(
        new ConstantOfShapePlugin(value, new_shape),
        inputs_));
  //方法1:直接返回一个inputs 同dim的ShapedWeights，且用value初始化(需要构建一个ShapedWeights,需要自己申请gpu)
  //方法2:用TensorRT的constant layer初始化(需要构建一个weights,需要申请gpu)
  //方法3:使用plugin实现
  
}
#if 0
DEFINE_BUILTIN_OP_IMPORTER(Add) {
  ASSERT(inputs.size() == 2, ErrorCode::kINVALID_NODE);
  return combineTensorsMyElementwise(
      ctx, node, inputs, MyElementWiseType::Add, true);
}
DEFINE_BUILTIN_OP_IMPORTER(Mul) {
  ASSERT(inputs.size() == 2, ErrorCode::kINVALID_NODE);
  return combineTensorsMyElementwise(
      ctx, node, inputs, MyElementWiseType::Mul, true);
}
DEFINE_BUILTIN_OP_IMPORTER(Pow) {
  ASSERT(inputs.size() == 2, ErrorCode::kINVALID_NODE);
  return combineTensorsMyElementwise(
      ctx, node, inputs, MyElementWiseType::Pow, true);
}
#endif


DEFINE_BUILTIN_OP_IMPORTER(Equal) {
  ASSERT(inputs.size() == 2, ErrorCode::kINVALID_NODE);
  return combineTensorsMyElementwise(
      ctx, node, inputs, MyElementWiseType::Equal, true);
}
DEFINE_BUILTIN_OP_IMPORTER(Not)
{
  ASSERT(inputs.size() == 1, ErrorCode::kINVALID_NODE);
  return combineTensorsMyElementwise(
      ctx, node, inputs, MyElementWiseType::Not, true);

}

//sds-check

DEFINE_BUILTIN_OP_IMPORTER(Less) {
  ASSERT(inputs.size() == 2, ErrorCode::kINVALID_NODE);
  return combineTensorsMyElementwise(
      ctx, node, inputs, MyElementWiseType::Less, true);
}
//sds-check
DEFINE_BUILTIN_OP_IMPORTER(Where) {
  ASSERT(inputs.size() == 3, ErrorCode::kINVALID_NODE);
  return combineTensorsMyElementwise(
      ctx, node, inputs, MyElementWiseType::Where, true);
}
//sds-check
DEFINE_BUILTIN_OP_IMPORTER(NonZero) {
  ASSERT(inputs.size() == 1, ErrorCode::kINVALID_NODE);

  nvinfer1::Dims dims_ = inputs.at(0).shape();

  #if 0
  unsigned int _rows = _dims.nbDims;
  unsigned long long _numbers = 1;
  for(int i = 0; i <_rows ;i++)
  {
      _numbers *= _dims.d[i];
  }
  #endif
  
  
  RETURN_FIRST_OUTPUT(
      ctx->addPluginV2(
        new NonZeroPlugin(( unsigned int)0, (unsigned long long)0),
        {&inputs.at(0).tensor()}));
}


//sds-check
DEFINE_BUILTIN_OP_IMPORTER(Expand) {
  ASSERT(inputs.size() == 2, ErrorCode::kINVALID_NODE);

  nvinfer1::ITensor& data = convertToTensor(inputs.at(0), ctx);
  ASSERT(inputs.at(1).is_weights(), ErrorCode::kINVALID_NODE);

   //shape 必须是weights
   auto weights = inputs.at(1).weights();
   nvinfer1::Dims dims = weights.shape;
   ASSERT(dims.nbDims ==1, ErrorCode::kINVALID_NODE);//必须是1-d的shape
   
   nvinfer1::Dims  new_shape;
   
   int len = dims.d[0]; 
   new_shape.nbDims = len;
   int32_t* pTemp_ = static_cast<int32_t*>(weights.values);
   std::copy(pTemp_, pTemp_ + len, new_shape.d);
    
   new_shape = set_dims_CHW(new_shape);

 // Support broadcasting for tensor inputs by expanding dimensions.
 //保证rank 一样
  if (data.getDimensions().nbDims != new_shape.nbDims)
  {
    nvinfer1::Dims new_dims = expand_dims(data.getDimensions(), new_shape.nbDims);
    nvinfer1::ITensor* tensor_ptr = reshape_tensor(ctx, data, new_dims);

    RETURN_FIRST_OUTPUT(
        ctx->addPluginV2(
          new ExpandPlugin(new_shape),
          {tensor_ptr}));
  }
  else
  {
    RETURN_FIRST_OUTPUT(
        ctx->addPluginV2(
          new ExpandPlugin(new_shape),
          {&data}));

  }
       

}
DEFINE_BUILTIN_OP_IMPORTER(FC) 
{
    nvinfer1::ITensor& tensor0 = inputs.at(0).tensor();

    nvinfer1::IShuffleLayer* layer0 = ctx->network()->addShuffle(tensor0);
      nvinfer1::Dims shape = tensor0.getDimensions();
      {
        nvinfer1::Dims new_shape;
        new_shape.nbDims = 4;
        new_shape.d[0] = shape.d[0]*shape.d[1];
        new_shape.d[1] = shape.d[2];
        new_shape.d[2] = 1;
        new_shape.d[3] = 1;
        layer0->setReshapeDimensions(new_shape);
      }

    nvinfer1::ITensor& tensor1 = *layer0->getOutput(0);
    ShapedWeights weights = inputs.at(1).weights();
    //if (!transB)
    //{
     // auto transposedWeights = ctx->createTempWeights(weights.type, weights.shape);
     // ASSERT(transposeWeights(weights, {1, 0}, &transposedWeights),
    //         ErrorCode::kUNSUPPORTED_NODE);
     // weights = transposedWeights;
   // }
    ShapedWeights biases = inputs.at(2).weights();
    nvinfer1::ILayer* layer1 = ctx->network()->addFullyConnected(tensor1, biases.shape.d[0], weights, biases);
    

    nvinfer1::ITensor& tensor2 = *layer1->getOutput(0);

    nvinfer1::IShuffleLayer* layer2 = ctx->network()->addShuffle(tensor2);

    nvinfer1::Dims new_shape2;
    new_shape2.nbDims = 3;
    new_shape2.d[0] = shape.d[0];
    new_shape2.d[1] = shape.d[1];
    new_shape2.d[2] = biases.shape.d[0];
    layer2->setReshapeDimensions(new_shape2);


    if(debug_on)
        return {{add_print_plugin(ctx, node,layer2->getOutput(0))}};
    else
        return {{layer2->getOutput(0)}}; 

    
}


DEFINE_BUILTIN_OP_IMPORTER(SkipLayerNorm) 
{
    //128*10*512
    nvinfer1::ITensor& tensor1 = inputs.at(0).tensor();
    nvinfer1::ITensor& tensor2 = inputs.at(1).tensor();
    ShapedWeights weights = inputs.at(2).weights();
    ShapedWeights biases = inputs.at(3).weights();

    int hiddeSize = tensor1.getDimensions().d[2];
        
    nvinfer1::ILayer* layer_ptr = ctx->addPluginV2(
          new SkipLayerNormPlugin("skipln", 512, biases, weights),
          {&tensor1, &tensor2});


    if(debug_on)
     return {{add_print_plugin(ctx, node,layer_ptr->getOutput(0))}};
    else
     RETURN_FIRST_OUTPUT(layer_ptr);
}

DEFINE_BUILTIN_OP_IMPORTER(QKVToContext) 
{
    //128*10*1536
    nvinfer1::ITensor& tensor1 = inputs.at(0).tensor();//128*10*1536
    nvinfer1::ITensor& tensor2 = inputs.at(1).tensor();//mask, 10*128

    
    nvinfer1::Dims idims = tensor1.getDimensions();
    int hiddenSize = idims.d[2]; //1536
    const int S = idims.d[0];//128
    const int numHeads = 8;//
    const int headSize = hiddenSize / numHeads;

    assert(hiddenSize % numHeads == 0);

  
    const bool hasMask = true;
    
    nvinfer1::ILayer* layer_ptr = ctx->addPluginV2Ext(
          new QKVToContextPlugin("qkv2ctx", hiddenSize/3, numHeads, S, 16, hasMask),
          {&tensor1, &tensor2});
       

    if(debug_on)
     return {{add_print_plugin(ctx, node,layer_ptr->getOutput(0))}};
    else
     RETURN_FIRST_OUTPUT(layer_ptr);
  
}

DEFINE_BUILTIN_OP_IMPORTER(EmbedPosition) 
{
    //128*10*512
    nvinfer1::ITensor& tensor1 = inputs.at(0).tensor();
    //nvinfer1::ITensor& tensor2 = inputs.at(1).tensor();
    //ShapedWeights weights = inputs.at(1).weights();
    //ShapedWeights biases = inputs.at(3).weights();

    //int hiddeSize = weights.values[0];
    int padType = 0;     
    nvinfer1::ILayer* layer_ptr = ctx->addPluginV2(
          new EmbedPositionPlugin(padType),
          {&tensor1});

    nvinfer1::Dims shape = tensor1.getDimensions();
    ASSERT(shape.nbDims==2,  ErrorCode::kUNSUPPORTED_NODE);

    nvinfer1::ITensor& tensor2 = *layer_ptr->getOutput(0);
    nvinfer1::IShuffleLayer* layer2 = ctx->network()->addShuffle(tensor2);
    
    nvinfer1::Dims new_shape2;
    new_shape2.nbDims = 1;
    new_shape2.d[0] = shape.d[0]* shape.d[1];
    layer2->setReshapeDimensions(new_shape2);
       

    if(debug_on)
     return {{add_print_plugin(ctx, node,layer2->getOutput(0))}};
    else
     RETURN_FIRST_OUTPUT(layer2);
}

DEFINE_BUILTIN_OP_IMPORTER(Conv) {
    ASSERT(inputs.at(0).is_tensor(),  ErrorCode::kUNSUPPORTED_NODE);
    ASSERT(inputs.at(1).is_weights(), ErrorCode::kUNSUPPORTED_NODE);

    nvinfer1::ITensor* tensor_ptr = &inputs.at(0).tensor();
    auto kernel_weights = inputs.at(1).weights();
    nvinfer1::Dims dims = tensor_ptr->getDimensions();
    #if NV_TENSORRT_MAJOR >= 4
    bool need_to_expand_dims = (dims.nbDims == 2);
    if( need_to_expand_dims ) {
        // Expand spatial dims from 1D to 2D
        nvinfer1::DimsCHW new_shape(dims.d[0], dims.d[1], 1);
        tensor_ptr = reshape_tensor(ctx, *tensor_ptr, new_shape);
        ASSERT(tensor_ptr, ErrorCode::kUNSUPPORTED_NODE);
        dims = tensor_ptr->getDimensions();
    }
    if( kernel_weights.shape.nbDims == 3 ) {
        kernel_weights.shape.nbDims = 4;
        kernel_weights.shape.d[3] = 1;
    }
    #endif // NV_TENSORRT_MAJOR >= 4
    ASSERT(dims.nbDims == 3, ErrorCode::kUNSUPPORTED_NODE);
    ASSERT(kernel_weights.shape.nbDims == 4, ErrorCode::kUNSUPPORTED_NODE);
    nvinfer1::Weights bias_weights;
    if( inputs.size() == 3 ) {
        ASSERT(inputs.at(2).is_weights(), ErrorCode::kUNSUPPORTED_NODE);
        auto shaped_bias_weights = inputs.at(2).weights();
        ASSERT(shaped_bias_weights.shape.nbDims == 1, ErrorCode::kINVALID_NODE);
        ASSERT(shaped_bias_weights.shape.d[0] == kernel_weights.shape.d[0], ErrorCode::kINVALID_NODE);
        bias_weights = shaped_bias_weights;
    } else {
        bias_weights = ShapedWeights::empty(kernel_weights.type);
    }
    nvinfer1::DimsHW kernel_size;
    kernel_size.h() = kernel_weights.shape.d[2];
    kernel_size.w() = kernel_weights.shape.d[3];
    nvinfer1::DimsHW strides(1, 1);
    nvinfer1::DimsHW beg_padding(0, 0), end_padding(0, 0);
    nvinfer1::DimsHW dilations(1, 1);
    nvinfer1::PaddingMode paddingMode;
    get_kernel_params(node, get_DimsHW_from_CHW(dims), &kernel_size,
        &strides, &beg_padding, &end_padding, paddingMode, &dilations);
    ASSERT(kernel_size.h() == kernel_weights.shape.d[2], ErrorCode::kINVALID_NODE);
    ASSERT(kernel_size.w() == kernel_weights.shape.d[3], ErrorCode::kINVALID_NODE);
    int nchan = dims.d[0];
    int noutput = kernel_weights.shape.d[0]; // Note: Weights order is KCRS
    nvinfer1::IConvolutionLayer* layer = ctx->network()->addConvolution(
        *tensor_ptr, noutput, kernel_size, kernel_weights, bias_weights);

    ASSERT(layer, ErrorCode::kUNSUPPORTED_NODE);
    layer->setStride(strides);
    layer->setPaddingMode(paddingMode);
    layer->setPrePadding(beg_padding);
    layer->setPostPadding(end_padding);
    layer->setDilation(dilations);
    OnnxAttrs attrs(node);
    int ngroup = attrs.get("group", 1);
    ASSERT(kernel_weights.shape.d[1] * ngroup == nchan, ErrorCode::kINVALID_NODE);
    layer->setNbGroups(ngroup);
    tensor_ptr = layer->getOutput(0);
    dims = tensor_ptr->getDimensions();
    #if NV_TENSORRT_MAJOR >= 4
    if( need_to_expand_dims ) {
        // Un-expand spatial dims back to 1D
        nvinfer1::Dims new_shape{2, {dims.d[0], dims.d[1]}};
        tensor_ptr = reshape_tensor(ctx, *tensor_ptr, new_shape);
        ASSERT(tensor_ptr, ErrorCode::kUNSUPPORTED_NODE);
    }
    #endif // NV_TENSORRT_MAJOR >= 4
    return {{tensor_ptr}};
}

DEFINE_BUILTIN_OP_IMPORTER(ConvTranspose) {
  ASSERT(inputs.at(0).is_tensor(),  ErrorCode::kUNSUPPORTED_NODE);
  ASSERT(inputs.at(1).is_weights(), ErrorCode::kUNSUPPORTED_NODE);
  nvinfer1::ITensor* tensor_ptr = &inputs.at(0).tensor();
  auto kernel_weights = inputs.at(1).weights();
  nvinfer1::Dims dims = tensor_ptr->getDimensions();
#if NV_TENSORRT_MAJOR >= 4
  bool need_to_expand_dims = (dims.nbDims == 2);
  if( need_to_expand_dims ) {
    // Expand spatial dims from 1D to 2D
    nvinfer1::DimsCHW new_shape(dims.d[0], dims.d[1], 1);
    tensor_ptr = reshape_tensor(ctx, *tensor_ptr, new_shape);
    ASSERT(tensor_ptr, ErrorCode::kUNSUPPORTED_NODE);
    dims = tensor_ptr->getDimensions();
  }
  if( kernel_weights.shape.nbDims == 3 ) {
    kernel_weights.shape.nbDims = 4;
    kernel_weights.shape.d[3] = 1;
  }
#endif // NV_TENSORRT_MAJOR >= 4
  ASSERT(dims.nbDims == 3, ErrorCode::kUNSUPPORTED_NODE);
  ASSERT(kernel_weights.shape.nbDims == 4, ErrorCode::kUNSUPPORTED_NODE);
  nvinfer1::Weights bias_weights;
  if( inputs.size() == 3 ) {
    ASSERT(inputs.at(2).is_weights(), ErrorCode::kUNSUPPORTED_NODE);
    auto shaped_bias_weights = inputs.at(2).weights();
    ASSERT(shaped_bias_weights.shape.nbDims == 1, ErrorCode::kINVALID_NODE);
    ASSERT(shaped_bias_weights.shape.d[0] == kernel_weights.shape.d[1],
           ErrorCode::kINVALID_NODE);
    bias_weights = shaped_bias_weights;
  } else {
    bias_weights = ShapedWeights::empty(kernel_weights.type);
  }
  OnnxAttrs attrs(node);
  nvinfer1::DimsHW input_shape = get_DimsHW_from_CHW(dims);
  nvinfer1::DimsHW output_shape;
  if( attrs.count("output_shape") ) {
    output_shape = attrs.get<nvinfer1::DimsHW>("output_shape");
  } else {
    ASSERT(attrs.get("auto_pad", std::string("VALID")) == "VALID",
           ErrorCode::kINVALID_NODE);
  }
  nvinfer1::DimsHW kernel_size;
  kernel_size.h() = kernel_weights.shape.d[2];
  kernel_size.w() = kernel_weights.shape.d[3];
  nvinfer1::DimsHW strides(1, 1);
  nvinfer1::DimsHW beg_padding(0, 0), end_padding(0, 0);
  nvinfer1::DimsHW dilations(1, 1);
  nvinfer1::PaddingMode paddingMode;
  // Note: output_shape/input_shape are swapped here so that the padding
  //       calculations operate as if it were a regular forward convolution.
  get_kernel_params(node, output_shape,
                    &kernel_size, &strides,
                    &beg_padding, &end_padding, paddingMode, &dilations, &input_shape);
  ASSERT(kernel_size.h() == kernel_weights.shape.d[2], ErrorCode::kINVALID_NODE);
  ASSERT(kernel_size.w() == kernel_weights.shape.d[3], ErrorCode::kINVALID_NODE);
  ASSERT(dims.nbDims == 3, ErrorCode::kUNSUPPORTED_NODE);
  int nchan = dims.d[0];
  int ngroup = attrs.get("group", 1);
  int noutput = kernel_weights.shape.d[1] * ngroup; // Note: Weights order is CKRS
  nvinfer1::IDeconvolutionLayer* deconv_layer = ctx->network()->addDeconvolution(
    *tensor_ptr, noutput, kernel_size, kernel_weights, bias_weights);
  nvinfer1::ILayer* layer = deconv_layer;
  ASSERT(layer, ErrorCode::kUNSUPPORTED_NODE);
  deconv_layer->setStride(strides);
  if( !attrs.count("output_shape") && attrs.count("output_padding") ) {
    auto output_padding = attrs.get<nvinfer1::DimsHW>("output_padding");
    end_padding.h() -= output_padding.h();
    end_padding.w() -= output_padding.w();
  }
  deconv_layer->setPaddingMode(paddingMode);
  deconv_layer->setPrePadding(beg_padding);
  deconv_layer->setPostPadding(end_padding);
  ASSERT(dilations.h() == 1 && dilations.w() == 1, ErrorCode::kUNSUPPORTED_NODE);
  ASSERT(kernel_weights.shape.d[0] == nchan, ErrorCode::kINVALID_NODE);
  deconv_layer->setNbGroups(ngroup);
  tensor_ptr = layer->getOutput(0);
  dims = tensor_ptr->getDimensions();
#if NV_TENSORRT_MAJOR >= 4
  if( need_to_expand_dims ) {
    // Un-expand spatial dims back to 1D
    nvinfer1::Dims new_shape{2, {dims.d[0], dims.d[1]}};
    tensor_ptr = reshape_tensor(ctx, *tensor_ptr, new_shape);
    ASSERT(tensor_ptr, ErrorCode::kUNSUPPORTED_NODE);
  }
  #endif // NV_TENSORRT_MAJOR >= 4
  return {{tensor_ptr}};
}

DEFINE_BUILTIN_OP_IMPORTER(Cos)
{
    return unaryHelper(ctx, node, inputs, nvinfer1::UnaryOperation::kCOS);
}

DEFINE_BUILTIN_OP_IMPORTER(Cosh)
{
    return unaryHelper(ctx, node, inputs, nvinfer1::UnaryOperation::kCOSH);
}


#if NV_TENSORRT_MAJOR >= 4
DEFINE_BUILTIN_OP_IMPORTER(DepthToSpace) {
  ASSERT(inputs.at(0).is_tensor(), ErrorCode::kUNSUPPORTED_NODE);
  nvinfer1::ITensor* tensor_ptr = &inputs.at(0).tensor();
  nvinfer1::IShuffleLayer* layer = ctx->network()->addShuffle(*tensor_ptr);
  ASSERT(layer, ErrorCode::kUNSUPPORTED_NODE);
  OnnxAttrs attrs(node);
  int block_size = attrs.get<int>("blocksize");
  nvinfer1::Dims dims = tensor_ptr->getDimensions();
  int ndim_spatial = dims.nbDims - 1;
  nvinfer1::Dims new_shape1;
  new_shape1.nbDims = dims.nbDims + ndim_spatial;
  new_shape1.d[ndim_spatial] = dims.d[0];
  for( int i=0; i<ndim_spatial; ++i ) {
    ASSERT(new_shape1.d[ndim_spatial] % block_size == 0, ErrorCode::kINVALID_NODE);
    new_shape1.d[ndim_spatial] /= block_size;
    new_shape1.d[i] = block_size;
    new_shape1.d[ndim_spatial + 1 + i] = dims.d[1 + i];
  }
  layer->setReshapeDimensions(new_shape1);
  nvinfer1::Permutation perm;
  perm.order[0] = ndim_spatial;
  for( int i=0; i<ndim_spatial; ++i ) {
    perm.order[1 + 2*i + 0] = ndim_spatial + 1 + i;
    perm.order[1 + 2*i + 1] = i;
  }
  layer->setSecondTranspose(perm);
  tensor_ptr = layer->getOutput(0);
  dims = tensor_ptr->getDimensions();
  nvinfer1::Dims new_shape2;
  new_shape2.nbDims = dims.nbDims - ndim_spatial;
  new_shape2.d[0] = dims.d[0];
  for( int i=0; i<ndim_spatial; ++i ) {
    new_shape2.d[1 + i] = dims.d[1 + 2*i + 0] * dims.d[1 + 2*i + 1];
  }
  tensor_ptr = reshape_tensor(ctx, *tensor_ptr, new_shape2);
  ASSERT(tensor_ptr, ErrorCode::kUNSUPPORTED_NODE);
  return {{tensor_ptr}};
}
#endif // NV_TENSORRT_MAJOR >= 4

DECLARE_BUILTIN_OP_IMPORTER(Mul);
DEFINE_BUILTIN_OP_IMPORTER(Div) {
  ASSERT(inputs.size() == 2, ErrorCode::kINVALID_NODE);
  return combineTensorsElementwise(
      ctx, node, inputs, nvinfer1::ElementWiseOperation::kDIV, true);
}

DEFINE_BUILTIN_OP_IMPORTER(Dropout) {
  // TensorRT 5.1 only supports up to opset 9.
  ASSERT(ctx->getOpsetVersion() < 10, ErrorCode::kUNSUPPORTED_NODE);
  int noutputs = node.output().size();
  if (noutputs == 1)
  {
    RETURN_IDENTITY(inputs.at(0));
  }
  else 
  {
    // Return both Dropout outputs: (output + mask)
    std::vector<TensorOrWeights> outputs;
    outputs.push_back(identity(ctx,inputs.at(0)));
    outputs.push_back(identity(ctx,inputs.at(0)));
    return outputs;
  }
}

DEFINE_BUILTIN_OP_IMPORTER(Elu) {
  OnnxAttrs attrs(node);
  float alpha = attrs.get<float>("alpha", 1.f);
  return activationHelper(ctx, node, inputs, nvinfer1::ActivationType::kELU, &alpha);
}

DEFINE_BUILTIN_OP_IMPORTER(Exp) {
  return apply_unary_function(ctx, inputs.at(0), nvinfer1::UnaryOperation::kEXP);
}

DEFINE_BUILTIN_OP_IMPORTER(Flatten) {
  OnnxAttrs attrs(node);
  int axis = attrs.get("axis", 1);
  // Note: Flattening to shape=[batch, n] is currently the only sensible
  //       operation, because we can't remove or merge into the batch dim.
  ASSERT(axis == 1, ErrorCode::kUNSUPPORTED_NODE);
  nvinfer1::Dims dims = inputs.at(0).shape();
  ASSERT(inputs.at(0).is_tensor(), ErrorCode::kUNSUPPORTED_NODE);
  nvinfer1::ITensor* tensor_ptr;
#if NV_TENSORRT_MAJOR < 4
  // Note: TRT3 requires that the shape remain 3D (CHW)
  tensor_ptr = flatten_tensor(ctx, inputs.at(0).tensor());
#else // NV_TENSORRT_MAJOR >= 4
  nvinfer1::Dims new_shape{1, {(int)get_shape_size(dims)}};
  tensor_ptr = reshape_tensor(ctx, inputs.at(0).tensor(), new_shape);
#endif // NV_TENSORRT_MAJOR >= 4
  ASSERT(tensor_ptr, ErrorCode::kUNSUPPORTED_NODE);
  return {{tensor_ptr}};
}


bool  gather_scalar(IImporterContext* ctx, std::vector<TensorOrWeights>& inputs, ShapedWeights& newWeights)
{

    for( auto& input : inputs )
    {
        //必须是weights
        if(!input.is_weights()) return false;
    }
    
    auto weights0 = inputs.at(0).weights();
    nvinfer1::Dims dims0 = weights0.shape;
    if(dims0.nbDims !=1) return false;

    auto weights1 = inputs.at(1).weights();
    nvinfer1::Dims dims1 = weights1.shape;
    if(dims1.nbDims !=1 ||  dims1.d[0] !=1) return false;
    int32_t _index = 0;
    switch(weights1.type)
    {
        case((int32_t)::ONNX_NAMESPACE::TensorProto::FLOAT):
        {
             _index = (int32_t)(static_cast<float*>(weights1.values)[0]);
            break;
        }
        case((int32_t)::ONNX_NAMESPACE::TensorProto::INT32):
        {
             _index = (int32_t)(static_cast<int32_t *>(weights1.values)[0]);
            break;
        }
        //inputs[2]有可能是从文件读的
        case((int32_t)::ONNX_NAMESPACE::TensorProto::INT64):
        {
             _index = (int32_t)(static_cast<int64_t *>(weights1.values)[0]);
            break;
        }
        default:
            break;
    }

    if(_index >= dims0.d[0])
    {
        return false;   
    }
    int32_t _value = 0;

    switch(weights0.type)
    {
        case((int32_t)::ONNX_NAMESPACE::TensorProto::FLOAT):
        {
             _value = (int32_t)(static_cast<float*>(weights0.values)[_index]);
            break;
        }
        case((int32_t)::ONNX_NAMESPACE::TensorProto::INT32):
        {
             _value = (int32_t)(static_cast<int32_t *>(weights0.values)[_index]);
            break;
        }
        //input[1]一定是传递过来的，不是从文件读的
        default:
            break;
    }


    int ndim = 1;
    auto scale_dtype = ::ONNX_NAMESPACE::TensorProto::INT32;
    auto scale_shape = nvinfer1::Dims{ndim, {1, 1, 1, 1, 1, 1, 1, 1}};
    scale_shape.d[0]=1;
    newWeights = ctx->createTempWeights(scale_dtype, scale_shape);
    static_cast<int32_t*>(newWeights.values)[0] = _value;
    //std::copy(temp_value.begin(), temp_value.end(), static_cast<int64_t *>(newWeights.values));


    return true;
}

  

//sds-check
#if NV_TENSORRT_MAJOR >= 4
DEFINE_BUILTIN_OP_IMPORTER(Gather) {

    //Begin: added by shendasai for output is scalar case, 2019年8月23日14:03:19.
    bool isOutScalar =false;
    ShapedWeights newWeights;
    isOutScalar = gather_scalar(ctx, inputs, newWeights);
    if(isOutScalar)
    {
        return {{newWeights}};
    }
    //End: added by shendasai for output is scalar case.

    //如果输入是
    nvinfer1::ITensor& data = convertToTensor(inputs.at(0), ctx);
    nvinfer1::ITensor& indices = convertToTensor(inputs.at(1), ctx);
    //如果indices是float类型，转化为int32类型
    #if 1
    nvinfer1::ILayer* layer_ptr = ctx->addPluginV2Ext(
        new MyCastPlugin(nvinfer1::DataType::kFLOAT, nvinfer1::DataType::kINT32),
        {&indices});

    nvinfer1::ITensor* indices_new = layer_ptr->getOutput(0);
    #endif
    OnnxAttrs attrs(node);
    int axis = attrs.get<int>("axis", 0);
    int nbDims = inputs.at(0).shape().nbDims;
    //sds-temp 
    //(1)embeding (2)shape (3)其它  的输出不要含有batch
    //axis +=1;
    TRT_CHECK(convert_axis(axis, nbDims));
    //sds
    RETURN_FIRST_OUTPUT(ctx->network()->addGather(data, *indices_new, axis));
}
#endif // NV_TENSORRT_MAJOR >= 4

DEFINE_BUILTIN_OP_IMPORTER(Floor) {
    return unaryHelper(ctx, node, inputs, nvinfer1::UnaryOperation::kFLOOR);
}

DEFINE_BUILTIN_OP_IMPORTER(Gemm) {
    OnnxAttrs attrs(node);
    float alpha = attrs.get("alpha", 1.f);
    float beta = attrs.get("beta", 1.f);
    bool transA = attrs.get("transA", false);
    bool transB = attrs.get("transB", false);
    nvinfer1::ITensor& inputA = convertToTensor(inputs.at(0), ctx);
    nvinfer1::ITensor* inputB{nullptr};
    nvinfer1::ITensor& inputC = convertToTensor(inputs.at(2), ctx);

    // Use FC if it is likely to be faster - which is usually when no Shuffles are required.
    bool canUseFC = inputs.at(0).is_tensor() && inputs.at(1).is_weights()
        && inputs.at(2).is_weights() && alpha == 1.f && beta == 1.f && inputs.at(0).tensor().getDimensions().nbDims == 3
        && inputs.at(1).weights().shape.nbDims == 2 && inputs.at(2).weights().shape.nbDims == 1;
    if (canUseFC)
    {
        nvinfer1::ITensor& tensor = inputs.at(0).tensor();
        ShapedWeights weights = inputs.at(1).weights();
        if (!transB)
        {
          auto transposedWeights = ctx->createTempWeights(weights.type, weights.shape);
          ASSERT(transposeWeights(weights, {1, 0}, &transposedWeights),
                 ErrorCode::kUNSUPPORTED_NODE);
          weights = transposedWeights;
        }
        ShapedWeights biases = inputs.at(2).weights();
        RETURN_FIRST_OUTPUT(ctx->network()->addFullyConnected(tensor, biases.shape.d[0], weights, biases));
    }

    // If input B is a constant, we transpose at parse time if necessary,
    // because In some cases, A * Bt is much slower than A * B.
    if (inputs.at(1).is_weights())
    {
        ShapedWeights weights = inputs.at(1).weights();
        if (transB)
        {
          auto transposedWeights = ctx->createTempWeights(weights.type, weights.shape);
          ASSERT(transposeWeights(weights, {1, 0}, &transposedWeights),
                 ErrorCode::kUNSUPPORTED_NODE);
          weights = transposedWeights;
          // Since we've already transposed now, we can set transpose to false.
          transB = false;
        }
        nvinfer1::IConstantLayer* weightsLayer = ctx->network()->addConstant(weights.shape, static_cast<nvinfer1::Weights>(weights));
        inputB = weightsLayer->getOutput(0);
    }
    else
    {
        inputB = &inputs.at(1).tensor();
    }

    if (ctx->getOpsetVersion() < 7)
    {
        ASSERT(attrs.get("broadcast", false), ErrorCode::kUNSUPPORTED_NODE);
    }

    nvinfer1::ITensor* inputASqueezed = &inputA;
    nvinfer1::Dims newDims = squeeze_trailing_dims(inputA.getDimensions());
    // When A has more than 2 dimensions, it needs to be flattened.
    if (newDims.nbDims > 2)
    {
        newDims = nvinfer1::Dims{1, {-1}};
    }
    // Due to other TRT layers, inputA may sometimes have trailing 1s that need to be removed.
    if (newDims.nbDims < inputA.getDimensions().nbDims)
    {
        nvinfer1::IShuffleLayer* squeeze = ctx->network()->addShuffle(inputA);
        squeeze->setReshapeDimensions(newDims);
        inputASqueezed = squeeze->getOutput(0);
    }

    constexpr auto getMatrixOp = [] (const nvinfer1::ITensor& input, bool transpose)
    {
        return (input.getDimensions().nbDims == 1) ?
        nvinfer1::MatrixOperation::kVECTOR :
        (transpose) ?
        nvinfer1::MatrixOperation::kTRANSPOSE :
        nvinfer1::MatrixOperation::kNONE;
    };

    nvinfer1::MatrixOperation opA = getMatrixOp(*inputASqueezed, transA);
    nvinfer1::MatrixOperation opB = getMatrixOp(*inputB, transB);


    if (opA == nvinfer1::MatrixOperation::kVECTOR && opB == nvinfer1::MatrixOperation::kVECTOR)
    {
      ASSERT(inputASqueezed->getDimensions() == inputB->getDimensions(), ErrorCode::kUNSUPPORTED_NODE);
    }

    nvinfer1::IMatrixMultiplyLayer* matmul = ctx->network()->addMatrixMultiply(*inputASqueezed, opA, *inputB, opB);
    nvinfer1::ITensor* matmulTensor = matmul->getOutput(0);

    // Scale A*B if needed.
    if (alpha != 1.f)
    {
        nvinfer1::IConstantLayer* alphaConstant = addConstantScalar(ctx, alpha, ::ONNX_NAMESPACE::TensorProto_DataType_FLOAT);
        nvinfer1::IElementWiseLayer* scaledMatmul = ctx->network()->addElementWise(*alphaConstant->getOutput(0), *matmulTensor, nvinfer1::ElementWiseOperation::kPROD);
        matmulTensor = scaledMatmul->getOutput(0);
    }
    // Scale C if needed.
    nvinfer1::ITensor* biasTensor = &inputC;
    if (beta != 1.f)
    {
        nvinfer1::IConstantLayer* betaConstant = addConstantScalar(ctx, beta, ::ONNX_NAMESPACE::TensorProto_DataType_FLOAT);
        nvinfer1::IElementWiseLayer* scaledBias = ctx->network()->addElementWise(*betaConstant->getOutput(0), *biasTensor, nvinfer1::ElementWiseOperation::kPROD);
        biasTensor = scaledBias->getOutput(0);
    }

    RETURN_FIRST_OUTPUT(ctx->network()->addElementWise(*matmulTensor, *biasTensor, nvinfer1::ElementWiseOperation::kSUM));
}

DEFINE_BUILTIN_OP_IMPORTER(GlobalAveragePool) {
  ASSERT(inputs.at(0).is_tensor(), ErrorCode::kUNSUPPORTED_NODE);
  nvinfer1::ITensor& tensor = inputs.at(0).tensor();
  nvinfer1::Dims dims = tensor.getDimensions();
  ASSERT(dims.nbDims == 3, ErrorCode::kUNSUPPORTED_NODE);
  nvinfer1::DimsHW kernel_size(dims.d[1], dims.d[2]);
  RETURN_FIRST_OUTPUT(
    ctx->network()->addPooling(
      tensor, nvinfer1::PoolingType::kAVERAGE, kernel_size));
}

// TODO: GlobalLpPool: pow(reduce_mean(pow(abs(x), p)), 1./p)

DEFINE_BUILTIN_OP_IMPORTER(GlobalMaxPool) {
  ASSERT(inputs.at(0).is_tensor(), ErrorCode::kUNSUPPORTED_NODE);
  nvinfer1::ITensor& tensor = inputs.at(0).tensor();
  nvinfer1::Dims dims = tensor.getDimensions();
  ASSERT(dims.nbDims == 3, ErrorCode::kUNSUPPORTED_NODE);
  nvinfer1::DimsHW kernel_size(dims.d[1], dims.d[2]);
  RETURN_FIRST_OUTPUT(
    ctx->network()->addPooling(
      tensor, nvinfer1::PoolingType::kMAX, kernel_size));
}

DEFINE_BUILTIN_OP_IMPORTER(HardSigmoid) {
  ASSERT(inputs.at(0).is_tensor(), ErrorCode::kUNSUPPORTED_NODE);
  OnnxAttrs attrs(node);
  float alpha = attrs.get<float>("alpha", 0.2f);
  float beta  = attrs.get<float>("beta",  0.5f);
  return activationHelper(ctx, node, inputs, nvinfer1::ActivationType::kHARD_SIGMOID, &alpha, &beta);
}

DEFINE_BUILTIN_OP_IMPORTER(Identity) {
  RETURN_IDENTITY(inputs.at(0));
}

DEFINE_BUILTIN_OP_IMPORTER(ImageScaler) {
    ASSERT(inputs.at(0).is_tensor(), ErrorCode::kUNSUPPORTED_NODE);
    OnnxAttrs attrs{node};
    // Shift the input by a per-channel bias value.
    std::vector<float> biases = attrs.get<std::vector<float>>("bias");
    nvinfer1::Dims dims{1, static_cast<int>(biases.size())};
    ShapedWeights shiftWeights = ctx->createTempWeights(::ONNX_NAMESPACE::TensorProto_DataType_FLOAT, dims);
    std::copy(biases.begin(), biases.end(), static_cast<float*>(shiftWeights.values));
    // Scale is applied to every element of the input, but we need to duplicate it over every channel.
    float scale = attrs.get<float>("scale", 1.0f);
    ShapedWeights scaleWeights = ctx->createTempWeights(::ONNX_NAMESPACE::TensorProto_DataType_FLOAT, dims);
    std::fill(static_cast<float*>(scaleWeights.values), static_cast<float*>(scaleWeights.values) + scaleWeights.count(), scale);
    // Finally add the scale layer.
    RETURN_FIRST_OUTPUT(
        ctx->network()->addScale(inputs.at(0).tensor(), nvinfer1::ScaleMode::kCHANNEL,
            shiftWeights, scaleWeights, nvinfer1::Weights{})
    );
}

DEFINE_BUILTIN_OP_IMPORTER(InstanceNormalization) {
  ASSERT(inputs.at(0).is_tensor(),  ErrorCode::kUNSUPPORTED_NODE);
  ASSERT(inputs.at(1).is_weights(), ErrorCode::kUNSUPPORTED_NODE);
  ASSERT(inputs.at(2).is_weights(), ErrorCode::kUNSUPPORTED_NODE);
  auto scale_weights = inputs.at(1).weights();
  auto bias_weights  = inputs.at(2).weights();
  OnnxAttrs attrs(node);
  float epsilon = attrs.get("epsilon", 1e-5f);
  // HACK TODO: Values < ~1e-4 were found to cause corrupt output in a RTST model. Need to suss this out.
  epsilon = std::max(epsilon, 1e-4f);
  RETURN_FIRST_OUTPUT(
      ctx->addPluginV2(
        new InstanceNormalizationPlugin(epsilon, scale_weights, bias_weights),
        {&inputs.at(0).tensor()}));
}

DEFINE_BUILTIN_OP_IMPORTER(LeakyRelu) {
    OnnxAttrs attrs(node);
    float alpha = attrs.get<float>("alpha", 0.01f);
    return activationHelper(ctx, node, inputs, nvinfer1::ActivationType::kLEAKY_RELU, &alpha);
}

DEFINE_BUILTIN_OP_IMPORTER(Log) {
  return apply_unary_function(ctx, inputs.at(0), nvinfer1::UnaryOperation::kLOG);
}

DECLARE_BUILTIN_OP_IMPORTER(Softmax);
DEFINE_BUILTIN_OP_IMPORTER(LogSoftmax) {
  auto result = importSoftmax(ctx, node, inputs);
  if( result.is_error() ) {
    return result;
  } else {
    auto& input = result.value().at(0);
    return apply_unary_function(ctx, input, nvinfer1::UnaryOperation::kLOG);
  }
}

DEFINE_BUILTIN_OP_IMPORTER(LRN) {
  ASSERT(inputs.at(0).is_tensor(), ErrorCode::kUNSUPPORTED_NODE);
  nvinfer1::ITensor& tensor = inputs.at(0).tensor();
  OnnxAttrs attrs(node);
  int   size  = attrs.get<int>("size");
  float alpha = attrs.get<float>("alpha", 0.0001f);
  float beta  = attrs.get<float>("beta", 0.75f);
  float bias  = attrs.get<float>("bias", 1.0f);
  RETURN_FIRST_OUTPUT(
    ctx->network()->addLRN(tensor, size, alpha, beta, bias));
}

DEFINE_BUILTIN_OP_IMPORTER(MatMul) {
    nvinfer1::ITensor& inputA = convertToTensor(inputs.at(0), ctx);
    nvinfer1::ITensor& inputB = convertToTensor(inputs.at(1), ctx);

    constexpr auto getMatrixOp = [] (const nvinfer1::ITensor& input)
    {
        return (input.getDimensions().nbDims == 1) ?
            nvinfer1::MatrixOperation::kVECTOR :
            nvinfer1::MatrixOperation::kNONE;
    };

    nvinfer1::MatrixOperation opA = getMatrixOp(inputA);
    nvinfer1::MatrixOperation opB = getMatrixOp(inputB);

    //sds-temp
    //如果inputB's rank比inputA小。则unsqueeze。
    int a_n = inputA.getDimensions().nbDims;
    int b_n = inputB.getDimensions().nbDims;
    
    if(a_n > b_n)
    {
        int sub_n = a_n - b_n;
        nvinfer1::Dims new_shape;
        new_shape.nbDims = a_n;
        for(int i = 0; i < new_shape.nbDims ; i++)
        {
            if(i < sub_n)
                new_shape.d[i] = 1;
            else
                new_shape.d[i] = inputB.getDimensions().d[i-sub_n];
               
        }

        nvinfer1::IShuffleLayer* layer = ctx->network()->addShuffle(inputB);
        ASSERT(layer, ErrorCode::kUNSUPPORTED_NODE);
        layer->setReshapeDimensions(new_shape);
        //ASSERT(get_shape_size(layer->getOutput(0)->getDimensions()) == get_shape_size(old_shape),
          //ErrorCode::kUNSUPPORTED_NODE);
        nvinfer1::ITensor* pNewB = layer->getOutput(0);
        nvinfer1::ILayer* layer_ptr = ctx->network()->addMatrixMultiply(inputA, opA, *pNewB, opB);
        ASSERT(layer_ptr, ErrorCode::kUNSUPPORTED_NODE); 

        if(debug_on)
        return {{add_print_plugin(ctx, node,layer_ptr->getOutput(0))}};
    else
        return {{layer_ptr->getOutput(0)}}; 
    
        //return {{layer_ptr->getOutput(0)}}; 
    }
    
  
    nvinfer1::ILayer* layer_ptr = ctx->network()->addMatrixMultiply(inputA, opA, inputB, opB);
    ASSERT(layer_ptr, ErrorCode::kUNSUPPORTED_NODE); 
    if(debug_on)
        return {{add_print_plugin(ctx, node,layer_ptr->getOutput(0))}};
    else
        return {{layer_ptr->getOutput(0)}}; 
    
}

DEFINE_BUILTIN_OP_IMPORTER(Max) {
  return combineTensorsElementwise(
    ctx, node, inputs, nvinfer1::ElementWiseOperation::kMAX);
}

DEFINE_BUILTIN_OP_IMPORTER(MaxPool) {
  // TensorRT 5.1 only supports up to opset 9.
  ASSERT(ctx->getOpsetVersion() < 10, ErrorCode::kUNSUPPORTED_NODE);
  ASSERT(inputs.at(0).is_tensor(), ErrorCode::kUNSUPPORTED_NODE);
  nvinfer1::ITensor* tensor_ptr = &inputs.at(0).tensor();
  nvinfer1::Dims dims = tensor_ptr->getDimensions();
  ASSERT(dims.nbDims >= 2, ErrorCode::kINVALID_NODE);
#if NV_TENSORRT_MAJOR >= 4
  bool need_to_expand_dims = (dims.nbDims == 2);
  if( need_to_expand_dims ) {
    // Expand spatial dims from 1D to 2D
    nvinfer1::DimsCHW new_shape(dims.d[0], dims.d[1], 1);
    tensor_ptr = reshape_tensor(ctx, *tensor_ptr, new_shape);
    ASSERT(tensor_ptr, ErrorCode::kUNSUPPORTED_NODE);
    dims = tensor_ptr->getDimensions();
  }
#endif // NV_TENSORRT_MAJOR >= 4
  ASSERT(dims.nbDims == 3, ErrorCode::kUNSUPPORTED_NODE);
  nvinfer1::DimsHW kernel_size(1, 1), strides(1, 1), beg_padding(0, 0), end_padding(0, 0);
  nvinfer1::PaddingMode paddingMode;
  get_kernel_params(node, get_DimsHW_from_CHW(dims),
                    &kernel_size, &strides, &beg_padding, &end_padding, paddingMode);
  nvinfer1::IPoolingLayer* layer = ctx->network()->addPooling(
    *tensor_ptr, nvinfer1::PoolingType::kMAX, kernel_size);
  ASSERT(layer, ErrorCode::kUNSUPPORTED_NODE);
  layer->setStride(strides);
  layer->setPaddingMode(paddingMode);
  layer->setPrePadding(beg_padding);
  layer->setPostPadding(end_padding);
  tensor_ptr = layer->getOutput(0);
  dims = tensor_ptr->getDimensions();
#if NV_TENSORRT_MAJOR >= 4
  if( need_to_expand_dims ) {
    // Un-expand spatial dims back to 1D
    nvinfer1::Dims new_shape{2, {dims.d[0], dims.d[1]}};
    tensor_ptr = reshape_tensor(ctx, *tensor_ptr, new_shape);
    ASSERT(tensor_ptr, ErrorCode::kUNSUPPORTED_NODE);
  }
#endif // NV_TENSORRT_MAJOR >= 4
  return {{tensor_ptr}};
}

#if NV_TENSORRT_MAJOR >= 4
DEFINE_BUILTIN_OP_IMPORTER(Mean) {
  auto sum_result = combineTensorsElementwise(
    ctx, node, inputs, nvinfer1::ElementWiseOperation::kSUM);
  if( sum_result.is_error() ) {
    return sum_result;
  }
  auto& sum_input = sum_result.value().at(0);
  ASSERT(sum_input.is_tensor(), ErrorCode::kUNSUPPORTED_NODE);
  nvinfer1::ITensor& sum_tensor = sum_input.tensor();

  int ndim = sum_tensor.getDimensions().nbDims;
  float scale_value = 1.f / inputs.size();
  auto scale_dtype = ::ONNX_NAMESPACE::TensorProto::FLOAT;
  auto scale_shape = nvinfer1::Dims{ndim, {1, 1, 1, 1, 1, 1, 1, 1}};
  auto scale_weights = ctx->createTempWeights(scale_dtype, scale_shape);
  static_cast<float*>(scale_weights.values)[0] = scale_value;
  auto* constant_layer = ctx->network()->addConstant(
      scale_weights.shape, scale_weights);
  ASSERT(constant_layer, ErrorCode::kUNSUPPORTED_NODE);
  nvinfer1::ITensor& scale_constant = *constant_layer->getOutput(0);
  RETURN_FIRST_OUTPUT(
      ctx->network()->addElementWise(
          sum_tensor, scale_constant, nvinfer1::ElementWiseOperation::kPROD));
}
#endif // NV_TENSORRT_MAJOR >= 4

DEFINE_BUILTIN_OP_IMPORTER(Min) {
  return combineTensorsElementwise(
    ctx, node, inputs, nvinfer1::ElementWiseOperation::kMIN);
}


bool  mul_scalar(IImporterContext* ctx, std::vector<TensorOrWeights>& inputs, ShapedWeights& newWeights)
{

    for( auto& input : inputs )
    {
        //必须是weights
        if(!input.is_weights()) return false;
    }

    

    auto weights0 = inputs.at(0).weights();
    nvinfer1::Dims dims0 = weights0.shape;
    if(dims0.nbDims !=1 ||  dims0.d[0] !=1) return false;

    auto weights1 = inputs.at(1).weights();
    nvinfer1::Dims dims1 = weights1.shape;
    if(dims1.nbDims !=1 ||  dims1.d[0] !=1) return false;
    int32_t _b = 0;
    switch(weights1.type)
    {
       case((int32_t)::ONNX_NAMESPACE::TensorProto::FLOAT):
       {
            _b = (int32_t)(static_cast<float*>(weights1.values)[0]);
           break;
       }
       case((int32_t)::ONNX_NAMESPACE::TensorProto::INT32):
       {
            _b = (int32_t)(static_cast<int32_t *>(weights1.values)[0]);
           break;
       }
       //inputs[2]有可能是从文件读的
       case((int32_t)::ONNX_NAMESPACE::TensorProto::INT64):
       {
            _b = (int32_t)(static_cast<int64_t *>(weights1.values)[0]);
           break;
       }
       default:
           break;
    }


    int32_t _a = 0;

    switch(weights0.type)
    {
       case((int32_t)::ONNX_NAMESPACE::TensorProto::FLOAT):
       {
            _a = (int32_t)(static_cast<float*>(weights0.values)[0]);
           break;
       }
       case((int32_t)::ONNX_NAMESPACE::TensorProto::INT32):
       {
            _a = (int32_t)(static_cast<int32_t *>(weights0.values)[0]);
           break;
       }
       //input[1]一定是传递过来的，不是从文件读的
       default:
           break;
    }

    int32_t out_value = _a * _b;
    int ndim = 1;
    auto scale_dtype = ::ONNX_NAMESPACE::TensorProto::INT32;
    auto scale_shape = nvinfer1::Dims{ndim, {1, 1, 1, 1, 1, 1, 1, 1}};
    scale_shape.d[0]=1;
    newWeights = ctx->createTempWeights(scale_dtype, scale_shape);
    static_cast<int32_t*>(newWeights.values)[0] = out_value;
    //std::copy(temp_value.begin(), temp_value.end(), static_cast<int64_t *>(newWeights.values));

    return true;
}


#if 1
DEFINE_BUILTIN_OP_IMPORTER(Mul) {

  bool isScalar =false;

  //Begin:added by shendasai ,2019-8-23
  ShapedWeights newWeights;
  isScalar = mul_scalar(ctx, inputs, newWeights);

  if(isScalar)
  {
      return {{newWeights}};
  }


  

  ASSERT(inputs.size() == 2, ErrorCode::kINVALID_NODE);
  return combineTensorsElementwise(
      ctx, node, inputs, nvinfer1::ElementWiseOperation::kPROD, true);
}
#endif
DEFINE_BUILTIN_OP_IMPORTER(Neg) {
  return apply_unary_function(ctx, inputs.at(0), nvinfer1::UnaryOperation::kNEG);
}

DEFINE_BUILTIN_OP_IMPORTER(Pad) {
  ASSERT(inputs.at(0).is_tensor(), ErrorCode::kUNSUPPORTED_NODE);
  nvinfer1::ITensor& tensor = inputs.at(0).tensor();
  nvinfer1::DimsHW beg_padding, end_padding;
  OnnxAttrs attrs(node);
  auto mode = attrs.get<std::string>("mode", "constant");
  float value = attrs.get<float>("value", 0.f);
  ASSERT(mode == "constant" && value == 0, ErrorCode::kUNSUPPORTED_NODE);
  if( attrs.count("paddings") ) {
    // TODO: This is a WAR for old versions of ONNX and should be removed in future
    auto onnx_padding = attrs.get<std::vector<int>>("paddings");
    ASSERT(onnx_padding.size() == 8, ErrorCode::kUNSUPPORTED_NODE);
    ASSERT(onnx_padding[0] == 0 && onnx_padding[1] == 0 &&
           onnx_padding[2] == 0 && onnx_padding[3] == 0,
           ErrorCode::kUNSUPPORTED_NODE);
    beg_padding.h() = onnx_padding[4];
    end_padding.h() = onnx_padding[5];
    beg_padding.w() = onnx_padding[6];
    end_padding.w() = onnx_padding[7];
    RETURN_FIRST_OUTPUT(
      ctx->network()->addPadding(tensor, beg_padding, end_padding));
  }
  auto onnx_padding = attrs.get<std::vector<int>>("pads");
  ASSERT(onnx_padding.size() == 8, ErrorCode::kUNSUPPORTED_NODE);
  ASSERT(onnx_padding[0] == 0 && onnx_padding[1] == 0 &&
         onnx_padding[4] == 0 && onnx_padding[5] == 0,
         ErrorCode::kUNSUPPORTED_NODE);
  beg_padding.h() = onnx_padding[2];
  beg_padding.w() = onnx_padding[3];
  end_padding.h() = onnx_padding[6];
  end_padding.w() = onnx_padding[7];
  RETURN_FIRST_OUTPUT(
    ctx->network()->addPadding(tensor, beg_padding, end_padding));
}

DEFINE_BUILTIN_OP_IMPORTER(ParametricSoftplus) {
    ASSERT(inputs.at(0).is_tensor(), ErrorCode::kUNSUPPORTED_NODE);
    OnnxAttrs attrs(node);
    float alpha = attrs.get<float>("alpha");
    float beta = attrs.get<float>("beta");
    return activationHelper(ctx, node, inputs, nvinfer1::ActivationType::kSOFTPLUS, &alpha, &beta);
}
#if 1
DEFINE_BUILTIN_OP_IMPORTER(Pow) {
  ASSERT(inputs.size() == 2, ErrorCode::kINVALID_NODE);
  return combineTensorsElementwise(
      ctx, node, inputs, nvinfer1::ElementWiseOperation::kPOW, true);
}
#endif
// TODO: Prelu is currently ONLY supported with a constant scale factor, making it
// identcal with LeakyRelu. Removing the op from the registry until it is fully supported.

// DEFINE_BUILTIN_OP_IMPORTER(PRelu) {
//   ASSERT(inputs.at(0).is_tensor(),  ErrorCode::kUNSUPPORTED_NODE);
//   ASSERT(inputs.at(1).is_weights(), ErrorCode::kUNSUPPORTED_NODE);
//   ShapedWeights weights = inputs.at(1).weights();
//   ASSERT(weights.type == ::ONNX_NAMESPACE::TensorProto::FLOAT,
//          ErrorCode::kUNSUPPORTED_NODE);
//   // TODO: Add support for per-channel scale factor
//   nvinfer1::Dims scalar_shape{1, {1}};
//   ASSERT(weights.shape == scalar_shape, ErrorCode::kUNSUPPORTED_NODE);
//   float alpha = *reinterpret_cast<float const*>(weights.values);
//   RETURN_FIRST_OUTPUT(
//       ctx->addPluginV2(
//          new FancyActivationPlugin(FancyActivationPlugin::LEAKY_RELU, alpha),
//          {&inputs.at(0).tensor()}));
// }

DEFINE_BUILTIN_OP_IMPORTER(Reciprocal) {
  return apply_unary_function(ctx, inputs.at(0), nvinfer1::UnaryOperation::kRECIP);
}

#if NV_TENSORRT_MAJOR >= 4
NodeImportResult reduceTensor(IImporterContext* ctx,
                              ::ONNX_NAMESPACE::NodeProto const& node,
                              TensorOrWeights input,
                              nvinfer1::ReduceOperation operation) {
  ASSERT(input.is_tensor(), ErrorCode::kUNSUPPORTED_NODE);
  nvinfer1::ITensor& tensor = input.tensor();
  OnnxAttrs attrs(node);
  bool keepdims = attrs.get("keepdims", 1);
  int ndim = tensor.getDimensions().nbDims;
  std::vector<int> axes;
  if( attrs.count("axes") ) {
    axes = attrs.get<std::vector<int>>("axes");
  } else {
    axes.resize(ndim);
    std::iota(axes.begin(), axes.end(), 0);
  }
  uint32_t axis_mask = 0;
  for( int axis : axes ) {
    // Adjust axis to TensorRT format
    TRT_CHECK(convert_axis(axis, ndim));
    axis_mask |= 1 << axis;
  }
  RETURN_FIRST_OUTPUT(
      ctx->network()->addReduce(tensor, operation, axis_mask, keepdims));
}
DEFINE_BUILTIN_OP_IMPORTER(ReduceL1) {
  NodeImportResult abs_result = apply_unary_function(
      ctx, inputs.at(0), nvinfer1::UnaryOperation::kABS);
  if( abs_result.is_error() ) {
    return abs_result;
  }
  TensorOrWeights abs_input = abs_result.value().at(0);
  return reduceTensor(ctx, node, abs_input, nvinfer1::ReduceOperation::kSUM);
}
DECLARE_BUILTIN_OP_IMPORTER(ReduceSum);
DEFINE_BUILTIN_OP_IMPORTER(ReduceLogSum) {
  auto sum_result = importReduceSum(ctx, node, inputs);
  if( sum_result.is_error() ) {
    return sum_result;
  }
  TensorOrWeights sum_input = sum_result.value().at(0);
  return apply_unary_function(ctx, sum_input, nvinfer1::UnaryOperation::kLOG);
}
DEFINE_BUILTIN_OP_IMPORTER(ReduceLogSumExp) {
  // TODO: Abstract this sequence with a function or macro
  auto exp_result = apply_unary_function(
      ctx, inputs.at(0), nvinfer1::UnaryOperation::kEXP);
  if( exp_result.is_error() ) {
    return exp_result;
  }
  auto exp_inputs = exp_result.value();
  return importReduceLogSum(ctx, node, exp_inputs);
}
DECLARE_BUILTIN_OP_IMPORTER(ReduceSumSquare);
DEFINE_BUILTIN_OP_IMPORTER(ReduceL2) {
  auto sum_sqr_result = importReduceSumSquare(ctx, node, inputs);
  if( sum_sqr_result.is_error() ) {
    return sum_sqr_result;
  }
  TensorOrWeights sum_sqr = sum_sqr_result.value().at(0);
  return apply_unary_function(ctx, sum_sqr, nvinfer1::UnaryOperation::kSQRT);
}
DEFINE_BUILTIN_OP_IMPORTER(ReduceMax) {
  return reduceTensor(ctx, node, inputs.at(0), nvinfer1::ReduceOperation::kMAX);
}
DEFINE_BUILTIN_OP_IMPORTER(ReduceMean) {
  return reduceTensor(ctx, node, inputs.at(0), nvinfer1::ReduceOperation::kAVG);
}
DEFINE_BUILTIN_OP_IMPORTER(ReduceMin) {
  return reduceTensor(ctx, node, inputs.at(0), nvinfer1::ReduceOperation::kMIN);
}
DEFINE_BUILTIN_OP_IMPORTER(ReduceProd) {
  return reduceTensor(ctx, node, inputs.at(0), nvinfer1::ReduceOperation::kPROD);
}
DEFINE_BUILTIN_OP_IMPORTER(ReduceSum) {

      //转成float类型，elementwise不支持int32
  if(inputs.at(0).is_tensor())
  {
      nvinfer1::DataType  dtype= nvinfer1::DataType::kFLOAT;
      nvinfer1::ITensor& tensor = inputs.at(0).tensor();
      if(tensor.getType() == nvinfer1::DataType::kINT32)
      {
          //tensor.setType(nvinfer1::DataType::kFLOAT);
       
          nvinfer1::ILayer* layer_ptr =  ctx->addPluginV2Ext(
                    new MyCastPlugin(nvinfer1::DataType::kINT32, nvinfer1::DataType::kFLOAT),
                    {&tensor});
          nvinfer1::ITensor* cast_tensor = layer_ptr->getOutput(0);
          TensorOrWeights _tmp(cast_tensor);
          return reduceTensor(ctx, node, _tmp, nvinfer1::ReduceOperation::kSUM);
      }
  }
  return reduceTensor(ctx, node, inputs.at(0), nvinfer1::ReduceOperation::kSUM);
}
DEFINE_BUILTIN_OP_IMPORTER(ReduceSumSquare) {
  nvinfer1::ITensor& tensor = inputs.at(0).tensor();
  auto* sqr_layer = ctx->network()->addElementWise(
      tensor, tensor, nvinfer1::ElementWiseOperation::kPROD);
  ASSERT(sqr_layer, ErrorCode::kUNSUPPORTED_NODE);
  nvinfer1::ITensor* sqr_tensor_ptr = sqr_layer->getOutput(0);
  return reduceTensor(
      ctx, node, sqr_tensor_ptr, nvinfer1::ReduceOperation::kSUM);
}

#endif // NV_TENSORRT_MAJOR >= 4

DEFINE_BUILTIN_OP_IMPORTER(Relu) {
  ASSERT(inputs.at(0).is_tensor(), ErrorCode::kUNSUPPORTED_NODE);
  RETURN_FIRST_OUTPUT(
    ctx->network()->addActivation(
      inputs.at(0).tensor(), nvinfer1::ActivationType::kRELU));
}

DEFINE_BUILTIN_OP_IMPORTER(Reshape) {
  auto input = inputs.at(0);
  nvinfer1::Dims new_shape;
  if( ctx->getOpsetVersion() >= 5 ) {
    ASSERT(inputs.size() == 2, ErrorCode::kINVALID_NODE);
    auto new_shape_input = inputs.at(1);
    //sds-temp,输入的shape必须是weights。itensor在创建engine时值是不确定的.
    ASSERT(new_shape_input.is_weights(), ErrorCode::kUNSUPPORTED_NODE);
    {

        ShapedWeights new_shape_weights = new_shape_input.weights();
        ASSERT(new_shape_weights.shape.nbDims == 1, ErrorCode::kINVALID_NODE);

        //if float, convert to int32
        if(new_shape_weights.type == ::ONNX_NAMESPACE::TensorProto::FLOAT)
        {
          ShapedWeights::DataType _type = (int32_t)::ONNX_NAMESPACE::TensorProto::INT32;
    
          auto newW = ctx->createTempWeights(_type, new_shape_weights.shape);
          bool ret = onnx2trt::newWghFloat2Int(new_shape_weights, newW);
          if(ret)new_shape_weights = newW;
        }
        else if(new_shape_weights.type == ::ONNX_NAMESPACE::TensorProto::INT64)
        {
          ShapedWeights::DataType _type = (int32_t)::ONNX_NAMESPACE::TensorProto::INT32;
    
          auto newW = ctx->createTempWeights(_type, new_shape_weights.shape);
          bool ret = onnx2trt::newWghInt64ToInt(new_shape_weights, newW);
          if(ret)new_shape_weights = newW;
        }
        
        ASSERT(new_shape_weights.type == ::ONNX_NAMESPACE::TensorProto::INT32,
               ErrorCode::kINVALID_NODE);
        int32_t const* new_shape_ptr =
          static_cast<int32_t const*>(new_shape_weights.values);
        new_shape.nbDims = new_shape_weights.shape.d[0];
        std::copy(new_shape_ptr, new_shape_ptr + new_shape.nbDims, new_shape.d);
    }
  } else {
    OnnxAttrs attrs(node);
    new_shape = attrs.get<nvinfer1::Dims>("shape");
  }
    int infer_dim = -1;
    //sds-temp, gather输出的scalar 会走进这里
    if( input.is_weights() ) {
      auto weights = input.weights();
      TRT_CHECK(get_infer_dim(infer_dim,new_shape));
      if (infer_dim >= 0)
      {
        // Check that the -1 Dimension is correct.
        ASSERT(get_shape_size(weights.shape) % (-1*get_shape_size(new_shape)) == 0,
          ErrorCode::kINVALID_NODE);

        // Update the dim to the correct value
        int new_dim = get_shape_size(weights.shape) / (-1*get_shape_size(new_shape));
        new_shape.d[infer_dim] = new_dim;
        weights.shape = new_shape;
        ASSERT(get_shape_size(new_shape) == get_shape_size(weights.shape),
           ErrorCode::kUNSUPPORTED_NODE);
        return {{weights}};
      }
      else
      {
        weights.shape = new_shape;
        return {{weights}};
      }
    }
    else
    {
      nvinfer1::ITensor& tensor = input.tensor();

      //sds-temp,modifyed by shendasai, 输入的权重已经去掉batch_dim
      //(1)shape(2)constant(3)concat
      //new_shape = set_dims_CHW(remove_dim(new_shape, BATCH_DIM));
      new_shape = set_dims_CHW(new_shape);
      
      // Check for -1 dimension in new shape
      //检查有且只能有一个 -1
      TRT_CHECK(get_infer_dim(infer_dim,new_shape));

      //不存在-1
      if (infer_dim < 0) {
        //input[0]和input[1]的大小要一致
        //sds-temp, trt默认不支持broadcast
        ASSERT(get_shape_size(new_shape) ==
                   get_shape_size(tensor.getDimensions()),
               ErrorCode::kUNSUPPORTED_NODE);
      }
#if NV_TENSORRT_MAJOR < 4
      if( new_shape.nbDims == 1 ) {
        // Note: TRT implicitly flattens the input to FC layers, and in fact
        //       requires that it still has 4D shape, so in this case we
        //       simply ignore the reshape.
        RETURN_IDENTITY(inputs.at(0));
      } else {
        ASSERT(new_shape.nbDims == 3, ErrorCode::kUNSUPPORTED_NODE);
        nvinfer1::IShuffleLayer* layer = ctx->network()->addShuffle(tensor);
        ASSERT(layer, ErrorCode::kUNSUPPORTED_NODE);
        layer->setReshapeDimensions(new_shape);
        ASSERT(get_shape_size(layer->getOutput(0)->getDimensions()) ==
               get_shape_size(input.shape()), ErrorCode::kUNSUPPORTED_NODE);
        RETURN_FIRST_OUTPUT(layer);
      }
#else // NV_TENSORRT_MAJOR >= 4
      nvinfer1::IShuffleLayer* layer = ctx->network()->addShuffle(tensor);
      ASSERT(layer, ErrorCode::kUNSUPPORTED_NODE);
      layer->setReshapeDimensions(new_shape);
      ASSERT(get_shape_size(layer->getOutput(0)->getDimensions()) ==
             get_shape_size(input.shape()), ErrorCode::kUNSUPPORTED_NODE);
      //RETURN_FIRST_OUTPUT(layer);

      if(debug_on)
        return {{add_print_plugin(ctx, node,layer->getOutput(0))}};
      else
        return {{layer->getOutput(0)}}; 

    
#endif // NV_TENSORRT_MAJOR >= 4
  }
}

//DEFINE_BUILTIN_OP_IMPORTER(RNN) {
//  OnnxAttrs attrs(node);
//  std::string direction_str = attrs.get("direction", "forward");
//  ASSERT(direction_str == "forward" || direction_str == "bidirectional",
//         ErrorCode::kUNSUPPORTED_NODE);
//  nvinfer1::RNNDirection direction = (direction_str == "forward" ?
//                                      nvinfer1::RNNDirection::kUNIDIRECTION :
//                                      nvinfer1::RNNDirection::kBIDIRECTION);
//  int hidden_size = attrs.get<int>("hidden_size");
//  std::vector<std::string> default_activation_strs = {"TanH", "TanH"};
//  auto activation_strs = attrs.get("activations", default_activation_strs);
//  ASSERT(activation_strs.size() == 1 || activation_strs.size() == 2,
//         ErrorCode::kINVALID_NODE);
//  if( activation_strs.size() == 2 ) {
//    ASSERT(activation_strs.at(1) == activation_strs.at(0),
//           ErrorCode::kUNSUPPORTED_NODE);
//  }
//  std::string activation_str = activation_strs.at(0);
//  ASSERT(activation_str == "TanH" || activation_str == "Relu",
//         ErrorCode::kUNSUPPORTED_NODE);
//  nvinfer1::RNNOperation op = (activation_str == "TanH" ?
//                                nvinfer1::RNNOperation::kTANH :
//                                nvinfer1::RNNOperation::kRELU);
//  nvinfer1::RNNMode mode = nvinfer1::RnnMode::kLINEAR;
//  int do_output_sequence = attrs.get("output_sequence", 0);
//  ASSERT(inputs.at(0).is_tensor(), ErrorCode::kUNSUPPORTED_NODE);
//  int layer_count = 1;
//  int max_sequence_length = 64; // TODO: How to specify this?
//
//  // TODO: weights = concatenate inputs.at(1).weights() and inputs.at(2).weights() over slowest dim
//  //       biases  = inputs.at(3).weights(); [OPTIONAL, default 0]
//
//  auto* layer = ctx->network()->addRNN(
//    inputs.at(0).tensor(), layer_count, hidden_size, max_seq_length,
//    op, mode, direction, weights, biases);
//
//  // TODO: Looks like we need to transpose the outputs from [1, T, N, dir, C] to [1, T, dir, N, C]
//  //       Return {{output 0, output 1}}, but take care of outputs being optional (i.e., check how many outputs there are, as well as output_sequence)
//}

DEFINE_BUILTIN_OP_IMPORTER(ScaledTanh) {
  ASSERT(inputs.at(0).is_tensor(), ErrorCode::kUNSUPPORTED_NODE);
  OnnxAttrs attrs(node);
  float alpha = attrs.get<float>("alpha");
  float beta = attrs.get<float>("beta");
  return activationHelper(ctx, node, inputs, nvinfer1::ActivationType::kSCALED_TANH, &alpha, &beta);
}

DEFINE_BUILTIN_OP_IMPORTER(Selu) {
  ASSERT(inputs.at(0).is_tensor(), ErrorCode::kUNSUPPORTED_NODE);
  OnnxAttrs attrs(node);
  float alpha = attrs.get("alpha", 1.6732f);
  float beta = attrs.get("gamma", 1.0507f);
  return activationHelper(ctx, node, inputs, nvinfer1::ActivationType::kSELU, &alpha, &beta);
}

//sds:当遇到shape后，会返回一个weights。并不创建layer。
DEFINE_BUILTIN_OP_IMPORTER(Shape) {
  //shape is Dims
  auto shape = inputs.at(0).shape();
  if( inputs.at(0).is_tensor() ) {
   //sds-delete, 不扩展维度，batch-size这一维度暂不体现(shape后面跟gather和expand，都不需要体现)
   //sds,若不删，gather的indict需要+1
   // shape = insert_dim(shape, BATCH_DIM, -1);
   
  }
  nvinfer1::Dims weight_dims;
  weight_dims.nbDims = 1;
  weight_dims.d[0] = shape.nbDims;
  // Note: Should technically be int64, but int32 allows for TRT compatibility
  auto weights = ctx->createTempWeights(
      ::ONNX_NAMESPACE::TensorProto::INT32, weight_dims);
  std::copy(&shape.d[0], &shape.d[0] + shape.nbDims,
            static_cast<int32_t*>(const_cast<void*>(weights.values)));
  //sds，如果input是权重，比如2*2的矩阵，返回[2,2]
  //     如果input是其它层的输出, 2*2,   返回是[-1,2,2]
  return {{weights}};
}

DEFINE_BUILTIN_OP_IMPORTER(Sigmoid) {
  ASSERT(inputs.at(0).is_tensor(), ErrorCode::kUNSUPPORTED_NODE);
  RETURN_FIRST_OUTPUT(
    ctx->network()->addActivation(
      inputs.at(0).tensor(), nvinfer1::ActivationType::kSIGMOID));
}

DEFINE_BUILTIN_OP_IMPORTER(Sin)
{
    return unaryHelper(ctx, node, inputs, nvinfer1::UnaryOperation::kSIN);
}

DEFINE_BUILTIN_OP_IMPORTER(Sinh)
{
    return unaryHelper(ctx, node, inputs, nvinfer1::UnaryOperation::kSINH);
}

DEFINE_BUILTIN_OP_IMPORTER(Size) {
  // Can't support tensors because we don't know the batch dim until runtime
  ASSERT(inputs.at(0).is_tensor(), ErrorCode::kUNSUPPORTED_NODE);
  auto shape = inputs.at(0).shape();
  nvinfer1::Dims weight_dims;
  weight_dims.nbDims = 1;
  weight_dims.d[0] = 1;
  // Note: Should technically be int64, but int32 allows for TRT compatibility
  auto weights = ctx->createTempWeights(
      ::ONNX_NAMESPACE::TensorProto::INT32, weight_dims);
  int32_t size = get_shape_size(shape);
  *static_cast<int32_t*>(const_cast<void*>(weights.values)) = size;
  return {{weights}};
}

DEFINE_BUILTIN_OP_IMPORTER(Slice) {
  // TensorRT 5.1 only supports up to opset 9.
  ASSERT(ctx->getOpsetVersion() < 10, ErrorCode::kUNSUPPORTED_NODE);
  ASSERT(inputs.at(0).is_tensor(), ErrorCode::kUNSUPPORTED_NODE);
  nvinfer1::ITensor& tensor = inputs.at(0).tensor();;
  OnnxAttrs attrs(node);
  const auto starts = attrs.get<std::vector<int64_t>>("starts");
  const auto ends = attrs.get<std::vector<int64_t>>("ends");
  auto axes = attrs.get<std::vector<int64_t>>("axes");
  // If axes are empty, follow the ONNX spec and populate it with [0, 1, ..., len(starts) - 1]
  if (axes.size() == 0)
  {
    for (size_t i = 0; i < starts.size(); i++)
    {
      axes.push_back(i);
    }
  }
  ASSERT(axes.size() == starts.size() && axes.size() == ends.size(), ErrorCode::kINVALID_VALUE);

  const nvinfer1::Dims dims = tensor.getDimensions();
  const int nbDims = dims.nbDims;
  auto makeDims = [nbDims](int initVal)->nvinfer1::Dims{
    nvinfer1::Dims result{nbDims, {},{}};
    std::fill_n(&result.d[0], nbDims, initVal);
    return result;
  };
  nvinfer1::Dims sliceStart = makeDims(0);
  nvinfer1::Dims sliceSize = dims;
  const nvinfer1::Dims sliceStride = makeDims(1); // ONNX has no support for strides in Slice
  for (size_t i = 0; i < axes.size(); i++){
    int axis = axes[i];
    if (axis == 0) {
      // We can only check that starts is properly 0
      // but can't check end as we don't know batch size
      ASSERT(starts[i] == 0, ErrorCode::kINVALID_VALUE);
      std::cerr << "Warning: slice with starts=0 on batch axis is ignored" << std::endl;
      continue;
    }
    TRT_CHECK(convert_axis(axis, nbDims));
    int dim = dims.d[axis];
    int start = starts[i] >= 0 ? starts[i] : dim + starts[i];
    int end = ends[i] >= 0 ? ends[i] : dim + ends[i];
    sliceStart.d[axis] = start;
    sliceSize.d[axis] = end < dim ? end - start : dim - start;
  }

  // If entire slice op was a no-op, simply return the input tensor
  if (sliceStart == makeDims(0) && sliceSize == dims)
  {
    return {{&tensor}};
  }
  RETURN_FIRST_OUTPUT(ctx->network()->addSlice(tensor, sliceStart, sliceSize, sliceStride));
}

DEFINE_BUILTIN_OP_IMPORTER(Softmax) {
  ASSERT(inputs.at(0).is_tensor(), ErrorCode::kUNSUPPORTED_NODE);
  OnnxAttrs attrs(node);
  int axis = attrs.get("axis", 1);
  int ndim = inputs.at(0).shape().nbDims;
  TRT_CHECK(convert_axis(axis, ndim));
  nvinfer1::ITensor* tensor_ptr = &inputs.at(0).tensor();
  nvinfer1::Dims shape = tensor_ptr->getDimensions();
  // Reshape the tensor so that the softmax axis is 0
  if (axis > 0)
  {
    ASSERT(tensor_ptr = flatten_tensor(ctx, *tensor_ptr, axis), ErrorCode::kUNSUPPORTED_NODE);
    ASSERT(tensor_ptr = move_tensor_dimension(ctx, *tensor_ptr, axis, 0), ErrorCode::kUNSUPPORTED_NODE);
  }
  auto* layer = ctx->network()->addSoftMax(*tensor_ptr);
  ASSERT(layer, ErrorCode::kUNSUPPORTED_NODE);
  tensor_ptr = layer->getOutput(0);
  // Reshape the tensor back if it was reshaped above
  if (axis > 0)
  {
    ASSERT(tensor_ptr = move_tensor_dimension(ctx, *tensor_ptr, 0, axis), ErrorCode::kUNSUPPORTED_NODE);
    ASSERT(tensor_ptr = reshape_tensor(ctx, *tensor_ptr, shape), ErrorCode::kUNSUPPORTED_NODE);
  }
  return {{tensor_ptr}};
}

//sds:添加onnx-tensorrt自定义算子的两种方法
//    (1)activationHelper类似，转化为nvinfer内置layer
//    (2)类似于split,自定义plugin的方式。然后把op转为该plugin的方式。
DEFINE_BUILTIN_OP_IMPORTER(Softplus) {
    ASSERT(inputs.at(0).is_tensor(), ErrorCode::kUNSUPPORTED_NODE);
	//sds:转化为TensorRT的内置layer方法
    return activationHelper(ctx, node, inputs, nvinfer1::ActivationType::kSOFTPLUS);
}

DEFINE_BUILTIN_OP_IMPORTER(Softsign) {
    ASSERT(inputs.at(0).is_tensor(), ErrorCode::kUNSUPPORTED_NODE);
    return activationHelper(ctx, node, inputs, nvinfer1::ActivationType::kSOFTSIGN);
}

#if NV_TENSORRT_MAJOR >= 4
DEFINE_BUILTIN_OP_IMPORTER(SpaceToDepth) {
  ASSERT(inputs.at(0).is_tensor(), ErrorCode::kUNSUPPORTED_NODE);
  nvinfer1::ITensor* tensor_ptr = &inputs.at(0).tensor();
  nvinfer1::IShuffleLayer* layer = ctx->network()->addShuffle(*tensor_ptr);
  ASSERT(layer, ErrorCode::kUNSUPPORTED_NODE);
  OnnxAttrs attrs(node);
  int block_size = attrs.get<int>("blocksize");
  nvinfer1::Dims dims = tensor_ptr->getDimensions();
  int ndim_spatial = dims.nbDims - 1;
  nvinfer1::Dims new_shape1;
  new_shape1.nbDims = dims.nbDims + ndim_spatial;
  new_shape1.d[0] = dims.d[0];
  for( int i=0; i<ndim_spatial; ++i ) {
    ASSERT(dims.d[1 + i] % block_size == 0, ErrorCode::kINVALID_NODE);
    new_shape1.d[1 + 2*i + 0] = dims.d[1 + i] / block_size;
    new_shape1.d[1 + 2*i + 1] = block_size;
  }
  layer->setReshapeDimensions(new_shape1);
  nvinfer1::Permutation perm;
  perm.order[ndim_spatial] = 0;
  for( int i=0; i<ndim_spatial; ++i ) {
    perm.order[ndim_spatial + 1 + i] = 1 + 2*i + 0;
    perm.order[i] = 1 + 2*i + 1;
  }
  layer->setSecondTranspose(perm);
  tensor_ptr = layer->getOutput(0);
  dims = tensor_ptr->getDimensions();
  nvinfer1::Dims new_shape2;
  new_shape2.nbDims = dims.nbDims - ndim_spatial;
  new_shape2.d[0] = dims.d[ndim_spatial];
  for( int i=0; i<ndim_spatial; ++i ) {
    new_shape2.d[0] *= dims.d[i];
    new_shape2.d[1 + i] = dims.d[ndim_spatial + 1 + i];
  }
  tensor_ptr = reshape_tensor(ctx, *tensor_ptr, new_shape2);
  ASSERT(tensor_ptr, ErrorCode::kUNSUPPORTED_NODE);
  dims = tensor_ptr->getDimensions();
  return {{tensor_ptr}};
}
#endif // NV_TENSORRT_MAJOR >= 4

// TODO: Legacy op for pre-1.0 ONNX spec; can be removed at some point
DEFINE_BUILTIN_OP_IMPORTER(SpatialBN) {
  return importBatchNormalization(ctx, node, inputs);
}

//sds:node, inputs, ctx ; outputs
//sds:split, 安装要求分割成n份。
DEFINE_BUILTIN_OP_IMPORTER(Split) {
  ASSERT(inputs.at(0).is_tensor(), ErrorCode::kUNSUPPORTED_NODE);
  ASSERT(inputs.size() == 1, ErrorCode::kUNSUPPORTED_NODE);
  nvinfer1::Dims dims = inputs.at(0).shape();
  int nbDims = dims.nbDims;
  OnnxAttrs attrs(node);
  int axis = attrs.get<int>("axis", 0);
  TRT_CHECK(convert_axis(axis, nbDims));
  std::vector<int> output_lengths;
  int noutput = node.output().size();
  if( attrs.count("split") ) {
    output_lengths = attrs.get<std::vector<int>>("split");
    ASSERT((int)output_lengths.size() == noutput, ErrorCode::kINVALID_NODE);
  } else {
    ASSERT(dims.d[axis] % noutput == 0, ErrorCode::kINVALID_NODE);
    output_lengths.assign(noutput, dims.d[axis] / noutput);
  }
  //sds,通过split插件的形式存储在engine中。为什么这样呢?(因为plugin对象支持序列化
  //和反序列化等函数。)
  //对比caffe plugin, 需要通过pluginFactory工厂来过滤每个caffe.prototxt的layername判断是否创建plugin。
  //onnx则可以支持(1)直接buildin_op的方式。根据op name 的build in入口创建plugin。          
  //               (2)类似通用plugin，直接通过pluginCreator的createPlugin入口创建(createPlugin(const char* name, const nvinfer1::PluginFieldCollection* fc))
  //               (暂不支持，后续可以在每个build in入口的最开始先走createPlugin的方式支持)
  nvinfer1::IPluginV2Layer* layer =
      ctx->addPluginV2(new SplitPlugin(axis, output_lengths),
                     {&inputs.at(0).tensor()});
  ASSERT(layer, ErrorCode::kUNSUPPORTED_NODE);
  ASSERT(layer->getNbOutputs() == noutput, ErrorCode::kINTERNAL_ERROR);
  std::vector<TensorOrWeights> outputs;
  for( int i=0; i<noutput; ++i ) {
    outputs.push_back(layer->getOutput(i));
  }
  return outputs;
}

DEFINE_BUILTIN_OP_IMPORTER(Sqrt) {
  return apply_unary_function(ctx, inputs.at(0), nvinfer1::UnaryOperation::kSQRT);
}

#if NV_TENSORRT_MAJOR >= 4
DEFINE_BUILTIN_OP_IMPORTER(Squeeze) {
  ASSERT(inputs.at(0).is_tensor(), ErrorCode::kUNSUPPORTED_NODE);
  nvinfer1::ITensor& tensor = inputs.at(0).tensor();
  nvinfer1::Dims old_shape = tensor.getDimensions();
  int ndim_in = old_shape.nbDims;
  OnnxAttrs attrs(node);
  auto axes = attrs.get<std::vector<int>>("axes");
  // Note: Can't handle batch dim as it is implicit in TRT
  for( auto& axis : axes ) {
    TRT_CHECK(convert_axis(axis, ndim_in));
  }
  std::set<int> axes_set(axes.begin(), axes.end());
  int ndim_out = ndim_in - axes_set.size();
  ASSERT(ndim_out <= nvinfer1::Dims::MAX_DIMS,
         ErrorCode::kUNSUPPORTED_NODE);
  nvinfer1::Dims new_shape;
  new_shape.nbDims = ndim_out;
  for( int i=0,j=0; i<old_shape.nbDims; ++i ) {
    if( !axes_set.count(i) ) {
      new_shape.d[j++] = old_shape.d[i];
    } else {
      //sds-temp, onnx要求，如果axis = a,但a这个轴的dNum不是1，那么则不作处理
      //ASSERT(old_shape.d[i] == 1, ErrorCode::kINVALID_NODE);
      if(old_shape.d[i] != 1)
      {
          new_shape.d[j++] = old_shape.d[i];
          new_shape.nbDims +=1;
      }
    }
  }
  nvinfer1::IShuffleLayer* layer = ctx->network()->addShuffle(tensor);
  ASSERT(layer, ErrorCode::kUNSUPPORTED_NODE);
  layer->setReshapeDimensions(new_shape);
  ASSERT(get_shape_size(layer->getOutput(0)->getDimensions()) ==
         get_shape_size(old_shape), ErrorCode::kUNSUPPORTED_NODE);
  RETURN_FIRST_OUTPUT(layer);
}
#endif // NV_TENSORRT_MAJOR >= 4

DECLARE_BUILTIN_OP_IMPORTER(Add);
DEFINE_BUILTIN_OP_IMPORTER(Sub) {
  ASSERT(inputs.size() == 2, ErrorCode::kINVALID_NODE);
  return combineTensorsElementwise(
      ctx, node, inputs, nvinfer1::ElementWiseOperation::kSUB, true);
}

DEFINE_BUILTIN_OP_IMPORTER(Sum) {
  return combineTensorsElementwise(
    ctx, node, inputs, nvinfer1::ElementWiseOperation::kSUM);
}

DEFINE_BUILTIN_OP_IMPORTER(Tan)
{
    return unaryHelper(ctx, node, inputs, nvinfer1::UnaryOperation::kTAN);
}

DEFINE_BUILTIN_OP_IMPORTER(Tanh) {
  ASSERT(inputs.at(0).is_tensor(), ErrorCode::kUNSUPPORTED_NODE);
  RETURN_FIRST_OUTPUT(
    ctx->network()->addActivation(
      inputs.at(0).tensor(), nvinfer1::ActivationType::kTANH));
}

DEFINE_BUILTIN_OP_IMPORTER(ThresholdedRelu) {
    ASSERT(inputs.at(0).is_tensor(), ErrorCode::kUNSUPPORTED_NODE);
    OnnxAttrs attrs(node);
    float alpha = attrs.get<float>("alpha", 1.f);
    return activationHelper(ctx, node, inputs, nvinfer1::ActivationType::kTHRESHOLDED_RELU, &alpha);
}

#if NV_TENSORRT_MAJOR >= 4
DEFINE_BUILTIN_OP_IMPORTER(TopK) {
  // TensorRT 5.1 only supports up to opset 9.
  ASSERT(ctx->getOpsetVersion() < 10, ErrorCode::kUNSUPPORTED_NODE);
  ASSERT(inputs.at(0).is_tensor(), ErrorCode::kUNSUPPORTED_NODE);
  nvinfer1::ITensor& tensor = inputs.at(0).tensor();
  ASSERT(tensor.getType() != nvinfer1::DataType::kINT32,
         ErrorCode::kUNSUPPORTED_NODE);
  OnnxAttrs attrs(node);
  ASSERT(attrs.count("k"), ErrorCode::kINVALID_NODE);
  int k    = attrs.get<int>("k");
  int axis = attrs.get("axis", -1);
  int nbDims = tensor.getDimensions().nbDims;
  // Adjust axis to TensorRT format
  TRT_CHECK(convert_axis(axis, nbDims));

  uint32_t axis_mask = 1 << axis;
  auto* layer = ctx->network()->addTopK(
      tensor, nvinfer1::TopKOperation::kMAX, k, axis_mask);
  ASSERT(layer, ErrorCode::kUNSUPPORTED_NODE);
  return {{layer->getOutput(0), layer->getOutput(1)}};
}
#endif // NV_TENSORRT_MAJOR >= 4

DEFINE_BUILTIN_OP_IMPORTER(Transpose) {
  TensorOrWeights input = inputs.at(0);
  OnnxAttrs attrs(node);
  int ndim = input.shape().nbDims;
  ASSERT(ndim <= nvinfer1::Dims::MAX_DIMS, ErrorCode::kUNSUPPORTED_NODE);
  nvinfer1::Permutation default_perm; // Default is to reverse dims
  for( int i=0; i<ndim; ++i ) {
    default_perm.order[i] = ndim - 1 - i;
  }
  nvinfer1::Permutation perm = attrs.get("perm", default_perm);
  if( input.is_tensor() ) {
    // TRT doesn't support moving the batch dim
    //sds-temp,del this two line below.
    //输入的perm是不包含batch这个维度的
    //ASSERT(perm.order[BATCH_DIM] == BATCH_DIM, ErrorCode::kUNSUPPORTED_NODE);
    //perm = remove_first_dim(perm);
    
    //nvinfer1::Permutation perm_new = add_perm_value(perm);
    // Note: Dimension types kept unchanged in order to avoid TRT complaining about CHW order
    nvinfer1::ITensor* output_tensor =
        transpose_tensor(ctx, input.tensor(), perm, false);
    ASSERT(output_tensor, ErrorCode::kUNSUPPORTED_NODE);

    if(debug_on)
        return {{add_print_plugin(ctx, node,output_tensor)}};
    else
        return {{output_tensor}}; 
    
  }
  //sds-temp,也要考虑。只支持2d转换
  else {
    auto weights = input.weights();
    auto new_weights = ctx->createTempWeights(weights.type, weights.shape);
    ASSERT(transposeWeights(weights, perm, &new_weights),
           ErrorCode::kUNSUPPORTED_NODE);
    weights = new_weights;
    return {{weights}};
  }
}


bool  unsqueeze_scalar(IImporterContext* ctx, std::vector<TensorOrWeights>& inputs, ShapedWeights& newWeights)
{
    if(!inputs.at(0).is_weights()) return false;

    auto weights0 = inputs.at(0).weights();
    nvinfer1::Dims dims0 = weights0.shape;
    if(dims0.nbDims !=1 || dims0.d[0] !=1) return false;

    int32_t _value = static_cast<int32_t*>(weights0.values)[0];


    int ndim = 1;
    auto scale_dtype = ::ONNX_NAMESPACE::TensorProto::INT32;
    auto scale_shape = nvinfer1::Dims{ndim, {1, 1, 1, 1, 1, 1, 1, 1}};
    scale_shape.d[0]=1;
    newWeights = ctx->createTempWeights(scale_dtype, scale_shape);
    static_cast<int32_t*>(newWeights.values)[0] = _value;
    //std::copy(temp_value.begin(), temp_value.end(), static_cast<int64_t *>(newWeights.values));

    return true;
}

  
 

#if NV_TENSORRT_MAJOR >= 4
DEFINE_BUILTIN_OP_IMPORTER(Unsqueeze) {

  //Begin: added by shendasai for output is scalar case, 2019年8月23日14:03:19.
  bool isOutScalar =false;
  ShapedWeights newWeights;
  isOutScalar = unsqueeze_scalar(ctx, inputs, newWeights);
  if(isOutScalar)
  {
      return {{newWeights}};
  }
  //End: added by shendasai for input is scalar case, 2019年8月23日14:03:19.

  
  nvinfer1::ITensor& tensor = convertToTensor(inputs.at(0), ctx);
  nvinfer1::Dims old_shape = tensor.getDimensions();
  int ndim_in = old_shape.nbDims;
  OnnxAttrs attrs(node);
  auto axes = attrs.get<std::vector<int>>("axes");
  // If the input was already a tensor, then we're dealing with a TRT shape,
  // so subtract 1 from the axes. Otherwise, this is an ONNX shape.
  if (inputs.at(0).is_tensor())
  {
      for (auto& axis : axes)
      {
          //sds-temp,下面注释掉
          #if 0
          ASSERT(axis != BATCH_DIM, ErrorCode::kUNSUPPORTED_NODE);//不能是0
          --axis;//轴依次-1
          #endif
          
      }
  }

  std::set<int> axes_set(axes.begin(), axes.end());
  int ndim_out = ndim_in + axes_set.size();
  ASSERT(ndim_out <= nvinfer1::Dims::MAX_DIMS, ErrorCode::kUNSUPPORTED_NODE);
  nvinfer1::Dims new_shape;
  new_shape.nbDims = ndim_out;

  for (int i = 0, j = 0; j < new_shape.nbDims; ++j )
  {
      if( !axes_set.count(j) )
      {
          new_shape.d[j] = old_shape.d[i++];
      }
      else
      {
          new_shape.d[j] = 1;
      }
  }
  nvinfer1::IShuffleLayer* layer = ctx->network()->addShuffle(tensor);
  ASSERT(layer, ErrorCode::kUNSUPPORTED_NODE);
  layer->setReshapeDimensions(new_shape);
  ASSERT(get_shape_size(layer->getOutput(0)->getDimensions()) == get_shape_size(old_shape),
      ErrorCode::kUNSUPPORTED_NODE);
  RETURN_FIRST_OUTPUT(layer);
}
#endif // NV_TENSORRT_MAJOR >= 4

DEFINE_BUILTIN_OP_IMPORTER(Upsample) {
  ASSERT(inputs.at(0).is_tensor(), ErrorCode::kUNSUPPORTED_NODE);
  nvinfer1::ITensor &tensor = inputs.at(0).tensor();
  ASSERT(tensor.getDimensions().nbDims == 3, ErrorCode::kUNSUPPORTED_NODE);
  OnnxAttrs attrs(node);
  float height_scale, width_scale;
  if (ctx->getOpsetVersion() >= 9) {
    ASSERT(inputs.size() == 2, ErrorCode::kINVALID_NODE);
    auto scales_input = inputs.at(1);
    ASSERT(scales_input.is_weights(), ErrorCode::kUNSUPPORTED_NODE);
    ShapedWeights scales_weights = scales_input.weights();
    ASSERT(scales_weights.shape.nbDims == 1, ErrorCode::kUNSUPPORTED_NODE);
    ASSERT(scales_weights.count() == 4, ErrorCode::kUNSUPPORTED_NODE);
    ASSERT(scales_weights.type == ::ONNX_NAMESPACE::TensorProto::FLOAT,
           ErrorCode::kINVALID_NODE);
    float const *scales_ptr = static_cast<float const *>(scales_weights.values);
    ASSERT(scales_ptr[0] == 1 && scales_ptr[1] == 1,
           ErrorCode::kUNSUPPORTED_NODE);
    height_scale = scales_ptr[2];
    width_scale = scales_ptr[3];
  } else {
    if (!attrs.count("scales")) {
      height_scale = attrs.get<float>("height_scale");
      width_scale = attrs.get<float>("width_scale");
    } else {
      auto scales = attrs.get<std::vector<float>>("scales");
      ASSERT(scales.size() == 4, ErrorCode::kUNSUPPORTED_NODE);
      ASSERT(scales[0] == 1 && scales[1] == 1, ErrorCode::kUNSUPPORTED_NODE);
      height_scale = scales[2];
      width_scale = scales[3];
    }
  }
  auto scale = {height_scale, width_scale};
  auto mode = attrs.get<std::string>("mode", "nearest");
  ASSERT(mode == "nearest", ErrorCode::kUNSUPPORTED_NODE);
  RETURN_FIRST_OUTPUT(
      ctx->addPluginV2(new ResizeNearestPlugin(scale), {&inputs.at(0).tensor()}));
}

} // namespace

} // namespace onnx2trt
