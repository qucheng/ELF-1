/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include "../base/go_state.h"
#include "go_game_specific.h"
#include "record.h"
#include "game_utils.h"

enum FinishReason {
  FR_RESIGN = 0,
  FR_TWO_PASSES,
  FR_MAX_STEP,
  FR_CLEAR,
  FR_ILLEGAL,
  FR_CHEAT_NEWER_WINS_HALF,
  FR_CHEAT_SELFPLAY_RANDOM_RESULT,
};

struct GoStateExt {
 public:
  GoStateExt(int game_idx, const GameOptionsSelfPlay& options)
      : _game_idx(game_idx),
        _last_move_for_the_game(M_INVALID),
        _last_value(0.0),
        _options(options) {
    restart();
  }

  std::string dumpSgf(const std::string& filename) const;
  void dumpSgf() const {
    std::string filename = _options.dump_record_prefix + "_" +
        std::to_string(_game_idx) + "_" + std::to_string(_seq) + "_" +
        (_state.getFinalValue() > 0 ? "B" : "W") + ".sgf";
    std::string sgf_record = dumpSgf(filename);
    std::ofstream oo(filename);
    oo << sgf_record << std::endl;
  }

  void setRequest(const Request& request) {
    curr_request_ = request;
    _resign_check.resign_thres = request.resign_thres;
    _resign_check.never_resign_ratio = request.never_resign_prob;
  }

  void addCurrentModel() {
    if (curr_request_.vers.black_ver >= 0)
      using_models_.insert(curr_request_.vers.black_ver);
    if (curr_request_.vers.white_ver >= 0)
      using_models_.insert(curr_request_.vers.white_ver);
  }

  const Request& currRequest() const {
    return curr_request_;
  }

  float setFinalValue(FinishReason reason, std::mt19937* rng) {
    float final_value = 0.0;
    _last_move_for_the_game = _state.lastMove();

    if (reason == FR_RESIGN) {
      final_value = _state.nextPlayer() == S_WHITE ? 1.0 : -1.0;
      _last_move_for_the_game = M_RESIGN;
    } else if (
        reason == FR_CHEAT_NEWER_WINS_HALF &&
        !curr_request_.vers.is_selfplay()) {
      auto h = std::hash<std::string>{}(
                   std::to_string(curr_request_.vers.black_ver)) ^
          std::hash<std::string>{}(
                   std::to_string(curr_request_.vers.white_ver));
      final_value = h % 2 == 0 ? 1.0 : -1.0;
      if (curr_request_.player_swap)
        final_value = -final_value;
    } else if (
        reason == FR_CHEAT_SELFPLAY_RANDOM_RESULT &&
        curr_request_.vers.is_selfplay()) {
      final_value = ((*rng)() % 2 == 0 ? 1.0 : -1.0);
    } else {
      final_value = _state.evaluate(_options.common.komi);
    }
    _state.setFinalValue(final_value);
    return final_value;
  }

  Coord lastMove() const {
    if (_state.justStarted())
      return _last_move_for_the_game;
    else
      return _state.lastMove();
  }

  void restart() {
    _last_value = _state.getFinalValue();
    _state.reset();
    _mcts_policies.clear();
    _predicted_values.clear();

    using_models_.clear();

    _resign_check.reset();
    _seq++;

    addCurrentModel();
  }

  Result dumpResult() const {
    Result result; 

    result.reward = _state.getFinalValue();
    result.content = coords2sgfstr(_state.getAllMoves());
    result.never_resign = _resign_check.never_resign;
    result.using_models =
        std::vector<int64_t>(using_models_.begin(), using_models_.end());
    result.policies = _mcts_policies;
    result.num_move = _state.getPly() - 1;
    result.values = _predicted_values;

    return result;
  }

  void saveCurrentTree(const std::string& tree_info) const {
    // Dump the tree as well.
    std::string filename = _options.dump_record_prefix + "_" +
        std::to_string(_game_idx) + "_" + std::to_string(_seq) + "_" +
        std::to_string(_state.getPly()) + ".tree";
    std::ofstream oo(filename);
    oo << _state.showBoard() << std::endl;
    oo << tree_info;
  }

  float getLastGameFinalValue() const {
    return _last_value;
  }

  void addMCTSPolicy(const std::vector<std::pair<Coord, float>> &policy) {
    // First find the max value
    float max_val = 0.0;
    for (size_t k = 0; k < policy.size(); k++) {
      const auto& entry = policy[k];
      max_val = std::max(max_val, entry.second);
    }

    _mcts_policies.emplace_back();
    std::fill(
        _mcts_policies.back().prob,
        _mcts_policies.back().prob + BOUND_COORD,
        0);
    for (size_t k = 0; k < policy.size(); k++) {
      const auto& entry = policy[k];
      unsigned char c =
          static_cast<unsigned char>(entry.second / max_val * 255);
      _mcts_policies.back().prob[entry.first] = c;
    }
  }

  void addPredictedValue(float predicted_value) {
    _predicted_values.push_back(predicted_value);
  }

  float getLastPredictedValue() const {
    if (_predicted_values.empty())
      return 0.0;
    else
      return _predicted_values.back();
  }

  bool shouldResign(std::mt19937* rng) {
    // Run it after addPredictedValue
    float predicted_value = getLastPredictedValue();
    Stone player = _state.nextPlayer();
    return (
        player == S_BLACK ? _resign_check.check(predicted_value, rng)
                          : _resign_check.check(-predicted_value, rng));
  }

  void showFinishInfo(FinishReason reason) const;

  bool forward(Coord c) {
    return _state.forward(c);
  }

  const GoState& state() const {
    return _state;
  }
  int seq() const { return _seq; }
  int gameIdx() const { return _game_idx; }

  bool finished() const {
    return _options.num_game_per_thread > 0 &&
        _seq >= _options.num_game_per_thread;
  }

  const GameOptionsSelfPlay& options() const {
    return _options;
  }

 protected:
  const int _game_idx;
  int _seq = 0;

  GoState _state;
  Coord _last_move_for_the_game;

  Request curr_request_;
  std::set<int64_t> using_models_;

  float _last_value;

  ResignCheck _resign_check;
  GameOptionsSelfPlay _options;

  std::vector<CoordRecord> _mcts_policies;
  std::vector<float> _predicted_values;
};

class GoStateExtOffline {
 public:
  friend class GoFeature;

  GoStateExtOffline(int game_idx, const GameOptionsTrain& options)
      : _game_idx(game_idx), _bf(_state), _options(options) {}

  void fromData(int seq, const Request &request, const Result &result) {
    _offline_all_moves = sgfstr2coords(result.content);
    _offline_winner = result.reward > 0 ? 1.0 : -1.0;

    _mcts_policies = result.policies;
    curr_request_ = request;
    _seq = seq;
    _predicted_values = result.values;
    _state.reset();
  }

  bool switchRandomMove(std::mt19937* rng) {
    // Random sample one move
    if ((int)_offline_all_moves.size() <= _options.num_future_actions - 1) {
      std::cout << "[" << _game_idx << "] #moves " << _offline_all_moves.size()
                << " smaller than " << _options.num_future_actions << " - 1"
                << std::endl;
      return false;
    }
    size_t move_to = (*rng)() %
        (_offline_all_moves.size() - _options.num_future_actions + 1);
    switchBeforeMove(move_to);
    return true;
  }

  void generateD4Code(std::mt19937* rng) {
    _bf.setD4Code((*rng)() % 8);
  }

  void switchBeforeMove(size_t move_to) {
    assert(move_to < _offline_all_moves.size());

    _state.reset();
    for (size_t i = 0; i < move_to; ++i) {
      _state.forward(_offline_all_moves[i]);
    }
  }

  int getNumMoves() const {
    return _offline_all_moves.size();
  }

  float getPredictedValue(int move_idx) const {
    return _predicted_values[move_idx];
  }

 private:
  const int _game_idx;
  GoState _state;
  BoardFeature _bf;
  GameOptionsTrain _options;

  int _seq;
  Request curr_request_;

  std::vector<Coord> _offline_all_moves;
  float _offline_winner;

  std::vector<CoordRecord> _mcts_policies;
  std::vector<float> _predicted_values;
};
