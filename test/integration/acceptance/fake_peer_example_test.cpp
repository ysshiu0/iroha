/**
 * Copyright Soramitsu Co., Ltd. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "framework/integration_framework/fake_peer/behaviour/honest.hpp"
#include "framework/integration_framework/fake_peer/block_storage.hpp"
#include "framework/integration_framework/fake_peer/fake_peer.hpp"
#include "framework/integration_framework/integration_test_framework.hpp"
#include "integration/acceptance/acceptance_fixture.hpp"
#include "module/irohad/multi_sig_transactions/mst_mocks.hpp"
#include "module/shared_model/builders/protobuf/block.hpp"

using namespace common_constants;
using namespace shared_model;
using namespace integration_framework;
using namespace shared_model::interface::permissions;

using ::testing::_;
using ::testing::Invoke;

static constexpr std::chrono::seconds kMstStateWaitingTime(10);
static constexpr std::chrono::seconds kSynchronizerWaitingTime(10);
static constexpr std::chrono::seconds kOrderingMessageWaitingTime(10);

class FakePeerExampleFixture : public AcceptanceFixture {
 public:
  using FakePeerPtr = std::shared_ptr<fake_peer::FakePeer>;

  std::unique_ptr<IntegrationTestFramework> itf_;

  /**
   * Prepare state of ledger:
   * - create honest fake iroha peers
   * - create account of target user
   * - add assets to admin
   *
   * @param num_fake_peers - the amount of fake peers to create
   * @return reference to ITF
   */
  IntegrationTestFramework &prepareState(size_t num_fake_peers) {
    // request the fake peers construction
    std::vector<std::future<FakePeerPtr>> fake_peers_futures;
    std::generate_n(std::back_inserter(fake_peers_futures),
                    num_fake_peers,
                    [this] { return itf_->addInitialPeer({}); });

    itf_->setInitialState(kAdminKeypair);

    auto permissions =
        interface::RolePermissionSet({Role::kReceive, Role::kTransfer});

    // get the constructed fake peers & set honest behaviour
    for (auto &fake_peer_future : fake_peers_futures) {
      assert(fake_peer_future.valid() && "fake peer must be ready");
      FakePeerPtr fake_peer = fake_peer_future.get();
      fake_peer->setBehaviour(std::make_shared<fake_peer::HonestBehaviour>());
      fake_peers_.emplace_back(std::move(fake_peer));
    }

    // inside prepareState we can use lambda for such assert, since
    // prepare transactions are not going to fail
    auto block_with_tx = [](auto &block) {
      ASSERT_EQ(block->transactions().size(), 1);
    };

    return itf_->sendTxAwait(makeUserWithPerms(permissions), block_with_tx)
        .sendTxAwait(
            complete(baseTx(kAdminId).addAssetQuantity(kAssetId, "20000.0"),
                     kAdminKeypair),
            block_with_tx);
  }

 protected:
  void SetUp() override {
    itf_ =
        std::make_unique<IntegrationTestFramework>(1, boost::none, true, true);
  }

  std::vector<FakePeerPtr> fake_peers_;
};

/**
 * Check that after sending a not fully signed transaction, an MST state
 * propagtes to another peer
 * @given a not fully signed transaction
 * @when such transaction is sent to one of two iroha peers in the network
 * @then that peer propagates MST state to another peer
 */
TEST_F(FakePeerExampleFixture,
       MstStateOfTransactionWithoutAllSignaturesPropagtesToOtherPeer) {
  std::mutex mst_mutex;
  std::condition_variable mst_cv;
  std::atomic_bool got_state_notification(false);
  auto &itf = prepareState(1);
  fake_peers_.front()->getMstStatesObservable().subscribe(
      [&mst_cv, &got_state_notification](const auto &state) {
        got_state_notification.store(true);
        mst_cv.notify_one();
      });
  itf.sendTxWithoutValidation(complete(
      baseTx(kAdminId)
          .transferAsset(kAdminId, kUserId, kAssetId, "income", "500.0")
          .quorum(2),
      kAdminKeypair));
  std::unique_lock<std::mutex> mst_lock(mst_mutex);
  mst_cv.wait_for(mst_lock, kMstStateWaitingTime, [&got_state_notification] {
    return got_state_notification.load();
  });
  EXPECT_TRUE(got_state_notification.load())
      << "Reached timeout waiting for MST State.";
}

/**
 * Check that Irohad loads correct block version when having a malicious fork on
 * the network.
 * @given a less then 1/3 of peers having a malicious fork of the ledger
 * @when the irohad needs to synchronize
 * @then it refuses the malicious fork and applies the valid one
 */
TEST_F(FakePeerExampleFixture,
       SynchronizeTheRightVersionOfForkedLedger) {
  static constexpr size_t num_bad_peers = 3;  ///< bad fake peers - the ones
                                              ///< creating a malicious fork
  // the real peer is added to the bad peers as they once are failing together
  static constexpr size_t num_peers = (num_bad_peers + 1) * 3 + 1;  ///< BFT
  static constexpr size_t num_fake_peers = num_peers - 1;  ///< one peer is real

  auto &itf = prepareState(num_fake_peers);

  // let the first peers be bad
  const std::vector<FakePeerPtr> bad_fake_peers(
      fake_peers_.begin(), fake_peers_.begin() + num_bad_peers);
  const std::vector<FakePeerPtr> good_fake_peers(
      fake_peers_.begin() + num_bad_peers, fake_peers_.end());
  const FakePeerPtr &rantipole_peer =
      bad_fake_peers.front();  // the malicious actor

  // Add two blocks to the ledger.
  itf.sendTx(complete(baseTx(kAdminId).transferAsset(
                          kAdminId, kUserId, kAssetId, "common_tx1", "1.0"),
                      kAdminKeypair))
      .skipBlock();
  itf.sendTx(complete(baseTx(kAdminId).transferAsset(
                          kAdminId, kUserId, kAssetId, "common_tx2", "2.0"),
                      kAdminKeypair))
      .skipBlock();

  // Create the valid branch, supported by the good fake peers:
  auto valid_block_storage = std::make_shared<fake_peer::BlockStorage>();
  for (const auto &block : itf.getIrohaInstance()
                               ->getIrohaInstance()
                               ->getStorage()
                               ->getBlockQuery()
                               ->getBlocksFrom(1)) {
    valid_block_storage->storeBlock(
        std::static_pointer_cast<shared_model::proto::Block>(block));
  }

  // From now the itf peer is considered unreachable from the rest network. //

  // Function to sign a block with a peer's key.
  auto sign_block_by_peers = [](auto &&block, const auto &peers) {
    for (auto &peer : peers) {
      block.signAndAddSignature(peer->getKeypair());
    }
    return std::move(block);
  };

  // Function to create a block
  auto build_block =
      [](const auto &parent_block,
         std::initializer_list<shared_model::proto::Transaction> transactions) {
        return proto::BlockBuilder()
            .height(parent_block->height() + 1)
            .prevHash(parent_block->hash())
            .createdTime(iroha::time::now())
            .transactions(transactions)
            .build();
      };

  // Create the malicious fork of the ledger:
  auto bad_block_storage =
      std::make_shared<fake_peer::BlockStorage>(*valid_block_storage);
  bad_block_storage->storeBlock(std::make_shared<shared_model::proto::Block>(
      sign_block_by_peers(
          build_block(
              valid_block_storage->getTopBlock(),
              {complete(baseTx(kAdminId).transferAsset(
                            kAdminId, kUserId, kAssetId, "bad_tx3", "300.0"),
                        kAdminKeypair)}),
          bad_fake_peers)
          .finish()));
  for (auto &bad_fake_peer : bad_fake_peers) {
    bad_fake_peer->setBlockStorage(bad_block_storage);
  }

  // Extend the valid ledger:
  valid_block_storage->storeBlock(std::make_shared<shared_model::proto::Block>(
      sign_block_by_peers(
          build_block(
              valid_block_storage->getTopBlock(),
              {complete(baseTx(kAdminId).transferAsset(
                            kAdminId, kUserId, kAssetId, "valid_tx3", "3.0"),
                        kAdminKeypair)}),
          good_fake_peers)
          .finish()));
  for (auto &good_fake_peer : good_fake_peers) {
    good_fake_peer->setBlockStorage(valid_block_storage);
  }

  // Create the new block that the good peers are about to commit now.
  auto new_valid_block = std::make_shared<shared_model::proto::Block>(
      sign_block_by_peers(
          build_block(
              valid_block_storage->getTopBlock(),
              {complete(baseTx(kAdminId).transferAsset(
                            kAdminId, kUserId, kAssetId, "valid_tx4", "4.0"),
                        kAdminKeypair)})
              .signAndAddSignature(rantipole_peer->getKeypair()),
          good_fake_peers)
          .finish());

  // From now the itf peer is considered reachable from the rest network. //

  // Suppose the rantipole peer created a valid Commit message for the tip of
  // the valid branch, containing its own vote in the beginning of the votes
  // list. So he forces the real peer to download the missing blocks from it.
  std::vector<iroha::consensus::yac::VoteMessage> valid_votes;
  valid_votes.reserve(good_fake_peers.size() + 1);
  const iroha::consensus::yac::YacHash good_yac_hash(
      iroha::consensus::Round(new_valid_block->height(), 0),
      new_valid_block->hash().hex(),
      new_valid_block->hash().hex());
  valid_votes.emplace_back(rantipole_peer->makeVote(good_yac_hash));
  std::transform(good_fake_peers.begin(),
                 good_fake_peers.end(),
                 std::back_inserter(valid_votes),
                 [&good_yac_hash](auto &good_fake_peer) {
                   return good_fake_peer->makeVote(good_yac_hash);
                 });
  rantipole_peer->sendYacState(valid_votes);

  // the good peers committed the block
  valid_block_storage->storeBlock(new_valid_block);

  // wait for the real peer to commit the blocks and check they are from the
  // vlid branch
  std::mutex wait_mutex;
  std::condition_variable wait_cv;
  std::atomic_bool commited_all_blocks(false);
  itf.getIrohaInstance()
      ->getIrohaInstance()
      ->getStorage()
      ->on_commit()
      .subscribe([&wait_cv, &commited_all_blocks, &valid_block_storage](
                     const auto &committed_block) {
        const auto valid_hash =
            valid_block_storage->getBlockByHeight(committed_block->height())
                ->hash()
                .hex();
        const auto commited_hash = committed_block->hash().hex();
        if (commited_hash != valid_hash) {
          ASSERT_EQ(commited_hash, valid_hash) << "Wrong block got committed!";
          wait_cv.notify_one();
        }
        if (committed_block->height()
            == valid_block_storage->getTopBlock()->height()) {
          commited_all_blocks.store(true);
          wait_cv.notify_one();
        }
      });
  std::unique_lock<std::mutex> wait_lock(wait_mutex);
  ASSERT_TRUE(wait_cv.wait_for(
      wait_lock,
      kSynchronizerWaitingTime,
      [&commited_all_blocks] { return commited_all_blocks.load(); }))
      << "Reached timeout waiting for all blocks to be committed.";
}

/**
 * Check that after receiving a valid command the ITF peer sends either a
 * proposal or a batch to another peer
 *
 * \attention this code is nothing more but an example of Fake Peer usage
 *
 * @given a network of two iroha peers
 * @when a valid command is sent to one
 * @then it must propagate either a proposal or a batch
 */
TEST_F(FakePeerExampleFixture,
       OrderingMessagePropagationAfterValidCommandReceived) {
  std::mutex m;
  std::condition_variable cv;
  std::atomic_bool got_message(false);
  auto checker = [&cv, &got_message](const auto &message) {
    got_message.store(true);
    cv.notify_one();
  };
  auto &itf = prepareState(1);
  fake_peers_.front()->getOsBatchesObservable().subscribe(checker);
  fake_peers_.front()->getOgProposalsObservable().subscribe(checker);
  itf.sendTxWithoutValidation(complete(
      baseTx(kAdminId)
          .transferAsset(kAdminId, kUserId, kAssetId, "income", "500.0")
          .quorum(1),
      kAdminKeypair));
  std::unique_lock<std::mutex> lock(m);
  cv.wait_for(lock, kOrderingMessageWaitingTime, [&got_message] {
    return got_message.load();
  });
  EXPECT_TRUE(got_message.load())
      << "Reached timeout waiting for an ordering message.";
}