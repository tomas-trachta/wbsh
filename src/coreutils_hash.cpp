/**
 * @file coreutils_hash.cpp
 * @brief Bundled cryptographic-hash builtins (md5sum, sha1sum, sha256sum,
 *        sha512sum).
 *
 * Computes file hashes using BCrypt on Windows. On other platforms the
 * builtins emit "hash failed" since we don't ship our own SHA / MD5
 * implementations.
 *
 * Split out of coreutils.cpp to keep the bcrypt dependency localised.
 * The single public entry point is registerHashBuiltins().
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

#ifdef _WIN32
		// Hash the contents of `fp` with the algorithm identified by `algId`
		// (one of BCRYPT_MD5_ALGORITHM, BCRYPT_SHA*_ALGORITHM). Writes the
		// lowercase hex digest into `hex_out`. Returns false on any BCrypt
		// failure.
		static bool computeHashHex(LPCWSTR algId, FILE* fp, std::string& hex_out) {
			BCRYPT_ALG_HANDLE hAlg = nullptr;
			if (BCryptOpenAlgorithmProvider(&hAlg, algId, nullptr, 0) != 0)
				return false;
			DWORD hashLen = 0;
			DWORD dummy = 0;
			BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH,
				reinterpret_cast<PUCHAR>(&hashLen), sizeof(hashLen), &dummy, 0);

			BCRYPT_HASH_HANDLE hHash = nullptr;
			if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) != 0) {
				BCryptCloseAlgorithmProvider(hAlg, 0);
				return false;
			}
			unsigned char buf[8192];
			while (true) {
				const std::size_t n = std::fread(buf, 1, sizeof(buf), fp);
				if (n == 0) break;
				BCryptHashData(hHash, buf, static_cast<ULONG>(n), 0);
			}
			std::vector<unsigned char> bytes(hashLen);
			BCryptFinishHash(hHash, bytes.data(), hashLen, 0);
			BCryptDestroyHash(hHash);
			BCryptCloseAlgorithmProvider(hAlg, 0);

			char hex[3];
			hex_out.reserve(hashLen * 2);
			for (DWORD i = 0; i < hashLen; ++i) {
				std::snprintf(hex, sizeof(hex), "%02x", bytes[i]);
				hex_out += hex;
			}
			return true;
		}
#endif  // _WIN32

#ifdef _WIN32
		using HashAlgId = LPCWSTR;
#else
		using HashAlgId = const wchar_t*;   // unused
#endif

		static int hashImpl(Executor& exec, const std::vector<std::string>& args,
		                    HashAlgId algId, const char* cmd)
		{
			std::vector<std::string> files;
			for (const auto& a : args) {
				if (!a.empty() && a[0] == '-' && a != "-") continue;
				files.push_back(a);
			}
			if (files.empty()) files.push_back("-");

			int rc = 0;
			for (const auto& f : files) {
				FILE* fp = (f == "-") ? stdin
					: openUtf8(exec.pathConv().toWin32(f), "rb");
				if (!fp) {
					std::fprintf(stderr, "wbsh: %s: %s: %s\n",
						cmd, f.c_str(), std::strerror(errno));
					rc = 1;
					continue;
				}
				std::string hex;
#ifdef _WIN32
				const bool ok = computeHashHex(algId, fp, hex);
#else
				const bool ok = false;
				(void)algId;
#endif
				if (fp != stdin) std::fclose(fp);
				if (!ok) {
					std::fprintf(stderr, "wbsh: %s: hash failed\n", cmd);
					rc = 1;
					continue;
				}
				std::printf("%s  %s\n", hex.c_str(), f.c_str());
			}
			std::fflush(stdout);
			return rc;
		}

#ifdef _WIN32
		static int builtin_md5sum(Executor& e, const std::vector<std::string>& a) {
			return hashImpl(e, a, BCRYPT_MD5_ALGORITHM, "md5sum");
		}
		static int builtin_sha1sum(Executor& e, const std::vector<std::string>& a) {
			return hashImpl(e, a, BCRYPT_SHA1_ALGORITHM, "sha1sum");
		}
		static int builtin_sha256sum(Executor& e, const std::vector<std::string>& a) {
			return hashImpl(e, a, BCRYPT_SHA256_ALGORITHM, "sha256sum");
		}
		static int builtin_sha512sum(Executor& e, const std::vector<std::string>& a) {
			return hashImpl(e, a, BCRYPT_SHA512_ALGORITHM, "sha512sum");
		}
#else
		static int builtin_md5sum(Executor& e, const std::vector<std::string>& a) {
			return hashImpl(e, a, nullptr, "md5sum");
		}
		static int builtin_sha1sum(Executor& e, const std::vector<std::string>& a) {
			return hashImpl(e, a, nullptr, "sha1sum");
		}
		static int builtin_sha256sum(Executor& e, const std::vector<std::string>& a) {
			return hashImpl(e, a, nullptr, "sha256sum");
		}
		static int builtin_sha512sum(Executor& e, const std::vector<std::string>& a) {
			return hashImpl(e, a, nullptr, "sha512sum");
		}
#endif

	}  // namespace hash_detail

	void registerHashBuiltins(Executor& exec) {
		exec.registerBuiltin("md5sum",    hash_detail::builtin_md5sum);
		exec.registerBuiltin("sha1sum",   hash_detail::builtin_sha1sum);
		exec.registerBuiltin("sha256sum", hash_detail::builtin_sha256sum);
		exec.registerBuiltin("sha512sum", hash_detail::builtin_sha512sum);
	}

}  // namespace wbsh
