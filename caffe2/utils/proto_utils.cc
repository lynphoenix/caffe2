#include "caffe2/utils/proto_utils.h"

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <fstream>

#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/io/zero_copy_stream_impl.h"

#ifndef CAFFE2_USE_LITE_PROTO
#include "google/protobuf/text_format.h"
#endif  // !CAFFE2_USE_LITE_PROTO

#include "caffe2/core/logging.h"

using ::google::protobuf::Message;
using ::google::protobuf::MessageLite;

namespace caffe2 {

bool ReadStringFromFile(const char* filename, string* str) {
  std::ifstream ifs(filename, std::ios::in);
  if (!ifs) {
    VLOG(1) << "File cannot be opened: " << filename
            << " error: " << ifs.rdstate();
    return false;
  }
  ifs.seekg(0, std::ios::end);
  size_t n = ifs.tellg();
  str->resize(n);
  ifs.seekg(0);
  ifs.read(&(*str)[0], n);
  return true;
}

bool WriteStringToFile(const string& str, const char* filename) {
  std::ofstream ofs(filename, std::ios::out | std::ios::trunc);
  if (!ofs.is_open()) {
    VLOG(1) << "File cannot be created: " << filename
            << " error: " << ofs.rdstate();
    return false;
  }
  ofs << str;
  return true;
}

// IO-specific proto functions: we will deal with the protocol buffer lite and
// full versions differently.

#ifdef CAFFE2_USE_LITE_PROTO

// Lite runtime.

namespace {
class IfstreamInputStream : public ::google::protobuf::io::CopyingInputStream {
 public:
  explicit IfstreamInputStream(const string& filename)
      : ifs_(filename.c_str(), std::ios::in | std::ios::binary) {}
  ~IfstreamInputStream() { ifs_.close(); }

  int Read(void* buffer, int size) {
    if (!ifs_) {
      return -1;
    }
    ifs_.read(static_cast<char*>(buffer), size);
    return ifs_.gcount();
  }

 private:
  std::ifstream ifs_;
};
}  // namespace

bool ReadProtoFromBinaryFile(const char* filename, MessageLite* proto) {
  ::google::protobuf::io::CopyingInputStreamAdaptor stream(
      new IfstreamInputStream(filename));
  stream.SetOwnsCopyingStream(true);
  // Total bytes hard limit / warning limit are set to 1GB and 512MB
  // respectively.
  ::google::protobuf::io::CodedInputStream coded_stream(&stream);
  coded_stream.SetTotalBytesLimit(1024LL << 20, 512LL << 20);
  return proto->ParseFromCodedStream(&coded_stream);
}

void WriteProtoToBinaryFile(const MessageLite& proto, const char* filename) {
  LOG(FATAL) << "Not implemented yet.";
}

#else  // CAFFE2_USE_LITE_PROTO

// Full protocol buffer.

using ::google::protobuf::io::FileInputStream;
using ::google::protobuf::io::FileOutputStream;
using ::google::protobuf::io::ZeroCopyInputStream;
using ::google::protobuf::io::CodedInputStream;
using ::google::protobuf::io::ZeroCopyOutputStream;
using ::google::protobuf::io::CodedOutputStream;

bool ReadProtoFromTextFile(const char* filename, Message* proto) {
  int fd = open(filename, O_RDONLY);
  CAFFE_ENFORCE_NE(fd, -1, "File not found: ", filename);
  FileInputStream* input = new FileInputStream(fd);
  bool success = google::protobuf::TextFormat::Parse(input, proto);
  delete input;
  close(fd);
  return success;
}

void WriteProtoToTextFile(const Message& proto, const char* filename) {
  int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  FileOutputStream* output = new FileOutputStream(fd);
  CAFFE_ENFORCE(google::protobuf::TextFormat::Print(proto, output));
  delete output;
  close(fd);
}

bool ReadProtoFromBinaryFile(const char* filename, MessageLite* proto) {
  int fd = open(filename, O_RDONLY);
  CAFFE_ENFORCE_NE(fd, -1, "File not found: ", filename);
  std::unique_ptr<ZeroCopyInputStream> raw_input(new FileInputStream(fd));
  std::unique_ptr<CodedInputStream> coded_input(
      new CodedInputStream(raw_input.get()));
  // A hack to manually allow using very large protocol buffers.
  coded_input->SetTotalBytesLimit(1073741824, 536870912);
  bool success = proto->ParseFromCodedStream(coded_input.get());
  coded_input.reset();
  raw_input.reset();
  close(fd);
  return success;
}

void WriteProtoToBinaryFile(const MessageLite& proto, const char* filename) {
  int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  CAFFE_ENFORCE_NE(
      fd, -1, "File cannot be created: ", filename, " error number: ", errno);
  std::unique_ptr<ZeroCopyOutputStream> raw_output(new FileOutputStream(fd));
  std::unique_ptr<CodedOutputStream> coded_output(
      new CodedOutputStream(raw_output.get()));
  CAFFE_ENFORCE(proto.SerializeToCodedStream(coded_output.get()));
  coded_output.reset();
  raw_output.reset();
  close(fd);
}

#endif  // CAFFE2_USE_LITE_PROTO

ArgumentHelper::ArgumentHelper(const OperatorDef& def) {
  for (auto& arg : def.arg()) {
    if (arg_map_.count(arg.name())) {
      if (arg.SerializeAsString() !=
          arg_map_[arg.name()]->SerializeAsString()) {
        // If there are two arguments of the same name but different contents,
        // we will throw an error.
        CAFFE_THROW(
            "Found argument of the same name ",
            arg.name(),
            "but with different contents.",
            ProtoDebugString(def));
      } else {
        LOG(WARNING) << "Duplicated argument name found in operator def: "
                     << ProtoDebugString(def);
      }
    }
    arg_map_[arg.name()] = &arg;
  }
}

bool ArgumentHelper::HasArgument(const string& name) const {
  return arg_map_.count(name);
}

#define INSTANTIATE_GET_SINGLE_ARGUMENT(T, fieldname)                         \
  template <>                                                                 \
  T ArgumentHelper::GetSingleArgument<T>(                                     \
      const string& name, const T& default_value) const {                     \
    if (arg_map_.count(name) == 0) {                                          \
      VLOG(1) << "Using default parameter value " << default_value            \
              << " for parameter " << name;                                   \
      return default_value;                                                   \
    }                                                                         \
    CAFFE_ENFORCE(                                                            \
        arg_map_.at(name)->has_##fieldname(),                                 \
        "Argument ",                                                          \
        name,                                                                 \
        " does not have the right field: expected field " #fieldname);        \
    return arg_map_.at(name)->fieldname();                                    \
  }                                                                           \
  template <>                                                                 \
  bool ArgumentHelper::HasSingleArgumentOfType<T>(const string& name) const { \
    if (arg_map_.count(name) == 0) {                                          \
      return false;                                                           \
    }                                                                         \
    return arg_map_.at(name)->has_##fieldname();                              \
  }

INSTANTIATE_GET_SINGLE_ARGUMENT(float, f)
INSTANTIATE_GET_SINGLE_ARGUMENT(int, i)
INSTANTIATE_GET_SINGLE_ARGUMENT(bool, i)
INSTANTIATE_GET_SINGLE_ARGUMENT(int64_t, i)
INSTANTIATE_GET_SINGLE_ARGUMENT(size_t, i)
INSTANTIATE_GET_SINGLE_ARGUMENT(string, s)
#undef INSTANTIATE_GET_SINGLE_ARGUMENT

#define INSTANTIATE_GET_REPEATED_ARGUMENT(T, fieldname)                        \
  template <>                                                                  \
  vector<T> ArgumentHelper::GetRepeatedArgument<T>(const string& name) const { \
    if (arg_map_.count(name) == 0) {                                           \
      return vector<T>();                                                      \
    }                                                                          \
    vector<T> values;                                                          \
    for (const auto& v : arg_map_.at(name)->fieldname())                       \
      values.push_back(v);                                                     \
    return values;                                                             \
  }

INSTANTIATE_GET_REPEATED_ARGUMENT(float, floats)
INSTANTIATE_GET_REPEATED_ARGUMENT(int, ints)
INSTANTIATE_GET_REPEATED_ARGUMENT(bool, ints)
INSTANTIATE_GET_REPEATED_ARGUMENT(int64_t, ints)
INSTANTIATE_GET_REPEATED_ARGUMENT(size_t, ints)
INSTANTIATE_GET_REPEATED_ARGUMENT(string, strings)
#undef INSTANTIATE_GET_REPEATED_ARGUMENT

#define CAFFE2_MAKE_SINGULAR_ARGUMENT(T, fieldname)                            \
template <>                                                                    \
Argument MakeArgument(const string& name, const T& value) {                    \
  Argument arg;                                                                \
  arg.set_name(name);                                                          \
  arg.set_##fieldname(value);                                                  \
  return arg;                                                                  \
}

CAFFE2_MAKE_SINGULAR_ARGUMENT(bool, i)
CAFFE2_MAKE_SINGULAR_ARGUMENT(float, f)
CAFFE2_MAKE_SINGULAR_ARGUMENT(int, i)
CAFFE2_MAKE_SINGULAR_ARGUMENT(int64_t, i)
CAFFE2_MAKE_SINGULAR_ARGUMENT(string, s)
#undef CAFFE2_MAKE_SINGULAR_ARGUMENT

template <>
Argument MakeArgument(const string& name, const MessageLite& value) {
  Argument arg;
  arg.set_name(name);
  arg.set_s(value.SerializeAsString());
  return arg;
}

#define CAFFE2_MAKE_REPEATED_ARGUMENT(T, fieldname)                            \
template <>                                                                    \
Argument MakeArgument(const string& name, const vector<T>& value) {            \
  Argument arg;                                                                \
  arg.set_name(name);                                                          \
  for (const auto& v : value) {                                                \
    arg.add_##fieldname(v);                                                    \
  }                                                                            \
  return arg;                                                                  \
}

CAFFE2_MAKE_REPEATED_ARGUMENT(float, floats)
CAFFE2_MAKE_REPEATED_ARGUMENT(int, ints)
CAFFE2_MAKE_REPEATED_ARGUMENT(int64_t, ints)
CAFFE2_MAKE_REPEATED_ARGUMENT(string, strings)
#undef CAFFE2_MAKE_REPEATED_ARGUMENT

const Argument& GetArgument(const OperatorDef& def, const string& name) {
  for (const Argument& arg : def.arg()) {
    if (arg.name() == name) {
      return arg;
    }
  }
  LOG(FATAL) << "Argument named " << name << " does not exist.";
  // To suppress compiler warning of return values. This will never execute.
  static Argument _dummy_arg_to_suppress_compiler_warning;
  return _dummy_arg_to_suppress_compiler_warning;
}

Argument* GetMutableArgument(
    const string& name, const bool create_if_missing, OperatorDef* def) {
  for (int i = 0; i < def->arg_size(); ++i) {
    if (def->arg(i).name() == name) {
      return def->mutable_arg(i);
    }
  }
  // If no argument of the right name is found...
  if (create_if_missing) {
    Argument* arg = def->add_arg();
    arg->set_name(name);
    return arg;
  } else {
    return nullptr;
  }
}

}  // namespace caffe2
