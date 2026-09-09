#pragma once
#include <memory>
#include <string>

class Operation;

// Reconstructs a fresh, default-constructed Operation from its typeId() string
// so a saved project's history steps can come back as their real op type (and
// stay parameter-editable) instead of a baked ReplayOp.
//
// The returned op has default parameters; the caller is expected to call
// deserializeParams() then rehydrateFromReload() to fully restore it. Returns
// nullptr for a typeId this factory doesn't know how to build (sketch edits,
// sub-shape-referencing ops without persistent naming, or legacy/unknown
// ids) - the loader then falls back to ReplayOp.
namespace OperationFactory {
std::unique_ptr<Operation> create(const std::string& typeId);
}
