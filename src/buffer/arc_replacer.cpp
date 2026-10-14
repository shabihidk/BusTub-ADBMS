//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// arc_replacer.cpp
//
// Identification: src/buffer/arc_replacer.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "buffer/arc_replacer.h"
#include <algorithm>
#include <memory>
#include <optional>
#include "common/config.h"
#include "common/exception.h"
#include "common/macros.h"

namespace bustub {

/**
 *
 * TODO(P1): Add implementation
 *
 * @brief a new ArcReplacer, with lists initialized to be empty and target size to 0
 * @param num_frames the maximum number of frames the ArcReplacer will be required to cache
 */
ArcReplacer::ArcReplacer(size_t num_frames) : replacer_size_(num_frames) {}

void ArcReplacer::EraseGhost(page_id_t page_id) {
  auto it = ghost_map_.find(page_id);
  if (it == ghost_map_.end()) {
    return;
  }
  auto status = it->second;
  if (status->arc_status_ == ArcStatus::MRU_GHOST) {
    mru_ghost_.erase(status->ghost_it_);
  } else {
    mfu_ghost_.erase(status->ghost_it_);
  }
  ghost_map_.erase(it);
}

void ArcReplacer::EraseAliveNoGhost(frame_id_t frame_id) {
  auto it = alive_map_.find(frame_id);
  if (it == alive_map_.end()) {
    return;
  }
  auto status = it->second;
  if (status->arc_status_ == ArcStatus::MRU) {
    mru_.erase(status->alive_it_);
  } else {
    mfu_.erase(status->alive_it_);
  }
  if (status->evictable_) {
    curr_size_--;
  }
  alive_map_.erase(it);
}

auto ArcReplacer::AddAlive(frame_id_t frame_id, page_id_t page_id, ArcStatus status) -> std::shared_ptr<FrameStatus> {
  auto entry = std::make_shared<FrameStatus>(page_id, frame_id, false, status);
  if (status == ArcStatus::MRU) {
    mru_.push_front(frame_id);
    entry->alive_it_ = mru_.begin();
  } else {
    mfu_.push_front(frame_id);
    entry->alive_it_ = mfu_.begin();
  }
  alive_map_[frame_id] = entry;
  return entry;
}

void ArcReplacer::MoveAliveToMfuFront(const std::shared_ptr<FrameStatus> &status) {
  if (status->arc_status_ == ArcStatus::MRU) {
    mru_.erase(status->alive_it_);
  } else if (status->arc_status_ == ArcStatus::MFU) {
    mfu_.erase(status->alive_it_);
  }
  mfu_.push_front(status->frame_id_);
  status->alive_it_ = mfu_.begin();
  status->arc_status_ = ArcStatus::MFU;
}

void ArcReplacer::MoveAliveToGhost(const std::shared_ptr<FrameStatus> &status, bool to_mru_ghost) {
  if (status->arc_status_ == ArcStatus::MRU) {
    mru_.erase(status->alive_it_);
  } else {
    mfu_.erase(status->alive_it_);
  }
  if (status->evictable_) {
    curr_size_--;
    status->evictable_ = false;
  }
  alive_map_.erase(status->frame_id_);

  page_id_t page_id = status->page_id_;
  // A page should not already be in a ghost list while still alive.
  EraseGhost(page_id);

  auto ghost = std::make_shared<FrameStatus>(page_id, INVALID_FRAME_ID, false,
                                             to_mru_ghost ? ArcStatus::MRU_GHOST : ArcStatus::MFU_GHOST);
  if (to_mru_ghost) {
    mru_ghost_.push_front(page_id);
    ghost->ghost_it_ = mru_ghost_.begin();
  } else {
    mfu_ghost_.push_front(page_id);
    ghost->ghost_it_ = mfu_ghost_.begin();
  }
  ghost_map_[page_id] = ghost;
}

auto ArcReplacer::EvictFromList(std::list<frame_id_t> *list, bool to_mru_ghost) -> std::optional<frame_id_t> {
  for (auto it = list->rbegin(); it != list->rend(); ++it) {
    frame_id_t frame_id = *it;
    auto status = alive_map_[frame_id];
    if (status->evictable_) {
      MoveAliveToGhost(status, to_mru_ghost);
      return frame_id;
    }
  }
  return std::nullopt;
}

/**
 * TODO(P1): Add implementation
 *
 * @brief Performs the Replace operation as described by the writeup
 * that evicts from either mfu_ or mru_ into its corresponding ghost list
 * according to balancing policy.
 *
 * If you wish to refer to the original ARC paper, please note that there are
 * two changes in our implementation:
 * 1. When the size of mru_ equals the target size, we don't check
 * the last access as the paper did when deciding which list to evict from.
 * This is fine since the original decision is stated to be arbitrary.
 * 2. Entries that are not evictable are skipped. If all entries from the desired side
 * (mru_ / mfu_) are pinned, we instead try victimize the other side (mfu_ / mru_),
 * and move it to its corresponding ghost list (mfu_ghost_ / mru_ghost_).
 *
 * @return frame id of the evicted frame, or std::nullopt if cannot evict
 */
auto ArcReplacer::Evict() -> std::optional<frame_id_t> {
  std::scoped_lock lock(latch_);
  if (curr_size_ == 0) {
    return std::nullopt;
  }

  // Prefer MFU when actual MRU size is strictly below the target; otherwise prefer MRU
  // (including equality).
  bool prefer_mfu = mru_.size() < mru_target_size_;
  std::optional<frame_id_t> victim;
  if (prefer_mfu) {
    victim = EvictFromList(&mfu_, false);
    if (!victim.has_value()) {
      victim = EvictFromList(&mru_, true);
    }
  } else {
    victim = EvictFromList(&mru_, true);
    if (!victim.has_value()) {
      victim = EvictFromList(&mfu_, false);
    }
  }
  return victim;
}

/**
 * TODO(P1): Add implementation
 *
 * @brief Record access to a frame, adjusting ARC bookkeeping accordingly
 * by bring the accessed page to the front of mfu_ if it exists in any of the lists
 * or the front of mru_ if it does not.
 *
 * Performs the operations EXCEPT REPLACE described in original paper, which is
 * handled by `Evict()`.
 *
 * Consider the following four cases, handle accordingly:
 * 1. Access hits mru_ or mfu_
 * 2/3. Access hits mru_ghost_ / mfu_ghost_
 * 4. Access misses all the lists
 *
 * This routine performs all changes to the four lists as preperation
 * for `Evict()` to simply find and evict a victim into ghost lists.
 *
 * Note that frame_id is used as identifier for alive pages and
 * page_id is used as identifier for the ghost pages, since page_id is
 * the unique identifier to the page after it's dead.
 * Using page_id for alive pages should be the same since it's one to one mapping,
 * but using frame_id is slightly more intuitive.
 *
 * @param frame_id id of frame that received a new access.
 * @param page_id id of page that is mapped to the frame.
 * @param access_type type of access that was received. This parameter is only needed for
 * leaderboard tests.
 */
void ArcReplacer::RecordAccess(frame_id_t frame_id, page_id_t page_id, [[maybe_unused]] AccessType access_type) {
  std::scoped_lock lock(latch_);

  auto alive_it = alive_map_.find(frame_id);
  if (alive_it != alive_map_.end()) {
    auto status = alive_it->second;
    if (status->page_id_ != page_id) {
      // Frame reused for a different page without going through Evict/Remove.
      EraseAliveNoGhost(frame_id);
    } else {
      // Case 1: cache hit on MRU or MFU — move to front of MFU.
      MoveAliveToMfuFront(status);
      return;
    }
  }

  auto ghost_it = ghost_map_.find(page_id);
  if (ghost_it != ghost_map_.end()) {
    auto ghost = ghost_it->second;
    if (ghost->arc_status_ == ArcStatus::MRU_GHOST) {
      // Case 2: MRU-ghost hit — increase target.
      size_t b1 = mru_ghost_.size();
      size_t b2 = mfu_ghost_.size();
      size_t delta = (b1 >= b2) ? 1 : (b2 / b1);
      mru_target_size_ = std::min(replacer_size_, mru_target_size_ + delta);
      EraseGhost(page_id);
      AddAlive(frame_id, page_id, ArcStatus::MFU);
      return;
    }
    // Case 3: MFU-ghost hit — decrease target.
    size_t b1 = mru_ghost_.size();
    size_t b2 = mfu_ghost_.size();
    size_t delta = (b2 >= b1) ? 1 : (b1 / b2);
    mru_target_size_ = (mru_target_size_ > delta) ? (mru_target_size_ - delta) : 0;
    EraseGhost(page_id);
    AddAlive(frame_id, page_id, ArcStatus::MFU);
    return;
  }

  // Case 4: complete miss — possibly trim a ghost, then insert at front of MRU.
  size_t mru_side = mru_.size() + mru_ghost_.size();
  if (mru_side >= replacer_size_) {
    // Case 4A
    if (!mru_ghost_.empty()) {
      EraseGhost(mru_ghost_.back());
    }
  } else {
    size_t total = mru_.size() + mru_ghost_.size() + mfu_.size() + mfu_ghost_.size();
    if (total >= 2 * replacer_size_) {
      // Case 4B
      if (!mfu_ghost_.empty()) {
        EraseGhost(mfu_ghost_.back());
      }
    }
  }
  AddAlive(frame_id, page_id, ArcStatus::MRU);
}

/**
 * TODO(P1): Add implementation
 *
 * @brief Toggle whether a frame is evictable or non-evictable. This function also
 * controls replacer's size. Note that size is equal to number of evictable entries.
 *
 * If a frame was previously evictable and is to be set to non-evictable, then size should
 * decrement. If a frame was previously non-evictable and is to be set to evictable,
 * then size should increment.
 *
 * If frame id is invalid, throw an exception or abort the process.
 *
 * For other scenarios, this function should terminate without modifying anything.
 *
 * @param frame_id id of frame whose 'evictable' status will be modified
 * @param set_evictable whether the given frame is evictable or not
 */
void ArcReplacer::SetEvictable(frame_id_t frame_id, bool set_evictable) {
  std::scoped_lock lock(latch_);
  // Frame IDs are not required to lie in [0, capacity); capacity bounds the number of
  // resident frames. Reject only clearly invalid identifiers.
  if (frame_id == INVALID_FRAME_ID) {
    throw Exception("SetEvictable: invalid frame id");
  }
  auto it = alive_map_.find(frame_id);
  if (it == alive_map_.end()) {
    return;
  }
  auto status = it->second;
  if (status->evictable_ == set_evictable) {
    return;
  }
  status->evictable_ = set_evictable;
  if (set_evictable) {
    curr_size_++;
  } else {
    curr_size_--;
  }
}

/**
 * TODO(P1): Add implementation
 *
 * @brief Remove an evictable frame from replacer.
 * This function should also decrement replacer's size if removal is successful.
 *
 * Note that this is different from evicting a frame, which always remove the frame
 * decided by the ARC algorithm.
 *
 * If Remove is called on a non-evictable frame, throw an exception or abort the
 * process.
 *
 * If specified frame is not found, directly return from this function.
 *
 * @param frame_id id of frame to be removed
 */
void ArcReplacer::Remove(frame_id_t frame_id) {
  std::scoped_lock lock(latch_);
  auto it = alive_map_.find(frame_id);
  if (it == alive_map_.end()) {
    return;
  }
  auto status = it->second;
  if (!status->evictable_) {
    throw Exception("Remove: frame is not evictable");
  }
  if (status->arc_status_ == ArcStatus::MRU) {
    mru_.erase(status->alive_it_);
  } else {
    mfu_.erase(status->alive_it_);
  }
  curr_size_--;
  alive_map_.erase(it);
}

/**
 * TODO(P1): Add implementation
 *
 * @brief Return replacer's size, which tracks the number of evictable frames.
 *
 * @return size_t
 */
auto ArcReplacer::Size() -> size_t {
  std::scoped_lock lock(latch_);
  return curr_size_;
}

}  // namespace bustub
