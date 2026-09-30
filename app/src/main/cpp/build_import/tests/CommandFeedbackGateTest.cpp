#include "CommandFeedbackGate.h"

#include <cassert>

using build_import::CommandFeedbackGate;

namespace {

void expectAction(const CommandFeedbackGate& gate, CommandFeedbackGate::Action action) {
    assert(gate.nextAction() == action);
}

void acknowledge(CommandFeedbackGate* gate, CommandFeedbackGate::Action action,
                 bool accepted = true) {
    gate->markDispatched(action);
    gate->complete(action, accepted);
}

}  // namespace

int main() {
    CommandFeedbackGate gate;
    assert(gate.restored());
    expectAction(gate, CommandFeedbackGate::Action::None);

    // No TP, cleanup, or fill may proceed until the disable acknowledgement.
    gate.requestSuppression(true);
    expectAction(gate, CommandFeedbackGate::Action::Disable);
    gate.markDispatched(CommandFeedbackGate::Action::Disable);
    assert(!gate.readyForSuppression());
    expectAction(gate, CommandFeedbackGate::Action::None);
    gate.complete(CommandFeedbackGate::Action::Disable, true);
    assert(gate.readyForSuppression());
    expectAction(gate, CommandFeedbackGate::Action::None);

    // Pause/cancel after a successful import returns the game rule to true.
    gate.requestSuppression(false);
    expectAction(gate, CommandFeedbackGate::Action::Restore);
    acknowledge(&gate, CommandFeedbackGate::Action::Restore);
    assert(gate.restored());

    // A world change while the disable command is in flight must still issue a
    // restore after its late acknowledgement instead of treating it as idle.
    gate.requestSuppression(true);
    gate.markDispatched(CommandFeedbackGate::Action::Disable);
    gate.requestSuppression(false);
    gate.complete(CommandFeedbackGate::Action::Disable, true);
    expectAction(gate, CommandFeedbackGate::Action::Restore);
    acknowledge(&gate, CommandFeedbackGate::Action::Restore);
    assert(gate.restored());

    // An uncertain disable may have changed the remote gamerule. The terminal
    // path therefore restores it before reporting itself idle.
    gate.requestSuppression(true);
    gate.markDispatched(CommandFeedbackGate::Action::Disable);
    gate.complete(CommandFeedbackGate::Action::Disable, false);
    gate.requestSuppression(false);
    expectAction(gate, CommandFeedbackGate::Action::Restore);
    gate.markDispatched(CommandFeedbackGate::Action::Restore);
    gate.complete(CommandFeedbackGate::Action::Restore, false);
    expectAction(gate, CommandFeedbackGate::Action::Restore);
    acknowledge(&gate, CommandFeedbackGate::Action::Restore);
    assert(gate.restored());

    // Starting a new run after a successful disable must restore first, even
    // when the new run immediately requests suppression again.
    gate.requestSuppression(true);
    acknowledge(&gate, CommandFeedbackGate::Action::Disable);
    gate.prepareForNewRun();
    gate.requestSuppression(true);
    expectAction(gate, CommandFeedbackGate::Action::Restore);
    acknowledge(&gate, CommandFeedbackGate::Action::Restore);
    expectAction(gate, CommandFeedbackGate::Action::Disable);
    return 0;
}
