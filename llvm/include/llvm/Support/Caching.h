//===- Caching.h - LLVM Local File Cache ------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the CachedFileStream and the localCache function, which
// simplifies caching files on the local filesystem in a directory whose
// contents are managed by a CachePruningPolicy.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_CACHING_H
#define LLVM_SUPPORT_CACHING_H

#include "llvm/Support/Error.h"

namespace llvm {

class MemoryBuffer;

/// This class wraps an output stream for a file. Most clients should just be
/// able to return an instance of this base class from the stream callback, but
/// if a client needs to perform some action after the stream is written to,
/// that can be done by deriving from this class and overriding the destructor.
class CachedFileStream {
public:
  CachedFileStream(std::unique_ptr<raw_pwrite_stream> OS) : OS(std::move(OS)) {}
  std::unique_ptr<raw_pwrite_stream> OS;
  virtual ~CachedFileStream() = default;
};

/// This type defines the callback to add a file that is generated on the fly.
///
/// Stream callbacks must be thread safe.
using AddStreamFn =
    std::function<Expected<std::unique_ptr<CachedFileStream>>(unsigned Task)>;

/// This is the type of a file cache. To request an item from the cache, pass a
/// unique string as the Key. For hits, the cached file will be added to the
/// link and this function will return AddStreamFn(). For misses, the cache will
/// return a stream callback which must be called at most once to produce
/// content for the stream. The file stream produced by the stream callback will
/// add the file to the link after the stream is written to.
///
/// Clients generally look like this:
///
/// if (AddStreamFn AddStream = Cache(Task, Key))
///   ProduceContent(AddStream);
using FileCache =
    std::function<Expected<AddStreamFn>(unsigned Task, StringRef Key)>;

/// This type defines the callback to add a pre-existing file (e.g. in a cache).
///
/// Buffer callbacks must be thread safe.
using AddBufferFn =
    std::function<void(unsigned Task, std::unique_ptr<MemoryBuffer> MB)>;

/// Create a local file system cache which uses the given cache name, temporary
/// file prefix, cache directory and file callback. This function also creates
/// the cache directory if it does not already exist. The cache name appears in
/// error messages for errors during caching. The temporary file prefix is used
/// in the temporary file naming scheme used when writing files atomically.
Expected<FileCache> localCache(
    Twine CacheNameRef, Twine TempFilePrefixRef, Twine CacheDirectoryPathRef,
    AddBufferFn AddBuffer = [](size_t Task, std::unique_ptr<MemoryBuffer> MB) {
    });
} // namespace llvm

#endif
