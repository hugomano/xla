/* Copyright 2024 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/python/sdy.h"

#include <string>

#include "mhlo/transforms/passes.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "llvm/Support/raw_ostream.h"
#include "mlir/Bytecode/BytecodeWriter.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Support/LLVM.h"
#include "nanobind/nanobind.h"
#include "nanobind/stl/string.h"  // IWYU pragma: keep
#include "nanobind/stl/string_view.h"  // IWYU pragma: keep
#include "xla/hlo/translate/hlo_to_mhlo/hlo_to_mlir_hlo.h"
#include "xla/mlir_hlo/mhlo/transforms/passes.h"
#include "xla/pjrt/mlir_to_hlo.h"
#include "xla/pjrt/status_casters.h"
#include "xla/service/spmd/shardy/sdy_round_trip/pipelines.h"
#include "xla/tsl/framework/mlir/status_scoped_diagnostic_handler.h"
#include "tsl/platform/logging.h"

namespace nb = nanobind;

namespace xla {

namespace {

absl::StatusOr<std::string> SerializeUsingBytecode(mlir::ModuleOp module) {
  std::string bytecode;
  llvm::raw_string_ostream os(bytecode);
  mlir::BytecodeWriterConfig config;
  if (mlir::failed(mlir::writeBytecodeToFile(module, os, config))) {
    return absl::InvalidArgumentError("mlir::writeBytecodeToFile failed");
  }
  return bytecode;
}

}  // namespace

void BuildSdySubmodule(nb::module_& m) {
  nb::module_ mlir_module = m.def_submodule("sdy", "Shardy/XLA integration");

  mlir_module.def(
      "sdy_round_trip_export_pipeline",
      [](const nb::bytes& bytecode) {
        mlir::MLIRContext context;
        mlir::OwningOpRef<mlir::ModuleOp> module =
            xla::ValueOrThrow(ParseMlirModuleString(
                absl::string_view(bytecode.c_str(), bytecode.size()), context));
        mlir::PassManager pm(&context);
        sdy::addSdyRoundTripExportPipeline(pm);
        tsl::StatusScopedDiagnosticHandler diagnosticHandler(&context);
        absl::Status success =
            diagnosticHandler.consumeStatus(pm.run(module.get()));
        ThrowIfError(success);
        std::string module_str =
            xla::ValueOrThrow(SerializeUsingBytecode(module.get()));
        return nb::bytes(module_str.data(), module_str.size());
      },
      nb::arg("module"));
}

}  // namespace xla
