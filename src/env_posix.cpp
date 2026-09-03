#include "env.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace minikv {
namespace {

Status PosixError(const std::string& context, int err_number) {
  return Status::IOError(context + ": " + std::strerror(err_number));
}

class PosixSequentialFile : public SequentialFile {
 public:
  explicit PosixSequentialFile(int fd) : fd_(fd) {}
  ~PosixSequentialFile() override {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  Status Read(size_t n, Slice* result, char* scratch) override {
    ssize_t r;
    do {
      r = ::read(fd_, scratch, n);
    } while (r < 0 && errno == EINTR);
    if (r < 0) {
      return PosixError("read", errno);
    }
    *result = Slice(scratch, static_cast<size_t>(r));
    return Status::OK();
  }

  Status Skip(uint64_t n) override {
    if (::lseek(fd_, static_cast<off_t>(n), SEEK_CUR) < 0) {
      return PosixError("lseek", errno);
    }
    return Status::OK();
  }

 private:
  int fd_;
};

class PosixWritableFile : public WritableFile {
 public:
  explicit PosixWritableFile(int fd) : fd_(fd) {}
  ~PosixWritableFile() override { Close().ok(); }

  Status Append(const Slice& data) override {
    size_t left = data.size();
    const char* src = data.data();
    while (left > 0) {
      const ssize_t written = ::write(fd_, src, left);
      if (written < 0) {
        if (errno == EINTR) {
          continue;
        }
        return PosixError("write", errno);
      }
      left -= static_cast<size_t>(written);
      src += written;
    }
    return Status::OK();
  }

  Status Flush() override {
    // write 已进入内核缓冲；显式 flush 留给 Sync。
    return Status::OK();
  }

  Status Sync() override {
    if (::fsync(fd_) != 0) {
      return PosixError("fsync", errno);
    }
    return Status::OK();
  }

  Status Close() override {
    if (fd_ < 0) {
      return Status::OK();
    }
    const int fd = fd_;
    fd_ = -1;
    if (::close(fd) != 0) {
      return PosixError("close", errno);
    }
    return Status::OK();
  }

 private:
  int fd_;
};

class PosixRandomAccessFile : public RandomAccessFile {
 public:
  explicit PosixRandomAccessFile(int fd) : fd_(fd) {}
  ~PosixRandomAccessFile() override {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  Status Read(uint64_t offset, size_t n, Slice* result,
              char* scratch) const override {
    size_t total = 0;
    while (total < n) {
      ssize_t r;
      do {
        r = ::pread(fd_, scratch + total, n - total,
                    static_cast<off_t>(offset + total));
      } while (r < 0 && errno == EINTR);
      if (r < 0) return PosixError("pread", errno);
      if (r == 0) break;
      total += static_cast<size_t>(r);
    }
    *result = Slice(scratch, total);
    return Status::OK();
  }

 private:
  int fd_;
};

class PosixEnv : public Env {
 public:
  Status NewSequentialFile(const std::string& fname,
                           std::unique_ptr<SequentialFile>* result) override {
    const int fd = ::open(fname.c_str(), O_RDONLY);
    if (fd < 0) {
      return PosixError(fname, errno);
    }
    result->reset(new PosixSequentialFile(fd));
    return Status::OK();
  }

  Status NewRandomAccessFile(
      const std::string& fname,
      std::unique_ptr<RandomAccessFile>* result) override {
    const int fd = ::open(fname.c_str(), O_RDONLY);
    if (fd < 0) {
      return PosixError(fname, errno);
    }
    result->reset(new PosixRandomAccessFile(fd));
    return Status::OK();
  }

  Status NewWritableFile(const std::string& fname,
                         std::unique_ptr<WritableFile>* result) override {
    const int fd =
        ::open(fname.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) {
      return PosixError(fname, errno);
    }
    result->reset(new PosixWritableFile(fd));
    return Status::OK();
  }

  Status NewAppendableFile(const std::string& fname,
                           std::unique_ptr<WritableFile>* result) override {
    const int fd =
        ::open(fname.c_str(), O_CREAT | O_RDWR | O_APPEND, 0644);
    if (fd < 0) {
      return PosixError(fname, errno);
    }
    result->reset(new PosixWritableFile(fd));
    return Status::OK();
  }

  bool FileExists(const std::string& fname) override {
    return ::access(fname.c_str(), F_OK) == 0;
  }

  Status GetFileSize(const std::string& fname, uint64_t* size) override {
    struct stat sbuf {};
    if (::stat(fname.c_str(), &sbuf) != 0) {
      *size = 0;
      return PosixError(fname, errno);
    }
    *size = static_cast<uint64_t>(sbuf.st_size);
    return Status::OK();
  }

  Status RenameFile(const std::string& src,
                    const std::string& target) override {
    if (::rename(src.c_str(), target.c_str()) != 0) {
      return PosixError(src + " -> " + target, errno);
    }
    return Status::OK();
  }

  Status SyncDir(const std::string& dirname) override {
    const int fd = ::open(dirname.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
      return PosixError(dirname, errno);
    }
    const int rc = ::fsync(fd);
    const int saved_errno = errno;
    ::close(fd);
    if (rc != 0) {
      return PosixError("fsync directory " + dirname, saved_errno);
    }
    return Status::OK();
  }

  Status DeleteFile(const std::string& fname) override {
    if (::unlink(fname.c_str()) != 0) {
      return PosixError(fname, errno);
    }
    return Status::OK();
  }

  Status CreateDir(const std::string& name) override {
    if (::mkdir(name.c_str(), 0755) != 0) {
      if (errno != EEXIST) {
        return PosixError(name, errno);
      }
    }
    return Status::OK();
  }

  bool FileIsDirectory(const std::string& name) override {
    struct stat sbuf {};
    if (::stat(name.c_str(), &sbuf) != 0) {
      return false;
    }
    return S_ISDIR(sbuf.st_mode);
  }
};

}  // namespace

Env* Env::Default() {
  static PosixEnv env;
  return &env;
}

}  // namespace minikv
