#include "pending_checkpoint.h"

namespace NYq {

TPendingCheckpoint::TPendingCheckpoint(THashSet<NActors::TActorId> toBeAcknowledged, TPendingCheckpointStats stats)
    : NotYetAcknowledged(std::move(toBeAcknowledged))
    , Stats(std::move(stats)) {
}

void TPendingCheckpoint::Acknowledge(const NActors::TActorId& actorId) {
    Acknowledge(actorId, 0);
}

void TPendingCheckpoint::Acknowledge(const NActors::TActorId& actorId, ui64 stateSize) {
    NotYetAcknowledged.erase(actorId);
    Stats.StateSize += stateSize;
}

bool TPendingCheckpoint::GotAllAcknowledges() const {
    return NotYetAcknowledged.empty();
}

size_t TPendingCheckpoint::NotYetAcknowledgedCount() const {
    return NotYetAcknowledged.size();
}

const TPendingCheckpointStats& TPendingCheckpoint::GetStats() const {
    return Stats;
}

TPendingRestoreCheckpoint::TPendingRestoreCheckpoint(TCheckpointId checkpointId, bool commitAfterRestore, THashSet<NActors::TActorId> toBeAcknowledged)
    : CheckpointId(checkpointId)
    , CommitAfterRestore(commitAfterRestore)
    , NotYetAcknowledged(std::move(toBeAcknowledged)) {
}

void TPendingRestoreCheckpoint::Acknowledge(const NActors::TActorId& actorId) {
    NotYetAcknowledged.erase(actorId);
}

bool TPendingRestoreCheckpoint::GotAllAcknowledges() const {
    return NotYetAcknowledged.empty();
}

size_t TPendingRestoreCheckpoint::NotYetAcknowledgedCount() const {
    return NotYetAcknowledged.size();
}

void TPendingInitCoordinator::OnNewCheckpointCoordinatorAck() {
    ++NewCheckpointCoordinatorAcksGot;
    Y_VERIFY(NewCheckpointCoordinatorAcksGot <= ActorsCount);
}

bool TPendingInitCoordinator::AllNewCheckpointCoordinatorAcksProcessed() const {
    return NewCheckpointCoordinatorAcksGot == ActorsCount;
}

bool TPendingInitCoordinator::CanInjectCheckpoint() const {
    return AllNewCheckpointCoordinatorAcksProcessed() && CheckpointId;
}

} // namespace NYq
