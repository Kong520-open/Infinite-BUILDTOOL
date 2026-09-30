#ifndef INFINITE_TEXTURE_COMMAND_FEEDBACK_GATE_H
#define INFINITE_TEXTURE_COMMAND_FEEDBACK_GATE_H

namespace build_import {

// Tracks the desired command-feedback setting independently from transport
// retries. A timed-out disable may still reach the server, so restoration is
// retried even when the disable acknowledgement was never observed.
class CommandFeedbackGate {
public:
    enum class Action { None, Disable, Restore };

    void requestSuppression(bool requested) noexcept {
        suppression_requested_ = requested;
    }

    Action nextAction() const noexcept {
        if (stage_ == Stage::Enabled) {
            // A previous lifecycle may have sent a disable command without
            // observing its acknowledgement. Restore that uncertainty before
            // accepting a new suppression request.
            if (may_be_suppressed_) return Action::Restore;
            if (suppression_requested_) return Action::Disable;
            return Action::None;
        }
        if (stage_ == Stage::Disabled && !suppression_requested_) {
            return Action::Restore;
        }
        return Action::None;
    }

    void markDispatched(Action action) noexcept {
        if (action == Action::Disable) {
            // A send can reach the server even if the local RPC bridge fails
            // before returning an acknowledgement, so retain this obligation.
            may_be_suppressed_ = true;
            stage_ = Stage::Disabling;
        } else if (action == Action::Restore) {
            stage_ = Stage::Restoring;
        }
    }

    void complete(Action action, bool accepted) noexcept {
        if (action == Action::Disable && stage_ == Stage::Disabling) {
            stage_ = accepted ? Stage::Disabled : Stage::Enabled;
        } else if (action == Action::Restore && stage_ == Stage::Restoring) {
            if (accepted) {
                stage_ = Stage::Enabled;
                may_be_suppressed_ = false;
            } else {
                // Keep forcing the known default until the server accepts it.
                stage_ = Stage::Disabled;
            }
        }
    }

    bool readyForSuppression() const noexcept {
        return stage_ == Stage::Disabled;
    }

    bool restored() const noexcept {
        return stage_ == Stage::Enabled && !may_be_suppressed_;
    }

    void prepareForNewRun() noexcept {
        suppression_requested_ = false;
        // Keep the uncertainty marker and force the next action to Restore.
        // The caller may request suppression again after this transition.
        stage_ = Stage::Enabled;
    }

private:
    enum class Stage { Enabled, Disabling, Disabled, Restoring };

    Stage stage_ = Stage::Enabled;
    bool suppression_requested_ = false;
    bool may_be_suppressed_ = false;
};

}  // namespace build_import

#endif  // INFINITE_TEXTURE_COMMAND_FEEDBACK_GATE_H
