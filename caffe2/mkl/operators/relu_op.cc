#include "caffe2/operators/relu_op.h"
#include "caffe2/utils/mkl_utils.h"

#include "caffe2/utils/math.h"

#ifdef CAFFE2_HAS_MKL_DNN

namespace caffe2 {
namespace mkl {

template <typename T>
class MKLReluOp : public MKLOperator<T> {
 public:
  USE_MKLOPERATOR_FUNCTIONS(T);
  USE_SIMPLE_MKL_CTOR_DTOR(MKLReluOp, T);
  bool RunOnDevice() override {
    auto& X = Input(0);
    auto* Y = Output(0);
    if (input_size_cache_.size() != 1 || input_size_cache_[0] != X.dims()) {
      // First run or changed input size, will need to recreate environment
      primitive_.Reset(dnnReLUCreateForward<T>, nullptr, X.layout(), 0.f);
      Y->Reset(X.dims(), primitive_, dnnResourceDst);
      buffer_.Reset(X.dims(), primitive_, dnnResourceDst, true);
    }
    // Try to share from the output: this will save a copy if the output is
    // already allocated and is having the same layout as the buffer has.
    buffer_.ShareFrom(*Y);
    resources_[dnnResourceSrc] = X.buffer();
    resources_[dnnResourceDst] = buffer_.buffer();
    ExecutePrimitive();
    buffer_.CopyTo(Y, primitive_, dnnResourceDst);
    return true;
  }
};

template <typename T>
class MKLReluGradientOp : public MKLOperator<T> {
 public:
  USE_MKLOPERATOR_FUNCTIONS(T);
  USE_SIMPLE_MKL_CTOR_DTOR(MKLReluGradientOp, T);
  bool RunOnDevice() override {
    auto& Y = Input(0);
    auto& dY = Input(1);
    auto* dX = Output(0);
    if (input_size_cache_.size() != 1 || input_size_cache_[0] != Y.dims()) {
      // First run or changed input size, will need to recreate environment
      primitive_.Reset(
          dnnReLUCreateBackward<T>, nullptr, dY.layout(), Y.layout(), 0.f);
      dX->Reset(Y.dims(), primitive_, dnnResourceDiffSrc);
      buffer_.Reset(Y.dims(), primitive_, dnnResourceDiffSrc, true);
    }
    // Try to share from the output: this will save a copy if the output is
    // already allocated and is having the same layout as the buffer has.
    buffer_.ShareFrom(*dX);
    // MKLDNN seems to use X instead of Y for src, let's see if this works.
    resources_[dnnResourceSrc] = Y.buffer();
    resources_[dnnResourceDiffDst] = dY.buffer();
    resources_[dnnResourceDiffSrc] = buffer_.buffer();
    ExecutePrimitive();
    buffer_.CopyTo(dX);
    return true;
  }
};
} // namespace mkl

REGISTER_MKL_OPERATOR(Relu, mkl::MKLReluOp<float>);
REGISTER_MKL_OPERATOR(ReluGradient, mkl::MKLReluGradientOp<float>);

} // namespace caffe2

#endif // CAFFE2_HAS_MKL_DNN
