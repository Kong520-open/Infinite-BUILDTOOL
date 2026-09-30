#pragma once

// Legacy call sites retain these local availability helpers until their
// independent operations are simplified. No network or account state is read.
bool IsBuildToolsAuthorized();
bool IsNativeSessionAuthorized();
bool IsNativeOperationAuthorized();
