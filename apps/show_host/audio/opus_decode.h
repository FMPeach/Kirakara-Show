#pragma once

#include <string>

struct DecodedAudioBuffer;
enum class OpusDecodeResult { not_opus, decoded, failed };

// Local files are identified by content. Unknown HTTP URLs are tried only
// after the existing Media Foundation path fails.
OpusDecodeResult decode_ogg_opus(const std::wstring& path,
    DecodedAudioBuffer& output, bool allow_unknown_http = false);
