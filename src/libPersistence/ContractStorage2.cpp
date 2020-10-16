/*
 * Copyright (C) 2019 Zilliqa
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "ContractStorage2.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "ScillaMessage.pb.h"
#pragma GCC diagnostic pop

#include "libCrypto/Sha2.h"
#include "libMessage/Messenger.h"
#include "libUtils/DataConversion.h"
#include "libUtils/JsonUtils.h"

#include <bits/stdc++.h>
#include <boost/algorithm/string.hpp>

using namespace std;
using namespace ZilliqaMessage;

namespace Contract {
// Code
// ======================================

bool ContractStorage2::PutContractCode(const dev::h160& address,
                                       const bytes& code) {
  lock_guard<mutex> g(m_codeMutex);
  return m_codeDB.Insert(address.hex(), code) == 0;
}

bool ContractStorage2::PutContractCodeBatch(
    const unordered_map<string, string>& batch) {
  lock_guard<mutex> g(m_codeMutex);
  return m_codeDB.BatchInsert(batch);
}

bytes ContractStorage2::GetContractCode(const dev::h160& address) {
  lock_guard<mutex> g(m_codeMutex);
  return DataConversion::StringToCharArray(m_codeDB.Lookup(address.hex()));
}

bool ContractStorage2::DeleteContractCode(const dev::h160& address) {
  lock_guard<mutex> g(m_codeMutex);
  return m_codeDB.DeleteKey(address.hex()) == 0;
}

// InitData
// ========================================
bool ContractStorage2::PutInitData(const dev::h160& address,
                                   const bytes& initData) {
  lock_guard<mutex> g(m_initDataMutex);
  return m_initDataDB.Insert(address.hex(), initData) == 0;
}

bool ContractStorage2::PutInitDataBatch(
    const unordered_map<string, string>& batch) {
  lock_guard<mutex> g(m_initDataMutex);
  return m_initDataDB.BatchInsert(batch);
}

bytes ContractStorage2::GetInitData(const dev::h160& address) {
  lock_guard<mutex> g(m_initDataMutex);
  return DataConversion::StringToCharArray(m_initDataDB.Lookup(address.hex()));
}

bool ContractStorage2::DeleteInitData(const dev::h160& address) {
  lock_guard<mutex> g(m_initDataMutex);
  return m_initDataDB.DeleteKey(address.hex()) == 0;
}
// State
// ========================================
template <class T>
bool SerializeToArray(const T& protoMessage, bytes& dst,
                      const unsigned int offset) {
  if ((offset + protoMessage.ByteSize()) > dst.size()) {
    dst.resize(offset + protoMessage.ByteSize());
  }

  return protoMessage.SerializeToArray(dst.data() + offset,
                                       protoMessage.ByteSize());
}

string ContractStorage2::GenerateStorageKey(const dev::h160& addr,
                                            const string& vname,
                                            const vector<string>& indices) {
  string ret = addr.hex();
  if (!vname.empty()) {
    ret += SCILLA_INDEX_SEPARATOR + vname + SCILLA_INDEX_SEPARATOR;
    for (const auto& index : indices) {
      ret += index + SCILLA_INDEX_SEPARATOR;
    }
  }
  return ret;
}

bool ContractStorage2::FetchStateValue(const dev::h160& addr, const bytes& src,
                                       unsigned int s_offset, bytes& dst,
                                       unsigned int d_offset, bool& foundVal) {
  if (LOG_SC) {
    LOG_MARKER();
  }

  lock_guard<mutex> g(m_stateDataMutex);

  foundVal = true;

  if (s_offset > src.size()) {
    LOG_GENERAL(WARNING, "Invalid src data and offset, data size "
                             << src.size() << ", offset " << s_offset);
  }
  if (d_offset > dst.size()) {
    LOG_GENERAL(WARNING, "Invalid dst data and offset, data size "
                             << dst.size() << ", offset " << d_offset);
  }

  ProtoScillaQuery query;
  query.ParseFromArray(src.data() + s_offset, src.size() - s_offset);

  if (!query.IsInitialized()) {
    LOG_GENERAL(WARNING, "Parse bytes into ProtoScillaQuery failed");
    return false;
  }

  if (LOG_SC) {
    LOG_GENERAL(INFO, "query for fetch: " << query.DebugString());
  }

  if (query.name() == FIELDS_MAP_DEPTH_INDICATOR || query.name() == SHARDING_INFO_INDICATOR) {
    LOG_GENERAL(WARNING, "query name is " << query.name());
    return false;
  }

  string key = addr.hex() + SCILLA_INDEX_SEPARATOR + query.name() +
               SCILLA_INDEX_SEPARATOR;

  ProtoScillaVal value;

  for (const auto& index : query.indices()) {
    key += index + SCILLA_INDEX_SEPARATOR;
  }

  if ((unsigned int)query.indices().size() > query.mapdepth()) {
    LOG_GENERAL(WARNING, "indices is deeper than map depth");
    return false;
  }

  auto d_found = t_indexToBeDeleted.find(key);
  if (d_found != t_indexToBeDeleted.end()) {
    // ignore the deleted empty placeholder
    if ((unsigned int)query.indices().size() == query.mapdepth()) {
      foundVal = false;
      return true;
    }
  }

  d_found = m_indexToBeDeleted.find(key);
  if (d_found != m_indexToBeDeleted.end() &&
      t_stateDataMap.find(key) == t_stateDataMap.end()) {
    // ignore the deleted empty placeholder
    if ((unsigned int)query.indices().size() == query.mapdepth()) {
      foundVal = false;
      return true;
    }
  }

  if ((unsigned int)query.indices().size() == query.mapdepth()) {
    // result will not be a map and can be just fetched into the store
    bytes bval;
    bool found = false;

    const auto& t_found = t_stateDataMap.find(key);
    if (t_found != t_stateDataMap.end()) {
      bval = t_found->second;
      found = true;
    }
    if (!found) {
      const auto& m_found = m_stateDataMap.find(key);
      if (m_found != m_stateDataMap.end()) {
        bval = m_found->second;
        found = true;
      }
    }
    if (!found) {
      if (m_stateDataDB.Exists(key)) {
        if (query.ignoreval()) {
          return true;
        }
        bval = DataConversion::StringToCharArray(m_stateDataDB.Lookup(key));
      } else {
        if (query.mapdepth() == 0) {
          // for non-map value, should be existing in db otherwise error
          return false;
        } else {
          // for in-map value, it's okay if cannot find
          foundVal = false;
          return true;
        }
      }
    }

    value.set_bval(bval.data(), bval.size());
    if (LOG_SC) {
      LOG_GENERAL(INFO, "value to fetch 1: " << value.DebugString());
    }
    return SerializeToArray(value, dst, 0);
  }

  // We're fetching a Map value. Need to iterate level-db lexicographically
  // first fetch from t_data, then m_data, lastly db
  auto p = t_stateDataMap.lower_bound(key);

  unordered_map<string, bytes> entries;

  while (p != t_stateDataMap.end() &&
         p->first.compare(0, key.size(), key) == 0) {
    if (query.ignoreval()) {
      return true;
    }
    entries.emplace(p->first, p->second);
    ++p;
  }

  p = m_stateDataMap.lower_bound(key);

  while (p != m_stateDataMap.end() &&
         p->first.compare(0, key.size(), key) == 0) {
    if (query.ignoreval()) {
      return true;
    }
    auto exist = entries.find(p->first);
    if (exist == entries.end()) {
      entries.emplace(p->first, p->second);
    }
    ++p;
  }

  std::unique_ptr<leveldb::Iterator> it(
      m_stateDataDB.GetDB()->NewIterator(leveldb::ReadOptions()));

  it->Seek({key});
  if (!it->Valid() || it->key().ToString().compare(0, key.size(), key) != 0) {
    // no entry
    if (entries.empty()) {
      foundVal = false;
      /// if querying the var without indices but still failed
      /// maybe trying to fetching an invalid vname
      /// as empty map will always have
      /// an empty serialized ProtoScillaVal placeholder
      /// so shouldn't be empty normally
      return !query.indices().empty();
    }
  } else {
    if (query.ignoreval()) {
      return true;
    }
    // found entries
    for (; it->Valid() && it->key().ToString().compare(0, key.size(), key) == 0;
         it->Next()) {
      auto exist = entries.find(it->key().ToString());
      if (exist == entries.end()) {
        bytes val(it->value().data(), it->value().data() + it->value().size());
        entries.emplace(it->key().ToString(), val);
      }
    }
  }

  set<string>::iterator isDeleted;

  uint32_t counter = 0;

  for (const auto& entry : entries) {
    isDeleted = t_indexToBeDeleted.find(entry.first);
    if (isDeleted != t_indexToBeDeleted.end()) {
      continue;
    }
    isDeleted = m_indexToBeDeleted.find(entry.first);
    if (isDeleted != m_indexToBeDeleted.end() &&
        t_stateDataMap.find(entry.first) == t_stateDataMap.end()) {
      continue;
    }

    counter++;

    std::vector<string> indices;
    // remove the prefixes, as shown below surrounded by []
    // [address.vname.index0.index1.(...).]indexN0.indexN1.(...).indexNn
    if (!boost::starts_with(entry.first, key)) {
      LOG_GENERAL(WARNING, "Key is not a prefix of stored entry");
      return false;
    }
    if (entry.first.size() > key.size()) {
      string key_non_prefix =
          entry.first.substr(key.size(), entry.first.size());
      boost::split(indices, key_non_prefix,
                   bind1st(std::equal_to<char>(), SCILLA_INDEX_SEPARATOR));
    }
    if (indices.size() > 0 && indices.back().empty()) indices.pop_back();

    ProtoScillaVal* t_value = &value;
    for (const auto& index : indices) {
      t_value = &(t_value->mutable_mval()->mutable_m()->operator[](index));
    }
    if (query.indices().size() + indices.size() < query.mapdepth()) {
      // Assert that we have a protobuf encoded empty map.
      ProtoScillaVal emap;
      emap.ParseFromArray(entry.second.data(), entry.second.size());
      if (!emap.has_mval() || !emap.mval().m().empty()) {
        LOG_GENERAL(WARNING,
                    "Expected protobuf encoded empty map since entry has fewer "
                    "keys than mapdepth");
        return false;
      }
      t_value->mutable_mval()->mutable_m();  // Create empty map.
    } else {
      t_value->set_bval(entry.second.data(), entry.second.size());
    }
  }

  if (!counter) {
    foundVal = false;
    return true;
  }

  if (LOG_SC) {
    LOG_GENERAL(INFO, "value to fetch 2: " << value.DebugString());
  }
  return SerializeToArray(value, dst, 0);
}

void ContractStorage2::DeleteByPrefix(const string& prefix) {
  auto p = t_stateDataMap.lower_bound(prefix);
  while (p != t_stateDataMap.end() &&
         p->first.compare(0, prefix.size(), prefix) == 0) {
    t_indexToBeDeleted.emplace(p->first);
    ++p;
  }

  p = m_stateDataMap.lower_bound(prefix);
  while (p != m_stateDataMap.end() &&
         p->first.compare(0, prefix.size(), prefix) == 0) {
    t_indexToBeDeleted.emplace(p->first);
    ++p;
  }

  std::unique_ptr<leveldb::Iterator> it(
      m_stateDataDB.GetDB()->NewIterator(leveldb::ReadOptions()));

  it->Seek({prefix});
  if (!it->Valid() ||
      it->key().ToString().compare(0, prefix.size(), prefix) != 0) {
    // no entry
  } else {
    for (; it->Valid() &&
           it->key().ToString().compare(0, prefix.size(), prefix) == 0;
         it->Next()) {
      t_indexToBeDeleted.emplace(it->key().ToString());
    }
  }
}

void ContractStorage2::DeleteByIndex(const string& index) {
  auto p = t_stateDataMap.find(index);
  if (p != t_stateDataMap.end()) {
    if (LOG_SC) {
      LOG_GENERAL(INFO, "delete index from t: " << index);
    }
    t_indexToBeDeleted.emplace(index);
    return;
  }

  p = m_stateDataMap.find(index);
  if (p != m_stateDataMap.end()) {
    if (LOG_SC) {
      LOG_GENERAL(INFO, "delete index from m: " << index);
    }
    t_indexToBeDeleted.emplace(index);
    return;
  }

  if (m_stateDataDB.Exists(index)) {
    if (LOG_SC) {
      LOG_GENERAL(INFO, "delete index from db: " << index);
    }
    t_indexToBeDeleted.emplace(index);
  }
}

bool ContractStorage2::FetchContractShardingInfo(const dev::h160& address,
                                                 Json::Value& sharding_info_json) {
  std::map<std::string, bytes> sharding_info;
  // GEORGE: Should this have temp = true?
  FetchStateDataForContract(sharding_info, address,
                            SHARDING_INFO_INDICATOR, {}, false);

  string sh_str;
  const auto key = address.hex() + SCILLA_INDEX_SEPARATOR +
                    SHARDING_INFO_INDICATOR + SCILLA_INDEX_SEPARATOR;
  if (sharding_info.size() == 1 &&
      sharding_info.find(key) != sharding_info.end()) {
    sh_str = DataConversion::CharArrayToString(sharding_info.at(key));
  } else {
    LOG_GENERAL(WARNING, "Cannot find SHARDING_INFO_INDICATOR");
    return false;
  }

  if (!sharding_info.empty() && !JSONUtils::GetInstance().convertStrtoJson(
                                    sh_str, sharding_info_json)) {
    return false;
  }
  return true;
}

bool ContractStorage2::FetchContractFieldsMapDepth(const dev::h160& address,
                                                   Json::Value& map_depth_json,
                                                   bool temp) {
  std::map<std::string, bytes> map_depth_data_in_map;
  string map_depth_data;
  FetchStateDataForContract(map_depth_data_in_map, address,
                            FIELDS_MAP_DEPTH_INDICATOR, {}, temp);

  /// check the data obtained from storage
  if (map_depth_data_in_map.size() == 1 &&
      map_depth_data_in_map.find(
          address.hex() + SCILLA_INDEX_SEPARATOR + FIELDS_MAP_DEPTH_INDICATOR +
          SCILLA_INDEX_SEPARATOR) != map_depth_data_in_map.end()) {
    map_depth_data = DataConversion::CharArrayToString(map_depth_data_in_map.at(
        address.hex() + SCILLA_INDEX_SEPARATOR + FIELDS_MAP_DEPTH_INDICATOR +
        SCILLA_INDEX_SEPARATOR));
  } else {
    LOG_GENERAL(WARNING, "Cannot find FIELDS_MAP_DEPTH_INDICATOR");
    return false;
  }

  if (!map_depth_data.empty() && !JSONUtils::GetInstance().convertStrtoJson(
                                     map_depth_data, map_depth_json)) {
    LOG_GENERAL(WARNING, "Cannot parse " << map_depth_data << " to JSON");
    return false;
  }
  return true;
}

void UnquoteString(string& input) {
  if (input.empty()) {
    return;
  }
  if (input.front() == '"') {
    input.erase(0, 1);
  }
  if (input.back() == '"') {
    input.pop_back();
  }
}

void ContractStorage2::InsertValueToStateJson(Json::Value& _json, string key,
                                              string value, bool unquote,
                                              bool nokey) {
  if (unquote) {
    // unquote key
    UnquoteString(key);
  }

  Json::Value j_value;

  if (JSONUtils::GetInstance().convertStrtoJson(value, j_value) &&
      (j_value.type() == Json::arrayValue ||
       j_value.type() == Json::objectValue)) {
    if (nokey) {
      _json = j_value;
    } else {
      if (unquote && !nokey) {
        UnquoteString(value);
      }
      _json[key] = j_value;
    }
  } else {
    if (nokey) {
      _json = j_value;
    } else {
      if (unquote && !nokey) {
        UnquoteString(value);
      }
      _json[key] = value;
    }
  }
}

bool ContractStorage2::FetchStateJsonForContract(Json::Value& _json,
                                                 const dev::h160& address,
                                                 const string& vname,
                                                 const vector<string>& indices,
                                                 bool temp) {
  lock_guard<mutex> g(m_stateDataMutex);

  std::map<std::string, bytes> states;
  FetchStateDataForContract(states, address, vname, indices, temp);

  /// get the map depth
  Json::Value map_depth_json;
  if (!FetchContractFieldsMapDepth(address, map_depth_json, temp)) {
    LOG_GENERAL(WARNING, "FetchContractFieldsMapDepth failed for contract: "
                             << address.hex());
  }

  for (const auto& state : states) {
    vector<string> fragments;
    boost::split(fragments, state.first,
                 bind1st(std::equal_to<char>(), SCILLA_INDEX_SEPARATOR));
    if (fragments.at(0) != address.hex()) {
      LOG_GENERAL(WARNING, "wrong state fetched: " << state.first);
      return false;
    }
    if (fragments.back().empty()) fragments.pop_back();

    string vname = fragments.at(1);

    if (vname == FIELDS_MAP_DEPTH_INDICATOR || vname == SHARDING_INFO_INDICATOR) {
      continue;
    }

    /// addr+vname+[indices...]
    vector<string> map_indices(fragments.begin() + 2, fragments.end());

    std::function<void(Json::Value&, const vector<string>&, const bytes&,
                       unsigned int, int)>
        jsonMapWrapper = [&](Json::Value& _json, const vector<string>& indices,
                             const bytes& value, unsigned int cur_index,
                             int mapdepth) -> void {
      if (cur_index + 1 < indices.size()) {
        string key = indices.at(cur_index);
        UnquoteString(key);
        jsonMapWrapper(_json[key], indices, value, cur_index + 1, mapdepth);
      } else {
        if (mapdepth > 0) {
          if ((int)indices.size() == mapdepth) {
            InsertValueToStateJson(_json, indices.at(cur_index),
                                   DataConversion::CharArrayToString(value));
          } else {
            if (indices.empty()) {
              _json = Json::objectValue;
            } else {
              string key = indices.at(cur_index);
              UnquoteString(key);
              _json[key] = Json::objectValue;
            }
          }
        } else if (mapdepth == 0) {
          InsertValueToStateJson(
              _json, "", DataConversion::CharArrayToString(value), true, true);
        } else {
          /// Enters only when the fields_map_depth not available, almost
          /// impossible Check value whether parsable to Protobuf
          ProtoScillaVal empty_val;
          if (empty_val.ParseFromArray(value.data(), value.size()) &&
              empty_val.IsInitialized() && empty_val.has_mval() &&
              empty_val.mval().m().empty()) {
            string key = indices.at(cur_index);
            UnquoteString(key);
            _json[key] = Json::objectValue;
          } else {
            InsertValueToStateJson(_json, indices.at(cur_index),
                                   DataConversion::CharArrayToString(value));
          }
        }
      }
    };

    jsonMapWrapper(_json[vname], map_indices, state.second, 0,
                   (!map_depth_json.empty() && map_depth_json.isMember(vname))
                       ? map_depth_json[vname].asInt()
                       : -1);
  }

  return true;
}

void ContractStorage2::FetchStateDataForKey(map<string, bytes>& states,
                                            const string& key, bool temp) {
  std::map<std::string, bytes>::iterator p;
  if (temp) {
    p = t_stateDataMap.lower_bound(key);
    while (p != t_stateDataMap.end() &&
           p->first.compare(0, key.size(), key) == 0) {
      states.emplace(p->first, p->second);
      ++p;
    }
  }

  p = m_stateDataMap.lower_bound(key);
  while (p != m_stateDataMap.end() &&
         p->first.compare(0, key.size(), key) == 0) {
    if (states.find(p->first) == states.end()) {
      states.emplace(p->first, p->second);
    }
    ++p;
  }

  std::unique_ptr<leveldb::Iterator> it(
      m_stateDataDB.GetDB()->NewIterator(leveldb::ReadOptions()));

  it->Seek({key});
  if (!it->Valid() || it->key().ToString().compare(0, key.size(), key) != 0) {
    // no entry
  } else {
    for (; it->Valid() && it->key().ToString().compare(0, key.size(), key) == 0;
         it->Next()) {
      if (states.find(it->key().ToString()) == states.end()) {
        bytes val(it->value().data(), it->value().data() + it->value().size());
        states.emplace(it->key().ToString(), val);
      }
    }
  }

  if (temp) {
    for (auto it = states.begin(); it != states.end();) {
      if (t_indexToBeDeleted.find(it->first) != t_indexToBeDeleted.cend()) {
        it = states.erase(it);
      } else {
        it++;
      }
    }
  }

  for (auto it = states.begin(); it != states.end();) {
    if (m_indexToBeDeleted.find(it->first) != m_indexToBeDeleted.cend() &&
        ((temp && t_stateDataMap.find(it->first) == t_stateDataMap.end()) ||
         !temp)) {
      it = states.erase(it);
    } else {
      it++;
    }
  }
}

void ContractStorage2::FetchStateDataForContract(map<string, bytes>& states,
                                                 const dev::h160& address,
                                                 const string& vname,
                                                 const vector<string>& indices,
                                                 bool temp) {
  string key = GenerateStorageKey(address, vname, indices);
  FetchStateDataForKey(states, key, temp);
}

void ContractStorage2::FetchUpdatedStateValuesForAddress(
    const dev::h160& address, map<string, bytes>& t_states,
    vector<std::string>& toDeletedIndices, bool temp) {
  if (LOG_SC) {
    LOG_MARKER();
  }

  lock_guard<mutex> g(m_stateDataMutex);

  if (address == dev::h160()) {
    LOG_GENERAL(WARNING, "address provided is empty");
    return;
  }

  if (temp) {
    auto p = t_stateDataMap.lower_bound(address.hex());
    while (p != t_stateDataMap.end() &&
           p->first.compare(0, address.hex().size(), address.hex()) == 0) {
      t_states.emplace(p->first, p->second);
      ++p;
    }

    auto r = t_indexToBeDeleted.lower_bound(address.hex());
    while (r != t_indexToBeDeleted.end() &&
           r->compare(0, address.hex().size(), address.hex()) == 0) {
      toDeletedIndices.emplace_back(*r);
      ++r;
    }
  } else {
    auto p = m_stateDataMap.lower_bound(address.hex());
    while (p != m_stateDataMap.end() &&
           p->first.compare(0, address.hex().size(), address.hex()) == 0) {
      if (t_states.find(p->first) == t_states.end()) {
        t_states.emplace(p->first, p->second);
      }
      ++p;
    }

    std::unique_ptr<leveldb::Iterator> it(
        m_stateDataDB.GetDB()->NewIterator(leveldb::ReadOptions()));

    it->Seek({address.hex()});
    if (!it->Valid() || it->key().ToString().compare(0, address.hex().size(),
                                                     address.hex()) != 0) {
      // no entry
    } else {
      for (; it->Valid() && it->key().ToString().compare(
                                0, address.hex().size(), address.hex()) == 0;
           it->Next()) {
        if (t_states.find(it->key().ToString()) == t_states.end()) {
          bytes val(it->value().data(),
                    it->value().data() + it->value().size());
          t_states.emplace(it->key().ToString(), val);
        }
      }
    }

    auto r = m_indexToBeDeleted.lower_bound(address.hex());
    while (r != m_indexToBeDeleted.end() &&
           r->compare(0, address.hex().size(), address.hex()) == 0) {
      toDeletedIndices.emplace_back(*r);
      ++r;
    }
  }
}

bool ContractStorage2::CleanEmptyMapPlaceholders(const string& key) {
  // key = 0xabc.vname.[index1.index2.[...].indexn.
  vector<string> indices;
  boost::split(indices, key,
               bind1st(std::equal_to<char>(), SCILLA_INDEX_SEPARATOR));
  if (indices.size() < 2) {
    LOG_GENERAL(WARNING, "indices size too small: " << indices.size());
    return false;
  }
  if (indices.back().empty()) indices.pop_back();

  string scankey = indices.at(0) + SCILLA_INDEX_SEPARATOR + indices.at(1) +
                   SCILLA_INDEX_SEPARATOR;
  DeleteByIndex(scankey);  // clean root level

  for (unsigned int i = 2; i < indices.size() - 1 /*exclude the value key*/;
       ++i) {
    scankey += indices.at(i) + SCILLA_INDEX_SEPARATOR;
    DeleteByIndex(scankey);
  }
  return true;
}

void ContractStorage2::UpdateStateData(const string& key, const bytes& value,
                                       bool cleanEmpty) {
  if (LOG_SC) {
    LOG_GENERAL(INFO, "key: " << key << " value: "
                              << DataConversion::CharArrayToString(value));
  }

  if (cleanEmpty) {
    CleanEmptyMapPlaceholders(key);
  }

  auto pos = t_indexToBeDeleted.find(key);
  if (pos != t_indexToBeDeleted.end()) {
    t_indexToBeDeleted.erase(pos);
  }

  t_stateDataMap[key] = value;
}

bool ContractStorage2::UpdateStateValue(const dev::h160& addr, const bytes& q,
                                        unsigned int q_offset, const bytes& v,
                                        unsigned int v_offset) {
  if (LOG_SC) {
    LOG_MARKER();
  }

  lock_guard<mutex> g(m_stateDataMutex);

  if (q_offset > q.size()) {
    LOG_GENERAL(WARNING, "Invalid query data and offset, data size "
                             << q.size() << ", offset " << q_offset);
    return false;
  }

  if (v_offset > v.size()) {
    LOG_GENERAL(WARNING, "Invalid value data and offset, data size "
                             << v.size() << ", offset " << v_offset);
  }

  ProtoScillaQuery query;
  query.ParseFromArray(q.data() + q_offset, q.size() - q_offset);

  if (!query.IsInitialized()) {
    LOG_GENERAL(WARNING, "Parse bytes into ProtoScillaQuery failed");
    return false;
  }

  ProtoScillaVal value;
  value.ParseFromArray(v.data() + v_offset, v.size() - v_offset);

  if (!value.IsInitialized()) {
    LOG_GENERAL(WARNING, "Parse bytes into ProtoScillaVal failed");
    return false;
  }

  if (query.name() == FIELDS_MAP_DEPTH_INDICATOR || query.name() == SHARDING_INFO_INDICATOR) {
    LOG_GENERAL(WARNING, "query name is " << query.name());
    return false;
  }

  string key = addr.hex() + SCILLA_INDEX_SEPARATOR + query.name() +
               SCILLA_INDEX_SEPARATOR;

  if (query.ignoreval()) {
    if (query.indices().size() < 1) {
      LOG_GENERAL(WARNING, "indices cannot be empty")
      return false;
    }
    for (int i = 0; i < query.indices().size() - 1; ++i) {
      key += query.indices().Get(i) + SCILLA_INDEX_SEPARATOR;
    }
    string parent_key = key;
    key += query.indices().Get(query.indices().size() - 1) +
           SCILLA_INDEX_SEPARATOR;
    if (LOG_SC) {
      LOG_GENERAL(INFO, "Delete key: " << key);
    }
    DeleteByPrefix(key);

    map<string, bytes> t_states;
    FetchStateDataForKey(t_states, parent_key, true);
    if (t_states.empty()) {
      ProtoScillaVal empty_val;
      empty_val.mutable_mval()->mutable_m();
      bytes dst;
      if (!SerializeToArray(empty_val, dst, 0)) {
        LOG_GENERAL(WARNING, "empty_mval SerializeToArray failed");
        return false;
      }
      UpdateStateData(parent_key, dst);
    }
  } else {
    for (const auto& index : query.indices()) {
      key += index + SCILLA_INDEX_SEPARATOR;
    }

    if ((unsigned int)query.indices().size() > query.mapdepth()) {
      LOG_GENERAL(WARNING, "indices is deeper than map depth");
      return false;
    } else if ((unsigned int)query.indices().size() == query.mapdepth()) {
      if (value.has_mval()) {
        LOG_GENERAL(WARNING, "val is not bytes but supposed to be");
        return false;
      }
      UpdateStateData(key, DataConversion::StringToCharArray(value.bval()),
                      true);
      return true;
    } else {
      DeleteByPrefix(key);

      std::function<bool(const string&, const ProtoScillaVal&)> mapHandler =
          [&](const string& keyAcc, const ProtoScillaVal& value) -> bool {
        if (!value.has_mval()) {
          LOG_GENERAL(WARNING, "val is not map but supposed to be");
          return false;
        }
        if (value.mval().m().empty()) {
          // We have an empty map. Insert an entry for keyAcc in
          // the store to indicate that the key itself exists.
          bytes dst;
          if (!SerializeToArray(value, dst, 0)) {
            return false;
          }
          // DB Put
          UpdateStateData(keyAcc, dst, true);
          return true;
        }
        for (const auto& entry : value.mval().m()) {
          string index(keyAcc);
          index += entry.first + SCILLA_INDEX_SEPARATOR;
          if (entry.second.has_mval()) {
            // We haven't reached the deepeast nesting
            if (!mapHandler(index, entry.second)) {
              return false;
            }
          } else {
            // DB Put
            if (LOG_SC) {
              LOG_GENERAL(INFO, "mval().m() first: " << entry.first
                                                     << " second: "
                                                     << entry.second.bval());
            }
            UpdateStateData(
                index, DataConversion::StringToCharArray(entry.second.bval()),
                true);
          }
        }
        return true;
      };

      return mapHandler(key, value);
    }
  }
  return true;
}

void ContractStorage2::UpdateStateDatasAndToDeletes(
    const dev::h160& addr, const std::map<std::string, bytes>& t_states,
    const std::vector<std::string>& toDeleteIndices, dev::h256& stateHash,
    bool temp, bool revertible,
    const uint32_t& shardId, const uint32_t& numShards) {
  if (LOG_SC) {
    LOG_MARKER();
  }

  lock_guard<mutex> g(m_stateDataMutex);

  // This function is used in several ways, which are not immediately obvious
  // by just looking at the function's code. In particular, the function is used to:
  //    1) merge a contribution from a particular shard (temp && shardId != UNKNOWN_SHARD_ID)
  //    2) overwrite the existing tempAccountStore with a new one, i.e. a contribution from
  //        an unknown shard or more than one shard (temp && shard == UNKNOWN_SHARD_ID)
  //    3) commit some sets into the permanent account store (!temp)
  // In case (1), we perform a three-way merge according to addr's sharding_info.
  if (temp) {
    Json::Value sh_info;
    auto& cs = Contract::ContractStorage2::GetContractStorage();
    std::chrono::system_clock::time_point tpStart;

    if (ENABLE_CHECK_PERFORMANCE_LOG) {
        tpStart = r_timer_start();
    }

    // Case (1) -- three-way merge for a contract with sharding info
    if (SEMANTIC_SHARDING && t_states.size() > 0 && shardId != UNKNOWN_SHARD_ID
        && numShards != UNKNOWN_SHARD_ID
        && cs.FetchContractShardingInfo(addr, sh_info)) {
      std::chrono::system_clock::time_point genStart;
      std::chrono::system_clock::time_point callStart;
      std::chrono::system_clock::time_point writeStart;

      double genTime, callTime;

      if (ENABLE_CHECK_PERFORMANCE_LOG) {
        genStart = r_timer_start();
      }

      Json::Value merge_req = Json::objectValue;
      merge_req["req_type"] = "join";
      merge_req["shard_id"] = shardId;
      merge_req["contract_shard"] = AddressShardIndex(addr, numShards);
      merge_req["num_shards"] = numShards;
      merge_req["sharding_info"] = sh_info;
      merge_req["states"] = Json::objectValue;

      for (const auto& state : t_states) {
        std::map<string, bytes> ancestor_m, temp_m;
        FetchStateDataForKey(ancestor_m, state.first, false);
        FetchStateDataForKey(temp_m, state.first, true);
        auto ancestor = ancestor_m[state.first];
        auto temp = temp_m[state.first];
        auto shard = state.second;

        Json::Value st = Json::objectValue;
        st["ancestor"] = DataConversion::CharArrayToString(ancestor);
        st["temp"] = DataConversion::CharArrayToString(temp);
        st["shard"] = DataConversion::CharArrayToString(shard);

        merge_req["states"][state.first] = st;
      }

    string req_str = JSONUtils::GetInstance().convertJsontoStr(merge_req);
    Json::Value req = Json::objectValue;
    req["req"] = req_str;

    if (ENABLE_CHECK_PERFORMANCE_LOG) {
      genTime = r_timer_end(genStart);
      callStart = r_timer_start();
    }

    // Ensure we call the merger for the appropriate Scilla version
    Account* acc = AccountStore::GetInstance().GetAccount(addr);
    uint32_t scilla_version;
    string result = "";
    bool call_succeeded =
      acc->GetScillaVersion(scilla_version) &&
      ScillaClient::GetInstance().CallSharding(scilla_version, req, result);

    if (ENABLE_CHECK_PERFORMANCE_LOG) {
      callTime = r_timer_end(callStart);
      writeStart = r_timer_start();
    }
    if (LOG_SC) {
      LOG_GENERAL(INFO, "Merge request\n" << req_str << "\nResponse:\n" << result);
    }

    // TODO: is there any recovery option if this fails?
    Json::Value resp;
    if (call_succeeded
        && JSONUtils::GetInstance().convertStrtoJson(result, resp)
        && resp.isMember("states")) {
      for (const auto& state_key : resp["states"].getMemberNames()) {
        bytes state_value = DataConversion::StringToCharArray(resp["states"][state_key].asString());
        t_stateDataMap[state_key] = state_value;
      }
    }
    else {
      LOG_GENERAL(FATAL, "Merge request failed!");
    }

    if (ENABLE_CHECK_PERFORMANCE_LOG) {
      string timing_str = "";
      if (resp.isMember("timing") && resp["timing"].isString()) {
        timing_str = resp["timing"].asString();
      }

      LOG_GENERAL(INFO, "Merged " << t_states.size() << " account deltas in "
                        << r_timer_end(tpStart) << " microseconds"
                        << " (Serialize: " << genTime << ", Call: " << callTime
                        << " [" << timing_str << "]"
                        << ", Write: " << r_timer_end(writeStart) << ")" );
    }
    // Case (2) -- overwrite
    } else {
      for (const auto& state : t_states) {
        t_stateDataMap[state.first] = state.second;
        auto pos = t_indexToBeDeleted.find(state.first);
        if (pos != t_indexToBeDeleted.end()) {
          t_indexToBeDeleted.erase(pos);
        }
      }

      if (ENABLE_CHECK_PERFORMANCE_LOG) {
        LOG_GENERAL(INFO, "Merged " << t_states.size() << " account deltas in "
                          << r_timer_end(tpStart) << " microseconds");
      }
    }

    for (const auto& index : toDeleteIndices) {
      t_indexToBeDeleted.emplace(index);
    }
  // Case (3) -- commit / overwrite
  } else {
    for (const auto& state : t_states) {
      if (revertible) {
        if (m_stateDataMap.find(state.first) != m_stateDataMap.end()) {
          r_stateDataMap[state.first] = m_stateDataMap[state.first];
        } else {
          r_stateDataMap[state.first] = {};
        }
      }
      if (LOG_SC) {
        LOG_GENERAL(INFO, "Commit " <<
            "state key: " << state.first <<
            " old: " << DataConversion::CharArrayToString(m_stateDataMap[state.first]) <<
            " new: " << DataConversion::CharArrayToString(state.second));
      }

      m_stateDataMap[state.first] = state.second;
      auto pos = m_indexToBeDeleted.find(state.first);
      if (pos != m_indexToBeDeleted.end()) {
        m_indexToBeDeleted.erase(pos);
        if (revertible) {
          r_indexToBeDeleted.emplace(state.first, false);
        }
      }
    }
    for (const auto& toDelete : toDeleteIndices) {
      if (revertible) {
        r_indexToBeDeleted.emplace(toDelete, true);
      }
      m_indexToBeDeleted.emplace(toDelete);
    }
  }

  stateHash = GetContractStateHash(addr, temp);
}

void ContractStorage2::BufferCurrentState() {
  LOG_MARKER();
  lock_guard<mutex> g(m_stateDataMutex);
  p_stateDataMap = t_stateDataMap;
  p_indexToBeDeleted = m_indexToBeDeleted;
}

void ContractStorage2::RevertPrevState() {
  LOG_MARKER();
  lock_guard<mutex> g(m_stateDataMutex);
  t_stateDataMap = std::move(p_stateDataMap);
  m_indexToBeDeleted = std::move(p_indexToBeDeleted);
}

void ContractStorage2::RevertContractStates() {
  LOG_MARKER();
  lock_guard<mutex> g(m_stateDataMutex);

  for (const auto& data : r_stateDataMap) {
    if (data.second.empty()) {
      m_stateDataMap.erase(data.first);
    } else {
      m_stateDataMap[data.first] = data.second;
    }
  }

  for (const auto& index : r_indexToBeDeleted) {
    if (index.second) {
      // revert newly added indexToBeDeleted
      const auto& found = m_indexToBeDeleted.find(index.first);
      if (found != m_indexToBeDeleted.end()) {
        m_indexToBeDeleted.erase(found);
      }
    } else {
      // revert newly deleted indexToBeDeleted
      m_indexToBeDeleted.emplace(index.first);
    }
  }
}

void ContractStorage2::InitRevertibles() {
  LOG_MARKER();
  lock_guard<mutex> g(m_stateDataMutex);
  r_stateDataMap.clear();
  r_indexToBeDeleted.clear();
}

bool ContractStorage2::CommitStateDB() {
  LOG_MARKER();
  lock_guard<mutex> g(m_stateDataMutex);
  // copy everything into m_stateXXDB;
  // Index
  unordered_map<string, std::string> batch;
  unordered_map<string, std::string> reset_buffer;

  batch.clear();
  // Data
  for (const auto& i : m_stateDataMap) {
    batch.insert({i.first, DataConversion::CharArrayToString(i.second)});
  }
  if (!m_stateDataDB.BatchInsert(batch)) {
    LOG_GENERAL(WARNING, "BatchInsert m_stateDataDB failed");
    return false;
  }
  // ToDelete
  for (const auto& index : m_indexToBeDeleted) {
    if (m_stateDataDB.DeleteKey(index) < 0) {
      LOG_GENERAL(WARNING, "DeleteKey " << index << " failed");
      return false;
    }
  }

  m_stateDataMap.clear();
  m_indexToBeDeleted.clear();

  InitTempState();

  return true;
}

void ContractStorage2::InitTempStateCore() {
  t_stateDataMap.clear();
  t_indexToBeDeleted.clear();
}

void ContractStorage2::InitTempState(bool callFromExternal) {
  LOG_MARKER();

  if (callFromExternal) {
    lock_guard<mutex> g(m_stateDataMutex);
    InitTempStateCore();
  } else {
    InitTempStateCore();
  }
}

dev::h256 ContractStorage2::GetContractStateHashCore(const dev::h160& address,
                                                     bool temp) {
  if (IsNullAddress(address)) {
    LOG_GENERAL(WARNING, "Null address rejected");
    return dev::h256();
  }

  std::map<std::string, bytes> states;
  FetchStateDataForContract(states, address, "", {}, temp);

  // iterate the raw protobuf string and hash
  SHA2<HashType::HASH_VARIANT_256> sha2;
  for (const auto& state : states) {
    if (LOG_SC) {
      LOG_GENERAL(INFO, "state key: "
                            << state.first << " value: "
                            << DataConversion::CharArrayToString(state.second));
    }
    sha2.Update(DataConversion::StringToCharArray(state.first));
    if (!state.second.empty()) {
      sha2.Update(state.second);
    }
  }
  // return dev::h256(sha2.Finalize());
  dev::h256 ret(sha2.Finalize());
  return ret;
}

dev::h256 ContractStorage2::GetContractStateHash(const dev::h160& address,
                                                 bool temp,
                                                 bool callFromExternal) {
  if (LOG_SC) {
    LOG_MARKER();
  }

  if (callFromExternal) {
    lock_guard<mutex> g(m_stateDataMutex);
    return GetContractStateHashCore(address, temp);
  } else {
    return GetContractStateHashCore(address, temp);
  }
}

void ContractStorage2::Reset() {
  {
    lock_guard<mutex> g(m_codeMutex);
    m_codeDB.ResetDB();
  }
  {
    lock_guard<mutex> g(m_initDataMutex);
    m_initDataDB.ResetDB();
  }
  {
    lock_guard<mutex> g(m_stateDataMutex);
    m_stateDataDB.ResetDB();

    p_stateDataMap.clear();
    p_indexToBeDeleted.clear();

    t_stateDataMap.clear();
    t_indexToBeDeleted.clear();

    r_stateDataMap.clear();
    r_indexToBeDeleted.clear();

    m_stateDataMap.clear();
    m_indexToBeDeleted.clear();
  }
}

bool ContractStorage2::RefreshAll() {
  bool ret;
  {
    lock_guard<mutex> g(m_codeMutex);
    ret = m_codeDB.RefreshDB();
  }
  if (ret) {
    lock_guard<mutex> g(m_initDataMutex);
    ret = m_initDataDB.RefreshDB();
  }
  if (ret) {
    lock_guard<mutex> g(m_stateDataMutex);
    ret = m_stateDataDB.RefreshDB();
  }
  return ret;
}

}  // namespace Contract
