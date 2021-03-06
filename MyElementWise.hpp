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
#include <NvInfer.h>

#include "plugin.hpp"
#include "serialize.hpp"


#include <thrust/device_vector.h>
#include <cassert>

namespace {
    constexpr const char* MYELEMENTWISE_PLUGIN_VERSION{"001"};
    constexpr const char* MYELEMENTWISE_PLUGIN_NAME{"MyElementWise"};
}
enum class MyElementWiseType : int {
    Equal,
    Less,
    Where,
    Not,
    Add,
    Mul,
    Pow,
    MAX_VALUE //no mean
};
class MyElementWisePlugin final : public onnx2trt::PluginV2 {
private:
  MyElementWiseType _type;
  unsigned long long _numbers;
  nvinfer1::Dims output_dims;
  //int _nx, _ny, _nz;
  //int _x_stride, _y_stride, _z_stride;
  //thrust::device_vector<int> _d_segment_offsets;
  //thrust::device_vector<float*> _d_output_ptrs;

  int rank;
  //输入1 2 3 ， 输出1。共四个。
  thrust::device_vector<int> dims_a;
  thrust::device_vector<int> dims_a_mul;
  thrust::device_vector<int> dims_b;
  thrust::device_vector<int> dims_b_mul;
  thrust::device_vector<int> dims_c;
  thrust::device_vector<int> dims_c_mul;
  thrust::device_vector<int> dims_y;
  thrust::device_vector<int> dims_y_mul;


  
    thrust::host_vector<int> h_dims_a;
    thrust::host_vector<int> h_dims_a_mul  ;
    thrust::host_vector<int> h_dims_b  ;
    thrust::host_vector<int> h_dims_b_mul ;
    thrust::host_vector<int> h_dims_c ;
    thrust::host_vector<int> h_dims_c_mul ;
    thrust::host_vector<int> h_dims_y ;
    thrust::host_vector<int> h_dims_y_mul  ;

        
protected:
  void deserialize(void const* serialData, size_t serialLength) {
    deserializeBase(serialData, serialLength);
    deserialize_value(&serialData, &serialLength, &_type);
     deserialize_value(&serialData, &serialLength, &output_dims);
  }
  virtual size_t getSerializationSize() const override {
    return serialized_size(_type) + serialized_size(output_dims) +getBaseSerializationSize();
  }
  virtual void serialize(void *buffer) const override {
    serializeBase(buffer);
    serialize_value(&buffer, _type);
    serialize_value(&buffer, output_dims);
  }
public:
  MyElementWisePlugin(MyElementWiseType op)
    : _type(op) {
    assert(op <= MyElementWiseType::MAX_VALUE);
  }
  MyElementWisePlugin(void const* serialData, size_t serialLength) {
    this->deserialize(serialData, serialLength);
  }
  virtual const char* getPluginType() const override { return MYELEMENTWISE_PLUGIN_NAME; }

  virtual void destroy() override { delete this; }

  virtual nvinfer1::IPluginV2* clone() const override { return new MyElementWisePlugin{_type}; }

  virtual const char* getPluginVersion() const override { return MYELEMENTWISE_PLUGIN_VERSION; }

  virtual void setPluginNamespace(const char* pluginNamespace) override {}

  virtual const char* getPluginNamespace() const override { return ""; }
  //sds:The number of the output tensor. 分割成几分，就是介个tensor.
  virtual int getNbOutputs() const override { return 1; };
  virtual nvinfer1::Dims getOutputDimensions(int index,
                                             const nvinfer1::Dims *inputs, int nbInputDims) override;
  virtual int initialize() override;
  virtual int enqueue(int batchSize,
                      const void *const *inputs, void **outputs,
                      void *workspace, cudaStream_t stream) override;
};

//sds:最新的添加plugin的方式。
class MyElementWisePluginCreator : public nvinfer1::IPluginCreator
{
public:
  MyElementWisePluginCreator() {}

  ~MyElementWisePluginCreator() {}

  const char* getPluginName() const { return MYELEMENTWISE_PLUGIN_NAME; }

  const char* getPluginVersion() const { return MYELEMENTWISE_PLUGIN_VERSION; }

  const nvinfer1::PluginFieldCollection* getFieldNames() { std::cerr<< "Function not implemented" << std::endl; return nullptr; }
  //不支持
  nvinfer1::IPluginV2* createPlugin(const char* name, const nvinfer1::PluginFieldCollection* fc) { std::cerr<< "Function not implemented" << std::endl; return nullptr; }
  //sds:支持反序列化(新版本的添加plugin方式。如果是老版本的，需要在pluginFactory中注册deserialize)。
  nvinfer1::IPluginV2* deserializePlugin(const char* name, const void* serialData, size_t serialLength) { return new MyElementWisePlugin{serialData, serialLength}; }

  void setPluginNamespace(const char* libNamespace) { mNamespace = libNamespace; }

  const char* getPluginNamespace() const { return mNamespace.c_str(); }
private:
    std::string mNamespace;
};

REGISTER_TENSORRT_PLUGIN(MyElementWisePluginCreator);
