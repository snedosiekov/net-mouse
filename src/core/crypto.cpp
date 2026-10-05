#include "core/crypto.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <sys/random.h>
#include <unistd.h>
#endif

#include "monocypher.h"

namespace nm::crypto {

void randomBytes(void* out, size_t size) {
#ifdef _WIN32
  if (BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) !=
      0) {
    std::abort();  // no randomness means no security; never continue with predictable keys
  }
#else
  auto* p = static_cast<uint8_t*>(out);
  while (size > 0) {
    const ssize_t n = getrandom(p, size, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      std::abort();
    }
    p += n;
    size -= static_cast<size_t>(n);
  }
#endif
}

Identity generateIdentity() {
  Identity id;
  randomBytes(id.secret.data(), id.secret.size());
  crypto_x25519_public_key(id.publicKey.data(), id.secret.data());
  return id;
}

bool loadOrCreateIdentity(const std::filesystem::path& path, Identity& out, std::string& error) {
  {
    std::ifstream in(path);
    std::string hex;
    if (in && std::getline(in, hex)) {
      while (!hex.empty() && (hex.back() == '\r' || hex.back() == ' ')) hex.pop_back();
      if (!fromHex(hex, out.secret.data(), out.secret.size())) {
        error = "corrupt identity key file " + path.string();
        return false;
      }
      crypto_x25519_public_key(out.publicKey.data(), out.secret.data());
      return true;
    }
  }
  std::error_code ec;
  if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
  out = generateIdentity();
  const std::string hex = toHex(out.secret.data(), out.secret.size()) + "\n";
#ifdef _WIN32
  // %APPDATA% is private to the user already.
  std::ofstream file(path, std::ios::trunc);
  if (!(file << hex)) {
    error = "cannot write " + path.string();
    return false;
  }
#else
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0 || ::write(fd, hex.data(), hex.size()) != static_cast<ssize_t>(hex.size())) {
    error = "cannot write " + path.string() + ": " + std::strerror(errno);
    if (fd >= 0) ::close(fd);
    return false;
  }
  ::close(fd);
#endif
  return true;
}

Key pairKey(const Identity& own, const Key& peerPublic, bool ownIsMaster) {
  uint8_t material[96];
  crypto_x25519(material, own.secret.data(), peerPublic.data());
  const Key& masterPub = ownIsMaster ? own.publicKey : peerPublic;
  const Key& slavePub = ownIsMaster ? peerPublic : own.publicKey;
  std::memcpy(material + 32, masterPub.data(), 32);
  std::memcpy(material + 64, slavePub.data(), 32);
  Key key;
  crypto_blake2b(key.data(), key.size(), material, sizeof(material));
  crypto_wipe(material, sizeof(material));
  return key;
}

Key sessionKey(const Key& pairKey, const Nonce16& masterNonce, const Nonce16& slaveNonce) {
  static constexpr char kLabel[] = "net-mouse session v1";
  uint8_t material[sizeof(kLabel) + 32];
  std::memcpy(material, kLabel, sizeof(kLabel));
  std::memcpy(material + sizeof(kLabel), masterNonce.data(), 16);
  std::memcpy(material + sizeof(kLabel) + 16, slaveNonce.data(), 16);
  Key key;
  crypto_blake2b_keyed(key.data(), key.size(), pairKey.data(), pairKey.size(), material, sizeof(material));
  return key;
}

uint64_t keyId(const Key& key) {
  uint8_t hash[8];
  crypto_blake2b(hash, sizeof(hash), key.data(), key.size());
  uint64_t id = 0;
  for (uint8_t b : hash) id = (id << 8) | b;
  return id ? id : 1;
}

std::string fingerprint(const Key& publicKey) {
  uint8_t hash[8];
  crypto_blake2b(hash, sizeof(hash), publicKey.data(), publicKey.size());
  std::string hex = toHex(hash, sizeof(hash));
  for (char& c : hex) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return hex.substr(0, 4) + "-" + hex.substr(4, 4) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4);
}

Mac mac(const Key& key, const uint8_t* data, size_t size) {
  Mac m;
  crypto_blake2b_keyed(m.data(), m.size(), key.data(), key.size(), data, size);
  return m;
}

bool macEquals(const Mac& a, const Mac& b) { return crypto_verify16(a.data(), b.data()) == 0; }

void seal(const Key& key, const uint8_t nonce[kNonceSize], const uint8_t* ad, size_t adSize, const uint8_t* plain,
          size_t size, uint8_t* cipherOut, uint8_t macOut[kMacSize]) {
  crypto_aead_lock(cipherOut, macOut, key.data(), nonce, ad, adSize, plain, size);
}

bool open(const Key& key, const uint8_t nonce[kNonceSize], const uint8_t* ad, size_t adSize, const uint8_t* cipher,
          size_t size, const uint8_t mac[kMacSize], uint8_t* plainOut) {
  return crypto_aead_unlock(plainOut, mac, key.data(), nonce, ad, adSize, cipher, size) == 0;
}

std::string toHex(const uint8_t* data, size_t size) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string s;
  s.reserve(size * 2);
  for (size_t i = 0; i < size; ++i) {
    s += kDigits[data[i] >> 4];
    s += kDigits[data[i] & 15];
  }
  return s;
}

bool fromHex(const std::string& hex, uint8_t* out, size_t size) {
  if (hex.size() != size * 2) return false;
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < size; ++i) {
    const int hi = nibble(hex[2 * i]);
    const int lo = nibble(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<uint8_t>(hi << 4 | lo);
  }
  return true;
}

}  // namespace nm::crypto
