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

#pragma once

#include "ShapedWeights.hpp"
#include "trt_utils.hpp"
#include "OnnxAttrs.hpp"

#include <onnx/onnx_pb.h>
#include <onnx/onnxifi.h>
#include <NvInfer.h>

#include <iostream>
using std::cerr;
using std::endl;

inline std::ostream& operator<<(std::ostream& stream, nvinfer1::Dims const& shape) {
  if( shape.nbDims == 0 ) {
    return stream;
  }
  stream << "(" << shape.d[0];
  for( int i=1; i<shape.nbDims; ++i ) {
    stream << ", " << shape.d[i];
  }
  stream << ")";
  return stream;
}

inline std::ostream& operator<<(std::ostream& stream, nvinfer1::DataType const& dtype) {
  switch( dtype ) {
  case nvinfer1::DataType::kFLOAT: return stream << "float32";
  case nvinfer1::DataType::kHALF:  return stream << "float16";
  case nvinfer1::DataType::kINT8:  return stream << "int8";
#if NV_TENSORRT_MAJOR >= 4
  case nvinfer1::DataType::kINT32: return stream << "int32";
#endif
  default: throw std::runtime_error("Unknown dtype");
  }
}

// TODO: Remove this when finished debugging
inline std::ostream& operator<<(std::ostream& stream, nvinfer1::Permutation const& perm) {
  int ndims = nvinfer1::Dims::MAX_DIMS;
  stream << "(" << perm.order[0];
  for( int i=1; i<ndims; ++i ) {
    stream << ", " << perm.order[i];
  }
  stream << ")";
  return stream;
}
/*
// TODO: Remove this when finished debugging
inline std::ostream& operator<<(std::ostream& stream, google::protobuf::Message const& message) {
  stream << print_onnx_to_string(message);
  return stream;
}
*/
namespace onnx2trt {

inline bool convertINT32ToFloat(void* weightValues, size_t nbWeights, std::vector<float>& converted_weights)
{
  int32_t * weights = static_cast<int32_t *>(weightValues);
  for (size_t i = 0; i < nbWeights; i++)
  {
    if (weights[i] > INT32_MAX || weights[i] < INT32_MIN)
    {
      return false;
    }
    else
    {
      converted_weights[i] = static_cast<float>(weights[i]);
    }
  }
  return true;
}

inline bool convertFloatToInt32(void* weightValues, size_t nbWeights, std::vector<int32_t>& converted_weights)
{
  float * weights = static_cast<float *>(weightValues);
  for (size_t i = 0; i < nbWeights; i++)
  {
    if (weights[i] > INT32_MAX || weights[i] < INT32_MIN)
    {
      return false;
    }
    else
    {
      converted_weights[i] = static_cast<int32_t>(weights[i]);
    }
  }
  return true;
}

inline bool getNewWeights(ShapedWeights& w , ShapedWeights& newW){
  //int64的，在初始化时已经被转成了int32,所以实际是int32到float的转化
  if ((w.type) == (int32_t)::ONNX_NAMESPACE::TensorProto::INT64  && newW.type ==(int32_t)::ONNX_NAMESPACE::TensorProto::FLOAT )
  {
    std::vector<float> _weights;
    _weights.resize(w.count());

    if (!onnx2trt::convertINT32ToFloat(w.values, w.count(), _weights))
    {
      cerr << "ERROR: Weights cannot be cast down to FLOAT." << endl;
      // Return empty w on failure
      return false;
    }
    else
    {
      void * _weights_ptr = static_cast<void *>(_weights.data());
      std::memcpy(newW.values, _weights_ptr, _weights.size() * sizeof(float));
      cout << "Successfully casted down from int64 to float." << endl;
    }
  }
  else
  {
    cout << "only support int32 to float now." << endl;
    return false;
  }
  
  //newW.type= (int32_t)nvinfer1::DataType::kFLOAT;
 // newW.count =w.count();
  return true;
}

inline bool newWghFloat2Int(ShapedWeights& w , ShapedWeights& newW){

  if ((w.type) == (int32_t)::ONNX_NAMESPACE::TensorProto::FLOAT  && newW.type ==(int32_t)::ONNX_NAMESPACE::TensorProto::INT32 )
  {
    std::vector<int32_t> _weights;
    _weights.resize(w.count());

    if (!onnx2trt::convertFloatToInt32(w.values, w.count(), _weights))
    {
      cerr << "ERROR: Weights cannot be cast down to FLOAT." << endl;
      // Return empty w on failure
      return false;
    }
    else
    {
      void * _weights_ptr = static_cast<void *>(_weights.data());
      std::memcpy(newW.values, _weights_ptr, _weights.size() * sizeof(int32_t));
      cout << "Successfully casted down from float to INT32." << endl;
    }
  }
  else
  {
    cout << "only support float to int32 now." << endl;
    return false;
  }
  
  //newW.type= (int32_t)nvinfer1::DataType::kFLOAT;
 // newW.count =w.count();
  return true;
}

inline bool newWghInt64ToInt(ShapedWeights& w , ShapedWeights& newW){

  if ((w.type) == (int32_t)::ONNX_NAMESPACE::TensorProto::INT64  && newW.type ==(int32_t)::ONNX_NAMESPACE::TensorProto::INT32 )
  {
    std::vector<int32_t> _weights;
    _weights.resize(w.count());

    if (!onnx2trt::convertINT64(w.values, w.count(), _weights))
    {
      cerr << "ERROR: Weights cannot be cast down to FLOAT." << endl;
      // Return empty w on failure
      return false;
    }
    else
    {
      void * _weights_ptr = static_cast<void *>(_weights.data());
      std::memcpy(newW.values, _weights_ptr, _weights.size() * sizeof(int32_t));
      cout << "Successfully casted down from int64 to  INT32." << endl;
    }
  }
  else
  {
    cout << "only support float to int32 now." << endl;
    return false;
  }

  //newW.type= (int32_t)nvinfer1::DataType::kFLOAT;
 // newW.count =w.count();
  return true;
}


#if 0
inline nvinfer1::ITensor* reshape_tensor(IImporterContext* ctx, nvinfer1::ITensor& tensor, nvinfer1::Dims shape) 
{
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
#endif
inline int get_dtype_size(int32_t onnx_dtype) {
  switch( onnx_dtype ) {
  case ::ONNX_NAMESPACE::TensorProto::FLOAT16:    return 2;
  case ::ONNX_NAMESPACE::TensorProto::FLOAT:      return 4;
  case ::ONNX_NAMESPACE::TensorProto::DOUBLE:     return 8;
  case ::ONNX_NAMESPACE::TensorProto::COMPLEX64:  return 8;
  case ::ONNX_NAMESPACE::TensorProto::COMPLEX128: return 16;
  case ::ONNX_NAMESPACE::TensorProto::UINT8:      return 1;
  case ::ONNX_NAMESPACE::TensorProto::INT8:       return 1;
  case ::ONNX_NAMESPACE::TensorProto::UINT16:     return 2;
  case ::ONNX_NAMESPACE::TensorProto::INT16:      return 2;
  case ::ONNX_NAMESPACE::TensorProto::UINT32:     return 4;
  case ::ONNX_NAMESPACE::TensorProto::INT32:      return 4;
  case ::ONNX_NAMESPACE::TensorProto::UINT64:     return 8;
  case ::ONNX_NAMESPACE::TensorProto::INT64:      return 8;
  // TODO: Add BOOL if necessary...
    // TODO: Some sort of error handling
  default: return -1;//throw std::invalid_argument("Unsupported TRT data type: " +
                     //                  std::to_string((int)trt_dtype));
  }
}

inline const char* get_dtype_name(int32_t onnx_dtype) {
  switch( onnx_dtype ) {
  case ::ONNX_NAMESPACE::TensorProto::FLOAT:      return "FLOAT";
  case ::ONNX_NAMESPACE::TensorProto::UINT8:      return "UINT8";
  case ::ONNX_NAMESPACE::TensorProto::INT8:       return "INT8";
  case ::ONNX_NAMESPACE::TensorProto::UINT16:     return "UINT16";
  case ::ONNX_NAMESPACE::TensorProto::INT16:      return "INT16";
  case ::ONNX_NAMESPACE::TensorProto::INT32:      return "INT32";
  case ::ONNX_NAMESPACE::TensorProto::INT64:      return "INT64";
  case ::ONNX_NAMESPACE::TensorProto::STRING:     return "STRING";
  case ::ONNX_NAMESPACE::TensorProto::BOOL:       return "BOOL";
  case ::ONNX_NAMESPACE::TensorProto::FLOAT16:    return "FLOAT16";
  case ::ONNX_NAMESPACE::TensorProto::DOUBLE:     return "DOUBLE";
  case ::ONNX_NAMESPACE::TensorProto::UINT32:     return "UINT32";
  case ::ONNX_NAMESPACE::TensorProto::UINT64:     return "UINT64";
  case ::ONNX_NAMESPACE::TensorProto::COMPLEX64:  return "COMPLEX64";
  case ::ONNX_NAMESPACE::TensorProto::COMPLEX128: return "COMPLEX128";
  default: return "<UNKNOWN>";
  }
}
#if 0
inline void broadcast_tensors(IImporterContext* ctx, nvinfer1::ITensor*& t1, nvinfer1::ITensor*& t2)
{
    if (t1->getDimensions().nbDims == t2->getDimensions().nbDims)
    {
        return;
    }
    nvinfer1::ITensor* largeTensor;
    nvinfer1::ITensor* smallTensor;
    if (t1->getDimensions().nbDims > t2->getDimensions().nbDims)
    {
        largeTensor = t1;
        smallTensor = t2;
    }
    else
    {
        largeTensor = t2;
        smallTensor = t1;
    }

    nvinfer1::Dims largeDims = largeTensor->getDimensions();
    nvinfer1::Dims smallDims = smallTensor->getDimensions();
    nvinfer1::Dims newDims({largeDims.nbDims, {1, 1, 1, 1, 1, 1, 1, 1}});

    int i(0), j(0);
    while (i < smallDims.nbDims && j < largeDims.nbDims)
    {
        if (smallDims.d[i] == largeDims.d[j])
        {
            newDims.d[j] = largeDims.d[j];
            i++;
            j++;
        }
        else
        {
            j++;
        }
    }

    t1 == smallTensor ? t1 = reshape_tensor(ctx, *t1, newDims) : t2 = reshape_tensor(ctx, *t2, newDims);
}
#endif
inline bool check_for_input(::ONNX_NAMESPACE::NodeProto const& node, std::string const& input_node)
{
  for (auto input : node.input())
  {
    if (input_node == input)
    {
      return true;
    }
  }
  return false;
}

inline bool convert_dtype(int32_t onnx_dtype,
                          nvinfer1::DataType* trt_dtype) {
  switch( onnx_dtype ) {
  case ::ONNX_NAMESPACE::TensorProto::FLOAT:   *trt_dtype = nvinfer1::DataType::kFLOAT; break;
  case ::ONNX_NAMESPACE::TensorProto::INT8:    *trt_dtype = nvinfer1::DataType::kINT8;  break;
  case ::ONNX_NAMESPACE::TensorProto::FLOAT16: *trt_dtype = nvinfer1::DataType::kHALF;  break;
#if NV_TENSORRT_MAJOR >= 4
  // See ShapedWeights.cpp for sanity check if all values can be safetly downcasted to INT32
 //case ::ONNX_NAMESPACE::TensorProto::INT64:   *trt_dtype = nvinfer1::DataType::kINT32; break;
case ::ONNX_NAMESPACE::TensorProto::INT64:   *trt_dtype = nvinfer1::DataType::kFLOAT; break;
  case ::ONNX_NAMESPACE::TensorProto::INT32:   *trt_dtype = nvinfer1::DataType::kINT32; break;
  case ::ONNX_NAMESPACE::TensorProto::BOOL:   *trt_dtype = nvinfer1::DataType::kINT32; break;//sds
#endif
  default:
    cerr << "Unsupported ONNX data type: " << get_dtype_name(onnx_dtype)
         << " (" << std::to_string(onnx_dtype) << ")" << endl;
    return false;
  }
  return true;
}

inline bool convert_input_dtype(int32_t onnx_dtype,
                          nvinfer1::DataType* trt_dtype) {
  switch( onnx_dtype ) {
  case ::ONNX_NAMESPACE::TensorProto::FLOAT:   *trt_dtype = nvinfer1::DataType::kFLOAT; break;
  case ::ONNX_NAMESPACE::TensorProto::INT8:    *trt_dtype = nvinfer1::DataType::kINT8;  break;
  case ::ONNX_NAMESPACE::TensorProto::FLOAT16: *trt_dtype = nvinfer1::DataType::kHALF;  break;
#if NV_TENSORRT_MAJOR >= 4
  //modify:added by shendasai for solver not support input int64. no check!
  //case ::ONNX_NAMESPACE::TensorProto::INT64:   *trt_dtype = nvinfer1::DataType::kINT32; break;
  case ::ONNX_NAMESPACE::TensorProto::INT64:   *trt_dtype = nvinfer1::DataType::kFLOAT; break;
  case ::ONNX_NAMESPACE::TensorProto::INT32:   *trt_dtype = nvinfer1::DataType::kINT32; break;
#endif
  default:
    cerr << "Unsupported ONNX data type: " << get_dtype_name(onnx_dtype)
         << " (" << std::to_string(onnx_dtype) << ")" << endl;
    return false;
  }
  return true;
}

//sds:需要删除batch信息。
template<typename OnnxDims>
inline bool convert_dims(OnnxDims const& onnx_dims, nvinfer1::Dims& trt_dims) {
  enum { BATCH_DIM = 0 };
  std::vector<int> onnx_dims_vector;
  for( auto const& onnx_dim : onnx_dims ) {
    // TODO: Unknown dimensions are represented using onnx_dim.dim_param
    // Dynamically sized inputs are currently not supported. Catch these cases
    // as onnx_dim.dim_value() == 0 on non-batch dimensions and throw an error.
    //ASSERT(onnx_dims_vector.empty() || onnx_dim.dim_value() != 0, ErrorCode::kUNSUPPORTED_GRAPH);
    if (onnx_dims_vector.empty() || onnx_dim.dim_value() != 0)
    {
      onnx_dims_vector.push_back(onnx_dim.dim_value());
    }
    else
    {
      return false;
    }
  }
  trt_dims.nbDims = onnx_dims_vector.size();
  if (trt_dims.nbDims >= nvinfer1::Dims::MAX_DIMS)
  {
    return false;
  }
  std::copy(onnx_dims_vector.begin(), onnx_dims_vector.end(), trt_dims.d);
  // Remove batch dimension from trt_dims.
  //sds: tensorrt的input itensor是针对单个batch的，所以需要删除batch信息。
  //trt_dims = set_dims_CHW(remove_dim(trt_dims, BATCH_DIM));
  trt_dims = set_dims_CHW((trt_dims));//sds, 不删除batach .faiseq的batch和trt不是同一理念
  return true;
}

inline bool convert_weight_descriptor(onnxTensorDescriptorV1 const &desc,
                                      onnx2trt::ShapedWeights *weights) {
  nvinfer1::Dims shape;
  shape.nbDims = desc.dimensions;
  // Special case for scalars
  if( shape.nbDims == 0 ) {
    shape.nbDims = 1;
    shape.d[0] = 1;
    shape.type[0] = nvinfer1::DimensionType::kCHANNEL;
  } else {
    std::copy(desc.shape, desc.shape + desc.dimensions, shape.d);
  }

  size_t element_count = 1;
  for (int i = 0; i < shape.nbDims; ++i) {
    element_count *= shape.d[i];
  }

  void* data_ptr;
  size_t nbytes;
  int32_t dtype;
  data_ptr = (void*)(desc. buffer);
  if (desc.dataType == ONNXIFI_DATATYPE_FLOAT32) {
    dtype = ::ONNX_NAMESPACE::TensorProto::FLOAT;
    nbytes = element_count * sizeof(float);
  } else if (desc.dataType == ONNXIFI_DATATYPE_FLOAT16) {
    dtype = ::ONNX_NAMESPACE::TensorProto::FLOAT16;
    nbytes = element_count * sizeof(float) / 2;
  } else if (desc.dataType == ONNXIFI_DATATYPE_INT32) {
    dtype = ::ONNX_NAMESPACE::TensorProto::INT32;
    nbytes = element_count * sizeof(int32_t);
  } else if (desc.dataType == ONNXIFI_DATATYPE_INT64) {
    dtype = ::ONNX_NAMESPACE::TensorProto::INT64;
    nbytes = element_count * sizeof(int64_t);
  } else {
    // Unsupported format
    return false;
  }

  onnx2trt::ShapedWeights trt_weights(dtype, data_ptr, shape);
  (void)nbytes;
  assert(trt_weights.size_bytes() == nbytes);
  *weights = trt_weights;
  return true;
}

inline bool convert_onnx_weights(::ONNX_NAMESPACE::TensorProto const& onnx_tensor,
                                 onnx2trt::ShapedWeights* weights) {
  nvinfer1::Dims shape;
  shape.nbDims = onnx_tensor.dims().size();
  std::copy(onnx_tensor.dims().begin(), onnx_tensor.dims().end(),
            shape.d);
  // Special case for scalars
  //sds,trt中scalar, nbDims是1，不是0
  if( shape.nbDims == 0 ) {
    shape.nbDims = 1;
    shape.d[0] = 1;
    shape.type[0] = nvinfer1::DimensionType::kCHANNEL;
  }
  auto dtype = onnx_tensor.data_type();
  void* data_ptr; // TODO: See if can make const*
  size_t nbytes;
  if( onnx_tensor.raw_data().size() > 0 ) {
    data_ptr = (void*)onnx_tensor.raw_data().data();
    nbytes = onnx_tensor.raw_data().size();
  } else if( onnx_tensor.float_data().size() > 0 ) {
    assert(dtype == ::ONNX_NAMESPACE::TensorProto::FLOAT);
    data_ptr = (void*)onnx_tensor.float_data().data();
    nbytes = onnx_tensor.float_data().size() * sizeof(float);
  } else if( onnx_tensor.int32_data().size() > 0 ) {
    assert(dtype == ::ONNX_NAMESPACE::TensorProto::INT32 ||
           dtype == ::ONNX_NAMESPACE::TensorProto::INT16 ||
	   dtype == ::ONNX_NAMESPACE::TensorProto::INT8 ||
	   dtype == ::ONNX_NAMESPACE::TensorProto::UINT16 ||
	   dtype == ::ONNX_NAMESPACE::TensorProto::UINT8 ||
	   dtype == ::ONNX_NAMESPACE::TensorProto::BOOL ||
	   dtype == ::ONNX_NAMESPACE::TensorProto::FLOAT16);
    data_ptr = (void*)onnx_tensor.int32_data().data();
    nbytes = onnx_tensor.int32_data().size() * sizeof(int32_t);
  } else if( onnx_tensor.int64_data().size() > 0 ) {
    assert(dtype == ::ONNX_NAMESPACE::TensorProto::INT64);
    data_ptr = (void*)onnx_tensor.int64_data().data();
    nbytes = onnx_tensor.int64_data().size() * sizeof(int64_t);
  } else {
    // Unsupported ONNX tensor format!
    return false;
  }

  onnx2trt::ShapedWeights trt_weights(dtype, data_ptr, shape);
  (void)nbytes;
  assert(trt_weights.size_bytes() == nbytes);
  *weights = trt_weights;
  return true;
}

// Returns the input if it is already a tensor. If it is of type ShapedWeights, adds a new
// constant layer to the TRT network and returns its output.
inline nvinfer1::ITensor& convertToTensor(TensorOrWeights& input, IImporterContext* ctx)
{
    if (input.is_tensor())
    {
        return input.tensor();
    }
    else
    {
        // Handle non-tensor indices input by adding a new constant layer to the network.
        const ShapedWeights& weights = input.weights();
#if 0
        //sds-temp转成float
    ShapedWeights::DataType _type = (int32_t)::ONNX_NAMESPACE::TensorProto::FLOAT;
    
    auto newW = ctx->createTempWeights(_type, weights.shape);
    bool ret = onnx2trt::getNewWeights(weights, newW);
#endif
  return *(ctx->network()->addConstant(weights.shape, weights)->getOutput(0));
        //addConstant中申请显存会在batchsize这一维度上作broadcast吗?
        //注意看下这里的getOutput是什么维度
        //sds_check?
    }

}

inline nvinfer1::ITensor& convert_output_weight_to_tensor(TensorOrWeights& input, IImporterContext* ctx)
{
    if (input.is_tensor())
    {
        return input.tensor();
    }
    else
    {
        // Convert weight output to tensor output. Strip batch dimension here.
        const ShapedWeights& weights = input.weights();
        nvinfer1::Dims tensor_shape = weights.shape;
        //sds，DCHW->CHW, 去除了batch dimension.
        tensor_shape= set_dims_CHW(remove_dim(tensor_shape, 0));
        return *(ctx->network()->addConstant(tensor_shape, weights)->getOutput(0));
    }

}

inline int div_ceil(int n, int d) {
  return (n - 1) / d + 1;
}

// Convert an ONNX axis into a TRT axis
inline Status convert_axis(int& axis, int nbDims)
{
  // Support negative indexing
  if (axis < 0)
  {
    axis += nbDims;
  }
  // If axis was positive, subtract 1 to strip batch dimension
  else
  {
    //sds-temp, 不需要-1了。我的模型中本来也不含batch这个维度
    //axis = axis - 1;
  }
  ASSERT(axis >= 0 && axis <= nbDims, ErrorCode::kUNSUPPORTED_NODE);
  cout <<" the nbDims and axis is: " << axis <<"   ,  " << nbDims << endl;
  
  return Status::success();
}

inline int get_conv_output_size(int input_size, int filter_size,
                                int stride, int dilation_rate,
                                int total_padding) {
  // This is based on the CUDNN formula
  int effective_input_size  = input_size + total_padding;
  int effective_filter_size = (filter_size - 1) * dilation_rate + 1;
  return div_ceil(effective_input_size - (effective_filter_size - 1), stride);
}

// Helper function to help extract the index of a potential -1 dimension in the reshape node
inline Status get_infer_dim(int& infer_dim, nvinfer1::Dims const& new_shape) {
  for (int i = 0; i < new_shape.nbDims; ++i) {
    if (new_shape.d[i] < 0) {
      // -1 bears special meaning, which means the current dimension can
      // be inferred while keepin the total number of elements the same.
      // https://github.com/onnx/onnx/blob/9b9f595107e3fc0295d50f6294d43879df17552f/onnx/defs/tensor/defs.cc#L73-L75
      ASSERT(new_shape.d[i] == -1, ErrorCode::kUNSUPPORTED_NODE);
      // We can only one dimension that has -1
      ASSERT(infer_dim == -1, ErrorCode::kUNSUPPORTED_NODE);
      infer_dim = i;
    }
  }
  return Status::success();
}

void get_kernel_params(::ONNX_NAMESPACE::NodeProto const& onnx_node,
                       nvinfer1::DimsHW const& input_shape,
                       nvinfer1::DimsHW* kernel_size,
                       nvinfer1::DimsHW* strides,
                       nvinfer1::DimsHW* beg_padding,
                       nvinfer1::DimsHW* end_padding,
                       nvinfer1::PaddingMode& paddingMode,
                       nvinfer1::DimsHW* dilations=nullptr,
                       nvinfer1::DimsHW const* output_shape=nullptr);

inline nvinfer1::ScaleMode get_scale_mode(nvinfer1::Dims const& weights_shape) {
  if( weights_shape.nbDims == 1 ) {
    if( weights_shape.d[0] == 1 ) {
      return nvinfer1::ScaleMode::kUNIFORM;
    } else {
      return nvinfer1::ScaleMode::kCHANNEL;
    }
  } else {
    return nvinfer1::ScaleMode::kELEMENTWISE;
  }
}

inline void update_padded_values(std::vector<float>&pad_values, const nvinfer1::DimsHW beg_padding,
  const nvinfer1::DimsHW end_padding, const nvinfer1::Dims padded_shape, const float pad_value)
{
  int pad_h = padded_shape.d[1];
  int pad_w = padded_shape.d[2];
  int num_elements = pad_values.size();

  // Handle H padding. First beg_padding.h * pad_w and last end_padding.h * pad_w
  // elements need to be updated to pad_value
  if (beg_padding.h() != 0)
  {
    int end = beg_padding.h() * pad_w;
    for (int i = 0; i < end; i++)
    {
      pad_values[i] = pad_value;
    }
  }
  if (end_padding.h() != 0)
  {
    for (int start = (pad_h - end_padding.h()) * pad_w; 
        start < num_elements; start++)
    {
      pad_values[start] = pad_value;
    }

  }
  // Handle W padding. First beg_padding.w() and last end_padding.w() 
  // elements of each row needs to be updated to pad_value
  if (beg_padding.w() != 0)
  {
    for (int h_dim = 0; h_dim < pad_h; h_dim++)
    {
      for (int w_dim = 0; w_dim < beg_padding.w(); w_dim++)
      {
        int row_base_index = h_dim*pad_h;
        pad_values[row_base_index + w_dim] = pad_value;
      }
    }
  }
  if (end_padding.w() != 0)
  {
    for (int h_dim = 0; h_dim < pad_h; h_dim++)
    {
      for (int w_dim = pad_w - end_padding.w();
          w_dim < pad_w; w_dim++)
      {
        int row_base_index = h_dim*pad_h;
        pad_values[row_base_index + w_dim] = pad_value;
      }
    }
  }



} // namespace onnx2trt


}

