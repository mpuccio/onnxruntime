// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include <string>
#include <string.h>
#include <sstream>
#include <stdint.h>
#include <assert.h>
#include <stdexcept>
#include <setjmp.h>
#include <algorithm>
#include <vector>
#include <memory>
#include <atomic>

#include "providers.h"
#include "local_filesystem.h"
#include <glib.h>

#include <onnxruntime/core/session/onnxruntime_c_api.h>
#include "image_loader.h"
#include "AsyncRingBuffer.h"
#include <fstream>
#include <condition_variable>

static std::vector<std::string> readFileToVec(const std::string& file_path, size_t expected_line_count) {
  std::ifstream ifs(file_path);
  if (!ifs) {
    throw std::runtime_error("open file failed");
  }
  std::string line;
  std::vector<std::string> labels;
  while (std::getline(ifs, line)) {
    if (!line.empty()) labels.push_back(line);
  }
  if (labels.size() != expected_line_count) {
    std::ostringstream oss;
    oss << "line count mismatch, expect " << expected_line_count << " from " << file_path << ", got " << labels.size();
    throw std::runtime_error(oss.str());
  }
  return labels;
}

static int ExtractImageNumberFromFileName(const TCharString& image_file) {
  size_t s = image_file.rfind('.');
  if (s == std::string::npos) throw std::runtime_error("illegal filename");
  size_t s2 = image_file.rfind('_');
  if (s2 == std::string::npos) throw std::runtime_error("illegal filename");

  const char* start_ptr = image_file.c_str() + s2 + 1;
  const char* endptr = nullptr;
  long value = strtol(start_ptr, (char**)&endptr, 10);
  if (start_ptr == endptr || value > INT32_MAX || value <= 0) throw std::runtime_error("illegal filename");
  return static_cast<int>(value);
}

static void verify_input_output_count(OrtSession* session) {
  size_t count;
  ORT_THROW_ON_ERROR(OrtSessionGetInputCount(session, &count));
  assert(count == 1);
  ORT_THROW_ON_ERROR(OrtSessionGetOutputCount(session, &count));
  assert(count == 1);
}

void thread_pool_dispatcher(void* data, void* user_data) { (*(RunnableTask*)data)(); }

class Validator : public OutputCollector {
 private:
  OrtSession* session_ = nullptr;
  const int output_class_count_ = 1001;
  std::vector<TCharString> image_file_paths_;
  std::vector<std::string> labels_;
  std::vector<std::string> validation_data_;
  std::atomic<int> top_1_correct_count_;
  std::atomic<int> finished_count_;
  int image_size_;

  std::mutex m_;
  std::condition_variable cond_var_;
  std::atomic<int> refcount_;

 public:
  int GetImageSize() const { return image_size_; }

  int IncRef() override { return refcount_.fetch_add(1); }

  ~Validator() { OrtReleaseSession(session_); }

  void FinishAndDecRef(const char* errmsg) override {
    if (errmsg != nullptr) fprintf(stderr, "%s\n", errmsg);
    if (--refcount_ == 0) {
      cond_var_.notify_all();
    }
  }

  void Wait() {
    {
      std::unique_lock<std::mutex> l(m_);
      while (refcount_ != 0) cond_var_.wait(l);
    }
    printf("Top-1 Accuracy %f\n", ((float)top_1_correct_count_.load() / image_file_paths_.size()));
  }

  Validator(OrtEnv* env, const std::vector<TCharString>& image_file_paths, const TCharString& model_path,
            const TCharString& label_file_path, const TCharString& validation_file_path)
      : image_file_paths_(image_file_paths),
        labels_(readFileToVec(label_file_path, 1000)),
        validation_data_(readFileToVec(validation_file_path, image_file_paths_.size())),
        top_1_correct_count_(0),
        finished_count_(0),
        refcount_(1) {
    OrtSessionOptions* session_option;
    ORT_THROW_ON_ERROR(OrtCreateSessionOptions(&session_option));
#ifdef USE_CUDA
    ORT_THROW_ON_ERROR(OrtSessionOptionsAppendExecutionProvider_CUDA(session_option, 0));
#endif
    ORT_THROW_ON_ERROR(OrtCreateSession(env, model_path.c_str(), session_option, &session_));
    OrtReleaseSessionOptions(session_option);
    verify_input_output_count(session_);

    OrtTypeInfo* info;
    ORT_THROW_ON_ERROR(OrtSessionGetInputTypeInfo(session_, 0, &info));
    const OrtTensorTypeAndShapeInfo* tensor_info;
    ORT_THROW_ON_ERROR(OrtCastTypeInfoToTensorInfo(info, &tensor_info));
    size_t dim_count;
    ORT_THROW_ON_ERROR(OrtGetDimensionsCount(tensor_info, &dim_count));
    assert(dim_count == 4);
    std::vector<int64_t> dims(dim_count);
    ORT_THROW_ON_ERROR(OrtGetDimensions(tensor_info, dims.data(), dims.size()));
    if (dims[1] != dims[2] || dims[3] != 3) {
      throw std::runtime_error("This model is not supported by this program. input tensor need be in NHWC format");
    }

    image_size_ = static_cast<int>(dims[1]);
  }

  void operator()(const std::vector<int>& task_id_list, const OrtValue* input_tensor) override {
    const size_t remain = task_id_list.size();
    const char* input_name = "input:0";
    const char* output_name = "InceptionV4/Logits/Predictions:0";
    OrtValue* output_tensor = nullptr;
    ORT_THROW_ON_ERROR(OrtRun(session_, nullptr, &input_name, &input_tensor, 1, &output_name, 1, &output_tensor));
    float* probs;
    ORT_THROW_ON_ERROR(OrtGetTensorMutableData(output_tensor, (void**)&probs));
    for (size_t i = 0; i != remain; ++i) {
      float* end = probs + output_class_count_;
      float* max_p = std::max_element(probs + 1, end);
      auto max_prob_index = std::distance(probs, max_p);
      assert(max_prob_index >= 1);
      // TODO:extract number from filename, to index validation_data
      int taskid = task_id_list[i];
      const auto& s = image_file_paths_[taskid];
      int test_data_id = ExtractImageNumberFromFileName(s);
      assert(test_data_id > 1);
      // printf("%d\n",(int)max_prob_index);
      // printf("%s\n",labels[max_prob_index - 1].c_str());
      // printf("%s\n",validation_data[test_data_id - 1].c_str());
      if (labels_[max_prob_index - 1] == validation_data_[test_data_id - 1]) {
        ++top_1_correct_count_;
      }
      probs = end;
    }
    finished_count_ += remain;
    printf("%d\n", finished_count_.load());
    OrtReleaseValue(output_tensor);
    if (--refcount_ == 0) {
      cond_var_.notify_all();
    }
  }
};

int real_main(int argc, ORTCHAR_T* argv[]) {
  if (argc < 5) return -1;
  std::vector<TCharString> image_file_paths;
  TCharString data_dir = argv[1];
  TCharString model_path = argv[2];
  // imagenet_lsvrc_2015_synsets.txt
  TCharString label_file_path = argv[3];
  TCharString validation_file_path = argv[4];
  const int batch_size = 64;

  // TODO: remove the slash at the end of data_dir string
  LoopDir(data_dir, [&data_dir, &image_file_paths](const ORTCHAR_T* filename, OrtFileType filetype) -> bool {
    if (filetype != OrtFileType::TYPE_REG) return true;
    if (filename[0] == '.') return true;
    const char* p = strrchr(filename, '.');
    if (p == nullptr) return true;
    // as we tested filename[0] is not '.', p should larger than filename
    assert(p > filename);
    if (strcasecmp(p, ".JPEG") != 0 && strcasecmp(p, ".JPG") != 0) return true;
    TCharString v(data_dir);
#ifdef _WIN32
    v.append(1, '\\');
#else
    v.append(1, '/');
#endif
    v.append(filename);
    image_file_paths.emplace_back(v);
    return true;
  });

  std::vector<uint8_t> data;
  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "Default");

  GError* err = NULL;
  GThreadPool* threadpool = g_thread_pool_new(thread_pool_dispatcher, nullptr, 8, TRUE, &err);
  if (err != NULL) {
    fprintf(stderr, "Unable to create thread pool: %s\n", err->message);
    g_error_free(err);
    return -1;
  }
  assert(threadpool != nullptr);

  Validator v(env, image_file_paths, model_path, label_file_path, validation_file_path);

  int image_size = v.GetImageSize();
  const int channels = 3;
  std::atomic<int> finished(0);

  InceptionPreprocessing prepro(image_size, image_size, channels);

  AsyncRingBuffer buffer(batch_size, 160, threadpool, image_file_paths, &prepro, &v);
  buffer.StartDownloadTasks();
  v.Wait();

  return 0;
}

int main(int argc, ORTCHAR_T* argv[]) {
  try {
    return real_main(argc, argv);
  } catch (const std::exception& ex) {
    fprintf(stderr, "%s\n", ex.what());
    return -1;
  }
}