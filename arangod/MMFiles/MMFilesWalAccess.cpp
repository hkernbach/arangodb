////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2017 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#include "MMFiles/MMFilesWalAccess.h"
#include "Basics/ReadLocker.h"
#include "Basics/StaticStrings.h"
#include "Basics/StringRef.h"
#include "Basics/VPackStringBufferAdapter.h"
#include "Logger/Logger.h"
#include "MMFiles/MMFilesCompactionLocker.h"
#include "MMFiles/MMFilesDitch.h"
#include "MMFiles/MMFilesLogfileManager.h"
#include "MMFiles/mmfiles-replication-common.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/vocbase.h"

#include <velocypack/Dumper.h>
#include <velocypack/Options.h>
#include <velocypack/Slice.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using namespace arangodb::mmfilesutils;

Result MMFilesWalAccess::tickRange(
    std::pair<TRI_voc_tick_t, TRI_voc_tick_t>& minMax) const {
  auto const ranges = MMFilesLogfileManager::instance()->ranges();
  if (!ranges.empty()) {
    minMax.first = ranges[0].tickMin;
    minMax.second = ranges[0].tickMax;
  } else {
    return Result(TRI_ERROR_INTERNAL, "could not load tick ranges");
  }
  for (auto const& range : ranges) {
    if (range.tickMin < minMax.first) {
      minMax.first = range.tickMin;
    }
    if (range.tickMax > minMax.second) {
      minMax.second = range.tickMax;
    }
  }
  return TRI_ERROR_NO_ERROR;
}

/// {"lastTick":"123",
///  "version":"3.2",
///  "serverId":"abc",
///  "clients": {
///    "serverId": "ass", "lastTick":"123", ...
///  }}
///
TRI_voc_tick_t MMFilesWalAccess::lastTick() const {
  TRI_voc_tick_t maxTick = 0;
  auto const& ranges = MMFilesLogfileManager::instance()->ranges();
  for (auto& it : ranges) {
    if (maxTick < it.tickMax) {
      maxTick = it.tickMax;
    }
  }
  return maxTick;
}

/// should return the list of transactions started, but not committed in that
/// range (range can be adjusted)
WalAccessResult MMFilesWalAccess::openTransactions(
    uint64_t tickStart, uint64_t tickEnd, WalAccess::Filter const& filter,
    TransactionCallback const& cb) const {
  LOG_TOPIC(TRACE, arangodb::Logger::FIXME)
      << "determining transactions, tick range " << tickStart << " - "
      << tickEnd;

  std::unordered_map<TRI_voc_tid_t, TRI_voc_tick_t> transactions;

  // ask the logfile manager which datafiles qualify
  bool fromTickIncluded = false;
  std::vector<arangodb::MMFilesWalLogfile*> logfiles =
      MMFilesLogfileManager::instance()->getLogfilesForTickRange(
          tickStart, tickEnd, fromTickIncluded);
  // always return the logfiles we have used
  TRI_DEFER(MMFilesLogfileManager::instance()->returnLogfiles(logfiles));

  // setup some iteration state
  TRI_voc_tick_t lastFoundTick = 0;
  WalAccessResult res;

  // LOG_TOPIC(INFO, arangodb::Logger::FIXME) << "found logfiles: " <<
  // logfiles.size();

  try {
    // iterate over the datafiles found
    size_t const n = logfiles.size();
    for (size_t i = 0; i < n; ++i) {
      arangodb::MMFilesWalLogfile* logfile = logfiles[i];

      char const* ptr;
      char const* end;
      MMFilesLogfileManager::instance()->getActiveLogfileRegion(logfile, ptr,
                                                                end);

      // LOG_TOPIC(INFO, arangodb::Logger::FIXME) << "scanning logfile " << i;
      while (ptr < end) {
        auto const* marker = reinterpret_cast<MMFilesMarker const*>(ptr);

        if (marker->getSize() == 0) {
          // end of datafile
          break;
        }

        MMFilesMarkerType const type = marker->getType();

        if (type <= TRI_DF_MARKER_MIN || type >= TRI_DF_MARKER_MAX) {
          // somehow invalid
          break;
        }

        ptr += MMFilesDatafileHelper::AlignedMarkerSize<size_t>(marker);

        // get the marker's tick and check whether we should include it
        TRI_voc_tick_t const foundTick = marker->getTick();

        if (foundTick <= tickStart) {
          // marker too old
          continue;
        }

        if (foundTick > tickEnd) {
          // marker too new
          break;
        }

        // note the last tick we processed
        if (foundTick > lastFoundTick) {
          lastFoundTick = foundTick;
        }

        // first check the marker type
        if (!IsTransactionWalMarkerType(marker)) {
          continue;
        }
        // then check if the marker belongs to the "correct" database
        if (filter.vocbase != 0 &&
            filter.vocbase != MMFilesDatafileHelper::DatabaseId(marker)) {
          continue;
        }

        TRI_voc_tid_t tid = MMFilesDatafileHelper::TransactionId(marker);
        TRI_ASSERT(tid > 0);

        if (type == TRI_DF_MARKER_VPACK_BEGIN_TRANSACTION) {
          transactions.emplace(tid, foundTick);
        } else if (type == TRI_DF_MARKER_VPACK_COMMIT_TRANSACTION ||
                   type == TRI_DF_MARKER_VPACK_ABORT_TRANSACTION) {
          transactions.erase(tid);
        } else {
          THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                         "found invalid marker type");
        }
      }
    }

    // LOG_TOPIC(INFO, arangodb::Logger::FIXME) << "found transactions: " <<
    // transactions.size();
    // LOG_TOPIC(INFO, arangodb::Logger::FIXME) << "last tick: " <<
    // lastFoundTick;
    for (auto const& it : transactions) {
      if (it.second - 1 < lastFoundTick) {
        lastFoundTick = it.second - 1;
      }
      cb(it.first, it.second);
    }

    MMFilesLogfileManagerState const state =
        MMFilesLogfileManager::instance()->state();
    res.reset(TRI_ERROR_NO_ERROR, fromTickIncluded, lastFoundTick,
              /*latest*/ state.lastCommittedTick);
  } catch (arangodb::basics::Exception const& ex) {
    LOG_TOPIC(ERR, arangodb::Logger::FIXME)
        << "caught exception while determining open transactions: "
        << ex.what();
    res.reset(ex.code(), false, 0, 0);
  } catch (std::exception const& ex) {
    LOG_TOPIC(ERR, arangodb::Logger::FIXME)
        << "caught exception while determining open transactions: "
        << ex.what();
    res.reset(TRI_ERROR_INTERNAL, false, 0, 0);
  } catch (...) {
    LOG_TOPIC(ERR, arangodb::Logger::FIXME)
        << "caught unknown exception while determining open transactions";
    res.reset(TRI_ERROR_INTERNAL, false, 0, 0);
  }

  return res;
}

struct MMFilesWalAccessContext : WalAccessContext {
  MMFilesWalAccessContext(WalAccess::Filter const& f,
                          WalAccess::MarkerCallback const& c)
      : WalAccessContext(f, c) {}

  /// @brief whether or not a marker belongs to a transaction
  bool isTransactionWalMarker(MMFilesMarker const* marker) {
    // first check the marker type
    if (!IsTransactionWalMarkerType(marker)) {
      return false;
    }

    // then check if the marker belongs to the "correct" database
    if (_filter.vocbase != 0 &&
        _filter.vocbase != MMFilesDatafileHelper::DatabaseId(marker)) {
      return false;
    }
    return true;
  }

  /// @brief whether or not a marker is replicated
  bool mustReplicateWalMarker(MMFilesMarker const* marker,
                              TRI_voc_tick_t databaseId, TRI_voc_cid_t cid) {
    // first check the marker type
    if (!MustReplicateWalMarkerType(marker, true)) {
      return false;
    }

    // then check if the marker belongs to the "correct" database
    if (_filter.vocbase != 0 && _filter.vocbase != databaseId) {
      return false;
    }

    // finally check if the marker is for a collection that we want to ignore
    if (cid != 0) {
      if (_filter.collection != 0 &&
          (cid != _filter.collection && !isTransactionWalMarker(marker))) {
        // restrict output to a single collection, but a different one
        return false;
      }

      LogicalCollection* collection = loadCollection(databaseId, cid);
      if (collection != nullptr) {  // db may be already dropped
        if (TRI_ExcludeCollectionReplication(collection->name(),
                                             _filter.includeSystem)) {
          return false;
        }
      }
    }

    // after first regular tick, dump all transactions normally
    if (marker->getTick() < _filter.firstRegularTick) {
      if (!_filter.transactionIds.empty()) {
        TRI_voc_tid_t tid = MMFilesDatafileHelper::TransactionId(marker);
        if (tid == 0 ||
            _filter.transactionIds.find(tid) == _filter.transactionIds.end()) {
          return false;
        }
      }
    }

    return true;
  }

  int sliceifyMarker(TRI_voc_tick_t databaseId, TRI_voc_cid_t collectionId,
                     MMFilesMarker const* marker) {
    TRI_ASSERT(MustReplicateWalMarkerType(marker, true));
    MMFilesMarkerType const type = marker->getType();

    /*VPackBuffer<uint8_t> buffer;
     std::shared_ptr<VPackBuffer<uint8_t>> bufferPtr;
     bufferPtr.reset(&buffer,
     arangodb::velocypack::BufferNonDeleter<uint8_t>());
     */
    TRI_DEFER(_builder.clear());
    _builder.openObject();
    // logger-follow command
    _builder.add("tick", VPackValue(std::to_string(marker->getTick())));
    TRI_replication_operation_e tt = TranslateType(marker);
    _builder.add("type", VPackValue(static_cast<uint64_t>(tt)));

    if (type == TRI_DF_MARKER_VPACK_DOCUMENT ||
        type == TRI_DF_MARKER_VPACK_REMOVE ||
        type == TRI_DF_MARKER_VPACK_BEGIN_TRANSACTION ||
        type == TRI_DF_MARKER_VPACK_COMMIT_TRANSACTION ||
        type == TRI_DF_MARKER_VPACK_ABORT_TRANSACTION) {
      // transaction id
      TRI_voc_tid_t tid = MMFilesDatafileHelper::TransactionId(marker);
      _builder.add("tid", VPackValue(std::to_string(tid)));
    }

    if (type == TRI_DF_MARKER_VPACK_DROP_DATABASE) {
      VPackSlice slice(reinterpret_cast<char const*>(marker) +
                       MMFilesDatafileHelper::VPackOffset(type));
      _builder.add("db", slice.get("name"));
    } else if (type == TRI_DF_MARKER_VPACK_DROP_COLLECTION) {
      TRI_ASSERT(databaseId != 0);
      TRI_vocbase_t* vocbase = loadVocbase(databaseId);
      if (vocbase == nullptr) {
        // ignore markers from dropped dbs
        return TRI_ERROR_NO_ERROR;
      }
      VPackSlice slice(reinterpret_cast<char const*>(marker) +
                       MMFilesDatafileHelper::VPackOffset(type));
      _builder.add("db", VPackValue(vocbase->name()));
      _builder.add("cuid", slice.get("cuid"));
    } else {
      TRI_ASSERT(databaseId != 0);
      TRI_vocbase_t* vocbase = loadVocbase(databaseId);
      if (vocbase == nullptr) {
        return TRI_ERROR_NO_ERROR;  // ignore dropped dbs
      }
      _builder.add("db", VPackValue(vocbase->name()));
      if (collectionId > 0) {
        LogicalCollection* col = loadCollection(databaseId, collectionId);
        if (col == nullptr) {
          return TRI_ERROR_NO_ERROR;  // ignore dropped collections
        }
        _builder.add("cuid", VPackValue(col->globallyUniqueId()));
      }
    }

    switch (type) {
      case TRI_DF_MARKER_VPACK_DOCUMENT:
      case TRI_DF_MARKER_VPACK_REMOVE:
      case TRI_DF_MARKER_VPACK_CREATE_DATABASE:
      case TRI_DF_MARKER_VPACK_CREATE_COLLECTION:
      case TRI_DF_MARKER_VPACK_CREATE_INDEX:
      case TRI_DF_MARKER_VPACK_CREATE_VIEW:
      case TRI_DF_MARKER_VPACK_RENAME_COLLECTION:
      case TRI_DF_MARKER_VPACK_CHANGE_COLLECTION:
      case TRI_DF_MARKER_VPACK_CHANGE_VIEW:
      case TRI_DF_MARKER_VPACK_DROP_INDEX:
      case TRI_DF_MARKER_VPACK_DROP_VIEW: {
        VPackSlice slice(reinterpret_cast<char const*>(marker) +
                         MMFilesDatafileHelper::VPackOffset(type));
        _builder.add("data", slice);
        break;
      }

      case TRI_DF_MARKER_VPACK_DROP_DATABASE:
      case TRI_DF_MARKER_VPACK_DROP_COLLECTION:
      case TRI_DF_MARKER_VPACK_BEGIN_TRANSACTION:
      case TRI_DF_MARKER_VPACK_COMMIT_TRANSACTION:
      case TRI_DF_MARKER_VPACK_ABORT_TRANSACTION: {
        // nothing to do
        break;
      }

      default: {
        TRI_ASSERT(false);
        LOG_TOPIC(ERR, arangodb::Logger::FIXME) << "got invalid marker of type "
                                                << static_cast<int>(type);
        return TRI_ERROR_INTERNAL;
      }
    }

    _builder.close();
    _callback(loadVocbase(databaseId), _builder.slice());
    _responseSize += _builder.size();
    _builder.clear();

    return TRI_ERROR_NO_ERROR;
  }

  WalAccessResult tail(uint64_t tickStart, uint64_t tickEnd, size_t chunkSize) {
    
    MMFilesLogfileManagerState const state =
    MMFilesLogfileManager::instance()->state();
    
    // ask the logfile manager which datafiles qualify
    bool fromTickIncluded = false;
    std::vector<arangodb::MMFilesWalLogfile*> logfiles =
        MMFilesLogfileManager::instance()->getLogfilesForTickRange(
            tickStart, tickEnd, fromTickIncluded);
    // always return the logfiles we have used
    TRI_DEFER(MMFilesLogfileManager::instance()->returnLogfiles(logfiles));

    // setup some iteration state
    int res = TRI_ERROR_NO_ERROR;
    TRI_voc_tick_t lastFoundTick = 0;
    TRI_voc_tick_t lastDatabaseId = 0;
    TRI_voc_cid_t lastCollectionId = 0;

    try {
      bool hasMore = true;
      bool bufferFull = false;
      // iterate over the datafiles found
      size_t const n = logfiles.size();

      for (size_t i = 0; i < n; ++i) {
        arangodb::MMFilesWalLogfile* logfile = logfiles[i];

        char const* ptr;
        char const* end;
        MMFilesLogfileManager::instance()->getActiveLogfileRegion(logfile, ptr,
                                                                  end);

        while (ptr < end) {
          auto const* marker = reinterpret_cast<MMFilesMarker const*>(ptr);
          if (marker->getSize() == 0) {
            // end of datafile
            break;
          }
          MMFilesMarkerType type = marker->getType();
          if (type <= TRI_DF_MARKER_MIN || type >= TRI_DF_MARKER_MAX) {
            break;
          }
          ptr += MMFilesDatafileHelper::AlignedMarkerSize<size_t>(marker);

          // handle special markers
          if (type == TRI_DF_MARKER_PROLOGUE) {
            lastDatabaseId = MMFilesDatafileHelper::DatabaseId(marker);
            lastCollectionId = MMFilesDatafileHelper::CollectionId(marker);
          } else if (type == TRI_DF_MARKER_HEADER ||
                     type == TRI_DF_MARKER_FOOTER) {
            lastDatabaseId = 0;
            lastCollectionId = 0;
          } else if (type == TRI_DF_MARKER_VPACK_CREATE_COLLECTION) {
            // fill collection name cache
            TRI_voc_tick_t databaseId =
                MMFilesDatafileHelper::DatabaseId(marker);
            TRI_ASSERT(databaseId != 0);
            TRI_voc_cid_t collectionId =
                MMFilesDatafileHelper::CollectionId(marker);
            TRI_ASSERT(collectionId != 0);

            // this will cache the vocbase and collection for this run
            loadVocbase(databaseId);
            loadCollection(databaseId, collectionId);
          } /* else if (type == TRI_DF_MARKER_VPACK_RENAME_COLLECTION) {
             // invalidate collection name cache because this is a
             // rename operation
             dump->_collectionNames.clear();
           }*/

          // get the marker's tick and check whether we should include it
          TRI_voc_tick_t foundTick = marker->getTick();

          if (foundTick <= tickStart) {
            // marker too old
            continue;
          }

          if (foundTick >= tickEnd) {
            hasMore = false;

            if (foundTick > tickEnd) {
              // marker too new
              break;
            }
          }

          TRI_voc_tick_t databaseId;
          TRI_voc_cid_t collectionId;

          if (type == TRI_DF_MARKER_VPACK_DOCUMENT ||
              type == TRI_DF_MARKER_VPACK_REMOVE) {
            databaseId = lastDatabaseId;
            collectionId = lastCollectionId;
          } else {
            databaseId = MMFilesDatafileHelper::DatabaseId(marker);
            collectionId = MMFilesDatafileHelper::CollectionId(marker);
          }

          if (!mustReplicateWalMarker(marker, databaseId, collectionId)) {
            continue;
          }

          // note the last tick we processed
          lastFoundTick = foundTick;

          res = sliceifyMarker(databaseId, collectionId, marker);
          if (res != TRI_ERROR_NO_ERROR) {
            THROW_ARANGO_EXCEPTION(res);
          }

          if (_responseSize >= chunkSize) {
            // abort the iteration
            bufferFull = true;
            break;
          }
        }

        if (!hasMore || bufferFull) {
          break;
        }
      }
    } catch (arangodb::basics::Exception const& ex) {
      LOG_TOPIC(ERR, arangodb::Logger::FIXME)
          << "caught exception while dumping replication log: " << ex.what();
      res = ex.code();
    } catch (std::exception const& ex) {
      LOG_TOPIC(ERR, arangodb::Logger::FIXME)
          << "caught exception while dumping replication log: " << ex.what();
      res = TRI_ERROR_INTERNAL;
    } catch (...) {
      LOG_TOPIC(ERR, arangodb::Logger::FIXME)
          << "caught unknown exception while dumping replication log";
      res = TRI_ERROR_INTERNAL;
    }

    return WalAccessResult(res, fromTickIncluded, lastFoundTick,
                           state.lastCommittedTick);
  }
};

/// Tails the wall, this will already sanitize the
WalAccessResult MMFilesWalAccess::tail(uint64_t tickStart, uint64_t tickEnd,
                                       size_t chunkSize,
                                       TRI_voc_tid_t barrierId,
                                       WalAccess::Filter const& filter,
                                       MarkerCallback const& callback) const {
  /*OG_TOPIC(WARN, Logger::FIXME)
      << "1. Starting tailing: tickStart " << tickStart << " tickEnd "
      << tickEnd << " chunkSize " << chunkSize << " includeSystem "
      << filter.includeSystem << " firstRegularTick" <<
     filter.firstRegularTick;*/

  LOG_TOPIC(TRACE, arangodb::Logger::REPLICATION)
      << "dumping log, tick range " << tickStart << " - " << tickEnd;

  if (barrierId > 0) {
    // extend the WAL logfile barrier
    MMFilesLogfileManager::instance()->extendLogfileBarrier(barrierId, 180,
                                                            tickStart);
  }
  MMFilesWalAccessContext ctx(filter, callback);
  return ctx.tail(tickStart, tickEnd, chunkSize);
}
