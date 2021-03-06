/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

// TODO: Figure out how to remove this (ssengupta@fb)
#include <time.h>

#include <iostream>
#include <memory>
#include <random>
#include <set>
#include <vector>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "elf/base/sharedmem_serializer.h"
#include "elf/distributed/addrs.h"
#include "elf/distributed/shared_rw_buffer3.h"
#include "game_context.h"
#include "elf/interface/game_interface.h"

#include "remote_sender.h"
#include "remote_receiver.h"

namespace elf {

using json = nlohmann::json;

class BatchSender : public GameContext {
 public:
  BatchSender(const Options& options, remote::Interface &remote_comm)
    : GameContext(options), remote_comm_(remote_comm) {

     auto f = [&]() {
       std::string reply, label, identity;
       SharedMemOptions opts("", 1);
        while (true) {
          remote_comm_.recvFromAll(&label, &reply, &identity);
          // std::cout << "got reply: "<< reply << std::endl;
          json j = json::parse(reply);
          from_json(j["opts"], opts);
          getQ(opts).push(j);
        }
     };
     q_thread_.reset(new std::thread(f));
  }

  void setRemoteLabels(const std::set<std::string>& remote_labels) {
    remote_labels_ = remote_labels;
  }

  SharedMemData& allocateSharedMem(
      const SharedMemOptions& options,
      const std::vector<std::string>& keys) override {
    GameStateCollector::BatchCollectFunc func = nullptr;

    const std::string& label = options.getRecvOptions().label;

    auto it = remote_labels_.find(label);
    if (it == remote_labels_.end()) {
      BatchClient* batch_client = getBatchContext()->getClient();
      func = [batch_client](SharedMemData* smem_data) {
        return batch_client->sendWait(smem_data, {""});
      };
    } else {
      // Send to the client and wait for its response.
      func = [&](SharedMemData* smem_data) {
        const SharedMemOptions &opts = smem_data->getSharedMemOptions();
        json j;
        PRINT("converting smem to json");
        SMemToJson(*smem_data, input_keys_, j);

        const std::string &label = opts.getLabel();
        std::string identity;
        auto data = j.dump();
        PRINT("sending now, #size: " << data.size());
        remote_comm_.sendToEligible(label, data, &identity);
        PRINT("sending complete");

        auto &q = getQ(opts);
        q.pop(&j);
        PRINT("got reply_j: ");
        SMemFromJson(j, *smem_data);
        PRINT("after parsing smem");
        //
        /*
        const AnyP *anyp = smem_data->get("hash");
        const AnyP *anyp2 = smem_data->get("rhash");
        assert(anyp != nullptr);
        assert(anyp2 != nullptr);

        for (size_t i = 0; i < smem_data->getEffectiveBatchSize(); ++i) {
          uint64_t r1 = *anyp->getAddress<uint64_t>(i);
          uint64_t r2 = *anyp2->getAddress<uint64_t>(i);
          if (r1 != r2) {
            std::cout << " hash: " << r1 << " != " << r2 << std::endl;
          }
        }
        */
        //
        return comm::SUCCESS;
      };
    }

    SharedMemData &smem_data = getCollectorContext()->allocateSharedMem(options, keys, func);
    q_[smem_data.getSharedMemOptions()].reset(new remote::Queue<json>());

    return smem_data;
  }

private:
  remote::Interface &remote_comm_;
  std::set<std::string> remote_labels_;
  std::set<std::string> input_keys_ {"s", "hash"};

  std::unique_ptr<std::thread> q_thread_;
  std::unordered_map<SharedMemOptions, std::unique_ptr<remote::Queue<json>>> q_;
  remote::Queue<json> &getQ(const SharedMemOptions &opts) {
    auto it = q_.find(opts);
    assert(it != q_.end());
    return *it->second;
  }
};

// Directly send data to remote.
class EnvSender {
 public:
  EnvSender(remote::Interface &remote_comm)
    : remote_comm_(remote_comm) {
  }

  void setInputKeys(const std::set<std::string> &input_keys) {
    input_keys_ = input_keys;
  }

  SharedMemData& allocateSharedMem(
      const SharedMemOptions& options,
      const std::vector<std::string>& keys) {
    assert(smem_data_ == nullptr);
    SharedMemOptions options2 = options;
    options2.setIdx(0);
    options2.setLabelIdx(0);

    smem_data_.reset(new SharedMemData(options2, extractor_.getAnyP(keys)));
    smem_data_->setEffectiveBatchSize(1);
    return *smem_data_;
  }

  void sendAndWaitReply() {
    // we send it.
    json j;
    // std::cout << "saving to json" << std::endl;
    SMemToJson(*smem_data_, input_keys_, j);

    // std::cout << "sendToClient" << std::endl;
    const std::string &label = smem_data_->getSharedMemOptions().getLabel();
    std::string identity;
    remote_comm_.sendToEligible(label, j.dump(), &identity);

    std::string reply;
    // std::cout << ", wait_for_reply: "<< std::endl;
    remote_comm_.recv(label, &reply, identity);

    // std::cout << ", got reply_j: "<< std::endl;
    SMemFromJson(json::parse(reply), *smem_data_);
    // std::cout << ", after parsing smem: "<< std::endl;
    // after that all the tensors should contain the reply.
  }

  Extractor &getExtractor() {
    return extractor_;
  }

 private:
  remote::Interface &remote_comm_;

  Extractor extractor_;
  std::set<std::string> input_keys_;
  std::unique_ptr<SharedMemData> smem_data_;
};

class Stats {
 public:
  void feed(int idx, int batchsize) {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_[idx] ++;
    sum_batchsize_ += batchsize;
    total_batchsize_ += batchsize;
    count_ ++;
    if (count_ >= 5000) {
      int min_val = std::numeric_limits<int>::max();
      int max_val = -std::numeric_limits<int>::max();
      for (const auto &p : stats_) {
        if (p.first > max_val) max_val = p.first;
        if (p.first < min_val) min_val = p.first;
      }
      // std::cout << elf_utils::now() << " Key range: [" << min_val << ", " << max_val << "]: ";
      std::vector<int> zero_entries;
      for (int i = min_val; i <= max_val; ++i) {
        // std::cout << stats_[i] << ",";
        if (stats_[i] == 0) zero_entries.push_back(i);
      }
      if (!zero_entries.empty()) {
        std::cout << elf_utils::now() << ", zero entry: ";
        for (const auto &entry : zero_entries) {
          std::cout << entry << ",";
        }
        std::cout << std::endl;
      }

      std::cout << elf_utils::now() << " Avg batchsize: "
        << static_cast<float>(sum_batchsize_) / count_
        << ", #sample: " << total_batchsize_
        << ", #replied: " << total_release_batchsize_
        << ", #in_queue: " << total_batchsize_ - total_release_batchsize_
        << std::endl;

      reset();
    }
  }

  void recordRelease(int batchsize) {
    std::lock_guard<std::mutex> lock(mutex_);
    total_release_batchsize_ += batchsize;
  }

 private:
  std::mutex mutex_;
  std::unordered_map<int, int> stats_;
  int count_ = 0;
  int sum_batchsize_ = 0;

  int total_batchsize_ = 0;
  int total_release_batchsize_ = 0;

  void reset() {
    stats_.clear();
    count_ = 0;
    sum_batchsize_ = 0;
  }
};

using ReplyRecvFunc = std::function<void (int, std::string &&)>;

class SharedMemRemote : public SharedMem {
 public:
  enum Mode { RECV_SMEM, RECV_ENTRY };

  SharedMemRemote(
      const SharedMemOptions& opts,
      const std::unordered_map<std::string, AnyP>& mem,
      remote::Interface &remote_comm,
      Stats *stats,
      Mode mode = RECV_ENTRY)
      : SharedMem(opts, mem),
        mode_(mode),
        remote_comm_(remote_comm),
        stats_(stats) {
  }

  void start() override {
      // std::cout << "SharedMemRemote start..." << std::endl;
      // We need to have a (or one per sample) remote_smem_ and a local smem_
      // Note that all remote_smem_ shared memory with smem_.
      remote_smem_.clear();
      identities_.clear();
      switch (mode_) {
        case RECV_SMEM:
          PRINT("RECV_SMEM");
          remote_smem_.emplace_back(smem_);
          // std::cout << "smem info: " << remote_smem_.back().info() << std::endl;
          break;
        case RECV_ENTRY:
          PRINT("RECV_ENTRY");
          for (int i = 0; i < smem_.getSharedMemOptions().getBatchSize(); ++i) {
            remote_smem_.emplace_back(smem_.copySlice(i));
            std::cout << "smem info: " << remote_smem_.back().info() << std::endl;
          }
          break;
      }
  }

  void waitBatchFillMem() override {
    const auto& opt = smem_.getSharedMemOptions();
    do {
      std::string identity, msg;
      PRINT("remote_smem waiting...");
      remote_comm_.recvFromEligible(opt.getLabel(), &msg, &identity);

      // std::cout << "smem info: " << smem_.info() << std::endl;
      auto &curr_smem = remote_smem_[identities_.size()];
      identities_.push_back(identity);

      PRINT("remote_smem info: " << curr_smem.info() << ", id.size(): " << identities_.size());

      SMemFromJson(json::parse(msg), curr_smem);
      cum_batchsize_ += curr_smem.getEffectiveBatchSize();
    } while (identities_.size() < remote_smem_.size());

    PRINT("get all remote_smem remote_smem.size(): " << remote_smem_.size());

    if (stats_ != nullptr)
      stats_->feed(opt.getLabelIdx(), smem_.getEffectiveBatchSize());
    smem_.setEffectiveBatchSize(cum_batchsize_);
    // Note that all remote_smem_ shared memory with smem_.
    // After waitBatchFillMem() return, smem_ has all the content ready.
    PRINT("remote_smem batch received.. ");
  }

  void waitReplyReleaseBatch(comm::ReplyStatus batch_status) override {
    (void)batch_status;

    if (stats_ != nullptr) {
      stats_->recordRelease(smem_.getEffectiveBatchSize());
    }

    // std::cout << "Input_keys: ";
    // for (const auto &key : input_keys_) std::cout << key << ", ";
    // std::cout << std::endl;
    const auto& opt = smem_.getSharedMemOptions();

    for (size_t i = 0; i < identities_.size(); ++i) {
      const auto &identity = identities_[i];
      const auto &remote = remote_smem_[i];

      json j;
      PRINT("About to reply: remote_smem info: " << remote.info());
      SMemToJsonExclude(remote, input_keys_, j);

      // Notify that we should send the content to remote back.
      remote_comm_.send(opt.getLabel(), j.dump(), identity);
    }
    PRINT("Remote_smem sent.. ");
    identities_.clear();
    cum_batchsize_ = 0;
  }

 private:
  const Mode mode_;
  std::vector<SharedMemData> remote_smem_;
  std::vector<std::string> identities_;
  int cum_batchsize_ = 0;

  remote::Interface &remote_comm_;
  std::set<std::string> input_keys_{"s", "hash"};
  Stats *stats_ = nullptr;
};

class BatchReceiver : public GCInterface {
 public:
  BatchReceiver(const Options& options, remote::Interface &remote_comm)
    : GCInterface(options), rng_(time(NULL)), remote_comm_(remote_comm) {
    batchContext_.reset(new BatchContext);
    collectors_.reset(new Collectors);
  }

  void setMode(SharedMemRemote::Mode mode) {
    mode_ = mode;
  }

  void start() override {
    batchContext_->start();
    collectors_->start();
  }

  void stop() override {
    batchContext_->stop(nullptr);
  }

  GameClientInterface *getClient() override { return nullptr; }

  SharedMemData* wait(int time_usec = 0) override {
    return batchContext_->getWaiter()->wait(time_usec);
  }

  void step(comm::ReplyStatus success = comm::SUCCESS) override {
    return batchContext_->getWaiter()->step(success);
  }

  SharedMemData& allocateSharedMem(
      const SharedMemOptions& opt,
      const std::vector<std::string>& keys) override {
    const std::string& label = opt.getRecvOptions().label;

    // Allocate data.
    auto idx = collectors_->getNextIdx(label);

    SharedMemOptions options_with_idx = opt;
    options_with_idx.setIdx(idx.first);
    options_with_idx.setLabelIdx(idx.second);

    auto creator = [&](const SharedMemOptions& options,
                       const std::unordered_map<std::string, AnyP>& anyps) {
      return std::unique_ptr<SharedMemRemote>(
          new SharedMemRemote(options, anyps, remote_comm_, &stats_, mode_));
    };

    BatchClient* batch_client = batchContext_->getClient();
    auto collect_func = [batch_client](SharedMemData* smem_data) {
      return batch_client->sendWait(smem_data, {""});
    };

    return collectors_->allocateSharedMem(options_with_idx, keys, creator, collect_func);
  }

  Extractor& getExtractor() override {
    return collectors_->getExtractor();
  }

private:
  std::unique_ptr<BatchContext> batchContext_;
  std::unique_ptr<Collectors> collectors_;
  std::mt19937 rng_;
  remote::Interface &remote_comm_;
  Stats stats_;

  SharedMemRemote::Mode mode_ = SharedMemRemote::RECV_SMEM;
};

} // namespace elf
