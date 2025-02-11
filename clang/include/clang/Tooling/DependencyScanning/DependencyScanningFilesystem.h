//===- DependencyScanningFilesystem.h - clang-scan-deps fs ===---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLING_DEPENDENCY_SCANNING_FILESYSTEM_H
#define LLVM_CLANG_TOOLING_DEPENDENCY_SCANNING_FILESYSTEM_H

#include "clang/Basic/LLVM.h"
#include "clang/Lex/PreprocessorExcludedConditionalDirectiveSkipMapping.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/VirtualFileSystem.h"
#include <mutex>

namespace clang {
namespace tooling {
namespace dependencies {

/// An in-memory representation of a file system entity that is of interest to
/// the dependency scanning filesystem.
///
/// It represents one of the following:
/// - an opened source file with minimized contents and a stat value.
/// - an opened source file with original contents and a stat value.
/// - a directory entry with its stat value.
/// - an error value to represent a file system error.
/// - uninitialized entry with unknown status.
class CachedFileSystemEntry {
public:
  /// Creates an uninitialized entry.
  CachedFileSystemEntry() : MaybeStat(llvm::vfs::Status()) {}

  /// Initialize the cached file system entry.
  void init(llvm::ErrorOr<llvm::vfs::Status> &&MaybeStatus, StringRef Filename,
            llvm::vfs::FileSystem &FS, bool ShouldMinimize = true);

  /// Initialize the entry as file with minimized or original contents.
  ///
  /// The filesystem opens the file even for `stat` calls open to avoid the
  /// issues with stat + open of minimized files that might lead to a
  /// mismatching size of the file.
  llvm::ErrorOr<llvm::vfs::Status>
  initFile(StringRef Filename, llvm::vfs::FileSystem &FS, bool Minimize = true);

  /// \returns True if the entry is initialized.
  bool isInitialized() const {
    return !MaybeStat || MaybeStat->isStatusKnown();
  }

  /// \returns True if the current entry points to a directory.
  bool isDirectory() const { return MaybeStat && MaybeStat->isDirectory(); }

  /// \returns The error or the file's contents.
  llvm::ErrorOr<StringRef> getContents() const {
    if (!MaybeStat)
      return MaybeStat.getError();
    assert(!MaybeStat->isDirectory() && "not a file");
    assert(isInitialized() && "not initialized");
    return Contents->getBuffer();
  }

  /// \returns The error or the status of the entry.
  llvm::ErrorOr<llvm::vfs::Status> getStatus() const {
    assert(isInitialized() && "not initialized");
    return MaybeStat;
  }

  /// \returns the name of the file.
  StringRef getName() const {
    assert(isInitialized() && "not initialized");
    return MaybeStat->getName();
  }

  /// Return the mapping between location -> distance that is used to speed up
  /// the block skipping in the preprocessor.
  const PreprocessorSkippedRangeMapping &getPPSkippedRangeMapping() const {
    return PPSkippedRangeMapping;
  }

  CachedFileSystemEntry(CachedFileSystemEntry &&) = default;
  CachedFileSystemEntry &operator=(CachedFileSystemEntry &&) = default;

  CachedFileSystemEntry(const CachedFileSystemEntry &) = delete;
  CachedFileSystemEntry &operator=(const CachedFileSystemEntry &) = delete;

private:
  llvm::ErrorOr<llvm::vfs::Status> MaybeStat;
  std::unique_ptr<llvm::MemoryBuffer> Contents;
  PreprocessorSkippedRangeMapping PPSkippedRangeMapping;
};

/// This class is a shared cache, that caches the 'stat' and 'open' calls to the
/// underlying real file system. It distinguishes between minimized and original
/// files.
///
/// It is sharded based on the hash of the key to reduce the lock contention for
/// the worker threads.
class DependencyScanningFilesystemSharedCache {
public:
  struct SharedFileSystemEntry {
    std::mutex ValueLock;
    CachedFileSystemEntry Value;
  };

  /// Returns a cache entry for the corresponding key.
  ///
  /// A new cache entry is created if the key is not in the cache. This is a
  /// thread safe call.
  SharedFileSystemEntry &get(StringRef Key, bool Minimized);

private:
  class SingleCache {
  public:
    SingleCache();

    SharedFileSystemEntry &get(StringRef Key);

  private:
    struct CacheShard {
      std::mutex CacheLock;
      llvm::StringMap<SharedFileSystemEntry, llvm::BumpPtrAllocator> Cache;
    };
    std::unique_ptr<CacheShard[]> CacheShards;
    unsigned NumShards;
  };

  SingleCache CacheMinimized;
  SingleCache CacheOriginal;
};

/// This class is a local cache, that caches the 'stat' and 'open' calls to the
/// underlying real file system. It distinguishes between minimized and original
/// files.
class DependencyScanningFilesystemLocalCache {
private:
  using SingleCache =
      llvm::StringMap<const CachedFileSystemEntry *, llvm::BumpPtrAllocator>;

  SingleCache CacheMinimized;
  SingleCache CacheOriginal;

  SingleCache &selectCache(bool Minimized) {
    return Minimized ? CacheMinimized : CacheOriginal;
  }

public:
  void setCachedEntry(StringRef Filename, bool Minimized,
                      const CachedFileSystemEntry *Entry) {
    SingleCache &Cache = selectCache(Minimized);
    bool IsInserted = Cache.try_emplace(Filename, Entry).second;
    (void)IsInserted;
    assert(IsInserted && "local cache is updated more than once");
  }

  const CachedFileSystemEntry *getCachedEntry(StringRef Filename,
                                              bool Minimized) {
    SingleCache &Cache = selectCache(Minimized);
    auto It = Cache.find(Filename);
    return It == Cache.end() ? nullptr : It->getValue();
  }
};

/// A virtual file system optimized for the dependency discovery.
///
/// It is primarily designed to work with source files whose contents was was
/// preprocessed to remove any tokens that are unlikely to affect the dependency
/// computation.
///
/// This is not a thread safe VFS. A single instance is meant to be used only in
/// one thread. Multiple instances are allowed to service multiple threads
/// running in parallel.
class DependencyScanningWorkerFilesystem : public llvm::vfs::ProxyFileSystem {
public:
  DependencyScanningWorkerFilesystem(
      DependencyScanningFilesystemSharedCache &SharedCache,
      IntrusiveRefCntPtr<llvm::vfs::FileSystem> FS,
      ExcludedPreprocessorDirectiveSkipMapping *PPSkipMappings)
      : ProxyFileSystem(std::move(FS)), SharedCache(SharedCache),
        PPSkipMappings(PPSkipMappings) {}

  llvm::ErrorOr<llvm::vfs::Status> status(const Twine &Path) override;
  llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>>
  openFileForRead(const Twine &Path) override;

  /// Disable minimization of the given file.
  void disableMinimization(StringRef Filename);
  /// Enable minimization of all files.
  void enableMinimizationOfAllFiles() { NotToBeMinimized.clear(); }

private:
  /// Check whether the file should be minimized.
  bool shouldMinimize(StringRef Filename);

  llvm::ErrorOr<const CachedFileSystemEntry *>
  getOrCreateFileSystemEntry(const StringRef Filename);

  /// The global cache shared between worker threads.
  DependencyScanningFilesystemSharedCache &SharedCache;
  /// The local cache is used by the worker thread to cache file system queries
  /// locally instead of querying the global cache every time.
  DependencyScanningFilesystemLocalCache Cache;
  /// The optional mapping structure which records information about the
  /// excluded conditional directive skip mappings that are used by the
  /// currently active preprocessor.
  ExcludedPreprocessorDirectiveSkipMapping *PPSkipMappings;
  /// The set of files that should not be minimized.
  llvm::StringSet<> NotToBeMinimized;
};

} // end namespace dependencies
} // end namespace tooling
} // end namespace clang

#endif // LLVM_CLANG_TOOLING_DEPENDENCY_SCANNING_FILESYSTEM_H
