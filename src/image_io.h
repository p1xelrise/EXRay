// EXRay (HDR & EXR viewer)
// Copyright (C) 2026 Giorgos Eleftheroglou (pixelrise)
// SPDX-License-Identifier: GPL-3.0-or-later
// Loading .exr (OpenEXR) and .hdr (Radiance RGBE) files.
#pragma once

#include <string>
#include "hdr_image.h"

// Path is UTF-16 on Windows so folders with Greek or other non-ASCII
// characters open correctly. On other platforms it is a UTF-8 path.
#ifdef _WIN32
using PathString = std::wstring;
#else
using PathString = std::string;
#endif

#include <memory>
#include <vector>

class ExrDocument;

// Loads the file at `path` into `out`. Detects the format from the file
// contents (not the extension). Returns false and fills `error` on failure.
bool LoadHdrFile(const PathString& path, HdrImage& out, std::string& error);

// The same in two steps, for callers that want to look at the file first.
bool ReadFileBytes(const PathString& path, std::vector<unsigned char>& data, std::string& error);
// For EXR files with more than one layer, `exrLayers` (if given) receives the
// decoded file so other layers can be shown without reading it again.
bool DecodeHdrImage(const std::vector<unsigned char>& data, HdrImage& out, std::string& error,
                    std::shared_ptr<ExrDocument>* exrLayers = nullptr);

// For thumbnails: a .hdr is shrunk by a whole factor while it is decoded, to
// at least `maxWidth` pixels wide, so huge files need little memory. Other
// formats are decoded at full size.
bool DecodeHdrImageReduced(const std::vector<unsigned char>& data, HdrImage& out, std::string& error, int maxWidth);

// Reads just the image size from the file header (cheap). False if unknown.
bool PeekImageSize(const std::vector<unsigned char>& data, int& width, int& height);

// True if the extension is one this app opens (.exr or .hdr, any case).
bool IsSupportedExtension(const PathString& path);
