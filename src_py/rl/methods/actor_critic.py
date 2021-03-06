# Copyright (c) 2018-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

from elf.options import auto_import_options, PyOptionSpec
from .policy_gradient import PolicyGradient
from .discounted_reward import DiscountedReward
from .value_matcher import ValueMatcher
from .utils import add_err

import torch

class ActorCritic(object):
    """An actor critic model."""

    @classmethod
    def get_option_spec(cls):
        spec = PyOptionSpec()
        spec.addStrOption(
            'value_node',
            'name of the value node',
            'V')
        spec.addFloatOption(
            'adv_clip',
            'clip value of advantage. 0.0 means no clipping',
            0.0)

        spec.merge(PyOptionSpec.fromClasses(
            (PolicyGradient, DiscountedReward, ValueMatcher)
        ))

        return spec

    @auto_import_options
    def __init__(self, option_map):
        """Initialization of ActorCritic component methods:

        `PolicyGradient`, `DiscountedReward`, and `ValueMatcher`.
        """
        self.discounted_reward = DiscountedReward(option_map)
        self.pg = PolicyGradient(option_map)
        self.value_matcher = ValueMatcher(option_map)

    def update(self, m, batch, stats):
        """Actor critic model update.

        Feed stats for later summarization.

        Args:
            mi(`ModelInterface`): mode interface used
            batch(dict): batch of data. Keys in a batch:
                ``s``: state,
                ``r``: immediate reward,
                ``terminal``: if game is terminated
            stats(`Stats`): Feed stats for later summarization.
        """
        value_node = self.options.value_node

        T = batch.histSize("s")

        state_curr = m(batch.hist(T - 1))
        self.discounted_reward.setR(
            state_curr[value_node].squeeze().data, stats)

        err = None

        for t in range(T - 2, -1, -1):
            bht = batch.hist(t)
            state_curr = m.forward(bht)

            # go through the sample and get the rewards.
            V = state_curr[value_node].squeeze()

            R = self.discounted_reward.feed(
                batch.hist(t, key=['r', "terminal"]),
                stats=stats)

            A = R - V.data
            if self.options.adv_clip > 0.0:
                # Clip the advantage. 
                A = torch.clamp(A, min=-self.options.adv_clip, max=self.options.adv_clip)
                R = V.data + A

            policy_err = self.pg.feed(
                A, state_curr, bht, stats, old_pi_s=bht)
            err = add_err(err, policy_err)
            err = add_err(err, self.value_matcher.feed(
                {value_node: V, "target": R}, stats))

        stats["cost"].feed(err.item() / (T - 1))
        err.backward()
