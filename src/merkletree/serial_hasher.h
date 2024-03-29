#ifndef SERIAL_HASHER_H
#define SERIAL_HASHER_H

#include <openssl/sha.h>
#include <stddef.h>
#include <string>

class SerialHasher {
 public:
  SerialHasher() {}
  virtual ~SerialHasher() {}

  virtual size_t DigestSize() const = 0;

  // Reset the context. Must be called before the first
  // Update() call (and again after each Final() call).
  virtual void Reset() = 0;

  // Update the hash context with (binary) data.
  virtual void Update(const std::string &data) = 0;

  // Finalize the hash context and return the binary digest blob.
  virtual std::string Final() = 0;
};

class Sha256Hasher : public SerialHasher {
 public:
  Sha256Hasher();

  size_t DigestSize() const { return kDigestSize; }

  void Reset();
  void Update(const std::string &data);
  std::string Final();

  // Create a new hasher and call Reset(), Update(), and Final().
  static std::string Sha256Digest(const std::string &data);

 private:
  SHA256_CTX ctx_;
  bool initialized_;
  static const size_t kDigestSize;
};
#endif
