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

#include <leveldb/db.h>

#include "AccountStore.h"
#include "libCrypto/Sha2.h"
#include "libMessage/Messenger.h"
#include "libPersistence/BlockStorage.h"
#include "libPersistence/ContractStorage.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "libPersistence/ScillaMessage.pb.h"
#pragma GCC diagnostic pop
#include "libServer/ScillaIPCServer.h"
#include "libUtils/SysCommand.h"

using namespace std;
using namespace dev;
using namespace boost::multiprecision;
using namespace Contract;

AccountStore::AccountStore() {
  m_accountStoreTemp = make_unique<AccountStoreTemp>(*this);

  if (ENABLE_SC) {
    /// Scilla IPC Server
    /// clear path
    boost::filesystem::remove_all(SCILLA_IPC_SOCKET_PATH);
    m_scillaIPCServerConnector =
        make_unique<jsonrpc::UnixDomainSocketServer>(SCILLA_IPC_SOCKET_PATH);
    m_scillaIPCServer =
        make_shared<ScillaIPCServer>(*m_scillaIPCServerConnector);
    ScillaClient::GetInstance().Init();
    if (m_scillaIPCServer == nullptr) {
      LOG_GENERAL(WARNING, "m_scillaIPCServer NULL");
    } else {
      m_accountStoreTemp->SetScillaIPCServer(m_scillaIPCServer);
      if (m_scillaIPCServer->StartListening()) {
        LOG_GENERAL(INFO, "Scilla IPC Server started successfully");
      } else {
        LOG_GENERAL(WARNING, "Scilla IPC Server couldn't start")
      }
    }
  }
}

AccountStore::~AccountStore() {
  // boost::filesystem::remove_all("./state");
  if (m_scillaIPCServer != nullptr) {
    m_scillaIPCServer->StopListening();
  }
}

void AccountStore::Init() {
  LOG_MARKER();

  InitSoft();

  lock_guard<mutex> g(m_mutexDB);

  ContractStorage2::GetContractStorage().Reset();
  m_db.ResetDB();
}

void AccountStore::InitSoft() {
  LOG_MARKER();

  unique_lock<shared_timed_mutex> g(m_mutexPrimary);

  AccountStoreTrie<OverlayDB, unordered_map<Address, Account>>::Init();

  InitRevertibles();

  InitTemp();
}

bool AccountStore::RefreshDB() {
  LOG_MARKER();
  lock_guard<mutex> g(m_mutexDB);
  return m_db.RefreshDB();
}

void AccountStore::InitTemp() {
  LOG_MARKER();

  lock_guard<mutex> g(m_mutexDelta);

  m_accountStoreTemp->Init();
  m_stateDeltaSerialized.clear();

  ContractStorage2::GetContractStorage().InitTempState(true);
}

void AccountStore::InitRevertibles() {
  LOG_MARKER();

  lock_guard<mutex> g(m_mutexRevertibles);

  m_addressToAccountRevChanged.clear();
  m_addressToAccountRevCreated.clear();

  ContractStorage2::GetContractStorage().InitRevertibles();
}

AccountStore& AccountStore::GetInstance() {
  static AccountStore accountstore;
  return accountstore;
}

bool AccountStore::Serialize(bytes& src, unsigned int offset) const {
  LOG_MARKER();
  shared_lock<shared_timed_mutex> lock(m_mutexPrimary);
  return AccountStoreTrie<
      dev::OverlayDB, std::unordered_map<Address, Account>>::Serialize(src,
                                                                       offset);
}

bool AccountStore::Deserialize(const bytes& src, unsigned int offset) {
  LOG_MARKER();

  this->Init();

  unique_lock<shared_timed_mutex> g(m_mutexPrimary);

  if (!Messenger::GetAccountStore(src, offset, *this)) {
    LOG_GENERAL(WARNING, "Messenger::GetAccountStore failed.");
    return false;
  }

  return true;
}

bool AccountStore::SerializeDelta() {
  LOG_MARKER();

  unique_lock<mutex> g(m_mutexDelta, defer_lock);
  shared_lock<shared_timed_mutex> g2(m_mutexPrimary, defer_lock);
  lock(g, g2);

  m_stateDeltaSerialized.clear();

  if (!Messenger::SetAccountStoreDelta(m_stateDeltaSerialized, 0,
                                       *m_accountStoreTemp, *this)) {
    LOG_GENERAL(WARNING, "Messenger::SetAccountStoreDelta failed.");
    return false;
  }

  return true;
}

void AccountStore::GetSerializedDelta(bytes& dst) {
  lock_guard<mutex> g(m_mutexDelta);

  dst.clear();

  copy(m_stateDeltaSerialized.begin(), m_stateDeltaSerialized.end(),
       back_inserter(dst));
}

bool AccountStore::DeserializeDelta(const bytes& src, unsigned int offset,
                                    bool revertible) {
  LOG_MARKER();

  if (revertible) {
    unique_lock<shared_timed_mutex> g(m_mutexPrimary, defer_lock);
    unique_lock<mutex> g2(m_mutexRevertibles, defer_lock);
    lock(g, g2);

    if (!Messenger::GetAccountStoreDelta(src, offset, *this, revertible,
                                         false)) {
      LOG_GENERAL(WARNING, "Messenger::GetAccountStoreDelta failed.");
      return false;
    }
  } else {
    unique_lock<shared_timed_mutex> g(m_mutexPrimary);

    if (!Messenger::GetAccountStoreDelta(src, offset, *this, revertible,
                                         false)) {
      LOG_GENERAL(WARNING, "Messenger::GetAccountStoreDelta failed.");
      return false;
    }
  }

  return true;
}

bool AccountStore::DeserializeDeltaTemp(const bytes& src, unsigned int offset,
    const uint32_t& shardId, const uint32_t& numShards) {
  lock_guard<mutex> g(m_mutexDelta);
  return m_accountStoreTemp->DeserializeDelta(src, offset, shardId, numShards);
}

bool AccountStore::MoveRootToDisk(const h256& root) {
  // convert h256 to bytes
  if (!BlockStorage::GetBlockStorage().PutStateRoot(root.asBytes())) {
    LOG_GENERAL(INFO, "FAIL: Put state root failed " << root.hex());
    return false;
  }
  return true;
}

bool AccountStore::MoveUpdatesToDisk() {
  LOG_MARKER();

  unique_lock<shared_timed_mutex> g(m_mutexPrimary, defer_lock);
  unique_lock<mutex> g2(m_mutexDB, defer_lock);
  lock(g, g2);

  unordered_map<string, string> code_batch;
  unordered_map<string, string> initdata_batch;

  for (const auto& i : *m_addressToAccount) {
    if (i.second.isContract()) {
      if (ContractStorage2::GetContractStorage()
              .GetContractCode(i.first)
              .empty()) {
        code_batch.insert({i.first.hex(), DataConversion::CharArrayToString(
                                              i.second.GetCode())});
      }

      if (ContractStorage2::GetContractStorage().GetInitData(i.first).empty()) {
        initdata_batch.insert({i.first.hex(), DataConversion::CharArrayToString(
                                                  i.second.GetInitData())});
      }
    }
  }

  if (!ContractStorage2::GetContractStorage().PutContractCodeBatch(
          code_batch)) {
    LOG_GENERAL(WARNING, "PutContractCodeBatch failed");
    return false;
  }

  if (!ContractStorage2::GetContractStorage().PutInitDataBatch(
          initdata_batch)) {
    LOG_GENERAL(WARNING, "PutInitDataBatch failed");
    return false;
  }

  bool ret = true;

  if (ret) {
    if (!ContractStorage2::GetContractStorage().CommitStateDB()) {
      LOG_GENERAL(WARNING,
                  "CommitTempStateDB failed. need to rever the change on "
                  "ContractCode");
      ret = false;
    }
  }

  if (!ret) {
    for (const auto& it : code_batch) {
      if (!ContractStorage2::GetContractStorage().DeleteContractCode(
              h160(it.first))) {
        LOG_GENERAL(WARNING, "Failed to delete contract code for " << it.first);
      }
    }
  }

  try {
    lock_guard<mutex> g(m_mutexTrie);
    if (!m_state.db()->commit()) {
      LOG_GENERAL(WARNING, "LevelDB commit failed");
    }
    if (!MoveRootToDisk(m_state.root())) {
      LOG_GENERAL(WARNING, "MoveRootToDisk failed " << m_state.root().hex());
      return false;
    }
    m_prevRoot = m_state.root();
  } catch (const boost::exception& e) {
    LOG_GENERAL(WARNING, "Error with AccountStore::MoveUpdatesToDisk. "
                             << boost::diagnostic_information(e));
    return false;
  }

  m_addressToAccount->clear();

  return true;
}

bool AccountStore::UpdateStateTrieFromTempStateDB() {
  LOG_MARKER();

  leveldb::Iterator* iter = nullptr;

  while (iter == nullptr || iter->Valid()) {
    vector<StateSharedPtr> states;
    if (!BlockStorage::GetBlockStorage().GetTempStateInBatch(iter, states)) {
      LOG_GENERAL(WARNING, "GetTempStateInBatch failed");
      delete iter;
      return false;
    }
    for (const auto& state : states) {
      UpdateStateTrie(state->first, state->second);
    }
  }

  delete iter;

  if (!BlockStorage::GetBlockStorage().ResetDB(BlockStorage::TEMP_STATE)) {
    LOG_GENERAL(WARNING, "BlockStorage::ResetDB (TEMP_STATE) failed");
    return false;
  }
  return true;
}

void AccountStore::DiscardUnsavedUpdates() {
  LOG_MARKER();

  unique_lock<shared_timed_mutex> g(m_mutexPrimary, defer_lock);
  unique_lock<mutex> g2(m_mutexDB, defer_lock);
  lock(g, g2);

  try {
    {
      lock_guard<mutex> g(m_mutexTrie);
      m_state.db()->rollback();
      m_state.setRoot(m_prevRoot);
    }
    m_addressToAccount->clear();
  } catch (const boost::exception& e) {
    LOG_GENERAL(WARNING, "Error with AccountStore::DiscardUnsavedUpdates. "
                             << boost::diagnostic_information(e));
  }
}

bool AccountStore::RetrieveFromDisk() {
  LOG_MARKER();

  InitSoft();

  unique_lock<shared_timed_mutex> g(m_mutexPrimary, defer_lock);
  unique_lock<mutex> g2(m_mutexDB, defer_lock);
  lock(g, g2);

  bytes rootBytes;
  if (!BlockStorage::GetBlockStorage().GetStateRoot(rootBytes)) {
    // To support backward compatibilty - lookup with new binary trying to
    // recover from old database
    if (BlockStorage::GetBlockStorage().GetMetadata(STATEROOT, rootBytes)) {
      if (!BlockStorage::GetBlockStorage().PutStateRoot(rootBytes)) {
        LOG_GENERAL(WARNING,
                    "BlockStorage::PutStateRoot failed "
                        << DataConversion::CharArrayToString(rootBytes));
        return false;
      }
    } else {
      LOG_GENERAL(WARNING, "Failed to retrieve StateRoot from disk");
      return false;
    }
  }

  try {
    h256 root(rootBytes);
    LOG_GENERAL(INFO, "StateRootHash:" << root.hex());
    lock_guard<mutex> g(m_mutexTrie);
    m_state.setRoot(root);
  } catch (const boost::exception& e) {
    LOG_GENERAL(WARNING, "Error with AccountStore::RetrieveFromDisk. "
                             << boost::diagnostic_information(e));
    return false;
  }
  return true;
}

Account* AccountStore::GetAccountTemp(const Address& address) {
  return m_accountStoreTemp->GetAccount(address);
}

bool AccountStore::UpdateAccountsTemp(const uint64_t& blockNum,
                                      const unsigned int& numShards,
                                      const bool& isDS,
                                      const Transaction& transaction,
                                      TransactionReceipt& receipt,
                                      ErrTxnStatus& error_code) {
  // LOG_MARKER();

  unique_lock<shared_timed_mutex> g(m_mutexPrimary, defer_lock);
  unique_lock<mutex> g2(m_mutexDelta, defer_lock);
  lock(g, g2);

  return m_accountStoreTemp->UpdateAccounts(blockNum, numShards, isDS,
                                            transaction, receipt, error_code);
}

bool AccountStore::UpdateCoinbaseTemp(const Address& rewardee,
                                      const Address& genesisAddress,
                                      const uint128_t& amount) {
  // LOG_MARKER();

  lock_guard<mutex> g(m_mutexDelta);

  if (m_accountStoreTemp->GetAccount(rewardee) == nullptr) {
    m_accountStoreTemp->AddAccount(rewardee, {0, 0});
  }
  return m_accountStoreTemp->TransferBalance(genesisAddress, rewardee, amount);
  // Should the nonce increase ??
}

uint128_t AccountStore::GetNonceTemp(const Address& address) {
  lock_guard<mutex> g(m_mutexDelta);

  if (m_accountStoreTemp->GetAddressToAccount()->find(address) !=
      m_accountStoreTemp->GetAddressToAccount()->end()) {
    return m_accountStoreTemp->GetNonce(address);
  } else {
    return this->GetNonce(address);
  }
}

StateHash AccountStore::GetStateDeltaHash() {
  lock_guard<mutex> g(m_mutexDelta);

  bool isEmpty = true;

  for (unsigned char i : m_stateDeltaSerialized) {
    if (i != 0) {
      isEmpty = false;
      break;
    }
  }

  if (isEmpty) {
    return StateHash();
  }

  SHA2<HashType::HASH_VARIANT_256> sha2;
  sha2.Update(m_stateDeltaSerialized);
  return StateHash(sha2.Finalize());
}

void AccountStore::CommitTemp() {
  LOG_MARKER();
  if (!DeserializeDelta(m_stateDeltaSerialized, 0)) {
    LOG_GENERAL(WARNING, "DeserializeDelta failed.");
  }
}

void AccountStore::CommitTempRevertible() {
  LOG_MARKER();

  InitRevertibles();

  if (!DeserializeDelta(m_stateDeltaSerialized, 0, true)) {
    LOG_GENERAL(WARNING, "DeserializeDelta failed.");
  }
}

void AccountStore::RevertCommitTemp() {
  LOG_MARKER();

  unique_lock<shared_timed_mutex> g(m_mutexPrimary);

  // Revert changed
  for (auto const& entry : m_addressToAccountRevChanged) {
    // LOG_GENERAL(INFO, "Revert changed address: " << entry.first);
    (*m_addressToAccount)[entry.first] = entry.second;
    UpdateStateTrie(entry.first, entry.second);
  }
  for (auto const& entry : m_addressToAccountRevCreated) {
    // LOG_GENERAL(INFO, "Remove created address: " << entry.first);
    RemoveAccount(entry.first);
    RemoveFromTrie(entry.first);
  }

  ContractStorage2::GetContractStorage().RevertContractStates();
}