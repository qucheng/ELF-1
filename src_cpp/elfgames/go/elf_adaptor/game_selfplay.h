/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <random>
#include <string>

#include "elf/interface/game_base.h"
#include "elf/logging/IndexedLoggerFactory.h"

#include "../sgf/sgf.h"
#include "../ctrl/stats/game_stats.h"

#include "./mcts/mcts.h"
#include "game_feature.h"

#include "elf/distri/game_interface.h"

constexpr int NO_OP = 0;
constexpr int ONLY_WAIT = 1;
constexpr int UPDATE_REQUEST_ONLY = 2;
constexpr int UPDATE_MODEL = 3;
constexpr int UPDATE_MODEL_ASYNC = 4;
constexpr int UPDATE_COMPLETE = 5;

using elf::cs::StepStatus;
using elf::cs::ThreadState;
using elf::cs::MsgReply;
using elf::cs::MsgRequest;
using elf::cs::Record;

// Game interface for Go.
class GoGameSelfPlay : public elf::cs::ClientGame {
 public:
  GoGameSelfPlay(
      int game_idx,
      const GameOptionsSelfPlay& options, 
      GameStats &game_stats);

  void onEnd(elf::game::Base*) override {
    _ai.reset(nullptr);
    _ai2.reset(nullptr);
  }

  StepStatus step(elf::game::Base*, Record *) override;
  bool onReceive(const MsgRequest &, MsgReply* reply) override;
  ThreadState getThreadState() const override;

  void addMCTSParams(const elf::ai::tree_search::CtrlOptions &ctrl_options) {
    _ai->addMCTSParams(ctrl_options);
  }

  std::string peekMCTS(int topn) {
    auto sorted = _ai->peekMCTS();

    std::stringstream ss;
    for (int i = 0; i < topn; ++i) {
      ss << "[" << i + 1 << "] " 
         << elf::ai::tree_search::ActionTrait<Coord>::to_string(sorted[i].first) 
         << ", info: " << sorted[i].second.info() << std::endl;
    }
    return ss.str();
  }

  std::string showBoard() const {
    return _state_ext.state().showBoard();
  }
  std::string getNextPlayer() const {
    return player2str(_state_ext.state().nextPlayer());
  }
  std::string getLastMove() const {
    return coord2str2(_state_ext.lastMove());
  }
  float getScore() {
    return _state_ext.state().evaluate(options_.common.komi);
  }

  float getLastScore() const {
    return _state_ext.getLastGameFinalValue();
  }

 private:
  void setAsync();
  void restart();

  MCTSGoAI* init_ai(
      const std::string& actor_name,
      const elf::ai::tree_search::TSOptions& mcts_opt,
      float second_puct,
      int second_mcts_rollout_per_batch,
      int second_mcts_rollout_per_thread,
      int64_t model_ver);
  Coord mcts_make_diverse_move(MCTSGoAI* curr_ai, Coord c);
  Coord mcts_update_info(MCTSGoAI* mcts_go_ai, Coord c);
  StepStatus finish_game(FinishReason reason, Record *);

 private:
  GoStateExt _state_ext;
  Sgf _preload_sgf;
  Sgf::iterator _sgf_iter;

  int _online_counter = 0;

  // used to communicate info.
  elf::game::Base* base_ = nullptr;

  const GameOptionsSelfPlay options_;

  std::unique_ptr<MCTSGoAI> _ai;
  // Opponent ai (used for selfplay evaluation)
  std::unique_ptr<MCTSGoAI> _ai2;
  std::unique_ptr<HumanPlayer> _human_player;

  // A shared stats for all game threads.
  GameStats &game_stats_;

  std::shared_ptr<spdlog::logger> logger_;
};
