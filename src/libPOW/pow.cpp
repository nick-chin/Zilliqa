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

#include <boost/algorithm/string/predicate.hpp>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>

#include "common/Serializable.h"
#include "libCrypto/Sha2.h"
#include "libUtils/DataConversion.h"
#include "pow.h"

#ifdef OPENCL_MINE
#include "depends/libethash-cl/CLMiner.h"
#endif

#ifdef CUDA_MINE
#include "depends/libethash-cuda/CUDAMiner.h"
#endif

POW::POW() {
  m_currentBlockNum = 0;
  m_epochContextLight =
      ethash::create_epoch_context(ethash::get_epoch_number(m_currentBlockNum));

  if (REMOTE_MINE) {
    m_httpClient = std::make_unique<jsonrpc::HttpClient>(MINING_PROXY_URL);
  }

  if (FULL_DATASET_MINE && !CUDA_GPU_MINE && !OPENCL_GPU_MINE) {
    m_epochContextFull = ethash::create_epoch_context_full(
        ethash::get_epoch_number(m_currentBlockNum));
  }

  if (!LOOKUP_NODE_MODE) {
    if (OPENCL_GPU_MINE) {
      InitOpenCL();
    } else if (CUDA_GPU_MINE) {
      InitCUDA();
    }
  }
}

POW::~POW() {}

POW& POW::GetInstance() {
  static POW pow;
  return pow;
}

void POW::StopMining() { m_shouldMine = false; }

std::string POW::BytesToHexString(const uint8_t* str, const uint64_t s) {
  std::ostringstream ret;

  for (size_t i = 0; i < s; ++i)
    ret << std::hex << std::setfill('0') << std::setw(2) << std::nouppercase
        << (int)str[i];

  return ret.str();
}

bytes POW::HexStringToBytes(std::string const& _s) {
  unsigned s = (_s[0] == '0' && _s[1] == 'x') ? 2 : 0;
  bytes ret;
  ret.reserve((_s.size() - s + 1) / 2);

  if (_s.size() % 2) try {
      ret.push_back(FromHex(_s[s++]));
    } catch (...) {
      ret.push_back(0);
    }
  for (unsigned i = s; i < _s.size(); i += 2) try {
      ret.push_back((uint8_t)(FromHex(_s[i]) * 16 + FromHex(_s[i + 1])));
    } catch (...) {
      ret.push_back(0);
    }
  return ret;
}

std::string POW::BlockhashToHexString(const ethash_hash256& _hash) {
  return BytesToHexString(_hash.bytes, 32);
}

int POW::FromHex(char _i) {
  if (_i >= '0' && _i <= '9') return _i - '0';
  if (_i >= 'a' && _i <= 'f') return _i - 'a' + 10;
  if (_i >= 'A' && _i <= 'F') return _i - 'A' + 10;
  return -1;
}

ethash_hash256 POW::StringToBlockhash(std::string const& _s) {
  ethash_hash256 ret;
  bytes b = HexStringToBytes(_s);
  if (b.size() != 32) {
    LOG_GENERAL(WARNING,
                "Input to StringToBlockhash is not of size 32. Returning "
                "uninitialize ethash_hash256. Size is "
                    << b.size());
    return ret;
  }
  copy(b.begin(), b.end(), ret.bytes);
  return ret;
}

bool POW::CheckDificulty(const ethash_hash256& result,
                         const ethash_hash256& boundary) {
  return ethash::is_less_or_equal(result, boundary);
}

ethash_hash256 POW::DifficultyLevelInInt(uint8_t difficulty) {
  uint8_t b[UINT256_SIZE];
  std::fill(b, b + UINT256_SIZE, 0xFF);
  uint8_t firstNbytesToSet = difficulty / 8;
  uint8_t nBytesBitsToSet = difficulty % 8;

  for (int i = 0; i < firstNbytesToSet; i++) {
    b[i] = 0;
  }

  const unsigned char masks[] = {0xFF, 0x7F, 0x3F, 0x1F,
                                 0x0F, 0x07, 0x03, 0x01};
  b[firstNbytesToSet] = masks[nBytesBitsToSet];
  return StringToBlockhash(BytesToHexString(b, UINT256_SIZE));
}

bool POW::EthashConfigureClient(uint64_t block_number, bool fullDataset) {
  std::lock_guard<std::mutex> g(m_mutexLightClientConfigure);

  if (block_number < m_currentBlockNum) {
    LOG_GENERAL(WARNING,
                "WARNING: How come the latest block number is smaller than "
                "current block number? block_number: "
                    << block_number
                    << " currentBlockNum: " << m_currentBlockNum);
  }

  if (ethash::get_epoch_number(block_number) !=
      ethash::get_epoch_number(m_currentBlockNum)) {
    auto epochNumber = ethash::get_epoch_number(block_number);
    m_epochContextLight = ethash::create_epoch_context(epochNumber);
  }

  bool isMineFullCpu = fullDataset && !CUDA_GPU_MINE && !OPENCL_GPU_MINE;

  if (isMineFullCpu && (m_epochContextFull == nullptr ||
                        ethash::get_epoch_number(block_number) !=
                            ethash::get_epoch_number(m_currentBlockNum))) {
    m_epochContextFull = ethash::create_epoch_context_full(
        ethash::get_epoch_number(block_number));
  }

  m_currentBlockNum = block_number;

  return true;
}

ethash_mining_result_t POW::MineLight(ethash_hash256 const& headerHash,
                                      ethash_hash256 const& boundary,
                                      uint64_t startNonce) {
  uint64_t nonce = startNonce;
  while (m_shouldMine) {
    auto mineResult = ethash::hash(*m_epochContextLight, headerHash, nonce);
    if (ethash::is_less_or_equal(mineResult.final_hash, boundary)) {
      ethash_mining_result_t winning_result = {
          BlockhashToHexString(mineResult.final_hash),
          BlockhashToHexString(mineResult.mix_hash), nonce, true};
      return winning_result;
    }
    nonce++;
  }

  ethash_mining_result_t failure_result = {"", "", 0, false};
  return failure_result;
}

ethash_mining_result_t POW::MineFull(ethash_hash256 const& headerHash,
                                     ethash_hash256 const& boundary,
                                     uint64_t startNonce) {
  uint64_t nonce = startNonce;
  while (m_shouldMine) {
    auto mineResult = ethash::hash(*m_epochContextFull, headerHash, nonce);
    if (ethash::is_less_or_equal(mineResult.final_hash, boundary)) {
      ethash_mining_result_t winning_result = {
          BlockhashToHexString(mineResult.final_hash),
          BlockhashToHexString(mineResult.mix_hash), nonce, true};
      return winning_result;
    }
    nonce++;
  }

  ethash_mining_result_t failure_result = {"", "", 0, false};
  return failure_result;
}

ethash_mining_result_t POW::MineFullGPU(uint64_t blockNum,
                                        ethash_hash256 const& headerHash,
                                        uint8_t difficulty,
                                        uint64_t startNonce) {
  std::vector<std::unique_ptr<std::thread>> vecThread;
  uint64_t nonce = startNonce;
  m_minerIndex = 0;
  // Clear old result
  for (auto& miningResult : m_vecMiningResult) {
    miningResult = ethash_mining_result_t{"", "", 0, false};
  }
  for (size_t i = 0; i < m_miners.size(); ++i) {
    vecThread.push_back(std::make_unique<std::thread>(
        [&] { MineFullGPUThread(blockNum, headerHash, difficulty, nonce); }));
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  std::unique_lock<std::mutex> lk(m_mutexMiningResult);
  m_cvMiningResult.wait(lk);
  m_shouldMine = false;
  for (auto& ptrThead : vecThread) {
    ptrThead->join();
  }

  for (const auto& miningResult : m_vecMiningResult) {
    if (miningResult.success) {
      return miningResult;
    }
  }

  return ethash_mining_result_t{"", "", 0, false};
}

ethash_mining_result_t POW::RemoteMine(const PairOfKey& pairOfKey,
                                       uint64_t blockNum,
                                       ethash_hash256 const& headerHash,
                                       ethash_hash256 const& boundary) {
  LOG_MARKER();

  m_shouldMine = true;

  ethash_mining_result_t miningResult{"", "", 0, false};
  if (!SendWorkToProxy(pairOfKey, blockNum, headerHash, boundary)) {
    LOG_GENERAL(WARNING, "Failed to send work package to mining proxy.");
    return miningResult;
  }

  bool verifyResult = CheckMiningResult(pairOfKey, blockNum, headerHash,
                                        boundary, miningResult);
  if (!verifyResult) {
    LOG_GENERAL(WARNING, "Failed to check pow result from mining proxy.");
  }

  if (!SendVerifyResult(pairOfKey, headerHash, boundary, verifyResult)) {
    LOG_GENERAL(WARNING, "Failed to send verify result to mining proxy.");
  }

  return miningResult;
}

bool POW::SendWorkToProxy(const PairOfKey& pairOfKey, uint64_t blockNum,
                          ethash_hash256 const& headerHash,
                          ethash_hash256 const& boundary) {
  LOG_MARKER();

  bytes tmp;

  Json::Value jsonValue;

  bytes pubKeyData;
  pairOfKey.second.Serialize(pubKeyData, 0);
  jsonValue[0] = "0x" + BytesToHexString(pubKeyData.data(), pubKeyData.size());
  tmp.insert(tmp.end(), pubKeyData.begin(), pubKeyData.end());

  jsonValue[1] = "0x" + POW::BlockhashToHexString(headerHash);
  tmp.insert(tmp.end(), headerHash.bytes,
             headerHash.bytes + sizeof(ethash_hash256));

  auto strBlcokNumber =
      DataConversion::IntegerToHexString<uint64_t, sizeof(uint64_t)>(blockNum);
  jsonValue[2] = "0x" + strBlcokNumber;

  auto blockNumberBytes =
      DataConversion::IntegerToBytes<uint64_t, sizeof(uint64_t)>(blockNum);
  tmp.insert(tmp.end(), blockNumberBytes.begin(), blockNumberBytes.end());

  jsonValue[3] = "0x" + POW::BlockhashToHexString(boundary);
  tmp.insert(tmp.end(), boundary.bytes,
             boundary.bytes + sizeof(ethash_hash256));

  auto strPoWTime =
      DataConversion::IntegerToHexString<uint32_t, sizeof(uint32_t)>(
          POW_WINDOW_IN_SECONDS);
  jsonValue[4] = "0x" + strPoWTime;
  auto powTimeBytes =
      DataConversion::IntegerToBytes<uint32_t, sizeof(uint32_t)>(
          POW_WINDOW_IN_SECONDS);
  tmp.insert(tmp.end(), powTimeBytes.begin(), powTimeBytes.end());

  if (tmp.size() != (PUB_KEY_SIZE + BLOCK_HASH_SIZE + sizeof(uint64_t) +
                     BLOCK_HASH_SIZE + sizeof(uint32_t))) {
    LOG_GENERAL(WARNING, "Size of the buffer "
                             << tmp.size()
                             << " to generate signature is not correct.");
    return false;
  }

  Signature signature;
  if (!Schnorr::GetInstance().Sign(tmp, pairOfKey.first, pairOfKey.second,
                                   signature)) {
    LOG_GENERAL(WARNING, "Failed to sign zil_requestWork json value.");
    return false;
  }
  jsonValue[5] = "0x" + DataConversion::SerializableToHexStr(signature);

  LOG_GENERAL(INFO, "Json value send out: " << jsonValue);

  try {
    jsonrpc::Client client(*m_httpClient);
    Json::Value ret = client.CallMethod("zil_requestWork", jsonValue);
    LOG_GENERAL(INFO, "zil_requestWork return: " << ret);
    return ret.asBool();
  } catch (std::exception& e) {
    LOG_GENERAL(WARNING,
                "Exception captured in jsonrpc api zil_requestWork, exception: "
                    << e.what());
    return false;
  }
}

bool POW::CheckMiningResult(const PairOfKey& pairOfKey, uint64_t blockNum,
                            ethash_hash256 const& headerHash,
                            ethash_hash256 const& boundary,
                            ethash_mining_result_t& miningResult) {
  Json::Value jsonValue;

  bytes tmp;
  bytes pubKeyData;

  pairOfKey.second.Serialize(pubKeyData, 0);
  jsonValue[0] = "0x" + BytesToHexString(pubKeyData.data(), pubKeyData.size());
  tmp.insert(tmp.end(), pubKeyData.begin(), pubKeyData.end());

  jsonValue[1] = "0x" + BlockhashToHexString(headerHash);
  tmp.insert(tmp.end(), headerHash.bytes,
             headerHash.bytes + sizeof(ethash_hash256));

  jsonValue[2] = "0x" + BlockhashToHexString(boundary);
  tmp.insert(tmp.end(), boundary.bytes,
             boundary.bytes + sizeof(ethash_hash256));

  Signature signature;
  if (!Schnorr::GetInstance().Sign(tmp, pairOfKey.first, pairOfKey.second,
                                   signature)) {
    LOG_GENERAL(WARNING, "Failed to sign zil_checkWorkStatus json value.");
    return false;
  }
  jsonValue[3] = "0x" + DataConversion::SerializableToHexStr(signature);

  LOG_GENERAL(INFO, "Json value send out: " << jsonValue);

  while (m_shouldMine) {
    std::this_thread::sleep_for(
        std::chrono::seconds(CHECK_MINING_RESULT_INTERVAL));

    try {
      jsonrpc::Client client(*m_httpClient);
      Json::Value ret = client.CallMethod("zil_checkWorkStatus", jsonValue);
      LOG_GENERAL(INFO, "zil_checkWorkStatus return: " << ret);
      bool workDone = ret[0].asBool();
      if (!workDone) {
        continue;
      }

      if (ret.size() < 4) {
        LOG_GENERAL(WARNING,
                    "Mining proxy return invalid result, ret array size: "
                        << ret.size());
        return false;
      }

      uint64_t nonce = std::strtoull(ret[1].asCString(), NULL, 16);
      ethash_hash256 headerHash = StringToBlockhash(ret[2].asString());
      ethash_hash256 mixHash = StringToBlockhash(ret[3].asString());
      LOG_GENERAL(INFO, "PoW result from proxy, Nonce: "
                            << nonce << ", headerHash: " << ret[2].asString()
                            << "mix hash: " << ret[3].asString());

      ethash_hash256 hashResult;
      if (!VerifyRemoteSoln(blockNum, boundary, nonce, headerHash, mixHash,
                            hashResult)) {
        LOG_GENERAL(WARNING, "Failed to verify PoW result from proxy.");
        return false;
      }

      miningResult =
          ethash_mining_result_t{BlockhashToHexString(hashResult),
                                 BlockhashToHexString(mixHash), nonce, true};

      return true;
    } catch (std::exception& e) {
      LOG_GENERAL(
          WARNING,
          "Exception captured in jsonrpc api zil_checkWorkStatus, exception: "
              << e.what());
      return false;
    }
  }
  return false;
}

bool POW::VerifyRemoteSoln(uint64_t blockNum, ethash_hash256 const& boundary,
                           uint64_t nonce, const ethash_hash256& headerHash,
                           const ethash_hash256& mixHash,
                           ethash_hash256& hashResult) {
  LOG_MARKER();

  hashResult = LightHash(blockNum, headerHash, nonce).final_hash;
  if (!ethash::is_less_or_equal(hashResult, boundary)) {
    return false;
  }

  return ethash::verify(*m_epochContextLight, headerHash, mixHash, nonce,
                        boundary);
}

bool POW::SendVerifyResult(const PairOfKey& pairOfKey,
                           const ethash_hash256& headerHash,
                           ethash_hash256 const& boundary, bool verifyResult) {
  Json::Value jsonValue;

  bytes tmp;
  bytes pubKeyData;

  pairOfKey.second.Serialize(pubKeyData, 0);
  jsonValue[0] = "0x" + BytesToHexString(pubKeyData.data(), pubKeyData.size());
  tmp.insert(tmp.end(), pubKeyData.begin(), pubKeyData.end());

  auto strVerifyResult =
      DataConversion::IntegerToHexString<uint8_t, sizeof(uint8_t)>(
          verifyResult);
  jsonValue[1] = "0x" + strVerifyResult;
  tmp.push_back(verifyResult);

  jsonValue[2] = "0x" + BlockhashToHexString(headerHash);
  tmp.insert(tmp.end(), headerHash.bytes,
             headerHash.bytes + sizeof(ethash_hash256));

  jsonValue[3] = "0x" + BlockhashToHexString(boundary);
  tmp.insert(tmp.end(), boundary.bytes,
             boundary.bytes + sizeof(ethash_hash256));

  Signature signature;
  if (!Schnorr::GetInstance().Sign(tmp, pairOfKey.first, pairOfKey.second,
                                   signature)) {
    LOG_GENERAL(WARNING, "Failed to sign zil_verifyResult json value.");
    return false;
  }
  jsonValue[4] = "0x" + DataConversion::SerializableToHexStr(signature);

  LOG_GENERAL(INFO, "Json value send out: " << jsonValue);

  try {
    jsonrpc::Client client(*m_httpClient);
    Json::Value ret = client.CallMethod("zil_verifyResult", jsonValue);
    LOG_GENERAL(INFO, "zil_verifyResult return: " << ret);
    return ret.asBool();
  } catch (std::exception& e) {
    LOG_GENERAL(
        WARNING,
        "Exception captured in jsonrpc api zil_verifyResult, exception: "
            << e.what());
    return false;
  }
}

void POW::MineFullGPUThread(uint64_t blockNum, ethash_hash256 const& headerHash,
                            uint8_t difficulty, uint64_t nonce) {
  LOG_MARKER();
  auto index = m_minerIndex.load(std::memory_order_relaxed);
  ++m_minerIndex;
  LOG_GENERAL(INFO, "Difficulty : " << std::to_string(difficulty)
                                    << ", miner index " << index);
  dev::eth::WorkPackage wp;
  wp.blockNumber = blockNum;
  wp.boundary = (dev::h256)(dev::u256)((dev::bigint(1) << 256) /
                                       (dev::u256(1) << difficulty));

  wp.header = dev::h256{headerHash.bytes, dev::h256::ConstructFromPointer};

  constexpr uint32_t NONCE_SEGMENT_WIDTH = 40;
  const uint64_t NONCE_SEGMENT = (uint64_t)pow(2, NONCE_SEGMENT_WIDTH);
  wp.startNonce = nonce + index * NONCE_SEGMENT;

  dev::eth::Solution solution;
  while (m_shouldMine) {
    if (!m_miners[index]->mine(wp, solution)) {
      LOG_GENERAL(WARNING, "GPU failed to do mine, GPU miner log: "
                               << m_miners[index]->getLog());
      m_vecMiningResult[index] = ethash_mining_result_t{"", "", 0, false};
      m_cvMiningResult.notify_one();
      return;
    }
    auto hashResult = LightHash(blockNum, headerHash, solution.nonce);
    auto boundary = DifficultyLevelInInt(difficulty);
    if (ethash::is_less_or_equal(hashResult.final_hash, boundary)) {
      m_vecMiningResult[index] =
          ethash_mining_result_t{BlockhashToHexString(hashResult.final_hash),
                                 solution.mixHash.hex(), solution.nonce, true};
      m_cvMiningResult.notify_one();
      return;
    }
    wp.startNonce = solution.nonce;
  }
  m_vecMiningResult[index] = ethash_mining_result_t{"", "", 0, false};
  m_cvMiningResult.notify_one();
  return;
}

bytes POW::ConcatAndhash(const std::array<unsigned char, UINT256_SIZE>& rand1,
                         const std::array<unsigned char, UINT256_SIZE>& rand2,
                         const boost::multiprecision::uint128_t& ipAddr,
                         const PubKey& pubKey, uint32_t lookupId,
                         const boost::multiprecision::uint128_t& gasPrice) {
  bytes vec;
  for (const auto& s1 : rand1) {
    vec.push_back(s1);
  }

  for (const auto& s1 : rand2) {
    vec.push_back(s1);
  }

  bytes ipAddrVec;
  Serializable::SetNumber<boost::multiprecision::uint128_t>(
      ipAddrVec, 0, ipAddr, UINT128_SIZE);
  vec.insert(std::end(vec), std::begin(ipAddrVec), std::end(ipAddrVec));

  pubKey.Serialize(vec, vec.size());

  Serializable::SetNumber<uint32_t>(vec, vec.size(), lookupId,
                                    sizeof(uint32_t));
  Serializable::SetNumber<boost::multiprecision::uint128_t>(
      vec, vec.size(), gasPrice, UINT128_SIZE);

  SHA2<256> sha2;
  sha2.Update(vec);
  bytes sha2_result = sha2.Finalize();
  return sha2_result;
}

ethash_hash256 POW::GenHeaderHash(
    const std::array<unsigned char, UINT256_SIZE>& rand1,
    const std::array<unsigned char, UINT256_SIZE>& rand2,
    const boost::multiprecision::uint128_t& ipAddr, const PubKey& pubKey,
    uint32_t lookupId, const boost::multiprecision::uint128_t& gasPrice) {
  bytes sha2_result =
      ConcatAndhash(rand1, rand2, ipAddr, pubKey, lookupId, gasPrice);

  // Let's hash the inputs before feeding to ethash
  return StringToBlockhash(DataConversion::Uint8VecToHexStr(sha2_result));
}

ethash_mining_result_t POW::PoWMine(uint64_t blockNum, uint8_t difficulty,
                                    const PairOfKey& pairOfKey,
                                    const ethash_hash256& headerHash,
                                    bool fullDataset, uint64_t startNonce) {
  LOG_MARKER();
  // mutex required to prevent a new mining to begin before previous mining
  // operation has ended(ie. m_shouldMine=false has been processed) and
  // result.success has been returned)
  std::lock_guard<std::mutex> g(m_mutexPoWMine);
  EthashConfigureClient(blockNum, fullDataset);
  auto boundary = DifficultyLevelInInt(difficulty);

  ethash_mining_result_t result;

  m_shouldMine = true;

  if (REMOTE_MINE) {
    result = RemoteMine(pairOfKey, blockNum, headerHash, boundary);
  } else if (OPENCL_GPU_MINE || CUDA_GPU_MINE) {
    result = MineFullGPU(blockNum, headerHash, difficulty, startNonce);
  } else if (fullDataset) {
    result = MineFull(headerHash, boundary, startNonce);
  } else {
    result = MineLight(headerHash, boundary, startNonce);
  }
  return result;
}

bool POW::PoWVerify(uint64_t blockNum, uint8_t difficulty,
                    const ethash_hash256& headerHash, uint64_t winning_nonce,
                    const std::string& winning_result,
                    const std::string& winning_mixhash) {
  LOG_MARKER();
  EthashConfigureClient(blockNum);
  const auto boundary = DifficultyLevelInInt(difficulty);
  auto winnning_result = StringToBlockhash(winning_result);
  auto winningMixhash = StringToBlockhash(winning_mixhash);

  if (!ethash::is_less_or_equal(winnning_result, boundary)) {
    LOG_GENERAL(WARNING, "PoW solution doesn't meet difficulty requirement");
    return false;
  }

  return ethash::verify(*m_epochContextLight, headerHash, winningMixhash,
                        winning_nonce, boundary);
}

ethash::result POW::LightHash(uint64_t blockNum,
                              ethash_hash256 const& headerHash,
                              uint64_t nonce) {
  EthashConfigureClient(blockNum);
  return ethash::hash(*m_epochContextLight, headerHash, nonce);
}

bool POW::CheckSolnAgainstsTargetedDifficulty(const ethash_hash256& result,
                                              uint8_t difficulty) {
  const auto boundary = DifficultyLevelInInt(difficulty);
  return ethash::is_less_or_equal(result, boundary);
}

bool POW::CheckSolnAgainstsTargetedDifficulty(const std::string& result,
                                              uint8_t difficulty) {
  const auto boundary = DifficultyLevelInInt(difficulty);
  ethash_hash256 hashResult = StringToBlockhash(result);
  return ethash::is_less_or_equal(hashResult, boundary);
}

void POW::InitOpenCL() {
#ifdef OPENCL_MINE
  using namespace dev::eth;

  CLMiner::setCLKernel(CLKernelName::Stable);

  if (!CLMiner::configureGPU(OPENCL_LOCAL_WORK_SIZE,
                             OPENCL_GLOBAL_WORK_SIZE_MULTIPLIER, 0,
                             OPENCL_START_EPOCH, 0, 0, false, false)) {
    LOG_GENERAL(FATAL, "Failed to configure OpenCL GPU, please check hardware");
  }

  auto gpuToUse = GetGpuToUse();
  auto totalGpuDevice = CLMiner::getNumDevices();

  CLMiner::setNumInstances(gpuToUse.size());

  for (const auto gpuIndex : gpuToUse) {
    if (gpuIndex >= totalGpuDevice) {
      LOG_GENERAL(FATAL, "Selected GPU "
                             << gpuIndex
                             << " exceed the physical OpenCL GPU number "
                             << totalGpuDevice);
    }

    m_miners.push_back(std::make_unique<CLMiner>(gpuIndex));
    m_vecMiningResult.push_back(ethash_mining_result_t{"", "", 0, false});
  }
  LOG_GENERAL(INFO, "OpenCL GPU initialized in POW");
#else
  LOG_GENERAL(FATAL,
              "The software is not build with OpenCL. Please enable the "
              "OpenCL build option and "
              "and build software again");
#endif
}

void POW::InitCUDA() {
#ifdef CUDA_MINE
  using namespace dev::eth;

  auto gpuToUse = GetGpuToUse();
  auto deviceGenerateDag = *gpuToUse.begin();
  LOG_GENERAL(INFO, "Generate dag Nvidia GPU #" << deviceGenerateDag);

  if (!CUDAMiner::configureGPU(CUDA_BLOCK_SIZE, CUDA_GRID_SIZE, CUDA_STREAM_NUM,
                               CUDA_SCHEDULE_FLAG, 0, deviceGenerateDag, false,
                               false)) {
    LOG_GENERAL(FATAL, "Failed to configure CUDA GPU, please check hardware");
  }

  CUDAMiner::setNumInstances(gpuToUse.size());

  auto totalGpuDevice = CUDAMiner::getNumDevices();
  for (const auto gpuIndex : gpuToUse) {
    if (gpuIndex >= totalGpuDevice) {
      LOG_GENERAL(FATAL, "Selected GPU "
                             << gpuIndex
                             << " exceed the physical Nvidia GPU number "
                             << totalGpuDevice);
    }

    m_miners.push_back(std::make_unique<CUDAMiner>(gpuIndex));
    m_vecMiningResult.push_back(ethash_mining_result_t{"", "", 0, false});
  }
  LOG_GENERAL(INFO, "CUDA GPU initialized in POW");
#else
  LOG_GENERAL(FATAL,
              "The software is not build with CUDA. Please enable the CUDA "
              "build option "
              "and build software again");
#endif
}

std::set<unsigned int> POW::GetGpuToUse() {
  std::set<unsigned int> gpuToUse;
  std::stringstream ss(GPU_TO_USE);
  std::string item;
  while (std::getline(ss, item, ',')) {
    unsigned int index = strtol(item.c_str(), NULL, 10);
    gpuToUse.insert(index);
  }

  if (gpuToUse.empty()) {
    LOG_GENERAL(FATAL, "Please select at least one GPU to use.");
  }

  return gpuToUse;
}
