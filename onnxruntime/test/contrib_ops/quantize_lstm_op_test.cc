
#include "gtest/gtest.h"

#include <algorithm>
#include <vector>
#include <iostream>

#include "core/mlas/inc/mlas.h"
#include "core/util/qmath.h"
#include "core/providers/cpu/rnn/deep_cpu_lstm.h"
#include "test/common/tensor_op_test_utils.h"
#include "test/util/include/default_providers.h"
#include "test/providers/provider_test_utils.h"

namespace onnxruntime {
namespace test {

template <typename QType,
          typename std::enable_if<is_quant_type<QType>::value, int>::type = 0>
static std::vector<float> ApplyQDQ(const std::vector<float>& data, size_t num_direction) {
  std::vector<float> result(data.size());
  size_t size_per_dir = data.size() / num_direction;

  for (size_t dir_idx = 0; dir_idx < num_direction; dir_idx++) {
    QType zp = 0;
    float scale = 1.0f;
    const float* data_buf = data.data() + size_per_dir * dir_idx;
    GetQuantizationParameter(data_buf, size_per_dir, scale, zp);

    std::vector<QType> quant_data(size_per_dir);
    MlasQuantizeLinear(data_buf, quant_data.data(), size_per_dir, scale, zp);

    std::cout << "ApplyQDQ" << std::endl;
    for (auto itr = quant_data.begin(); itr < quant_data.end(); itr++) {
      std::cout << int(*itr) << ",";
    }
    std::cout << std::endl
              << std::endl;

    std::transform(quant_data.begin(),
                   quant_data.end(),
                   result.begin() + size_per_dir * dir_idx,
                   [&zp, &scale](QType q) {
                     return (static_cast<int32_t>(q) - zp) * scale;
                   });
  }

  return result;
}

template <typename QType,
          typename std::enable_if<is_quant_type<QType>::value, int>::type = 0>
void QuantizeWeight(std::vector<QType>& w_quant,
                    std::vector<float>& scale,
                    std::vector<QType>& zp,
                    const std::vector<float>& w,
                    size_t num_direction,
                    size_t row,
                    size_t col) {
  std::vector<float> w_transpose(w.size());

  for (size_t dir_idx = 0; dir_idx < num_direction; dir_idx++) {
    const float* w_buffer = w.data() + dir_idx * row * col;
    float* w_transpose_buffer = w_transpose.data() + dir_idx * row * col;
    for (size_t r = 0; r < row; r++) {
      for (size_t c = 0; c < col; c++) {
        *(w_transpose_buffer + r + c * row) = *w_buffer++;
      }
    }
  }

  w_quant.resize(w.size());
  scale.resize(num_direction);
  zp.resize(num_direction);

  size_t size_per_dir = row * col;
  for (size_t dir_idx = 0; dir_idx < num_direction; dir_idx++) {
    GetQuantizationParameter(w_transpose.data() + dir_idx * size_per_dir, size_per_dir, scale[dir_idx], zp[dir_idx]);
    MlasQuantizeLinear(w_transpose.data() + dir_idx * size_per_dir,
                       w_quant.data() + dir_idx * size_per_dir,
                       size_per_dir,
                       scale[dir_idx],
                       zp[dir_idx]);
  }

  std::cout << "QuantizeWeight" << std::endl;
  for (auto itr = w_quant.begin(); itr < w_quant.end(); itr++) {
    std::cout << int(*itr) << ",";
  }
  std::cout << std::endl
            << std::endl;
}

template <typename QType,
          typename std::enable_if<is_quant_type<QType>::value, int>::type = 0>
static void ComputeRefOutput(std::vector<float>& Y_data,
                             std::vector<float>& Y_h_data,
                             std::vector<float>& Y_c_data,
                             int64_t input_size,
                             int64_t batch_size,
                             int64_t hidden_size,
                             const std::vector<float>& X_data,
                             const std::vector<float>& W_data,
                             const std::vector<float>& R_data,
                             const std::vector<float>* B_data,
                             const std::vector<float>* P_data,
                             const std::vector<float> initial_h_data,
                             const std::vector<float> initial_c_data,
                             const std::string& direction,
                             const std::vector<std::string>& activations) {
  OpTester test("LSTM", 7 /*opset_version*/, onnxruntime::kOnnxDomain /*domain*/, false /*verify_output*/);

  test.AddAttribute<std::vector<std::string>>("activations", activations);
  test.AddAttribute("direction", direction);
  test.AddAttribute("hidden_size", hidden_size);
  test.AddAttribute<int64_t>("input_forget", 0);

  int64_t seq_length = 1;  // only use seq length 1
  int64_t num_directions = (direction == "bidirectional") ? 2 : 1;
  std::vector<int64_t> X_dims = {seq_length, batch_size, input_size};
  std::vector<int64_t> W_dims = {num_directions, 4 * hidden_size, input_size};
  std::vector<int64_t> R_dims = {num_directions, 4 * hidden_size, hidden_size};

  test.AddInput<float>("X", X_dims, ApplyQDQ<uint8_t>(X_data, 1));
  test.AddInput<float>("W", W_dims, ApplyQDQ<QType>(W_data, num_directions));
  test.AddInput<float>("R", R_dims, ApplyQDQ<QType>(R_data, num_directions));

  if (B_data) {
    std::vector<int64_t> B_dims = {num_directions, 8 * hidden_size};
    test.AddInput<float>("B", B_dims, *B_data);
  } else {
    test.AddMissingOptionalInput<float>();
  }

  // sequence_lens
  test.AddMissingOptionalInput<int>();

  std::vector<int64_t> initial_h_dims = {num_directions, batch_size, hidden_size};
  test.AddInput<float>("initial_h", initial_h_dims, ApplyQDQ<uint8_t>(initial_h_data, num_directions));

  std::vector<int64_t> initial_c_dims = {num_directions, batch_size, hidden_size};
  test.AddInput<float>("initial_c", initial_c_dims, initial_c_data);

  if (P_data && !P_data->empty()) {
    std::vector<int64_t> P_dims = {num_directions, 3 * hidden_size};
    test.AddInput<float>("P", P_dims, *P_data);
  } else {
    test.AddMissingOptionalInput<float>();
  }

  size_t y_data_size = seq_length * num_directions * batch_size * hidden_size;
  Y_data.resize(seq_length * num_directions * batch_size * hidden_size);
  std::vector<int64_t> Y_dims = {seq_length, num_directions, batch_size, hidden_size};
  test.AddOutput<float>("Y", Y_dims, Y_data);

  size_t y_h_data_size = num_directions * batch_size * hidden_size;
  Y_h_data.resize(num_directions * batch_size * hidden_size);
  std::vector<int64_t> Y_h_dims{num_directions, batch_size, hidden_size};
  test.AddOutput<float>("Y_h", Y_h_dims, Y_h_data);

  size_t y_c_data_size = num_directions * batch_size * hidden_size;
  Y_c_data.resize(num_directions * batch_size * hidden_size);
  std::vector<int64_t> Y_c_dims{num_directions, batch_size, hidden_size};
  test.AddOutput<float>("Y_c", Y_c_dims, Y_c_data);

  std::vector<std::unique_ptr<IExecutionProvider>> execution_providers;
  execution_providers.push_back(DefaultCpuExecutionProvider());
  test.Run(OpTester::ExpectResult::kExpectSuccess, "", {}, nullptr, &execution_providers);

  std::vector<MLValue> outputs = test.GetFetches();

  const float* y_buffer = outputs[0].Get<Tensor>().Data<float>();
  std::copy(y_buffer, y_buffer + y_data_size, Y_data.begin());

  const float* y_h_buffer = outputs[1].Get<Tensor>().Data<float>();
  std::copy(y_h_buffer, y_h_buffer + y_h_data_size, Y_h_data.begin());

  const float* y_c_buffer = outputs[2].Get<Tensor>().Data<float>();
  std::copy(y_c_buffer, y_c_buffer + y_c_data_size, Y_c_data.begin());
}

template <typename QType,
          typename std::enable_if<std::is_same<QType, uint8_t>::value || std::is_same<QType, int8_t>::value, int>::type = 0>
static void RunQuantLSTM(int64_t input_size,
                         int64_t batch_size,
                         int64_t hidden_size,
                         bool has_bias,
                         bool has_P,
                         bool is_initializer_W,
                         bool is_initializer_R,
                         const std::string& direction) {
  OpTester test("DynamicQuantizeLSTM", 1 /*opset_version*/, onnxruntime::kMSDomain /*domain*/);

  int num_directions = (direction == "bidirectional") ? 2 : 1;

  std::vector<std::string> activations;
  if (num_directions == 2) {
    activations = {"sigmoid", "tanh", "tanh", "sigmoid", "tanh", "tanh"};
  } else {
    activations = {"sigmoid", "tanh", "tanh"};
  }
  test.AddAttribute<std::vector<std::string>>("activations", activations);

  test.AddAttribute("direction", direction);
  test.AddAttribute("hidden_size", hidden_size);
  test.AddAttribute<int64_t>("input_forget", 0);

  RandomValueGenerator rand_gen;

  // X
  int64_t seq_len = 1;  // only use seq length 1 to model the test
  std::vector<int64_t> X_dims = {seq_len, batch_size, input_size};
  std::vector<float> X_data = rand_gen.Gaussian<float>({seq_len, batch_size, input_size}, 0.0f, 0.25f);
  test.AddInput<float>("X", X_dims, X_data);

  // W
  std::vector<int64_t> W_dims = {num_directions, input_size, 4 * hidden_size};
  std::vector<float> W_data = rand_gen.Gaussian<float>({num_directions, 4 * hidden_size, input_size}, 0.0f, 0.25f);

  std::vector<float> w_scale;
  std::vector<QType> w_zp;
  std::vector<QType> w_quant;
  QuantizeWeight(w_quant, w_scale, w_zp, W_data, num_directions, 4 * hidden_size, input_size);
  test.AddInput<QType>("W", W_dims, w_quant, is_initializer_W);

  // R
  std::vector<int64_t> R_dims = {num_directions, hidden_size, 4 * hidden_size};
  std::vector<float> R_data = rand_gen.Gaussian<float>({num_directions, 4 * hidden_size, hidden_size}, 0.0f, 0.25f);

  std::vector<float> r_scale;
  std::vector<QType> r_zp;
  std::vector<QType> r_quant;
  QuantizeWeight(r_quant, r_scale, r_zp, R_data, num_directions, 4 * hidden_size, hidden_size);
  test.AddInput<QType>("R", R_dims, r_quant, is_initializer_R);

  std::vector<float> B_data;
  if (has_bias) {
    std::vector<int64_t> B_dims = {num_directions, 8 * hidden_size};
    B_data = rand_gen.Gaussian<float>(B_dims, 0.0f, 0.25f);

    test.AddInput<float>("B", B_dims, B_data);
  } else {
    test.AddMissingOptionalInput<float>();
  }

  // sequence_lens
  test.AddMissingOptionalInput<int>();

  // initial_h
  std::vector<int64_t> initial_h_dims = {num_directions, batch_size, hidden_size};
  std::vector<float> initial_h_data = rand_gen.Gaussian<float>(initial_h_dims, 0.0f, 0.25f);
  test.AddInput<float>("initial_h", initial_h_dims, initial_h_data);

  // initial_c
  std::vector<int64_t> initial_c_dims = {num_directions, batch_size, hidden_size};
  std::vector<float> initial_c_data = rand_gen.Gaussian<float>(initial_c_dims, 0.0f, 0.25f);
  test.AddInput<float>("initial_c", initial_c_dims, initial_c_data);

  std::vector<float> P_data;
  if (has_P) {
    std::vector<int64_t> P_dims = {num_directions, 3 * hidden_size};
    P_data = rand_gen.Gaussian<float>(P_dims, 0.0f, 0.25f);
    test.AddInput<float>("P", P_dims, P_data);
  } else {
    test.AddMissingOptionalInput<float>();
  }

  test.AddInput<float>("W_scale", {num_directions}, w_scale);
  test.AddInput<QType>("W_zero_point", {num_directions}, w_zp);

  test.AddInput<float>("R_scale", {num_directions}, r_scale);
  test.AddInput<QType>("R_zero_point", {num_directions}, r_zp);

  std::vector<float> Y_data;
  std::vector<float> Y_h_data;
  std::vector<float> Y_c_data;
  ComputeRefOutput<QType>(Y_data, Y_h_data, Y_c_data,
                          input_size, batch_size, hidden_size,
                          X_data, W_data, R_data,
                          has_bias ? &B_data : nullptr,
                          has_P ? &P_data : nullptr,
                          initial_h_data, initial_c_data,
                          direction, activations);

  std::vector<int64_t> Y_dims = {seq_len, num_directions, batch_size, hidden_size};
  test.AddOutput<float>("Y", Y_dims, Y_data);

  std::vector<int64_t> Y_h_dims{num_directions, batch_size, hidden_size};
  test.AddOutput<float>("Y_h", Y_h_dims, Y_h_data);

  std::vector<int64_t> Y_c_dims{num_directions, batch_size, hidden_size};
  test.AddOutput<float>("Y_c", Y_c_dims, Y_c_data);

  test.Run();
}

template <typename QType,
          typename std::enable_if<std::is_same<QType, uint8_t>::value || std::is_same<QType, int8_t>::value, int>::type = 0>
static void RunQuantLSTM(int64_t input_size,
                         int64_t batch_size,
                         int64_t hidden_size) {
  // bias + P: 0, prepacking: 0, bidirectional: 0
  RunQuantLSTM<QType>(input_size, batch_size, hidden_size,
                      false /*has_bias*/, false /*has_P*/,
                      false /*is_initializer_W*/, false /*is_initializer_R*/,
                      "forward");

  // bias + P: 0, prepacking: 0, bidirectional: 1
  RunQuantLSTM<QType>(input_size, batch_size, hidden_size,
                      false /*has_bias*/, false /*has_P*/,
                      false /*is_initializer_W*/, false /*is_initializer_R*/,
                      "bidirectional");

  // bias + P: 0, prepacking: 1, bidirectional: 0
  RunQuantLSTM<QType>(input_size, batch_size, hidden_size,
                      false /*has_bias*/, false /*has_P*/,
                      true /*is_initializer_W*/, true /*is_initializer_R*/,
                      "forward");

  // bias + P: 0, prepacking: 1, bidirectional: 1
  RunQuantLSTM<QType>(input_size, batch_size, hidden_size,
                      false /*has_bias*/, false /*has_P*/,
                      true /*is_initializer_W*/, true /*is_initializer_R*/,
                      "bidirectional");

  // bias + P: 1, prepacking: 0, bidirectional: 0
  RunQuantLSTM<QType>(input_size, batch_size, hidden_size,
                      true /*has_bias*/, true /*has_P*/,
                      false /*is_initializer_W*/, false /*is_initializer_R*/,
                      "forward");

  // bias + P: 1, prepacking: 0, bidirectional: 1
  RunQuantLSTM<QType>(input_size, batch_size, hidden_size,
                      true /*has_bias*/, true /*has_P*/,
                      false /*is_initializer_W*/, false /*is_initializer_R*/,
                      "bidirectional");

  // bias + P: 1, prepacking: 1, bidirectional: 0
  RunQuantLSTM<QType>(input_size, batch_size, hidden_size,
                      true /*has_bias*/, true /*has_P*/,
                      true /*is_initializer_W*/, true /*is_initializer_R*/,
                      "forward");

  // bias + P: 1, prepacking: 1, bidirectional: 1
  RunQuantLSTM<QType>(input_size, batch_size, hidden_size,
                      true /*has_bias*/, true /*has_P*/,
                      true /*is_initializer_W*/, true /*is_initializer_R*/,
                      "bidirectional");
}

//TEST(DynamicQuantLSTMTest, Input_2_Batch_1_Hidden_2) {
//  RunQuantLSTM<int8_t>(2, 1, 2);
//  RunQuantLSTM<uint8_t>(2, 1, 2);
//}

TEST(DynamicQuantLSTMTest, Input_2_Batch_3_Hidden_2) {
  RunQuantLSTM<int8_t>(2, 3, 2);
  RunQuantLSTM<uint8_t>(2, 3, 2);
}
//
//TEST(DynamicQuantLSTMTest, Input_2_Batch_3_Hidden_2_uint8_t) {
//  RunQuantLSTM<uint8_t>(2, 3, 2);
//}

TEST(DynamicQuantLSTMTest, Input_2_Batch_3_Hidden_2_int8_t) {
  RunQuantLSTM<int8_t>(2, 3, 2,
                       false /*has_bias*/, false /*has_P*/,
                       false /*is_initializer_W*/, false /*is_initializer_R*/,
                       "forward");
}

//TEST(DynamicQuantLSTMTest, Input_2_Batch_3_Hidden_2_uint8_t) {
//  RunQuantLSTM<uint8_t>(2, 3, 2);
//}

//TEST(DynamicQuantLSTMTest, Input_12_Batch_3_Hidden_18)
//{
//    RunQuantLSTM<int8_t>(12, 3, 18);
//    RunQuantLSTM<uint8_t>(12, 3, 18);
//}

// TEST(DynamicQuantLSTMTest, Bidirectional_NoBias_NoP_NoClip) {
//   int batch_size = 2;
//   int64_t input_size = 3;
//   int64_t hidden_size = 2;
//
//   RunQuantLSTM(input_size, batch_size, hidden_size, false, false, false, "bidirectional");
// }
//
// TEST(DynamicQuantLSTMTest, Bidirectional_Bias_P_NoClip) {
//   int batch_size = 2;
//   int64_t input_size = 3;
//   int64_t hidden_size = 2;
//
//   RunQuantLSTM(input_size, batch_size, hidden_size, true, true, false, "bidirectional");
// }
//
// TEST(DynamicQuantLSTMTest, Bidirectional_Bias_P_Clip) {
//   int batch_size = 4;
//   int64_t input_size = 16;
//   int64_t hidden_size = 18;
//
//   RunQuantLSTM(input_size, batch_size, hidden_size, true, true, false, "bidirectional");
// }

}  // namespace test
}  // namespace onnxruntime