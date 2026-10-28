/**
 * @file filemagic_exec.cpp
 * @brief Executables for `file`: PE (machine, subsystem, DLL, .NET,
 *        sections), MS-DOS, ELF, Mach-O (thin and universal), Java class
 *        files and WebAssembly modules.
 */

#include "filemagic.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wbsh {

	using magic_bytes::be32;
	using magic_bytes::has;
	using magic_bytes::le16;
	using magic_bytes::le32;
	using magic_bytes::le64;
	using magic_bytes::matches;

	namespace exec_detail {

		struct CodeName {
			std::uint32_t code;
			const char* name;
		};

		struct ElfLayout {
			bool wide = false;
			bool little = true;
			std::uint64_t ph_offset = 0;
			std::uint32_t ph_size = 0;
			std::uint32_t ph_count = 0;
			std::uint64_t sh_offset = 0;
			std::uint32_t sh_size = 0;
			std::uint32_t sh_count = 0;
		};

		struct ElfSegments {
			bool dynamic = false;
			bool readable = false;
			std::string interpreter;
		};

	}  // namespace exec_detail

	using exec_detail::CodeName;
	using exec_detail::ElfLayout;
	using exec_detail::ElfSegments;

	static const CodeName kPeMachines[] = {
		{ 0x014c, "Intel 80386" }, { 0x8664, "x86-64" }, { 0x01c0, "ARM" },
		{ 0x01c4, "ARMv7 Thumb" }, { 0xaa64, "Aarch64" }, { 0x0200, "Intel Itanium" },
		{ 0x01c2, "ARM Thumb" }, { 0x5064, "RISC-V 64-bit" }, { 0x5032, "RISC-V 32-bit" },
	};

	static const CodeName kPeSubsystems[] = {
		{ 1, "native" }, { 2, "GUI" }, { 3, "console" }, { 5, "OS/2 CUI" }, { 7, "POSIX CUI" },
		{ 9, "Windows CE GUI" }, { 10, "EFI application" }, { 11, "EFI boot service driver" },
		{ 12, "EFI runtime driver" }, { 13, "EFI ROM" }, { 14, "XBOX" },
		{ 16, "Windows boot application" },
	};

	static const CodeName kElfMachines[] = {
		{ 2, "SPARC" }, { 3, "Intel 80386" }, { 8, "MIPS" }, { 20, "PowerPC" },
		{ 21, "64-bit PowerPC or cisco 7500" }, { 22, "IBM S/390" }, { 40, "ARM" },
		{ 43, "SPARC V9" }, { 50, "IA-64" }, { 62, "x86-64" }, { 183, "ARM aarch64" },
		{ 243, "UCB RISC-V" }, { 258, "LoongArch" },
	};

	static const CodeName kElfAbis[] = {
		{ 0, "SYSV" }, { 1, "HP-UX" }, { 2, "NetBSD" }, { 3, "GNU/Linux" }, { 6, "Solaris" },
		{ 9, "FreeBSD" }, { 12, "OpenBSD" },
	};

	static const CodeName kMachCpus[] = {
		{ 7, "i386" }, { 0x01000007, "x86_64" }, { 12, "arm" }, { 0x0100000c, "arm64" },
		{ 18, "ppc" }, { 0x01000012, "ppc64" },
	};

	static const CodeName kMachFileTypes[] = {
		{ 1, "object" }, { 2, "executable" }, { 3, "fixed virtual memory shared library" },
		{ 4, "core" }, { 5, "preload executable" }, { 6, "dynamically linked shared library" },
		{ 7, "dynamic linker" }, { 8, "bundle" }, { 9, "dynamically linked shared library stub" },
		{ 10, "dSYM companion file" }, { 11, "kext bundle" },
	};

	static const CodeName kMachFlags[] = {
		{ 0x1, "NOUNDEFS" }, { 0x2, "INCRLINK" }, { 0x4, "DYLDLINK" }, { 0x8, "BINDATLOAD" },
		{ 0x10, "PREBOUND" }, { 0x20, "SPLIT_SEGS" }, { 0x40, "LAZY_INIT" }, { 0x80, "TWOLEVEL" },
		{ 0x100, "FORCE_FLAT" }, { 0x200, "NOMULTIDEFS" }, { 0x400, "NOFIXPREBINDING" },
		{ 0x800, "PREBINDABLE" }, { 0x1000, "ALLMODSBOUND" },
		{ 0x2000, "SUBSECTIONS_VIA_SYMBOLS" }, { 0x4000, "CANONICAL" },
		{ 0x8000, "WEAK_DEFINES" }, { 0x10000, "BINDS_TO_WEAK" },
		{ 0x20000, "ALLOW_STACK_EXECUTION" }, { 0x40000, "ROOT_SAFE" },
		{ 0x80000, "SETUID_SAFE" }, { 0x100000, "NO_REEXPORTED_DYLIBS" }, { 0x200000, "PIE" },
		{ 0x400000, "DEAD_STRIPPABLE_DYLIB" }, { 0x800000, "HAS_TLV_DESCRIPTORS" },
		{ 0x1000000, "NO_HEAP_EXECUTION" }, { 0x2000000, "APP_EXTENSION_SAFE" },
	};

	static const std::uint32_t kPeDllFlag = 0x2000;
	static const std::uint32_t kPe32Magic = 0x10b;
	static const std::uint32_t kPe32PlusMagic = 0x20b;
	static const std::uint32_t kClrDirectory = 14;
	static const std::uint32_t kElfDynamic = 2;
	static const std::uint32_t kElfInterpreter = 3;
	static const std::uint32_t kElfSymbolTable = 2;
	static const std::uint32_t kMaxFatArchitectures = 20;
	static const std::uint32_t kFirstJavaMajor = 45;

	template <std::size_t N>
	static const char* nameFor(const CodeName (&table)[N], std::uint32_t code) {
		for (const CodeName& entry : table) {
			if (entry.code == code) return entry.name;
		}

		return nullptr;
	}

	static std::string hexText(std::uint32_t value) {
		static const char kDigits[] = "0123456789abcdef";
		std::string text;
		do {
			text.insert(text.begin(), kDigits[value & 0xf]);
			value >>= 4;
		} while (value != 0);
		return "0x" + text;
	}

	static bool hasClrHeader(const FileSample& sample, std::size_t optional, bool plus) {
		const std::size_t count_at = optional + (plus ? 108 : 92);
		const std::size_t directories = optional + (plus ? 112 : 96);
		if (!has(sample, count_at, 4)) return false;
		if (le32(&sample.bytes[count_at]) <= kClrDirectory) return false;

		const std::size_t clr = directories + kClrDirectory * 8;
		return has(sample, clr, 8) && le32(&sample.bytes[clr]) != 0;
	}

	static std::string peSubsystemText(std::uint32_t subsystem) {
		const char* name = nameFor(kPeSubsystems, subsystem);
		if (name == nullptr) return " (Unknown subsystem " + hexText(subsystem) + ")";
		return std::string(" (") + name + ")";
	}

	static std::string peMachineText(std::uint32_t machine) {
		const char* name = nameFor(kPeMachines, machine);
		if (name == nullptr) return "Unknown processor type " + hexText(machine);
		return name;
	}

	static bool describePe(const FileSample& sample, std::size_t pe, MagicResult& result) {
		const std::size_t optional = pe + 24;
		if (!has(sample, optional, 70)) return false;

		const std::uint8_t* coff = &sample.bytes[pe + 4];
		const std::uint32_t magic = le16(&sample.bytes[optional]);
		if (magic != kPe32Magic && magic != kPe32PlusMagic) return false;

		const bool plus = magic == kPe32PlusMagic;
		std::string text = plus ? "PE32+ executable" : "PE32 executable";
		if ((le16(coff + 18) & kPeDllFlag) != 0) text += " (DLL)";
		text += peSubsystemText(le16(&sample.bytes[optional + 68]));
		text += " " + peMachineText(le16(coff));
		if (hasClrHeader(sample, optional, plus)) text += " Mono/.Net assembly";
		text += ", for MS Windows";

		const std::uint32_t sections = le16(coff + 2);
		text += ", " + std::to_string(sections) + (sections == 1 ? " section" : " sections");
		result.description = text;
		result.mime_type = "application/vnd.microsoft.portable-executable";
		return true;
	}

	static bool identifyMz(const FileSample& sample, MagicResult& result) {
		if (!matches(sample, 0, "MZ", 2) || !has(sample, 0x3c, 4)) return false;

		const std::size_t pe = le32(&sample.bytes[0x3c]);
		if (matches(sample, pe, "PE\0\0", 4) && describePe(sample, pe, result)) return true;

		result.description = "MS-DOS executable";
		result.mime_type = "application/x-dosexec";
		return true;
	}

	static std::uint64_t elfWord(const FileSample& sample, std::size_t at, bool little, bool wide) {
		const std::uint8_t* p = &sample.bytes[at];
		if (wide) return little ? le64(p) : magic_bytes::be64(p);
		return little ? le32(p) : be32(p);
	}

	static std::uint32_t elfHalf(const std::uint8_t* p, bool little) {
		return little ? le16(p) : magic_bytes::be16(p);
	}

	static std::uint32_t elfInt(const std::uint8_t* p, bool little) {
		return little ? le32(p) : be32(p);
	}

	static std::uint64_t elfLong(const std::uint8_t* p, bool little) {
		return little ? le64(p) : magic_bytes::be64(p);
	}

	static ElfLayout readElfLayout(const FileSample& sample) {
		ElfLayout layout;
		layout.wide = sample.bytes[4] == 2;
		layout.little = sample.bytes[5] != 2;
		const bool w = layout.wide;
		const bool l = layout.little;
		layout.ph_offset = elfWord(sample, w ? 32 : 28, l, w);
		layout.sh_offset = elfWord(sample, w ? 40 : 32, l, w);
		layout.ph_size = elfHalf(&sample.bytes[w ? 54 : 42], l);
		layout.ph_count = elfHalf(&sample.bytes[w ? 56 : 44], l);
		layout.sh_size = elfHalf(&sample.bytes[w ? 58 : 46], l);
		layout.sh_count = elfHalf(&sample.bytes[w ? 60 : 48], l);
		return layout;
	}

	static std::string readInterpreter(const FileSample& sample, const ElfLayout& layout,
	                                   const std::uint8_t* header) {
		const bool l = layout.little;
		const std::uint64_t offset = layout.wide ? elfLong(header + 8, l) : elfInt(header + 4, l);
		const std::uint64_t size = layout.wide ? elfLong(header + 32, l) : elfInt(header + 16, l);
		std::vector<std::uint8_t> text;
		if (size == 0 || size > 4096) return std::string();
		const std::size_t length = static_cast<std::size_t>(size);
		if (!readSampleAt(sample, offset, length, text)) return std::string();

		std::string path(text.begin(), text.end());
		const std::size_t nul = path.find('\0');
		if (nul != std::string::npos) path.resize(nul);
		return path;
	}

	static ElfSegments readElfSegments(const FileSample& sample, const ElfLayout& layout) {
		ElfSegments segments;
		std::vector<std::uint8_t> table;
		const std::size_t length = static_cast<std::size_t>(layout.ph_size) * layout.ph_count;
		if (layout.ph_size < 32 || !readSampleAt(sample, layout.ph_offset, length, table)) {
			return segments;
		}

		segments.readable = true;
		for (std::uint32_t k = 0; k < layout.ph_count; ++k) {
			const std::uint8_t* header = &table[static_cast<std::size_t>(k) * layout.ph_size];
			const std::uint32_t type = elfInt(header, layout.little);
			if (type == kElfDynamic) segments.dynamic = true;
			if (type == kElfInterpreter) {
				segments.interpreter = readInterpreter(sample, layout, header);
			}
		}

		return segments;
	}

	static const char* elfStripState(const FileSample& sample, const ElfLayout& layout) {
		std::vector<std::uint8_t> table;
		const std::size_t length = static_cast<std::size_t>(layout.sh_size) * layout.sh_count;
		if (layout.sh_count == 0 || layout.sh_size < 8) return nullptr;
		if (!readSampleAt(sample, layout.sh_offset, length, table)) return nullptr;

		for (std::uint32_t k = 0; k < layout.sh_count; ++k) {
			const std::uint8_t* header = &table[static_cast<std::size_t>(k) * layout.sh_size];
			if (elfInt(header + 4, layout.little) == kElfSymbolTable) return "not stripped";
		}

		return "stripped";
	}

	static std::string elfTypeText(std::uint32_t type, const ElfSegments& segments) {
		switch (type) {
			case 1:
				return "relocatable";
			case 2:
				return "executable";
			case 3:
				return segments.interpreter.empty() ? "shared object" : "pie executable";
			case 4:
				return "core file";
			default:
				return "unknown type " + hexText(type);
		}
	}

	static const char* elfMime(std::uint32_t type, const ElfSegments& segments) {
		switch (type) {
			case 1:
				return "application/x-object";
			case 2:
				return "application/x-executable";
			case 3:
				return segments.interpreter.empty() ? "application/x-sharedlib"
					: "application/x-pie-executable";
			case 4:
				return "application/x-coredump";
			default:
				return "application/octet-stream";
		}
	}

	static std::string elfLinkage(std::uint32_t type, const ElfSegments& segments) {
		if (type != 2 && type != 3) return std::string();
		if (!segments.readable) return std::string();

		std::string text = segments.dynamic ? ", dynamically linked" : ", statically linked";
		if (!segments.interpreter.empty()) text += ", interpreter " + segments.interpreter;
		return text;
	}

	static bool identifyElf(const FileSample& sample, MagicResult& result) {
		if (!matches(sample, 0, "\x7f" "ELF", 4) || !has(sample, 0, 64)) return false;

		const ElfLayout layout = readElfLayout(sample);
		const std::uint32_t type = elfHalf(&sample.bytes[16], layout.little);
		const std::uint32_t machine = elfHalf(&sample.bytes[18], layout.little);
		const ElfSegments segments = readElfSegments(sample, layout);
		const char* machine_name = nameFor(kElfMachines, machine);
		const char* abi = nameFor(kElfAbis, sample.bytes[7]);

		std::string text = std::string("ELF ") + (layout.wide ? "64-bit " : "32-bit ")
			+ (layout.little ? "LSB " : "MSB ") + elfTypeText(type, segments);
		text += ", " + (machine_name != nullptr ? std::string(machine_name)
			: "unknown arch " + hexText(machine));
		text += ", version " + std::to_string(sample.bytes[6]);
		if (abi != nullptr) text += std::string(" (") + abi + ")";
		text += elfLinkage(type, segments);

		const char* strip = elfStripState(sample, layout);
		if (strip != nullptr) text += std::string(", ") + strip;
		result.description = text;
		result.mime_type = elfMime(type, segments);
		return true;
	}

	static std::string machFlagsText(std::uint32_t flags) {
		std::string text;
		for (const CodeName& flag : kMachFlags) {
			if ((flags & flag.code) == 0) continue;
			text += (text.empty() ? "" : "|") + std::string(flag.name);
		}

		return text.empty() ? std::string() : ", flags:<" + text + ">";
	}

	static bool describeMachHeader(const std::uint8_t* header, std::size_t available,
	                               std::string& text) {
		if (available < 28) return false;

		const std::uint32_t magic = be32(header);
		const bool little = magic == 0xcefaedfe || magic == 0xcffaedfe;
		const bool wide = magic == 0xfeedfacf || magic == 0xcffaedfe;
		if (!little && magic != 0xfeedface && !wide) return false;

		const std::uint32_t cpu = little ? le32(header + 4) : be32(header + 4);
		const std::uint32_t type = little ? le32(header + 12) : be32(header + 12);
		const std::uint32_t flags = little ? le32(header + 24) : be32(header + 24);
		const char* cpu_name = nameFor(kMachCpus, cpu);
		const char* type_name = nameFor(kMachFileTypes, type);
		text = std::string("Mach-O ") + (wide ? "64-bit " : "")
			+ (cpu_name != nullptr ? cpu_name : hexText(cpu).c_str()) + " "
			+ (type_name != nullptr ? type_name : "unknown file type") + machFlagsText(flags);
		return true;
	}

	static bool identifyThinMach(const FileSample& sample, MagicResult& result) {
		std::string text;
		if (sample.bytes.empty()) return false;
		if (!describeMachHeader(sample.bytes.data(), sample.bytes.size(), text)) return false;

		result.description = text;
		result.mime_type = "application/x-mach-binary";
		return true;
	}

	static std::string fatSliceText(const FileSample& sample, const std::uint8_t* arch) {
		const char* cpu_name = nameFor(kMachCpus, be32(arch));
		const std::string cpu = cpu_name != nullptr ? std::string(cpu_name) : hexText(be32(arch));
		std::string text = "[" + cpu;
		std::vector<std::uint8_t> header;
		std::string slice;
		const bool readable = readSampleAt(sample, be32(arch + 8), 28, header);
		if (readable && describeMachHeader(header.data(), header.size(), slice)) {
			text += ":" + slice;
		}

		return text + "]";
	}

	static bool identifyUniversalMach(const FileSample& sample, std::uint32_t count,
	                                  MagicResult& result) {
		if (!has(sample, 8, static_cast<std::size_t>(count) * 20)) return false;

		std::string text = "Mach-O universal binary with " + std::to_string(count)
			+ (count == 1 ? " architecture:" : " architectures:");
		for (std::uint32_t k = 0; k < count; ++k) {
			text += " " + fatSliceText(sample, &sample.bytes[8 + static_cast<std::size_t>(k) * 20]);
		}

		result.description = text;
		result.mime_type = "application/x-mach-binary";
		return true;
	}

	static std::string javaVersionName(std::uint32_t major) {
		if (major <= 52) return "Java 1." + std::to_string(major - 44);
		return "Java SE " + std::to_string(major - 44);
	}

	static bool identifyCafeBabe(const FileSample& sample, MagicResult& result) {
		if (!matches(sample, 0, "\xca\xfe\xba\xbe", 4) || !has(sample, 0, 8)) return false;

		const std::uint32_t count = be32(&sample.bytes[4]);
		if (count > 0 && count < kMaxFatArchitectures) {
			return identifyUniversalMach(sample, count, result);
		}

		const std::uint32_t minor = magic_bytes::be16(&sample.bytes[4]);
		const std::uint32_t major = magic_bytes::be16(&sample.bytes[6]);
		if (major < kFirstJavaMajor) return false;

		result.description = "compiled Java class data, version " + std::to_string(major) + "."
			+ std::to_string(minor) + " (" + javaVersionName(major) + ")";
		result.mime_type = "application/x-java-applet";
		return true;
	}

	static bool identifyWasm(const FileSample& sample, MagicResult& result) {
		if (!matches(sample, 0, "\0asm", 4) || !has(sample, 0, 8)) return false;

		const std::uint32_t version = le32(&sample.bytes[4]);
		result.description = "WebAssembly (wasm) binary module version " + hexText(version);
		if (version == 1) result.description += " (MVP)";
		result.mime_type = "application/wasm";
		return true;
	}

	bool identifyExecutable(const FileSample& sample, MagicResult& result) {
		return identifyMz(sample, result) || identifyElf(sample, result)
			|| identifyThinMach(sample, result) || identifyCafeBabe(sample, result)
			|| identifyWasm(sample, result);
	}

}  // namespace wbsh
