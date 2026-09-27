/**
 * @file coreutils_hash.cpp
 * @brief Bundled cryptographic-hash builtins (md5sum, sha1sum, sha256sum,
 *        sha512sum).
 *
 * Computes file hashes using BCrypt on Windows. On other platforms the
 * builtins emit "hash failed" since we don't ship our own SHA / MD5
 * implementations.
 */

#include "coreutils_internal.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "executor.h"
#include "pathconv.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <bcrypt.h>
#  pragma comment(lib, "bcrypt.lib")
#endif

namespace wbsh {

	namespace hash_detail {

		static const std::size_t kHashChunk = 8192;

#ifdef _WIN32
		using HashAlgId = LPCWSTR;

		static const HashAlgId kMd5Algorithm    = BCRYPT_MD5_ALGORITHM;
		static const HashAlgId kSha1Algorithm   = BCRYPT_SHA1_ALGORITHM;
		static const HashAlgId kSha256Algorithm = BCRYPT_SHA256_ALGORITHM;
		static const HashAlgId kSha512Algorithm = BCRYPT_SHA512_ALGORITHM;

		static DWORD hashLength(BCRYPT_ALG_HANDLE algorithm) {
			DWORD length = 0;
			DWORD written = 0;
			::BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
				reinterpret_cast<PUCHAR>(&length), sizeof(length), &written, 0);
			return length;
		}

		static void hashStream(BCRYPT_HASH_HANDLE hash, std::FILE* stream) {
			unsigned char chunk[kHashChunk];
			for (;;) {
				const std::size_t got = std::fread(chunk, 1, sizeof(chunk), stream);
				if (got == 0) break;
				::BCryptHashData(hash, chunk, static_cast<ULONG>(got), 0);
			}
		}

		static std::string toHex(const std::vector<unsigned char>& bytes) {
			std::string hex;
			hex.reserve(bytes.size() * 2);
			char pair[3];
			for (const unsigned char byte : bytes) {
				std::snprintf(pair, sizeof(pair), "%02x", byte);
				hex += pair;
			}

			return hex;
		}

		static bool computeHashHex(HashAlgId algorithm_id, std::FILE* stream,
		                           std::string& hex_out) {
			BCRYPT_ALG_HANDLE algorithm = nullptr;
			if (::BCryptOpenAlgorithmProvider(&algorithm, algorithm_id, nullptr, 0) != 0) {
				return false;
			}

			const DWORD length = hashLength(algorithm);
			BCRYPT_HASH_HANDLE hash = nullptr;
			if (::BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) != 0) {
				::BCryptCloseAlgorithmProvider(algorithm, 0);
				return false;
			}

			hashStream(hash, stream);

			std::vector<unsigned char> digest(length);
			::BCryptFinishHash(hash, digest.data(), length, 0);
			::BCryptDestroyHash(hash);
			::BCryptCloseAlgorithmProvider(algorithm, 0);

			hex_out = toHex(digest);
			return true;
		}
#else
		using HashAlgId = const wchar_t*;

		static const HashAlgId kMd5Algorithm    = nullptr;
		static const HashAlgId kSha1Algorithm   = nullptr;
		static const HashAlgId kSha256Algorithm = nullptr;
		static const HashAlgId kSha512Algorithm = nullptr;

		static bool computeHashHex(HashAlgId, std::FILE*, std::string&) {
			return false;
		}
#endif

		static std::vector<std::string> hashOperands(const std::vector<std::string>& args) {
			std::vector<std::string> files;
			for (const auto& arg : args) {
				if (!arg.empty() && arg[0] == '-' && arg != "-") continue;
				files.push_back(arg);
			}

			if (files.empty()) files.push_back("-");
			return files;
		}

		static bool hashOneFile(Executor& exec, const std::string& file, HashAlgId algorithm_id,
		                        const char* cmd) {
			std::FILE* stream = (file == "-") ? stdin : fopenNative(exec, file, "rb");
			if (stream == nullptr) {
				perr(cmd, file + ": " + std::strerror(errno));
				return false;
			}

			std::string hex;
			const bool ok = computeHashHex(algorithm_id, stream, hex);
			if (stream != stdin) std::fclose(stream);
			if (!ok) {
				perr(cmd, "hash failed");
				return false;
			}

			std::printf("%s  %s\n", hex.c_str(), file.c_str());
			return true;
		}

		static int hashImpl(Executor& exec, const std::vector<std::string>& args,
		                    HashAlgId algorithm_id, const char* cmd) {
			int status = 0;
			for (const auto& file : hashOperands(args)) {
				if (!hashOneFile(exec, file, algorithm_id, cmd)) status = 1;
			}

			std::fflush(stdout);
			return status;
		}

		static int builtin_md5sum(Executor& exec, const std::vector<std::string>& args) {
			return hashImpl(exec, args, kMd5Algorithm, "md5sum");
		}

		static int builtin_sha1sum(Executor& exec, const std::vector<std::string>& args) {
			return hashImpl(exec, args, kSha1Algorithm, "sha1sum");
		}

		static int builtin_sha256sum(Executor& exec, const std::vector<std::string>& args) {
			return hashImpl(exec, args, kSha256Algorithm, "sha256sum");
		}

		static int builtin_sha512sum(Executor& exec, const std::vector<std::string>& args) {
			return hashImpl(exec, args, kSha512Algorithm, "sha512sum");
		}

	}  // namespace hash_detail

	void registerHashBuiltins(Executor& exec) {
		exec.registerBuiltin("md5sum",    hash_detail::builtin_md5sum);
		exec.registerBuiltin("sha1sum",   hash_detail::builtin_sha1sum);
		exec.registerBuiltin("sha256sum", hash_detail::builtin_sha256sum);
		exec.registerBuiltin("sha512sum", hash_detail::builtin_sha512sum);
	}

}  // namespace wbsh
