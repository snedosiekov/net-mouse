#pragma once

// Cryptography for the protected transport (Monocypher underneath):
//   * identity: an X25519 key pair per PC, stored in a key file only that PC can read;
//   * pair key: static X25519 between a master and a slave, so only those two can talk;
//   * session key: pair key mixed with fresh random nonces from both sides (no replay across sessions);
//   * packets: XChaCha20-Poly1305 with a random 24-byte nonce per packet.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace nm::crypto {

using Key = std::array<uint8_t, 32>;
using Nonce16 = std::array<uint8_t, 16>;
using Mac = std::array<uint8_t, 16>;
constexpr size_t kNonceSize = 24;
constexpr size_t kMacSize = 16;

// Operating system CSPRNG (BCryptGenRandom / getrandom). Aborts if it fails.
void randomBytes(void* out, size_t size);

template <typename T>
T randomArray() {
  T value;
  randomBytes(value.data(), value.size());
  return value;
}

struct Identity {
  Key secret{};
  Key publicKey{};
};

Identity generateIdentity();
// Reads the secret key (hex) from `path`, or creates a new one there (mode 0600 on POSIX).
bool loadOrCreateIdentity(const std::filesystem::path& path, Identity& out, std::string& error);

// Long-term key of one master/slave pair: BLAKE2b(X25519(own secret, peer public) || master pub || slave pub).
Key pairKey(const Identity& own, const Key& peerPublic, bool ownIsMaster);
// Per-connection key: BLAKE2b keyed with the pair key over both sides' fresh nonces.
Key sessionKey(const Key& pairKey, const Nonce16& masterNonce, const Nonce16& slaveNonce);
// Short public identifier of a key (never 0), used to tell sessions apart.
uint64_t keyId(const Key& key);
// Human-comparable form of a public key, e.g. "3F9A-12C4-77D0-B81E".
std::string fingerprint(const Key& publicKey);

Mac mac(const Key& key, const uint8_t* data, size_t size);
bool macEquals(const Mac& a, const Mac& b);  // constant time

// XChaCha20-Poly1305. `ad` is authenticated but not encrypted.
void seal(const Key& key, const uint8_t nonce[kNonceSize], const uint8_t* ad, size_t adSize, const uint8_t* plain,
          size_t size, uint8_t* cipherOut, uint8_t macOut[kMacSize]);
bool open(const Key& key, const uint8_t nonce[kNonceSize], const uint8_t* ad, size_t adSize, const uint8_t* cipher,
          size_t size, const uint8_t mac[kMacSize], uint8_t* plainOut);

std::string toHex(const uint8_t* data, size_t size);
bool fromHex(const std::string& hex, uint8_t* out, size_t size);

}  // namespace nm::crypto
