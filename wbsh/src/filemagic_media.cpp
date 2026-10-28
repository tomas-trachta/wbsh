/**
 * @file filemagic_media.cpp
 * @brief Images, audio and video for `file`: PNG, GIF, JPEG, BMP, ICO,
 *        TIFF, RIFF (WebP, WAVE, AVI), Ogg, FLAC, MP3, ISO media and
 *        Matroska, with the dimensions and stream parameters file(1) shows.
 */

#include "filemagic.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace wbsh {

	using magic_bytes::be16;
	using magic_bytes::be32;
	using magic_bytes::has;
	using magic_bytes::le16;
	using magic_bytes::le32;
	using magic_bytes::matches;

	namespace media_detail {

		struct BrandName {
			const char* brand;
			const char* description;
			const char* mime;
		};

		struct Mp3Frame {
			std::uint32_t version = 0;
			std::uint32_t layer = 0;
			std::uint32_t bitrate_index = 0;
			std::uint32_t rate_index = 0;
			std::uint32_t channel_mode = 0;
		};

	}  // namespace media_detail

	using media_detail::BrandName;
	using media_detail::Mp3Frame;

	static const BrandName kIsoBrands[] = {
		{ "isom", "MP4 Base Media v1 [ISO 14496-12:2003]", "video/mp4" },
		{ "iso2", "MP4 Base Media v2 [ISO 14496-12:2005]", "video/mp4" },
		{ "mp41", "MP4 v1 [ISO 14496-1:ch13]", "video/mp4" },
		{ "mp42", "MP4 v2 [ISO 14496-14]", "video/mp4" },
		{ "M4A ", "Apple iTunes ALAC/AAC-LC (.M4A) Audio", "audio/x-m4a" },
		{ "M4V ", "Apple iTunes Video (.M4V) Video", "video/x-m4v" },
		{ "qt  ", "Apple QuickTime movie, Apple QuickTime (.MOV/QT)", "video/quicktime" },
		{ "3gp4", "MPEG v4 system, 3GPP", "video/3gpp" },
		{ "3gp5", "MPEG v4 system, 3GPP", "video/3gpp" },
		{ "3gp6", "MPEG v4 system, 3GPP Release 6", "video/3gpp" },
		{ "dash", "MPEG v4 system, Dynamic Adaptive Streaming over HTTP", "video/mp4" },
		{ "heic", "HEIF Image HEVC Main or Main Still Picture Profile", "image/heic" },
		{ "mif1", "HEIF Image", "image/heif" },
		{ "avif", "AVIF Image", "image/avif" },
	};

	static const char* const kPngColorTypes[] = {
		"grayscale", "", "/color RGB", "colormap", "gray+alpha", "", "/color RGBA",
	};

	static const std::uint32_t kMp3BitratesV1[3][16] = {
		{ 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0 },
		{ 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0 },
		{ 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 },
	};

	static const std::uint32_t kMp3BitratesV2[3][16] = {
		{ 0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0 },
		{ 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 },
		{ 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 },
	};

	static const std::uint32_t kMp3Rates[3] = { 44100, 48000, 32000 };

	static const char* const kMp3Channels[] = { "Stereo", "JntStereo", "2x Monaural", "Monaural" };

	static const char* const kMp3Versions[] = { "v2.5", "", "v2", "v1" };

	static const char* const kMp3Layers[] = { "", "III", "II", "I" };

	static std::string dimensions(std::uint32_t width, std::uint32_t height, const char* glue) {
		return std::to_string(width) + glue + std::to_string(height);
	}

	static void setResult(MagicResult& result, const std::string& description, const char* mime) {
		result.description = description;
		result.mime_type = mime;
	}

	static bool identifyPng(const FileSample& sample, MagicResult& result) {
		if (!matches(sample, 0, "\x89PNG\r\n\x1a\n", 8)) return false;
		if (!has(sample, 16, 13) || !matches(sample, 12, "IHDR", 4)) {
			setResult(result, "PNG image data", "image/png");
			return true;
		}

		const std::uint8_t* header = &sample.bytes[16];
		const std::uint32_t color = header[9];
		const char* color_text = color < 7 ? kPngColorTypes[color] : "";
		std::string text = "PNG image data, " + dimensions(be32(header), be32(header + 4), " x ")
			+ ", " + std::to_string(header[8]) + "-bit";
		text += color_text[0] == '/' ? color_text : std::string(" ") + color_text;
		text += header[12] != 0 ? ", interlaced" : ", non-interlaced";
		setResult(result, text, "image/png");
		return true;
	}

	static bool identifyGif(const FileSample& sample, MagicResult& result) {
		const bool gif = matches(sample, 0, "GIF87a", 6) || matches(sample, 0, "GIF89a", 6);
		if (!gif || !has(sample, 6, 4)) return false;

		const std::string version(sample.bytes.begin() + 3, sample.bytes.begin() + 6);
		const std::string size = dimensions(le16(&sample.bytes[6]), le16(&sample.bytes[8]), " x ");
		setResult(result, "GIF image data, version " + version + ", " + size, "image/gif");
		return true;
	}

	static std::string jfifText(const std::uint8_t* segment, std::uint32_t length) {
		if (length < 16 || std::memcmp(segment + 4, "JFIF\0", 5) != 0) return std::string();

		static const char* const kUnits[] = {
			"aspect ratio", "resolution (DPI)", "resolution (DPCM)",
		};
		const std::uint32_t units = segment[11];
		std::string text = ", JFIF standard " + std::to_string(segment[9]) + "."
			+ (segment[10] < 10 ? "0" : "") + std::to_string(segment[10]);
		text += std::string(", ") + (units < 3 ? kUnits[units] : "unknown units");
		text += ", density " + dimensions(be16(segment + 12), be16(segment + 14), "x");
		return text + ", segment length " + std::to_string(length);
	}

	static std::string frameText(const std::uint8_t* segment, std::uint32_t marker) {
		static const char* const kProcesses[] = {
			"baseline", "extended sequential", "progressive", "lossless",
		};
		std::string text = std::string(", ") + kProcesses[(marker - 0xc0) & 3];
		text += ", precision " + std::to_string(segment[4]);
		text += ", " + dimensions(be16(segment + 7), be16(segment + 5), "x");
		return text + ", components " + std::to_string(segment[9]);
	}

	static bool isFrameMarker(std::uint32_t marker) {
		return marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8
			&& marker != 0xcc;
	}

	static std::string jpegDetails(const FileSample& sample) {
		std::string text;
		std::size_t at = 2;
		while (has(sample, at, 4) && sample.bytes[at] == 0xff) {
			const std::uint32_t marker = sample.bytes[at + 1];
			const std::uint32_t length = be16(&sample.bytes[at + 2]);
			const std::uint8_t* segment = &sample.bytes[at];
			if (marker == 0xda || length < 2 || !has(sample, at, 2 + length)) break;
			if (isFrameMarker(marker) && length >= 8) return text + frameText(segment, marker);
			if (marker == 0xe0) text += jfifText(segment, length);
			if (marker == 0xe1 && length >= 6 && std::memcmp(segment + 4, "Exif", 4) == 0) {
				text += ", Exif standard";
			}

			at += 2 + length;
		}

		return text;
	}

	static bool identifyJpeg(const FileSample& sample, MagicResult& result) {
		if (!matches(sample, 0, "\xff\xd8\xff", 3)) return false;

		setResult(result, "JPEG image data" + jpegDetails(sample), "image/jpeg");
		return true;
	}

	static std::string bmpFormat(std::uint32_t header_size) {
		switch (header_size) {
			case 12:
				return "OS/2 1.x format";
			case 64:
				return "OS/2 2.x format";
			case 40:
				return "Windows 3.x format";
			case 108:
				return "Windows 95/NT4 and newer format";
			case 124:
				return "Windows 98/2000 and newer format";
			default:
				return "unknown format";
		}
	}

	static std::string bmpGeometry(const FileSample& sample, std::uint32_t header_size) {
		const std::uint8_t* info = &sample.bytes[14];
		if (header_size == 12) {
			return dimensions(le16(info + 4), le16(info + 6), " x ") + " x "
				+ std::to_string(le16(info + 10));
		}

		const std::int32_t height = static_cast<std::int32_t>(le32(info + 8));
		std::string text = std::to_string(static_cast<std::int32_t>(le32(info + 4))) + " x "
			+ std::to_string(height) + " x " + std::to_string(le16(info + 14));
		if (le32(info + 20) != 0) text += ", image size " + std::to_string(le32(info + 20));
		if (le32(info + 24) != 0 || le32(info + 28) != 0) {
			text += ", resolution " + dimensions(le32(info + 24), le32(info + 28), " x ") + " px/m";
		}

		return text;
	}

	static bool identifyBmp(const FileSample& sample, MagicResult& result) {
		if (!matches(sample, 0, "BM", 2) || !has(sample, 14, 4)) return false;

		const std::uint32_t header_size = le32(&sample.bytes[14]);
		const bool known = header_size == 12 || header_size == 40 || header_size == 64
			|| header_size == 108 || header_size == 124;
		if (!known || !has(sample, 14, header_size)) return false;

		std::string text = "PC bitmap, " + bmpFormat(header_size) + ", "
			+ bmpGeometry(sample, header_size);
		if (header_size != 12) {
			text += ", cbSize " + std::to_string(le32(&sample.bytes[2]))
				+ ", bits offset " + std::to_string(le32(&sample.bytes[10]));
		}

		setResult(result, text, "image/bmp");
		return true;
	}

	static std::string iconEntryText(const std::uint8_t* entry) {
		const std::uint32_t width = entry[0] == 0 ? 256 : entry[0];
		const std::uint32_t height = entry[1] == 0 ? 256 : entry[1];
		std::string text = ", " + dimensions(width, height, "x");
		if (entry[2] != 0) return text + ", " + std::to_string(entry[2]) + " colors";
		return text + ", " + std::to_string(le16(entry + 6)) + " bits/pixel";
	}

	static bool identifyIcon(const FileSample& sample, MagicResult& result) {
		if (!has(sample, 0, 22) || le16(&sample.bytes[0]) != 0) return false;

		const std::uint32_t type = le16(&sample.bytes[2]);
		const std::uint32_t count = le16(&sample.bytes[4]);
		const bool plausible = (type == 1 || type == 2) && count > 0 && count < 256
			&& sample.bytes[9] == 0 && has(sample, 6, count * 16);
		if (!plausible) return false;

		std::string text = std::string("MS Windows ") + (type == 1 ? "icon" : "cursor")
			+ " resource - " + std::to_string(count) + (count == 1 ? " icon" : " icons");
		for (std::uint32_t k = 0; k < count && type == 1; ++k) {
			text += iconEntryText(&sample.bytes[6 + k * 16]);
		}

		setResult(result, text, type == 1 ? "image/vnd.microsoft.icon" : "image/x-win-bitmap");
		return true;
	}

	static bool identifyTiff(const FileSample& sample, MagicResult& result) {
		const bool little = matches(sample, 0, "II*\0", 4);
		if (!little && !matches(sample, 0, "MM\0*", 4)) return false;

		std::string text = std::string("TIFF image data, ")
			+ (little ? "little-endian" : "big-endian");
		const std::uint32_t ifd = little ? le32(&sample.bytes[4]) : be32(&sample.bytes[4]);
		if (has(sample, ifd, 2)) {
			const std::uint8_t* count = &sample.bytes[ifd];
			const std::uint32_t entries = little ? le16(count) : be16(count);
			text += ", direntries=" + std::to_string(entries);
		}

		setResult(result, text, "image/tiff");
		return true;
	}

	static std::string channelText(std::uint32_t channels) {
		if (channels == 1) return "mono";
		if (channels == 2) return "stereo";
		return std::to_string(channels) + " channels";
	}

	static std::string waveText(const FileSample& sample) {
		if (!matches(sample, 12, "fmt ", 4) || !has(sample, 20, 16)) return ", WAVE audio";

		const std::uint8_t* format = &sample.bytes[20];
		const std::uint32_t tag = le16(format);
		std::string text = ", WAVE audio";
		if (tag == 1) text += ", Microsoft PCM";
		else if (tag == 3) text += ", IEEE Float";
		else if (tag == 0xfffe) text += ", WAVE_FORMAT_EXTENSIBLE";
		else text += ", format " + std::to_string(tag);
		return text + ", " + std::to_string(le16(format + 14)) + " bit, "
			+ channelText(le16(format + 2)) + " " + std::to_string(le32(format + 4)) + " Hz";
	}

	static std::string webpText(const FileSample& sample) {
		std::string text = ", Web/P image";
		if (!has(sample, 30, 0)) return text;

		const std::uint8_t* chunk = &sample.bytes[12];
		if (std::memcmp(chunk, "VP8 ", 4) == 0) {
			const std::uint32_t width = le16(chunk + 14) & 0x3fff;
			const std::uint32_t height = le16(chunk + 16) & 0x3fff;
			return text + ", VP8 encoding, " + dimensions(width, height, "x")
				+ ", Scaling: [none]x[none], YUV color, decoders should clamp";
		}

		if (std::memcmp(chunk, "VP8L", 4) == 0 && chunk[8] == 0x2f) {
			const std::uint32_t bits = le32(chunk + 9);
			const std::uint32_t width = (bits & 0x3fff) + 1;
			const std::uint32_t height = ((bits >> 14) & 0x3fff) + 1;
			return text + ", with lossless, " + dimensions(width, height, "x");
		}

		return text;
	}

	static bool identifyRiff(const FileSample& sample, MagicResult& result) {
		if (!matches(sample, 0, "RIFF", 4) || !has(sample, 8, 4)) return false;

		const std::string base = "RIFF (little-endian) data";
		if (matches(sample, 8, "WEBP", 4)) {
			setResult(result, base + webpText(sample), "image/webp");
		} else if (matches(sample, 8, "WAVE", 4)) {
			setResult(result, base + waveText(sample), "audio/x-wav");
		} else if (matches(sample, 8, "AVI ", 4)) {
			setResult(result, base + ", AVI", "video/x-msvideo");
		} else {
			setResult(result, base, "application/octet-stream");
		}

		return true;
	}

	static std::string vorbisText(const std::uint8_t* packet) {
		const std::uint32_t bitrate = le32(packet + 20);
		std::string text = ", Vorbis audio, " + channelText(packet[11]) + ", "
			+ std::to_string(le32(packet + 12)) + " Hz";
		if (bitrate != 0 && bitrate != 0xffffffff) text += ", ~" + std::to_string(bitrate) + " bps";
		return text;
	}

	static std::string opusText(const std::uint8_t* packet) {
		return ", Opus audio, version 0." + std::to_string(packet[8]) + ", "
			+ channelText(packet[9]) + ", " + std::to_string(le32(packet + 12))
			+ " Hz (Input Sample Rate)";
	}

	static bool identifyOgg(const FileSample& sample, MagicResult& result) {
		if (!matches(sample, 0, "OggS", 4) || !has(sample, 26, 1)) return false;

		const std::size_t packet = 27 + sample.bytes[26];
		std::string text = "Ogg data";
		const char* mime = "audio/ogg";
		if (matches(sample, packet, "\x01vorbis", 7) && has(sample, packet, 24)) {
			text += vorbisText(&sample.bytes[packet]);
		} else if (matches(sample, packet, "OpusHead", 8) && has(sample, packet, 16)) {
			text += opusText(&sample.bytes[packet]);
		} else if (matches(sample, packet, "\x7f" "FLAC", 5)) {
			text += ", FLAC audio";
		} else if (matches(sample, packet, "\x80theora", 7)) {
			text += ", Theora video";
			mime = "video/ogg";
		}

		setResult(result, text, mime);
		return true;
	}

	static std::string kilohertzText(std::uint32_t rate) {
		std::string text = std::to_string(rate / 1000);
		const std::uint32_t tenths = (rate % 1000) / 100;
		if (rate % 1000 != 0) text += "." + std::to_string(tenths);
		return text + " kHz";
	}

	static bool identifyFlac(const FileSample& sample, MagicResult& result) {
		if (!matches(sample, 0, "fLaC", 4)) return false;
		if (!has(sample, 8, 18)) {
			setResult(result, "FLAC audio bitstream data", "audio/flac");
			return true;
		}

		const std::uint8_t* info = &sample.bytes[18];
		const std::uint32_t rate = (static_cast<std::uint32_t>(info[0]) << 12) | (info[1] << 4)
			| (info[2] >> 4);
		const std::uint32_t channels = ((info[2] >> 1) & 7) + 1;
		const std::uint32_t bits = (((info[2] & 1) << 4) | (info[3] >> 4)) + 1;
		const std::uint64_t samples = (static_cast<std::uint64_t>(info[3] & 0x0f) << 32)
			| be32(info + 4);
		std::string text = "FLAC audio bitstream data, " + std::to_string(bits) + " bit, "
			+ channelText(channels) + ", " + kilohertzText(rate);
		if (samples != 0) text += ", " + std::to_string(samples) + " samples";
		setResult(result, text, "audio/flac");
		return true;
	}

	static bool readMp3Frame(const FileSample& sample, std::size_t at, Mp3Frame& frame) {
		if (!has(sample, at, 4) || sample.bytes[at] != 0xff) return false;
		if ((sample.bytes[at + 1] & 0xe0) != 0xe0) return false;

		frame.version = (sample.bytes[at + 1] >> 3) & 3;
		frame.layer = (sample.bytes[at + 1] >> 1) & 3;
		frame.bitrate_index = sample.bytes[at + 2] >> 4;
		frame.rate_index = (sample.bytes[at + 2] >> 2) & 3;
		frame.channel_mode = sample.bytes[at + 3] >> 6;
		return frame.version != 1 && frame.layer != 0 && frame.bitrate_index != 15
			&& frame.rate_index != 3;
	}

	static std::string mp3FrameText(const Mp3Frame& frame) {
		const std::uint32_t layer_row = 3 - frame.layer;
		const std::uint32_t* bitrates = frame.version == 3 ? kMp3BitratesV1[layer_row]
			: kMp3BitratesV2[layer_row];
		const std::uint32_t kbps = bitrates[frame.bitrate_index];
		const std::uint32_t divisor = frame.version == 3 ? 1 : frame.version == 2 ? 2 : 4;
		std::string text = std::string("MPEG ADTS, layer ") + kMp3Layers[frame.layer] + ", "
			+ kMp3Versions[frame.version];
		if (kbps != 0) text += ", " + std::to_string(kbps) + " kbps";
		text += ", " + kilohertzText(kMp3Rates[frame.rate_index] / divisor);
		return text + ", " + kMp3Channels[frame.channel_mode];
	}

	static bool identifyMp3(const FileSample& sample, MagicResult& result) {
		Mp3Frame frame;
		if (matches(sample, 0, "ID3", 3) && has(sample, 0, 10)) {
			const std::uint8_t* size = &sample.bytes[6];
			const std::size_t body = (size[0] << 21) | (size[1] << 14) | (size[2] << 7) | size[3];
			std::string text = "Audio file with ID3 version 2." + std::to_string(sample.bytes[3])
				+ "." + std::to_string(sample.bytes[4]);
			if (readMp3Frame(sample, 10 + body, frame)) {
				text += ", contains: " + mp3FrameText(frame);
			}

			setResult(result, text, "audio/mpeg");
			return true;
		}

		if (!readMp3Frame(sample, 0, frame)) return false;

		setResult(result, mp3FrameText(frame), "audio/mpeg");
		return true;
	}

	static bool identifyIsoMedia(const FileSample& sample, MagicResult& result) {
		if (!matches(sample, 4, "ftyp", 4) || !has(sample, 8, 4)) return false;

		for (const BrandName& brand : kIsoBrands) {
			if (!matches(sample, 8, brand.brand, 4)) continue;
			setResult(result, std::string("ISO Media, ") + brand.description, brand.mime);
			return true;
		}

		setResult(result, "ISO Media", "video/mp4");
		return true;
	}

	static bool identifyMatroska(const FileSample& sample, MagicResult& result) {
		if (!matches(sample, 0, "\x1a\x45\xdf\xa3", 4)) return false;

		const std::size_t limit = sample.bytes.size() < 64 ? sample.bytes.size() : 64;
		const std::string head(sample.bytes.begin(), sample.bytes.begin() + limit);
		if (head.find("webm") != std::string::npos) setResult(result, "WebM", "video/webm");
		else setResult(result, "Matroska data", "video/x-matroska");
		return true;
	}

	bool identifyMedia(const FileSample& sample, MagicResult& result) {
		return identifyPng(sample, result) || identifyGif(sample, result)
			|| identifyJpeg(sample, result) || identifyBmp(sample, result)
			|| identifyTiff(sample, result) || identifyRiff(sample, result)
			|| identifyOgg(sample, result) || identifyFlac(sample, result)
			|| identifyIsoMedia(sample, result) || identifyMatroska(sample, result)
			|| identifyMp3(sample, result) || identifyIcon(sample, result);
	}

}  // namespace wbsh
